#include "eyelids.h"

#include <windows.h>
#include <atomic>

#include "log.h"

namespace s4vr::eyelids {

namespace {

std::atomic<bool> g_sleeping{false};
State g_state;
// Your Sim faded out (setCovered): quicker than sleep, and on its own.
std::atomic<bool> g_covered{false};
std::atomic<ULONGLONG> g_coveredAt{0}, g_uncoveredAt{0};
State g_cover;
float g_waited = 0;  // seconds asleep before the lids start to close
LARGE_INTEGER g_freq{}, g_last{};

// Seconds for each part, chosen to read as falling asleep, not a blackout.
constexpr float kDelay = 0.6f;   // lying down first
constexpr float kClose = 1.6f;   // the lids drooping shut
constexpr float kDarken = 2.5f;  // the glow through them fading
constexpr float kLighten = 0.6f;
constexpr float kOpen = 0.9f;
// Fade-outs: shut before the game moves the Sim (its fade takes about a
// second), dark at once, open shortly after it fades back in. Quick blink
// and short dark (user 2026-10-19 01:34Z: WW switches closed too slowly and
// stayed dark too long).
constexpr float kCoverClose = 0.12f;
constexpr float kCoverDarken = 0.1f;
constexpr float kCoverLighten = 0.15f;
constexpr float kCoverOpen = 0.25f;
constexpr ULONGLONG kCoverHoldMs = 150;    // still shut this long after the fade-in (the jump can lag it)
constexpr ULONGLONG kCoverMaxMs = 5000;    // no fade-in after this: open anyway

float approach(float v, float target, float step) {
    return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
}

}  // namespace

void setSleeping(bool sleeping) {
    if (g_sleeping.exchange(sleeping) != sleeping) LOG("Eyelids: %s", sleeping ? "asleep, closing" : "awake, opening");
}

bool sleeping() { return g_sleeping.load(std::memory_order_relaxed); }

void setCovered(bool covered) {
    const ULONGLONG now = GetTickCount64();
    (covered ? g_coveredAt : g_uncoveredAt).store(now, std::memory_order_relaxed);
    if (g_covered.exchange(covered) != covered)
        LOG("Eyelids: %s", covered ? "your Sim fades out, closing" : "your Sim fades back in, opening");
}

State advance() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart) : 0;
    g_last = now;
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    if (g_sleeping.load(std::memory_order_relaxed)) {
        g_waited += dt;
        if (g_waited > kDelay) g_state.closure = approach(g_state.closure, 1, dt / kClose);
        if (g_state.closure >= 1) g_state.dark = approach(g_state.dark, 1, dt / kDarken);
    } else {
        g_waited = 0;
        g_state.dark = approach(g_state.dark, 0, dt / kLighten);
        if (g_state.dark < 0.3f) g_state.closure = approach(g_state.closure, 0, dt / kOpen);
    }
    const ULONGLONG ms = GetTickCount64();
    bool shut = g_covered.load(std::memory_order_relaxed);
    if (shut && ms - g_coveredAt.load(std::memory_order_relaxed) > kCoverMaxMs) {
        g_covered.store(false, std::memory_order_relaxed);
        LOG("Eyelids: your Sim stayed faded out %llu s, opening", kCoverMaxMs / 1000);
        shut = false;
    }
    if (!shut && ms - g_uncoveredAt.load(std::memory_order_relaxed) < kCoverHoldMs && g_cover.closure > 0) shut = true;
    if (shut) {
        g_cover.closure = approach(g_cover.closure, 1, dt / kCoverClose);
        if (g_cover.closure >= 1) g_cover.dark = approach(g_cover.dark, 1, dt / kCoverDarken);
    } else {
        g_cover.dark = approach(g_cover.dark, 0, dt / kCoverLighten);
        if (g_cover.dark < 0.3f) g_cover.closure = approach(g_cover.closure, 0, dt / kCoverOpen);
    }
    State out = g_state;
    if (g_cover.closure > out.closure) out.closure = g_cover.closure;
    if (g_cover.dark > out.dark) out.dark = g_cover.dark;
    return out;
}

}  // namespace s4vr::eyelids
