#include "texbias.h"

#include <windows.h>
#include <atomic>

#include "MinHook.h"
#include "log.h"

namespace s4vr::texbias {

namespace {

using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
CreateSamplerFn g_original = nullptr;
float g_bias = 0;
std::atomic<unsigned> g_biased{0}, g_left{0};

bool material(const D3D11_SAMPLER_DESC& d) {
    const bool comparison = (d.Filter & 0x80) != 0;  // D3D11_FILTER_COMPARISON_* / MINIMUM_* / MAXIMUM_*
    const bool mips = d.Filter == D3D11_FILTER_ANISOTROPIC || (d.Filter & 0x1) != 0;  // linear between mips
    return !comparison && mips && d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP && d.MaxLOD > 0;
}

HRESULT STDMETHODCALLTYPE hookedCreateSampler(ID3D11Device* device, const D3D11_SAMPLER_DESC* desc,
                                             ID3D11SamplerState** sampler) {
    if (!desc || g_bias == 0 || !material(*desc)) {
        g_left.fetch_add(1, std::memory_order_relaxed);
        return g_original(device, desc, sampler);
    }
    D3D11_SAMPLER_DESC d = *desc;
    d.MipLODBias = d.MipLODBias + g_bias;
    if (d.MipLODBias < -16) d.MipLODBias = -16;
    const unsigned n = g_biased.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 10 || n == 100 || n == 1000)
        LOG("Texture sharpness: %u game samplers made sharper so far (mip bias %+.2f), %u left as they were", n,
            g_bias, g_left.load(std::memory_order_relaxed));
    return g_original(device, &d, sampler);
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
    void** vtable = *reinterpret_cast<void***>(device);
    void* target = vtable[23];  // IUnknown 3 + CreateBuffer .. CreateRasterizerState: CreateSamplerState
    MH_STATUS init = MH_Initialize();
    if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(target, reinterpret_cast<void*>(&hookedCreateSampler), reinterpret_cast<void**>(&g_original)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_original = nullptr;
        LOG("Texture sharpness: hook failed; off");
        return;
    }
    LOG("Texture sharpness: on (mip bias %+.2f on the game's material samplers made from now on)", bias);
}

}  // namespace s4vr::texbias
