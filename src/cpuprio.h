#pragma once

// CPU placement for the two threads a frame waits on ([perf] fast_cores, docs/PERF-PLAN.md section 8, item 7):
// the game's main thread (scene render, Python, Sim builds) and our frame clock thread (sends every headset refresh).
// Both get a soft preference for the performance cores (CPU sets of the highest efficiency class, so Windows can still
// use the others when those are busy), no power throttling (EcoQoS), and a higher thread priority: the clock thread
// HIGHEST plus the multimedia "Games" class, the main thread ABOVE_NORMAL. Nothing TIME_CRITICAL (that would starve
// Virtual Desktop's streamer and the audio threads). Game job threads are left alone. Logged once per thread.
namespace s4vr::cpuprio {

void configure(bool on);
void boostMainThread();   // call on the game's main thread (once is enough; later calls do nothing)
void boostClockThread();  // call at the start of the frame clock thread

}  // namespace s4vr::cpuprio
