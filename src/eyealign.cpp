#include "eyealign.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>

#include "log.h"

namespace s4vr::eyealign {

namespace {

const char kShader[] = R"HLSL(
// The older eye's image, turned to where that eye looks now. For each pixel
// of the target: its view direction, turned into the kept image's view, and
// the kept image read where that direction lands (its edge where it falls
// outside).
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);

cbuffer Params : register(b0) {
    float4 row0;  // target view direction -> kept image view direction
    float4 row1;
    float4 row2;
    float4 tangents;  // tan of the field of view: left, right, up, down
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vsMain(uint id : SV_VertexID) {
    // One triangle that covers the whole target.
    float2 uv = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 psMain(VsOut i) : SV_Target {
    float3 d = float3(lerp(tangents.x, tangents.y, i.uv.x), lerp(tangents.z, tangents.w, i.uv.y), -1);
    float3 s = float3(dot(row0.xyz, d), dot(row1.xyz, d), dot(row2.xyz, d));
    float2 t = s.xy / max(-s.z, 1e-4);
    float2 uv = float2((t.x - tangents.x) / (tangents.y - tangents.x), (tangents.z - t.y) / (tangents.z - tangents.w));
    return float4(source.SampleLevel(linearClamp, uv, 0).rgb, 1);
}
)HLSL";

struct Params {
    float rows[3][4];
    float tangents[4];
};

struct Kept {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool valid = false;
};

bool g_failed = false;  // gave up for this session
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_params = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3DDeviceContextState* g_state = nullptr;  // our own clean pipeline state
Kept g_kept[2];

// Render target views for the eye swapchain images, which the runtime reuses.
struct TargetView {
    ID3D11Texture2D* texture;
    DXGI_FORMAT format;
    ID3D11RenderTargetView* view;
};
TargetView g_targets[16] = {};

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

ID3DBlob* compile(pD3DCompile compileFn, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compileFn(kShader, sizeof(kShader) - 1, "eyealign", nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Eye align: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        safeRelease(code);
    }
    safeRelease(errors);
    return code;
}

void releaseKept(Kept& k) {
    safeRelease(k.view);
    safeRelease(k.texture);
    k = Kept();
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        LOG("Eye align: d3dcompiler_47.dll not available; off");
        return false;
    }
    ID3DBlob* vs = compile(compileFn, "vsMain", "vs_5_0");
    ID3DBlob* ps = compile(compileFn, "psMain", "ps_5_0");
    bool ok = vs && ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    safeRelease(vs);
    safeRelease(ps);
    if (!ok) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

    // A separate pipeline state, swapped in for our draw and out again, so the
    // game's own bindings are restored exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        LOG("Eye align: no D3D11.1 device; off");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    if (FAILED(hr)) {
        LOG("Eye align: CreateDeviceContextState failed (0x%08lx); off", static_cast<unsigned long>(hr));
        return false;
    }
    LOG("Eye align: ready (the older eye is turned to the newest head pose every frame)");
    return true;
}

// The device the images live on; set up on first use.
bool ready(ID3D11Texture2D* texture) {
    if (g_failed) return false;
    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    if (device != g_device) {
        release();
        g_device = device;
        if (!init(device)) {
            release();
            g_failed = true;
            device->Release();
            return false;
        }
    }
    device->Release();  // g_device is only compared, never used after the device goes away
    return true;
}

ID3D11RenderTargetView* targetView(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format) {
    for (TargetView& t : g_targets)
        if (t.texture == texture && t.format == format) return t.view;
    TargetView* slot = nullptr;
    for (TargetView& t : g_targets)
        if (!t.texture) {
            slot = &t;
            break;
        }
    if (!slot) {  // runtime made new images: start over
        forgetTargets();
        slot = &g_targets[0];
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = format;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateRenderTargetView(texture, &rtv, &slot->view))) return nullptr;
    slot->texture = texture;
    slot->format = format;
    return slot->view;
}

}  // namespace

bool keep(int eye, ID3D11Texture2D* image, DXGI_FORMAT format, UINT width, UINT height) {
    if (eye < 0 || eye > 1 || !image || !ready(image)) return false;
    Kept& k = g_kept[eye];
    if (!k.texture || k.width != width || k.height != height || k.format != format) {
        releaseKept(k);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;  // the swapchain's: sRGB images are read back as linear and written as sRGB
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device* device = nullptr;
        image->GetDevice(&device);
        bool made = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &k.texture)) &&
                    SUCCEEDED(device->CreateShaderResourceView(k.texture, nullptr, &k.view));
        device->Release();
        if (!made) {
            LOG("Eye align: could not keep a %ux%u format %d image; off", width, height, static_cast<int>(format));
            releaseKept(k);
            g_failed = true;
            return false;
        }
        k.width = width;
        k.height = height;
        k.format = format;
    }
    ID3D11Device* device = nullptr;
    image->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    context->CopyResource(k.texture, image);
    context->Release();
    device->Release();
    k.valid = true;
    return true;
}

void forgetTargets() {
    for (TargetView& t : g_targets) {
        safeRelease(t.view);
        t.texture = nullptr;
    }
}

bool has(int eye) { return eye >= 0 && eye <= 1 && g_kept[eye].valid; }

ID3D11ShaderResourceView* keptView(int eye, UINT& width, UINT& height) {
    if (!has(eye) || !g_kept[eye].view) return nullptr;
    width = g_kept[eye].width;
    height = g_kept[eye].height;
    g_kept[eye].view->AddRef();
    return g_kept[eye].view;
}

void forget(int eye) {
    if (eye >= 0 && eye <= 1) g_kept[eye].valid = false;
}

bool draw(int eye, ID3D11Texture2D* target, DXGI_FORMAT format, const float rotation[9], const float tangents[4]) {
    if (!has(eye) || !target || !ready(target)) return false;
    const Kept& k = g_kept[eye];
    ID3D11Device* device = nullptr;
    target->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11DeviceContext1* context1 = nullptr;
    bool drawn = false;
    ID3D11RenderTargetView* rtv = targetView(device, target, format);
    if (rtv && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
        Params params{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) params.rows[r][c] = rotation[r * 3 + c];
        std::memcpy(params.tangents, tangents, sizeof(params.tangents));

        ID3DDeviceContextState* gameState = nullptr;
        context1->SwapDeviceContextState(g_state, &gameState);
        context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(k.width), static_cast<float>(k.height), 0, 1};
        context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context1->VSSetShader(g_vs, nullptr, 0);
        context1->PSSetShader(g_ps, nullptr, 0);
        context1->PSSetShaderResources(0, 1, &k.view);
        context1->PSSetSamplers(0, 1, &g_sampler);
        context1->PSSetConstantBuffers(0, 1, &g_params);
        context1->RSSetViewports(1, &viewport);
        context1->OMSetRenderTargets(1, &rtv, nullptr);
        context1->Draw(3, 0);
        // Leave nothing of ours bound, then hand the game its state back.
        ID3D11ShaderResourceView* noView = nullptr;
        ID3D11RenderTargetView* noTarget = nullptr;
        context1->PSSetShaderResources(0, 1, &noView);
        context1->OMSetRenderTargets(1, &noTarget, nullptr);
        context1->SwapDeviceContextState(gameState, nullptr);
        safeRelease(gameState);
        drawn = true;
    }
    safeRelease(context1);
    context->Release();
    device->Release();
    return drawn;
}

void release() {
    forgetTargets();
    releaseKept(g_kept[0]);
    releaseKept(g_kept[1]);
    safeRelease(g_state);
    safeRelease(g_sampler);
    safeRelease(g_params);
    safeRelease(g_ps);
    safeRelease(g_vs);
    g_device = nullptr;
}

}  // namespace s4vr::eyealign
