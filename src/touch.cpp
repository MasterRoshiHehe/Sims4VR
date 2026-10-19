#include "touch.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::touch {

namespace {

// Globals and functions of 1.128.90.1030 (DX11), see touch.h. The offsets
// below are unchanged from 1.112 (dispatcher made at 0x1333f20, its pick code
// 0x135f710 / 0x135f900, the pick manager 0x11cb440 / 0x11cc760).
constexpr uintptr_t kRenderSystemGlobal = 0x2982de8;  // RenderSystem*; +0x138 the pick dispatcher
constexpr uintptr_t kSceneGlobal = 0x2965ce8;         // render scene; +0x80 the RenderPickManager
constexpr uint32_t kRequestPickRva = 0x1211d90;
constexpr uint8_t kRequestPickBytes[16] = {0x80, 0xb9, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x4c,
                                           0x8b, 0xc9, 0x74, 0x72, 0x44, 0x8b, 0x91, 0xec};
// void RequestPick(RenderPickManager*, int x, int y): pick at that render
// pixel with the camera of the frame that renders it.
using RequestPickFn = void (*)(void* manager, int x, int y);

// Camera fields (Sims3::Cameras::Camera): 2 tan(half fov) across and up,
// then the right, up and back axes and the position.
constexpr size_t kTanX2 = 0x14, kTanY2 = 0x18, kRight = 0x30, kUp = 0x40, kBack = 0x50, kPosition = 0x60;
// No frustum centre (camera.cpp kCentreOffset): the pick setup (0x12105f2)
// copies the camera and sets the copy's centre to the pixel's direction in a
// centred frustum of the drawn size (0x1209d20), and the answer's ray
// (0x1211f80) is made the same way. With the lopsided eye view a pixel asked
// with the centre picked about 18 degrees off what the laser points at.

// Clickable pick types: objects, Sims, misc. Not floors, walls, terrain, stairs, roofs.
bool clickable(int type) { return type == 2 || type == 3 || type == 9; }

constexpr float kTouchGap = 0.005f;   // the surface is this close past the fingertip, or the finger is in it
constexpr float kTouchDepth = 0.12f;  // ... but not this far in (something between your eye and your hand)

uintptr_t g_renderSystemGlobal = 0, g_sceneGlobal = 0;
RequestPickFn g_requestPick = nullptr;
std::atomic<int> g_pointing{-1};
std::atomic<uint32_t> g_presents{0};
uint32_t g_lastRequestPresent = ~0u;

struct Request {
    bool used = false;
    bool laser = false;  // a point on the laser, not the fingertip
    int x = 0, y = 0;
    float position[3] = {}, back[3] = {};
    float depth = 0;  // the fingertip's view depth
    float origin[3] = {}, dir[3] = {};  // the laser it was asked for
    ULONGLONG tick = 0;
};
Request g_requests[8];
int g_nextRequest = 0;

std::mutex g_contactMutex;
Contact g_contact;
int g_logs = 0;

template <typename T>
bool read(uintptr_t address, T& out) {
    SIZE_T got = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &out, sizeof(T), &got) &&
           got == sizeof(T);
}

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void publish(const Contact& c) {
    std::lock_guard<std::mutex> lock(g_contactMutex);
    g_contact = c;
}

// The laser (touch.h), set by the Present thread each frame it points into the world.
struct Laser {
    bool on = false;
    float origin[3] = {}, dir[3] = {};
    float maxLength = 0, startLength = 0;
    float length = 0;  // where its point sits now (world units along it)
    ULONGLONG tick = 0;
};
constexpr float kLaserMin = 0.05f;  // world units: never ends inside the hand
std::mutex g_laserMutex;
Laser g_laser;
LaserEnd g_laserEnd;
int g_laserLogs = 0;

// Pick types with a place in the world (PickType, server/pick_info.py): not
// portraits (10), the skewer (11) or the club panel (18), which are the UI.
bool inWorld(int type) { return type != 10 && type != 11 && type != 18; }

// An answer to a laser ask: the point moves to the depth of what was hit.
void answerLaser(const Request& r, const uint8_t* d, uint64_t self, ULONGLONG now) {
    int type = *reinterpret_cast<const int*>(d + 0x68);
    uint64_t id = *reinterpret_cast<const uint64_t*>(d + 0x88);
    float hit[3];
    std::memcpy(hit, d + 0x70, sizeof(hit));
    std::lock_guard<std::mutex> lock(g_laserMutex);
    if (!g_laser.on) return;
    float length = g_laser.maxLength;  // nothing there: as far as it goes
    if (type != 0 && (id == self || !inWorld(type))) {
        // Your own body (your arm in front of the laser's point, seen from the
        // eye) or the UI: it stays where it was until the point shows again.
        length = g_laser.length;
        type = g_laserEnd.pickType;
        id = g_laserEnd.objectId;
    } else if (type != 0) {
        float rel[3] = {hit[0] - r.origin[0], hit[1] - r.origin[1], hit[2] - r.origin[2]};
        float t = dot(rel, r.dir);
        if (!std::isfinite(t)) return;
        length = std::min(std::max(t, kLaserMin), g_laser.maxLength);
    }
    g_laser.length = length;
    g_laserEnd.valid = true;
    g_laserEnd.length = length;
    g_laserEnd.pickType = type;
    g_laserEnd.objectId = id;
    g_laserEnd.tick = now;
    static uint64_t lastId = ~0ull;
    if (id != lastId && g_laserLogs < 60) {
        ++g_laserLogs;
        LOG("Laser: ends on %016llx (pick type %d), %.2f along it (world units)", static_cast<unsigned long long>(id),
            type, length);
    }
    lastId = id;
}

}  // namespace

bool install() {
    uint8_t* request = gameaddr::function(kRequestPickRva, kRequestPickBytes);
    g_renderSystemGlobal = reinterpret_cast<uintptr_t>(gameaddr::global(kRenderSystemGlobal));
    g_sceneGlobal = reinterpret_cast<uintptr_t>(gameaddr::global(kSceneGlobal));
    if (!request || !g_renderSystemGlobal || !g_sceneGlobal) {
        LOG("Touch: the game's pick request was not found (other game version); touching objects is off");
        return false;
    }
    g_requestPick = reinterpret_cast<RequestPickFn>(request);
    LOG("Touch: ready (point with your index finger and touch things to click them)");
    return true;
}

void setPointing(int hand) {
    g_pointing = hand;
    ++g_presents;
}

int pointingHand() { return g_requestPick ? g_pointing.load() : -1; }

void update(const uint8_t* camera, uint64_t self, bool haveTip, const camera::Vec3& tip) {
    if (!g_requestPick) return;
    Laser laser;
    {
        std::lock_guard<std::mutex> lock(g_laserMutex);
        laser = g_laser;
    }
    laser.on = laser.on && GetTickCount64() - laser.tick < 250;
    if (!haveTip && !laser.on) {
        for (Request& r : g_requests) r.used = false;
        return;
    }
    uintptr_t renderSystem = 0, dispatcher = 0, scene = 0, manager = 0, dispatcherManager = 0;
    if (!read(g_renderSystemGlobal, renderSystem) || !read(renderSystem + 0x138, dispatcher) ||
        !read(g_sceneGlobal, scene) || !read(scene + 0x80, manager) || !manager || !dispatcher ||
        !read(dispatcher + 0x30, dispatcherManager) || dispatcherManager != manager) {
        if (g_logs < 40 && ++g_logs)
            LOG("Touch: pick objects not found (render system %p, dispatcher %p, pick manager %p / %p)",
                reinterpret_cast<void*>(renderSystem), reinterpret_cast<void*>(dispatcher),
                reinterpret_cast<void*>(manager), reinterpret_cast<void*>(dispatcherManager));
        return;
    }
    const uint8_t* m = reinterpret_cast<const uint8_t*>(manager);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(dispatcher);
    int W = *reinterpret_cast<const int*>(m + 0x90), H = *reinterpret_cast<const int*>(m + 0x94);
    bool enabled = m[0xa8] != 0, simsOnly = m[0xa9] != 0, mouseActive = d[0x4e] != 0;
    static bool loggedSetup = false;
    if (!loggedSetup) {
        loggedSetup = true;
        void* pickCamera = *reinterpret_cast<void* const*>(m + 0x18);
        LOG("Touch: pick manager %p (%dx%d, picking %s%s), dispatcher %p (mouse %s), pick camera %s the camera we set",
            m, W, H, enabled ? "on" : "off", simsOnly ? ", Sims only" : "", d, mouseActive ? "active" : "inactive",
            pickCamera == camera ? "is" : "is NOT");
    }
    if (W <= 0 || H <= 0 || !enabled || simsOnly) return;

    // 1. The answer to an earlier ask (the dispatcher's hover result, same thread).
    int hx = *reinterpret_cast<const int*>(d + 0x60), hy = *reinterpret_cast<const int*>(d + 0x64);
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < 8; ++k) {
        Request& r = g_requests[(g_nextRequest + 7 - k) % 8];  // newest first
        if (!r.used || r.x != hx || r.y != hy || now - r.tick > 500) continue;
        if (r.laser) {
            answerLaser(r, d, self, now);
            r.used = false;
            break;
        }
        Contact c;
        c.valid = true;
        c.pickType = *reinterpret_cast<const int*>(d + 0x68);
        c.objectId = *reinterpret_cast<const uint64_t*>(d + 0x88);
        c.u = (hx + 0.5f) / W;
        c.v = (hy + 0.5f) / H;
        c.tick = now;
        c.gap = 1e9f;
        if (c.pickType != 0) {
            float hit[3], ray[3];
            std::memcpy(hit, d + 0x70, sizeof(hit));
            std::memcpy(ray, d + 0xa0, sizeof(ray));
            float rel[3] = {hit[0] - r.position[0], hit[1] - r.position[1], hit[2] - r.position[2]};
            float depth = -dot(rel, r.back);
            c.gap = (depth - r.depth) * std::sqrt(dot(ray, ray));
            if (!std::isfinite(c.gap)) c.gap = 1e9f;
        }
        bool isSelf = c.objectId == self;
        c.touching = !isSelf && clickable(c.pickType) && c.gap < kTouchGap && c.gap > -kTouchDepth;
        // The two eyes' rays part behind the finger, so far answers alternate
        // between objects every frame: log touches, and a sample now and then.
        static bool lastTouching = false;
        static uint32_t answers = 0;
        if (g_logs < 80 && (c.touching != lastTouching || ++answers % 300 == 1)) {
            ++g_logs;
            LOG("Touch: finger over %s %016llx (pick type %d), %.3f m from its surface%s", isSelf ? "yourself" : "object",
                static_cast<unsigned long long>(c.objectId), c.pickType, c.gap, c.touching ? ": TOUCHING" : "");
        }
        lastTouching = c.touching;
        publish(c);
        r.used = false;
        break;
    }

    // 2. Ask for a pick through the fingertip (or the laser's point) with this frame's camera, once per frame.
    uint32_t present = g_presents.load();
    if (present == g_lastRequestPresent) return;
    float tanX2, tanY2, right[3], up[3], back[3], position[3];
    std::memcpy(&tanX2, camera + kTanX2, sizeof(float));
    std::memcpy(&tanY2, camera + kTanY2, sizeof(float));
    std::memcpy(right, camera + kRight, sizeof(right));
    std::memcpy(up, camera + kUp, sizeof(up));
    std::memcpy(back, camera + kBack, sizeof(back));
    std::memcpy(position, camera + kPosition, sizeof(position));
    camera::Vec3 at = tip;
    if (!haveTip)
        at = {laser.origin[0] + laser.dir[0] * laser.length, laser.origin[1] + laser.dir[1] * laser.length,
              laser.origin[2] + laser.dir[2] * laser.length};
    float rel[3] = {at.x - position[0], at.y - position[1], at.z - position[2]};
    float depth = -dot(rel, back);
    if (!(depth > 0.05f) || !(tanX2 > 0) || !(tanY2 > 0)) return;
    float fx = (dot(rel, right) / depth / tanX2 + 0.5f) * W - 0.5f;
    float fy = (0.5f - dot(rel, up) / depth / tanY2) * H - 0.5f;
    if (!(fx >= 0 && fx < W && fy >= 0 && fy < H)) return;
    Request& r = g_requests[g_nextRequest];
    g_nextRequest = (g_nextRequest + 1) % 8;
    r.used = true;
    r.laser = !haveTip;
    std::memcpy(r.origin, laser.origin, sizeof(r.origin));
    std::memcpy(r.dir, laser.dir, sizeof(r.dir));
    r.x = static_cast<int>(fx + 0.5f);
    r.y = static_cast<int>(fy + 0.5f);
    std::memcpy(r.position, position, sizeof(position));
    std::memcpy(r.back, back, sizeof(back));
    r.depth = depth;
    r.tick = now;
    g_lastRequestPresent = present;
    g_requestPick(const_cast<uint8_t*>(m), r.x, r.y);
    static int asked = 0;
    if (haveTip && ++asked == 1) LOG("Touch: first pick asked at %d,%d (fingertip %.2f m from the eye)", r.x, r.y, depth);
}

Contact latest() {
    std::lock_guard<std::mutex> lock(g_contactMutex);
    return g_contact;
}

void setLaser(const camera::Vec3& origin, const camera::Vec3& direction, float maxLength, float startLength) {
    float n = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(n > 1e-6f) || !(maxLength > kLaserMin)) return laserOff();
    std::lock_guard<std::mutex> lock(g_laserMutex);
    const float start = std::min(std::max(startLength, kLaserMin), maxLength);
    if (!g_laser.on) {
        g_laser.length = start;  // a new laser starts from its far end (or the floor)
        g_laserEnd.valid = false;
    }
    g_laser.on = true;
    g_laser.origin[0] = origin.x;
    g_laser.origin[1] = origin.y;
    g_laser.origin[2] = origin.z;
    g_laser.dir[0] = direction.x / n;
    g_laser.dir[1] = direction.y / n;
    g_laser.dir[2] = direction.z / n;
    g_laser.maxLength = maxLength;
    g_laser.startLength = start;
    g_laser.length = std::min(std::max(g_laser.length, kLaserMin), maxLength);
    g_laser.tick = GetTickCount64();
}

void laserOff() {
    std::lock_guard<std::mutex> lock(g_laserMutex);
    g_laser.on = false;
    g_laserEnd.valid = false;
}

LaserEnd laserEnd() {
    std::lock_guard<std::mutex> lock(g_laserMutex);
    return g_laserEnd;
}

}  // namespace s4vr::touch
