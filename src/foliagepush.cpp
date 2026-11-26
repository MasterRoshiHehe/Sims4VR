#include "foliagepush.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "bodyshape.h"
#include "camera.h"
#include "foliagepatch.h"
#include "log.h"
#include "patchreport.h"

namespace s4vr::foliagepush {

namespace {

namespace fp = s4vr::foliagepatch;

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11VertexShader**);
using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
CreateVertexShaderFn g_createVertexShader = nullptr;
using VSSetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
VSSetCbFn g_vsSetCb[2] = {};
VSSetShaderFn g_vsSetShader[2] = {};  // immediate context's, deferred contexts' (when they differ)

std::atomic<bool> g_use{false};              // the patched shaders are bound (otherwise the game's own)
std::atomic<ID3D11Buffer*> g_cb{nullptr};    // the spheres, bound at slot 6 after a patched shader
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_presentSeen{false};      // the first Present has come: shaders made from now on are "late"

// The shaders we made: the pointer the game holds (the patched one) -> the original, for when the push is off.
// A fixed open-addressing table, lock-free (the game binds from many threads). Both are AddRef'd for good, so an
// address never comes back as another shader.
constexpr size_t kSlots = 2048;
struct Slot {
    std::atomic<ID3D11VertexShader*> key{nullptr};
    std::atomic<ID3D11VertexShader*> original{nullptr};
};
Slot g_table[kSlots];

bool add(ID3D11VertexShader* patched, ID3D11VertexShader* original) {
    size_t i = (reinterpret_cast<uintptr_t>(patched) >> 4) % kSlots;
    for (size_t probes = 0; probes < kSlots; ++probes, i = (i + 1) % kSlots) {
        ID3D11VertexShader* expected = nullptr;
        if (g_table[i].key.compare_exchange_strong(expected, patched, std::memory_order_acq_rel)) {
            g_table[i].original.store(original, std::memory_order_release);
            return true;
        }
    }
    return false;
}

// The original behind a patched shader, or null when `shader` is not one of ours.
ID3D11VertexShader* originalOf(ID3D11VertexShader* shader) {
    size_t i = (reinterpret_cast<uintptr_t>(shader) >> 4) % kSlots;
    for (size_t probes = 0; probes < kSlots; ++probes, i = (i + 1) % kSlots) {
        ID3D11VertexShader* key = g_table[i].key.load(std::memory_order_acquire);
        if (!key) return nullptr;
        if (key == shader) return g_table[i].original.load(std::memory_order_acquire);
    }
    return nullptr;
}

std::atomic<unsigned> g_seen{0}, g_seenLate{0}, g_wind{0}, g_windLate{0}, g_static{0}, g_instanced{0}, g_refused{0},
    g_createFailed{0}, g_tableFull{0}, g_sets{0}, g_setsOriginal{0}, g_allSets{0}, g_cbCover{0}, g_cbCoverNull{0};
std::wstring g_dumpDir;

void logShader(const char* what, const uint8_t* code, const fp::Info& info, size_t before, size_t after) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1) >= 40) return;
    LOGT("foliage", "Foliage push: shader %02x%02x%02x%02x %s (%zu -> %zu bytes, family %d%s%s)%s", code[4], code[5], code[6],
         code[7], what, before, after, static_cast<int>(info.family), info.why.empty() ? "" : ": ", info.why.c_str(),
         g_presentSeen.load() ? " [made after the first Present]" : "");
}

// A wind shader the patch refused, kept as a file to read offline (foliage_<hash>.dxbc next to the log).
void dumpRefused(const uint8_t* code, size_t size) {
    static std::atomic<unsigned> dumped{0};
    if (g_dumpDir.empty() || dumped.fetch_add(1) >= 40) return;
    wchar_t name[64];
    swprintf(name, 64, L"\\foliage_%02x%02x%02x%02x.dxbc", code[4], code[5], code[6], code[7]);
    HANDLE f = CreateFileW((g_dumpDir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, code, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(f);
}

HRESULT STDMETHODCALLTYPE hookedCreateVertexShader(ID3D11Device* device, const void* code, SIZE_T size,
                                                   ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
    if (!out || !code || size < 64) return g_createVertexShader(device, code, size, linkage, out);
    const bool late = g_presentSeen.load(std::memory_order_relaxed);
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (late) g_seenLate.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* bytes = static_cast<const uint8_t*>(code);
    // A wind shader has a sincos and reads the frame time; most shaders are rejected in the first lines of the patch.
    std::vector<uint8_t> patched;
    fp::Info info;
    bool ok = false;
    try {
        ok = fp::patch(bytes, size, patched, &info);
    } catch (...) {
        ok = false;
        info.why = "exception in the patch";
    }
    if (!ok) {
        const bool wind = info.wind;
        if (wind) {
            g_wind.fetch_add(1, std::memory_order_relaxed);
            if (late) g_windLate.fetch_add(1, std::memory_order_relaxed);
            g_refused.fetch_add(1, std::memory_order_relaxed);
            logShader("left alone", bytes, info, size, 0);
            dumpRefused(bytes, size);
        }
        return g_createVertexShader(device, code, size, linkage, out);
    }
    g_wind.fetch_add(1, std::memory_order_relaxed);
    if (late) g_windLate.fetch_add(1, std::memory_order_relaxed);
    ID3D11VertexShader* original = nullptr;
    HRESULT hr = g_createVertexShader(device, code, size, linkage, &original);
    if (FAILED(hr) || !original) return hr;
    ID3D11VertexShader* made = nullptr;
    if (FAILED(g_createVertexShader(device, patched.data(), patched.size(), linkage, &made)) || !made) {
        g_createFailed.fetch_add(1, std::memory_order_relaxed);
        logShader("patched copy refused by the device", bytes, info, size, patched.size());
        *out = original;
        return hr;
    }
    if (!add(made, original)) {
        g_tableFull.fetch_add(1, std::memory_order_relaxed);
        made->Release();
        *out = original;  // the creation's reference goes to the game
        return hr;
    }
    // Both are held for good (the address is the key): `original` by the reference its creation gave us, `made` by
    // one more beside the game's.
    made->AddRef();
    if (info.family == fp::kStatic) g_static.fetch_add(1, std::memory_order_relaxed);
    else g_instanced.fetch_add(1, std::memory_order_relaxed);
    logShader("patched", bytes, info, size, patched.size());
    *out = made;
    return hr;
}

// Counts the game's own constant buffer binds that reach slot 6 (they would replace the spheres after a patched
// shader was set); the lot of them is logged in the summary.
template <int Which>
void STDMETHODCALLTYPE hookedVSSetConstantBuffers(ID3D11DeviceContext* context, UINT start, UINT count,
                                                  ID3D11Buffer* const* buffers) {
    if (start <= fp::kSlot && start + count > fp::kSlot) {
        g_cbCover.fetch_add(1, std::memory_order_relaxed);
        if (!buffers || !buffers[fp::kSlot - start]) g_cbCoverNull.fetch_add(1, std::memory_order_relaxed);
    }
    g_vsSetCb[Which](context, start, count, buffers);
}

template <int Which>
void STDMETHODCALLTYPE hookedVSSetShader(ID3D11DeviceContext* context, ID3D11VertexShader* shader,
                                         ID3D11ClassInstance* const* instances, UINT count) {
    g_allSets.fetch_add(1, std::memory_order_relaxed);
    ID3D11VertexShader* original = shader ? originalOf(shader) : nullptr;
    if (!original) return g_vsSetShader[Which](context, shader, instances, count);
    if (!g_use.load(std::memory_order_relaxed)) {
        g_setsOriginal.fetch_add(1, std::memory_order_relaxed);
        return g_vsSetShader[Which](context, original, instances, count);
    }
    g_sets.fetch_add(1, std::memory_order_relaxed);
    g_vsSetShader[Which](context, shader, instances, count);
    if (ID3D11Buffer* cb = g_cb.load(std::memory_order_acquire)) context->VSSetConstantBuffers(fp::kSlot, 1, &cb);
}

bool hook(void* target, void* detour, void** original, const char* name) {
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) return false;
    patchreport::nameHook(target, name);
    return true;
}

// ---- Spheres ----
struct Sphere {
    float x, y, z, r;
};

struct Block {
    float row[fp::kRows][4];
};

void far_(float* row) {
    row[0] = row[1] = row[2] = 1e6f;
    row[3] = -1;
}

void put(Block& b, int i, const Sphere& s) {
    b.row[i][0] = -s.x;
    b.row[i][1] = -s.y;
    b.row[i][2] = -s.z;
    b.row[i][3] = -1.0f / (s.r > 0.01f ? s.r : 0.01f);
}

}  // namespace

void installEarly(const Config& config, const std::wstring& moduleDir) {
    g_dumpDir = moduleDir;
    if (!config.foliagePatch) {
        LOGT("foliage", "Foliage push: shader patching is off ([foliage] patch_shaders = 0)");
        return;
    }
    if (g_hooked.exchange(true)) return;
    HMODULE module = LoadLibraryW(L"d3d11.dll");
    auto create = module ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(module, "D3D11CreateDevice")) : nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                 &immediate)
                        : E_NOINTERFACE;
    if (FAILED(hr) || !device || !immediate) {
        LOGT("foliage", "Foliage push: no device to find the hooks with (0x%08lX)", hr);
        g_hooked = false;
        return;
    }
    ID3D11DeviceContext* deferred = nullptr;
    device->CreateDeferredContext(0, &deferred);
    constexpr int kCreateVertexShader = 12;  // ID3D11Device: IUnknown 3 + CreateBuffer .. CreateInputLayout (3..11)
    constexpr int kVSSetConstantBuffers = 7;
    constexpr int kVSSetShader = 11;         // ID3D11DeviceContext: IUnknown 3 + DeviceChild 4 + VSSetConstantBuffers..
    void* create_ = (*reinterpret_cast<void***>(device))[kCreateVertexShader];
    void* a = (*reinterpret_cast<void***>(immediate))[kVSSetShader];
    void* b = deferred ? (*reinterpret_cast<void***>(deferred))[kVSSetShader] : nullptr;
    void* cbA = (*reinterpret_cast<void***>(immediate))[kVSSetConstantBuffers];
    void* cbB = deferred ? (*reinterpret_cast<void***>(deferred))[kVSSetConstantBuffers] : nullptr;
    if (deferred) deferred->Release();
    immediate->Release();
    device->Release();
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOGT("foliage", "Foliage push: MinHook failed: %s", MH_StatusToString(init));
        g_hooked = false;
        return;
    }
    bool ok = hook(a, reinterpret_cast<void*>(&hookedVSSetShader<0>), reinterpret_cast<void**>(&g_vsSetShader[0]),
                   "foliage VSSetShader");
    if (ok && b && b != a)
        ok = hook(b, reinterpret_cast<void*>(&hookedVSSetShader<1>), reinterpret_cast<void**>(&g_vsSetShader[1]),
                  "foliage VSSetShader (deferred)");
    // Only a counter: a failure here does not stop the push.
    hook(cbA, reinterpret_cast<void*>(&hookedVSSetConstantBuffers<0>), reinterpret_cast<void**>(&g_vsSetCb[0]),
         "foliage VSSetConstantBuffers");
    if (cbB && cbB != cbA)
        hook(cbB, reinterpret_cast<void*>(&hookedVSSetConstantBuffers<1>), reinterpret_cast<void**>(&g_vsSetCb[1]),
             "foliage VSSetConstantBuffers (deferred)");
    if (ok)
        ok = hook(create_, reinterpret_cast<void*>(&hookedCreateVertexShader), reinterpret_cast<void**>(&g_createVertexShader),
                  "foliage CreateVertexShader");
    LOGT("foliage", "Foliage push: %s (CreateVertexShader %p, VSSetShader %p%s); wind shaders are patched as the game makes them",
         ok ? "hooks on" : "HOOK FAILED, off", create_, a, b && b != a ? " and a deferred one" : "");
}

void onFrame(ID3D11Device* device, ID3D11DeviceContext* context, const Config& config) {
    g_presentSeen.store(true, std::memory_order_relaxed);
    static ID3D11Buffer* buffer = nullptr;
    static unsigned long long frame = 0;
    static bool wasOn = false;
    static int lastCount = -1;
    if (!g_hooked.load()) return;
    ++frame;

    const bool body = camera::bodyActive();
    const bool want = config.foliagePush && body;
    g_use.store(config.foliagePush, std::memory_order_relaxed);
    if (want != wasOn) {
        wasOn = want;
        LOGT("foliage", "Foliage push: %s (switch %s, first person %s)", want ? "on" : "off", config.foliagePush ? "on" : "off",
             body ? "yes" : "no");
    }

    if (!buffer) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = sizeof(Block);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Block zero{};
        for (int i = 0; i < static_cast<int>(fp::kSpheres); ++i) far_(zero.row[i]);
        D3D11_SUBRESOURCE_DATA init{&zero, 0, 0};
        if (FAILED(device->CreateBuffer(&d, &init, &buffer))) {
            LOGT("foliage", "Foliage push: the sphere buffer could not be made");
            buffer = nullptr;
            g_hooked = false;
            return;
        }
        g_cb.store(buffer, std::memory_order_release);
    }

    Block block{};
    for (int i = 0; i < static_cast<int>(fp::kSpheres); ++i) far_(block.row[i]);
    int count = 0;
    if (want) {
        Sphere spheres[fp::kSpheres];
        auto add = [&](float x, float y, float z, float r) {
            if (count < static_cast<int>(fp::kSpheres)) spheres[count++] = {x, y, z, r};
        };
        camera::Vec3 head, right, up, back;
        const bool haveHead = camera::headInWorld(head, right, up, back);
        if (haveHead) {
            if (config.foliageHead) add(head.x, head.y, head.z, config.foliageHeadRadius);
            if (config.foliageBody) {
                add(head.x - up.x * 0.35f, head.y - up.y * 0.35f, head.z - up.z * 0.35f, config.foliageBodyRadius);
                add(head.x - up.x * 0.8f, head.y - up.y * 0.8f, head.z - up.z * 0.8f, config.foliageBodyRadius);
            }
        }
        if (config.foliageHands)
            for (int hand = 0; hand < 2; ++hand) {
                uint64_t sim = 0;
                float p[3], q[4];
                if (camera::handJoint(hand, sim, p, q)) add(p[0], p[1], p[2], config.foliageHandRadius);
            }
        if (config.foliageSims) {
            for (int i = 0, n = bodyshape::count(); i < n; ++i) {
                bodyshape::Shape s;
                camera::Vec3 w;
                if (bodyshape::get(i, s) && !s.player && camera::xrToWorld(s.centre, w))
                    add(w.x, w.y, w.z, std::fmin(s.boundRadius, 0.5f));
            }
        }
        for (int i = 0; i < count; ++i) put(block, i, spheres[i]);
        static float lastLift = -1.0f;
        if (config.foliageLiftCm != lastLift) {
            lastLift = config.foliageLiftCm;
            LOGT("foliage", "Foliage push: lift test %s (every leaf of a patched shader rises %.0f cm)",
                 lastLift != 0 ? "APPLIED" : "off", lastLift);
        }
        block.row[fp::kSpheres][0] = config.foliagePushCm * 0.01f;
        block.row[fp::kSpheres][1] = config.foliageLiftCm * 0.01f;
        block.row[fp::kSpheres][2] = (count > 0 || config.foliageLiftCm != 0) ? 1.0f : 0.0f;
    }
    if (count != lastCount) {
        lastCount = count;
        if (want) LOGT("foliage", "Foliage push: %d sphere(s) now", count);
    }
    if (want || frame % 60 == 0) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &block, sizeof(block));
            context->Unmap(buffer, 0);
        }
    }

    if (frame % 1800 == 0) {
        LOGT("foliage",
             "Foliage push: shaders created %u (%u after the first Present), wind shaders %u (%u late): %u static + %u "
             "instanced patched, %u left alone, %u copies refused, %u table full; %u binds of a patched shader in the "
             "last minute (%u given the original), %u VSSetShader calls of any shader; "
             "game binds reaching slot 6: %u (%u with nothing in it)",
             g_seen.load(), g_seenLate.load(), g_wind.load(), g_windLate.load(), g_static.load(), g_instanced.load(),
             g_refused.load(), g_createFailed.load(), g_tableFull.load(), g_sets.exchange(0), g_setsOriginal.exchange(0), g_allSets.exchange(0),
             g_cbCover.exchange(0), g_cbCoverNull.exchange(0));
    }
}

}  // namespace s4vr::foliagepush
