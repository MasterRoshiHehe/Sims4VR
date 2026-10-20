#include "uihook.h"

#include <windows.h>
#include <d3d11_1.h>
#include <algorithm>
#include <atomic>
#include <mutex>

#include "MinHook.h"
#include "bubbles.h"
#include "camera.h"
#include "facehide.h"
#include "framemap.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"
#include "nearplane.h"
#include "nodim.h"
#include "phone3d.h"

namespace s4vr::uihook {

namespace {

// RenderManager::Render(rm, movie or null): every UI movie, once a frame.
// Scaleform::Render::Sims4::HAL::applyBlendModeImpl(hal, mode, sourceAc, forceAc).
// The render threading manager (its first byte: threading on), and the game's
// D3D11 wrapper ("gfx") by index: Render draws with gfx 9 (a job's) with
// threading on, gfx 0 (the main one) otherwise. Since 1.121 every gfx,
// gfx 0 too, records into a deferred context; the game's submission thread
// executes the command lists, then presents (the Present hook runs there).

// RenderManager +0x18 its HAL; HAL +0xc180 the gfx the HAL draws with (set
// when the UI starts; the getters above give the same one).
constexpr size_t kRmHal = 0x18, kHalGfx = 0xc180;
// gfx +0xd0 main colour target object (its texture at +0x30: the pointer
// changes on a device reset, the object doesn't), +0x1d8 its (deferred)
// context, +0x250 its parent (set on the job ones). 1.121 grew the gfx: these
// were +0xc8, +0x1b8, +0x1c0 (deferred gfx setup 0x1257860, Present job 0x1252c90).
constexpr size_t kGfxMainTarget = 0xd0, kTargetTexture = 0x30, kGfxContext = 0x1d8, kGfxParent = 0x250;
constexpr size_t kSetTargetsSlot = 33;  // ID3D11DeviceContext::OMSetRenderTargets
constexpr size_t kClearDepthSlot = 53;  // ID3D11DeviceContext::ClearDepthStencilView

using RenderFn = void (*)(void* rm, void* movie);
using ThreadingFn = const char* (*)();
using GfxByIndexFn = uint8_t* (*)(int index);
using BlendFn = void (*)(void* hal, int mode, bool sourceAc, bool forceAc);
using SetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*);
using ClearDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

RenderFn g_render = nullptr;
ThreadingFn g_threading = nullptr;
GfxByIndexFn g_gfxByIndex = nullptr;
BlendFn g_blend = nullptr;
// OMSetRenderTargets of immediate and deferred contexts (d3d11.dll may use two implementations).
void* g_setTargetsCode[2] = {};
SetTargetsFn g_setTargets[2] = {};
std::atomic<bool> g_contextHooked{false};
void* g_clearDepthCode[2] = {};
ClearDepthFn g_clearDepth[2] = {};

// The 3D scene's targets, for drawing the phone into it (phone3d): the view
// the game binds its main colour target with, and the depth-stencil bound
// alongside it outside the UI. Learned from the game's own binds; one
// reference each, swapped under g_sceneMutex.
std::atomic<ID3D11Resource*> g_mainResource{nullptr};  // not owned: the main colour texture
std::atomic<ID3D11RenderTargetView*> g_sceneTarget{nullptr};
std::atomic<ID3D11DepthStencilView*> g_sceneDepth{nullptr};
std::atomic<int> g_depthClear{-1};  // the scene depth's clear value: 0 (reversed), 1 (normal), -1 not seen
std::mutex g_sceneMutex;
bool g_installed = false;

// The UI texture, made on the UI's thread, read on the Present thread.
std::mutex g_mutex;
ID3D11Texture2D* g_texture = nullptr;
ID3D11RenderTargetView* g_view = nullptr;
D3D11_TEXTURE2D_DESC g_desc{};
ID3D11Device* g_textureDevice = nullptr;  // not owned: what g_texture was made on

std::atomic<bool> g_enabled{false};
std::atomic<int> g_rendering{0};  // UI renders in progress (any thread): binds elsewhere skip the thread-local check
std::atomic<bool> g_clearPending{true};
std::atomic<bool> g_phonePending{true};  // the phone goes into the 3D view once a frame
std::atomic<bool> g_secondPass{false};    // beginSecondPass .. endSecondPass
std::atomic<uint32_t> g_uiRenders{0}, g_uiSecondRenders{0};
std::atomic<bool> g_drawn{false};
std::atomic<uint64_t> g_renders{0};

// The UI's thread, inside Render only.
thread_local ID3D11DeviceContext* t_context = nullptr;
thread_local ID3D11Resource* t_main = nullptr;
thread_local ID3D11RenderTargetView* t_ui = nullptr;
thread_local ID3D11RenderTargetView* t_mainView = nullptr;  // the game's view of its main target (one reference)
thread_local bool t_inRender = false;

// Speech bubbles and the like (bubbles.h): the cards of the UI drawn last, from
// the UI's thread to the Present thread.
std::mutex g_cardMutex;
bubbles::Card g_cards[bubbles::kMaxCards];
int g_cardCount = 0;

// Present thread only.
ID3D11Texture2D* g_resolved = nullptr;
ID3D11RenderTargetView* g_resolvedView = nullptr;  // to cut the bubbles out of it
D3D11_TEXTURE2D_DESC g_resolvedDesc{};
ID3D11Texture2D* g_raw = nullptr;  // this frame's UI as drawn, before nodim takes the dimming out
ID3D11Device* g_resolvedDevice = nullptr;  // not owned
int g_framesSinceDraw = 1000;
// The bubbles cut out of the UI, side by side (single sample, like g_resolved).
ID3D11Texture2D* g_atlas = nullptr;
D3D11_TEXTURE2D_DESC g_atlasDesc{};
constexpr UINT kAtlasSize = 2048;
bool g_bubblesShown = false;

SetTargetsFn originalFor(ID3D11DeviceContext* context) {
    void* code = (*reinterpret_cast<void***>(context))[kSetTargetsSlot];
    for (int i = 0; i < 2; ++i)
        if (g_setTargetsCode[i] == code && g_setTargets[i]) return g_setTargets[i];
    return nullptr;
}

// Views for a bind on the UI's context: the main target becomes ours.
bool swapIn(UINT count, ID3D11RenderTargetView* const* views, ID3D11RenderTargetView** out) {
    bool any = false;
    for (UINT i = 0; i < count && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        out[i] = views[i];
        if (!views[i]) continue;
        ID3D11Resource* resource = nullptr;
        views[i]->GetResource(&resource);
        if (resource && resource == t_main) {
            if (!t_mainView) {
                t_mainView = views[i];
                t_mainView->AddRef();
            }
            out[i] = t_ui;
            any = true;
        }
        if (resource) resource->Release();
    }
    return any;
}

// A bind of the main colour target with a depth-stencil, outside the UI: the 3D scene's.
void learnScene(ID3D11RenderTargetView* view, ID3D11DepthStencilView* depth) {
    if (view == g_sceneTarget.load(std::memory_order_relaxed) && depth == g_sceneDepth.load(std::memory_order_relaxed))
        return;
    ID3D11Resource* main = g_mainResource.load(std::memory_order_relaxed);
    if (!main) return;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    bool isMain = resource == main;
    if (resource) resource->Release();
    if (!isMain) return;
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    ID3D11RenderTargetView* oldView = g_sceneTarget.exchange(view);
    ID3D11DepthStencilView* oldDepth = g_sceneDepth.exchange(depth);
    view->AddRef();
    depth->AddRef();
    if (oldView) oldView->Release();
    if (oldDepth) oldDepth->Release();
    static int logged = 0;
    if (logged++ < 5) LOG("Phone 3D: the scene's targets are view %p, depth %p", static_cast<void*>(view), static_cast<void*>(depth));
}

// Whether `depth` is (a view of) the 3D scene's depth-stencil: the passes that
// draw what your eyes see (face hiding swaps your Sim's face out of those).
bool sceneDepthBound(ID3D11DepthStencilView* depth) {
    if (!depth) return false;
    ID3D11DepthStencilView* scene = g_sceneDepth.load(std::memory_order_relaxed);
    if (!scene) return false;
    if (depth == scene) return true;
    thread_local ID3D11DepthStencilView* t_view = nullptr;
    thread_local ID3D11DepthStencilView* t_scene = nullptr;
    thread_local bool t_same = false;
    if (depth == t_view && scene == t_scene) return t_same;
    ID3D11Resource* a = nullptr;
    ID3D11Resource* b = nullptr;
    depth->GetResource(&a);
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        scene = g_sceneDepth.load();
        if (scene) scene->GetResource(&b);
    }
    t_view = depth;
    t_scene = scene;
    t_same = a && a == b;
    if (a) a->Release();
    if (b) b->Release();
    return t_same;
}

void setTargets(SetTargetsFn original, ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views,
                ID3D11DepthStencilView* depth) {
    facehide::targets(context, sceneDepthBound(depth));
    if (!t_inRender && depth && views && count && views[0]) learnScene(views[0], depth);
    if (!depth)
        framemap::mark(framemap::ColourBind, static_cast<int>(count));
    else if (!views || !count || !views[0])
        framemap::mark(framemap::DepthOnlyBind, static_cast<int>(count));
    else if (views[0] == g_sceneTarget.load(std::memory_order_relaxed))
        framemap::mark(framemap::SceneBind, static_cast<int>(count));
    else
        framemap::mark(framemap::OtherBind, static_cast<int>(count));
    if (g_rendering.load(std::memory_order_relaxed) && context == t_context && t_ui && views && count) {
        ID3D11RenderTargetView* swapped[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        if (count <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && swapIn(count, views, swapped)) {
            original(context, count, swapped, depth);  // the game's depth-stencil stays
            return;
        }
    }
    original(context, count, views, depth);
}

void STDMETHODCALLTYPE hookedSetTargets0(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v,
                                         ID3D11DepthStencilView* d) {
    setTargets(g_setTargets[0], c, n, v, d);
}
void STDMETHODCALLTYPE hookedSetTargets1(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v,
                                         ID3D11DepthStencilView* d) {
    setTargets(g_setTargets[1], c, n, v, d);
}

// The UI cleared the scene's depth while drawing (it uses the depth-stencil for its masks): then
// the depth at Present is no longer the 3D view's (sceneDepthAtPresent).
std::atomic<bool> g_uiClearsDepth{false};

void clearDepth(ClearDepthFn original, ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT flags, FLOAT value,
                UINT8 stencil) {
    if (d && (flags & D3D11_CLEAR_DEPTH) && d == g_sceneDepth.load(std::memory_order_relaxed)) {
        if (t_inRender) {
            if (!g_uiClearsDepth.exchange(true))
                LOG("UI layer: the UI clears the scene depth; the depth eye uses the copy taken before the UI");
        } else {
            int seen = value < 0.5f ? 0 : 1;
            if (g_depthClear.exchange(seen) != seen) LOG("Phone 3D: the scene's depth clears to %.2f", value);
        }
    }
    original(c, d, flags, value, stencil);
}
void STDMETHODCALLTYPE hookedClearDepth0(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT v, UINT8 s) {
    clearDepth(g_clearDepth[0], c, d, f, v, s);
}
void STDMETHODCALLTYPE hookedClearDepth1(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT v, UINT8 s) {
    clearDepth(g_clearDepth[1], c, d, f, v, s);
}

// First person's near limit: how close the nearest thing in the finished 3D
// view is (nearplane.h), before anything of ours goes into it.
void senseNear(ID3D11DeviceContext* context) {
    ID3D11DepthStencilView* depth = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        depth = g_sceneDepth.load();
        if (!depth) return;
        depth->AddRef();
    }
    nearplane::sense(context, depth, g_depthClear.load() == 0);
    depth->Release();
}

// The depth eye's copy of the scene depth, our own pieces in it.
void senseLate(ID3D11DeviceContext* context) {
    ID3D11DepthStencilView* depth = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        depth = g_sceneDepth.load();
        if (!depth) return;
        depth->AddRef();
    }
    nearplane::senseLate(context, depth);
    depth->Release();
}

// Our phone into the finished 3D view, before the UI goes on top (or elsewhere).
void drawPhone(ID3D11DeviceContext* context) {
    ID3D11RenderTargetView* view = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        view = g_sceneTarget.load();
        depth = g_sceneDepth.load();
        if (!view || !depth) return;
        view->AddRef();
        depth->AddRef();
    }
    phone3d::draw(context, view, depth, g_depthClear.load() == 0);
    view->Release();
    depth->Release();
}

// Rebinds what is bound now on `context`, with the main target swapped for
// ours (toUi) or back.
void rebind(ID3D11DeviceContext* context, bool toUi) {
    SetTargetsFn original = originalFor(context);
    if (!original) return;
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, &depth);
    UINT count = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (bound[i]) count = i + 1;
    ID3D11RenderTargetView* swapped[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    bool any = false;
    if (toUi) {
        any = count && swapIn(count, bound, swapped);
    } else if (t_mainView) {
        for (UINT i = 0; i < count; ++i) {
            swapped[i] = bound[i] == t_ui ? t_mainView : bound[i];
            any = any || bound[i] == t_ui;
        }
    }
    if (any) original(context, count, swapped, depth);
    for (auto* view : bound)
        if (view) view->Release();
    if (depth) depth->Release();
}

// Our view for a main target like this one (same size, format and MSAA),
// with a reference for the caller.
ID3D11RenderTargetView* uiViewFor(ID3D11Texture2D* main) {
    D3D11_TEXTURE2D_DESC desc;
    main->GetDesc(&desc);
    ID3D11Device* mainDevice = nullptr;
    main->GetDevice(&mainDevice);
    if (mainDevice) mainDevice->Release();  // only compared
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_view || mainDevice != g_textureDevice || desc.Width != g_desc.Width || desc.Height != g_desc.Height || desc.Format != g_desc.Format ||
        desc.SampleDesc.Count != g_desc.SampleDesc.Count || desc.SampleDesc.Quality != g_desc.SampleDesc.Quality) {
        if (g_view) g_view->Release();
        if (g_texture) g_texture->Release();
        g_view = nullptr;
        g_texture = nullptr;
        ID3D11Device* device = nullptr;
        main->GetDevice(&device);
        D3D11_TEXTURE2D_DESC ui = desc;
        ui.MipLevels = 1;
        ui.ArraySize = 1;
        ui.Usage = D3D11_USAGE_DEFAULT;
        ui.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ui.CPUAccessFlags = 0;
        ui.MiscFlags = 0;
        bool ok = device && SUCCEEDED(device->CreateTexture2D(&ui, nullptr, &g_texture)) &&
                  SUCCEEDED(device->CreateRenderTargetView(g_texture, nullptr, &g_view));
        if (device) device->Release();
        if (!ok) {
            if (g_texture) g_texture->Release();
            g_texture = nullptr;
            g_view = nullptr;
            static int failed = 0;
            if (failed++ < 3)
                LOG("UI layer: could not make the UI texture (%ux%u format %d, %u samples)", desc.Width, desc.Height,
                    static_cast<int>(desc.Format), desc.SampleDesc.Count);
            return nullptr;
        }
        g_desc = desc;
        g_textureDevice = mainDevice;
        LOG("UI layer: UI texture %ux%u format %d, %u samples", desc.Width, desc.Height, static_cast<int>(desc.Format),
            desc.SampleDesc.Count);
    }
    g_view->AddRef();
    return g_view;
}

void hookedRender(void* rm, void* movie) {
    framemap::mark(framemap::UiRender);
    if (!g_enabled.load(std::memory_order_acquire) || t_inRender || !g_contextHooked.load() || !rm) {
        g_render(rm, movie);
        return;
    }
    // The gfx the HAL draws with; else the one Render picks.
    uint8_t* hal = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(rm) + kRmHal);
    uint8_t* gfx = hal ? *reinterpret_cast<uint8_t**>(hal + kHalGfx) : nullptr;
    if (!gfx) {
        const char* threading = g_threading();
        gfx = g_gfxByIndex(threading && *threading ? 9 : 0);
    }
    uint8_t* target = gfx ? *reinterpret_cast<uint8_t**>(gfx + kGfxMainTarget) : nullptr;
    auto* context = gfx ? *reinterpret_cast<ID3D11DeviceContext**>(gfx + kGfxContext) : nullptr;
    auto* main = target ? *reinterpret_cast<ID3D11Texture2D**>(target + kTargetTexture) : nullptr;
    ID3D11RenderTargetView* ui = main && context && originalFor(context) ? uiViewFor(main) : nullptr;
    if (!ui) {
        static int skipped = 0;
        if (skipped++ < 3)
            LOG("UI layer: UI left in the 3D view (gfx %p, context %p, main target %p%s)", static_cast<void*>(gfx), static_cast<void*>(context), static_cast<void*>(main),
                context && !originalFor(context) ? ", context not hooked" : "");
        g_render(rm, movie);
        return;
    }
    // Cleared once a frame (a loading screen may draw twice), in the UI's own
    // command list so the clear lands in order.
    if (g_clearPending.exchange(false)) {
        const float transparent[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(ui, transparent);
    }
    g_mainResource.store(main, std::memory_order_relaxed);
    g_uiRenders.fetch_add(1, std::memory_order_relaxed);
    if (g_phonePending.exchange(false)) {
        if (!nearplane::skipPass(g_secondPass.load(std::memory_order_acquire))) senseNear(context);
        drawPhone(context);
        if (phone3d::writesDepth()) senseLate(context);
    }
    if (g_secondPass.load(std::memory_order_acquire)) {
        // The right eye's pass: our pieces only; the UI texture has this frame's UI already.
        g_uiSecondRenders.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_rendering.fetch_add(1, std::memory_order_relaxed);
    t_inRender = true;
    t_context = context;
    t_main = main;
    t_ui = ui;
    rebind(context, true);  // drawing may start into whatever is bound already
    g_render(rm, movie);
    rebind(context, false);  // the game believes its main target is bound
    // The bubbles this UI holds, laid in the world by the camera it was drawn for.
    {
        bubbles::Card cards[bubbles::kMaxCards];
        int count = 0;
        camera::View view;
        if (bubbles::wanted() && camera::lastView(view)) {
            D3D11_TEXTURE2D_DESC md;
            main->GetDesc(&md);
            count = bubbles::frameCards(view, static_cast<int>(md.Width), static_cast<int>(md.Height), cards,
                                        bubbles::kMaxCards);
        }
        std::lock_guard<std::mutex> lock(g_cardMutex);
        std::copy(cards, cards + count, g_cards);
        g_cardCount = count;
    }
    t_context = nullptr;
    t_main = nullptr;
    t_ui = nullptr;
    if (t_mainView) t_mainView->Release();
    t_mainView = nullptr;
    t_inRender = false;
    g_rendering.fetch_sub(1, std::memory_order_relaxed);
    ui->Release();
    g_drawn.store(true, std::memory_order_release);
    if (g_renders.fetch_add(1) == 0)
        LOG("UI layer: first UI frame drawn off the 3D view (%s gfx)",
            *reinterpret_cast<uint8_t**>(gfx + kGfxParent) ? "job" : "main");
}

void hookedBlend(void* hal, int mode, bool sourceAc, bool forceAc) {
    // Separate alpha (A = As + Ad(1 - As)): correct coverage over a clear target.
    g_blend(hal, mode, sourceAc, forceAc || t_inRender);
}

void hookContexts(ID3D11Device* device) {
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    ID3D11DeviceContext* deferred = nullptr;
    device->CreateDeferredContext(0, &deferred);
    void* code[2] = {(*reinterpret_cast<void***>(immediate))[kSetTargetsSlot],
                     deferred ? (*reinterpret_cast<void***>(deferred))[kSetTargetsSlot] : nullptr};
    void* clearCode[2] = {(*reinterpret_cast<void***>(immediate))[kClearDepthSlot],
                          deferred ? (*reinterpret_cast<void***>(deferred))[kClearDepthSlot] : nullptr};
    if (clearCode[1] == clearCode[0]) clearCode[1] = nullptr;
    void* clearHooks[2] = {reinterpret_cast<void*>(&hookedClearDepth0), reinterpret_cast<void*>(&hookedClearDepth1)};
    for (int i = 0; i < 2; ++i) {
        if (!clearCode[i]) continue;
        MH_STATUS status = MH_CreateHook(clearCode[i], clearHooks[i], reinterpret_cast<void**>(&g_clearDepth[i]));
        if (status == MH_OK) status = MH_EnableHook(clearCode[i]);
        if (status == MH_OK)
            g_clearDepthCode[i] = clearCode[i];
        else
            LOG("Phone 3D: ClearDepthStencilView hook %d failed (%d)", i, static_cast<int>(status));
    }
    if (deferred) deferred->Release();
    immediate->Release();
    if (code[1] == code[0]) code[1] = nullptr;
    void* hooks[2] = {reinterpret_cast<void*>(&hookedSetTargets0), reinterpret_cast<void*>(&hookedSetTargets1)};
    int hooked = 0;
    for (int i = 0; i < 2; ++i) {
        if (!code[i]) continue;
        MH_STATUS status = MH_CreateHook(code[i], hooks[i], reinterpret_cast<void**>(&g_setTargets[i]));
        if (status == MH_OK) status = MH_EnableHook(code[i]);
        if (status == MH_OK) {
            g_setTargetsCode[i] = code[i];
            ++hooked;
        } else {
            LOG("UI layer: OMSetRenderTargets hook %d failed (%d)", i, static_cast<int>(status));
        }
    }
    LOG("UI layer: OMSetRenderTargets hooked (%d implementation%s)", hooked, hooked == 1 ? "" : "s");
    g_contextHooked = hooked > 0;
    facehide::hookContexts(device);
}

// Present thread, the UI just resolved into g_resolved: its bubbles move to the
// atlas, out of the flat UI, and go into the world as cards (phone3d).
void cutBubbles(ID3D11Device* device, ID3D11DeviceContext* context) {
    bubbles::Card cards[bubbles::kMaxCards];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_cardMutex);
        count = g_cardCount;
        std::copy(g_cards, g_cards + count, cards);
        g_cardCount = 0;
    }
    if (!count || !g_resolvedView) {
        if (g_bubblesShown) phone3d::setBubbles(nullptr, 0, nullptr);
        g_bubblesShown = false;
        return;
    }
    if (!g_atlas || g_atlasDesc.Format != g_resolvedDesc.Format) {
        if (g_atlas) g_atlas->Release();
        g_atlas = nullptr;
        D3D11_TEXTURE2D_DESC a{};
        a.Width = a.Height = kAtlasSize;
        a.MipLevels = a.ArraySize = 1;
        a.Format = g_resolvedDesc.Format;
        a.SampleDesc.Count = 1;
        a.Usage = D3D11_USAGE_DEFAULT;
        a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&a, nullptr, &g_atlas))) {
            g_atlas = nullptr;
            static int failed = 0;
            if (failed++ < 3) LOG("Bubbles: could not make the atlas (format %d)", static_cast<int>(a.Format));
            return;
        }
        g_atlasDesc = a;
    }
    // Shelves, left to right, top to bottom; what doesn't fit stays on the flat UI.
    phone3d::Panel panels[phone3d::kMaxBubbles];
    D3D11_RECT cut[phone3d::kMaxBubbles];
    int placed = 0;
    UINT x = 0, y = 0, shelf = 0;
    for (int i = 0; i < count && placed < phone3d::kMaxBubbles; ++i) {
        const bubbles::Card& c = cards[i];
        UINT w = static_cast<UINT>(c.rect[2] - c.rect[0]), h = static_cast<UINT>(c.rect[3] - c.rect[1]);
        if (c.rect[2] > static_cast<int>(g_resolvedDesc.Width) || c.rect[3] > static_cast<int>(g_resolvedDesc.Height) ||
            w > kAtlasSize || h > kAtlasSize)
            continue;
        if (x + w > kAtlasSize) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + h > kAtlasSize) break;
        D3D11_BOX box{static_cast<UINT>(c.rect[0]), static_cast<UINT>(c.rect[1]), 0, static_cast<UINT>(c.rect[2]),
                      static_cast<UINT>(c.rect[3]), 1};
        context->CopySubresourceRegion(g_atlas, 0, x, y, 0, g_resolved, 0, &box);
        phone3d::Panel& p = panels[placed];
        p = phone3d::Panel{};
        p.show = true;
        p.clear = true;
        p.opacity = 1;
        std::copy(c.centre, c.centre + 3, p.centre);
        std::copy(c.x, c.x + 3, p.x);
        std::copy(c.y, c.y + 3, p.y);
        p.width = c.width;
        p.height = c.height;
        p.crop[0] = static_cast<float>(x) / kAtlasSize;
        p.crop[1] = static_cast<float>(y) / kAtlasSize;
        p.crop[2] = static_cast<float>(x + w) / kAtlasSize;
        p.crop[3] = static_cast<float>(y + h) / kAtlasSize;
        cut[placed] = D3D11_RECT{c.rect[0], c.rect[1], c.rect[2], c.rect[3]};
        ++placed;
        x += w;
        shelf = std::max(shelf, h);
    }
    if (!placed) return;
    ID3D11DeviceContext1* context1 = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        context1) {
        const float transparent[4] = {0, 0, 0, 0};
        context1->ClearView(g_resolvedView, transparent, cut, static_cast<UINT>(placed));
        context1->Release();
    } else {
        static int failed = 0;
        if (failed++ < 3) LOG("Bubbles: no D3D11.1 context; they show in the world and on the flat UI");
    }
    phone3d::setBubbles(panels, placed, g_atlas);
    g_bubblesShown = true;
}

}  // namespace

bool install() {
    if (gameaddr::dx9()) {
        LOG("UI layer: not available in the DX9 game yet; the UI stays in the 3D view");
        return false;
    }
    void* render = addrs::fn("Render");
    void* blend = addrs::fn("ApplyBlend");
    g_threading = reinterpret_cast<ThreadingFn>(addrs::fn("Threading"));
    g_gfxByIndex = reinterpret_cast<GfxByIndexFn>(addrs::fn("GfxByIndex"));
    if (!render || !blend || !g_threading || !g_gfxByIndex) {
        LOG("UI layer: game build not recognised (Render %p, blend %p, threading %p, gfx %p); the UI stays in the 3D "
            "view", render, blend, reinterpret_cast<void*>(g_threading), reinterpret_cast<void*>(g_gfxByIndex));
        return false;
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    bool ok = MH_CreateHook(render, reinterpret_cast<void*>(&hookedRender), reinterpret_cast<void**>(&g_render)) == MH_OK &&
              MH_CreateHook(blend, reinterpret_cast<void*>(&hookedBlend), reinterpret_cast<void**>(&g_blend)) == MH_OK &&
              MH_EnableHook(render) == MH_OK && MH_EnableHook(blend) == MH_OK;
    LOG("UI layer: game hooks %s", ok ? "installed" : "FAILED");
    g_installed = ok;
    return ok;
}

ID3D11Texture2D* onPresent(ID3D11Texture2D* backbuffer, bool enabled) {
    if (!g_installed) return nullptr;
    if (!g_contextHooked.load() && enabled) {
        static bool tried = false;
        if (!tried) {
            tried = true;
            ID3D11Device* device = nullptr;
            backbuffer->GetDevice(&device);
            if (device) {
                hookContexts(device);
                device->Release();
            }
        }
    }
    ID3D11Texture2D* result = nullptr;
    bool drawn = g_drawn.exchange(false, std::memory_order_acquire);
    // A frame without a UI render (it happens) keeps the last UI for a moment
    // rather than closing menus over it.
    if (!drawn && g_resolved && g_enabled.load() && ++g_framesSinceDraw <= 10) result = g_resolved;
    if (drawn) {
        ID3D11Texture2D* ui = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ui = g_texture;
            if (ui) ui->AddRef();
        }
        if (ui) {
            D3D11_TEXTURE2D_DESC desc;
            ui->GetDesc(&desc);
            ID3D11Device* uiDevice = nullptr;
            ui->GetDevice(&uiDevice);
            if (uiDevice) uiDevice->Release();  // only compared
            if (!g_resolved || uiDevice != g_resolvedDevice || g_resolvedDesc.Width != desc.Width ||
                g_resolvedDesc.Height != desc.Height || g_resolvedDesc.Format != desc.Format) {
                if (g_resolvedView) g_resolvedView->Release();
                g_resolvedView = nullptr;
                if (g_resolved) g_resolved->Release();
                g_resolved = nullptr;
                if (g_raw) g_raw->Release();
                g_raw = nullptr;
                D3D11_TEXTURE2D_DESC one = desc;
                one.SampleDesc.Count = 1;
                one.SampleDesc.Quality = 0;
                one.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                ID3D11Device* device = nullptr;
                ui->GetDevice(&device);
                if (device && SUCCEEDED(device->CreateTexture2D(&one, nullptr, &g_resolved))) {
                    g_resolvedDesc = desc;
                    g_resolvedDevice = uiDevice;
                    if (FAILED(device->CreateRenderTargetView(g_resolved, nullptr, &g_resolvedView))) g_resolvedView = nullptr;
                    one.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (FAILED(device->CreateTexture2D(&one, nullptr, &g_raw))) g_raw = nullptr;
                }
                if (device) device->Release();
            }
            if (g_resolved) {
                ID3D11Device* device = nullptr;
                ui->GetDevice(&device);
                ID3D11DeviceContext* context = nullptr;
                device->GetImmediateContext(&context);
                // First person: the game's blue modal dimming comes out on the way in.
                ID3D11Texture2D* in = g_raw && g_resolvedView ? g_raw : g_resolved;
                if (desc.SampleDesc.Count > 1)
                    context->ResolveSubresource(in, 0, ui, 0, desc.Format);
                else
                    context->CopyResource(in, ui);
                if (in == g_raw && !nodim::apply(device, context, g_raw, g_resolvedView, desc))
                    context->CopyResource(g_resolved, g_raw);
                cutBubbles(device, context);
                context->Release();
                device->Release();
                result = g_resolved;
                g_framesSinceDraw = 0;
            }
            ui->Release();
        }
    }
    if (!g_enabled.load() && g_bubblesShown) {
        phone3d::setBubbles(nullptr, 0, nullptr);
        g_bubblesShown = false;
    }
    g_clearPending = true;
    g_phonePending = true;
    g_enabled.store(enabled && g_contextHooked.load(), std::memory_order_release);
    return result;
}

void beginSecondPass() {
    g_phonePending.store(true, std::memory_order_relaxed);
    g_secondPass.store(true, std::memory_order_release);
}

void endSecondPass() { g_secondPass.store(false, std::memory_order_release); }

void takeRenders(uint32_t& all, uint32_t& second) {
    all = g_uiRenders.exchange(0, std::memory_order_relaxed);
    second = g_uiSecondRenders.exchange(0, std::memory_order_relaxed);
}

ID3D11DepthStencilView* sceneDepthAtPresent(bool& reversed) {
    if (g_uiClearsDepth.load()) return nullptr;
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    ID3D11DepthStencilView* depth = g_sceneDepth.load();
    if (!depth) return nullptr;
    reversed = g_depthClear.load() == 0;
    depth->AddRef();
    return depth;
}

}  // namespace s4vr::uihook
