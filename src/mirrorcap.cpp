#include "mirrorcap.h"

#include <windows.h>
#include "MinHook.h"
#include <cmath>
#include <cstring>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::mirrorcap {

namespace {

constexpr uint32_t kOptGetBool = 0x117e430, kOptGetFloat = 0x117e520;

using FloatFn = float(__fastcall*)(void* options, const char* name, float fallback);
using BoolFn = bool(__fastcall*)(void* options, const char* name, bool fallback);
FloatFn g_float = nullptr;
BoolFn g_bool = nullptr;
float g_interior = 0, g_exterior = 0;
bool g_reflectionsOff = false;

float __fastcall hookedFloat(void* options, const char* name, float fallback) {
    float v = g_float(options, name, fallback);
    if (!name) return v;
    float cap = 0;
    if (g_interior > 0 && !std::strcmp(name, "InteriorMirrorFarPlane"))
        cap = g_interior;
    else if (g_exterior > 0 && !std::strcmp(name, "ExteriorMirrorFarPlane"))
        cap = g_exterior;
    if (cap > 0 && v > cap) {
        LOG("Mirror cap: %s %.1f m -> %.1f m", name, v, cap);
        return cap;
    }
    return v;
}

bool __fastcall hookedBool(void* options, const char* name, bool fallback) {
    bool v = g_bool(options, name, fallback);
    if (g_reflectionsOff && name && !std::strcmp(name, "RenderReflectionsEnabled") && v) {
        LOG("Mirror cap: reflections off");
        return false;
    }
    return v;
}

}  // namespace

void install(float interiorMetres, float exteriorMetres, bool reflectionsOff) {
    g_interior = interiorMetres > 0 ? interiorMetres : 0;
    g_exterior = exteriorMetres > 0 ? exteriorMetres : 0;
    g_reflectionsOff = reflectionsOff;
    if (!(g_interior > 0 || g_exterior > 0 || g_reflectionsOff)) return;
    if (!gameaddr::recognised() || gameaddr::dx9()) {
        LOG("Mirror cap: game build not recognised; the game's own mirror distance");
        return;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    uint8_t* fl = base + kOptGetFloat;
    uint8_t* bo = base + kOptGetBool;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(fl, reinterpret_cast<void*>(&hookedFloat), reinterpret_cast<void**>(&g_float)) != MH_OK ||
        MH_CreateHook(bo, reinterpret_cast<void*>(&hookedBool), reinterpret_cast<void**>(&g_bool)) != MH_OK ||
        MH_EnableHook(fl) != MH_OK || MH_EnableHook(bo) != MH_OK) {
        LOG("Mirror cap: hook failed; the game's own mirror distance");
        return;
    }
    LOG("Mirror cap: rooms %.1f m, outdoors %.1f m (0 = the game's), reflections %s; takes effect when the game reads its options",
        g_interior, g_exterior, g_reflectionsOff ? "off" : "on");
}

}  // namespace s4vr::mirrorcap
