#pragma once
#include <d3d11.h>

// Separates the game's UI from the 3D view (DX11 game only).
//
// The game draws its UI with Scaleform once a frame in RenderManager::Render
// (rva 0xd8df50 in 1.121.361.1020), on a job thread into a deferred context
// (or inline on the main one with render threading off), straight into its
// main colour render target: the finished 3D frame. While that call runs, binds of the
// main target on its context go to a texture of our own instead (same size
// and MSAA, so the game's depth-stencil, used for UI masks, still fits),
// cleared to transparent once a frame. Scaleform's alpha is made to
// accumulate properly (HAL::applyBlendModeImpl with forceAc), so the texture
// holds the UI as premultiplied RGBA. The main target, and so the eye images,
// then hold the 3D view only. RE notes: /tmp/claude-0/re/ui_layer_notes.md.
namespace s4vr::uihook {

// Hooks the game. Call once at start; false (and the UI stays in the 3D view)
// in the DX9 game or another game version.
bool install();

// Present thread (the game's submission thread since 1.121, after it executed
// the frame's command lists), at the start of every Present, before using the images.
// `enabled` turns the redirection on for the next frame. Returns this frame's
// UI as a single-sample texture (owned here, valid until the next call), or
// null if no UI was redirected.
ID3D11Texture2D* onPresent(ID3D11Texture2D* backbuffer, bool enabled);

// Present thread: the 3D view's depth-stencil (a reference of the caller's own), which holds the
// presented frame's finished depth there (its command lists ran, the next frame's not yet), our 3D
// pieces in it; and whether it clears to 0. Null if not learned, or if the UI clears it.
ID3D11DepthStencilView* sceneDepthAtPresent(bool& reversed);

}  // namespace s4vr::uihook
