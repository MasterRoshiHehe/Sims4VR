#include "modelbox.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <map>
#include <vector>

#include "log.h"

namespace s4vr::modelbox {
namespace {

constexpr uint32_t kModel = 0x01661233;

struct Entry {
    uint32_t file;
    uint64_t position;
    uint32_t stored;  // bytes in the package
    uint32_t size;    // bytes unpacked
    uint16_t compression;
};

std::atomic<int> g_state{0};  // 0 not started, 1 scanning, 2 done
std::mutex g_mutex;
std::vector<std::wstring> g_files;
std::unordered_map<uint64_t, Entry> g_models;

// The game folder: the exe is in <root>\Game\Bin (or Game-cracked etc.), so two up.
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

// Client*Build*.package files under dir, a few folders deep.
void findPackages(const std::wstring& dir, int depth, std::vector<std::wstring>& full, std::vector<std::wstring>& delta) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && _wcsicmp(name.c_str(), L"Game") != 0 && _wcsicmp(name.c_str(), L"__Installer") != 0 &&
                _wcsicmp(name.c_str(), L"Soundtrack") != 0 && _wcsicmp(name.c_str(), L"Support") != 0)
                findPackages(path, depth - 1, full, delta);
            continue;
        }
        if (_wcsnicmp(name.c_str(), L"Client", 6) != 0) continue;
        size_t n = name.size();
        if (n < 8 || _wcsicmp(name.c_str() + n - 8, L".package") != 0) continue;
        if (name.find(L"FullBuild") != std::wstring::npos)
            full.push_back(path);
        else if (name.find(L"DeltaBuild") != std::wstring::npos)
            delta.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool readAt(HANDLE f, uint64_t at, void* out, uint32_t size) {
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(at);
    o.OffsetHigh = static_cast<DWORD>(at >> 32);
    DWORD got = 0;
    return ReadFile(f, out, size, &got, &o) && got == size;
}

// Adds the models in one package's index. Returns how many.
int scanPackage(uint32_t fileIndex, const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    uint8_t header[96];
    int added = -1;
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
            added = 0;
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
            std::lock_guard<std::mutex> lock(g_mutex);
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
                if (field[0] != kModel) continue;
                uint64_t instance = (static_cast<uint64_t>(field[2]) << 32) | field[3];
                if (compression == 0xFFE0) {  // deleted by an update
                    g_models.erase(instance);
                    continue;
                }
                g_models[instance] = Entry{fileIndex, at, stored & 0x7FFFFFFFu, unpacked, compression};
                ++added;
            }
        }
    }
    CloseHandle(f);
    return added;
}

// Resources looked up by key (readResource). One worker thread answers
// every key asked for, and each answer is kept for the session: the package
// list is made once, and one pass over the packages' indexes answers every key
// waiting by then. (Up to v0.13.146 each new key started its own thread that
// listed and read all ~850 packages; the HUD asking for two missing icons in
// turn started ten such threads a second, which piled up into hundreds,
// slowed every file access in the game and stalled the frame for 0.5 s.)
struct Found {
    int state = 0;  // 2 found, 3 not found
    std::wstring path;
    Entry entry{};
};
std::mutex g_lookupMutex;
std::map<std::pair<uint32_t, uint64_t>, Found> g_found;
std::vector<std::pair<uint32_t, uint64_t>> g_wanted;  // asked for, not looked up yet
bool g_working = false;                                // the worker thread runs
std::vector<std::wstring> g_order;                     // game full, game delta, then mods (later ones win)
size_t g_gamePackages = 0;
bool g_listed = false;

// Pictures, for type 0 ("any picture"): PNG, DST/DDS and RLE2 images.
bool isPicture(uint32_t type) { return type == 0x2F7D0004u || type == 0x00B2D882u || type == 0x3453CF95u; }
bool typeMatches(uint32_t wanted, uint32_t type) { return wanted ? type == wanted : isPicture(type); }

// Every entry in one package's index: fn(type, instance, entry).
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
                   Entry{0, at, stored & 0x7FFFFFFFu, unpacked, compression});
            }
        }
    }
    CloseHandle(f);
}

// Every .package under dir, a few folders deep (the Mods folder).
void findModPackages(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) findModPackages(path, depth - 1, out);
            continue;
        }
        size_t n = name.size();
        if (n >= 8 && _wcsicmp(name.c_str() + n - 8, L".package") == 0) out.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The Mods folder: this DLL sits in Mods\sims4vr.
std::wstring modsFolder() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&modsFolder), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    for (int i = 0; i < 2; ++i) {
        size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return L"";
        p.resize(cut);
    }
    return p;
}

// The game's Client packages, then the Mods folder's (worker thread, once).
void listPackages() {
    std::wstring root = gameRoot();
    std::vector<std::wstring> full, delta, mods;
    if (!root.empty()) findPackages(root, 3, full, delta);
    std::wstring modsDir = modsFolder();
    if (!modsDir.empty()) findModPackages(modsDir, 5, mods);
    g_order = full;
    g_order.insert(g_order.end(), delta.begin(), delta.end());
    g_gamePackages = g_order.size();
    g_order.insert(g_order.end(), mods.begin(), mods.end());
    g_listed = true;
}

// One pass over every package's index for all the keys in batch. A picture
// key that isn't there under its own type is taken under another picture type
// with the same instance (the game keeps some icons as DST, not PNG).
void lookUp(const std::vector<std::pair<uint32_t, uint64_t>>& batch) {
    ULONGLONG started = GetTickCount64();
    if (!g_listed) listPackages();
    struct Hit {
        Found exact, picture;
    };
    std::unordered_map<uint64_t, std::vector<size_t>> byInstance;
    std::vector<Hit> hits(batch.size());
    for (size_t k = 0; k < batch.size(); ++k) byInstance[batch[k].second].push_back(k);
    for (size_t i = 0; i < g_order.size(); ++i) {
        forEachEntry(g_order[i], [&](uint32_t type, uint64_t instance, const Entry& e) {
            auto it = byInstance.find(instance);
            if (it == byInstance.end()) return;
            for (size_t k : it->second) {
                uint32_t wanted = batch[k].first;
                Found* slot = typeMatches(wanted, type) ? &hits[k].exact
                              : (isPicture(wanted) && isPicture(type)) ? &hits[k].picture
                                                                       : nullptr;
                if (!slot) continue;
                if (e.compression == 0xFFE0) {  // deleted by an update
                    slot->state = 0;
                    continue;
                }
                slot->state = 2;
                slot->path = g_order[i];
                slot->entry = e;
            }
        });
    }
    int found = 0;
    std::string missing;
    {
        std::lock_guard<std::mutex> lock(g_lookupMutex);
        for (size_t k = 0; k < batch.size(); ++k) {
            Found r = hits[k].exact.state == 2 ? hits[k].exact : hits[k].picture;
            if (r.state != 2) {
                r.state = 3;
                if (missing.size() < 400) {
                    char key[40];
                    snprintf(key, sizeof(key), " %08x:%016llx", batch[k].first,
                             static_cast<unsigned long long>(batch[k].second));
                    missing += key;
                }
            } else {
                ++found;
            }
            g_found[batch[k]] = r;
        }
    }
    LOG("Resources: %zu looked up in one pass over %zu game and %zu mod packages, %d found, %llu ms%s%s", batch.size(),
        g_gamePackages, g_order.size() - g_gamePackages, found, GetTickCount64() - started,
        missing.empty() ? "" : "; not found:", missing.c_str());
}

void lookUpWanted() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::vector<std::pair<uint32_t, uint64_t>> batch;
        {
            std::lock_guard<std::mutex> lock(g_lookupMutex);
            batch.swap(g_wanted);
            if (batch.empty()) {
                g_working = false;
                return;
            }
        }
        lookUp(batch);
    }
}

void scan() {
    ULONGLONG started = GetTickCount64();
    std::wstring root = gameRoot();
    std::vector<std::wstring> full, delta;
    if (!root.empty()) findPackages(root, 3, full, delta);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_files = full;
        g_files.insert(g_files.end(), delta.begin(), delta.end());
    }
    int packages = 0;
    for (uint32_t i = 0; i < g_files.size(); ++i)
        if (scanPackage(i, g_files[i]) >= 0) ++packages;
    size_t models;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        models = g_models.size();
    }
    LOG("Models: %zu object models indexed from %d of %zu game packages under %ls in %llu ms", models, packages,
        g_files.size(), root.c_str(), GetTickCount64() - started);
    g_state = 2;
}

}  // namespace

void start() {
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        scan();
    }).detach();
}

int readResource(uint32_t type, uint64_t instance, uint16_t& compression, uint32_t& size, std::vector<uint8_t>& bytes) {
    Entry e;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(g_lookupMutex);
        auto key = std::make_pair(type, instance);
        auto it = g_found.find(key);
        if (it == g_found.end()) {
            if (std::find(g_wanted.begin(), g_wanted.end(), key) == g_wanted.end()) g_wanted.push_back(key);
            if (!g_working) {
                g_working = true;
                std::thread(lookUpWanted).detach();
            }
            return 1;
        }
        if (it->second.state == 3) return 2;
        e = it->second.entry;
        path = it->second.path;
    }
    if (e.stored > (64u << 20)) return 3;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    bytes.resize(e.stored);
    bool ok = readAt(f, e.position, bytes.data(), e.stored);
    CloseHandle(f);
    if (!ok) return 3;
    compression = e.compression;
    size = e.size;
    return 0;
}

int read(uint64_t instance, uint16_t& compression, uint32_t& size, std::vector<uint8_t>& bytes) {
    if (g_state.load() != 2) return 1;
    Entry e;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_models.find(instance);
        if (it == g_models.end()) return 2;
        e = it->second;
        path = g_files[e.file];
    }
    if (e.stored > (64u << 20)) return 3;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    bytes.resize(e.stored);
    bool ok = readAt(f, e.position, bytes.data(), e.stored);
    CloseHandle(f);
    if (!ok) return 3;
    compression = e.compression;
    size = e.size;
    return 0;
}

}  // namespace s4vr::modelbox
