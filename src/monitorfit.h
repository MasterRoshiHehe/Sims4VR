#pragma once
#include <windows.h>
#include <d3d11.h>

namespace s4vr::monitorfit {

// Desktop window picture (what a screen share shows): the game's back buffer is
// headset-shaped (near square) and the window stretches it. mode 1 = crop to the
// window's shape (centred), 2 = whole picture with black bars, else untouched.
// zoom (>= 1) narrows the field of view in crop mode. Call right before the real Present. Does nothing when the shapes already match.
void apply(ID3D11Texture2D* backbuffer, HWND window, int mode, float zoom);
void shutdown();

}  // namespace s4vr::monitorfit
