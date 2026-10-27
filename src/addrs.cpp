#include "addrs.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::addrs {

namespace {

struct Row {
    const char* name;
    char kind;  // 'f' function, 'g' global or vtable
    uint32_t rva;
    const char* sig;     // hex bytes, ?? = any (relative operands)
    const char* anchor;  // globals: the table function that reads it through a rip-relative operand
    uint32_t dispOffset;  // ... offset of that disp32 inside the function
    uint32_t endOffset;   // ... offset of the instruction's end (rip) inside the function
};

#include "addrs_table.inc"

constexpr size_t kCount = sizeof(kRows) / sizeof(kRows[0]);

enum class State : uint8_t { Pending, Ok, Moved, Unresolved, Ambiguous };

struct Slot {
    uint8_t* addr = nullptr;   // what the mod may use (null: gate closed or not resolved)
    uint8_t* found = nullptr;  // where the signature / anchor says it is, used or not (patch report, RTTI fallbacks)
    State state = State::Pending;
    const char* why = "";      // globals: why nothing was found
};

Slot g_slots[kCount];
std::once_flag g_once;
bool g_recognised = false;
bool g_usable = false;  // the table may be used: the exe is the table's, or an unrecognised build is tried (by signature only)
uint8_t* g_text = nullptr;
size_t g_textSize = 0;

uint8_t* base() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = compare
};

Pattern parse(const char* sig) {
    Pattern p;
    for (const char* s = sig; *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
        } else {
            p.bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            p.mask.push_back(1);
        }
        s += 2;
    }
    return p;
}

bool readableBytes(const uint8_t* at, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    while (n) {
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const uint8_t* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        size_t have = static_cast<size_t>(end - at);
        if (have >= n) return true;
        at += have;
        n -= have;
    }
    return true;
}

bool matchAt(const uint8_t* at, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); ++i)
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    return true;
}

// Matches of a signature in .text, up to two (unique or not). Only committed, readable pages are looked at:
// an unreadable gap in a protected exe must not take the game down.
int scan(const Pattern& p, uint8_t** first) {
    if (p.bytes.empty() || !g_text) return 0;
    size_t lead = 0;
    while (lead < p.bytes.size() && !p.mask[lead]) ++lead;
    if (lead == p.bytes.size()) return 0;
    int hits = 0;
    const size_t n = p.bytes.size();
    const uint8_t* at = g_text;
    const uint8_t* const end = g_text + g_textSize;
    MEMORY_BASIC_INFORMATION mbi;
    while (at < end && VirtualQuery(at, &mbi, sizeof(mbi))) {
        const uint8_t* regionEnd = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd > end) regionEnd = end;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            for (const uint8_t* q = at; q + n <= regionEnd; ++q) {
                if (q[lead] != p.bytes[lead] || !matchAt(q, p)) continue;
                if (!hits++ && first) *first = const_cast<uint8_t*>(q);
                if (hits == 2) return hits;
            }
        }
        at = regionEnd;
    }
    return hits;
}

void findText() {
    uint8_t* b = base();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 6) == 0) {
            g_text = b + sec[i].VirtualAddress;
            g_textSize = sec[i].Misc.VirtualSize;
        }
    }
}

const Row* find(const char* name, size_t* index) {
    for (size_t i = 0; i < kCount; ++i)
        if (std::strcmp(kRows[i].name, name) == 0) {
            *index = i;
            return &kRows[i];
        }
    return nullptr;
}

// A global through its anchor function's rip-relative operand. Null with a reason when the anchor was not
// found, or the bytes there are no rip-relative operand (the function changed shape), or the target is
// outside the image or, for a vtable, does not start with code addresses.
uint8_t* viaAnchor(const Row& r, const char** why) {
    size_t ai;
    if (!r.anchor[0]) {
        *why = "no anchor in the table";
        return nullptr;
    }
    if (!find(r.anchor, &ai) || !g_slots[ai].found) {
        *why = "anchor function not found";
        return nullptr;
    }
    const uint8_t* f = g_slots[ai].found;
    if (!readableBytes(f + r.dispOffset - 3, 7)) {
        *why = "anchor bytes unreadable";
        return nullptr;
    }
    if ((f[r.dispOffset - 1] & 0xc7) != 0x05) {  // modrm: mod 00, rm 101 = [rip+disp32]
        *why = "anchor operand is not rip-relative";
        return nullptr;
    }
    int32_t disp;
    std::memcpy(&disp, f + r.dispOffset, sizeof(disp));
    uint8_t* target = const_cast<uint8_t*>(f) + r.endOffset + disp;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base() + dos->e_lfanew);
    if (target < base() || target >= base() + nt->OptionalHeader.SizeOfImage) {
        *why = "anchor target outside the image";
        return nullptr;
    }
    const size_t nameLen = std::strlen(r.name);
    if (nameLen > 6 && std::strcmp(r.name + nameLen - 6, "VTable") == 0) {
        uintptr_t first = 0, second = 0;
        if (!readableBytes(target, 16)) {
            *why = "vtable unreadable";
            return nullptr;
        }
        std::memcpy(&first, target, 8);
        std::memcpy(&second, target + 8, 8);
        auto inText = [](uintptr_t v) { return v >= reinterpret_cast<uintptr_t>(g_text) && v < reinterpret_cast<uintptr_t>(g_text) + g_textSize; };
        if (!inText(first) || !inText(second)) {
            *why = "anchor target does not look like a vtable";
            return nullptr;
        }
    }
    return target;
}

void resolveAll() {
    g_recognised = gameaddr::recognised();
    g_usable = gameaddr::usable();
    const bool dx9 = gameaddr::dx9();
    if (!dx9) findText();
    uint8_t* b = base();
    int ok = 0, moved = 0, bad = 0, anchored = 0, byStamp = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const Row& r = kRows[i];
        Slot& s = g_slots[i];
        if (r.kind != 'f' || dx9) continue;
        Pattern p = parse(r.sig);
        if (readableBytes(b + r.rva, p.bytes.size()) && matchAt(b + r.rva, p)) {
            s.found = b + r.rva;
            s.addr = g_usable ? s.found : nullptr;
            s.state = State::Ok;
            ++ok;
            continue;
        }
        uint8_t* found = nullptr;
        int hits = scan(p, &found);
        if (hits == 1) {
            LOG("Game address %s: 0x%x -> 0x%llx (by signature)", r.name, r.rva,
                static_cast<unsigned long long>(found - b));
            s.state = State::Moved;
            s.found = found;
            s.addr = g_usable ? found : nullptr;  // the bytes verify, only the rva moved
            ++moved;
        } else {
            LOG("Game address %s: %s (table rva 0x%x)", r.name, hits ? "signature not unique" : "not found", r.rva);
            s.state = hits ? State::Ambiguous : State::Unresolved;
            ++bad;
        }
    }
    for (size_t i = 0; i < kCount; ++i) {
        const Row& r = kRows[i];
        Slot& s = g_slots[i];
        if (r.kind != 'g' || dx9) continue;
        const char* why = "";
        uint8_t* via = viaAnchor(r, &why);
        if (g_recognised) {
            if (via && via != b + r.rva) {
                LOG("Game address %s: table 0x%x, anchor %s gives 0x%llx; using the anchor", r.name, r.rva, r.anchor,
                    static_cast<unsigned long long>(via - b));
                s.addr = s.found = via;
                s.state = State::Moved;
                ++moved;
            } else {
                s.addr = s.found = b + r.rva;
                s.state = State::Ok;
                via ? ++anchored : ++byStamp;
            }
        } else if (via) {
            // Another build: only a global reached through an anchor function that was found by signature.
            s.found = via;
            s.addr = g_usable ? via : nullptr;
            s.state = State::Moved;
            ++moved;
        } else {
            s.state = State::Unresolved;
            s.why = why;
            ++bad;
        }
    }
    if (g_recognised)
        LOG("Game addresses (%s): %u entries; functions: %d at their table rva, %d found by signature, %d unresolved; "
            "globals: %d confirmed through an anchor, %d by exe stamp only",
            kGameVersion, static_cast<unsigned>(kCount), ok, moved, bad, anchored, byStamp);
    else if (!dx9)
        LOG("Game addresses: exe stamp is not the table's (%08x): %d at their table rva, %d found elsewhere, %d not found "
            "(%u entries). %s",
            kTableStamp, ok, moved, bad, static_cast<unsigned>(kCount),
            g_usable ? "Unrecognised build mode: rows found by signature are used, features with a missing row stay off."
                     : "Nothing is used until the table is remapped or [patch] try_unknown_build is on "
                       "(re/exe/README.md, After a game patch).");
}

void init() { std::call_once(g_once, resolveAll); }

uint8_t* get(const char* name, char kind) {
    init();
    size_t i;
    const Row* r = find(name, &i);
    if (!r || r->kind != kind) {
        LOG("Game address %s: not in the table (code bug)", name);
        return nullptr;
    }
    return g_slots[i].addr;
}

}  // namespace

uint8_t* fn(const char* name) { return get(name, 'f'); }
uint8_t* global(const char* name) { return get(name, 'g'); }
uint32_t tableStamp() { return kTableStamp; }

void prepare() { init(); }

uint8_t* resolved(const char* name) {
    init();
    size_t i;
    const Row* r = find(name, &i);
    return r ? g_slots[i].found : nullptr;
}

size_t rowCount() { return kCount; }

RowInfo row(size_t i) {
    init();
    const Row& r = kRows[i];
    const Slot& s = g_slots[i];
    static const char* const kStates[] = {"pending", "at the table rva", "moved (found by signature)", "not found",
                                          "signature not unique"};
    RowInfo info;
    info.name = r.name;
    info.global = r.kind == 'g';
    info.tableRva = r.rva;
    info.foundRva = s.found ? static_cast<uint32_t>(s.found - base()) : 0;
    info.used = s.addr != nullptr;
    info.anchor = r.anchor;
    info.state = info.global && s.state == State::Ok ? "by exe stamp only" : kStates[static_cast<int>(s.state)];
    info.why = s.why;
    return info;
}

const char* nameAt(const void* address) {
    if (!address) return nullptr;
    for (size_t i = 0; i < kCount; ++i)
        if (g_slots[i].addr == address && kRows[i].kind == 'f') return kRows[i].name;
    return nullptr;
}
const char* gameVersion() { return kGameVersion; }

}  // namespace s4vr::addrs
