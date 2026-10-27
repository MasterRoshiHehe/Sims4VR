#pragma once

// Ragdoll for other Sims (docs/RAGDOLL.md; user 2026-10-27 14:59Z): a small
// position-based physics body of 15 points (the main joints of a Sim) that
// takes over the Sim's pose from the game's animation, falls, lies on the
// floor, and blends back. First person only (callers gate on that).
//
// This module knows nothing about the game's rig: armik.cpp (touchOther) feeds
// it the points of the game's own pose each frame and writes the points it
// gives back into the Sim's joints. World space, metres, Y up.
//
// Interface for everyone who wants a Sim to fall (punches, knockout, shove):
//   ragdoll::Params p; p.velocity = {...}; ragdoll::start(actorPtr, p);
//   ragdoll::release(actorPtr);   // blend back to the game's pose
namespace s4vr::ragdoll {

// The points, in the order of the arrays below. Left is the rig's L side.
enum Point {
    kPelvis, kHipL, kHipR, kNeck, kHead, kShoulderL, kShoulderR, kElbowL, kElbowR, kHandL, kHandR,
    kKneeL, kKneeR, kFootL, kFootR, kPoints,
    // Virtual points the body adds itself (not joints): one above the pelvis
    // and one above the neck along the spine, so the pelvis and the chest each
    // have an orientation and the spine can bend between them.
    kPelvisUp = kPoints, kChestUp, kSimPoints
};

struct Params {
    float velocity[3] = {0, 0, 0};  // m/s the blow gives the body (world)
    float point[3] = {0, 0, 0};     // where it landed (world), if hasPoint
    bool hasPoint = false;          // else the whole body gets it evenly
    float limpSeconds = 4.0f;       // limp this long, then blend back; 0: until release()
};

// [ragdoll] settings (debug menu: Other Sims > Ragdoll falls), read live.
bool enabled();        // [ragdoll] on
bool testOnHit();      // [ragdoll] test_on_hit: your slaps and punches drop the Sim (step 1 test)
float blowStrength();  // [ragdoll] strength: how much of your hand's speed the body takes
float limpSeconds();   // [ragdoll] limp_seconds

// Any thread. False: not started (off, or this Sim is already a ragdoll).
bool start(const void* actor, const Params& params);
// Any thread: blend back to the game's pose and end.
void release(const void* actor);
bool active(const void* actor);
// Any thread: end at once, no blend (the Sim can't be posed).
void cancel(const void* actor);

// Pose hook: this Sim's joints in the game's pose (`in`), the way it faces
// (world, unit), its floor height, its size against an adult (1) and the
// seconds since the last call. Fills `out` with the points to pose it at and
// returns true; false: no (more) ragdoll for this Sim.
bool step(const void* actor, const float in[kPoints][3], const float forward[3], float floorY, float size, float dt,
          float out[kSimPoints][3]);

// The orientations (x y z w) of the pelvis (hips) and the chest (shoulders).
// sim false: from the game's joints (the first kPoints points of `pts`);
// true: from the ragdoll's own (all kSimPoints, as step() gives them).
void frames(const float pts[kSimPoints][3], bool sim, float pelvis[4], float chest[4]);

}  // namespace s4vr::ragdoll
