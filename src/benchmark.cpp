#include "benchmark.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "camera.h"
#include "config.h"
#include "gpuload.h"
#include "log.h"

#ifndef S4VR_BUILD_VERSION
#define S4VR_BUILD_VERSION "dev"
#endif

namespace s4vr::benchmark {
namespace {

enum Phase { Idle, Countdown, Still1, TurnRight, TurnLeft, Still2 };
constexpr double kTwoPi = 6.283185307;
constexpr double kTurnRadians = 3 * kTwoPi;
constexpr ULONGLONG kCountdownMs = 8000, kStillMs = 5000;

std::atomic<bool> g_requested{false}, g_requestPaused{false};
Phase g_phase = Idle;
bool g_paused = false;
ULONGLONG g_phaseStart = 0;
double g_turned = 0;  // radians in the current turn phase

struct Stats {
    std::vector<float> gameMs;
    double wait = 0;
    int frames = 0, pictures = 0, slow = 0, missed = 0;
    float backMax = 0;
    ULONGLONG start = 0, end = 0;
};
Stats g_still, g_turn;
gpuload::Busy g_busy;
double g_turnSeconds = 0;

float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

void enter(Phase p) {
    g_phase = p;
    g_phaseStart = GetTickCount64();
    g_turned = 0;
    if (p == Still1) {
        g_still = Stats();
        g_turn = Stats();
        g_still.start = g_phaseStart;
    } else if (p == TurnRight) {
        g_still.end = g_phaseStart;
        g_turn.start = g_phaseStart;
        gpuload::takeBusy();  // restart the averages
    } else if (p == Still2) {
        g_turn.end = g_phaseStart;
        g_busy = gpuload::takeBusy();
    }
}

void report(const Config& c) {
    const double stillS = std::max(0.001, (g_still.end - g_still.start) / 1000.0);
    const double turnS = std::max(0.001, (g_turn.end - g_turn.start) / 1000.0);
    LOGT("benchmark",
         "Benchmark: v%s game %s | still %.1f s: %d pictures (%.1f/s), game ms p50 %.1f p95 %.1f | turning 2x3 turns (%.0f deg at %.0f deg/s) in %.1f s: "
         "%d pictures (%.1f/s, %.1f pairs/s), %d refreshes (%.1f/s), game ms p50 %.1f p95 %.1f p99 %.1f worst %.1f, "
         "xrWaitFrame avg %.1f ms, GPU 3D busy avg %.0f%% worst %.0f%% (%d samples; the game's share %.0f%%), %d slow frames (>100 ms), "
         "eyes turned back max %.1f deg, %d refreshes missed | render_scale %.2f msaa %d exact_fov %d paired_eyes %d mirrors %.0f/%.0f m reflections %s "
         "ssao %s shadow_map %.0f px mode %s",
         S4VR_BUILD_VERSION, g_paused ? "paused" : "running", stillS, g_still.frames, g_still.frames / stillS,
         percentile(g_still.gameMs, 0.5), percentile(g_still.gameMs, 0.95), 2 * kTurnRadians * 57.29578, c.bodyTurnRate, turnS,
         g_turn.pictures, g_turn.pictures / turnS, g_turn.pictures / turnS / 2, g_turn.frames, g_turn.frames / turnS,
         percentile(g_turn.gameMs, 0.5), percentile(g_turn.gameMs, 0.95), percentile(g_turn.gameMs, 0.99),
         g_turn.gameMs.empty() ? 0.0f : *std::max_element(g_turn.gameMs.begin(), g_turn.gameMs.end()),
         g_turn.frames ? g_turn.wait / g_turn.frames : 0.0, g_busy.all3d, g_busy.worst3d, g_busy.samples, g_busy.game3d,
         g_turn.slow, g_turn.backMax, g_turn.missed, c.renderScale, c.renderMsaa, c.exactFov ? 1 : 0, c.pairedEyes ? 1 : 0,
         c.mirrorInterior, c.mirrorExterior, c.reflectionsOff ? "off" : "on", c.ssaoOff ? "off" : "on", c.shadowMap,
         camera::bodyActive() ? "first person" : "other");
}

}  // namespace

void start(bool gamePaused) {
    g_requestPaused.store(gamePaused);
    g_requested.store(true);
}

void cancel() {
    if (g_phase != Idle) LOGT("benchmark", "Benchmark: cancelled");
    g_phase = Idle;
}

bool active() { return g_phase != Idle; }

void stick(float& rightX) {
    if (g_phase == TurnRight) rightX = 1.0f;
    else if (g_phase == TurnLeft) rightX = -1.0f;
}

void turned(float radians) {
    if (g_phase == TurnRight || g_phase == TurnLeft) g_turned += std::fabs(radians);
}

void missedRefreshes(int count) {
    if (g_phase == TurnRight || g_phase == TurnLeft) g_turn.missed += count;
}

void turnFrame(bool newPicture, float backDegrees) {
    if (g_phase != TurnRight && g_phase != TurnLeft) return;
    if (newPicture) ++g_turn.pictures;
    g_turn.backMax = std::max(g_turn.backMax, std::fabs(backDegrees));
}

void frame(const Config& config, double gameMs, double waitMs, double renderMs, double endMs, bool stereoFrame) {
    if (g_requested.exchange(false)) {
        if (!camera::bodyActive()) {
            LOGT("benchmark", "Benchmark: needs first person (the game camera is on); not started");
        } else {
            g_paused = g_requestPaused.load();
            enter(Countdown);
            LOGT("benchmark", "Benchmark: starts in %llu s (game %s): close the menu and stand still",
                 kCountdownMs / 1000, g_paused ? "paused" : "running");
        }
    }
    if (g_phase == Idle) return;
    if (!camera::bodyActive()) {
        LOGT("benchmark", "Benchmark: stopped, first person ended");
        g_phase = Idle;
        return;
    }
    if (!stereoFrame) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG inPhase = now - g_phaseStart;
    Stats* st = g_phase == Still1 ? &g_still : (g_phase == TurnRight || g_phase == TurnLeft) ? &g_turn : nullptr;
    if (st && gameMs > 0 && gameMs < 2000) {
        st->gameMs.push_back(static_cast<float>(gameMs));
        st->wait += waitMs;
        ++st->frames;
        if (gameMs + waitMs + renderMs + endMs > 100) ++st->slow;
        if (g_phase == Still1) ++st->pictures;
    }
    switch (g_phase) {
        case Countdown:
            if (inPhase >= kCountdownMs) enter(Still1);
            break;
        case Still1:
            if (inPhase >= kStillMs) enter(TurnRight);
            break;
        case TurnRight:
            if (g_turned >= kTurnRadians) enter(TurnLeft);
            break;
        case TurnLeft:
            if (g_turned >= kTurnRadians) enter(Still2);
            break;
        case Still2:
            if (inPhase >= kStillMs) {
                report(config);
                g_phase = Idle;
            }
            break;
        default: break;
    }
}

}  // namespace s4vr::benchmark
