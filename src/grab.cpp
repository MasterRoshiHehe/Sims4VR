#include "grab.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"

namespace s4vr::grab {

namespace {

using camera::Quat;
using camera::Vec3;

constexpr float kGripOn = 0.55f, kGripOff = 0.35f;  // grip value that closes / opens the hand
// A hand that has just closed still grabs this long after: the index finger
// often settles on the trigger a moment after the grip (until then it points).
constexpr ULONGLONG kGrabWindowMs = 250;

std::mutex g_mutex;  // everything below; never held while calling out
uint64_t g_ids[kMaxGrabbables] = {};
float g_xyzr[kMaxGrabbables][4] = {};
int g_count = 0;
ULONGLONG g_listTick = 0;

Event g_events[16];
int g_eventHead = 0, g_eventCount = 0;

Snapshot g_snapshot;
bool g_snapshotValid = false;
ULONGLONG g_snapshotTick = 0;

struct Buzz {
    float amplitude = 0, seconds = 0;
    bool pending = false;
};
Buzz g_buzz[2];

std::atomic<uint64_t> g_held[2] = {{0}, {0}};
bool g_closed[2] = {false, false};  // render thread: grip down (with hysteresis)
ULONGLONG g_closedAt[2] = {0, 0};    // when it closed
int g_logs = 0;

void push(int kind, int hand, uint64_t id) {
    if (g_eventCount == 16) {  // Python stopped reading; drop the oldest
        g_eventHead = (g_eventHead + 1) % 16;
        --g_eventCount;
    }
    g_events[(g_eventHead + g_eventCount++) % 16] = {kind, hand, id};
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// World orientation of an app-space orientation: the app space's heading in
// the world turned first. Built from two world directions.
bool worldOrientation(const Quat& q, float out[4]) {
    Vec3 f{}, u{};
    if (!camera::xrDirectionToWorld(rotate(q, {0, 0, -1}), f) || !camera::xrDirectionToWorld(rotate(q, {0, 1, 0}), u))
        return false;
    // Rotation whose -Z is f and +Y is u (both unit): from its basis.
    Vec3 z{-f.x, -f.y, -f.z};
    Vec3 x{u.y * z.z - u.z * z.y, u.z * z.x - u.x * z.z, u.x * z.y - u.y * z.x};
    float m00 = x.x, m01 = u.x, m02 = z.x, m10 = x.y, m11 = u.y, m12 = z.y, m20 = x.z, m21 = u.z, m22 = z.z;
    float trace = m00 + m11 + m22;
    float qx, qy, qz, qw;
    if (trace > 0) {
        float s = std::sqrt(trace + 1.0f) * 2;
        qw = 0.25f * s;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        qw = (m21 - m12) / s;
        qx = 0.25f * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25f * s;
        qz = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25f * s;
    }
    float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(n > 0.5f)) return false;
    out[0] = qx / n;
    out[1] = qy / n;
    out[2] = qz / n;
    out[3] = qw / n;
    return true;
}

}  // namespace

void setGrabbables(const uint64_t* ids, const float* xyzr, int count) {
    if (count < 0) count = 0;
    if (count > kMaxGrabbables) count = kMaxGrabbables;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_count = count;
    std::memcpy(g_ids, ids, sizeof(uint64_t) * count);
    std::memcpy(g_xyzr, xyzr, sizeof(float) * 4 * count);
    g_listTick = GetTickCount64();
}

bool takeEvent(Event& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_eventCount) return false;
    out = g_events[g_eventHead];
    g_eventHead = (g_eventHead + 1) % 16;
    --g_eventCount;
    return true;
}

void setHeld(int hand, uint64_t id) {
    if (hand < 0 || hand > 1) return;
    g_held[hand].store(id);
}

bool snapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_snapshotValid || GetTickCount64() - g_snapshotTick > 500) return false;
    out = g_snapshot;
    for (int i = 0; i < 2; ++i) out.hand[i].held = g_held[i].load();
    return true;
}

void haptic(int hand, float amplitude, float seconds) {
    if (hand < 0 || hand > 1) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_buzz[hand] = {amplitude < 0 ? 0 : amplitude > 1 ? 1 : amplitude, seconds < 0 ? 0 : seconds > 1 ? 1 : seconds,
                    true};
}

bool takeHaptic(int hand, float& amplitude, float& seconds) {
    if (hand < 0 || hand > 1) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_buzz[hand].pending) return false;
    g_buzz[hand].pending = false;
    amplitude = g_buzz[hand].amplitude;
    seconds = g_buzz[hand].seconds;
    return true;
}

uint64_t holding(int hand) { return hand >= 0 && hand < 2 ? g_held[hand].load() : 0; }

namespace {
int g_controls[8];
int g_controlCount = 0;
}  // namespace

void pushControl(int what) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_controlCount < 8) g_controls[g_controlCount++] = what;
}

bool takeControl(int& what) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_controlCount) return false;
    what = g_controls[0];
    for (int i = 1; i < g_controlCount; ++i) g_controls[i - 1] = g_controls[i];
    --g_controlCount;
    return true;
}

void update(const armik::Hand hands[2], const float trigger[2], const bool pointing[2], const HeadPose& head,
            bool firstPerson, bool grabbing) {
    // World positions first: camera:: takes its own lock, never under ours.
    Snapshot snap;
    Vec3 palm[2];
    bool palmOk[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        if (!firstPerson || !hands[i].valid) continue;
        palmOk[i] = camera::xrToWorld(hands[i].position, palm[i]);
        if (!palmOk[i]) continue;
        auto& h = snap.hand[i];
        h.valid = worldOrientation(hands[i].orientation, h.q);
        h.palm[0] = palm[i].x;
        h.palm[1] = palm[i].y;
        h.palm[2] = palm[i].z;
        h.grip = hands[i].grip;
        h.trigger = trigger[i];
        palmOk[i] = h.valid;
    }
    if (firstPerson && head.valid) {
        Vec3 p{}, f{}, u{};
        if (camera::xrToWorld(head.position, p) && camera::xrDirectionToWorld(rotate(head.orientation, {0, 0, -1}), f) &&
            camera::xrDirectionToWorld(rotate(head.orientation, {0, 1, 0}), u)) {
            snap.head = true;
            snap.hmd[0] = p.x, snap.hmd[1] = p.y, snap.hmd[2] = p.z;
            snap.forward[0] = f.x, snap.forward[1] = f.y, snap.forward[2] = f.z;
            snap.up[0] = u.x, snap.up[1] = u.y, snap.up[2] = u.z;
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    g_snapshot = snap;
    g_snapshotValid = firstPerson;
    g_snapshotTick = now;
    bool listFresh = now - g_listTick < 1000;
    for (int i = 0; i < 2; ++i) {
        float grip = firstPerson && grabbing ? hands[i].grip : 0.0f;
        bool closed = g_closed[i] ? grip > kGripOff : grip > kGripOn;
        bool wasClosed = g_closed[i];
        g_closed[i] = closed;
        if (closed && !wasClosed) g_closedAt[i] = now;
        uint64_t held = g_held[i].load();
        bool closing = closed && now - g_closedAt[i] <= kGrabWindowMs;
        if (closing && !held && !pointing[i] && palmOk[i] && listFresh) {
            // The nearest object within its reach, not already in the other hand.
            int best = -1;
            float bestD = 1e9f;
            for (int k = 0; k < g_count; ++k) {
                if (g_ids[k] == g_held[1 - i].load()) continue;
                float dx = palm[i].x - g_xyzr[k][0], dy = palm[i].y - g_xyzr[k][1], dz = palm[i].z - g_xyzr[k][2];
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < g_xyzr[k][3] && d < bestD) {
                    best = k;
                    bestD = d;
                }
            }
            if (best >= 0) {
                g_held[i].store(g_ids[best]);
                push(1, i, g_ids[best]);
                if (g_logs++ < 40)
                    LOG("Grab: %s hand closed on object %016llx (%.2f m from its centre)", i ? "right" : "left",
                        static_cast<unsigned long long>(g_ids[best]), bestD);
            }
        } else if (!closed && wasClosed && held) {
            g_held[i].store(0);
            push(2, i, held);
            if (g_logs++ < 40)
                LOG("Grab: %s hand let go of object %016llx", i ? "right" : "left", static_cast<unsigned long long>(held));
        } else if (!closed && held) {
            // Python set a hold while the hand was open (or first person ended): let go.
            g_held[i].store(0);
            push(2, i, held);
        }
    }
}

}  // namespace s4vr::grab
