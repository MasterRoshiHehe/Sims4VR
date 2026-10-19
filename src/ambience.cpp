#include "ambience.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "MinHook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"

namespace s4vr::ambience {
namespace {

// Game 1.128.90.1030 (DX11 rvas; gameaddr maps them for DX9).
// FUN_14185bbf0: distance from a spot (tile coordinates x, z, level) to the
// nearest wall with inside on one side and outside on the other, looking at
// most `radius` tiles around; false if there is none that close.
// FUN_141868e80: AmbienceInsideOutsideGC's value (vtable 0x200b798 slot 5).
// FUN_14188ff10: a GC's curve at x (piecewise linear, held flat past its ends).
// The audio manager the GCs read (DAT_1425dcc48).
constexpr size_t kManagerDistance = 0x3350;  // float: the distance, negative inside
constexpr size_t kManagerOverride = 0x3391;  // char: negative = the game turns the control off (value -2)
constexpr size_t kGcCurve = 0x28;            // the GC's curve
constexpr size_t kGcWidth = 0x40;            // float T: the GC's tuned width

using WallDistanceFn = bool (*)(void* world, float* coords, char lowestLevel, float* out, int flags, float radius,
                                char inside);
using ValueFn = float (*)(uint8_t* gc);
using CurveFn = float (*)(void* curve, float x);

WallDistanceFn g_wallDistance = nullptr;
ValueFn g_value = nullptr;
CurveFn g_curve = nullptr;
uint8_t* g_manager = nullptr;
float g_fade = 0;
std::atomic<bool> g_firstPerson{false};

// The way out through doors and arches (outdoors.py, a few times a second):
// how much of the outdoors reaches you inside, 0..1 (-1: not known, the wall
// distance alone), how much comes through plain walls, and when it was sent.
std::atomic<float> g_openings{-1};
std::atomic<float> g_wallLeak{0.25f};
std::atomic<ULONGLONG> g_openingsAt{0};
std::mutex g_easeMutex;
float g_eased = -1;           // g_openings followed smoothly (a door swinging open fades the sound in)
ULONGLONG g_easedAt = 0;
constexpr float kEaseSeconds = 0.35f;
constexpr ULONGLONG kOpeningsFresh = 2000;  // ms: older than this, back to the wall distance alone

float easedOpenings(float target, ULONGLONG now) {
    std::lock_guard<std::mutex> lock(g_easeMutex);
    if (g_eased < 0 || now - g_easedAt > 1000) {
        g_eased = target;
    } else if (now > g_easedAt) {
        float k = 1 - std::exp(-float(now - g_easedAt) * 0.001f / kEaseSeconds);
        g_eased += (target - g_eased) * k;
    }
    g_easedAt = now;
    return g_eased;
}

// Now and then: what the doors gave, and how often the game asked.
void logOpenings(float distance, float through, float walls, float share) {
    static ULONGLONG last = 0;
    static int logs = 0, asked = 0;
    ++asked;
    ULONGLONG now = GetTickCount64();
    if (logs >= 40 || now - last < 5000) return;
    if (last) {
        ++logs;
        LOG("Ambience: inside %.2f m from the wall to the outside; through doors and arches %.2f, through the walls "
            "%.2f: outdoors at %.0f%% of the way to outside (%d reads in %.0f s)",
            -distance, through, walls, share * 100, asked, (now - last) * 0.001);
    }
    last = now;
    asked = 0;
}

// What the first-person search found, now and then (the first 30).
void logDistance(float radius, bool found, float distance, char inside) {
    static ULONGLONG last = 0;
    static int logs = 0;
    ULONGLONG now = GetTickCount64();
    if (logs >= 30 || now - last < 2000) return;
    last = now;
    ++logs;
    if (found)
        LOG("Ambience: %s, %.2f m from the nearest wall to the %s (the game looks %.1f m out, first person %.1f m)",
            inside ? "inside" : "outside", distance, inside ? "outside" : "inside", radius, g_fade);
    else
        LOG("Ambience: %s, no wall to the %s within %.1f m (the game looks %.1f m out)", inside ? "inside" : "outside",
            inside ? "outside" : "inside", g_fade, radius);
}

bool hookedWallDistance(void* world, float* coords, char lowestLevel, float* out, int flags, float radius,
                        char inside) {
    if (!g_firstPerson.load(std::memory_order_relaxed) || !(g_fade > radius))
        return g_wallDistance(world, coords, lowestLevel, out, flags, radius, inside);
    bool found = g_wallDistance(world, coords, lowestLevel, out, flags, g_fade, inside);
    if (!found) *out = g_fade;
    logDistance(radius, found, *out, inside);
    return true;
}

// Each GC's tuning, the first time first person uses it (8 at most).
void logGc(uint8_t* gc, float width) {
    static std::atomic<uintptr_t> seen[8] = {};
    static std::atomic<int> count{0};
    uintptr_t self = reinterpret_cast<uintptr_t>(gc);
    int n = count.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < 8; ++i)
        if (seen[i].load(std::memory_order_relaxed) == self) return;
    if (n >= 8 || !count.compare_exchange_strong(n, n + 1)) return;
    seen[n].store(self, std::memory_order_relaxed);
    void* curve = gc + kGcCurve;
    LOG("Ambience: inside/outside control %p, width %.2f m; its curve at 0, 0.25, 0.5, 0.75, 1: %.3f %.3f %.3f %.3f "
        "%.3f",
        static_cast<void*>(gc), width, g_curve(curve, 0), g_curve(curve, 0.25f), g_curve(curve, 0.5f),
        g_curve(curve, 0.75f), g_curve(curve, 1));
}

// First person: from the curve's deep-inside value to its deep-outside value,
// smoothly over g_fade metres either side of the wall.
float hookedValue(uint8_t* gc) {
    if (!g_firstPerson.load(std::memory_order_relaxed)) return g_value(gc);
    float width = *reinterpret_cast<float*>(gc + kGcWidth);
    uint8_t* manager = *reinterpret_cast<uint8_t**>(g_manager);
    if (width == 0 || !manager || static_cast<int8_t>(manager[kManagerOverride]) < 0) return g_value(gc);
    float distance = *reinterpret_cast<float*>(manager + kManagerDistance);
    // Outside the outdoors is heard fully, right up to the doorway (user
    // 2026-10-14: no quieter at all until you are inside, also on a porch);
    // inside it fades out over g_fade metres from the outside walls.
    float x = 1 + distance / g_fade;
    if (!(x > 0)) x = 0;
    if (x > 1) x = 1;
    float s = distance >= 0 ? 1.0f : x * x * (3 - 2 * x);
    // Inside, with the way out known: the outdoors comes in through the doors
    // and arches (open ones fully, closed doors muffled) and a little through
    // the walls; in an open doorway it is all of it, as just outside.
    float openings = g_openings.load(std::memory_order_relaxed);
    ULONGLONG now = GetTickCount64();
    if (distance < 0 && openings >= 0 && now - g_openingsAt.load(std::memory_order_relaxed) < kOpeningsFresh) {
        float through = easedOpenings(openings, now);
        float walls = x * g_wallLeak.load(std::memory_order_relaxed);
        s = std::fmax(through, walls);
        logOpenings(distance, through, walls, s);
    }
    void* curve = gc + kGcCurve;
    float in = g_curve(curve, 0), out = g_curve(curve, 1);
    logGc(gc, width);
    return in + (out - in) * s;
}

}  // namespace

void install(float fadeMetres) {
    if (!(fadeMetres > 0)) {
        LOG("Ambience: the game's own inside/outside switch (outdoor_fade 0)");
        return;
    }
    uint8_t* wall = addrs::fn("WallDistance");
    uint8_t* value = addrs::fn("Value");
    g_curve = reinterpret_cast<CurveFn>(addrs::fn("Curve"));
    g_manager = addrs::global("AudioManagerGlobal");
    if (!wall || !value || !g_curve || !g_manager) {
        LOG("Ambience: game build not recognised; the game's own inside/outside switch");
        return;
    }
    g_fade = fadeMetres;
    if (MH_CreateHook(wall, reinterpret_cast<void*>(&hookedWallDistance), reinterpret_cast<void**>(&g_wallDistance)) !=
            MH_OK ||
        MH_CreateHook(value, reinterpret_cast<void*>(&hookedValue), reinterpret_cast<void**>(&g_value)) != MH_OK ||
        MH_EnableHook(wall) != MH_OK || MH_EnableHook(value) != MH_OK) {
        LOG("Ambience: hook failed; the game's own inside/outside switch");
        return;
    }
    LOG("Ambience: in first person the outdoor sounds are full outside and fade out over %.1f m inside", g_fade);
}

void setFirstPerson(bool on) { g_firstPerson.store(on, std::memory_order_relaxed); }

void setOpenings(float through, float wallLeak) {
    if (!(through <= 1)) through = 1;
    if (!(wallLeak >= 0)) wallLeak = 0;
    if (wallLeak > 1) wallLeak = 1;
    g_wallLeak.store(wallLeak, std::memory_order_relaxed);
    g_openings.store(through >= 0 ? through : -1, std::memory_order_relaxed);
    g_openingsAt.store(GetTickCount64(), std::memory_order_relaxed);
}

}  // namespace s4vr::ambience
