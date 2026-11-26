#include "monitorfit.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

#include "log.h"

namespace s4vr::monitorfit {

namespace {

const char kCode[] = R"HLSL(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Params : register(b0) { float4 map; };  // xy scale, zw offset (uv' = (uv - 0.5) * scale + 0.5 + offset)
struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VsOut vsMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 psMain(VsOut i) : SV_Target {
    float2 t = (i.uv - 0.5) * map.xy + 0.5 + map.zw;
    if (any(t < 0) || any(t > 1)) return float4(0, 0, 0, 1);
    return float4(source.SampleLevel(linearClamp, t, 0).rgb, 1);
}
)HLSL";

ID3D11Device* g_device = nullptr;  // compared only
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_params = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3DDeviceContextState* g_state = nullptr;
ID3D11Texture2D* g_copy = nullptr;
ID3D11ShaderResourceView* g_copyView = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc{};
bool g_failed = false;

template <typename T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void release() {
    rel(g_copyView);
    rel(g_copy);
    rel(g_state);
    rel(g_sampler);
    rel(g_params);
    rel(g_ps);
    rel(g_vs);
    g_copyDesc = {};
    g_device = nullptr;
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) return false;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(compileFn(kCode, sizeof(kCode) - 1, "monitorfit", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vs, &err));
    rel(err);
    ok = ok && SUCCEEDED(compileFn(kCode, sizeof(kCode) - 1, "monitorfit", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &ps, &err));
    if (err) {
        LOG("Monitor fit: shader: %.300s", static_cast<const char*>(err->GetBufferPointer()));
        rel(err);
    }
    ok = ok && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    rel(vs);
    rel(ps);
    if (!ok) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params)) || FAILED(device->CreateSamplerState(&s, &g_sampler)))
        return false;
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) return false;
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
    device1->Release();
    if (FAILED(hr)) return false;
    g_device = device;
    return true;
}

}  // namespace

void apply(ID3D11Texture2D* backbuffer, HWND window, int mode, float zoom) {
    if ((mode != 1 && mode != 2) || !backbuffer || !window || g_failed) return;
    RECT rc;
    if (!GetClientRect(window, &rc) || rc.right < 64 || rc.bottom < 64) return;
    D3D11_TEXTURE2D_DESC desc;
    backbuffer->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) return;
    const float src = static_cast<float>(desc.Width) / desc.Height, dst = static_cast<float>(rc.right) / rc.bottom;
    if (std::fabs(src / dst - 1.0f) < 0.03f) return;  // already the window's shape

    ID3D11Device* device = nullptr;
    backbuffer->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11DeviceContext1* context1 = nullptr;
    context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
    if (g_device && g_device != device) release();
    if (!g_device && !init(device)) {
        LOG("Monitor fit: could not set up; the desktop window stays stretched");
        release();
        g_failed = true;
    }
    if (!g_failed && context1) {
        if (!g_copy || g_copyDesc.Width != desc.Width || g_copyDesc.Height != desc.Height || g_copyDesc.Format != desc.Format) {
            rel(g_copyView);
            rel(g_copy);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = desc.Width;
            d.Height = desc.Height;
            d.MipLevels = d.ArraySize = 1;
            d.Format = desc.Format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &g_copy)) ||
                FAILED(device->CreateShaderResourceView(g_copy, nullptr, &g_copyView))) {
                rel(g_copyView);
                rel(g_copy);
                g_failed = true;
                LOG("Monitor fit: could not create the copy texture (format %d)", static_cast<int>(desc.Format));
            } else {
                g_copyDesc = d;
                LOG("Monitor fit: mode %d, picture %ux%u in a %ldx%ld window", mode, desc.Width, desc.Height, rc.right, rc.bottom);
            }
        }
        ID3D11RenderTargetView* rtv = nullptr;
        if (!g_failed && FAILED(device->CreateRenderTargetView(backbuffer, nullptr, &rtv))) {
            g_failed = true;
            LOG("Monitor fit: the back buffer can't be drawn to; the desktop window stays stretched");
        }
        if (rtv) {
            context->CopyResource(g_copy, backbuffer);
            // Crop: show the part of the picture with the window's shape. Fit: the whole picture, bars around it.
            float sx = 1, sy = 1;
            if (mode == 1) {
                if (dst > src) sy = src / dst; else sx = dst / src;
                const float z = zoom < 1.0f ? 1.0f : zoom > 4.0f ? 4.0f : zoom;
                sx /= z;
                sy /= z;
            } else {
                if (dst > src) sx = dst / src; else sy = src / dst;
            }
            const float map[4] = {sx, sy, 0, 0};
            ID3DDeviceContextState* gameState = nullptr;
            context1->SwapDeviceContextState(g_state, &gameState);
            context1->UpdateSubresource(g_params, 0, nullptr, map, 0, 0);
            D3D11_VIEWPORT vp{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
            ID3D11ShaderResourceView* noView = nullptr;
            ID3D11RenderTargetView* noTarget = nullptr;
            context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context1->VSSetShader(g_vs, nullptr, 0);
            context1->PSSetShader(g_ps, nullptr, 0);
            context1->PSSetConstantBuffers(0, 1, &g_params);
            context1->PSSetSamplers(0, 1, &g_sampler);
            context1->PSSetShaderResources(0, 1, &g_copyView);
            context1->RSSetViewports(1, &vp);
            context1->OMSetRenderTargets(1, &rtv, nullptr);
            context1->Draw(3, 0);
            context1->PSSetShaderResources(0, 1, &noView);
            context1->OMSetRenderTargets(1, &noTarget, nullptr);
            context1->SwapDeviceContextState(gameState, nullptr);
            rel(gameState);
            rtv->Release();
        }
    }
    if (context1) context1->Release();
    context->Release();
    device->Release();
}

void shutdown() { release(); }

}  // namespace s4vr::monitorfit
