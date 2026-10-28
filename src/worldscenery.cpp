#include "worldscenery.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "log.h"
#include "modelparse.h"

namespace s4vr::worldscenery {
namespace {

constexpr uint32_t kLot = 0x12952634;     // a lot of the area (instance = its lot description id)
constexpr uint32_t kObjects = 0xFCB1A1E4;  // the area's placed objects (WorldLandingStripData)

struct Entry {
    uint64_t position = 0;
    uint32_t stored = 0, size = 0;
    uint16_t compression = 0;
};

struct Area {
    std::wstring path;
    Entry objects;
    bool hasObjects = false;
};

std::atomic<int> g_state{0};  // 0 not started, 1 reading the indexes, 2 done
std::mutex g_mutex;
std::vector<Area> g_areas;
std::unordered_map<uint64_t, size_t> g_lots;  // lot description id -> g_areas index

// The game folder: the exe is in <root>\Game\Bin, so three up (as modelbox.cpp).
std::wstring gameRoot() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    for (int i = 0; i < 3; ++i) {
        size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return L"";
        p.resize(cut);
    }
    return p;
}

bool readAt(HANDLE f, uint64_t at, void* out, uint32_t size) {
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(at);
    o.OffsetHigh = static_cast<DWORD>(at >> 32);
    DWORD got = 0;
    return ReadFile(f, out, size, &got, &o) && got == size;
}

// Every entry in one package's index: fn(type, instance, entry) (DBPF 2.1, as modelbox.cpp forEachEntry).
template <class Fn>
void forEachEntry(const std::wstring& path, Fn&& fn) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    uint8_t header[96];
    if (readAt(f, 0, header, sizeof(header)) && std::memcmp(header, "DBPF", 4) == 0) {
        uint32_t count, size, low;
        uint64_t position;
        std::memcpy(&count, header + 36, 4);
        std::memcpy(&low, header + 40, 4);
        std::memcpy(&size, header + 44, 4);
        std::memcpy(&position, header + 64, 8);
        if (!position) position = low;
        std::vector<uint8_t> index(size);
        if (count && size >= 4 && size < (512u << 20) && readAt(f, position, index.data(), size)) {
            const uint8_t* p = index.data();
            const uint8_t* end = p + size;
            auto u32 = [&](uint32_t& v) {
                if (p + 4 > end) return false;
                std::memcpy(&v, p, 4);
                p += 4;
                return true;
            };
            uint32_t flags = 0, constant[4] = {};
            u32(flags);
            for (int i = 0; i < 4; ++i)
                if (flags & (1u << i)) u32(constant[i]);
            for (uint32_t e = 0; e < count; ++e) {
                uint32_t field[4], at, stored, unpacked;
                bool ok = true;
                for (int i = 0; i < 4 && ok; ++i) {
                    if (flags & (1u << i))
                        field[i] = constant[i];
                    else
                        ok = u32(field[i]);
                }
                ok = ok && u32(at) && u32(stored) && u32(unpacked);
                uint16_t compression = 0;
                if (ok && (stored & 0x80000000u)) {
                    if (p + 4 > end) break;
                    std::memcpy(&compression, p, 2);
                    p += 4;
                }
                if (!ok) break;
                fn(field[0], (static_cast<uint64_t>(field[2]) << 32) | field[3],
                   Entry{at, stored & 0x7FFFFFFFu, unpacked, compression});
            }
        }
    }
    CloseHandle(f);
}

// *.world files in dir into found (by name: a later one wins).
void addWorlds(const std::wstring& dir, std::map<std::wstring, std::wstring>& found) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.world").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring name = fd.cFileName;
            for (auto& c : name) c = towlower(c);
            found[name] = dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Subfolders of dir (the packs: EP01, GP04, ...).
std::vector<std::wstring> folders(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// The areas: Data\Shared\Worlds\Areas, then each pack's Worlds\Areas, then Delta\<pack>\Worlds\Areas (the
// updated copies win).
void scan() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::wstring root = gameRoot();
    std::map<std::wstring, std::wstring> found;
    addWorlds(root + L"\\Data\\Shared\\Worlds\\Areas", found);
    for (const auto& pack : folders(root))
        if (_wcsicmp(pack.c_str(), L"Delta") != 0) addWorlds(root + L"\\" + pack + L"\\Worlds\\Areas", found);
    for (const auto& pack : folders(root + L"\\Delta")) addWorlds(root + L"\\Delta\\" + pack + L"\\Worlds\\Areas", found);
    std::vector<Area> areas;
    std::unordered_map<uint64_t, size_t> lots;
    for (const auto& [name, path] : found) {
        Area area{path, {}, false};
        std::vector<uint64_t> mine;
        forEachEntry(path, [&](uint32_t type, uint64_t instance, const Entry& e) {
            if (type == kLot)
                mine.push_back(instance);
            else if (type == kObjects && !area.hasObjects)
                area.objects = e, area.hasObjects = true;
        });
        for (uint64_t lot : mine) lots[lot] = areas.size();
        areas.push_back(area);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_areas.swap(areas);
        g_lots.swap(lots);
    }
    LOG("worldscenery: %zu areas, %zu lots indexed", areas.size(), lots.size());
    g_state = 2;
}

// The placed objects (arrays A and B: objects; C sim locators and D stair locators are skipped). Layout:
// re/client/tools/formats.py parse_world_objects (every file parses to its exact length).
bool parse(const std::vector<uint8_t>& d, std::vector<Placed>& out) {
    size_t o = 8;
    if (d.size() < 8) return false;
    uint32_t chunks;
    std::memcpy(&chunks, d.data() + 4, 4);
    static const size_t kSize[4] = {82, 82, 56, 60};
    for (uint32_t c = 0; c < chunks; ++c) {
        o += 4 + 16;  // id, region key
        for (int a = 0; a < 4; ++a) {
            if (o + 4 > d.size()) return false;
            uint32_t n;
            std::memcpy(&n, d.data() + o, 4);
            o += 4;
            if (o + static_cast<size_t>(n) * kSize[a] > d.size()) return false;
            if (a < 2)
                for (uint32_t i = 0; i < n; ++i) {
                    const uint8_t* r = d.data() + o + kSize[a] * i;
                    Placed p;
                    std::memcpy(&p.model, r + 8, 8);
                    std::memcpy(&p.x, r + 16, 32);  // x y z qx qy qz qw scale
                    out.push_back(p);
                }
            o += static_cast<size_t>(n) * kSize[a];
        }
    }
    return true;
}

}  // namespace

int forLot(uint64_t lotDescription, std::vector<Placed>& out, std::string& area) {
    out.clear();
    area.clear();
    int expected = 0;
    if (g_state.compare_exchange_strong(expected, 1)) std::thread(scan).detach();
    if (g_state != 2) return 1;
    Area a;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_lots.find(lotDescription);
        if (it == g_lots.end()) return 2;
        a = g_areas[it->second];
    }
    size_t cut = a.path.find_last_of(L"\\/");
    for (wchar_t ch : a.path.substr(cut + 1)) area.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    if (!a.hasObjects) return 0;  // an area without scenery
    HANDLE f = CreateFileW(a.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    std::vector<uint8_t> stored(a.objects.stored), bytes;
    bool ok = readAt(f, a.objects.position, stored.data(), a.objects.stored);
    CloseHandle(f);
    if (!ok || !cbo::parse::unpack(a.objects.compression, stored.data(), stored.size(), a.objects.size, bytes)) return 3;
    return parse(bytes, out) ? 0 : 3;
}

}  // namespace s4vr::worldscenery
