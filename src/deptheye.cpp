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

constexpr UINT kStep = 3;  // pixels between grid points (the depth is followed this finely)

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
    float4 srcTan;  // left, right, up, down
    float4 dstTan;
    float nearZ, farZ;
    uint reversed, step;
    uint cellsX, width, height, pad;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float viewZ(float d) {
    float z = reversed ? nearZ * farZ / (nearZ + d * (farZ - nearZ)) : nearZ * farZ / (farZ - d * (farZ - nearZ));
    return min(z, farZ);
}

static const uint2 kCorner[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1)};

// Two triangles per grid cell; each corner is a source pixel moved by its depth.
VsOut vsMain(uint id : SV_VertexID) {
    uint cell = id / 6;
    uint2 g = uint2(cell % cellsX, cell / cellsX) + kCorner[id % 6];
    int2 p = min(int2(g * step), int2(width - 1, height - 1));
    float2 uv = (float2(p) + 0.5) / float2(width, height);
    float z = viewZ(DEPTH(p));
    float3 s = float3(lerp(srcTan.x, srcTan.y, uv.x) * z, lerp(srcTan.z, srcTan.w, uv.y) * z, z);
    float3 t = float3(dot(row0.xyz, s) + row0.w, dot(row1.xyz, s) + row1.w, dot(row2.xyz, s) + row2.w);
    VsOut o;
    o.uv = uv;
    if (t.z < 1e-3) {
        o.position = float4(0, 0, -1, 1);  // behind the target eye: clipped
        return o;
    }
    float2 a = t.xy / t.z;
    float2 q = float2((a.x - dstTan.x) / (dstTan.y - dstTan.x), (dstTan.z - a.y) / (dstTan.z - dstTan.w));
    // Nearer is larger: the depth test keeps the nearest surface.
    o.position = float4(q.x * 2 - 1, 1 - q.y * 2, 1 / (1 + t.z), 1);
    return o;
}

float4 psMain(VsOut i) : SV_Target {
    return float4(source.SampleLevel(linearClamp, i.uv, 0).rgb, 1);
}
)HLSL";

struct Params {
    float rows[3][4];
    float srcTan[4];
    float dstTan[4];
    float nearZ, farZ;
    uint32_t reversed, step;
    uint32_t cellsX, width, height, pad;
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

bool g_failed = false;
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs[2] = {};  // single-sampled depth, MSAA depth
ID3D11PixelShader* g_ps = nullptr;
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
    bool ok = true;
    for (int ms = 0; ms < 2 && ok; ++ms) {
        ID3DBlob* vs = compile(compileFn, "vsMain", "vs_5_0", ms != 0);
        ok = vs && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs[ms]));
        safeRelease(vs);
    }
    ID3DBlob* ps = ok ? compile(compileFn, "psMain", "ps_5_0", false) : nullptr;
    ok = ps && SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    safeRelease(ps);
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
    if (!source || !target || width < 2 || height < 2 || !(nearZ > 0) || !(farZ > nearZ)) return false;
    ID3D11Device* device = nullptr;
    target->GetDevice(&device);
    device->Release();  // g_device is only compared, never used after the device goes away
    if (!ready(device)) return false;
    logNow();

    UINT samples = 0, depthWidth = 0, depthHeight = 0;
    bool reversed = false;
    ID3D11ShaderResourceView* depth = nearplane::latestDepth(samples, depthWidth, depthHeight, reversed);
    if (!depth) {
        ++g_noDepth;
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
        std::memcpy(params.srcTan, srcTan, sizeof(params.srcTan));
        std::memcpy(params.dstTan, dstTan, sizeof(params.dstTan));
        params.nearZ = nearZ;
        params.farZ = farZ;
        params.reversed = reversed ? 1u : 0u;
        params.step = kStep;
        const UINT cellsX = (width - 1 + kStep - 1) / kStep, cellsY = (height - 1 + kStep - 1) / kStep;
        params.cellsX = cellsX;
        params.width = width;
        params.height = height;

        ID3DDeviceContextState* gameState = nullptr;
        context1->SwapDeviceContextState(g_state, &gameState);
        context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
        context1->ClearDepthStencilView(g_depthView, D3D11_CLEAR_DEPTH, 0.0f, 0);
        context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context1->VSSetShader(g_vs[samples > 1 ? 1 : 0], nullptr, 0);
        context1->VSSetConstantBuffers(0, 1, &g_params);
        ID3D11ShaderResourceView* views[2] = {source, depth};
        context1->VSSetShaderResources(0, 2, views);
        context1->PSSetShader(g_ps, nullptr, 0);
        context1->PSSetShaderResources(0, 1, &source);
        context1->PSSetSamplers(0, 1, &g_sampler);
        context1->RSSetState(g_raster);
        context1->OMSetDepthStencilState(g_depthState, 0);
        context1->OMSetRenderTargets(1, &targetRtv, g_depthView);
        D3D11_VIEWPORT v{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context1->RSSetViewports(1, &v);
        context1->Draw(cellsX * cellsY * 6, 0);

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
            LOG("Depth eye: the other eye made from the fresh one (%ux%u, depth x%u%s, grid %u px)", width, height,
                samples, reversed ? ", reversed" : "", kStep);
    }
    safeRelease(targetRtv);
    safeRelease(context1);
    context->Release();
    depth->Release();
    return done;
}

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
    safeRelease(g_ps);
    safeRelease(g_vs[0]);
    safeRelease(g_vs[1]);
    g_device = nullptr;
}

}  // namespace s4vr::deptheye
