#pragma once

// Where the game's time between two pictures goes, for the log: before it
// starts drawing (its update), drawing Sims for shadows and mirrors, drawing
// Sims in your view, after the last Sim, and Present with the VR work in it.
// Read to tell what a lower frame rate comes from (user 2026-10-18 21:22Z:
// 50-65 fps). Marks are a timestamp each; one log line every 10 s.
namespace s4vr::framecost {

// The game's Present was called (before the VR work in it) / has returned.
void presentStart();
void presentEnd();
// The game copied the camera it draws with (camera.cpp hookedCopy, main thread).
void renderCamera();
// A Sim drawn through your eyes' camera, or another one (shadows, mirrors).
void simDraw(bool eyes);

}  // namespace s4vr::framecost
