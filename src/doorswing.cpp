#include "doorswing.h"

#include <windows.h>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::doorswing {

namespace {

// Game functions (rva), checked against their first bytes. The lookup chain
// is the one the PortalEnter route event runs (rva 0x212bf0; game
// 1.121.361.1020, 0x396a30 in 1.112).
struct Known {
    uint32_t rva;
    uint8_t bytes[16];
};
const Known kFindObject{0x162030, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0xda}};
const Known kComponent{0x9b0430, {0x8b, 0x41, 0x10, 0x4c, 0x8b, 0xc1, 0x85, 0xc2, 0x74, 0x3e, 0xff, 0xca, 0x23, 0xd0, 0x8b, 0xc2}};
const Known kPortalOf{0x2147a0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x99, 0xb0, 0x00, 0x00}};
const Known kEnter{0x9cc020, {0xff, 0x41, 0x38, 0x88, 0x91, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x49, 0x18, 0xe9, 0x6e, 0xec}};
const Known kExit{0x9cc040, {0x8b, 0x51, 0x38, 0x33, 0xc0, 0xff, 0xca, 0x85, 0xd2, 0x0f, 0x4f, 0xc2, 0x89, 0x41, 0x38, 0x48}};
constexpr uint32_t kComponentMaskGlobal = 0x2956160;  // the component kind the event looks up
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
std::vector<std::pair<uint64_t, bool>> g_pending;
int g_logged = 0;

bool ready() {
    if (g_ready) return g_ready > 0;
    for (const Known* k : {&kFindObject, &kComponent, &kPortalOf, &kEnter, &kExit}) {
        if (!gameaddr::function(k->rva, k->bytes)) {
            LOG("Doors: game build not recognised (no match for rva 0x%x); doors won't swing", k->rva);
            g_ready = -1;
            return false;
        }
    }
    g_maskGlobal = gameaddr::global(kComponentMaskGlobal);
    if (!g_maskGlobal) {
        g_ready = -1;
        return false;
    }
    g_findObject = reinterpret_cast<FindObjectFn>(gameaddr::function(kFindObject.rva, kFindObject.bytes));
    g_component = reinterpret_cast<ComponentFn>(gameaddr::function(kComponent.rva, kComponent.bytes));
    g_portalOf = reinterpret_cast<PortalOfFn>(gameaddr::function(kPortalOf.rva, kPortalOf.bytes));
    g_enter = reinterpret_cast<EnterFn>(gameaddr::function(kEnter.rva, kEnter.bytes));
    g_exit = reinterpret_cast<ExitFn>(gameaddr::function(kExit.rva, kExit.bytes));
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

void request(uint64_t objectId, bool open) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() < 64) g_pending.emplace_back(objectId, open);
}

void pump() {
    std::vector<std::pair<uint64_t, bool>> work;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending.empty()) return;
        work.swap(g_pending);
    }
    if (!ready()) return;
    for (const auto& [id, open] : work) {
        void* portal = portalOf(id);
        if (portal) {
            // Game routes always send "entering front"; the door picks its swing.
            if (open)
                g_enter(portal, 1);
            else
                g_exit(portal);
        }
        if (g_logged < 20) {
            ++g_logged;
            LOG("Doors: %s %016llx%s", open ? "open" : "close", static_cast<unsigned long long>(id),
                portal ? "" : " (no swinging door there)");
        }
    }
}

}  // namespace s4vr::doorswing
