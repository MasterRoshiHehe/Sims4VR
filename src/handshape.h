#pragma once
#include "armik.h"
#include "camera.h"

// The real shape of your Sim's hands (first person), as the game drew them
// this frame: the palm and every finger segment, from the posed skeleton
// (after our arm IK, finger curls, grips and the game's own animation), as
// capsules. Use it for anything that should hit the whole visible hand:
// lasers, pokes, touches, and later hand tracking, gestures and grabbing.
//
// armik's pose hook reads the hand and finger joints once the game has made
// the final model-space joints (PoseToJoints, then ModelTransform: rows in
// re/exe/.../known.tsv), every frame, and publishes them here in
// OpenXR app space together with the controller pose that solve used. A
// query passes the controller pose of now (armik::Hand), and the shape is
// carried along with it: no frame of lag, and the offset between controller
// and Sim hand (hand_pitch, the laser-pointing turn, a reach limit, a hand
// stopped at a Sim's body) stays as drawn.
//
// All positions are OpenXR app space, metres. Thread-safe: queries never block.
namespace s4vr::handshape {

// Parts: the palm (with the back of the hand) and the five fingers.
enum Part { kPalm = 0, kThumb, kIndex, kMiddle, kRing, kLittle, kParts };
extern const char* const kPartNames[kParts];

struct Segment {
    camera::Vec3 a, b;  // capsule axis ends
    float radius = 0;   // metres
    int part = kPalm;
    int bone = 0;  // palm: 0; fingers: 1 knuckle segment .. 3 tip segment
};

struct Shape {
    bool valid = false;
    int side = 0;  // 0 left, 1 right
    bool tracked = false;  // posed from hand tracking (your real hand), not a controller
    camera::Vec3 wrist;    // the hand joint
    camera::Vec3 palmCentre;  // middle of the palm's surface
    camera::Vec3 palmNormal;  // out of the palm (the inside of the hand)
    camera::Vec3 forward;     // wrist to knuckles
    camera::Vec3 thumbSide;   // across the hand, toward the thumb
    // Each finger's joints from its root out, then its tip: joint[f][0..count]
    // (thumb, index, middle, ring, little = 0..4).
    camera::Vec3 joint[5][4];
    int jointCount[5] = {};
    float size = 1;  // this hand against an adult's (palm length)
    int segmentCount = 0;
    Segment segment[24];
    unsigned long long tick = 0;  // GetTickCount64 of the pose
};

// The hand `side` now: `live` is that controller's pose this frame (the same
// armik::Hand vr.cpp hands to armik::setHands). False if no pose of that hand
// came in the last 300 ms (not first person, hand not tracked).
bool get(int side, const armik::Hand& live, Shape& out);
// The hand as it was posed, in app space, not carried by a controller (for a
// tracked hand, or when no live pose is at hand).
bool latest(int side, Shape& out);

struct Hit {
    int side = 0;
    int part = kPalm;
    int bone = 0;
    float distance = 0;  // along the ray (raycast) or from the point to the surface (closest; negative inside)
    camera::Vec3 point;   // on the hand's surface
    camera::Vec3 normal;  // the surface's outward normal there
    bool palmSide = false;  // on the inside of the hand (palm or the fingers' undersides), not the back
};

// A ray (origin, unit direction) against the hand: the nearest hit within
// maxDistance. `pad` (metres) fattens every capsule, for forgiving aiming.
bool raycast(const Shape& s, const camera::Vec3& origin, const camera::Vec3& dir, float maxDistance, Hit& hit,
             float pad = 0);
// A point against the hand: the closest surface place (Hit::distance is from
// the point to it, negative inside). False if the shape is empty.
bool closest(const Shape& s, const camera::Vec3& point, Hit& hit);
// A sphere (a fingertip, another hand's part) touching the hand: closest with
// a radius. True if it touches.
bool touches(const Shape& s, const camera::Vec3& centre, float radius, Hit& hit);
// One finger's tip (finger 0 thumb .. 4 little). False if unknown.
bool tip(const Shape& s, int finger, camera::Vec3& out);
// How far each finger is curled (0 straight .. 1 a fist), from the shape:
// for gestures. False if unknown.
bool curls(const Shape& s, float out[5]);

// ---- armik's side (game job thread, inside the pose hook) -----------------
struct Raw {
    bool tracked = false;
    camera::Vec3 gripPos;  // the controller grip pose the solve used (app space)
    camera::Quat gripRot;
    camera::Vec3 wrist;    // hand joint
    camera::Quat handRot;  // hand joint's rotation (app space)
    camera::Vec3 forward, thumbSide, palmNormal;  // in the hand joint's frame
    camera::Vec3 joint[5][4];  // app space, root out, then the tip
    int jointCount[5] = {};    // joints + tip (0: finger missing)
};
void publish(int side, const Raw& raw);

}  // namespace s4vr::handshape
