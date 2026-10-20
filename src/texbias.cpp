#include "texbias.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

#include "MinHook.h"
#include "log.h"

namespace s4vr::texbias {

namespace {

// v0.13.310 hooked CreateSamplerState: the game makes its samplers before the mod starts, so it
// caught none (log: no "made sharper" line). Now the samplers the game binds are swapped as it
// binds them (PSSetSamplers on its contexts) for a copy with the bias added, made once each.
using PSSetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
PSSetSamplersFn g_original[2] = {};  // immediate context's, deferred contexts' (when they differ)
float g_bias = 0;
std::atomic<bool> g_active{false};
std::atomic<unsigned> g_biased{0}, g_kept{0};

// The game's sampler -> the one bound in its place (itself when it isn't a material sampler).
// Few dozen samplers: a fixed open-addressing table, lock-free (the game binds from many threads).
// Both are AddRef'd for good, so an address never comes back as another sampler.
constexpr size_t kSlots = 4096;
struct Slot {
    std::atomic<ID3D11SamplerState*> key{nullptr};
    std::atomic<ID3D11SamplerState*> value{nullptr};
};
Slot g_table[kSlots];

bool material(const D3D11_SAMPLER_DESC& d) {
    const bool comparison = (d.Filter & 0x80) != 0;  // D3D11_FILTER_COMPARISON_* / MINIMUM_* / MAXIMUM_*
    const bool mips = d.Filter == D3D11_FILTER_ANISOTROPIC || (d.Filter & 0x1) != 0;  // linear between mips
    return !comparison && mips && d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP && d.MaxLOD > 0;
}

ID3D11SamplerState* makeFor(ID3D11SamplerState* sampler) {
    D3D11_SAMPLER_DESC d;
    sampler->GetDesc(&d);
    if (!material(d)) {
        g_kept.fetch_add(1, std::memory_order_relaxed);
        return sampler;
    }
    d.MipLODBias = d.MipLODBias + g_bias < -16 ? -16 : d.MipLODBias + g_bias;
    ID3D11Device* device = nullptr;
    sampler->GetDevice(&device);
    ID3D11SamplerState* biased = nullptr;
    if (!device || FAILED(device->CreateSamplerState(&d, &biased))) biased = nullptr;
    if (device) device->Release();
    if (!biased) return sampler;
    const unsigned n = g_biased.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 10 || n == 30 || n == 100 || n == 300)
        LOG("Texture sharpness: %u of the game's samplers made sharper (mip bias %+.2f), %u left as they were", n,
            g_bias, g_kept.load(std::memory_order_relaxed));
    return biased;
}

ID3D11SamplerState* swapFor(ID3D11SamplerState* sampler) {
    size_t i = (reinterpret_cast<uintptr_t>(sampler) >> 4) % kSlots;
    for (size_t probes = 0; probes < kSlots; ++probes, i = (i + 1) % kSlots) {
        Slot& s = g_table[i];
        ID3D11SamplerState* key = s.key.load(std::memory_order_acquire);
        if (key == sampler) {
            ID3D11SamplerState* value = s.value.load(std::memory_order_acquire);
            return value ? value : sampler;  // another thread is still making it
        }
        if (key) continue;
        ID3D11SamplerState* expected = nullptr;
        if (!s.key.compare_exchange_strong(expected, sampler, std::memory_order_acq_rel)) {
            if (expected == sampler) {
                ID3D11SamplerState* value = s.value.load(std::memory_order_acquire);
                return value ? value : sampler;
            }
            continue;
        }
        sampler->AddRef();
        ID3D11SamplerState* value = makeFor(sampler);
        if (value == sampler) sampler->AddRef();  // the value's own reference, as a made one has
        s.value.store(value, std::memory_order_release);
        return value;
    }
    return sampler;  // table full
}

template <int Which>
void STDMETHODCALLTYPE hookedPSSetSamplers(ID3D11DeviceContext* context, UINT start, UINT count,
                                           ID3D11SamplerState* const* samplers) {
    if (!g_active.load(std::memory_order_relaxed) || !samplers || count == 0 || count > 16)
        return g_original[Which](context, start, count, samplers);
    ID3D11SamplerState* swapped[16];
    for (UINT i = 0; i < count; ++i) swapped[i] = samplers[i] ? swapFor(samplers[i]) : nullptr;
    g_original[Which](context, start, count, swapped);
}

bool hook(void* target, void* detour, int which) {
    return MH_CreateHook(target, detour, reinterpret_cast<void**>(&g_original[which])) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

}  // namespace

void install(ID3D11Device* device, float bias) {
    static bool tried = false;
    if (tried || !device) return;
    tried = true;
    g_bias = bias;
    if (bias == 0) {
        LOG("Texture sharpness: off");
        return;
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    constexpr int kPSSetSamplers = 10;  // IUnknown 3 + ID3D11DeviceChild 4 + VSSetConstantBuffers,
                                        // PSSetShaderResources, PSSetShader
    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    ID3D11DeviceContext* deferred = nullptr;
    device->CreateDeferredContext(0, &deferred);
    void* a = immediate ? (*reinterpret_cast<void***>(immediate))[kPSSetSamplers] : nullptr;
    void* b = deferred ? (*reinterpret_cast<void***>(deferred))[kPSSetSamplers] : nullptr;
    if (immediate) immediate->Release();
    if (deferred) deferred->Release();
    bool ok = a && hook(a, reinterpret_cast<void*>(&hookedPSSetSamplers<0>), 0);
    if (ok && b && b != a) ok = hook(b, reinterpret_cast<void*>(&hookedPSSetSamplers<1>), 1);
    LOG("Texture sharpness: %s (mip bias %+.2f on the game's material samplers as it binds them, in the "
        "headset view%s)",
        ok ? "on" : "hook failed; off", bias, b && b != a ? "; immediate and deferred contexts" : "");
}

void setActive(bool on) { g_active.store(on, std::memory_order_relaxed); }

}  // namespace s4vr::texbias
