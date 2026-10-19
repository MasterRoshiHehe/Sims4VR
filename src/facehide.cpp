#include "facehide.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "MinHook.h"
#include "camera.h"
#include "framecost.h"
#include "framemap.h"
#include "gameaddr.h"
#include "log.h"
#include "player.h"

namespace s4vr::facehide {

namespace {

// The game's skinning of one mesh of a Sim, every frame it is drawn (called
// for each mesh by SceneModel vtable slot 67, game+0x11d32e0): when the mesh
// has a CPU skin buffer (+0x150) and isn't skinned on the graphics card
// (+0x1cb bit 2), its bone matrices go into the buffer's palette, then
// game+0x1285010 skins its vertices into the locked vertex buffer.
using SkinUpdateFn = void (*)(uint8_t* mesh, void* pose);
constexpr uint32_t kSkinUpdateRva = 0x1220550;
constexpr uint8_t kSkinUpdateBytes[16] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83,
                                          0xec, 0x30, 0x48, 0x8b, 0x81, 0x50, 0x01, 0x00};
// The caller: a SceneModel's skinning of all its meshes. Its RenderState
// (+0x438) gives the pose (vt+0x28, game+0x9b5eb0: the RenderState's +0x20
// actor's +0xf0). v0.13.188 matched your Sim by that pose pointer and never
// matched once (log 2026-10-17 03:23-03:37 local: no mesh classified), so now
// the actor itself is compared too.
using SceneSkinFn = void (*)(uint8_t* sceneModel);
constexpr uint32_t kSceneSkinRva = 0x11d32e0;
constexpr uint8_t kSceneSkinBytes[16] = {0x48, 0x89, 0x6c, 0x24, 0x20, 0x57, 0x48, 0x83,
                                         0xec, 0x20, 0x48, 0x8b, 0xe9, 0x48, 0x81, 0xc1};
constexpr size_t kSceneRenderState = 0x438, kRenderStateActor = 0x20;
// Game 1.128 DX11 skins Sims on the graphics card (v0.13.198; v0.13.194 log:
// the CPU skinning above ran 0 times in minutes). Each mesh's draw
// (game+0x121dcf0, mesh and its draw parameters; pose at +0x38) has the bone
// palette built by game+0x121d850(mesh, out, pose, inverse binds, rows): one
// 3x4 matrix (3 float4 rows) per bone into a scratch block that becomes the
// shader's SkinMatrices. In a pass into the 3D scene's depth (your eyes'
// view) and for your Sim's pose, the head's and the face joints' matrices are
// replaced by one that puts every vertex at the head joint (the face, eyes,
// teeth and lashes have no area left); a mesh only on joints below the head
// (eyes, teeth, lashes; one-bone meshes skip the palette) isn't drawn there.
// Mirrors, water and shadows draw into other targets and keep the face.
using PaletteFn = void (*)(uint8_t* mesh, uint8_t* out, uint8_t* pose, void* inverseBinds, uint64_t rows);
constexpr uint32_t kPaletteRva = 0x121d850;
constexpr uint8_t kPaletteBytes[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x57,
                                       0x48, 0x83, 0xec, 0x70, 0x0f, 0x29, 0x70, 0xe8};
using MeshDrawFn = void (*)(uint8_t* mesh, uint8_t* params);
constexpr uint32_t kMeshDrawRva = 0x121dcf0;
constexpr uint8_t kMeshDrawBytes[16] = {0x40, 0x55, 0x57, 0x48, 0x81, 0xec, 0xf8, 0x01,
                                        0x00, 0x00, 0xf6, 0x81, 0xcb, 0x01, 0x00, 0x00};
constexpr size_t kDrawPose = 0x38, kPoseJointSize = 0x30;
// SceneModel vtable slot 5 (game+0x11d0160, model, lod, view) draws one Sim
// for one pass; view+0x30 is that pass's camera (eye position at +0x60).
// Mirrors and shadows draw into the scene's depth too, so the camera is what
// tells your eyes' passes apart.
using SceneDrawFn = void (*)(void* model, uint32_t lod, uint8_t* view);
constexpr uint32_t kSceneDrawRva = 0x11d0160;
constexpr uint8_t kSceneDrawBytes[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x55,
                                         0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
constexpr size_t kViewCamera = 0x30;
constexpr size_t kModelCentre = 0xb0;  // the model's world bounds centre (slot 5 measures LOD from it)
// Skins `count` vertex records (0x50 bytes each: position, normal, four
// weights, four pointers into the palette) into `out` (12 bytes a vertex: int16
// position xyz + the record's w, then the normal as 4 bytes) and `positions`
// (8 bytes a vertex: the position again, the game's CPU copy).
using SkinVertsFn = void (*)(void* out, void* positions, const void* records, int count);
constexpr uint32_t kSkinVertsRva = 0x1296ee0;
constexpr uint8_t kSkinVertsBytes[16] = {0x55, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d, 0x6c,
                                         0x24, 0x20, 0x0f, 0x29, 0x75, 0xe0, 0x0f, 0x29};
// The joint index lookup (by name hash) of a skeleton's joint table.
using JointIndexFn = uint32_t (*)(void* joints, uint32_t hash);
constexpr uint32_t kJointIndexRva = 0x9119e0;
constexpr uint8_t kJointIndexBytes[16] = {0x48, 0x83, 0xec, 0x18, 0x44, 0x8b, 0xc2, 0x48,
                                          0xb8, 0xc5, 0x4e, 0xec, 0xc4, 0x4e, 0xec, 0xc4};

// The mesh (game+0x121c340 builds it).
constexpr size_t kMeshSlots = 0x138;      // uint32: bones in its palette
constexpr size_t kMeshCpuSkin = 0x150;    // its CPU skin buffer
constexpr size_t kMeshPairs = 0x160;      // uint16 pairs (pose joint, palette row = slot * 3), or null:
constexpr size_t kMeshPoseJoints = 0x130; // then uint32 pose joint per slot
constexpr size_t kMeshFlags = 0x1cb;      // bit 2: skinned on the graphics card
// The CPU skin buffer (0xa0 bytes).
constexpr size_t kSkinCount = 0x0;     // uint32 vertices
constexpr size_t kSkinRecords = 0x8;   // the vertex records
constexpr size_t kSkinPalette = 0x70;  // 3x4 matrices, 48 bytes a slot
constexpr size_t kSkinLocked = 0x80;   // the vertex buffer locked for this frame (game+0x12848d0)
constexpr size_t kRecordSize = 0x50, kRecordWeights = 0x20, kRecordBones = 0x30, kOutStride = 12;
constexpr size_t kPaletteSlot = 48;
// The game's buffer object (lock game+0x12af070): the D3D11 buffer drawn
// from, and its size.
constexpr size_t kBufferD3D = 0x10, kBufferSize = 0x20;
// The client Actor: its pose (model space, 48 bytes a joint; the same pointer
// the skinning gets: AnimationComponent::RenderState vt+0x28, game+0x9b5eb0),
// and its skeleton (Actor vt+0x20 -> +0x370), whose joint table is at +0x48
// (0x70 bytes a joint, parent index at +0x58: game+0x911b60).
constexpr size_t kActorPoseBegin = 0xf0, kActorPoseEnd = 0xf8;
constexpr size_t kActorSkeletonSlot = 0x20 / 8, kSkeletonJoints = 0x48;
constexpr size_t kJointsBegin = 0x8, kJointsEnd = 0x10, kJointSize = 0x70, kJointParent = 0x58;
constexpr uint32_t kMaxJoints = 256, kMaxSlots = 256;

constexpr ULONGLONG kStale = 500;   // ms: the Sim or a mesh not seen this long is left alone
constexpr float kFolded = 0.5f;     // a vertex this much or more on face joints is folded away
constexpr size_t kMaxPadding = 256; // bytes past the vertices a buffer may have

SkinUpdateFn g_skinUpdate = nullptr;  // the game's, behind our hook
SceneSkinFn g_sceneSkin = nullptr;    // the game's, behind our hook
PaletteFn g_palette = nullptr;        // the game's, behind our hook
MeshDrawFn g_meshDraw = nullptr;      // the game's, behind our hook
SceneDrawFn g_sceneDraw = nullptr;    // the game's, behind our hook
SkinVertsFn g_skinVerts = nullptr;
JointIndexFn g_jointIndex = nullptr;
std::atomic<bool> g_enabled{true};
bool g_installed = false;

uint32_t jointHash(const char* name) {
    uint32_t h = 0x811c9dc5;
    for (; *name; ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        h = (h * 0x1000193) ^ static_cast<uint8_t>(c);
    }
    return h;
}

// The Sim you are in (game thread writes, skinning threads read).
std::mutex g_bodyMutex;
std::atomic<void*> g_pose{nullptr};
std::atomic<void*> g_actor{nullptr};
thread_local bool t_oursByActor = false;  // inside the skinning of a SceneModel drawn from your actor
std::atomic<ULONGLONG> g_bodyTick{0};
void* g_skeleton = nullptr;
uint32_t g_jointCount = 0;
uint64_t g_face[kMaxJoints / 64] = {};    // b__Head__ and every joint below it
uint64_t g_below[kMaxJoints / 64] = {};   // only those below it (eyes, jaw, lids...)
uint64_t g_faceVersion = 0;               // changes with the skeleton
std::atomic<uint32_t> g_headJoint{~0u};   // b__Head__'s index

bool bit(const uint64_t* bits, uint32_t i) { return i < kMaxJoints && (bits[i / 64] >> (i % 64) & 1); }

// One vertex buffer of your Sim with face vertices: the game's, and ours.
struct Entry {
    ID3D11Buffer* game = nullptr;  // not owned; only compared
    uint32_t size = 0;
    std::vector<uint8_t> data;     // our copy, folded
    uint64_t gen = 0;
    ULONGLONG tick = 0;
    ID3D11Buffer* ours = nullptr;
    uint32_t oursSize = 0;
    struct Sent {
        ID3D11DeviceContext* context;
        uint64_t gen;
    } sent[16] = {};
    // What this mesh is (logged once).
    uint32_t vertices = 0, folded = 0;
};
constexpr int kMaxEntries = 16;
std::mutex g_entryMutex;
Entry g_entries[kMaxEntries];
std::atomic<ID3D11Buffer*> g_lookup[kMaxEntries] = {};  // the entries' game buffers, read without the lock
std::atomic<ULONGLONG> g_lastSwap{0};
std::atomic<uint32_t> g_swaps{0};

// What each mesh of your Sim is, by mesh: which palette slots follow face joints.
struct MeshKind {
    const uint8_t* mesh = nullptr;
    uint32_t slots = 0;
    const void* map = nullptr;
    uint64_t faceVersion = 0;
    bool face = false;  // has a joint below the head
    bool allBelow = false;  // every joint below the head (eyes, teeth, lashes)
    uint8_t slotFace[kMaxSlots] = {};
    uint32_t paletteSlots = 0;
};
constexpr int kMaxKinds = 32;
std::mutex g_kindMutex;
MeshKind g_kinds[kMaxKinds];
int g_kindNext = 0;

// Stats for the log, every 10 s.
std::atomic<ULONGLONG> g_statTick{0};
std::atomic<uint32_t> g_statMeshes{0}, g_statGpu{0}, g_statSkipped{0};
std::atomic<uint32_t> g_statCalls{0}, g_statByActor{0}, g_statByPose{0}, g_statScenes{0}, g_statOurScenes{0};
std::atomic<void*> g_statOtherPose{nullptr};
// Every call, whoever's mesh (v0.13.194: v0.13.192 logged nothing at all, so
// whether the game skins Sims here in 1.128 is the question).
std::atomic<uint32_t> g_allSkins{0}, g_allGpu{0}, g_allScenes{0};
std::atomic<uint32_t> g_palScene{0}, g_palOurs{0}, g_palFolded{0}, g_drawOurs{0}, g_drawSkipped{0};
std::atomic<ULONGLONG> g_allTick{0};
std::atomic<int> g_logs{0};

bool classify(const uint8_t* mesh, MeshKind& kind) {
    uint32_t slots = *reinterpret_cast<const uint32_t*>(mesh + kMeshSlots);
    const uint16_t* pairs = *reinterpret_cast<uint16_t* const*>(mesh + kMeshPairs);
    const uint32_t* joints = *reinterpret_cast<uint32_t* const*>(mesh + kMeshPoseJoints);
    const void* map = pairs ? static_cast<const void*>(pairs) : static_cast<const void*>(joints);
    uint64_t version = g_faceVersion;
    if (kind.mesh == mesh && kind.slots == slots && kind.map == map && kind.faceVersion == version) return kind.face;
    kind = MeshKind{};
    kind.mesh = mesh;
    kind.slots = slots;
    kind.map = map;
    kind.faceVersion = version;
    if (!slots || slots > kMaxSlots || !map) return false;
    int below = 0, face = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        uint32_t joint = pairs ? pairs[i * 2] : joints[i];
        uint32_t slot = pairs ? pairs[i * 2 + 1] / 3u : i;
        if (slot >= kMaxSlots) continue;
        kind.paletteSlots = std::max(kind.paletteSlots, slot + 1);
        if (bit(g_face, joint)) {
            kind.slotFace[slot] = 1;
            ++face;
        }
        if (bit(g_below, joint)) ++below;
    }
    kind.face = below > 0;
    kind.allBelow = below > 0 && static_cast<uint32_t>(below) == slots;
    if (g_logs.fetch_add(1) < 40)
        LOG("Face hiding: a mesh of your Sim with %u bones, %d on your head, %d of them below b__Head__%s", slots,
            face, below, kind.face ? ": its face is folded away in your view" : " (left alone)");
    return kind.face;
}

Entry* entryFor(ID3D11Buffer* game, uint32_t size, ULONGLONG now) {
    Entry* free = nullptr;
    Entry* oldest = nullptr;
    for (int i = 0; i < kMaxEntries; ++i) {
        Entry& e = g_entries[i];
        if (e.game == game) return &e;
        if (!e.game || now - e.tick > kStale) {
            if (!free) free = &e;
        } else if (!oldest || e.tick < oldest->tick) {
            oldest = &e;
        }
    }
    Entry* e = free ? free : oldest;
    int index = static_cast<int>(e - g_entries);
    e->game = game;
    e->size = size;
    e->gen = 0;
    std::memset(e->sent, 0, sizeof(e->sent));
    g_lookup[index].store(game, std::memory_order_release);
    return e;
}

thread_local std::vector<uint8_t> t_positions;

// After the game skinned `mesh` of your Sim: our folded copy of its vertices.
void fold(uint8_t* mesh) {
    uint8_t* skin = *reinterpret_cast<uint8_t**>(mesh + kMeshCpuSkin);
    if (!skin) return;
    if (mesh[kMeshFlags] & 2) {
        g_statGpu.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t* buffer = *reinterpret_cast<uint8_t**>(skin + kSkinLocked);
    uint32_t count = *reinterpret_cast<uint32_t*>(skin + kSkinCount);
    const uint8_t* records = *reinterpret_cast<uint8_t**>(skin + kSkinRecords);
    const uint8_t* palette = *reinterpret_cast<uint8_t**>(skin + kSkinPalette);
    if (!buffer || !count || !records || !palette) return;
    auto* game = *reinterpret_cast<ID3D11Buffer**>(buffer + kBufferD3D);
    uint32_t size = *reinterpret_cast<uint32_t*>(buffer + kBufferSize);
    if (!game) return;

    MeshKind kind;
    {
        std::lock_guard<std::mutex> lock(g_kindMutex);
        MeshKind* found = nullptr;
        for (auto& k : g_kinds)
            if (k.mesh == mesh) found = &k;
        if (!found) {
            found = &g_kinds[g_kindNext];
            g_kindNext = (g_kindNext + 1) % kMaxKinds;
            found->mesh = nullptr;
        }
        std::lock_guard<std::mutex> body(g_bodyMutex);
        if (!classify(mesh, *found)) return;
        kind = *found;
    }
    size_t bytes = static_cast<size_t>(count) * kOutStride;
    if (size < bytes || size - bytes > kMaxPadding) {
        g_statSkipped.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 5)
            LOG("Face hiding: a face mesh's vertex buffer is %u bytes for %u vertices (%zu expected): left alone",
                size, count, bytes);
        return;
    }

    // Skin it again, into our copy.
    std::vector<uint8_t> out(size, 0);
    t_positions.resize(static_cast<size_t>(count) * 8);
    g_skinVerts(out.data(), t_positions.data(), records, static_cast<int>(count));

    // The vertices that mostly follow face joints go to one point: where the
    // neck meets the head (the middle of the vertices only partly on it), or
    // the middle of the folded ones when nothing joins them.
    std::vector<uint32_t> folded;
    folded.reserve(count);
    double seam[3] = {}, all[3] = {};
    uint32_t seamCount = 0;
    const uint8_t* paletteEnd = palette + kind.paletteSlots * kPaletteSlot;
    for (uint32_t v = 0; v < count; ++v) {
        const uint8_t* r = records + v * kRecordSize;
        const float* w = reinterpret_cast<const float*>(r + kRecordWeights);
        const uint8_t* const* bones = reinterpret_cast<const uint8_t* const*>(r + kRecordBones);
        float face = 0;
        for (int k = 0; k < 4; ++k) {
            const uint8_t* m = bones[k];
            if (!(w[k] > 0) || m < palette || m >= paletteEnd) continue;
            if (kind.slotFace[static_cast<size_t>(m - palette) / kPaletteSlot]) face += w[k];
        }
        if (face <= 0) continue;
        const int16_t* p = reinterpret_cast<const int16_t*>(out.data() + v * kOutStride);
        if (face >= kFolded) {
            folded.push_back(v);
            for (int k = 0; k < 3; ++k) all[k] += p[k];
        } else {
            for (int k = 0; k < 3; ++k) seam[k] += p[k];
            ++seamCount;
        }
    }
    if (folded.empty()) return;
    int16_t point[3];
    for (int k = 0; k < 3; ++k) {
        double c = seamCount ? seam[k] / seamCount : all[k] / folded.size();
        point[k] = static_cast<int16_t>(std::lround(c));
    }
    for (uint32_t v : folded) std::memcpy(out.data() + v * kOutStride, point, sizeof(point));

    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_entryMutex);
    Entry* e = entryFor(game, size, now);
    bool first = e->vertices != count || e->folded != folded.size();
    e->data.swap(out);
    e->size = size;
    e->tick = now;
    ++e->gen;
    e->vertices = count;
    e->folded = static_cast<uint32_t>(folded.size());
    g_statMeshes.fetch_add(1, std::memory_order_relaxed);
    if (first && g_logs.fetch_add(1) < 40)
        LOG("Face hiding: %u of %u vertices folded away (%u join the neck)", e->folded, count, seamCount);
}

void logStats(ULONGLONG now) {
    ULONGLONG last = g_statTick.load(std::memory_order_relaxed);
    if (now - last < 10000 || !g_statTick.compare_exchange_strong(last, now) || !last) return;
    void* actor = g_actor.load(std::memory_order_relaxed);
    void* livePose = actor ? *reinterpret_cast<void**>(static_cast<uint8_t*>(actor) + kActorPoseBegin) : nullptr;
    LOG("Face hiding: in the last 10 s, %u face meshes of your Sim skinned, %u binds for your view got the copy "
        "without your face (%u meshes skinned on the graphics card and %u with unexpected buffers, left alone); "
        "%u mesh skins seen, %u yours by actor, %u by pose; %u scene models, %u drawn from your actor %p "
        "(pose %p, now %p, another %p)",
        g_statMeshes.exchange(0), g_swaps.exchange(0), g_statGpu.exchange(0), g_statSkipped.exchange(0),
        g_statCalls.exchange(0), g_statByActor.exchange(0), g_statByPose.exchange(0), g_statScenes.exchange(0),
        g_statOurScenes.exchange(0), actor, g_pose.load(std::memory_order_relaxed), livePose,
        g_statOtherPose.load(std::memory_order_relaxed));
}

void hookedSkinUpdate(uint8_t* mesh, void* pose) {
    g_skinUpdate(mesh, pose);
    g_allSkins.fetch_add(1, std::memory_order_relaxed);
    if (mesh && (mesh[kMeshFlags] & 2)) g_allGpu.fetch_add(1, std::memory_order_relaxed);
    if (!pose || !mesh || !g_enabled.load(std::memory_order_relaxed)) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_bodyTick.load(std::memory_order_relaxed) > kStale) return;
    g_statCalls.fetch_add(1, std::memory_order_relaxed);
    bool byPose = pose == g_pose.load(std::memory_order_acquire);
    if (!byPose) {
        // The actor's pose as it is now (its buffer may change between the
        // camera and the drawing).
        void* actor = g_actor.load(std::memory_order_acquire);
        byPose = actor && pose == *reinterpret_cast<void* volatile*>(static_cast<uint8_t*>(actor) + kActorPoseBegin);
    }
    if (byPose) g_statByPose.fetch_add(1, std::memory_order_relaxed);
    if (t_oursByActor) g_statByActor.fetch_add(1, std::memory_order_relaxed);
    if (!byPose && !t_oursByActor) {
        g_statOtherPose.store(pose, std::memory_order_relaxed);
    } else {
        fold(mesh);
    }
    logStats(now);
}

void hookedSceneSkin(uint8_t* sceneModel) {
    g_allScenes.fetch_add(1, std::memory_order_relaxed);
    bool ours = false;
    if (sceneModel && g_enabled.load(std::memory_order_relaxed) &&
        GetTickCount64() - g_bodyTick.load(std::memory_order_relaxed) <= kStale) {
        g_statScenes.fetch_add(1, std::memory_order_relaxed);
        auto* state = *reinterpret_cast<uint8_t**>(sceneModel + kSceneRenderState);
        void* actor = g_actor.load(std::memory_order_acquire);
        ours = state && actor && *reinterpret_cast<void**>(state + kRenderStateActor) == actor;
        if (ours) g_statOurScenes.fetch_add(1, std::memory_order_relaxed);
    }
    t_oursByActor = ours;
    g_sceneSkin(sceneModel);
    t_oursByActor = false;
}

thread_local ID3D11DeviceContext* t_sceneContext = nullptr;  // the context last bound to the scene's depth here
// 1 while a Sim is drawn from your eyes' camera, 0 from another camera, -1
// outside any Sim draw we saw (then the scene depth decides, as before).
thread_local int t_eyePass = -1;
std::atomic<uint32_t> g_passEyes{0}, g_passOther{0};
// Your Sim drawn away from you (user 2026-10-17 14:23Z: "my whole body" shows
// in front of you for a frame): its scene model, learned from its pose in the
// palette builder, is left out of your eyes' passes while its centre is
// farther than kStrayDraw from where it is pinned under you. Only once the
// centre has been seen next to the pin often (so the offset is right).
thread_local void* t_model = nullptr;
std::atomic<void*> g_ourModel{nullptr};
std::atomic<uint32_t> g_nearPin{0}, g_strayDrawn{0}, g_strayLogs{0};
constexpr float kStrayDraw = 0.45f, kNearPin = 0.25f;
constexpr uint32_t kNearPinNeeded = 300;

bool ourPose(const void* pose) {
    if (!pose) return false;
    if (pose == g_pose.load(std::memory_order_acquire)) return true;
    void* actor = g_actor.load(std::memory_order_acquire);
    return actor && pose == *reinterpret_cast<void* volatile*>(static_cast<uint8_t*>(actor) + kActorPoseBegin);
}

// This mesh's kind (which palette slots are face joints), or false.
bool kindOf(uint8_t* mesh, MeshKind& kind) {
    std::lock_guard<std::mutex> lock(g_kindMutex);
    MeshKind* found = nullptr;
    for (auto& k : g_kinds)
        if (k.mesh == mesh) found = &k;
    if (!found) {
        found = &g_kinds[g_kindNext];
        g_kindNext = (g_kindNext + 1) % kMaxKinds;
        found->mesh = nullptr;
    }
    std::lock_guard<std::mutex> body(g_bodyMutex);
    if (!classify(mesh, *found)) return false;
    kind = *found;
    return true;
}

bool inYourView() {
    return t_sceneContext && t_eyePass != 0 && g_enabled.load(std::memory_order_relaxed) &&
           GetTickCount64() - g_bodyTick.load(std::memory_order_relaxed) <= kStale;
}

void hookedPalette(uint8_t* mesh, uint8_t* out, uint8_t* pose, void* inverseBinds, uint64_t rows) {
    g_palette(mesh, out, pose, inverseBinds, rows);
    if (t_model && t_eyePass == 1 && ourPose(pose))
        g_ourModel.store(t_model, std::memory_order_relaxed);
    if (!mesh || !out || !inYourView()) return;
    g_palScene.fetch_add(1, std::memory_order_relaxed);
    if (!ourPose(pose)) return;
    g_palOurs.fetch_add(1, std::memory_order_relaxed);
    MeshKind kind;
    if (!kindOf(mesh, kind)) return;
    uint32_t head = g_headJoint.load(std::memory_order_relaxed);
    if (head >= g_jointCount) return;
    float h[3];
    std::memcpy(h, pose + static_cast<size_t>(head) * kPoseJointSize, sizeof(h));
    if (!std::isfinite(h[0]) || !std::isfinite(h[1]) || !std::isfinite(h[2])) return;
    const float rowsAt[12] = {0, 0, 0, h[0], 0, 0, 0, h[1], 0, 0, 0, h[2]};
    uint32_t n = 0;
    for (uint32_t slot = 0; slot < kind.paletteSlots && slot < kMaxSlots; ++slot) {
        if (!kind.slotFace[slot]) continue;
        std::memcpy(out + static_cast<size_t>(slot) * kPaletteSlot, rowsAt, sizeof(rowsAt));
        ++n;
    }
    if (n) {
        g_palFolded.fetch_add(1, std::memory_order_relaxed);
        g_lastSwap.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

void hookedMeshDraw(uint8_t* mesh, uint8_t* params) {
    if (mesh && params && inYourView() && ourPose(*reinterpret_cast<void**>(params + kDrawPose))) {
        g_drawOurs.fetch_add(1, std::memory_order_relaxed);
        MeshKind kind;
        if (kindOf(mesh, kind) && kind.allBelow) {
            g_drawSkipped.fetch_add(1, std::memory_order_relaxed);
            g_lastSwap.store(GetTickCount64(), std::memory_order_relaxed);
            return;
        }
    }
    g_meshDraw(mesh, params);
}

void hookedSceneDraw(void* model, uint32_t lod, uint8_t* view) {
    int previous = t_eyePass;
    if (view) {
        bool eyes = camera::yourEyeCamera(*reinterpret_cast<void**>(view + kViewCamera));
        t_eyePass = eyes ? 1 : 0;
        (eyes ? g_passEyes : g_passOther).fetch_add(1, std::memory_order_relaxed);
        framecost::simDraw(eyes);
        framemap::mark(framemap::SceneDraw, eyes ? 1 : 0);
    }
    if (t_eyePass == 1 && model && model == g_ourModel.load(std::memory_order_relaxed)) {
        float pin[7];
        if (player::pinnedAt(g_actor.load(std::memory_order_acquire), pin)) {
            const float* c = reinterpret_cast<const float*>(static_cast<uint8_t*>(model) + kModelCentre);
            float dx = c[0] - pin[0], dz = c[2] - pin[2];
            float off = std::sqrt(dx * dx + dz * dz);
            if (off < kNearPin) {
                g_nearPin.fetch_add(1, std::memory_order_relaxed);
            } else if (off > kStrayDraw && g_nearPin.load(std::memory_order_relaxed) >= kNearPinNeeded &&
                       std::isfinite(off)) {
                g_strayDrawn.fetch_add(1, std::memory_order_relaxed);
                if (g_strayLogs.fetch_add(1, std::memory_order_relaxed) < 100)
                    LOG("Player: your Sim was about to be drawn %.2f m from you in your view (centre %.2f %.2f %.2f, "
                        "pinned at %.2f %.2f %.2f): left out of that frame",
                        off, c[0], c[1], c[2], pin[0], pin[1], pin[2]);
                t_eyePass = previous;
                return;
            }
        }
    }
    void* outer = t_model;
    t_model = model;
    g_sceneDraw(model, lod, view);
    t_model = outer;
    t_eyePass = previous;
}

// D3D11 IASetVertexBuffers, of the immediate and the deferred contexts' implementations.
constexpr size_t kSetVertexBuffersSlot = 18;
using SetVertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*,
                                                    const UINT*, const UINT*);
void* g_setCode[2] = {};
SetVertexBuffersFn g_set[2] = {};
std::atomic<bool> g_any{false};  // an entry is fresh


// Our copy of entry i for a draw on `context` (its data sent through this
// context once per change), or null.
ID3D11Buffer* ours(int i, ID3D11DeviceContext* context) {
    std::lock_guard<std::mutex> lock(g_entryMutex);
    Entry& e = g_entries[i];
    ULONGLONG now = GetTickCount64();
    if (!e.game || now - e.tick > kStale || e.data.size() != e.size) return nullptr;
    if (!e.ours || e.oursSize != e.size) {
        if (e.ours) e.ours->Release();
        e.ours = nullptr;
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) return nullptr;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = e.size;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{e.data.data(), 0, 0};
        HRESULT hr = device->CreateBuffer(&desc, &init, &e.ours);
        device->Release();
        if (FAILED(hr)) {
            e.ours = nullptr;
            LOG("Face hiding: our vertex buffer (%u bytes) could not be made (0x%08lx)", e.size, hr);
            return nullptr;
        }
        e.oursSize = e.size;
        std::memset(e.sent, 0, sizeof(e.sent));
    }
    Entry::Sent* slot = nullptr;
    for (auto& s : e.sent)
        if (s.context == context) slot = &s;
    if (!slot) {
        slot = &e.sent[0];
        for (auto& s : e.sent)
            if (s.gen < slot->gen || !s.context) slot = &s;
        slot->context = context;
        slot->gen = 0;
    }
    if (slot->gen != e.gen) {
        context->UpdateSubresource(e.ours, 0, nullptr, e.data.data(), 0, 0);
        slot->gen = e.gen;
    }
    return e.ours;
}

void setVertexBuffers(SetVertexBuffersFn original, ID3D11DeviceContext* context, UINT start, UINT count,
                      ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    if (!g_any.load(std::memory_order_relaxed) || !buffers || !count || context != t_sceneContext ||
        count > D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
        original(context, start, count, buffers, strides, offsets);
        return;
    }
    ID3D11Buffer* swapped[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    bool any = false;
    for (UINT i = 0; i < count; ++i) {
        swapped[i] = buffers[i];
        if (!buffers[i]) continue;
        for (int k = 0; k < kMaxEntries; ++k) {
            if (g_lookup[k].load(std::memory_order_acquire) != buffers[i]) continue;
            if (ID3D11Buffer* b = ours(k, context)) {
                swapped[i] = b;
                any = true;
            }
            break;
        }
    }
    if (any) {
        g_swaps.fetch_add(1, std::memory_order_relaxed);
        g_lastSwap.store(GetTickCount64(), std::memory_order_relaxed);
    }
    original(context, start, count, any ? swapped : buffers, strides, offsets);
}

void STDMETHODCALLTYPE hookedSet0(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st,
                                  const UINT* o) {
    setVertexBuffers(g_set[0], c, s, n, b, st, o);
}
void STDMETHODCALLTYPE hookedSet1(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st,
                                  const UINT* o) {
    setVertexBuffers(g_set[1], c, s, n, b, st, o);
}

}  // namespace

void install(bool on) {
    g_enabled.store(on);
    if (g_installed) return;
    g_installed = true;
    uint8_t* update = gameaddr::function(kSkinUpdateRva, kSkinUpdateBytes);
    g_skinVerts = reinterpret_cast<SkinVertsFn>(gameaddr::function(kSkinVertsRva, kSkinVertsBytes));
    g_jointIndex = reinterpret_cast<JointIndexFn>(gameaddr::function(kJointIndexRva, kJointIndexBytes));
    if (!update || !g_skinVerts || !g_jointIndex) {
        LOG("Face hiding: the game's skinning not found (game build not recognised); your face shows from inside");
        return;
    }
    MH_STATUS status = MH_CreateHook(update, reinterpret_cast<void*>(&hookedSkinUpdate),
                                     reinterpret_cast<void**>(&g_skinUpdate));
    if (status == MH_OK) status = MH_EnableHook(update);
    LOG("Face hiding: hooked the game's skinning (hide_face %d): %s", on ? 1 : 0, MH_StatusToString(status));
    if (uint8_t* scene = gameaddr::function(kSceneSkinRva, kSceneSkinBytes)) {
        MH_STATUS s2 = MH_CreateHook(scene, reinterpret_cast<void*>(&hookedSceneSkin),
                                     reinterpret_cast<void**>(&g_sceneSkin));
        if (s2 == MH_OK) s2 = MH_EnableHook(scene);
        if (s2 != MH_OK) g_sceneSkin = nullptr;
        LOG("Face hiding: hooked the scene models' skinning: %s", MH_StatusToString(s2));
    }
    uint8_t* palette = gameaddr::function(kPaletteRva, kPaletteBytes);
    uint8_t* draw = gameaddr::function(kMeshDrawRva, kMeshDrawBytes);
    if (palette && draw) {
        MH_STATUS a = MH_CreateHook(palette, reinterpret_cast<void*>(&hookedPalette), reinterpret_cast<void**>(&g_palette));
        if (a == MH_OK) a = MH_EnableHook(palette);
        MH_STATUS b = MH_CreateHook(draw, reinterpret_cast<void*>(&hookedMeshDraw), reinterpret_cast<void**>(&g_meshDraw));
        if (b == MH_OK) b = MH_EnableHook(draw);
        LOG("Face hiding: hooked the graphics-card skinning (bone palettes %s, mesh draws %s)", MH_StatusToString(a),
            MH_StatusToString(b));
    } else {
        LOG("Face hiding: the graphics-card skinning not found (game build not recognised)");
    }
    if (uint8_t* scene = gameaddr::function(kSceneDrawRva, kSceneDrawBytes)) {
        MH_STATUS c = MH_CreateHook(scene, reinterpret_cast<void*>(&hookedSceneDraw),
                                    reinterpret_cast<void**>(&g_sceneDraw));
        if (c == MH_OK) c = MH_EnableHook(scene);
        LOG("Face hiding: hooked the Sims' per-camera draws: %s", MH_StatusToString(c));
    } else {
        LOG("Face hiding: the Sims' per-camera draw not found; mirrors may lose your head");
    }
}

void setEnabled(bool on) {
    if (g_enabled.exchange(on) != on) LOG("Face hiding %s", on ? "on" : "off");
}

void setBody(void* actor) {
    if (!actor || !g_skinUpdate) return;
    auto* a = static_cast<uint8_t*>(actor);
    void* pose = *reinterpret_cast<void**>(a + kActorPoseBegin);
    if (!pose || pose == *reinterpret_cast<void**>(a + kActorPoseEnd)) return;
    using SkeletonFn = uint8_t* (*)(void*);
    uint8_t* skeleton = (*reinterpret_cast<SkeletonFn**>(a))[kActorSkeletonSlot](a);
    if (!skeleton) return;
    uint8_t* joints = skeleton + kSkeletonJoints;
    auto* begin = *reinterpret_cast<uint8_t**>(joints + kJointsBegin);
    auto* end = *reinterpret_cast<uint8_t**>(joints + kJointsEnd);
    uint32_t count = begin && end > begin ? static_cast<uint32_t>((end - begin) / kJointSize) : 0;
    if (skeleton != g_skeleton || count != g_jointCount) {
        std::lock_guard<std::mutex> lock(g_bodyMutex);
        std::memset(g_face, 0, sizeof(g_face));
        std::memset(g_below, 0, sizeof(g_below));
        g_skeleton = skeleton;
        g_jointCount = count;
        ++g_faceVersion;
        uint32_t head = g_jointIndex(joints, jointHash("b__Head__"));
        g_headJoint.store(head, std::memory_order_relaxed);
        int face = 0;
        if (head < count && count <= kMaxJoints) {
            for (uint32_t j = 0; j < count; ++j) {
                int k = static_cast<int>(j);
                for (int steps = 0; k >= 0 && k < static_cast<int>(count) && steps < 64; ++steps) {
                    if (static_cast<uint32_t>(k) == head) {
                        g_face[j / 64] |= 1ull << (j % 64);
                        if (j != head) g_below[j / 64] |= 1ull << (j % 64);
                        ++face;
                        break;
                    }
                    k = *reinterpret_cast<int32_t*>(begin + static_cast<size_t>(k) * kJointSize + kJointParent);
                }
            }
        }
        uint32_t eyes[2] = {g_jointIndex(joints, jointHash("b__L_Eye__")), g_jointIndex(joints, jointHash("b__R_Eye__"))};
        LOG("Face hiding: your Sim's skeleton has %u joints, %d of them are the head and its face (head %d, eyes %s)",
            count, face, static_cast<int>(head),
            bit(g_below, eyes[0]) && bit(g_below, eyes[1]) ? "below it" : "NOT below it");
    }
    g_pose.store(pose, std::memory_order_release);
    g_actor.store(actor, std::memory_order_release);
    ULONGLONG now = GetTickCount64();
    ULONGLONG lastAll = g_allTick.load(std::memory_order_relaxed);
    if (!lastAll) {
        g_allTick.store(now, std::memory_order_relaxed);
    } else if (now - lastAll >= 10000) {
        g_allTick.store(now, std::memory_order_relaxed);
        LOG("Face hiding: in the last 10 s, your view's passes built %u bone palettes, %u for your Sim, %u with its "
            "face folded; %u of its mesh draws there, %u eye/teeth/lash meshes left out (CPU skinning: %u meshes, "
            "%u scene models); Sim draws from your eyes %u, from other cameras (mirrors, shadows) %u; your Sim's "
            "model %p seen next to you %u times in all, drawn away from you and left out %u",
            g_palScene.exchange(0), g_palOurs.exchange(0), g_palFolded.exchange(0), g_drawOurs.exchange(0),
            g_drawSkipped.exchange(0), g_allSkins.exchange(0), g_allScenes.exchange(0), g_passEyes.exchange(0),
            g_passOther.exchange(0), g_ourModel.load(), g_nearPin.load(), g_strayDrawn.exchange(0));
    }
    g_bodyTick.store(now, std::memory_order_relaxed);
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_entryMutex);
        for (auto& e : g_entries) fresh = fresh || (e.game && now - e.tick <= kStale);
    }
    g_any.store(fresh && g_enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void hookContexts(ID3D11Device* device) {
    static bool done = false;
    if (done || !g_skinUpdate) return;
    done = true;
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    ID3D11DeviceContext* deferred = nullptr;
    device->CreateDeferredContext(0, &deferred);
    void* code[2] = {(*reinterpret_cast<void***>(immediate))[kSetVertexBuffersSlot],
                     deferred ? (*reinterpret_cast<void***>(deferred))[kSetVertexBuffersSlot] : nullptr};
    if (deferred) deferred->Release();
    immediate->Release();
    if (code[1] == code[0]) code[1] = nullptr;
    void* hooks[2] = {reinterpret_cast<void*>(&hookedSet0), reinterpret_cast<void*>(&hookedSet1)};
    int hooked = 0;
    for (int i = 0; i < 2; ++i) {
        if (!code[i]) continue;
        MH_STATUS status = MH_CreateHook(code[i], hooks[i], reinterpret_cast<void**>(&g_set[i]));
        if (status == MH_OK) status = MH_EnableHook(code[i]);
        if (status == MH_OK) {
            g_setCode[i] = code[i];
            ++hooked;
        } else {
            LOG("Face hiding: IASetVertexBuffers hook %d failed (%d)", i, static_cast<int>(status));
        }
    }
    LOG("Face hiding: IASetVertexBuffers hooked (%d implementation%s)", hooked, hooked == 1 ? "" : "s");
}

void targets(ID3D11DeviceContext* context, bool sceneDepth) {
    if (sceneDepth)
        t_sceneContext = context;
    else if (t_sceneContext == context)
        t_sceneContext = nullptr;
}

bool hiding() { return GetTickCount64() - g_lastSwap.load(std::memory_order_relaxed) < 300; }

}  // namespace s4vr::facehide
