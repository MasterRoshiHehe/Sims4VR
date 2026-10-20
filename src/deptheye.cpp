#include "deptheye.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "log.h"
#include "nearplane.h"

namespace s4vr::deptheye {

namespace {

constexpr uint32_t kIterations = 64;  // steps each pixel walks along its ray (STEPS in the shader)

const char kShader[] = R"HLSL(
// The fresh eye's picture, as the swapchain shows it (an sRGB view reads linear).
Texture2D<float4> source : register(t0);
#ifdef MS
Texture2DMS<float> depthTex : register(t1);
#define DEPTH(p) depthTex.Load(p, 0)
#else
Texture2D<float> depthTex : register(t1);
#define DEPTH(p) depthTex.Load(int3(p, 0))
#endif
SamplerState linearClamp : register(s0);

cbuffer Params : register(b0) {
    float4 row0;  // a source view point (x right, y up, z ahead) to the target view; w: translation
    float4 row1;
    float4 row2;
    float4 inv0;  // the target view to the source view
    float4 inv1;
    float4 inv2;
    float4 srcTan;  // left, right, up, down
    float4 dstTan;
    float nearZ, farZ;
    uint reversed, iterations;
    uint width, height, samples, pad1;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vsMain(uint id : SV_VertexID) {
    // One triangle that covers the whole target.
    float2 uv = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.uv = uv;
    return o;
}

float viewZ(float d) {
    float z = reversed ? nearZ * farZ / (nearZ + d * (farZ - nearZ)) : nearZ * farZ / (farZ - d * (farZ - nearZ));
    return min(z, farZ);
}

float2 project(float3 v, float4 tan) {
    float2 a = v.xy / max(v.z, 1e-4);
    return float2((a.x - tan.x) / (tan.y - tan.x), (tan.z - a.y) / (tan.z - tan.w));
}

float3 apply(float4 r0, float4 r1, float4 r2, float3 v) {
    return float3(dot(r0.xyz, v) + r0.w, dot(r1.xyz, v) + r1.w, dot(r2.xyz, v) + r2.w);
}

uint2 texel(float2 uv) {
    return clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width - 1, height - 1));
}

float3 at(float2 uv, float z) {
    return apply(row0, row1, row2, float3(lerp(srcTan.x, srcTan.y, uv.x) * z, lerp(srcTan.z, srcTan.w, uv.y) * z, z));
}

// The source point seen through source pixel uv, in the target view.
float3 seen(float2 uv) {
    return at(uv, viewZ(DEPTH(texel(uv))));
}

// Each target pixel walks its own ray out from the eye, in steps even in
// 1/distance (even in picture shift), and looks at where each step lands in
// the fresh picture. The first step that goes behind the surface seen there,
// with that surface between this step and the last, is what this eye sees:
// refined by halving, and that spot of the fresh picture is its colour. The
// first hit is the nearest one, so a near thing's edge is never replaced by
// the wall behind it. A ray that only passes behind near things sees
// something the fresh eye can't: it takes the farthest surface met on the
// way, stepped clear of the near thing's edge.
#define STEPS 64
float4 psMain(VsOut i) : SV_Target {
    float3 ray = float3(lerp(dstTan.x, dstTan.y, i.uv.x), lerp(dstTan.z, dstTan.w, i.uv.y), 1);
    float wNear = 1.0 / max(nearZ, 0.08), wFar = 1.0 / farZ;
    float zPrev = 1.0 / wNear;
    bool hit = false;
    float zHit = farZ;
    float farthest = -1, zFill = farZ;
    [loop] for (uint k = 1; k <= STEPS; ++k) {
        float z = 1.0 / lerp(wNear, wFar, k / float(STEPS));
        float2 u = project(apply(inv0, inv1, inv2, ray * z), srcTan);
        float d = viewZ(DEPTH(texel(saturate(u))));
        if (d > farthest) {
            farthest = d;
            zFill = z;
        }
        if (z >= d && d >= zPrev * 0.97) {
            // Crossed the surface between zPrev and z: halve onto it.
            float lo = zPrev, hi = z;
            [unroll] for (uint h = 0; h < 5; ++h) {
                float m = 0.5 * (lo + hi);
                float2 um = project(apply(inv0, inv1, inv2, ray * m), srcTan);
                if (m >= viewZ(DEPTH(texel(saturate(um))))) hi = m; else lo = m;
            }
            zHit = hi;
            hit = true;
            break;
        }
        zPrev = z;
    }
    float2 us;
    if (hit) {
        us = project(apply(inv0, inv1, inv2, ray * zHit), srcTan);
    } else {
        us = project(apply(inv0, inv1, inv2, ray * zFill), srcTan);
        float2 nearSide = project(apply(inv0, inv1, inv2, ray * (1.0 / wNear)), srcTan);
        float2 away = us - nearSide;
        float len = length(away * float2(width, height));
        if (len > 1e-3) us += away / len * 2.0;
    }
    return float4(source.SampleLevel(linearClamp, saturate(us), 0).rgb, 1);
}
)HLSL";

struct Params {
    float rows[3][4];
    float inverse[3][4];
    float srcTan[4];
    float dstTan[4];
    float nearZ, farZ;
    uint32_t reversed, iterations;
    uint32_t width, height, samples, pad1;
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

bool g_failed = false;
const char* g_failure = nullptr;  // why the last draw made nothing
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps[2] = {};  // single-sampled depth, MSAA depth
ID3D11Buffer* g_params = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3DDeviceContextState* g_state = nullptr;

// Our depth buffer, sized to the eye pictures.
UINT g_width = 0, g_height = 0;
ID3D11Texture2D* g_depth = nullptr;
ID3D11DepthStencilView* g_depthView = nullptr;

struct TargetView {
    ID3D11Texture2D* texture;
    DXGI_FORMAT format;
    ID3D11RenderTargetView* view;
    uint64_t used;
};
constexpr int kTargets = 16;
TargetView g_targets[kTargets] = {};
uint64_t g_targetUse = 0;

// What it costs the graphics card: timestamps read back a few frames later.
struct Timing {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool pending = false;
};
constexpr int kTimings = 8;
Timing g_timing[kTimings];
int g_timingNext = 0;
double g_gpuSum = 0, g_gpuWorst = 0;
uint64_t g_gpuCount = 0;
uint64_t g_drawn = 0, g_noDepth = 0, g_otherSize = 0;
ULONGLONG g_logAt = 0;
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
    HRESULT hr = compileFn(kShader, sizeof(kShader) - 1, "deptheye", ms ? defines : nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Depth eye: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
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
        LOG("Depth eye: d3dcompiler_47.dll not available; off");
        return false;
    }
    ID3DBlob* vs = compile(compileFn, "vsMain", "vs_5_0", false);
    bool ok = vs && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs));
    safeRelease(vs);
    for (int ms = 0; ms < 2 && ok; ++ms) {
        ID3DBlob* ps = compile(compileFn, "psMain", "ps_5_0", ms != 0);
        ok = ps && SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps[ms]));
        safeRelease(ps);
    }
    if (!ok) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // a moved triangle may turn over
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params)) || FAILED(device->CreateSamplerState(&sd, &g_sampler)) ||
        FAILED(device->CreateRasterizerState(&rd, &g_raster)) ||
        FAILED(device->CreateDepthStencilState(&dd, &g_depthState)))
        return false;

    // Our own pipeline state, swapped in for the pass and out again, so the
    // game's bindings come back exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        LOG("Depth eye: no D3D11.1 device; off");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    if (FAILED(hr)) {
        LOG("Depth eye: CreateDeviceContextState failed (0x%08lx); off", static_cast<unsigned long>(hr));
        return false;
    }
    for (Timing& t : g_timing) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device->CreateQuery(&q, &t.disjoint)) || FAILED(device->CreateQuery(&stamp, &t.begin)) ||
            FAILED(device->CreateQuery(&stamp, &t.end))) {
            safeRelease(t.disjoint);
            safeRelease(t.begin);
            safeRelease(t.end);
        }
        t.pending = false;
    }
    LOG("Depth eye: ready");
    return true;
}

bool ready(ID3D11Device* device) {
    if (g_failed) return false;
    if (device != g_device) {
        release();
        g_device = device;
        if (!init(device)) {
            release();
            g_failed = true;
            return false;
        }
    }
    return true;
}

bool ensureDepth(ID3D11Device* device, UINT width, UINT height) {
    if (g_depth && g_width == width && g_height == height) return true;
    safeRelease(g_depthView);
    safeRelease(g_depth);
    g_width = g_height = 0;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &g_depth)) ||
        FAILED(device->CreateDepthStencilView(g_depth, nullptr, &g_depthView))) {
        LOG("Depth eye: could not make a %ux%u depth buffer; off", width, height);
        safeRelease(g_depthView);
        safeRelease(g_depth);
        return false;
    }
    g_width = width;
    g_height = height;
    return true;
}

// The cached view of `texture`, with a reference of the caller's own (Release it).
ID3D11RenderTargetView* targetView(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format) {
    TargetView* slot = nullptr;
    for (TargetView& t : g_targets)
        if (t.texture == texture && t.format == format && t.view) {
            slot = &t;
            break;
        }
    if (!slot) {
        slot = &g_targets[0];
        for (TargetView& t : g_targets) {
            if (!t.texture) {
                slot = &t;
                break;
            }
            if (t.used < slot->used) slot = &t;
        }
        safeRelease(slot->view);
        slot->texture = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = format;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateRenderTargetView(texture, &rtv, &slot->view))) {
            slot->view = nullptr;
            return nullptr;
        }
        slot->texture = texture;
        slot->format = format;
    }
    slot->used = ++g_targetUse;
    slot->view->AddRef();
    return slot->view;
}

void collectTimings(ID3D11DeviceContext* context) {
    for (Timing& t : g_timing) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        UINT64 a = 0, b = 0;
        if (context->GetData(t.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.begin, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(t.end, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (d.Disjoint || !d.Frequency || b < a) continue;
        const double ms = double(b - a) * 1000.0 / double(d.Frequency);
        g_gpuSum += ms;
        g_gpuWorst = std::max(g_gpuWorst, ms);
        ++g_gpuCount;
    }
}

void logNow() {
    ULONGLONG now = GetTickCount64();
    if (!g_logAt) g_logAt = now;
    if (now - g_logAt < 30000 || g_logs >= 120) return;
    ++g_logs;
    LOG("Depth eye: last 30 s, %llu other-eye pictures made, %llu without this frame's depth, %llu of another size; "
        "graphics card %.2f ms each on average, worst %.2f (%llu timed)",
        static_cast<unsigned long long>(g_drawn), static_cast<unsigned long long>(g_noDepth),
        static_cast<unsigned long long>(g_otherSize), g_gpuCount ? g_gpuSum / double(g_gpuCount) : 0.0, g_gpuWorst,
        static_cast<unsigned long long>(g_gpuCount));
    g_drawn = g_noDepth = g_otherSize = 0;
    g_gpuSum = g_gpuWorst = 0;
    g_gpuCount = 0;
    g_logAt = now;
}

}  // namespace

bool draw(ID3D11ShaderResourceView* source, ID3D11Texture2D* target, DXGI_FORMAT format, UINT width, UINT height,
          const float rows[12], const float srcTan[4], const float dstTan[4], float nearZ, float farZ) {
    g_failure = nullptr;
    if (!source || !target || width < 2 || height < 2) {
        g_failure = "no fresh picture";
        return false;
    }
    if (!(nearZ > 0) || !(farZ > nearZ)) {
        g_failure = "the frame's near/far planes unknown";
        return false;
    }
    ID3D11Device* device = nullptr;
    target->GetDevice(&device);
    device->Release();  // g_device is only compared, never used after the device goes away
    if (!ready(device)) {
        g_failure = "the pass couldn't be set up";
        return false;
    }
    logNow();

    UINT samples = 0, depthWidth = 0, depthHeight = 0;
    bool reversed = false;
    // The copy with our own 3D pieces in it (phone3d::setWriteDepth), else the plain one.
    ID3D11ShaderResourceView* depth = nearplane::lateDepth(samples, depthWidth, depthHeight, reversed);
    if (!depth) depth = nearplane::latestDepth(samples, depthWidth, depthHeight, reversed);
    if (!depth) {
        ++g_noDepth;
        g_failure = "no scene depth from this frame";
        return false;
    }
    if (depthWidth != width || depthHeight != height || samples < 1 || samples > 8) {
        ++g_otherSize;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("Depth eye: the scene depth is %ux%u x%u, the eye picture %ux%u; not made", depthWidth, depthHeight,
                samples, width, height);
        }
        depth->Release();
        g_failure = "the scene depth is another size";
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11RenderTargetView* targetRtv = nullptr;
    bool done = false;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        ensureDepth(device, width, height) && (targetRtv = targetView(device, target, format))) {
        collectTimings(context);
        Timing* timing = &g_timing[g_timingNext];
        if (timing->pending || !timing->disjoint) {
            timing = nullptr;
        } else {
            g_timingNext = (g_timingNext + 1) % kTimings;
            context->Begin(timing->disjoint);
            context->End(timing->begin);
        }
        Params params{};
        std::memcpy(params.rows, rows, sizeof(params.rows));
        // The inverse of a rigid move: rotation transposed, translation turned back.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) params.inverse[r][c] = rows[c * 4 + r];
            params.inverse[r][3] = -(rows[0 * 4 + r] * rows[3] + rows[1 * 4 + r] * rows[7] + rows[2 * 4 + r] * rows[11]);
        }
        std::memcpy(params.srcTan, srcTan, sizeof(params.srcTan));
        std::memcpy(params.dstTan, dstTan, sizeof(params.dstTan));
        params.nearZ = nearZ;
        params.farZ = farZ;
        params.reversed = reversed ? 1u : 0u;
        params.iterations = kIterations;
        params.width = width;
        params.height = height;
        params.samples = samples;

        ID3DDeviceContextState* gameState = nullptr;
        context1->SwapDeviceContextState(g_state, &gameState);
        context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
        context1->ClearDepthStencilView(g_depthView, D3D11_CLEAR_DEPTH, 0.0f, 0);
        context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context1->VSSetShader(g_vs, nullptr, 0);
        ID3D11ShaderResourceView* views[2] = {source, depth};
        context1->PSSetConstantBuffers(0, 1, &g_params);
        context1->PSSetShaderResources(0, 2, views);
        context1->PSSetShader(g_ps[samples > 1 ? 1 : 0], nullptr, 0);
        context1->PSSetSamplers(0, 1, &g_sampler);
        context1->RSSetState(g_raster);
        context1->OMSetDepthStencilState(g_depthState, 0);
        context1->OMSetRenderTargets(1, &targetRtv, g_depthView);
        D3D11_VIEWPORT v{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context1->RSSetViewports(1, &v);
        context1->Draw(3, 0);

        ID3D11ShaderResourceView* none[2] = {};
        ID3D11RenderTargetView* noTarget = nullptr;
        context1->VSSetShaderResources(0, 2, none);
        context1->PSSetShaderResources(0, 2, none);
        context1->OMSetRenderTargets(1, &noTarget, nullptr);
        context1->SwapDeviceContextState(gameState, nullptr);
        safeRelease(gameState);
        if (timing) {
            context->End(timing->end);
            context->End(timing->disjoint);
            timing->pending = true;
        }
        done = true;
        if (++g_drawn == 1 && g_logs == 0)
            LOG("Depth eye: the other eye made from the fresh one (%ux%u, depth x%u%s, %u steps a pixel)", width, height,
                samples, reversed ? ", reversed" : "", kIterations);
    }
    if (!done) g_failure = !context1 ? "no D3D11.1 context" : !g_depthView ? "no depth buffer" : "no view of the eye image";
    safeRelease(targetRtv);
    safeRelease(context1);
    context->Release();
    depth->Release();
    return done;
}

const char* failure() { return g_failure ? g_failure : "unknown"; }
bool broken() { return g_failed; }

void forgetTargets() {
    for (TargetView& t : g_targets) {
        safeRelease(t.view);
        t.texture = nullptr;
    }
}

void release() {
    forgetTargets();
    safeRelease(g_depthView);
    safeRelease(g_depth);
    g_width = g_height = 0;
    for (Timing& t : g_timing) {
        safeRelease(t.disjoint);
        safeRelease(t.begin);
        safeRelease(t.end);
        t.pending = false;
    }
    safeRelease(g_state);
    safeRelease(g_depthState);
    safeRelease(g_raster);
    safeRelease(g_sampler);
    safeRelease(g_params);
    safeRelease(g_ps[0]);
    safeRelease(g_ps[1]);
    safeRelease(g_vs);
    g_device = nullptr;
}

}  // namespace s4vr::deptheye
