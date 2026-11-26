#pragma once

// Walls and furniture you collide with in first person (docs/COLLISION-PLAN.md).
//
// collide.py builds the shapes around you (walls with doorways cut out,
// object footprints with the height range their model covers) and sends them
// here whenever they change. The game thread pushes your body out of them
// every frame (player.cpp), whether the stick or your own feet took you in.
namespace s4vr::collide {

// Python (any thread): the scene, packed as floats:
//   wall count, then 7 per wall side (ax, az, bx, bz, y0, y1, fence 0/1),
//   shape count, then per shape: point count n, y0, y1, flags (1: stops your
//   body; 2: what your hands meet of an object, its model's box; 4: a
//   footprint your hands skip, as it has a 2), then n x, z pairs.
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
// [body] hand_wall_gap / hand_object_gap (debug menu, Controls > Moving): metres
// your hand keeps from walls / furniture beyond its own shape (minus: it sinks in).
void setHandGaps(float wall, float object);

// [body] soft_plants / plant_slowdown (debug menu, Controls > Moving): plants
// (shapes with flag 16: bushes, flowers, the leaves of other plants and trees)
// don't stop your body or hands; walking into a bush's footprint (flag 32)
// slows you, up to maxSlow (0.5 = half speed) deep in a tall one.
void configurePlants(bool soft, float maxSlow);
// Game thread: the stick walking speed factor at (x, z) for feet at feetY
// (1: nothing slows you; 1 - maxSlow at most).
float plantSpeed(float x, float z, float feetY);

// [body] step_over / step_height (debug menu, Controls > Moving): objects up
// to maxHeight tall (shapes with flag 64, marked by collide.py) don't stop your
// body or hands; you walk over them.
void configureStep(bool on, float maxHeight);
// Game thread: how far above the ground (y `ground` under your head) your feet
// ride at (x, z): the top of the stepped-on object there (0 on bare ground).
float stepHeight(float x, float z, float ground);

// The pose hook (armik.cpp), every solve of your Sim's arm (side 0 left):
// where your real palm is (`target`, world) and the hand's shape around it:
// `count` balls, each `offsets` (x, y, z world vector from the palm) with
// its radius (the palm's and every finger segment's thickness, from
// handshape). Gives where the drawn palm goes: moved from its last place
// toward yours in small steps, every ball kept out of walls and objects
// (sliding along them), so the hand rests flush on the surface while your
// real hand goes on, and catches up as you pull back. Caught behind
// something (a door jamb as you walk through, the side of a flight of
// steps), it comes the way your arm does instead, out from `shoulder`
// (world; null: never), when that gets it nearer your hand. Starts over at
// your hand after a jump of over `snap` or a pause. Returns how far apart
// they are (0: the drawn hand is where yours is).
float stopHand(int side, const float target[3], const float* offsets, const float* radii, int count, float snap,
               const float* shoulder, float out[3]);

// Any thread (the pose hook: touch sounds, armik.cpp): your hand, a ball of `radius` at
// (x, y, z), touching a wall or an object of the scene (on or off [body] collision). The
// deepest contact: its normal (out of the surface toward the hand: the top of a table is
// straight up) and material (collide.py: 0 hard, 1 metal, 2 soft, 3 glass; 4 a wall).
bool handContact(float x, float y, float z, float radius, float normal[3], int& material);

}  // namespace s4vr::collide

namespace s4vr::collide {

// Muffling (Extended Sound Control's esc_set_occlusion): collide.py sends what blocks sound, as floats:
//   segment count, then 7 per segment (ax, az, bx, bz, y0, y1, weight 0..1: the share it blocks per crossing),
//   floor count, then per floor slab: y, weight.
// An empty array switches it off.
void setOcclusion(const float* data, int count);
// Audio thread: false while off (nothing sent to the sound mod).
bool occlusionActive();
// Audio thread: 0 (clear) .. 0.95 (nearly blocked) between a listener and a sound (world metres x, y, z).
float occlusion(const float* from, const float* to);

}  // namespace s4vr::collide
