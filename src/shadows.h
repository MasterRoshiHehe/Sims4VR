#pragma once

// Steady sun shadows in first person.
//
// The game fits each of its sun-shadow cascades to the view every frame
// (FUN_141225f60, rva 0x1225f60, called per cascade by the renderer):
// fit(sun direction, view camera, the cascade's light camera, settings,
// cascade index, shadow map size). The settings are Renderer.GlobalShadowSettings
// (renderer+0x570): snap intervals, depth bias and two cascade layouts, one
// for a camera close to what it looks at and one for a camera far off, blended
// by that distance. The fit keeps a cascade's centre until the view has moved
// mPositionSnapInterval, and it snaps the light camera to whole shadow-map
// texels, so moving the camera doesn't make edges crawl, but only while the
// sun direction is exactly the last frame's. The sun moves with the clock
// every frame, so that snap never happens and every shadow edge shimmers.
// In first person the head never holds still, cascade 0 starts at the view's
// near plane (which follows what is close), and the zoom blend follows our
// camera, which made the shadows wobble more.
//
// In first person this gives the fit a sun that only moves in small steps (so
// the texel snap works between steps), one fixed cascade layout (the close
// one, its first cascades pulled in close), and a fixed near plane. God view
// keeps the game's own shadows.
//
// The shadow map's size (ShadowMapSize, 2048 at the top Lighting setting in
// GraphicsRules.sgr) is read when the game makes the map: at start and on every
// display reset. While the headset view is on (our render resolution resets
// the display when it starts) the game is given a bigger one, in god view too.
//
// Static analysis of 1.121.361.1020 (DX11 and DX9); off on other builds.
namespace s4vr::shadows {

// Hooks the fit. stepDegrees: how far the sun moves before the shadows follow
// (0: every frame, as the game does). sharpRange: metres the first (sharpest)
// cascade reaches in first person (0: the game's 10). mapSize: shadow map
// pixels while the headset view is on (0: the game's own; at most 4096).
void install(bool on, float stepDegrees, float sharpRange, int mapSize);
void configure(bool on, float stepDegrees, float sharpRange);

}  // namespace s4vr::shadows
