#include "player.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "armik.h"
#include "crashlog.h"
#include "gameaddr.h"
#include "log.h"
#include "rtti.h"
#include "vr.h"

namespace s4vr::player {

namespace {

constexpr size_t kPositionOffset = 0x2e0, kRotationOffset = 0x2f0;
constexpr float kPi = 3.14159265f, kTwoPi = 6.2831853f;
constexpr float kDeadZone = 0.2f;
constexpr float kSpeedSmoothing = 10.0f;  // 1/s: how quickly walking speeds up and slows down
constexpr float kBodyTurnRate = 8.0f;     // 1/s: how quickly the body turns to its target
constexpr float kStandingSlack = 0.6f;    // radians you can look aside before the standing body turns
constexpr float kFloorSmoothing = 12.0f;  // 1/s
constexpr float kEyeSmoothing = 0.5f;     // 1/s: eye height follows the Sim's standing head height
constexpr float kEyeCatchUp = 0.05f;      // metres: a bigger gap is a real change of height
constexpr float kEyeCatchUpRate = 6.0f;   // 1/s
constexpr ULONGLONG kStateTimeout = 1000; // ms without Python news: stop moving freely
constexpr ULONGLONG kMaskTimeout = 600;   // ms: wall data older than this is ignored
constexpr float kStairSide = 0.25f;       // metres your body keeps from a stair's sides
constexpr float kSimSpacing = 0.45f;      // world units walking keeps between you and another Sim (centres)
constexpr float kEyeReach = 0.6f;         // metres: eyes farther than this from the body's root mean it was moved

// Shared with Python.
std::mutex g_mutex;
bool g_freeAllowed = false;
ULONGLONG g_stateTick = 0;
uint32_t g_blocked = 0;
float g_floor[7] = {0, 0, 0, 0, 0, -1e9f, 1e9f};  // plane, then lowest and highest height
float g_flight[6] = {};  // stairs: bottom centre x z, up x z, run, half width
float g_pose[8] = {};
float g_restPoint[3] = {};  // where slowing down ends (x, z) and how far off
bool g_poseValid = false;

// Game thread.
float g_walkSpeed = 1.5f, g_runSpeed = 3.0f, g_eyeForward = 0.10f, g_eyeHeight = 0.15f;
bool g_simCollision = true;
// Your head against other Sims ([physical] head, user 2026-10-17 04:06Z: "if
// I try and put my head through another sim, I will get repelled"): leaning
// into one pushes the player back out, sideways, so your view stays out of
// their body. Only the player moves; their Sim and yours run nothing.
std::atomic<bool> g_headRepel{true};
std::atomic<float> g_headRadius{0.08f};  // metres around your view
int g_repelLogs = 0;
ULONGLONG g_repelLogTick = 0;
std::atomic<bool> g_running{false};  // left stick tapped: run speed and the Sim's run walk
bool g_free = false;
uint8_t* g_actor = nullptr;
bool g_actorOk = false;
float g_base[2] = {};  // app space origin in the world, x and z
float g_viewYaw = 0;
float g_simYaw = 0;
bool g_facingHead = false;
float g_dir[2] = {0, 1};
float g_speed = 0;
// Slowing down (v0.13.188, user 2026-10-16 23:15Z: "I stop very abrupt"): the
// speed follows a critically damped spring, so braking starts gently, peaks,
// and eases into the stop instead of the hardest jolt right at release.
float g_speedRate = 0;  // m/s per s
int g_eyeLogs = 0;
float g_poseMark[3] = {};
bool g_poseMarkValid = false;
int g_poseJumpLogs = 0;
ULONGLONG g_eyeLogTick = 0;
std::atomic<float> g_stopTime{0.5f};
// The game's own stop clip (v0.13.194, user 2026-10-17 01:37Z: "you need to
// implement the start walking and stop walking animations"): with walk clips
// on, slowing down covers the walkstyle's stop clip distance (read from the
// client's walk controller, walkstop.cpp), so the feet that play the stop
// land where you stop. 0 = unknown: [body] stop_time decides.
std::atomic<float> g_clipStop{0};
bool g_slowing = false;
bool g_stopForClip = false;
float g_stopW = 0;
float g_rest = 0;  // metres still to go while slowing (published with the pose)
float g_floorY = 0;
float g_eyeAboveFloor = 0;
float g_mark[2] = {};  // last sane eye offset from the root, in the body's frame
bool g_markValid = false;
int g_markSkips = 0;
LARGE_INTEGER g_last{}, g_freq{};

bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The Sim's facing from its rotation: +Z turned by q, on the floor.
float yawOf(const float* q) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    return std::atan2(2 * (x * z + w * y), 1 - 2 * (x * x + y * y));
}

// (x, z) turned about +Y by a, as the quaternion (0, sin a/2, 0, cos a/2) does.
void turnXZ(float a, float x, float z, float& ox, float& oz) {
    float c = std::cos(a), s = std::sin(a);
    ox = x * c + z * s;
    oz = -x * s + z * c;
}

float ease(float rate, float dt) { return 1 - std::exp(-rate * dt); }

// Python's wall check: kRays directions, bit i = heading i * 360/kRays
// degrees from +Z toward +X, set when something blocks it close by.
constexpr int kRays = 24;
constexpr uint32_t kAllRays = (1u << kRays) - 1;

// Walking where the way is clear goes ahead untouched, so narrow gaps stay
// passable. Walking into something turns the step toward the nearest clear
// direction, keeping only the part of it that goes that way: pushing into a
// wall at an angle slides along it, pushing straight at it stops.
void slide(float& vx, float& vz, uint32_t mask) {
    if (mask == kAllRays) return;  // boxed in (inside a wall?): let the player walk out
    float len = std::sqrt(vx * vx + vz * vz);
    if (len < 1e-5f) return;
    const float step = kTwoPi / kRays;
    float heading = std::atan2(vx, vz);
    int i = static_cast<int>(std::lround(heading / step));
    i = ((i % kRays) + kRays) % kRays;
    if (!(mask & (1u << i))) return;
    float best = 0;
    bool found = false;
    for (int k = 1; k <= kRays / 2 && !found; ++k) {
        for (int side : {1, -1}) {
            int j = ((i + side * k) % kRays + kRays) % kRays;
            if (mask & (1u << j)) continue;
            float a = j * step;
            float off = std::fabs(std::remainder(a - heading, kTwoPi));
            if (!found || off < std::fabs(std::remainder(best - heading, kTwoPi))) best = a;
            found = true;
        }
    }
    float off = std::fabs(std::remainder(best - heading, kTwoPi));
    if (!found || off > 1.4f) {  // nothing clear within ~80 degrees: stop
        vx = vz = 0;
        return;
    }
    float along = len * std::cos(off);
    vx = std::sin(best) * along;
    vz = std::cos(best) * along;
}

// Smooth sliding ([body] smooth_slide): pushing into something keeps the part
// of the step that goes along it, the way the blocked rays say the obstacle
// faces (the middle of the run of blocked rays around where you push). That direction is eased from frame to frame, so walking
// along a wall or around furniture glides instead of jumping between the 15
// degree rays each time Python's check comes in (10 times a second). Shapes
// it can't make sense of (the slide would still go into something) fall back
// to slide().
std::atomic<bool> g_smoothSlide{true};
constexpr float kWallEase = 15.0f;  // 1/s: how quickly the obstacle's direction follows the check
float g_wall[2] = {};               // eased direction toward what you push into
bool g_wallValid = false;

void slideSmooth(float& vx, float& vz, uint32_t mask, float dt) {
    if (mask == kAllRays) return;
    float len = std::sqrt(vx * vx + vz * vz);
    if (len < 1e-5f) return;
    const float step = kTwoPi / kRays;
    float heading = std::atan2(vx, vz);
    int i = static_cast<int>(std::lround(heading / step));
    i = ((i % kRays) + kRays) % kRays;
    if (!(mask & (1u << i))) {  // the way ahead is clear (a narrow gap too): untouched
        g_wallValid = false;
        return;
    }
    // The run of blocked rays around where you push: a wall or a piece of
    // furniture blocks an arc centred on the direction it faces you from.
    int left = 0, right = 0;
    while (left < kRays / 2 && (mask & (1u << ((i - left - 1 + kRays) % kRays)))) ++left;
    while (right < kRays / 2 && (mask & (1u << ((i + right + 1) % kRays)))) ++right;
    float centre = (i + (right - left) * 0.5f) * step;
    float dx = std::sin(centre), dz = std::cos(centre);
    if (!g_wallValid) {
        g_wall[0] = dx;
        g_wall[1] = dz;
        g_wallValid = true;
    } else {
        float k = ease(kWallEase, dt);
        float wx = g_wall[0] + (dx - g_wall[0]) * k, wz = g_wall[1] + (dz - g_wall[1]) * k;
        float wl = std::sqrt(wx * wx + wz * wz);
        if (wl > 1e-4f) {
            g_wall[0] = wx / wl;
            g_wall[1] = wz / wl;
        } else {
            g_wall[0] = dx;
            g_wall[1] = dz;
        }
    }
    float ox = vx, oz = vz;
    float into = vx * g_wall[0] + vz * g_wall[1];
    if (into > 0) {
        vx -= g_wall[0] * into;
        vz -= g_wall[1] * into;
    }
    float nl = std::sqrt(vx * vx + vz * vz);
    if (nl < 0.15f * len) {  // straight at it, or into a corner: stop
        vx = vz = 0;
        return;
    }
    int n = static_cast<int>(std::lround(std::atan2(vx, vz) / step));
    n = ((n % kRays) + kRays) % kRays;
    if (mask & (1u << n)) {  // the slide still runs into something
        vx = ox;
        vz = oz;
        slide(vx, vz, mask);
    }
}

// Through the actor's own SetTransform (vtable +0x28), as the game's root
// motion does: it also bumps the change counter and tells everything attached
// to the Sim that it moved. Writing the floats directly corrupted the heap.
using SetTransformFn = void (*)(void* actor, const float* transform);
constexpr size_t kSetTransformSlot = 0x28 / 8;

// The pin: the actor whose transform is ours, and where it goes. Any thread
// may call SetTransform (job threads run root motion); only the pinned actor
// takes the lock.
std::atomic<uint8_t*> g_pinActor{nullptr};
SRWLOCK g_pinLock = SRWLOCK_INIT;
float g_pin[7] = {};  // position, rotation xyzw
SetTransformFn g_setTransform = nullptr;  // the game's, behind our hook
bool g_pinHookTried = false;

void pinnedSetTransform(void* actor, const float* t) {
    if (actor && actor == g_pinActor.load(std::memory_order_acquire)) {
        alignas(16) float c[12];
        std::memcpy(c, t, sizeof(c));
        AcquireSRWLockShared(&g_pinLock);
        std::memcpy(c, g_pin, 3 * sizeof(float));
        std::memcpy(c + 4, g_pin + 3, 4 * sizeof(float));
        ReleaseSRWLockShared(&g_pinLock);
        g_setTransform(actor, c);
        return;
    }
    g_setTransform(actor, t);
}

// Where the game draws the Sim. Each time the actor moves, the Sim's animation
// component (its actor listener, game+0x991630) copies the actor's transform to
// the object transform the renderer draws from (game+0x9996e0), and on the way
// game+0x994180 snaps its height: when the Sim's routing surface is a level's
// floor and no DisableConformToSurface controller runs (the game's own stair
// and ladder clips run one), the height becomes the floor of that level at x, z
// (the terrain and floor height query, game+0x38edd0, given the routing level).
// The joints keep the actor's height, so your head and view were right while
// the drawn body sat on that floor: under the steps going up, above them going
// down, and over a stairwell at the bare level height (screenshots 2026-10-14:
// 195.27 bottom floor, 198.02 level 1 over the stairwell, 199.27 landing).
// For the pinned body the snap is skipped, so it is drawn where you are. Other
// Sims, god view and a body the game moves keep the game's snap. A call with a
// routing location of its own (where a Sim will stand, a walk stop) is left to
// the game too.
using FloorSnapFn = bool (*)(uint8_t* component, float* transform, char noConform, void* routing);
constexpr uint32_t kFloorSnapRva = 0x9b41f0;
constexpr uint8_t kFloorSnapBytes[16] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57,
                                         0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24};
constexpr size_t kComponentActor = 0x68;  // the animation component's actor (its listener's +0x58)
constexpr size_t kTransformY = 5;         // rotation xyzw, then position xyzw
FloorSnapFn g_floorSnap = nullptr;  // the game's, behind our hook
std::atomic<bool> g_keepHeight{true};
std::atomic<ULONGLONG> g_snapLogTick{0};
std::atomic<int> g_snapLogs{0};

// The drawn body away from you for a frame (user 2026-10-16 23:15Z: "when I
// enter a new room I see my character in front of me for a frame"): whatever
// moves the drawn transform of the pinned body (a route's place, a location
// from the simulation), it is put back under you here, and the log says how.
std::atomic<int> g_strayLogs{0};
std::atomic<uint32_t> g_strayRouting{0};
std::atomic<ULONGLONG> g_strayTick{0};

void keepUnderYou(uint8_t* component, float* t, void* routing) {
    uint8_t* pinned = g_pinActor.load(std::memory_order_acquire);
    if (!pinned || !component || *reinterpret_cast<uint8_t**>(component + kComponentActor) != pinned) return;
    float px, pz;
    AcquireSRWLockShared(&g_pinLock);
    px = g_pin[0];
    pz = g_pin[2];
    ReleaseSRWLockShared(&g_pinLock);
    float dx = t[4] - px, dz = t[6] - pz;
    // Any offset: the pinned body is always drawn at the pin (v0.13.195; user
    // 2026-10-17 02:26Z: walking and looking down, the body creeps forward and
    // back with the walk; offsets under 0.25 m were left to the game before).
    if (dx * dx + dz * dz < 0.01f * 0.01f) return;
    float off = std::sqrt(dx * dx + dz * dz);
    // Route places come every walk update (where the client's route says the
    // Sim is, 0.4-0.8 m ahead): counted every 10 s. Anything else is logged.
    if (routing) {
        if (off > 0.25f) g_strayRouting.fetch_add(1, std::memory_order_relaxed);
        ULONGLONG now = GetTickCount64(), last = g_strayTick.load(std::memory_order_relaxed);
        if (now - last >= 10000 && g_strayTick.compare_exchange_strong(last, now))
            LOG("Player: in the last 10 s the game put your drawn body at %u route places away from you (up to %.2f m); "
                "kept under you", g_strayRouting.exchange(0), off);
    } else if (off > 0.25f && g_strayLogs.fetch_add(1, std::memory_order_relaxed) < 200) {
        LOG("Player: the game drew your body %.2f m from you (moved, not a route place); put back under you", off);
    }
    t[4] = px;
    t[6] = pz;
}

bool keptFloorSnap(uint8_t* component, float* t, char noConform, void* routing) {
    keepUnderYou(component, t, routing);
    uint8_t* pinned = g_pinActor.load(std::memory_order_acquire);
    if (!pinned || routing || !component || !g_keepHeight.load(std::memory_order_relaxed) ||
        *reinterpret_cast<uint8_t**>(component + kComponentActor) != pinned)
        return g_floorSnap(component, t, noConform, routing);
    // Twice a second, what the game would have done, for the log.
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_snapLogTick.load(std::memory_order_relaxed);
    if (now - last >= 500 && g_snapLogs.load(std::memory_order_relaxed) < 300 &&
        g_snapLogTick.compare_exchange_strong(last, now)) {
        alignas(16) float game[8];
        std::memcpy(game, t, sizeof(game));
        if (g_floorSnap(component, game, noConform, nullptr) && std::fabs(game[kTransformY] - t[kTransformY]) > 0.05f) {
            g_snapLogs.fetch_add(1, std::memory_order_relaxed);
            LOG("Player: body drawn at your height %.2f; the game would have put it on the floor at %.2f (%+.2f)",
                t[kTransformY], game[kTransformY], game[kTransformY] - t[kTransformY]);
        }
    }
    return false;
}

void installFloorSnap() {
    void* target = gameaddr::function(kFloorSnapRva, kFloorSnapBytes);
    if (!target) {
        LOG("Player: the game's floor snap not found (game build not recognised); on stairs the body is drawn on "
            "a level's floor");
        return;
    }
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&keptFloorSnap),
                                     reinterpret_cast<void**>(&g_floorSnap));
    if (status == MH_OK) status = MH_EnableHook(target);
    LOG("Player: body drawn at your height, not snapped to its level's floor (keep_height %d): %s",
        g_keepHeight.load() ? 1 : 0, MH_StatusToString(status));
}

void installPin(uint8_t* actor) {
    if (g_pinHookTried) return;
    g_pinHookTried = true;
    installFloorSnap();
    auto vtable = *reinterpret_cast<SetTransformFn**>(actor);
    void* target = reinterpret_cast<void*>(vtable[kSetTransformSlot]);
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&pinnedSetTransform),
                                     reinterpret_cast<void**>(&g_setTransform));
    if (status == MH_OK) status = MH_EnableHook(target);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LOG("Player: body pinned to you while you move freely (SetTransform at game+0x%llx): %s",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - base), MH_StatusToString(status));
    if (status != MH_OK) g_setTransform = nullptr;
}

void pin(uint8_t* actor, const float p[3], float yaw) {
    if (!g_setTransform) return;
    AcquireSRWLockExclusive(&g_pinLock);
    g_pin[0] = p[0];
    g_pin[1] = p[1];
    g_pin[2] = p[2];
    g_pin[3] = 0;
    g_pin[4] = std::sin(yaw / 2);
    g_pin[5] = 0;
    g_pin[6] = std::cos(yaw / 2);
    ReleaseSRWLockExclusive(&g_pinLock);
    g_pinActor.store(actor, std::memory_order_release);
}

// When the pin last let go: a walk stop that comes right after (stride.py
// ends its route as the game takes the Sim) still puts the body where it was.
std::atomic<ULONGLONG> g_unpinTick{0};
constexpr ULONGLONG kRecentPin = 3000;  // ms

void unpin() {
    if (g_pinActor.exchange(nullptr, std::memory_order_acq_rel)) g_unpinTick.store(GetTickCount64());
}

void place(uint8_t* actor, const float p[3], float yaw) {
    alignas(16) float t[12];
    std::memcpy(t, actor + kPositionOffset, sizeof(t));  // position, rotation, scale
    t[0] = p[0];
    t[1] = p[1];
    t[2] = p[2];
    t[4] = 0;
    t[5] = std::sin(yaw / 2);
    t[6] = 0;
    t[7] = std::cos(yaw / 2);
    auto vtable = *reinterpret_cast<SetTransformFn**>(actor);
    crashlog::note(crashlog::kSetTransform);
    vtable[kSetTransformSlot](actor, t);
}

}  // namespace

bool pinPlace(float out[7]) {
    if (!g_pinActor.load(std::memory_order_acquire)) {
        ULONGLONG since = g_unpinTick.load();
        if (!since || GetTickCount64() - since > kRecentPin) return false;
    }
    AcquireSRWLockShared(&g_pinLock);
    std::memcpy(out, g_pin, sizeof(g_pin));
    ReleaseSRWLockShared(&g_pinLock);
    return true;
}

bool pinnedAt(const void* actor, float out[7]) {
    if (!actor || actor != g_pinActor.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&g_pinLock);
    std::memcpy(out, g_pin, sizeof(g_pin));
    ReleaseSRWLockShared(&g_pinLock);
    return true;
}

bool settle(uint8_t* actor) {
    float at[7];
    if (!actor || actor != g_actor || !g_free || !pinnedAt(actor, at)) return false;
    float cur[7];
    std::memcpy(cur, actor + kPositionOffset, 3 * sizeof(float));
    std::memcpy(cur + 3, actor + kRotationOffset, 4 * sizeof(float));
    float moved = 0;
    for (int i = 0; i < 7; ++i) moved = std::max(moved, std::fabs(cur[i] - at[i]));
    if (!(moved > 1e-4f)) return false;
    place(actor, at, 2 * std::atan2(at[4], at[6]));
    return true;
}

void setHeadRepel(bool on, float radiusMetres) {
    g_headRepel.store(on, std::memory_order_relaxed);
    if (std::isfinite(radiusMetres)) g_headRadius.store(std::clamp(radiusMetres, 0.02f, 0.3f), std::memory_order_relaxed);
}

bool headRepelOn(float& radiusMetres) {
    radiusMetres = g_headRadius.load(std::memory_order_relaxed);
    return g_simCollision && g_headRepel.load(std::memory_order_relaxed);
}

void configure(float walkSpeed, float runSpeed, float eyeForward, float eyeHeight, bool simCollision) {
    g_simCollision = simCollision;
    if (walkSpeed > 0.1f && walkSpeed < 10) g_walkSpeed = walkSpeed;
    if (runSpeed > 0.1f && runSpeed < 15) g_runSpeed = runSpeed;
    g_eyeForward = eyeForward;
    g_eyeHeight = eyeHeight;
}

void setState(bool freeAllowed, uint32_t blockedMask, const float floor[7], const float flight[6]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_freeAllowed = freeAllowed;
    g_blocked = blockedMask;
    std::memcpy(g_floor, floor, sizeof(g_floor));
    std::memcpy(g_flight, flight, sizeof(g_flight));
    g_stateTick = GetTickCount64();
}

bool pose(float out[8]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_poseValid) return false;
    std::memcpy(out, g_pose, sizeof(g_pose));
    return true;
}

bool rest(float out[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_poseValid || !(g_restPoint[2] > 0)) return false;
    std::memcpy(out, g_restPoint, sizeof(g_restPoint));
    return true;
}

void setClipStop(float metres) {
    if (!(metres > 0.05f && metres < 2)) metres = 0;
    if (g_clipStop.exchange(metres) != metres)
        LOG("Player: walking comes to rest over the walk's stop clip (%.2f m)%s", metres,
            metres ? "" : ": off, [body] stop_time instead");
}

void release() { unpin(); }

void setKeepHeight(bool on) { g_keepHeight.store(on, std::memory_order_relaxed); }

void setStopTime(float seconds) {
    if (!(seconds >= 0 && seconds <= 3)) seconds = 0.5f;
    if (g_stopTime.exchange(seconds) != seconds) LOG("Player: walking comes to rest over %.2f s after you let go", seconds);
}

void setSmoothSlide(bool on) {
    if (g_smoothSlide.exchange(on) != on) LOG("Player: smooth wall sliding %s", on ? "on" : "off");
}

void setRunning(bool on) { g_running.store(on, std::memory_order_relaxed); }
bool running() { return g_running.load(std::memory_order_relaxed); }

void reset() {
    unpin();
    g_free = false;
    g_running.store(false, std::memory_order_relaxed);
    g_actor = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_poseValid = false;
    g_freeAllowed = false;
    g_stateTick = 0;
}

bool update(const Input& in, float viewYaw, float turn, Output& out) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart) : 0;
    g_last = now;
    if (!(dt > 0) || dt > 0.1f) dt = dt > 0.1f ? 0.1f : 0;

    if (in.actor != g_actor) {
        unpin();
        g_free = false;
        g_actor = in.actor;
        // Only ever move the kind of object we checked live.
        char name[64] = {};
        g_actorOk = rtti::className(reinterpret_cast<uintptr_t>(in.actor), name, sizeof(name)) &&
                    std::strcmp(name, ".?AVActor@Animation@Sims3@@") == 0;
        LOG("Player: body object %p is %s%s", in.actor, name[0] ? name : "unknown",
            g_actorOk ? "" : " (not an Actor: free movement off)");
    }
    if (!g_actorOk || !finite3(in.headJoint) || !finite3(in.hmd)) {
        unpin();
        return false;
    }

    // Python's view of the Sim, if recent.
    bool freeAllowed;
    uint32_t mask;
    float floor[7], flight[6];
    bool maskFresh, floorFresh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ULONGLONG age = GetTickCount64() - g_stateTick;
        freeAllowed = g_stateTick && age < kStateTimeout && g_freeAllowed;
        maskFresh = g_stateTick && age < kMaskTimeout;
        floorFresh = g_stateTick && age < kStateTimeout;
        mask = g_blocked;
        std::memcpy(floor, g_floor, sizeof(floor));
        std::memcpy(flight, g_flight, sizeof(flight));
    }
    auto floorAt = [&](float x, float z, float fallback) {
        if (!floorFresh) return fallback;
        float y = floor[2] + floor[3] * (x - floor[0]) + floor[4] * (z - floor[1]);
        y = y < floor[5] ? floor[5] : y > floor[6] ? floor[6] : y;  // a stair ramp ends flat at both floors
        return std::isfinite(y) ? y : fallback;
    };

    float cur[3], q[4];
    std::memcpy(cur, in.actor + kPositionOffset, sizeof(cur));
    std::memcpy(q, in.actor + kRotationOffset, sizeof(q));
    if (!finite3(cur) || !std::isfinite(q[3])) {
        unpin();
        return false;
    }
    float actorYaw = yawOf(q);

    float sx = 0, sz = 0, strength = 0;
    bool pushing = vr::walkInput(sx, sz, strength) && strength >= kDeadZone;
    const float s = in.scale;

    if (!g_free) {
        if (!freeAllowed) return false;
        // Start where the view already is: the camera stays put.
        g_viewYaw = viewYaw;
        float hx, hz;
        turnXZ(g_viewYaw, in.hmd[0] * s, in.hmd[2] * s, hx, hz);
        g_base[0] = in.headJoint[0] - hx;
        g_base[1] = in.headJoint[2] - hz;
        g_simYaw = actorYaw;
        g_facingHead = false;
        g_speed = 0;
        g_floorY = cur[1];
        g_eyeAboveFloor = in.headJoint[1] - cur[1] + (std::isfinite(in.eyeUp) ? in.eyeUp : g_eyeHeight * s);
        g_free = true;
        g_markValid = false;
        LOG("Player: free movement on at %.2f %.2f %.2f (eye %.2f above the floor)", cur[0], cur[1], cur[2],
            g_eyeAboveFloor);
    } else if (!freeAllowed) {
        // The game is moving the Sim now (or Python went quiet): the view
        // follows the Sim's head again.
        unpin();
        g_free = false;
        LOG("Player: the game has the Sim; free movement off");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pose[4] = g_pose[5] = g_pose[6] = g_pose[7] = 0;
        return false;
    }

    // Right stick: turn around your head, not around the room's centre.
    if (turn != 0 && std::isfinite(turn)) {
        float ax, az, bx, bz;
        turnXZ(g_viewYaw, in.hmd[0] * s, in.hmd[2] * s, ax, az);
        g_viewYaw = std::remainder(g_viewYaw + turn, kTwoPi);
        turnXZ(g_viewYaw, in.hmd[0] * s, in.hmd[2] * s, bx, bz);
        g_base[0] += ax - bx;
        g_base[1] += az - bz;
    }

    // Left stick: walk where you push, at a set speed, eased in and out.
    float target = 0;
    if (pushing) {
        float len = std::sqrt(sx * sx + sz * sz);
        if (len > 1e-4f) {
            g_dir[0] = sx / len;
            g_dir[1] = sz / len;
        }
        target = (running() ? g_runSpeed : g_walkSpeed) * s * (strength > 0.8f ? 1.0f : strength / 0.8f);
    }
    float stopTime = g_stopTime.load(std::memory_order_relaxed);
    float clipStop = g_clipStop.load(std::memory_order_relaxed);
    if (target < g_speed && (stopTime > 0.05f || clipStop > 0.05f) && dt > 0) {
        if (!g_slowing) {
            // v(t) = v0 (1 + wt) e^-wt reaches 1% of v0 at wt = 6.6 and goes
            // 2 v0 / w in all.
            g_slowing = true;
            g_stopForClip = false;
            g_stopW = 6.6f / std::max(stopTime, 0.05f);
        }
        if (clipStop > 0.05f && target <= 0 && !g_stopForClip) {
            // The stick is fully let go (it often eases back over a few
            // frames): from here the rest of the slowing covers the stop clip
            // (v0.13.199: v0.13.198 often came to rest in 0.1-0.25 m of the
            // clip's 0.87, so its feet slid).
            g_stopForClip = true;
            g_stopW = std::clamp(2 * g_speed / clipStop, 2.5f, 15.0f);
        }
        float w = g_stopW;
        float k = std::exp(-w * dt);
        float x = g_speed - target, v = g_speedRate;
        float x1 = (x + (v + w * x) * dt) * k;
        g_speedRate = (v - (v + w * x) * w * dt) * k;
        g_speed = target + x1;
    } else {
        g_speed += (target - g_speed) * ease(kSpeedSmoothing, dt);
        g_speedRate = 0;
        g_slowing = false;
    }
    if (g_speed < 0.01f) g_speed = 0, g_speedRate = 0;
    g_rest = g_slowing && g_stopW > 0 ? 2 * g_speed / g_stopW + g_speedRate / (g_stopW * g_stopW) : 0;
    float vx = g_dir[0] * g_speed, vz = g_dir[1] * g_speed;
    if (maskFresh) {
        if (g_smoothSlide.load(std::memory_order_relaxed))
            slideSmooth(vx, vz, mask, dt);
        else
            slide(vx, vz, mask);
    } else {
        g_wallValid = false;
    }
    // Other Sims: walking stops at them (leaning in with your real head still
    // gets you close, for a kiss).
    if (g_simCollision) {
        float sims[16][2];
        int n = armik::nearbySims(sims, 16);
        // A few passes: taking out the push toward one Sim can add some toward
        // another close by.
        for (int pass = 0; pass < 3 && n; ++pass) {
            bool changed = false;
            for (int k = 0; k < n; ++k) {
                float dx = sims[k][0] - cur[0], dz = sims[k][1] - cur[2];
                float d = std::sqrt(dx * dx + dz * dz);
                if (!(d > 1e-3f) || d > kSimSpacing) continue;
                float nx = dx / d, nz = dz / d, toward = vx * nx + vz * nz;
                if (toward > 1e-5f) {
                    vx -= nx * toward;
                    vz -= nz * toward;
                    changed = true;
                }
            }
            if (!changed) break;
        }
    }
    g_base[0] += vx * dt;
    g_base[1] += vz * dt;

    // Where your head is over the floor.
    float hx, hz;
    turnXZ(g_viewYaw, in.hmd[0] * s, in.hmd[2] * s, hx, hz);
    hx += g_base[0];
    hz += g_base[1];

    // On a flight of stairs its sides hold you (the railings, the walls beside
    // it): you only leave it at the top or the bottom. Leaning out is pushed
    // back like walking into a wall.
    if (floorFresh && flight[4] > 0 && std::isfinite(flight[5])) {
        float rx = hx - flight[0], rz = hz - flight[1];
        float along = rx * flight[2] + rz * flight[3];
        float side = rx * flight[3] - rz * flight[2];  // across, toward (dz, -dx)
        float limit = flight[5] - kStairSide * s;
        if (limit < 0.1f * s) limit = 0.1f * s;
        if (along > 0 && along < flight[4] && std::fabs(side) > limit) {
            float push = side > 0 ? side - limit : side + limit;
            g_base[0] -= push * flight[3];
            g_base[1] += push * flight[2];
            hx -= push * flight[3];
            hz += push * flight[2];
        }
    }

    // Your head out of other Sims: the player is moved the shortest way
    // sideways until your view is clear of their bodies, along the walls like
    // walking (a wall behind you holds you where you are).
    if (g_simCollision && g_headRepel.load(std::memory_order_relaxed)) {
        float view[3] = {hx, g_floorY + g_eyeAboveFloor + in.hmd[1] * s, hz};
        float push[2];
        float depth = armik::headOutOfBodies(view, g_headRadius.load(std::memory_order_relaxed) * s, push);
        if (depth > 0) {
            float px = push[0], pz = push[1];
            if (maskFresh) slide(px, pz, mask);
            g_base[0] += px;
            g_base[1] += pz;
            hx += px;
            hz += pz;
            ULONGLONG tick = GetTickCount64();
            if (g_repelLogs < 60 && tick - g_repelLogTick > 500) {
                g_repelLogTick = tick;
                ++g_repelLogs;
                LOG("Player: your head was %.0f mm into another Sim: pushed out %.0f mm%s", depth * 1000 / s,
                    std::sqrt(px * px + pz * pz) * 1000 / s,
                    px * px + pz * pz < 0.5f * (push[0] * push[0] + push[1] * push[1]) ? " (a wall holds you)" : "");
            }
        }
    }

    // Body facing follows where you look: closely while you walk (sideways
    // or backwards too, so the body never turns its back on you), and when
    // standing only once you look far enough aside (glancing around doesn't
    // spin the body).
    bool moving = std::sqrt(vx * vx + vz * vz) > 0.15f * s;
    float want = g_simYaw;
    float fx, fz;
    turnXZ(g_viewYaw, in.hmdForward[0], in.hmdForward[1], fx, fz);
    bool lookValid = fx * fx + fz * fz > 0.01f;
    float look = lookValid ? std::atan2(fx, fz) : g_simYaw;
    if (moving) {
        want = lookValid ? look : std::atan2(vx, vz);
        g_facingHead = false;
    } else if (lookValid) {
        float off = std::remainder(look - g_simYaw, kTwoPi);
        if (std::fabs(off) > kStandingSlack) g_facingHead = true;
        if (g_facingHead) {
            want = look;
            if (std::fabs(off) < 0.05f) g_facingHead = false;
        }
    }
    g_simYaw = std::remainder(g_simYaw + std::remainder(want - g_simYaw, kTwoPi) * ease(kBodyTurnRate, dt), kTwoPi);

    // Put the Sim so its eyes are just ahead of yours, along where you look:
    // your view is inside its head, where its own face and hair never show
    // (mirrors still do). The eyes' offset from the Sim's root (it sways with
    // the animation and nods with your head) turned to the new facing, taken
    // off where your head is. A rig without eye joints puts its head joint a
    // set distance behind your eyes instead.
    bool eyes = finite3(in.eyes);
    const float* mark = eyes ? in.eyes : in.headJoint;
    float ahead = eyes ? in.eyeInset * s : -g_eyeForward * s;
    float lx, lz, wx, wz;
    turnXZ(-actorYaw, mark[0] - cur[0], mark[2] - cur[2], lx, lz);
    // Eyes far from the root: something else moved the body this frame (the
    // game snapping it to a walk's end), so the joints and the root don't
    // match. Placing by them would throw the body a stride away for a frame.
    if (lx * lx + lz * lz > kEyeReach * kEyeReach * s * s && g_markValid) {
        if (g_markSkips < 5)
            LOG("Player: the body's eyes are %.2f from its root (it was moved): kept their last offset",
                std::sqrt(lx * lx + lz * lz));
        ++g_markSkips;
        lx = g_mark[0];
        lz = g_mark[1];
    } else {
        g_mark[0] = lx;
        g_mark[1] = lz;
        g_markValid = true;
    }
    // A jump of the pose while you stand (user 2026-10-17 02:54Z: after the
    // stop clip the body blinks to the idle pose): logged with the time, to
    // line up with the walk clip and teleport lines.
    {
        float jy = mark[1] - cur[1];
        float dx = lx - g_poseMark[0], dz = lz - g_poseMark[1], dy = jy - g_poseMark[2];
        float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (g_poseMarkValid && !pushing && jump > 0.05f * s && g_poseJumpLogs < 100) {
            ++g_poseJumpLogs;
            LOG("Player: your Sim's eyes jumped %.0f cm on the body in one frame (speed %.2f)", jump * 100 / s,
                g_speed);
        }
        g_poseMark[0] = lx;
        g_poseMark[1] = lz;
        g_poseMark[2] = jy;
        g_poseMarkValid = true;
    }
    turnXZ(g_simYaw, lx, lz, wx, wz);
    float root[3];
    root[0] = hx + std::sin(look) * ahead - wx;
    root[2] = hz + std::cos(look) * ahead - wz;
    root[1] = floorAt(root[0], root[2], cur[1]);
    installPin(in.actor);
    pin(in.actor, root, g_simYaw);
    if (!in.deferPlace) place(in.actor, root, g_simYaw);

    // Camera: eye height above the floor under your head, from how tall the
    // Sim stands (slowly, so the walk's bob doesn't move the view).
    float floorHere = floorAt(hx, hz, root[1]);
    g_floorY += (floorHere - g_floorY) * ease(kFloorSmoothing, dt);
    float eye = in.headJoint[1] - cur[1] + (std::isfinite(in.eyeUp) ? in.eyeUp : g_eyeHeight * s);
    // Not at stairs: a walk the client still plays there keeps the body on
    // the floor you came from for a moment, and the view must not follow it.
    bool atStairs = floorFresh && flight[4] > 0;
    if (!atStairs && std::isfinite(eye) && eye > 0.3f * s) {
        // Slowly for the walk's bob, quickly for a real change of height
        // (v0.13.197, user 2026-10-17 02:32Z: standing up from a seat, the view
        // crept up into place: free movement starts while the Sim is still
        // rising, and 0.5/s took seconds to catch up).
        float gap = eye - g_eyeAboveFloor;
        g_eyeAboveFloor += gap * ease(std::fabs(gap) > kEyeCatchUp * s ? kEyeCatchUpRate : kEyeSmoothing, dt);
        if (g_eyeLogs < 40 && std::fabs(gap) > kEyeCatchUp * s) {
            ULONGLONG tick = GetTickCount64();
            if (tick - g_eyeLogTick > 1000) {
                g_eyeLogTick = tick;
                ++g_eyeLogs;
                LOG("Player: your Sim's eyes are %.2f above the floor, the view %.2f: catching up", eye,
                    g_eyeAboveFloor);
            }
        }
    }
    if (atStairs) {
        // Where the body and your view really are on the steps, twice a second
        // (the first 120): the body's eyes should sit at your eyes.
        static ULONGLONG lastLog = 0;
        static int logs = 0;
        ULONGLONG tick = GetTickCount64();
        if (logs < 120 && tick - lastLog > 500) {
            lastLog = tick;
            ++logs;
            const float* its = eyes ? in.eyes : in.headJoint;
            float view[3] = {hx, g_floorY + g_eyeAboveFloor + in.hmd[1] * s, hz};
            LOG("Player: on stairs%s: floor under the body %.2f, under your head %.2f (view floor %.2f); body at "
                "%.2f %.2f %.2f (pinned to %.2f %.2f %.2f); its eyes %.2f %.2f %.2f, your view %.2f %.2f %.2f "
                "(off by %.2f %.2f %.2f); head %.2f above its floor",
                moving ? "" : " (standing)", root[1], floorHere, g_floorY, cur[0], cur[1], cur[2], root[0], root[1],
                root[2], its[0], its[1], its[2], view[0], view[1], view[2], its[0] - view[0], its[1] - view[1],
                its[2] - view[2], in.headJoint[1] - root[1]);
        }
    }

    out.base[0] = g_base[0];
    out.base[1] = g_floorY + g_eyeAboveFloor;
    out.base[2] = g_base[1];
    out.viewYaw = g_viewYaw;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pose[0] = root[0];
    g_pose[1] = root[1];
    g_pose[2] = root[2];
    g_pose[3] = g_simYaw;
    g_pose[4] = moving || pushing ? 1.0f : 0.0f;
    g_pose[5] = 1;
    g_pose[6] = vx;
    g_pose[7] = vz;
    {
        float v = std::sqrt(vx * vx + vz * vz);
        float dx = v > 1e-3f ? vx / v : g_dir[0], dz = v > 1e-3f ? vz / v : g_dir[1];
        g_restPoint[0] = root[0] + dx * g_rest;
        g_restPoint[1] = root[2] + dz * g_rest;
        g_restPoint[2] = g_rest;
    }
    g_poseValid = true;
    return true;
}

}  // namespace s4vr::player
