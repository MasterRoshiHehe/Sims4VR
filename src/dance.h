#pragma once
#include "armik.h"
#include "grab.h"

// Dancing in real life (thread "Research passive actions framework", user
// 2026-11-01; docs/PASSIVE-ACTIONS.md section 3). Read from your headset and
// controllers (or tracked hands) in OpenXR app space, so it is your body, not
// the Sim's. Python (passive.py) decides from these numbers whether you dance
// to the music you hear and gives the game's own dance gains (passive_gain.py,
// the stereo's dance interaction's tuning). Your Sim plays nothing.
//
// Every 1/30 s a sample: head height, head place on the floor, both hands
// relative to the head. Over the last 4 s:
// - energy: rms of the head's up/down speed + half the mean hand speed
//   relative to the head (m/s). Standing still ~0.05, dancing 0.3..1.
// - rhythm: the best normalised autocorrelation (0..1) of the head's up/down
//   speed plus the hands' speed, for lags 0.27..1.5 s (beats 40..220 a
//   minute), and that lag as bpm.
// - walking: the left stick was used in the last second, or your head
//   moved more than 1 m over the floor in the last 2 s (walking, not dancing).
namespace s4vr::dance {

struct State {
    bool valid = false;  // first person, tracked, a full window
    float energy = 0;
    float rhythm = 0;
    float bpm = 0;
    bool walking = false;
};

// Render thread, every frame (after gesture::update).
void update(const armik::Hand hands[2], const grab::HeadPose& head, float leftStick, bool firstPerson);

// Any thread: the latest numbers.
State current();

}  // namespace s4vr::dance
