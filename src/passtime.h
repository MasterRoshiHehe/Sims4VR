#pragma once

// Both real eyes: CPU time per eye of the scene lists' other stages (job threads), to find the next
// part of the right eye's pass worth leaving out (v0.13.321). Logs "Pass time" every 10 s; changes
// nothing in the game.
#include <cstdint>

namespace s4vr::passtime {

void install();
// Runs of the water/mirror reflection stage (0x11c99f0) that have finished, on any thread.
uint32_t waterDone();
// The scene object (FrameBegin's first argument) of the main view: only its water runs are counted (other views,
// such as portraits, run the same stage).
void setMainScene(void* scene);
// FrameBegin of the main view finished: keep its water state for this eye (0 left, 1 right).
void noteFrameBegin(void* scene, int eye);
// Times the water state had to be put back since the last call.
uint32_t stateRestores();

}  // namespace s4vr::passtime
