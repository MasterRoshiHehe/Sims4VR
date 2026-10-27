#include "doorswing.h"

#include <windows.h>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "addrs.h"
#include "gameaddr.h"
#include "log.h"

namespace s4vr::doorswing {

namespace {

// Game functions (rva), checked against their first bytes. The lookup chain
// is the one the PortalEnter route event runs (rva 0x212bf0; game
constexpr size_t kSceneObject = 0x68;                // that component's scene object (getter 0x4300a0, unchanged)

struct ObjectHandle {
    uint32_t kind = 1;
    uint32_t pad = 0;
    uint64_t id = 0;
};
using FindObjectFn = bool (*)(const ObjectHandle* handle, void** object);
using ComponentFn = uint8_t* (*)(void* object, uint32_t mask);
using PortalOfFn = void* (*)(void* sceneObject);
using EnterFn = void (*)(void* portal, uint8_t enteringFront);
using ExitFn = void (*)(void* portal);

FindObjectFn g_findObject = nullptr;
ComponentFn g_component = nullptr;
PortalOfFn g_portalOf = nullptr;
EnterFn g_enter = nullptr;
ExitFn g_exit = nullptr;
const uint8_t* g_maskGlobal = nullptr;
int g_ready = 0;  // 0 not checked, 1 ready, -1 unsupported build
std::mutex g_mutex;
std::vector<std::pair<uint64_t, int>> g_pending;
int g_logged = 0;
constexpr int kWatchMax = 8;
uint64_t g_watchIds[kWatchMax] = {};
int g_watchCount = 0;
bool g_watchOpen[kWatchMax] = {};

bool ready() {
    if (g_ready) return g_ready > 0;
    for (const char* k : {"FindObject", "Component", "PortalOf", "Enter", "Exit"}) {
        if (!addrs::fn(k)) {
            LOG("Doors: game build not recognised (no match for %s); doors won't swing", k);
            g_ready = -1;
            return false;
        }
    }
    g_maskGlobal = addrs::global("ComponentMaskGlobal");
    if (!g_maskGlobal) {
        g_ready = -1;
        return false;
    }
    g_findObject = reinterpret_cast<FindObjectFn>(addrs::fn("FindObject"));
    g_component = reinterpret_cast<ComponentFn>(addrs::fn("Component"));
    g_portalOf = reinterpret_cast<PortalOfFn>(addrs::fn("PortalOf"));
    g_enter = reinterpret_cast<EnterFn>(addrs::fn("Enter"));
    g_exit = reinterpret_cast<ExitFn>(addrs::fn("Exit"));
    g_ready = 1;
    return true;
}

// The door's swing counter, or null if the object has none.
void* portalOf(uint64_t id) {
    ObjectHandle handle;
    handle.id = id;
    void* object = nullptr;
    if (!g_findObject(&handle, &object) || !object) return nullptr;
    uint32_t mask;
    std::memcpy(&mask, g_maskGlobal, sizeof(mask));
    uint8_t* component = g_component(object, mask);
    if (!component) return nullptr;
    void* scene = nullptr;
    std::memcpy(&scene, component + kSceneObject, sizeof(scene));
    if (!scene) return nullptr;
    return g_portalOf(scene);
}

}  // namespace

void watch(const uint64_t* ids, int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    count = count < 0 ? 0 : (count > kWatchMax ? kWatchMax : count);
    bool open[kWatchMax] = {};
    for (int i = 0; i < count; ++i)  // keep what is known for a door still on the list
        for (int j = 0; j < g_watchCount; ++j)
            if (g_watchIds[j] == ids[i]) open[i] = g_watchOpen[j];
    for (int i = 0; i < count; ++i) {
        g_watchIds[i] = ids[i];
        g_watchOpen[i] = open[i];
    }
    g_watchCount = count;
}

bool isOpen(uint64_t objectId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < g_watchCount; ++i)
        if (g_watchIds[i] == objectId) return g_watchOpen[i];
    return false;
}

void request(uint64_t objectId, int mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() < 64) g_pending.emplace_back(objectId, mode);
}

void pump() {
    std::vector<std::pair<uint64_t, int>> work;
    uint64_t watched[kWatchMax];
    int watchCount;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        work.swap(g_pending);
        watchCount = g_watchCount;
        std::memcpy(watched, g_watchIds, sizeof(watched));
    }
    if (!ready()) return;
    if (watchCount > 0) {
        bool open[kWatchMax] = {};
        for (int i = 0; i < watchCount; ++i) {
            void* portal = portalOf(watched[i]);
            int32_t count = 0;
            if (portal) std::memcpy(&count, static_cast<uint8_t*>(portal) + 0x38, sizeof(count));
            open[i] = count > 0;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < watchCount && i < g_watchCount; ++i)
            if (g_watchIds[i] == watched[i]) g_watchOpen[i] = open[i];
    }
    for (const auto& [id, mode] : work) {
        const bool open = mode != 0;
        void* portal = portalOf(id);
        if (portal) {
            // The flag picks the swing (DoorOpenForward / DoorOpenBackward): the side we come from.
            if (open)
                g_enter(portal, mode == 2 ? 0 : 1);
            else
                g_exit(portal);
        }
        if (g_logged < 20) {
            ++g_logged;
            LOG("Doors: %s %016llx%s", open ? (mode == 2 ? "open (back)" : "open (front)") : "close", static_cast<unsigned long long>(id),
                portal ? "" : " (no swinging door there)");
        }
    }
}

}  // namespace s4vr::doorswing
