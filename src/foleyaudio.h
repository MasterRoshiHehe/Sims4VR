#pragma once
#include <string>

// Your own touch sounds (user 2026-10-26): WAV files in the mod folder's
// sounds\touch\<kind>\ (48 kHz mono 16-bit, made from the user's files), played
// straight to the speakers with XAudio2 on our own thread, in 3D where the touch is
// (binaural: the far ear later, quieter and duller, quieter further away). A kind
// without files (or [foley] own_sounds = 0) leaves the touch to foley.py's game sounds.
namespace s4vr::foleyaudio {

enum Kind { kTouch, kPat, kSlap, kPunch, kGrab, kObjects };
// Your hand hitting an object or wall (user 2026-10-27): kObjects + material * 3 + hit, files in
// sounds\objects\<material>\<hit>\ (hard, metal, soft, glass, wall; tap, slap, punch).
enum ObjectHit { kObjTap, kObjSlap, kObjPunch, kObjHits };
constexpr int kMaterials = 5;  // collide::handContact: 0 hard, 1 metal, 2 soft, 3 glass, 4 wall
constexpr int kLightSwitch = kObjects + kMaterials * kObjHits;  // sounds\objects\lightswitch*.wav (lightswitch.py)
constexpr int kKnockDoor = kLightSwitch + 1;    // sounds\knock\door\*.wav (knock.cpp, armik objectTrack)
constexpr int kKnockWindow = kLightSwitch + 2;  // sounds\knock\window\*.wav
constexpr int kKinds = kKnockWindow + 1;

// Once at start: where the mod is (the sounds load on the audio thread at the first touch).
void init(const std::wstring& modDir);
// Any thread: the kind for a touch, or -1 for none of ours. firstContact: your hand met a
// body at `into` metres a second (fist: grip and trigger held); grab: grip + trigger took
// hold; theirHand: the place is their hand (left to foley.py's high five).
int kindFor(bool grab, bool fist, bool theirHand, float into);
// Any thread: a hit at `into` m/s: kPunch (fist, [foley] punch_speed), kSlap (open hand,
// slap_speed) or -1, whether or not there are sounds for it (knockback).
int hitKind(bool fist, float into);
// `into` held under the slap and punch speeds (walk_hits off while you walk).
float belowHits(float into);
// Any thread: an open palm meeting a body at `into` m/s with `along` m/s sideways is a shove
// ([foley] shove, shove_speed, shove_ratio: along at most that share of into): forward, not a swipe.
bool isShove(bool fist, float into, float along);
float shoveForce();      // [foley] shove_force: share of your hand's speed the body takes
float shoveFallSpeed();  // [foley] shove_fall_speed: from this speed they fall, below they stumble
float shoveLimp();       // [foley] shove_limp: seconds a fallen Sim lies limp
bool walkHits();    // [foley] walk_hits: hits count while the stick moves you
// Any thread: a hit on an object at `into` m/s: kObjPunch (fist, punch_speed), kObjSlap (open
// hand, slap_speed), kObjTap (either, [foley] object_tap_speed) or -1 (too soft, or [foley]
// objects off).
int objectHit(bool fist, float into);
// Your own sound kind for it (that material's files, else hard's), or -1 for foley.py's.
int objectKind(int material, int hit);
// [foley] knock (on/off) and knock_speed: a fist into a door or window at least this fast (m/s) knocks.
float knockSpeed();  // 0 when knocking is off
float knockback();  // [foley] knockback: 0 none, 1 normal, 2 twice as far
// Any thread (the pose hook too): plays one sound of that kind at `at` (metres from the
// middle of your eyes, in the headset's frame: +x right, +y up, +z behind), `strength`
// 0..1 (how hard). False when there's none to play.
bool play(int kind, const float at[3], float strength);
// The phone's own ringtones (sounds\\ringtones\\*.wav in name order): ringtone n at `at`, stopped with stopRing (the call
// is over). ringCount: how many were found.
bool playRing(int n, const float at[3], bool notification = false);  // notification: sounds\\notifications instead
void stopRing();
int ringCount();
// Every frame (VR code): where the phone is in the headset's frame (+x right, +y up, +z behind), metres. The ringtones and
// notification sounds follow it (streamed, re-placed every 10 ms); without it for 0.3 s they stay where they started.
void setPhoneAt(const float at[3]);
// The elevator ride (sounds\\elevator\\*.wav, the first): plays from `at` (headset frame) and is placed with setElevatorAt every
// frame while elevatorPlaying(); stopElevator ends it. playElevator is false without the file.
bool playElevator(const float at[3], float fromSeconds = 0.0f);
void stopElevator();
bool elevatorPlaying();
bool takeElevatorAnchor();  // VR code: true once per playElevator, when to fix the sound above the head
void setElevatorAt(const float at[3]);

}  // namespace s4vr::foleyaudio
