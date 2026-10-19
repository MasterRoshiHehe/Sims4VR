#pragma once

// Frame map ([stereo] frame_map, diagnostic): the order of the game's per-frame
// render work and the call stacks behind it, logged for a few frames, so the
// both-eyes-per-frame mode (docs/STEREO-FEASIBILITY.md step 1) can be built on
// the game's real render entry. Nothing is changed while it runs; every mark
// is one atomic read when it is off.
namespace s4vr::framemap {

enum Site {
    CameraCopy = 0,  // the game's camera copy into the render camera (camera.cpp hookedCopy)
    SceneBind,       // the scene's colour target bound with its depth (uihook.cpp setTargets)
    DepthOnlyBind,   // a depth-only bind (shadow maps)
    OtherBind,       // any other bind with a depth-stencil (reflections, pick ...)
    ColourBind,      // a bind with no depth-stencil (post passes, UI)
    SceneDraw,       // a Sim drawn (facehide.cpp hookedSceneDraw): extra 1 = in your view
    CullRooms,       // room culling (gamefix.cpp): extra = view kind
    UiRender,        // the UI's render (uihook.cpp hookedRender)
    Present,         // the swapchain Present (d3d11_hook.cpp)
    SiteCount
};

void init(bool enabled);
void mark(Site site, int extra = 0);

}  // namespace s4vr::framemap
