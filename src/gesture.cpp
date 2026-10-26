#include "gesture.h"

#include <windows.h>
#include <cmath>
#include <mutex>

#include "handshape.h"
#include "log.h"

namespace s4vr::gesture {

namespace {

using camera::Quat;
using camera::Vec3;

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 flat(Vec3 a) {
    float l = std::sqrt(a.x * a.x + a.z * a.z);
    return l > 1e-5f ? Vec3{a.x / l, 0, a.z / l} : Vec3{0, 0, 0};
}
Vec3 rotate(const Quat& q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

constexpr float kBelowEyes = 0.30f;  // wave: the hand at most this far below your eyes (about shoulder height)
constexpr float kOpen = 0.40f;       // mean curl of the four fingers at most this (0 straight .. 1 fist)
constexpr float kUp = 0.35f;         // wave: wrist-to-knuckles at least this much up
constexpr float kPalmOut = 0.25f;    // wave: palm facing away from you at least this much
constexpr float kPalmIn = 0.45f;     // come hither: palm up or toward your face at least this much
constexpr float kWaveSwing = 0.05f;  // a turn counts after the fingertips came back this far (metres)
constexpr float kPullSwing = 0.04f;
constexpr int kWaveTurns = 3, kPullTurns = 3;
constexpr ULONGLONG kWindowMs = 1600, kLostMs = 350, kCooldownMs = 2500;

// A hand held in one pose and moved back and forth along one measure (the
// fingertips sideways for a wave, their distance to your eyes for a come
// hither): counts the turns.
struct Swing {
    bool raised = false;
    ULONGLONG lostAt = 0;
    int dir = 0;        // +1 the measure growing, -1 shrinking, 0 not yet
    float extreme = 0;  // furthest value in this direction
    ULONGLONG turns[8] = {};
    int turnCount = 0;
    ULONGLONG firedAt = 0;
};
Swing g_wave[2], g_pull[2];

std::mutex g_mutex;
Event g_queue[8];
int g_count = 0;
int g_logs = 0;

void push(const Event& e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_count < 8) g_queue[g_count++] = e;
}

void reset(Swing& w) {
    w.raised = false;
    w.lostAt = 0;
    w.dir = 0;
    w.turnCount = 0;
}

// One frame: true when `turns` turns of at least `swing` came within the window.
bool step(Swing& w, bool held, float x, float swing, int turns, ULONGLONG now) {
    if (!held) {
        if (w.raised && !w.lostAt) w.lostAt = now;
        if (w.lostAt && now - w.lostAt > kLostMs) reset(w);
        return false;
    }
    w.lostAt = 0;
    if (!w.raised) {
        w.raised = true;
        w.dir = 0;
        w.extreme = x;
        w.turnCount = 0;
        return false;
    }
    if (w.dir == 0) {
        if (std::fabs(x - w.extreme) >= swing) w.dir = x > w.extreme ? 1 : -1, w.extreme = x;
        return false;
    }
    if ((x - w.extreme) * w.dir > 0) {
        w.extreme = x;  // still going the same way
        return false;
    }
    if (std::fabs(x - w.extreme) < swing) return false;
    // Turned back: one turn.
    w.dir = -w.dir;
    w.extreme = x;
    if (w.turnCount == 8) {
        for (int k = 1; k < 8; ++k) w.turns[k - 1] = w.turns[k];
        --w.turnCount;
    }
    w.turns[w.turnCount++] = now;
    int recent = 0;
    for (int k = 0; k < w.turnCount; ++k)
        if (now - w.turns[k] <= kWindowMs) ++recent;
    if (recent < turns || now - w.firedAt < kCooldownMs) return false;
    w.firedAt = now;
    w.turnCount = 0;
    return true;
}

void fire(int kind, int hand, const Vec3& eye, const Vec3& fwd, const handshape::Shape& s) {
    Event e;
    e.kind = kind;
    e.hand = hand;
    Vec3 p{}, f{}, palm{};
    if (!camera::xrToWorld(eye, p) || !camera::xrDirectionToWorld(fwd, f) || !camera::xrToWorld(s.palmCentre, palm))
        return;
    e.head[0] = p.x, e.head[1] = p.y, e.head[2] = p.z;
    e.forward[0] = f.x, e.forward[1] = f.y, e.forward[2] = f.z;
    e.palm[0] = palm.x, e.palm[1] = palm.y, e.palm[2] = palm.z;
    push(e);
    if (g_logs++ < 60)
        LOG("Gesture: %s hand %s (%s)", hand ? "right" : "left", kind == kWave ? "waves" : "beckons (come hither)",
            s.tracked ? "tracked hand" : "controller");
}

}  // namespace

bool takeEvent(Event& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_count) return false;
    out = g_queue[0];
    for (int i = 1; i < g_count; ++i) g_queue[i - 1] = g_queue[i];
    --g_count;
    return true;
}

void update(const grab::HeadPose& head, bool firstPerson) {
    ULONGLONG now = GetTickCount64();
    if (!firstPerson || !head.valid) {
        for (int i = 0; i < 2; ++i) reset(g_wave[i]), reset(g_pull[i]);
        return;
    }
    Vec3 eye = head.position;
    Vec3 fwd = flat(rotate(head.orientation, {0, 0, -1}));
    if (fwd.x == 0 && fwd.z == 0) return;
    Vec3 right{-fwd.z, 0, fwd.x};
    for (int i = 0; i < 2; ++i) {
        handshape::Shape s;
        float c[5];
        Vec3 tip{};
        bool ok = handshape::latest(i, s) && now - s.tick < 200 && handshape::curls(s, c) &&
                  handshape::tip(s, 2, tip);
        bool wave = false, pull = false;
        if (ok) {
            float open = (c[1] + c[2] + c[3] + c[4]) * 0.25f;
            bool inFront = dot(sub(s.palmCentre, eye), fwd) > -0.05f;
            // Wave: hand up, palm away from you.
            wave = open <= kOpen && inFront && s.wrist.y > eye.y - kBelowEyes && s.forward.y >= kUp &&
                   dot(flat(s.palmNormal), fwd) >= kPalmOut;
            // Come hither: open palm up or toward your face, the fingers
            // pulled toward you and back (hand or wrist).
            Vec3 toEye = sub(eye, s.palmCentre);
            float l = std::sqrt(dot(toEye, toEye));
            float facing = l > 1e-4f ? dot(s.palmNormal, Vec3{toEye.x / l, toEye.y / l, toEye.z / l}) : 0;
            pull = open <= kOpen && inFront && (s.palmNormal.y >= kPalmIn || facing >= kPalmIn);
        }
        if (step(g_wave[i], wave, ok ? dot(sub(tip, eye), right) : 0, kWaveSwing, kWaveTurns, now))
            fire(kWave, i, eye, fwd, s);
        Vec3 d = sub(tip, eye);
        if (step(g_pull[i], pull, ok ? std::sqrt(dot(d, d)) : 0, kPullSwing, kPullTurns, now))
            fire(kComeHither, i, eye, fwd, s);
    }
}

}  // namespace s4vr::gesture
