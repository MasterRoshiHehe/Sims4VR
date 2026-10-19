#include "armik.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "handshape.h"
#include "log.h"
#include "othermods.h"
#include "player.h"

namespace s4vr::armik {

namespace {

using camera::Quat;
using camera::Vec3;

// ---------------------------------------------------------------------------
// Game functions (rva), checked against their first bytes. Game 1.128.90.1030
// (in 1.112.519.1020: 0x982530, 0x9b55f0, 0x988b80, 0x988cd0, 0x988d00; the
// code of all five is unchanged, only moved).


// One joint: position, rotation, scale (16-byte aligned, as the game keeps them).
struct alignas(16) Xf {
    float p[4];
    float q[4];
    float s[4];
};

using PoseToJointsFn = void (*)(void* actor, void* pose);
using ModelTransformFn = Xf* (*)(void* pose, Xf* out, uint32_t joint, void* extra);
using JointIndexFn = uint32_t (*)(void* skeleton, uint32_t hash);
using JointCountFn = uint32_t (*)(void* skeleton);
using ParentFn = int (*)(void* skeleton, uint32_t joint);

// Checked again in 1.121.361.1020 (RigPose ctor 0x91d4c0, ModelTransform, rig
// parse at 0x94ee36, Actor vt+0x48 0x8ea5f0): unchanged. The RigPose grew a
// 200-byte per-joint array at +0x3d0a0, past everything read here.
constexpr size_t kPoseRig = 0x30, kPoseLocals = 0x38, kPoseJointCount = 0x5e;
constexpr size_t kRigSkeleton = 0x48, kRigBindLocals = 0xf8;
constexpr size_t kActorPosition = 0x2e0, kActorRotation = 0x2f0, kActorScale = 0x300;
constexpr size_t kActorSecondPosition = 0x310;  // a second transform the actor keeps (set with the first at birth)
constexpr uint32_t kMaxJoints = 200;  // the pose's inline joint array

PoseToJointsFn g_original = nullptr;
ModelTransformFn g_model = nullptr;
JointIndexFn g_jointIndex = nullptr;
JointCountFn g_jointCount = nullptr;
ParentFn g_parent = nullptr;

// ---------------------------------------------------------------------------
// Small vector maths (game and OpenXR: right-handed, Y up)

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-8f ? mul(a, 1 / l) : Vec3{0, 0, 0};
}
bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

Quat qmul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat qnormalize(Quat q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-8f)) return {0, 0, 0, 1};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
Vec3 rotate(const Quat& q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = mul(cross(u, v), 2);
    return add(add(v, mul(t, q.w)), cross(u, t));
}
Quat axisAngle(Vec3 axis, float angle) {
    float s = std::sin(angle / 2);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2)};
}
// Shortest rotation taking direction a to direction b.
Quat fromTo(Vec3 a, Vec3 b) {
    a = normalize(a);
    b = normalize(b);
    float d = dot(a, b);
    if (d < -0.9999f) {
        Vec3 axis = cross(a, {1, 0, 0});
        if (length(axis) < 1e-3f) axis = cross(a, {0, 1, 0});
        return axisAngle(normalize(axis), 3.14159265f);
    }
    Vec3 c = cross(a, b);
    return qnormalize({c.x, c.y, c.z, 1 + d});
}
Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return qnormalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
// The rotation whose columns are x, y, z (an orthonormal right-handed basis).
Quat fromBasis(Vec3 x, Vec3 y, Vec3 z) {
    float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
    }
    return qnormalize(q);
}
float quatDistance(const Quat& a, const Quat& b) {
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 1 - (d > 1 ? 1 : d);
}
Quat quatOf(const float* f) { return {f[0], f[1], f[2], f[3]}; }
Vec3 vecOf(const float* f) { return {f[0], f[1], f[2]}; }
void store(float* f, const Quat& q) {
    f[0] = q.x;
    f[1] = q.y;
    f[2] = q.z;
    f[3] = q.w;
}

float ease(float rate, float dt) { return 1 - std::exp(-rate * dt); }

// ---------------------------------------------------------------------------
// Data from other threads, read without blocking (the hook runs on the game's
// job threads inside the actor's lock, so it must never wait on our locks).

template <typename T>
class SeqLock {
public:
    void write(const T& value) {
        while (writing_.test_and_set(std::memory_order_acquire)) {
        }
        uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
        writing_.clear(std::memory_order_release);
    }
    bool read(T& out) const {
        for (int tries = 0; tries < 16; ++tries) {
            uint32_t before = seq_.load(std::memory_order_acquire);
            if (before & 1) continue;
            std::memcpy(&out, &data_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before) return before != 0;
        }
        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
    T data_{};
};

struct Frame {
    void* actor;
    bool free;  // you move the Sim (not the game)
    float anchor[3];
    float eye[3];  // the middle between your eyes in the world
    Quat heading;
    Quat hmd;  // headset orientation in app space
    float scale;
    ULONGLONG tick;
};
struct Hands {
    Hand hand[2];
    ULONGLONG closedAt[2];  // when each hand last closed (grip pressed from open)
    ULONGLONG squeezedAt[2];  // when grip and trigger were last squeezed together (from not)
    bool squeezing[2];        // grip and trigger squeezed together now (with some give to let go)
    ULONGLONG tick;
};
SeqLock<Frame> g_frame;
SeqLock<Hands> g_hands;
std::atomic<void*> g_player{nullptr};
std::atomic<bool> g_holding[2] = {false, false};
std::atomic<float> g_gameCurl[2] = {0.0f, 0.0f};  // how far the game's animation curls the three outer fingers (radians)
std::atomic<float> g_contact[2] = {0.0f, 0.0f};    // metres each hand pressed into another Sim, latest solve
std::atomic<ULONGLONG> g_contactTick[2] = {0, 0};
// Where on another Sim's body each hand holds on (world), as of that Sim's
// latest pose: your Sim's hand is drawn stuck there while you hold (user
// 2026-10-17: "magnetically stuck to the body part").
struct GrabPlace {
    bool on = false;
    Vec3 at{0, 0, 0};
    ULONGLONG tick = 0;
};
SeqLock<GrabPlace> g_grabPlace[2];
// Debug menu ([physical] grab_curl, grab_drag): how far the fingers close on a
// held body (0 open .. 1 a fist) and how far you can drag the held place.
std::atomic<float> g_grabCurl{0.45f}, g_grabDrag{0.15f};
float grabCurlFor(int side);  // grab_curl, or Claude Jiggle's own for a held breast or butt
// Milliseconds since `t`, never negative: another thread may have stored a
// tick later than the `now` read here.
long long ageMs(ULONGLONG now, ULONGLONG t) {
    long long age = static_cast<long long>(now - t);
    return age < 0 ? 0 : age;
}
std::atomic<bool> g_fkVerified{false};              // the player's rig composed as expected: other Sims' will too
bool g_touchBodies = true, g_holdHands = true, g_lookAt = true, g_grabBodies = true;
std::atomic<bool> g_busy{false};
bool g_enabled = false;
float g_handPitch = 0;      // radians the hand points below the grip pose's -Z
bool g_headTracking = true;

// ---------------------------------------------------------------------------
// Per-rig joints, found once per rig (outfit, age and so on change the rig)

uint32_t jointHash(const char* name) {
    uint32_t h = 0x811c9dc5;
    for (; *name; ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h = (h * 0x1000193) ^ static_cast<uint8_t>(c);
    }
    return h;
}

enum FingerKind { kThumb = 0, kIndex = 1, kOther = 2 };
const char* const kFingerNames[5] = {"Thumb", "Index", "Mid", "Ring", "Pinky"};
// Finger curl, radians per joint from the knuckle out: a fist, and the index
// finger of the OK sign (trigger without grip) whose tip meets the thumb's.
const float kFist[3] = {1.2f, 1.65f, 1.05f};
const float kOk[3] = {0.8f, 1.0f, 0.7f};
// The thumb bent all the way toward the palm (setHoldPose bend 1), radians per joint.
const float kThumbBend[3] = {0.5f, 0.7f, 0.9f};

struct Finger {
    int joints[3];
    int count = 0;
    Quat bind[3];  // bind-pose local rotations
    Vec3 axis[3];  // curl axis in each joint's own frame
    Vec3 spread;   // the palm's normal in the first joint's frame, turning it away from the hand
    Quat rel[3];   // each joint's bind-pose rotation in the hand's frame
    int kind = kOther;
    int name = 0;  // kFingerNames
};

struct Arm {
    bool ok = false;
    int clavicle = -1, upper = -1, fore = -1, hand = -1;
    int prop = -1;  // the hand's prop joint (b__L_Prop__ / b__R_Prop__): animations hang props on it
    bool frame = false;   // hand axes known: forward, toward the thumb, and their cross
    Vec3 forward, side, up;
    // The straight index finger in the hand's frame: its knuckle and the way it points.
    bool pointer = false;
    Vec3 indexBase, indexDir;
    Finger fingers[5];
    int fingerCount = 0;
    int thumb = -1, index = -1;  // in fingers[]
    float outward = 1;    // +1 if the shoulder sits on the model's +X side
    int which = 0;        // 0 the left arm, 1 the right
};

struct RigCache {
    void* rig = nullptr;
    bool fkChecked = false;
    bool fkOk = false;
    Arm arm[2];
    int head = -1, neck = -1;
    bool headOk = false;
    Vec3 headForward, headUp;  // the face's forward and up in the head joint's frame
    int eye[2] = {-1, -1};
    bool eyesOk = false;
    Vec3 eyeForward[2];        // each eye's gaze in its joint's frame
    int spine[4] = {-1, -1, -1, -1};  // the joints that lean the head (seated), from the hips up: spine 0-2, neck
    int spineCount = 0;
};
RigCache g_cache;

bool isAncestor(uint8_t* skeleton, int ancestor, int joint) {
    for (int k = 0, j = joint; k < 64 && j >= 0; ++k) {
        j = g_parent(skeleton, static_cast<uint32_t>(j));
        if (j == ancestor) return true;
    }
    return false;
}

// Position and rotation of `joint` in `root`'s frame, from the bind pose.
bool bindInFrame(uint8_t* skeleton, const Xf* bind, int root, int joint, Vec3& position, Quat& rotation) {
    Vec3 p{0, 0, 0};
    Quat q{0, 0, 0, 1};
    int j = joint;
    for (int k = 0; k < 32 && j != root; ++k) {
        if (j < 0) return false;
        const Xf& t = bind[j];
        // parent (x) child: p = t.p + rot(t.q, t.s * p); q = t.q * q
        p = add(vecOf(t.p), rotate(quatOf(t.q), {t.s[0] * p.x, t.s[1] * p.y, t.s[2] * p.z}));
        q = qmul(quatOf(t.q), q);
        j = g_parent(skeleton, static_cast<uint32_t>(j));
    }
    if (j != root) return false;
    position = p;
    rotation = q;
    return true;
}

void resolveArm(uint8_t* rig, int sideIndex, uint32_t count, Arm& arm) {
    uint8_t* skeleton = rig + kRigSkeleton;
    const char* s = sideIndex ? "R" : "L";
    char name[48];
    auto find = [&](const char* fmt, const char* part) {
        std::snprintf(name, sizeof(name), fmt, s, part);
        uint32_t i = g_jointIndex(skeleton, jointHash(name));
        return i < count ? static_cast<int>(i) : -1;
    };
    arm = Arm{};
    arm.which = sideIndex;
    arm.upper = find("b__%s_%s__", "UpperArm");
    arm.fore = find("b__%s_%s__", "Forearm");
    arm.hand = find("b__%s_%s__", "Hand");
    arm.prop = find("b__%s_%s__", "Prop");
    if (arm.upper < 0 || arm.fore < 0 || arm.hand < 0) {
        LOG("Arm IK: %s arm joints missing (upper %d, forearm %d, hand %d)", s, arm.upper, arm.fore, arm.hand);
        return;
    }
    arm.clavicle = g_parent(skeleton, static_cast<uint32_t>(arm.upper));
    if (arm.clavicle < 0 || !isAncestor(skeleton, arm.upper, arm.fore) || !isAncestor(skeleton, arm.fore, arm.hand)) {
        LOG("Arm IK: %s arm chain unexpected (clavicle %d, parents: forearm %d, hand %d)", s, arm.clavicle,
            g_parent(skeleton, arm.fore), g_parent(skeleton, arm.hand));
        return;
    }
    arm.ok = true;

    // Fingers by name: up to three joints each, from the knuckle out.
    const Xf* bind = *reinterpret_cast<Xf* const*>(rig + kRigBindLocals);
    Vec3 tip[5] = {}, base[5] = {};
    bool have[5] = {};
    for (int f = 0; f < 5 && bind; ++f) {
        Finger finger;
        finger.kind = f == 0 ? kThumb : f == 1 ? kIndex : kOther;
        finger.name = f;
        for (int k = 0; k < 3; ++k) {
            char part[16];
            std::snprintf(part, sizeof(part), "%s%d", kFingerNames[f], k);
            int j = find("b__%s_%s__", part);
            if (j < 0 || !isAncestor(skeleton, arm.hand, j)) break;
            finger.joints[finger.count++] = j;
        }
        if (!finger.count) continue;
        Quat q;
        if (!bindInFrame(skeleton, bind, arm.hand, finger.joints[0], base[f], q) ||
            !bindInFrame(skeleton, bind, arm.hand, finger.joints[finger.count - 1], tip[f], q))
            continue;
        have[f] = true;
        if (f == 0) arm.thumb = arm.fingerCount;
        if (f == 1) arm.index = arm.fingerCount;
        arm.fingers[arm.fingerCount++] = finger;
    }

    // The hand's own axes, in its local frame, from where the fingers sit.
    Vec3 forward{0, 0, 0}, thumbSide{0, 0, 0};
    int n = 0;
    for (int f = 1; f < 5; ++f) {
        if (have[f]) {
            forward = add(forward, tip[f]);
            ++n;
        }
    }
    if (n >= 2) {
        int a = have[1] ? 1 : have[2] ? 2 : 3, b = have[4] ? 4 : have[3] ? 3 : 2;
        if (a != b) thumbSide = sub(base[a], base[b]);  // toward the index finger (the thumb side)
    }
    forward = normalize(forward);
    thumbSide = sub(thumbSide, mul(forward, dot(thumbSide, forward)));
    if (n >= 2 && length(thumbSide) > 1e-4f && length(forward) > 0.5f) {
        arm.forward = forward;
        arm.side = normalize(thumbSide);
        arm.up = cross(arm.forward, arm.side);
        arm.frame = true;
        if (have[1] && length(sub(tip[1], base[1])) > 1e-4f) {
            arm.indexBase = base[1];
            arm.indexDir = normalize(sub(tip[1], base[1]));
            arm.pointer = true;
        }
    }

    // Curl axes: each finger turns toward the palm, about forward x palm.
    // The palm faces +up on the left hand and -up on the right one (see
    // setHands: the controller grip pose's +X is the left palm, -X the right).
    Vec3 palm = mul(arm.up, sideIndex ? -1.0f : 1.0f);
    for (int i = 0; i < arm.fingerCount && arm.frame; ++i) {
        Finger& finger = arm.fingers[i];
        Vec3 p0, p1;
        Quat q;
        bindInFrame(skeleton, bind, arm.hand, finger.joints[0], p0, q);
        bindInFrame(skeleton, bind, arm.hand, finger.joints[finger.count - 1], p1, q);
        Vec3 along = finger.count > 1 ? normalize(sub(p1, p0)) : arm.forward;
        if (length(along) < 0.5f) along = arm.forward;
        Vec3 axis = normalize(cross(along, palm));
        if (length(axis) < 0.5f) axis = normalize(cross(arm.forward, palm));
        // Spread: about the palm's normal, signed so a positive turn swings
        // the finger toward the thumb side (for the thumb: away from the hand).
        Vec3 spread = dot(cross(palm, along), arm.side) >= 0 ? palm : mul(palm, -1.0f);
        for (int k = 0; k < finger.count; ++k) {
            Vec3 pk;
            Quat rel;
            finger.bind[k] = quatOf(bind[finger.joints[k]].q);
            if (!bindInFrame(skeleton, bind, arm.hand, finger.joints[k], pk, rel)) rel = {0, 0, 0, 1};
            finger.axis[k] = rotate(conj(rel), axis);
            finger.rel[k] = rel;
            if (k == 0) finger.spread = rotate(conj(rel), spread);
        }
    }
    LOG("Arm IK: %s arm joints clavicle %d upper %d forearm %d hand %d; %d fingers%s; hand axes %s (forward %.2f %.2f "
        "%.2f, thumb side %.2f %.2f %.2f)",
        s, arm.clavicle, arm.upper, arm.fore, arm.hand, arm.fingerCount, bind ? "" : " (no bind pose)",
        arm.frame ? "found" : "NOT found", arm.forward.x, arm.forward.y, arm.forward.z, arm.side.x, arm.side.y,
        arm.side.z);
    LOG("Arm IK: %s prop joint %d (its parent %d, the hand %d)", s, arm.prop,
        arm.prop >= 0 ? g_parent(skeleton, static_cast<uint32_t>(arm.prop)) : -1, arm.hand);
    if (arm.fingerCount < 5 && bind) {
        // Unknown finger names: list the hand's children so they can be matched.
        for (uint32_t j = 0; j < count; ++j) {
            if (g_parent(skeleton, j) != arm.hand) continue;
            uint32_t hash = 0;
            std::memcpy(&hash, *reinterpret_cast<uint8_t* const*>(skeleton + 8) + j * 0x70, sizeof(hash));
            LOG("Arm IK: %s hand child %u (first word %08x) at %.3f %.3f %.3f", s, j, hash, bind[j].p[0], bind[j].p[1],
                bind[j].p[2]);
        }
    }
}

// The head joint, and which way the face looks in its frame: in the bind pose
// the Sim faces the model's +Z with +Y up.
void resolveHead(uint8_t* rig, uint32_t count, RigCache& cache) {
    uint8_t* skeleton = rig + kRigSkeleton;
    const Xf* bind = *reinterpret_cast<Xf* const*>(rig + kRigBindLocals);
    uint32_t head = g_jointIndex(skeleton, jointHash("b__Head__")), neck = g_jointIndex(skeleton, jointHash("b__Neck__"));
    cache.head = head < count ? static_cast<int>(head) : -1;
    cache.neck = neck < count && cache.head >= 0 && isAncestor(skeleton, static_cast<int>(neck), cache.head)
                     ? static_cast<int>(neck)
                     : -1;
    Vec3 p;
    Quat q;
    cache.headOk = cache.head >= 0 && bind && g_parent(skeleton, cache.head) >= 0 &&
                   bindInFrame(skeleton, bind, -1, cache.head, p, q);
    if (cache.headOk) {
        cache.headForward = rotate(conj(q), {0, 0, 1});
        cache.headUp = rotate(conj(q), {0, 1, 0});
    }
    LOG("Head tracking: head joint %d, neck %d%s (face forward %.2f %.2f %.2f, up %.2f %.2f %.2f in the head's frame)",
        cache.head, cache.neck, cache.headOk ? "" : ", NOT usable", cache.headForward.x, cache.headForward.y,
        cache.headForward.z, cache.headUp.x, cache.headUp.y, cache.headUp.z);
    // The eyes, for looking where you point: each must sit under the head.
    cache.eyesOk = cache.headOk;
    const char* const eyeNames[2] = {"b__L_Eye__", "b__R_Eye__"};
    for (int i = 0; i < 2; ++i) {
        uint32_t e = g_jointIndex(skeleton, jointHash(eyeNames[i]));
        cache.eye[i] = e < count && cache.head >= 0 && isAncestor(skeleton, cache.head, static_cast<int>(e))
                           ? static_cast<int>(e)
                           : -1;
        Vec3 ep;
        Quat eq;
        if (cache.eye[i] < 0 || !bindInFrame(skeleton, bind, -1, cache.eye[i], ep, eq)) {
            cache.eyesOk = false;
            continue;
        }
        cache.eyeForward[i] = rotate(conj(eq), {0, 0, 1});  // the bind pose looks along the model's +Z
    }
    LOG("Eyes: joints %d %d%s", cache.eye[0], cache.eye[1], cache.eyesOk ? "" : ", NOT usable");
    cache.spineCount = 0;
    if (cache.head >= 0) {
        for (const char* name : {"b__Spine0__", "b__Spine1__", "b__Spine2__", "b__Neck__"}) {
            uint32_t j = g_jointIndex(skeleton, jointHash(name));
            if (j < count && isAncestor(skeleton, static_cast<int>(j), cache.head))
                cache.spine[cache.spineCount++] = static_cast<int>(j);
        }
    }
    LOG("Lean: %d spine joints carry the head (%d %d %d %d)", cache.spineCount, cache.spine[0], cache.spine[1],
        cache.spine[2], cache.spine[3]);
}

// ---------------------------------------------------------------------------
// The solve, for the player's Sim only

struct Side {
    float weight = 0;
    float index = 0, grip = 0, thumb = 0;
    float hold = 0;  // 1 while the hand holds something: the game's grip, not the buttons
    // A fixed holding pose (setHoldPose, your phone): the four fingers
    // curled by holdCurl, the thumb bent by thumbBend and swung away from
    // the hand by thumbSpread (radians); the buttons don't move that hand.
    bool holdPose = false;
    float holdCurl = 0, thumbBend = 0, thumbSpread = 0;
    float point = 0;  // 1 while the hand points the laser (setPointing)
    float tracked = 0;      // 1 while hand tracking moves it (your real hand): the fingers take curl
    float curl[5][3] = {};  // the real hand's finger bends, smoothed (Hand::curl)
};
struct HoldPose {
    bool on;
    float curl, bend, spread;
    Vec3 move;
    Quat turn;
};
SeqLock<HoldPose> g_holdPose[2];
// The laser of a hand that points it (setPointing), in OpenXR app space.
struct Point {
    bool on;
    Vec3 from, dir;
    ULONGLONG tick;
};
SeqLock<Point> g_point[2];
// The last laser, in that hand's grip frame (tilted by hand_pitch): the hand eases back from it.
Vec3 g_pointFrom[2] = {{0, 0, 0}, {0, 0, 0}}, g_pointDir[2] = {{0, 0, -1}, {0, 0, -1}};
int g_pointLogs = 0;
// The player's index fingertip on the latest solve, in its controller's grip frame (fingertip).
struct Tip {
    bool valid;
    Vec3 grip;
    ULONGLONG tick;
};
SeqLock<Tip> g_tip[2];
// The player's hand axes in each hand joint's own frame (handAxes).
std::mutex g_axesMutex;
// Where the player's hands ended up on the latest solve (handPose): the wrist
// joint in the world, the grip frame its hand was turned to, metres to world units.
struct HandPose {
    bool valid = false;
    Vec3 wrist{0, 0, 0};
    Quat grip{0, 0, 0, 1};
    float scale = 1;
    ULONGLONG tick = 0;
};
std::mutex g_handPoseMutex;
HandPose g_handPose[2];
float g_frameScale = 1;  // the frame scale of the solve in progress (job thread)
bool g_axesKnown[2] = {false, false};
Vec3 g_axes[2][3];
Side g_side[2];
float g_headWeight = 0;
// Seated lean (setLean): where your head moved from where you sat down, in the
// world; eased here, and what the solve really moved the head joint by.
struct Lean {
    float world[3];
    ULONGLONG tick;
};
SeqLock<Lean> g_leanWanted, g_leanDone;
Vec3 g_leanNow{0, 0, 0};
// Seated: the spine and neck keep the animation's posture, slowly averaged,
// not its moves (typing at a computer sways the upper body): your head moves
// it instead (solveLean). setSeatTorso.
std::atomic<bool> g_seatTorso{true};
Quat g_torsoRest[4];
bool g_torsoRestOk = false;
float g_torsoWeight = 0;
uint64_t g_leanLogs = 0;
// Where the head ended up this solve (model space), for the eyes under it.
bool g_headMoved = false;
Quat g_headBefore{0, 0, 0, 1}, g_headAfter{0, 0, 0, 1};
// Where your eyes look (grip + A / X laser), OpenXR app space.
struct LookTarget {
    float point[3];
    bool on;
    ULONGLONG tick;
};
SeqLock<LookTarget> g_lookTarget;
float g_eyeWeight = 0;
LARGE_INTEGER g_freq{}, g_last{};
uint64_t g_solves = 0;

Xf modelOf(void* pose, int joint) {
    Xf t;
    g_model(pose, &t, static_cast<uint32_t>(joint), nullptr);
    return t;
}

void writeLocal(Xf* locals, int joint, const Quat& target, float weight) {
    Quat now = quatOf(locals[joint].q);
    Quat q = qnormalize(weight >= 0.999f ? target : slerp(now, target, weight));
    if (std::isfinite(q.x) && std::isfinite(q.w)) store(locals[joint].q, q);
}

// ---------------------------------------------------------------------------
// The game's own grip on a prop (the TV remote, 2026-10-14). While the game's
// remote animation runs on your Sim (learnGrip), the pose it makes, before
// our arms and fingers, has the remote's joint (the hand's prop joint) in the
// hand and the fingers closed around it. Once the prop joint has sat still in
// the hand for kLearnSolves solves in a row, where it sits in the hand joint's
// frame and each finger joint's turn from its bind pose are kept. The other
// hand gets the same grip mirrored. Python then hangs our remote on the hand
// joint at that place (grip) and the fingers take that pose while it is held
// (holdGrip). Kept across sessions in sims4vr_grips.ini.

constexpr int kLearnSolves = 12;
constexpr float kLearnNear = 0.25f;  // the prop joint this close to the hand (rig units): the animation holds the prop
constexpr ULONGLONG kLearnFor = 30000;  // ms a learn request lasts

struct GripData {
    bool valid = false;
    int side = 1;        // the hand it was learned on
    Vec3 p{0, 0, 0};     // the prop joint in that hand joint's frame
    Quat q{0, 0, 0, 1};
    bool finger[5][3] = {};
    Quat turn[5][3] = {};  // each finger joint's local rotation is its bind rotation times this
};
struct GripPlace {
    bool valid;
    Vec3 p;
    Quat q;
};
SeqLock<GripData> g_gripLearned;
std::atomic<uint32_t> g_gripVersion{0};
std::atomic<bool> g_gripUnsaved{false};
std::atomic<ULONGLONG> g_gripLearnUntil{0};
std::atomic<bool> g_gripHold[2] = {false, false};
SeqLock<GripPlace> g_gripPlace[2];  // for Python: where the prop goes on each hand joint
// Job thread (the player's solve): the learned grip on each hand of the current rig.
GripData g_sideGrip[2];
uint32_t g_sideGripVersion = ~0u;
void* g_sideGripRig = nullptr;
// Learning, per hand: the last sample and how many solves in a row it held still.
Vec3 g_learnP[2];
Quat g_learnQ[2];
Quat g_learnFingers[2][15];
int g_learnStill[2] = {0, 0};
int g_learnLogs = 0;

// A vector from one hand's frame to the other's, mirrored: along the fingers
// and toward the thumb stay, the palm's normal flips side (the palm faces +up
// on the left hand, -up on the right).
Vec3 mirrorHand(const Arm& from, const Arm& to, Vec3 v) {
    float f = dot(v, from.forward), t = dot(v, from.side), u = dot(v, from.up);
    return add(add(mul(to.forward, f), mul(to.side, t)), mul(to.up, -u));
}
// A rotation (axis-angle) mirrored the same way: its axis is a turning
// direction, so it flips with the mirror.
Quat mirrorTurn(const Arm& from, const Arm& to, const Quat& q) {
    Vec3 v = mul(mirrorHand(from, to, {q.x, q.y, q.z}), -1.0f);
    return qnormalize({v.x, v.y, v.z, q.w});
}

GripData gripFor(const GripData& g, int side) {
    if (!g.valid || g.side == side) return g;
    const Arm& from = g_cache.arm[g.side];
    const Arm& to = g_cache.arm[side];
    GripData out;
    if (!from.frame || !to.frame) return out;
    out.valid = true;
    out.side = side;
    out.p = mirrorHand(from, to, g.p);
    // The prop's axes mirrored; its own left-right axis (x) flips back so it
    // stays a rotation (the remote is the same seen from either side).
    Vec3 x = mul(mirrorHand(from, to, rotate(g.q, {1, 0, 0})), -1.0f);
    Vec3 y = mirrorHand(from, to, rotate(g.q, {0, 1, 0}));
    Vec3 z = mirrorHand(from, to, rotate(g.q, {0, 0, 1}));
    out.q = fromBasis(normalize(x), normalize(y), normalize(z));
    for (int i = 0; i < from.fingerCount; ++i) {
        const Finger& a = from.fingers[i];
        const Finger* b = nullptr;
        for (int j = 0; j < to.fingerCount; ++j)
            if (to.fingers[j].name == a.name) b = &to.fingers[j];
        if (!b) continue;
        for (int k = 0; k < a.count && k < b->count; ++k) {
            if (!g.finger[a.name][k]) continue;
            // The turn in the hand's frame, mirrored, back into the other joint's frame.
            Quat inHand = qmul(a.rel[k], qmul(g.turn[a.name][k], conj(a.rel[k])));
            Quat mirrored = mirrorTurn(from, to, inHand);
            out.turn[a.name][k] = qnormalize(qmul(conj(b->rel[k]), qmul(mirrored, b->rel[k])));
            out.finger[a.name][k] = true;
        }
    }
    return out;
}

// Job thread, every solve: the learned grip mapped onto this rig's hands.
void deriveGrips(void* rig) {
    uint32_t version = g_gripVersion.load(std::memory_order_acquire);
    if (version == g_sideGripVersion && rig == g_sideGripRig) return;
    GripData g;
    if (!g_gripLearned.read(g)) g = GripData{};
    g_sideGripVersion = version;
    g_sideGripRig = rig;
    for (int side = 0; side < 2; ++side) {
        g_sideGrip[side] = gripFor(g, side);
        const GripData& s = g_sideGrip[side];
        g_gripPlace[side].write(GripPlace{s.valid, s.p, s.q});
    }
}

// Job thread, before our arms and fingers: the game's grip while its remote
// animation holds the prop.
void learnGrip(void* pose, const Xf* locals) {
    ULONGLONG until = g_gripLearnUntil.load(std::memory_order_relaxed);
    if (!until) return;
    if (GetTickCount64() > until) {
        g_gripLearnUntil.store(0, std::memory_order_relaxed);
        g_learnStill[0] = g_learnStill[1] = 0;
        return;
    }
    for (int side = 0; side < 2; ++side) {
        const Arm& arm = g_cache.arm[side];
        if (!arm.ok || arm.prop < 0) continue;
        Xf hand = modelOf(pose, arm.hand), prop = modelOf(pose, arm.prop);
        Quat hq = quatOf(hand.q);
        Vec3 d = rotate(conj(hq), sub(vecOf(prop.p), vecOf(hand.p)));
        if (!(hand.s[0] > 1e-3f && hand.s[1] > 1e-3f && hand.s[2] > 1e-3f)) continue;
        Vec3 p{d.x / hand.s[0], d.y / hand.s[1], d.z / hand.s[2]};
        Quat q = qnormalize(qmul(conj(hq), quatOf(prop.q)));
        if (!finite(p) || length(p) > kLearnNear) {
            g_learnStill[side] = 0;
            continue;
        }
        // Still: the prop in the hand, and the fingers closed around it (not still closing).
        bool still = g_learnStill[side] > 0 && length(sub(p, g_learnP[side])) < 0.004f &&
                     quatDistance(q, g_learnQ[side]) < 2e-4f;
        int n = 0;
        for (int i = 0; i < arm.fingerCount; ++i) {
            const Finger& f = arm.fingers[i];
            for (int k = 0; k < f.count && n < 15; ++k, ++n) {
                Quat now = quatOf(locals[f.joints[k]].q);
                if (quatDistance(now, g_learnFingers[side][n]) >= 2e-4f) still = false;
                g_learnFingers[side][n] = now;
            }
        }
        g_learnStill[side] = still ? g_learnStill[side] + 1 : 1;
        g_learnP[side] = p;
        g_learnQ[side] = q;
        if (g_learnStill[side] < kLearnSolves) continue;
        GripData g;
        g.valid = true;
        g.side = side;
        g.p = p;
        g.q = q;
        int fingers = 0;
        for (int i = 0; i < arm.fingerCount; ++i) {
            const Finger& f = arm.fingers[i];
            for (int k = 0; k < f.count; ++k) {
                g.turn[f.name][k] = qnormalize(qmul(conj(f.bind[k]), quatOf(locals[f.joints[k]].q)));
                g.finger[f.name][k] = true;
                ++fingers;
            }
        }
        g_gripLearned.write(g);
        g_gripVersion.fetch_add(1, std::memory_order_release);
        g_gripUnsaved.store(true, std::memory_order_release);
        g_gripLearnUntil.store(0, std::memory_order_relaxed);
        g_learnStill[0] = g_learnStill[1] = 0;
        if (g_learnLogs++ < 20)
            LOG("Arm IK: learned the game's grip on the %s hand: prop joint at %.3f %.3f %.3f (q %.3f %.3f %.3f %.3f) "
                "in the hand's frame, %d finger joints",
                side ? "right" : "left", p.x, p.y, p.z, q.x, q.y, q.z, q.w, fingers);
        return;
    }
}

// The fingers of a hand holding our prop: the learned grip.
void poseGrip(Xf* locals, const Arm& arm, const GripData& g, float weight) {
    for (int i = 0; i < arm.fingerCount; ++i) {
        const Finger& f = arm.fingers[i];
        for (int k = 0; k < f.count; ++k)
            if (g.finger[f.name][k]) writeLocal(locals, f.joints[k], qmul(f.bind[k], g.turn[f.name][k]), weight);
    }
}

// A joint below the hand, in the hand's frame, from the pose's local
// transforms with some rotations replaced.
struct Override {
    int joint;
    Quat q;
};
struct Chain {
    Vec3 p;
    Quat q;
    Vec3 s;
};
bool inHand(uint8_t* skeleton, const Xf* locals, int hand, int joint, const Override* ov, int ovCount, Chain& out) {
    int chain[8], n = 0, j = joint;
    while (j != hand && j >= 0 && n < 8) {
        chain[n++] = j;
        j = g_parent(skeleton, static_cast<uint32_t>(j));
    }
    if (j != hand) return false;
    Chain c{{0, 0, 0}, {0, 0, 0, 1}, {locals[hand].s[0], locals[hand].s[1], locals[hand].s[2]}};
    for (int i = n - 1; i >= 0; --i) {
        int k = chain[i];
        Quat lq = quatOf(locals[k].q);
        for (int o = 0; o < ovCount; ++o)
            if (ov[o].joint == k) lq = ov[o].q;
        Vec3 lp = vecOf(locals[k].p);
        c.p = add(c.p, rotate(c.q, {c.s.x * lp.x, c.s.y * lp.y, c.s.z * lp.z}));
        c.q = qmul(c.q, lq);
        c.s = {c.s.x * locals[k].s[0], c.s.y * locals[k].s[1], c.s.z * locals[k].s[2]};
    }
    out = c;
    return true;
}
// Fingertip: past the last joint by most of that joint's own bone length.
Vec3 tipOf(const Chain& last, const Xf* locals, int lastJoint) {
    Vec3 v = mul(vecOf(locals[lastJoint].p), 0.8f);
    return add(last.p, rotate(last.q, {last.s.x * v.x, last.s.y * v.y, last.s.z * v.z}));
}

// Fingers from the bind pose: index on the trigger, the other three on the
// grip. The thumb, when down on a button, reaches for the index finger: its
// tip when the index curls alone (the OK sign), its middle joint otherwise.
void poseFingers(uint8_t* skeleton, Xf* locals, const Arm& arm, const Side& side, float weight) {
    if (!arm.frame) return;
    Override ov[16];
    int n = 0, thumbAt = -1;
    for (int i = 0; i < arm.fingerCount; ++i) {
        const Finger& finger = arm.fingers[i];
        if (finger.kind == kThumb) continue;
        for (int k = 0; k < finger.count && n < 16; ++k) {
            float angle = side.holdPose            ? side.holdCurl * kFist[k]
                          : finger.kind == kIndex ? side.index * (kOk[k] + (kFist[k] - kOk[k]) * side.grip)
                                                  : side.grip * kFist[k];
            // Your real hand (hand tracking): each joint bends like yours.
            if (side.tracked > 0 && !side.holdPose) angle += (side.curl[finger.name][k] - angle) * side.tracked;
            ov[n++] = {finger.joints[k], qmul(finger.bind[k], axisAngle(finger.axis[k], angle))};
        }
    }
    if (arm.thumb >= 0) {
        const Finger& thumb = arm.fingers[arm.thumb];
        thumbAt = n;
        for (int k = 0; k < thumb.count && n < 16; ++k) {
            Quat q = thumb.bind[k];
            if (side.holdPose) {
                // Fixed: swung about the palm's normal at its root, then bent.
                if (k == 0) q = qmul(q, axisAngle(thumb.spread, side.thumbSpread));
                q = qmul(q, axisAngle(thumb.axis[k], side.thumbBend * kThumbBend[k]));
            } else if (side.tracked > 0) {
                q = qmul(q, axisAngle(thumb.axis[k], side.curl[0][k] * side.tracked));
            }
            ov[n++] = {thumb.joints[k], q};
        }
        if (side.thumb > 0.01f && !side.holdPose && arm.index >= 0 && n - thumbAt == thumb.count) {
            const Finger& index = arm.fingers[arm.index];
            int mid = index.joints[index.count > 1 ? 1 : 0], last = index.joints[index.count - 1];
            Chain m, l;
            if (inHand(skeleton, locals, arm.hand, mid, ov, n, m) && inHand(skeleton, locals, arm.hand, last, ov, n, l)) {
                Vec3 tip = tipOf(l, locals, last);
                float toTip = side.index * (1 - side.grip);
                Vec3 goal = add(m.p, mul(sub(tip, m.p), toTip));
                // CCD: turn each thumb joint, tip first, to bring the thumb tip to the goal.
                int end = thumb.joints[thumb.count - 1];
                for (int iter = 0; iter < 10; ++iter) {
                    for (int k = thumb.count - 1; k >= 0; --k) {
                        Chain jk, e;
                        if (!inHand(skeleton, locals, arm.hand, thumb.joints[k], ov, n, jk) ||
                            !inHand(skeleton, locals, arm.hand, end, ov, n, e))
                            break;
                        Vec3 now = tipOf(e, locals, end);
                        if (length(sub(now, goal)) < 0.002f) break;
                        Quat r = fromTo(sub(now, jk.p), sub(goal, jk.p));
                        float angle = 2 * std::acos(r.w > 1 ? 1 : r.w);
                        if (angle > 0.3f) r = slerp({0, 0, 0, 1}, r, 0.3f / angle);
                        Quat& q = ov[thumbAt + k].q;
                        q = qnormalize(qmul(q, qmul(conj(jk.q), qmul(r, jk.q))));
                    }
                }
                for (int k = 0; k < thumb.count; ++k)
                    ov[thumbAt + k].q = slerp(thumb.bind[k], ov[thumbAt + k].q, side.thumb);
            }
        }
    }
    for (int i = 0; i < n; ++i) writeLocal(locals, ov[i].joint, ov[i].q, weight);
}

// Seated: the spine and neck bend so the head joint moves by `lean` (world):
// your real head's move since you sat down. Each joint, from the hips up,
// turns its share of the way toward the target (the last takes the rest),
// each at most kLeanTurn; two passes. solveHead then sets the head's own turn.
// Publishes how far the head joint really went, in the world.
void turnJoint(void* pose, uint8_t* skeleton, Xf* locals, int joint, const Quat& delta);
constexpr float kLeanTurn = 0.5f;  // radians per joint, about 29 degrees
void solveLean(void* pose, uint8_t* skeleton, Xf* locals, const Xf& actor, Vec3 lean) {
    const RigCache& c = g_cache;
    Quat actorQ = quatOf(actor.q);
    Vec3 d = rotate(conj(actorQ), lean);
    d = {d.x / actor.s[0], d.y / actor.s[1], d.z / actor.s[2]};
    Vec3 start = vecOf(modelOf(pose, c.head).p);
    Vec3 target = add(start, d);
    const float share[4] = {0.4f, 0.45f, 0.6f, 1.0f};  // of what's left, from the hips up
    float turned[4] = {0, 0, 0, 0};
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = 0; k < c.spineCount; ++k) {
            int j = c.spine[k];
            Vec3 at = vecOf(modelOf(pose, j).p), head = vecOf(modelOf(pose, c.head).p);
            Vec3 a = sub(head, at), b = sub(target, at);
            if (length(a) < 1e-4f || length(b) < 1e-4f) continue;
            float part = k == c.spineCount - 1 ? share[3] : share[k];
            Quat full = fromTo(normalize(a), normalize(b));
            if (full.w < 0) full = {-full.x, -full.y, -full.z, -full.w};
            float angle = 2 * std::acos(full.w > 1 ? 1 : full.w) * part;
            float room = kLeanTurn - turned[k];
            if (room <= 0) continue;
            if (angle > room) angle = room;
            float fullAngle = 2 * std::acos(full.w > 1 ? 1 : full.w);
            if (!(fullAngle > 1e-5f)) continue;
            turnJoint(pose, skeleton, locals, j, slerp({0, 0, 0, 1}, full, angle / fullAngle));
            turned[k] += angle;
        }
    }
    Vec3 moved = sub(vecOf(modelOf(pose, c.head).p), start);
    moved = rotate(actorQ, {moved.x * actor.s[0], moved.y * actor.s[1], moved.z * actor.s[2]});
    Lean done{{moved.x, moved.y, moved.z}, GetTickCount64()};
    g_leanDone.write(done);
    if (g_leanLogs < 40 && (g_solves % 600) == 1) {
        ++g_leanLogs;
        LOG("Lean: wanted %.0f %.0f %.0f cm, the head went %.0f %.0f %.0f cm", lean.x * 100, lean.y * 100,
            lean.z * 100, moved.x * 100, moved.y * 100, moved.z * 100);
    }
}

// The head looks where you look; the neck takes part of the turn.
void solveHead(void* pose, uint8_t* skeleton, Xf* locals, const Frame& frame, const Xf& actor, float weight) {
    const RigCache& c = g_cache;
    Quat view = qmul(conj(quatOf(actor.q)), qmul(frame.heading, frame.hmd));
    Vec3 f = rotate(view, {0, 0, -1}), u = rotate(view, {0, 1, 0});
    Quat want = qmul(fromBasis(f, u, cross(f, u)),
                     conj(fromBasis(c.headForward, c.headUp, cross(c.headForward, c.headUp))));
    Quat now = quatOf(modelOf(pose, c.head).q);
    Quat delta = qmul(want, conj(now));
    if (delta.w < 0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    float angle = 2 * std::acos(delta.w > 1 ? 1 : delta.w);
    const float kMax = 1.75f;  // never wring the neck past ~100 degrees from the animation
    if (angle > kMax) {
        delta = slerp({0, 0, 0, 1}, delta, kMax / angle);
        want = qmul(delta, now);
    }
    int parent = g_parent(skeleton, static_cast<uint32_t>(c.head));
    g_headMoved = true;
    g_headBefore = now;
    g_headAfter = qnormalize(slerp(now, want, weight));
    if (c.neck >= 0) {
        Quat part = slerp({0, 0, 0, 1}, delta, 0.35f);
        Quat neckRot = qmul(part, quatOf(modelOf(pose, c.neck).q));
        Quat neckParent = quatOf(modelOf(pose, g_parent(skeleton, static_cast<uint32_t>(c.neck))).q);
        Quat headParent = parent == c.neck ? neckRot : qmul(part, quatOf(modelOf(pose, parent).q));
        writeLocal(locals, c.neck, qmul(conj(neckParent), neckRot), weight);
        writeLocal(locals, c.head, qmul(conj(headParent), want), weight);
    } else {
        writeLocal(locals, c.head, qmul(conj(quatOf(modelOf(pose, parent).q)), want), weight);
    }
}

// The Sim's eyes turn toward where you point (the grip + A / X laser's end),
// on top of wherever the head is: both converge on the point, within limits
// measured from the face's straight ahead; past them they stop at the edge.
// Up is kept small: the eyelids don't follow, so looking far up rolled the
// irises out of sight (only the whites showed).
constexpr float kEyeSide = 0.52f;  // radians left or right, about 30 degrees
constexpr float kEyeUp = 0.17f;    // about 10 degrees
constexpr float kEyeDown = 0.35f;  // about 20 degrees
void solveEyes(void* pose, uint8_t* skeleton, Xf* locals, const Xf& actor, const Vec3& target, float weight) {
    const RigCache& c = g_cache;
    Quat headBefore = g_headMoved ? g_headBefore : quatOf(modelOf(pose, c.head).q);
    Quat headAfter = g_headMoved ? g_headAfter : headBefore;
    Quat headDelta = qmul(headAfter, conj(headBefore));
    Vec3 headPos = vecOf(modelOf(pose, c.head).p);
    // The target in model space.
    Quat toModel = conj(quatOf(actor.q));
    Vec3 m = rotate(toModel, sub(target, vecOf(actor.p)));
    Vec3 goal{m.x / actor.s[0], m.y / actor.s[1], m.z / actor.s[2]};
    if (!finite(goal)) return;
    // The face's axes once the head has turned: forward, up, and to one side.
    Vec3 faceF = normalize(rotate(headAfter, c.headForward)), faceU = normalize(rotate(headAfter, c.headUp));
    Vec3 faceS = normalize(cross(faceU, faceF));
    for (int i = 0; i < 2; ++i) {
        int eye = c.eye[i];
        int parent = g_parent(skeleton, static_cast<uint32_t>(eye));
        if (parent < 0) continue;
        Xf now = modelOf(pose, eye);
        // Where the eye is once the head has turned.
        Quat eyeRot = qmul(headDelta, quatOf(now.q));
        Vec3 eyePos = add(headPos, rotate(headDelta, sub(vecOf(now.p), headPos)));
        Quat parentRot = parent == c.head ? headAfter : qmul(headDelta, quatOf(modelOf(pose, parent).q));
        Vec3 gaze = normalize(rotate(eyeRot, c.eyeForward[i]));
        Vec3 to = sub(goal, eyePos);
        float d = length(to);
        if (!(d > 1e-3f)) continue;
        to = mul(to, 1 / d);
        // Keep the gaze inside the limits: sideways and up/down from the face.
        float f = dot(to, faceF), u = dot(to, faceU), sd = dot(to, faceS);
        float yaw = std::atan2(sd, f), pitch = std::atan2(u, std::sqrt(f * f + sd * sd));
        yaw = yaw > kEyeSide ? kEyeSide : yaw < -kEyeSide ? -kEyeSide : yaw;
        pitch = pitch > kEyeUp ? kEyeUp : pitch < -kEyeDown ? -kEyeDown : pitch;
        Vec3 flat = add(mul(faceF, std::cos(yaw)), mul(faceS, std::sin(yaw)));
        to = normalize(add(mul(flat, std::cos(pitch)), mul(faceU, std::sin(pitch))));
        Quat turn = fromTo(gaze, to);
        writeLocal(locals, eye, qmul(conj(parentRot), qmul(turn, eyeRot)), weight);
    }
}

// Your reach on the Sim's arm. Your arms and shoulders are not the Sim's, so
// your arm stretched all the way often ends short of the Sim's full reach and
// its elbow stays bent, by different amounts forward, out to the side and up
// (v0.12.7 log: forward 0.90, out and up 1.0). Each side learns the farthest
// you reach (as a share of the Sim's arm, held for a moment; it only grows,
// up to the whole arm) toward six directions (out, in, up, down, forward,
// back), and reaches past kReachKnee below the farthest in that direction
// (blended between them) are stretched so that yours maps to the Sim's arm
// straight. Closer in, the hand stays where yours is.
constexpr int kReachDirs = 6;
const Vec3 kReachDir[kReachDirs] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const char* const kReachName[kReachDirs] = {"out", "in", "up", "down", "forward", "back"};
struct Reach {
    float peak[kReachDirs] = {0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.85f};  // start: a guess, raised by your stretches
    float logged[kReachDirs] = {};
    float smooth = 0;       // your reach, held ~150 ms (a tracking blip doesn't count)
    float window = 0;       // the farthest since the last log line
};
Reach g_reach[2];
float g_solveDt = 0;
constexpr float kReachKnee = 0.30f;  // share of the arm below your farthest reach that is left as it is

float stretchReach(int side, float r, Vec3 dir, float outward) {
    Reach& reach = g_reach[side];
    reach.smooth += (r - reach.smooth) * ease(7.0f, g_solveDt);
    if (reach.smooth > reach.window) reach.window = reach.smooth;
    Vec3 local{dir.x * outward, dir.y, dir.z};  // x out from this shoulder, y up, z forward
    float weightSum = 0, peak = 0;
    for (int k = 0; k < kReachDirs; ++k) {
        float a = dot(local, kReachDir[k]);
        if (a <= 0) continue;
        if (a > 0.6f && reach.smooth > reach.peak[k] && g_side[side].weight > 0.9f) {
            reach.peak[k] = reach.smooth < 1.0f ? reach.smooth : 1.0f;
            static int logged = 0;
            if (reach.peak[k] - reach.logged[k] >= 0.02f && logged++ < 60) {
                reach.logged[k] = reach.peak[k];
                LOG("Arm IK: %s reach learned %s: %.2f of the Sim's arm", side ? "R" : "L", kReachName[k], reach.peak[k]);
            }
        }
        float w = a * a;
        weightSum += w;
        peak += w * reach.peak[k];
    }
    if (!(weightSum > 0)) return r;
    peak /= weightSum;
    float knee = peak - kReachKnee;
    if (r <= knee || peak >= 0.999f) return r;
    return knee + (r - knee) * (1 - knee) / (peak - knee);
}

// The torso's own axes in model space as this pose has them: up the spine,
// the way the chest faces, and out from this arm's shoulder. Some animations
// turn the whole body away from the model's axes (in WickedWhims scenes it
// faces the other way, so elbows posed by the model's axes met in the middle:
// user 2026-10-17 04:04Z). False (axes left as they are) if they can't be read.
bool torsoAxes(void* pose, uint8_t* skeleton, const Arm& arm, const Arm* other, Vec3& up, Vec3& ahead, Vec3& out) {
    if (!other || !other->ok || arm.clavicle < 0 || other->upper < 0) return false;
    int low = arm.clavicle;
    for (int k = 0; k < 3 && low >= 0; ++k) low = g_parent(skeleton, static_cast<uint32_t>(low));  // up to spine 0
    if (low < 0) return false;
    Vec3 s = vecOf(modelOf(pose, arm.upper).p), so = vecOf(modelOf(pose, other->upper).p);
    Vec3 u = sub(mul(add(s, so), 0.5f), vecOf(modelOf(pose, low).p));
    Vec3 across = sub(s, so);
    if (!(length(u) > 0.05f) || !(length(across) > 0.05f)) return false;
    u = normalize(u);
    Vec3 o = sub(across, mul(u, dot(across, u)));
    if (!(length(o) > 0.05f)) return false;
    o = normalize(o);
    Vec3 leftToRight = arm.which == 0 ? mul(across, -1) : across;
    Vec3 a = normalize(cross(u, leftToRight));
    if (!finite(a) || !finite(o)) return false;
    up = u;
    ahead = a;
    out = o;
    return true;
}

// Two-bone IK: the arm reaches so its wrist joint lands on `wrist` (world)
// and the hand turns like `control` (world; a grip pose's frame: -Z along the
// hand toward the fingers, +Y toward the thumb). False if nothing was solved.
// For the player's own hand (stretchSide 0 or 1) it publishes where the wrist
// went (handPose), less pinShift, with pinGrip (if given) as its frame: the
// hand as it would be without the phone hand's shift. reachOut and handOut
// get the wrist's place and the hand's turn in model space.
bool reachArm(void* pose, uint8_t* skeleton, Xf* locals, const Arm& arm, const Arm* other, Vec3 wrist,
              const Quat& controlWorld,
              const Xf& actor, float weight, bool logIt, const char* who, int stretchSide = -1,
              Vec3 pinShift = {0, 0, 0}, const Quat* pinGrip = nullptr, Vec3* reachOut = nullptr,
              Quat* handOut = nullptr) {
    Quat actorRot = quatOf(actor.q);
    Quat toModel = conj(actorRot);
    Vec3 m = rotate(toModel, sub(wrist, vecOf(actor.p)));
    Vec3 target{m.x / actor.s[0], m.y / actor.s[1], m.z / actor.s[2]};
    Quat control = qmul(toModel, controlWorld);
    if (!finite(target)) return false;

    Xf clav = modelOf(pose, arm.clavicle), upper = modelOf(pose, arm.upper), fore = modelOf(pose, arm.fore),
       wristNow = modelOf(pose, arm.hand);
    Vec3 S = vecOf(upper.p), E = vecOf(fore.p), W = vecOf(wristNow.p);
    float l1 = length(sub(E, S)), l2 = length(sub(W, E));
    if (!(l1 > 1e-3f) || !(l2 > 1e-3f)) return false;

    // Two bones: the elbow goes where the arm bends toward the pole.
    Vec3 toTarget = sub(target, S);
    float d = length(toTarget);
    if (!(d > 1e-4f)) return false;
    Vec3 dir = mul(toTarget, 1 / d);
    float raw = d;
    Vec3 up{0, 1, 0}, ahead{0, 0, 1}, out{arm.outward, 0, 0};
    torsoAxes(pose, skeleton, arm, other, up, ahead, out);
    const Vec3 local{dot(dir, out), dot(dir, up), dot(dir, ahead)};  // out from this shoulder, up, forward
    if (stretchSide >= 0) d = stretchReach(stretchSide, d / (l1 + l2), local, 1.0f) * (l1 + l2);
    float dMin = std::fabs(l1 - l2) + 0.02f * (l1 + l2), dMax = (l1 + l2) * 0.999f;
    d = d < dMin ? dMin : d > dMax ? dMax : d;
    float cosA = (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d);
    cosA = cosA > 1 ? 1 : cosA < -1 ? -1 : cosA;
    float sinA = std::sqrt(1 - cosA * cosA);
    // Elbows hang down, a little out and back; raising the hand swings them out.
    Vec3 pole = sub(sub(mul(out, 0.5f + (local.y > 0 ? local.y : 0)), up), mul(ahead, 0.35f));
    Vec3 bend = sub(pole, mul(dir, dot(pole, dir)));
    if (length(bend) < 1e-3f) bend = sub(sub(E, S), mul(dir, dot(sub(E, S), dir)));
    bend = normalize(bend);
    Vec3 elbow = add(S, mul(add(mul(dir, cosA), mul(bend, sinA)), l1));
    Vec3 reach = add(S, mul(dir, d));
    if (stretchSide >= 0 && stretchSide < 2) {
        // The player's own hand: publish where its wrist goes, in the world.
        Vec3 scaled{reach.x * actor.s[0], reach.y * actor.s[1], reach.z * actor.s[2]};
        HandPose hp;
        hp.valid = true;
        hp.wrist = sub(add(vecOf(actor.p), rotate(actorRot, scaled)), pinShift);
        hp.grip = pinGrip ? *pinGrip : controlWorld;
        hp.scale = g_frameScale;
        hp.tick = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_handPoseMutex);
        g_handPose[stretchSide] = hp;
    }
    Quat d1 = fromTo(sub(E, S), sub(elbow, S));
    Vec3 W1 = add(S, rotate(d1, sub(W, S)));
    Quat d2 = fromTo(sub(W1, elbow), sub(reach, elbow));
    Quat upperRot = qmul(d1, quatOf(upper.q));
    Quat foreRot = qmul(d2, qmul(d1, quatOf(fore.q)));

    // The hand turns like the controller: its forward (wrist to knuckles) along
    // the grip pose's -Z, its thumb side along +Y.
    Quat handRot;
    if (arm.frame) {
        Vec3 f = rotate(control, {0, 0, -1}), t = rotate(control, {0, 1, 0});
        Quat aim = fromBasis(f, t, cross(f, t));
        Quat local = fromBasis(arm.forward, arm.side, arm.up);
        handRot = qmul(aim, conj(local));
        // Most of the turn around the forearm happens in the forearm (the
        // radius rolls over the ulna), not at the wrist.
        Vec3 along = normalize(rotate(conj(foreRot), sub(reach, elbow)));
        Quat rel = qmul(conj(foreRot), handRot);
        float twist = 2 * std::atan2(rel.x * along.x + rel.y * along.y + rel.z * along.z, rel.w);
        twist = std::remainder(twist, 6.2831853f);
        if (std::isfinite(twist)) foreRot = qmul(foreRot, axisAngle(along, twist * 0.6f));
    } else {
        handRot = qmul(qmul(foreRot, conj(quatOf(fore.q))), quatOf(wristNow.q));
    }
    if (reachOut) *reachOut = reach;
    if (handOut) *handOut = handRot;

    // Back to parent-relative rotations. A joint between two of ours (a twist
    // helper) turned with the one above it.
    Quat upperDelta = qmul(upperRot, conj(quatOf(upper.q))), foreDelta = qmul(foreRot, conj(quatOf(fore.q)));
    auto parentRot = [&](int joint) -> Quat {
        int p = g_parent(skeleton, static_cast<uint32_t>(joint));
        if (p == arm.upper) return upperRot;
        if (p == arm.fore) return foreRot;
        Quat old = quatOf(modelOf(pose, p).q);
        if (isAncestor(skeleton, arm.fore, p)) return qmul(foreDelta, old);
        return qmul(upperDelta, old);
    };
    Quat foreParent = parentRot(arm.fore), handParent = parentRot(arm.hand);
    writeLocal(locals, arm.upper, qmul(conj(quatOf(clav.q)), upperRot), weight);
    writeLocal(locals, arm.fore, qmul(conj(foreParent), foreRot), weight);
    writeLocal(locals, arm.hand, qmul(conj(handParent), handRot), weight);
    if (logIt) {
        LOG("Arm IK: %s target %.2f %.2f %.2f (model), shoulder %.2f %.2f %.2f, arm %.2f + %.2f, reach %.2f -> %.2f%s; "
            "torso faces %.2f %.2f %.2f, up %.2f %.2f %.2f",
            who, target.x, target.y, target.z, S.x, S.y, S.z, l1, l2, raw, d, d >= (l1 + l2) * 0.999f ? " (straight)" : "",
            ahead.x, ahead.y, ahead.z, up.x, up.y, up.z);
    }
    return true;
}

// Your hand pushed back out of other Sims' bodies; returns how deep it went (world units).
float keepOutOfBodies(int sideIndex, Vec3& palm, Vec3& wrist, const Quat& gripWorld, float scale);

void solveArm(void* pose, uint8_t* skeleton, Xf* locals, const Arm& arm, int sideIndex, const Hand& hand,
              const Frame& frame, const Xf& actor, float weight) {
    const Side& side = g_side[sideIndex];
    float s = sideIndex == 0 ? 1.0f : -1.0f;  // palm along +X (left) or -X (right) of the grip pose

    // The hand points a little below the grip pose's -Z (hand_pitch). The grip
    // pose sits in the palm; the wrist joint is 8 cm behind it and 2 cm toward
    // the back of the hand.
    Quat grip = qmul(hand.orientation, axisAngle({1, 0, 0}, -g_handPitch));
    g_frameScale = frame.scale;
    // A hand holding the phone sits where [phone] hand_offset and hand_turn
    // put it (moved, and turned about the palm); the phone keeps to the
    // controller, so handPose reports the hand without that shift.
    HoldPose hold;
    const bool held = g_holdPose[sideIndex].read(hold) && hold.on;
    const Vec3 toWrist{-s * 0.02f, 0, 0.08f};
    Vec3 palmXr = hand.position;
    Quat handGrip = grip;
    if (held) {
        palmXr = add(hand.position, rotate(grip, hold.move));
        handGrip = qnormalize(qmul(grip, hold.turn));
    } else if (side.point > 0 && arm.pointer && arm.frame && frame.scale > 0) {
        // Pointing the laser (grip + A / X): the laser comes from the
        // controller's aim pose, which points elsewhere than the Sim's hand
        // does. The hand turns about the palm, and shifts, so its straight
        // index finger lies along the laser.
        Point point;
        if (g_point[sideIndex].read(point) && point.on) {
            Quat back = conj(grip);
            g_pointFrom[sideIndex] = rotate(back, sub(point.from, hand.position));
            g_pointDir[sideIndex] = normalize(rotate(back, point.dir));
        }
        auto toGrip = [&](Vec3 v) { return Vec3{dot(v, arm.up), dot(v, arm.side), -dot(v, arm.forward)}; };
        const Vec3 dir = g_pointDir[sideIndex];
        const float unit = actor.s[0] / frame.scale;  // the Sim's hand frame to metres
        Vec3 finger = toGrip(arm.indexDir);
        Quat turn = fromTo(finger, dir);
        Vec3 knuckle = rotate(turn, add(toWrist, mul(toGrip(arm.indexBase), unit)));
        Vec3 off = sub(g_pointFrom[sideIndex], knuckle);
        Vec3 shift = sub(off, mul(dir, dot(off, dir)));
        float apart = length(shift);
        if (apart > 0.1f) shift = mul(shift, 0.1f / apart);
        if (side.point > 0.99f && g_pointLogs < 6) {
            ++g_pointLogs;
            float d = dot(finger, dir);
            LOG("Arm IK: %s hand points the laser: index finger turned %.0f degrees onto it, hand moved %.1f cm "
                "(laser in the grip frame %.2f %.2f %.2f from %.3f %.3f %.3f)",
                sideIndex ? "R" : "L", std::acos(d > 1 ? 1 : d < -1 ? -1 : d) * 57.29578f, apart * 100, dir.x, dir.y,
                dir.z, g_pointFrom[sideIndex].x, g_pointFrom[sideIndex].y, g_pointFrom[sideIndex].z);
        }
        palmXr = add(hand.position, rotate(grip, mul(shift, side.point)));
        handGrip = qnormalize(qmul(grip, slerp({0, 0, 0, 1}, turn, side.point)));
    }
    Vec3 wristXr = add(palmXr, rotate(handGrip, toWrist));
    Vec3 world = add(vecOf(frame.anchor), rotate(frame.heading, mul(wristXr, frame.scale)));
    Vec3 palm = add(vecOf(frame.anchor), rotate(frame.heading, mul(palmXr, frame.scale)));
    Quat gripWorld = qmul(frame.heading, handGrip);
    Vec3 pinShift = rotate(frame.heading, mul(sub(wristXr, add(hand.position, rotate(grip, toWrist))), frame.scale));
    Quat pinGrip = qmul(frame.heading, grip);
    // A tracked hand (your real one, looks only) touches no Sim.
    float depth = hand.tracked ? 0.0f : keepOutOfBodies(sideIndex, palm, world, gripWorld, frame.scale);
    GrabPlace stuck;
    const bool grabbing =
        !hand.tracked && g_grabPlace[sideIndex].read(stuck) && stuck.on && ageMs(GetTickCount64(), stuck.tick) < 200;
    if (grabbing) {
        Vec3 shift = sub(stuck.at, palm);
        palm = add(palm, shift);
        world = add(world, shift);
    }
    g_contact[sideIndex].store(frame.scale > 0 ? depth / frame.scale : 0, std::memory_order_relaxed);
    g_contactTick[sideIndex].store(GetTickCount64(), std::memory_order_relaxed);
    bool logIt = g_solves <= 2 || g_solves % 3000 == 0;
    Vec3 reachModel{0, 0, 0};
    Quat handModel{0, 0, 0, 1};
    if (!reachArm(pose, skeleton, locals, arm, &g_cache.arm[1 - sideIndex], world, gripWorld, actor, weight, logIt,
                  sideIndex ? "R" : "L", sideIndex,
                  pinShift, &pinGrip, &reachModel, &handModel))
        return;

    // How far the game's own animation curls the middle, ring and little
    // fingers (before ours): a hand closed around a prop.
    float curl = 0;
    int curled = 0;
    for (int i = 0; i < arm.fingerCount; ++i) {
        const Finger& finger = arm.fingers[i];
        if (finger.kind != kOther) continue;
        for (int k = 0; k < finger.count; ++k) {
            float d = std::fabs(dot(quatOf(locals[finger.joints[k]].q), finger.bind[k]));
            curl += 2 * std::acos(d > 1 ? 1 : d);
            ++curled;
        }
    }
    if (curled) g_gameCurl[sideIndex].store(curl / curled, std::memory_order_relaxed);

    // Our phone in this hand (setHoldPose): its fingers are ours whatever the
    // game's animation does or holds (an idle's hand pose, a carried prop's grip).
    float fingers = held ? weight : weight * (1 - side.hold);
    Side posed = side;
    posed.index = side.index * (1 - side.point);  // a pointing finger stays straight
    if (grabbing) {
        // Holding a body: the fingers close as far as grab_curl, whatever the buttons.
        float c = grabCurlFor(sideIndex);
        posed.index = posed.grip = posed.thumb = c;
    }
    if (held) {
        posed.holdPose = true;
        posed.holdCurl = hold.curl;
        posed.thumbBend = hold.bend;
        posed.thumbSpread = hold.spread;
    }
    if (fingers > 0.01f) {
        if (!held && g_gripHold[sideIndex].load(std::memory_order_relaxed) && g_sideGrip[sideIndex].valid)
            poseGrip(locals, arm, g_sideGrip[sideIndex], fingers);
        else
            poseFingers(skeleton, locals, arm, posed, fingers);
    }

    // The index fingertip as posed, in the controller's grip frame: what pokes
    // the phone's screen (vr.cpp), so a press comes when the finger you see
    // reaches the glass.
    if (arm.frame && arm.index >= 0 && frame.scale > 0) {
        const Finger& index = arm.fingers[arm.index];
        int last = index.joints[index.count - 1];
        Chain l;
        if (index.count > 0 && inHand(skeleton, locals, arm.hand, last, nullptr, 0, l)) {
            Vec3 m = add(reachModel, rotate(handModel, tipOf(l, locals, last)));
            Vec3 tipWorld = add(vecOf(actor.p), rotate(quatOf(actor.q), {m.x * actor.s[0], m.y * actor.s[1], m.z * actor.s[2]}));
            Vec3 controller = add(vecOf(frame.anchor), rotate(frame.heading, mul(hand.position, frame.scale)));
            Vec3 local = mul(rotate(conj(qmul(frame.heading, grip)), sub(tipWorld, controller)), 1 / frame.scale);
            if (finite(local)) g_tip[sideIndex].write(Tip{true, local, GetTickCount64()});
        }
    }
}

// Once per rig: does the game compose rotations as parent * local, as the
// static analysis says? Wrong order would twist every arm.
bool checkFk(void* pose, uint8_t* skeleton, Xf* locals, const Arm& arm) {
    int parent = g_parent(skeleton, static_cast<uint32_t>(arm.hand));
    if (parent < 0) return false;
    Quat model = quatOf(modelOf(pose, arm.hand).q), parentModel = quatOf(modelOf(pose, parent).q);
    Quat local = quatOf(locals[arm.hand].q);
    float ours = quatDistance(model, qmul(parentModel, local)), other = quatDistance(model, qmul(local, parentModel));
    LOG("Arm IK: joint rotations compose as %s (error %.5f, other order %.5f)",
        ours < 1e-3f ? "parent * local, as expected" : "NOT parent * local: arm IK off", ours, other);
    return ours < 1e-3f;
}

// What the latest solve on this job thread posed the hands for, so the hook
// can read the finished hands right after the game makes its joints
// (captureShape): the frame, the controllers, and the actor's transform.
struct SolveContext {
    bool ok = false;
    Frame frame;
    Hand hand[2];
    Xf world;
};
thread_local SolveContext t_solved;

void solve(uint8_t* actor, uint8_t* pose) {
    t_solved.ok = false;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(g_freq.QuadPart) : 0;
    g_last = now;
    if (!(dt > 0) || dt > 0.1f) dt = dt > 0.1f ? 0.1f : 0;
    g_solveDt = dt;
    {
        // Your farthest reach every 30 s, to tune the start guess.
        static ULONGLONG since = 0;
        static int lines = 0;
        ULONGLONG t = GetTickCount64();
        if (!since) since = t;
        if (t - since >= 30000) {
            since = t;
            if ((g_reach[0].window > 0 || g_reach[1].window > 0) && lines++ < 60)
                LOG("Arm IK: farthest reach in the last 30 s: L %.2f, R %.2f of the Sim's arm (learned forward L %.2f R "
                    "%.2f, out L %.2f R %.2f, up L %.2f R %.2f)",
                    g_reach[0].window, g_reach[1].window, g_reach[0].peak[4], g_reach[1].peak[4], g_reach[0].peak[0],
                    g_reach[1].peak[0], g_reach[0].peak[2], g_reach[1].peak[2]);
            g_reach[0].window = g_reach[1].window = 0;
        }
    }

    Frame frame{};
    Hands hands{};
    ULONGLONG tick = GetTickCount64();
    bool frameOk = g_frame.read(frame) && frame.actor == actor && ageMs(tick, frame.tick) < 300;
    bool handsOk = g_hands.read(hands) && ageMs(tick, hands.tick) < 300;
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        Side& side = g_side[i];
        const Hand& hand = hands.hand[i];
        bool on = frameOk && handsOk && hand.valid;
        side.weight += ((on ? 1.0f : 0.0f) - side.weight) * ease(12.0f, dt);
        if (!on && side.weight < 0.01f) side.weight = 0;
        if (on) {
            side.index += (hand.index - side.index) * ease(25.0f, dt);
            side.grip += (hand.grip - side.grip) * ease(25.0f, dt);
            side.thumb += (hand.thumb - side.thumb) * ease(20.0f, dt);
            side.tracked += ((hand.tracked ? 1.0f : 0.0f) - side.tracked) * ease(10.0f, dt);
            if (hand.tracked)
                for (int f = 0; f < 5; ++f)
                    for (int k = 0; k < 3; ++k) side.curl[f][k] += (hand.curl[f][k] - side.curl[f][k]) * ease(30.0f, dt);
        }
        float holding = g_holding[i].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        side.hold += (holding - side.hold) * ease(10.0f, dt);
        Point point;
        bool pointing = on && g_point[i].read(point) && point.on && ageMs(tick, point.tick) < 200;
        side.point += ((pointing ? 1.0f : 0.0f) - side.point) * ease(15.0f, dt);
        if (!pointing && side.point < 0.01f) side.point = 0;
        any = any || side.weight > 0;
    }
    bool headOn = frameOk && g_headTracking;
    g_headWeight += ((headOn ? 1.0f : 0.0f) - g_headWeight) * ease(8.0f, dt);
    if (!headOn && g_headWeight < 0.01f) g_headWeight = 0;
    any = any || g_headWeight > 0;
    bool leanOn = false;
    {
        Lean wanted{};
        leanOn = frameOk && !frame.free && g_leanWanted.read(wanted) && ageMs(tick, wanted.tick) < 300;
        Vec3 want = leanOn ? Vec3{wanted.world[0], wanted.world[1], wanted.world[2]} : Vec3{0, 0, 0};
        g_leanNow = add(g_leanNow, mul(sub(want, g_leanNow), ease(25.0f, dt)));
        if (!leanOn && length(g_leanNow) < 0.002f) g_leanNow = {0, 0, 0};
        any = any || length(g_leanNow) > 0;
    }
    LookTarget look{};
    bool lookOn = frameOk && g_lookTarget.read(look) && look.on && ageMs(tick, look.tick) < 200;
    g_eyeWeight += ((lookOn ? 1.0f : 0.0f) - g_eyeWeight) * ease(15.0f, dt);
    if (!lookOn && g_eyeWeight < 0.01f) g_eyeWeight = 0;
    // Eyes ease back from the last point they looked at.
    static Vec3 lastLook{0, 0, 0};
    if (lookOn) {
        Vec3 xr{look.point[0], look.point[1], look.point[2]};
        lastLook = add(vecOf(frame.anchor), rotate(frame.heading, mul(xr, frame.scale)));
    }
    any = any || g_eyeWeight > 0;
    if (!any || !frameOk) return;

    uint8_t* rig = *reinterpret_cast<uint8_t**>(pose + kPoseRig);
    Xf* locals = *reinterpret_cast<Xf**>(pose + kPoseLocals);
    uint32_t poseCount = *reinterpret_cast<uint16_t*>(pose + kPoseJointCount);
    if (!rig || !locals) return;
    uint8_t* skeleton = rig + kRigSkeleton;
    uint32_t count = g_jointCount(skeleton);
    if (count > poseCount) count = poseCount;
    if (!count || count > kMaxJoints) return;
    if (rig != g_cache.rig) {
        g_cache = RigCache{};
        g_cache.rig = rig;
        for (int i = 0; i < 2; ++i) resolveArm(rig, i, count, g_cache.arm[i]);
        resolveHead(rig, count, g_cache);
        std::lock_guard<std::mutex> lock(g_axesMutex);
        for (int i = 0; i < 2; ++i) {
            const Arm& arm = g_cache.arm[i];
            g_axesKnown[i] = arm.ok && arm.frame;
            g_axes[i][0] = arm.forward;
            g_axes[i][1] = arm.side;
            g_axes[i][2] = arm.up;
        }
    }
    if (!g_cache.fkChecked && (g_cache.arm[0].ok || g_cache.arm[1].ok)) {
        g_cache.fkChecked = true;
        g_cache.fkOk = checkFk(pose, skeleton, locals, g_cache.arm[g_cache.arm[0].ok ? 0 : 1]);
        if (g_cache.fkOk) g_fkVerified = true;
        for (Arm& arm : g_cache.arm)
            if (arm.ok) arm.outward = modelOf(pose, arm.upper).p[0] >= 0 ? 1.0f : -1.0f;
        LOG("Arm IK: left shoulder on the model's %s side", g_cache.arm[0].outward > 0 ? "+X" : "-X");
    }
    if (!g_cache.fkOk) return;

    Xf world;
    std::memcpy(world.p, actor + kActorPosition, sizeof(world.p));
    std::memcpy(world.q, actor + kActorRotation, sizeof(world.q));
    std::memcpy(world.s, actor + kActorScale, sizeof(world.s));
    // Paired eyes: the body can be pinned where the next pair is drawn from
    // before it is there (the game's root motion or the camera hook puts it
    // there before it is drawn). Arms and head are posed for that place, the
    // one your view is at, or the hands lag it (user 2026-10-14 19:59Z).
    float pinned[7];
    if (player::pinnedAt(actor, pinned)) {
        std::memcpy(world.p, pinned, 3 * sizeof(float));
        std::memcpy(world.q, pinned + 3, 4 * sizeof(float));
    }
    if (!(world.s[0] > 1e-3f && world.s[1] > 1e-3f && world.s[2] > 1e-3f) || !finite(vecOf(world.p))) return;
    ++g_solves;
    // Head first: the arms hang off the clavicles, not the neck, so order
    // does not matter for them, but it keeps the head out of their reads.
    g_headMoved = false;
    {
        bool torsoOn = leanOn && g_seatTorso.load(std::memory_order_relaxed) && g_cache.spineCount > 0;
        g_torsoWeight += ((torsoOn ? 1.0f : 0.0f) - g_torsoWeight) * ease(4.0f, dt);
        if (!torsoOn && g_torsoWeight < 0.01f) g_torsoWeight = 0;
        if (g_torsoWeight > 0) {
            for (int k = 0; k < g_cache.spineCount; ++k) {
                Quat anim = quatOf(locals[g_cache.spine[k]].q);
                if (!g_torsoRestOk) g_torsoRest[k] = anim;
                else if (torsoOn) g_torsoRest[k] = qnormalize(slerp(g_torsoRest[k], anim, ease(0.6f, dt)));
                writeLocal(locals, g_cache.spine[k], g_torsoRest[k], g_torsoWeight);
            }
            g_torsoRestOk = true;
        } else {
            g_torsoRestOk = false;
        }
    }
    if (length(g_leanNow) > 0 && g_cache.headOk && g_cache.spineCount > 0) {
        solveLean(pose, skeleton, locals, world, g_leanNow);
    } else {
        Lean none{{0, 0, 0}, GetTickCount64()};
        g_leanDone.write(none);
    }
    if (g_headWeight > 0 && g_cache.headOk) solveHead(pose, skeleton, locals, frame, world, g_headWeight);
    if (g_eyeWeight > 0 && g_cache.eyesOk) solveEyes(pose, skeleton, locals, world, lastLook, g_eyeWeight);
    learnGrip(pose, locals);
    deriveGrips(rig);
    if (!handsOk) return;
    t_solved.ok = true;
    t_solved.frame = frame;
    t_solved.hand[0] = hands.hand[0];
    t_solved.hand[1] = hands.hand[1];
    t_solved.world = world;
    for (int i = 0; i < 2; ++i) {
        if (g_side[i].weight > 0 && g_cache.arm[i].ok && hands.hand[i].valid)
            solveArm(pose, skeleton, locals, g_cache.arm[i], i, hands.hand[i], frame, world, g_side[i].weight);
    }
}


// ---------------------------------------------------------------------------
// Physical touch with other Sims (first person). Runs in the same hook, on the
// game's job threads, for every other Sim near you as its pose is made: so
// several at once, each on its own thread. Shared data is lock-free (seqlocks,
// atomics); the rig table takes a short spinlock and hands out copies.

enum Part { kTorso, kHeadPart, kUpperArmL, kUpperArmR, kForearmL, kForearmR, kThighL, kThighR, kCalfL, kCalfR, kParts };
const char* const kPartNames[kParts] = {"torso", "head", "left upper arm", "right upper arm", "left forearm",
                                        "right forearm", "left thigh", "right thigh", "left calf", "right calf"};
// Radius of each part for an adult (metres; scaled by the Sim's size), and
// how far a push may move it (0: it doesn't give, legs stand their ground).
const float kPartRadius[kParts] = {0.14f, 0.10f, 0.05f, 0.05f, 0.04f, 0.04f, 0.08f, 0.08f, 0.055f, 0.055f};
const float kPartGive[kParts] = {0.08f, 0.06f, 0.15f, 0.15f, 0.15f, 0.15f, 0, 0, 0, 0};
constexpr float kAdultTorso = 0.75f;  // pelvis to head joint of an adult, metres
constexpr float kHandRadius = 0.035f;
constexpr float kFingerReach = 0.09f;  // palm to fingertips

struct SimRig {
    void* rig = nullptr;
    uint32_t count = 0;
    bool sim = false;
    int a[kParts], b[kParts];  // capsule ends (joints); b -1 for the head: its eyes
    int turn[kParts];          // the joint that turns when the part is pushed
    int lever[kParts];         // the end of what turns, which a push moves (-1 for the head: its eyes)
    int eyes[2] = {-1, -1};
    int head = -1, neck = -1, pelvis = -1;
    bool headOk = false, eyesOk = false;
    Vec3 headForward{0, 0, 1}, headUp{0, 1, 0};
    Vec3 eyeForward[2];  // each eye's gaze in its joint's frame
    Arm arm[2];
};
constexpr int kMaxRigs = 32;
SimRig g_rigs[kMaxRigs];
int g_rigCount = 0, g_rigNext = 0;
std::atomic_flag g_rigLock = ATOMIC_FLAG_INIT;

int findJoint(uint8_t* skeleton, uint32_t count, const char* name) {
    uint32_t i = g_jointIndex(skeleton, jointHash(name));
    return i < count ? static_cast<int>(i) : -1;
}

void resolveSim(uint8_t* rig, uint32_t count, SimRig& r) {
    r = SimRig{};
    r.rig = rig;
    r.count = count;
    uint8_t* skeleton = rig + kRigSkeleton;
    auto j = [&](const char* name) { return findJoint(skeleton, count, name); };
    r.pelvis = j("b__Pelvis__");
    r.head = j("b__Head__");
    r.neck = j("b__Neck__");
    r.eyes[0] = j("b__L_Eye__");
    r.eyes[1] = j("b__R_Eye__");
    int spine = j("b__Spine1__");
    if (spine < 0) spine = j("b__Spine0__");
    if (spine < 0) spine = j("b__Spine2__");
    int upper[2] = {j("b__L_UpperArm__"), j("b__R_UpperArm__")}, fore[2] = {j("b__L_Forearm__"), j("b__R_Forearm__")},
        hand[2] = {j("b__L_Hand__"), j("b__R_Hand__")}, thigh[2] = {j("b__L_Thigh__"), j("b__R_Thigh__")},
        calf[2] = {j("b__L_Calf__"), j("b__R_Calf__")}, foot[2] = {j("b__L_Foot__"), j("b__R_Foot__")};
    for (int p = 0; p < kParts; ++p) r.a[p] = r.b[p] = r.turn[p] = r.lever[p] = -1;
    r.a[kTorso] = r.pelvis;
    r.b[kTorso] = r.neck >= 0 ? r.neck : r.head;
    r.turn[kTorso] = spine;
    r.lever[kTorso] = r.b[kTorso];
    r.a[kHeadPart] = r.head;
    r.turn[kHeadPart] = r.neck;
    for (int s = 0; s < 2; ++s) {
        r.a[kUpperArmL + s] = upper[s];
        r.b[kUpperArmL + s] = fore[s];
        r.turn[kUpperArmL + s] = upper[s];
        r.lever[kUpperArmL + s] = fore[s];
        r.a[kForearmL + s] = fore[s];
        r.b[kForearmL + s] = hand[s];
        r.turn[kForearmL + s] = upper[s];
        r.lever[kForearmL + s] = hand[s];
        r.a[kThighL + s] = thigh[s];
        r.b[kThighL + s] = calf[s];
        r.a[kCalfL + s] = calf[s];
        r.b[kCalfL + s] = foot[s];
    }
    // A Sim: a body with a head and two arms (pets and rigged objects aren't).
    r.sim = r.pelvis >= 0 && r.head >= 0 && upper[0] >= 0 && upper[1] >= 0 && hand[0] >= 0 && hand[1] >= 0;
    if (!r.sim) return;
    const Xf* bind = *reinterpret_cast<Xf* const*>(rig + kRigBindLocals);
    Vec3 p;
    Quat q;
    r.headOk = bind && g_parent(skeleton, static_cast<uint32_t>(r.head)) >= 0 &&
               bindInFrame(skeleton, bind, -1, r.head, p, q);
    if (r.headOk) {
        r.headForward = rotate(conj(q), {0, 0, 1});
        r.headUp = rotate(conj(q), {0, 1, 0});
    }
    // The bind pose looks along the model's +Z (as the player's RigCache).
    r.eyesOk = bind && r.eyes[0] >= 0 && r.eyes[1] >= 0;
    for (int i = 0; i < 2 && r.eyesOk; ++i) {
        r.eyesOk = bindInFrame(skeleton, bind, -1, r.eyes[i], p, q);
        if (r.eyesOk) r.eyeForward[i] = rotate(conj(q), {0, 0, 1});
    }
    for (int s = 0; s < 2; ++s) {
        resolveArm(rig, s, count, r.arm[s]);
        // Which side of the model each shoulder is on, for the elbows' pole.
        if (r.arm[s].ok && bind && bindInFrame(skeleton, bind, -1, r.arm[s].upper, p, q))
            r.arm[s].outward = p.x >= 0 ? 1.0f : -1.0f;
    }
    int found = 0;
    for (int k = 0; k < kParts; ++k) found += r.a[k] >= 0 && (r.b[k] >= 0 || k == kHeadPart);
    LOG("Touch: Sim rig %p: %d of %d body parts found (spine %d, neck %d, eyes %d %d)", static_cast<void*>(rig), found,
        kParts, spine, r.neck, r.eyes[0], r.eyes[1]);
}

void lockRigs() {
    while (g_rigLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
}

int findRig(uint8_t* rig, uint32_t count) {
    for (int i = 0; i < g_rigCount; ++i)
        if (g_rigs[i].rig == rig && g_rigs[i].count == count) return i;
    return -1;
}

// A copy of the rig's joints (resolved the first time it is seen).
bool rigFor(uint8_t* rig, uint32_t count, SimRig& out) {
    lockRigs();
    int found = findRig(rig, count);
    if (found >= 0) out = g_rigs[found];
    g_rigLock.clear(std::memory_order_release);
    if (found >= 0) return true;
    // New (or reused memory with another joint count): resolve outside the lock
    // (it logs), then add it, over the oldest one once the table is full.
    resolveSim(rig, count, out);
    lockRigs();
    if (findRig(rig, count) < 0) g_rigs[g_rigCount < kMaxRigs ? g_rigCount++ : (g_rigNext++ % kMaxRigs)] = out;
    g_rigLock.clear(std::memory_order_release);
    return true;
}

struct Capsule {
    Vec3 a, b;
    float r;
};
struct BodyShape {
    void* actor;
    ULONGLONG tick;
    Vec3 root;
    uint32_t valid;  // bit per part
    Capsule part[kParts];
};
// Kept between frames for one Sim, touched only by its own hook.
struct BodyState {
    Quat give[kParts];
    float holdWeight[2];
    Vec3 holdWrist[2];
    Quat holdControl[2];
    float look;
    LARGE_INTEGER last;
    // Joints turned on top of the game's pose last time, and what was written.
    int turned[8];
    float wrote[8][4];
    int turnedCount;
};
struct BodySlot {
    std::atomic<void*> actor{nullptr};
    std::atomic<ULONGLONG> seen{0};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    SeqLock<BodyShape> shape;
    BodyState state;
};
constexpr int kMaxBodies = 16;
BodySlot g_bodies[kMaxBodies];

// Your hands holding Sims' hands: which Sim (actor), which of its hands.
std::atomic<void*> g_holdActor[2] = {nullptr, nullptr};
std::atomic<int> g_holdHand[2] = {0, 0};
std::atomic<bool> g_holdFacing[2] = {false, false};  // its fingers point back along yours (face to face)
std::atomic<ULONGLONG> g_holdAt[2] = {0, 0};

// Grabbing a Sim's body (user 2026-10-17: the breast grab of Claude Jiggle
// extended to the whole body): which Sim each hand holds, and where. The
// place is kept on the part's capsule (how far along it, and the offset from
// its axis in the Sim's model space), so it moves with the body.
constexpr float kGrabPress = 0.6f;  // grip and trigger both past this take hold
constexpr float kGrabKeep = 0.35f;  // either below this lets go
constexpr float kGrabSurface = 0.05f;  // metres off a part's surface the palm may be to take it
constexpr float kGrabSlip = 0.60f;     // metres your hand may get from the held place before it slips out
constexpr float kGrabYank = 0.15f;     // metres of pull that count as pulling hard
enum GrabRegion {
    kRegionHand, kRegionForearm, kRegionUpperArm, kRegionShoulder, kRegionHair, kRegionFace, kRegionNeck,
    kRegionChest, kRegionBelly, kRegionBack, kRegionWaist, kRegionButt, kRegionCrotch, kRegionHips,
    kRegionThigh, kRegionInnerThigh, kRegionLeg, kRegions
};
const char* const kGrabRegionNames[kRegions] = {"hand",  "forearm", "upper arm", "shoulder", "hair",  "face",
                                                "neck",  "chest",   "belly",     "back",     "waist", "butt",
                                                "crotch", "hips",   "thigh",     "inner thigh", "leg"};
struct BodyGrab {
    int part = 0, region = 0;
    float t = 0;          // along the part's capsule, 0..1
    Vec3 offset{0, 0, 0};  // model space, from the axis
    bool pull = true;     // the part follows your hand (not breasts/butt: Claude Jiggle holds those)
    bool yanked = false;
    bool started = false;  // `drawn` set
    Vec3 drawn{0, 0, 0};   // where your Sim's hand is drawn, smoothed
};
std::atomic<void*> g_grabActor[2] = {nullptr, nullptr};
std::atomic<int> g_grabRegion[2] = {0, 0};
std::atomic<ULONGLONG> g_grabAt[2] = {0, 0};
BodyGrab g_grab[2];  // written by whoever won g_grabActor; read by the thread posing that Sim

// A held breast or butt with Integrated Body Physics loaded: its breast_grab_curl / butt_grab_curl (user 2026-10-17
// 14:21Z); anything else, or without it, grab_curl.
float grabCurlFor(int side) {
    float c = g_grabCurl.load(std::memory_order_relaxed);
    int region = g_grabRegion[side].load(std::memory_order_relaxed);
    if (region != kRegionChest && region != kRegionButt) return c;
    using GetFn = othermods::GetFn;
    static std::atomic<GetFn> get{nullptr};
    GetFn fn = get.load(std::memory_order_relaxed);
    if (!fn) {
        othermods::SetFn set;
        if (!othermods::api(othermods::kBodyPhysics, fn, set)) return c;
        get.store(fn, std::memory_order_relaxed);
    }
    float v;
    if (!fn(region == kRegionChest ? "breast_grab_curl" : "butt_grab_curl", &v) || !std::isfinite(v)) return c;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}
std::atomic<void*> g_restricted[16] = {};

bool restricted(void* actor) {
    for (auto& r : g_restricted)
        if (r.load(std::memory_order_relaxed) == actor) return true;
    return false;
}

// The only places a restricted Sim's body may be held.
bool allowedOnRestricted(int region) {
    return region == kRegionHand || region == kRegionForearm || region == kRegionUpperArm ||
           region == kRegionShoulder || region == kRegionBack || region == kRegionHair;
}

// Touch events for the script mod (any thread in, the game thread out).
struct TouchQueue {
    SRWLOCK lock = SRWLOCK_INIT;
    TouchEvent e[32];
    int head = 0, count = 0;
} g_touchQueue;

void pushTouch(int kind, int hand, void* actor, int region, float value) {
    AcquireSRWLockExclusive(&g_touchQueue.lock);
    if (g_touchQueue.count == 32) {  // full: the oldest goes
        g_touchQueue.head = (g_touchQueue.head + 1) % 32;
        --g_touchQueue.count;
    }
    TouchEvent& e = g_touchQueue.e[(g_touchQueue.head + g_touchQueue.count) % 32];
    e.kind = kind;
    e.hand = hand;
    e.actor = actor;
    e.region = region;
    e.value = value;
    ++g_touchQueue.count;
    ReleaseSRWLockExclusive(&g_touchQueue.lock);
}

void releaseGrab(int side, const char* why) {
    void* actor = g_grabActor[side].exchange(nullptr);
    if (!actor) return;
    g_grabPlace[side].write(GrabPlace{});
    int region = g_grabRegion[side].load();
    float held = static_cast<float>(ageMs(GetTickCount64(), g_grabAt[side].load())) / 1000.0f;
    LOG("Touch: your %s hand lets go of a Sim's %s after %.1f s (%s)", side ? "right" : "left",
        kGrabRegionNames[region], held, why);
    pushTouch(2, side, actor, region, held);
}

void resetState(BodyState& st) {
    for (Quat& q : st.give) q = {0, 0, 0, 1};
    st.holdWeight[0] = st.holdWeight[1] = 0;
    st.look = 0;
    st.last.QuadPart = 0;
    st.turnedCount = 0;
}

BodySlot* slotFor(void* actor, ULONGLONG now) {
    for (BodySlot& slot : g_bodies)
        if (slot.actor.load(std::memory_order_relaxed) == actor) {
            slot.seen.store(now, std::memory_order_relaxed);
            return &slot;
        }
    for (BodySlot& slot : g_bodies) {
        void* old = slot.actor.load(std::memory_order_relaxed);
        if (old && ageMs(now, slot.seen.load(std::memory_order_relaxed)) < 2000) continue;
        if (slot.busy.test_and_set(std::memory_order_acquire)) continue;
        if (slot.actor.compare_exchange_strong(old, actor)) {
            resetState(slot.state);
            slot.seen.store(now, std::memory_order_relaxed);
            slot.busy.clear(std::memory_order_release);
            return &slot;
        }
        slot.busy.clear(std::memory_order_release);
    }
    return nullptr;
}

void letGo(int side, const char* why) {
    void* actor = g_holdActor[side].exchange(nullptr);
    if (!actor) return;
    LOG("Touch: your %s hand lets go of a Sim's hand (%s)", side ? "right" : "left", why);
    pushTouch(2, side, actor, kRegionHand, static_cast<float>(ageMs(GetTickCount64(), g_holdAt[side].load())) / 1000.0f);
}

Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = sub(b, a);
    float len2 = dot(ab, ab);
    float t = len2 > 1e-10f ? dot(sub(p, a), ab) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return add(a, mul(ab, t));
}

struct Pose3 {
    void* pose;
    Xf world;
    Vec3 toWorld(Vec3 m) const {
        return add(vecOf(world.p), rotate(quatOf(world.q), {m.x * world.s[0], m.y * world.s[1], m.z * world.s[2]}));
    }
    Vec3 toModel(Vec3 w) const {
        Vec3 m = rotate(conj(quatOf(world.q)), sub(w, vecOf(world.p)));
        return {m.x / world.s[0], m.y / world.s[1], m.z / world.s[2]};
    }
    Vec3 joint(int j) const { return toWorld(vecOf(modelOf(pose, j).p)); }
};

void buildShape(const Pose3& p, const SimRig& r, float size, BodyShape& out) {
    out.valid = 0;
    for (int k = 0; k < kParts; ++k) {
        if (r.a[k] < 0) continue;
        Vec3 a = p.joint(r.a[k]), b;
        if (k == kHeadPart) {
            if (r.eyes[0] >= 0 && r.eyes[1] >= 0)
                b = mul(add(p.joint(r.eyes[0]), p.joint(r.eyes[1])), 0.5f);
            else
                b = add(a, mul(sub(a, p.joint(r.pelvis)), 0.15f));
        } else {
            if (r.b[k] < 0) continue;
            b = p.joint(r.b[k]);
        }
        if (!finite(a) || !finite(b)) continue;
        out.part[k] = {a, b, kPartRadius[k] * size};
        out.valid |= 1u << k;
    }
}

// A Sim's palm (world), from its hand joint: a little along the hand.
Vec3 simPalm(const Pose3& p, const Arm& arm, float size) {
    Xf h = modelOf(p.pose, arm.hand);
    Vec3 along = arm.frame ? arm.forward : Vec3{0, 0, 0};
    return p.toWorld(add(vecOf(h.p), rotate(quatOf(h.q), mul(along, 0.07f * size))));
}

// Turns `joint` by `delta` (model space) about itself, taking its children along.
void turnJoint(void* pose, uint8_t* skeleton, Xf* locals, int joint, const Quat& delta) {
    int parent = g_parent(skeleton, static_cast<uint32_t>(joint));
    if (parent < 0) return;
    Quat now = quatOf(modelOf(pose, joint).q), parentQ = quatOf(modelOf(pose, parent).q);
    writeLocal(locals, joint, qmul(conj(parentQ), qmul(delta, now)), 1);
}

// turnJoint, remembering the joint and what it now holds.
void turnRecorded(BodyState& st, void* pose, uint8_t* skeleton, Xf* locals, int joint, const Quat& delta) {
    turnJoint(pose, skeleton, locals, joint, delta);
    int w = 0;
    while (w < st.turnedCount && st.turned[w] != joint) ++w;
    if (w == st.turnedCount) {
        if (w == 8) return;
        ++st.turnedCount;
    }
    st.turned[w] = joint;
    std::memcpy(st.wrote[w], locals[joint].q, sizeof(st.wrote[w]));
}

// A Sim's eyes turn toward `you` (model space) from wherever its head points,
// within the limits the player's eyes have (solveEyes), by `weight`.
void lookWithEyes(BodyState& st, void* pose, uint8_t* skeleton, Xf* locals, const SimRig& r, Vec3 you,
                  float weight) {
    if (!finite(you)) return;
    Quat headQ = quatOf(modelOf(pose, r.head).q);
    Vec3 faceF = normalize(rotate(headQ, r.headForward)), faceU = normalize(rotate(headQ, r.headUp));
    Vec3 faceS = normalize(cross(faceU, faceF));
    for (int i = 0; i < 2; ++i) {
        Xf now = modelOf(pose, r.eyes[i]);
        Vec3 to = sub(you, vecOf(now.p));
        float d = length(to);
        if (!(d > 1e-3f)) continue;
        to = mul(to, 1 / d);
        float f = dot(to, faceF), u = dot(to, faceU), sd = dot(to, faceS);
        float yaw = std::atan2(sd, f), pitch = std::atan2(u, std::sqrt(f * f + sd * sd));
        yaw = yaw > kEyeSide ? kEyeSide : yaw < -kEyeSide ? -kEyeSide : yaw;
        pitch = pitch > kEyeUp ? kEyeUp : pitch < -kEyeDown ? -kEyeDown : pitch;
        Vec3 flat = add(mul(faceF, std::cos(yaw)), mul(faceS, std::sin(yaw)));
        to = normalize(add(mul(flat, std::cos(pitch)), mul(faceU, std::sin(pitch))));
        Vec3 gaze = normalize(rotate(quatOf(now.q), r.eyeForward[i]));
        Quat turn = fromTo(gaze, to);
        if (turn.w < 0) turn = {-turn.x, -turn.y, -turn.z, -turn.w};
        turnRecorded(st, pose, skeleton, locals, r.eyes[i], slerp({0, 0, 0, 1}, turn, weight));
    }
}

// The ends of a part's capsule in the Sim's model space (the head's far end is
// between its eyes).
bool partEnds(const Pose3& p, const SimRig& r, int k, Vec3& a, Vec3& b) {
    if (r.a[k] < 0) return false;
    a = vecOf(modelOf(p.pose, r.a[k]).p);
    if (k == kHeadPart) {
        if (r.eyes[0] < 0 || r.eyes[1] < 0) return false;
        b = mul(add(vecOf(modelOf(p.pose, r.eyes[0]).p), vecOf(modelOf(p.pose, r.eyes[1]).p)), 0.5f);
    } else {
        if (r.b[k] < 0) return false;
        b = vecOf(modelOf(p.pose, r.b[k]).p);
    }
    return finite(a) && finite(b);
}

// Which place on the body a hold at `t` along part k, `off` from its axis
// (model space: +Z the Sim's front, +Y up), is. `axisX`: the axis point's x
// (the Sim's middle is x 0). `size` scales an adult's.
int grabRegion(int k, float t, Vec3 off, float axisX, float size) {
    float front = off.z, side = std::fabs(off.x);
    bool isFront = front > 0.03f * size && front > side * 0.7f;
    bool isBack = front < -0.03f * size && -front > side * 0.7f;
    switch (k) {
        case kHeadPart:
            if (t < 0.15f && off.y < 0) return kRegionNeck;
            return isFront ? kRegionFace : kRegionHair;
        case kTorso:
            if (t > 0.88f) return side > 0.08f * size ? kRegionShoulder : kRegionNeck;
            if (t > 0.55f) return isFront ? kRegionChest : isBack ? kRegionBack : t > 0.75f ? kRegionShoulder : kRegionWaist;
            if (t > 0.28f) return isFront ? kRegionBelly : isBack ? kRegionBack : kRegionWaist;
            if (isBack) return kRegionButt;
            if (isFront) return side < 0.07f * size ? kRegionCrotch : kRegionHips;
            return kRegionHips;
        case kUpperArmL:
        case kUpperArmR: return t < 0.2f ? kRegionShoulder : kRegionUpperArm;
        case kForearmL:
        case kForearmR: return kRegionForearm;
        case kThighL:
        case kThighR: {
            // The inside of a thigh faces the other leg: toward the Sim's middle (x 0).
            if (t < 0.18f && isBack) return kRegionButt;
            bool inside = off.x * axisX < 0;
            if (t < 0.2f && isFront && inside) return kRegionCrotch;
            return inside && !isBack && side > 0.02f * size ? kRegionInnerThigh : kRegionThigh;
        }
        default: return kRegionLeg;
    }
}

void touchOther(uint8_t* actor, uint8_t* pose) {
    Frame frame{};
    ULONGLONG now = GetTickCount64();
    if (!g_fkVerified.load(std::memory_order_relaxed) || !g_frame.read(frame) || !frame.actor ||
        ageMs(now, frame.tick) > 300 || !(frame.scale > 0))
        return;
    Pose3 p{pose, {}};
    std::memcpy(p.world.p, actor + kActorPosition, sizeof(p.world.p));
    std::memcpy(p.world.q, actor + kActorRotation, sizeof(p.world.q));
    std::memcpy(p.world.s, actor + kActorScale, sizeof(p.world.s));
    if (!(p.world.s[0] > 1e-3f && p.world.s[1] > 1e-3f && p.world.s[2] > 1e-3f) || !finite(vecOf(p.world.p))) return;
    const float sc = frame.scale;
    Vec3 eye = vecOf(frame.eye), root = vecOf(p.world.p);
    Vec3 away = sub(root, eye);
    if (dot(away, away) > 9.0f * sc * sc) {
        for (int i = 0; i < 2; ++i)
            if (g_holdActor[i].load(std::memory_order_relaxed) == actor) letGo(i, "too far away");
        for (int i = 0; i < 2; ++i)
            if (g_grabActor[i].load(std::memory_order_relaxed) == actor) releaseGrab(i, "too far away");
        return;
    }
    uint8_t* rig = *reinterpret_cast<uint8_t**>(pose + kPoseRig);
    Xf* locals = *reinterpret_cast<Xf**>(pose + kPoseLocals);
    uint32_t poseCount = *reinterpret_cast<uint16_t*>(pose + kPoseJointCount);
    if (!rig || !locals) return;
    uint8_t* skeleton = rig + kRigSkeleton;
    uint32_t count = g_jointCount(skeleton);
    if (count > poseCount) count = poseCount;
    if (!count || count > kMaxJoints) return;
    SimRig r;
    if (!rigFor(rig, count, r) || !r.sim) return;
    BodySlot* slot = slotFor(actor, now);
    if (!slot || slot->busy.test_and_set(std::memory_order_acquire)) return;
    BodyState& st = slot->state;
    // The give and the look turn joints on top of this frame's pose. If a joint
    // still holds what was written last time, the game didn't pose the Sim
    // again: turning it again would stack up, so this time nothing turns.
    bool fresh = true;
    for (int w = 0; w < st.turnedCount; ++w)
        if (st.turned[w] < static_cast<int>(count) &&
            std::memcmp(locals[st.turned[w]].q, st.wrote[w], sizeof(st.wrote[w])) == 0)
            fresh = false;
    if (fresh) st.turnedCount = 0;

    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float dt = st.last.QuadPart ? float(t.QuadPart - st.last.QuadPart) / float(g_freq.QuadPart) : 0;
    st.last = t;
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;

    float size = length(sub(p.joint(r.head), p.joint(r.pelvis))) / kAdultTorso;
    size = size < 0.3f ? 0.3f : size > 2.0f ? 2.0f : size;
    BodyShape before{};
    buildShape(p, r, size, before);

    // Your hands: palm and fingertips, in the world.
    Hands hands{};
    bool handsOk = g_hands.read(hands) && ageMs(now, hands.tick) < 300;
    Vec3 palm[2], tip[2];
    Quat grip[2];
    bool use[2] = {false, false};
    for (int i = 0; i < 2 && handsOk; ++i) {
        const Hand& h = hands.hand[i];
        if (!h.valid || h.tracked) continue;  // a tracked hand (looks only) touches no Sim
        grip[i] = qmul(frame.heading, qmul(h.orientation, axisAngle({1, 0, 0}, -g_handPitch)));
        palm[i] = add(vecOf(frame.anchor), rotate(frame.heading, mul(h.position, sc)));
        tip[i] = add(palm[i], rotate(grip[i], {0, 0, -kFingerReach * sc}));
        use[i] = true;
    }

    // Squeezing a part of its body (grip and trigger together) holds it.
    // Near one of its hands that's holding hands (below), which the grip alone
    // starts.
    Vec3 pull[2];
    bool pulling[2] = {false, false};
    for (int i = 0; i < 2 && handsOk && g_grabBodies; ++i) {
        void* holder = g_grabActor[i].load(std::memory_order_relaxed);
        if (holder && holder != actor) continue;
        if (!holder) {
            if (!use[i] || !hands.squeezing[i] || ageMs(now, hands.squeezedAt[i]) > 250 ||
                g_holding[i].load(std::memory_order_relaxed) || g_holdActor[i].load(std::memory_order_relaxed))
                continue;
            bool nearHand = false;
            for (int jh = 0; jh < 2; ++jh)
                if (r.arm[jh].ok && length(sub(simPalm(p, r.arm[jh], size), palm[i])) < 0.13f * sc) nearHand = true;
            if (nearHand) continue;
            int best = -1;
            float bestGap = kGrabSurface * sc;
            Vec3 bestAt{};
            for (int k = 0; k < kParts; ++k) {
                if (!(before.valid & (1u << k))) continue;
                const Capsule& c = before.part[k];
                Vec3 q = closestOnSegment(palm[i], c.a, c.b);
                float gap = length(sub(palm[i], q)) - c.r;
                if (gap < bestGap) {
                    bestGap = gap;
                    best = k;
                    // The place is on its surface, under your palm.
                    Vec3 outward = sub(palm[i], q);
                    float ol = length(outward);
                    bestAt = ol > 1e-4f ? add(q, mul(outward, c.r / ol)) : palm[i];
                }
            }
            Vec3 a, b;
            if (best < 0 || !partEnds(p, r, best, a, b)) continue;
            Vec3 m = p.toModel(bestAt), ab = sub(b, a);
            float len2 = dot(ab, ab);
            float t = len2 > 1e-10f ? dot(sub(m, a), ab) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            Vec3 offset = sub(m, add(a, mul(ab, t)));
            int region = grabRegion(best, t, offset, a.x + ab.x * t, size);
            if (restricted(actor) && !allowedOnRestricted(region)) continue;  // nothing there to hold
            void* expected = nullptr;
            if (!g_grabActor[i].compare_exchange_strong(expected, actor)) continue;
            BodyGrab& g = g_grab[i];
            g.part = best;
            g.t = t;
            g.offset = offset;
            g.region = region;
            // Breasts and butt: Integrated Body Physics holds them with its own settings.
            g.pull = !((g.region == kRegionChest || g.region == kRegionButt) && othermods::loaded(othermods::kBodyPhysics));
            g.yanked = false;
            g.started = false;
            g_grabRegion[i] = g.region;
            g_grabAt[i] = now;
            LOG("Touch: your %s hand holds a Sim's %s (%s, %.2f along, off %.2f %.2f %.2f)", i ? "right" : "left",
                kGrabRegionNames[g.region], kPartNames[best], t, g.offset.x, g.offset.y, g.offset.z);
            pushTouch(1, i, actor, g.region, 0);
            holder = actor;
        }
        // Where the held place is now, and how far your hand pulls it.
        BodyGrab& g = g_grab[i];
        Vec3 a, b;
        if (!partEnds(p, r, g.part, a, b)) continue;
        Vec3 placeW = p.toWorld(add(add(a, mul(sub(b, a), g.t)), g.offset));
        Vec3 d = sub(palm[i], placeW);
        float dist = length(d);
        // Your hand stays on the held place as it is dragged along: free to
        // move within how far the part can be dragged (grab_drag; a part that
        // can't turn, like the legs, not at all), held at that reach beyond.
        // Breasts and butt under Claude Jiggle: your hand is left alone.
        if (g.pull) {
            float reach = r.turn[g.part] >= 0 ? g_grabDrag.load(std::memory_order_relaxed) * size * sc : 0;
            Vec3 target = dist > reach && dist > 1e-5f ? add(placeW, mul(d, reach / dist)) : palm[i];
            if (!g.started) {
                g.drawn = target;
                g.started = true;
            }
            g.drawn = add(g.drawn, mul(sub(target, g.drawn), ease(25.0f, dt)));
            if (g_grabActor[i].load(std::memory_order_relaxed) == actor) g_grabPlace[i].write(GrabPlace{true, g.drawn, now});
        }
        if (dist > kGrabSlip * sc) {
            releaseGrab(i, "slipped out");
            continue;
        }
        if (!g_grab[i].yanked && dist > kGrabYank * sc) {
            g_grab[i].yanked = true;
            LOG("Touch: your %s hand pulls hard on a Sim's %s (%.2f m)", i ? "right" : "left",
                kGrabRegionNames[g.region], dist / sc);
            pushTouch(3, i, actor, g.region, dist / sc);
        }
        if (g.pull && dist > 0.005f * sc) {
            pull[i] = d;
            pulling[i] = true;
        }
    }

    // Pushes: each part gives toward where your hand pushes it.
    float pen[kParts] = {};
    Vec3 push[kParts], at[kParts];
    for (int k = 0; k < kParts && g_touchBodies; ++k) {
        if (!(before.valid & (1u << k))) continue;
        const Capsule& c = before.part[k];
        for (int i = 0; i < 2; ++i) {
            if (!use[i] || g_holdActor[i].load(std::memory_order_relaxed) == actor ||
                g_grabActor[i].load(std::memory_order_relaxed) == actor)
                continue;
            for (Vec3 point : {palm[i], tip[i]}) {
                Vec3 q = closestOnSegment(point, c.a, c.b);
                Vec3 out = sub(q, point);
                float d = length(out), depth = c.r + kHandRadius * sc - d;
                if (depth > pen[k] && d > 1e-5f) {
                    pen[k] = depth;
                    push[k] = mul(out, 1 / d);
                    at[k] = q;
                }
            }
        }
    }
    // A held part is pulled toward your hand like a push, as far as grab_drag.
    bool dragged[kParts] = {};
    for (int i = 0; i < 2; ++i) {
        if (!pulling[i]) continue;
        int k = g_grab[i].part;
        float d = length(pull[i]);
        if (d > pen[k]) {
            pen[k] = d;
            push[k] = mul(pull[i], 1 / d);
            dragged[k] = true;
        }
    }
    // The part turns at its joint so that its far end (the neck, an elbow, a
    // hand, the eyes) moves along the push, by up to a set amount and angle.
    static std::atomic<int> pushLogs{0};
    const float kMaxGive = 0.3f;  // radians
    for (int k = 0; k < kParts; ++k) {
        int j = r.turn[k];
        const float give = dragged[k] ? g_grabDrag.load(std::memory_order_relaxed) : kPartGive[k];
        bool pushed = pen[k] > 0 && give > 0 && j >= 0;
        Quat target{0, 0, 0, 1};
        if (pushed) {
            float move = pen[k] < give * size ? pen[k] : give * size;
            Vec3 jp = p.joint(j), end;
            if (r.lever[k] >= 0)
                end = p.joint(r.lever[k]);
            else if (r.eyes[0] >= 0 && r.eyes[1] >= 0)
                end = mul(add(p.joint(r.eyes[0]), p.joint(r.eyes[1])), 0.5f);
            else
                end = p.joint(r.head);
            Vec3 lever = sub(end, jp);
            float len = length(lever);
            Vec3 across = len > 1e-3f ? sub(push[k], mul(lever, dot(push[k], lever) / (len * len))) : Vec3{0, 0, 0};
            float acrossLen = length(across);
            pushed = len > 0.02f * sc && acrossLen > 0.2f;
            if (pushed) {
                float angle = std::atan2(move * acrossLen, len);
                const float maxAngle = dragged[k] ? 0.7f : kMaxGive;
                if (angle > maxAngle) angle = maxAngle;
                Vec3 moved = add(jp, add(mul(lever, std::cos(angle)), mul(across, len * std::sin(angle) / acrossLen)));
                target = fromTo(sub(p.toModel(end), p.toModel(jp)), sub(p.toModel(moved), p.toModel(jp)));
                if (pushLogs.fetch_add(1) < 12)
                    LOG("Touch: your hand pushes a Sim's %s (%.0f mm deep, turns %.0f degrees)", kPartNames[k],
                        pen[k] * 1000 / sc, angle * 57.2958f);
            }
        }
        st.give[k] = qnormalize(slerp(st.give[k], target, ease(pushed ? 14.0f : 5.0f, dt)));
    }
    // Spine first, then neck, then arms: each turn takes its children along.
    for (int k : {kTorso, kHeadPart, kUpperArmL, kUpperArmR, kForearmL, kForearmR}) {
        if (fresh && r.turn[k] >= 0 && st.give[k].w < 0.99999f)
            turnRecorded(st, pose, skeleton, locals, r.turn[k], st.give[k]);
    }

    // Holding hands. Without fresh hand data this time, holds stay as they were.
    bool held[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        void* holder = g_holdActor[i].load(std::memory_order_relaxed);
        if (holder && holder != actor) continue;
        if (!handsOk) {
            if (holder) held[g_holdHand[i].load()] = true;
            continue;
        }
        const Hand& h = hands.hand[i];
        if (!holder) {
            bool closing = g_holdHands && use[i] && h.grip > 0.55f && ageMs(now, hands.closedAt[i]) < 250 &&
                           !g_holding[i].load(std::memory_order_relaxed) && !g_grabActor[i].load(std::memory_order_relaxed);
            if (!closing) continue;
            int best = -1;
            float bestD = 0.13f * sc;
            for (int jh = 0; jh < 2; ++jh) {
                if (!r.arm[jh].ok) continue;
                int other = 1 - i;
                if (g_holdActor[other].load() == actor && g_holdHand[other].load() == jh) continue;
                float d = length(sub(simPalm(p, r.arm[jh], size), palm[i]));
                if (d < bestD) {
                    bestD = d;
                    best = jh;
                }
            }
            if (best < 0) continue;
            void* expected = nullptr;
            if (!g_holdActor[i].compare_exchange_strong(expected, actor)) continue;
            // Face to face (a handshake, leading from in front), its fingers
            // point back along yours; side by side, the same way.
            Vec3 fingers = rotate(grip[i], {0, 0, -1});
            bool facing = dot(fingers, sub(palm[i], p.joint(r.arm[best].upper))) < 0;
            g_holdHand[i] = best;
            g_holdFacing[i] = facing;
            g_holdAt[i] = now;
            pushTouch(1, i, actor, kRegionHand, 0);
            LOG("Touch: your %s hand holds a Sim's %s hand (%s)", i ? "right" : "left", best ? "right" : "left",
                facing ? "face to face" : "side by side");
            holder = actor;
        }
        if (!use[i] || h.grip < 0.35f) {
            letGo(i, "hand opened");
            continue;
        }
        int jh = g_holdHand[i].load();
        const Arm& arm = r.arm[jh];
        if (!arm.ok) {
            letGo(i, "no arm");
            continue;
        }
        // Palm to palm: its fingers along yours (or back along them, face to
        // face), its palm facing yours.
        float sOur = i == 0 ? 1.0f : -1.0f, sTheir = jh == 0 ? 1.0f : -1.0f;
        Vec3 normal = rotate(grip[i], {sOur, 0, 0});
        Vec3 forward = rotate(grip[i], {0, 0, -1});
        Vec3 theirForward = g_holdFacing[i].load() ? mul(forward, -1) : forward;
        Vec3 x = mul(normal, -sTheir);
        Vec3 z = mul(theirForward, -1);
        Vec3 y = normalize(cross(z, x));
        Quat control = fromBasis(x, y, z);
        Vec3 theirPalm = add(palm[i], mul(normal, 0.03f * sc));
        Vec3 wrist = add(theirPalm, rotate(control, {-sTheir * 0.02f * sc, 0, 0.08f * sc}));
        Vec3 shoulder = p.joint(arm.upper);
        float reach = (length(sub(p.joint(arm.fore), shoulder)) + length(sub(p.joint(arm.hand), p.joint(arm.fore))));
        if (length(sub(wrist, shoulder)) > 1.35f * reach) {
            letGo(i, "out of reach");
            continue;
        }
        st.holdWrist[jh] = wrist;
        st.holdControl[jh] = control;
        held[jh] = true;
    }
    for (int jh = 0; jh < 2; ++jh) {
        st.holdWeight[jh] += ((held[jh] ? 1.0f : 0.0f) - st.holdWeight[jh]) * ease(held[jh] ? 12.0f : 8.0f, dt);
        if (!held[jh] && st.holdWeight[jh] < 0.01f) st.holdWeight[jh] = 0;
        if (st.holdWeight[jh] <= 0 || !r.arm[jh].ok) continue;
        if (reachArm(pose, skeleton, locals, r.arm[jh], &r.arm[1 - jh], st.holdWrist[jh], st.holdControl[jh], p.world,
                     st.holdWeight[jh], false, "")) {
            Side fingers;
            fingers.index = 0.55f;
            fingers.grip = 0.6f;
            fingers.thumb = 0.5f;
            poseFingers(skeleton, locals, r.arm[jh], fingers, st.holdWeight[jh]);
        }
    }

    // Lean in close in front of a Sim, or talk with one that looks your way,
    // and it looks you in the eyes. Aimed from its eyes, not its head joint:
    // that sits below and behind them, so the face pointed over your eyes, at
    // your hair (user 2026-10-19 01:29Z). Its eyes turn the rest of the way.
    if (r.headOk) {
        Xf hm = modelOf(pose, r.head);
        Vec3 fromM = vecOf(hm.p);
        if (r.eyes[0] >= 0 && r.eyes[1] >= 0)
            fromM = mul(add(vecOf(modelOf(pose, r.eyes[0]).p), vecOf(modelOf(pose, r.eyes[1]).p)), 0.5f);
        Vec3 toYou = sub(eye, p.toWorld(fromM));
        float d = length(toYou);
        Vec3 face = rotate(quatOf(p.world.q), rotate(quatOf(hm.q), r.headForward));
        float facing = d > 1e-3f ? dot(face, mul(toYou, 1 / d)) : 0;
        constexpr float kLookTalk = 2.0f;  // metres: a Sim talking with you, already facing you
        bool want = g_lookAt && d > 1e-3f &&
                    ((d < 1.0f * sc && facing > 0.2f) || (d < kLookTalk * sc && facing > 0.6f));
        st.look += ((want ? 1.0f : 0.0f) - st.look) * ease(3.0f, dt);
        if (!want && st.look < 0.01f) st.look = 0;
        if (fresh && st.look > 0 && d > 1e-3f) {
            Vec3 dirM = normalize(rotate(conj(quatOf(p.world.q)), toYou));
            Vec3 faceM = rotate(quatOf(hm.q), r.headForward);
            Quat delta = fromTo(faceM, dirM);
            if (delta.w < 0) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
            float angle = 2 * std::acos(delta.w > 1 ? 1 : delta.w);
            const float kMax = 0.9f;
            if (angle > kMax) delta = slerp({0, 0, 0, 1}, delta, kMax / angle);
            delta = slerp({0, 0, 0, 1}, delta, st.look);
            if (r.neck >= 0 && r.neck != r.head) {
                turnRecorded(st, pose, skeleton, locals, r.neck, slerp({0, 0, 0, 1}, delta, 0.4f));
                Quat headNow = quatOf(modelOf(pose, r.head).q);
                // The rest at the head, so it ends up turned by the whole delta.
                Quat rest = qmul(qmul(delta, quatOf(hm.q)), conj(headNow));
                turnRecorded(st, pose, skeleton, locals, r.head, rest);
            } else {
                turnRecorded(st, pose, skeleton, locals, r.head, delta);
            }
            if (r.eyesOk) lookWithEyes(st, pose, skeleton, locals, r, p.toModel(eye), st.look);
        }
    }

    BodyShape after{};
    after.actor = actor;
    after.tick = now;
    after.root = root;
    buildShape(p, r, size, after);
    slot->shape.write(after);
    slot->busy.clear(std::memory_order_release);
}

float keepOutOfBodies(int sideIndex, Vec3& palm, Vec3& wrist, const Quat& gripWorld, float scale) {
    if (!g_touchBodies) return 0;
    void* self = g_player.load(std::memory_order_relaxed);
    void* held = g_holdActor[sideIndex].load(std::memory_order_relaxed);
    void* grabbed = g_grabActor[sideIndex].load(std::memory_order_relaxed);
    ULONGLONG now = GetTickCount64();
    float radius = kHandRadius * scale, deepest = 0;
    Vec3 reach = rotate(gripWorld, {0, 0, -kFingerReach * scale});
    BodyShape shapes[kMaxBodies];
    int n = 0;
    for (BodySlot& slot : g_bodies) {
        void* actor = slot.actor.load(std::memory_order_relaxed);
        if (!actor || actor == self || actor == held || actor == grabbed) continue;
        if (slot.shape.read(shapes[n]) && shapes[n].actor == actor && ageMs(now, shapes[n].tick) < 200) ++n;
    }
    for (int iter = 0; iter < 2 && n; ++iter) {
        float best = 0;
        Vec3 push{0, 0, 0};
        for (int b = 0; b < n; ++b) {
            for (int k = 0; k < kParts; ++k) {
                if (!(shapes[b].valid & (1u << k))) continue;
                const Capsule& c = shapes[b].part[k];
                for (Vec3 point : {palm, add(palm, reach)}) {
                    Vec3 q = closestOnSegment(point, c.a, c.b);
                    Vec3 out = sub(point, q);
                    float d = length(out), depth = c.r + radius - d;
                    if (depth > best && d > 1e-5f) {
                        best = depth;
                        push = mul(out, depth / d);
                    }
                }
            }
        }
        if (best <= 0) break;
        palm = add(palm, push);
        wrist = add(wrist, push);
        if (iter == 0) deepest = best;
    }
    return deepest;
}

// The Sim was posed near you in the last moments.
bool posedLately(void* actor, ULONGLONG now) {
    for (BodySlot& slot : g_bodies)
        if (slot.actor.load(std::memory_order_relaxed) == actor)
            return ageMs(now, slot.seen.load(std::memory_order_relaxed)) < 400;
    return false;
}

// ---------------------------------------------------------------------------
// Your voice on your Sim: its jaw opens as you talk (voice.cpp), in god cam
// as in first person, on top of whatever the game's animation does.

std::atomic<void*> g_voiceActor{nullptr};
std::atomic<float> g_mouth{0.0f};
std::atomic<bool> g_mouthBusy{false};
struct MouthRig {
    void* rig = nullptr;
    int jaw = -1;
    Vec3 axis{1, 0, 0};  // the model's left-right axis in the jaw's parent frame: turning about it drops the chin
};
MouthRig g_mouthRig;

void poseMouth(uint8_t* pose, float angle) {
    uint8_t* rig = *reinterpret_cast<uint8_t**>(pose + kPoseRig);
    Xf* locals = *reinterpret_cast<Xf**>(pose + kPoseLocals);
    uint32_t poseCount = *reinterpret_cast<uint16_t*>(pose + kPoseJointCount);
    if (!rig || !locals) return;
    uint8_t* skeleton = rig + kRigSkeleton;
    uint32_t count = g_jointCount(skeleton);
    if (count > poseCount) count = poseCount;
    if (!count || count > kMaxJoints) return;
    if (rig != g_mouthRig.rig) {
        g_mouthRig = MouthRig{};
        g_mouthRig.rig = rig;
        uint32_t jaw = g_jointIndex(skeleton, jointHash("b__Jaw__"));
        const Xf* bind = *reinterpret_cast<Xf* const*>(rig + kRigBindLocals);
        int parent = jaw < count ? g_parent(skeleton, jaw) : -1;
        Vec3 p;
        Quat q;
        // In the bind pose the Sim faces the model's +Z with +Y up, so a
        // turn about +X takes the chin down and back.
        if (parent >= 0 && bind && bindInFrame(skeleton, bind, -1, parent, p, q)) {
            g_mouthRig.jaw = static_cast<int>(jaw);
            g_mouthRig.axis = normalize(rotate(conj(q), {1, 0, 0}));
        }
        static int logged = 0;
        if (logged++ < 10)
            LOG("Voice: jaw joint %d%s (opens about %.2f %.2f %.2f in its parent's frame)", g_mouthRig.jaw,
                g_mouthRig.jaw >= 0 ? "" : " NOT found: the mouth can't move", g_mouthRig.axis.x, g_mouthRig.axis.y,
                g_mouthRig.axis.z);
    }
    if (g_mouthRig.jaw < 0) return;
    Quat turned = qnormalize(qmul(axisAngle(g_mouthRig.axis, angle), quatOf(locals[g_mouthRig.jaw].q)));
    if (std::isfinite(turned.x) && std::isfinite(turned.w)) store(locals[g_mouthRig.jaw].q, turned);
}

// Stairs check (2026-10-14): on the steps the body was drawn at the bottom
// floor's height while the joints said it was at your eyes. Where the actor is
// when the game poses it, against where it is pinned: a difference over 5 cm
// (once a second, 40 times), and now and then a line either way.
void checkPosedPlace(uint8_t* actor) {
    float pin[7];
    if (!player::pinPlace(pin)) return;
    static ULONGLONG last = 0;
    static int offLogs = 0, checks = 0, sameLogs = 0;
    float p[3], q[3];
    std::memcpy(p, actor + kActorPosition, sizeof(p));
    std::memcpy(q, actor + kActorSecondPosition, sizeof(q));
    float dx = p[0] - pin[0], dy = p[1] - pin[1], dz = p[2] - pin[2];
    bool off = dx * dx + dy * dy + dz * dz > 0.05f * 0.05f;
    ++checks;
    ULONGLONG now = GetTickCount64();
    if (off ? (offLogs >= 40 || now - last < 1000) : (sameLogs >= 5 || checks % 600 != 1)) return;
    last = now;
    ++(off ? offLogs : sameLogs);
    LOG("Arm IK: the body is posed at %.2f %.2f %.2f, pinned at %.2f %.2f %.2f (off by %.2f %.2f %.2f); its second "
        "transform at %.2f %.2f %.2f",
        p[0], p[1], p[2], pin[0], pin[1], pin[2], dx, dy, dz, q[0], q[1], q[2]);
}

// Right after the game made the player's model-space joints: the hands as
// drawn (palm and every finger joint, plus each fingertip) for handshape, in
// app space, with the controller poses the solve used.
void captureShape(uint8_t* pose) {
    SolveContext& c = t_solved;
    if (!c.ok) return;
    c.ok = false;
    uint8_t* rig = *reinterpret_cast<uint8_t**>(pose + kPoseRig);
    const Xf* locals = *reinterpret_cast<Xf* const*>(pose + kPoseLocals);
    if (!rig || rig != g_cache.rig || !locals || !g_cache.fkOk || !(c.frame.scale > 0)) return;
    const Quat actorQ = quatOf(c.world.q), back = conj(c.frame.heading);
    const float inv = 1 / c.frame.scale;
    auto toApp = [&](const float* m) {
        Vec3 w = add(vecOf(c.world.p), rotate(actorQ, {m[0] * c.world.s[0], m[1] * c.world.s[1], m[2] * c.world.s[2]}));
        return mul(rotate(back, sub(w, vecOf(c.frame.anchor))), inv);
    };
    for (int i = 0; i < 2; ++i) {
        const Arm& arm = g_cache.arm[i];
        const Hand& h = c.hand[i];
        if (!arm.ok || !arm.frame || !h.valid || g_side[i].weight < 0.5f) continue;
        handshape::Raw r;
        r.tracked = h.tracked;
        r.gripPos = h.position;
        r.gripRot = h.orientation;
        Xf hx = modelOf(pose, arm.hand);
        r.wrist = toApp(hx.p);
        r.handRot = qnormalize(qmul(back, qmul(actorQ, quatOf(hx.q))));
        r.forward = arm.forward;
        r.thumbSide = arm.side;
        r.palmNormal = mul(arm.up, i ? -1.0f : 1.0f);  // the left palm faces +up, the right one -up (resolveArm)
        bool ok = finite(r.wrist);
        for (int k = 0; k < arm.fingerCount; ++k) {
            const Finger& finger = arm.fingers[k];
            const int f = finger.name;
            int n = 0;
            Xf m{};
            for (int j = 0; j < finger.count && j < 3; ++j) {
                m = modelOf(pose, finger.joints[j]);
                r.joint[f][n++] = toApp(m.p);
            }
            if (!n) continue;
            // The tip: past the last joint by most of the segment before it, as tipOf.
            Vec3 v = mul(vecOf(locals[finger.joints[finger.count - 1]].p), 0.8f);
            Vec3 t = add(vecOf(m.p), rotate(quatOf(m.q), {m.s[0] * v.x, m.s[1] * v.y, m.s[2] * v.z}));
            float tm[3] = {t.x, t.y, t.z};
            r.joint[f][n++] = toApp(tm);
            r.jointCount[f] = n;
            for (int j = 0; j < n; ++j) ok = ok && finite(r.joint[f][j]);
        }
        if (ok) handshape::publish(i, r);
    }
}

void hookedPoseToJoints(void* actor, void* pose) {
    if (pose && actor && actor == g_voiceActor.load(std::memory_order_relaxed)) {
        float angle = g_mouth.load(std::memory_order_relaxed);
        if (angle > 0.002f && !g_mouthBusy.exchange(true, std::memory_order_acquire)) {
            poseMouth(static_cast<uint8_t*>(pose), angle);
            g_mouthBusy.store(false, std::memory_order_release);
        }
    }
    void* player = g_player.load(std::memory_order_relaxed);
    if (pose && player) {
        if (actor == player) {
            checkPosedPlace(static_cast<uint8_t*>(actor));
            if (!g_busy.exchange(true, std::memory_order_acquire)) {
                solve(static_cast<uint8_t*>(actor), static_cast<uint8_t*>(pose));
                g_busy.store(false, std::memory_order_release);
            }
            g_original(actor, pose);
            // The hands as finished (handshape); t_solved is this thread's own,
            // g_cache is shared with solves on other threads.
            if (!g_busy.exchange(true, std::memory_order_acquire)) {
                captureShape(static_cast<uint8_t*>(pose));
                g_busy.store(false, std::memory_order_release);
            }
            return;
        } else if (g_touchBodies || g_holdHands || g_lookAt) {
            touchOther(static_cast<uint8_t*>(actor), static_cast<uint8_t*>(pose));
        }
    }
    g_original(actor, pose);
}

}  // namespace

void install(bool enabled, float handPitchDegrees, bool headTracking) {
    g_handPitch = handPitchDegrees * 3.14159265f / 180.0f;
    g_headTracking = headTracking;
    if (!enabled) {
        LOG("Arm IK: off in sims4vr.ini");
        return;
    }
    for (const char* k : {"PoseToJoints", "ModelTransform", "JointIndex", "JointCount", "Parent"}) {
        if (!addrs::fn(k)) {
            LOG("Arm IK: game build not recognised (no match for %s); off", k);
            return;
        }
    }
    g_model = reinterpret_cast<ModelTransformFn>(addrs::fn("ModelTransform"));
    g_jointIndex = reinterpret_cast<JointIndexFn>(addrs::fn("JointIndex"));
    g_jointCount = reinterpret_cast<JointCountFn>(addrs::fn("JointCount"));
    g_parent = reinterpret_cast<ParentFn>(addrs::fn("Parent"));
    void* target = addrs::fn("PoseToJoints");
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedPoseToJoints), reinterpret_cast<void**>(&g_original)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Arm IK: hook failed; off");
        return;
    }
    g_enabled = true;
    LOG("Arm IK: on (hand pitch %.0f degrees, head tracking %s)", handPitchDegrees, headTracking ? "on" : "off");
}

void setHands(const Hand hands[2]) {
    if (!g_enabled) return;
    static bool open[2] = {true, true};
    static ULONGLONG closedAt[2] = {0, 0};
    static bool squeezed[2] = {false, false};
    static ULONGLONG squeezedAt[2] = {0, 0};
    Hands h{};
    h.tick = GetTickCount64();
    for (int i = 0; i < 2; ++i) {
        h.hand[i] = hands[i];
        bool valid = hands[i].valid;
        if (valid && open[i] && hands[i].grip > 0.55f) {
            open[i] = false;
            closedAt[i] = h.tick;
        } else if (!valid || hands[i].grip < 0.35f) {
            open[i] = true;
        }
        h.closedAt[i] = closedAt[i];
        // Squeezing a body: grip and trigger both past kGrabPress (the same
        // press Claude Jiggle takes for breasts); held until either drops
        // below kGrabKeep.
        bool both = valid && hands[i].grip > kGrabPress && hands[i].trigger > kGrabPress;
        bool keep = valid && hands[i].grip > kGrabKeep && hands[i].trigger > kGrabKeep;
        if (both && !squeezed[i]) {
            squeezed[i] = true;
            squeezedAt[i] = h.tick;
        } else if (!keep) {
            squeezed[i] = false;
        }
        h.squeezedAt[i] = squeezedAt[i];
        h.squeezing[i] = squeezed[i];
        void* grabbed = g_grabActor[i].load(std::memory_order_relaxed);
        if (grabbed && !squeezed[i])
            releaseGrab(i, valid ? "hand opened" : "hand lost");
        else if (grabbed && !posedLately(grabbed, h.tick))
            releaseGrab(i, "the Sim is gone");
        void* held = g_holdActor[i].load(std::memory_order_relaxed);
        if (held && (!valid || hands[i].grip < 0.35f))
            letGo(i, valid ? "hand opened" : "hand lost");
        else if (held && !posedLately(held, h.tick))
            letGo(i, "the Sim is gone");
    }
    g_hands.write(h);
}

void setHoldPose(int side, bool on, float fingerCurl, float thumbBend, float thumbSpread, const Vec3& move,
                 const Quat& turn) {
    if (side < 0 || side > 1) return;
    g_holdPose[side].write(HoldPose{on, fingerCurl, thumbBend, thumbSpread, move, turn});
}

void setPointing(int side, bool on, const Vec3& from, const Vec3& dir) {
    if (side < 0 || side > 1) return;
    g_point[side].write(Point{on, from, dir, GetTickCount64()});
}

bool fingertip(int side, Vec3& grip) {
    if (side < 0 || side > 1) return false;
    Tip t;
    if (!g_tip[side].read(t) || !t.valid || GetTickCount64() - t.tick >= 300) return false;
    grip = t.grip;
    return true;
}

bool handPose(int side, Vec3& wrist, Quat& grip, float& scale) {
    if (side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lock(g_handPoseMutex);
    const HandPose& h = g_handPose[side];
    wrist = h.wrist;
    grip = h.grip;
    scale = h.scale;
    return h.valid && GetTickCount64() - h.tick < 300;
}

bool handAxes(int side, Vec3& forward, Vec3& thumb, Vec3& back) {
    if (side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lock(g_axesMutex);
    forward = g_axes[side][0];
    thumb = g_axes[side][1];
    back = g_axes[side][2];
    return g_axesKnown[side];
}

void palms(Vec3 out[2], bool valid[2]) {
    Hands h{};
    bool ok = g_enabled && g_hands.read(h) && ageMs(GetTickCount64(), h.tick) < 300;
    for (int i = 0; i < 2; ++i) {
        valid[i] = ok && h.hand[i].valid;
        out[i] = h.hand[i].position;
    }
}

float gameCurl(int side) { return side >= 0 && side < 2 ? g_gameCurl[side].load(std::memory_order_relaxed) : 0; }

bool holding(int side) { return side >= 0 && side < 2 && g_holding[side].load(std::memory_order_relaxed); }

void setHolding(bool left, bool right) {
    for (int i = 0; i < 2; ++i) {
        bool now = i == 0 ? left : right;
        if (g_holding[i].exchange(now) != now)
            LOG("Arm IK: %s hand %s", i ? "right" : "left", now ? "holds something: the game's grip" : "free again: your buttons");
    }
}

void setVoiceActor(void* actor) { g_voiceActor.store(actor, std::memory_order_relaxed); }

void setLookTarget(bool on, const float point[3]) {
    LookTarget t{};
    t.on = on && point && std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
    if (t.on) std::memcpy(t.point, point, sizeof(t.point));
    t.tick = GetTickCount64();
    g_lookTarget.write(t);
}
void setMouth(float radians) { g_mouth.store(radians, std::memory_order_relaxed); }

void learnGameGrip(bool on) {
    bool was = g_gripLearnUntil.exchange(on ? GetTickCount64() + kLearnFor : 0, std::memory_order_relaxed) != 0;
    static int logs = 0;
    if (on && !was && logs++ < 20) LOG("Arm IK: watching the game's animation for its grip on the prop");
}

void holdGameGrip(int side, bool on) {
    if (side < 0 || side > 1) return;
    if (g_gripHold[side].exchange(on, std::memory_order_relaxed) != on)
        LOG("Arm IK: %s hand %s", side ? "right" : "left", on ? "holds our prop: the game's grip" : "lets our prop go");
}

bool gameGrip(int side, Vec3& position, Quat& rotation) {
    GripPlace g;
    if (side < 0 || side > 1 || !g_gripPlace[side].read(g) || !g.valid) return false;
    position = g.p;
    rotation = g.q;
    return true;
}

namespace {
std::wstring g_gripPath;
}

void loadGameGrip(const std::wstring& path) {
    g_gripPath = path;
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) {
        LOG("Arm IK: no learned grip yet (sims4vr_grips.ini); the TV remote shows once your Sim has used it");
        return;
    }
    GripData g;
    bool prop = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        int side = 0, finger = 0, joint = 0;
        float v[7];
        if (std::sscanf(line, " side = %d", &side) == 1) {
            g.side = side;
        } else if (std::sscanf(line, " prop = %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) ==
                   7) {
            g.p = {v[0], v[1], v[2]};
            g.q = qnormalize({v[3], v[4], v[5], v[6]});
            prop = true;
        } else if (std::sscanf(line, " finger%d_%d = %f %f %f %f", &finger, &joint, &v[0], &v[1], &v[2], &v[3]) == 6 &&
                   finger >= 0 && finger < 5 && joint >= 0 && joint < 3) {
            g.turn[finger][joint] = qnormalize({v[0], v[1], v[2], v[3]});
            g.finger[finger][joint] = true;
        }
    }
    std::fclose(f);
    g.valid = prop && (g.side == 0 || g.side == 1) && finite(g.p) && std::isfinite(g.q.w);
    if (!g.valid) {
        LOG("Arm IK: sims4vr_grips.ini not readable; the grip is learned again");
        return;
    }
    g_gripLearned.write(g);
    g_gripVersion.fetch_add(1, std::memory_order_release);
    LOG("Arm IK: the game's TV remote grip loaded (learned on the %s hand)", g.side ? "right" : "left");
}

void saveGameGrip() {
    if (!g_gripUnsaved.exchange(false, std::memory_order_acq_rel) || g_gripPath.empty()) return;
    GripData g;
    if (!g_gripLearned.read(g) || !g.valid) return;
    FILE* f = _wfopen(g_gripPath.c_str(), L"w");
    if (!f) {
        LOG("Arm IK: could not save the learned grip");
        return;
    }
    std::fprintf(f,
                 "; Sims4VR: how the game's own TV remote animation holds the remote, learned in first person.\n"
                 "; Delete this file to learn it again the next time your Sim uses a remote.\n[remote]\nside = %d\n"
                 "prop = %.5f %.5f %.5f %.6f %.6f %.6f %.6f\n",
                 g.side, g.p.x, g.p.y, g.p.z, g.q.x, g.q.y, g.q.z, g.q.w);
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 3; ++k)
            if (g.finger[i][k])
                std::fprintf(f, "finger%d_%d = %.6f %.6f %.6f %.6f\n", i, k, g.turn[i][k].x, g.turn[i][k].y,
                             g.turn[i][k].z, g.turn[i][k].w);
    std::fclose(f);
    LOG("Arm IK: saved the learned grip to sims4vr_grips.ini");
}

void setFrame(void* actor, bool free, const float anchor[3], const Quat& heading, float scale, const Quat& hmd,
              const float eye[3]) {
    if (!g_enabled) return;
    if (!actor) {
        letGo(0, "left first person");
        letGo(1, "left first person");
        releaseGrab(0, "left first person");
        releaseGrab(1, "left first person");
    }
    Frame f{};
    f.actor = actor;
    f.free = free;
    std::memcpy(f.anchor, anchor, sizeof(f.anchor));
    std::memcpy(f.eye, eye, sizeof(f.eye));
    f.heading = heading;
    f.hmd = hmd;
    f.scale = scale;
    f.tick = GetTickCount64();
    g_frame.write(f);
    g_player.store(actor, std::memory_order_relaxed);
}

}  // namespace s4vr::armik

namespace s4vr::armik {

void setSeatTorso(bool on) {
    g_seatTorso.store(on, std::memory_order_relaxed);
    LOG("Seated upper body: %s", on ? "your head moves it (the animation's sway left out)" : "the animation moves it");
}

void setLean(const float world[3]) {
    Lean l{{world[0], world[1], world[2]}, GetTickCount64()};
    g_leanWanted.write(l);
}

bool leanDone(camera::Vec3& world) {
    Lean l{};
    if (!g_leanDone.read(l) || ageMs(GetTickCount64(), l.tick) > 300) return false;
    world = {l.world[0], l.world[1], l.world[2]};
    return true;
}

void configureHands(float handPitchDegrees, bool headTracking) {
    g_handPitch = handPitchDegrees * 3.14159265f / 180.0f;
    g_headTracking = headTracking;
}

void configureTouch(bool bodies, bool holdHands, bool lookAt, bool grabBodies) {
    g_touchBodies = bodies;
    g_holdHands = holdHands;
    g_lookAt = lookAt;
    g_grabBodies = grabBodies;
    if (!grabBodies) {
        releaseGrab(0, "switched off");
        releaseGrab(1, "switched off");
    }
    LOG("Touch: other Sims' bodies %s, holding hands %s, looking at you %s, grabbing bodies %s",
        bodies ? "on" : "off", holdHands ? "on" : "off", lookAt ? "on" : "off", grabBodies ? "on" : "off");
}

void setTouchRestricted(void* const* actors, int count) {
    for (int i = 0; i < 16; ++i) g_restricted[i].store(i < count ? actors[i] : nullptr, std::memory_order_relaxed);
}

void configureGrab(float curl, float drag) {
    g_grabCurl = curl < 0 ? 0 : curl > 1 ? 1 : curl;
    g_grabDrag = drag < 0 ? 0 : drag > 0.6f ? 0.6f : drag;
    LOG("Touch: holding a body closes the fingers %.0f%%, drags it up to %.2f m", g_grabCurl.load() * 100,
        g_grabDrag.load());
}

bool takeTouchEvent(TouchEvent& e) {
    AcquireSRWLockExclusive(&g_touchQueue.lock);
    bool any = g_touchQueue.count > 0;
    if (any) {
        e = g_touchQueue.e[g_touchQueue.head];
        g_touchQueue.head = (g_touchQueue.head + 1) % 32;
        --g_touchQueue.count;
    }
    ReleaseSRWLockExclusive(&g_touchQueue.lock);
    return any;
}

float contact(int side) {
    if (side < 0 || side > 1 || ageMs(GetTickCount64(), g_contactTick[side].load(std::memory_order_relaxed)) > 150)
        return 0;
    return g_contact[side].load(std::memory_order_relaxed);
}

bool holdingSim(int side) { return side >= 0 && side < 2 && g_holdActor[side].load(std::memory_order_relaxed); }

int nearbySims(float (*xz)[2], int max) {
    void* self = g_player.load(std::memory_order_relaxed);
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (BodySlot& slot : g_bodies) {
        if (n >= max) break;
        void* actor = slot.actor.load(std::memory_order_relaxed);
        if (!actor || actor == self) continue;
        BodyShape b;
        if (!slot.shape.read(b) || b.actor != actor || ageMs(now, b.tick) > 300) continue;
        xz[n][0] = b.root.x;
        xz[n][1] = b.root.z;
        ++n;
    }
    return n;
}

int nearbyHeads(SimHead* out, int max) {
    void* self = g_player.load(std::memory_order_relaxed);
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (BodySlot& slot : g_bodies) {
        if (n >= max) break;
        void* actor = slot.actor.load(std::memory_order_relaxed);
        if (!actor || actor == self) continue;
        BodyShape b;
        if (!slot.shape.read(b) || b.actor != actor || ageMs(now, b.tick) > 300 || !(b.valid & (1u << kHeadPart)))
            continue;
        const Vec3& eyes = b.part[kHeadPart].b;
        out[n] = {{b.root.x, b.root.y, b.root.z}, {eyes.x, eyes.y, eyes.z}};
        ++n;
    }
    return n;
}

float headOutOfBodies(const float eye[3], float radius, float push[2], bool headAndTorso) {
    constexpr float kChestExtra = 0.05f;  // metres added around the torso for your head
    // World units per metre, as the touch frame has it (1 without one).
    Frame frame{};
    float scale = g_frame.read(frame) && frame.scale > 0 ? frame.scale : 1.0f;
    push[0] = push[1] = 0;
    void* self = g_player.load(std::memory_order_relaxed);
    ULONGLONG now = GetTickCount64();
    BodyShape shapes[kMaxBodies];
    int n = 0;
    for (BodySlot& slot : g_bodies) {
        void* actor = slot.actor.load(std::memory_order_relaxed);
        if (!actor || actor == self) continue;
        if (slot.shape.read(shapes[n]) && shapes[n].actor == actor && ageMs(now, shapes[n].tick) < 200) ++n;
    }
    // Their head, torso, upper arms and thighs: a hand waved while they talk
    // (forearms) or a step (calves) doesn't shove your view about.
    const uint32_t parts = headAndTorso ? (1u << kTorso) | (1u << kHeadPart)
                                        : (1u << kTorso) | (1u << kHeadPart) | (1u << kUpperArmL) |
                                              (1u << kUpperArmR) | (1u << kThighL) | (1u << kThighR);
    Vec3 p{eye[0], eye[1], eye[2]};
    float deepest = 0;
    // A few passes: out of one part can be into the next.
    for (int iter = 0; iter < 3 && n; ++iter) {
        float best = 0;
        float mx = 0, mz = 0;
        for (int b = 0; b < n; ++b) {
            for (int k = 0; k < kParts; ++k) {
                if (!(shapes[b].valid & parts & (1u << k))) continue;
                const Capsule& c = shapes[b].part[k];
                // The torso's capsule follows the spine; a chest (and a
                // bust) stands out past it (user 2026-10-17 14:20Z: in a
                // scene the view still went into a partner's chest).
                float reach = c.r + radius + (k == kTorso ? kChestExtra * scale : 0);
                Vec3 q = closestOnSegment(p, c.a, c.b);
                Vec3 v = sub(p, q);
                float d = length(v);
                if (d >= reach) continue;
                // Sideways only: your view keeps its height. Straight above
                // or below the part, away from its middle instead.
                float hx = v.x, hz = v.z, h = std::sqrt(hx * hx + hz * hz);
                if (h < 0.3f * d || h < 1e-4f) {
                    Vec3 mid = mul(add(c.a, c.b), 0.5f);
                    hx = p.x - mid.x;
                    hz = p.z - mid.z;
                    float m = std::sqrt(hx * hx + hz * hz);
                    if (m < 1e-4f) continue;
                    hx /= m;
                    hz /= m;
                    h = 0;
                } else {
                    hx /= h;
                    hz /= h;
                }
                float vy = std::fabs(v.y);
                float need = (vy < reach ? std::sqrt(reach * reach - vy * vy) : 0) - h;
                if (need > best) {
                    best = need;
                    mx = hx * need;
                    mz = hz * need;
                }
            }
        }
        if (best <= 1e-5f) break;
        p.x += mx;
        p.z += mz;
        push[0] += mx;
        push[1] += mz;
        if (iter == 0) deepest = best;
    }
    return deepest;
}

}  // namespace s4vr::armik
