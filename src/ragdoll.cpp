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
std::atomic<float> g_strength{0.5f}, g_limp{4.0f}, g_gravity{1.0f}, g_damping{1.5f}, g_tether{1.2f}, g_blendOut{1.0f};
std::atomic<float> g_soft{0.8f}, g_relaxed{0.5f}, g_spin{0.5f};
std::atomic<bool> g_world{true}, g_flinch{true};
std::atomic<float> g_fStrength{0.6f}, g_fRecover{0.4f}, g_fLocal{0.1f}, g_fBody{0.9f}, g_fSpread{0.45f};
std::atomic<float> g_stOmega{5.0f}, g_stLean{0.7f}, g_stStep{0.08f}, g_stStepTime{0.22f};
std::atomic<WorldQuery> g_query{nullptr};
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
    g_tether = number("tether", 1.2f);
    g_blendOut = number("blend_out", 1.0f);
    g_world = number("world", 1) != 0;
    // Showcase (default on): the test-relevant rows are forced to values that show the system.
    const bool show = number("showcase", 1) != 0;
    g_soft = show ? 0.8f : number("soft_limits", 0.8f);
    g_relaxed = show ? 0.5f : number("relaxed", 0.5f);
    g_spin = show ? 0.5f : number("hit_spin", 0.5f);
    g_flinch = show || number("flinch", 1) != 0;
    g_fStrength = show ? 0.6f : number("flinch_strength", 0.35f);
    g_fRecover = number("flinch_recover", 0.4f);
    g_fLocal = number("flinch_local", 0.1f);
    g_fBody = number("flinch_body", 0.9f);
    g_fSpread = number("flinch_spread", 0.45f);
    g_stOmega = number("stagger_omega", 5.0f);
    g_stLean = number("stagger_lean", 0.7f);
    g_stStep = number("stagger_step", 0.08f);
    g_stStepTime = number("stagger_step_time", 0.22f);
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
constexpr ULONGLONG kStaleMs = 2000;   // a ragdoll nobody poses for this long is over (the Sim went out of range)
constexpr ULONGLONG kMaxMs = 60000;    // and none lasts longer than this
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
    float stiff[kPoints]; // reaction mode: each point's stiffness at t = 0
    V x[kSimPoints], v[kSimPoints];
    Con cons[kMaxCons];
    int consCount = 0;
    V legFwd{0, 0, 1}, armFwd{0, 0, 1};  // the way the body faces, in the pelvis' and the chest's own frame
    float size = 1;
    ULONGLONG startedAt = 0, lastStep = 0;
    float root[3] = {0, 0, 0};
    // Stagger: the hips' offset from the pose (d, dv), each foot's offset (fo) and its step
    // (from, to, progress 0..1, < 0: planted).
    V d{0, 0, 0}, dv{0, 0, 0};
    V fo[2]{{0, 0, 0}, {0, 0, 0}}, foFrom[2]{{0, 0, 0}, {0, 0, 0}}, foTo[2]{{0, 0, 0}, {0, 0, 0}};
    float stepU[2] = {-1, -1};
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
    const bool reaction = pr.recoverSeconds > 0;
    for (int i = 0; i < kSimPoints; ++i) {
        float w = 1;
        if (pr.hasPoint) {
            V d = s.x[i] - load(pr.point);
            float r = (reaction ? pr.spread : 0.35f) * size;
            w = std::exp(-dot(d, d) / (r * r));
            if (!reaction && w < 0.25f) w = 0.25f;
            if (i < kPoints) s.stiff[i] = pr.localStiffness + (pr.stiffness - pr.localStiffness) * (1 - w);
        } else if (i < kPoints) {
            s.stiff[i] = pr.stiffness;
        }
        s.v[i] = s.v[i] + vel * w;
    }
    // Spin: the shoulders and hips take a turn from the blow, so the arms trail
    // along it and the legs lag behind (feet resist), each limb a little
    // different. Falls only; a flinch keeps its limbs on the pose.
    const float spin = g_spin.load(std::memory_order_relaxed);
    const float speed = len(vel);
    if (!reaction && spin > 0 && speed > 0.2f) {
        V dir = vel * (1 / speed);
        ULONGLONG seed = GetTickCount64() * 2654435761u + reinterpret_cast<ULONGLONG>(&s);
        auto rnd = [&seed]() {
            seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
            return 0.6f + 0.8f * static_cast<float>(seed & 1023) / 1023.0f;
        };
        const int limbs[4][3] = {{kShoulderL, kElbowL, kHandL}, {kShoulderR, kElbowR, kHandR},
                                 {kHipL, kKneeL, kFootL}, {kHipR, kKneeR, kFootR}};
        for (int l = 0; l < 4; ++l) {
            float k = spin * speed * rnd() * (l < 2 ? 0.6f : -0.5f);
            for (int j = 1; j < 3; ++j) s.v[limbs[l][j]] = s.v[limbs[l][j]] + dir * (k * j * 0.5f);
        }
    }
    s.d = V{0, 0, 0};
    s.dv = V{pr.staggerVelocity[0], 0, pr.staggerVelocity[2]};
    for (int f = 0; f < 2; ++f) s.fo[f] = s.foFrom[f] = s.foTo[f] = V{0, 0, 0}, s.stepU[f] = -1;
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

// Reaction mode: how much of the way to the game's pose each point goes per
// second, as a share (1 - exp(-rate * stiffness * h) a step), rising to full
// stiffness over recoverSeconds.
constexpr float kPullRate = 60.0f;

float ramp(float x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }

// Soft limits: a limb slows as it nears full stretch or folded shut, and the
// range cones of the spine as they near their edge, instead of stopping dead
// (muscles and tendons taking up the motion). Relaxed pose: a weak pull of the
// knees and elbows toward a slight bend, so a limp body settles like a person
// and not like a rod. Both act on the speed the points will have after this step
// (s.v is derived from x - old x, so they are applied to the old place).
void softAndRelax(Slot& s, V* xp, V legF, V armF, float h) {
    const float soft = g_soft.load(std::memory_order_relaxed);
    const float relaxed = g_relaxed.load(std::memory_order_relaxed);
    if (soft <= 0 && relaxed <= 0) return;
    const int limb[4][3] = {{kHipL, kKneeL, kFootL}, {kHipR, kKneeR, kFootR},
                            {kShoulderL, kElbowL, kHandL}, {kShoulderR, kElbowR, kHandR}};
    for (int l = 0; l < 4; ++l) {
        const int a = limb[l][0], k = limb[l][1], b = limb[l][2];
        V fwd = l < 2 ? legF : armF;
        float sign = l < 2 ? 1.0f : -1.0f;
        V ab = xp[b] - xp[a];
        float d = len(ab), sum = len(xp[k] - xp[a]) + len(xp[b] - xp[k]);
        if (soft > 0 && d > 1e-4f && sum > 1e-4f) {
            V u = ab * (1 / d);
            float e = d / sum;
            // Old distance along the bone: the speed this step is (xp - x); take the radial part off.
            float f = ramp((e - 0.9f) / 0.1f) + ramp((0.5f - e) / 0.15f);
            if (f > 0) {
                V rel = (xp[b] - s.x[b]) - (xp[a] - s.x[a]);
                float radial = dot(rel, u);
                bool towardLimit = (e > 0.7f) ? radial > 0 : radial < 0;
                if (towardLimit) xp[b] = xp[b] - u * (radial * f * soft * 0.5f);
            }
        }
        if (relaxed > 0 && d > 1e-4f) {
            V ab2 = xp[b] - xp[a];
            float tk = dot(xp[k] - xp[a], ab2) / dot(ab2, ab2);
            V off = xp[k] - (xp[a] + ab2 * tk);
            float cur = dot(off, fwd) * sign;
            float want = (l < 2 ? 0.07f : 0.05f) * s.size;
            // A gentle spring (about 3 m/s^2 per 10 cm), never against the hinge side.
            xp[k] = xp[k] + fwd * (sign * (want - cur) * relaxed * 30.0f * h * h);
        }
    }
    // Spine: slow the pieces as the bend cones near their edge.
    if (soft > 0) {
        const int pairs[2][2] = {{kPelvisUp, kNeck}, {kChestUp, kPelvis}};
        for (int c = 0; c < 2; ++c) {
            for (int ci = 0; ci < s.consCount; ++ci) {
                const Con& con = s.cons[ci];
                if (!(con.a == pairs[c][0] && con.b == pairs[c][1])) continue;
                V d = xp[con.b] - xp[con.a];
                float l = len(d);
                if (l < 1e-5f) break;
                V u = d * (1 / l);
                float f = c == 0 ? ramp((l / con.hi - 0.9f) / 0.1f) : ramp((1.1f - l / con.lo) / 0.1f);
                if (f <= 0) break;
                V rel = (xp[con.b] - s.x[con.b]) - (xp[con.a] - s.x[con.a]);
                float radial = dot(rel, u);
                if (c == 0 ? radial > 0 : radial < 0) {
                    V cut = u * (radial * f * soft * 0.5f);
                    xp[con.b] = xp[con.b] - cut;
                    xp[con.a] = xp[con.a] + cut;
                }
                break;
            }
        }
    }
}

// Stagger: the hips' offset d is a damped spring thrown by the blow; the feet stay planted until
// they lag it by more than stagger_step, then one steps to where the body went (a lifted arc),
// and when the body comes back they step back. Fills `tgt` (the pose to hold, kPoints) from `anim`.
void staggerTargets(Slot& s, const V* anim, V* tgt, float h) {
    const float omega = g_stOmega.load(std::memory_order_relaxed), lean = g_stLean.load(std::memory_order_relaxed);
    float thr = g_stStep.load(std::memory_order_relaxed) * s.size;
    if (s.t > s.params.recoverSeconds * 0.7f) thr = 0.01f * s.size;  // the last steps put the feet back exactly
    const float stepTime = g_stStepTime.load(std::memory_order_relaxed) > 0.05f ? g_stStepTime.load(std::memory_order_relaxed) : 0.05f;
    // The spring (about half damped).
    s.dv = s.dv - (s.d * (omega * omega) + s.dv * (omega)) * h;
    s.d = s.d + s.dv * h;
    // Where the planted feet should be: under the body while it is away, the pose when it is back.
    V want = len(s.d) > 0.04f * s.size ? s.d * 0.95f : V{0, 0, 0};
    if (s.stepU[0] < 0 && s.stepU[1] < 0) {
        int pick = -1;
        float worst = thr;
        for (int f = 0; f < 2; ++f) {
            float miss = len(want - s.fo[f]);
            if (miss > worst) worst = miss, pick = f;
        }
        if (pick >= 0) s.foFrom[pick] = s.fo[pick], s.foTo[pick] = want, s.stepU[pick] = 0;
    }
    float lift[2] = {0, 0};
    for (int f = 0; f < 2; ++f) {
        if (s.stepU[f] < 0) continue;
        s.stepU[f] += h / stepTime;
        float u = s.stepU[f] >= 1 ? 1 : s.stepU[f];
        s.fo[f] = s.foFrom[f] + (s.foTo[f] - s.foFrom[f]) * ramp(u);
        lift[f] = 0.07f * s.size * std::sin(3.14159265f * u);
        if (s.stepU[f] >= 1) s.stepU[f] = -1;
    }
    const V up{0, 1, 0};
    V arms = s.d * (0.6f + lean * 1.2f) + up * (0.25f * len(s.d));
    V hands = s.d * (0.6f + lean * 1.6f) + up * (0.3f * len(s.d));
    for (int i = 0; i < kPoints; ++i) tgt[i] = anim[i];
    tgt[kPelvis] = anim[kPelvis] + s.d * 0.6f;
    tgt[kHipL] = anim[kHipL] + s.d * 0.6f;
    tgt[kHipR] = anim[kHipR] + s.d * 0.6f;
    tgt[kNeck] = anim[kNeck] + s.d * (0.6f + lean);
    tgt[kShoulderL] = anim[kShoulderL] + s.d * (0.6f + lean);
    tgt[kShoulderR] = anim[kShoulderR] + s.d * (0.6f + lean);
    tgt[kHead] = anim[kHead] + s.d * (0.6f + lean * 1.3f);
    tgt[kElbowL] = anim[kElbowL] + arms;
    tgt[kElbowR] = anim[kElbowR] + arms;
    tgt[kHandL] = anim[kHandL] + hands;
    tgt[kHandR] = anim[kHandR] + hands;
    tgt[kFootL] = anim[kFootL] + s.fo[0] + up * lift[0];
    tgt[kFootR] = anim[kFootR] + s.fo[1] + up * lift[1];
    tgt[kKneeL] = anim[kKneeL] + (s.d * 0.6f + s.fo[0]) * 0.5f + up * (lift[0] * 0.8f);
    tgt[kKneeR] = anim[kKneeR] + (s.d * 0.6f + s.fo[1]) * 0.5f + up * (lift[1] * 0.8f);
}

void substep(Slot& s, const V* anim, float animWeight, float floorY, float h) {
    const bool reaction = s.params.recoverSeconds > 0;
    const float rise = reaction ? (s.t / s.params.recoverSeconds > 1 ? 1 : s.t / s.params.recoverSeconds) : 0;
    const float tether = g_tether.load(std::memory_order_relaxed);
    const WorldQuery query = g_world.load(std::memory_order_relaxed) ? g_query.load(std::memory_order_relaxed) : nullptr;
    const float g = 9.81f * g_gravity.load(std::memory_order_relaxed);
    const float damp = std::exp(-g_damping.load(std::memory_order_relaxed) * h);
    V xp[kSimPoints];
    for (int i = 0; i < kSimPoints; ++i) {
        s.v[i].y -= g * h;
        s.v[i] = s.v[i] * damp;
        xp[i] = s.x[i] + s.v[i] * h;
        if (reaction && i < kPoints) {  // a spring toward the animation: the speed it gives is kept
            float k = s.stiff[i] + (1 - s.stiff[i]) * rise;
            xp[i] = xp[i] + (anim[i] - xp[i]) * (1 - std::exp(-kPullRate * k * h));
        } else if (animWeight > 0 && i < kPoints) {  // held toward the animation without giving it speed
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
    basis(xp[kHipL] - xp[kHipR], xp[kPelvisUp] - xp[kPelvis], side, up, fwd);
    V legF = side * s.legFwd.x + up * s.legFwd.y + fwd * s.legFwd.z;
    basis(xp[kShoulderL] - xp[kShoulderR], xp[kChestUp] - xp[kNeck], side, up, fwd);
    V armF = side * s.armFwd.x + up * s.armFwd.y + fwd * s.armFwd.z;
    softAndRelax(s, xp, legF, armF, h);  // these change the speed (before the hinge limits, which do not)
    V before[kSimPoints];
    for (int i = 0; i < kSimPoints; ++i) before[i] = xp[i];
    hinge(xp, kHipL, kKneeL, kFootL, legF, 1);
    hinge(xp, kHipR, kKneeR, kFootR, legF, 1);
    hinge(xp, kShoulderL, kElbowL, kHandL, armF, -1);
    hinge(xp, kShoulderR, kElbowR, kHandR, armF, -1);
    for (int i = 0; i < kPoints; ++i) {
        float floor = floorY + kRadius[i] * s.size;
        if (xp[i].y < floor) xp[i].y = floor;
    }
    // The world: points inside a wall or a piece of furniture are pushed out
    // along its surface (a few small steps, the way out unknown), and lose
    // the speed they had into it. The body also stays near where the Sim is.
    for (int i = 0; i < kPoints; ++i) {
        if (query) {
            for (int k = 0; k < 3; ++k) {
                float n[3];
                if (!query(xp[i].x, xp[i].y, xp[i].z, kRadius[i] * s.size, n)) break;
                V normal{n[0], n[1], n[2]};
                xp[i] = xp[i] + normal * (0.25f * kRadius[i] * s.size + 0.005f);
                float into = dot(s.v[i], normal);
                if (into < 0) s.v[i] = s.v[i] - normal * into;
            }
        }
        if (tether > 0) {
            V d{xp[i].x - s.root[0], 0, xp[i].z - s.root[2]};
            float l = len(d);
            if (l > tether) {
                V out = d * (1 / l);
                xp[i] = xp[i] - out * (l - tether);
                float into = dot(s.v[i], out);
                if (into > 0) s.v[i] = s.v[i] - out * into;
            }
        }
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

bool reactionOn() {
    readSettings();
    return g_flinch.load(std::memory_order_relaxed);
}
float reactionStrength() {
    readSettings();
    return g_fStrength.load(std::memory_order_relaxed);
}
void reactionDefaults(Params& p) {
    readSettings();
    p.recoverSeconds = g_fRecover.load(std::memory_order_relaxed);
    if (!(p.recoverSeconds > 0.05f)) p.recoverSeconds = 0.05f;
    p.localStiffness = g_fLocal.load(std::memory_order_relaxed);
    p.stiffness = g_fBody.load(std::memory_order_relaxed);
    p.spread = g_fSpread.load(std::memory_order_relaxed);
    p.limpSeconds = 0;
}

void setWorldQuery(WorldQuery query) { g_query.store(query, std::memory_order_relaxed); }

bool start(const void* actor, const Params& params) {
    if (!actor || !enabled()) return false;
    Lock lock;
    ULONGLONG now = GetTickCount64();
    if (Slot* have = find(actor)) {
        // A reaction gives way to a fall or a newer reaction; a fall is never replaced.
        if (now - have->lastStep < kStaleMs && have->params.recoverSeconds <= 0) return false;
        have->actor.store(nullptr, std::memory_order_release);  // left over, or a reaction being replaced
    }
    for (Slot& s : g_slots) {
        const void* a = s.actor.load(std::memory_order_relaxed);
        if (a && now - s.lastStep < kStaleMs && now - s.startedAt < kMaxMs) continue;
        s.startedAt = now;
        s.lastStep = now;
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

bool step(const void* actor, const float in[kPoints][3], const float forward[3], const float root[3], float size,
          float dt, float out[kSimPoints][3]) {
    Lock lock;
    Slot* s = find(actor);
    if (!s) return false;
    ULONGLONG now = GetTickCount64();
    if (now - s->startedAt > kMaxMs) {
        s->actor.store(nullptr, std::memory_order_release);
        return false;
    }
    s->lastStep = now;
    s->root[0] = root[0], s->root[1] = root[1], s->root[2] = root[2];
    const float floorY = root[1];
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
    const bool stagger = s->params.recoverSeconds > 0 && (s->params.staggerVelocity[0] != 0 || s->params.staggerVelocity[2] != 0);
    if (s->params.recoverSeconds > 0) {  // a reaction ends when the pose holds again
        // A stagger also waits for the feet to be back in place (at most a second more).
        bool feet = !stagger || (s->t >= s->params.recoverSeconds + 1.0f) ||
                    (s->stepU[0] < 0 && s->stepU[1] < 0 && len(s->fo[0]) < 0.01f && len(s->fo[1]) < 0.01f);
        if (s->t >= s->params.recoverSeconds && feet) {
            LOG("Ragdoll: reaction ends on %p after %.2f s", actor, s->t);
            s->actor.store(nullptr, std::memory_order_release);
            return false;
        }
    } else if (!s->releasing && s->params.limpSeconds > 0 && s->t >= s->params.limpSeconds) {
        s->releasing = true;
        s->releaseAt = s->t;
    }
    if (s->releasing) {
        s->blend = 1 - (s->t - s->releaseAt) / (g_blendOut.load(std::memory_order_relaxed) > 0.05f ? g_blendOut.load(std::memory_order_relaxed) : 0.05f);
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
        float animWeight = s->params.recoverSeconds > 0 ? 0 : 0.5f * (1 - s->t / kRamp);
        if (stagger) {
            V tgt[kSimPoints];
            staggerTargets(*s, anim, tgt, h);
            substep(*s, tgt, 0, floorY, h);
        } else {
            substep(*s, anim, animWeight > 0 ? animWeight : 0, floorY, h);
        }
        s->t += h;
    }
    float b = s->releasing ? s->blend : 1;
    b = b * b * (3 - 2 * b);  // smoothstep
    for (int i = 0; i < kSimPoints; ++i) store(anim[i] + (s->x[i] - anim[i]) * b, out[i]);
    return true;
}

}  // namespace s4vr::ragdoll
