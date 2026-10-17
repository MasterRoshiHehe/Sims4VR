#pragma once
#include <cstdint>

#include "armik.h"

// Picking things up with your hands in first person.
//
// Python sends the objects you could pick up near you (grabbables: plates,
// cups, books; carryable and not in use) with a centre and a reach. Closing a
// hand (grip pressed, index finger not pointing) within reach of one grabs
// it: the hand counts as holding it at once (no pointing, no laser) and Python
// gets a grab event, which parents the object to the Sim's hand joint. Opening
// the hand lets go: a release event, and Python puts the object down on the
// nearest surface or the floor. Python also reads where your head and hands
// are in the world (eating: a hand at your mouth) and asks for vibrations.
namespace s4vr::grab {

constexpr int kMaxGrabbables = 12;

// Python: the objects within reach, replacing the previous list.
// xyzr: centre x, y, z (world) and reach in metres, four floats per object.
void setGrabbables(const uint64_t* ids, const float* xyzr, int count);

struct Event {
    int kind = 0;  // 1 grabbed, 2 let go
    int hand = 0;  // 0 left, 1 right
    uint64_t id = 0;
};
// Python: the oldest event not taken yet.
bool takeEvent(Event& out);
// Python: what a hand holds now (0: nothing), after Python grabbed or failed to.
void setHeld(int hand, uint64_t id);

struct Snapshot {
    bool head = false;
    float hmd[3] = {}, forward[3] = {}, up[3] = {};  // world
    struct {
        bool valid = false;
        float palm[3] = {};     // world, the controller's grip (your palm)
        float q[4] = {0, 0, 0, 1};  // world orientation of the grip, xyzw
        float grip = 0, trigger = 0;
        uint64_t held = 0;
    } hand[2];
};
// Python: where your head and hands are now. False until first person runs.
bool snapshot(Snapshot& out);

// Python: vibrate a controller (amplitude 0..1, seconds).
void haptic(int hand, float amplitude, float seconds);
// Render thread: a vibration asked for that hand, if any.
bool takeHaptic(int hand, float& amplitude, float& seconds);

// Render thread, every frame: controllers and head in OpenXR app space.
// pointing[i]: that hand points (index finger out). Keeps the snapshot while
// in first person, and runs the grab and let-go edges when grabbing is on
// (a hand's grip counts even while its pose is lost, so a held object stays
// held). firstPerson false lets go of everything.
struct HeadPose {
    bool valid = false;
    camera::Vec3 position;
    camera::Quat orientation;
};
void update(const armik::Hand hands[2], const float trigger[2], const bool pointing[2], const HeadPose& head,
            bool firstPerson, bool grabbing);

// Any thread: the object that hand holds (0: none).
uint64_t holding(int hand);

// Controller shortcuts for Python: 1 switch first person / game camera, 2 next Sim.
void pushControl(int what);
bool takeControl(int& what);

}  // namespace s4vr::grab
