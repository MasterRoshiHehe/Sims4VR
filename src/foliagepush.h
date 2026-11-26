#pragma once
#include <d3d11.h>

#include <string>

#include "config.h"

// Plants, bushes, grass and leaves give way to your head, hands and body (first person; user 2026-10-28,
// re/foliage/FOLIAGE_VS_REPORT.md). The game's wind vertex shaders (307 of them) are replaced, as the game
// creates them, by copies that also push each leaf and blade away from up to eight spheres (foliagepatch.cpp).
// The spheres go to the shaders in a small constant buffer (slot 6) written once per frame.
//
// [foliage] patch_shaders (next start): hook shader creation and make the patched copies. push (live): use them;
// off, the game gets its own shader back at every draw, so nothing differs from a game without this.
namespace s4vr::foliagepush {

// s4vr_init, as early as possible (the game makes most of its shaders at start): hooks CreateVertexShader and
// VSSetShader of d3d11.dll. Does nothing when [foliage] patch_shaders is 0.
void installEarly(const Config& config, const std::wstring& moduleDir);

// Present thread, every frame: writes the spheres (and the live settings) into the constant buffer, logs a summary
// now and then.
void onFrame(ID3D11Device* device, ID3D11DeviceContext* context, const Config& config);

}  // namespace s4vr::foliagepush
