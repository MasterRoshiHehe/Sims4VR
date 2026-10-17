#pragma once
#include <d3d11.h>

#include "camera.h"

// First person: your own body on a layer of its own (vr.cpp renderStereo).
//
// Smooth stick turning (Config::turnReprojection) turns each eye picture by
// how far the world has turned since it was drawn, every headset refresh. Your
// body turns with you, but it is in the same picture, so it turned with the
// world: while you turned, your hands jumped back by up to a pair's turn and
// forward again with each new pair (doubled hands, user 2026-10-14 20:37Z).
//
// So each eye picture is split in two as it goes to the headset. Your body
// (pixels whose depth puts them inside the capsules camera.cpp sends along,
// camera::BodyParts) goes to a layer of its own that the turn leaves alone;
// in the eye picture, where your body was, the world is filled in from
// around it (pull-push: averages of the surrounding pixels, finer where they
// are near), which only shows beside your body while you turn.
namespace s4vr::bodylayer {

// Present thread, the immediate context, right after `eye` (an eye swapchain
// image, width x height, viewed as `format`) was drawn: your body goes into
// `body` (a swapchain image like it, premultiplied alpha, transparent
// elsewhere) and is filled in in `eye`. fov: the half-angles `eye` was drawn
// with. Leaves the game's pipeline state untouched. False when nothing was
// done (no scene depth this frame, sizes differ, the pass unavailable): then
// `eye` is as it was and `body` untouched.
bool split(ID3D11Texture2D* eye, ID3D11Texture2D* body, DXGI_FORMAT format, UINT width, UINT height,
           const camera::Fov& fov, const camera::BodyParts& parts);

// Present thread: `body` all transparent. False if it couldn't be.
bool clear(ID3D11Texture2D* body, DXGI_FORMAT format);

// Drops the views of swapchain images (call before the runtime destroys them).
void forgetTargets();
// Frees everything (session end).
void release();

}  // namespace s4vr::bodylayer
