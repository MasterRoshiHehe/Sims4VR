#include "sharpen.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>

#include "log.h"

namespace s4vr::sharpen {

namespace {

const char kShader[] = R"HLSL(
// Contrast-adaptive sharpening for the eye images. Algorithm after AMD
// FidelityFX CAS (MIT): each pixel is sharpened against its four
// neighbours, less where local contrast is already high, so edges get
// crisper without halos or noise.
Texture2D<float4> source : register(t0);

cbuffer Params : register(b0) {
    float sharpness;     // 0..1 (0 = a plain copy)
    float outputLinear;  // 1 when the target is an sRGB view (hardware re-encodes)
    float closure;       // eyelids: 0 open .. 1 shut
    float dark;          // 0 the warm glow through shut lids .. 1 black
};

struct VsOut {
    float4 position : SV_Position;
};

VsOut vsMain(uint id : SV_VertexID) {
    // One triangle that covers the whole target.
    float2 uv = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 tap(int2 p, int2 size) {
    return source.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb;
}

float3 toLinear(float3 c) {
    float3 low = c / 12.92, high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(high, low, step(c, 0.04045));
}

float4 psMain(VsOut i) : SV_Target {
    uint width, height;
    source.GetDimensions(width, height);
    int2 size = int2(width, height);
    int2 p = int2(i.position.xy);
    //  a b c
    //  d e f
    //  g h k
    float3 a = tap(p + int2(-1, -1), size), b = tap(p + int2(0, -1), size), c = tap(p + int2(1, -1), size);
    float3 d = tap(p + int2(-1, 0), size), e = tap(p, size), f = tap(p + int2(1, 0), size);
    float3 g = tap(p + int2(-1, 1), size), h = tap(p + int2(0, 1), size), k = tap(p + int2(1, 1), size);

    // Soft min and max over the cross plus the corners.
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));

    // How much headroom there is to sharpen without clipping.
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
    float3 weight = amp * peak;
    float3 result = saturate(((b + d + f + h) * weight + e) / (4.0 * weight + 1.0));
    if (sharpness <= 0) result = e;

    if (closure > 0) {
        // Upper and lower lids, curved like an eye opening, meeting a little
        // below the middle; a soft rim where the lashes are.
        float2 uv = (float2(p) + 0.5) / float2(size);
        float c = smoothstep(0.0, 1.0, closure);
        float dx = uv.x * 2.0 - 1.0;
        float upper = -0.22 + c * (0.80 + 0.18 * dx * dx);
        float lower = 1.22 - c * (0.70 + 0.12 * dx * dx);
        float rim = 0.035 + 0.03 * c;
        float lid = saturate(smoothstep(upper + rim, upper - rim, uv.y) + smoothstep(lower - rim, lower + rim, uv.y));
        // What still shows dims as the eyes get heavy; the lids glow a deep
        // red-brown (light through skin) that fades out.
        float3 glow = float3(0.16, 0.045, 0.03) * (1.0 - dark);
        result = lerp(result * (1.0 - 0.45 * c), glow, lid);
        result *= 1.0 - dark;
    }
    if (outputLinear > 0.5) result = toLinear(result);
    return float4(result, 1);
}
)HLSL";

struct Params {
    float sharpness;
    float outputLinear;
    float closure;
    float dark;
};

bool g_failed = false;  // gave up for this session; plain copies from now on
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_params = nullptr;
ID3DDeviceContextState* g_state = nullptr;  // our own clean pipeline state
ID3D11Texture2D* g_copy = nullptr;           // the frame, readable by the shader
ID3D11ShaderResourceView* g_copyView = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc{};
Params g_lastParams{-1, -1, -1, -1};

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

DXGI_FORMAT plainFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

bool isSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

ID3DBlob* compile(pD3DCompile compileFn, const char* entry, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compileFn(kShader, sizeof(kShader) - 1, "sharpen", nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Sharpen: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
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
        LOG("Sharpen: d3dcompiler_47.dll not available; plain copies");
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

    // A separate pipeline state, swapped in for our draw and out again, so the
    // game's own bindings are restored exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        LOG("Sharpen: no D3D11.1 device; plain copies");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    if (FAILED(hr)) {
        LOG("Sharpen: CreateDeviceContextState failed (0x%08lx); plain copies", static_cast<unsigned long>(hr));
        return false;
    }
    LOG("Sharpen: ready");
    return true;
}

bool ensureCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) {
    DXGI_FORMAT format = plainFormat(source.Format);
    if (g_copy && g_copyDesc.Width == source.Width && g_copyDesc.Height == source.Height && g_copyDesc.Format == format)
        return true;
    safeRelease(g_copyView);
    safeRelease(g_copy);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = source.Width;
    d.Height = source.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &g_copy)) ||
        FAILED(device->CreateShaderResourceView(g_copy, nullptr, &g_copyView))) {
        LOG("Sharpen: could not create a %ux%u format %d copy", d.Width, d.Height, static_cast<int>(format));
        safeRelease(g_copy);
        return false;
    }
    g_copyDesc = d;
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
        for (TargetView& t : g_targets) {
            safeRelease(t.view);
            t.texture = nullptr;
        }
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

bool draw(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& sourceDesc, ID3D11Texture2D* target,
          DXGI_FORMAT targetFormat, float strength, float closure, float dark) {
    if (g_failed || (strength <= 0 && closure <= 0)) return false;
    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
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

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11DeviceContext1* context1 = nullptr;
    bool drawn = false;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        ensureCopy(device, sourceDesc)) {
        ID3D11RenderTargetView* rtv = targetView(device, target, targetFormat);
        if (rtv) {
            // The frame into a texture the shader can read (resolving MSAA).
            if (sourceDesc.SampleDesc.Count > 1)
                context->ResolveSubresource(g_copy, 0, source, 0, g_copyDesc.Format);
            else
                context->CopyResource(g_copy, source);

            Params params{strength, isSrgb(targetFormat) ? 1.0f : 0.0f, closure, dark};
            ID3DDeviceContextState* gameState = nullptr;
            context1->SwapDeviceContextState(g_state, &gameState);
            if (std::memcmp(&params, &g_lastParams, sizeof(params)) != 0) {
                context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
                g_lastParams = params;
            }
            D3D11_VIEWPORT viewport{0, 0, static_cast<float>(sourceDesc.Width), static_cast<float>(sourceDesc.Height),
                                    0, 1};
            context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context1->VSSetShader(g_vs, nullptr, 0);
            context1->PSSetShader(g_ps, nullptr, 0);
            context1->PSSetShaderResources(0, 1, &g_copyView);
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
    }
    safeRelease(context1);
    context->Release();
    if (!drawn) {
        static bool logged = false;
        if (!logged) LOG("Sharpen: draw unavailable; plain copies");
        logged = true;
    }
    return drawn;
}

void forgetTargets() {
    for (TargetView& t : g_targets) {
        safeRelease(t.view);
        t.texture = nullptr;
    }
}

void release() {
    forgetTargets();
    safeRelease(g_copyView);
    safeRelease(g_copy);
    safeRelease(g_state);
    safeRelease(g_params);
    safeRelease(g_ps);
    safeRelease(g_vs);
    g_copyDesc = {};
    g_lastParams = {-1, -1, -1, -1};
    g_device = nullptr;
}

}  // namespace s4vr::sharpen
