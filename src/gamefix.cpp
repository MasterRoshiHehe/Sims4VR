#include "gamefix.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <xmmintrin.h>

#include "MinHook.h"
#include "camera.h"
#include "gameaddr.h"
#include "log.h"
#include "settings.h"

namespace s4vr::gamefix {

namespace {

// Game 1.128.90.1030 (DX11); 1.112.519.1020 rvas and layouts in the RE notes.
constexpr uint32_t kCullRoomsRva = 0xa6bdc0;
constexpr uint8_t kCullRoomsBytes[16] = {0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20, 0x44,
                                         0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55};

// (lot room data, view, view kind, floor)
using CullRoomsFn = void (*)(void* rooms, void* view, uint8_t kind, char floor);
CullRoomsFn g_cullRooms = nullptr;

// Since 1.121 the room data has a lock of its own: a CRITICAL_SECTION at
// +0x20 (made in its ctor 0xa45170), which the culling takes while +0x48 is
// set and +0x49 clear (otherwise it returns at once). That ends the race our
// per-kind locks were for, and the game also calls the culling with it held
// (0xa4d7a6), so a lock of ours around the call could deadlock against it.
// We take the game's lock instead (it nests), which keeps the result still
// while we touch it up, and ours only around the history all lots share.
constexpr size_t kRoomsLock = 0x20, kRoomsLockOn = 0x48, kRoomsLockOff = 0x49;
SRWLOCK g_historyLock = SRWLOCK_INIT;

// In first person (setFirstPerson): what follows changes the game's drawing
// only then; god view keeps the game's own.
std::atomic<bool> g_firstPerson{false};
bool g_nearFadeWanted = false;  // near_fade = 0: no near or camera fading in first person

// First person: rooms seen from the main camera stay drawn for a moment
// after the culling drops them. Standing in a doorway the game can lose the
// room ahead for a frame or two: it finds your room from a point 8 cm in front
// of the eye but tests the doorway from the eye itself, the doorway is nearly edge-on, and
// the two eyes (drawn on alternate frames) sit on different sides of it.
// Each per-kind block of the room data (0x48 bytes from +0x50; from +0x28
// before 1.121, which put the lock in front): +0x68/+0x70 the room entries
// (0x28 bytes: byte +0 visible, +8/+0x10 the screen rects it is seen
// through), +0x84 the last frame culled. Every culled frame starts by
// resetting all entries, so a forced entry never outlives the frame.
//
// Standing in a doorway (user 2026-10-14 16:56Z: in the middle of a door
// frame the room ahead showed its walls and floor but none of its things):
// the eye is inside the doorway itself, so the doorway is seen edge-on and the
// room beyond is seen "through" a sliver or not at all. Objects in a room the
// camera isn't in are only drawn when the room is whole (+0 set) or when they
// fall inside one of the screen rects it is seen through (FUN_140ad7570), so
// the room's things vanished while its walls and floor stayed. Now ([body]
// rooms_whole) a room seen through any doorway is drawn whole, and a room
// seen from within kNearHold of where the eye is now stays whole for up to
// kNearHoldMs (5 min) even when the game loses it. That is how god view draws every
// room anyway (the camera overhead), so it costs no more than god view.
constexpr size_t kRoomsKinds = 0x50, kKindSize = 0x48, kKindEntries = 0x18, kKindRoomHere = 0x30,
                 kKindFrameDone = 0x34;
constexpr ULONGLONG kRoomHoldMs = 500;
constexpr ULONGLONG kNearHoldMs = 300000;
constexpr float kNearHold = 0.75f;  // world units (metres)
struct RoomHistory {
    void* rooms = nullptr;
    std::vector<ULONGLONG> seen;
    std::vector<camera::Vec3> seenFrom;
};
RoomHistory g_history[4];
int g_historyNext = 0;
uint64_t g_held = 0, g_widened = 0;

// The main thread (setFirstPerson) for the culling threads: where the eye
// was drawn from last, and the switch.
std::atomic<float> g_eyeX{0}, g_eyeY{0}, g_eyeZ{0};
std::atomic<bool> g_eyeKnown{false};
std::atomic<bool> g_roomsWhole{true};
ULONGLONG g_nextSwitchRead = 0;

void holdRooms(uint8_t* rooms) {
    uint8_t* block = rooms + kRoomsKinds;  // kind 0
    uint8_t* begin = *reinterpret_cast<uint8_t**>(block + kKindEntries);
    uint8_t* end = *reinterpret_cast<uint8_t**>(block + kKindEntries + 8);
    if (!begin || end <= begin) return;
    size_t count = static_cast<size_t>(end - begin) / 0x28;
    if (count > 4096) return;
    RoomHistory* h = nullptr;
    for (RoomHistory& r : g_history)
        if (r.rooms == rooms) h = &r;
    if (!h) {
        h = &g_history[g_historyNext];
        g_historyNext = (g_historyNext + 1) % 4;
        h->rooms = rooms;
        h->seen.clear();
    }
    if (h->seen.size() != count) {
        h->seen.assign(count, 0);
        h->seenFrom.assign(count, camera::Vec3{0, 0, 0});
    }
    ULONGLONG now = GetTickCount64();
    bool whole = g_roomsWhole.load(std::memory_order_relaxed);
    bool eyeKnown = whole && g_eyeKnown.load(std::memory_order_relaxed);
    camera::Vec3 eye{g_eyeX.load(std::memory_order_relaxed), g_eyeY.load(std::memory_order_relaxed),
                     g_eyeZ.load(std::memory_order_relaxed)};
    bool held = false, widened = false;
    for (size_t i = 0; i < count; ++i) {
        uint8_t* entry = begin + i * 0x28;
        bool throughDoorway = *reinterpret_cast<uint64_t*>(entry + 0x10) != *reinterpret_cast<uint64_t*>(entry + 8);
        if (entry[0] != 0 || throughDoorway) {
            h->seen[i] = now;
            if (eyeKnown) h->seenFrom[i] = eye;
            if (whole && entry[0] == 0) {
                entry[0] = 1;  // all of it, not only what lies inside the doorway's outline
                widened = true;
            }
            continue;
        }
        if (!h->seen[i]) continue;
        ULONGLONG age = now - h->seen[i];
        bool nearby = false;
        if (eyeKnown && age < kNearHoldMs) {
            const camera::Vec3& from = h->seenFrom[i];
            float dx = eye.x - from.x, dy = eye.y - from.y, dz = eye.z - from.z;
            nearby = dx * dx + dy * dy + dz * dz < kNearHold * kNearHold;
        }
        if (age < kRoomHoldMs || nearby) {
            entry[0] = 1;  // the whole room is drawn, as when the camera is overhead
            held = true;
        }
    }
    if (held && (++g_held == 1 || g_held == 100 || g_held == 10000 || g_held % 100000 == 0))
        LOG("Room culling: kept a room drawn that the game dropped near you (%llu frames so far)",
            static_cast<unsigned long long>(g_held));
    if (widened && (++g_widened == 1 || g_widened == 1000 || g_widened % 100000 == 0))
        LOG("Room culling: rooms seen through a doorway drawn whole (%llu frames so far)",
            static_cast<unsigned long long>(g_widened));
}

void hookedCullRooms(void* rooms, void* view, uint8_t kind, char floor) {
    auto* r = static_cast<uint8_t*>(rooms);
    if (!r[kRoomsLockOn] || r[kRoomsLockOff]) return g_cullRooms(rooms, view, kind, floor);  // returns at once
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(r + kRoomsLock);
    EnterCriticalSection(lock);
    auto* frameDone = reinterpret_cast<uint32_t*>(r + kRoomsKinds + kind * kKindSize + kKindFrameDone);
    uint32_t before = *frameDone;
    g_cullRooms(rooms, view, kind, floor);
    if (kind == 0 && *frameDone != before && g_firstPerson.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_historyLock);
        holdRooms(r);
        ReleaseSRWLockExclusive(&g_historyLock);
    }
    LeaveCriticalSection(lock);
}

// Near fading: the game hides any object whose bounding box, grown by a
// margin, holds the camera (rva 0x11c4910, 0x12310c0 in 1.112; returns 1
// shown or 0 hidden). On the active lot the margin is a fixed 1 m whatever the
// fade tuning says, so trees vanish ~3 m from the trunk and a fridge vanishes
// when your head is in it. In first person it always answers "shown"; in god
// view the game's own check runs. The thunk touches no argument register, so
// the game's call goes on to it unchanged.
constexpr uint32_t kNearFadeRva = 0x120a130;
constexpr uint8_t kNearFadeBytes[16] = {0x40, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00,
                                        0x00, 0x48, 0x8b, 0x9c, 0x24, 0xc8, 0x00, 0x00};

// Camera fades (Renderer.TunableFadeValues, loaded into a global at rva
// 0x249bb60; DX9 0x2390b30): objects, walls and Sims fade out when the camera
// gets within these distances (object 0.5 m, building 2 m, Sim 0.6 to 0.4 m).
// In first person they are close to 0: your hands and the counter in front of
// you never fade. The game's values go back for god view.
constexpr uint32_t kFadeValuesGlobal = 0x280ccf0;
constexpr int kFadeFields = 6;  // object start, building start, object end, building end, Sim start, Sim end
constexpr float kNoFade[kFadeFields] = {0.001f, 0.001f, 0.0f, 0.0f, 0.001f, 0.0f};
float* g_fade = nullptr;
float g_gameFade[kFadeFields] = {};
bool g_fadeSaved = false, g_fadeOurs = false;

}  // namespace
}  // namespace s4vr::gamefix

// Read by the thunk below; set with the first-person switch.
extern "C" {
volatile uint8_t s4vr_nearFadeOff = 0;
void* s4vr_nearFadeOriginal = nullptr;
alignas(16) float s4vr_nearFadeShown[4] = {1.0f, 1.0f, 1.0f, 1.0f};
void s4vr_nearFadeThunk();
}
// Every caller reads lane 0 of xmm0; all four lanes say "shown".
asm(R"(
    .text
    .p2align 4
    .globl s4vr_nearFadeThunk
s4vr_nearFadeThunk:
    cmpb $0, s4vr_nearFadeOff(%rip)
    je 1f
    movaps s4vr_nearFadeShown(%rip), %xmm0
    ret
1:
    jmp *s4vr_nearFadeOriginal(%rip)
)");

namespace s4vr::gamefix {
namespace {

void hookNearFade() {
    uint8_t* target = gameaddr::function(kNearFadeRva, kNearFadeBytes);
    if (!target) {
        LOG("Near fading: game build not recognised; left on");
        return;
    }
    // MinHook rather than a byte patch: it parks the game's threads while it
    // writes, so none is caught halfway through the old first instructions.
    if (MH_CreateHook(target, reinterpret_cast<void*>(&s4vr_nearFadeThunk), &s4vr_nearFadeOriginal) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Near fading: hook failed; left on");
        return;
    }
    LOG("Near fading: off in first person (nothing turns see-through when you get close); the game's own in god view");
}

void applyFades(bool firstPerson) {
    if (!g_fade) return;
    if (firstPerson) {
        if (!g_fadeOurs) {
            std::memcpy(g_gameFade, g_fade, sizeof(g_gameFade));
            g_fadeSaved = true;
            static int logged = 0;
            if (logged++ < 3)
                LOG("Camera fades: off in first person (the game's: object %.2f, building %.2f, Sim %.2f to %.2f m)",
                    g_gameFade[0], g_gameFade[1], g_gameFade[4], g_gameFade[5]);
        }
        // Every frame: a lot load reads the tuning in again.
        if (std::memcmp(g_fade, kNoFade, sizeof(kNoFade)) != 0) std::memcpy(g_fade, kNoFade, sizeof(kNoFade));
        g_fadeOurs = true;
    } else if (g_fadeOurs) {
        if (g_fadeSaved && std::memcmp(g_fade, kNoFade, sizeof(kNoFade)) == 0)
            std::memcpy(g_fade, g_gameFade, sizeof(g_gameFade));
        g_fadeOurs = false;
    }
}

// Top floor in first person. The view has to show every floor (ceilings are
// the floors above), with walls up. Body mode asks for that through the
// OverrideWallsUp op (lot override slot 3), but the game drops the slot on
// its own: focusing the camera on an object (FUN_14015d330), a floor change,
// "a user moving the camera manually". The lot keeps 4 override slots at
// lot+0xec + i*0x14 (+0 on, +2 top level, +4 wall mode; slot 0 wins);
// lot+0x14c is the level shown, lot+0x150 the wall mode in effect (2 = up).
// (All unchanged in 1.121: SetOverride is the same code, and the apply
// functions 0x9fc530 / 0x9fc7d0 use the same fields.)
struct Known {
    uint32_t rva;
    uint8_t bytes[16];
};
constexpr Known kLotById{0x9e1a20, {0x48, 0x83, 0xec, 0x18, 0x44, 0x8b, 0x49, 0x20, 0x4c, 0x8b, 0x51, 0x18, 0x44, 0x8b, 0xc2, 0x8b}};
constexpr Known kTopLevel{0xa178a0, {0x40, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0xf1, 0x84, 0xd2, 0x74, 0x2a, 0x32, 0xc0, 0x90}};
constexpr Known kSetOverride{0xa1aae0, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x63, 0x44, 0x24, 0x58, 0x48, 0x8b, 0xd9, 0x4c, 0x8d}};
constexpr uint32_t kLotManagerGlobal = 0x2956f28;  // FUN_1409c1a70 returns it
constexpr uint32_t kZoneGlobal = 0x2956c30;        // the zone
// The zone's active lot id (FUN_1409b0f90): +0x160 since 1.121 (+0x168 before).
constexpr size_t kZoneLotId = 0x160;
using LotByIdFn = void* (*)(void* manager, uint32_t id);
using TopLevelFn = uint32_t (*)(void* lot, char withOverrides);
using SetOverrideFn = void (*)(void* lot, uint8_t top, uint8_t bottom, uint32_t wallMode, uint8_t a, int slot,
                               uint32_t b, uint8_t keepLevel);
LotByIdFn g_lotById = nullptr;
TopLevelFn g_topLevel = nullptr;
SetOverrideFn g_setOverride = nullptr;
uint8_t* g_zoneGlobal = nullptr;
uint8_t* g_lotManagerGlobal = nullptr;
ULONGLONG g_nextTopFloor = 0;
uint64_t g_topFloorFixes = 0;
ULONGLONG g_fightSince = 0;  // restores in a row, counted over 10 s
int g_fights = 0;

// What drops the slot: FUN_1409f5b60(lot, slot) clears one. Slot 3 is cleared
// by the camera focusing on an object (FUN_14015d330, again and again while
// it follows a Sim), Page Up / Page Down (FUN_14016aaa0), the UI's floor
// buttons (FUN_140e02e90) and the OverrideWallsUp op itself (FUN_140205350,
// walls_up False). Putting the slot back after each clear rebuilt the walls
// every half second: the walls flickered. In first person the clear is now
// refused, except from the op (leaving first person turns it off that way).
constexpr Known kClearOverride{0xa15970, {0x48, 0x63, 0xc2, 0x48, 0x8d, 0x14, 0x80, 0x80, 0xbc, 0x91, 0xec, 0x00, 0x00, 0x00, 0x00, 0x74}};
constexpr Known kWallsUpOp{0x21c390, {0x40, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7, 0x44, 0x24, 0x40, 0xfe, 0xff, 0xff, 0xff, 0x48}};
constexpr uint32_t kWallsUpOpSize = 0x118;  // same code in 1.121
using ClearOverrideFn = void (*)(void* lot, int slot, uint8_t reapply);
ClearOverrideFn g_clearOverride = nullptr;
uint8_t* g_wallsUpOp = nullptr;
std::atomic<ULONGLONG> g_holdWallsUntil{0};  // first person: keepTopFloor keeps pushing this ahead
std::atomic<uint64_t> g_refusedClears{0};

void hookedClearOverride(void* lot, int slot, uint8_t reapply) {
    if (slot == 3 && GetTickCount64() < g_holdWallsUntil.load(std::memory_order_relaxed)) {
        auto* from = static_cast<uint8_t*>(__builtin_return_address(0));
        bool fromOp = g_wallsUpOp && from >= g_wallsUpOp && from < g_wallsUpOp + kWallsUpOpSize;
        if (!fromOp) {
            uint64_t n = ++g_refusedClears;
            if (n <= 10 || n % 500 == 0)
                LOG("Walls up: kept up and all floors shown; the game tried to drop them (#%llu, from +0x%llx)",
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(from - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
            return;
        }
    }
    g_clearOverride(lot, slot, reapply);
}

}  // namespace

// The zone's active lot (the game's main thread), or null.
uint8_t* activeLot() {
    if (!g_lotById || !g_zoneGlobal || !g_lotManagerGlobal) return nullptr;
    void* zone = *reinterpret_cast<void**>(g_zoneGlobal);
    void* manager = *reinterpret_cast<void**>(g_lotManagerGlobal);
    if (!zone || !manager) return nullptr;
    // The lot table is a hash map (+0x18 buckets, +0x20 bucket count); empty
    // before a lot loads, and the lookup divides by the count.
    if (!*reinterpret_cast<void**>(static_cast<uint8_t*>(manager) + 0x18) ||
        !*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(manager) + 0x20))
        return nullptr;
    uint32_t lotId = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(zone) + kZoneLotId);
    return static_cast<uint8_t*>(g_lotById(manager, lotId));
}

int shownLevel() {
    uint8_t* lot = activeLot();
    return lot ? static_cast<int8_t>(lot[0x14c]) : kNoLevel;
}

void keepTopFloor() {
    g_holdWallsUntil.store(GetTickCount64() + 1000, std::memory_order_relaxed);
    if (!g_setOverride || !g_topLevel) return;
    uint8_t* lot = activeLot();
    if (!lot) return;
    uint8_t top = static_cast<uint8_t>(g_topLevel(lot, 0));
    constexpr int kSlot = 3;
    uint8_t* slot = lot + 0xec + kSlot * 0x14;
    bool ours = slot[0] && slot[2] == top && *reinterpret_cast<uint32_t*>(slot + 4) == 2;
    bool shown = static_cast<uint8_t>(lot[0x14c]) == top && *reinterpret_cast<uint32_t*>(lot + 0x150) == 2;
    if (ours && shown) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_nextTopFloor) return;  // re-applying rebuilds the walls; never every frame
    g_nextTopFloor = now + 500;
    // Something keeps taking it away: stop fighting for a while rather than
    // flicker the walls twice a second.
    if (now - g_fightSince > 10000) {
        g_fightSince = now;
        g_fights = 0;
    }
    if (++g_fights > 6) {
        g_nextTopFloor = now + 10000;
        static int logged = 0;
        if (logged++ < 10)
            LOG("Top floor: the game keeps dropping walls-up (showing level %d, walls %u); leaving it for 10 s",
                static_cast<int8_t>(lot[0x14c]), *reinterpret_cast<uint32_t*>(lot + 0x150));
        return;
    }
    int wasLevel = static_cast<int8_t>(lot[0x14c]);
    uint32_t wasWalls = *reinterpret_cast<uint32_t*>(lot + 0x150);
    g_setOverride(lot, top, 0, 2, 0, kSlot, 0, 0);  // what the OverrideWallsUp op does
    bool won = static_cast<uint8_t>(lot[0x14c]) == top && *reinterpret_cast<uint32_t*>(lot + 0x150) == 2;
    if (!won) g_nextTopFloor = now + 5000;  // a higher-priority override holds it; try again later
    ++g_topFloorFixes;
    if (g_topFloorFixes <= 5 || g_topFloorFixes % 100 == 0)
        LOG("Top floor #%llu: the game dropped walls-up / all floors (showing level %d, walls %u); %s (top level %d)",
            static_cast<unsigned long long>(g_topFloorFixes), wasLevel, wasWalls,
            won ? "restored" : "another override still wins", static_cast<int>(top));
}

void setFirstPerson(bool on) {
    if (g_firstPerson.exchange(on, std::memory_order_relaxed) != on)
        LOG("Game changes for first person: %s", on ? "on" : "off (god view plays like the game)");
    if (on) {
        // The eye as applyEye last drew it (a frame old at most), for holdRooms.
        camera::View view;
        bool known = camera::lastView(view);
        if (known) {
            g_eyeX.store(view.position.x, std::memory_order_relaxed);
            g_eyeY.store(view.position.y, std::memory_order_relaxed);
            g_eyeZ.store(view.position.z, std::memory_order_relaxed);
        }
        g_eyeKnown.store(known, std::memory_order_relaxed);
        ULONGLONG now = GetTickCount64();
        if (now >= g_nextSwitchRead) {
            g_nextSwitchRead = now + 500;
            std::string value;
            bool whole = !settings::liveValue("body", "rooms_whole", value) || value.empty() || value[0] != '0';
            if (g_roomsWhole.exchange(whole, std::memory_order_relaxed) != whole)
                LOG("Room culling: rooms you look into %s", whole ? "drawn whole" : "culled like the game does");
        }
    }
    s4vr_nearFadeOff = on && g_nearFadeWanted ? 1 : 0;
    if (g_nearFadeWanted) applyFades(on);
}

void install(bool nearFade) {
    bool lotOk = true;
    for (const Known* k : {&kLotById, &kTopLevel, &kSetOverride}) lotOk = lotOk && gameaddr::function(k->rva, k->bytes);
    g_zoneGlobal = gameaddr::global(kZoneGlobal);
    g_lotManagerGlobal = gameaddr::global(kLotManagerGlobal);
    lotOk = lotOk && g_zoneGlobal && g_lotManagerGlobal;
    if (lotOk) {
        g_lotById = reinterpret_cast<LotByIdFn>(gameaddr::function(kLotById.rva, kLotById.bytes));
        g_topLevel = reinterpret_cast<TopLevelFn>(gameaddr::function(kTopLevel.rva, kTopLevel.bytes));
        g_setOverride = reinterpret_cast<SetOverrideFn>(gameaddr::function(kSetOverride.rva, kSetOverride.bytes));
    } else {
        LOG("Top floor keeper: game build not recognised; off");
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    g_nearFadeWanted = !nearFade;
    if (!nearFade) hookNearFade();
    g_fade = reinterpret_cast<float*>(gameaddr::global(kFadeValuesGlobal));
    if (!g_fade) LOG("Camera fades: game build not recognised; the game's own in first person too");
    if (lotOk) {
        void* clear = gameaddr::function(kClearOverride.rva, kClearOverride.bytes);
        g_wallsUpOp = gameaddr::function(kWallsUpOp.rva, kWallsUpOp.bytes);
        if (clear && g_wallsUpOp &&
            MH_CreateHook(clear, reinterpret_cast<void*>(&hookedClearOverride),
                          reinterpret_cast<void**>(&g_clearOverride)) == MH_OK &&
            MH_EnableHook(clear) == MH_OK) {
            LOG("Walls up keeper: on (first person refuses the game's walls-up drops)");
        } else {
            g_clearOverride = nullptr;
            LOG("Walls up keeper: game build not recognised; off");
        }
    }
    void* target = gameaddr::function(kCullRoomsRva, kCullRoomsBytes);
    if (!target) {
        LOG("Room culling fix: game build not recognised; off");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedCullRooms), reinterpret_cast<void**>(&g_cullRooms)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Room culling fix: hook failed; off");
        g_cullRooms = nullptr;
        return;
    }
    LOG("Room culling fix: on");
}

}  // namespace s4vr::gamefix
