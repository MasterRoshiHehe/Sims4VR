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
// [body] furniture_radius: how far your body keeps clear of furniture (walls
// keep body_radius). Smaller than the walls' so you squeeze between pieces
// placed close together, as Sims do (their routing radius is 0.123 m).
void setFurnitureRadius(float metres);
float furnitureRadius();

// Game thread: pushes (x, z) out of the walls and body-stopping objects
// around feet height feetY, keeping radius clear of walls and objectRadius
// clear of objects (world metres). A point
// already deeper than maxDepth inside something is left alone (teleported
// in: you walk out). Returns how far it moved (0: untouched); `what` names
// the kind of shape it last pushed out of (0 wall, 1 object).
float pushOut(float& x, float& z, float radius, float objectRadius, float feetY, float maxDepth, int& what);

// Game thread: on, with a scene; radius from configure (game metres, scale 1).
bool active(float& bodyRadius);

// [body] hand_collision (debug menu: Controls > Moving): your Sim's hands stop
// at walls and furniture.
void configureHands(bool on);

// The pose hook (armik.cpp), every solve of your Sim's arm (side 0 left):
// where your real palm is (`target`, world) and the fingertips from it
// (`reach`, world vector). Gives where the drawn palm goes: moved from its
// last place toward yours in small steps, kept `radius` clear of walls and
// objects (sliding along them), so it stops at the surface while your real
// hand goes on, and catches up as you pull back. Starts over at your hand
// after a jump of over `snap` or a pause. Returns how far apart they are
// (0: the drawn hand is where yours is).
float stopHand(int side, const float target[3], const float reach[3], float radius, float snap, float out[3]);

}  // namespace s4vr::collide
