#pragma once
#include <cstdint>
#include <string>

namespace s4vr::voice {

// Your microphone moves your Sim's mouth: a capture thread reads the mic
// (Windows' default input, or the one whose name contains `device`), turns
// how loud you are into how far the jaw opens, and hands that to the pose
// hook (armik), which opens the active Sim's b__Jaw__ that much. Loudness,
// not lip reading: it talks along with you.
struct Settings {
    bool on = true;           // [voice] mic = true / false
    std::string device;       // part of the mic's name; empty = Windows' default input
    float gateDb = -50;       // quieter than this (dBFS) keeps the mouth shut
    float fullDb = -20;       // this loud opens it all the way
    float openDegrees = 14;   // the jaw's widest opening
};

void install(const Settings& settings);
// The Sim whose mouth moves in first person (the active Sim on the lot; 0 =
// none, the mic is released then too).
void setSim(uint64_t objectId);
uint64_t sim();
// First person on or off: the mic is open and the mouth moves only in first
// person (god view stays the game's own).
void setFirstPerson(bool on);
void shutdown();

// What has come in from the mic so far (counters only grow): for telling,
// when the headset link stalls, whether the headset's own mic stream (Virtual
// Desktop sends it over the same link) was still arriving.
struct Flow {
    bool open = false;      // a mic is being read
    uint64_t frames = 0;    // samples read
    uint64_t live = 0;      // 10 ms blocks with any sound at all (not digital silence)
};
Flow flow();

}  // namespace s4vr::voice
