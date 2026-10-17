#pragma once

// The DX9 game (TS4_DX9_x64.exe). OpenXR has no Direct3D 9 binding, so every
// finished frame is handed to the VR code (vr::onFrame) as a D3D11 texture:
// at Present the backbuffer is copied into a texture shared with a D3D11
// device of the mod's own on the same GPU. Sharing needs the game's device to
// be a D3D9Ex one, which the game creates when Windows has it (Vista and up).
// The copy is waited for before D3D11 reads it, and the monitor is kept
// steady on the D3D9 side (the left eye shows on right-eye frames).
namespace s4vr::d3d9_hook {

// Hooks Direct3D 9 Present (device, Ex device and swapchain). Call once.
bool install();

}  // namespace s4vr::d3d9_hook
