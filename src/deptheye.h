#pragma once
#include <d3d11.h>

// Both eyes every frame from one game frame ([stereo] depth_eye, first person;
// docs/handoffs/SINGLE-VIEW-HANDOFF.md option D).
//
// The game draws one eye per frame (alternate eyes). With this on, every
// frame the other eye is made from the eye just drawn: each of its pixels
// looks back into the fresh picture along its own ray, stepping to the
// surface the scene depth (nearplane's copy) puts there (an iterative warp,
// sharp at edges). Pixels that find no surface keep the other eye's own last
// picture turned to now (eyealign::draw): edges of the view, behind near things. Both eyes then show the
// same moment, so moving Sims no longer differ between the eyes, and each
// eye's picture changes every frame instead of every other one.
namespace s4vr::deptheye {

// Present thread, the immediate context: draws `source` (the fresh eye's
// picture, width x height) into `target` (the other eye's swapchain image,
// the same size, viewed as `format`), moved by the scene depth of this frame.
// rows: 3x4 row-major, a point in the source view (x right, y up, z ahead,
// metres) to the target view; srcTan / dstTan: tan of each view's left,
// right, up, down; nearZ / farZ: the planes the depth was drawn with. Depth
// tested against nothing but itself: what `target` holds stays where nothing
// lands. False when nothing was drawn (no scene depth of that size, the pass
// unavailable).
bool draw(ID3D11ShaderResourceView* source, ID3D11Texture2D* target, DXGI_FORMAT format, UINT width, UINT height,
          const float rows[12], const float srcTan[4], const float dstTan[4], float nearZ, float farZ);

// Drops the views of swapchain images (call before the runtime destroys them).
void forgetTargets();
// Frees everything (session end).
void release();

}  // namespace s4vr::deptheye
