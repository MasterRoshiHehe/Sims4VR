#pragma once
#include <cstdint>
#include <string>

#include "camera.h"

// Your hands on the Sim: its arms reach for the controllers and its fingers
// follow the buttons (trigger = index finger, grip = the other three, any
// thumb button or the thumb rest = thumb).
//
// The game poses every animated actor once a frame on its job threads, ending
// with FUN_140982530 (rva 0x982530; 0x8eb270 in 1.121.361.1020): it turns the
// final pose (each joint relative to its parent, RigPose+0x38, 0x30 bytes:
// float4 position, float4 quat xyzw, float4 scale) into the model-space joints
// that skinning and joint reads use. We hook it and, for the player's Sim
// only, rewrite the arm and finger rotations first: two-bone IK for the upper
// arm and forearm (bone lengths from the current pose, elbow pointing down and
// out), the hand turned like the controller, and the fingers curled from the
// bind pose. Model space
// joints compose as q_model = q_parent * q_local (static analysis of
// 1.112.519.1020, unchanged in 1.121.361.1020, checked again at runtime).
// Refuses to install on other builds.
namespace s4vr::armik {

struct Hand {
    bool valid = false;       // tracked this frame
    camera::Vec3 position;    // grip pose in OpenXR app space (metres)
    camera::Quat orientation;
    float index = 0;          // 0 straight .. 1 curled
    float grip = 0;           // middle, ring and pinky
    float thumb = 0;
    float trigger = 0;        // the trigger itself (0..1), for squeezing a Sim's body
    // Hand tracking (vr.cpp): your real hand, not a controller. Looks only:
    // its fingers take `curl` (radians per joint, from the knuckle out;
    // thumb, index, middle, ring, little) and it touches no Sim.
    bool tracked = false;
    float curl[5][3] = {};
};

void install(bool enabled, float handPitchDegrees, bool headTracking);
// The settings menu: hand angle and head tracking, live.
void configureHands(float handPitchDegrees, bool headTracking);

// Present thread, every frame: both controllers (0 left, 1 right).
void setHands(const Hand hands[2]);

// Camera copy hook, every frame: the player's actor in first person (null
// otherwise), whether you move it freely, where app space sits in the world,
// and the headset's orientation in app space. Head and hands follow you
// either way, in the game's animations too.
void setFrame(void* actor, bool free, const float anchor[3], const camera::Quat& heading, float scale,
              const camera::Quat& hmd, const float eye[3]);

// Camera copy hook, every frame while the game moves your Sim (seated and the
// like): how far your real head moved since you sat down, in the world. The
// spine and neck bend so the Sim's head goes there too. Stops after 300 ms.
void setLean(const float world[3]);
// Any thread: how far the latest solve really moved the head joint by that
// lean, in the world (false: no solve lately).
bool leanDone(camera::Vec3& world);
// Seated upper body ([body] seat_torso): the spine and neck keep the
// animation's slowly averaged posture, without its moves, so only your head
// (setLean) moves them. Off: the animation moves them as before.
void setSeatTorso(bool on);

// Present thread: hold that hand in one fixed pose (your phone), whatever the
// buttons do: its four fingers curled by fingerCurl (0 open .. 1 a fist), its
// thumb bent toward the palm by thumbBend (0 .. 1) and swung away from the
// hand by thumbSpread (radians, negative toward it). The hand also sits moved
// by `move` (metres) and turned by `turn` about the palm, both in the
// controller's grip frame (tilted by hand_pitch); handPose still reports the
// hand without them, so what hangs on it (the phone) stays put. on = false:
// the buttons drive it again.
void setHoldPose(int side, bool on, float fingerCurl, float thumbBend, float thumbSpread,
                 const camera::Vec3& move, const camera::Quat& turn);
// Present thread, every frame: the laser a hand points (grip + A / X), from
// and along, in OpenXR app space. That hand turns and shifts so the Sim's
// straight index finger lies along it; on = false eases the hand back.
void setPointing(int side, bool on, const camera::Vec3& from, const camera::Vec3& dir);
// Any thread: where the player's hand went on the latest solve (under 300 ms
// old): its wrist joint in the world, and the grip frame (tilted by
// hand_pitch, in the world) the hand was turned to, with metres to world units.
bool handPose(int side, camera::Vec3& wrist, camera::Quat& grip, float& scale);
// Any thread: where the player's index fingertip went on the latest solve
// (under 300 ms old), in that controller's grip frame (tilted by hand_pitch),
// metres: add it to the controller's pose for the Sim's fingertip now.
bool fingertip(int side, camera::Vec3& grip);
// Any thread: the player's hand axes in that hand joint's own frame: forward
// (wrist to knuckles, the tilted grip's -Z), toward the thumb (+Y) and their
// cross (the tilted grip's +X). False until the rig is read.
bool handAxes(int side, camera::Vec3& forward, camera::Vec3& thumb, camera::Vec3& up);

// Present thread, every frame: where your eyes look, in OpenXR app space
// (on = false: they follow the game's animation again). The Sim's eyes turn
// toward it, the head stays with your headset.
void setLookTarget(bool on, const float point[3]);

// Main thread: where the controllers' grips (your palms) are in OpenXR app
// space; valid[i] false for an untracked hand.
void palms(camera::Vec3 out[2], bool valid[2]);

// Main thread: which hands hold something (a prop or carried object sits in
// the palm). Those fingers keep the game's grip; the buttons drive the rest.
void setHolding(bool left, bool right);
// Any thread: whether that hand (0 left) holds something.
bool holding(int side);
// Any thread: how far the game's animation curls that hand's middle, ring and
// little fingers (radians, averaged over their joints), before our curls.
float gameCurl(int side);

// Physical touch with other Sims in first person (like Skyrim VR's PLANCK).
// The same hook sees every other Sim's pose as it is made; for Sims near you
// it builds their body as capsules (torso, head, arms, legs), from the joints
// of that very frame:
//  - your hands stop at their bodies instead of passing through (the Sim's
//    hand target is pushed back out of the capsule), and the part you push
//    gives a little (their spine, neck or arm turns away, smoothly, a few
//    centimetres at most);
//  - close a hand around one of their hands and it holds yours: their arm
//    reaches for your hand, palm to palm, until you open the hand or they get
//    out of reach (lead them, dance);
//  - lean in close in front of a Sim and its head turns to look at you;
//  - Python-free walking can't take you through them (player.cpp).
//  - squeeze grip and trigger together on any part of their body and that
//    hand holds it (user 2026-10-17): the part is pulled along with your hand
//    a little, and the script mod (bodygrab.py) gets a touch event naming
//    the place, for the Sim's reaction.
// Nothing here runs a game interaction or changes relationships.
void configureTouch(bool bodies, bool holdHands, bool lookAt, bool grabBodies);
// Touch events for the script mod, oldest first: kind 1 a hand took hold of a
// Sim's body (or its hand), 2 let go (value: seconds held), 3 pulled it hard
// (value: metres). region: kGrabRegionNames below. Returns false if none.
struct TouchEvent {
    int kind = 0, hand = 0, region = 0;
    void* actor = nullptr;
    float value = 0;
};
bool takeTouchEvent(TouchEvent& e);
// How far the fingers close on a held body (0..1) and how far (metres, for an
// adult) the held place can be dragged; Claude Jiggle's breasts and butt keep
// their own settings.
void configureGrab(float curl, float drag);
// Sims (client objects) whose body may only be held at the hand, arms,
// shoulders, back and hair: children and younger, and teens with an adult
// (or adults with a teen). The script mod sends them; up to 16.
void setTouchRestricted(void* const* actors, int count);
// Any thread: how deep that hand (0 left) pressed into another Sim on its
// latest solve, in metres (0: not touching), and whether it holds a Sim's hand.
float contact(int side);
bool holdingSim(int side);
// Any thread: where the other Sims near you stand (world x, z), as of their
// latest poses (under 300 ms old). Returns how many were written.
int nearbySims(float (*xz)[2], int max);
// Your head against other Sims (player.cpp, [physical] head): how far a sphere
// of `radius` (world units) at `eye` (world) must move sideways (push: x, z)
// to be out of their head, torso, upper arms and thighs, as of their latest
// poses near you. Returns how deep it was in (0: clear). Needs their bodies
// ([physical] bodies), which this reads.
// headAndTorso: only their head and torso count (in the game's own animations).
float headOutOfBodies(const float eye[3], float radius, float push[2], bool headAndTorso = false);

// The game's own grip on a prop (the TV remote). Main thread: while the
// game's remote animation runs on your Sim (on), learn how it holds the prop:
// where the hand's prop joint sits in the hand joint's frame and how the
// fingers close, once it holds still in the hand (then learning stops by
// itself; it also stops after 30 s). The other hand gets it mirrored.
void learnGameGrip(bool on);
// Main thread: that hand (0 left) holds our prop: its fingers take the learned grip.
void holdGameGrip(int side, bool on);
// Any thread: where the prop goes on that hand's joint (its frame, rig units),
// learned from the game. False until learned (or loaded).
bool gameGrip(int side, camera::Vec3& position, camera::Quat& rotation);
// At start: the grip learned in an earlier session (sims4vr_grips.ini), kept there when a new one is learned.
void loadGameGrip(const std::wstring& path);
// Main thread: write a newly learned grip to that file.
void saveGameGrip();

// Your voice on your Sim (voice.cpp): the Sim whose jaw opens (its actor, as
// the game thread finds it; nullptr = none), and how far, in radians, about
// the head's left-right axis on top of the game's animation.
void setVoiceActor(void* actor);
void setMouth(float radians);

}  // namespace s4vr::armik
