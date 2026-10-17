#pragma once

#include <string>

// Crash recorder. The game's own crash report doesn't get written when its
// memory manager is already broken, so on a fatal exception this writes
// sims4vr_crash.txt (exception, registers, stack with module offsets, what
// the mod did last) and sims4vr_crash_N.dmp (threads and the memory around
// the fault) next to the DLL, before the game's handler runs.
namespace s4vr::crashlog {

void install(const std::wstring& directory);

// What the mod last did, by kind; the crash file says how long ago each was.
enum Activity {
    kCopyHook,        // eye pose written into the render camera
    kJointRead,       // Sim head joint read
    kSetTransform,    // Sim body placed under the player
    kListener,        // audio listener moved to the headset
    kRenderScale,     // display settings re-applied
    kPython,          // a call from the script mod
    kPresent,         // frame sent to the headset
    kActivityCount
};
void note(Activity what);

// Exceptions on this thread are not recorded while quiet (the link snapshot:
// writing another program's dump faults and recovers inside dbghelp).
void quietThread(bool quiet);

}  // namespace s4vr::crashlog
