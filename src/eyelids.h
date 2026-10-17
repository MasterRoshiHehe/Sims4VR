#pragma once

// Closed eyes while the Sim you are in sleeps or naps: eyelids close over the
// view from top and bottom, the light through them fades to dark, and they
// open again when it wakes. Blinks never do this: only the game's sleep state
// (Python's sim.sleeping, set by the sleep and nap interactions) closes them.
namespace s4vr::eyelids {

// Python, whenever it changes (false outside first person).
void setSleeping(bool sleeping);

struct State {
    float closure = 0;  // 0 open .. 1 shut
    float dark = 0;     // 0 the warm glow through the lids .. 1 black
};
// Present thread, once per eye image: advances by real time and returns the
// look for this image.
State advance();
// Any thread: the Sim you are in sleeps (first person).
bool sleeping();

}  // namespace s4vr::eyelids
