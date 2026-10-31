#include "casbody.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "addrs.h"
#include "armik.h"
#include "camera.h"
#include "log.h"
#include "rtti.h"
#include "singleview.h"

namespace s4vr::casbody {

namespace {

using camera::Quat;
using camera::Vec3;

// Game layout (1.128, still read by facehide.cpp/godhands.cpp in 1.129):
// SceneModel: +0x438 its RenderState, whose +0x20 is the client Actor (pose vector +0xf0..+0xf8, 0x30 bytes a joint:
// position +0x00, quaternion +0x10, scale +0x20, model space); +0xb0 the model's world bounds centre; +0xc0 its world
// matrix, 4 rows of 4 floats, row vectors (world = x*row0 + y*row1 + z*row2 + row3; slot 5 0x11d0160 reads it so).
// Actor vtable slot 4 (0x90a5d0 in 1.128: `mov rax,[rcx+0x370]; ret`) is its skeleton, joint table at +0x48 (begin
// +0x8, end +0x10, 0x70 bytes a joint, parent index int32 at +0x58). The joint index lookup is addrs "JointIndex".
// Mesh: +0x138 bone count, +0x160 uint16 pairs (pose joint, palette row) or null, then +0x130 uint32 pose joints.
constexpr size_t kSceneRenderState = 0x438, kRenderStateActor = 0x20;
constexpr size_t kModelCentre = 0xb0, kModelWorld = 0xc0;
constexpr size_t kActorPoseBegin = 0xf0, kActorPoseEnd = 0xf8;
constexpr size_t kSkeletonJoints = 0x48;
constexpr size_t kJointsBegin = 0x8, kJointsEnd = 0x10, kJointSize = 0x70, kJointParent = 0x58;
constexpr size_t kMeshSlots = 0x138, kMeshPairs = 0x160, kMeshPoseJoints = 0x130;
constexpr size_t kPoseJointSize = 0x30;
constexpr size_t kPaletteSlot = 48;
constexpr uint32_t kMaxJoints = 256, kMaxSlots = 256;
constexpr ULONGLONG kStale = 500;
// Where CAS stands its Sim (camera.cpp kCasSim): feet at about (128, 157, 128).
constexpr float kCasX = 128.0f, kCasY = 157.0f, kCasZ = 128.0f;

using JointIndexFn = uint32_t (*)(void* joints, uint32_t hash);
JointIndexFn g_jointIndex = nullptr;

uint32_t jointHash(const char* name) {
    uint32_t h = 0x811c9dc5;
    for (; *name; ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h = (h * 0x1000193) ^ static_cast<uint8_t>(c);
    }
    return h;
}

// ---- small maths (game and OpenXR: right-handed, Y up) ----
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
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

// An affine map p -> R p + t (3 rows of [r0 r1 r2 t]), the same layout as one palette slot.
struct Affine {
    float m[3][4];
};
Affine identity() {
    Affine a{};
    a.m[0][0] = a.m[1][1] = a.m[2][2] = 1;
    return a;
}
Vec3 apply(const Affine& a, Vec3 p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z + a.m[0][3],
            a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z + a.m[1][3],
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z + a.m[2][3]};
}
Vec3 applyLinear(const Affine& a, Vec3 p) {
    return {a.m[0][0] * p.x + a.m[0][1] * p.y + a.m[0][2] * p.z, a.m[1][0] * p.x + a.m[1][1] * p.y + a.m[1][2] * p.z,
            a.m[2][0] * p.x + a.m[2][1] * p.y + a.m[2][2] * p.z};
}
// a after b.
Affine compose(const Affine& a, const Affine& b) {
    Affine r{};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) {
            float v = a.m[i][0] * b.m[0][c] + a.m[i][1] * b.m[1][c] + a.m[i][2] * b.m[2][c];
            if (c == 3) v += a.m[i][3];
            r.m[i][c] = v;
        }
    }
    return r;
}
bool invert(const Affine& a, Affine& out) {
    const float(*m)[4] = a.m;
    float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
          c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-8f) || !std::isfinite(det)) return false;
    float k = 1 / det;
    out.m[0][0] = c00 * k;
    out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    out.m[1][0] = c01 * k;
    out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    out.m[2][0] = c02 * k;
    out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    Vec3 t = applyLinear(out, {m[0][3], m[1][3], m[2][3]});
    out.m[0][3] = -t.x;
    out.m[1][3] = -t.y;
    out.m[2][3] = -t.z;
    return true;
}
// Rotation q about the point `from`, then moved so `from` lands on `to`: x -> to + q (x - from).
Affine turnAbout(const Quat& q, Vec3 from, Vec3 to) {
    Affine a{};
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z,
          wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    a.m[0][0] = 1 - 2 * (yy + zz);
    a.m[0][1] = 2 * (xy - wz);
    a.m[0][2] = 2 * (xz + wy);
    a.m[1][0] = 2 * (xy + wz);
    a.m[1][1] = 1 - 2 * (xx + zz);
    a.m[1][2] = 2 * (yz - wx);
    a.m[2][0] = 2 * (xz - wy);
    a.m[2][1] = 2 * (yz + wx);
    a.m[2][2] = 1 - 2 * (xx + yy);
    Vec3 r = applyLinear(a, from);
    a.m[0][3] = to.x - r.x;
    a.m[1][3] = to.y - r.y;
    a.m[2][3] = to.z - r.z;
    return a;
}

bool readable(const void* p, size_t size) { return p && rtti::readable(reinterpret_cast<uintptr_t>(p), size); }

// ---- the CAS Sim's rig ----
// Which of the body's moves each joint takes: the nearest of these at or above it.
enum Chain : uint8_t { kRoot = 0, kUpperL, kForeL, kHandL, kUpperR, kForeR, kHandR, kHead, kChains };
struct Rig {
    const void* actor = nullptr;
    const void* skeleton = nullptr;
    uint32_t count = 0;
    int head = -1;
    int upper[2] = {-1, -1}, fore[2] = {-1, -1}, hand[2] = {-1, -1};
    int knuckle[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // index, middle, ring, little
    bool arm[2] = {false, false};
    int thigh[2] = {-1, -1}, calf[2] = {-1, -1}, foot[2] = {-1, -1};
    uint8_t chain[kMaxJoints] = {};
    int16_t parent[kMaxJoints] = {};  // -1: none
};
std::mutex g_rigMutex;
std::shared_ptr<const Rig> g_rig;
std::atomic<int> g_rigLogs{0};

// The actor's skeleton through its vtable slot 4, read from that function's code (a plain `mov rax,[rcx+N]; ret`)
// instead of calling it on a render thread.
const uint8_t* skeletonOf(const uint8_t* actor) {
    const uintptr_t vtable = rtti::vtableOf(reinterpret_cast<uintptr_t>(actor));
    if (!vtable || !rtti::readable(vtable + 4 * 8, 8)) return nullptr;
    const uint8_t* code = *reinterpret_cast<const uint8_t* const*>(vtable + 4 * 8);
    if (!readable(code, 8)) return nullptr;
    int32_t offset = -1;
    if (code[0] == 0x48 && code[1] == 0x8b && code[2] == 0x81 && code[7] == 0xc3)
        std::memcpy(&offset, code + 3, sizeof(offset));
    else if (code[0] == 0x48 && code[1] == 0x8b && code[2] == 0x41 && code[4] == 0xc3)
        offset = static_cast<int8_t>(code[3]);
    if (offset < 0 || offset > 0x2000 || !readable(actor + offset, 8)) {
        static int logs = 0;
        if (logs++ < 3)
            LOG("CAS body: the Sim's skeleton getter is not a plain read (%02x %02x %02x %02x %02x %02x %02x %02x); no body",
                code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
        return nullptr;
    }
    return *reinterpret_cast<const uint8_t* const*>(actor + offset);
}

std::shared_ptr<const Rig> rigFor(const uint8_t* actor, uint32_t poseCount) {
    {
        std::lock_guard<std::mutex> lock(g_rigMutex);
        if (g_rig && g_rig->actor == actor && g_rig->count == poseCount) return g_rig;
    }
    if (!g_jointIndex) g_jointIndex = reinterpret_cast<JointIndexFn>(addrs::fn("JointIndex"));
    const uint8_t* skeleton = skeletonOf(actor);
    if (!g_jointIndex || !readable(skeleton, kSkeletonJoints + kJointsEnd + 8)) return nullptr;
    uint8_t* joints = const_cast<uint8_t*>(skeleton) + kSkeletonJoints;
    auto* begin = *reinterpret_cast<uint8_t**>(joints + kJointsBegin);
    auto* end = *reinterpret_cast<uint8_t**>(joints + kJointsEnd);
    const uint32_t count = begin && end > begin ? static_cast<uint32_t>((end - begin) / kJointSize) : 0;
    if (count != poseCount || count < 16 || count > kMaxJoints || !readable(begin, count * kJointSize)) {
        if (g_rigLogs.fetch_add(1) < 5)
            LOG("CAS body: the Sim's skeleton has %u joints, its pose %u: not used", count, poseCount);
        return nullptr;
    }
    auto r = std::make_shared<Rig>();
    r->actor = actor;
    r->skeleton = skeleton;
    r->count = count;
    auto find = [&](const char* name) {
        uint32_t i = g_jointIndex(joints, jointHash(name));
        return i < count ? static_cast<int>(i) : -1;
    };
    auto parent = [&](int j) { return *reinterpret_cast<const int32_t*>(begin + static_cast<size_t>(j) * kJointSize + kJointParent); };
    r->head = find("b__Head__");
    static const char* const kKnuckle[4] = {"Index0", "Mid0", "Ring0", "Pinky0"};
    char name[48];
    for (int side = 0; side < 2; ++side) {
        const char* s = side ? "R" : "L";
        std::snprintf(name, sizeof(name), "b__%s_UpperArm__", s);
        r->upper[side] = find(name);
        std::snprintf(name, sizeof(name), "b__%s_Forearm__", s);
        r->fore[side] = find(name);
        std::snprintf(name, sizeof(name), "b__%s_Hand__", s);
        r->hand[side] = find(name);
        bool knuckles = true;
        for (int k = 0; k < 4; ++k) {
            std::snprintf(name, sizeof(name), "b__%s_%s__", s, kKnuckle[k]);
            r->knuckle[side][k] = find(name);
            knuckles = knuckles && r->knuckle[side][k] >= 0;
        }
        r->arm[side] = r->upper[side] >= 0 && r->fore[side] >= 0 && r->hand[side] >= 0 && knuckles;
        std::snprintf(name, sizeof(name), "b__%s_Thigh__", s);
        r->thigh[side] = find(name);
        std::snprintf(name, sizeof(name), "b__%s_Calf__", s);
        r->calf[side] = find(name);
        std::snprintf(name, sizeof(name), "b__%s_Foot__", s);
        r->foot[side] = find(name);
    }
    for (uint32_t j = 0; j < count; ++j) {
        const int p = parent(static_cast<int>(j));
        r->parent[j] = static_cast<int16_t>(p >= 0 && p < static_cast<int>(count) && p != static_cast<int>(j) ? p : -1);
    }
    if (r->head < 0 || !r->arm[0] || !r->arm[1]) {
        if (g_rigLogs.fetch_add(1) < 5)
            LOG("CAS body: the Sim's skeleton (%u joints) lacks a head or arms (head %d, upper arms %d %d, forearms %d %d, "
                "hands %d %d): not a Sim we can wear",
                count, r->head, r->upper[0], r->upper[1], r->fore[0], r->fore[1], r->hand[0], r->hand[1]);
        return nullptr;
    }
    const int key[kChains] = {-1, r->upper[0], r->fore[0], r->hand[0], r->upper[1], r->fore[1], r->hand[1], r->head};
    int counts[kChains] = {};
    for (uint32_t j = 0; j < count; ++j) {
        uint8_t c = kRoot;
        int k = static_cast<int>(j);
        for (int steps = 0; k >= 0 && k < static_cast<int>(count) && steps < 64 && c == kRoot; ++steps) {
            for (int i = 1; i < kChains; ++i)
                if (key[i] == k) c = static_cast<uint8_t>(i);
            k = parent(k);
        }
        r->chain[j] = c;
        ++counts[c];
    }
    if (g_rigLogs.fetch_add(1) < 10)
        LOG("CAS body: the CAS Sim's skeleton %p has %u joints: %d head and face, arms %d/%d/%d (left upper/fore/hand "
            "and fingers), %d/%d/%d (right), %d follow the body",
            skeleton, count, counts[kHead], counts[kUpperL], counts[kForeL], counts[kHandL], counts[kUpperR],
            counts[kForeR], counts[kHandR], counts[kRoot]);
    std::lock_guard<std::mutex> lock(g_rigMutex);
    g_rig = r;
    return r;
}

// ---- state ----
std::atomic<bool> g_on{true};
std::atomic<const uint8_t*> g_model{nullptr};
std::atomic<ULONGLONG> g_modelTick{0};
std::atomic<float> g_modelOff{1e9f};  // how far its centre is from the CAS spot (the nearest wins)
std::atomic<int> g_lod{-1};
std::atomic<bool> g_skipBatch{false};
std::atomic<uint32_t> g_emptyDraws{0};

// One pass's moves: every chain's model-space map, the fold point of the head, for this model and pose.
struct Prepared {
    const uint8_t* model = nullptr;
    const uint8_t* pose = nullptr;
    std::shared_ptr<const Rig> rig;
    Affine joint[kMaxJoints];  // every joint's model-space map (the CAS Sim's pose -> your body)
    Vec3 fold{0, 0, 0};
    ULONGLONG tick = 0;
    bool usedSecond = false;
    bool valid = false;
};
std::mutex g_cacheMutex;
Prepared g_cache;
thread_local Prepared t_prep;

// Where the body faces (world yaw), turned toward your head's heading past a dead zone and eased.
float g_bodyYaw = NAN;
ULONGLONG g_yawTick = 0;
constexpr float kYawDeadZone = 0.5f;  // radians (~29 degrees) your head turns before the body follows
constexpr float kYawEase = 1.2f;      // seconds: inside the dead zone the body still turns slowly to your head
constexpr float kHeadBehindEyes = 0.08f;  // metres: the head joint sits behind your eyes (live first person ~ the same)
constexpr float kHeadBelowEyes = 0.09f;   // metres: and below them; a taller Sim is lowered so you are not in its neck
constexpr float kMaxDrop = 0.30f;

// Stats for the 10 s log.
std::atomic<uint32_t> g_statPrepared{0}, g_statDraws{0}, g_statInjected{0}, g_statMeshes{0}, g_statPalettes{0},
    g_statSkipped{0}, g_statArms[2] = {{0}, {0}}, g_statNoPose{0};
std::atomic<ULONGLONG> g_statTick{0};
std::atomic<int> g_placeLogs{0};
float g_lastDrop = 0, g_lastYaw = 0;

// Your eyes at the body's eye height ([cas] body_eyes): the head joint's world height plus kHeadBelowEyes, eased over
// seconds and moved only past 6 cm, so the CAS Sim's idle (head bob) does not move your view.
std::atomic<bool> g_eyesAtBody{true};
std::atomic<float> g_eyeY{NAN};
std::atomic<ULONGLONG> g_eyeTick{0};
float g_eyeWant = NAN;
ULONGLONG g_eyeWantTick = 0;
void noteEyeHeight(float headJointY, ULONGLONG now) {
    const float want = headJointY + kHeadBelowEyes;
    if (!std::isfinite(want)) return;
    const float dt = g_eyeWantTick ? static_cast<float>(now - g_eyeWantTick) * 0.001f : 0;
    g_eyeWantTick = now;
    float y = g_eyeY.load(std::memory_order_relaxed);
    if (!std::isfinite(y) || dt > 2.0f || std::fabs(want - y) > 0.4f) {
        y = want;  // first sight, a pause, another Sim (age, height)
        g_eyeWant = want;
        static int logs = 0;
        if (logs++ < 10) LOG("CAS body: your eyes go to the body's eye height, %.2f (world)", y);
    } else {
        if (!std::isfinite(g_eyeWant) || std::fabs(want - g_eyeWant) > 0.06f) g_eyeWant = want;
        y += (g_eyeWant - y) * (1 - std::exp(-dt / 2.5f));
    }
    g_eyeY.store(y, std::memory_order_relaxed);
    g_eyeTick.store(now, std::memory_order_relaxed);
}

void logStats(ULONGLONG now) {
    ULONGLONG last = g_statTick.load(std::memory_order_relaxed);
    if (!last) {
        g_statTick.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10000 || !g_statTick.compare_exchange_strong(last, now)) return;
    LOG("CAS body: in the last 10 s, %u passes worked out, %u body draws (%u put in where the game left the Sim out), "
        "%u meshes drawn, %u left out (one-bone pieces, eyes/teeth/lashes), %u palettes; arms on your controllers "
        "%u left / %u right; %u passes with no pose; body faces %.0f deg, lowered %.2f m; batch sink %s",
        g_statPrepared.exchange(0), g_statDraws.exchange(0), g_statInjected.exchange(0), g_statMeshes.exchange(0),
        g_statSkipped.exchange(0), g_statPalettes.exchange(0), g_statArms[0].exchange(0), g_statArms[1].exchange(0),
        g_statNoPose.exchange(0), g_lastYaw * 57.2958f, g_lastDrop, g_skipBatch.load() ? "left out" : "kept");
}

bool actorPose(const uint8_t* model, const uint8_t*& actor, const uint8_t*& pose, uint32_t& count) {
    if (!readable(model + kSceneRenderState, 8)) return false;
    const uint8_t* state = *reinterpret_cast<const uint8_t* const*>(model + kSceneRenderState);
    if (!readable(state + kRenderStateActor, 8)) return false;
    actor = *reinterpret_cast<const uint8_t* const*>(state + kRenderStateActor);
    if (!readable(actor + kActorPoseBegin, 16)) return false;
    pose = *reinterpret_cast<const uint8_t* const*>(actor + kActorPoseBegin);
    const uint8_t* end = *reinterpret_cast<const uint8_t* const*>(actor + kActorPoseEnd);
    if (!pose || end <= pose) return false;
    count = static_cast<uint32_t>((end - pose) / kPoseJointSize);
    return count >= 16 && count <= kMaxJoints && readable(pose, count * kPoseJointSize);
}

Vec3 jointAt(const uint8_t* pose, int joint) {
    Vec3 p;
    std::memcpy(&p, pose + static_cast<size_t>(joint) * kPoseJointSize, sizeof(p));
    return p;
}

// Two-bone reach of one arm in the world: shoulder S, elbow E, wrist W of the body as placed, toward `wrist` with the
// hand turned like `grip` (a grip pose's frame: -Z toward the fingers, +Y toward the thumb). Gives the world moves of
// the upper arm, forearm and hand (each about its own joint).
bool reach(Vec3 S, Vec3 E, Vec3 W, const Vec3 knuckles[4], Vec3 wrist, const Quat& grip, Vec3 up, Vec3 ahead,
           Vec3 out, Affine& upper, Affine& fore, Affine& hand) {
    const float l1 = length(sub(E, S)), l2 = length(sub(W, E));
    if (!(l1 > 0.02f) || !(l2 > 0.02f)) return false;
    Vec3 toTarget = sub(wrist, S);
    float d = length(toTarget);
    if (!(d > 1e-4f)) return false;
    Vec3 dir = mul(toTarget, 1 / d);
    const float dMin = std::fabs(l1 - l2) + 0.02f * (l1 + l2), dMax = (l1 + l2) * 0.999f;
    d = d < dMin ? dMin : d > dMax ? dMax : d;
    float cosA = (l1 * l1 + d * d - l2 * l2) / (2 * l1 * d);
    cosA = cosA > 1 ? 1 : cosA < -1 ? -1 : cosA;
    const float sinA = std::sqrt(1 - cosA * cosA);
    // Elbows hang down, a little out and back; raising the hand swings them out (armik.cpp's pole).
    const float rise = dot(dir, up);
    Vec3 pole = sub(sub(mul(out, 0.5f + (rise > 0 ? rise : 0)), up), mul(ahead, 0.35f));
    Vec3 bend = sub(pole, mul(dir, dot(pole, dir)));
    if (length(bend) < 1e-3f) bend = sub(sub(E, S), mul(dir, dot(sub(E, S), dir)));
    bend = normalize(bend);
    const Vec3 elbow = add(S, mul(add(mul(dir, cosA), mul(bend, sinA)), l1));
    const Vec3 at = add(S, mul(dir, d));
    Quat q1 = fromTo(sub(E, S), sub(elbow, S));
    Vec3 W1 = add(S, rotate(q1, sub(W, S)));
    Quat q2 = qnormalize(qmul(fromTo(sub(W1, elbow), sub(at, elbow)), q1));

    // The hand: its forward (wrist to the middle of the knuckles) along the grip's -Z, its thumb side along +Y.
    Vec3 mid = mul(add(add(knuckles[0], knuckles[1]), add(knuckles[2], knuckles[3])), 0.25f);
    Vec3 fNow = normalize(sub(mid, W));
    Vec3 tNow = sub(knuckles[0], knuckles[3]);
    tNow = normalize(sub(tNow, mul(fNow, dot(tNow, fNow))));
    Vec3 fWant = rotate(grip, {0, 0, -1}), tWant = rotate(grip, {0, 1, 0});
    if (!finite(fNow) || !finite(tNow) || length(fNow) < 0.5f || length(tNow) < 0.5f) return false;
    Quat q3 = qnormalize(qmul(fromBasis(fWant, tWant, cross(fWant, tWant)), conj(fromBasis(fNow, tNow, cross(fNow, tNow)))));
    // Most of the turn around the forearm happens in the forearm (the radius rolls over the ulna), not the wrist.
    {
        Vec3 along = normalize(sub(at, elbow));
        Quat rel = qmul(q3, conj(q2));
        float twist = 2 * std::atan2(rel.x * along.x + rel.y * along.y + rel.z * along.z, rel.w);
        twist = std::remainder(twist, 6.2831853f);
        if (std::isfinite(twist)) q2 = qnormalize(qmul(axisAngle(along, twist * 0.6f), q2));
    }
    upper = turnAbout(q1, S, S);
    fore = turnAbout(q2, E, elbow);
    hand = turnAbout(q3, W, at);
    return true;
}

// ---- a still body ([cas] body_still) ----
// The body stands in one pose instead of playing the CAS Sim's idle (user spec 2026-10-29: the double does not copy
// the CAS Sim's motion): the most upright pose seen in the first 3 s with this skeleton (head over the middle of the
// feet), kept as joint rotations; the bone offsets come from the live pose every frame, so edits that move joints still
// fit. Self-check: an arm or leg bone's offset in its parent's frame is the same in any animation frame, so if the
// frozen and the live offsets disagree the quaternions are being read wrong (or the skeleton changed): picked again,
// and after 3 such picks the body plays the live idle (logged).
std::atomic<bool> g_still{true};
struct Freeze {
    std::shared_ptr<const Rig> rig;
    std::vector<uint8_t> pose;
    float score = 1e9f;
    ULONGLONG since = 0;
    int bad = 0;      // frames in a row the offsets disagreed
    int picks = 0;    // picks for this skeleton
    bool off = false; // given up: live idle
    bool proven = false;
} g_freeze;
constexpr ULONGLONG kFreezePick = 3000;
constexpr float kFreezeMismatch = 0.015f;  // metres

Quat jointQuat(const uint8_t* pose, int joint) {
    Quat q;
    std::memcpy(&q, pose + static_cast<size_t>(joint) * kPoseJointSize + 0x10, sizeof(q));
    return qnormalize(q);
}

float uprightScore(const uint8_t* pose, const Rig& r) {
    if (r.foot[0] < 0 || r.foot[1] < 0) return 0;
    const Vec3 h = jointAt(pose, r.head), f = mul(add(jointAt(pose, r.foot[0]), jointAt(pose, r.foot[1])), 0.5f);
    const float dx = h.x - f.x, dz = h.z - f.z;
    return std::sqrt(dx * dx + dz * dz);
}

// The largest difference (metres) between live and frozen arm/leg bone offsets in their parents' frames, and how far
// those bones moved in the model between the two poses (to know the check meant something).
float offsetMismatch(const uint8_t* live, const uint8_t* frozen, const Rig& r, float& moved) {
    const int joints[] = {r.fore[0], r.fore[1], r.hand[0], r.hand[1], r.calf[0], r.calf[1], r.foot[0], r.foot[1]};
    float worst = 0;
    moved = 0;
    for (int j : joints) {
        if (j < 0 || r.parent[j] < 0) continue;
        const int p = r.parent[j];
        const Vec3 dl = sub(jointAt(live, j), jointAt(live, p)), df = sub(jointAt(frozen, j), jointAt(frozen, p));
        const Vec3 a = rotate(conj(jointQuat(live, p)), dl), b = rotate(conj(jointQuat(frozen, p)), df);
        const float d = length(sub(a, b)), m = length(sub(dl, df));
        if (!std::isfinite(d)) return 1e9f;
        worst = d > worst ? d : worst;
        moved = m > moved ? m : moved;
    }
    return worst;
}

// The frozen pose for this frame, or null (the live idle). Under g_cacheMutex.
const uint8_t* frozenPose(const uint8_t* pose, const std::shared_ptr<const Rig>& rig, ULONGLONG now) {
    if (!g_still.load(std::memory_order_relaxed)) return nullptr;
    const Rig& r = *rig;
    const size_t bytes = static_cast<size_t>(r.count) * kPoseJointSize;
    auto pick = [&](bool fresh) {
        g_freeze.pose.assign(pose, pose + bytes);
        g_freeze.score = uprightScore(pose, r);
        g_freeze.since = now;
        g_freeze.bad = 0;
        if (fresh) {
            g_freeze.picks = 0;
            g_freeze.off = false;
            g_freeze.proven = false;
        }
        ++g_freeze.picks;
    };
    if (g_freeze.rig != rig || g_freeze.pose.size() != bytes) {
        g_freeze.rig = rig;
        pick(true);
        return g_freeze.pose.data();
    }
    if (g_freeze.off) return nullptr;
    if (now - g_freeze.since < kFreezePick) {
        const float score = uprightScore(pose, r);
        if (score < g_freeze.score - 0.005f) {
            g_freeze.pose.assign(pose, pose + bytes);
            g_freeze.score = score;
        }
        return g_freeze.pose.data();
    }
    float moved = 0;
    const float mismatch = offsetMismatch(pose, g_freeze.pose.data(), r, moved);
    if (mismatch < kFreezeMismatch) {
        g_freeze.bad = 0;
        if (!g_freeze.proven && moved > 0.05f) {
            g_freeze.proven = true;
            LOG("CAS body: still pose checked: the bones moved %.0f mm in the model since it was picked and their "
                "offsets agree within %.1f mm (head over feet %.0f mm)",
                moved * 1000, mismatch * 1000, g_freeze.score * 1000);
        }
    } else if (++g_freeze.bad >= 60) {
        if (g_freeze.picks >= 3) {
            g_freeze.off = true;
            LOG("CAS body: still pose given up (bone offsets off by %.0f mm after 3 picks): the body plays the CAS idle",
                mismatch * 1000);
            return nullptr;
        }
        LOG("CAS body: still pose off by %.0f mm from the live bones (skeleton changed?): picked again", mismatch * 1000);
        pick(false);
    }
    return g_freeze.pose.data();
}

// The body's base pose: the frozen rotations on the live bone offsets (or the live pose itself), model space.
void basePose(const uint8_t* live, const uint8_t* frozen, const Rig& r, Vec3* bp, Quat* bq) {
    const uint32_t n = r.count;
    if (!frozen) {
        for (uint32_t j = 0; j < n; ++j) {
            bp[j] = jointAt(live, static_cast<int>(j));
            bq[j] = jointQuat(live, static_cast<int>(j));
        }
        return;
    }
    bool done[kMaxJoints] = {};
    for (int pass = 0; pass < 64; ++pass) {
        bool more = false;
        for (uint32_t j = 0; j < n; ++j) {
            if (done[j]) continue;
            const int p = r.parent[j];
            const int jj = static_cast<int>(j);
            if (p < 0) {
                bp[j] = jointAt(frozen, jj);
                bq[j] = jointQuat(frozen, jj);
            } else if (done[p]) {
                const Quat local = qmul(conj(jointQuat(frozen, p)), jointQuat(frozen, jj));
                const Vec3 offset = rotate(conj(jointQuat(live, p)), sub(jointAt(live, jj), jointAt(live, p)));
                bq[j] = qnormalize(qmul(bq[p], local));
                bp[j] = add(bp[p], rotate(bq[p], offset));
            } else {
                more = true;
                continue;
            }
            done[j] = true;
        }
        if (!more) break;
    }
    for (uint32_t j = 0; j < n; ++j)
        if (!done[j]) bp[j] = jointAt(live, static_cast<int>(j)), bq[j] = jointQuat(live, static_cast<int>(j));
}

bool compute(const uint8_t* model, const uint8_t* pose, const std::shared_ptr<const Rig>& rig, Prepared& out) {
    // The CAS Sim's world matrix (row vectors) as a column map: world = A x + t.
    float rows[16];
    std::memcpy(rows, model + kModelWorld, sizeof(rows));
    Affine sim{};
    for (int i = 0; i < 3; ++i) {
        sim.m[i][0] = rows[0 + i];
        sim.m[i][1] = rows[4 + i];
        sim.m[i][2] = rows[8 + i];
        sim.m[i][3] = rows[12 + i];
    }
    const Vec3 t{sim.m[0][3], sim.m[1][3], sim.m[2][3]};
    const float colLen = length(Vec3{sim.m[0][1], sim.m[1][1], sim.m[2][1]});
    Affine simInv;
    if (!finite(t) || std::fabs(t.x - kCasX) > 4 || std::fabs(t.z - kCasZ) > 4 || t.y < 150 || t.y > 170 ||
        !(colLen > 0.2f && colLen < 5) || !invert(sim, simInv)) {
        static int logs = 0;
        if (logs++ < 5)
            LOG("CAS body: the CAS Sim's world matrix looks wrong (place %.2f %.2f %.2f, up axis %.3f): no body",
                t.x, t.y, t.z, colLen);
        return false;
    }

    Vec3 head, hRight, hUp, hBack;
    camera::View view;
    if (!camera::headInWorld(head, hRight, hUp, hBack) || !camera::lastView(view) || !(view.scale > 0)) return false;
    const float scale = view.scale;
    const ULONGLONG now = GetTickCount64();

    // The pose the body takes before the arms: still ([cas] body_still) or the CAS Sim's own.
    static Vec3 bp[kMaxJoints];  // under g_cacheMutex
    static Quat bq[kMaxJoints];
    const uint8_t* frozen = frozenPose(pose, rig, now);
    basePose(pose, frozen, *rig, bp, bq);
    auto P = [&](int joint) { return bp[joint]; };

    // Which way the Sim faces in the world now, from its shoulders.
    const Vec3 across = applyLinear(sim, sub(P(rig->upper[1]), P(rig->upper[0])));
    const Vec3 simAhead{across.z, 0, -across.x};  // up x (left to right)
    if (!(length(simAhead) > 1e-4f)) return false;
    const float simYaw = std::atan2(simAhead.x, simAhead.z);

    // Which way you face: the head's forward, or (looking down) its top, which then points ahead.
    const float down = hBack.y > 0 ? hBack.y : 0;
    Vec3 facing{-hBack.x + hUp.x * down, 0, -hBack.z + hUp.z * down};
    {
        const float dt = g_yawTick ? static_cast<float>(now - g_yawTick) * 0.001f : 0;
        g_yawTick = now;
        if (length(facing) > 0.1f) {
            const float headYaw = std::atan2(facing.x, facing.z);
            if (!std::isfinite(g_bodyYaw) || dt > 1.0f) {
                g_bodyYaw = headYaw;
            } else {
                float diff = std::remainder(headYaw - g_bodyYaw, 6.2831853f);
                if (diff > kYawDeadZone) g_bodyYaw += diff - kYawDeadZone, diff = kYawDeadZone;
                if (diff < -kYawDeadZone) g_bodyYaw += diff + kYawDeadZone, diff = -kYawDeadZone;
                g_bodyYaw += diff * (1 - std::exp(-dt / kYawEase));
                g_bodyYaw = std::remainder(g_bodyYaw, 6.2831853f);
            }
        }
        if (!std::isfinite(g_bodyYaw)) return false;
    }
    const Vec3 bodyAhead{std::sin(g_bodyYaw), 0, std::cos(g_bodyYaw)};
    const Quat turn = axisAngle({0, 1, 0}, std::remainder(g_bodyYaw - simYaw, 6.2831853f));

    // The body as placed in the world: the Sim turned to face your way, its head joint under and behind your eyes,
    // its feet on the CAS floor (lowered if it is taller than your eyes).
    Affine body = compose(turnAbout(turn, {0, 0, 0}, {0, 0, 0}), sim);
    const Vec3 headLinear = applyLinear(body, P(rig->head));
    Vec3 aheadOfHead = normalize(Vec3{facing.x, 0, facing.z});
    if (!(length(aheadOfHead) > 0.5f)) aheadOfHead = bodyAhead;
    const Vec3 headAt = sub(head, mul(aheadOfHead, kHeadBehindEyes * scale));
    noteEyeHeight(t.y + headLinear.y, now);
    float drop = (t.y + headLinear.y) - (head.y - kHeadBelowEyes * scale);
    drop = drop > 0 ? (drop < kMaxDrop * scale ? drop : kMaxDrop * scale) : 0;
    body.m[0][3] = headAt.x - headLinear.x;
    body.m[1][3] = t.y - drop;
    body.m[2][3] = headAt.z - headLinear.z;
    g_lastDrop = drop / scale;
    g_lastYaw = g_bodyYaw;

    const Affine root = compose(simInv, body);  // CAS Sim model space -> the body, in the same model space
    Affine move[kChains];
    for (auto& m : move) m = root;
    out.fold = apply(root, P(rig->head));

    // The arms reach for your controllers.
    armik::Hand hands[2];
    if (armik::controllers(hands)) {
        const Vec3 up{0, 1, 0};
        const Vec3 rightDir{-bodyAhead.z, 0, bodyAhead.x};  // ahead x up: the body's right
        for (int side = 0; side < 2; ++side) {
            if (!hands[side].valid || !rig->arm[side]) continue;
            const float s = side == 0 ? 1.0f : -1.0f;  // palm along +X (left) or -X (right) of the grip pose
            Quat gripXr = qmul(hands[side].orientation, axisAngle({1, 0, 0}, -armik::handPitch()));
            Vec3 wristXr = add(hands[side].position, rotate(gripXr, Vec3{-s * 0.02f, 0, 0.08f}));
            Vec3 wrist = add(view.anchor, rotate(view.heading, mul(wristXr, scale)));
            Quat grip = qnormalize(qmul(view.heading, gripXr));
            Vec3 S = apply(body, P(rig->upper[side])), E = apply(body, P(rig->fore[side])), W = apply(body, P(rig->hand[side]));
            Vec3 knuckles[4];
            for (int k = 0; k < 4; ++k) knuckles[k] = apply(body, P(rig->knuckle[side][k]));
            const Vec3 outward = side == 0 ? mul(rightDir, -1) : rightDir;
            Affine upper, fore, hand;
            if (!finite(wrist) || !reach(S, E, W, knuckles, wrist, grip, up, bodyAhead, outward, upper, fore, hand))
                continue;
            const int base = side == 0 ? kUpperL : kUpperR;
            move[base] = compose(simInv, compose(upper, body));
            move[base + 1] = compose(simInv, compose(fore, body));
            move[base + 2] = compose(simInv, compose(hand, body));
            g_statArms[side].fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Each joint: from where the CAS Sim's pose has it to the base pose (a turn about it), then its chain's move.
    for (uint32_t j = 0; j < rig->count; ++j) {
        const int jj = static_cast<int>(j);
        const Affine still =
            frozen ? turnAbout(qnormalize(qmul(bq[j], conj(jointQuat(pose, jj)))), jointAt(pose, jj), bp[j]) : identity();
        out.joint[j] = compose(move[rig->chain[j]], still);
    }
    if (g_placeLogs.fetch_add(1) < 6) {
        const Vec3 feet = apply(body, Vec3{0, 0, 0});
        LOG("CAS body: placed with its feet at %.2f %.2f %.2f facing %.0f deg (the CAS Sim at %.2f %.2f %.2f faces "
            "%.0f deg); your eyes at %.2f %.2f %.2f, its head joint %.2f m above its feet, lowered %.2f m; %s pose",
            feet.x, feet.y, feet.z, g_bodyYaw * 57.2958f, t.x, t.y, t.z, simYaw * 57.2958f, head.x, head.y, head.z,
            headLinear.y / scale, drop / scale, frozen ? "still" : "the CAS Sim's own");
    }
    out.model = model;
    out.pose = pose;
    out.rig = rig;
    out.tick = now;
    out.usedSecond = false;
    out.valid = true;
    return true;
}

}  // namespace

void setEnabled(bool on) {
    if (g_on.exchange(on) != on) LOG("CAS body %s", on ? "on" : "off");
}
bool enabled() { return g_on.load(std::memory_order_relaxed); }

bool isCasSim(const uint8_t* sceneModel) {
    if (!sceneModel || !enabled() || !readable(sceneModel + kModelCentre, 16)) return false;
    float c[3];
    std::memcpy(c, sceneModel + kModelCentre, sizeof(c));
    const float dx = c[0] - kCasX, dz = c[2] - kCasZ;
    const float off = std::sqrt(dx * dx + dz * dz);
    if (!std::isfinite(off) || off > 2.5f || c[1] < kCasY - 3 || c[1] > kCasY + 5) return false;
    const uint8_t* actor;
    const uint8_t* pose;
    uint32_t count;
    if (!actorPose(sceneModel, actor, pose, count)) return false;
    const ULONGLONG now = GetTickCount64();
    const uint8_t* known = g_model.load(std::memory_order_acquire);
    const bool fresh = known && now - g_modelTick.load(std::memory_order_relaxed) <= kStale;
    if (fresh && known != sceneModel && g_modelOff.load(std::memory_order_relaxed) < off) return false;  // another is nearer
    if (known != sceneModel) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 10)
            LOG("CAS body: the CAS Sim's scene model is %p (centre %.2f %.2f %.2f, %u joints)", sceneModel, c[0], c[1],
                c[2], count);
        g_model.store(sceneModel, std::memory_order_release);
        g_lod.store(-1, std::memory_order_relaxed);
    }
    g_modelOff.store(off, std::memory_order_relaxed);
    g_modelTick.store(now, std::memory_order_relaxed);
    return true;
}

const uint8_t* model() {
    if (GetTickCount64() - g_modelTick.load(std::memory_order_relaxed) > kStale) return nullptr;
    return g_model.load(std::memory_order_acquire);
}
int lodHint() { return g_lod.load(std::memory_order_relaxed); }
void setLodHint(int lod) { g_lod.store(lod, std::memory_order_relaxed); }

bool prepare(const uint8_t* sceneModel) {
    t_prep.valid = false;
    if (!sceneModel || !enabled() || !camera::casActive()) return false;
    const ULONGLONG now = GetTickCount64();
    logStats(now);
    const uint8_t* actor;
    const uint8_t* pose;
    uint32_t count;
    if (!actorPose(sceneModel, actor, pose, count)) {
        g_statNoPose.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto rig = rigFor(actor, count);
    if (!rig) return false;
    const bool second = singleview::inSecond();
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    // Both eyes of a frame (the right eye's pass runs after the left's) get the same body; several draws in one pass
    // (depth first, then colour) too.
    const bool same = g_cache.valid && g_cache.model == sceneModel && g_cache.pose == pose && g_cache.rig == rig;
    if (same && (second ? now - g_cache.tick < 60 : (!g_cache.usedSecond && now - g_cache.tick < 6))) {
        if (second) g_cache.usedSecond = true;
        t_prep = g_cache;
        return true;
    }
    Prepared fresh;
    if (!compute(sceneModel, pose, rig, fresh)) {
        g_cache.valid = false;
        return false;
    }
    fresh.usedSecond = second;
    g_cache = fresh;
    t_prep = fresh;
    g_statPrepared.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool drawMesh(const uint8_t* mesh) {
    if (!t_prep.valid || !mesh) return false;
    const uint32_t slots = *reinterpret_cast<const uint32_t*>(mesh + kMeshSlots);
    const uint16_t* pairs = *reinterpret_cast<uint16_t* const*>(mesh + kMeshPairs);
    const uint32_t* joints = *reinterpret_cast<uint32_t* const*>(mesh + kMeshPoseJoints);
    // One-bone pieces skip the palette (drawn rigidly where the CAS Sim is): left out.
    if (slots <= 1 || slots > kMaxSlots || (!pairs && !joints)) {
        g_statSkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const Rig& r = *t_prep.rig;
    bool body = false;
    for (uint32_t i = 0; i < slots && !body; ++i) {
        const uint32_t joint = pairs ? pairs[i * 2] : joints[i];
        body = joint < r.count && r.chain[joint] != kHead;
    }
    if (!body) {  // eyes, teeth, lashes: nothing to see from inside the head
        g_statSkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_statMeshes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void palette(const uint8_t* mesh, uint8_t* out, const uint8_t* pose) {
    if (!t_prep.valid || !mesh || !out || pose != t_prep.pose) return;
    const uint32_t slots = *reinterpret_cast<const uint32_t*>(mesh + kMeshSlots);
    const uint16_t* pairs = *reinterpret_cast<uint16_t* const*>(mesh + kMeshPairs);
    const uint32_t* joints = *reinterpret_cast<uint32_t* const*>(mesh + kMeshPoseJoints);
    if (!slots || slots > kMaxSlots || (!pairs && !joints)) return;
    const Rig& r = *t_prep.rig;
    const float folded[12] = {0, 0, 0, t_prep.fold.x, 0, 0, 0, t_prep.fold.y, 0, 0, 0, t_prep.fold.z};
    for (uint32_t i = 0; i < slots; ++i) {
        const uint32_t joint = pairs ? pairs[i * 2] : joints[i];
        const uint32_t slot = pairs ? pairs[i * 2 + 1] / 3u : i;
        if (slot >= kMaxSlots || joint >= r.count) continue;
        auto* m = reinterpret_cast<float*>(out + static_cast<size_t>(slot) * kPaletteSlot);
        const uint8_t chain = r.chain[joint];
        if (chain == kHead) {
            std::memcpy(m, folded, sizeof(folded));
            continue;
        }
        Affine old;
        std::memcpy(old.m, m, sizeof(old.m));
        const Affine moved = compose(t_prep.joint[joint], old);
        std::memcpy(m, moved.m, sizeof(moved.m));
    }
    g_statPalettes.fetch_add(1, std::memory_order_relaxed);
}

bool eyeHeight(float& worldY) {
    if (!enabled() || !g_eyesAtBody.load(std::memory_order_relaxed) ||
        GetTickCount64() - g_eyeTick.load(std::memory_order_relaxed) > 1000)
        return false;
    const float y = g_eyeY.load(std::memory_order_relaxed);
    if (!std::isfinite(y)) return false;
    worldY = y;
    return true;
}

void setEyesAtBody(bool on) {
    if (g_eyesAtBody.exchange(on) != on) LOG("CAS body: eyes at the body's height %s", on ? "on" : "off");
}

void setStill(bool on) {
    if (g_still.exchange(on) != on) LOG("CAS body: still pose %s", on ? "on" : "off");
}

bool skipBatch() { return g_skipBatch.load(std::memory_order_relaxed); }

void drawn(uint32_t meshes, bool injected) {
    g_statDraws.fetch_add(1, std::memory_order_relaxed);
    if (injected) g_statInjected.fetch_add(1, std::memory_order_relaxed);
    if (meshes) {
        g_emptyDraws.store(0, std::memory_order_relaxed);
        return;
    }
    // The game queued the body's meshes for a batched draw later, outside our draw: they would show at the CAS Sim
    // with its own palette. After 30 empty draws in a row, the body's draws leave the batch sink out.
    if (g_emptyDraws.fetch_add(1, std::memory_order_relaxed) + 1 == 30 && !g_skipBatch.exchange(true))
        LOG("CAS body: 30 body draws in a row reached no mesh; leaving the view's batch sink out for them");
}

}  // namespace s4vr::casbody
