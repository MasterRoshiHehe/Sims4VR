#pragma once
#include <d3d11.h>

// Sharper textures at a distance in the headset (v0.13.310). The game draws each eye below the
// headset's own resolution (render_scale), so its textures pick their mip level for that smaller
// picture and blur a few metres out. A negative mip bias on the game's material samplers (wrapped,
// mipmapped, not comparison) picks the sharper level, as VR runtimes' upscalers do.
namespace s4vr::texbias {

// Present thread, once: hooks ID3D11Device::CreateSamplerState. Samplers made from then on get
// `bias` added to their MipLODBias (0: unchanged). The game makes most while loading a world.
void install(ID3D11Device* device, float bias);

}  // namespace s4vr::texbias
