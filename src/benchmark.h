#pragma once

// Turn-in-place benchmark (docs/PERF-PLAN.md item 0). B menu > Advanced > View > Speed test. First person only:
// a 8 s countdown (close the menu, stand still), 5 s standing still, 3 turns of 360 degrees one way and 3 the other
// at [body] turn_rate (the right stick pushed fully, so everything the real stick turn does runs), 5 s still.
// One log line "Benchmark:" at the end. Run it with the game paused and again at speed 1; the label only says which.
// Everything is called from the headset frame (Present) thread.
namespace s4vr {
struct Config;
}

namespace s4vr::benchmark {

void start(bool gamePaused);  // the menu rows
void cancel();
bool active();
// updateControls: replaces the right stick's x while it turns (1 / -1), else leaves it.
void stick(float& rightX);
// updateControls: the stick turn just turned the view by this many radians.
void turned(float radians);
// updateControls: refreshes the turn skipped (display clock steps).
void missedRefreshes(int count);
// turnCheck: one headset frame of stick turning; backDegrees = how far the eyes were turned back.
void turnFrame(bool newPicture, float backDegrees);
// onFrame: every game picture.
void frame(const Config& config, double gameMs, double waitMs, double renderMs, double endMs, bool stereoFrame);

}  // namespace s4vr::benchmark
