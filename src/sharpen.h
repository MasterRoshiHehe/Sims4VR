#pragma once
#include <d3d11.h>

// Contrast-adaptive sharpening while copying a game frame into an eye image.
// The game renders fewer pixels than the headset shows (2560x1440 stretched
// over an eye that wants about 3072x3264), so the plain copy looks soft.
namespace s4vr::sharpen {

// Draws `source` (single-sampled or MSAA) into `target` (an eye swapchain
// image of the same size, viewed as `targetFormat`), sharpened by `strength`
// 0..1, with eyelids drawn over it (`closure` 0 open .. 1 shut, `dark` 0..1;
// see eyelids.h). Leaves the game's pipeline state untouched. False if there
// is nothing to do or the pass is unavailable (no shader compiler, old
// runtime); the caller then copies.
bool draw(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& sourceDesc, ID3D11Texture2D* target,
          DXGI_FORMAT targetFormat, float strength, float closure = 0, float dark = 0);

// Drops the views of the eye images (call before the runtime destroys them).
void forgetTargets();
// Frees everything (session end).
void release();

}  // namespace s4vr::sharpen
