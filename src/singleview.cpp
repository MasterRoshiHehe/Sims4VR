#include "singleview.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "MinHook.h"
#include "addrs.h"
#include "camera.h"
#include "log.h"

namespace s4vr::singleview {
namespace {

// Scene render (renderer, viewport rect, seconds since the last frame): records
// shadows, reflections, the main view and the final copy into the gfx deferred
// contexts (job slots 6..10) and waits for those jobs. The time step goes to
// reflections and the effect update (0x1378030).
using SceneFn = void (*)(void* renderer, const void* viewport, float seconds);
SceneFn g_scene = nullptr;

std::atomic<bool> g_probe{false};
std::atomic<uint32_t> g_presents{0};
std::atomic<uint32_t> g_culls[2] = {};

int64_t ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

const double g_msPerTick = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 / static_cast<double>(f.QuadPart);
}();

// Main thread only.
struct Sums {
    int frames = 0, doubled = 0;
    double first = 0, firstWorst = 0, second = 0, secondWorst = 0, between = 0, betweenWorst = 0;
};
Sums g_sums;
int64_t g_lastStart = 0, g_logStart = 0;
bool g_wasProbing = false;

void log(const Sums& s, double seconds) {
    const uint32_t presents = g_presents.exchange(0, std::memory_order_relaxed);
    const uint32_t main = g_culls[0].exchange(0, std::memory_order_relaxed);
    const uint32_t other = g_culls[1].exchange(0, std::memory_order_relaxed);
    auto per = [](double total, int n) { return n ? total / n : 0.0; };
    LOG("Single view (last %.0f s, %d frames, %.1f a second): scene render %.2f ms on the main thread (worst %.1f); "
        "second pass %d frames, %.2f ms (worst %.1f); main frame every %.1f ms (worst %.0f); %.2f pictures, room "
        "culls %.2f main / %.2f other views a frame",
        seconds, s.frames, s.frames / seconds, per(s.first, s.frames), s.firstWorst, s.doubled,
        per(s.second, s.doubled), s.secondWorst, per(s.between, s.frames), s.betweenWorst,
        s.frames ? double(presents) / s.frames : 0.0, s.frames ? double(main) / s.frames : 0.0,
        s.frames ? double(other) / s.frames : 0.0);
}

void hookedScene(void* renderer, const void* viewport, float seconds) {
    const int64_t start = ticks();
    // Both real eyes every frame (camera::setBothEyes): the left eye, then the right eye from the
    // same place with no time step, so both pictures show the same moment.
    const bool both = camera::beginBoth();
    g_scene(renderer, viewport, seconds);
    const int64_t mid = ticks();
    if (both && camera::secondEye()) {
        g_scene(renderer, viewport, 0.0f);
        const double ms = (ticks() - mid) * g_msPerTick;
        ++g_sums.doubled;
        g_sums.second += ms;
        g_sums.secondWorst = std::max(g_sums.secondWorst, ms);
    }
    const bool probe = g_probe.load(std::memory_order_relaxed) && !both;
    if (probe != g_wasProbing) {
        g_wasProbing = probe;
        LOG("Single view: probe %s (the scene is drawn %s a frame)", probe ? "on" : "off", probe ? "twice" : "once");
    }
    Sums& s = g_sums;
    if (probe) {
        // Same camera, no time step: what a second eye's pass costs.
        g_scene(renderer, viewport, 0.0f);
        const double ms = (ticks() - mid) * g_msPerTick;
        ++s.doubled;
        s.second += ms;
        s.secondWorst = std::max(s.secondWorst, ms);
    }
    const double ms = (mid - start) * g_msPerTick;
    ++s.frames;
    s.first += ms;
    s.firstWorst = std::max(s.firstWorst, ms);
    if (g_lastStart) {
        const double between = (start - g_lastStart) * g_msPerTick;
        s.between += between;
        s.betweenWorst = std::max(s.betweenWorst, between);
    }
    g_lastStart = start;
    if (!g_logStart) g_logStart = start;
    const double secs = (start - g_logStart) * g_msPerTick / 1000.0;
    if (secs >= 10) {
        log(s, secs);
        s = Sums{};
        g_logStart = start;
    }
}

}  // namespace

void install() {
    void* target = addrs::fn("SceneRender");
    if (!target) {
        LOG("Single view: game build not recognised; off");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedScene), reinterpret_cast<void**>(&g_scene)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Single view: hook failed; off");
        g_scene = nullptr;
        return;
    }
    LOG("Single view: scene render timed");
}

void setProbe(bool on) { g_probe.store(on, std::memory_order_relaxed); }

void presented() { g_presents.fetch_add(1, std::memory_order_relaxed); }

void culled(int kind) { g_culls[kind == 0 ? 0 : 1].fetch_add(1, std::memory_order_relaxed); }

}  // namespace s4vr::singleview
