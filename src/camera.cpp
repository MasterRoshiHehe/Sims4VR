#include "camera.h"
#include "player.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include "MinHook.h"
#include "crashlog.h"
#include "armik.h"
#include "voice.h"
#include "doorswing.h"
#include "gamefix.h"
#include "ambience.h"
#include "thumbs.h"
#include "walkstop.h"
#include "facehide.h"
#include "framecost.h"
#include "framemap.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"
#include "nearplane.h"
#include "phone3d.h"
#include "rtti.h"
#include "touch.h"

namespace s4vr::camera {

namespace {

constexpr char kManagerClass[] = ".?AVCameraControllerManager@ClientApp@Sims4@EA@@";
constexpr char kCameraClass[] = ".?AVCamera@Cameras@Sims3@@";
constexpr size_t kManagerCameraOffset = 0x10;
constexpr size_t kFrustumOffset = 0x14;
// The frustum's centre at distance 1 (x right, y up; float2): the game's
// projection (TS4_x64 1.128 0x1208640, from Camera update 0x1208eb0) builds
// left/right = x -/+ width/2 and bottom/top = y -/+ height/2 from it, so a
// lopsided eye needs no patch. The camera copy copies it from the live camera.
constexpr size_t kCentreOffset = 0x1f0;
constexpr size_t kNearOffset = 0x24;  // near clip distance; +0x28 is the far one
constexpr size_t kRightOffset = 0x30, kUpOffset = 0x40, kBackOffset = 0x50, kPositionOffset = 0x60;

// The camera copy function, located by its body 46 bytes in (unique in both
// game executables). The old convexvr mod hooked the same function.
const uint8_t kCopyPattern[] = {0x0f, 0x11, 0x41, 0x30, 0x0f, 0x10, 0x4a, 0x40, 0x0f, 0x11, 0x49, 0x40, 0x0f, 0x10, 0x42, 0x50,
                                0x0f, 0x11, 0x41, 0x50, 0x0f, 0x10, 0x4a, 0x60, 0x0f, 0x11, 0x49, 0x60, 0x0f, 0x10, 0x42, 0x70};
const uint8_t kCopyStart[] = {0x8b, 0x42, 0x10, 0x89, 0x41, 0x10};  // mov eax,[rdx+10h]; mov [rcx+10h],eax
constexpr size_t kCopyPatternOffset = 46;

// Body mode: the game's own skeleton API, as its first-person controller uses
// it (CameraControllerFPSImpl). 0x100 in a pattern matches any byte.
//   object   = lookupObject(&objectId)
//   skeleton = object->vtable[0x20 / 8](object)
//   index    = jointIndex(skeleton + 0x48, fnv(jointName))   (~0u if missing)
//   object->vtable[0x48 / 8](object, index, &transform)      (48 bytes, 16-aligned:
//              float4 translation, float4 rotation xyzw, float4 scale)
const uint16_t kLookupPattern[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0x100, 0x100, 0x100,
                                   0x100, 0x48, 0x85, 0xc0, 0x74, 0x15, 0x48, 0x8b, 0x10, 0x48, 0x8b, 0xc8, 0x4c,
                                   0x8b, 0x42, 0x20, 0x48, 0x8b, 0xd3, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0x49, 0xff, 0xe0};
const uint16_t kJointIndexPattern[] = {0x48, 0x83, 0xec, 0x18, 0x44, 0x8b, 0xc2, 0x48, 0xb8, 0xc5, 0x4e, 0xec, 0xc4,
                                       0x4e, 0xec, 0xc4, 0x4e, 0x44, 0x8b, 0xca, 0x4c, 0x8b, 0xd1, 0x49, 0xf7, 0xe1,
                                       0x48, 0xc1, 0xea, 0x02, 0x48, 0x6b, 0xc2, 0x0d, 0x4c, 0x2b, 0xc0, 0x41, 0x8b,
                                       0xc0, 0x48, 0x83, 0xc0, 0x05, 0x48, 0x8d, 0x14, 0xc1, 0x48, 0x8b, 0x04, 0xc1,
                                       0x48, 0x85, 0xc0, 0x74, 0x15};
constexpr size_t kSkeletonSlot = 0x20 / 8, kJointTransformSlot = 0x48 / 8;
using LookupFn = void* (*)(const uint64_t* id);
using JointIndexFn = uint32_t (*)(void* table, uint32_t hash);
using SkeletonFn = uint8_t* (*)(void* object);
using JointTransformFn = void (*)(void* object, uint32_t index, float* transform);
LookupFn g_lookup = nullptr;
JointIndexFn g_jointIndex = nullptr;
std::atomic<uint64_t> g_bodyId{0};
uint64_t g_bodyCachedId = 0;
uint32_t g_headIndex = ~0u;
uint32_t g_eyeIndex[2] = {~0u, ~0u};  // b__L_Eye__, b__R_Eye__
uint32_t g_handIndex[2] = {~0u, ~0u};  // b__L_Hand__, b__R_Hand__
// The body Sim's hand joints in the world, read on the game thread each frame
// for Python (holding things), which must not touch client objects itself.
struct HandJoints {
    uint64_t id = 0;
    bool ok[2] = {false, false};
    float p[2][3] = {};
    float q[2][4] = {};
    ULONGLONG tick = 0;
};
std::mutex g_handJointMutex;
HandJoints g_handJoints;
// Where the Sim's eyes sit from its head joint while it looks level: up, and
// ahead (horizontally). Measured live; NAN until then.
float g_eyeUp = NAN, g_eyeAhead = NAN;
uint32_t g_rootIndex = ~0u;   // a joint that turns with the whole Sim
int g_facingAxis = -1;        // which of its axes (+X -X +Y -Y +Z -Z) points where the Sim faces
int g_upAxis = -1;            // and which points up its body (toward the head), -1 if not found
// The view lying with the body (Python set_body_view, [body] body_view_anims):
// the upper body's joint and its forward and up axes, found while the Sim stands.
uint32_t g_chestIndex = ~0u;
int g_chestForward = -1, g_chestUp = -1;
int g_chestTries = 0;
std::atomic<bool> g_bodyViewWanted{false};
Quat g_viewFrame{0, 0, 0, 1};  // the view's base as drawn while it isn't level
bool g_viewFrameOn = false;    // g_viewFrame is in use (lying with the body, or easing back)
bool g_viewFrameLead = false;  // the body leads it now
float g_frameYaw = 0, g_framePitch = 0;  // stick turns meanwhile, in the body's frame
constexpr float kFramePitchMax = 1.48f;  // about 85 degrees either way
int g_frameLogs = 0;
constexpr float kViewFrameEase = 0.02f;  // per view drawn: the base follows the body (and comes back) this much; about half a second, so its sway is smoothed out
float g_bodyYaw = NAN;        // smoothed view heading in body mode
float g_bodyYawOffset = 0;    // view heading minus Sim facing, kept from the last stick walk
std::atomic<bool> g_holdTurn{false};
std::atomic<float> g_turnRequest{0};
// Turn lead (user 2026-10-18 15:07Z: with smooth turning the world is steady
// but your Sim and other Sims wobble). Each pair was drawn 4 to 13 degrees of
// turn before the headset showed it, and the turn correction (vr.cpp) can only
// turn the picture back, not move near things the way your eyes move when the
// world turns about your head. While the stick turns in free movement, each
// pair is now drawn at the heading it will have when shown (vr.cpp works out
// how far ahead), so the correction stays small. viewYaw() leaves the lead out.
std::atomic<float> g_leadWanted{0};
float g_turnLead = 0;    // the lead in the heading the view moves on with (game thread, g_poseMutex)
float g_latestLead = 0;  // the lead in g_latestYaw (g_poseMutex)
bool g_lastFree = false;  // free movement carried the last frame
std::atomic<float> g_pitchRequest{0};  // right stick up/down, used only while the view lies with the body
bool g_bodySmoothed = false;
// Your head out of other Sims while the game moves your Sim (WickedWhims
// scenes, seated; user 2026-10-17 05:13Z: "the sim collision isn't active at
// all in wicked whims"): only the view is moved out sideways, eased in and
// out, and your Sim's head stays where the animation puts it.
float g_viewPush[2] = {0, 0};
constexpr float kViewPushMax = 0.22f;   // metres (v0.13.213: 0.10, scenes went 12 cm deep)
constexpr float kViewPushRate = 4.0f;   // 1/s, easing back out
constexpr float kViewPushIn = 12.0f;    // 1/s, while going in deeper
LARGE_INTEGER g_viewPushLast{};
int g_viewPushLogs = 0;
ULONGLONG g_viewPushLogTick = 0;
float g_bodyAnchor[3] = {};
uint64_t g_bodyFrames = 0;
std::atomic<bool> g_playerReset{false};  // body mode changed: start the player afresh
// The Sim's root transform as Python last reported it; used if the joint
// turns out to be relative to the Sim instead of in world space.
std::mutex g_rootMutex;
bool g_rootKnown = false;
float g_rootPosition[3] = {};
Quat g_rootOrientation{0, 0, 0, 1};
int g_jointSpace = 0;  // 0 unknown, 1 world, 2 relative to the Sim

Config g_config;
uintptr_t g_cameraVTable = 0;
std::atomic<uintptr_t> g_manager{0};
std::atomic<bool> g_searching{false};

using CopyFn = void* (*)(void* dst, const void* src);
CopyFn g_originalCopy = nullptr;
std::mutex g_hookMutex;
bool g_hooked = false;
std::atomic<CopyObserver> g_observer{nullptr};

// Eye pose for the next frame, written by the Present thread and read by the
// game thread inside the copy hook.
std::mutex g_poseMutex;
bool g_active = false;
Rendered g_pending{};
// Both real eyes every frame (setBothEyes): the right eye's pose for the second pass, the count of
// apply() calls (so secondEye's restore never undoes a newer left eye), g_applied at the last
// scene render, and the frustum applyEye last wrote (+0x14..+0x20).
std::atomic<bool> g_bothEyes{false};
Rendered g_right{};
bool g_rightValid = false;
uint64_t g_applyGen = 0;
uint64_t g_appliedAtScene = 0;
float g_writtenFrustum[4] = {};
// The render camera our eye pose went into, and the eye positions written
// (left, right): the face is hidden only in passes drawn from them.
std::atomic<uintptr_t> g_writtenCamera{0};
std::atomic<float> g_writtenEye[2][3] = {{NAN, NAN, NAN}, {NAN, NAN, NAN}};
Vec3 g_headCentre{0, 0, 0};  // headset centre (between the eyes) for the frame being rendered
Quat g_headOrientation{0, 0, 0, 1};
// Frustum for the game camera; tanX = 0 leaves the game's own values.
float g_tanX = 0, g_tanY = 0;
// Its centre (kCentreOffset) when each eye gets its own lopsided frustum
// (Config::exactFov), and the centred frustum that covers it (what a centred
// test of what's in view needs).
bool g_centred = true;
float g_centreX = 0, g_centreY = 0, g_coverX = 0, g_coverY = 0;
std::atomic<bool> g_exactFov{true};
// The centre applyEye last wrote into the render camera (NAN: none, centred).
// The projection build gets it again (hookedProjection): something between
// the copy and the camera's update puts the live camera's 0,0 back (v0.13.239
// log: the drawn projection had no shift, so the eyes looked crossed).
std::atomic<float> g_appliedCentre[2] = {NAN, NAN};
// The centre applyEye last wrote for each eye (NAN: none yet). The game's
// cameras keep it once the eyes stop (CAS, god view's flat screen) or the
// switch goes off: the camera copy carries +0x1f0 over from the live camera,
// which had picked up an eye's centre, and CAS drew its Sim off to the side
// with release() alone (user 2026-10-18 21:59Z, switch on and off).
std::atomic<float> g_eyeCentre[2][2] = {{NAN, NAN}, {NAN, NAN}};
int g_oldCentreLogs = 0;
// Copies the hook changed and Present hasn't consumed yet.
Rendered g_queue[4];
size_t g_queueHead = 0, g_queueSize = 0;
uint64_t g_applied = 0;
// Where app space sat in the world for the latest changed frame.
bool g_anchorValid = false;
float g_anchorPosition[3] = {};
Quat g_anchorHeading{0, 0, 0, 1};
std::atomic<float> g_floorY{NAN};
std::atomic<float> g_latestYaw{NAN};  // the heading the newest frame was rendered with (viewYaw)
// Paired eyes (first person). The game draws one eye per frame, so walking
// moved the view a few centimetres between the left and the right eye's
// pictures: double vision (user 2026-10-14). A right eye straight after a left
// one is drawn from that left eye's place and heading, and the body isn't
// moved in between; Present shows only whole pairs (vr.cpp renderStereo).
std::atomic<bool> g_pairedEyes{true};
struct PairBase {
    bool valid = false;
    bool freeMove = false;
    float yaw = 0;
    float lead = 0;  // the turn lead in yaw
    Quat heading{0, 0, 0, 1};  // the view's base (level at yaw, or lying with the body)
    float ahead = NAN;  // the neck's reach ahead (body mode without free movement), NAN without
    float position[3] = {};
};
PairBase g_pairBase;
int g_lastAppliedEye = -1;
uint64_t g_pairs = 0;
// Hands with paired eyes (user 2026-10-14 19:59Z: "my hands are jiggling when
// I move around"). The game poses the body (the arm IK too) before the camera
// copy, from where the view was on the frame before. A left eye moved the view
// on by a pair but its hands were posed from two frames back; the right eye's
// were right: the hands sat in different places in the two eyes. So the right
// eye of a free-moving pair works out where the next pair is drawn from: the
// body is pinned there (the game's root motion puts it there as it poses the
// Sim, player::settle on the left eye otherwise) and the arms reach from
// there, and that left eye is then drawn from that very place.
std::atomic<bool> g_pairedHands{true};
std::atomic<bool> g_bodyLayer{true};  // Rendered::body filled in (bodyParts)
struct NextBase {
    bool valid = false;
    void* actor = nullptr;
    float yaw = 0;
    float lead = 0;  // the turn lead in yaw
    float position[3] = {};
};
NextBase g_nextBase;
uint64_t g_aheadPairs = 0, g_aheadSettled = 0, g_aheadPairsLogged = 0;
ULONGLONG g_aheadLogTick = 0;
int g_aheadLogs = 0;

// Audio listener. The game places its listener from its own live camera (near
// the point it looks at), which in VR is metres from your head. We hand it the
// headset pose instead: its orientation always, its position in body mode.
struct ListenerPose {
    Vec3 position, right, up, back;
    bool body = false;
    ULONGLONG tick = 0;
    float root[3] = {};  // where your Sim stands (its object's place), when rooted
    bool rooted = false;
};
constexpr size_t kActorPlace = 0x2e0;  // an actor's world position (armik kActorPosition)
std::mutex g_listenerMutex;
std::mutex g_viewMutex;
View g_view{};
bool g_viewValid = false;
ListenerPose g_listener;

bool safeRead(uintptr_t address, void* out, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &read) && read == size;
}

// The manager's camera, if it still looks like a live camera object.
uintptr_t liveCamera(uintptr_t manager) {
    uintptr_t camera = 0, vtable = 0;
    float w = 0;
    if (!manager || !safeRead(manager + kManagerCameraOffset, &camera, sizeof(camera))) return 0;
    if (!camera || !safeRead(camera, &vtable, sizeof(vtable)) || vtable != g_cameraVTable) return 0;
    if (!safeRead(camera + kPositionOffset + 12, &w, sizeof(w)) || w != 1.0f) return 0;
    return camera;
}

uintptr_t findPattern(HMODULE module, const uint8_t* pattern, size_t size) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* begin = base + section->VirtualAddress;
        uint8_t* end = begin + section->Misc.VirtualSize - size;
        for (uint8_t* p = begin; p <= end; ++p) {
            if (*p == pattern[0] && std::memcmp(p, pattern, size) == 0) return reinterpret_cast<uintptr_t>(p);
        }
    }
    return 0;
}

uintptr_t findMasked(HMODULE module, const uint16_t* pattern, size_t size) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* begin = base + section->VirtualAddress;
        uint8_t* end = begin + section->Misc.VirtualSize - size;
        for (uint8_t* p = begin; p <= end; ++p) {
            size_t k = 0;
            while (k < size && (pattern[k] > 0xff || p[k] == pattern[k])) ++k;
            if (k == size) return reinterpret_cast<uintptr_t>(p);
        }
    }
    return 0;
}

// The game's joint name hash: FNV-1 over lower-case characters.
uint32_t jointHash(const char* name) {
    uint32_t h = 0x811c9dc5;
    for (; *name; ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h = (h * 0x1000193) ^ static_cast<uint8_t>(c);
    }
    return h;
}

template <typename T>
T vcall(void* object, size_t slot) {
    return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[slot]);
}

bool findSkeletonApi() {
    HMODULE game = GetModuleHandleW(nullptr);
    auto lookup = findMasked(game, kLookupPattern, sizeof(kLookupPattern) / 2);
    auto index = findMasked(game, kJointIndexPattern, sizeof(kJointIndexPattern) / 2);
    auto base = reinterpret_cast<uintptr_t>(game);
    LOG("Skeleton API: object lookup %s game+0x%llx, joint index %s game+0x%llx", lookup ? "at" : "NOT FOUND",
        static_cast<unsigned long long>(lookup ? lookup - base : 0), index ? "at" : "NOT FOUND",
        static_cast<unsigned long long>(index ? index - base : 0));
    g_lookup = reinterpret_cast<LookupFn>(lookup);
    g_jointIndex = reinterpret_cast<JointIndexFn>(index);
    return lookup && index;
}

Vec3 rotate(const Quat& q, const Vec3& v);

// View heading (camera yaw, as atan2 of its back axis) for where the Sim
// faces, from the root joint. Which joint axis is "forward" is found once by
// comparing with the orientation Python reports for the Sim.
float facingYaw(void* object) {
    alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    vcall<JointTransformFn>(object, kJointTransformSlot)(object, g_rootIndex, t);
    Quat q{t[4], t[5], t[6], t[7]};
    if (!std::isfinite(q.x) || !std::isfinite(q.w) || std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) > 0.05f)
        return NAN;
    static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    if (g_facingAxis < 0) {
        Quat root;
        {
            std::lock_guard<std::mutex> lock(g_rootMutex);
            if (!g_rootKnown) return NAN;
            root = g_rootOrientation;
        }
        Vec3 f = rotate(root, {0, 0, 1});  // the game's forward axis is +Z
        float fl = std::sqrt(f.x * f.x + f.z * f.z);
        if (fl < 0.5f) return NAN;
        float best = 0.9f;
        for (int i = 0; i < 6; ++i) {
            Vec3 v = rotate(q, kAxes[i]);
            float l = std::sqrt(v.x * v.x + v.z * v.z);
            if (l < 0.5f) continue;
            float score = (v.x * f.x + v.z * f.z) / (l * fl);
            if (score > best) {
                best = score;
                g_facingAxis = i;
            }
        }
        // Up the body: the axis pointing up while the Sim stands.
        g_upAxis = -1;
        float upBest = 0.8f;
        for (int i = 0; g_facingAxis >= 0 && i < 6; ++i) {
            if (i / 2 == g_facingAxis / 2) continue;
            float y = rotate(q, kAxes[i]).y;
            if (y > upBest) {
                upBest = y;
                g_upAxis = i;
            }
        }
        static int logged = 0;
        if (g_facingAxis >= 0 || logged++ < 3)
            LOG("Body facing axis: %d (match %.2f, joint q %.3f %.3f %.3f %.3f, Sim forward %.2f %.2f), up axis %d", g_facingAxis,
                best, q.x, q.y, q.z, q.w, f.x / fl, f.z / fl, g_upAxis);
        if (g_facingAxis < 0) return NAN;
    }
    Vec3 v = rotate(q, kAxes[g_facingAxis]);
    // Lying down or leaning far back (a bed, WickedWhims animations), its
    // forward points up or down and says little about where it faces. Tilted
    // by the body's up axis the heading stays the way the body points: toward
    // its feet when lying on its back, its head when lying face down, and
    // unchanged when it stands or lies on its side.
    const char* kind = "forward";
    if (g_upAxis >= 0) {
        const Vec3 u = rotate(q, kAxes[g_upAxis]);
        if (std::fabs(v.y) > 0.3f) kind = "tilted";
        v = {v.x - v.y * u.x, 0, v.z - v.y * u.z};
    }
    const bool ok = v.x * v.x + v.z * v.z >= 0.25f;
    static const char* lastKind = "";
    static int kindLogs = 0;
    const char* now = ok ? kind : "none (the view keeps its heading)";
    if (std::strcmp(now, lastKind) != 0) {
        lastKind = now;
        if (kindLogs++ < 40) LOG("Body facing: %s (joint q %.3f %.3f %.3f %.3f)", now, q.x, q.y, q.z, q.w);
    }
    if (!ok) return NAN;
    return std::atan2(-v.x, -v.z);  // the camera looks along -back
}

// World position of the body's head joint. Runs on the game thread inside the
// camera copy, where the game's own first-person camera reads the same joint.
// eyes: the midpoint between its eyes, NAN when the rig has none.
bool headPosition(float out[3], float* facing, uint8_t** actor, float* eyes) {
    if (g_playerReset.exchange(false)) player::reset();
    uint64_t id = g_bodyId.load();
    if (!id || !g_lookup || !g_jointIndex) return false;
    void* object = g_lookup(&id);
    if (!object) return false;
    if (id != g_bodyCachedId) {
        player::reset();
        uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
        if (!skeleton) return false;
        g_headIndex = g_jointIndex(skeleton + 0x48, jointHash("b__Head__"));
        g_eyeIndex[0] = g_jointIndex(skeleton + 0x48, jointHash("b__L_Eye__"));
        g_eyeIndex[1] = g_jointIndex(skeleton + 0x48, jointHash("b__R_Eye__"));
        g_handIndex[0] = g_jointIndex(skeleton + 0x48, jointHash("b__L_Hand__"));
        g_handIndex[1] = g_jointIndex(skeleton + 0x48, jointHash("b__R_Hand__"));
        g_eyeUp = g_eyeAhead = NAN;
        LOG("Body eyes: joints %d %d%s", static_cast<int>(g_eyeIndex[0]), static_cast<int>(g_eyeIndex[1]),
            g_eyeIndex[0] == ~0u || g_eyeIndex[1] == ~0u ? " (not found: [body] forward/height place the view)" : "");
        g_rootIndex = ~0u;
        for (const char* name : {"b__ROOT_bind__", "b__ROOT__", "b__Pelvis__"}) {
            g_rootIndex = g_jointIndex(skeleton + 0x48, jointHash(name));
            if (g_rootIndex != ~0u) {
                LOG("Body facing from joint %s (index %d)", name, static_cast<int>(g_rootIndex));
                break;
            }
        }
        g_facingAxis = -1;
        g_upAxis = -1;
        g_chestIndex = g_jointIndex(skeleton + 0x48, jointHash("b__Spine2__"));
        g_chestForward = g_chestUp = -1;
        g_chestTries = 0;
        g_bodyYaw = NAN;
        g_bodyYawOffset = 0;
        g_bodyCachedId = id;
        g_bodySmoothed = false;
        LOG("Body %llu: object %p, head joint index %d", static_cast<unsigned long long>(id), object,
            static_cast<int>(g_headIndex));
    }
    if (g_headIndex == ~0u) return false;
    crashlog::note(crashlog::kJointRead);
    alignas(16) float transform[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    vcall<JointTransformFn>(object, kJointTransformSlot)(object, g_headIndex, transform);
    if (!std::isfinite(transform[0]) || !std::isfinite(transform[1]) || !std::isfinite(transform[2])) return false;
    std::memcpy(out, transform, 3 * sizeof(float));
    if (eyes) {
        eyes[0] = eyes[1] = eyes[2] = NAN;
        if (g_eyeIndex[0] != ~0u && g_eyeIndex[1] != ~0u) {
            float sum[3] = {0, 0, 0};
            bool ok = true;
            for (uint32_t index : g_eyeIndex) {
                alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
                vcall<JointTransformFn>(object, kJointTransformSlot)(object, index, t);
                ok = ok && std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]);
                for (int k = 0; k < 3; ++k) sum[k] += t[k];
            }
            // An eye more than 40 cm from the head joint isn't one.
            float dx = sum[0] / 2 - out[0], dy = sum[1] / 2 - out[1], dz = sum[2] / 2 - out[2];
            if (ok && dx * dx + dy * dy + dz * dz < 0.16f * g_config.worldScale * g_config.worldScale)
                for (int k = 0; k < 3; ++k) eyes[k] = sum[k] / 2;
        }
    }
    if (facing && g_rootIndex != ~0u) *facing = facingYaw(object);
    if (g_jointSpace == 1) {
        HandJoints hands;
        hands.id = id;
        hands.tick = GetTickCount64();
        for (int i = 0; i < 2; ++i) {
            if (g_handIndex[i] == ~0u) continue;
            alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
            vcall<JointTransformFn>(object, kJointTransformSlot)(object, g_handIndex[i], t);
            bool ok = true;
            for (int k = 0; k < 8; ++k) ok = ok && std::isfinite(t[k]);
            if (!ok) continue;
            hands.ok[i] = true;
            std::memcpy(hands.p[i], t, sizeof(hands.p[i]));
            std::memcpy(hands.q[i], t + 4, sizeof(hands.q[i]));
        }
        std::lock_guard<std::mutex> lock(g_handJointMutex);
        g_handJoints = hands;
    }
    facehide::setBody(object);
    if (actor) *actor = static_cast<uint8_t*>(object);
    if (++g_bodyFrames <= 3 || g_bodyFrames % 2000 == 0) {
        LOG("Head joint: t %.3f %.3f %.3f  q %.3f %.3f %.3f %.3f", transform[0], transform[1], transform[2],
            transform[4], transform[5], transform[6], transform[7]);
    }
    return true;
}

// Several manager objects exist (menus, CAS, build); the live one points at a
// Camera whose transform has w = 1.
void searchManager() {
    if (g_searching.exchange(true)) return;
    std::thread([] {
        HMODULE game = GetModuleHandleW(nullptr);
        uintptr_t managerVTable = rtti::findVTable(game, kManagerClass);
        g_cameraVTable = rtti::findVTable(game, kCameraClass);
        if (!managerVTable || !g_cameraVTable) {
            LOG("Camera classes not found in this game version; stereo mode unavailable");
            return;  // g_searching stays set: no point retrying
        }
        for (int attempt = 0; !g_manager; ++attempt) {
            for (uintptr_t manager : rtti::findInstances(managerVTable, 32)) {
                if (uintptr_t camera = liveCamera(manager)) {
                    g_manager = manager;
                    LOG("Game camera found: manager %p, camera %p", reinterpret_cast<void*>(manager),
                        reinterpret_cast<void*>(camera));
                    break;
                }
            }
            if (!g_manager) {
                if (attempt == 0) LOG("Game camera not found yet; retrying every 5 s");
                Sleep(5000);
            }
        }
        g_searching = false;
    }).detach();
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w(u x v) + 2u x (u x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

Quat multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Joint positions are expected in world space. If the first one lands far
// from where Python says the Sim is, treat them as relative to the Sim.
bool toWorld(float head[3]) {
    std::lock_guard<std::mutex> lock(g_rootMutex);
    if (g_jointSpace == 0) {
        if (!g_rootKnown) return true;  // nothing to compare with yet
        float dx = head[0] - g_rootPosition[0], dz = head[2] - g_rootPosition[2];
        g_jointSpace = dx * dx + dz * dz < 4.0f ? 1 : 2;
        LOG("Head joint is %s (head %.2f %.2f %.2f, Sim at %.2f %.2f %.2f)",
            g_jointSpace == 1 ? "in world space" : "relative to the Sim", head[0], head[1], head[2], g_rootPosition[0],
            g_rootPosition[1], g_rootPosition[2]);
    }
    if (g_jointSpace == 2) {
        if (!g_rootKnown) return false;
        Vec3 w = rotate(g_rootOrientation, {head[0], head[1], head[2]});
        head[0] = g_rootPosition[0] + w.x;
        head[1] = g_rootPosition[1] + w.y;
        head[2] = g_rootPosition[2] + w.z;
    }
    return true;
}

void writeAxis(uint8_t* camera, size_t offset, const Vec3& v, float w) {
    float f[4] = {v.x, v.y, v.z, w};
    std::memcpy(camera + offset, f, sizeof(f));
}

void checkHeld();
bool fingertipWorld(void* object, int hand, Vec3& out);

// While you look about level (so the Sim's head, following yours, is level
// too): how far above and ahead of the head joint its eyes are.
void measureEyes(const float head[3], const float eyes[3]) {
    Vec3 f = rotate(g_headOrientation, {0, 0, -1});
    if (std::fabs(f.y) > 0.26f) return;  // more than about 15 degrees up or down
    float up = eyes[1] - head[1];
    float ahead = std::sqrt((eyes[0] - head[0]) * (eyes[0] - head[0]) + (eyes[2] - head[2]) * (eyes[2] - head[2]));
    bool first = !std::isfinite(g_eyeUp);
    g_eyeUp = first ? up : g_eyeUp + (up - g_eyeUp) * 0.02f;
    g_eyeAhead = first ? ahead : g_eyeAhead + (ahead - g_eyeAhead) * 0.02f;
    if (first) LOG("Body eyes: %.3f above and %.3f ahead of the head joint", up, ahead);
}

// Replaces the render camera's transform with: game camera position and
// heading (yaw only, so the horizon stays level), plus the eye pose.
// Your own body for the near limit (nearplane.h): its joints as chains of
// spheres a little wider than the limbs, the hands with room for what they
// hold; the head, which the view sits inside, only gives headEnd: the view
// depth where it ends ahead of you (0 not known). Returns the
// nearest view depth of what must never be cut off (+inf: none in view; 0: not
// known): hands and legs whole; torso, shoulders and arms only from
// kBodyClipNear on (where they enter the view at its edge, right under your
// chin, they may be cut). The chains also go to nearplane::ownBody so the
// measured scene leaves your body out.
const char* const kBodyJoints[] = {"b__Pelvis__",     "b__Spine0__",     "b__Spine1__",    "b__Spine2__",
                                   "b__L_UpperArm__", "b__R_UpperArm__", "b__L_Forearm__", "b__R_Forearm__",
                                   "b__L_Hand__",     "b__R_Hand__",     "b__L_Thigh__",   "b__R_Thigh__",
                                   "b__L_Calf__",     "b__R_Calf__",     "b__L_Foot__",    "b__R_Foot__",
                                   "b__Neck__",       "b__Head__"};
constexpr int kNeckJoint = 16, kHeadJoint = 17;
constexpr float kHeadRadius = 0.15f;  // metres around the head joint: face, eyes, teeth, jaw
constexpr int kBodyJointCount = sizeof(kBodyJoints) / sizeof(kBodyJoints[0]);
struct BodyBone {
    int from, to;  // into kBodyJoints; to < 0: one sphere at `from`
    float radius;  // metres
    bool clip;     // may be cut off closer than kBodyClipNear
};
constexpr BodyBone kBodyBones[] = {
    {0, 1, 0.17f, true},   {1, 2, 0.17f, true},   {2, 3, 0.17f, true},                          // torso
    {3, 4, 0.09f, true},   {3, 5, 0.09f, true},                                                 // shoulders
    {4, 6, 0.07f, true},   {5, 7, 0.07f, true},   {6, 8, 0.06f, true},   {7, 9, 0.06f, true},   // arms
    {8, -1, 0.2f, false},  {9, -1, 0.2f, false},                                                // hands and what they hold
    {0, 10, 0.12f, false}, {0, 11, 0.12f, false}, {10, 12, 0.1f, false}, {11, 13, 0.1f, false}, // hips, thighs
    {12, 14, 0.08f, false}, {13, 15, 0.08f, false}, {14, -1, 0.15f, false}, {15, -1, 0.15f, false},  // calves, feet
};
static_assert(sizeof(kBodyBones) / sizeof(kBodyBones[0]) + 1 <= nearplane::kMaxCapsules, "too many body capsules");
constexpr float kPieHandReach = 0.14f;   // metres from the wrist joint along the hand, for the pie menu ...
constexpr float kPieHandRadius = 0.08f;  // ... and around it: palm and fingers, however the wrist bends
constexpr float kBodyClipNear = 0.15f;  // metres: torso and arms may be cut off closer than this
// Added to the view's half-width and half-height (tangent) when your body is checked against it: the
// near limit is chosen once for both eyes from the left eye, so a hand at the edge only the right eye
// sees (or one inside the eye offset) must still count, else it is cut off there.
constexpr float kBodyViewPad = 0.2f;
constexpr float kBodyMargin = 0.06f;    // metres added around your body when the measurement leaves it out
                                        // (the other eye, a frame of movement)

// The nearest view depth of the part of a sphere inside the view (+inf: none
// of it). Sides of the view: planes through the eye; where the sphere's
// nearest point is past one, the nearest inside lies on that plane.
float sphereInView(float x, float y, float z, float r, float tanX, float tanY, float secX, float secY) {
    if (z + r <= 0) return std::numeric_limits<float>::infinity();  // behind the eye
    const float sides[4][3] = {{1 / secX, 0, tanX / secX},
                               {-1 / secX, 0, tanX / secX},
                               {0, 1 / secY, tanY / secY},
                               {0, -1 / secY, tanY / secY}};  // inward normals
    float nearest = z - r;
    for (const auto& n : sides) {
        float s = n[0] * x + n[1] * y + n[2] * z;  // the centre's distance inside this side
        if (s < -r) return std::numeric_limits<float>::infinity();  // wholly outside
        if (s - r * n[2] >= 0) continue;  // the sphere's nearest point is inside this side
        float rho = std::sqrt(std::max(0.f, r * r - s * s));  // the circle the side cuts out
        float qz = z - s * n[2];
        nearest = std::max(nearest, qz - rho * std::sqrt(std::max(0.f, 1 - n[2] * n[2])));
    }
    return nearest;
}
uint32_t g_bodyJoints[kBodyJointCount];
uint64_t g_bodyJointsId = 0;
// Your forearms as last read (forearm()): elbow and wrist joints, world.
std::mutex g_forearmMutex;
float g_forearm[2][2][3] = {};
bool g_forearmOk[2] = {};
ULONGLONG g_forearmTick = 0;
// Your torso as last read (torso()): pelvis, spine 0-2, left and right upper arm, world.
float g_torso[6][3] = {};
bool g_torsoOk = false;

float bodyNearest(void* object, const float eye[3], const Vec3& right, const Vec3& up, const Vec3& ahead, float tanX,
                  float tanY, nearplane::Capsule* capsules, int& count, float& headEnd) {
    count = 0;
    headEnd = 0;
    if (g_jointSpace != 1 || !g_jointIndex || !object || !(tanX > 0 && tanY > 0)) return 0;
    uint64_t id = g_bodyId.load();
    if (id != g_bodyJointsId) {
        uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
        if (!skeleton) return 0;
        int missing = 0;
        for (int i = 0; i < kBodyJointCount; ++i) {
            g_bodyJoints[i] = g_jointIndex(skeleton + 0x48, jointHash(kBodyJoints[i]));
            if (g_bodyJoints[i] == ~0u) ++missing;
        }
        g_bodyJointsId = id;
        LOG("Near limit: own body from %d joints%s", kBodyJointCount - missing,
            missing ? " (some not found: those limbs are left out)" : "");
    }
    Vec3 p[kBodyJointCount];
    bool ok[kBodyJointCount] = {};
    for (int i = 0; i < kBodyJointCount; ++i) {
        if (g_bodyJoints[i] == ~0u) continue;
        alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
        vcall<JointTransformFn>(object, kJointTransformSlot)(object, g_bodyJoints[i], t);
        ok[i] = std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]);
        p[i] = {t[0], t[1], t[2]};
    }
    if (!ok[0] || !ok[3]) return 0;  // no torso: not known
    // Your arms and hands for the pie menu, which is drawn in front of
    // everything else (phone3d psPie): upper arm, forearm, and the hand on out
    // past the fingertips along the forearm.
    {
        phone3d::HandCapsule arms[6];
        int armCount = 0;
        for (int side = 0; side < 2; ++side) {
            const int shoulder = 4 + side, elbow = 6 + side, wrist = 8 + side;
            if (ok[shoulder] && ok[elbow]) arms[armCount++] = {{p[shoulder].x, p[shoulder].y, p[shoulder].z}, {p[elbow].x, p[elbow].y, p[elbow].z}, 0.08f};
            if (!ok[elbow] || !ok[wrist]) continue;
            const Vec3 &e = p[elbow], &w = p[wrist];
            arms[armCount++] = {{e.x, e.y, e.z}, {w.x, w.y, w.z}, 0.07f};
            Vec3 d{w.x - e.x, w.y - e.y, w.z - e.z};
            float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (length < 1e-4f) continue;
            const float reach = kPieHandReach / length;
            arms[armCount++] = {{w.x, w.y, w.z}, {w.x + d.x * reach, w.y + d.y * reach, w.z + d.z * reach}, kPieHandRadius};
        }
        phone3d::setHands(arms, armCount);
        std::lock_guard<std::mutex> lock(g_forearmMutex);
        for (int side = 0; side < 2; ++side) {
            const int elbow = 6 + side, wrist = 8 + side;
            g_forearmOk[side] = ok[elbow] && ok[wrist];
            if (!g_forearmOk[side]) continue;
            const float e[3] = {p[elbow].x, p[elbow].y, p[elbow].z}, w[3] = {p[wrist].x, p[wrist].y, p[wrist].z};
            std::memcpy(g_forearm[side][0], e, sizeof(e));
            std::memcpy(g_forearm[side][1], w, sizeof(w));
        }
        g_torsoOk = ok[0] && ok[1] && ok[2] && ok[3] && ok[4] && ok[5];
        for (int k = 0; k < 6 && g_torsoOk; ++k) {
            g_torso[k][0] = p[k].x;
            g_torso[k][1] = p[k].y;
            g_torso[k][2] = p[k].z;
        }
        g_forearmTick = GetTickCount64();
    }
    const float secX = std::sqrt(1 + tanX * tanX), secY = std::sqrt(1 + tanY * tanY);
    float nearest = std::numeric_limits<float>::infinity();
    auto sphere = [&](const Vec3& c, float r, bool clip) {
        const Vec3 v{c.x - eye[0], c.y - eye[1], c.z - eye[2]};
        float z = v.x * ahead.x + v.y * ahead.y + v.z * ahead.z;
        float x = v.x * right.x + v.y * right.y + v.z * right.z;
        float y = v.x * up.x + v.y * up.y + v.z * up.z;
        float n = sphereInView(x, y, z, r, tanX, tanY, secX, secY);
        nearest = std::min(nearest, clip ? std::max(n, kBodyClipNear) : n);
    };
    for (const auto& b : kBodyBones) {
        if (!ok[b.from] || (b.to >= 0 && !ok[b.to])) continue;
        const Vec3 a = p[b.from], c = b.to < 0 ? a : p[b.to];
        nearplane::Capsule& capsule = capsules[count++];
        capsule = {{a.x, a.y, a.z}, {c.x, c.y, c.z}, b.radius + kBodyMargin};
        if (b.to < 0) {
            sphere(a, b.radius, b.clip);
            continue;
        }
        float length = std::sqrt((c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y) + (c.z - a.z) * (c.z - a.z));
        int steps = std::min(8, std::max(1, static_cast<int>(std::ceil(length / b.radius))));
        for (int k = 0; k <= steps; ++k) {
            float t = static_cast<float>(k) / steps;
            sphere({a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t, a.z + (c.z - a.z) * t}, b.radius, b.clip);
        }
    }
    // Your head: the view sits inside it. Where it ends ahead of you, the near
    // limit cuts away its inside (eyes, teeth) even when the game's animation
    // moves it in front of the view; hair farther out stays.
    if (ok[kHeadJoint]) {
        const Vec3& h = p[kHeadJoint];
        const Vec3 v{h.x - eye[0], h.y - eye[1], h.z - eye[2]};
        if (v.x * v.x + v.y * v.y + v.z * v.z < kHeadRadius * kHeadRadius * 4)  // the view is in or by it
            headEnd = std::max(0.f, v.x * ahead.x + v.y * ahead.y + v.z * ahead.z + kHeadRadius);
        const Vec3& n = ok[kNeckJoint] ? p[kNeckJoint] : h;
        capsules[count++] = {{n.x, n.y, n.z}, {h.x, h.y, h.z}, kHeadRadius + kBodyMargin};
    }
    return nearest;
}

// Your own body as the game drew it in this view, for the body layer
// (Rendered::body, vr.cpp): tighter than the near limit's chains, since what
// they take in goes with you when you turn (a table under your hand would turn
// with you): torso, arms, palms, each finger in two pieces, legs, head.
// Joints read here are the pose this frame is drawn with; kSplitMargin covers
// a frame of movement.
constexpr int kSplitFingerJoint = kBodyJointCount;  // finger joints follow the body's: [side][finger][0..2]
constexpr int kSplitJointCount = kBodyJointCount + 2 * 5 * 3 + 2;  // + toes
constexpr int kSplitToeJoint = kBodyJointCount + 30;
constexpr float kSplitMargin = 0.035f;
struct SplitBone {
    int from, to;  // into the joints above; to < 0: one sphere at `from`
    float radius;
};
constexpr SplitBone kSplitBones[] = {
    {0, 1, 0.15f},   {1, 2, 0.15f},   {2, 3, 0.15f},                      // torso
    {3, 4, 0.08f},   {3, 5, 0.08f},                                       // shoulders
    {4, 6, 0.065f},  {5, 7, 0.065f},  {6, 8, 0.055f},  {7, 9, 0.055f},    // arms
    {0, 10, 0.11f},  {0, 11, 0.11f},  {10, 12, 0.09f}, {11, 13, 0.09f},   // hips, thighs
    {12, 14, 0.065f}, {13, 15, 0.065f},                                   // calves
    {16, 17, 0.13f},                                                      // neck, head
};
uint32_t g_splitJoints[kSplitJointCount];
uint64_t g_splitJointsId = 0;
constexpr float kSplitHeld = 0.15f;  // metres around the palm while the game's animation holds something there
extern bool g_heldByCurl[2];

void bodyParts(void* object, const float eye[3], const Vec3& right, const Vec3& up, const Vec3& ahead, float s,
               BodyParts& out) {
    out.count = 0;
    if (g_jointSpace != 1 || !g_jointIndex || !object) return;
    uint64_t id = g_bodyId.load();
    if (id != g_splitJointsId) {
        uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
        if (!skeleton) return;
        int missing = 0;
        auto find = [&](const char* name) {
            uint32_t j = g_jointIndex(skeleton + 0x48, jointHash(name));
            if (j == ~0u) ++missing;
            return j;
        };
        for (int i = 0; i < kBodyJointCount; ++i) g_splitJoints[i] = find(kBodyJoints[i]);
        const char* const fingers[5] = {"Thumb", "Index", "Mid", "Ring", "Pinky"};
        for (int side = 0; side < 2; ++side) {
            for (int f = 0; f < 5; ++f)
                for (int k = 0; k < 3; ++k) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "b__%s_%s%d__", side ? "R" : "L", fingers[f], k);
                    g_splitJoints[kSplitFingerJoint + side * 15 + f * 3 + k] = find(name);
                }
            g_splitJoints[kSplitToeJoint + side] = find(side ? "b__R_Toe__" : "b__L_Toe__");
        }
        g_splitJointsId = id;
        LOG("Body layer: your body from %d of %d joints", kSplitJointCount - missing, kSplitJointCount);
    }
    Vec3 p[kSplitJointCount];
    bool ok[kSplitJointCount] = {};
    for (int i = 0; i < kSplitJointCount; ++i) {
        if (g_splitJoints[i] == ~0u) continue;
        alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
        vcall<JointTransformFn>(object, kJointTransformSlot)(object, g_splitJoints[i], t);
        ok[i] = std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]);
        p[i] = {t[0], t[1], t[2]};
    }
    if (!ok[0] || !ok[3]) return;  // no torso: not known
    auto add = [&](const Vec3& a, const Vec3& b, float radius) {
        if (out.count >= kBodyParts) return;
        auto toView = [&](const Vec3& w, float* v) {
            const Vec3 d{w.x - eye[0], w.y - eye[1], w.z - eye[2]};
            v[0] = d.x * right.x + d.y * right.y + d.z * right.z;
            v[1] = d.x * up.x + d.y * up.y + d.z * up.z;
            v[2] = d.x * ahead.x + d.y * ahead.y + d.z * ahead.z;
        };
        toView(a, out.a[out.count]);
        toView(b, out.b[out.count]);
        out.a[out.count][3] = (radius + kSplitMargin) * s;
        out.b[out.count][3] = 0;
        ++out.count;
    };
    for (const auto& b : kSplitBones) {
        if (!ok[b.from] || (b.to >= 0 && !ok[b.to])) continue;
        add(p[b.from], b.to < 0 ? p[b.from] : p[b.to], b.radius);
    }
    for (int side = 0; side < 2; ++side) {
        const int wrist = 8 + side, foot = 14 + side, toe = kSplitToeJoint + side;
        auto finger = [&](int f, int k) { return kSplitFingerJoint + side * 15 + f * 3 + k; };
        if (ok[foot]) add(p[foot], ok[toe] ? p[toe] : p[foot], ok[toe] ? 0.06f : 0.12f);  // foot, or the ankle
        if (!ok[wrist]) continue;
        // The palm: wrist to the middle knuckle, and across the knuckles.
        if (ok[finger(2, 0)]) add(p[wrist], p[finger(2, 0)], 0.045f);
        else add(p[wrist], p[wrist], 0.1f);
        // What the hand holds (a remote, a cup) goes with it.
        if (g_heldByCurl[side] && ok[finger(2, 0)]) {
            const Vec3 c{(p[wrist].x + p[finger(2, 0)].x) / 2, (p[wrist].y + p[finger(2, 0)].y) / 2,
                         (p[wrist].z + p[finger(2, 0)].z) / 2};
            add(c, c, kSplitHeld);
        }
        if (ok[finger(1, 0)] && ok[finger(4, 0)]) add(p[finger(1, 0)], p[finger(4, 0)], 0.03f);
        for (int f = 0; f < 5; ++f) {
            const int j0 = finger(f, 0), j1 = finger(f, 1), j2 = finger(f, 2);
            if (!ok[j0] || !ok[j1]) continue;
            const float r = f == 0 ? 0.022f : 0.018f;
            add(p[j0], p[j1], r);
            if (!ok[j2]) continue;
            // The tip: the last bone is about as long as the one before it.
            const Vec3 d{p[j2].x - p[j1].x, p[j2].y - p[j1].y, p[j2].z - p[j1].z};
            add(p[j1], {p[j2].x + d.x * 0.9f, p[j2].y + d.y * 0.9f, p[j2].z + d.z * 0.9f}, r);
        }
    }
}

// Seated, and in the game's other animations (seatView). Where your neck was
// when the Sim sat down (app space, metres): your head's moves from there lean
// the Sim (armik setLean). Past what a seated body can lean, where you sat
// moves along: a real move (you sat down or stood up for real, stepped away)
// costs no more than that.
struct Seat {
    bool on = false;
    Vec3 neutral{0, 0, 0}, last{0, 0, 0};
    uint64_t frames = 0, held = 0;
    ULONGLONG logAt = 0;
    int logs = 0;
};
Seat g_seat;
constexpr float kSeatSide = 0.30f;  // metres of lean forward, back and sideways
constexpr float kSeatUp = 0.04f, kSeatDown = 0.06f;
constexpr float kSeatHold = 0.08f;  // the view stays this close to the Sim's eyes (metres)
// Your eyes from the point your head turns about (your neck), in the
// headset's frame, metres: the headset's own turns don't count as a lean.
const Vec3 kNeckToEyes{0, 0.08f, -0.08f};

// The view while the game moves your Sim: the game's head (anim, smoothed in
// g_bodyAnchor) moved by your lean, the Sim's eyes swung around it as you
// look, measured on the Sim (up, ahead), held within kSeatHold of where its
// eyes really are (head, after our lean). Returns app space's origin in the
// world: your head centre lands there.
void seatView(float position[3], const Quat& heading, float up, float ahead, const float head[3], float s) {
    Vec3 n = rotate(g_headOrientation, kNeckToEyes);
    Vec3 pivot{g_headCentre.x - n.x, g_headCentre.y - n.y, g_headCentre.z - n.z};
    Vec3 jump{pivot.x - g_seat.last.x, pivot.y - g_seat.last.y, pivot.z - g_seat.last.z};
    bool recentered = jump.x * jump.x + jump.y * jump.y + jump.z * jump.z > 0.09f;
    if (!g_seat.on || recentered) {
        g_seat.neutral = pivot;
        if (g_seat.logs < 60) {
            ++g_seat.logs;
            LOG("Seat view: %s; your view starts at the Sim's eyes", g_seat.on ? "the headset recentred" :
                                                                                  "the game moves your Sim");
        }
        g_seat.on = true;
    }
    g_seat.last = pivot;
    Vec3 lean{pivot.x - g_seat.neutral.x, pivot.y - g_seat.neutral.y, pivot.z - g_seat.neutral.z};
    float side = std::sqrt(lean.x * lean.x + lean.z * lean.z);
    if (side > kSeatSide) {
        float k = kSeatSide / side;
        g_seat.neutral.x += lean.x * (1 - k);
        g_seat.neutral.z += lean.z * (1 - k);
        lean.x *= k;
        lean.z *= k;
    }
    if (lean.y > kSeatUp) {
        g_seat.neutral.y += lean.y - kSeatUp;
        lean.y = kSeatUp;
    } else if (lean.y < -kSeatDown) {
        g_seat.neutral.y += lean.y + kSeatDown;
        lean.y = -kSeatDown;
    }
    Vec3 leanWorld = rotate(heading, {lean.x * s, lean.y * s, lean.z * s});
    const float leanOut[3] = {leanWorld.x, leanWorld.y, leanWorld.z};
    armik::setLean(leanOut);
    Vec3 eyes = rotate(multiply(heading, g_headOrientation), {0, up, -ahead});
    Vec3 want{g_bodyAnchor[0] + leanWorld.x + eyes.x, g_bodyAnchor[1] + leanWorld.y + eyes.y,
              g_bodyAnchor[2] + leanWorld.z + eyes.z};
    Vec3 r{want.x - head[0] - eyes.x, want.y - head[1] - eyes.y, want.z - head[2] - eyes.z};
    float off = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), hold = kSeatHold * s;
    ++g_seat.frames;
    if (off > hold) {
        float k = 1 - hold / off;
        want = {want.x - r.x * k, want.y - r.y * k, want.z - r.z * k};
        ++g_seat.held;
    }
    Vec3 centre = rotate(heading, {g_headCentre.x * s, g_headCentre.y * s, g_headCentre.z * s});
    position[0] = want.x - centre.x;
    position[1] = want.y - centre.y;
    position[2] = want.z - centre.z;
    ULONGLONG now = GetTickCount64();
    if (g_seat.logs < 60 && now - g_seat.logAt > 5000) {
        g_seat.logAt = now;
        ++g_seat.logs;
        LOG("Seat view: lean %.0f %.0f %.0f cm, the Sim's eyes %.0f cm from where you'd be, held by its eyes in %llu "
            "of %llu frames",
            lean.x * 100, lean.y * 100, lean.z * 100, off / s * 100, static_cast<unsigned long long>(g_seat.held),
            static_cast<unsigned long long>(g_seat.frames));
        g_seat.held = g_seat.frames = 0;
    }
}

// The upper body's frame as a view base (camera axes: +X right, +Y up, +Z
// back), from b__Spine2__. Which of its axes are forward and up is found once
// while the Sim stands (free movement), against where it faces (facing, a view
// heading).
bool chestFrame(uint8_t* actor, bool standing, float facing, Quat& out) {
    if (g_chestIndex == ~0u || !actor) return false;
    alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    vcall<JointTransformFn>(actor, kJointTransformSlot)(actor, g_chestIndex, t);
    Quat q{t[4], t[5], t[6], t[7]};
    if (!std::isfinite(q.x) || !std::isfinite(q.w) || std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) > 0.05f)
        return false;
    static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    if (g_chestForward < 0) {
        if (!standing || !std::isfinite(facing) || g_chestTries >= 20) return false;
        ++g_chestTries;
        const Vec3 ahead{-std::sin(facing), 0, -std::cos(facing)};
        int up = -1, forward = -1;
        float upBest = 0.8f, forwardBest = 0.7f;
        for (int i = 0; i < 6; ++i) {
            float y = rotate(q, kAxes[i]).y;
            if (y > upBest) {
                upBest = y;
                up = i;
            }
        }
        for (int i = 0; up >= 0 && i < 6; ++i) {
            if (i / 2 == up / 2) continue;
            Vec3 v = rotate(q, kAxes[i]);
            float l = std::sqrt(v.x * v.x + v.z * v.z);
            if (l < 0.5f) continue;
            float score = (v.x * ahead.x + v.z * ahead.z) / l;
            if (score > forwardBest) {
                forwardBest = score;
                forward = i;
            }
        }
        LOG("Body view: upper body axes forward %d (match %.2f), up %d (%.2f)%s", forward, forwardBest, up, upBest,
            forward < 0 ? ": not found while standing, trying again" : "");
        if (forward < 0) return false;
        g_chestForward = forward;
        g_chestUp = up;
    }
    Vec3 f = rotate(q, kAxes[g_chestForward]), u = rotate(q, kAxes[g_chestUp]);
    float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (!(fl > 0.5f)) return false;
    f = {f.x / fl, f.y / fl, f.z / fl};
    float d = u.x * f.x + u.y * f.y + u.z * f.z;
    u = {u.x - f.x * d, u.y - f.y * d, u.z - f.z * d};
    float ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    if (!(ul > 0.3f)) return false;
    u = {u.x / ul, u.y / ul, u.z / ul};
    const Vec3 b{-f.x, -f.y, -f.z};
    const Vec3 r{u.y * b.z - u.z * b.y, u.z * b.x - u.x * b.z, u.x * b.y - u.y * b.x};
    // The rotation with columns right, up, back.
    float m00 = r.x, m01 = u.x, m02 = b.x, m10 = r.y, m11 = u.y, m12 = b.y, m20 = r.z, m21 = u.z, m22 = b.z;
    float trace = m00 + m11 + m22;
    if (trace > 0) {
        float s = std::sqrt(trace + 1) * 2;
        out = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        out = {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        out = {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        out = {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
    }
    return true;
}

// a eased toward b by k (normalised lerp along the shorter way).
Quat easeQuat(const Quat& a, const Quat& b, float k) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    float sign = d < 0 ? -1.0f : 1.0f;
    Quat q{a.x + (sign * b.x - a.x) * k, a.y + (sign * b.y - a.y) * k, a.z + (sign * b.z - a.z) * k,
           a.w + (sign * b.w - a.w) * k};
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-6f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : b;
}

// The view's base: level at the heading (level). While the game animates
// your Sim in an animation named in [body] body_view_anims (Python
// set_body_view) it lies the way the Sim's upper body does instead: lying on
// your back you look up when you look straight ahead, and your headset still
// looks around from there (user 2026-10-17 03:55Z). It eases in and back out.
Quat viewBase(const Quat& level, bool animated, uint8_t* actor, bool freeMove, float facing, float turn, float pitch) {
    Quat body;
    if (g_chestForward < 0 && freeMove) chestFrame(actor, true, facing, body);  // learn its axes while you stand
    const bool lead = animated && g_bodyViewWanted.load(std::memory_order_relaxed) && chestFrame(actor, false, facing, body);
    if (lead != g_viewFrameLead) {
        g_viewFrameLead = lead;
        if (lead) {
            g_frameYaw = g_framePitch = 0;
            if (!g_viewFrameOn) g_viewFrame = level;
            g_viewFrameOn = true;
        }
        if (g_frameLogs++ < 40)
            LOG("Body view: %s", lead ? "your view lies the way your Sim's body does" : "your view levels out again");
    }
    if (lead) {
        // The stick turns you about the body's up (left/right, user 04:50Z)
        // and pitches you about the view's right (up/down, user 05:13Z), kept
        // as two angles in the body's frame, so the view never rolls against
        // your Sim. Turning about the headset's own axes added up: looking
        // aside or down while turning tipped the view sideways (user 12:06Z).
        // Applied at once, and it moves with the body.
        auto frameTurn = [] {
            const Quat yaw{0, std::sin(g_frameYaw / 2), 0, std::cos(g_frameYaw / 2)};
            const Quat tip{std::sin(g_framePitch / 2), 0, 0, std::cos(g_framePitch / 2)};
            return multiply(yaw, tip);
        };
        if (turn != 0 || pitch != 0) {
            const Quat before = frameTurn();
            g_frameYaw = std::remainder(g_frameYaw + turn, 6.2831853f);
            g_framePitch = std::fmax(-kFramePitchMax, std::fmin(kFramePitchMax, g_framePitch + pitch));
            const Quat undo{-before.x, -before.y, -before.z, before.w};
            const Quat moved = multiply(multiply(g_viewFrame, undo), frameTurn());
            g_viewFrame = easeQuat(moved, moved, 1);  // normalised
        }
        g_viewFrame = easeQuat(g_viewFrame, multiply(body, frameTurn()), kViewFrameEase);
        // How the view lies against your Sim's head (both without the
        // headset), every 10 s while it lies with the body.
        static ULONGLONG nextLog = 0;
        static int rollLogs = 0;
        const ULONGLONG now = GetTickCount64();
        if (now >= nextLog && rollLogs < 60 && g_headIndex != ~0u) {
            nextLog = now + 10000;
            alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
            vcall<JointTransformFn>(actor, kJointTransformSlot)(actor, g_headIndex, t);
            const Quat head{t[4], t[5], t[6], t[7]};
            if (std::isfinite(head.x) && std::isfinite(head.w)) {
                ++rollLogs;
                auto angle = [](Vec3 a, Vec3 b) {
                    float d = a.x * b.x + a.y * b.y + a.z * b.z;
                    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, d))) * 57.2958f;
                };
                const Vec3 viewUp = rotate(g_viewFrame, {0, 1, 0}), viewAhead = rotate(g_viewFrame, {0, 0, -1});
                const Vec3 headUp = rotate(head, {0, 1, 0}), headAhead = rotate(head, {0, 0, 1});
                // The roll: the head's up against the view's, both seen along the view.
                const float d = headUp.x * viewAhead.x + headUp.y * viewAhead.y + headUp.z * viewAhead.z;
                const Vec3 flat{headUp.x - viewAhead.x * d, headUp.y - viewAhead.y * d, headUp.z - viewAhead.z * d};
                const float fl = std::sqrt(flat.x * flat.x + flat.y * flat.y + flat.z * flat.z);
                const Vec3 viewRight = rotate(g_viewFrame, {1, 0, 0});
                const float roll = fl > 0.2f ? std::atan2((flat.x * viewRight.x + flat.y * viewRight.y + flat.z * viewRight.z) / fl,
                                                         (flat.x * viewUp.x + flat.y * viewUp.y + flat.z * viewUp.z) / fl) * 57.2958f
                                             : NAN;
                LOG("Body view: looks %.0f deg from where your Sim's head looks, rolled %.0f deg against it (left +); "
                    "stick turn %.0f, pitch %.0f deg; view up %.2f %.2f %.2f",
                    angle(viewAhead, headAhead), roll, g_frameYaw * 57.2958f, g_framePitch * 57.2958f, viewUp.x,
                    viewUp.y, viewUp.z);
            }
        }
        return g_viewFrame;
    }
    if (!g_viewFrameOn) return level;
    if (!animated) {
        g_viewFrameOn = false;  // free movement: level at once
        return level;
    }
    g_viewFrame = easeQuat(g_viewFrame, level, kViewFrameEase);
    float d = std::fabs(g_viewFrame.x * level.x + g_viewFrame.y * level.y + g_viewFrame.z * level.z +
                        g_viewFrame.w * level.w);
    if (d > 0.99996f) {  // within about a degree
        g_viewFrameOn = false;
        return level;
    }
    return g_viewFrame;
}

void applyEye(uint8_t* dst) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_active) return;
    crashlog::note(crashlog::kCopyHook);
    float back[4], position[4];
    std::memcpy(back, dst + kBackOffset, sizeof(back));
    std::memcpy(position, dst + kPositionOffset, sizeof(position));
    float yaw = std::atan2(back[0], back[2]);  // heading of the game camera's back axis
    if (!std::isfinite(yaw)) return;

    float s = g_config.worldScale;
    const Vec3& eye = g_pending.position;
    float head[3], facing = NAN, eyes[3] = {NAN, NAN, NAN};
    uint8_t* actor = nullptr;
    bool body = headPosition(head, &facing, &actor, eyes) && toWorld(head);
    bool firstPerson = g_bodyId.load() != 0;
    gamefix::setFirstPerson(firstPerson);
    voice::setFirstPerson(firstPerson);
    ambience::setFirstPerson(firstPerson);
    // The Sim your mic talks through (voice.cpp), found here on the game thread.
    {
        uint64_t voiceSim = firstPerson ? voice::sim() : 0;
        armik::setVoiceActor(voiceSim && g_lookup ? g_lookup(&voiceSim) : nullptr);
    }
    if (!body || g_jointSpace != 1) eyes[0] = NAN;  // eyes are only read as world positions
    if (std::isfinite(eyes[0])) measureEyes(head, eyes);
    // The right eye of a pair: drawn from its left eye's place and heading.
    // The stick's turn waits for the next pair and the body stays where it is.
    const int eyeIndex = g_pending.eye;
    const bool paired = g_pairedEyes.load(std::memory_order_relaxed) && body;
    const bool second = paired && eyeIndex == 1 && g_lastAppliedEye == 0 && g_pairBase.valid;
    // A left eye straight after a right eye that worked out its place
    // (paired hands): drawn from there, nothing moves on.
    const NextBase next = g_nextBase;
    g_nextBase.valid = false;
    const bool useNext = paired && eyeIndex == 0 && g_lastAppliedEye == 1 && next.valid && next.actor == actor &&
                         actor && g_jointSpace == 1;
    g_lastAppliedEye = eyeIndex;
    if (second && ++g_pairs == 1) LOG("Paired eyes: the right eye drawn from where the left eye was (first person)");
    // Paired hands: the right eye of a free-moving pair moves the player on
    // for the next pair (taking the stick's turn), without moving the body yet.
    const bool goAhead = second && g_pairedHands.load(std::memory_order_relaxed) && g_pairBase.freeMove && actor &&
                       g_jointSpace == 1;
    float turn = (second && !goAhead) || useNext ? 0.0f : g_turnRequest.exchange(0);
    const float oldLead = g_turnLead;
    if (!((second && !goAhead) || useNext)) {
        // The lead only in free movement; anything else turns it back out.
        const float wanted = g_lastFree && body && actor ? g_leadWanted.load() : 0.0f;
        turn += wanted - g_turnLead;
        g_turnLead = wanted;
    }
    float pitch = second || useNext ? 0.0f : g_pitchRequest.exchange(0);  // dropped unless lying with the body
    // Free movement: our own player carries the view, and the Sim is put
    // under the headset (player.cpp). Needs the joints in world space.
    player::Output free{}, nextFree{};
    bool freeMove = false, movedAhead = false;
    if (((!second && !useNext) || goAhead) && body && actor && g_jointSpace == 1) {
        player::Input in{};
        in.actor = actor;
        std::memcpy(in.headJoint, head, sizeof(head));
        std::memcpy(in.eyes, eyes, sizeof(eyes));
        in.eyeUp = g_eyeUp;
        in.eyeInset = g_config.bodyEyeInset;
        in.hmd[0] = g_headCentre.x;
        in.hmd[1] = g_headCentre.y;
        in.hmd[2] = g_headCentre.z;
        Vec3 f = rotate(g_headOrientation, {0, 0, -1});
        in.hmdForward[0] = f.x;
        in.hmdForward[1] = f.z;
        in.scale = s;
        in.deferPlace = goAhead;
        bool on = player::update(in, std::isfinite(g_bodyYaw) ? g_bodyYaw : yaw, turn, goAhead ? nextFree : free);
        (goAhead ? movedAhead : freeMove) = on;
    }
    if (goAhead && !movedAhead && turn != 0) {
        // Free movement ended: the next frame takes the turn (without the lead).
        turnBody(turn - (g_turnLead - oldLead));
        g_turnLead = oldLead;
    }
    if (second) {
        freeMove = g_pairBase.freeMove;
        yaw = g_pairBase.yaw;
        if (movedAhead) {
            g_bodyYaw = nextFree.viewYaw;
            if (std::isfinite(facing)) g_bodyYawOffset = std::remainder(nextFree.viewYaw - facing, 6.2831853f);
            std::memcpy(g_bodyAnchor, head, sizeof(head));
            g_bodySmoothed = true;
            g_nextBase.valid = true;
            g_nextBase.actor = actor;
            g_nextBase.yaw = nextFree.viewYaw;
            g_nextBase.lead = g_turnLead;
            std::memcpy(g_nextBase.position, nextFree.base, sizeof(g_nextBase.position));
        }
    } else if (useNext) {
        freeMove = true;
        yaw = next.yaw;
    } else if (freeMove) {
        yaw = g_bodyYaw = free.viewYaw;
        // So the view carries on smoothly if the game takes the Sim over.
        if (std::isfinite(facing)) g_bodyYawOffset = std::remainder(yaw - facing, 6.2831853f);
    } else if (body && std::isfinite(facing)) {
        // Turn with the Sim, smoothed so turns feel like a glide, not a snap.
        // While you steer with the stick the view holds still instead (the
        // Sim turning toward where you push must not turn where you look);
        // afterwards it follows the Sim's turns from where it was.
        if (!std::isfinite(g_bodyYaw)) g_bodyYaw = facing;
        g_bodyYaw = std::remainder(g_bodyYaw + turn, 6.2831853f);
        g_bodyYawOffset += turn;
        if (g_holdTurn.load()) {
            g_bodyYawOffset = std::remainder(g_bodyYaw - facing, 6.2831853f);
        } else {
            float delta = std::remainder(facing + g_bodyYawOffset - g_bodyYaw, 6.2831853f);
            g_bodyYaw = std::remainder(g_bodyYaw + delta * g_config.bodyTurnSmoothing, 6.2831853f);
        }
        yaw = g_bodyYaw;
    } else if (body) {
        // Where the Sim faces can't be read this frame: the view keeps its
        // heading (not the game camera's) and the stick still turns it.
        if (!std::isfinite(g_bodyYaw)) g_bodyYaw = yaw;
        g_bodyYaw = std::remainder(g_bodyYaw + turn, 6.2831853f);
        g_bodyYawOffset += turn;
        yaw = g_bodyYaw;
    }
    Quat heading{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    if (second)
        heading = g_pairBase.heading;
    else if (!useNext)
        heading = viewBase(heading, body && !freeMove, actor, freeMove, facing, turn, pitch);
    Quat q = multiply(heading, g_pending.orientation);
    Vec3 neck{0, 0, 0};
    float neckAhead = NAN;
    bool seated = false;
    if (second) {
        std::memcpy(position, g_pairBase.position, sizeof(g_pairBase.position));
        neckAhead = g_pairBase.ahead;
        if (std::isfinite(neckAhead)) neck = rotate(q, {0, 0, -neckAhead});
    } else if (useNext) {
        std::memcpy(position, next.position, sizeof(next.position));
        // The game's root motion put the body there as it posed it; if not, here.
        ++g_aheadPairs;
        if (player::settle(actor)) ++g_aheadSettled;
        ULONGLONG now = GetTickCount64();
        if (!g_aheadLogTick) g_aheadLogTick = now;
        if (now - g_aheadLogTick >= 10000 && g_aheadLogs < 30) {
            uint64_t pairs = g_aheadPairs - g_aheadPairsLogged;
            ++g_aheadLogs;
            LOG("Paired hands: %llu pairs in 10 s drawn from where the body was put ahead; the game had it there by "
                "the left eye in %llu, the camera hook moved it in %llu",
                static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(pairs - g_aheadSettled),
                static_cast<unsigned long long>(g_aheadSettled));
            g_aheadPairsLogged = g_aheadPairs;
            g_aheadSettled = 0;
            g_aheadLogTick = now;
        }
    } else if (body && freeMove) {
        std::memcpy(position, free.base, sizeof(free.base));
        // Where the view would be if the game takes the Sim over next frame.
        std::memcpy(g_bodyAnchor, head, sizeof(head));
        g_bodySmoothed = true;
    } else if (body) {
        seated = true;
        const bool follow = g_config.bodySeatFollow;
        // The head as the game poses it: without the lean we add (armik),
        // which comes from your head and must not move the view twice.
        float anim[3] = {head[0], head[1], head[2]};
        Vec3 leaned{0, 0, 0};
        if (follow && armik::leanDone(leaned)) {
            anim[0] -= leaned.x;
            anim[1] -= leaned.y;
            anim[2] -= leaned.z;
        }
        // Follow the head, smoothed so walk-cycle bob doesn't shake the view.
        if (!g_bodySmoothed) {
            std::memcpy(g_bodyAnchor, anim, sizeof(anim));
            g_bodySmoothed = true;
            LOG("Body view: game camera at %.3f %.3f %.3f, head at %.3f %.3f %.3f", position[0], position[1],
                position[2], head[0], head[1], head[2]);
        }
        for (int i = 0; i < 3; ++i) g_bodyAnchor[i] += (anim[i] - g_bodyAnchor[i]) * g_config.bodySmoothing;
        // The eyes sit in front of the head and swing around it as you look
        // around, like a neck. With the Sim's own eyes measured the view sits
        // just behind them, inside its head, where its face never shows.
        bool measured = std::isfinite(g_eyeUp) && std::isfinite(g_eyeAhead);
        float up = measured ? g_eyeUp : g_config.bodyHeight;
        float ahead = measured ? g_eyeAhead - g_config.bodyEyeInset * s : g_config.bodyForward;
        if (follow) {
            seatView(position, heading, up, ahead, head, s);
        } else {
            position[0] = g_bodyAnchor[0];
            position[1] = g_bodyAnchor[1] + up;
            position[2] = g_bodyAnchor[2];
            neck = rotate(q, {0, 0, -ahead});
            neckAhead = ahead;
        }
    }
    if (!second && !seated) g_seat.on = false;
    // The right eye of a pair takes its left eye's place, push and all.
    if (!second && !useNext) {
        float radius = 0;
        float target[2] = {0, 0};
        float depth = 0;
        if (seated && player::headRepelOn(radius)) {
            Vec3 o = rotate(heading, {eye.x * s, eye.y * s, eye.z * s});
            const float at[3] = {position[0] + o.x + neck.x, position[1] + o.y + neck.y, position[2] + o.z + neck.z};
            // From where the view would be without last frame's push.
            // Their head and torso only, and at most kViewPushMax: a scene's
            // animation itself puts bodies together, and following every limb
            // (v0.13.212: up to 25 cm, flipping sides) made the view twitch.
            depth = armik::headOutOfBodies(at, radius * s, target, true);
            float len = std::sqrt(target[0] * target[0] + target[1] * target[1]);
            if (len > kViewPushMax * s) {
                target[0] *= kViewPushMax * s / len;
                target[1] *= kViewPushMax * s / len;
            }
        }
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        float dt = g_viewPushLast.QuadPart ? float(t.QuadPart - g_viewPushLast.QuadPart) / float(f.QuadPart) : 0;
        g_viewPushLast = t;
        dt = dt > 0.1f ? 0.1f : dt > 0 ? dt : 0;
        // Gently, both ways, so a target that moves with the animation (or
        // swaps sides) glides instead of shaking the view.
        // Quicker while the push grows (going in), slow on the way back.
        bool deeper = target[0] * target[0] + target[1] * target[1] >
                      g_viewPush[0] * g_viewPush[0] + g_viewPush[1] * g_viewPush[1];
        float k = 1 - std::exp(-(deeper ? kViewPushIn : kViewPushRate) * dt);
        for (int i = 0; i < 2; ++i) g_viewPush[i] += (target[i] - g_viewPush[i]) * k;
        if (!seated) g_viewPush[0] = g_viewPush[1] = 0;
        position[0] += g_viewPush[0];
        position[2] += g_viewPush[1];
        ULONGLONG tick = GetTickCount64();
        if (depth > 0 && g_viewPushLogs < 60 && tick - g_viewPushLogTick > 500) {
            g_viewPushLogTick = tick;
            ++g_viewPushLogs;
            LOG("Body view: your head was %.0f mm into another Sim during the game's animation: view moved out %.0f mm",
                depth * 1000 / s, std::sqrt(target[0] * target[0] + target[1] * target[1]) * 1000 / s);
        }
    }
    // A left eye: its right eye comes from here.
    const float yawLead = second ? g_pairBase.lead : useNext ? next.lead : g_turnLead;
    g_lastFree = freeMove || movedAhead;
    g_pairBase.valid = paired && !second && eyeIndex == 0;
    if (g_pairBase.valid) {
        g_pairBase.freeMove = freeMove;
        g_pairBase.yaw = yaw;
        g_pairBase.lead = yawLead;
        g_pairBase.heading = heading;
        g_pairBase.ahead = neckAhead;
        std::memcpy(g_pairBase.position, position, sizeof(g_pairBase.position));
    }
    Vec3 offset = rotate(heading, {eye.x * s, eye.y * s, eye.z * s});
    std::memcpy(g_anchorPosition, position, sizeof(g_anchorPosition));
    offset = {offset.x + neck.x, offset.y + neck.y, offset.z + neck.z};
    g_anchorHeading = heading;
    g_anchorValid = true;
    // Your head and hands drive the Sim's whenever you are in its body, in
    // the game's own animations too (sitting, eating); the game keeps the
    // torso and legs.
    const float eyeWorld[3] = {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z};
    // Other Sims look at the middle between your eyes: each frame draws one
    // eye, and looking at that eye turned their heads by the eyes' gap from
    // frame to frame (user 2026-10-19 00:02Z: other Sims' heads twitchy).
    const Vec3 mid = rotate(heading, {g_headCentre.x * s, g_headCentre.y * s, g_headCentre.z * s});
    const float midWorld[3] = {position[0] + mid.x + neck.x, position[1] + mid.y + neck.y, position[2] + mid.z + neck.z};
    if (movedAhead) {
        // The game poses the body for the next pair before its left eye is
        // drawn: the arms reach from where that pair is drawn from.
        const float y = nextFree.viewYaw;
        const Quat nextHeading{0, std::sin(y / 2), 0, std::cos(y / 2)};
        const Vec3 o = rotate(nextHeading, {g_headCentre.x * s, g_headCentre.y * s, g_headCentre.z * s});
        const float nextMid[3] = {nextFree.base[0] + o.x, nextFree.base[1] + o.y, nextFree.base[2] + o.z};
        armik::setFrame(actor, true, nextFree.base, nextHeading, s, g_headOrientation, nextMid);
    } else {
        armik::setFrame(body ? actor : nullptr, freeMove, position, heading, s, g_headOrientation, midWorld);
    }
    if (body) {
        gamefix::keepTopFloor();
        checkHeld();
    }
    writeAxis(dst, kRightOffset, rotate(q, {1, 0, 0}), 0);
    writeAxis(dst, kUpOffset, rotate(q, {0, 1, 0}), 0);
    writeAxis(dst, kBackOffset, rotate(q, {0, 0, 1}), 0);
    writeAxis(dst, kPositionOffset, {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z}, 1);
    {
        const int slot = g_pending.eye == 1 ? 1 : 0;
        const float eyeAt[3] = {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z};
        for (int i = 0; i < 3; ++i) g_writtenEye[slot][i].store(eyeAt[i], std::memory_order_relaxed);
        g_writtenCamera.store(reinterpret_cast<uintptr_t>(dst), std::memory_order_release);
    }
    // The mirror pass clips its camera at the glass (CameraObliqueClip 0x1209c30, run when the byte at
    // 0x280cb7c is set, game 1.128): on the eye's off-centre projection that skews the clip and objects pop in
    // and out of mirrors. In first person it goes off ([stereo] mirror_clip = 0; user A2), the game's again
    // outside it.
    {
        static uint8_t* flag = nullptr;
        static int original = -1;
        static bool tried = false;
        if (!tried) {
            tried = true;
            if (gameaddr::recognised() && !gameaddr::dx9()) flag = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + 0x280cb7c;
        }
        if (flag) {
            if (original < 0) {
                original = *flag;
                LOG("Mirror clip: the game's glass clip is %d", original);
            }
            const uint8_t want = body && !g_centred && !g_config.mirrorClip ? 0 : static_cast<uint8_t>(original);
            if (*flag != want) {
                *flag = want;
                LOG("Mirror clip: %d (%s)", want, want ? "the game's" : "off while the eye is lopsided");
            }
        }
    }
    // The game's near limit (about 1.5 m) is meant for a camera far above; at
    // eye height it cuts off the table in front of you. In first person it
    // follows what is actually close (nearplane.h), never under near_clip.
    if (body && g_config.bodyNearClip > 0) {
        float nearZ = g_config.bodyNearClip;
        if (g_config.bodyNearAdaptive) {
            const Vec3 right = rotate(q, {1, 0, 0}), up = rotate(q, {0, 1, 0}), back = rotate(q, {0, 0, 1});
            const Vec3 ahead{-back.x, -back.y, -back.z};
            const float forward[3] = {ahead.x, ahead.y, ahead.z};
            float tanX = g_coverX, tanY = g_coverY, centreX = g_centreX, centreY = g_centreY;
            if (!(g_tanX > 0 && g_tanY > 0)) {
                centreX = centreY = 0;
                float frustum[2];
                std::memcpy(frustum, dst + kFrustumOffset, sizeof(frustum));
                tanX = frustum[0] / 2;
                tanY = frustum[1] / 2;
            }
            float farZ = 0;
            std::memcpy(&farZ, dst + kNearOffset + sizeof(float), sizeof(farZ));
            nearplane::Capsule capsules[nearplane::kMaxCapsules];
            int count = 0;
            float headEnd = 0;
            float own =
                actor ? bodyNearest(actor, eyeWorld, right, up, ahead, tanX + kBodyViewPad, tanY + kBodyViewPad,
                                    capsules, count, headEnd)
                      : 0;
            const float rightF[3] = {right.x, right.y, right.z}, upF[3] = {up.x, up.y, up.z};
            // The depth check rebuilds each pixel's ray, so it gets the exact frustum.
            const bool ours = g_tanX > 0 && g_tanY > 0;
            nearplane::ownBody(eyeWorld, rightF, upF, forward, ours ? g_tanX : tanX, ours ? g_tanY : tanY, centreX,
                               centreY, capsules, count);
            // With your face folded out of this view (facehide.h) nothing needs cutting past your head.
            if (facehide::hiding()) headEnd = 0;
            nearZ = nearplane::choose(eyeWorld, forward, own, headEnd, farZ, g_config.bodyNearClip,
                                      std::max(g_config.bodyNearMax, g_config.bodyNearClip));
        }
        std::memcpy(dst + kNearOffset, &nearZ, sizeof(float));
    }
    {
        std::lock_guard<std::mutex> listenerLock(g_listenerMutex);
        g_listener.right = rotate(q, {1, 0, 0});
        g_listener.up = rotate(q, {0, 1, 0});
        g_listener.back = rotate(q, {0, 0, 1});
        g_listener.position = {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z};
        g_listener.body = g_bodyId.load() != 0 && g_bodySmoothed;
        g_listener.tick = GetTickCount64();
        g_listener.rooted = actor != nullptr;
        if (actor) std::memcpy(g_listener.root, actor + kActorPlace, sizeof(g_listener.root));
    }
    if (g_tanX > 0) {
        // Same fields the game's own SetAspect fills in (see camera.h).
        float frustum[4] = {2 * g_tanX, 2 * g_tanY, 2 * std::atan(g_tanX), g_tanX / g_tanY};
        std::memcpy(dst + kFrustumOffset, frustum, sizeof(frustum));
        if (!g_centred) {
            const float centre[2] = {g_centreX, g_centreY};
            std::memcpy(dst + kCentreOffset, centre, sizeof(centre));
            const int slot = g_pending.eye == 1 ? 1 : 0;
            g_eyeCentre[slot][0].store(g_centreX, std::memory_order_relaxed);
            g_eyeCentre[slot][1].store(g_centreY, std::memory_order_relaxed);
        }
        g_appliedCentre[0].store(g_centred ? NAN : g_centreX, std::memory_order_relaxed);
        g_appliedCentre[1].store(g_centred ? NAN : g_centreY, std::memory_order_relaxed);
    } else {
        g_appliedCentre[0].store(NAN, std::memory_order_relaxed);
    }
    {
        View v{};
        v.right = rotate(q, {1, 0, 0});
        v.up = rotate(q, {0, 1, 0});
        v.back = rotate(q, {0, 0, 1});
        v.position = {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z};
        float frustum[2], clip[2];
        std::memcpy(frustum, dst + kFrustumOffset, sizeof(frustum));
        std::memcpy(clip, dst + kNearOffset, sizeof(clip));
        v.tanX = frustum[0] / 2;
        v.tanY = frustum[1] / 2;
        float centre[2] = {0, 0};
        if (g_tanX > 0 && !g_centred) std::memcpy(centre, dst + kCentreOffset, sizeof(centre));
        v.centreX = centre[0];
        v.centreY = centre[1];
        v.nearZ = clip[0];
        v.farZ = clip[1];
        v.anchor = {position[0], position[1], position[2]};
        v.heading = heading;
        v.scale = s;
        std::lock_guard<std::mutex> viewLock(g_viewMutex);
        g_view = v;
        g_viewValid = std::isfinite(v.tanX) && v.tanX > 0 && v.tanY > 0 && v.nearZ > 0 && v.farZ > v.nearZ;
    }
    // Touch: a pick through your pointing fingertip with exactly this camera.
    {
        int hand = body && actor ? touch::pointingHand() : -1;
        Vec3 tip{};
        bool haveTip = hand >= 0 && fingertipWorld(actor, hand, tip);
        touch::update(dst, g_bodyId.load(), haveTip, tip);
    }
    std::memcpy(g_writtenFrustum, dst + kFrustumOffset, sizeof(g_writtenFrustum));
    if (g_queueSize == 4) {  // Present stopped consuming; drop the oldest
        g_queueHead = (g_queueHead + 1) % 4;
        --g_queueSize;
    }
    Rendered rendered = g_pending;
    rendered.yaw = yaw;
    rendered.lead = yawLead;
    rendered.pairFirst = g_pairBase.valid;
    rendered.worldPosition = {position[0] + offset.x, position[1] + offset.y, position[2] + offset.z};
    rendered.worldOrientation = q;
    rendered.worldScale = s;
    {
        float clip[2];
        std::memcpy(clip, dst + kNearOffset, sizeof(clip));
        if (clip[0] > 0 && clip[1] > clip[0]) {
            rendered.nearZ = clip[0];
            rendered.farZ = clip[1];
        }
        // Row-vector projection: depth = a + b / distance, a = m22 * m23 (m23 is
        // the w sign), b = m32; near = -b / a, far = a near / (a - 1).
        float m[16];
        std::memcpy(m, dst + 0x70, sizeof(m));
        const float a = m[10] * m[11], b = m[14];
        if (std::isfinite(a) && std::isfinite(b) && a != 0 && a != 1) {
            rendered.drawnNear = -b / a;
            rendered.drawnFar = a * rendered.drawnNear / (a - 1);
        }
    }
    rendered.body.count = 0;
    // Free movement turns your body with the view (the stick turns both), so
    // it can go to the headset on its own layer; in the game's animations
    // (seated) the view turns over a body that stays.
    if (body && actor && freeMove && g_bodyLayer.load(std::memory_order_relaxed)) {
        const Vec3 right = rotate(q, {1, 0, 0}), up = rotate(q, {0, 1, 0}), back = rotate(q, {0, 0, 1});
        bodyParts(actor, eyeWorld, right, up, {-back.x, -back.y, -back.z}, s, rendered.body);
        float clip[2];
        std::memcpy(clip, dst + kNearOffset, sizeof(clip));
        rendered.body.nearZ = clip[0];
        rendered.body.farZ = clip[1];
        if (!(clip[0] > 0 && clip[1] > clip[0])) rendered.body.count = 0;
    }
    g_queue[(g_queueHead + g_queueSize++) % 4] = rendered;
    // The newest heading: the next pair's once worked out (its turn is taken).
    g_latestYaw = movedAhead ? nextFree.viewYaw : yaw;
    g_latestLead = movedAhead ? g_turnLead : yawLead;
    if (++g_applied == 1) LOG("Head pose reached the game camera");
}

// The live camera is copied into the render camera each frame; the eye pose
// only ever goes into an object of that same class (the copy leaves the
// vtable alone), never into another copy of the live camera.
bool renderCamera(void* dst) {
    static uintptr_t expected = 0;
    static int strays = 0;
    uintptr_t vtable = 0;
    if (!safeRead(reinterpret_cast<uintptr_t>(dst), &vtable, sizeof(vtable)) || !vtable) return false;
    if (!expected) {
        char name[128] = {};
        rtti::className(reinterpret_cast<uintptr_t>(dst), name, sizeof(name));
        expected = vtable;
        LOG("Render camera %p is %s (main thread %lu)", dst, name[0] ? name : "unknown", GetCurrentThreadId());
    }
    if (vtable == expected) return true;
    if (strays++ < 3) {
        char name[128] = {};
        rtti::className(reinterpret_cast<uintptr_t>(dst), name, sizeof(name));
        LOG("Camera copy into %p (%s), not the render camera: left alone", dst, name[0] ? name : "unknown");
    }
    return false;
}

// exact_fov check (v0.13.239): the render camera's projection (+0x70, rows) as
// the game left it since last frame's copy, i.e. what that frame drew with.
// A lopsided eye shows its centre in row 2 x (-centre x / half width).
void logDrawnProjection(const void* dst, void* caller) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (g_centred || now - last < 5000) return;
    last = now;
    float m[16], centre[2];
    if (!safeRead(reinterpret_cast<uintptr_t>(dst) + 0x70, m, sizeof(m)) ||
        !safeRead(reinterpret_cast<uintptr_t>(dst) + kCentreOffset, centre, sizeof(centre)))
        return;
    LOG("Lopsided eye check: drawn projection x %.3f, row2 %.3f %.3f, y %.3f; centre there %.3f %.3f; "
        "written centre %.3f %.3f half %.3f %.3f; copy from game+0x%llx",
        m[0], m[8], m[9], m[5], centre[0], centre[1], g_centreX, g_centreY, g_tanX, g_tanY,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(caller) -
                                        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
}

// While no eye's centre is applied, an eye centre left in a game camera goes
// back to the game's own 0,0 (exact matches only: the game sets other centres
// itself, e.g. for picks). (Cameras made from the render camera while an eye is
// applied keep its centre on purpose: mirrors and the sky need it, v0.13.272
// cleared it and they drew from different angles per eye.)
void clearOldCentre(void* camera, const char* where) {
    if (!std::isnan(g_appliedCentre[0].load(std::memory_order_relaxed))) return;
    float* at = reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + kCentreOffset);
    if (at[0] == 0 && at[1] == 0) return;
    for (const auto& eye : g_eyeCentre) {
        if (at[0] != eye[0].load(std::memory_order_relaxed) || at[1] != eye[1].load(std::memory_order_relaxed))
            continue;
        if (g_oldCentreLogs < 5) {
            ++g_oldCentreLogs;
            LOG("Lopsided eye: an eye's centre %.3f %.3f was still in game camera %p (%s); back to the game's 0 0",
                at[0], at[1], camera, where);
        }
        at[0] = at[1] = 0;
        return;
    }
}

void* hookedCopy(void* dst, const void* src) {
    if (CopyObserver observer = g_observer.load()) observer(dst, src, __builtin_return_address(0));
    if (dst != src && reinterpret_cast<uintptr_t>(dst) == g_writtenCamera.load(std::memory_order_relaxed))
        logDrawnProjection(dst, __builtin_return_address(0));
    void* result = g_originalCopy(dst, src);
    uintptr_t manager = g_manager.load();
    if (manager && dst != src) {
        uintptr_t live = 0;
        safeRead(manager + kManagerCameraOffset, &live, sizeof(live));
        if (reinterpret_cast<uintptr_t>(src) == live && renderCamera(dst)) {
            framecost::renderCamera();
            framemap::mark(framemap::CameraCopy);
            applyEye(static_cast<uint8_t*>(dst));
            doorswing::pump();  // the game's main thread, where routes swing doors too
            walkstop::pump();
            thumbs::pump();  // where the game's UI asks for its pictures
        }
    }
    if (dst != src) {
        clearOldCentre(const_cast<void*>(src), "copied from");
        clearOldCentre(dst, "copied into");
    }
    return result;
}

// The game's listener update (EA::Audio, client side). It reads the live camera
// through a global view object: position (+0x60), a 4x4 matrix (+0xf0) and the
// point the camera looks at (+0x200, or +0x240 when +0x6e5 is set).
const uint16_t kListenerPattern[] = {0x44, 0x88, 0x4c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48,
                                     0x8d, 0xac, 0x24, 0xf0, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0x10, 0x02, 0x00,
                                     0x00, 0x41, 0x0f, 0xb6, 0xf9, 0x4d, 0x8b, 0xe0, 0x48, 0x8b, 0xf1, 0xe8};
constexpr size_t kListenerViewCall = 0x25;  // call that returns the view object
constexpr size_t kViewCameraOffset = 0x60;
constexpr size_t kMatrixOffset = 0xf0;
constexpr size_t kTargetOffset = 0x200, kTargetAltOffset = 0x240, kTargetSelectOffset = 0x6e5;

using ListenerFn = void (*)(void* self, void* arg, void* audio, char flag);
ListenerFn g_originalListener = nullptr;
std::atomic<bool> g_panFirstPerson{false};  // the listener is at your head in body mode (hookedListener)
uintptr_t* g_viewGlobal = nullptr;
int g_matrixLayout = -1;  // -1 unknown, 0 none matched, 1 camera-to-world rows, 2 view (row vectors), 3 view (columns)
uint64_t g_listenerCalls = 0;

float dot(const float* a, const Vec3& b) { return a[0] * b.x + a[1] * b.y + a[2] * b.z; }

// The camera matrix in the given layout, from axes and position.
void buildMatrix(int layout, const Vec3& r, const Vec3& u, const Vec3& b, const Vec3& p, float m[16]) {
    const Vec3 axes[3] = {r, u, b};
    if (layout == 1) {
        const Vec3 rows[4] = {r, u, b, p};
        for (int i = 0; i < 4; ++i) {
            m[i * 4] = rows[i].x;
            m[i * 4 + 1] = rows[i].y;
            m[i * 4 + 2] = rows[i].z;
            m[i * 4 + 3] = i == 3 ? 1.0f : 0.0f;
        }
    } else if (layout == 2) {
        for (int i = 0; i < 3; ++i) {
            float c[3] = {i == 0 ? r.x : i == 1 ? r.y : r.z, i == 0 ? u.x : i == 1 ? u.y : u.z,
                          i == 0 ? b.x : i == 1 ? b.y : b.z};
            m[i * 4] = c[0];
            m[i * 4 + 1] = c[1];
            m[i * 4 + 2] = c[2];
            m[i * 4 + 3] = 0;
        }
        float pp[3] = {p.x, p.y, p.z};
        for (int k = 0; k < 3; ++k) m[12 + k] = -dot(pp, axes[k]);
        m[15] = 1;
    } else {
        float pp[3] = {p.x, p.y, p.z};
        for (int k = 0; k < 3; ++k) {
            m[k * 4] = axes[k].x;
            m[k * 4 + 1] = axes[k].y;
            m[k * 4 + 2] = axes[k].z;
            m[k * 4 + 3] = -dot(pp, axes[k]);
        }
        m[12] = m[13] = m[14] = 0;
        m[15] = 1;
    }
}

Vec3 readVec(uintptr_t address) {
    float f[3];
    std::memcpy(f, reinterpret_cast<void*>(address), sizeof(f));
    return {f[0], f[1], f[2]};
}

// Which layout the game's matrix uses, checked against the camera's own axes.
int detectLayout(uintptr_t camera) {
    Vec3 r = readVec(camera + kRightOffset), u = readVec(camera + kUpOffset), b = readVec(camera + kBackOffset),
         p = readVec(camera + kPositionOffset);
    float live[16];
    std::memcpy(live, reinterpret_cast<void*>(camera + kMatrixOffset), sizeof(live));
    for (int layout = 1; layout <= 3; ++layout) {
        float m[16];
        buildMatrix(layout, r, u, b, p, m);
        bool match = true;
        for (int i = 0; i < 16 && match; ++i) match = std::fabs(m[i] - live[i]) <= 1e-3f * (1 + std::fabs(live[i]));
        if (match) return layout;
    }
    LOG("Audio listener: camera matrix matches no known layout; rows %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / "
        "%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f; axes r %.3f %.3f %.3f u %.3f %.3f %.3f b %.3f %.3f %.3f p %.3f %.3f %.3f",
        live[0], live[1], live[2], live[3], live[4], live[5], live[6], live[7], live[8], live[9], live[10], live[11],
        live[12], live[13], live[14], live[15], r.x, r.y, r.z, u.x, u.y, u.z, b.x, b.y, b.z, p.x, p.y, p.z);
    return 0;
}

void hookedListener(void* self, void* arg, void* audio, char flag) {
    ++g_listenerCalls;
    ListenerPose pose;
    {
        std::lock_guard<std::mutex> lock(g_listenerMutex);
        pose = g_listener;
    }
    uintptr_t view = 0, camera = 0;
    bool fresh = pose.tick && GetTickCount64() - pose.tick < 500 && g_active;
    if (fresh && g_viewGlobal && safeRead(reinterpret_cast<uintptr_t>(g_viewGlobal), &view, sizeof(view)) && view)
        safeRead(view + kViewCameraOffset, &camera, sizeof(camera));
    if (camera && g_cameraVTable) {
        uintptr_t vtable = 0;
        if (!safeRead(camera, &vtable, sizeof(vtable)) || vtable != g_cameraVTable) camera = 0;
    }
    static int attempts = 0;
    if (camera && g_matrixLayout <= 0 && attempts < 5) {
        // A few tries, in case the matrix lags the axes on the first frames.
        ++attempts;
        g_matrixLayout = detectLayout(camera);
        if (g_matrixLayout > 0) LOG("Audio listener follows the headset (camera matrix layout %d)", g_matrixLayout);
    }
    g_panFirstPerson.store(camera && g_matrixLayout > 0 && pose.body, std::memory_order_relaxed);
    if (!camera || g_matrixLayout <= 0) {
        g_originalListener(self, arg, audio, flag);
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(camera);
    uint8_t savedPosition[16], savedMatrix[64], savedTarget[16];
    const size_t target = base[kTargetSelectOffset] ? kTargetAltOffset : kTargetOffset;
    std::memcpy(savedPosition, base + kPositionOffset, 16);
    std::memcpy(savedMatrix, base + kMatrixOffset, 64);
    std::memcpy(savedTarget, base + target, 16);

    Vec3 position = pose.body ? pose.position : readVec(camera + kPositionOffset);
    float m[16];
    buildMatrix(g_matrixLayout, pose.right, pose.up, pose.back, position, m);
    crashlog::note(crashlog::kListener);
    static DWORD listenerThread = 0;
    if (!listenerThread) {
        listenerThread = GetCurrentThreadId();
        LOG("Audio listener update runs on thread %lu", listenerThread);
    }
    std::memcpy(base + kMatrixOffset, m, sizeof(m));
    if (pose.body) {
        // Listener at your head: the game puts it between the look-at point
        // and the camera, so both go there.
        std::memcpy(base + kPositionOffset, &position, sizeof(position));
        std::memcpy(base + target, &position, sizeof(position));
    }
    g_originalListener(self, arg, audio, flag);
    std::memcpy(base + kPositionOffset, savedPosition, 16);
    std::memcpy(base + kMatrixOffset, savedMatrix, 64);
    std::memcpy(base + target, savedTarget, 16);
}

// Where each 3D sound sits between the speakers (game 1.128, audio thread).
// A sound's spatial update (sound vtable slot 0x98, 0x1a9f780) gets the pan
// listener (the audio manager's listener 1; listener 0 sets loudness) and
// calls this for the sound's Pan2D1 panner with the sound's position and its
// near radius (its minimum distance, 0x1a9f930: a base plus a curve of the
// listener's height above the sound). It sets the panner's angle (param 0)
// and its distance (param 1): the sound's distance across the listener's
// ground plane, capped at the near radius. Pan2D1 (0x1b10210, 0x1b106b0) puts
// the sound on its speaker circle only at a distance of 1 or more; below 1 it
// spreads over every speaker, and at 0 it plays in the middle of your head.
// From the god-view camera high above, the near radius is large; at head
// height in first person a near radius under 1 m makes that sound play partly
// mid-head however far away it is (user 2026-10-17 14:45Z: "When I got closer
// to the npcs in question the voices in my head stopped"). First person: the
// cap is at least 1 m, and what is within 1 m of you is placed by placeClose.
constexpr float kPanCircle = 1.0f;  // Pan2D1's speaker circle
// (sound, its panner, the sound's position, the pan listener's position, its
// rows (3 x 4 floats, row 0 right, row 2 back), the sound's near radius)
using PanFn = void (*)(void* sound, void* panner, const float* position, const float* listener, const float* rows,
                       float nearRadius);
PanFn g_originalPan = nullptr;
std::atomic<bool> g_panAnyDistance{true};
// What the last stretch of pans looked like (audio thread only), logged now and then.
struct PanStats {
    ULONGLONG since = 0;
    uint64_t pans = 0, centred = 0, lifted = 0;
    float farthestCentred = 0, centredRadius = 0, centredAcross = 0;
    float listener[3] = {};
};
PanStats g_pan;
int g_panLogs = 0;
constexpr int kPanMaxLogs = 40;

// Sounds close to you in first person (user 2026-10-18 23:19Z: in a
// WickedWhims scene the other Sims' voices came from the middle of the head
// while their mouths moved). Two things can put them there: Pan2D1 spreads
// anything under 1 m across toward the middle (the cap above stops at the
// sound's own distance), and a Sim's sound may sit where the Sim stands (its
// object's place), which in an animation can be far from its head. A sound
// where another Sim stands goes to that Sim's head; one 15 cm to 1 m across
// from your head is moved out to the speaker circle along its own direction,
// so it comes from there. What sits at your head, or where your own Sim
// stands, keeps the game's spread (your own Sim's sounds). Not when another
// Sim's eyes are nearer the sound than yours: its head is right by yours
// (cuddling on a sofa, a kiss, a WickedWhims scene) and the voice is its own
// (user 2026-10-19 01:29Z: flirty voices in the head). A sound where you and
// another Sim both stand (a WickedWhims scene puts everyone on one spot) goes
// to that Sim's head: beside you rather than inside your head either way.
constexpr float kOwnHead = 0.15f;     // m across: at your head
constexpr float kTheirHead = 0.2f;    // m: a sound this close to a Sim's eyes is that Sim's voice
constexpr float kMinAcross = 0.01f;   // m: closer to straight above or below you has no direction
constexpr float kAtRoot = 0.12f;      // m: a sound this close to where a Sim stands is that Sim's
constexpr float kCloseSound = 2.0f;   // m: sounds counted for the log
struct VoicePlaces {
    ULONGLONG at = 0;
    int count = 0;
    armik::SimHead sims[16];
    bool rooted = false;
    float own[3] = {};
};
VoicePlaces g_places;  // audio thread only
struct CloseStats {
    ULONGLONG since = 0;
    uint64_t close = 0, atHead = 0, theirs = 0, toHead = 0, shared = 0, ownRoot = 0, other = 0, opened = 0;
    float example[3] = {};               // one of the "elsewhere" ones, from your head
    float exampleEyes = -1, exampleRoot = -1;  // its distance to the nearest Sim's eyes / where one stands
};
CloseStats g_close;
int g_closeLogs = 0;
constexpr int kCloseMaxLogs = 90;

float distance3(const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Across the listener's ground plane, the pan step's own measure (rows 0 and 2).
float acrossOf(const float* d, const float* rows) {
    const float x = rows[0] * d[0] + rows[4] * d[1] + rows[8] * d[2];
    const float z = rows[2] * d[0] + rows[6] * d[1] + rows[10] * d[2];
    return std::sqrt(x * x + z * z);
}

// Where the pan step places a sound close to you (placed: in and out).
void placeClose(float* placed, const float* listener, const float* rows) {
    const ULONGLONG now = GetTickCount64();
    VoicePlaces& v = g_places;
    if (now - v.at >= 50) {
        v.at = now;
        v.count = armik::nearbyHeads(v.sims, 16);
        std::lock_guard<std::mutex> lock(g_listenerMutex);
        v.rooted = g_listener.rooted;
        std::memcpy(v.own, g_listener.root, sizeof(v.own));
    }
    float d[3] = {placed[0] - listener[0], placed[1] - listener[1], placed[2] - listener[2]};
    const float away = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(away < kCloseSound)) return;
    CloseStats& s = g_close;
    if (!s.since) s.since = now;
    ++s.close;
    bool open = true;
    if (acrossOf(d, rows) < kOwnHead) {
        float nearestEyes = 1e9f;
        for (int i = 0; i < v.count; ++i) nearestEyes = std::min(nearestEyes, distance3(placed, v.sims[i].eyes));
        if (nearestEyes < kTheirHead && nearestEyes < away) {
            ++s.theirs;  // at another Sim's head right by yours: from its direction
        } else {
            ++s.atHead;
            open = false;
        }
    } else {
        const bool atOwn = v.rooted && distance3(placed, v.own) < kAtRoot;
        int sim = -1;
        float nearestRoot = 1e9f, nearestEyes = 1e9f;
        for (int i = 0; i < v.count; ++i) {
            const float r = distance3(placed, v.sims[i].root);
            if (r < nearestRoot) {
                nearestRoot = r;
                sim = r < kAtRoot ? i : -1;
            }
            nearestEyes = std::min(nearestEyes, distance3(placed, v.sims[i].eyes));
        }
        if (sim >= 0 && !atOwn) {
            ++s.toHead;
            std::memcpy(placed, v.sims[sim].eyes, sizeof(v.sims[sim].eyes));
            for (int i = 0; i < 3; ++i) d[i] = placed[i] - listener[i];
        } else if (sim >= 0) {
            ++s.shared;  // yours or theirs: can't tell, so from their head beside you
            std::memcpy(placed, v.sims[sim].eyes, sizeof(v.sims[sim].eyes));
            for (int i = 0; i < 3; ++i) d[i] = placed[i] - listener[i];
        } else if (atOwn) {
            ++s.ownRoot;
            open = false;
        } else {
            ++s.other;
            std::memcpy(s.example, d, sizeof(s.example));
            s.exampleEyes = v.count ? nearestEyes : -1;
            s.exampleRoot = v.count ? nearestRoot : -1;
        }
    }
    if (open) {
        const float across = acrossOf(d, rows);
        if (across >= kMinAcross && across < kPanCircle) {
            const float k = kPanCircle * 1.02f / across;
            for (int i = 0; i < 3; ++i) placed[i] = listener[i] + d[i] * k;
            ++s.opened;
        }
    }
    if (now - s.since < 10000) return;
    if (g_closeLogs < kCloseMaxLogs) {
        ++g_closeLogs;
        LOG("Close sounds: %llu updates within 2 m in %.0f s: %llu at your head (kept in the middle), %llu at "
            "another Sim's head right by yours (from its direction), %llu where another Sim stands (moved to its "
            "head), %llu where you and another Sim both stand (moved to its head), %llu where "
            "your Sim stands (left), %llu elsewhere (one %.2f %.2f %.2f m from your head, %.2f m from a Sim's eyes, "
            "%.2f m from where one stands); %llu under 1 m across moved out to come from their direction",
            static_cast<unsigned long long>(s.close), (now - s.since) / 1000.0,
            static_cast<unsigned long long>(s.atHead), static_cast<unsigned long long>(s.theirs),
            static_cast<unsigned long long>(s.toHead),
            static_cast<unsigned long long>(s.shared), static_cast<unsigned long long>(s.ownRoot),
            static_cast<unsigned long long>(s.other), s.example[0], s.example[1], s.example[2], s.exampleEyes,
            s.exampleRoot, static_cast<unsigned long long>(s.opened));
    }
    s = CloseStats{};
    s.since = now;
}

void hookedPan(void* sound, void* panner, const float* position, const float* listener, const float* rows,
               float nearRadius) {
    const bool firstPerson = g_panFirstPerson.load(std::memory_order_relaxed);
    if (firstPerson && g_panLogs < kPanMaxLogs) {
        // The game's own distance across the listener's ground plane (rows 0 and 2).
        const float d[3] = {position[0] - listener[0], position[1] - listener[1], position[2] - listener[2]};
        const float x = rows[0] * d[0] + rows[4] * d[1] + rows[8] * d[2];
        const float z = rows[2] * d[0] + rows[6] * d[1] + rows[10] * d[2];
        const float across = std::sqrt(x * x + z * z);
        const ULONGLONG now = GetTickCount64();
        if (!g_pan.since) g_pan.since = now;
        ++g_pan.pans;
        if (across >= kPanCircle && nearRadius < kPanCircle) {
            // The game would play this one spread toward the middle of your head.
            ++g_pan.centred;
            const float away = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (away > g_pan.farthestCentred) {
                g_pan.farthestCentred = away;
                g_pan.centredRadius = nearRadius;
                g_pan.centredAcross = across;
            }
            if (g_panAnyDistance.load(std::memory_order_relaxed)) ++g_pan.lifted;
        }
        std::memcpy(g_pan.listener, listener, sizeof(g_pan.listener));
        if (now - g_pan.since >= 10000) {
            ListenerPose pose;
            {
                std::lock_guard<std::mutex> lock(g_listenerMutex);
                pose = g_listener;
            }
            const float hx = g_pan.listener[0] - pose.position.x, hy = g_pan.listener[1] - pose.position.y,
                        hz = g_pan.listener[2] - pose.position.z;
            ++g_panLogs;
            if (g_pan.centred)
                LOG("Sound placement: %llu sound updates in %.0f s; %llu were 1 m or more away but had a near radius "
                    "under 1 m, which the game plays spread toward the middle of your head (%s); farthest %.1f m "
                    "away (%.1f m across), near radius %.2f m. Pan listener %.0f cm from your head",
                    static_cast<unsigned long long>(g_pan.pans), (now - g_pan.since) / 1000.0,
                    static_cast<unsigned long long>(g_pan.centred),
                    g_pan.lifted ? "lifted to 1 m: placed where they are" : "left as the game does it",
                    g_pan.farthestCentred, g_pan.centredAcross, g_pan.centredRadius,
                    std::sqrt(hx * hx + hy * hy + hz * hz) * 100);
            else
                LOG("Sound placement: %llu sound updates in %.0f s, none spread by a small near radius. Pan "
                    "listener %.0f cm from your head",
                    static_cast<unsigned long long>(g_pan.pans), (now - g_pan.since) / 1000.0,
                    std::sqrt(hx * hx + hy * hy + hz * hz) * 100);
            g_pan = PanStats{};
            g_pan.since = now;
        }
    }
    alignas(16) float placed[4];
    std::memcpy(placed, position, sizeof(placed));  // the pan step reads 16 bytes
    if (firstPerson && g_panAnyDistance.load(std::memory_order_relaxed)) {
        if (nearRadius < kPanCircle) nearRadius = kPanCircle;
        placeClose(placed, listener, rows);
    }
    g_originalPan(sound, panner, placed, listener, rows, nearRadius);
}

void installPanHook() {
    uint8_t* target = addrs::fn("Pan");
    if (!target) {
        LOG("Sound placement: game build not recognised; far sounds keep the game's own spread");
        return;
    }
    MH_STATUS status =
        MH_CreateHook(target, reinterpret_cast<void*>(&hookedPan), reinterpret_cast<void**>(&g_originalPan));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        LOG("Sound placement hook failed: %s", MH_StatusToString(status));
        return;
    }
    LOG("Sound placement: in first person every sound 1 m or more away is placed where it is (%s)",
        g_panAnyDistance.load() ? "on" : "off: the game's own spread");
}

// Which sounds play loud or flat in first person (diagnostic, user 2026-10-18
// 21:51Z: a Sim snoring in bed heard as if lying next to them while standing
// outside; the pan lift above was working on every 3D sound then). The
// game's per-sound distance update (0x1a9f530, audio thread) does nothing for
// a sound without the 3D bit (+0xf8 bit 0): no fall-off and no pan step, so
// it plays at full loudness in the middle of your head. For a 3D sound it
// sets the fall-off level from the distance to listener 0 (0x1a9f930) into
// the float its distance state (+0x510) points to first.
constexpr size_t kSoundPosition = 0x30, kSoundFlags = 0xf8, kSoundPanner = 0x408, kSoundDistance = 0x510;
using DistanceUpdateFn = void (*)(void* sound);
DistanceUpdateFn g_originalDistanceUpdate = nullptr;
struct HeardSound {
    float away = -1, level = 0;
    float at[3] = {};
    uint32_t flags = 0;
    bool panner = false, falloff = false;
    uintptr_t vtable = 0;
};
struct LoudStats {
    ULONGLONG since = 0, headAt = 0;
    uint64_t updates = 0, flat = 0, flatAtOrigin = 0, flatAway = 0, loudAway = 0;
    float head[3] = {};
    HeardSound flatFarthest, loudest;
};
LoudStats g_loud;
int g_loudLogs = 0;
constexpr int kLoudMaxLogs = 60;
constexpr float kLoudAway = 4.0f;  // m: a 3D sound this far off playing loud is worth a line

void describe(const HeardSound& s, char* out, size_t size) {
    const uintptr_t game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    snprintf(out, size, "%.1f m away at %.2f %.2f %.2f, level %.2f, flags 0x%x, panner %s, fall-off %s, vtable game+0x%llx",
             s.away, s.at[0], s.at[1], s.at[2], s.level, s.flags, s.panner ? "yes" : "no", s.falloff ? "yes" : "no",
             static_cast<unsigned long long>(s.vtable >= game ? s.vtable - game : s.vtable));
}

void hookedDistanceUpdate(void* sound) {
    g_originalDistanceUpdate(sound);
    if (!g_panFirstPerson.load(std::memory_order_relaxed) || g_loudLogs >= kLoudMaxLogs) return;
    LoudStats& s = g_loud;
    const ULONGLONG now = GetTickCount64();
    if (!s.since) s.since = now;
    if (now - s.headAt >= 100) {
        std::lock_guard<std::mutex> lock(g_listenerMutex);
        s.head[0] = g_listener.position.x;
        s.head[1] = g_listener.position.y;
        s.head[2] = g_listener.position.z;
        s.headAt = now;
    }
    const uint8_t* base = static_cast<const uint8_t*>(sound);
    HeardSound h;
    std::memcpy(&h.flags, base + kSoundFlags, sizeof(h.flags));
    std::memcpy(h.at, base + kSoundPosition, sizeof(h.at));
    uintptr_t panner = 0, falloff = 0;
    std::memcpy(&panner, base + kSoundPanner, sizeof(panner));
    std::memcpy(&falloff, base + kSoundDistance, sizeof(falloff));
    std::memcpy(&h.vtable, base, sizeof(h.vtable));
    h.panner = panner != 0;
    h.falloff = falloff != 0;
    const float dx = h.at[0] - s.head[0], dy = h.at[1] - s.head[1], dz = h.at[2] - s.head[2];
    h.away = std::sqrt(dx * dx + dy * dy + dz * dz);
    ++s.updates;
    if (!(h.flags & 1)) {
        ++s.flat;
        h.level = 1;  // no fall-off
        if (std::fabs(h.at[0]) + std::fabs(h.at[1]) + std::fabs(h.at[2]) < 0.01f) {
            ++s.flatAtOrigin;  // music, the UI: not placed anywhere
        } else if (h.away >= 1.5f && h.away < 500) {
            ++s.flatAway;
            if (h.away > s.flatFarthest.away) s.flatFarthest = h;
        }
    } else if (falloff && h.away >= kLoudAway) {
        uintptr_t levelAt = 0;
        if (safeRead(falloff, &levelAt, sizeof(levelAt)) && levelAt && safeRead(levelAt, &h.level, sizeof(h.level)) &&
            h.level >= 0.3f) {
            ++s.loudAway;
            if (h.level * h.away > s.loudest.level * s.loudest.away) s.loudest = h;
        }
    }
    if (now - s.since < 10000) return;
    ++g_loudLogs;
    char flat[256] = "none", loud[256] = "none";
    if (s.flatAway) describe(s.flatFarthest, flat, sizeof(flat));
    if (s.loudAway) describe(s.loudest, loud, sizeof(loud));
    LOG("Sound loudness: %llu updates in %.0f s; %llu flat (no fall-off, no direction: middle of your head), %llu of "
        "them unplaced, %llu placed 1.5 m or more away (farthest: %s); %llu 3D updates %.0f m or more away at "
        "level 0.3 or more (loudest for its distance: %s)",
        static_cast<unsigned long long>(s.updates), (now - s.since) / 1000.0, static_cast<unsigned long long>(s.flat),
        static_cast<unsigned long long>(s.flatAtOrigin), static_cast<unsigned long long>(s.flatAway), flat,
        static_cast<unsigned long long>(s.loudAway), kLoudAway, loud);
    s = LoudStats{};
    s.since = now;
}

void installLoudnessLog() {
    uint8_t* target = addrs::fn("DistanceUpdate");
    if (!target) {
        LOG("Sound loudness log: game build not recognised");
        return;
    }
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&hookedDistanceUpdate),
                                     reinterpret_cast<void**>(&g_originalDistanceUpdate));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) LOG("Sound loudness log hook failed: %s", MH_StatusToString(status));
}

// Which hands hold something: an object from Python's list whose actor sits
// in your palm. Main thread, a few times a second.
std::mutex g_heldMutex;
uint64_t g_heldIds[32];
int g_heldCount = 0;
int g_heldFrame = 0;
int g_heldLogs = 0;

// xrToWorld for code that already holds g_poseMutex (the camera copy hook):
// std::mutex is not re-entrant, so locking it again there hangs the game.
bool xrToWorldHeld(const Vec3& xr, Vec3& world) {
    if (!g_anchorValid) return false;
    float s = g_config.worldScale;
    Vec3 w = rotate(g_anchorHeading, {xr.x * s, xr.y * s, xr.z * s});
    world = {g_anchorPosition[0] + w.x, g_anchorPosition[1] + w.y, g_anchorPosition[2] + w.z};
    return true;
}

// The body's index fingertip of one hand (0 left) in the world, pushed a
// little further along the finger, so a pick through it passes just beyond
// the finger instead of hitting it. Main thread (applyEye).
uint32_t g_indexJoints[2][2] = {{~0u, ~0u}, {~0u, ~0u}};  // [hand][Index1, Index2]
uint64_t g_indexJointsId = 0;

constexpr float kHeldCurlOn = 0.7f, kHeldCurlOff = 0.5f;  // radians of game finger curl: a closed hand
bool g_heldByCurl[2] = {false, false};
int g_curlLogs = 0;

bool fingertipWorld(void* object, int hand, Vec3& out) {
    if (g_jointSpace != 1 || !g_jointIndex || hand < 0 || hand > 1) return false;
    uint64_t id = g_bodyId.load();
    if (id != g_indexJointsId) {
        uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
        if (!skeleton) return false;
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < 2; ++k) {
                char name[32];
                std::snprintf(name, sizeof(name), "b__%s_Index%d__", h ? "R" : "L", k + 1);
                g_indexJoints[h][k] = g_jointIndex(skeleton + 0x48, jointHash(name));
            }
        g_indexJointsId = id;
        LOG("Touch: index finger joints L %d %d, R %d %d", static_cast<int>(g_indexJoints[0][0]),
            static_cast<int>(g_indexJoints[0][1]), static_cast<int>(g_indexJoints[1][0]),
            static_cast<int>(g_indexJoints[1][1]));
    }
    uint32_t a = g_indexJoints[hand][0], b = g_indexJoints[hand][1];
    if (a == ~0u || b == ~0u) return false;
    alignas(16) float ta[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    alignas(16) float tb[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    vcall<JointTransformFn>(object, kJointTransformSlot)(object, a, ta);
    vcall<JointTransformFn>(object, kJointTransformSlot)(object, b, tb);
    Vec3 bone{tb[0] - ta[0], tb[1] - ta[1], tb[2] - ta[2]};
    float length = std::sqrt(bone.x * bone.x + bone.y * bone.y + bone.z * bone.z);
    if (!(length > 0.005f && length < 0.1f)) return false;
    // The last finger bone is about as long as the one before it.
    float past = length * 0.9f + g_config.touchPast;
    out = {tb[0] + bone.x / length * past, tb[1] + bone.y / length * past, tb[2] + bone.z / length * past};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

// Runs inside applyEye, with g_poseMutex held.
void checkHeld() {
    if (++g_heldFrame % 6) return;
    uint64_t ids[32];
    int n;
    {
        std::lock_guard<std::mutex> lock(g_heldMutex);
        n = g_heldCount;
        std::memcpy(ids, g_heldIds, sizeof(uint64_t) * n);
    }
    bool holding[2] = {false, false};
    // Where the candidates are, for the log only (the first dozen): the
    // lookups cost every sixth frame for nothing once the log is full.
    Vec3 palm[2];
    bool valid[2] = {false, false};
    Vec3 world[2];
    if (g_lookup && n > 0 && g_heldLogs < 12) {
        armik::palms(palm, valid);
        for (int h = 0; h < 2; ++h) valid[h] = valid[h] && xrToWorldHeld(palm[h], world[h]);
    }
    if (g_lookup && (valid[0] || valid[1])) {
        for (int i = 0; i < n; ++i) {
            uint64_t id = ids[i];
            auto* object = static_cast<uint8_t*>(g_lookup(&id));
            if (!object) continue;
            // The object's root joint in the world (it follows the hand it is
            // attached to); the actor's own position as a fallback.
            float at[3];
            std::memcpy(at, object + 0x2e0, sizeof(at));
            uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
            bool rooted = false;
            if (std::fabs(at[0]) + std::fabs(at[1]) + std::fabs(at[2]) < 0.01f) at[0] = at[1] = at[2] = NAN;
            if (skeleton) {
                uint8_t* joints = skeleton + 0x48;
                uint64_t begin = *reinterpret_cast<uint64_t*>(joints + 8), end = *reinterpret_cast<uint64_t*>(joints + 0x10);
                if (end > begin && (end - begin) / 0x70 >= 1) {
                    alignas(16) float t[12] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
                    vcall<JointTransformFn>(object, kJointTransformSlot)(object, 0, t);
                    if (std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) &&
                        std::fabs(t[0]) + std::fabs(t[1]) + std::fabs(t[2]) > 0.01f) {
                        std::memcpy(at, t, sizeof(at));
                        rooted = true;
                    }
                }
            }
            float best = 1e9f;
            int hand = -1;
            for (int h = 0; h < 2; ++h) {
                if (!valid[h]) continue;
                float dx = at[0] - world[h].x, dy = at[1] - world[h].y, dz = at[2] - world[h].z;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < best) {
                    best = d;
                    hand = h;
                }
            }
            if (g_heldLogs < 12) {
                ++g_heldLogs;
                LOG("Held check: object %016llx at %.2f %.2f %.2f (%s), nearest palm %s %.2f m",
                    static_cast<unsigned long long>(id), at[0], at[1], at[2], rooted ? "root joint" : "actor",
                    hand == 0 ? "L" : hand == 1 ? "R" : "-", best);
            }
            (void)hand;  // positions are logged only: an object merely near the palm stuck the grip on
        }
    }
    // Props an animation attaches to a hand report the origin (their place
    // comes from the hand they hang on), so the hand's own grip decides: with
    // a candidate in play, a hand the game's animation closes holds it, until
    // the animation opens it again (on and off thresholds apart, so a grip
    // near the line doesn't flicker).
    for (int h = 0; h < 2; ++h) {
        float curl = armik::gameCurl(h);
        g_heldByCurl[h] = n > 0 && (g_heldByCurl[h] ? curl > kHeldCurlOff : curl > kHeldCurlOn);
        holding[h] = g_heldByCurl[h];
        if (n > 0 && g_curlLogs < 12) {
            ++g_curlLogs;
            LOG("Held check: %d candidate(s), %s hand curled %.2f rad by the game%s", n, h ? "right" : "left", curl,
                holding[h] ? ": holding" : "");
        }
    }
    armik::setHolding(holding[0], holding[1]);
}

}  // namespace

bool installCopyHook() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_hooked) return true;
    HMODULE game = GetModuleHandleW(nullptr);
    uintptr_t hit = findPattern(game, kCopyPattern, sizeof(kCopyPattern));
    if (!hit || std::memcmp(reinterpret_cast<void*>(hit - kCopyPatternOffset), kCopyStart, sizeof(kCopyStart)) != 0) {
        LOG("Camera copy function not found (pattern %s)", hit ? "found, start mismatch" : "missing");
        return false;
    }
    void* target = reinterpret_cast<void*>(hit - kCopyPatternOffset);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(target, reinterpret_cast<void*>(&hookedCopy), reinterpret_cast<void**>(&g_originalCopy));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        LOG("Hooking camera copy function failed: %s", MH_StatusToString(status));
        return false;
    }
    g_hooked = true;
    LOG("Hooked camera copy function at game+0x%llx",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(game)));
    return true;
}

void setCopyObserver(CopyObserver observer) { g_observer = observer; }

// The game's projection build (1.128 0x1208640, Camera update calls it): for
// the render camera with a lopsided eye, our centre goes back in first.
using ProjectionFn = void (*)(void* camera);
ProjectionFn g_originalProjection = nullptr;
int g_projectionLogs = 0;

void hookedProjection(void* camera) {
    if (reinterpret_cast<uintptr_t>(camera) == g_writtenCamera.load(std::memory_order_relaxed)) {
        const float cx = g_appliedCentre[0].load(std::memory_order_relaxed);
        if (!std::isnan(cx)) {
            float* at = reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + kCentreOffset);
            const float centre[2] = {cx, g_appliedCentre[1].load(std::memory_order_relaxed)};
            if (g_projectionLogs < 3 && (at[0] != centre[0] || at[1] != centre[1])) {
                ++g_projectionLogs;
                LOG("Lopsided eye: centre was %.3f %.3f at the projection build, put back %.3f %.3f", at[0], at[1],
                    centre[0], centre[1]);
            }
            std::memcpy(at, centre, sizeof(centre));
        }
    }
    {
        // The mirror pass's camera has the eye's centre (copied) and its glass clip on (+0x280). The game's clip
        // maths assumes a centred frustum, which popped objects out of mirrors. Build it with no centre, so the clip
        // is right, then put the centre's shift (third column, x and y) back into the matrix: it is only a shear of
        // x/y, the clip is the depth row (user marks 2026-10-20: right with the lopsided eye off).
        uint8_t* cam = static_cast<uint8_t*>(camera);
        float* centre = reinterpret_cast<float*>(cam + kCentreOffset);
        uint32_t* clip = reinterpret_cast<uint32_t*>(cam + 0x280);
        float* m = reinterpret_cast<float*>(cam + 0x70);
        if (reinterpret_cast<uintptr_t>(camera) != g_writtenCamera.load(std::memory_order_relaxed) && *clip != 0 &&
            std::isfinite(centre[0]) && std::isfinite(centre[1]) && (centre[0] != 0 || centre[1] != 0)) {
            const float keep[2] = {centre[0], centre[1]};
            // The mirror camera copies its centre from a camera that still holds the left eye's (both eyes:
            // the right eye's pass reflected the room with the left eye's lopsided view, 327 screenshots:
            // its mirror showed the room shifted to one side, objects at the glass edges missing). The eye
            // being drawn now is the one applyEye last wrote: shear by that eye's centre.
            const float ax = g_appliedCentre[0].load(std::memory_order_relaxed);
            const float ay = g_appliedCentre[1].load(std::memory_order_relaxed);
            // Only a centre in the eyes' units (tangents); other cameras with a clip use pixels (1280 720).
            const bool own = std::isfinite(ax) && std::isfinite(ay) && std::fabs(keep[0]) < 1.5f && std::fabs(keep[1]) < 1.5f;
            static int compareLogs = 0;
            if (compareLogs < 8 && own && (ax != keep[0] || ay != keep[1])) {
                ++compareLogs;
                LOG("Mirror clip: camera %p had the centre %.3f %.3f, the eye being drawn has %.3f %.3f; using the eye's",
                    camera, keep[0], keep[1], ax, ay);
            }
            if (own) {
                centre[0] = ax;
                centre[1] = ay;
            }
            const uint32_t keepClip = *clip;
            *clip = 0;
            g_originalProjection(camera);  // the eye's own projection, no clip
            const float shear[2] = {m[8], m[9]};
            *clip = keepClip;
            centre[0] = centre[1] = 0;
            g_originalProjection(camera);  // centred, with the clip
            m[8] = shear[0];
            m[9] = shear[1];
            centre[0] = keep[0];
            centre[1] = keep[1];
            static int logs = 0;
            if (logs < 5) {
                ++logs;
                LOG("Mirror clip: camera %p built centred with the glass clip, then sheared by the eye's centre %.3f %.3f (%.3f %.3f)",
                    camera, keep[0], keep[1], shear[0], shear[1]);
            }
            return;
        }
    }
    clearOldCentre(camera, "projection build");
    g_originalProjection(camera);
}

void installProjectionHook() {
    uint8_t* target = addrs::fn("Projection");
    if (!target) {
        LOG("Lopsided eye: projection build not found for this game build; the view stays centred");
        return;
    }
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&hookedProjection),
                                     reinterpret_cast<void**>(&g_originalProjection));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) {
        g_originalProjection = nullptr;
        LOG("Lopsided eye: projection hook failed: %s", MH_StatusToString(status));
        return;
    }
    LOG("Lopsided eye: hooked the projection build");
}

bool installListenerHook() {
    HMODULE game = GetModuleHandleW(nullptr);
    uintptr_t hit = findMasked(game, kListenerPattern, sizeof(kListenerPattern) / sizeof(kListenerPattern[0]));
    if (!hit) {
        LOG("Audio listener update not found; audio stays at the game camera");
        return false;
    }
    // call rel32 -> getter "mov rax, [rip+rel32]; ret" -> the view global
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<void*>(hit + kListenerViewCall + 1), 4);
    auto* getter = reinterpret_cast<uint8_t*>(hit + kListenerViewCall + 5 + rel);
    if (!(getter[0] == 0x48 && getter[1] == 0x8b && getter[2] == 0x05 && getter[7] == 0xc3)) {
        LOG("Audio listener: unexpected view getter; not hooked");
        return false;
    }
    std::memcpy(&rel, getter + 3, 4);
    g_viewGlobal = reinterpret_cast<uintptr_t*>(getter + 7 + rel);
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(hit), reinterpret_cast<void*>(&hookedListener),
                                     reinterpret_cast<void**>(&g_originalListener));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(hit));
    if (status != MH_OK) {
        LOG("Audio listener hook failed: %s", MH_StatusToString(status));
        return false;
    }
    LOG("Hooked audio listener update at game+0x%llx",
        static_cast<unsigned long long>(hit - reinterpret_cast<uintptr_t>(game)));
    return true;
}

void init(const Config& config) {
    player::configure(config.walkSpeed, config.runSpeed, config.bodyForward, config.bodyHeight, config.touchBodies && config.armIk);
    player::setKeepHeight(config.bodyKeepHeight);
    player::setSmoothSlide(config.bodySmoothSlide);
    player::setHeadRepel(config.headRepel, config.headRadius);
    player::setStopTime(config.bodyStopTime);
    walkstop::setClips(config.bodyWalkClips);
    g_config = config;
    // The test host has no game camera; only look inside the game.
    if (!GetModuleHandleW(L"python37_x64.dll")) return;
    if (installCopyHook()) {
        searchManager();
        installProjectionHook();
    }
    findSkeletonApi();
    g_panAnyDistance.store(config.audioPanAnyDistance);
    if (config.audioFollowsHead && installListenerHook()) {
        installPanHook();
        installLoudnessLog();
    }
}

void setSeatFollow(bool on) { g_config.bodySeatFollow = on; }

void setPanAnyDistance(bool on) {
    g_panAnyDistance.store(on);
    LOG("Sound placement: far sounds in first person %s", on ? "placed where they are" : "spread as the game does");
}

void configureView(float smoothing, float turnSmoothing, float eyeInset, float nearClip, bool nearAdaptive,
                   float nearMax) {
    g_config.bodySmoothing = smoothing;
    g_config.bodyTurnSmoothing = turnSmoothing;
    g_config.bodyEyeInset = eyeInset;
    g_config.bodyNearClip = nearClip;
    g_config.bodyNearAdaptive = nearAdaptive;
    g_config.bodyNearMax = nearMax;
}

void setHeldCandidates(const uint64_t* ids, int count) {
    std::lock_guard<std::mutex> lock(g_heldMutex);
    g_heldCount = count < 32 ? (count > 0 ? count : 0) : 32;
    std::memcpy(g_heldIds, ids, sizeof(uint64_t) * g_heldCount);
}

void setBodyRoot(const Vec3& position, const Quat& orientation) {
    std::lock_guard<std::mutex> lock(g_rootMutex);
    g_rootPosition[0] = position.x;
    g_rootPosition[1] = position.y;
    g_rootPosition[2] = position.z;
    g_rootOrientation = orientation;
    g_rootKnown = true;
}

void setBody(uint64_t objectId) {
    if (objectId != g_bodyId.load()) {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        g_rootKnown = false;
        g_jointSpace = 0;
    }
    g_bodyId = objectId;
    g_playerReset = true;
    // The Sim's body is the game's again at once (the reset itself waits for
    // the next frame the camera runs, which god view or menus may not have).
    if (!objectId) player::release();
    LOG("Body mode %s (object id %llu)", objectId ? "on" : "off", static_cast<unsigned long long>(objectId));
}

bool available() {
    uintptr_t manager = g_manager.load();
    if (manager && liveCamera(manager)) return true;
    if (manager) {
        // The manager went away (zone change, CAS, back to menu): search again.
        g_manager = 0;
        LOG("Game camera lost; searching again");
        if (g_hooked) searchManager();
    }
    return false;
}

bool yourEyeCamera(const void* camera) {
    if (!camera) return false;
    if (reinterpret_cast<uintptr_t>(camera) == g_writtenCamera.load(std::memory_order_acquire)) return true;
    float p[3];  // the game reads it there itself for every model it draws
    std::memcpy(p, static_cast<const uint8_t*>(camera) + kPositionOffset, sizeof(p));
    for (const auto& eye : g_writtenEye) {
        float dx = p[0] - eye[0].load(std::memory_order_relaxed), dy = p[1] - eye[1].load(std::memory_order_relaxed),
              dz = p[2] - eye[2].load(std::memory_order_relaxed);
        if (dx * dx + dy * dy + dz * dz < 0.0004f) return true;  // within 2 cm
    }
    return false;
}

// The eye applyEye draws next and its frustum; g_poseMutex held.
void setPending(int eye, const Vec3& eyePosition, const Quat& eyeOrientation, const Fov& fov) {
    g_pending = {eye, eyePosition, eyeOrientation, fov};
    g_tanX = 0;
    g_centred = true;
    g_centreX = g_centreY = 0;
    if (g_config.fovOverride) {
        const float l = std::tan(fov.left), r = std::tan(fov.right), u = std::tan(fov.up), d = std::tan(fov.down);
        g_coverX = std::fmax(-l, r);
        g_coverY = std::fmax(-d, u);
        if (g_exactFov.load(std::memory_order_relaxed) && g_originalProjection && r > l && u > d) {
            // The eye's own lopsided frustum, off centre (kCentreOffset).
            g_tanX = (r - l) / 2;
            g_tanY = (u - d) / 2;
            g_centreX = (r + l) / 2;
            g_centreY = (u + d) / 2;
            g_centred = false;
        } else {
            // A centred frustum covering the eye's wider side.
            g_tanX = g_coverX;
            g_tanY = g_coverY;
            const float halfX = std::atan(g_tanX), halfY = std::atan(g_tanY);
            g_pending.fov = {-halfX, halfX, halfY, -halfY};
        }
    }
}

void apply(int eye, const Vec3& eyePosition, const Quat& eyeOrientation, const Fov& fov) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    g_active = true;
    setPending(eye, eyePosition, eyeOrientation, fov);
    ++g_applyGen;
}

void setBothEyes(bool on) {
    if (g_bothEyes.exchange(on) != on)
        LOG("Both eyes: %s", on ? "on (the game draws the left and the right eye every frame)" : "off");
    if (!on) {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_rightValid = false;
    }
}

void applyRight(const Vec3& eyePosition, const Quat& eyeOrientation, const Fov& fov) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    g_right = {1, eyePosition, eyeOrientation, fov};
    g_rightValid = true;
}

bool beginBoth() {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    const bool fresh = g_applied != g_appliedAtScene;  // a left eye was copied in since the last scene render
    g_appliedAtScene = g_applied;
    if (!fresh || !g_bothEyes.load() || !g_active || !g_rightValid || !g_pairBase.valid || g_lastAppliedEye != 0 ||
        !g_queueSize)
        return false;
    Rendered& newest = g_queue[(g_queueHead + g_queueSize - 1) % 4];
    if (newest.eye != 0) return false;
    newest.bothFirst = true;
    return true;
}

bool secondEye() {
    using UpdateFn = void (*)(void* camera, void* unused, void* matrix);
    static UpdateFn update = reinterpret_cast<UpdateFn>(addrs::fn("CameraUpdate"));
    static int logs = 0;
    uint8_t* dst = reinterpret_cast<uint8_t*>(g_writtenCamera.load(std::memory_order_acquire));
    if (!update || !dst) {
        if (logs < 1) {
            ++logs;
            LOG("Both eyes: no camera update for this game build; one eye a frame");
        }
        return false;
    }
    Rendered left;
    float tanX, tanY, centreX, centreY, coverX, coverY, written[4], before[4];
    bool centred;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        if (!g_bothEyes.load() || !g_active || !g_rightValid || !g_pairBase.valid || g_lastAppliedEye != 0)
            return false;
        left = g_pending;
        tanX = g_tanX, tanY = g_tanY, centreX = g_centreX, centreY = g_centreY, coverX = g_coverX, coverY = g_coverY;
        centred = g_centred;
        gen = g_applyGen;
        std::memcpy(written, g_writtenFrustum, sizeof(written));
        setPending(1, g_right.position, g_right.orientation, g_right.fov);
    }
    // What the renderer made of the left eye's frustum after the copy (the frame's aspect fix,
    // 0x1209d80 in 0x137b830): the same for the right eye.
    std::memcpy(before, dst + kFrustumOffset, sizeof(before));
    applyEye(dst);
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        const float kx = written[0] > 0 ? before[0] / written[0] : 1, ky = written[1] > 0 ? before[1] / written[1] : 1;
        if (std::isfinite(kx) && std::isfinite(ky) && kx > 0.5f && kx < 2 && ky > 0.5f && ky < 2 &&
            (kx != 1 || ky != 1)) {
            float f[4] = {g_writtenFrustum[0] * kx, g_writtenFrustum[1] * ky, 0, 0};
            f[2] = 2 * std::atan(f[0] / 2);
            f[3] = f[0] / f[1];
            std::memcpy(dst + kFrustumOffset, f, sizeof(f));
        }
        if (logs < 3) {
            ++logs;
            LOG("Both eyes: right eye written into %p; the renderer had scaled the left eye's frustum by %.4f / %.4f",
                dst, kx, ky);
        }
        if (g_applyGen == gen) {  // the left eye back for the next frame's copy, unless Present gave a newer one
            setPending(0, left.position, left.orientation, left.fov);
            g_pending = left;
            g_tanX = tanX, g_tanY = tanY, g_centreX = centreX, g_centreY = centreY, g_coverX = coverX,
            g_coverY = coverY;
            g_centred = centred;
        }
        g_appliedAtScene = g_applied;
    }
    update(dst, nullptr, nullptr);  // view matrices and projection (hookedProjection puts the centre in)
    return true;
}

bool frontIsBothFirst() {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    return g_queueSize && g_queue[g_queueHead].bothFirst;
}

void setHead(const Vec3& centre, const Quat& orientation) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    g_headCentre = centre;
    g_headOrientation = orientation;
}

bool xrToWorld(const Vec3& xr, Vec3& world) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    return xrToWorldHeld(xr, world);
}

bool xrDirectionToWorld(const Vec3& xr, Vec3& world) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_anchorValid) return false;
    world = rotate(g_anchorHeading, xr);
    return true;
}

bool lastView(View& out) {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    out = g_view;
    return g_viewValid;
}

void holdBodyTurn(bool hold) { g_holdTurn = hold; }

void setBodyView(bool on) { g_bodyViewWanted = on; }

void turnBody(float radians) {
    float current = g_turnRequest.load();
    while (!g_turnRequest.compare_exchange_weak(current, current + radians)) {
    }
}

void pitchBody(float radians) {
    float current = g_pitchRequest.load();
    while (!g_pitchRequest.compare_exchange_weak(current, current + radians)) {
    }
}

bool bodyActive() { return g_bodyId.load() != 0 && g_bodySmoothed; }

uint64_t bodyId() { return g_bodyId.load(); }

bool headInWorld(Vec3& position, Vec3& right, Vec3& up, Vec3& back) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!xrToWorldHeld(g_headCentre, position)) return false;
    Quat q = multiply(g_anchorHeading, g_headOrientation);
    right = rotate(q, {1, 0, 0});
    up = rotate(q, {0, 1, 0});
    back = rotate(q, {0, 0, 1});
    return true;
}

bool handJoint(int hand, uint64_t& simId, float position[3], float orientation[4]) {
    if (hand < 0 || hand > 1) return false;
    std::lock_guard<std::mutex> lock(g_handJointMutex);
    const HandJoints& h = g_handJoints;
    if (!h.ok[hand] || GetTickCount64() - h.tick > 500) return false;
    simId = h.id;
    std::memcpy(position, h.p[hand], sizeof(h.p[hand]));
    std::memcpy(orientation, h.q[hand], sizeof(h.q[hand]));
    return true;
}

bool forearm(int side, float elbow[3], float wrist[3]) {
    if (side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lock(g_forearmMutex);
    if (!g_forearmOk[side] || GetTickCount64() - g_forearmTick > 300) return false;
    std::memcpy(elbow, g_forearm[side][0], sizeof(float) * 3);
    std::memcpy(wrist, g_forearm[side][1], sizeof(float) * 3);
    return true;
}

bool torso(float joints[6][3]) {
    std::lock_guard<std::mutex> lock(g_forearmMutex);
    if (!g_torsoOk || GetTickCount64() - g_forearmTick > 300) return false;
    std::memcpy(joints, g_torso, sizeof(g_torso));
    return true;
}

void* objectPointer(uint64_t id) {
    if (!g_lookup) findSkeletonApi();
    return g_lookup ? g_lookup(&id) : nullptr;
}

bool jointTransform(uint64_t id, const char* joint, float out[12]) {
    void* object = objectPointer(id);
    if (!object || !g_jointIndex) return false;
    uint8_t* skeleton = vcall<SkeletonFn>(object, kSkeletonSlot)(object);
    if (!skeleton) return false;
    uint32_t index = g_jointIndex(skeleton + 0x48, jointHash(joint));
    if (index == ~0u) return false;
    alignas(16) float transform[12] = {};
    vcall<JointTransformFn>(object, kJointTransformSlot)(object, index, transform);
    std::memcpy(out, transform, sizeof(transform));
    return true;
}

void setWorldScale(float scale) {
    if (!(scale > 0.01f && scale < 100.0f)) return;
    std::lock_guard<std::mutex> lock(g_poseMutex);
    g_config.worldScale = scale;
    LOG("World scale set to %.2f", scale);
}

float worldScale() { return g_config.worldScale; }

void setFloor(float worldY) { g_floorY = worldY; }

bool floorInXr(float& y) {
    float floor = g_floorY.load();
    if (g_bodyId.load()) {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        if (g_rootKnown) floor = g_rootPosition[1];
    }
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_anchorValid || !std::isfinite(floor)) return false;
    y = (floor - g_anchorPosition[1]) / g_config.worldScale;
    return true;
}

bool projectLive(const Vec3& world, float& x, float& y) {
    uintptr_t camera = liveCamera(g_manager.load());
    float frustum[2], axes[16];
    if (!camera || !safeRead(camera + kFrustumOffset, frustum, sizeof(frustum)) ||
        !safeRead(camera + kRightOffset, axes, sizeof(axes)))
        return false;
    const float *right = axes, *up = axes + 4, *back = axes + 8, *position = axes + 12;
    float d[3] = {world.x - position[0], world.y - position[1], world.z - position[2]};
    float depth = -(d[0] * back[0] + d[1] * back[1] + d[2] * back[2]);
    if (depth < 0.01f || !(frustum[0] > 0) || !(frustum[1] > 0)) return false;
    x = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / depth / (frustum[0] / 2);
    y = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / depth / (frustum[1] / 2);
    return std::isfinite(x) && std::isfinite(y);
}

bool takeRendered(Rendered& out) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    static uint64_t presents = 0;
    if (++presents % 500 == 0) {
        // Expect about one camera copy per Present; more means eyes can drift.
        LOG("Stereo: %llu presents, %llu camera copies changed, %zu queued", static_cast<unsigned long long>(presents),
            static_cast<unsigned long long>(g_applied), g_queueSize);
    }
    if (!g_queueSize) return false;
    out = g_queue[g_queueHead];
    g_queueHead = (g_queueHead + 1) % 4;
    --g_queueSize;
    return true;
}

void release() {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    g_active = false;
    // No eye drawn any more (god view's flat screen, CAS): the projection build
    // stops putting the last eye's lopsided centre back, or CAS's camera kept
    // it and showed the Sim off to the side (user 2026-10-18 21:34Z).
    g_appliedCentre[0].store(NAN, std::memory_order_relaxed);
    g_appliedCentre[1].store(NAN, std::memory_order_relaxed);
    g_queueSize = 0;
    g_latestYaw = NAN;
    g_pairBase.valid = false;
    g_nextBase.valid = false;
    g_lastAppliedEye = -1;
    g_rightValid = false;
}

void setPairedEyes(bool on) { g_pairedEyes.store(on, std::memory_order_relaxed); }
void setExactFov(bool on) { g_exactFov.store(on, std::memory_order_relaxed); }

void setBodyLayer(bool on) { g_bodyLayer.store(on, std::memory_order_relaxed); }

void setPairedHands(bool on) {
    if (g_pairedHands.exchange(on, std::memory_order_relaxed) != on)
        LOG("Paired hands: %s", on ? "on (the body moves on before each pair)" : "off (the body moves on each left eye)");
}

float viewYaw() {
    // applyEye takes the pending turn and stores the new heading under this
    // lock: read both together, never one frame's turn missing or counted twice.
    std::lock_guard<std::mutex> lock(g_poseMutex);
    float yaw = g_latestYaw.load();
    return std::isfinite(yaw) ? std::remainder(yaw + g_turnRequest.load() - g_latestLead, 6.2831853f) : NAN;
}

void setTurnLead(float radians) { g_leadWanted.store(radians, std::memory_order_relaxed); }

}  // namespace s4vr::camera
