#include "fsr.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "log.h"

namespace s4vr::fsr {

namespace {

#include "fsr_ffx.inc"

// Two passes over AMD's code: EASU (edge-adaptive upscale, source -> a
// texture at the output size), then RCAS (robust contrast-adaptive sharpen,
// that texture -> the eye image) with the eyelids and the sRGB view handled
// as sharpen.cpp does.
const char kPrelude[] = R"HLSL(
#define A_GPU 1
#define A_HLSL 1
)HLSL";

const char kMiddle[] = R"HLSL(
Texture2D<float4> source : register(t0);  // EASU: the game frame; RCAS: the upscaled picture
SamplerState linearClamp : register(s0);

cbuffer Params : register(b0) {
    uint4 con0, con1, con2, con3;  // FsrEasuCon
    uint4 rcas;                    // FsrRcasCon
    float outputLinear;            // 1 when the target is an sRGB view (hardware re-encodes)
    float closure;                 // eyelids: 0 open .. 1 shut
    float dark;                    // 0 the warm glow through shut lids .. 1 black
    float pad;
};

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return source.GatherRed(linearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return source.GatherGreen(linearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return source.GatherBlue(linearClamp, p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return source.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)HLSL";

const char kPasses[] = R"HLSL(
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

float4 psEasu(VsOut i) : SV_Target {
    AF3 c;
    FsrEasuF(c, AU2(i.position.xy), con0, con1, con2, con3);
    return float4(c, 1);
}

float3 toLinear(float3 c) {
    float3 low = c / 12.92, high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(high, low, step(c, 0.04045));
}

float4 psRcas(VsOut i) : SV_Target {
    AU2 p = AU2(i.position.xy);
    AF1 r, g, b;
    FsrRcasF(r, g, b, p, rcas);
    float3 result = saturate(float3(r, g, b));

    if (closure > 0) {
        // Eyelids, as sharpen.cpp draws them.
        uint width, height;
        source.GetDimensions(width, height);
        float2 uv = (float2(p) + 0.5) / float2(width, height);
        float c = smoothstep(0.0, 1.0, closure);
        float dx = uv.x * 2.0 - 1.0;
        float upper = -0.22 + c * (0.80 + 0.18 * dx * dx);
        float lower = 1.22 - c * (0.70 + 0.12 * dx * dx);
        float rim = 0.035 + 0.03 * c;
        float lid = saturate(smoothstep(upper + rim, upper - rim, uv.y) + smoothstep(lower - rim, lower + rim, uv.y));
        float3 glow = float3(0.16, 0.045, 0.03) * (1.0 - dark);
        result = lerp(result * (1.0 - 0.45 * c), glow, lid);
        result *= 1.0 - dark;
    }
    if (outputLinear > 0.5) result = toLinear(result);
    return float4(result, 1);
}
)HLSL";

struct Params {
    uint32_t con0[4], con1[4], con2[4], con3[4];
    uint32_t rcas[4];
    float outputLinear, closure, dark, pad;
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

UINT g_render[2] = {}, g_output[2] = {};
bool g_failed = false;  // gave up for this session: no upscaling from now on
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psEasu = nullptr;
ID3D11PixelShader* g_psRcas = nullptr;
ID3D11Buffer* g_params = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3DDeviceContextState* g_state = nullptr;  // our own clean pipeline state
ID3D11Texture2D* g_copy = nullptr;           // the frame, readable by the shader
ID3D11ShaderResourceView* g_copyView = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc{};
ID3D11Texture2D* g_mid = nullptr;  // upscaled, before sharpening
ID3D11ShaderResourceView* g_midView = nullptr;
ID3D11RenderTargetView* g_midTarget = nullptr;
D3D11_TEXTURE2D_DESC g_midDesc{};
Params g_lastParams{};

// Graphics card time of the two passes, logged every 30 s.
struct Timing {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool pending = false;
};
constexpr int kTimings = 4;
Timing g_timing[kTimings];
int g_timingNext = 0;
double g_gpuMs = 0;
uint64_t g_timed = 0, g_drawn = 0;
ULONGLONG g_lastLog = 0;

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

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// ffx_fsr1.h FsrEasuCon, for a whole-texture input.
void easuCon(Params& p, float inW, float inH, float outW, float outH) {
    p.con0[0] = bits(inW / outW);
    p.con0[1] = bits(inH / outH);
    p.con0[2] = bits(0.5f * inW / outW - 0.5f);
    p.con0[3] = bits(0.5f * inH / outH - 0.5f);
    p.con1[0] = bits(1.0f / inW);
    p.con1[1] = bits(1.0f / inH);
    p.con1[2] = bits(1.0f / inW);
    p.con1[3] = bits(-1.0f / inH);
    p.con2[0] = bits(-1.0f / inW);
    p.con2[1] = bits(2.0f / inH);
    p.con2[2] = bits(1.0f / inW);
    p.con2[3] = bits(2.0f / inH);
    p.con3[0] = bits(0.0f / inW);
    p.con3[1] = bits(4.0f / inH);
    p.con3[2] = p.con3[3] = 0;
}

// ffx_fsr1.h FsrRcasCon (the half-precision word is unused by the float path).
void rcasCon(Params& p, float stops) {
    p.rcas[0] = bits(std::exp2(-stops));
    p.rcas[1] = p.rcas[2] = p.rcas[3] = 0;
}

ID3DBlob* compile(pD3DCompile compileFn, const std::string& code, const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compileFn(code.data(), code.size(), "fsr", nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) {
        LOG("FSR: %s failed to compile (0x%08lx): %.600s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        safeRelease(blob);
    }
    safeRelease(errors);
    return blob;
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        LOG("FSR: d3dcompiler_47.dll not available; no upscaling");
        return false;
    }
    std::string code = std::string("/*\n") + kFfxLicence + "*/\n" + kPrelude + kFfxA + kMiddle + kFfxFsr1 + kPasses;
    ID3DBlob* vs = compile(compileFn, code, "vsMain", "vs_5_0");
    ID3DBlob* easu = compile(compileFn, code, "psEasu", "ps_5_0");
    ID3DBlob* rcas = compile(compileFn, code, "psRcas", "ps_5_0");
    bool ok = vs && easu && rcas &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &g_psEasu)) &&
              SUCCEEDED(device->CreatePixelShader(rcas->GetBufferPointer(), rcas->GetBufferSize(), nullptr, &g_psRcas));
    safeRelease(vs);
    safeRelease(easu);
    safeRelease(rcas);
    if (!ok) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params))) return false;

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&s, &g_sampler))) return false;

    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC t{D3D11_QUERY_TIMESTAMP, 0};
    for (Timing& timing : g_timing)
        if (FAILED(device->CreateQuery(&q, &timing.disjoint)) || FAILED(device->CreateQuery(&t, &timing.begin)) ||
            FAILED(device->CreateQuery(&t, &timing.end))) {
            safeRelease(timing.disjoint);  // untimed
        }

    // A separate pipeline state, swapped in for our draw and out again, so the
    // game's own bindings are restored exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        LOG("FSR: no D3D11.1 device; no upscaling");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    if (FAILED(hr)) {
        LOG("FSR: CreateDeviceContextState failed (0x%08lx); no upscaling", static_cast<unsigned long>(hr));
        return false;
    }
    LOG("FSR: ready");
    return true;
}

bool ensureTexture(ID3D11Device* device, ID3D11Texture2D*& texture, ID3D11ShaderResourceView*& view,
                   ID3D11RenderTargetView** target, D3D11_TEXTURE2D_DESC& have, UINT width, UINT height,
                   DXGI_FORMAT format) {
    if (texture && have.Width == width && have.Height == height && have.Format == format) return true;
    if (target) safeRelease(*target);
    safeRelease(view);
    safeRelease(texture);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(device->CreateTexture2D(&d, nullptr, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture, nullptr, &view)) ||
        (target && FAILED(device->CreateRenderTargetView(texture, nullptr, target)))) {
        LOG("FSR: could not create a %ux%u format %d texture", width, height, static_cast<int>(format));
        if (target) safeRelease(*target);
        safeRelease(view);
        safeRelease(texture);
        have = {};
        return false;
    }
    have = d;
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

void collectTimings(ID3D11DeviceContext* context) {
    for (Timing& t : g_timing) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
        UINT64 a, b;
        if (context->GetData(t.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.begin, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.end, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (d.Disjoint || !d.Frequency || b < a) continue;
        g_gpuMs += static_cast<double>(b - a) * 1000.0 / static_cast<double>(d.Frequency);
        ++g_timed;
    }
    ULONGLONG now = GetTickCount64();
    if (!g_lastLog) g_lastLog = now;
    if (now - g_lastLog >= 30000 && g_drawn) {
        LOG("FSR: last 30 s, %llu eye pictures upscaled %ux%u -> %ux%u, graphics card %.2f ms each",
            static_cast<unsigned long long>(g_drawn), g_copyDesc.Width, g_copyDesc.Height, g_midDesc.Width,
            g_midDesc.Height, g_timed ? g_gpuMs / static_cast<double>(g_timed) : 0.0);
        g_lastLog = now;
        g_drawn = g_timed = 0;
        g_gpuMs = 0;
    }
}

}  // namespace

void setSizes(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight) {
    const bool on = renderWidth && renderHeight && outputWidth > renderWidth && outputHeight > renderHeight;
    g_render[0] = on ? renderWidth : 0;
    g_render[1] = on ? renderHeight : 0;
    g_output[0] = on ? outputWidth : 0;
    g_output[1] = on ? outputHeight : 0;
    if (on) LOG("FSR: game draws %ux%u, eyes go out at %ux%u", renderWidth, renderHeight, outputWidth, outputHeight);
}

D3D11_TEXTURE2D_DESC eyeDesc(const D3D11_TEXTURE2D_DESC& source) {
    D3D11_TEXTURE2D_DESC d = source;
    if (g_failed || !g_render[0]) return d;
    // The game can make its picture a little other than asked (the window's
    // height when that is taller): keep the same ratio. Far off (exclusive
    // fullscreen, a size from before the last change): as it is.
    const float sx = static_cast<float>(g_output[0]) / static_cast<float>(g_render[0]);
    const float sy = static_cast<float>(g_output[1]) / static_cast<float>(g_render[1]);
    const float offW = static_cast<float>(source.Width) / static_cast<float>(g_render[0]);
    const float offH = static_cast<float>(source.Height) / static_cast<float>(g_render[1]);
    if (offW < 0.8f || offW > 1.25f || offH < 0.8f || offH > 1.25f) return d;
    d.Width = std::min(4096u, static_cast<UINT>(std::lround(source.Width * sx)));
    d.Height = std::min(4096u, static_cast<UINT>(std::lround(source.Height * sy)));
    return d;
}

bool draw(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& sourceDesc, ID3D11Texture2D* target,
          DXGI_FORMAT targetFormat, UINT width, UINT height, float strength, float closure, float dark) {
    if (g_failed) return false;
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
    const DXGI_FORMAT format = plainFormat(sourceDesc.Format);
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        ensureTexture(device, g_copy, g_copyView, nullptr, g_copyDesc, sourceDesc.Width, sourceDesc.Height, format) &&
        ensureTexture(device, g_mid, g_midView, &g_midTarget, g_midDesc, width, height, format)) {
        ID3D11RenderTargetView* rtv = targetView(device, target, targetFormat);
        if (rtv) {
            collectTimings(context);
            Timing* timing = &g_timing[g_timingNext];
            if (timing->pending || !timing->disjoint) {
                timing = nullptr;  // still out: this one goes untimed
            } else {
                g_timingNext = (g_timingNext + 1) % kTimings;
                context->Begin(timing->disjoint);
                context->End(timing->begin);
            }
            // The frame into a texture the shader can read (resolving MSAA).
            if (sourceDesc.SampleDesc.Count > 1)
                context->ResolveSubresource(g_copy, 0, source, 0, format);
            else
                context->CopyResource(g_copy, source);

            Params params{};
            easuCon(params, static_cast<float>(sourceDesc.Width), static_cast<float>(sourceDesc.Height),
                    static_cast<float>(width), static_cast<float>(height));
            // Sharpness 1 -> 0 stops (AMD's strongest), 0 -> 2 stops (a quarter).
            rcasCon(params, 2.0f * (1.0f - std::clamp(strength, 0.0f, 1.0f)));
            params.outputLinear = isSrgb(targetFormat) ? 1.0f : 0.0f;
            params.closure = closure;
            params.dark = dark;

            ID3DDeviceContextState* gameState = nullptr;
            context1->SwapDeviceContextState(g_state, &gameState);
            if (std::memcmp(&params, &g_lastParams, sizeof(params)) != 0) {
                context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
                g_lastParams = params;
            }
            D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            ID3D11ShaderResourceView* noView = nullptr;
            ID3D11RenderTargetView* noTarget = nullptr;
            context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context1->VSSetShader(g_vs, nullptr, 0);
            context1->PSSetConstantBuffers(0, 1, &g_params);
            context1->PSSetSamplers(0, 1, &g_sampler);
            context1->RSSetViewports(1, &viewport);
            // 1. Upscale.
            context1->PSSetShader(g_psEasu, nullptr, 0);
            context1->PSSetShaderResources(0, 1, &g_copyView);
            context1->OMSetRenderTargets(1, &g_midTarget, nullptr);
            context1->Draw(3, 0);
            // 2. Sharpen (and eyelids) into the eye image.
            context1->OMSetRenderTargets(1, &noTarget, nullptr);
            context1->PSSetShader(g_psRcas, nullptr, 0);
            context1->PSSetShaderResources(0, 1, &g_midView);
            context1->OMSetRenderTargets(1, &rtv, nullptr);
            context1->Draw(3, 0);
            // Leave nothing of ours bound, then hand the game its state back.
            context1->PSSetShaderResources(0, 1, &noView);
            context1->OMSetRenderTargets(1, &noTarget, nullptr);
            context1->SwapDeviceContextState(gameState, nullptr);
            safeRelease(gameState);
            if (timing) {
                context->End(timing->end);
                context->End(timing->disjoint);
                timing->pending = true;
            }
            ++g_drawn;
            drawn = true;
        }
    }
    safeRelease(context1);
    context->Release();
    if (!drawn) {
        LOG("FSR: draw unavailable; no upscaling");
        g_failed = true;
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
    for (Timing& t : g_timing) {
        safeRelease(t.disjoint);
        safeRelease(t.begin);
        safeRelease(t.end);
        t.pending = false;
    }
    safeRelease(g_midTarget);
    safeRelease(g_midView);
    safeRelease(g_mid);
    safeRelease(g_copyView);
    safeRelease(g_copy);
    safeRelease(g_state);
    safeRelease(g_sampler);
    safeRelease(g_params);
    safeRelease(g_psRcas);
    safeRelease(g_psEasu);
    safeRelease(g_vs);
    g_copyDesc = g_midDesc = {};
    g_lastParams = {};
    g_device = nullptr;
}

}  // namespace s4vr::fsr
