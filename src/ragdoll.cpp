#include "ragdoll.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

#include "log.h"
#include "settings.h"

namespace s4vr::ragdoll {

namespace {

struct V {
    float x = 0, y = 0, z = 0;
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(V a) { return std::sqrt(dot(a, a)); }
V unit(V a) {
    float l = len(a);
    return l > 1e-8f ? a * (1 / l) : V{0, 0, 0};
}
V load(const float p[3]) { return {p[0], p[1], p[2]}; }
void store(V a, float p[3]) {
    p[0] = a.x;
    p[1] = a.y;
    p[2] = a.z;
}
bool finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// ---- settings ----------------------------------------------------------------

std::atomic<bool> g_on{true}, g_test{false};
std::atomic<float> g_strength{0.5f}, g_limp{4.0f}, g_gravity{1.0f}, g_damping{1.5f};
std::atomic<ULONGLONG> g_readAt{0};

float number(const char* key, float def) {
    std::string v;
    if (!settings::liveValue("ragdoll", key, v) || v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() || !std::isfinite(f) ? def : f;
}

void readSettings() {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_readAt.load(std::memory_order_relaxed);
    if (now - last < 1000 && last) return;
    g_readAt.store(now, std::memory_order_relaxed);
    g_on = number("on", 1) != 0;
    g_test = number("test_on_hit", 0) != 0;
    g_strength = number("strength", 0.5f);
    g_limp = number("limp_seconds", 4.0f);
    g_gravity = number("gravity", 1.0f);
    g_damping = number("damping", 1.5f);
}

// ---- the body ------------------------------------------------------------------
//
// Two rigid pieces, the pelvis (P, both hips and a point above it) and the
// chest (neck, both shoulders and a point above it), joined at P-neck, so the
// spine bends and twists between them within limits; the head, arms and legs
// hang off them on bones of fixed length, with knees and elbows that bend one
// way and never fold shut.

constexpr int kMaxCons = 48;
constexpr float kRamp = 0.15f;      // seconds the animation's hold on the body fades out after the blow
constexpr float kBlendOut = 0.7f;   // seconds from the ragdoll back to the game's pose
constexpr int kIterations = 8;
constexpr float kSubstep = 1.0f / 120.0f;
constexpr int kMaxSubsteps = 4;
constexpr float kSpineBend = 0.5f;  // radians each end may lean off the spine's line
constexpr float kUpReach = 0.25f;   // metres (adult) from the pelvis / neck to its virtual point

// Collision radius of each point (adult, metres; 0: no floor) and its weight.
const float kRadius[kSimPoints] = {0.11f, 0.09f, 0.09f, 0.08f, 0.10f, 0.06f, 0.06f, 0.04f, 0.04f,
                                   0.035f, 0.035f, 0.06f, 0.06f, 0.07f, 0.07f, 0, 0};
const float kMass[kSimPoints] = {3.0f, 1.5f, 1.5f, 2.0f, 1.5f, 1.0f, 1.0f, 0.6f, 0.6f,
                                 0.4f, 0.4f, 1.0f, 1.0f, 0.6f, 0.6f, 0.3f, 0.3f};

struct Con {
    int a, b;
    float lo, hi;  // allowed distance (equal: a bone); hi < 0: no upper limit
};

struct Slot {
    std::atomic<const void*> actor{nullptr};
    Params params;
    bool init = false;
    bool releasing = false;
    float t = 0;          // seconds since the blow
    float releaseAt = 0;  // t at which the blend back begins
    float blend = 1;      // 1 the ragdoll, 0 the game's pose
    V x[kSimPoints], v[kSimPoints];
    Con cons[kMaxCons];
    int consCount = 0;
    V legFwd{0, 0, 1}, armFwd{0, 0, 1};  // the way the body faces, in the pelvis' and the chest's own frame
    float size = 1;
};
constexpr int kSlots = 6;
Slot g_slots[kSlots];
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

struct Lock {
    Lock() {
        while (g_lock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    }
    ~Lock() { g_lock.clear(std::memory_order_release); }
};

Slot* find(const void* actor) {
    for (Slot& s : g_slots)
        if (s.actor.load(std::memory_order_relaxed) == actor) return &s;
    return nullptr;
}

// A right-handed frame from a sideways and an up direction (columns side, up, forward).
void basis(V sideDir, V upDir, V& side, V& up, V& fwd) {
    side = unit(sideDir);
    V u = unit(upDir);
    fwd = unit(cross(side, u));
    up = cross(fwd, side);
}

void toQuat(V x, V y, V z, float q[4]) {
    float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    float tr = m00 + m11 + m22, qx, qy, qz, qw;
    if (tr > 0) {
        float s = std::sqrt(tr + 1) * 2;
        qw = s / 4;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        qw = (m21 - m12) / s;
        qx = s / 4;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = s / 4;
        qz = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = s / 4;
    }
    float l = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(l > 1e-8f)) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    q[0] = qx / l;
    q[1] = qy / l;
    q[2] = qz / l;
    q[3] = qw / l;
}

void addCon(Slot& s, int a, int b, float lo = -1, float hi = -1) {
    if (s.consCount >= kMaxCons) return;
    float rest = len(s.x[a] - s.x[b]);
    if (lo < 0) {  // a bone: its length now
        lo = rest;
        hi = rest;
    } else {  // a range, as shares of the length now (hi < 0: no upper limit)
        lo *= rest;
        if (hi >= 0) hi *= rest;
    }
    s.cons[s.consCount++] = {a, b, lo, hi};
}

void begin(Slot& s, const V* in, V forward, float size) {
    for (int i = 0; i < kPoints; ++i) {
        s.x[i] = in[i];
        s.v[i] = {0, 0, 0};
    }
    s.size = size;
    V u0 = unit(s.x[kNeck] - s.x[kPelvis]);
    s.x[kPelvisUp] = s.x[kPelvis] + u0 * (kUpReach * size);
    s.x[kChestUp] = s.x[kNeck] + u0 * (kUpReach * size);
    s.v[kPelvisUp] = s.v[kChestUp] = {0, 0, 0};
    s.consCount = 0;
    const int pelvis[4] = {kPelvis, kHipL, kHipR, kPelvisUp}, chest[4] = {kNeck, kShoulderL, kShoulderR, kChestUp};
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) {
            addCon(s, pelvis[i], pelvis[j]);
            addCon(s, chest[i], chest[j]);
        }
    addCon(s, kPelvis, kNeck);
    // The spine bends and twists a good way, not all the way round.
    {
        const float L = len(s.x[kNeck] - s.x[kPelvis]), r = kUpReach * size, c = std::cos(kSpineBend);
        s.cons[s.consCount++] = {kPelvisUp, kNeck, 0, std::sqrt(L * L + r * r - 2 * L * r * c)};
        s.cons[s.consCount++] = {kChestUp, kPelvis, std::sqrt(L * L + r * r + 2 * L * r * c), -1};
    }
    addCon(s, kHipL, kShoulderL, 0.8f, 1.2f);
    addCon(s, kHipR, kShoulderR, 0.8f, 1.2f);
    addCon(s, kHipL, kShoulderR, 0.85f, 1.15f);
    addCon(s, kHipR, kShoulderL, 0.85f, 1.15f);
    addCon(s, kNeck, kHead);
    for (int side = 0; side < 2; ++side) {
        addCon(s, kShoulderL + side, kElbowL + side);
        addCon(s, kElbowL + side, kHandL + side);
        addCon(s, kHipL + side, kKneeL + side);
        addCon(s, kKneeL + side, kFootL + side);
        // Elbows and knees never fold shut.
        addCon(s, kShoulderL + side, kHandL + side, 0.3f, -1);
        addCon(s, kHipL + side, kFootL + side, 0.3f, -1);
    }
    // Which way the body faces, in the frames the knees and elbows bend by.
    forward = unit(forward);
    V side, up, fwd;
    basis(s.x[kHipL] - s.x[kHipR], s.x[kPelvisUp] - s.x[kPelvis], side, up, fwd);
    s.legFwd = {dot(forward, side), dot(forward, up), dot(forward, fwd)};
    basis(s.x[kShoulderL] - s.x[kShoulderR], s.x[kChestUp] - s.x[kNeck], side, up, fwd);
    s.armFwd = {dot(forward, side), dot(forward, up), dot(forward, fwd)};
    // The blow.
    const Params& pr = s.params;
    V vel = load(pr.velocity);
    for (int i = 0; i < kSimPoints; ++i) {
        float w = 1;
        if (pr.hasPoint) {
            V d = s.x[i] - load(pr.point);
            float r = 0.35f * size;
            w = std::exp(-dot(d, d) / (r * r));
            if (w < 0.25f) w = 0.25f;
        }
        s.v[i] = s.v[i] + vel * w;
    }
    s.init = true;
}

// Knees bend forward and elbows backward, from the way the body faces: pushes
// `k` (the knee or elbow) to the allowed side of the line a-b.
void hinge(V* x, int a, int k, int b, V fwd, float sign) {
    V ab = x[b] - x[a];
    float l2 = dot(ab, ab);
    if (l2 < 1e-8f) return;
    float t = dot(x[k] - x[a], ab) / l2;
    V off = x[k] - (x[a] + ab * t);
    float c = dot(off, fwd) * sign;
    if (c < 0) x[k] = x[k] - fwd * (c * sign);
}

void substep(Slot& s, const V* anim, float animWeight, float floorY, float h) {
    const float g = 9.81f * g_gravity.load(std::memory_order_relaxed);
    const float damp = std::exp(-g_damping.load(std::memory_order_relaxed) * h);
    V xp[kSimPoints];
    for (int i = 0; i < kSimPoints; ++i) {
        s.v[i].y -= g * h;
        s.v[i] = s.v[i] * damp;
        xp[i] = s.x[i] + s.v[i] * h;
        if (animWeight > 0 && i < kPoints) {  // held toward the animation without giving it speed
            V pull = (anim[i] - xp[i]) * (animWeight > 1 ? 1 : animWeight);
            xp[i] = xp[i] + pull;
            s.x[i] = s.x[i] + pull;
        }
    }
    for (int it = 0; it < kIterations; ++it) {
        for (int c = 0; c < s.consCount; ++c) {
            const Con& con = s.cons[c];
            V d = xp[con.b] - xp[con.a];
            float l = len(d);
            if (l < 1e-6f) continue;
            float target = l;
            if (l < con.lo) target = con.lo;
            else if (con.hi >= 0 && l > con.hi) target = con.hi;
            if (target == l) continue;
            float wa = 1 / kMass[con.a], wb = 1 / kMass[con.b];
            V corr = d * ((l - target) / (l * (wa + wb)));
            xp[con.a] = xp[con.a] + corr * wa;
            xp[con.b] = xp[con.b] - corr * wb;
        }
        for (int i = 0; i < kPoints; ++i) {
            float floor = floorY + kRadius[i] * s.size;
            if (xp[i].y < floor) xp[i].y = floor;
        }
    }
    // The joints' bend limits move the knee or elbow without giving it speed
    // (the next step's bone lengths take up the slack).
    V side, up, fwd;
    V before[kSimPoints];
    for (int i = 0; i < kSimPoints; ++i) before[i] = xp[i];
    basis(xp[kHipL] - xp[kHipR], xp[kPelvisUp] - xp[kPelvis], side, up, fwd);
    V legF = side * s.legFwd.x + up * s.legFwd.y + fwd * s.legFwd.z;
    basis(xp[kShoulderL] - xp[kShoulderR], xp[kChestUp] - xp[kNeck], side, up, fwd);
    V armF = side * s.armFwd.x + up * s.armFwd.y + fwd * s.armFwd.z;
    hinge(xp, kHipL, kKneeL, kFootL, legF, 1);
    hinge(xp, kHipR, kKneeR, kFootR, legF, 1);
    hinge(xp, kShoulderL, kElbowL, kHandL, armF, -1);
    hinge(xp, kShoulderR, kElbowR, kHandR, armF, -1);
    for (int i = 0; i < kPoints; ++i) {
        float floor = floorY + kRadius[i] * s.size;
        if (xp[i].y < floor) xp[i].y = floor;
    }
    for (int i = 0; i < kSimPoints; ++i) {
        // The limits moved the point; its old place moves with it, so that gives it no speed.
        s.x[i] = s.x[i] + (xp[i] - before[i]);
        s.v[i] = (xp[i] - s.x[i]) * (1 / h);
        if (i < kPoints && xp[i].y <= floorY + kRadius[i] * s.size + 1e-4f) {  // on the floor: friction
            s.v[i].x *= 0.75f;
            s.v[i].z *= 0.75f;
        }
        s.x[i] = xp[i];
    }
}

}  // namespace

bool enabled() {
    readSettings();
    return g_on.load(std::memory_order_relaxed);
}
bool testOnHit() {
    readSettings();
    return g_test.load(std::memory_order_relaxed);
}
float blowStrength() {
    readSettings();
    return g_strength.load(std::memory_order_relaxed);
}
float limpSeconds() {
    readSettings();
    return g_limp.load(std::memory_order_relaxed);
}

bool start(const void* actor, const Params& params) {
    if (!actor || !enabled()) return false;
    Lock lock;
    if (find(actor)) return false;
    for (Slot& s : g_slots) {
        if (s.actor.load(std::memory_order_relaxed)) continue;
        s.params = params;
        s.init = false;
        s.releasing = false;
        s.t = 0;
        s.blend = 1;
        s.actor.store(actor, std::memory_order_release);
        LOG("Ragdoll: starts on %p (blow %.1f m/s, limp %.1f s)", actor, len(load(params.velocity)),
            params.limpSeconds);
        return true;
    }
    return false;
}

void release(const void* actor) {
    Lock lock;
    Slot* s = find(actor);
    if (s && !s->releasing) {
        s->releasing = true;
        s->releaseAt = s->t;
    }
}

void cancel(const void* actor) {
    Lock lock;
    if (Slot* s = find(actor)) s->actor.store(nullptr, std::memory_order_release);
}

bool active(const void* actor) {
    if (!actor) return false;
    for (const Slot& s : g_slots)
        if (s.actor.load(std::memory_order_acquire) == actor) return true;
    return false;
}

void frames(const float pts[kSimPoints][3], bool sim, float pelvis[4], float chest[4]) {
    V p[kSimPoints];
    for (int i = 0; i < (sim ? kSimPoints : kPoints); ++i) p[i] = load(pts[i]);
    V side, up, fwd;
    basis(p[kHipL] - p[kHipR], (sim ? p[kPelvisUp] - p[kPelvis] : p[kNeck] - p[kPelvis]), side, up, fwd);
    toQuat(side, up, fwd, pelvis);
    basis(p[kShoulderL] - p[kShoulderR], (sim ? p[kChestUp] - p[kNeck] : p[kNeck] - p[kPelvis]), side, up, fwd);
    toQuat(side, up, fwd, chest);
}

bool step(const void* actor, const float in[kPoints][3], const float forward[3], float floorY, float size, float dt,
          float out[kSimPoints][3]) {
    Lock lock;
    Slot* s = find(actor);
    if (!s) return false;
    V anim[kSimPoints];
    for (int i = 0; i < kPoints; ++i) {
        anim[i] = load(in[i]);
        if (!finite(anim[i])) {
            s->actor.store(nullptr, std::memory_order_release);
            return false;
        }
    }
    V u = unit(anim[kNeck] - anim[kPelvis]);
    anim[kPelvisUp] = anim[kPelvis] + u * (kUpReach * size);
    anim[kChestUp] = anim[kNeck] + u * (kUpReach * size);
    if (!s->init) begin(*s, anim, load(forward), size);
    if (!(dt > 0)) dt = 0;
    if (dt > 0.05f) dt = 0.05f;
    // Past the limp time: blend back.
    if (!s->releasing && s->params.limpSeconds > 0 && s->t >= s->params.limpSeconds) {
        s->releasing = true;
        s->releaseAt = s->t;
    }
    if (s->releasing) {
        s->blend = 1 - (s->t - s->releaseAt) / kBlendOut;
        if (s->blend <= 0) {
            LOG("Ragdoll: ends on %p", actor);
            s->actor.store(nullptr, std::memory_order_release);
            return false;
        }
    }
    int n = static_cast<int>(std::ceil(dt / kSubstep));
    if (n > kMaxSubsteps) n = kMaxSubsteps;
    if (dt > 0 && n < 1) n = 1;
    for (int k = 0; k < n; ++k) {
        float h = dt / n;
        float animWeight = 0.5f * (1 - s->t / kRamp);
        substep(*s, anim, animWeight > 0 ? animWeight : 0, floorY, h);
        s->t += h;
    }
    float b = s->releasing ? s->blend : 1;
    b = b * b * (3 - 2 * b);  // smoothstep
    for (int i = 0; i < kSimPoints; ++i) store(anim[i] + (s->x[i] - anim[i]) * b, out[i]);
    return true;
}

}  // namespace s4vr::ragdoll
