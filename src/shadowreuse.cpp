#include "shadowreuse.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "MinHook.h"
#include "addrs.h"
#include "log.h"
#include "singleview.h"

namespace s4vr::shadowreuse {

namespace {

// ID3D11DeviceContext slots: DrawIndexed 12, Draw 13, DrawIndexedInstanced 20, DrawInstanced 21,
// DrawIndexedInstancedIndirect 39, DrawInstancedIndirect 40.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
constexpr size_t kSlots[6] = {12, 13, 20, 21, 39, 40};

// Per implementation (immediate, deferred when it differs).
DrawIndexedFn g_drawIndexed[2] = {};
DrawFn g_draw[2] = {};
DrawIndexedInstancedFn g_drawIndexedInstanced[2] = {};
DrawInstancedFn g_drawInstanced[2] = {};
DrawIndirectFn g_drawIndexedIndirect[2] = {};
DrawIndirectFn g_drawIndirect[2] = {};

std::atomic<bool> g_on{false};
std::atomic<bool> g_hooked{false};
std::atomic<uint32_t> g_draws{0}, g_clears{0}, g_binds{0};
std::atomic<ULONGLONG> g_lastLog{0};

// The context this thread last bound the sun shadow map on in the second pass (its draws are left
// out until it binds something else). A deferred context is recorded by one thread at a time.
thread_local ID3D11DeviceContext* t_skip = nullptr;

// Depth targets seen, and whether each is the sun shadow map: cascades side by side in one 2D
// texture, one side 3 times the other (shadows.cpp kCascades), at least 512 px.
struct Known {
    ID3D11DepthStencilView* view;
    bool sun;
};
constexpr int kKnown = 64;
Known g_known[kKnown];
int g_knownCount = 0;
std::mutex g_knownMutex;

bool isSunMap(ID3D11DepthStencilView* view) {
    thread_local ID3D11DepthStencilView* t_view = nullptr;
    thread_local bool t_sun = false;
    if (view == t_view) return t_sun;
    std::lock_guard<std::mutex> lock(g_knownMutex);
    for (int i = 0; i < g_knownCount; ++i)
        if (g_known[i].view == view) {
            t_view = view;
            t_sun = g_known[i].sun;
            return t_sun;
        }
    bool sun = false;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (resource) resource->GetType(&dim);
    D3D11_TEXTURE2D_DESC d = {};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
        const UINT lo = d.Width < d.Height ? d.Width : d.Height, hi = d.Width < d.Height ? d.Height : d.Width;
        sun = d.ArraySize == 1 && lo >= 512 && hi == 3 * lo;
    }
    if (resource) resource->Release();
    if (g_knownCount < kKnown) {
        view->AddRef();  // kept alive, so its address never comes back as another view
        g_known[g_knownCount++] = Known{view, sun};
        t_view = view;
        t_sun = sun;
        // Every depth target bound depth-only or cleared in a second pass, once: candidates to reuse.
        LOG("Shadow reuse: second-pass depth target %p, %ux%u x%u format %d%s", static_cast<void*>(view), d.Width,
            d.Height, d.ArraySize, static_cast<int>(d.Format), sun ? ": the sun shadow map, reused" : "");
    }
    return sun;
}

void maybeLog() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastLog.load(std::memory_order_relaxed);
    if (now - last < 10000 || !g_lastLog.compare_exchange_strong(last, now)) return;
    const uint32_t binds = g_binds.exchange(0), draws = g_draws.exchange(0), clears = g_clears.exchange(0);
    if (binds || draws || clears)
        LOG("Shadow reuse (last 10 s): the right eye drew into the sun shadow map %u times; %u draws and %u clears "
            "left out (the left eye's map is used)",
            binds, draws, clears);
}

inline bool skip(ID3D11DeviceContext* c) {
    if (t_skip != c) return false;
    g_draws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <int I>
void STDMETHODCALLTYPE hDrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
    if (!skip(c)) g_drawIndexed[I](c, a, b, d);
}
template <int I>
void STDMETHODCALLTYPE hDraw(ID3D11DeviceContext* c, UINT a, UINT b) {
    if (!skip(c)) g_draw[I](c, a, b);
}
template <int I>
void STDMETHODCALLTYPE hDrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    if (!skip(c)) g_drawIndexedInstanced[I](c, a, b, d, e, f);
}
template <int I>
void STDMETHODCALLTYPE hDrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    if (!skip(c)) g_drawInstanced[I](c, a, b, d, e);
}
template <int I>
void STDMETHODCALLTYPE hDrawIndexedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    if (!skip(c)) g_drawIndexedIndirect[I](c, b, o);
}
template <int I>
void STDMETHODCALLTYPE hDrawIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    if (!skip(c)) g_drawIndirect[I](c, b, o);
}

template <int I>
bool hookSet(void** vtbl) {
    void* detours[6] = {reinterpret_cast<void*>(&hDrawIndexed<I>), reinterpret_cast<void*>(&hDraw<I>),
                        reinterpret_cast<void*>(&hDrawIndexedInstanced<I>), reinterpret_cast<void*>(&hDrawInstanced<I>),
                        reinterpret_cast<void*>(&hDrawIndexedIndirect<I>), reinterpret_cast<void*>(&hDrawIndirect<I>)};
    void** originals[6] = {reinterpret_cast<void**>(&g_drawIndexed[I]), reinterpret_cast<void**>(&g_draw[I]),
                           reinterpret_cast<void**>(&g_drawIndexedInstanced[I]),
                           reinterpret_cast<void**>(&g_drawInstanced[I]),
                           reinterpret_cast<void**>(&g_drawIndexedIndirect[I]),
                           reinterpret_cast<void**>(&g_drawIndirect[I])};
    for (int k = 0; k < 6; ++k) {
        MH_STATUS s = MH_CreateHook(vtbl[kSlots[k]], detours[k], originals[k]);
        if (s != MH_OK) {
            LOG("Shadow reuse: draw hook %zu failed (%s); off", kSlots[k], MH_StatusToString(s));
            return false;
        }
    }
    for (int k = 0; k < 6; ++k)
        if (MH_EnableHook(vtbl[kSlots[k]]) != MH_OK) return false;
    return true;
}

// ShadowLists(renderer->a8, flag, kind mask, 0), on the scene lists' job thread.
using ListsFn = void (*)(void* renderer, uint64_t flag, uint64_t mask, uint64_t extra);
ListsFn g_lists = nullptr;
std::atomic<bool> g_skipLists{false};
std::atomic<uint32_t> g_listRuns[2] = {}, g_listSkips{0};
std::atomic<uint64_t> g_listTicks[2] = {};
std::atomic<ULONGLONG> g_listLog{0};

int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The kinds of calls (flag, mask) seen, with the highest index and per-eye counts, logged with the
// 10 s line. Only the sun shadow job's calls (flag 1) are left out of the right eye: the Sims' calls
// (flag 0, mask 0x200000) also feed the mirror's view, and leaving them out lost Sims, plumbobs
// and other objects from the right eye's mirror (326 screenshots, user 2026-10-22).
struct ListKind {
    std::atomic<uint64_t> key{0};  // flag << 32 | mask, 0 = free
    std::atomic<uint32_t> maxIndex{0};
    std::atomic<uint32_t> runs[2] = {}, skipped{0};
};
ListKind g_kinds[8];

void countKind(uint64_t flag, uint64_t mask, uint64_t extra, bool second, bool skipped) {
    const uint64_t key = (flag & 0xff) << 32 | (mask & 0xffffffffull) | 1ull << 40;
    for (ListKind& k : g_kinds) {
        uint64_t have = k.key.load(std::memory_order_relaxed);
        if (have == 0 && k.key.compare_exchange_strong(have, key)) have = key;
        if (have != key) continue;
        const uint32_t index = static_cast<uint32_t>(extra);
        uint32_t m = k.maxIndex.load(std::memory_order_relaxed);
        while (index > m && index < 1000 && !k.maxIndex.compare_exchange_weak(m, index)) {
        }
        (skipped ? k.skipped : k.runs[second]).fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void logLists() {
    const ULONGLONG at = GetTickCount64();
    ULONGLONG last = g_listLog.load(std::memory_order_relaxed);
    if (at - last < 10000 || !g_listLog.compare_exchange_strong(last, at)) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / static_cast<double>(f.QuadPart);
    uint32_t runs[2];
    double each[2];
    for (int i = 0; i < 2; ++i) {
        runs[i] = g_listRuns[i].exchange(0);
        const uint64_t t = g_listTicks[i].exchange(0);
        each[i] = runs[i] ? t * ms / runs[i] : 0.0;
    }
    LOG("Shadow lists (last 10 s): left eye %u runs %.2f ms each, right eye %u runs %.2f ms each, %u left out of "
        "the right eye",
        runs[0], each[0], runs[1], each[1], g_listSkips.exchange(0));
    for (ListKind& k : g_kinds) {
        const uint64_t key = k.key.load(std::memory_order_relaxed);
        if (!key) continue;
        LOG("Shadow lists kind: flag %u mask %08x (index up to %u): left eye %u, right eye %u, %u left out",
            static_cast<unsigned>((key >> 32) & 0xff), static_cast<unsigned>(key & 0xffffffffull), k.maxIndex.load(),
            k.runs[0].exchange(0), k.runs[1].exchange(0), k.skipped.exchange(0));
    }
}

void hookedLists(void* renderer, uint64_t flag, uint64_t mask, uint64_t extra) {
    const bool second = singleview::inSecond();
    const bool sunJob = (flag & 0xff) == 1;
    const bool skip = second && sunJob && g_skipLists.load(std::memory_order_relaxed) && on();
    countKind(flag, mask, extra, second, skip);
    if (skip) {
        g_listSkips.fetch_add(1, std::memory_order_relaxed);
        logLists();
        return;
    }
    const int64_t start = now();
    g_lists(renderer, flag, mask, extra);
    g_listTicks[second].fetch_add(static_cast<uint64_t>(now() - start), std::memory_order_relaxed);
    g_listRuns[second].fetch_add(1, std::memory_order_relaxed);
    logLists();
}

}  // namespace

void installGame() {
    void* fn = addrs::fn("ShadowLists");
    MH_STATUS s = MH_Initialize();
    if (!fn || (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)) {
        LOG("Shadow lists: game build not recognised; not timed");
        return;
    }
    s = MH_CreateHook(fn, reinterpret_cast<void*>(&hookedLists), reinterpret_cast<void**>(&g_lists));
    if (s == MH_OK) s = MH_EnableHook(fn);
    LOG("Shadow lists: timed per eye (%s)", MH_StatusToString(s));
}

void setSkipLists(bool on) {
    if (g_skipLists.exchange(on) != on)
        LOG("Shadow lists: the right eye %s", on ? "leaves them out (test)" : "walks them again");
}

void install(ID3D11Device* device) {
    static bool tried = false;
    if (tried || !device) return;
    tried = true;
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    ID3D11DeviceContext* deferred = nullptr;
    device->CreateDeferredContext(0, &deferred);
    void** a = immediate ? *reinterpret_cast<void***>(immediate) : nullptr;
    void** b = deferred ? *reinterpret_cast<void***>(deferred) : nullptr;
    bool ok = a && hookSet<0>(a);
    const bool two = b && b[kSlots[0]] != a[kSlots[0]];
    if (ok && two) ok = hookSet<1>(b);
    if (immediate) immediate->Release();
    if (deferred) deferred->Release();
    g_hooked.store(ok);
    LOG("Shadow reuse: %s (%s)", ok ? "ready" : "hooks failed; off", two ? "immediate and deferred contexts" : "one context type");
}

void setOn(bool on) {
    if (g_on.exchange(on) != on)
        LOG("Shadow reuse: %s (both real eyes: the right eye %s)", on ? "on" : "off",
            on ? "uses the left eye's sun shadow map" : "draws its own sun shadow map");
}

bool on() { return g_on.load(std::memory_order_relaxed) && g_hooked.load(std::memory_order_relaxed); }

void bound(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views,
           ID3D11DepthStencilView* depth) {
    const bool depthOnly = depth && (!views || !count || !views[0]);
    if (!depthOnly || !on() || !singleview::inSecond() || !isSunMap(depth)) {
        if (t_skip) t_skip = nullptr;
        return;
    }
    t_skip = context;
    g_binds.fetch_add(1, std::memory_order_relaxed);
    maybeLog();
}

bool skipClear(ID3D11DeviceContext*, ID3D11DepthStencilView* depth) {
    if (!depth || !on() || !singleview::inSecond()) return false;
    if (!isSunMap(depth)) return false;
    g_clears.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace s4vr::shadowreuse
