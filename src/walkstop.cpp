#include "walkstop.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "addrs.h"
#include "gameaddr.h"
#include "log.h"
#include "player.h"

namespace s4vr::walkstop {

namespace {

// Game 1.128.90.1030 (DX11); 1.112.519.1020 rvas in the RE notes.
// Object by id, then its routing component (as 0x9a4b80 does).
// Fallbacks if the path outlives the controller: fire route_complete, free the
// op channel, clear the route.
// Locomotion controllers' Stop (vtable slot 57): walk, simple, drift, master, proxy.
constexpr const char* kStops[] = {"StopWalk", "StopSimple", "StopDrift", "StopMaster", "StopProxy"};
constexpr size_t kStopSlot = 0x1c8 / 8;

// Unchanged in 1.121 (ClearRoute, ClearPath 0x2c36e0, SetPath 0x2c3810, the
// watchdog at 0x1919fa5, FireComplete, the path's end transform 0x2d77d0).
constexpr size_t kComponentEntity = 0x10, kEntityBody = 0x20, kEntityPath = 0x28, kBodyController = 0x1a8,
                 kPathRouteId = 0x168, kPathEndRotation = 0x190, kPathEndPosition = 0x1a0;

struct ObjectHandle {
    uint32_t kind = 1;
    uint32_t pad = 0;
    uint64_t id = 0;
};
using FindObjectFn = bool (*)(const ObjectHandle* handle, void** object);
using ComponentFn = uint8_t* (*)(void* object, uint32_t mask);
using StopFn = void (*)(void* controller, void*, void*, void*);
using FireCompleteFn = void (*)(void* path, uint64_t objectId);
using ReleaseBlockerFn = void (*)(void* path);
using ClearRouteFn = bool (*)(void* object);

FindObjectFn g_findObject = nullptr;
ComponentFn g_component = nullptr;
FireCompleteFn g_fireComplete = nullptr;
ReleaseBlockerFn g_releaseBlocker = nullptr;
ClearRouteFn g_clearRoute = nullptr;
const uint8_t* g_maskGlobal = nullptr;
uintptr_t g_pathVTable = 0;
uintptr_t g_stops[5] = {};
int g_ready = 0;  // 0 not checked, 1 ready, -1 unsupported build
struct Request {
    uint64_t object, route;
    ULONGLONG since;
};
constexpr ULONGLONG kWaitForRoute = 1000;  // ms a stop waits for a route the client hasn't started yet
std::mutex g_mutex;
std::vector<Request> g_pending;
std::vector<Request> g_waiting;  // game thread only
int g_logged = 0;
std::atomic<uint64_t> g_watchObject{0};  // clientRoute's object
std::atomic<uint64_t> g_watchRoute{0};   // its client route, read each frame
std::atomic<uint64_t> g_watchRead{0};    // the object that route was read for
uint64_t g_otherRoute = 0;               // the route a stop found instead (for the log)

// The player's walk controller (WalkLogic, the walk Stop above), read each
// frame (v0.13.194; offsets from WalkLogic slots 89/90/95 and the start check
// game+0x1b405e0 in 1.128.90.1030): its state (2 start clip, 4 stop clip), the
// path still ahead, and the walkstyle's stop and start clip distances. With
// walk clips on, stride.py ends a walk with a path the stop clip can finish
// instead of the stuck-route Stop, and the player slows down over that
// distance.
constexpr size_t kCtlState = 0x290, kCtlStartDistance = 0x298, kCtlStartMin = 0x29c, kCtlStopDistance = 0x2b4,
                 kCtlStartAngle = 0x2cc, kCtlRemaining = 0x2dc, kCtlStopEveryFrame = 0x32a;
// The body's controller (+0x1a8) is always the master (v0.13.194 never found
// a WalkLogic there); it, or a proxy, holds the active logic at +0x70.
constexpr size_t kActiveLogic = 0x70;
uintptr_t g_walkLogicVt = 0, g_masterVt = 0, g_proxyVt = 0;
std::atomic<bool> g_clips{true};
std::atomic<int> g_ctlState{-1};
std::atomic<float> g_ctlRemaining{0}, g_ctlStop{0}, g_ctlStart{0};
uint8_t* g_lastController = nullptr;
int g_lastState = -1;
int g_ctlLogs = 0;
float g_loggedStop = -1;

const char* const kNoPath = "already ended";
const char* const kOtherRoute = "another route is active";

bool ready() {
    if (g_ready) return g_ready > 0;
    for (const char* k : {"FindObject", "Component", "FireComplete", "ReleaseBlocker", "ClearRoute"}) {
        if (!addrs::fn(k)) {
            LOG("Walk stop: game build not recognised (no match for %s); off", k);
            g_ready = -1;
            return false;
        }
    }
    g_maskGlobal = addrs::global("RoutingMaskGlobal");
    g_pathVTable = reinterpret_cast<uintptr_t>(addrs::global("PathVTable"));
    for (int i = 0; i < 5; ++i) g_stops[i] = reinterpret_cast<uintptr_t>(addrs::global(kStops[i]));
    if (!g_maskGlobal || !g_pathVTable) {
        LOG("Walk stop: game build not recognised (routing globals); off");
        g_ready = -1;
        return false;
    }
    g_findObject = reinterpret_cast<FindObjectFn>(addrs::fn("FindObject"));
    g_component = reinterpret_cast<ComponentFn>(addrs::fn("Component"));
    g_fireComplete = reinterpret_cast<FireCompleteFn>(addrs::fn("FireComplete"));
    g_releaseBlocker =
        reinterpret_cast<ReleaseBlockerFn>(addrs::fn("ReleaseBlocker"));
    g_clearRoute = reinterpret_cast<ClearRouteFn>(addrs::fn("ClearRoute"));
    g_ready = 1;
    return true;
}

template <typename T>
T at(const uint8_t* p, size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof(v));
    return v;
}

// The client's routing entity of an object (nullptr: none).
uint8_t* routingEntity(uint64_t objectId, void** objectOut) {
    ObjectHandle handle;
    handle.id = objectId;
    void* object = nullptr;
    if (!g_findObject(&handle, &object) || !object) return nullptr;
    uint32_t mask = at<uint32_t>(g_maskGlobal, 0);
    uint8_t* component = g_component(object, mask);
    if (!component) return nullptr;
    if (objectOut) *objectOut = object;
    return at<uint8_t*>(component, kComponentEntity);
}

// The route id of the object's active client path (0: none).
uint64_t activeRoute(uint64_t objectId) {
    uint8_t* entity = routingEntity(objectId, nullptr);
    auto* path = entity ? at<uint8_t*>(entity, kEntityPath) : nullptr;
    if (!path || at<uintptr_t>(path, 0) != g_pathVTable) return 0;
    return at<uint64_t>(path, kPathRouteId);
}

const char* stopRoute(uint64_t objectId, uint64_t routeId) {
    void* object = nullptr;
    uint8_t* entity = routingEntity(objectId, &object);
    if (!object) return "no object";
    if (!entity) return "no routing entity";
    auto* path = at<uint8_t*>(entity, kEntityPath);
    if (!path) return kNoPath;
    if (at<uintptr_t>(path, 0) != g_pathVTable) return "unknown path object";
    uint64_t active = at<uint64_t>(path, kPathRouteId);
    if (routeId && active != routeId) {
        g_otherRoute = active;
        return kOtherRoute;
    }

    // The stop puts the body at the path's end (route ahead of you, 6 m when
    // running, through walls): make that where the body already is, also when
    // the game has just taken the Sim (free movement restarts from there).
    float place[7];
    if (player::pinPlace(place)) {
        std::memcpy(path + kPathEndRotation, place + 3, 4 * sizeof(float));
        std::memcpy(path + kPathEndPosition, place, 3 * sizeof(float));
    }

    auto* body = at<uint8_t*>(entity, kEntityBody);
    auto* controller = body ? at<uint8_t*>(body, kBodyController) : nullptr;
    const char* how = "path cleared";
    if (controller) {
        auto stop = at<uintptr_t>(at<uint8_t*>(controller, 0), kStopSlot * 8);
        bool known = false;
        for (uintptr_t s : g_stops) known = known || (s && stop == s);
        if (!known) return "unknown locomotion controller";
        reinterpret_cast<StopFn>(stop)(controller, nullptr, nullptr, nullptr);
        how = "controller stopped";
    }
    // Normally the stop ended the path; if not, end it by hand.
    if (at<uint8_t*>(entity, kEntityPath) == path) {
        g_fireComplete(path, objectId);
        g_releaseBlocker(path);
        g_clearRoute(object);
        how = controller ? "controller stopped, path cleared" : "path cleared";
    }
    return how;
}

void readController(uint64_t objectId) {
    uint8_t* entity = routingEntity(objectId, nullptr);
    auto* body = entity ? at<uint8_t*>(entity, kEntityBody) : nullptr;
    auto* controller = body ? at<uint8_t*>(body, kBodyController) : nullptr;
    if (!g_walkLogicVt) {
        g_walkLogicVt = reinterpret_cast<uintptr_t>(addrs::global("WalkLogicVTable"));
        g_masterVt = reinterpret_cast<uintptr_t>(addrs::global("MasterVTable"));
        g_proxyVt = reinterpret_cast<uintptr_t>(addrs::global("ProxyVTable"));
    }
    bool walk = false;
    for (int depth = 0; controller && depth < 4; ++depth) {
        uintptr_t vt = at<uintptr_t>(controller, 0);
        if (vt == g_walkLogicVt) {
            walk = true;
            break;
        }
        if (vt != g_masterVt && vt != g_proxyVt) break;
        controller = at<uint8_t*>(controller, kActiveLogic);
    }
    if (!walk) {
        if (g_lastController && g_ctlLogs < 300) {
            ++g_ctlLogs;
            LOG("Walk clips: the walk controller is gone (last state %d)", g_lastState);
        }
        g_ctlState = -1;
        g_lastController = nullptr;
        g_lastState = -1;
        return;
    }
    int state = at<int32_t>(controller, kCtlState);
    float remaining = at<float>(controller, kCtlRemaining), stop = at<float>(controller, kCtlStopDistance),
          start = at<float>(controller, kCtlStartDistance);
    g_ctlState = state;
    g_ctlRemaining = remaining;
    if (stop > 0.05f && stop < 2) g_ctlStop = stop;
    if (start > 0.05f && start < 3) g_ctlStart = start;
    if (stop > 0.05f && stop < 2 && std::fabs(stop - g_loggedStop) > 0.01f && g_ctlLogs < 300) {
        ++g_ctlLogs;
        g_loggedStop = stop;
        LOG("Walk clips: your walk's stop clip goes %.2f m, its start clip %.2f m (at least %.2f m a first step, "
            "within %.0f deg; stop checked every frame %d)",
            stop, start, at<float>(controller, kCtlStartMin), at<float>(controller, kCtlStartAngle) * 57.29578f,
            static_cast<int>(controller[kCtlStopEveryFrame]));
    }
    if ((controller != g_lastController || state != g_lastState) && g_ctlLogs < 300) {
        ++g_ctlLogs;
        LOG("Walk clips: controller %p state %d -> %d, %.2f m of path ahead", controller, g_lastState, state, remaining);
    }
    g_lastController = controller;
    g_lastState = state;
}

}  // namespace

void setClips(bool on) {
    if (g_clips.exchange(on) != on) LOG("Walk clips (the game's start and stop walk animations) %s", on ? "on" : "off");
    if (!on) player::setClipStop(0);
}

bool info(uint64_t objectId, float out[4]) {
    g_watchObject = objectId;
    if (g_watchRead.load() != objectId) return false;
    out[0] = static_cast<float>(g_ctlState.load());
    out[1] = g_ctlRemaining.load();
    out[2] = g_ctlStop.load();
    out[3] = g_ctlStart.load();
    return g_ctlState.load() >= 0 || out[2] > 0;
}

void request(uint64_t objectId, uint64_t routeId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() < 16) g_pending.push_back({objectId, routeId, GetTickCount64()});
}

// A stop can come before the client has started the route (it waits behind
// the Sim's earlier ops): it is tried again each frame until the route shows
// up, for a second at most.
uint64_t clientRoute(uint64_t objectId) {
    g_watchObject = objectId;
    return g_watchRead.load() == objectId ? g_watchRoute.load() : 0;
}

void pump() {
    uint64_t watched = g_watchObject.load();
    if (watched && ready()) {
        g_watchRoute = activeRoute(watched);
        readController(watched);
        g_watchRead = watched;
        player::setClipStop(g_clips.load() ? g_ctlStop.load() : 0);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending.empty() && g_waiting.empty()) return;
        g_waiting.insert(g_waiting.end(), g_pending.begin(), g_pending.end());
        g_pending.clear();
    }
    if (!ready()) {
        g_waiting.clear();
        return;
    }
    ULONGLONG now = GetTickCount64();
    std::vector<Request> still;
    for (const Request& r : g_waiting) {
        const char* result = stopRoute(r.object, r.route);
        ULONGLONG waited = now - r.since;
        if ((result == kNoPath || result == kOtherRoute) && r.route && waited < kWaitForRoute) {
            still.push_back(r);
            continue;
        }
        if (g_logged < 200) {
            ++g_logged;
            if (result == kOtherRoute)
                LOG("Walk stop: route %016llx: not on the client (%s: %016llx)",
                    static_cast<unsigned long long>(r.route), result, static_cast<unsigned long long>(g_otherRoute));
            else if (result == kNoPath)
                LOG("Walk stop: route %016llx: not on the client (%s)", static_cast<unsigned long long>(r.route),
                    result);
            else if (!r.route)
                LOG("Walk stop: the client's route on %016llx: %s", static_cast<unsigned long long>(r.object), result);
            else
                LOG("Walk stop: route %016llx: %s%s", static_cast<unsigned long long>(r.route), result,
                    waited > 50 ? " (after waiting for it to start)" : "");
        }
    }
    g_waiting.swap(still);
}

}  // namespace s4vr::walkstop
