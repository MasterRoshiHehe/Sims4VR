#include "bubbles.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "config.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"

namespace s4vr::bubbles {

namespace {

// The bound-element updater (manager): projects every bound UI piece, once a frame (game thread).
// A bound piece's anchor in the world (manager, float4 out, record) -> false if its object is gone.
// Scaleform display object: GetDisplayInfo / SetDisplayInfo (value, handle, DisplayInfo*).
// The UI object (its vtable +200: UI scale) and the render system with the screen size, as the
// updater uses them: stage = (0..1 across the screen) * scale * screen pixels.

// The manager's list of bound records (circular, next pointer first), and in a record
// (BindDisplayObject 0xe14f70): +0x30 screenOffset, +0x7c shown this frame, +0x80 object id,
// +0x9c 1 = bound to an object, +0xa8 occluded by the world, +0xa9 scaled by camera,
// +0xc0/+0xd0 the display object (GFx value and handle).
constexpr size_t kManagerList = 0x50;
constexpr size_t kRecScreenOffset = 0x30, kRecShown = 0x7c, kRecObject = 0x80, kRecKind = 0x9c, kRecOccluded = 0xa8,
                 kRecScaled = 0xa9, kRecValue = 0xc0, kRecHandle = 0xd0;
// GFx DisplayInfo: doubles x +0, y +8, xscale +0x18, yscale +0x20 (percent), alpha +0x28 (percent);
// flags +0xd4 (1 x, 2 y, 0x18 scale, 0x20 alpha, 0x40 visible), visible byte +0xd6.
constexpr size_t kInfoFlags = 0xd4, kInfoVisible = 0xd6;
constexpr uint16_t kSetX = 1, kSetY = 2, kSetScale = 0x18, kSetAlpha = 0x20, kSetVisible = 0x40;

using UpdaterFn = void (*)(void* manager);
using AnchorFn = char (*)(void* manager, float* out, void* record);
using InfoFn = uint64_t (*)(void* value, void* handle, void* info);
using GetFn = void* (*)();
using ScreenFn = void (*)(void* renderSystem, int* w, int* h);

UpdaterFn g_updater = nullptr;
AnchorFn g_anchor = nullptr;
InfoFn g_getInfo = nullptr, g_setInfo = nullptr;
GetFn g_uiObject = nullptr, g_renderSystem = nullptr;
ScreenFn g_screenSize = nullptr;
std::atomic<bool> g_wanted{false};

std::mutex g_optionMutex;
Config g_options;

// The two kinds we lift out (ObjectBoundElementsMain ELEMENT_LIST): SimBalloon (screenOffset 0,0,-0.1)
// and the skill / purchase-intent bar (ActionHeadline, PurchaseIntentMeter: screenOffset -100,-50).
// Their content around the element's origin, in its own units at scale 1 (y down), padded: the
// SWF timeline bounds (union of all bubble types, show and hide; the bar with its mood clip and
// level-up glow, not the far-reaching level-up VFX). The balloon's tail is its origin.
enum Type { kBalloon, kBar, kFlyaway, kHeadline };
struct Box {
    float l, t, r, b;
    float ax, ay;  // where the anchor is (the element's origin minus screenOffset)
};
// The stat flyaway (SimFlyaway, screenOffset -150,-50: the +/- icons that float up over a Sim, a relationship
// change among them): its text field [127.8,-45,611.8,62.6] and the 70 px it rises.
// The Sim nameplate (SimHeadline, screenOffset -210,-240, not scaled by the camera): name, headline and the
// relationship bars with their +/- on a change; its size is a generous guess around the card.
const Box kBoxes[4] = {{-210, -230, 40, 28, 0, 0}, {-22, -405, 220, 50, 100, 50}, {127, -115, 612, 64, 150, 50}, {0, 0, 430, 260, 210, 240}};
constexpr float kBalloonWidth = 245;  // the size settings are a bubble's width
// Lifted pieces are drawn at this scale into slots along the top of the UI.
constexpr float kSlotScale = 0.6f;

// A lifted piece keeps its slot on the UI for as long as its record lives (and a moment after), so the
// slot the card is cut from is the slot the piece is drawn into even when the UI texture lags the game
// thread by a frame, or other pieces come and go (before: slots followed the list order, so cards
// flickered and showed another Sim's piece). One pool per type, laid out in the top middle of the UI.
constexpr int kSlotCols[4] = {7, 7, 3, 3}, kSlotRows[4] = {2, 1, 3, 1};
constexpr int kSlotMax = 21;
struct SlotOwner {
    uintptr_t rec = 0;
    ULONGLONG seen = 0;
};
SlotOwner g_slotOwner[4][kSlotMax];
constexpr ULONGLONG kSlotHoldMs = 400;

// The slot of `rec` in its type's pool (-1: the pool is full).
int slotFor(int type, uintptr_t rec, ULONGLONG now) {
    const int n = kSlotCols[type] * kSlotRows[type];
    int free = -1;
    for (int i = 0; i < n; ++i) {
        SlotOwner& o = g_slotOwner[type][i];
        if (o.rec == rec) {
            o.seen = now;
            return i;
        }
        if (free < 0 && (!o.rec || now - o.seen > kSlotHoldMs)) free = i;
    }
    if (free >= 0) {
        g_slotOwner[type][free] = {rec, now};
    }
    return free;
}

struct Piece {
    uint64_t object;
    float p[3];      // the anchor in the world
    float uv[4];     // its slot on the UI: left, top, right, bottom (0..1)
    Type type;
    bool front;      // your own Sim's, in front of you
};
constexpr int kMaxPieces = kMaxCards;
std::mutex g_pieceMutex;
Piece g_pieces[kMaxPieces];
int g_pieceCount = 0;
ULONGLONG g_piecesAt = 0;

template <typename T>
T at(const void* base, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(T));
    return value;
}

bool about(float a, float b) { return std::fabs(a - b) < 0.5f; }

// The record's attach point (eastl string at +0x88): the inline text when short, else "".
void attachPoint(const uint8_t* rec, char out[16]) {
    std::memset(out, 0, 16);
    if (rec[0x97] & 0x80) return;  // on the heap: a long bone name, never BALLOON
    std::memcpy(out, rec + 0x88, 15);
}

int classify(const uint8_t* rec) {
    char bone[16];
    attachPoint(rec, bone);
    if (!std::strcmp(bone, "BALLOON")) return kBalloon;
    float so[3];
    std::memcpy(so, rec + kRecScreenOffset, sizeof(so));
    if (about(so[0], 0) && about(so[1], 0) && std::fabs(so[2] + 0.1f) < 0.02f) return kBalloon;
    if (about(so[0], -100) && about(so[1], -50)) return kBar;
    if (about(so[0], -150) && about(so[1], -50)) return kFlyaway;
    if (about(so[0], -210) && about(so[1], -240)) return kHeadline;
    return -1;
}

void setInfo(uint8_t* rec, uint16_t flags, double x, double y, double scale, bool visible, double alpha) {
    alignas(16) uint8_t info[0x100] = {};
    void* value = at<void*>(rec, kRecValue);
    void* handle = at<void*>(rec, kRecHandle);
    if (!value || !handle) return;
    g_getInfo(value, handle, info);
    auto put = [&](size_t off, double v) { std::memcpy(info + off, &v, sizeof(v)); };
    if (flags & kSetX) put(0, x);
    if (flags & kSetY) put(8, y);
    if (flags & kSetScale) put(0x18, scale), put(0x20, scale);
    if (flags & kSetAlpha) put(0x28, alpha);
    std::memcpy(info + kInfoFlags, &flags, sizeof(flags));
    info[kInfoVisible] = visible ? 1 : 0;
    g_setInfo(value, handle, info);
}

void hookedUpdater(void* manager) {
    g_updater(manager);
    if (!manager) return;
    const bool wanted = g_wanted.load(std::memory_order_relaxed);
    bool front, hideOwn, hideNpc;
    {
        std::lock_guard<std::mutex> lock(g_optionMutex);
        front = g_options.bubbleOwnFront;
        hideOwn = g_options.bubbleHideOwn;
        hideNpc = g_options.bubbleHideNpc;
    }
    const uint64_t own = camera::bodyId();
    // Stage size, as the updater works it out.
    float stageW = 0, stageH = 0;
    if (wanted) {
        void* ui = g_uiObject();
        void* rs = g_renderSystem();
        int w = 0, h = 0;
        if (ui && rs) {
            g_screenSize(rs, &w, &h);
            using ScaleFn = float (*)(void*);
            float scale = (*reinterpret_cast<ScaleFn*>(*reinterpret_cast<uint8_t**>(ui) + 200))(ui);
            stageW = scale * w;
            stageH = scale * h;
        }
    }
    const bool slots = wanted && stageW > 16 && stageH > 16;
    const ULONGLONG nowMs = GetTickCount64();
    Piece found[kMaxPieces];
    int count = 0, hidden = 0;
    auto* head = static_cast<uint8_t*>(manager) + kManagerList;
    auto* rec = at<uint8_t*>(head, 0);
    // Diagnostics: your own Sim's bound pieces, logged when they change (first person).
    static uint64_t s_ownSeen[8] = {};
    static int s_ownLogs = 0;
    uint64_t ownNow[8] = {};
    int ownCount = 0, total = 0;
    // Diagnostics: every new kind of bound piece (bone, kind, offsets, whose), once each.
    static uint64_t s_kinds[64] = {};
    static int s_kindCount = 0;
    for (int guard = 0; rec && rec != head && guard < 512; ++guard, rec = at<uint8_t*>(rec, 0)) {
        ++total;
        if (s_kindCount < 64) {
            char bone[16];
            attachPoint(rec, bone);
            float so[3];
            std::memcpy(so, rec + kRecScreenOffset, sizeof(so));
            uint64_t sig = 1469598103934665603ull;
            auto mix = [&sig](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
            for (char ch : bone) mix(static_cast<uint8_t>(ch));
            mix(at<int>(rec, kRecKind));
            mix(static_cast<uint64_t>(static_cast<int64_t>(so[0])) & 0xffff);
            mix(static_cast<uint64_t>(static_cast<int64_t>(so[1])) & 0xffff);
            mix(own && at<uint64_t>(rec, kRecObject) == own);
            if (std::find(s_kinds, s_kinds + s_kindCount, sig) == s_kinds + s_kindCount) {
                s_kinds[s_kindCount++] = sig;
                LOG("Bubbles: bound piece kind %d bone '%s' (heap %d) object %llu%s scaled %d occluded %d screenOffset %.2f %.2f %.2f "
                    "-> type %d",
                    at<int>(rec, kRecKind), bone, (rec[0x97] & 0x80) ? 1 : 0,
                    static_cast<unsigned long long>(at<uint64_t>(rec, kRecObject)),
                    own && at<uint64_t>(rec, kRecObject) == own ? " (yours)" : "", at<uint8_t>(rec, kRecScaled),
                    at<uint8_t>(rec, kRecOccluded), so[0], so[1], so[2], classify(rec));
            }
        }
        const uint64_t object = at<uint64_t>(rec, kRecObject);
        const bool mine = own && object == own;
        if (mine && ownCount < 8) {
            float so[3];
            std::memcpy(so, rec + kRecScreenOffset, sizeof(so));
            const uint64_t key = reinterpret_cast<uintptr_t>(rec) ^ (static_cast<uint64_t>(at<uint8_t>(rec, kRecShown)) << 63) ^
                                 (static_cast<uint64_t>(classify(rec) + 2) << 56);
            ownNow[ownCount++] = key;
            if (s_ownLogs < 60 && std::find(s_ownSeen, s_ownSeen + 8, key) == s_ownSeen + 8) {
                ++s_ownLogs;
                LOG("Bubbles: your Sim's piece %p kind %d type %d shown %d scaled %d occluded %d screenOffset %.2f %.2f %.2f; "
                    "slots %d front %d hideOwn %d",
                    static_cast<void*>(rec), at<int>(rec, kRecKind), classify(rec), at<uint8_t>(rec, kRecShown),
                    at<uint8_t>(rec, kRecScaled), at<uint8_t>(rec, kRecOccluded), so[0], so[1], so[2], slots ? 1 : 0,
                    front ? 1 : 0, hideOwn ? 1 : 0);
            }
        }
        if (at<int>(rec, kRecKind) != 1) continue;
        const int type = classify(rec);
        if (type == kHeadline) {  // diagnostics: when a nameplate is up, and what the game says about it
            static uint64_t s_plateKey[16] = {};
            static int s_plateLogs = 0;
            const uint64_t key = reinterpret_cast<uintptr_t>(rec) ^ (static_cast<uint64_t>(at<uint8_t>(rec, kRecShown)) << 62) ^
                                 (static_cast<uint64_t>(at<uint8_t>(rec, 0xae)) << 61);
            uint64_t& slot = s_plateKey[(reinterpret_cast<uintptr_t>(rec) >> 4) & 15];
            if (slot != key && s_plateLogs < 40) {
                slot = key;
                ++s_plateLogs;
                LOG("Bubbles: nameplate %p of %llu%s shown %d enabled %d scaled %d occluded %d slots %d", static_cast<void*>(rec),
                    static_cast<unsigned long long>(object), mine ? " (yours)" : "", at<uint8_t>(rec, kRecShown),
                    at<uint8_t>(rec, 0xae), at<uint8_t>(rec, kRecScaled), at<uint8_t>(rec, kRecOccluded), slots ? 1 : 0);
            }
        }
        if (type < 0 || (type != kHeadline && !at<uint8_t>(rec, kRecScaled))) continue;
        if (type == kBalloon && (mine ? hideOwn : hideNpc)) {
            setInfo(rec, kSetVisible, 0, 0, 0, false, 0);
            ++hidden;
            continue;
        }
        if (!slots) {
            rec[kRecOccluded] = 1;  // the game hides it behind walls again
            continue;
        }
        const bool inFront = mine && front;
        if (mine && !inFront) continue;  // in your head: left as the game has it
        if (mine && type == kHeadline) continue;  // your own nameplate stays on the flat UI (your stat changes and relationship +/- show in front of you)
        if (!inFront && !at<uint8_t>(rec, kRecShown)) continue;
        if (count >= kMaxPieces) continue;
        float p[4] = {};
        if (!inFront && (!g_anchor(manager, p, rec) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])))
            continue;
        const Box& box = kBoxes[type];
        const float w = (box.r - box.l) * kSlotScale, h = (box.b - box.t) * kSlotScale;
        const int slot = slotFor(type, reinterpret_cast<uintptr_t>(rec), nowMs);
        if (slot < 0) continue;  // no room left
        // The pools are stacked down the top middle: balloons, bars, flyaways.
        float poolY = stageH * 0.03f;
        for (int t = 0; t < type; ++t) poolY += (kBoxes[t].b - kBoxes[t].t) * kSlotScale * kSlotRows[t];
        const float shelfX = stageW * 0.2f + (slot % kSlotCols[type]) * w, shelfY = poolY + (slot / kSlotCols[type]) * h;
        if (shelfX + w > stageW || shelfY + h > stageH) continue;
        // Its element origin so its box lands in the slot.
        const double x = shelfX - box.l * kSlotScale, y = shelfY - box.t * kSlotScale;
        rec[kRecOccluded] = 0;  // walls hide the card in the world instead
        setInfo(rec, inFront ? kSetX | kSetY | kSetScale | kSetVisible | kSetAlpha : kSetX | kSetY | kSetScale, x, y,
                kSlotScale * 100.0, true, 100.0);
        Piece& piece = found[count++];
        piece.object = object;
        std::memcpy(piece.p, p, sizeof(piece.p));
        piece.uv[0] = shelfX / stageW;
        piece.uv[1] = shelfY / stageH;
        piece.uv[2] = (shelfX + w) / stageW;
        piece.uv[3] = (shelfY + h) / stageH;
        piece.type = static_cast<Type>(type);
        piece.front = inFront;
    }
    std::copy(ownNow, ownNow + 8, s_ownSeen);
    static uint64_t s_lastOwn = 0;
    if (own != s_lastOwn && s_ownLogs < 60) {
        ++s_ownLogs;
        LOG("Bubbles: first-person Sim now %llu (%d bound pieces in all)", static_cast<unsigned long long>(own), total);
    }
    s_lastOwn = own;
    static int logged = 0;
    static int s_lastFront = -1;
    const int frontNow = static_cast<int>(std::count_if(found, found + count, [](const Piece& p) { return p.front; }));
    if (frontNow != s_lastFront && s_ownLogs < 60) {
        ++s_ownLogs;
        LOG("Bubbles: %d of your Sim's pieces in front of you", frontNow);
    }
    s_lastFront = frontNow;
    if ((count || hidden) && logged < 6) {
        ++logged;
        LOG("Bubbles: %d lifted into the world (%s in front of you), %d hidden; stage %.0fx%.0f", count,
            std::any_of(found, found + count, [](const Piece& p) { return p.front; }) ? "yours" : "none",
            hidden, stageW, stageH);
    }
    std::lock_guard<std::mutex> lock(g_pieceMutex);
    std::copy(found, found + count, g_pieces);
    g_pieceCount = count;
    g_piecesAt = GetTickCount64();
}

}  // namespace

bool install() {
    if (gameaddr::dx9()) return false;
    void* updater = addrs::fn("Updater");
    g_anchor = reinterpret_cast<AnchorFn>(addrs::fn("Anchor"));
    g_getInfo = reinterpret_cast<InfoFn>(addrs::fn("GetInfo"));
    g_setInfo = reinterpret_cast<InfoFn>(addrs::fn("SetInfo"));
    g_uiObject = reinterpret_cast<GetFn>(addrs::fn("UiObject"));
    g_renderSystem = reinterpret_cast<GetFn>(addrs::fn("RenderSystem"));
    g_screenSize = reinterpret_cast<ScreenFn>(addrs::fn("ScreenSize"));
    if (!updater || !g_anchor || !g_getInfo || !g_setInfo || !g_uiObject || !g_renderSystem || !g_screenSize) {
        LOG("Bubbles: game build not recognised; bubbles stay on the flat UI");
        return false;
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    bool ok = MH_CreateHook(updater, reinterpret_cast<void*>(&hookedUpdater), reinterpret_cast<void**>(&g_updater)) == MH_OK &&
              MH_EnableHook(updater) == MH_OK;
    LOG("Bubbles: game hook %s", ok ? "installed" : "FAILED");
    return ok;
}

void setWanted(bool wanted) {
    if (g_wanted.exchange(wanted) != wanted) LOG("Bubbles: %s", wanted ? "drawn in the world" : "left on the flat UI");
}

bool wanted() { return g_wanted.load(std::memory_order_relaxed); }

void setOptions(const Config& config) {
    std::lock_guard<std::mutex> lock(g_optionMutex);
    g_options = config;
}

int frameCards(const camera::View& view, int textureWidth, int textureHeight, Card* out, int max) {
    if (!g_wanted.load(std::memory_order_relaxed) || textureWidth <= 0 || textureHeight <= 0) return 0;
    Piece pieces[kMaxPieces];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_pieceMutex);
        if (GetTickCount64() - g_piecesAt < 200) {
            count = g_pieceCount;
            std::copy(g_pieces, g_pieces + count, pieces);
        }
    }
    if (!count) return 0;
    Config o;
    {
        std::lock_guard<std::mutex> lock(g_optionMutex);
        o = g_options;
    }
    const float W = static_cast<float>(textureWidth), H = static_cast<float>(textureHeight);
    const camera::Vec3 fwd{-view.back.x, -view.back.y, -view.back.z};
    camera::Vec3 hp{}, hr{}, hu{}, hb{};
    const bool haveHead = camera::headInWorld(hp, hr, hu, hb);
    int written = 0;
    for (int i = 0; i < count && written < max; ++i) {
        const Piece& piece = pieces[i];
        const Box& box = kBoxes[piece.type];
        camera::Vec3 centre, right, up;
        float k;  // world units per element unit
        if (piece.front) {
            static int s_noHead = 0;
            if (!haveHead && s_noHead < 3) {
                ++s_noHead;
                LOG("Bubbles: your Sim's bubble has no head pose to sit in front of");
            }
            if (!haveHead) continue;
            static int s_frontLogs = 0;
            if (s_frontLogs < 10) {
                ++s_frontLogs;
                LOG("Bubbles: your Sim's %s card, slot %.3f,%.3f-%.3f,%.3f of the UI", piece.type == kBalloon ? "bubble" : piece.type == kFlyaway ? "stat/relationship change" : "skill bar",
                    piece.uv[0], piece.uv[1], piece.uv[2], piece.uv[3]);
            }
            const float s = o.worldScale;
            float pos[3];
            std::memcpy(pos, piece.type == kBalloon ? o.bubbleOwnPos : o.skillOwnPos, sizeof(pos));
            if (piece.type == kFlyaway) pos[1] += 0.07f;  // over the skill bar's place
            k = o.bubbleOwnSize * s / kBalloonWidth;
            centre = {hp.x + (hr.x * pos[0] + hu.x * pos[1] - hb.x * pos[2]) * s,
                      hp.y + (hr.y * pos[0] + hu.y * pos[1] - hb.y * pos[2]) * s,
                      hp.z + (hr.z * pos[0] + hu.z * pos[1] - hb.z * pos[2]) * s};
            right = hr;
            up = hu;
        } else {
            camera::Vec3 a{piece.p[0], piece.p[1], piece.p[2]};
            if (piece.type == kBalloon) {  // the balloon's viewOffset (-0.06, 0.13) in camera space
                a = {a.x - 0.06f * view.right.x + 0.13f * view.up.x, a.y - 0.06f * view.right.y + 0.13f * view.up.y,
                     a.z - 0.06f * view.right.z + 0.13f * view.up.z};
            }
            const float d = (a.x - view.position.x) * fwd.x + (a.y - view.position.y) * fwd.y + (a.z - view.position.z) * fwd.z;
            if (d < 0.3f) continue;  // behind you
            k = o.bubbleNpcSize / kBalloonWidth;
            // The box's middle from the anchor (element units, y down).
            const float cx = (box.l + box.r) * 0.5f - box.ax, cy = (box.t + box.b) * 0.5f - box.ay;
            centre = {a.x + (view.right.x * cx - view.up.x * cy) * k, a.y + (view.right.y * cx - view.up.y * cy) * k,
                      a.z + (view.right.z * cx - view.up.z * cy) * k};
            right = view.right;
            up = view.up;
        }
        Card& c = out[written++];
        c.rect[0] = std::max(static_cast<int>(piece.uv[0] * W) - 1, 0);
        c.rect[1] = std::max(static_cast<int>(piece.uv[1] * H) - 1, 0);
        c.rect[2] = std::min(static_cast<int>(piece.uv[2] * W) + 1, textureWidth);
        c.rect[3] = std::min(static_cast<int>(piece.uv[3] * H) + 1, textureHeight);
        c.centre[0] = centre.x;
        c.centre[1] = centre.y;
        c.centre[2] = centre.z;
        c.x[0] = right.x;
        c.x[1] = right.y;
        c.x[2] = right.z;
        c.y[0] = up.x;
        c.y[1] = up.y;
        c.y[2] = up.z;
        c.width = (box.r - box.l) * k;
        c.height = (box.b - box.t) * k;
    }
    static int logged = 0;
    if (written && logged < 6) {
        ++logged;
        LOG("Bubbles: %d cards; first rect %d,%d-%d,%d on a %dx%d UI, %.2f x %.2f units", written, out[0].rect[0],
            out[0].rect[1], out[0].rect[2], out[0].rect[3], textureWidth, textureHeight, out[0].width, out[0].height);
    }
    return written;
}

}  // namespace s4vr::bubbles
