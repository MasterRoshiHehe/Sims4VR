#pragma once
#include <d3d11.h>

// Sharper textures at a distance in the headset (v0.13.310). The game draws each eye below the
// headset's own resolution (render_scale), so its textures pick their mip level for that smaller
// picture and blur a few metres out. A negative mip bias on the game's material samplers (wrapped,
// mipmapped, not comparison) picks the sharper level, as VR runtimes' upscalers do.
namespace s4vr::texbias {

// Present thread, once: hooks PSSetSamplers on the game's contexts. While active, each material
// sampler the game binds is swapped for a copy with `bias` added to its MipLODBias (0: off).
void install(ID3D11Device* device, float bias);

// Present thread: on while the headset shows the stereo view (the game's own size otherwise).
void setActive(bool on);

}  // namespace s4vr::texbias
