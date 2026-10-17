#pragma once
#include <d3d11.h>
#include <string>

// How hard the graphics card is working, for the log: how far it runs behind
// the frames handed to it, and how much video memory the game uses against
// what Windows lets it have. Read when the headset link turns slow, to tell a
// card that is out of time or memory from a stream that stopped taking frames.
namespace s4vr::gpuload {

// Render thread, once a presented frame: marks the end of its GPU work.
void onFrame(ID3D11Device* device, ID3D11DeviceContext* context);

// Time from handing a frame to the card until the card finished it (ms),
// averaged and worst since the last take(). frames is 0 when nothing was
// measured.
struct Lag {
    double averageMs = 0;
    double worstMs = 0;
    int frames = 0;
};
Lag take();
// The last frame's lag (ms), or -1 if unknown.
double latestMs();

// Video memory (MB): what the game uses on the card against the budget
// Windows gives it, and what it uses in system memory. Over the budget,
// Windows starts moving things out of the card's memory, which stalls
// everything that draws. False if the card can't say.
struct Memory {
    unsigned long long usedMb = 0;
    unsigned long long budgetMb = 0;
    unsigned long long sharedMb = 0;
};
bool memory(Memory& out);

// "used of budget MB, N MB more in system memory", or "unknown".
std::string memoryText();

// How busy the card was (percent of the time), from Windows' own counters
// (what Task Manager shows), sampled every 2 s on a thread of its own: its 3D
// engine for everything that draws (the game, the headset's compositor or
// streamer), the game's own share of that, and its video encoder (Virtual
// Desktop and Steam Link stream with it). Near 100% the card is out of time:
// frames queue up and the stream to the headset stutters. samples is 0 when
// nothing was measured (no counters, or too soon).
struct Busy {
    double all3d = 0;   // average
    double game3d = 0;  // average
    double encode = 0;  // average
    double worst3d = 0;
    int samples = 0;
};
// Averaged since the last takeBusy().
Busy takeBusy();
// "3D 85% (the game 70%), worst 99%; video encoder 30%", or "unknown".
std::string busyText(const Busy& busy);
// The latest sample, as busyText.
std::string busyNowText();

// The game's priority on the graphics card: below normal while on is true,
// normal otherwise. Cheap to call every frame (acts only on a change). Logs
// what it set and the priority Virtual Desktop's streamer has.
void yieldToStreamer(bool on);

}  // namespace s4vr::gpuload
