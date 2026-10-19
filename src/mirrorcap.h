#pragma once

// Caps how far mirrors and water reflections render (user A2 option).
//
// The game reads its render options when graphics start (loader rva 0x11ca5c0,
// game 1.128.90.1030) through OptGetFloat / OptGetBool (rva 0x117e520 / 0x117e430;
// see re/exe/mirror_far_plane.md). The mirror camera's far plane is
// InteriorMirrorFarPlane (default 15 m) in rooms and ExteriorMirrorFarPlane
// (default 100 m) outdoors. This hook returns the smaller of the game's value
// and the cap for those two keys, and false for RenderReflectionsEnabled when
// reflections are switched off. It acts when the game reads its options (the
// next start); 0 keeps the game's own value. Untested at runtime.
namespace s4vr::mirrorcap {

void install(float interiorMetres, float exteriorMetres, bool reflectionsOff);

}  // namespace s4vr::mirrorcap
