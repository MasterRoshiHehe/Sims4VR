#include "rtti.h"

#include <cstring>

namespace s4vr::rtti {

namespace {

struct Section {
    uint8_t* begin = nullptr;
    size_t size = 0;
};

Section findSection(HMODULE module, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            return {base + section->VirtualAddress, section->Misc.VirtualSize};
    }
    return {};
}

// x64 RTTI Complete Object Locator; all pointers are image-relative.
struct CompleteObjectLocator {
    uint32_t signature;  // 1 on x64
    uint32_t offset;     // offset of this vtable's subobject in the full object
    uint32_t cdOffset;
    uint32_t typeDescriptor;
    uint32_t classDescriptor;
    uint32_t self;
};

template <typename T>
const uint8_t* findValue(const Section& s, const T& value, const uint8_t* from = nullptr) {
    const uint8_t* p = from ? from : s.begin;
    const uint8_t* end = s.begin + s.size - sizeof(T);
    for (; p <= end; p += 4) {  // RTTI records and vtables are at least 4-byte aligned
        if (std::memcmp(p, &value, sizeof(T)) == 0) return p;
    }
    return nullptr;
}

}  // namespace

bool readable(uintptr_t address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return address + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uintptr_t findVTable(HMODULE module, const char* mangledName) {
    auto base = reinterpret_cast<uintptr_t>(module);
    Section data = findSection(module, ".data");
    Section rdata = findSection(module, ".rdata");
    if (!data.begin || !rdata.begin) return 0;

    // TypeDescriptor = { vftable ptr, spare ptr, char name[] } lives in .data.
    size_t nameLength = std::strlen(mangledName) + 1;
    const uint8_t* name = nullptr;
    for (const uint8_t* p = data.begin; p + nameLength <= data.begin + data.size; ++p) {
        if (*p == '.' && std::memcmp(p, mangledName, nameLength) == 0) {
            name = p;
            break;
        }
    }
    if (!name) return 0;
    auto typeDescriptorRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(name) - 16 - base);

    // COL.typeDescriptor sits 12 bytes into the locator.
    for (const uint8_t* hit = findValue(rdata, typeDescriptorRva); hit; hit = findValue(rdata, typeDescriptorRva, hit + 4)) {
        auto* col = reinterpret_cast<const CompleteObjectLocator*>(hit - 12);
        auto colRva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(col) - base);
        if (col->signature != 1 || col->self != colRva || col->offset != 0) continue;
        // vtable[-1] points at the locator.
        uintptr_t colVa = reinterpret_cast<uintptr_t>(col);
        if (const uint8_t* slot = findValue(rdata, colVa)) return reinterpret_cast<uintptr_t>(slot) + 8;
    }
    return 0;
}

std::vector<uintptr_t> findInstances(uintptr_t vtable, size_t maxResults) {
    std::vector<uintptr_t> results;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<uintptr_t> chunk(1 << 17);  // 1 MB; read with ReadProcessMemory so freed pages can't fault
    MEMORY_BASIC_INFORMATION mbi;
    while (address < end && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = regionBase; p < regionBase + mbi.RegionSize; p += chunk.size() * 8) {
                size_t bytes = regionBase + mbi.RegionSize - p;
                if (bytes > chunk.size() * 8) bytes = chunk.size() * 8;
                SIZE_T read = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), chunk.data(), bytes, &read)) continue;
                for (size_t i = 0; i < read / 8; ++i) {
                    if (chunk[i] != vtable) continue;
                    results.push_back(p + i * 8);
                    if (results.size() >= maxResults) return results;
                }
            }
        }
        address = regionBase + mbi.RegionSize;
    }
    return results;
}

bool className(uintptr_t object, char* out, size_t size) {
    uintptr_t vtable = vtableOf(object);
    if (!vtable || !readable(vtable - 8, 8)) return false;
    uintptr_t colVa = *reinterpret_cast<uintptr_t*>(vtable - 8);
    if (!readable(colVa, sizeof(CompleteObjectLocator))) return false;
    auto* col = reinterpret_cast<const CompleteObjectLocator*>(colVa);
    if (col->signature != 1) return false;
    uintptr_t base = colVa - col->self;
    uintptr_t name = base + col->typeDescriptor + 16;
    if (!readable(name, 1)) return false;
    size_t i = 0;
    for (; i + 1 < size && readable(name + i, 1); ++i) {
        char c = *reinterpret_cast<const char*>(name + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

uintptr_t vtableOf(uintptr_t object) {
    if (!readable(object, sizeof(uintptr_t))) return 0;
    return *reinterpret_cast<uintptr_t*>(object);
}

}  // namespace s4vr::rtti
