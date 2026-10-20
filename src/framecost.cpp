#include "framecost.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "hitch.h"
#include "log.h"

namespace s4vr::framecost {
namespace {

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

// This picture's marks (0: none yet), taken back at its Present.
std::atomic<int64_t> g_camera{0}, g_shadowFirst{0}, g_shadowLast{0}, g_eyeFirst{0}, g_eyeLast{0};
int64_t g_lastEnd = 0;  // the previous Present returned (Present's thread only)
int64_t g_start = 0;    // this Present was called

struct Sums {
    int frames = 0, withCamera = 0, withShadows = 0, withEyes = 0, withDraws = 0, shadowsFirst = 0;
    double frame = 0, worst = 0, before = 0, shadows = 0, eyes = 0, after = 0, present = 0;
};
Sums g_sums;
int64_t g_logStart = 0;

void first(std::atomic<int64_t>& at, int64_t now) {
    int64_t none = 0;
    at.compare_exchange_strong(none, now, std::memory_order_relaxed);
}

void latest(std::atomic<int64_t>& at, int64_t now) {
    int64_t seen = at.load(std::memory_order_relaxed);
    while (seen < now && !at.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

void log(const Sums& s, double seconds) {
    auto per = [](double total, int n) { return n ? total / n : 0.0; };
    auto share = [&s](int n) { return 100.0 * n / s.frames; };
    LOG("Frame time (last %.0f s, %d pictures, %.1f a second): the game %.1f ms a picture (worst %.0f); its update "
        "before drawing %.1f ms (%.0f%% of pictures); Sims for shadows and mirrors drawn over %.1f ms (%.0f%%), in "
        "your view over %.1f ms (%.0f%%), shadows first in %.0f%%; %.1f ms after the last Sim; Present with the VR "
        "work and its wait %.1f ms",
        seconds, s.frames, s.frames / seconds, per(s.frame, s.frames), s.worst, per(s.before, s.withCamera),
        share(s.withCamera), per(s.shadows, s.withShadows), share(s.withShadows), per(s.eyes, s.withEyes),
        share(s.withEyes), share(s.shadowsFirst), per(s.after, s.withDraws), per(s.present, s.frames));
}

}  // namespace

void renderCamera() {
    hitch::mainThreadSeen();
    first(g_camera, ticks());
}

void simDraw(bool eyes) {
    const int64_t now = ticks();
    first(eyes ? g_eyeFirst : g_shadowFirst, now);
    latest(eyes ? g_eyeLast : g_shadowLast, now);
}

void presentStart() {
    hitch::presentStart();
    g_start = ticks();
}

void presentEnd() {
    hitch::presentEnd();
    const int64_t end = ticks();
    const int64_t camera = g_camera.exchange(0, std::memory_order_relaxed);
    const int64_t shadowFirst = g_shadowFirst.exchange(0, std::memory_order_relaxed);
    const int64_t shadowLast = g_shadowLast.exchange(0, std::memory_order_relaxed);
    const int64_t eyeFirst = g_eyeFirst.exchange(0, std::memory_order_relaxed);
    const int64_t eyeLast = g_eyeLast.exchange(0, std::memory_order_relaxed);
    Sums& s = g_sums;
    if (g_lastEnd && g_start > g_lastEnd) {
        const double frame = (g_start - g_lastEnd) * g_msPerTick;
        ++s.frames;
        s.frame += frame;
        s.worst = std::max(s.worst, frame);
        s.present += (end - g_start) * g_msPerTick;
        if (camera > g_lastEnd && camera <= g_start) {
            ++s.withCamera;
            s.before += (camera - g_lastEnd) * g_msPerTick;
        }
        if (shadowFirst && shadowLast >= shadowFirst) {
            ++s.withShadows;
            s.shadows += (shadowLast - shadowFirst) * g_msPerTick;
        }
        if (eyeFirst && eyeLast >= eyeFirst) {
            ++s.withEyes;
            s.eyes += (eyeLast - eyeFirst) * g_msPerTick;
            if (shadowFirst && shadowFirst < eyeFirst) ++s.shadowsFirst;
        }
        const int64_t last = std::max(shadowLast, eyeLast);
        if (last > g_lastEnd && last <= g_start) {
            ++s.withDraws;
            s.after += (g_start - last) * g_msPerTick;
        }
    }
    g_lastEnd = end;
    if (!g_logStart) g_logStart = end;
    const double seconds = (end - g_logStart) * g_msPerTick / 1000.0;
    if (seconds >= 10) {
        if (s.frames) log(s, seconds);
        s = Sums{};
        g_logStart = end;
    }
}

}  // namespace s4vr::framecost
