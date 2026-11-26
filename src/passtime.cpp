#include "passtime.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MinHook.h"
#include "addrs.h"
#include "log.h"
#include "camera.h"
#include "singleview.h"

namespace s4vr::passtime {

namespace {

// Each takes at most 4 integer arguments (read from their prologues and callers).
using StepFn = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
struct Step {
    const char* name;   // addrs table
    const char* label;  // log
};
constexpr Step kSteps[] = {{"ListStage0", "stage 0"},
                           {"ListStage4", "stage 4"},
                           {"ListMirrors", "mirrors"},
                           {"ListOverlay", "overlay"}};
constexpr int kCount = sizeof(kSteps) / sizeof(kSteps[0]);

StepFn g_original[kCount] = {};
std::atomic<uint32_t> g_runs[kCount][2] = {};
std::atomic<uint64_t> g_ticks[kCount][2] = {};
std::atomic<ULONGLONG> g_lastLog{0};
std::atomic<uint32_t> g_waterDone{0};
std::atomic<void*> g_mainScene{nullptr};

// The water state (scene+0xe08: heights +0x2c/+0x30, a pointer at +0x38, active flags +0x40/+0x41) is computed in
// FrameBegin (0x12596c0) and read by the water stage, a job that runs later. The 857 log showed it half-written or
// zeroed when the stage read it (world 1 lot 0, heights 150.00 0.00 with a still camera): something else recomputes it
// in between, so the pool's reflection was skipped or drawn from a stale camera in some passes. Keep FrameBegin's
// result per eye and put it back just before the stage.
constexpr size_t kStateFrom = 0x2c, kStateTo = 0x42;
struct StateCopy {
    uint8_t bytes[kStateTo - kStateFrom] = {};
    bool valid = false;
};
StateCopy g_state[2];
std::atomic<uint32_t> g_stateRestores{0};

uint8_t* stateOf(void* scene) {
    if (!scene) return nullptr;
    uint8_t* state = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(scene) + 0xe08);
    return state;
}

int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void maybeLog() {
    const ULONGLONG at = GetTickCount64();
    ULONGLONG last = g_lastLog.load(std::memory_order_relaxed);
    if (at - last < 10000 || !g_lastLog.compare_exchange_strong(last, at)) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    char line[600];
    int n = 0;
    for (int i = 0; i < kCount; ++i) {
        double each[2];
        uint32_t runs[2];
        for (int e = 0; e < 2; ++e) {
            runs[e] = g_runs[i][e].exchange(0);
            const uint64_t t = g_ticks[i][e].exchange(0);
            each[e] = runs[e] ? t * ms / runs[e] : 0.0;
        }
        n += std::snprintf(line + n, sizeof(line) - n, "%s%s: left %u x %.2f ms, right %u x %.2f ms", i ? "; " : "",
                           kSteps[i].label, runs[0], each[0], runs[1], each[1]);
        if (n >= static_cast<int>(sizeof(line))) break;
    }
    LOG("Pass time (last 10 s, scene list stages, CPU on job threads): %s", line);
}

template <int I>
uint64_t hooked(void* a, uint64_t b, uint64_t c, uint64_t d) {
    const int eye = singleview::inSecond() ? 1 : 0;
    if (I == 2) {
        camera::logWaterPass(a, eye);
        if (a == g_mainScene.load(std::memory_order_relaxed) && g_state[eye].valid) {
            if (uint8_t* state = stateOf(a)) {
                if (std::memcmp(state + kStateFrom, g_state[eye].bytes, sizeof(g_state[eye].bytes)) != 0) {
                    std::memcpy(state + kStateFrom, g_state[eye].bytes, sizeof(g_state[eye].bytes));
                    g_stateRestores.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    const int64_t start = now();
    const uint64_t r = g_original[I](a, b, c, d);
    g_ticks[I][eye].fetch_add(static_cast<uint64_t>(now() - start), std::memory_order_relaxed);
    g_runs[I][eye].fetch_add(1, std::memory_order_relaxed);
    if (I == 2 && a == g_mainScene.load(std::memory_order_relaxed)) g_waterDone.fetch_add(1, std::memory_order_release);
    maybeLog();
    return r;
}

template <int... Is>
constexpr void* detour(int i, std::integer_sequence<int, Is...>) {
    void* all[] = {reinterpret_cast<void*>(&hooked<Is>)...};
    return all[i];
}

}  // namespace

void noteFrameBegin(void* scene, int eye) {
    if (scene != g_mainScene.load(std::memory_order_relaxed)) return;
    camera::fixLotWater(scene);
    if (uint8_t* state = stateOf(scene)) {
        std::memcpy(g_state[eye].bytes, state + kStateFrom, sizeof(g_state[eye].bytes));
        g_state[eye].valid = true;
    }
}

uint32_t stateRestores() { return g_stateRestores.exchange(0, std::memory_order_relaxed); }

void setMainScene(void* scene) { g_mainScene.store(scene, std::memory_order_relaxed); }

uint32_t waterDone() { return g_waterDone.load(std::memory_order_acquire); }

void install() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return;
    int hooked = 0;
    for (int i = 0; i < kCount; ++i) {
        void* fn = addrs::fn(kSteps[i].name);
        if (!fn) continue;
        s = MH_CreateHook(fn, detour(i, std::make_integer_sequence<int, kCount>{}),
                          reinterpret_cast<void**>(&g_original[i]));
        if (s == MH_OK) s = MH_EnableHook(fn);
        if (s == MH_OK) ++hooked;
        else LOG("Pass time: %s not timed (%s)", kSteps[i].label, MH_StatusToString(s));
    }
    LOG("Pass time: %d of %d scene list stages timed per eye", hooked, kCount);
}

}  // namespace s4vr::passtime
