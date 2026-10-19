#include "nodim.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"

namespace s4vr::nodim {
namespace {

// At the source: the game's gradient colour ramp (Scaleform GradientRamp::Init,
// 1.128 0x107c540, records of {ratio byte, pad, ARGB at +4}, 8 bytes each). It
// runs when a gradient fill's picture is made, the first time the fill is drawn.
using RampFn = void (*)(void* ramp, const uint8_t* records, unsigned count, float gamma);
RampFn g_originalRamp = nullptr;
std::atomic<bool> g_sourceOn{false};
std::atomic<int> g_sourceLogs{0};

// DrawModality's stops: alphas 0.55 and 0.85 (140, 216) at ratios 0 and 255, in
// dark colours (#161D3A, #12152F; a UI mod may change those). No other gradient
// in the game's UI uses those alphas.
bool isDimming(const uint8_t* records, unsigned count) {
    if (count != 2 || !records) return false;
    if (records[0] != 0 || records[8] != 255 || records[7] != 140 || records[15] != 216) return false;
    for (int i : {4, 5, 6, 12, 13, 14})
        if (records[i] >= 0x60) return false;
    return true;
}

void hookedRamp(void* ramp, const uint8_t* records, unsigned count, float gamma) {
    if (g_sourceOn.load(std::memory_order_relaxed) && isDimming(records, count)) {
        uint8_t clear[16];
        std::memcpy(clear, records, sizeof(clear));
        clear[7] = clear[15] = 0;
        if (g_sourceLogs.fetch_add(1) < 5)
            LOG("No dimming: the game's dimming (#%02X%02X%02X to #%02X%02X%02X) made see-through", records[6],
                records[5], records[4], records[14], records[13], records[12]);
        g_originalRamp(ramp, clear, count, gamma);
        return;
    }
    g_originalRamp(ramp, records, count, gamma);
}

constexpr int kRows = 24;   // where the dimming is read (its gradient is linear down the screen)
constexpr int kMaxBoxes = 4;

const char kShader[] = R"(
cbuffer Dim : register(b0) {
    float4 rows[24];   // the dimming, premultiplied, at row centres
    float4 boxes[4];   // dialogs over it: x0, y0, x1, y1 (0..1)
    float4 info;       // x: box count
};
Texture2D<float4> ui : register(t0);
float4 vsMain(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psMain(float4 pos : SV_Position) : SV_Target {
    uint w, h;
    ui.GetDimensions(w, h);
    float2 uv = pos.xy / float2(w, h);
    float4 r = ui.Load(int3(pos.xy, 0));
    float fy = clamp(uv.y * 24 - 0.5, 0, 23);
    int i0 = (int)floor(fy);
    float4 d = lerp(rows[i0], rows[min(i0 + 1, 23)], fy - i0);
    float4 diff = abs(r - d);
    if (max(max(diff.r, diff.g), max(diff.b, diff.a)) < 5.0 / 255) return 0;  // the dimming alone
    for (int b = 0; b < (int)info.x; ++b)
        if (all(uv >= boxes[b].xy) && all(uv <= boxes[b].zw)) return r;      // a dialog over it
    float4 u = (r - d) / max(1 - d.a, 0.05);
    // Not something the dimming was drawn over (a dialog or a widget above it): as it is.
    if (u.a < -0.03 || u.a > 1.03 || any(u.rgb < -0.04) || any(u.rgb > u.a + 0.04)) return r;
    u = saturate(u);
    if (u.a < 0.01) return 0;
    return float4(min(u.rgb, u.a), u.a);
}
)";

std::mutex g_mutex;
bool g_on = false;
int g_boxes[kMaxBoxes][4] = {};
int g_boxCount = 0;

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Texture2D* g_staging[3] = {};  // the edge samples, read back two frames later
UINT g_stagingFormat = 0;
int g_frame = 0;
bool g_failed = false;
bool g_dimmed = false;               // the dimming was seen in the last samples read
float g_rows[kRows][4] = {};         // its colour per row (premultiplied, 0..1)

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void releaseAll() {
    safeRelease(g_vs);
    safeRelease(g_ps);
    safeRelease(g_constants);
    for (auto*& s : g_staging) safeRelease(s);
    safeRelease(g_device);
}

bool init(ID3D11Device* device) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compileFn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compileFn) {
        LOG("No dimming: d3dcompiler_47.dll not available; off");
        return false;
    }
    auto compile = [&](const char* entry, const char* target) -> ID3DBlob* {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(compileFn(kShader, sizeof(kShader) - 1, "nodim", nullptr, nullptr, entry, target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            LOG("No dimming: %s failed to compile: %s", entry,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            safeRelease(code);
        }
        safeRelease(errors);
        return code;
    };
    ID3DBlob* vs = compile("vsMain", "vs_5_0");
    ID3DBlob* ps = compile("psMain", "ps_5_0");
    bool ok = vs && ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
    safeRelease(vs);
    safeRelease(ps);
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = (kRows + kMaxBoxes + 1) * 16;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &g_constants));
    if (!ok) {
        releaseAll();
        return false;
    }
    g_device = device;
    g_device->AddRef();
    LOG("No dimming: ready");
    return true;
}

bool srgbFormat(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
bool bgrFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// One texel, as the shader would see it (linear if the format is sRGB).
void decode(const uint8_t* p, DXGI_FORMAT f, float out[4]) {
    float c[4] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    if (bgrFormat(f)) std::swap(c[0], c[2]);
    if (srgbFormat(f))
        for (int i = 0; i < 3; ++i) c[i] = c[i] <= 0.04045f ? c[i] / 12.92f : std::pow((c[i] + 0.055f) / 1.055f, 2.4f);
    std::memcpy(out, c, sizeof(c));
}

// Reads the samples copied two frames ago: is the dimming up, and its colour per row.
void readSamples(ID3D11DeviceContext* context, ID3D11Texture2D* staging, DXGI_FORMAT format) {
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) return;
    const uint8_t* row = static_cast<const uint8_t*>(map.pData);
    float left[kRows][4], right[kRows][4];
    for (int i = 0; i < kRows; ++i) {
        decode(row + i * 4, format, left[i]);
        decode(row + (kRows + i) * 4, format, right[i]);
    }
    context->Unmap(staging, 0);
    // A row shows the dimming where both edges agree, it is see-through in
    // between, and it is the dimming's dark blue (blue over green over red).
    bool seen[kRows];
    int count = 0;
    float rows[kRows][4];
    for (int i = 0; i < kRows; ++i) {
        float diff = 0;
        for (int c = 0; c < 4; ++c) diff = std::max(diff, std::fabs(left[i][c] - right[i][c]));
        const float* s = left[i];
        seen[i] = diff < 4.0f / 255 && s[3] > 0.4f && s[3] < 0.95f && s[2] >= s[1] && s[1] >= s[0] && s[2] < 0.3f;
        for (int c = 0; c < 4; ++c) rows[i][c] = (left[i][c] + right[i][c]) / 2;
        count += seen[i];
    }
    bool dimmed = count >= kRows * 6 / 10;
    if (dimmed) {
        // Rows covered at an edge: in between the seen ones (the gradient is linear).
        for (int i = 0; i < kRows; ++i) {
            if (seen[i]) continue;
            int a = i - 1, b = i + 1;
            while (a >= 0 && !seen[a]) --a;
            while (b < kRows && !seen[b]) ++b;
            for (int c = 0; c < 4; ++c) {
                if (a >= 0 && b < kRows)
                    rows[i][c] = rows[a][c] + (rows[b][c] - rows[a][c]) * (i - a) / static_cast<float>(b - a);
                else
                    rows[i][c] = rows[a >= 0 ? a : b][c];
            }
        }
        std::memcpy(g_rows, rows, sizeof(rows));
    }
    if (dimmed != g_dimmed) {
        static int logged = 0;
        if (logged++ < 40)
            LOG("No dimming: game dimming %s (%d of %d rows seen; top %.2f, bottom %.2f opaque)",
                dimmed ? "up, taken out" : "gone", count, kRows, rows[0][3], rows[kRows - 1][3]);
    }
    g_dimmed = dimmed;
}

}  // namespace

void installSource(bool on) {
    g_sourceOn = on;
    uint8_t* target = gameaddr::dx9() ? nullptr : addrs::fn("Ramp");
    if (!target) {
        LOG("No dimming: gradient ramp not found for this game build; dimming only taken out of the VR UI");
        return;
    }
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        status = MH_CreateHook(target, reinterpret_cast<void*>(&hookedRamp), reinterpret_cast<void**>(&g_originalRamp));
    if (status == MH_OK) status = MH_EnableHook(target);
    LOG("No dimming: gradient ramp hook %s (%s)", MH_StatusToString(status), on ? "on" : "off");
}

void setSource(bool on) {
    if (g_sourceOn.exchange(on) != on) LOG("No dimming at the source: %s", on ? "on" : "off");
}

void set(bool on, const int (*boxes)[4], int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on = on;
    g_boxCount = std::max(0, std::min(count, kMaxBoxes));
    for (int i = 0; i < g_boxCount; ++i) std::memcpy(g_boxes[i], boxes[i], sizeof(g_boxes[i]));
}

bool apply(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* raw, ID3D11RenderTargetView* out,
           const D3D11_TEXTURE2D_DESC& desc) {
    bool on;
    int boxes[kMaxBoxes][4];
    int boxCount;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        on = g_on;
        boxCount = g_boxCount;
        std::memcpy(boxes, g_boxes, sizeof(boxes));
    }
    if (!on || g_failed) {
        g_dimmed = false;
        return false;
    }
    if (g_device != device) {
        releaseAll();
        if (!init(device)) {
            g_failed = true;
            return false;
        }
    }
    const DXGI_FORMAT format = desc.Format;
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        static bool logged = false;
        if (!logged) LOG("No dimming: UI format %d not handled; off", static_cast<int>(desc.Format));
        logged = true;
        g_failed = true;
        return false;
    }
    // The edge samples: x 1 and W-2 at each row centre, into a staging row.
    if (!g_staging[0] || g_stagingFormat != static_cast<UINT>(format)) {
        for (auto*& s : g_staging) safeRelease(s);
        D3D11_TEXTURE2D_DESC s{};
        s.Width = kRows * 2;
        s.Height = 1;
        s.MipLevels = s.ArraySize = 1;
        s.Format = format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto*& t : g_staging)
            if (FAILED(device->CreateTexture2D(&s, nullptr, &t))) {
                g_failed = true;
                return false;
            }
        g_stagingFormat = format;
        g_frame = 0;
    }
    ID3D11Texture2D* now = g_staging[g_frame % 3];
    for (int i = 0; i < kRows; ++i) {
        UINT y = static_cast<UINT>((i + 0.5f) * desc.Height / kRows);
        D3D11_BOX l{1, y, 0, 2, y + 1, 1}, r{desc.Width - 2, y, 0, desc.Width - 1, y + 1, 1};
        context->CopySubresourceRegion(now, 0, i, 0, 0, raw, 0, &l);
        context->CopySubresourceRegion(now, 0, kRows + i, 0, 0, raw, 0, &r);
    }
    if (g_frame >= 2) readSamples(context, g_staging[(g_frame - 2) % 3], format);
    ++g_frame;
    if (!g_dimmed) return false;

    float constants[kRows + kMaxBoxes + 1][4] = {};
    std::memcpy(constants, g_rows, sizeof(g_rows));
    for (int i = 0; i < boxCount; ++i) {
        float* b = constants[kRows + i];
        b[0] = static_cast<float>(boxes[i][0]) / desc.Width;
        b[1] = static_cast<float>(boxes[i][1]) / desc.Height;
        b[2] = static_cast<float>(boxes[i][0] + boxes[i][2]) / desc.Width;
        b[3] = static_cast<float>(boxes[i][1] + boxes[i][3]) / desc.Height;
    }
    constants[kRows + kMaxBoxes][0] = static_cast<float>(boxCount);
    context->UpdateSubresource(g_constants, 0, nullptr, constants, 0, 0);

    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(device->CreateShaderResourceView(raw, nullptr, &view))) return false;
    // The game's state around this pass, put back after.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &dsv);
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&viewportCount, viewports);
    ID3D11RasterizerState* raster = nullptr;
    context->RSGetState(&raster);
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4];
    UINT sampleMask = 0;
    context->OMGetBlendState(&blend, blendFactor, &sampleMask);
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&depth, &stencilRef);
    ID3D11InputLayout* layout = nullptr;
    context->IAGetInputLayout(&layout);
    D3D11_PRIMITIVE_TOPOLOGY topology;
    context->IAGetPrimitiveTopology(&topology);
    ID3D11VertexShader* vs = nullptr;
    context->VSGetShader(&vs, nullptr, nullptr);
    ID3D11GeometryShader* gs = nullptr;
    context->GSGetShader(&gs, nullptr, nullptr);
    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader(&ps, nullptr, nullptr);
    ID3D11Buffer* psCb = nullptr;
    context->PSGetConstantBuffers(0, 1, &psCb);
    ID3D11ShaderResourceView* psSrv = nullptr;
    context->PSGetShaderResources(0, 1, &psSrv);

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    context->OMSetRenderTargets(1, &out, nullptr);
    context->RSSetViewports(1, &vp);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_constants);
    context->PSSetShaderResources(0, 1, &view);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    UINT count = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (targets[i]) count = i + 1;
    context->OMSetRenderTargets(count, targets, dsv);
    context->RSSetViewports(viewportCount, viewports);
    context->RSSetState(raster);
    context->OMSetBlendState(blend, blendFactor, sampleMask);
    context->OMSetDepthStencilState(depth, stencilRef);
    context->IASetInputLayout(layout);
    context->IASetPrimitiveTopology(topology);
    context->VSSetShader(vs, nullptr, 0);
    context->GSSetShader(gs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &psCb);
    context->PSSetShaderResources(0, 1, &psSrv);
    for (auto*& t : targets) safeRelease(t);
    safeRelease(dsv);
    safeRelease(raster);
    safeRelease(blend);
    safeRelease(depth);
    safeRelease(layout);
    safeRelease(vs);
    safeRelease(gs);
    safeRelease(ps);
    safeRelease(psCb);
    safeRelease(psSrv);
    safeRelease(view);
    return true;
}

}  // namespace s4vr::nodim
