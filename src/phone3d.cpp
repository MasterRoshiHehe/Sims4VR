#include "phone3d.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "armik.h"
#include "camera.h"
#include "simsforever.h"
#include "log.h"
#include "nearplane.h"
#include "trail.h"
#include "trail_art.h"

namespace s4vr::phone3d {

namespace {

const char kShader[] = R"(
cbuffer Draw : register(b0) {
    float4 clipX, clipY, clipZ, clipW;  // phone frame (metres) to clip space
    float4 viewX, viewY, viewZ;         // to view space (x right, y up, z toward the camera)
    float4 screenRect;                  // the glass: centre x, y, width, height
    float4 menuRect;                    // the menu on it
    float4 crop;                        // its part of the UI: u0, v0, u1, v1
    float4 misc;                        // x: 1 with a UI texture, y: corner radius, z: bezel,
                                        // w: > 0 a clear panel (no glass) at this opacity
    float4 depthInfo;                   // psPie: near, far, 1 reversed depth, hand capsules
    float4 capA[6];                     // psPie: your arms and hands in view space: start, radius
    float4 capB[6];                     // end
    float4 tileInfo;                    // x: tiles on the screen (the tablet; 0 = the crop above);
                                        // y: 1 = the wallpaper behind the UI, z: its width / height
    float4 tileDst[10];                 // each tile on the menu rect, from its top left: x0, y0, x1, y1 (0..1)
    float4 tileSrc[10];                  // its part of the UI: u0, v0, u1, v1
};
Texture2D ui : register(t0);
Texture2D wallpaper : register(t2);
#ifdef MS
Texture2DMS<float> sceneDepth : register(t1);
#define SCENE(p) sceneDepth.Load(p, 0)
#else
Texture2D<float> sceneDepth : register(t1);
#define SCENE(p) sceneDepth.Load(int3(p, 0))
#endif
SamplerState linearClamp : register(s0);

struct In { float3 pos : POSITION; float3 normal : NORMAL; float kind : KIND; };
struct Out {
    float4 clip : SV_Position;
    float3 view : VIEWPOS;
    float3 normal : NORMAL;
    float2 local : LOCAL;
    float kind : KIND;
};

Out vsMain(In v) {
    float4 p = float4(v.pos, 1);
    Out o;
    o.clip = float4(dot(clipX, p), dot(clipY, p), dot(clipZ, p), dot(clipW, p));
    o.view = float3(dot(viewX, p), dot(viewY, p), dot(viewZ, p));
    o.normal = float3(dot(viewX.xyz, v.normal), dot(viewY.xyz, v.normal), dot(viewZ.xyz, v.normal));
    o.local = v.pos.xy;
    o.kind = v.kind;
    return o;
}

float roundedBox(float2 p, float2 half, float r) {
    float2 q = abs(p) - (half - r);
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}

bool onHands(float3 q) {
    for (uint k = 0; k < (uint)depthInfo.w; ++k) {
        float3 a = capA[k].xyz, ab = capB[k].xyz - a;
        float t = saturate(dot(q - a, ab) / max(dot(ab, ab), 1e-8));
        float3 c = a + t * ab - q;
        if (dot(c, c) < capA[k].w * capA[k].w) return true;
    }
    return false;
}

// The pie menu (first person): in front of everything in the world but your
// own arms and hands. No depth test; where the scene is nearer than the pie,
// it hides the pie only if that point is on your arms or hands.
float4 psPie(Out i) : SV_Target {
    float2 m = (i.local - menuRect.xy) / menuRect.zw + 0.5;
    if (misc.x < 0.5 || any(m < 0) || any(m > 1)) discard;
    float4 u = ui.SampleLevel(linearClamp, lerp(crop.xy, crop.zw, float2(m.x, 1 - m.y)), 0) * misc.w;
    if (u.a < 0.004) discard;
    float d = SCENE(int2(i.clip.xy));
    float n = depthInfo.x, f = depthInfo.y;
    float sceneZ = depthInfo.z > 0.5 ? (d <= 0 ? 1e9 : n * f / (n + d * (f - n)))
                                     : (d >= 1 ? 1e9 : n * f / (f - d * (f - n)));
    float pieZ = -i.view.z;
    if (sceneZ < pieZ && onHands(i.view * (sceneZ / pieZ))) discard;
    return u;
}

float4 psMain(Out i) : SV_Target {
    float3 n = normalize(i.normal);
    if (dot(n, i.view) > 0) discard;  // a face turned away (no culling: works for either handedness)
    float3 toEye = normalize(-i.view);
    if (i.kind > 0.5 && i.kind < 1.5 && misc.w > 0) {
        // A clear panel: only what the UI draws there (premultiplied alpha).
        float2 m = (i.local - menuRect.xy) / menuRect.zw + 0.5;
        if (misc.x < 0.5 || any(m < 0) || any(m > 1)) discard;
        float4 u = ui.SampleLevel(linearClamp, lerp(crop.xy, crop.zw, float2(m.x, 1 - m.y)), 0) * misc.w;
        if (u.a < 0.004) discard;
        if (misc.z > 0.5 && misc.z < 1.5 && length(m - 0.5) > 0.5) discard;  // a round piece
        if (misc.z > 1.5 && u.a < (misc.z - 2) * misc.w) discard;        // only the solid parts
        return u;
    }
    if (i.kind > 0.5 && i.kind < 1.5) {
        // The glass: the menu where it is, black around it.
        float2 rel = i.local - screenRect.xy;
        if (roundedBox(rel, screenRect.zw * 0.5, max(misc.y - misc.z, 0.001)) > 0) discard;
        float3 c = float3(0.01, 0.01, 0.012);
        if (tileInfo.y > 0.5) {
            // The phone's background picture, filling the glass (cropped, not stretched).
            float2 g = float2(rel.x / screenRect.z + 0.5, 0.5 - rel.y / screenRect.w);
            float glassAspect = screenRect.z / screenRect.w, picture = max(tileInfo.z, 1e-3);
            if (picture > glassAspect) g.x = 0.5 + (g.x - 0.5) * glassAspect / picture;
            else g.y = 0.5 + (g.y - 0.5) * picture / glassAspect;
            c = wallpaper.SampleLevel(linearClamp, g, 0).rgb * 0.85;  // a little dimmer, so the pieces stand out
        } else if (tileInfo.x > 0.5) {
            // The tablet without a picture: a soft blue, lighter at the top.
            float k = saturate(0.5 - rel.y / screenRect.w);
            c = lerp(float3(0.10, 0.27, 0.40), float3(0.03, 0.09, 0.16), k);
        }
        float2 m = (i.local - menuRect.xy) / menuRect.zw + 0.5;
        // Its rate of change on screen, so a texture with mips (Claude's window) picks the level
        // that fits the distance; one-level textures sample as before.
        float2 uvRate = lerp(crop.xy, crop.zw, float2(m.x, 1 - m.y));
        float2 uvDx = ddx(uvRate), uvDy = ddy(uvRate);
        if (misc.x > 0.5 && all(m >= 0) && all(m <= 1)) {
            float2 t = float2(m.x, 1 - m.y);
            float2 uv = lerp(crop.xy, crop.zw, t);
            bool shown = true;
            if (tileInfo.x > 0.5) {
                // The tablet: pieces of the UI laid side by side.
                shown = false;
                for (uint k = 0; k < (uint)tileInfo.x; ++k) {
                    float4 d = tileDst[k];
                    if (all(t >= d.xy) && all(t <= d.zw)) {
                        uv = lerp(tileSrc[k].xy, tileSrc[k].zw, (t - d.xy) / max(d.zw - d.xy, 1e-6));
                        shown = true;
                    }
                }
            }
            if (shown) {
                float4 u = ui.SampleGrad(linearClamp, uv, uvDx, uvDy);
                c = u.rgb + (1 - u.a) * c;
            }
        }
        float glare = pow(saturate(dot(reflect(-toEye, n), normalize(float3(0.3, 0.8, 0.5)))), 40) * 0.08;
        return float4(c + glare, 1);
    }
    // The body: dark graphite, lit from the camera and above, a light rim on the sides.
    float facing = saturate(dot(n, toEye));
    float above = saturate(dot(n, normalize(float3(0.2, 1, 0.4))));
    float3 base = i.kind > 2.5 ? float3(0.16, 0.16, 0.17) : float3(0.05, 0.05, 0.055);
    float3 c = base * (0.45 + 0.55 * facing + 0.4 * above);
    float spec = pow(saturate(dot(reflect(-toEye, n), normalize(float3(0.2, 1, 0.6)))), 24) * 0.25;
    if (i.kind > 1.5 && i.kind < 2.5) {
        // The front: the camera dot in the band above the glass.
        float2 cam = float2(screenRect.x, screenRect.y + screenRect.w * 0.5 + (misc.z + 0.004) * 0.5);
        if (length(i.local - cam) < 0.0018) c = float3(0.01, 0.01, 0.02);
    }
    return float4(c + spec, 1);
}
)";

struct Vertex {
    float pos[3];
    float normal[3];
    float kind;  // 0 back, 1 glass, 2 front, 3 sides
};

constexpr int kHandCapsules = 6;
struct Constants {
    float clip[4][4];
    float view[3][4];
    float screen[4], menu[4], crop[4], misc[4];
    float depthInfo[4];
    float capA[kHandCapsules][4], capB[kHandCapsules][4];
    float tileInfo[4];
    float tileDst[kMaxTiles][4], tileSrc[kMaxTiles][4];
};

std::mutex g_mutex;
Phone g_phone;
ID3D11ShaderResourceView* g_uiView = nullptr;
ID3D11Texture2D* g_uiTexture = nullptr;  // what g_uiView shows (not owned)
// The panel's own picture (a cut of the game's UI): the phone's app screen shows the HUD atlas in g_uiView.
ID3D11ShaderResourceView* g_panelView = nullptr;
ID3D11Texture2D* g_panelTexture = nullptr;
ID3D11ShaderResourceView* g_pieView = nullptr;  // the pie's: the UI, or a still of it while it flies off
ID3D11Texture2D* g_pieTexture = nullptr;        // what g_pieView shows (not owned)
// Speech bubbles and the like (bubbles.h): clear cards in the world, from their atlas.
Panel g_bubbles[kMaxBubbles];
int g_bubbleCount = 0;
ID3D11ShaderResourceView* g_bubbleView = nullptr;
ID3D11Texture2D* g_bubbleTexture = nullptr;  // what g_bubbleView shows (not owned)
// The guiding trail's footprint (trail.h): its texture, made once per device.
ID3D11ShaderResourceView* g_trailView = nullptr;
// Pieces of the UI on your arms (setPieces).
Panel g_pieces[kMaxPieces];
int g_pieceCount = 0;
ID3D11ShaderResourceView* g_pieceView = nullptr;
ID3D11Texture2D* g_pieceTexture = nullptr;
// The tablet's wallpaper: decoded pixels (any thread) and their texture (render thread).
std::mutex g_wallMutex;
std::vector<uint32_t> g_wallPixels;  // RGBA
int g_wallW = 0, g_wallH = 0, g_wallSerial = 0, g_wallMade = 0;
ID3D11Texture2D* g_wallTexture = nullptr;
ID3D11ShaderResourceView* g_wallView = nullptr;
ID3D11Device* g_device = nullptr;        // not owned: what our objects were made on
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psPie[2] = {};  // the pie over the world: single sample, multisample scene depth
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11DepthStencilState* g_alwaysWrite = nullptr;  // over everything, writing depth (setWriteDepth)
std::atomic<bool> g_writeDepth{false};
// Your arms and hands (camera.cpp, game thread): world segments and radii.
HandCapsule g_hands[kHandCapsules];
int g_handCount = 0;
ULONGLONG g_handsAt = 0;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_constants = nullptr;
ID3D11Buffer* g_vertices = nullptr;
UINT g_vertexCount = 0;
float g_meshSize[4] = {};  // width, height, depth, radius the mesh was made for
ID3D11SamplerState* g_sampler = nullptr;
ID3D11SamplerState* g_screenSampler = nullptr;  // Sims 1 / Claude screens: anisotropic, a touch sharp
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depthState[2] = {};  // normal, reversed
ID3D11DepthStencilState* g_depthWrite[2] = {};  // the same, writing depth (the phone slab, for what is drawn after it)
ID3D11BlendState* g_premultiplied = nullptr;    // clear panels over the world
bool g_failed = false;
std::atomic<ULONGLONG> g_drawnAt{0};
Panel g_panel;
ID3D11Buffer* g_panelVertices = nullptr;  // a unit square, z = 0
std::atomic<ULONGLONG> g_panelDrawnAt{0};
Panel g_pie;
std::atomic<ULONGLONG> g_pieDrawnAt{0};
Panel g_sims1;  // Sims Forever
std::atomic<ULONGLONG> g_sims1DrawnAt{0};
ID3D11Buffer* g_sims1Vertices = nullptr;  // the screen's shape (simsforever::surface), unit square across
UINT g_sims1VertexCount = 0;
int g_sims1Shape = -1;  // the surface version it was built for
float g_sims1Width = 0;  // and the panel width

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void releaseAll() {
    safeRelease(g_uiView);
    g_uiTexture = nullptr;
    safeRelease(g_panelView);
    g_panelTexture = nullptr;
    safeRelease(g_pieView);
    g_pieTexture = nullptr;
    safeRelease(g_bubbleView);
    g_bubbleTexture = nullptr;
    safeRelease(g_pieceView);
    g_pieceTexture = nullptr;
    safeRelease(g_trailView);
    safeRelease(g_vs);
    safeRelease(g_ps);
    safeRelease(g_psPie[0]);
    safeRelease(g_psPie[1]);
    safeRelease(g_noDepth);
    safeRelease(g_alwaysWrite);
    safeRelease(g_layout);
    safeRelease(g_constants);
    safeRelease(g_vertices);
    safeRelease(g_panelVertices);
    safeRelease(g_sampler);
    safeRelease(g_screenSampler);
    safeRelease(g_raster);
    safeRelease(g_depthState[0]);
    safeRelease(g_depthState[1]);
    safeRelease(g_depthWrite[0]);
    safeRelease(g_depthWrite[1]);
    safeRelease(g_premultiplied);
    g_vertexCount = 0;
    g_device = nullptr;
}

ID3DBlob* compile(pD3DCompile fn, const char* entry, const char* target, bool ms = false) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {nullptr, nullptr}};
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "phone3d", ms ? defines : nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOG("Phone 3D: %s failed to compile (0x%08lx): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        safeRelease(code);
    }
    safeRelease(errors);
    return code;
}

// Shaders and states, once per device. Under g_mutex.
bool init(ID3D11Device* device) {
    if (g_device == device && g_vs) return true;
    if (g_failed) return false;
    releaseAll();
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!fn) {
        LOG("Phone 3D: d3dcompiler_47.dll not available; the phone stays flat");
        g_failed = true;
        return false;
    }
    ID3DBlob* vs = compile(fn, "vsMain", "vs_5_0");
    ID3DBlob* ps = compile(fn, "psMain", "ps_5_0");
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"KIND", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    bool ok = vs && ps &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps)) &&
              SUCCEEDED(device->CreateInputLayout(layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout));
    safeRelease(vs);
    safeRelease(ps);
    for (int ms = 0; ms < 2 && ok; ++ms) {
        ID3DBlob* pie = compile(fn, "psPie", "ps_5_0", ms != 0);
        // Without it the pie keeps the depth test (hidden by whatever is nearer).
        if (pie) device->CreatePixelShader(pie->GetBufferPointer(), pie->GetBufferSize(), nullptr, &g_psPie[ms]);
        safeRelease(pie);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &g_constants));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(device->CreateSamplerState(&sd, &g_sampler));
    // Screens are seen at an angle and from afar: 16x anisotropic keeps text crisp across the
    // mip levels (Claude's window has them), the small bias leans sharper.
    D3D11_SAMPLER_DESC ss = sd;
    ss.Filter = D3D11_FILTER_ANISOTROPIC;
    ss.MaxAnisotropy = 16;
    ss.MipLODBias = -0.5f;
    if (FAILED(device->CreateSamplerState(&ss, &g_screenSampler))) g_screenSampler = nullptr;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    rd.AntialiasedLineEnable = FALSE;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&rd, &g_raster));
    for (int i = 0; i < 2 && ok; ++i) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;  // the scene's depth stays as the game made it
        dd.DepthFunc = i ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
        dd.StencilEnable = FALSE;
        ok = SUCCEEDED(device->CreateDepthStencilState(&dd, &g_depthState[i]));
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ok = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_depthWrite[i]));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ok = SUCCEEDED(device->CreateDepthStencilState(&dd, &g_noDepth));
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ok = ok && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_alwaysWrite));
    }
    if (ok) {
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(device->CreateBlendState(&bd, &g_premultiplied));
    }
    if (!ok) {
        LOG("Phone 3D: could not make its shaders or states; the phone stays flat");
        releaseAll();
        g_failed = true;
        return false;
    }
    g_device = device;
    LOG("Phone 3D: ready");
    return true;
}

// The rounded slab, in the phone's frame (metres, z out of the screen), and
// the glass just above its front. Under g_mutex.
bool buildMesh(ID3D11Device* device, const Phone& p) {
    float size[4] = {p.width, p.height, p.depth, p.radius};
    if (g_vertices && !std::memcmp(size, g_meshSize, sizeof(size))) return true;
    safeRelease(g_vertices);
    const int perCorner = 8;
    const float hw = p.width / 2, hh = p.height / 2, hd = p.depth / 2;
    const float r = std::fmin(p.radius, std::fmin(hw, hh) * 0.9f);
    std::vector<float> ox, oy, nx, ny;  // outline, counter-clockwise from the top right corner
    const float cx[4] = {hw - r, -(hw - r), -(hw - r), hw - r}, cy[4] = {hh - r, hh - r, -(hh - r), -(hh - r)};
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k <= perCorner; ++k) {
            float a = (c + static_cast<float>(k) / perCorner) * 1.5707963f;
            ox.push_back(cx[c] + r * std::cos(a));
            oy.push_back(cy[c] + r * std::sin(a));
            nx.push_back(std::cos(a));
            ny.push_back(std::sin(a));
        }
    const size_t n = ox.size();
    std::vector<Vertex> v;
    auto add = [&](float x, float y, float z, float a, float b, float c, float kind) {
        v.push_back({{x, y, z}, {a, b, c}, kind});
    };
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        // Front and back.
        add(0, 0, hd, 0, 0, 1, 2);
        add(ox[i], oy[i], hd, 0, 0, 1, 2);
        add(ox[j], oy[j], hd, 0, 0, 1, 2);
        add(0, 0, -hd, 0, 0, -1, 0);
        add(ox[j], oy[j], -hd, 0, 0, -1, 0);
        add(ox[i], oy[i], -hd, 0, 0, -1, 0);
        // The sides, their normals leaning toward front and back (rounded edges).
        const float lean = 0.45f, k = 1 / std::sqrt(1 + lean * lean);
        add(ox[i], oy[i], hd, nx[i] * k, ny[i] * k, lean * k, 3);
        add(ox[i], oy[i], -hd, nx[i] * k, ny[i] * k, -lean * k, 3);
        add(ox[j], oy[j], -hd, nx[j] * k, ny[j] * k, -lean * k, 3);
        add(ox[i], oy[i], hd, nx[i] * k, ny[i] * k, lean * k, 3);
        add(ox[j], oy[j], -hd, nx[j] * k, ny[j] * k, -lean * k, 3);
        add(ox[j], oy[j], hd, nx[j] * k, ny[j] * k, lean * k, 3);
    }
    // The glass, 0.3 mm above the front.
    const float sx = p.screen[0], sy = p.screen[1], sw = p.screen[2] / 2, sh = p.screen[3] / 2, z = hd + 0.0003f;
    add(sx - sw, sy - sh, z, 0, 0, 1, 1);
    add(sx + sw, sy - sh, z, 0, 0, 1, 1);
    add(sx + sw, sy + sh, z, 0, 0, 1, 1);
    add(sx - sw, sy - sh, z, 0, 0, 1, 1);
    add(sx + sw, sy + sh, z, 0, 0, 1, 1);
    add(sx - sw, sy + sh, z, 0, 0, 1, 1);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(Vertex));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{v.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, &g_vertices))) return false;
    g_vertexCount = static_cast<UINT>(v.size());
    std::memcpy(g_meshSize, size, sizeof(size));
    return true;
}

// The menu panel's mesh: a unit square facing +z. Under g_mutex.
bool buildPanelMesh(ID3D11Device* device) {
    if (g_panelVertices) return true;
    const Vertex v[6] = {{{-0.5f, -0.5f, 0}, {0, 0, 1}, 1}, {{0.5f, -0.5f, 0}, {0, 0, 1}, 1},
                         {{0.5f, 0.5f, 0}, {0, 0, 1}, 1},   {{-0.5f, -0.5f, 0}, {0, 0, 1}, 1},
                         {{0.5f, 0.5f, 0}, {0, 0, 1}, 1},   {{-0.5f, 0.5f, 0}, {0, 0, 1}, 1}};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(v);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{v, 0, 0};
    return SUCCEEDED(device->CreateBuffer(&bd, &init, &g_panelVertices));
}

// Sims 1's screen: a grid over the unit square, each point pushed back by the
// screen's depth there (world units / the panel's width, as the panel's z axis
// is scaled by its width), so the picture lies on a curved screen's glass.
bool buildSims1Mesh(ID3D11Device* device, float width) {
    int nx = 0, ny = 0;
    std::vector<float> depths;
    int version = simsforever::surface(nx, ny, depths);
    if (g_sims1Vertices && version == g_sims1Shape && std::fabs(width - g_sims1Width) < 1e-4f) return true;
    if (g_sims1Vertices) {
        g_sims1Vertices->Release();
        g_sims1Vertices = nullptr;
    }
    if (nx < 2 || ny < 2) {
        nx = ny = 2;
        depths.assign(4, 0.0f);
    }
    const float zs = width > 0 ? 1.0f / width : 0.0f;
    std::vector<Vertex> v;
    v.reserve((nx - 1) * (ny - 1) * 6);
    auto point = [&](int i, int j) {
        Vertex p{};
        p.pos[0] = static_cast<float>(i) / (nx - 1) - 0.5f;
        p.pos[1] = static_cast<float>(j) / (ny - 1) - 0.5f;
        p.pos[2] = depths[j * nx + i] * zs;
        p.normal[2] = 1;
        p.kind = 1;
        return p;
    };
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            Vertex a = point(i, j), b = point(i + 1, j), c = point(i + 1, j + 1), d = point(i, j + 1);
            v.insert(v.end(), {a, b, c, a, c, d});
        }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(Vertex));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{v.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, &g_sims1Vertices))) return false;
    g_sims1VertexCount = static_cast<UINT>(v.size());
    g_sims1Shape = version;
    g_sims1Width = width;
    return true;
}

DXGI_FORMAT viewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

camera::Vec3 rotate(const camera::Quat& q, const camera::Vec3& v) {
    // v + 2w(q x v) + 2 q x (q x v)
    camera::Vec3 u{q.x, q.y, q.z};
    camera::Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}
float dot(const camera::Vec3& a, const camera::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Every state draw touches, taken from the context and put back after.
struct Saved {
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat{};
    UINT ibOffset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11Buffer* vsCb = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4]{};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;

    void take(ID3D11DeviceContext* c) {
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 1, &vsCb);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetConstantBuffers(0, 1, &psCb);
        c->PSGetShaderResources(0, 1, &psSrv);
        c->PSGetSamplers(0, 1, &psSampler);
        c->RSGetState(&raster);
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetScissorRects(&scissorCount, scissors);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &dsv);
    }
    void put(ID3D11DeviceContext* c) {
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(ib, ibFormat, ibOffset);
        c->VSSetShader(vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &vsCb);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &psCb);
        c->PSSetShaderResources(0, 1, &psSrv);
        c->PSSetSamplers(0, 1, &psSampler);
        c->RSSetState(raster);
        c->RSSetViewports(viewportCount, viewports);
        c->RSSetScissorRects(scissorCount, scissors);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        UINT count = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (targets[i]) count = i + 1;
        c->OMSetRenderTargets(count, targets, dsv);
        safeRelease(layout);
        safeRelease(vb);
        safeRelease(ib);
        safeRelease(vs);
        safeRelease(vsCb);
        safeRelease(gs);
        safeRelease(hs);
        safeRelease(ds);
        safeRelease(ps);
        safeRelease(psCb);
        safeRelease(psSrv);
        safeRelease(psSampler);
        safeRelease(raster);
        safeRelease(blend);
        safeRelease(depth);
        for (auto*& t : targets) safeRelease(t);
        safeRelease(dsv);
    }
};

}  // namespace

// A shader view of `ui` in `view`, made again when the texture changes. Under g_mutex.
static void viewOf(ID3D11Texture2D* ui, ID3D11ShaderResourceView*& view, ID3D11Texture2D*& texture) {
    if (ui) {
        ID3D11Device* device = nullptr;
        ui->GetDevice(&device);
        if (device) {
            if (init(device) && ui != texture) {
                safeRelease(view);
                texture = nullptr;
                D3D11_TEXTURE2D_DESC desc;
                ui->GetDesc(&desc);
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = viewFormat(desc.Format);
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                if (SUCCEEDED(device->CreateShaderResourceView(ui, &sv, &view))) {
                    texture = ui;
                } else {
                    static int failed = 0;
                    if (failed++ < 3) LOG("Phone 3D: can't read the UI texture (format %d)", static_cast<int>(desc.Format));
                }
            }
            device->Release();
        }
    }
}

static void useUi(ID3D11Texture2D* ui) { viewOf(ui, g_uiView, g_uiTexture); }

void setPhone(const Phone& phone, ID3D11Texture2D* ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phone = phone;
    if (phone.show) useUi(ui);
}

void setPanel(const Panel& panel, ID3D11Texture2D* ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_panel = panel;
    if (panel.show) viewOf(ui, g_panelView, g_panelTexture);
}

bool panelDrawnRecently() { return GetTickCount64() - g_panelDrawnAt.load(std::memory_order_relaxed) < 200; }

void setPie(const Panel& pie, ID3D11Texture2D* ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pie = pie;
    if (pie.show) viewOf(ui, g_pieView, g_pieTexture);
}

bool pieDrawnRecently() { return GetTickCount64() - g_pieDrawnAt.load(std::memory_order_relaxed) < 200; }

void setBubbles(const Panel* bubbles, int count, ID3D11Texture2D* atlas) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bubbleCount = atlas && bubbles ? std::max(0, std::min(count, kMaxBubbles)) : 0;
    for (int i = 0; i < g_bubbleCount; ++i) g_bubbles[i] = bubbles[i];
    if (g_bubbleCount) viewOf(atlas, g_bubbleView, g_bubbleTexture);
    if (!g_bubbleView) g_bubbleCount = 0;
}

std::atomic<void (*)(Panel*, int)> g_piecePlacer{nullptr};

void setPiecePlacer(void (*placer)(Panel* pieces, int count)) { g_piecePlacer = placer; }

void setPieces(const Panel* pieces, int count, ID3D11Texture2D* ui) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pieceCount = ui && pieces ? std::max(0, std::min(count, kMaxPieces)) : 0;
    for (int i = 0; i < g_pieceCount; ++i) g_pieces[i] = pieces[i];
    if (g_pieceCount) viewOf(ui, g_pieceView, g_pieceTexture);
    if (!g_pieceView) g_pieceCount = 0;
}

void setSims1Screen(const Panel& screen) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sims1 = screen;
}

void setHands(const HandCapsule* capsules, int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handCount = std::min(count, kHandCapsules);
    for (int i = 0; i < g_handCount; ++i) g_hands[i] = capsules[i];
    g_handsAt = GetTickCount64();
}

bool sims1ScreenDrawnRecently() { return GetTickCount64() - g_sims1DrawnAt.load(std::memory_order_relaxed) < 200; }

// The wallpaper's texture from the latest pixels (render thread).
static void makeWallpaper(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(g_wallMutex);
    if (g_wallMade == g_wallSerial || g_wallPixels.empty()) return;
    g_wallMade = g_wallSerial;
    if (g_wallView) g_wallView->Release();
    if (g_wallTexture) g_wallTexture->Release();
    g_wallView = nullptr;
    g_wallTexture = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(g_wallW);
    td.Height = static_cast<UINT>(g_wallH);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // raw, as the UI is sampled
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{g_wallPixels.data(), static_cast<UINT>(g_wallW * 4), 0};
    if (SUCCEEDED(device->CreateTexture2D(&td, &init, &g_wallTexture)))
        device->CreateShaderResourceView(g_wallTexture, nullptr, &g_wallView);
    LOG("Tablet: phone background picture %dx%d %s", g_wallW, g_wallH, g_wallView ? "ready" : "couldn't be made");
}

// EA's DST textures are DDS files with each block's parts stored in separate
// runs (all the alpha ends, all the alpha indices, all the colour ends, all
// the colour indices); put the blocks back together as DXT1/3/5.
static bool unshuffleDst(std::vector<uint8_t>& dds) {
    if (dds.size() < 128) return false;
    char fourcc[5] = {};
    std::memcpy(fourcc, dds.data() + 84, 4);
    size_t offset = 128, n = dds.size() - offset;
    std::vector<uint8_t> out(n);
    const uint8_t* in = dds.data() + offset;
    if (!std::strcmp(fourcc, "DST1")) {
        size_t count = n / 8;
        for (size_t b = 0; b < count; ++b) {
            std::memcpy(&out[b * 8], in + b * 4, 4);
            std::memcpy(&out[b * 8 + 4], in + count * 4 + b * 4, 4);
        }
        std::memcpy(dds.data() + 84, "DXT1", 4);
    } else if (!std::strcmp(fourcc, "DST5") || !std::strcmp(fourcc, "DST3")) {
        size_t count = n / 16;
        // Streams in the file: alpha endpoints (2 bytes a block), colour
        // endpoints (4), alpha indices (6), colour indices (4) (s4pi
        // DSTResource.Unshuffle; v0.13.149 and before read them in the wrong
        // order: every DST5 icon came out as noise).
        const uint8_t *a0 = in, *c0 = in + count * 2, *a1 = c0 + count * 4, *c1 = a1 + count * 6;
        for (size_t b = 0; b < count; ++b) {
            std::memcpy(&out[b * 16], a0 + b * 2, 2);
            std::memcpy(&out[b * 16 + 2], a1 + b * 6, 6);
            std::memcpy(&out[b * 16 + 8], c0 + b * 4, 4);
            std::memcpy(&out[b * 16 + 12], c1 + b * 4, 4);
        }
        std::memcpy(dds.data() + 84, fourcc[3] == '5' ? "DXT5" : "DXT3", 4);
    } else {
        return true;  // not shuffled
    }
    std::memcpy(dds.data() + offset, out.data(), n);
    return true;
}

// An uncompressed DDS (24 or 32 bits a pixel, by its channel masks) as RGBA.
static bool decodeDdsRgb(const std::vector<uint8_t>& dds, uint32_t width, uint32_t height, std::vector<uint32_t>& rgba,
                         int& w, int& h) {
    uint32_t flags, bits, mask[4];
    std::memcpy(&flags, dds.data() + 80, 4);
    std::memcpy(&bits, dds.data() + 88, 4);
    std::memcpy(mask, dds.data() + 92, 16);
    if (!(flags & 0x40) || (bits != 32 && bits != 24) || width == 0 || height == 0 || width > 8192 || height > 8192)
        return false;
    const size_t step = bits / 8, pitch = width * step;
    if (dds.size() < 128 + pitch * height) return false;
    auto channel = [](uint32_t v, uint32_t m) -> uint32_t {
        if (!m) return 255;
        int shift = 0;
        while (!((m >> shift) & 1)) ++shift;
        const uint32_t top = m >> shift;
        return top ? ((v & m) >> shift) * 255 / top : 255;
    };
    const bool hasAlpha = (flags & 1) && mask[3];
    w = static_cast<int>(width);
    h = static_cast<int>(height);
    rgba.assign(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = dds.data() + 128 + y * pitch + x * step;
            uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (step == 4 ? static_cast<uint32_t>(p[3]) << 24 : 0);
            rgba[y * width + x] = channel(v, mask[0]) | channel(v, mask[1]) << 8 | channel(v, mask[2]) << 16 |
                                  (hasAlpha ? channel(v, mask[3]) : 255) << 24;
        }
    return true;
}

// The top level of a DXT1/3/5 (or uncompressed) DDS as RGBA.
static bool decodeDds(const std::vector<uint8_t>& dds, std::vector<uint32_t>& rgba, int& w, int& h) {
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) return false;
    uint32_t height, width;
    std::memcpy(&height, dds.data() + 12, 4);
    std::memcpy(&width, dds.data() + 16, 4);
    char fourcc[5] = {};
    std::memcpy(fourcc, dds.data() + 84, 4);
    if (!fourcc[0]) return decodeDdsRgb(dds, width, height, rgba, w, h);
    int kind = !std::strcmp(fourcc, "DXT1") ? 1 : !std::strcmp(fourcc, "DXT3") ? 3 : !std::strcmp(fourcc, "DXT5") ? 5 : 0;
    if (!kind || width == 0 || height == 0 || width > 8192 || height > 8192) return false;
    size_t bw = (width + 3) / 4, bh = (height + 3) / 4, blockSize = kind == 1 ? 8 : 16;
    if (dds.size() < 128 + bw * bh * blockSize) return false;
    w = static_cast<int>(width);
    h = static_cast<int>(height);
    rgba.assign(static_cast<size_t>(w) * h, 0);
    const uint8_t* data = dds.data() + 128;
    for (size_t by = 0; by < bh; ++by)
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + (by * bw + bx) * blockSize;
            uint8_t alpha[16];
            for (int i = 0; i < 16; ++i) alpha[i] = 255;
            if (kind == 3) {
                for (int i = 0; i < 16; ++i) alpha[i] = static_cast<uint8_t>(((blk[i / 2] >> (4 * (i % 2))) & 15) * 17);
            } else if (kind == 5) {
                int a[8] = {blk[0], blk[1]};
                if (a[0] > a[1])
                    for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                else {
                    for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = static_cast<uint8_t>(a[(bits >> (3 * i)) & 7]);
            }
            const uint8_t* col = blk + (kind == 1 ? 0 : 8);
            uint16_t e0 = static_cast<uint16_t>(col[0] | col[1] << 8), e1 = static_cast<uint16_t>(col[2] | col[3] << 8);
            int c[4][3];
            auto expand = [](uint16_t v, int* o) {
                o[0] = ((v >> 11) & 31) * 255 / 31;
                o[1] = ((v >> 5) & 63) * 255 / 63;
                o[2] = (v & 31) * 255 / 31;
            };
            expand(e0, c[0]);
            expand(e1, c[1]);
            bool four = kind != 1 || e0 > e1;
            for (int k = 0; k < 3; ++k) {
                c[2][k] = four ? (2 * c[0][k] + c[1][k]) / 3 : (c[0][k] + c[1][k]) / 2;
                c[3][k] = four ? (c[0][k] + 2 * c[1][k]) / 3 : 0;
            }
            uint32_t idx = static_cast<uint32_t>(col[4] | col[5] << 8 | col[6] << 16 | static_cast<uint32_t>(col[7]) << 24);
            for (int i = 0; i < 16; ++i) {
                size_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= width || y >= height) continue;
                int s = (idx >> (2 * i)) & 3;
                uint32_t a = (kind == 1 && !four && s == 3) ? 0 : alpha[i];
                rgba[y * width + x] = static_cast<uint32_t>(c[s][0]) | static_cast<uint32_t>(c[s][1]) << 8 |
                                      static_cast<uint32_t>(c[s][2]) << 16 | a << 24;
            }
        }
    return true;
}

// PNG (or anything else Windows can read) as RGBA.
static bool decodeWic(const uint8_t* data, size_t size, std::vector<uint32_t>& rgba, int& w, int& h) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                         reinterpret_cast<void**>(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) &&
              SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                              WICBitmapPaletteTypeCustom));
    UINT uw = 0, uh = 0;
    if (ok) ok = SUCCEEDED(converter->GetSize(&uw, &uh)) && uw > 0 && uh > 0 && uw <= 8192 && uh <= 8192;
    if (ok) {
        rgba.assign(static_cast<size_t>(uw) * uh, 0);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, uw * 4, uw * uh * 4, reinterpret_cast<BYTE*>(rgba.data())));
        w = static_cast<int>(uw);
        h = static_cast<int>(uh);
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

bool decodePicture(const uint8_t* data, size_t size, std::vector<uint32_t>& rgba, int& w, int& h, const char** kind) {
    const char* k = "unknown";
    bool ok;
    if (size >= 128 && std::memcmp(data, "DDS ", 4) == 0) {
        k = "DDS";
        std::vector<uint8_t> dds(data, data + size);
        ok = unshuffleDst(dds) && decodeDds(dds, rgba, w, h);
    } else {
        k = size >= 4 && std::memcmp(data, "\x89PNG", 4) == 0 ? "PNG" : "other";
        ok = decodeWic(data, size, rgba, w, h);
    }
    if (kind) *kind = k;
    return ok;
}

bool setWallpaper(const uint8_t* data, size_t size) {
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
    const char* kind = "unknown";
    bool ok = decodePicture(data, size, rgba, w, h, &kind);
    LOG("Tablet: phone background picture (%s, %zu bytes) %s", kind, size, ok ? "read" : "couldn't be read");
    if (!ok) return false;
    std::lock_guard<std::mutex> lock(g_wallMutex);
    g_wallPixels.swap(rgba);
    g_wallW = w;
    g_wallH = h;
    ++g_wallSerial;
    return true;
}

// The footprint picture (trail_art.h) as a premultiplied, softly golden
// texture with mips (seen flat on the floor from afar). Render thread.
static ID3D11ShaderResourceView* trailTexture(ID3D11Device* device) {
    if (g_trailView) return g_trailView;
    static bool failed = false;
    if (failed) return nullptr;
    using trail::kPrint;
    using trail::kPrintW;
    using trail::kPrintH;
    std::vector<std::vector<uint32_t>> levels;
    std::vector<float> alpha(kPrint, kPrint + kPrintW * kPrintH);
    for (auto& a : alpha) a /= 255.0f;
    int w = kPrintW, h = kPrintH;
    for (;;) {
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        for (int i = 0; i < w * h; ++i) {
            const float a = alpha[i] * 0.9f;  // a glow, not paint
            auto c = [&](float v) { return static_cast<uint32_t>(std::lround(std::min(1.0f, v * a) * 255.0f)); };
            px[i] = c(1.0f) | (c(0.86f) << 8) | (c(0.52f) << 16) | (static_cast<uint32_t>(std::lround(a * 255.0f)) << 24);
        }
        levels.push_back(std::move(px));
        if (w == 1 && h == 1) break;
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<float> next(static_cast<size_t>(nw) * nh);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                float sum = 0;
                int k = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                        sum += alpha[sy * w + sx];
                        ++k;
                    }
                next[y * nw + x] = sum / k;
            }
        alpha.swap(next);
        w = nw;
        h = nh;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kPrintW;
    td.Height = kPrintH;
    td.MipLevels = static_cast<UINT>(levels.size());
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
    for (size_t i = 0; i < levels.size(); ++i) {
        init[i].pSysMem = levels[i].data();
        init[i].SysMemPitch = static_cast<UINT>(std::max(1, kPrintW >> i) * 4);
    }
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&td, init.data(), &texture)) ||
        FAILED(device->CreateShaderResourceView(texture, nullptr, &g_trailView))) {
        LOG("Trail: could not make the footprint texture; no trail");
        failed = true;
        g_trailView = nullptr;
    }
    if (texture) texture->Release();
    return g_trailView;
}

// A panel placed in app space: into the world with this frame's camera.
static void fromApp(Panel& p, const camera::View& view) {
    if (!p.app) return;
    const float s = view.scale > 0 ? view.scale : 1.0f;
    camera::Vec3 c = rotate(view.heading, {p.appAt[0] * s, p.appAt[1] * s, p.appAt[2] * s});
    camera::Vec3 x = rotate(view.heading, {p.appX[0], p.appX[1], p.appX[2]});
    camera::Vec3 y = rotate(view.heading, {p.appY[0], p.appY[1], p.appY[2]});
    p.centre[0] = view.anchor.x + c.x;
    p.centre[1] = view.anchor.y + c.y;
    p.centre[2] = view.anchor.z + c.z;
    p.x[0] = x.x;
    p.x[1] = x.y;
    p.x[2] = x.z;
    p.y[0] = y.x;
    p.y[1] = y.y;
    p.y[2] = y.z;
    p.width = p.appW * s;
    p.height = p.appH * s;
}

void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth, bool reversed) {
    if (!context || !target || !depth) return;
    camera::View view;
    if (!camera::lastView(view)) return;
    std::unique_lock<std::mutex> lock(g_mutex);
    bool showPhone = g_phone.show, showPanel = g_panel.show, showPie = g_pie.show, showSims1 = g_sims1.show;
    int bubbleCount = g_bubbleCount, pieceCount = g_pieceCount;
    // The guiding trail (first person only): footprints from the route the game planned.
    Panel prints[trail::kMaxPrints];
    int printCount = camera::bodyActive() && trail::active()
                         ? trail::build(prints, trail::kMaxPrints, view.position.x, view.position.z, view.scale)
                         : 0;
    if (!showPhone && !showPanel && !showPie && !showSims1 && !bubbleCount && !pieceCount && !printCount) return;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device) device->Release();  // only used here, while the context holds it
    if (!device || !init(device)) return;
    if (showPhone && !buildMesh(device, g_phone)) showPhone = false;
    if ((showPanel || showPie || showSims1 || bubbleCount || pieceCount || printCount) && !buildPanelMesh(device)) {
        showPanel = showPie = showSims1 = false;
        bubbleCount = pieceCount = printCount = 0;
    }
    ID3D11ShaderResourceView* trailView = printCount ? trailTexture(device) : nullptr;
    if (!trailView) printCount = 0;
    if (!showPhone && !showPanel && !showPie && !showSims1 && !bubbleCount && !pieceCount && !printCount) return;
    if (showPhone && g_phone.wallpaper) makeWallpaper(device);
    const Phone p = g_phone;
    Panel q = g_panel, pie = g_pie;
    const Panel sims1 = g_sims1;
    fromApp(q, view);
    fromApp(pie, view);
    ID3D11ShaderResourceView* uiView = g_uiView;
    if (uiView) uiView->AddRef();
    ID3D11ShaderResourceView* panelView = showPanel ? g_panelView : nullptr;
    if (panelView) panelView->AddRef();
    ID3D11ShaderResourceView* pieView = showPie ? g_pieView : nullptr;
    if (pieView) pieView->AddRef();
    Panel bubbles[kMaxBubbles];
    std::copy(g_bubbles, g_bubbles + bubbleCount, bubbles);
    ID3D11ShaderResourceView* bubbleView = bubbleCount ? g_bubbleView : nullptr;
    if (bubbleView) bubbleView->AddRef();
    Panel pieces[kMaxPieces];
    std::copy(g_pieces, g_pieces + pieceCount, pieces);
    ID3D11ShaderResourceView* pieceView = pieceCount ? g_pieceView : nullptr;
    if (pieceView) pieceView->AddRef();
    HandCapsule hands[kHandCapsules];
    const int handCount = GetTickCount64() - g_handsAt < 300 ? g_handCount : 0;
    std::copy(g_hands, g_hands + handCount, hands);
    lock.unlock();
    if (auto placer = g_piecePlacer.load()) placer(pieces, pieceCount);

    // The target's size, for the viewport; the depth must match it.
    ID3D11Resource* resource = nullptr;
    target->GetResource(&resource);
    D3D11_TEXTURE2D_DESC td{};
    if (resource) {
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&td);
        resource->Release();
    }
    ID3D11Resource* depthResource = nullptr;
    depth->GetResource(&depthResource);
    D3D11_TEXTURE2D_DESC dd{};
    if (depthResource) {
        static_cast<ID3D11Texture2D*>(depthResource)->GetDesc(&dd);
        depthResource->Release();
    }
    if (!td.Width || dd.Width != td.Width || dd.Height != td.Height || dd.SampleDesc.Count != td.SampleDesc.Count) {
        static int mismatched = 0;
        if (mismatched++ < 5)
            LOG("Phone 3D: depth %ux%u x%u doesn't fit the view %ux%u x%u; not drawn", dd.Width, dd.Height,
                dd.SampleDesc.Count, td.Width, td.Height, td.SampleDesc.Count);
        if (uiView) uiView->Release();
        if (panelView) panelView->Release();
        if (pieView) pieView->Release();
        if (bubbleView) bubbleView->Release();
        if (pieceView) pieceView->Release();
        return;
    }

    const camera::Vec3 forward{-view.back.x, -view.back.y, -view.back.z};
    const float n = view.nearZ, f = view.farZ;
    const float A = reversed ? -n / (f - n) : f / (f - n), B = reversed ? f * n / (f - n) : -f * n / (f - n);
    // Frame (centre relative to the camera, axes; world units) to clip and view space.
    auto project = [&](const camera::Vec3& c, const camera::Vec3 axis[3], Constants& k) {
        auto row = [&](const camera::Vec3& dir, float out[4]) {
            out[0] = dot(axis[0], dir);
            out[1] = dot(axis[1], dir);
            out[2] = dot(axis[2], dir);
            out[3] = dot(c, dir);
        };
        float rx[4], ry[4], rd[4];
        row(view.right, rx);
        row(view.up, ry);
        row(forward, rd);
        for (int i = 0; i < 4; ++i) {
            k.clip[0][i] = (rx[i] - view.centreX * rd[i]) / view.tanX;  // w = rd; an off-centre frustum shifts x/w
            k.clip[1][i] = (ry[i] - view.centreY * rd[i]) / view.tanY;
            k.clip[2][i] = A * rd[i] + (i == 3 ? B : 0);
            k.clip[3][i] = rd[i];
            k.view[0][i] = rx[i];
            k.view[1][i] = ry[i];
            k.view[2][i] = -rd[i];
        }
        return rd[3];
    };
    Constants k{}, kPanel{};
    if (showPhone) {
        // The phone's frame in the world (relative to the camera), then in the camera's.
        const float s = view.scale;
        camera::Vec3 c, axis[3];
        camera::Vec3 wrist;
        camera::Quat grip;
        float handScale = s;
        bool pinned = armik::handPose(p.side, wrist, grip, handScale);
        if (pinned) {
            // Where the Sim's hand went on its latest pose (armik): its wrist, and
            // the grip frame the hand was turned to (the wrist sits 8 cm behind the
            // grip and 2 cm toward the back of the hand). The phone keeps its place
            // in that frame, so it stays in the Sim's fingers.
            // The left hand's frame mirrors the right's across X (m = -1).
            const float m = p.side ? 1.0f : -1.0f;
            camera::Vec3 rel{(p.grip[0] - 0.02f * m) * handScale, p.grip[1] * handScale, (p.grip[2] - 0.08f) * handScale};
            camera::Vec3 w = rotate(grip, rel);
            c = {wrist.x + w.x - view.position.x, wrist.y + w.y - view.position.y, wrist.z + w.z - view.position.z};
            camera::Vec3 ax = rotate(grip, {0, 0, m}), ay = rotate(grip, {0, 1, 0}), az = rotate(grip, {-m, 0, 0});
            float ct = std::cos(p.tilt), st = std::sin(p.tilt);
            camera::Vec3 ay2{ay.x * ct - az.x * st, ay.y * ct - az.y * st, ay.z * ct - az.z * st};
            camera::Vec3 az2{ay.x * st + az.x * ct, ay.y * st + az.y * ct, ay.z * st + az.z * ct};
            float cy = std::cos(p.yaw), sy = std::sin(p.yaw);
            camera::Vec3 ax3{ax.x * cy - az2.x * sy, ax.y * cy - az2.y * sy, ax.z * cy - az2.z * sy};
            camera::Vec3 az3{ax.x * sy + az2.x * cy, ax.y * sy + az2.y * cy, ax.z * sy + az2.z * cy};
            float cp = std::cos(p.pitch), sp = std::sin(p.pitch);  // then about its own left-right axis
            camera::Vec3 ay3{ay2.x * cp - az3.x * sp, ay2.y * cp - az3.y * sp, ay2.z * cp - az3.z * sp};
            az3 = {ay2.x * sp + az3.x * cp, ay2.y * sp + az3.y * cp, ay2.z * sp + az3.z * cp};
            ay2 = ay3;
            axis[0] = {ax3.x * handScale, ax3.y * handScale, ax3.z * handScale};
            axis[1] = {ay2.x * handScale, ay2.y * handScale, ay2.z * handScale};
            axis[2] = {az3.x * handScale, az3.y * handScale, az3.z * handScale};
        } else {
            c = rotate(view.heading, {p.centre[0] * s, p.centre[1] * s, p.centre[2] * s});
            c = {view.anchor.x + c.x - view.position.x, view.anchor.y + c.y - view.position.y,
                 view.anchor.z + c.z - view.position.z};
            const float* a[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i) axis[i] = rotate(view.heading, {a[i][0] * s, a[i][1] * s, a[i][2] * s});
        }
        static bool wasPinned = false;
        static int pinLogs = 0;
        if (pinned != wasPinned && pinLogs++ < 10) LOG("Phone 3D: %s", pinned ? "on the Sim's hand" : "on the controller (no hand pose)");
        wasPinned = pinned;
        float ahead = project(c, axis, k);
        std::memcpy(k.screen, p.screen, sizeof(k.screen));
        std::memcpy(k.menu, p.menu, sizeof(k.menu));
        std::memcpy(k.crop, p.crop, sizeof(k.crop));
        k.tileInfo[0] = static_cast<float>(std::max(0, std::min(kMaxTiles, p.tiles)));
        if (p.wallpaper && g_wallView && g_wallW > 0 && g_wallH > 0) {
            k.tileInfo[1] = 1;
            k.tileInfo[2] = static_cast<float>(g_wallW) / static_cast<float>(g_wallH);
        }
        std::memcpy(k.tileDst, p.tileDst, sizeof(k.tileDst));
        std::memcpy(k.tileSrc, p.tileSrc, sizeof(k.tileSrc));
        k.misc[0] = uiView ? 1.0f : 0.0f;
        k.misc[1] = p.radius;
        k.misc[2] = (p.width - p.screen[2]) / 2;
        static int logged = 0;
        if (logged++ < 3)
            LOG("Phone 3D: drawing into %ux%u x%u (format %d), near %.3f far %.1f, tan %.3f %.3f, depth %s, %.2f units ahead",
                td.Width, td.Height, td.SampleDesc.Count, static_cast<int>(td.Format), n, f, view.tanX, view.tanY,
                reversed ? "reversed" : "normal", ahead);
    }
    // A menu panel: the unit square stretched to its size, in the world.
    auto panelConstants = [&](const Panel& q, Constants& kPanel, ID3D11ShaderResourceView* uiView) {
        camera::Vec3 c{q.centre[0] - view.position.x, q.centre[1] - view.position.y, q.centre[2] - view.position.z};
        camera::Vec3 xs{q.x[0], q.x[1], q.x[2]}, ys{q.y[0], q.y[1], q.y[2]};
        camera::Vec3 zs{xs.y * ys.z - xs.z * ys.y, xs.z * ys.x - xs.x * ys.z, xs.x * ys.y - xs.y * ys.x};
        camera::Vec3 axis[3] = {{xs.x * q.width, xs.y * q.width, xs.z * q.width},
                                {ys.x * q.height, ys.y * q.height, ys.z * q.height},
                                {zs.x * q.width, zs.y * q.width, zs.z * q.width}};
        project(c, axis, kPanel);
        const float whole[4] = {0, 0, 1, 1};
        std::memcpy(kPanel.screen, whole, sizeof(whole));
        std::memcpy(kPanel.menu, q.menu, sizeof(kPanel.menu));
        std::memcpy(kPanel.crop, q.crop, sizeof(kPanel.crop));
        kPanel.misc[0] = uiView ? 1.0f : 0.0f;
        kPanel.misc[1] = 0.02f;  // corner, of the unit square
        kPanel.misc[3] = q.clear ? std::max(q.opacity, 0.001f) : 0.0f;
        if (q.clear) kPanel.misc[2] = q.round ? 1.0f : q.key > 0 ? 2.0f + std::min(q.key, 1.0f) : 0.0f;
    };
    Constants kPie{}, kSims1{};
    // Sims 1's picture (on this thread: the upload uses the game's context).
    ID3D11ShaderResourceView* sims1View = showSims1 ? simsforever::frameView(context) : nullptr;
    if (showSims1) panelConstants(sims1, kSims1, sims1View);
    if (showSims1 && !buildSims1Mesh(device, sims1.width)) showSims1 = false;
    if (showPanel) panelConstants(q, kPanel, panelView);
    if (showPie) panelConstants(pie, kPie, pieView);
    // The pie in front of the world, your arms and hands excepted (psPie): with
    // this frame's scene depth and your arms in view space; else depth-tested.
    // A clear panel marked onTop (a popup in front of you) the same way.
    const bool panelTop = showPanel && q.clear && q.onTop;
    UINT depthSamples = 0;
    ID3D11ShaderResourceView* sceneDepth =
        ((showPie && pie.clear) || panelTop) && handCount ? nearplane::sceneDepth(depthSamples) : nullptr;
    ID3D11PixelShader* piePs = sceneDepth ? g_psPie[depthSamples > 1 ? 1 : 0] : nullptr;
    if (sceneDepth && !piePs) {
        sceneDepth->Release();
        sceneDepth = nullptr;
    }
    auto fillDepth = [&](Constants& kd) {
        kd.depthInfo[0] = n;
        kd.depthInfo[1] = f;
        kd.depthInfo[2] = reversed ? 1.0f : 0.0f;
        kd.depthInfo[3] = static_cast<float>(handCount);
        auto toView = [&](const float w[3], float out[4]) {
            const camera::Vec3 d{w[0] - view.position.x, w[1] - view.position.y, w[2] - view.position.z};
            out[0] = dot(d, view.right);
            out[1] = dot(d, view.up);
            out[2] = -dot(d, forward);
        };
        for (int i = 0; i < handCount; ++i) {
            toView(hands[i].a, kd.capA[i]);
            toView(hands[i].b, kd.capB[i]);
            kd.capA[i][3] = hands[i].radius;
        }
    };
    const bool piePsForPie = piePs && showPie && pie.clear, piePsForPanel = piePs && panelTop;
    if (piePsForPie) {
        fillDepth(kPie);
        static int logged = 0;
        if (logged++ < 3) LOG("Pie: drawn in front of the world, your %d arm and hand pieces in front of it", handCount);
    }
    if (piePsForPanel) fillDepth(kPanel);

    Saved saved;
    saved.take(context);
    ID3D11ShaderResourceView* savedSlot1 = nullptr;  // psPie's scene depth slot, as the game left it
    if (sceneDepth) context->PSGetShaderResources(1, 1, &savedSlot1);
    UINT stride = sizeof(Vertex), offset = 0;
    context->IASetInputLayout(g_layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &g_constants);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(g_ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_constants);
    context->PSSetShaderResources(0, 1, &uiView);
    ID3D11ShaderResourceView* savedSlot2 = nullptr;  // the wallpaper's slot, as the game left it
    context->PSGetShaderResources(2, 1, &savedSlot2);
    context->PSSetShaderResources(2, 1, &g_wallView);
    context->PSSetSamplers(0, 1, &g_sampler);
    context->RSSetState(g_raster);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1};
    context->RSSetViewports(1, &vp);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    // Depth eye (deptheye.h): every piece writes its depth, so the other eye
    // made from this picture puts it where it is, not where the wall behind it is.
    const bool writeDepth = g_writeDepth.load(std::memory_order_relaxed) && g_alwaysWrite && g_depthWrite[reversed ? 1 : 0];
    ID3D11DepthStencilState* tested = writeDepth ? g_depthWrite[reversed ? 1 : 0] : g_depthState[reversed ? 1 : 0];
    ID3D11DepthStencilState* over = writeDepth ? g_alwaysWrite : g_noDepth;
    ID3D11DepthStencilView* overDepth = writeDepth ? depth : nullptr;
    context->OMSetDepthStencilState(tested, 0);
    context->OMSetRenderTargets(1, &target, depth);
    if (showPhone) {
        // The phone is solid: it writes its depth when floor footprints or
        // bubbles come after it, so they don't show through it.
        const bool solid = (printCount || bubbleCount) && g_depthWrite[reversed ? 1 : 0];
        if (solid) context->OMSetDepthStencilState(g_depthWrite[reversed ? 1 : 0], 0);
        context->UpdateSubresource(g_constants, 0, nullptr, &k, 0, 0);
        context->IASetVertexBuffers(0, 1, &g_vertices, &stride, &offset);
        context->Draw(g_vertexCount, 0);
        if (solid) context->OMSetDepthStencilState(tested, 0);
    }
    if (showSims1) {
        context->PSSetShaderResources(0, 1, &sims1View);
        if (g_screenSampler) context->PSSetSamplers(0, 1, &g_screenSampler);
        context->UpdateSubresource(g_constants, 0, nullptr, &kSims1, 0, 0);
        context->IASetVertexBuffers(0, 1, &g_sims1Vertices, &stride, &offset);
        context->Draw(g_sims1VertexCount, 0);
        context->PSSetShaderResources(0, 1, &uiView);
        context->PSSetSamplers(0, 1, &g_sampler);
    }
    if (showPanel) {
        if (q.clear) context->OMSetBlendState(g_premultiplied, nullptr, 0xffffffff);
        if (panelTop) {
            // In front of the world (a popup in front of you): your hands still in front of it.
            if (piePsForPanel) {
                context->PSSetShader(piePs, nullptr, 0);
                context->PSSetShaderResources(1, 1, &sceneDepth);
                context->OMSetRenderTargets(1, &target, overDepth);  // the depth copy is read, the scene's bound only to be written
            }
            context->OMSetDepthStencilState(over, 0);
        }
        context->PSSetShaderResources(0, 1, &panelView);
        context->UpdateSubresource(g_constants, 0, nullptr, &kPanel, 0, 0);
        context->IASetVertexBuffers(0, 1, &g_panelVertices, &stride, &offset);
        context->Draw(6, 0);
        context->PSSetShaderResources(0, 1, &uiView);
        if (panelTop) {
            context->PSSetShader(g_ps, nullptr, 0);
            context->OMSetDepthStencilState(tested, 0);
            context->OMSetRenderTargets(1, &target, depth);
        }
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    if (printCount) {
        // The guiding trail: footprints lying on the floor, hidden by anything
        // nearer (walls, furniture, your own body), softly blended over it.
        context->OMSetBlendState(g_premultiplied, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &trailView);
        context->IASetVertexBuffers(0, 1, &g_panelVertices, &stride, &offset);
        for (int j = 0; j < printCount; ++j) {
            Constants kPrint{};
            panelConstants(prints[j], kPrint, trailView);
            context->UpdateSubresource(g_constants, 0, nullptr, &kPrint, 0, 0);
            context->Draw(6, 0);
        }
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &uiView);
        static int logged = 0;
        if (logged++ < 3) LOG("Trail: %d footprints drawn", printCount);
    }
    if (bubbleView) {
        // Speech bubbles and the like: over what is behind them, hidden by
        // anything nearer (walls, other Sims), farthest first.
        int order[kMaxBubbles];
        float ahead[kMaxBubbles];
        for (int i = 0; i < bubbleCount; ++i) {
            order[i] = i;
            ahead[i] = (bubbles[i].centre[0] - view.position.x) * forward.x +
                       (bubbles[i].centre[1] - view.position.y) * forward.y +
                       (bubbles[i].centre[2] - view.position.z) * forward.z;
        }
        std::sort(order, order + bubbleCount, [&](int a, int b) { return ahead[a] > ahead[b]; });
        context->OMSetBlendState(g_premultiplied, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &bubbleView);
        context->IASetVertexBuffers(0, 1, &g_panelVertices, &stride, &offset);
        for (int j = 0; j < bubbleCount; ++j) {
            Constants kBubble{};
            panelConstants(bubbles[order[j]], kBubble, bubbleView);
            context->UpdateSubresource(g_constants, 0, nullptr, &kBubble, 0, 0);
            context->Draw(6, 0);
        }
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &uiView);
        static int logged = 0;
        if (logged++ < 3) LOG("Bubbles: %d drawn in the world", bubbleCount);
    }
    if (pieceView) {
        // Pieces of the UI on your arms: depth-tested (your arm's own surface
        // is just under them), the ones marked onTop over the world.
        context->OMSetBlendState(g_premultiplied, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &pieceView);
        context->IASetVertexBuffers(0, 1, &g_panelVertices, &stride, &offset);
        for (int j = 0; j < pieceCount; ++j) {
            Constants kPiece{};
            panelConstants(pieces[j], kPiece, pieceView);
            context->OMSetDepthStencilState(pieces[j].onTop ? over : tested, 0);
            context->UpdateSubresource(g_constants, 0, nullptr, &kPiece, 0, 0);
            context->Draw(6, 0);
        }
        context->OMSetDepthStencilState(tested, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->PSSetShaderResources(0, 1, &uiView);
        static int logged = 0;
        if (logged++ < 3) LOG("Wrist pieces: %d drawn", pieceCount);
    }
    if (showPie) {
        // Last, blended over what is behind it; anything nearer (your hands)
        // already in the depth stays in front.
        if (pie.clear) context->OMSetBlendState(g_premultiplied, nullptr, 0xffffffff);
        if (piePsForPie) {
            context->PSSetShader(piePs, nullptr, 0);
            context->PSSetShaderResources(1, 1, &sceneDepth);
            context->OMSetDepthStencilState(over, 0);
            context->OMSetRenderTargets(1, &target, overDepth);  // the depth copy is read, the scene's bound only to be written
        }
        context->PSSetShaderResources(0, 1, &pieView);
        context->UpdateSubresource(g_constants, 0, nullptr, &kPie, 0, 0);
        context->IASetVertexBuffers(0, 1, &g_panelVertices, &stride, &offset);
        context->Draw(6, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->PSSetShaderResources(2, 1, &savedSlot2);
    if (savedSlot2) savedSlot2->Release();
    if (sceneDepth) {
        context->PSSetShaderResources(1, 1, &savedSlot1);
        if (savedSlot1) savedSlot1->Release();
        sceneDepth->Release();
    }
    saved.put(context);
    if (uiView) uiView->Release();
    if (panelView) panelView->Release();
    if (pieView) pieView->Release();
    if (bubbleView) bubbleView->Release();
    if (pieceView) pieceView->Release();
    if (showPhone) g_drawnAt.store(GetTickCount64(), std::memory_order_relaxed);
    if (showPanel) g_panelDrawnAt.store(GetTickCount64(), std::memory_order_relaxed);
    if (showPie) g_pieDrawnAt.store(GetTickCount64(), std::memory_order_relaxed);
    if (showSims1) g_sims1DrawnAt.store(GetTickCount64(), std::memory_order_relaxed);
}

bool drawnRecently() { return GetTickCount64() - g_drawnAt.load(std::memory_order_relaxed) < 200; }

void setWriteDepth(bool on) { g_writeDepth.store(on, std::memory_order_relaxed); }

bool writesDepth() { return g_writeDepth.load(std::memory_order_relaxed); }

}  // namespace s4vr::phone3d
