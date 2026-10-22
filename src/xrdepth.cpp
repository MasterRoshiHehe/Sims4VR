#include "xrdepth.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include "log.h"

namespace s4vr::xrdepth {

namespace {

const char kShader[] = R"HLSL(
#ifdef MS
Texture2DMS<float> depthTex : register(t0);
#else
Texture2D<float> depthTex : register(t0);
#endif
cbuffer Params : register(b0) {
    float2 scale;  // scene depth pixels per target pixel
    float2 pad;
};

float4 vsMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float psMain(float4 position : SV_Position) : SV_Depth {
    int2 p = int2(position.xy * scale);
#ifdef MS
    return depthTex.Load(p, 0);
#else
    return depthTex.Load(int3(p, 0));
#endif
}
)HLSL";

struct Params {
    float scale[2];
    float pad[2];
};

ID3D11Device* g_device = nullptr;  // not held: the shaders below belong to it
bool g_failed = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps[2] = {};  // plain, MSAA
ID3D11Buffer* g_params = nullptr;
ID3D11DepthStencilState* g_always = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3DDeviceContextState* g_state = nullptr;
int g_logs = 0;

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

ID3DBlob* compile(pD3DCompile compileFn, const char* entry, const char* target, bool ms) {
    const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compileFn(kShader, sizeof(kShader) - 1, "xrdepth", ms ? defines : nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Depth layer: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        safeRelease(code);
    }
    safeRelease(errors);
    return code;
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        LOG("Depth layer: d3dcompiler_47.dll not available; off");
        return false;
    }
    ID3DBlob* vs = compile(compileFn, "vsMain", "vs_5_0", false);
    bool ok = vs && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs));
    safeRelease(vs);
    for (int ms = 0; ok && ms < 2; ++ms) {
        ID3DBlob* ps = compile(compileFn, "psMain", "ps_5_0", ms != 0);
        ok = ps && SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps[ms]));
        safeRelease(ps);
    }
    if (!ok) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = FALSE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params)) || FAILED(device->CreateDepthStencilState(&ds, &g_always)) ||
        FAILED(device->CreateRasterizerState(&rs, &g_raster)))
        return false;
    // Our own pipeline state, swapped in and out, so the game's bindings come back exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) return false;
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    return SUCCEEDED(hr);
}

}  // namespace

bool fill(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, UINT samples, UINT srcW, UINT srcH,
          ID3D11Texture2D* target, DXGI_FORMAT format, UINT w, UINT h) {
    if (!context || !depth || !target || !w || !h || !srcW || !srcH || g_failed) return false;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return false;
    device->Release();
    if (device != g_device) {
        release();
        g_device = device;
        if (!init(device)) {
            LOG("Depth layer: shaders could not be made; no depth sent");
            release();
            g_failed = true;
            return false;
        }
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = format;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView* dsv = nullptr;
    if (FAILED(device->CreateDepthStencilView(target, &dv, &dsv))) {
        if (g_logs++ < 5) LOG("Depth layer: no depth view for the swapchain image (format %d)", static_cast<int>(format));
        return false;
    }
    ID3D11DeviceContext1* context1 = nullptr;
    if (FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
        dsv->Release();
        return false;
    }
    ID3DDeviceContextState* previous = nullptr;
    context1->SwapDeviceContextState(g_state, &previous);
    const Params p{{float(srcW) / float(w), float(srcH) / float(h)}, {0, 0}};
    context->UpdateSubresource(g_params, 0, nullptr, &p, 0, 0);
    const D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_raster);
    context->OMSetRenderTargets(0, nullptr, dsv);
    context->OMSetDepthStencilState(g_always, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs, nullptr, 0);
    context->PSSetShader(g_ps[samples > 1 ? 1 : 0], nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_params);
    context->PSSetShaderResources(0, 1, &depth);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context1->SwapDeviceContextState(previous, nullptr);
    if (previous) previous->Release();
    context1->Release();
    dsv->Release();
    if (g_logs < 5 && ++g_logs)
        LOG("Depth layer: scene depth %ux%u (%u samples) written into the headset's %ux%u depth image", srcW, srcH,
            samples, w, h);
    return true;
}

void release() {
    safeRelease(g_vs);
    safeRelease(g_ps[0]);
    safeRelease(g_ps[1]);
    safeRelease(g_params);
    safeRelease(g_always);
    safeRelease(g_raster);
    safeRelease(g_state);
    g_device = nullptr;
}

}  // namespace s4vr::xrdepth
