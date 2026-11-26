#include "beamlight.h"

#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "beampatch.h"
#include "camera.h"
#include "log.h"

namespace s4vr::beamlight {
namespace {

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);

CreatePixelShaderFn g_createPS = nullptr;
PSSetShaderFn g_setPS = nullptr;
bool g_installed = false;
std::atomic<int> g_patched{0}, g_seen{0}, g_failedCreate{0};
std::atomic<int> g_skipNoCube{0}, g_skipOther{0};

SRWLOCK g_lock = SRWLOCK_INIT;  // the patched shaders' pointers, and the beam
std::unordered_set<ID3D11PixelShader*> g_shaders;
Beam g_beam;
ULONGLONG g_beamAt = 0;

ID3D11Buffer* g_cb = nullptr;  // ours; made on the first patched shader's device
bool g_bound = false;          // slot kSlot holds g_cb (the render thread)
float g_lastCam[3] = {1e30f, 0, 0};
ULONGLONG g_lastBeamAt = 0;
bool g_lastOn = false;

bool beamLive(Beam& out) {
    AcquireSRWLockShared(&g_lock);
    out = g_beam;
    const bool fresh = g_beam.on && GetTickCount64() - g_beamAt < 300;
    ReleaseSRWLockShared(&g_lock);
    return fresh;
}

HRESULT STDMETHODCALLTYPE hookedCreatePixelShader(ID3D11Device* device, const void* code, SIZE_T size,
                                                  ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
    ++g_seen;
    if (code && size > 32 && out) {
        std::vector<uint8_t> patched;
        beampatch::Info info;
        bool ok = false;
        try {
            ok = beampatch::patch(static_cast<const uint8_t*>(code), size, patched, &info);
        } catch (...) {
            ok = false;
        }
        if (ok) {
            ID3D11PixelShader* shader = nullptr;
            const HRESULT hr = g_createPS(device, patched.data(), patched.size(), linkage, &shader);
            if (SUCCEEDED(hr) && shader) {
                if (!g_cb) {
                    D3D11_BUFFER_DESC bd{};
                    bd.ByteWidth = 4 * 16;
                    bd.Usage = D3D11_USAGE_DEFAULT;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    float zero[16] = {};
                    D3D11_SUBRESOURCE_DATA init{zero, 0, 0};
                    device->CreateBuffer(&bd, &init, &g_cb);
                }
                AcquireSRWLockExclusive(&g_lock);
                g_shaders.insert(shader);
                ReleaseSRWLockExclusive(&g_lock);
                const int n = ++g_patched;
                if (n <= 3 || n % 500 == 0) LOG("Beam: pixel shader %d patched (%u -> %u bytes)", n, (unsigned)size, (unsigned)patched.size());
                *out = shader;
                return hr;
            }
            const int f = ++g_failedCreate;  // the game's own shader is made instead
            if (f <= 5) LOG("Beam: a patched shader was refused (0x%08lx): the original is used", static_cast<unsigned long>(hr));
        } else if (info.cube) {
            ++g_skipOther;
        } else {
            ++g_skipNoCube;
        }
    }
    return g_createPS(device, code, size, linkage, out);
}

void STDMETHODCALLTYPE hookedPSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* shader, ID3D11ClassInstance* const* inst,
                                         UINT n) {
    g_setPS(ctx, shader, inst, n);
    if (!g_cb) return;
    Beam beam;
    const bool live = beamLive(beam);
    if (!live) {
        if (g_bound) {  // the beam went out: slot kSlot back to nothing (an unbound cbuffer reads as zero: off)
            ID3D11Buffer* none = nullptr;
            ctx->PSSetConstantBuffers(beampatch::kSlot, 1, &none);
            g_bound = false;
        }
        return;
    }
    if (!shader) return;
    AcquireSRWLockShared(&g_lock);
    const bool ours = g_shaders.count(shader) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (!ours) return;
    camera::View view;
    if (!camera::lastView(view)) return;
    // Fresh constants when the camera (this eye) or the beam changed.
    if (view.position.x != g_lastCam[0] || view.position.y != g_lastCam[1] || view.position.z != g_lastCam[2] ||
        g_lastBeamAt != g_beamAt || !g_lastOn) {
        const float rx = beam.pos[0] - view.position.x, ry = beam.pos[1] - view.position.y, rz = beam.pos[2] - view.position.z;
        const float cosOuter = std::cos(beam.cone * 0.5f), cosInner = std::cos(beam.cone * 0.3f);
        const float inv = 1.0f / std::max(1e-3f, cosInner - cosOuter);
        float c[16] = {rx, ry, rz, -1.0f / std::max(0.1f, beam.range),
                       -beam.dir[0], -beam.dir[1], -beam.dir[2], 0,
                       inv, -cosOuter * inv, 1.0f / std::max(1e-4f, beam.r0 * beam.r0), 1.0f,
                       beam.rgb[0], beam.rgb[1], beam.rgb[2], 0};
        ctx->UpdateSubresource(g_cb, 0, nullptr, c, 0, 0);
        g_lastCam[0] = view.position.x;
        g_lastCam[1] = view.position.y;
        g_lastCam[2] = view.position.z;
        g_lastBeamAt = g_beamAt;
        g_lastOn = true;
    }
    ctx->PSSetConstantBuffers(beampatch::kSlot, 1, &g_cb);
    g_bound = true;
}

}  // namespace

bool install(bool enable) {
    if (!enable || g_installed) return false;
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    auto create = d3d ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d, "D3D11CreateDevice")) : nullptr;
    if (!create) {
        LOG("Beam: no D3D11CreateDevice: the beam stays on the screen pass");
        return false;
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) ||
        !dev || !ctx) {
        LOG("Beam: no throwaway device: the beam stays on the screen pass");
        return false;
    }
    void* createAt = (*reinterpret_cast<void***>(dev))[15];  // ID3D11Device::CreatePixelShader
    void* setAt = (*reinterpret_cast<void***>(ctx))[9];      // ID3D11DeviceContext::PSSetShader
    const MH_STATUS init = MH_Initialize();
    bool ok = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED;
    ok = ok && MH_CreateHook(createAt, reinterpret_cast<void*>(&hookedCreatePixelShader), reinterpret_cast<void**>(&g_createPS)) == MH_OK;
    ok = ok && MH_CreateHook(setAt, reinterpret_cast<void*>(&hookedPSSetShader), reinterpret_cast<void**>(&g_setPS)) == MH_OK;
    ok = ok && MH_EnableHook(createAt) == MH_OK && MH_EnableHook(setAt) == MH_OK;
    ctx->Release();
    dev->Release();
    if (!ok) {
        LOG("Beam: hooks failed: the beam stays on the screen pass");
        return false;
    }
    g_installed = true;
    LOG("Beam: pixel shaders and PSSetShader hooked (cbuffer slot %u)", beampatch::kSlot);
    return true;
}

void set(const Beam& beam) {
    AcquireSRWLockExclusive(&g_lock);
    g_beam = beam;
    g_beamAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
}

int patchedCount() { return g_patched.load(); }
bool active() { return g_installed && g_patched.load() > 0; }

}  // namespace s4vr::beamlight
