#include "shadows.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "simcache.h"
#include "camera.h"
#include "addrs.h"
#include "gameaddr.h"
#include "framemap.h"
#include "log.h"
#include "renderscale.h"
#include "shadowreuse.h"
#include "singleview.h"

namespace s4vr::shadows {

namespace {

// fit(sun, view camera, light camera, settings, cascade, map size); 0x1212bc0 in DX9.
using FitFn = void (*)(const float* sun, const uint8_t* camera, uint8_t* light, float* settings, uint64_t cascade,
                       uint64_t mapSize);

// The fit's own memory, one float4 per cascade (3): the centre it keeps
// (lea rsi,[rip+X] at +0x31a) and the last sun it saw (lea r13,[rip+X] at +0x54a).
constexpr size_t kCentresLea = 0x31a, kSunsLea = 0x54a;
const uint8_t kCentresOp[3] = {0x48, 0x8d, 0x35}, kSunsOp[3] = {0x4c, 0x8d, 0x2d};
constexpr int kCascades = 3;

// The view camera: the fit reads it up to +0x6e7 (which target it looks at);
// +0x24 is the near plane, where cascade 0 starts.
constexpr size_t kCameraBytes = 0x6e8, kNearOffset = 0x24;
constexpr float kShadowNear = 0.1f;
// The light camera: +0x14 is the cascade's width.
constexpr size_t kLightWidth = 0x14;

// The game's config: int get(config, name, fallback), its "prop" values from
// GraphicsRules.sgr. The shadow map is made (and ShadowMapSize read) at start
// and again whenever the display is reset, which our render resolution does
// when the headset view starts or stops.
using ConfigIntFn = int32_t (*)(void* config, const char* name, int32_t fallback);
// One map holds the cascades side by side, mapSize x (mapSize * cascades):
// 4096 x 12288 at most, under the 16384 a texture can be (the game halves
// the texture if it can't make it, but keeps using the size it asked for).
constexpr int kMaxMapSize = 4096;

// Renderer.GlobalShadowSettings as floats: 2 position snap, 3 camera snap,
// then two cascade layouts of 8 at 6 (close camera) and 14 (far camera):
// 4 planes, 2 blend distances, fade-out distance, camera distance it is for.
constexpr int kPositionSnap = 2, kCameraSnap = 3, kLayouts = 6, kLayoutFloats = 16;

FitFn g_fit = nullptr;
const float* g_centres = nullptr;
const float* g_suns = nullptr;
std::atomic<bool> g_on{true};
std::atomic<float> g_step{0.2f};
std::atomic<float> g_sharp{5.0f};  // metres the first cascade reaches in first person (0: the game's)
ConfigIntFn g_configInt = nullptr;
int g_mapSize = 0;
std::atomic<bool> g_ssaoOff{false};

// The fit's thread only from here on.
bool g_active = false;
float* g_settings = nullptr;   // where the layouts were changed
float g_saved[kLayoutFloats];  // the game's layouts
float g_written[kLayoutFloats];
float g_sharpUsed = -1;
bool g_loggedSettings = false;
float g_held[4];
bool g_haveHeld = false;
alignas(16) uint8_t g_firstView[kCascades][kCameraBytes];  // the first (left eye) pass's fit cameras
bool g_haveFirst[kCascades] = {};

struct Stats {
    unsigned fits = 0, snapped = 0, steps = 0, mapSize = 0;
    unsigned moves[kCascades] = {};
    float width[kCascades] = {};
    ULONGLONG since = 0;
} g_stats;
constexpr ULONGLONG kLogEvery = 10000;

bool finite4(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
}

bool same3(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

// The sun the fit gets: the real one once it has moved `step` degrees.
void holdSun(const float* sun, float out[4]) {
    float step = g_step.load(std::memory_order_relaxed);
    if (step > 0 && g_haveHeld) {
        float a = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        float b = std::sqrt(g_held[0] * g_held[0] + g_held[1] * g_held[1] + g_held[2] * g_held[2]);
        float d = (sun[0] * g_held[0] + sun[1] * g_held[1] + sun[2] * g_held[2]) / std::max(a * b, 1e-12f);
        if (d >= std::cos(step * 3.14159265f / 180.f)) {
            std::memcpy(out, g_held, sizeof(g_held));
            return;
        }
    }
    std::memcpy(g_held, sun, sizeof(g_held));
    g_haveHeld = true;
    ++g_stats.steps;
    std::memcpy(out, g_held, sizeof(g_held));
}

bool saneLayout(const float* l) {
    for (int i = 0; i < 8; ++i)
        if (!std::isfinite(l[i])) return false;
    return l[0] >= 0 && l[0] <= l[1] && l[1] <= l[2] && l[2] <= l[3];
}

// One layout for every camera distance, the close camera's, with its first
// two cascades pulled in to `sharp` metres (the rest in proportion), so the
// shadows near you get most of the map's texels. A cascade covers the whole
// width of the view out to its far end (about 4 m across per metre with the
// headset's field of view), so the game's 10 m first cascade was 42 m wide.
void fixLayout(float* settings) {
    float* layouts = settings + kLayouts;
    float sharp = g_sharp.load(std::memory_order_relaxed);
    if (g_settings == settings && std::memcmp(layouts, g_written, sizeof(g_written)) == 0 && sharp == g_sharpUsed) return;
    // First time, or the game loaded its settings again: these are the game's.
    if (g_settings != settings || std::memcmp(layouts, g_written, sizeof(g_written)) != 0)
        std::memcpy(g_saved, layouts, sizeof(g_saved));
    g_settings = settings;
    g_sharpUsed = sharp;
    const float* closeCam = g_saved;
    const float* farCam = g_saved + 8;
    if (!g_loggedSettings) {
        g_loggedSettings = true;
        LOG("Shadows: the game's settings: centre snap %.2f m, size snap %.2f m; close camera (%.1f m) cascades "
            "%.1f %.1f %.1f %.1f m, blend %.1f %.1f, fade out %.1f m; far camera (%.1f m) cascades %.1f %.1f %.1f "
            "%.1f m, blend %.1f %.1f, fade out %.1f m",
            settings[kPositionSnap], settings[kCameraSnap], closeCam[7], closeCam[0], closeCam[1], closeCam[2], closeCam[3],
            closeCam[4], closeCam[5], closeCam[6], farCam[7], farCam[0], farCam[1], farCam[2], farCam[3], farCam[4], farCam[5], farCam[6]);
    }
    if (!saneLayout(closeCam) || !saneLayout(farCam)) {
        std::memcpy(g_written, layouts, sizeof(g_written));
        return;
    }
    float fixed[8];
    std::memcpy(fixed, closeCam, sizeof(fixed));
    if (sharp > 0 && closeCam[1] > sharp) {
        float k = sharp / closeCam[1];
        fixed[1] = sharp;
        fixed[2] = std::min(closeCam[2] * k, fixed[3]);
        fixed[4] = closeCam[4] * k;  // blends between cascades
        fixed[5] = closeCam[5] * k;
    }
    std::memcpy(layouts, fixed, 7 * sizeof(float));      // keeps its camera distance
    std::memcpy(layouts + 8, fixed, 7 * sizeof(float));  // (the blend divides by their difference)
    std::memcpy(g_written, layouts, sizeof(g_written));
    LOG("Shadows: first person cascades %.1f %.1f %.1f %.1f m, blend %.1f %.1f, fade out %.1f m", fixed[0], fixed[1],
        fixed[2], fixed[3], fixed[4], fixed[5], fixed[6]);
}

void restoreLayout() {
    if (!g_settings) return;
    float* layouts = g_settings + kLayouts;
    if (std::memcmp(layouts, g_written, sizeof(g_written)) == 0) std::memcpy(layouts, g_saved, sizeof(g_saved));
    g_settings = nullptr;
}

float texel(int cascade) {
    return g_stats.mapSize ? g_stats.width[cascade] * 100.f / static_cast<float>(g_stats.mapSize) : 0.f;
}

void logStats() {
    ULONGLONG now = GetTickCount64();
    if (!g_stats.since) g_stats.since = now;
    if (now - g_stats.since < kLogEvery) return;
    LOG("Shadows (first person): %u fits, %u kept still to the texel, the sun moved %u times; cascades %.1f / %.1f "
        "/ %.1f m wide (%.1f / %.1f / %.1f cm a texel at %u px), their centres moved %u / %u / %u times",
        g_stats.fits, g_stats.snapped, g_stats.steps, g_stats.width[0], g_stats.width[1], g_stats.width[2],
        texel(0), texel(1), texel(2), g_stats.mapSize, g_stats.moves[0], g_stats.moves[1], g_stats.moves[2]);
    g_stats = Stats();
    g_stats.since = now;
}

void steadyFit(const float* sun, const uint8_t* camera, uint8_t* light, float* settings, uint64_t cascadeArg,
               uint64_t mapSize) {
    int cascade = static_cast<int>(static_cast<uint32_t>(cascadeArg));
    singleview::fitted();
    framemap::mark(framemap::ShadowFit, cascade);
    bool want = g_on.load(std::memory_order_relaxed) && camera::bodyActive() && sun && camera && light &&
                settings && cascade >= 0 && cascade < kCascades && finite4(sun);
    if (!want) {
        if (g_active) {
            g_active = false;
            g_haveHeld = false;
            restoreLayout();
            g_stats = Stats();
            LOG("Shadows: the game's own again");
        }
        g_fit(sun, camera, light, settings, cascadeArg, mapSize);
        return;
    }
    if (!g_active) {
        g_active = true;
        LOG("Shadows: steady in first person (the sun moves them in %.2f degree steps, near %.2f m, map %u px)",
            g_step.load(), kShadowNear, static_cast<unsigned>(mapSize));
    }
    fixLayout(settings);
    alignas(16) float held[4];
    holdSun(sun, held);
    alignas(16) uint8_t view[kCameraBytes];
    std::memcpy(view, camera, sizeof(view));
    std::memcpy(view + kNearOffset, &kShadowNear, sizeof(float));
    // Both real eyes with the right eye reusing the left eye's shadow map (shadowreuse.h): the right
    // eye's pass fits each cascade from the left eye's camera, so its light matrices match the map.
    if (!singleview::inSecond()) {
        std::memcpy(g_firstView[cascade], view, sizeof(view));
        g_haveFirst[cascade] = true;
    } else if (shadowreuse::on() && g_haveFirst[cascade]) {
        std::memcpy(view, g_firstView[cascade], sizeof(view));
        g_haveFirst[cascade] = false;
    }

    float centre[4] = {};
    if (g_centres) std::memcpy(centre, g_centres + 4 * cascade, sizeof(centre));
    if (g_suns && same3(g_suns + 4 * cascade, held)) ++g_stats.snapped;
    g_fit(held, view, light, settings, cascadeArg, mapSize);
    ++g_stats.fits;
    if (g_centres && !same3(g_centres + 4 * cascade, centre)) ++g_stats.moves[cascade];
    float width;
    std::memcpy(&width, light + kLightWidth, sizeof(width));
    g_stats.width[cascade] = width;
    g_stats.mapSize = static_cast<unsigned>(mapSize);
    logStats();
}

// The game asks its config for ShadowMapSize when it makes the shadow map.
int32_t sizedConfig(void* config, const char* name, int32_t fallback) {
    int32_t value = simcache::intValue(name, g_configInt(config, name, fallback));
    if (name && name[0] == 'S' && g_ssaoOff.load(std::memory_order_relaxed) && value && !std::strcmp(name, "SsaoEnabled")) {
        LOG("Shadows: SsaoEnabled read as an int, forced off");
        return 0;
    }
    if (!name || name[0] != 'S' || std::strcmp(name, "ShadowMapSize") != 0) return value;
    int32_t want = renderscale::scaled() ? g_mapSize : 0;
    int32_t give = want > value ? want : value;
    if (give != value)
        LOG("Shadows: the game makes its shadow map at %d px (headset on; its own size is %d px)", give, value);
    else
        LOG("Shadows: the game makes its shadow map at %d px", value);
    return give;
}

const float* leaTarget(uint8_t* fit, size_t at, const uint8_t (&op)[3]) {
    if (std::memcmp(fit + at, op, sizeof(op)) != 0) return nullptr;
    int32_t disp;
    std::memcpy(&disp, fit + at + 3, sizeof(disp));
    return reinterpret_cast<const float*>(fit + at + 7 + disp);
}

}  // namespace

void configure(bool on, float stepDegrees, float sharpRange) {
    g_on.store(on);
    g_step.store(std::isfinite(stepDegrees) ? std::clamp(stepDegrees, 0.f, 5.f) : 0.2f);
    g_sharp.store(std::isfinite(sharpRange) ? std::clamp(sharpRange, 0.f, 50.f) : 5.f);
}

void install(bool on, float stepDegrees, float sharpRange, int mapSize, bool ssaoOff) {
    configure(on, stepDegrees, sharpRange);
    g_ssaoOff.store(ssaoOff);
    // A power of two from 1024 to 4096, or 0 for the game's own.
    int size = 0;
    if (mapSize > 0)
        for (size = 1024; size * 2 <= std::min(mapSize, kMaxMapSize);) size *= 2;
    g_mapSize = size;
    {
        uint8_t* get = addrs::fn("ConfigInt");
        MH_STATUS status = MH_Initialize();
        if (get && (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)) {
            status = MH_CreateHook(get, reinterpret_cast<void*>(&sizedConfig), reinterpret_cast<void**>(&g_configInt));
            if (status == MH_OK) status = MH_EnableHook(get);
            LOG("Shadows: shadow map %d px while the headset view is on (SSAO %s): %s", g_mapSize, ssaoOff ? "off" : "on", MH_StatusToString(status));
        } else {
            LOG("Shadows: game build not recognised; the game's own shadow map size");
        }
    }
    uint8_t* fit = addrs::fn("Fit");
    if (!fit) {
        LOG("Shadows: game build not recognised; the game's own in first person too");
        return;
    }
    g_centres = leaTarget(fit, kCentresLea, kCentresOp);
    g_suns = leaTarget(fit, kSunsLea, kSunsOp);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    status = MH_CreateHook(fit, reinterpret_cast<void*>(&steadyFit), reinterpret_cast<void**>(&g_fit));
    if (status == MH_OK) status = MH_EnableHook(fit);
    LOG("Shadows: sun shadow fit hooked at game+0x%llx: %s%s", static_cast<unsigned long long>(fit - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))),
        MH_StatusToString(status), g_centres && g_suns ? "" : " (no counters)");
}

}  // namespace s4vr::shadows
