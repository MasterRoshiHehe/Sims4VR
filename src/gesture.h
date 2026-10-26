#pragma once
#include "grab.h"

// Hand gestures at other Sims in first person (thread "Hand gesture
// interactions with Sims", 2026-10-24). Read from the posed Sim hand
// (handshape: palm and finger joints after our arm IK), so a gesture is the
// same made with a controller or a tracked hand. Your Sim plays nothing: your
// arm is the gesture. Python picks the Sim it is meant for and has the game
// answer it with its own tuning (scriptmod gestures.py).
//
// Wave: an open hand held up (fingers up, palm away from you, at about
// shoulder height or above) swung side to side: three turns of at least 5 cm
// at the fingertips within 1.6 s.
// Come hither (open palm, no buttons): an open hand, palm up or toward your
// face, its fingertips pulled toward you and back: three turns of at least
// 4 cm within 1.6 s.
namespace s4vr::gesture {

enum Kind { kWave = 1, kComeHither = 2 };

struct Event {
    int kind = 0;
    int hand = 0;  // 0 left, 1 right
    float head[3] = {};     // world, your eyes
    float forward[3] = {};  // world, where you look
    float palm[3] = {};     // world, the waving hand's palm
};
// Python: the oldest gesture not taken yet.
bool takeEvent(Event& out);

// Render thread, every frame (after grab::update): head in OpenXR app space.
void update(const grab::HeadPose& head, bool firstPerson);

}  // namespace s4vr::gesture
