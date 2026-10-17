#pragma once
#include "phone3d.h"

// First person: the guiding trail (GAMEPLAY-PLAN.md stage C, step C2; user
// 2026-10-15 21:27Z, footprints 21:28Z). An action you picked far away waits
// for you to walk there yourself (scriptmod trail.py holds the game's walk);
// the route the game planned for it is drawn on the floor as glowing
// footprints, left and right, ending in a pair standing side by side where the
// action starts, facing the way your Sim will face. Footprints behind you are
// gone, the ones at your feet fade in, a soft shimmer runs along them toward
// the goal. Drawn by phone3d into the game's view with its scene depth, so
// walls and furniture hide them. DX11 only.
namespace s4vr::trail {

constexpr int kMaxPoints = 256;
constexpr int kMaxPrints = 96;

// Any thread (Python): the path, world units: count points x, y, z (the floor
// along the route), and the goal: x, y, z and the facing there (fx, fz). count
// 0: no trail.
void setPath(const float* points, int count, const float goal[5]);
void clear();
bool active();

// Render thread (phone3d::draw), first person only: this frame's footprints as
// clear panels (crop: the left foot as is, the right one mirrored); the camera
// at (x, z) decides which ones are behind you. scale: game units per metre.
int build(phone3d::Panel* out, int max, float x, float z, float scale);

}  // namespace s4vr::trail
