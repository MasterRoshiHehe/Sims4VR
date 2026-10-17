#pragma once
#include <d3d11.h>
#include <dxgi.h>

namespace s4vr::d3d11_hook {

// Called on the game's render thread just before each IDXGISwapChain::Present,
// while the backbuffer still holds the finished frame.
using PresentCallback = void (*)(IDXGISwapChain* swapchain);

bool install(PresentCallback onPresent);

// D3D11 as Windows ships it, for the mod's own devices (the hidden one that
// finds Present, the DX9 bridge). The game folder may hold another d3d11.dll,
// an overlay such as ReShade or RTBP, which the game loads in its place; going
// through it would set the overlay up a second time. Null if unavailable.
PFN_D3D11_CREATE_DEVICE systemCreateDevice();
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN systemCreateDeviceAndSwapChain();

}  // namespace s4vr::d3d11_hook
