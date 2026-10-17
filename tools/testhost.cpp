// Stand-in for the game: a D3D11 window that loads sims4vr.dll exactly like the
// script mod does. Use it to test the headset pipeline without launching The Sims 4.
// Run from the package folder so sims4vr.dll, openxr_loader.dll and sims4vr.ini sit next to it.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using InitFn = int (*)();
using StatusFn = int (*)(char*, int);
using ShutdownFn = void (*)();

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Optional argument: seconds to run before exiting cleanly (default: until the window closes).
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const ULONGLONG deadline = argc > 1 ? GetTickCount64() + 1000ULL * atoi(argv[1]) : 0;
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sims4vr_testhost";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Sims4VR test host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                              1280, 720, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context))) {
        printf("D3D11 device creation failed\n");
        return 1;
    }
    ID3D11Texture2D* backbuffer = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    backbuffer->Release();

    HMODULE mod = LoadLibraryW(L"sims4vr.dll");
    if (!mod) {
        printf("sims4vr.dll not found in the current folder\n");
        return 1;
    }
    auto init = reinterpret_cast<InitFn>(reinterpret_cast<void*>(GetProcAddress(mod, "s4vr_init")));
    auto status = reinterpret_cast<StatusFn>(reinterpret_cast<void*>(GetProcAddress(mod, "s4vr_status")));
    auto shutdown = reinterpret_cast<ShutdownFn>(reinterpret_cast<void*>(GetProcAddress(mod, "s4vr_shutdown")));
    printf("s4vr_init returned %d\n", init());

    MSG msg{};
    for (int frame = 0; msg.message != WM_QUIT && (!deadline || GetTickCount64() < deadline); ++frame) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // Slowly cycling colour, so a frozen frame is obvious in the headset.
        float t = frame / 60.0f;
        float colour[4] = {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f), 0.5f + 0.5f * std::sin(t + 4.2f), 1};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, colour);
        swapchain->Present(1, 0);
        if (frame % 300 == 0) {
            char line[1024];
            status(line, sizeof(line));
            printf("%s\n", line);
        }
    }
    shutdown();
    printf("shut down cleanly\n");
    return 0;
}
