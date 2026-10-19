#include "nearplane.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#include "log.h"

namespace s4vr::nearplane {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kShare = 0.6f;             // the near limit sits at this share of the nearest thing seen
constexpr float kDrop = 0.85f;             // dropping: this far under the new target, so it doesn't hover at it
constexpr float kRise = 1.25f;             // rising: at most by this factor ...
constexpr ULONGLONG kRiseEvery = 100;      // ... this often (ms), and only toward a target this much higher
constexpr ULONGLONG kFresh = 400;          // a measurement this old (ms) still counts as fresh; older: minNear
constexpr ULONGLONG kRemember = 1500;      // what was seen close keeps the limit down this long (ms) ...
constexpr float kSlack = 0.05f;            // ... less how far you moved since, and this (metres)
constexpr float kTouch = 1.15f;            // nearest point this close to the near limit: something is cut off ...
constexpr ULONGLONG kTouchHold = 300;      // ... so minNear for this long (ms), until it is measured whole
constexpr float kJump = 0.5f;              // metres in one frame: a teleport; forget what was seen
constexpr float kSnapCos = 0.866f;         // more than 30 degrees in one frame: a snap turn
constexpr ULONGLONG kJumpHold = 300;       // either: minNear for this long (ms)
constexpr ULONGLONG kActive = 500;         // sense measures while choose ran this lately (ms)
constexpr int kSlots = 4;                  // read-back copies in flight
constexpr int kSeen = 24;
constexpr int kNearHistory = 16;
constexpr ULONGLONG kLogEvery = 10000;

// The nearest point of the 3D view: each of 256x256 threads takes the minimum
// of its tile, each group of 16x16 reduces those, then one atomic per group.
// Depths are 0..1 floats, which order like their bits. Points close enough to
// matter that lie on your own body (capsules in view space, x right, y up, z
// ahead) are left out.
const char kShader[] = R"(
#ifdef MS
Texture2DMS<float> depthTex : register(t0);
#define LOAD(p) depthTex.Load(p, 0)
#else
Texture2D<float> depthTex : register(t0);
#define LOAD(p) depthTex.Load(int3(p, 0))
#endif
RWByteAddressBuffer result : register(u0);
cbuffer Params : register(b0) {
    uint2 size;
    uint tag;
    uint reversed;  // 1: the scene clears depth to 0 (near is 1)
    float tanX, tanY, nearLow, nearHigh;  // the view; the near limits it may have been drawn with
    float farZ, considerZ;  // farther than considerZ: never your own body (it can't matter)
    uint count, pad;
    float4 centres;  // the frustum centre of this eye (xy) and of the one before (zw): the depth is one of them
    float4 capA[24];  // your body: segment start, radius
    float4 capB[24];  // segment end
};
groupshared uint nearest[256];

float viewZ(float d, float n) {
    return reversed ? n * farZ / (n + d * (farZ - n)) : n * farZ / (farZ - d * (farZ - n));
}

bool onBody(float3 q) {
    for (uint i = 0; i < count; ++i) {
        float3 a = capA[i].xyz, ab = capB[i].xyz - a;
        float t = saturate(dot(q - a, ab) / max(dot(ab, ab), 1e-8));
        float3 c = a + t * ab - q;
        if (dot(c, c) < capA[i].w * capA[i].w) return true;
    }
    return false;
}

bool ownBody(int2 p, float d) {
    float zl = viewZ(d, nearLow);
    if (!(zl < considerZ)) return false;
    float zh = viewZ(d, nearHigh);
    float2 uv = (float2(p) + 0.5) / float2(size);
    uv = float2(uv.x * 2 - 1, 1 - uv.y * 2) * float2(tanX, tanY);
    float2 a = uv + centres.xy, b = uv + centres.zw;
    if (onBody(float3(a * zl, zl)) || onBody(float3(a * zh, zh))) return true;
    return any(centres.xy != centres.zw) && (onBody(float3(b * zl, zl)) || onBody(float3(b * zh, zh)));
}

[numthreads(16, 16, 1)]
void csMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
    uint2 tile = (size + 255) / 256;
    uint2 start = (group.xy * 16 + thread.xy) * tile;
    uint m = 0x3f800000;
    for (uint y = 0; y < tile.y; ++y) {
        for (uint x = 0; x < tile.x; ++x) {
            int2 p = int2(start + uint2(x, y));
            if (p.x < (int)size.x && p.y < (int)size.y) {
                float d = LOAD(p);
                float n = saturate(reversed ? 1 - d : d);
                if (n < 1 && count > 0 && ownBody(p, d)) continue;
                m = min(m, asuint(n));
            }
        }
    }
    nearest[index] = m;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1) {
        if (index < s) nearest[index] = min(nearest[index], nearest[index + s]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        uint previous;
        result.InterlockedMin(0, nearest[0], previous);
        if (group.x == 0 && group.y == 0) result.Store(8, tag);
    }
}
)";

struct Params {
    uint32_t width, height, tag, reversed;
    float tanX, tanY, nearLow, nearHigh;
    float farZ, considerZ;
    uint32_t count, pad;
    float centres[4];
    float capA[kMaxCapsules][4];
    float capB[kMaxCapsules][4];
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

// One measurement in flight: the read-back copy, and the camera it was made with.
struct Slot {
    ID3D11Buffer* staging = nullptr;
    uint32_t tag = 0;
    bool pending = false;
    bool reversed = false;
    bool bodyLeftOut = false;
    int waits = 0;
    float nearLow = 0, nearHigh = 0, farZ = 0;
    float eye[3]{};
};

// A measured frame: the nearest point's view depth (a lower bound on how far
// it is from the eye), and where the eye was.
struct Seen {
    ULONGLONG tick = 0;
    float z = kInf;
    float eye[3]{};
    bool touch = false;  // something reached the near limit: cut off, its real distance unknown
};

struct NearAt {
    ULONGLONG tick = 0;
    float value = 0;
};

std::mutex g_mutex;  // everything below

// From ownBody (game thread).
Capsule g_body[kMaxCapsules];
int g_bodyCount = 0;
float g_bodyEye[3]{}, g_bodyOtherEye[3]{};  // this frame's eye and the one before (likely the other eye)
float g_bodyCentre[2]{}, g_bodyOtherCentre[2]{};  // their frustum centres
float g_bodyAxes[3][3]{};                    // right, up, ahead
float g_bodyTan[2]{};
ULONGLONG g_bodyTick = 0;

// From choose (game thread).
ULONGLONG g_activeTick = 0;
float g_maxNear = 0.75f;
float g_eye[3]{};
float g_far = 1000;
NearAt g_nearHistory[kNearHistory];
int g_nearNext = 0;
float g_current = 0;
ULONGLONG g_lastRise = 0, g_holdUntil = 0;
float g_prevEye[3]{}, g_prevForward[3]{};
bool g_prevValid = false;

// From collect (Present thread).
Seen g_seen[kSeen];
int g_seenNext = 0;

// GPU side, made by sense (render thread) on the game's device.
ID3D11Device* g_device = nullptr;  // not owned: what the resources below were made on
ID3D11ComputeShader* g_shader[2] = {};  // single sample, multisample
ID3D11Buffer* g_params = nullptr;
ID3D11Buffer* g_result = nullptr;
ID3D11UnorderedAccessView* g_resultView = nullptr;
ID3D11Texture2D* g_copy = nullptr;  // the scene depth, copied so a shader may read it
ID3D11ShaderResourceView* g_copyView = nullptr;
D3D11_TEXTURE2D_DESC g_copyDesc{};
ULONGLONG g_copiedAt = 0;  // when sense last copied the scene depth
bool g_copyReversed = false;  // that copy clears to 0 (near is 1)
Slot g_slots[kSlots];
int g_nextSlot = 0;
uint32_t g_tag = 0;
bool g_failed = false;

// For the log.
struct Stats {
    ULONGLONG since = 0;
    float low = kInf, high = 0;
    float nearestSeen = kInf, nearestBody = kInf;
    uint32_t frames = 0, measured = 0, touches = 0, stale = 0, held = 0, dropped = 0, jumps = 0, bodyLeftOut = 0, headHeld = 0;
} g_stats;

template <typename T>
void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

float distance(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void forget() {
    for (auto& s : g_seen) s = Seen{};
}

// The near limits used over the last moments: a frame read back now was
// rendered with one of them.
void nearRange(ULONGLONG now, float& low, float& high) {
    low = kInf;
    high = 0;
    for (const auto& n : g_nearHistory) {
        if (!n.tick || now - n.tick > 250) continue;
        low = std::min(low, n.value);
        high = std::max(high, n.value);
    }
    if (low == kInf) low = high = g_current;
}

void releaseCopy() {
    safeRelease(g_copyView);
    safeRelease(g_copy);
    g_copyDesc = {};
}

void releaseAll() {
    releaseCopy();
    for (auto*& s : g_shader) safeRelease(s);
    safeRelease(g_params);
    safeRelease(g_resultView);
    safeRelease(g_result);
    for (auto& slot : g_slots) {
        safeRelease(slot.staging);
        slot = Slot{};
    }
    g_device = nullptr;
}

// The scene depth's typeless format and the one a shader reads its depth as.
bool depthFormats(DXGI_FORMAT format, DXGI_FORMAT& typeless, DXGI_FORMAT& read) {
    switch (format) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            typeless = DXGI_FORMAT_R24G8_TYPELESS;
            read = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            return true;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            return true;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            typeless = DXGI_FORMAT_R32_TYPELESS;
            read = DXGI_FORMAT_R32_FLOAT;
            return true;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
            typeless = DXGI_FORMAT_R16_TYPELESS;
            read = DXGI_FORMAT_R16_UNORM;
            return true;
        default: return false;
    }
}

// Shaders and buffers, once per device. Under g_mutex.
bool init(ID3D11Device* device) {
    if (g_device == device && g_shader[0]) return true;
    if (g_failed) return false;
    releaseAll();
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        LOG("Near limit: d3dcompiler_47.dll not available; the near limit stays at near_clip");
        g_failed = true;
        return false;
    }
    bool ok = true;
    for (int ms = 0; ms < 2 && ok; ++ms) {
        const D3D_SHADER_MACRO defines[] = {{"MS", "1"}, {nullptr, nullptr}};
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(kShader, sizeof(kShader) - 1, "nearplane", ms ? defines : nullptr, nullptr, "csMain",
                             "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            LOG("Near limit: shader failed to compile (0x%08lx): %s", static_cast<unsigned long>(hr),
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            ok = false;
        } else {
            ok = SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                       &g_shader[ms]));
        }
        safeRelease(code);
        safeRelease(errors);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, &g_params));
    D3D11_BUFFER_DESC rb{};
    rb.ByteWidth = 16;
    rb.Usage = D3D11_USAGE_DEFAULT;
    rb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    rb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ok = ok && SUCCEEDED(device->CreateBuffer(&rb, nullptr, &g_result));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uv.Buffer.FirstElement = 0;
    uv.Buffer.NumElements = 4;
    uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ok = ok && SUCCEEDED(device->CreateUnorderedAccessView(g_result, &uv, &g_resultView));
    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 16;
    sb.Usage = D3D11_USAGE_STAGING;
    sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& slot : g_slots) ok = ok && SUCCEEDED(device->CreateBuffer(&sb, nullptr, &slot.staging));
    if (!ok) {
        LOG("Near limit: could not make its shader or buffers; the near limit stays at near_clip");
        releaseAll();
        g_failed = true;
        return false;
    }
    g_device = device;
    LOG("Near limit: ready (follows the nearest thing in view, %.0f%% of it)", kShare * 100);
    return true;
}

// Our copy of the scene depth, made to match it. Under g_mutex.
bool prepareCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& scene) {
    if (g_copy && g_copyDesc.Width == scene.Width && g_copyDesc.Height == scene.Height &&
        g_copyDesc.SampleDesc.Count == scene.SampleDesc.Count && g_copyDesc.SampleDesc.Quality == scene.SampleDesc.Quality &&
        g_copyDesc.MipLevels == scene.MipLevels && g_copyDesc.ArraySize == scene.ArraySize) {
        DXGI_FORMAT typeless, read;
        if (depthFormats(scene.Format, typeless, read) && typeless == g_copyDesc.Format) return true;
    }
    releaseCopy();
    static int logged = 0;
    if (logged++ < 5)
        LOG("Near limit: the scene depth is %ux%u x%u, format %d, bind 0x%x, %u mips", scene.Width, scene.Height,
            scene.SampleDesc.Count, static_cast<int>(scene.Format), scene.BindFlags, scene.MipLevels);
    DXGI_FORMAT typeless, read;
    if (!depthFormats(scene.Format, typeless, read) || scene.ArraySize != 1 || scene.MipLevels != 1) {
        LOG("Near limit: can't read that depth; the near limit stays at near_clip");
        g_failed = true;
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = scene;
    desc.Format = typeless;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = read;
    if (scene.SampleDesc.Count > 1) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
    }
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_copy)) ||
        FAILED(device->CreateShaderResourceView(g_copy, &sv, &g_copyView))) {
        LOG("Near limit: could not copy the scene depth; the near limit stays at near_clip");
        releaseCopy();
        g_failed = true;
        return false;
    }
    g_copyDesc = desc;
    return true;
}

// Distance along the view of a depth-buffer value.
float viewDepth(float d, float n, float f, bool reversed) {
    if (!(n > 0) || !(f > n)) return 0;
    if (reversed) return d <= 0 ? kInf : n * f / (n + d * (f - n));
    return d >= 1 ? kInf : n * f / (f - d * (f - n));
}

}  // namespace

void ownBody(const float eye[3], const float right[3], const float up[3], const float ahead[3], float tanX,
             float tanY, float centreX, float centreY, const Capsule* capsules, int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(g_bodyOtherEye, g_bodyEye, sizeof(g_bodyEye));
    std::memcpy(g_bodyEye, eye, sizeof(g_bodyEye));
    std::memcpy(g_bodyOtherCentre, g_bodyCentre, sizeof(g_bodyCentre));
    g_bodyCentre[0] = centreX;
    g_bodyCentre[1] = centreY;
    std::memcpy(g_bodyAxes[0], right, sizeof(g_bodyAxes[0]));
    std::memcpy(g_bodyAxes[1], up, sizeof(g_bodyAxes[1]));
    std::memcpy(g_bodyAxes[2], ahead, sizeof(g_bodyAxes[2]));
    g_bodyTan[0] = tanX;
    g_bodyTan[1] = tanY;
    g_bodyCount = capsules && tanX > 0 && tanY > 0 ? std::min(std::max(count, 0), kMaxCapsules) : 0;
    if (g_bodyCount) std::memcpy(g_body, capsules, sizeof(Capsule) * g_bodyCount);
    g_bodyTick = GetTickCount64();
}

float choose(const float eye[3], const float forward[3], float body, float head, float farZ, float minNear,
             float maxNear) {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!(minNear > 0)) minNear = 0.08f;
    if (!(maxNear >= minNear)) maxNear = minNear;
    g_maxNear = maxNear;
    if (!(body == body)) body = 0;  // NaN: not known
    // First person just started (or came back): start from the bottom.
    if (!g_activeTick || now - g_activeTick > 1000) {
        forget();
        g_current = minNear;
        g_prevValid = false;
        g_holdUntil = now + kJumpHold;
    }
    g_activeTick = now;
    std::memcpy(g_eye, eye, sizeof(g_eye));
    if (farZ > 0) g_far = farZ;
    if (g_prevValid) {
        float turned = forward[0] * g_prevForward[0] + forward[1] * g_prevForward[1] + forward[2] * g_prevForward[2];
        if (distance(eye, g_prevEye) > kJump) {
            forget();
            g_holdUntil = now + kJumpHold;
            ++g_stats.jumps;
        } else if (turned < kSnapCos) {
            g_holdUntil = now + kJumpHold;
            ++g_stats.jumps;
        }
    }
    std::memcpy(g_prevEye, eye, sizeof(g_prevEye));
    std::memcpy(g_prevForward, forward, sizeof(g_prevForward));
    g_prevValid = true;

    // How close things may be now: everything seen lately, nearer by however
    // far you moved since; anything cut off by the near limit counts as touching.
    float bound = kInf;
    bool fresh = false;
    for (const auto& s : g_seen) {
        if (!s.tick) continue;
        ULONGLONG age = now - s.tick;
        if (age <= kFresh) fresh = true;
        if (s.touch) {
            if (age <= kTouchHold) bound = 0;
            continue;
        }
        if (age > kRemember || s.z == kInf) continue;
        bound = std::min(bound, s.z - distance(eye, s.eye) - kSlack);
    }
    float target = minNear;
    if (!fresh) {
        ++g_stats.stale;
    } else if (now < g_holdUntil) {
        ++g_stats.held;
    } else {
        target = std::min(std::min(kShare * bound, body), maxNear);
        if (!(target > minNear)) target = minNear;
    }
    if (!(g_current >= minNear)) g_current = minNear;
    if (g_current > maxNear) g_current = maxNear;
    if (target < g_current) {
        g_current = target <= minNear ? minNear : std::max(minNear, target * kDrop);
        g_lastRise = now;
    } else if (target >= g_current * kRise && now - g_lastRise >= kRiseEvery) {
        g_current = std::min(target, g_current * kRise);
        g_lastRise = now;
    }
    // Past the inside of your own head, unless a hand is nearer.
    float result = g_current;
    if (head > result && body > 0) result = std::max(result, std::min(head, body));
    g_nearHistory[g_nearNext] = {now, result};
    g_nearNext = (g_nearNext + 1) % kNearHistory;

    if (!g_stats.since) g_stats.since = now;
    ++g_stats.frames;
    if (result > g_current) ++g_stats.headHeld;
    g_stats.low = std::min(g_stats.low, result);
    g_stats.high = std::max(g_stats.high, result);
    g_stats.nearestBody = std::min(g_stats.nearestBody, body);
    if (now - g_stats.since >= kLogEvery) {
        static int logs = 0;
        if (logs++ < 200)
            LOG("Near limit: %.2f m now, %.2f to %.2f over %llu s; nearest seen %.2f m, own body %.2f m; %u of %u "
                "frames measured (%u with your body left out), %u cut something off, %u stale, %u held after a jump "
                "(%u jumps), %u lost; %u frames pushed past your own head",
                result, g_stats.low, g_stats.high, static_cast<unsigned long long>((now - g_stats.since) / 1000),
                g_stats.nearestSeen, g_stats.nearestBody, g_stats.measured, g_stats.frames, g_stats.bodyLeftOut,
                g_stats.touches, g_stats.stale, g_stats.held, g_stats.jumps, g_stats.dropped, g_stats.headHeld);
        g_stats = Stats{};
        g_stats.since = now;
    }
    return result;
}

void sense(ID3D11DeviceContext* context, ID3D11DepthStencilView* depth, bool reversed) {
    if (!context || !depth) return;
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_failed || !g_activeTick || now - g_activeTick > kActive) return;
    ID3D11Resource* resource = nullptr;
    depth->GetResource(&resource);
    if (!resource) return;
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device) device->Release();  // only used here, while the context holds it
    D3D11_TEXTURE2D_DESC desc{};
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
    if (!device || !desc.Width || !init(device) || !prepareCopy(device, desc)) {
        resource->Release();
        return;
    }
    Slot& slot = g_slots[g_nextSlot];
    g_nextSlot = (g_nextSlot + 1) % kSlots;
    if (slot.pending) ++g_stats.dropped;
    if (++g_tag == 0) ++g_tag;

    context->CopyResource(g_copy, resource);
    resource->Release();
    g_copiedAt = now;
    g_copyReversed = reversed;
    float nearLow = 0, nearHigh = 0;
    nearRange(now, nearLow, nearHigh);
    Params params{};
    params.width = desc.Width;
    params.height = desc.Height;
    params.tag = g_tag;
    params.reversed = reversed ? 1u : 0u;
    params.tanX = g_bodyTan[0];
    params.tanY = g_bodyTan[1];
    params.centres[0] = g_bodyCentre[0];
    params.centres[1] = g_bodyCentre[1];
    params.centres[2] = g_bodyOtherCentre[0];
    params.centres[3] = g_bodyOtherCentre[1];
    params.nearLow = nearLow;
    params.nearHigh = nearHigh;
    params.farZ = g_far;
    params.considerZ = g_maxNear / kShare;
    // Your body in the view this depth was drawn with: from between the last
    // two eyes (it is one or the other; kBodyMargin in camera.cpp covers it).
    if (g_bodyCount && now - g_bodyTick <= kFresh && nearLow > 0 && g_far > nearHigh) {
        float eye[3];
        bool pair = distance(g_bodyEye, g_bodyOtherEye) < 0.1f;
        for (int i = 0; i < 3; ++i) eye[i] = pair ? (g_bodyEye[i] + g_bodyOtherEye[i]) / 2 : g_bodyEye[i];
        auto toView = [&](const float w[3], float* v) {
            const float d[3] = {w[0] - eye[0], w[1] - eye[1], w[2] - eye[2]};
            for (int axis = 0; axis < 3; ++axis)
                v[axis] = d[0] * g_bodyAxes[axis][0] + d[1] * g_bodyAxes[axis][1] + d[2] * g_bodyAxes[axis][2];
        };
        for (int i = 0; i < g_bodyCount; ++i) {
            toView(g_body[i].a, params.capA[i]);
            toView(g_body[i].b, params.capB[i]);
            params.capA[i][3] = g_body[i].radius;
        }
        params.count = static_cast<uint32_t>(g_bodyCount);
    }
    context->UpdateSubresource(g_params, 0, nullptr, &params, 0, 0);
    const UINT start[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};  // 1.0: nothing near
    context->ClearUnorderedAccessViewUint(g_resultView, start);

    // The compute stage as the game left it, put back after.
    ID3D11ComputeShader* savedShader = nullptr;
    ID3D11ShaderResourceView* savedView = nullptr;
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedParams = nullptr;
    context->CSGetShader(&savedShader, nullptr, nullptr);
    context->CSGetShaderResources(0, 1, &savedView);
    context->CSGetUnorderedAccessViews(0, 1, &savedUav);
    context->CSGetConstantBuffers(0, 1, &savedParams);
    const UINT keepCount = ~0u;
    context->CSSetShader(g_shader[desc.SampleDesc.Count > 1 ? 1 : 0], nullptr, 0);
    context->CSSetShaderResources(0, 1, &g_copyView);
    context->CSSetUnorderedAccessViews(0, 1, &g_resultView, &keepCount);
    context->CSSetConstantBuffers(0, 1, &g_params);
    context->Dispatch(16, 16, 1);
    context->CSSetShader(savedShader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &savedView);
    context->CSSetUnorderedAccessViews(0, 1, &savedUav, &keepCount);
    context->CSSetConstantBuffers(0, 1, &savedParams);
    safeRelease(savedShader);
    safeRelease(savedView);
    safeRelease(savedUav);
    safeRelease(savedParams);
    context->CopyResource(slot.staging, g_result);

    slot.tag = g_tag;
    slot.pending = true;
    slot.waits = 0;
    slot.reversed = reversed;
    slot.bodyLeftOut = params.count > 0;
    slot.nearLow = nearLow;
    slot.nearHigh = nearHigh;
    slot.farZ = g_far;
    std::memcpy(slot.eye, g_eye, sizeof(slot.eye));
}

ID3D11ShaderResourceView* sceneDepth(UINT& samples) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_copyView || GetTickCount64() - g_copiedAt > 30) return nullptr;
    samples = g_copyDesc.SampleDesc.Count;
    g_copyView->AddRef();
    return g_copyView;
}

ID3D11ShaderResourceView* latestDepth(UINT& samples, UINT& width, UINT& height, bool& reversed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_copyView || GetTickCount64() - g_copiedAt > 100) return nullptr;
    samples = g_copyDesc.SampleDesc.Count;
    width = g_copyDesc.Width;
    height = g_copyDesc.Height;
    reversed = g_copyReversed;
    g_copyView->AddRef();
    return g_copyView;
}

void collect(ID3D11DeviceContext* immediate) {
    if (!immediate) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_device) return;
    ID3D11Device* device = nullptr;
    immediate->GetDevice(&device);
    if (device) device->Release();
    if (device != g_device) return;
    ULONGLONG now = GetTickCount64();
    // Oldest first; one not done yet means the later ones aren't either.
    for (;;) {
        Slot* slot = nullptr;
        for (auto& s : g_slots)
            if (s.pending && (!slot || static_cast<int32_t>(s.tag - slot->tag) < 0)) slot = &s;
        if (!slot) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = immediate->Map(slot->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        uint32_t data[4] = {};
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (++slot->waits > 30) {
                slot->pending = false;
                ++g_stats.dropped;
                continue;
            }
            return;
        }
        if (FAILED(hr)) {
            slot->pending = false;
            ++g_stats.dropped;
            continue;
        }
        std::memcpy(data, mapped.pData, sizeof(data));
        immediate->Unmap(slot->staging, 0);
        if (data[2] != slot->tag) {  // its commands haven't run yet: this is an older result
            if (++slot->waits > 30) {
                slot->pending = false;
                ++g_stats.dropped;
                continue;
            }
            return;
        }
        slot->pending = false;
        float value;
        std::memcpy(&value, &data[0], sizeof(value));
        float d = slot->reversed ? 1 - value : value;
        Seen seen;
        seen.tick = now;
        seen.z = viewDepth(d, slot->nearLow, slot->farZ, slot->reversed);
        seen.touch = !(seen.z > slot->nearHigh * kTouch);
        std::memcpy(seen.eye, slot->eye, sizeof(seen.eye));
        g_seen[g_seenNext] = seen;
        g_seenNext = (g_seenNext + 1) % kSeen;
        ++g_stats.measured;
        if (slot->bodyLeftOut) ++g_stats.bodyLeftOut;
        if (seen.touch) ++g_stats.touches;
        if (!seen.touch) g_stats.nearestSeen = std::min(g_stats.nearestSeen, seen.z);
    }
}

}  // namespace s4vr::nearplane
