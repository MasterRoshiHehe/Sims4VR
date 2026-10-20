#include "simcache.h"

#include <windows.h>
#include "MinHook.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::simcache {

namespace {

constexpr uint32_t kOptGetUInt = 0x117e660, kOptGetVec4 = 0x117e770;

using UIntFn = uint32_t(__fastcall*)(void* options, const char* name, uint32_t fallback);
using Vec4Fn = bool(__fastcall*)(void* options, const char* name, float* out);
UIntFn g_uint = nullptr;
Vec4Fn g_vec = nullptr;
bool g_boost = false;
int g_cacheMb = 256;
float g_lodScale = 1;

uint32_t __fastcall hookedUInt(void* options, const char* name, uint32_t fallback) {
    uint32_t v = g_uint(options, name, fallback);
    if (name && name[0] == 'D' && !std::strcmp(name, "DeformerMapsCacheBudget")) {
        uint64_t give = g_boost ? std::min<uint64_t>(static_cast<uint64_t>(v) * 4, 0x7fffffffull) : v;
        LOG("Sim cache: DeformerMapsCacheBudget game value %u bytes (default %u) -> %llu", v, fallback, static_cast<unsigned long long>(give));
        return static_cast<uint32_t>(give);
    }
    return v;
}

bool __fastcall hookedVec(void* options, const char* name, float* out) {
    bool ok = g_vec(options, name, out);
    if (ok && out && name && name[0] == 'R' && !std::strcmp(name, "RenderSimLODDistances")) {
        float before[4];
        std::memcpy(before, out, sizeof(before));
        if (g_lodScale != 1)
            for (int i = 0; i < 4; ++i) out[i] *= g_lodScale;
        LOG("Sim cache: RenderSimLODDistances game %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f", before[0], before[1], before[2], before[3],
            out[0], out[1], out[2], out[3]);
    }
    return ok;
}

}  // namespace

int32_t intValue(const char* name, int32_t value) {
    if (!name || name[0] != 'S' || std::strcmp(name, "SimCacheSizeLimit") != 0) return value;
    int32_t give = value;
    if (g_boost) give = value > 0 ? static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(value) * 4, 0x7fff)) : g_cacheMb;
    LOG("Sim cache: SimCacheSizeLimit game value %d MB (0 = unset) -> %d MB", value, give);
    return give;
}

void install(bool boost, int cacheMb, float lodScale) {
    g_boost = boost;
    g_cacheMb = std::clamp(cacheMb, 16, 2048);
    g_lodScale = std::isfinite(lodScale) ? std::clamp(lodScale, 1.f, 4.f) : 1.f;
    if (!gameaddr::recognised() || gameaddr::dx9()) {
        LOG("Sim cache: game build not recognised; the game's own values");
        return;
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    uint8_t* u = base + kOptGetUInt;
    uint8_t* v = base + kOptGetVec4;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(u, reinterpret_cast<void*>(&hookedUInt), reinterpret_cast<void**>(&g_uint)) != MH_OK ||
        MH_CreateHook(v, reinterpret_cast<void*>(&hookedVec), reinterpret_cast<void**>(&g_vec)) != MH_OK ||
        MH_EnableHook(u) != MH_OK || MH_EnableHook(v) != MH_OK) {
        LOG("Sim cache: hook failed; the game's own values");
        return;
    }
    LOG("Sim cache: caches %s (x4, SimCacheSizeLimit unset -> %d MB), LOD distances x%.2f; takes effect when the game reads its options",
        g_boost ? "raised" : "as the game's", g_cacheMb, g_lodScale);
}

}  // namespace s4vr::simcache
