#include "singleview.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "MinHook.h"
#include "addrs.h"
#include "camera.h"
#include "casbody.h"
#include "passtime.h"
#include "log.h"
#include "uihook.h"

namespace s4vr::singleview {
namespace {

// Scene render (renderer, viewport rect, seconds since the last frame): records
// shadows, reflections, the main view and the final copy into the gfx deferred
// contexts (job slots 6..10) and waits for those jobs. The time step goes to
// reflections and the effect update (0x1378030).
using SceneFn = void (*)(void* renderer, const void* viewport, float seconds);
SceneFn g_scene = nullptr;

// The scene's render jobs (SceneJob 0x1379b20: task, data; data+0x20 the part's mode).
using JobFn = void (*)(void* task, uint8_t* data);
JobFn g_job = nullptr;
thread_local int t_part = -1;  // index of the part this job thread is drawing (-1 none)
// Modes 3, 4, 8, other; then the scene render's own steps on the main thread (v0.13.312, which of
// them holds the sun shadow fits and the shadow/mirror culls): FrameBegin 0x11c1a60, SceneViews
// 0x11c45a0 (views, before its draw lists), SceneLists 0x11c46d0 (draw lists, may run as a job).
constexpr int kParts = 7;
constexpr int kBegin = 4, kViews = 5, kLists = 6;
std::atomic<uint32_t> g_partRuns[kParts] = {}, g_partFits[kParts] = {}, g_partCulls[kParts][2] = {};
std::atomic<uint64_t> g_partTicks[kParts] = {};
std::atomic<uint32_t> g_skipped{0};
std::atomic<int> g_secondSkip{0};
std::atomic<bool> g_inSecond{false};

int partIndex(uint32_t mode) { return mode == 3 ? 0 : mode == 4 ? 1 : mode == 8 ? 2 : 3; }

int64_t ticks();

// FrameBegin takes the time step in xmm1 (movaps xmm6, xmm1), the others integers only.
using BeginFn = uint64_t (*)(void* renderer, float seconds);
using StepFn = uint64_t (*)(void* a, uint64_t b, uint64_t c, uint64_t d);
void* g_step[3] = {};  // FrameBegin, SceneViews, SceneLists (originals)

// One of the scene render's steps, tagged and timed like a part.
uint64_t runStep(int part, void* a, uint64_t b, uint64_t c, uint64_t d, float seconds = 0) {
    const int was = t_part;
    t_part = part;
    const int64_t start = ticks();
    const uint64_t r = part == kBegin ? reinterpret_cast<BeginFn>(g_step[0])(a, seconds)
                                      : reinterpret_cast<StepFn>(g_step[part - kBegin])(a, b, c, d);
    // SceneViews' time includes SceneLists' when that runs inline (renderer+0x6d3 off).
    g_partTicks[part].fetch_add(static_cast<uint64_t>(ticks() - start), std::memory_order_relaxed);
    g_partRuns[part].fetch_add(1, std::memory_order_relaxed);
    t_part = was;
    return r;
}
// Leaving FrameBegin out of the second pass crashed the game in the next job submit (v0.13.317).
// The right eye's FrameBegin rebuilds the water reflection state (0x11cefe0: which waters are active, their cameras at
// renderer+0x68/+0x70) while the left eye's draw-lists job (SceneLists -> ListMirrors 0x11c99f0, which draws the water
// reflections from that state) can still be running on a job thread: the left eye's lot water (the pool) was drawn
// with the right eye's flags and camera, or not at all, so the pool differed per eye (user 2026-11-01; the mirror pass
// copies its camera into its own stack camera and was spared). SceneViews submits that job and the game only waits
// for it at the next SceneViews. Count the jobs submitted and finished and, in the second pass, wait for the left eye's
// water stage before FrameBegin. Bounded: a missed count costs the time-out once, not every frame.
double msPerTick();
std::atomic<uint32_t> g_listsSubmitted{0};
std::atomic<int32_t> g_listsSkew{0};
std::atomic<uint32_t> g_waterWaits{0}, g_waterTimeouts{0};
std::atomic<void*> g_mainScene{nullptr};
uint64_t hookedBegin(void* renderer, float seconds) {
    if (g_inSecond.load(std::memory_order_acquire) && g_mainScene.load(std::memory_order_relaxed) != renderer) {
        g_mainScene.store(renderer, std::memory_order_relaxed);  // only the main view is drawn twice
        passtime::setMainScene(renderer);
        g_listsSkew.store(1);  // the first left pass's lists job was not counted (a time-out corrects a wrong guess)
    }
    if (renderer && renderer == g_mainScene.load(std::memory_order_relaxed)) {
        // Every main-view FrameBegin: the right eye's (left eye's job) and the next frame's left eye (right eye's job).
        const uint32_t want = g_listsSubmitted.load(std::memory_order_acquire) + static_cast<uint32_t>(g_listsSkew.load());
        const int64_t start = ticks();
        if (static_cast<int32_t>(passtime::waterDone() - want) < 0) {
            g_waterWaits.fetch_add(1, std::memory_order_relaxed);
            while (static_cast<int32_t>(passtime::waterDone() - want) < 0) {
                if ((ticks() - start) * msPerTick() > 8.0) {
                    g_waterTimeouts.fetch_add(1, std::memory_order_relaxed);
                    g_listsSkew.fetch_add(static_cast<int32_t>(passtime::waterDone() - want));
                    break;
                }
                YieldProcessor();
            }
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("Single view: the main view waits for the previous water reflection stage before rebuilding it");
        }
    }
    const uint64_t r = runStep(kBegin, renderer, 0, 0, 0, seconds);
    passtime::noteFrameBegin(renderer, g_inSecond.load(std::memory_order_acquire) ? 1 : 0);
    return r;
}
uint64_t hookedViews(void* a, uint64_t b, uint64_t c, uint64_t d) {
    // SceneViews returns early (no draw lists) when its third argument is 0 and renderer+0x6d6 is not 0xff.
    const bool lists = (c & 0xff) != 0 || (a && *(reinterpret_cast<uint8_t*>(a) + 0x6d6) == 0xff);
    const uint64_t r = runStep(kViews, a, b, c, d);
    if (lists && a == g_mainScene.load(std::memory_order_relaxed)) g_listsSubmitted.fetch_add(1, std::memory_order_release);
    return r;
}
uint64_t hookedLists(void* a, uint64_t b, uint64_t c, uint64_t d) { return runStep(kLists, a, b, c, d); }

std::atomic<bool> g_probe{false};
std::atomic<uint32_t> g_presents{0};
std::atomic<uint32_t> g_culls[2] = {};
std::atomic<uint32_t> g_secondFits{0}, g_secondCulls{0};  // in the second (right eye) pass, any thread

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

double msPerTick() { return g_msPerTick; }

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
    const uint32_t secondFits = g_secondFits.exchange(0, std::memory_order_relaxed);
    const uint32_t secondCulls = g_secondCulls.exchange(0, std::memory_order_relaxed);
    auto per = [](double total, int n) { return n ? total / n : 0.0; };
    uint32_t ui = 0, uiSecond = 0;
    uihook::takeRenders(ui, uiSecond);
    {
        // Per scene part: runs, CPU ms each (on its job thread), sun shadow fits and room culls.
        char parts[900];
        int n = 0;
        static const char* names[kParts] = {"3", "4", "8", "other", "frame begin", "views", "draw lists"};
        for (int i = 0; i < kParts; ++i) {
            const uint32_t runs = g_partRuns[i].exchange(0, std::memory_order_relaxed);
            const uint64_t t = g_partTicks[i].exchange(0, std::memory_order_relaxed);
            const uint32_t fits = g_partFits[i].exchange(0, std::memory_order_relaxed);
            const uint32_t cm = g_partCulls[i][0].exchange(0, std::memory_order_relaxed);
            const uint32_t co = g_partCulls[i][1].exchange(0, std::memory_order_relaxed);
            if (!runs && !fits && !cm && !co) continue;
            n += std::snprintf(parts + n, sizeof(parts) - n, "%s%s: %u runs %.2f ms, %u shadow fits, culls %u/%u",
                               n ? "; " : "", names[i], runs, runs ? t * g_msPerTick / runs : 0.0, fits, cm, co);
            if (n >= static_cast<int>(sizeof(parts))) break;
        }
        LOG("Scene parts (last %.0f s, mode: runs, CPU ms each, shadow fits, culls main/other): %s; second eye left "
            "out part %d %u times; in second passes %u shadow fits, %u culls",
            seconds, n ? parts : "none", g_secondSkip.load(), g_skipped.exchange(0), secondFits, secondCulls);
    }
    LOG("Single view: the water state was put back %u times (last %.0f s)", passtime::stateRestores(), seconds);
    LOG("Single view: the right eye waited for the water stage %u times, %u timed out (last %.0f s)",
        g_waterWaits.exchange(0), g_waterTimeouts.exchange(0), seconds);
    LOG("Single view (last %.0f s, %d frames, %.1f a second): scene render %.2f ms on the main thread (worst %.1f); "
        "second pass %d frames, %.2f ms (worst %.1f); main frame every %.1f ms (worst %.0f); %.2f pictures, room "
        "culls %.2f main / %.2f other views a frame; UI drawn %u times (%u in second passes)",
        seconds, s.frames, s.frames / seconds, per(s.first, s.frames), s.firstWorst, s.doubled,
        per(s.second, s.doubled), s.secondWorst, per(s.between, s.frames), s.betweenWorst,
        s.frames ? double(presents) / s.frames : 0.0, s.frames ? double(main) / s.frames : 0.0,
        s.frames ? double(other) / s.frames : 0.0, ui, uiSecond);
}

void hookedJob(void* task, uint8_t* data) {
    if (!data || data[0x1c]) return g_job(task, data);
    const uint32_t mode = *reinterpret_cast<uint32_t*>(data + 0x20);
    const int part = partIndex(mode);
    const int skip = g_secondSkip.load(std::memory_order_relaxed);
    if (skip && static_cast<uint32_t>(skip) == mode && g_inSecond.load(std::memory_order_acquire)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int was = t_part;
    t_part = part;
    const int64_t start = ticks();
    g_job(task, data);
    g_partTicks[part].fetch_add(static_cast<uint64_t>(ticks() - start), std::memory_order_relaxed);
    g_partRuns[part].fetch_add(1, std::memory_order_relaxed);
    t_part = was;
}

void hookedScene(void* renderer, const void* viewport, float seconds) {
    const int64_t start = ticks();
    // Both real eyes every frame (camera::setBothEyes): the left eye, then the right eye from the
    // same place with no time step, so both pictures show the same moment.
    camera::casFeedScene(renderer);
    const bool both = camera::beginBoth();
    if (camera::casActive()) casbody::newFrame();  // the CAS body: one for this frame's passes, both eyes (casbody.h)
    g_scene(renderer, viewport, seconds);
    if (camera::casActive()) camera::notePass(0);
    const int64_t mid = ticks();
    if (both && camera::secondEye()) {
        uihook::beginSecondPass();
        g_inSecond.store(true, std::memory_order_release);
        g_scene(renderer, viewport, 0.0f);
        g_inSecond.store(false, std::memory_order_release);
        if (camera::casActive()) camera::notePass(1);
        uihook::endSecondPass();
        // A photo session: a third pass from the phone's lens (the picture for the phone's glass).
        if (camera::photoSession() && camera::thirdEye()) {
            uihook::beginSecondPass();
            uihook::setLensPass(true);
            camera::setInLensPass(true);
            g_inSecond.store(true, std::memory_order_release);
            g_scene(renderer, viewport, 0.0f);
            g_inSecond.store(false, std::memory_order_release);
            camera::setInLensPass(false);
            uihook::setLensPass(false);
            uihook::endSecondPass();
        }
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
    if (void* job = addrs::fn("SceneJob")) {
        if (MH_CreateHook(job, reinterpret_cast<void*>(&hookedJob), reinterpret_cast<void**>(&g_job)) == MH_OK &&
            MH_EnableHook(job) == MH_OK)
            LOG("Single view: scene parts timed");
        else
            g_job = nullptr;
    }
    const char* steps[3] = {"FrameBegin", "SceneViews", "SceneLists"};
    void* detours[3] = {reinterpret_cast<void*>(&hookedBegin), reinterpret_cast<void*>(&hookedViews),
                        reinterpret_cast<void*>(&hookedLists)};
    int hooked = 0;
    for (int i = 0; i < 3; ++i) {
        void* fn = addrs::fn(steps[i]);
        MH_STATUS status = fn ? MH_CreateHook(fn, detours[i], &g_step[i]) : MH_ERROR_NOT_EXECUTABLE;
        if (status == MH_OK) status = MH_EnableHook(fn);
        if (status == MH_OK) {
            ++hooked;
        } else {
            LOG("Single view: step %s not timed (%s)", steps[i], fn ? MH_StatusToString(status) : "address not found");
            g_step[i] = nullptr;
        }
    }
    LOG("Single view: %d of 3 scene steps timed", hooked);
}

void setProbe(bool on) { g_probe.store(on, std::memory_order_relaxed); }

void presented() { g_presents.fetch_add(1, std::memory_order_relaxed); }

void culled(int kind) {
    g_culls[kind == 0 ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
    if (g_inSecond.load(std::memory_order_relaxed)) g_secondCulls.fetch_add(1, std::memory_order_relaxed);
    if (t_part >= 0) g_partCulls[t_part][kind == 0 ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
}

void fitted() {
    if (g_inSecond.load(std::memory_order_relaxed)) g_secondFits.fetch_add(1, std::memory_order_relaxed);
    if (t_part >= 0) g_partFits[t_part].fetch_add(1, std::memory_order_relaxed);
}

bool inSecond() { return g_inSecond.load(std::memory_order_relaxed); }

void setSecondSkip(int mode) {
    if (g_secondSkip.exchange(mode) != mode) LOG("Both eyes: the second eye leaves out scene part %d (0 = none)", mode);
}

}  // namespace s4vr::singleview
