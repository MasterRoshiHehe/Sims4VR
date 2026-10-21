#pragma once

#include <d3d11.h>

// Both real eyes: the right eye's pass reuses the sun shadow map the left eye's pass drew in the
// same frame ([stereo] reuse_shadows, v0.13.318). The sun is the same for both eyes, so in the
// second pass the game's draws and clears into that map are left out (they would draw the same
// picture again). shadows.cpp gives the second pass the left eye's cascade fit so the map and the
// matrices the lighting reads match. Leaving out the whole FrameBegin step instead crashed (317).
namespace s4vr::shadowreuse {

void install(ID3D11Device* device);  // once; hooks the contexts' draw calls
void setOn(bool on);                 // live, from the settings
bool on();
// uihook.cpp: a context's render targets were bound (before the game's call goes on).
void bound(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views,
           ID3D11DepthStencilView* depth);
// uihook.cpp: true when this depth clear is the sun shadow map's in the second pass (leave it out).
bool skipClear(ID3D11DeviceContext* context, ID3D11DepthStencilView* depth);

// The game's non-eye lists (ShadowLists 0x11c8080: sun shadow casters and Sims into the shadow map)
// left out of the right eye's pass altogether, not only their draws: saves the CPU work of walking
// and recording them ([stereo] skip_shadow_prep, v0.13.320; on by default from 321).
void installGame();
void setSkipLists(bool on);

}  // namespace s4vr::shadowreuse
