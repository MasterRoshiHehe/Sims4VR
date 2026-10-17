#pragma once

#include <d3d11.h>

// Re-aims the older eye image at the newest head pose (rotation only).
//
// The game renders one eye per frame, so each frame one eye is new and the
// other is a frame old, rendered with the head where it was then. Each view
// is submitted with the pose it was rendered from, which OpenXR allows, but
// SteamVR shows one eye as if the head hadn't moved since it was rendered
// (user 2026-10-14: the left eye extremely janky under SteamVR, fine under
// Virtual Desktop). With this on, every frame the older eye's image is turned
// to where that eye is now, as seen from the newest render's head pose, so
// both views carry the same head pose and both images are new each frame.
namespace s4vr::eyealign {

// Keeps a copy of an eye's image (the swapchain image it was just drawn
// into, before it is released). False if the copy couldn't be made.
bool keep(int eye, ID3D11Texture2D* image, DXGI_FORMAT format, UINT width, UINT height);

// An image of this eye is kept.
bool has(int eye);

// Drops the kept image of an eye (its swapchain was made again).
void forget(int eye);

// Forgets the swapchain images drawn into (a swapchain was destroyed).
void forgetTargets();

// Draws the kept image of `eye` into target, turned: rotation is a row-major
// 3x3 matrix taking a view direction in the target to one in the kept image;
// tangents are the field of view (tan left, right, up, down) of both.
bool draw(int eye, ID3D11Texture2D* target, DXGI_FORMAT format, const float rotation[9], const float tangents[4]);

void release();

}  // namespace s4vr::eyealign
