#pragma once
#include <d3d11.h>

// Depth layer for the headset ([stereo] depth_layer, XR_KHR_composition_layer_depth; docs/PERF-PLAN.md item 6):
// each eye picture's scene depth, written into a depth swapchain image the size of the eye picture (the game's depth
// is stretched to the upscaled size and its first MSAA sample taken), so the runtime (Virtual Desktop) can move the
// picture with your head by its depth when a refresh has no new picture. The depth is passed through as the game
// wrote it (reversed or not); the layer says which with its near/far distances.
namespace s4vr::xrdepth {

// `depth`: shader view of the scene depth (samples, srcW x srcH). `target`: a depth swapchain image (w x h).
bool fill(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, UINT samples, UINT srcW, UINT srcH,
          ID3D11Texture2D* target, DXGI_FORMAT format, UINT w, UINT h);
void release();

}  // namespace s4vr::xrdepth
