#include "godhands.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#include "addrs.h"
#include "armik.h"
#include "camera.h"
#include "log.h"

namespace s4vr::godhands {

namespace {

using camera::Quat;
using camera::Vec3;

// Same layout facehide.cpp reads (see there): the client Actor, its pose (model space, 48 bytes a joint:
// position, quaternion at +0x10), skeleton and world transform.
constexpr size_t kActorPoseBegin = 0xf0, kActorPoseEnd = 0xf8;
constexpr size_t kActorSkeletonSlot = 0x20 / 8, kSkeletonJoints = 0x48;
constexpr size_t kJointsBegin = 0x8, kJointsEnd = 0x10, kJointSize = 0x70, kJointParent = 0x58;
constexpr size_t kActorPosition = 0x2e0, kActorRotation = 0x2f0, kActorScale = 0x300;
constexpr size_t kSceneRenderState = 0x438, kRenderStateActor = 0x20;
constexpr size_t kMeshSlots = 0x138, kMeshPairs = 0x160, kMeshPoseJoints = 0x130;
constexpr size_t kPoseJointSize = 0x30, kPoseQuat = 0x10;
constexpr size_t kPaletteSlot = 48;
constexpr uint32_t kMaxJoints = 256, kMaxSlots = 256;
constexpr ULONGLONG kStale = 500;  // ms: an actor not set this long is gone

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
Vec3 normalize(Vec3 a) {
    float l = std::sqrt(dot(a, a));
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
// 3x3 rotation matrix of a quaternion, row-major.
void matrixOf(const Quat& q, float m[9]) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z,
          wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz);
    m[1] = 2 * (xy - wz);
    m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz);
    m[4] = 1 - 2 * (xx + zz);
    m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy);
    m[7] = 2 * (yz + wx);
    m[8] = 1 - 2 * (xx + yy);
}

// ---- the selected Sim's rig ----
// The joints that matter, found by name once per skeleton: each hand, its four knuckles (the finger roots,
// which stay put when the fingers curl) and every joint that follows the hand (the hand and below).
struct Rig {
    void* skeleton = nullptr;
    uint32_t count = 0;
    uint64_t version = 0;
    int hand[2] = {-1, -1};
    int knuckle[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};  // index, middle, ring, little
    uint64_t follows[2][kMaxJoints / 64] = {};                 // the hand joint and everything below it
};
std::mutex g_rigMutex;
std::shared_ptr<const Rig> g_rig;
uint64_t g_rigVersion = 0;

std::atomic<bool> g_on{true};
std::atomic<void*> g_actor{nullptr};
std::atomic<ULONGLONG> g_actorTick{0};

std::shared_ptr<const Rig> rig() {
    std::lock_guard<std::mutex> lock(g_rigMutex);
    return g_rig;
}

bool follows(const Rig& r, int side, uint32_t joint) {
    return joint < kMaxJoints && (r.follows[side][joint / 64] >> (joint % 64) & 1);
}

void buildRig(void* actor) {
    auto* a = static_cast<uint8_t*>(actor);
    using SkeletonFn = uint8_t* (*)(void*);
    uint8_t* skeleton = (*reinterpret_cast<SkeletonFn**>(a))[kActorSkeletonSlot](a);
    if (!skeleton) return;
    uint8_t* joints = skeleton + kSkeletonJoints;
    auto* begin = *reinterpret_cast<uint8_t**>(joints + kJointsBegin);
    auto* end = *reinterpret_cast<uint8_t**>(joints + kJointsEnd);
    uint32_t count = begin && end > begin ? static_cast<uint32_t>((end - begin) / kJointSize) : 0;
    {
        std::lock_guard<std::mutex> lock(g_rigMutex);
        if (g_rig && g_rig->skeleton == skeleton && g_rig->count == count) return;
    }
    auto r = std::make_shared<Rig>();
    r->skeleton = skeleton;
    r->count = count;
    static const char* const kKnuckle[4] = {"Index0", "Mid0", "Ring0", "Pinky0"};
    char name[48];
    for (int side = 0; side < 2; ++side) {
        const char* s = side ? "R" : "L";
        std::snprintf(name, sizeof(name), "b__%s_Hand__", s);
        uint32_t h = g_jointIndex(joints, jointHash(name));
        if (h >= count || count > kMaxJoints) continue;
        r->hand[side] = static_cast<int>(h);
        for (int k = 0; k < 4; ++k) {
            std::snprintf(name, sizeof(name), "b__%s_%s__", s, kKnuckle[k]);
            uint32_t j = g_jointIndex(joints, jointHash(name));
            r->knuckle[side][k] = j < count ? static_cast<int>(j) : -1;
        }
        for (uint32_t j = 0; j < count; ++j) {
            int k = static_cast<int>(j);
            for (int steps = 0; k >= 0 && k < static_cast<int>(count) && steps < 64; ++steps) {
                if (k == r->hand[side]) {
                    r->follows[side][j / 64] |= 1ull << (j % 64);
                    break;
                }
                k = *reinterpret_cast<int32_t*>(begin + static_cast<size_t>(k) * kJointSize + kJointParent);
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_rigMutex);
    r->version = ++g_rigVersion;
    LOG("God hands: the selected Sim's skeleton has %u joints, left hand %d, right hand %d", count, r->hand[0],
        r->hand[1]);
    g_rig = r;
}

// ---- what each mesh follows ----
struct MeshKind {
    const uint8_t* mesh = nullptr;
    uint32_t slots = 0;
    const void* map = nullptr;
    uint64_t version = 0;
    uint32_t paletteSlots = 0;
    int mask = 0;  // bit 0: some bone follows the left hand, bit 1: the right
    uint8_t slotSide[kMaxSlots] = {};  // 0 none, 1 left hand, 2 right hand
};
constexpr int kMaxKinds = 48;
std::mutex g_kindMutex;
MeshKind g_kinds[kMaxKinds];
int g_kindNext = 0;
std::atomic<int> g_kindLogs{0};

// The mesh's kind (copied out), or false when it has no bones.
bool kindOf(const uint8_t* mesh, const Rig& r, MeshKind& out) {
    uint32_t slots = *reinterpret_cast<const uint32_t*>(mesh + kMeshSlots);
    const uint16_t* pairs = *reinterpret_cast<uint16_t* const*>(mesh + kMeshPairs);
    const uint32_t* joints = *reinterpret_cast<uint32_t* const*>(mesh + kMeshPoseJoints);
    const void* map = pairs ? static_cast<const void*>(pairs) : static_cast<const void*>(joints);
    if (!slots || slots > kMaxSlots || !map) return false;
    std::lock_guard<std::mutex> lock(g_kindMutex);
    MeshKind* found = nullptr;
    for (auto& k : g_kinds)
        if (k.mesh == mesh) found = &k;
    if (found && found->slots == slots && found->map == map && found->version == r.version) {
        out = *found;
        return true;
    }
    if (!found) {
        found = &g_kinds[g_kindNext];
        g_kindNext = (g_kindNext + 1) % kMaxKinds;
    }
    *found = MeshKind{};
    found->mesh = mesh;
    found->slots = slots;
    found->map = map;
    found->version = r.version;
    int counts[2] = {0, 0};
    for (uint32_t i = 0; i < slots; ++i) {
        uint32_t joint = pairs ? pairs[i * 2] : joints[i];
        uint32_t slot = pairs ? pairs[i * 2 + 1] / 3u : i;
        if (slot >= kMaxSlots) continue;
        found->paletteSlots = slot + 1 > found->paletteSlots ? slot + 1 : found->paletteSlots;
        for (int side = 0; side < 2; ++side) {
            if (!follows(r, side, joint)) continue;
            found->slotSide[slot] = static_cast<uint8_t>(side + 1);
            ++counts[side];
        }
    }
    found->mask = (counts[0] ? 1 : 0) | (counts[1] ? 2 : 0);
    if (g_kindLogs.fetch_add(1) < 30)
        LOG("God hands: a mesh of the selected Sim with %u bones, %d on its left hand, %d on its right%s", slots,
            counts[0], counts[1], found->mask ? ": drawn again at your controllers" : "");
    out = *found;
    return true;
}

// ---- where the controllers put the hands ----
struct Target {
    bool ok = false;
    float rot[9];     // model space: takes the Sim's hand joint to the controller's
    Vec3 move;        // and the translation after that
    Vec3 wrist;       // the folded bones go here (model space)
};

Vec3 jointPos(const uint8_t* pose, int joint) {
    Vec3 p;
    std::memcpy(&p, pose + static_cast<size_t>(joint) * kPoseJointSize, sizeof(p));
    return p;
}
Quat jointQuat(const uint8_t* pose, int joint) {
    Quat q;
    std::memcpy(&q, pose + static_cast<size_t>(joint) * kPoseJointSize + kPoseQuat, sizeof(q));
    return qnormalize(q);
}

Target targetFor(const Rig& r, int side, const uint8_t* pose, const armik::Hand& hand) {
    Target t;
    void* actor = g_actor.load(std::memory_order_acquire);
    camera::View view;
    if (!actor || r.hand[side] < 0 || !camera::lastView(view) || !(view.scale > 0)) return t;
    const auto* a = static_cast<const uint8_t*>(actor);
    Vec3 pa;
    Quat qa;
    float sa[3];
    std::memcpy(&pa, a + kActorPosition, sizeof(pa));
    std::memcpy(&qa, a + kActorRotation, sizeof(qa));
    std::memcpy(sa, a + kActorScale, sizeof(sa));
    qa = qnormalize(qa);
    float scale = (sa[0] + sa[1] + sa[2]) / 3;
    if (!(scale > 1e-4f) || !finite(pa)) return t;

    // The controller: its grip pose tilted by hand_pitch (as armik turns the hand), the wrist 8 cm behind
    // the palm and 2 cm toward the back of the hand.
    const float s = side == 0 ? 1.0f : -1.0f;
    Quat gripXr = qmul(hand.orientation, axisAngle({1, 0, 0}, -armik::handPitch()));
    Vec3 wristXr = add(hand.position, rotate(gripXr, Vec3{-s * 0.02f, 0, 0.08f}));
    Vec3 wristWorld = add(view.anchor, rotate(view.heading, mul(wristXr, view.scale)));
    Quat gripWorld = qmul(view.heading, gripXr);
    Quat inv = conj(qa);
    Vec3 wrist = mul(rotate(inv, sub(wristWorld, pa)), 1 / scale);
    Quat grip = qmul(inv, gripWorld);

    // The Sim's hand axes in its hand joint's frame, from this very pose: forward (wrist to the middle of
    // the four knuckles), toward the thumb (little finger's knuckle to the index finger's) and their cross.
    Vec3 hp = jointPos(pose, r.hand[side]);
    Quat hq = jointQuat(pose, r.hand[side]);
    Vec3 base[4];
    for (int k = 0; k < 4; ++k) {
        if (r.knuckle[side][k] < 0) return t;
        base[k] = jointPos(pose, r.knuckle[side][k]);
    }
    Vec3 mid = mul(add(add(base[0], base[1]), add(base[2], base[3])), 0.25f);
    Quat hinv = conj(hq);
    Vec3 forward = normalize(rotate(hinv, sub(mid, hp)));
    Vec3 thumb = rotate(hinv, sub(base[0], base[3]));
    thumb = normalize(sub(thumb, mul(forward, dot(thumb, forward))));
    Vec3 up = cross(forward, thumb);
    if (!finite(forward) || !finite(thumb) || !finite(up) || !finite(hp) || !finite(wrist)) return t;
    // Grip coordinates (x, y, z) -> hand frame: x along `up`, y toward the thumb, z backwards.
    Quat toHand = fromBasis(up, thumb, mul(forward, -1));
    Quat target = qnormalize(qmul(grip, conj(toHand)));
    Quat turn = qnormalize(qmul(target, conj(hq)));
    matrixOf(turn, t.rot);
    t.move = sub(wrist, rotate(turn, hp));
    t.wrist = wrist;
    t.ok = true;
    return t;
}

// Stats for the log.
std::atomic<uint32_t> g_statMeshes{0}, g_statPalettes{0}, g_statFailed{0};
std::atomic<ULONGLONG> g_statTick{0};

}  // namespace

void setEnabled(bool on) {
    if (g_on.exchange(on) != on) LOG("God hands %s", on ? "on" : "off");
}

void setActor(void* actor) {
    if (!g_jointIndex) g_jointIndex = reinterpret_cast<JointIndexFn>(addrs::fn("JointIndex"));
    if (!actor || !g_jointIndex || !g_on.load(std::memory_order_relaxed)) {
        g_actor.store(nullptr, std::memory_order_release);
        return;
    }
    auto* a = static_cast<uint8_t*>(actor);
    void* pose = *reinterpret_cast<void**>(a + kActorPoseBegin);
    if (!pose || pose == *reinterpret_cast<void**>(a + kActorPoseEnd)) {
        g_actor.store(nullptr, std::memory_order_release);
        return;
    }
    if (g_actor.load(std::memory_order_relaxed) != actor) LOG("God hands: the selected Sim's actor is %p", actor);
    buildRig(actor);
    g_actor.store(actor, std::memory_order_release);
    ULONGLONG now = GetTickCount64();
    g_actorTick.store(now, std::memory_order_relaxed);
    ULONGLONG last = g_statTick.load(std::memory_order_relaxed);
    if (!last) {
        g_statTick.store(now, std::memory_order_relaxed);
    } else if (now - last >= 10000) {
        g_statTick.store(now, std::memory_order_relaxed);
        LOG("God hands: in the last 10 s, %u hand meshes drawn, %u palettes made, %u could not be placed",
            g_statMeshes.exchange(0), g_statPalettes.exchange(0), g_statFailed.exchange(0));
    }
}

bool isSelected(const uint8_t* sceneModel) {
    void* actor = g_actor.load(std::memory_order_acquire);
    if (!actor || !sceneModel || GetTickCount64() - g_actorTick.load(std::memory_order_relaxed) > kStale) return false;
    auto* state = *reinterpret_cast<const uint8_t* const*>(sceneModel + kSceneRenderState);
    return state && *reinterpret_cast<void* const*>(state + kRenderStateActor) == actor;
}

int sidesFor(const uint8_t* mesh, const void* pose) {
    void* actor = g_actor.load(std::memory_order_acquire);
    if (!actor || !mesh || !pose || GetTickCount64() - g_actorTick.load(std::memory_order_relaxed) > kStale) return 0;
    if (pose != *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + kActorPoseBegin)) return 0;
    auto r = rig();
    if (!r) return 0;
    MeshKind kind;
    if (!kindOf(mesh, *r, kind) || !kind.mask) return 0;
    armik::Hand hands[2];
    if (!armik::controllers(hands)) return 0;
    int mask = kind.mask & ((hands[0].valid ? 1 : 0) | (hands[1].valid ? 2 : 0));
    if (mask) g_statMeshes.fetch_add(1, std::memory_order_relaxed);
    return mask;
}

void palette(int side, const uint8_t* mesh, uint8_t* out, const uint8_t* pose) {
    if (side < 0 || side > 1 || !mesh || !out || !pose) return;
    auto r = rig();
    if (!r) return;
    MeshKind kind;
    if (!kindOf(mesh, *r, kind)) return;
    armik::Hand hands[2];
    if (!armik::controllers(hands) || !hands[side].valid) return;
    Target t = targetFor(*r, side, pose, hands[side]);
    if (!t.ok) {
        g_statFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_statPalettes.fetch_add(1, std::memory_order_relaxed);
    const float folded[12] = {0, 0, 0, t.wrist.x, 0, 0, 0, t.wrist.y, 0, 0, 0, t.wrist.z};
    const float move[3] = {t.move.x, t.move.y, t.move.z};
    for (uint32_t slot = 0; slot < kind.paletteSlots && slot < kMaxSlots; ++slot) {
        auto* m = reinterpret_cast<float*>(out + static_cast<size_t>(slot) * kPaletteSlot);
        if (kind.slotSide[slot] != side + 1) {
            std::memcpy(m, folded, sizeof(folded));
            continue;
        }
        // Rows are [r0 r1 r2 t]: rotate each column (including t) by the hand's turn, then add the move.
        float old[12];
        std::memcpy(old, m, sizeof(old));
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 4; ++c)
                m[i * 4 + c] = t.rot[i * 3] * old[c] + t.rot[i * 3 + 1] * old[4 + c] + t.rot[i * 3 + 2] * old[8 + c];
            m[i * 4 + 3] += move[i];
        }
    }
}

}  // namespace s4vr::godhands
