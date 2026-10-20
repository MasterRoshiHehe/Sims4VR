#pragma once
#include <d3d11.h>

// Both eyes every frame from one game frame ([stereo] depth_eye, first person;
// docs/handoffs/SINGLE-VIEW-HANDOFF.md option D).
//
// The game draws one eye per frame, alternating. With this on, the game draws
// the left eye every frame (its view widened to cover the right eye's too) and
// the right eye is made from it: each of its pixels looks back into the left
// picture along its own ray, stepping to the surface the scene depth puts
// there (an iterative warp, sharp at edges); where the left eye can't see
// (beside near things) the surface behind is stretched in. Our own 3D pieces
// (phone, menus, pie) write their depth for it (phone3d::setWriteDepth). Both
// eyes show the same moment and change every frame; the right eye's mistakes
// stay put instead of flickering between the eyes.
namespace s4vr::deptheye {

// Present thread, the immediate context: draws `source` (the fresh eye's
// picture, width x height) into `target` (the other eye's swapchain image,
// the same size, viewed as `format`), moved by the scene depth of this frame.
// rows: 3x4 row-major, a point in the source view (x right, y up, z ahead,
// metres) to the target view; srcTan / dstTan: tan of each view's left,
// right, up, down; nearZ / farZ: the planes the depth was drawn with. Fills
// all of `target`. False when nothing was drawn (no scene depth of that size,
// the pass unavailable).
// older: the other eye's own last picture from the game (eyes taking turns),
// for what the fresh eye can't see behind near things: its colour, a point in
// its view to the target view, its tangents and planes (its depth is the one
// keepDepth kept for that eye). Null: those spots are filled from beside.
struct Older {
    ID3D11ShaderResourceView* colour;
    int eye;
    float rows[12];
    float tan[4];
    float nearZ, farZ;
    double ageMs;
};
bool draw(ID3D11ShaderResourceView* source, ID3D11Texture2D* target, DXGI_FORMAT format, UINT width, UINT height,
          const float rows[12], const float srcTan[4], const float dstTan[4], float nearZ, float farZ,
          const Older* older = nullptr);

// Present thread, after the fresh eye's picture is made: keeps this frame's
// scene depth as that eye's (for Older the next frame). False if it couldn't.
bool keepDepth(int eye);

// Why the last draw returned false, and whether the pass can never be made
// this session (anything else may work next frame).
const char* failure();
bool broken();

// Test view ([stereo] depth_eye_debug): 0 the made eye; 1 its depth (grey,
// red where filled from behind); 2 the fresh picture turned only (no depth
// shift); 3 that with the depth's edges in magenta. The numbers log ("Depth
// eye numbers" / "Depth eye grid") comes on the first picture and on a change.
void setDebugView(int view);
// The near / far planes worked out from the projection the game drew the
// last frame with (for the numbers log; 0 if unknown).
void noteDrawnProjection(float nearZ, float farZ);

// Drops the views of swapchain images (call before the runtime destroys them).
void forgetTargets();
// Frees everything (session end).
void release();

}  // namespace s4vr::deptheye
