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
const Names kNames[2][2] = {
    {{L"integratedbodyphysics.dll", "ibp_get", "ibp_set"},
     {L"claudejiggle.dll", "claudejiggle_get", "claudejiggle_set"}},
    {{L"extendedsoundcontrol.dll", "esc_get", "esc_set"},
     {L"claudesound.dll", "claudesound_get", "claudesound_set"}},
};

std::atomic<int> g_scriptMods{0};

}  // namespace

bool api(Mod mod, GetFn& get, SetFn& set) {
    if (mod != kBodyPhysics && mod != kSoundControl) return false;
    for (const Names& file : kNames[mod]) {
        HMODULE m = GetModuleHandleW(file.file);
        if (!m) continue;
        for (const Names& calls : kNames[mod]) {
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

void setScriptMods(int bits) {
    if (g_scriptMods.exchange(bits) != bits)
        LOG("Other mods: Claude Performance %s", (bits >> kPerformance) & 1 ? "loaded" : "not loaded");
}

}  // namespace s4vr::othermods
