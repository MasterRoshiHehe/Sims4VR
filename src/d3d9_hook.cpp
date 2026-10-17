#include "d3d9_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d9.h>
#include <dxgi.h>
#include <MinHook.h>

#include "d3d11_hook.h"
#include "log.h"
#include "vr.h"

namespace s4vr::d3d9_hook {

namespace {

const GUID kIidDevice9Ex = {0xb18b10ce, 0x2649, 0x405a, {0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a}};
const GUID kIidD3D9Ex = {0x02177241, 0x69fc, 0x400c, {0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d}};

// Vtable slots: IDirect3DDevice9::Present, IDirect3DDevice9Ex::PresentEx,
// IDirect3DSwapChain9::Present, IDirect3DDevice9::Reset, IDirect3DDevice9Ex::ResetEx.
constexpr size_t kPresentSlot = 17, kPresentExSlot = 121, kSwapPresentSlot = 3, kResetSlot = 16, kResetExSlot = 132;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                DWORD);
using SwapPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                  const RGNDATA*, DWORD);
using CreateEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

PresentFn g_present = nullptr;
PresentExFn g_presentEx = nullptr;
SwapPresentFn g_swapPresent = nullptr;
ResetFn g_reset = nullptr;
ResetExFn g_resetEx = nullptr;
thread_local bool t_inPresent = false;  // one Present may call another internally

// Render thread only.
bool g_failed = false;  // can't share frames with this device: VR stays off
IDirect3DDevice9* g_device9 = nullptr;  // not owned: the device the resources below belong to
ID3D11Device* g_device11 = nullptr;
IDirect3DTexture9* g_shared9 = nullptr;
IDirect3DSurface9* g_sharedSurface9 = nullptr;
ID3D11Texture2D* g_shared11 = nullptr;
IDirect3DQuery9* g_done = nullptr;
IDirect3DSurface9* g_mirror9 = nullptr;  // the last left-eye frame, for the monitor
bool g_mirrorValid = false;
UINT g_width = 0, g_height = 0;
HWND g_window = nullptr;
uint64_t g_frames = 0, g_slowWaits = 0;

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void releaseFrameResources() {
    release(g_shared11);
    release(g_sharedSurface9);
    release(g_shared9);
    release(g_mirror9);
    g_mirrorValid = false;
    g_width = g_height = 0;
}


// A D3D11 device on the GPU the game renders with (shared textures only open there).
bool createDevice11(IDirect3DDevice9Ex* device) {
    D3DDEVICE_CREATION_PARAMETERS params{};
    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;
    LUID luid{};
    bool haveLuid = SUCCEEDED(device->GetCreationParameters(&params)) && SUCCEEDED(device->GetDirect3D(&d3d)) &&
                    SUCCEEDED(d3d->QueryInterface(kIidD3D9Ex, reinterpret_cast<void**>(&d3dEx))) &&
                    SUCCEEDED(d3dEx->GetAdapterLUID(params.AdapterOrdinal, &luid));
    release(d3dEx);
    release(d3d);
    if (!g_window) g_window = params.hFocusWindow;

    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (haveLuid && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(a->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
                desc.AdapterLuid.HighPart == luid.HighPart) {
                adapter = a;
                LOG("DX9: the game renders on %ls", desc.Description);
                break;
            }
            a->Release();
        }
        factory->Release();
    }
    if (!adapter) LOG("DX9: the game's GPU was not identified; using the default one");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    auto create = d3d11_hook::systemCreateDevice();
    HRESULT hr = create ? create(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &g_device11, nullptr,
                                 nullptr)
                        : E_NOINTERFACE;
    release(adapter);
    if (FAILED(hr)) {
        LOG("DX9: could not create the D3D11 device for the headset (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// The shared texture (and the monitor copy) at the backbuffer's size.
bool ensureShared(IDirect3DDevice9Ex* device, const D3DSURFACE_DESC& bb) {
    if (g_shared11 && bb.Width == g_width && bb.Height == g_height) return true;
    releaseFrameResources();
    HANDLE handle = nullptr;
    HRESULT hr = device->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                       &g_shared9, &handle);
    static int failures = 0;
    if (FAILED(hr) || !handle) {
        if (failures++ < 5)
            LOG("DX9: could not create the shared frame texture %ux%u (0x%08lx)", bb.Width, bb.Height,
                static_cast<unsigned long>(hr));
        return false;
    }
    g_shared9->GetSurfaceLevel(0, &g_sharedSurface9);
    hr = g_device11->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_shared11));
    if (FAILED(hr) || !g_sharedSurface9) {
        if (failures++ < 5)
            LOG("DX9: D3D11 could not open the shared frame texture (0x%08lx)", static_cast<unsigned long>(hr));
        releaseFrameResources();
        return false;
    }
    // The monitor copy keeps the backbuffer's own format (and any multisampling).
    if (FAILED(device->CreateRenderTarget(bb.Width, bb.Height, bb.Format, bb.MultiSampleType, bb.MultiSampleQuality,
                                          FALSE, &g_mirror9, nullptr)))
        g_mirror9 = nullptr;
    g_width = bb.Width;
    g_height = bb.Height;
    LOG("DX9: frames go to the headset through a shared %ux%u texture (backbuffer format %d, %d samples)%s", bb.Width,
        bb.Height, static_cast<int>(bb.Format), static_cast<int>(bb.MultiSampleType),
        g_mirror9 ? "" : "; the monitor may flicker between eyes");
    return true;
}

// Waits until the GPU has done everything this device was given so far.
void waitForGpu(IDirect3DDevice9* device) {
    if (!g_done && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &g_done))) g_done = nullptr;
    if (!g_done) return;
    g_done->Issue(D3DISSUE_END);
    ULONGLONG start = GetTickCount64();
    while (g_done->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount64() - start > 100) {
            if (g_slowWaits++ < 5) LOG("DX9: the frame copy took over 100 ms; sent as it is");
            break;
        }
        YieldProcessor();
    }
}

void onPresent(IDirect3DDevice9* device, IDirect3DSwapChain9* swapchain, HWND destination) {
    if (g_failed || !device) return;
    if (g_frames++ == 0) LOG("DX9: first Present seen");
    IDirect3DDevice9Ex* ex = nullptr;
    if (FAILED(device->QueryInterface(kIidDevice9Ex, reinterpret_cast<void**>(&ex)))) {
        LOG("DX9: the game's Direct3D 9 device is not a D3D9Ex device; frames can't reach the headset");
        g_failed = true;
        return;
    }
    if (device != g_device9) {
        // A new game device: new shared textures. The D3D11 device stays (the
        // headset session was made with it).
        releaseFrameResources();
        release(g_done);
        g_device9 = device;
        if (!g_device11 && !createDevice11(ex)) {
            g_failed = true;
            ex->Release();
            return;
        }
    }
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = swapchain ? swapchain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                           : device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(hr) && bb && SUCCEEDED(bb->GetDesc(&desc)) && ensureShared(ex, desc)) {
        if (destination) g_window = destination;
        if (!g_window && swapchain) {
            D3DPRESENT_PARAMETERS pp{};
            if (SUCCEEDED(swapchain->GetPresentParameters(&pp))) g_window = pp.hDeviceWindow;
        }
        if (SUCCEEDED(ex->StretchRect(bb, nullptr, g_sharedSurface9, nullptr, D3DTEXF_NONE))) {
            waitForGpu(ex);
            int eye = vr::onFrame(g_shared11, g_window, true);
            // Stereo renders one eye per frame; the monitor shows the left one.
            if (g_mirror9 && eye == 0) {
                g_mirrorValid = SUCCEEDED(ex->StretchRect(bb, nullptr, g_mirror9, nullptr, D3DTEXF_NONE));
            } else if (g_mirror9 && eye == 1 && g_mirrorValid) {
                ex->StretchRect(g_mirror9, nullptr, bb, nullptr, D3DTEXF_NONE);
            } else if (eye < 0) {
                g_mirrorValid = false;
            }
        } else {
            static int logged = 0;
            if (logged++ < 3)
                LOG("DX9: copying the backbuffer (format %d) failed", static_cast<int>(desc.Format));
        }
    }
    release(bb);
    ex->Release();
}

HRESULT STDMETHODCALLTYPE hookedPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                        const RGNDATA* dirty) {
    if (!t_inPresent) onPresent(device, nullptr, window);
    bool outer = !t_inPresent;
    t_inPresent = true;
    HRESULT hr = g_present(device, source, dest, window, dirty);
    if (outer) t_inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedPresentEx(IDirect3DDevice9Ex* device, const RECT* source, const RECT* dest, HWND window,
                                          const RGNDATA* dirty, DWORD flags) {
    if (flags & D3DPRESENT_DONOTWAIT) {
        static bool logged = false;
        if (!logged) LOG("DX9: the game presents without waiting; those calls are skipped");
        logged = true;
    }
    if (!t_inPresent && !(flags & D3DPRESENT_DONOTWAIT)) onPresent(device, nullptr, window);
    bool outer = !t_inPresent;
    t_inPresent = true;
    HRESULT hr = g_presentEx(device, source, dest, window, dirty, flags);
    if (outer) t_inPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedSwapPresent(IDirect3DSwapChain9* swapchain, const RECT* source, const RECT* dest,
                                            HWND window, const RGNDATA* dirty, DWORD flags) {
    if (!t_inPresent && !(flags & D3DPRESENT_DONOTWAIT)) {
        IDirect3DDevice9* device = nullptr;
        if (SUCCEEDED(swapchain->GetDevice(&device)) && device) {
            onPresent(device, swapchain, window);
            device->Release();
        }
    }
    bool outer = !t_inPresent;
    t_inPresent = true;
    HRESULT hr = g_swapPresent(swapchain, source, dest, window, dirty, flags);
    if (outer) t_inPresent = false;
    return hr;
}

// A device reset (the game changes its render size or MSAA) wants every
// default-pool resource of the device gone first: ours too. They are made
// again at the next Present.
HRESULT STDMETHODCALLTYPE hookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    if (device == g_device9) {
        releaseFrameResources();
        release(g_done);
    }
    HRESULT hr = g_reset(device, pp);
    static int logged = 0;
    if (logged++ < 10)
        LOG("DX9: device reset to %ux%u (0x%08lx)", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
            static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hookedResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    if (device == g_device9) {
        releaseFrameResources();
        release(g_done);
    }
    HRESULT hr = g_resetEx(device, pp, mode);
    static int logged = 0;
    if (logged++ < 10)
        LOG("DX9: device reset (Ex) to %ux%u (0x%08lx)", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
            static_cast<unsigned long>(hr));
    return hr;
}

// The Present functions live at fixed vtable slots shared by every device in
// the process, so a throwaway device on a hidden window finds them.
bool findPresent(void*& present, void*& presentEx, void*& swapPresent, void*& reset, void*& resetEx) {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<CreateEx>(GetProcAddress(d3d9, "Direct3DCreate9Ex")) : nullptr;
    if (!create) {
        LOG("DX9: Direct3DCreate9Ex not available");
        return false;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sims4vr_dummy9";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    IDirect3D9Ex* d3d = nullptr;
    IDirect3DDevice9Ex* device = nullptr;
    bool ok = false;
    if (SUCCEEDED(create(D3D_SDK_VERSION, &d3d))) {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = pp.BackBufferHeight = 1;
        pp.hDeviceWindow = hwnd;
        HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp,
                                         nullptr, &device);
        if (SUCCEEDED(hr)) {
            void** vtable = *reinterpret_cast<void***>(device);
            present = vtable[kPresentSlot];
            presentEx = vtable[kPresentExSlot];
            reset = vtable[kResetSlot];
            resetEx = vtable[kResetExSlot];
            IDirect3DSwapChain9* swapchain = nullptr;
            if (SUCCEEDED(device->GetSwapChain(0, &swapchain)) && swapchain) {
                swapPresent = (*reinterpret_cast<void***>(swapchain))[kSwapPresentSlot];
                swapchain->Release();
            }
            ok = true;
            device->Release();
        } else {
            LOG("DX9: test device failed (0x%08lx)", static_cast<unsigned long>(hr));
        }
        d3d->Release();
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

template <typename F>
bool hook(void* target, void* detour, F& original, const char* name) {
    if (!target) return false;
    bool ok = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) == MH_OK && MH_EnableHook(target) == MH_OK;
    LOG("DX9: %s %s at %p", name, ok ? "hooked" : "hook FAILED", target);
    return ok;
}

}  // namespace

bool install() {
    void *present = nullptr, *presentEx = nullptr, *swapPresent = nullptr, *reset = nullptr, *resetEx = nullptr;
    if (!findPresent(present, presentEx, swapPresent, reset, resetEx)) return false;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    bool any = hook(present, reinterpret_cast<void*>(&hookedPresent), g_present, "Present");
    if (presentEx != present) any = hook(presentEx, reinterpret_cast<void*>(&hookedPresentEx), g_presentEx, "PresentEx") || any;
    if (swapPresent && swapPresent != present && swapPresent != presentEx)
        any = hook(swapPresent, reinterpret_cast<void*>(&hookedSwapPresent), g_swapPresent, "swapchain Present") || any;
    if (any) {
        hook(reset, reinterpret_cast<void*>(&hookedReset), g_reset, "Reset");
        if (resetEx != reset) hook(resetEx, reinterpret_cast<void*>(&hookedResetEx), g_resetEx, "ResetEx");
    }
    return any;
}

}  // namespace s4vr::d3d9_hook
