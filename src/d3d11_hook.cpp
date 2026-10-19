#include "d3d11_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>

#include <string>

#include "framecost.h"
#include "framemap.h"
#include "log.h"

namespace s4vr::d3d11_hook {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static PresentFn g_originalPresent = nullptr;
static Present1Fn g_originalPresent1 = nullptr;
static PresentCallback g_callback = nullptr;
static thread_local bool t_inPresent = false;  // Present1 may call Present internally

static HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    // DXGI_PRESENT_TEST only queries occlusion; nothing is presented.
    const bool picture = !t_inPresent && !(flags & DXGI_PRESENT_TEST);
    if (picture) {
        framecost::presentStart();
        framemap::mark(framemap::Present);
    }
    if (g_callback && picture) g_callback(swapchain);
    t_inPresent = true;
    HRESULT hr = g_originalPresent(swapchain, syncInterval, flags);
    t_inPresent = false;
    if (picture) framecost::presentEnd();
    return hr;
}

// Flip-model swapchains (IDXGISwapChain1) can present with Present1 instead.
static HRESULT STDMETHODCALLTYPE hookedPresent1(IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG("Game presents with Present1");
    }
    const bool picture = !t_inPresent && !(flags & DXGI_PRESENT_TEST);
    if (picture) {
        framecost::presentStart();
        framemap::mark(framemap::Present);
    }
    if (g_callback && picture) g_callback(swapchain);
    t_inPresent = true;
    HRESULT hr = g_originalPresent1(swapchain, syncInterval, flags, params);
    t_inPresent = false;
    if (picture) framecost::presentEnd();
    return hr;
}

static HMODULE systemD3D11() {
    static HMODULE module = [] {
        wchar_t dir[MAX_PATH] = {};
        UINT length = GetSystemDirectoryW(dir, MAX_PATH);
        std::wstring path = std::wstring(dir, length) + L"\\d3d11.dll";
        HMODULE system = LoadLibraryW(path.c_str());
        if (!system) {
            LOG("D3D11: %ls could not be loaded (error %lu)", path.c_str(), GetLastError());
            return system;
        }
        wchar_t other[MAX_PATH] = {};
        HMODULE loaded = GetModuleHandleW(L"d3d11.dll");
        if (loaded && loaded != system && GetModuleFileNameW(loaded, other, MAX_PATH))
            LOG("D3D11: the game uses %ls; the mod's own devices use %ls", other, path.c_str());
        return system;
    }();
    return module;
}

PFN_D3D11_CREATE_DEVICE systemCreateDevice() {
    HMODULE module = systemD3D11();
    return module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(module, "D3D11CreateDevice")) : nullptr;
}

PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN systemCreateDeviceAndSwapChain() {
    HMODULE module = systemD3D11();
    return module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                        GetProcAddress(module, "D3D11CreateDeviceAndSwapChain"))
                  : nullptr;
}

// Present lives at a fixed vtable slot, shared by every swapchain in the
// process, so a throwaway device on a hidden window is enough to find it.
static void* g_present1 = nullptr;

static void* findPresentAddress() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sims4vr_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    auto create = systemCreateDeviceAndSwapChain();
    HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
                                 &swapchain, &device, nullptr, &context)
                        : E_NOINTERFACE;
    void* present = nullptr;
    if (SUCCEEDED(hr)) {
        void** vtable = *reinterpret_cast<void***>(swapchain);
        present = vtable[8];  // IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) -> Present
        IDXGISwapChain1* swapchain1 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapchain1)))) {
            g_present1 = (*reinterpret_cast<void***>(swapchain1))[22];  // IDXGISwapChain(18) + 4 -> Present1
            swapchain1->Release();
        }
        swapchain->Release();
        context->Release();
        device->Release();
    } else {
        LOG("D3D11CreateDeviceAndSwapChain failed: 0x%08lX", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return present;
}

bool install(PresentCallback onPresent) {
    g_callback = onPresent;
    void* present = findPresentAddress();
    if (!present) return false;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(present, reinterpret_cast<void*>(&hookedPresent), reinterpret_cast<void**>(&g_originalPresent));
    if (status != MH_OK) {
        LOG("MH_CreateHook(Present) failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(present);
    if (status != MH_OK) {
        LOG("MH_EnableHook(Present) failed: %s", MH_StatusToString(status));
        return false;
    }
    LOG("Present hooked at %p", present);
    if (g_present1 && g_present1 != present &&
        MH_CreateHook(g_present1, reinterpret_cast<void*>(&hookedPresent1), reinterpret_cast<void**>(&g_originalPresent1)) == MH_OK &&
        MH_EnableHook(g_present1) == MH_OK) {
        LOG("Present1 hooked at %p", g_present1);
    }
    return true;
}

}  // namespace s4vr::d3d11_hook
