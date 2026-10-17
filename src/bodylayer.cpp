#include "bodylayer.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "nearplane.h"

namespace s4vr::bodylayer {

namespace {

constexpr float kConsider = 2.5f;  // metres: nothing farther is your body (feet, looking down)
constexpr int kMaxLevels = 16;

const char kShader[] = R"HLSL(
// The eye picture, as the swapchain shows it (an sRGB view reads linear).
Texture2D<float4> scene : register(t0);
// Your body's share of each pixel (0, 0.5, 1 with 2x MSAA).
Texture2D<float> mask : register(t1);
// Pull-push averages: rgb * weight, weight (one level, or all of them).
Texture2D<float4> push : register(t2);
#ifdef MS
Texture2DMS<float> depthTex : register(t3);
#define DEPTH(p, k) depthTex.Load(p, k)
#else
Texture2D<float> depthTex : register(t3);
#define DEPTH(p, k) depthTex.Load(int3(p, 0))
#endif
SamplerState linearClamp : register(s0);

cbuffer Params : register(b0) {
    float4 tangents;  // the view's left, right, up, down
    float nearZ, farZ, considerZ;
    uint reversed;    // 1: the depth clears to 0 (near is 1)
    uint samples, count, levels, pad;
    float4 boundsLow, boundsHigh;  // around all of the parts
    float4 partA[48];  // your body: segment start, radius (view space: x right, y up, z ahead)
    float4 partB[48];  // segment end
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

float viewZ(float d) {
    return reversed ? nearZ * farZ / (nearZ + d * (farZ - nearZ)) : nearZ * farZ / (farZ - d * (farZ - nearZ));
}

bool onBody(float3 q) {
    [loop] for (uint i = 0; i < count; ++i) {
        float3 a = partA[i].xyz, ab = partB[i].xyz - a;
        float t = saturate(dot(q - a, ab) / max(dot(ab, ab), 1e-8));
        float3 c = a + t * ab - q;
        if (dot(c, c) < partA[i].w * partA[i].w) return true;
    }
    return false;
}

// Your body's share of each pixel, from the depth of each of its samples.
float psMask(VsOut i) : SV_Target {
    uint w, h;
    scene.GetDimensions(w, h);
    int2 p = int2(i.position.xy);
    float2 uv = (float2(p) + 0.5) / float2(w, h);
    float2 t = float2(lerp(tangents.x, tangents.y, uv.x), lerp(tangents.z, tangents.w, uv.y));
    float cover = 0;
    [loop] for (uint k = 0; k < samples; ++k) {
        float z = viewZ(DEPTH(p, k));
        float3 q = float3(t * z, z);
        if (z > 0 && z < considerZ && all(q >= boundsLow.xyz) && all(q <= boundsHigh.xyz) && onBody(q)) cover += 1;
    }
    return cover / samples;
}

// Your body and a pixel around it, so its edges keep their smoothing.
float hole(int2 p) {
    float m = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx) m = max(m, mask.Load(int3(p + int2(dx, dy), 0)));
    return m;
}

// The body layer: those pixels as they are, nothing elsewhere. Unturned,
// it covers its own place in the eye picture exactly.
float4 psBody(VsOut i) : SV_Target {
    int2 p = int2(i.position.xy);
    return hole(p) > 0 ? float4(scene.Load(int3(p, 0)).rgb, 1) : float4(0, 0, 0, 0);
}

// The first pull level (half size): the world around your body, averaged.
float4 psPush0(VsOut i) : SV_Target {
    uint w, h;
    scene.GetDimensions(w, h);
    int2 o = int2(i.position.xy) * 2;
    float4 sum = 0;
    [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 p = o + int2(x, y);
            if (p.x < (int)w && p.y < (int)h && hole(p) <= 0) sum += float4(scene.Load(int3(p, 0)).rgb, 1);
        }
    return sum / 4;
}

// Each further level: the one before, halved.
float4 psPush(VsOut i) : SV_Target {
    int2 o = int2(i.position.xy) * 2;
    return (push.Load(int3(o, 0)) + push.Load(int3(o + int2(1, 0), 0)) + push.Load(int3(o + int2(0, 1), 0)) +
            push.Load(int3(o + int2(1, 1), 0))) / 4;
}

// Your body in the eye picture: the finest level that has world there.
float4 psFill(VsOut i) : SV_Target {
    int2 p = int2(i.position.xy);
    if (hole(p) <= 0) discard;
    uint w, h;
    scene.GetDimensions(w, h);
    float2 uv = (float2(p) + 0.5) / float2(w, h);
    float4 c = 0;
    [loop] for (uint l = 0; l < levels; ++l) {
        c = push.SampleLevel(linearClamp, uv, l);
        if (c.a > 0.1) break;
    }
    return float4(c.a > 1e-5 ? c.rgb / c.a : scene.Load(int3(p, 0)).rgb, 1);
}
)HLSL";

struct Params {
    float tangents[4];
    float nearZ, farZ, considerZ;
    uint32_t reversed;
    uint32_t samples, count, levels, pad;
    float boundsLow[4], boundsHigh[4];
    float partA[camera::kBodyParts][4];
    float partB[camera::kBodyParts][4];
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");
static_assert(camera::kBodyParts == 48, "the shader holds 48 parts");

bool g_failed = false;
ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psMask[2] = {};  // single-sampled depth, MSAA depth
ID3D11PixelShader* g_psBody = nullptr;
ID3D11PixelShader* g_psPush0 = nullptr;
ID3D11PixelShader* g_psPush = nullptr;
ID3D11PixelShader* g_psFill = nullptr;
ID3D11Buffer* g_params = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3DDeviceContextState* g_state = nullptr;

// Sized to the eye pictures.
UINT g_width = 0, g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D* g_scene = nullptr;
ID3D11ShaderResourceView* g_sceneView = nullptr;
ID3D11Texture2D* g_mask = nullptr;
ID3D11ShaderResourceView* g_maskView = nullptr;
ID3D11RenderTargetView* g_maskTarget = nullptr;
ID3D11Texture2D* g_push = nullptr;
int g_levels = 0;
UINT g_levelSize[kMaxLevels][2] = {};
ID3D11RenderTargetView* g_pushTarget[kMaxLevels] = {};
ID3D11ShaderResourceView* g_pushLevel[kMaxLevels] = {};
ID3D11ShaderResourceView* g_pushAll = nullptr;

// Views of the swapchain images we draw into, kept while their swapchains
// live (forgetTargets). Three eye swapchains (left, right, the spare left of
// paired eyes) and their three body swapchains, three images each, made 18:
// the 16 slots overflowed, and the overflow threw every view away, the eye
// picture's too, in the middle of split(), which then drew with the freed
// view (crashes 2026-10-14 23:37, d3d11.dll reading 0x155 in OMSetRenderTargets,
// v0.13.109). Now there is room for them all, an overflow drops only the view
// used longest ago, and split() holds its own reference to its views.
struct TargetView {
    ID3D11Texture2D* texture;
    DXGI_FORMAT format;
    ID3D11RenderTargetView* view;
    uint64_t used;
};
constexpr int kTargets = 32;
TargetView g_targets[kTargets] = {};
uint64_t g_targetUse = 0;

uint64_t g_splits = 0, g_noDepth = 0, g_otherSize = 0;
ULONGLONG g_logAt = 0;
int g_logs = 0;

// What the split costs the graphics card (user 2026-10-14 21:52Z: "absolute
// laggy performance"; v0.13.110): timestamps around each split, read back a
// few frames later without waiting, averaged in the 30 s log line.
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

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

ID3DBlob* compile(pD3DCompile compileFn, const char* entry, const char* target, bool ms) {
    const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compileFn(kShader, sizeof(kShader) - 1, "bodylayer", ms ? defines : nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Body layer: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        safeRelease(code);
    }
    safeRelease(errors);
    return code;
}

bool pixelShader(ID3D11Device* device, pD3DCompile compileFn, const char* entry, bool ms, ID3D11PixelShader*& out) {
    ID3DBlob* code = compile(compileFn, entry, "ps_5_0", ms);
    bool ok = code && SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
    safeRelease(code);
    return ok;
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        LOG("Body layer: d3dcompiler_47.dll not available; off");
        return false;
    }
    ID3DBlob* vs = compile(compileFn, "vsMain", "vs_5_0", false);
    bool ok = vs && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs));
    safeRelease(vs);
    ok = ok && pixelShader(device, compileFn, "psMask", false, g_psMask[0]) &&
         pixelShader(device, compileFn, "psMask", true, g_psMask[1]) &&
         pixelShader(device, compileFn, "psBody", false, g_psBody) &&
         pixelShader(device, compileFn, "psPush0", false, g_psPush0) &&
         pixelShader(device, compileFn, "psPush", false, g_psPush) &&
         pixelShader(device, compileFn, "psFill", false, g_psFill);
    if (!ok) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g_params))) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

    // A separate pipeline state, swapped in for our passes and out again, so
    // the game's own bindings are restored exactly.
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        LOG("Body layer: no D3D11.1 device; off");
        return false;
    }
    D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    HRESULT hr = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                   &g_state);
    device1->Release();
    if (FAILED(hr)) {
        LOG("Body layer: CreateDeviceContextState failed (0x%08lx); off", static_cast<unsigned long>(hr));
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
    LOG("Body layer: ready");
    return true;
}

// Timings that have come back, into the averages.
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

void releaseSized() {
    for (int l = 0; l < kMaxLevels; ++l) {
        safeRelease(g_pushTarget[l]);
        safeRelease(g_pushLevel[l]);
    }
    safeRelease(g_pushAll);
    safeRelease(g_push);
    safeRelease(g_maskTarget);
    safeRelease(g_maskView);
    safeRelease(g_mask);
    safeRelease(g_sceneView);
    safeRelease(g_scene);
    g_levels = 0;
    g_width = g_height = 0;
    g_format = DXGI_FORMAT_UNKNOWN;
}

bool ensureSized(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_scene && g_width == width && g_height == height && g_format == format) return true;
    releaseSized();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_scene)) &&
              SUCCEEDED(device->CreateShaderResourceView(g_scene, nullptr, &g_sceneView));
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_mask)) &&
         SUCCEEDED(device->CreateShaderResourceView(g_mask, nullptr, &g_maskView)) &&
         SUCCEEDED(device->CreateRenderTargetView(g_mask, nullptr, &g_maskTarget));
    // The pull levels: half size down to a pixel.
    UINT w = (width + 1) / 2, h = (height + 1) / 2;
    int levels = 1;
    for (UINT m = std::max(w, h); m > 1 && levels < kMaxLevels; m /= 2) ++levels;
    d.Width = w;
    d.Height = h;
    d.MipLevels = static_cast<UINT>(levels);
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_push)) &&
         SUCCEEDED(device->CreateShaderResourceView(g_push, nullptr, &g_pushAll));
    for (int l = 0; ok && l < levels; ++l) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = d.Format;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = static_cast<UINT>(l);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = d.Format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = static_cast<UINT>(l);
        srv.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(device->CreateRenderTargetView(g_push, &rtv, &g_pushTarget[l])) &&
             SUCCEEDED(device->CreateShaderResourceView(g_push, &srv, &g_pushLevel[l]));
        g_levelSize[l][0] = std::max(1u, w >> l);
        g_levelSize[l][1] = std::max(1u, h >> l);
    }
    if (!ok) {
        LOG("Body layer: could not make its %ux%u format %d textures; off", width, height, static_cast<int>(format));
        releaseSized();
        return false;
    }
    g_levels = levels;
    g_width = width;
    g_height = height;
    g_format = format;
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
        // A free slot, else the one used longest ago.
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

void logNow() {
    ULONGLONG now = GetTickCount64();
    if (!g_logAt) g_logAt = now;
    if (now - g_logAt < 30000 || g_logs >= 60) return;
    ++g_logs;
    LOG("Body layer: last 30 s, %llu eye pictures split, %llu without this frame's depth, %llu of another size; "
        "graphics card %.2f ms per split on average, worst %.2f (%llu timed)",
        static_cast<unsigned long long>(g_splits), static_cast<unsigned long long>(g_noDepth),
        static_cast<unsigned long long>(g_otherSize), g_gpuCount ? g_gpuSum / double(g_gpuCount) : 0.0, g_gpuWorst,
        static_cast<unsigned long long>(g_gpuCount));
    g_splits = g_noDepth = g_otherSize = 0;
    g_gpuSum = g_gpuWorst = 0;
    g_gpuCount = 0;
    g_logAt = now;
}

}  // namespace

bool split(ID3D11Texture2D* eye, ID3D11Texture2D* body, DXGI_FORMAT format, UINT width, UINT height,
           const camera::Fov& fov, const camera::BodyParts& parts) {
    if (!eye || !body || parts.count <= 0) return false;
    ID3D11Device* device = nullptr;
    eye->GetDevice(&device);
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
            LOG("Body layer: the scene depth is %ux%u x%u, the eye picture %ux%u; not split", depthWidth, depthHeight,
                samples, width, height);
        }
        depth->Release();
        return false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11RenderTargetView* eyeTarget = nullptr;
    ID3D11RenderTargetView* bodyTarget = nullptr;
    bool done = false;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        ensureSized(device, width, height, format) && (eyeTarget = targetView(device, eye, format)) &&
        (bodyTarget = targetView(device, body, format))) {
        collectTimings(context);
        Timing* timing = &g_timing[g_timingNext];
        if (timing->pending || !timing->disjoint) {
            timing = nullptr;  // still out: this split goes untimed
        } else {
            g_timingNext = (g_timingNext + 1) % kTimings;
            context->Begin(timing->disjoint);
            context->End(timing->begin);
        }
        context->CopyResource(g_scene, eye);

        Params params{};
        params.tangents[0] = std::tan(fov.left);
        params.tangents[1] = std::tan(fov.right);
        params.tangents[2] = std::tan(fov.up);
        params.tangents[3] = std::tan(fov.down);
        params.nearZ = parts.nearZ;
        params.farZ = parts.farZ;
        params.considerZ = kConsider;
        params.reversed = reversed ? 1u : 0u;
        params.samples = samples;
        params.count = static_cast<uint32_t>(std::min(parts.count, camera::kBodyParts));
        params.levels = static_cast<uint32_t>(g_levels);
        std::memcpy(params.partA, parts.a, sizeof(float) * 4 * params.count);
        std::memcpy(params.partB, parts.b, sizeof(float) * 4 * params.count);
        for (int axis = 0; axis < 3; ++axis) {
            params.boundsLow[axis] = 1e9f;
            params.boundsHigh[axis] = -1e9f;
        }
        for (uint32_t i = 0; i < params.count; ++i) {
            const float r = parts.a[i][3];
            for (int axis = 0; axis < 3; ++axis) {
                params.boundsLow[axis] = std::min(params.boundsLow[axis], std::min(parts.a[i][axis], parts.b[i][axis]) - r);
                params.boundsHigh[axis] = std::max(params.boundsHigh[axis], std::max(parts.a[i][axis], parts.b[i][axis]) + r);
            }
        }

        ID3DDeviceContextState* gameState = nullptr;
        context1->SwapDeviceContextState(g_state, &gameState);
        context1->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
        context1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context1->VSSetShader(g_vs, nullptr, 0);
        context1->PSSetConstantBuffers(0, 1, &g_params);
        context1->PSSetSamplers(0, 1, &g_sampler);
        auto viewport = [&](UINT w, UINT h) {
            D3D11_VIEWPORT v{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
            context1->RSSetViewports(1, &v);
        };
        ID3D11ShaderResourceView* none[4] = {};

        // 1. Your body's share of each pixel.
        context1->OMSetRenderTargets(1, &g_maskTarget, nullptr);
        ID3D11ShaderResourceView* maskViews[4] = {g_sceneView, nullptr, nullptr, depth};
        context1->PSSetShaderResources(0, 4, maskViews);
        context1->PSSetShader(g_psMask[samples > 1 ? 1 : 0], nullptr, 0);
        viewport(width, height);
        context1->Draw(3, 0);

        // 2. Your body to its layer.
        context1->PSSetShaderResources(0, 4, none);
        context1->OMSetRenderTargets(1, &bodyTarget, nullptr);
        ID3D11ShaderResourceView* pushViews[2] = {g_sceneView, g_maskView};
        context1->PSSetShaderResources(0, 2, pushViews);
        context1->PSSetShader(g_psBody, nullptr, 0);
        context1->Draw(3, 0);

        // 3. The world around it, averaged into ever coarser levels.
        context1->OMSetRenderTargets(1, &g_pushTarget[0], nullptr);
        context1->PSSetShader(g_psPush0, nullptr, 0);
        viewport(g_levelSize[0][0], g_levelSize[0][1]);
        context1->Draw(3, 0);
        context1->PSSetShader(g_psPush, nullptr, 0);
        for (int l = 1; l < g_levels; ++l) {
            context1->OMSetRenderTargets(1, &g_pushTarget[l], nullptr);
            context1->PSSetShaderResources(2, 1, &g_pushLevel[l - 1]);
            viewport(g_levelSize[l][0], g_levelSize[l][1]);
            context1->Draw(3, 0);
        }

        // 4. Where your body was in the eye picture: the world filled in.
        context1->PSSetShaderResources(2, 1, none);
        context1->OMSetRenderTargets(1, &eyeTarget, nullptr);
        context1->PSSetShaderResources(2, 1, &g_pushAll);
        context1->PSSetShader(g_psFill, nullptr, 0);
        viewport(width, height);
        context1->Draw(3, 0);

        // Leave nothing of ours bound, then hand the game its state back.
        ID3D11RenderTargetView* noTarget = nullptr;
        context1->PSSetShaderResources(0, 4, none);
        context1->OMSetRenderTargets(1, &noTarget, nullptr);
        context1->SwapDeviceContextState(gameState, nullptr);
        safeRelease(gameState);
        if (timing) {
            context->End(timing->end);
            context->End(timing->disjoint);
            timing->pending = true;
        }
        done = true;
        if (++g_splits == 1 && g_logs == 0)
            LOG("Body layer: your body split off the eye pictures (%d parts, depth x%u%s)", parts.count, samples,
                reversed ? ", reversed" : "");
    }
    safeRelease(eyeTarget);
    safeRelease(bodyTarget);
    safeRelease(context1);
    context->Release();
    depth->Release();
    return done;
}

bool clear(ID3D11Texture2D* body, DXGI_FORMAT format) {
    if (!body) return false;
    ID3D11Device* device = nullptr;
    body->GetDevice(&device);
    device->Release();
    if (!ready(device)) return false;
    ID3D11RenderTargetView* target = targetView(device, body, format);
    if (!target) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const float transparent[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(target, transparent);
    context->Release();
    target->Release();
    return true;
}

void forgetTargets() {
    for (TargetView& t : g_targets) {
        safeRelease(t.view);
        t.texture = nullptr;
    }
}

void release() {
    forgetTargets();
    releaseSized();
    for (Timing& t : g_timing) {
        safeRelease(t.disjoint);
        safeRelease(t.begin);
        safeRelease(t.end);
        t.pending = false;
    }
    safeRelease(g_state);
    safeRelease(g_sampler);
    safeRelease(g_params);
    safeRelease(g_psFill);
    safeRelease(g_psPush);
    safeRelease(g_psPush0);
    safeRelease(g_psBody);
    safeRelease(g_psMask[0]);
    safeRelease(g_psMask[1]);
    safeRelease(g_vs);
    g_device = nullptr;
}

}  // namespace s4vr::bodylayer
