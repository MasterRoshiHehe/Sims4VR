#pragma once
#include <d3d11.h>

// Both eyes every frame from one game frame ([stereo] depth_eye, first person;
// docs/handoffs/SINGLE-VIEW-HANDOFF.md option D).
//
// The game draws one eye per frame (alternate eyes). With this on, every
// frame the other eye is made from the eye just drawn: each point of it is
// moved by its depth (the scene depth nearplane copies) to where the other
// eye sees it, a grid of triangles drawn over the other eye's own last
// picture turned to now (eyealign::draw), which fills what the fresh eye
// can't see (edges of the view, behind near things). Both eyes then show the
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
