#pragma once
#include "camera.h"

// The real shape of Sims' bodies, heads and hair (first person), as the game
// drew them this frame: capsules along the posed skeleton's bones, plus a
// skull sphere. Companion of handshape.h (hands and fingers); together they
// are the whole visible Sim for lasers, pokes, touches, head repel, grabbing
// and gestures.
//
// armik's pose hook builds one Shape for the player's Sim and one for every
// other Sim within kRange of your eye (default 1.2 m), from the joints of
// that very pose, and publishes it here in OpenXR app space (the same space
// handshape uses). Fingers are left to handshape; face bones (eyes, jaw,
// brows, lips) are left out; hair is the chains of bones that hang off the
// head (long hair, ponytails, ...), so a Sim with short hair has none.
//
// Queries never block. Positions: OpenXR app space, metres.
namespace s4vr::bodyshape {

enum Region {
    kTorso = 0,  // pelvis, spine, neck, shoulders
    kHead,       // the skull sphere
    kHair,       // chains hanging off the head
    kUpperArm,
    kForearm,
    kThigh,
    kCalf,
    kFoot,
    kOther,      // other bones (twists, tails, skirts, ...)
    kRegions
};
extern const char* const kRegionNames[kRegions];

constexpr int kMaxSegments = 96;
constexpr int kMaxSims = 9;  // the player plus eight others

struct Segment {
    camera::Vec3 a, b;  // capsule axis ends (a == b: a sphere)
    float radius = 0;
    uint8_t region = kOther;
    uint8_t side = 2;    // 0 left, 1 right, 2 middle
    int16_t joint = -1;  // the child joint of the bone (rig index)
};

struct Shape {
    bool valid = false;
    bool player = false;      // your own Sim
    const void* actor = nullptr;  // the game's actor (compare with native.object_ptr)
    float size = 1;           // against an adult (pelvis to head joint)
    camera::Vec3 centre;      // bounds
    float boundRadius = 0;
    camera::Vec3 head;        // skull sphere centre
    float headRadius = 0;
    camera::Vec3 up, forward; // the Sim's up, and the way it faces (horizontal)
    int segmentCount = 0;
    Segment segment[kMaxSegments];
    unsigned long long tick = 0;
};

struct Hit {
    const void* actor = nullptr;
    bool player = false;
    int region = kTorso, side = 2, joint = -1, segment = -1;
    float distance = 0;   // raycast: along the ray; closest: to the surface, negative inside
    camera::Vec3 point;   // on the surface
    camera::Vec3 normal;
};

// The Sims that have a shape now (under 300 ms old): how many, and each one.
int count();
bool get(int index, Shape& out);          // index 0..count()-1
bool forActor(const void* actor, Shape& out);
bool forPlayer(Shape& out);

// One shape: ray (unit direction), nearest point, sphere.
bool raycast(const Shape& s, const camera::Vec3& origin, const camera::Vec3& dir, float maxDistance, Hit& hit,
             float pad = 0);
bool closest(const Shape& s, const camera::Vec3& point, Hit& hit);
bool touches(const Shape& s, const camera::Vec3& centre, float radius, Hit& hit);

// Every Sim with a shape (includePlayer: yours too).
bool raycastAll(const camera::Vec3& origin, const camera::Vec3& dir, float maxDistance, Hit& hit,
                bool includePlayer = true, float pad = 0);
bool closestAll(const camera::Vec3& point, Hit& hit, bool includePlayer = true);
bool touchesAll(const camera::Vec3& centre, float radius, Hit& hit, bool includePlayer = true);

// The belly of your own Sim: between spine 0 and spine 1 as drawn, a place a
// poke or the laser can aim at (replaces vr.cpp's guess from camera::torso()).
// Returns the capsule hit for a sphere at `centre`, on the torso only.
bool bellyTouch(const camera::Vec3& centre, float radius, Hit& hit);

// ---- armik's side (game job thread) ----------------------------------------
void publish(const Shape& s);

}  // namespace s4vr::bodyshape
