#include "othermods.h"

#include <windows.h>
#include <atomic>

#include "log.h"

namespace s4vr::othermods {
namespace {

// Each DLL mod's file and its settings exports, the current names first, then
// the ones before the rename (an older release of that mod).
struct Names {
    const wchar_t* file;
    const char* get;
    const char* set;
};
const Names kNames[kModCount][2] = {
    {{L"integratedbodyphysics.dll", "ibp_get", "ibp_set"},
     {L"claudejiggle.dll", "claudejiggle_get", "claudejiggle_set"}},
    {{L"extendedsoundcontrol.dll", "esc_get", "esc_set"},
     {L"claudesound.dll", "claudesound_get", "claudesound_set"}},
    {},  // Claude Performance: a script mod
    {{L"claudebustinout.dll", "cbo_get", "cbo_set"}, {}},
};
// Each DLL mod's menu export (nullptr: none).
const char* const kMenuNames[kModCount] = {"ibp_menu", "esc_menu", nullptr, "cbo_menu"};

std::atomic<int> g_scriptMods{0};

}  // namespace

bool api(Mod mod, GetFn& get, SetFn& set) {
    if (mod < 0 || mod >= kModCount || mod == kPerformance) return false;
    for (const Names& file : kNames[mod]) {
        HMODULE m = file.file ? GetModuleHandleW(file.file) : nullptr;
        if (!m) continue;
        for (const Names& calls : kNames[mod]) {
            if (!calls.get) continue;
            get = reinterpret_cast<GetFn>(GetProcAddress(m, calls.get));
            set = reinterpret_cast<SetFn>(GetProcAddress(m, calls.set));
            if (get && set) return true;
        }
    }
    return false;
}

bool loaded(Mod mod) {
    if (mod == kPerformance) return (g_scriptMods.load(std::memory_order_relaxed) >> kPerformance) & 1;
    GetFn get;
    SetFn set;
    return api(mod, get, set);
}

const wchar_t* menu(Mod mod) {
    if (mod < 0 || mod >= kModCount || !kMenuNames[mod]) return nullptr;
    using MenuFn = const wchar_t* (*)();
    for (const Names& file : kNames[mod])
        if (HMODULE m = file.file ? GetModuleHandleW(file.file) : nullptr)
            if (auto fn = reinterpret_cast<MenuFn>(GetProcAddress(m, kMenuNames[mod]))) return fn();
    return nullptr;
}

void setScriptMods(int bits) {
    if (g_scriptMods.exchange(bits) != bits)
        LOG("Other mods: Claude Performance %s", (bits >> kPerformance) & 1 ? "loaded" : "not loaded");
}

}  // namespace s4vr::othermods
