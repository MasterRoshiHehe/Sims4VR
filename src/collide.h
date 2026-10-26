#pragma once

// Walls and furniture you collide with in first person (docs/COLLISION-PLAN.md).
//
// collide.py builds the shapes around you (walls with doorways cut out,
// object footprints with the height range their model covers) and sends them
// here whenever they change. The game thread pushes your body out of them
// every frame (player.cpp), whether the stick or your own feet took you in.
namespace s4vr::collide {

// Python (any thread): the scene, packed as floats:
//   wall count, then 6 per wall side (ax, az, bx, bz, y0, y1),
//   shape count, then per shape: point count n, y0, y1, flags (1: stops your
//   body), then n x, z pairs.
// An empty or short array clears the scene.
void setScene(const float* data, int count);

// [body] collision and body_radius (debug menu: Controls > Moving).
void configure(bool on, float bodyRadius);

// Game thread: pushes (x, z) out of the walls and body-stopping objects
// around feet height feetY, keeping radius clear (world metres). A point
// already deeper than maxDepth inside something is left alone (teleported
// in: you walk out). Returns how far it moved (0: untouched); `what` names
// the kind of shape it last pushed out of (0 wall, 1 object).
float pushOut(float& x, float& z, float radius, float feetY, float maxDepth, int& what);

// Game thread: on, with a scene; radius from configure (game metres, scale 1).
bool active(float& bodyRadius);

}  // namespace s4vr::collide
