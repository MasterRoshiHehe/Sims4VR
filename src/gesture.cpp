#include "gesture.h"

#include <atomic>
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
constexpr float kPalmIn = 0.55f;     // come hither: palm up or toward your face at least this much
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

// Finger come hither: the index finger crooked twice while the other fingers
// stay closed (on a controller: grip held, trigger pressed twice).
constexpr float kFist = 0.55f;       // middle, ring and little at least this curled
constexpr float kIndexIn = 0.60f;    // index crooked: curl above this
constexpr float kIndexOut = 0.35f;   // and straightened again below this
constexpr float kFaceYou = 0.35f;    // palm toward your face at least this much
constexpr ULONGLONG kCrookMs = 1200;  // both crooks within this
struct Crook {
    bool in = false;
    ULONGLONG lastAt = 0;  // the first crook's time (0: none yet)
    ULONGLONG lostAt = 0;
    ULONGLONG firedAt = 0;
};
Crook g_crook[2];
// When each hand was last held in the wave pose: a wave's swing also moves the
// fingertips toward and away from your eyes, so no come hither is read from a
// hand that waves (user 2026-10-26: far Sims walked over instead of waving).
ULONGLONG g_waveSeen[2] = {0, 0};
// High five: the raised hand, held still.
struct Raise {
    ULONGLONG since = 0;
    Vec3 anchor{};
    bool fired = false;
};
Raise g_raise[2];
std::atomic<ULONGLONG> g_hiHoldMs{2500};
constexpr float kRaiseAbove = 0.12f;  // palm at most this far below your eyes
constexpr float kRaiseStill = 0.06f;  // the fingertips stay within this (metres) while held
constexpr ULONGLONG kWaveQuietMs = 1000;
constexpr float kNotOut = 0.0f;  // come hither: palm not facing away from you at all

// Shoo (user 2026-10-27 15:01Z): both palms down, both hands flicked up
// together (the fingers swing up from the wrist), a couple of times. Each hand
// is "down" (palm down, fingers level, below your eyes), then "up" (fingers
// swung up by at least the shoo angle) within kShooSwingMs: one flick. Both
// hands flicking within kShooTogetherMs of each other is one shoo flick.
constexpr ULONGLONG kShooSwingMs = 600, kShooTogetherMs = 500, kShooWindowMs = 2000;
struct ShooHand {
    ULONGLONG downAt = 0;   // last time held palm down
    bool up = false;        // flicked and not yet back down
    ULONGLONG flickAt = 0;  // when it flicked
};
ShooHand g_shoo[2];
ULONGLONG g_shooTimes[6] = {};
int g_shooCount = 0;
ULONGLONG g_shooFiredAt = 0;
std::atomic<float> g_shooSin{0.707f};  // sine of the flick angle
std::atomic<int> g_shooFlicks{2};

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

const char* kindName(int kind) {
    return kind == kWave ? "waves" : kind == kComeHither ? "beckons (come hither)" : kind == kHighFive ? "is raised for a high five"
           : kind == kShoo ? "shoos (both hands flicked up)" : "crooks a finger (follow me)";
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
        LOG("Gesture: %s hand %s (%s)", hand ? "right" : "left",
            kindName(kind),
            s.tracked ? "tracked hand" : "controller");
}

}  // namespace

void setHighFiveHold(float seconds) {
    g_hiHoldMs.store(static_cast<ULONGLONG>((seconds < 0.5f ? 0.5f : seconds > 10 ? 10 : seconds) * 1000));
}

void setShoo(float angleDegrees, int flicks) {
    float a = angleDegrees < 20 ? 20 : angleDegrees > 80 ? 80 : angleDegrees;
    g_shooSin.store(std::sin(a * 3.14159265f / 180.0f));
    g_shooFlicks.store(flicks < 1 ? 1 : flicks > 4 ? 4 : flicks);
}

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
        for (int i = 0; i < 2; ++i) reset(g_wave[i]), reset(g_pull[i]), g_crook[i] = Crook{}, g_raise[i] = Raise{}, g_shoo[i] = ShooHand{};
        g_shooCount = 0;
        return;
    }
    Vec3 eye = head.position;
    Vec3 fwd = flat(rotate(head.orientation, {0, 0, -1}));
    if (fwd.x == 0 && fwd.z == 0) return;
    Vec3 right{-fwd.z, 0, fwd.x};
    handshape::Shape shapes[2];
    bool shooOk[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        handshape::Shape s;
        float c[5];
        Vec3 tip{};
        bool ok = handshape::latest(i, s) && now - s.tick < 200 && handshape::curls(s, c) &&
                  handshape::tip(s, 2, tip);
        bool wave = false, pull = false, raised = false;
        if (ok) {
            shapes[i] = s;
            shooOk[i] = true;
            float shooOpen = (c[1] + c[2] + c[3] + c[4]) * 0.25f;
            ShooHand& sh = g_shoo[i];
            bool down = shooOpen <= 0.55f && s.palmNormal.y <= -0.4f && s.forward.y <= 0.2f && s.wrist.y < eye.y - 0.1f &&
                        dot(sub(s.palmCentre, eye), fwd) > 0.1f;
            if (down) {
                sh.downAt = now;
                sh.up = false;
            } else if (!sh.up && sh.downAt && now - sh.downAt <= kShooSwingMs && shooOpen <= 0.6f &&
                       s.forward.y >= g_shooSin.load()) {
                sh.up = true;
                sh.flickAt = now;
            }
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
            if (wave) g_waveSeen[i] = now;
            raised = open <= kOpen && inFront && s.palmCentre.y > eye.y - kRaiseAbove && s.forward.y >= 0.6f &&
                     dot(flat(s.palmNormal), fwd) >= 0.2f;
            pull = open <= kOpen && inFront && (s.palmNormal.y >= kPalmIn || facing >= kPalmIn) &&
                   dot(flat(s.palmNormal), fwd) <= kNotOut && now - g_waveSeen[i] > kWaveQuietMs;
        }
        // Finger come hither: hand upright, palm toward your face, the other
        // fingers closed; each crook of the index finger counts.
        bool fist = false;
        if (ok) {
            Vec3 toEye = sub(eye, s.palmCentre);
            float l = std::sqrt(dot(toEye, toEye));
            float facing = l > 1e-4f ? dot(s.palmNormal, Vec3{toEye.x / l, toEye.y / l, toEye.z / l}) : 0;
            fist = c[2] >= kFist && c[3] >= kFist && c[4] >= kFist && s.forward.y >= kUp && facing >= kFaceYou;
        }
        Crook& k = g_crook[i];
        if (!fist) {
            if (!k.lostAt) k.lostAt = now;
            if (now - k.lostAt > kLostMs) k.in = false, k.lastAt = 0;
        } else {
            k.lostAt = 0;
            if (!k.in && c[1] > kIndexIn) {
                k.in = true;
                if (k.lastAt && now - k.lastAt <= kCrookMs && now - k.firedAt >= kCooldownMs) {
                    k.firedAt = now;
                    k.lastAt = 0;
                    fire(kFollowMe, i, eye, fwd, s);
                } else {
                    k.lastAt = now;
                }
            } else if (k.in && c[1] < kIndexOut) {
                k.in = false;
            }
        }
        Raise& rz = g_raise[i];
        if (!raised) {
            rz.since = 0;
            rz.fired = false;
        } else {
            Vec3 moved = sub(tip, rz.anchor);
            if (!rz.since || dot(moved, moved) > kRaiseStill * kRaiseStill) {
                rz.since = now;
                rz.anchor = tip;
            } else if (!rz.fired && now - rz.since >= g_hiHoldMs.load()) {
                rz.fired = true;
                fire(kHighFive, i, eye, fwd, s);
            }
        }
        if (step(g_wave[i], wave, ok ? dot(sub(tip, eye), right) : 0, kWaveSwing, kWaveTurns, now))
            fire(kWave, i, eye, fwd, s);
        Vec3 d = sub(tip, eye);
        if (!ok) g_shoo[i].up = false, g_shoo[i].downAt = 0;
        if (step(g_pull[i], pull, ok ? std::sqrt(dot(d, d)) : 0, kPullSwing, kPullTurns, now))
            fire(kComeHither, i, eye, fwd, s);
    }
    // Shoo: a flick of each hand, close together in time, counted per pair.
    ShooHand &l = g_shoo[0], &r = g_shoo[1];
    if (shooOk[0] && shooOk[1] && l.up && r.up) {
        ULONGLONG a = l.flickAt > r.flickAt ? l.flickAt : r.flickAt, b = l.flickAt > r.flickAt ? r.flickAt : l.flickAt;
        if (a - b <= kShooTogetherMs && (!g_shooCount || a > g_shooTimes[g_shooCount - 1])) {
            if (g_shooCount == 6) {
                for (int k = 1; k < 6; ++k) g_shooTimes[k - 1] = g_shooTimes[k];
                --g_shooCount;
            }
            g_shooTimes[g_shooCount++] = a;
        }
    }
    int recent = 0;
    for (int k = 0; k < g_shooCount; ++k)
        if (now - g_shooTimes[k] <= kShooWindowMs) ++recent;
    if (recent >= g_shooFlicks.load() && now - g_shooFiredAt >= kCooldownMs) {
        g_shooFiredAt = now;
        g_shooCount = 0;
        fire(kShoo, 0, eye, fwd, shapes[0]);
    }
}

}  // namespace s4vr::gesture
