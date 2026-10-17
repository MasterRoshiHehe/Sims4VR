#include "eyelids.h"

#include <windows.h>
#include <atomic>

#include "log.h"

namespace s4vr::eyelids {

namespace {

std::atomic<bool> g_sleeping{false};
State g_state;
float g_waited = 0;  // seconds asleep before the lids start to close
LARGE_INTEGER g_freq{}, g_last{};

// Seconds for each part, chosen to read as falling asleep, not a blackout.
constexpr float kDelay = 0.6f;   // lying down first
constexpr float kClose = 1.6f;   // the lids drooping shut
constexpr float kDarken = 2.5f;  // the glow through them fading
constexpr float kLighten = 0.6f;
constexpr float kOpen = 0.9f;

float approach(float v, float target, float step) {
    return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
}

}  // namespace

void setSleeping(bool sleeping) {
    if (g_sleeping.exchange(sleeping) != sleeping) LOG("Eyelids: %s", sleeping ? "asleep, closing" : "awake, opening");
}

bool sleeping() { return g_sleeping.load(std::memory_order_relaxed); }

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
    return g_state;
}

}  // namespace s4vr::eyelids
