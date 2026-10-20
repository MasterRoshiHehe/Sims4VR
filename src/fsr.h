#pragma once
#include <d3d11.h>

// AMD FidelityFX Super Resolution 1 (MIT, src/ffx) for the eye images: the
// game draws fewer pixels than the headset shows ([stereo] fsr_render_scale)
// and each eye picture is upscaled (EASU) and sharpened (RCAS) as it is
// copied into its eye swapchain image, which is the headset's size
// ([stereo] fsr_output_scale). Replaces the plain sharpen (sharpen.h) while
// on; [stereo] upscale = 0 goes back to it.
namespace s4vr::fsr {

// The sizes computeRenderSize asked the game for and the eye pictures go out
// at (all 0: no upscaling).
void setSizes(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight);

// `source` (a game frame) as its eye swapchain should be made: upscaled to the
// output size when it is the size asked for (or near it), else as it is.
D3D11_TEXTURE2D_DESC eyeDesc(const D3D11_TEXTURE2D_DESC& source);

// Draws `source` (single-sampled or MSAA) upscaled into `target` (an eye
// swapchain image of width x height, viewed as `targetFormat`), sharpened by
// `strength` 0..1, with eyelids drawn over it (`closure`, `dark`: eyelids.h).
// Leaves the game's pipeline state untouched. False if the pass is
// unavailable (no shader compiler, old runtime): eyeDesc stops upscaling then.
bool draw(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& sourceDesc, ID3D11Texture2D* target,
          DXGI_FORMAT targetFormat, UINT width, UINT height, float strength, float closure = 0, float dark = 0);

// Drops the views of the eye images (call before the runtime destroys them).
void forgetTargets();
// Frees everything (session end).
void release();

}  // namespace s4vr::fsr
