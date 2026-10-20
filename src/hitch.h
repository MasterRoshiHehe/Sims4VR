#pragma once

// Hitch catcher (docs/PERF-PLAN.md item 2, [perf] hitch_catcher): when the game's picture thread goes more than 60 ms
// between two Presents, a watchdog thread samples what that thread is doing every 5 ms (instruction pointer plus the
// return addresses found on its stack) and logs one "Hitch:" line per stall with the most common stacks as
// module+0xrva. Resolve game+0xrva with re/exe/<stamp>/ (known.tsv, classes.tsv). Nothing is allocated or locked while
// the sampled thread is suspended. 10 samples per stall at most, 300 lines per session.
namespace s4vr::hitch {

void install(bool on);
// framecost.cpp calls these from the Present hook (the thread that draws the pictures).
void presentStart();
void presentEnd();
// framecost::renderCamera: the camera copy runs on the game's main thread; that thread is sampled too ("MAIN:" in the line).
void mainThreadSeen();

}  // namespace s4vr::hitch
