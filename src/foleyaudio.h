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
constexpr int kKinds = kObjects + kMaterials * kObjHits;

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
bool walkHits();    // [foley] walk_hits: hits count while the stick moves you
// Any thread: a hit on an object at `into` m/s: kObjPunch (fist, punch_speed), kObjSlap (open
// hand, slap_speed), kObjTap (either, [foley] object_tap_speed) or -1 (too soft, or [foley]
// objects off).
int objectHit(bool fist, float into);
// Your own sound kind for it (that material's files, else hard's), or -1 for foley.py's.
int objectKind(int material, int hit);
float knockback();  // [foley] knockback: 0 none, 1 normal, 2 twice as far
// Any thread (the pose hook too): plays one sound of that kind at `at` (metres from the
// middle of your eyes, in the headset's frame: +x right, +y up, +z behind), `strength`
// 0..1 (how hard). False when there's none to play.
bool play(int kind, const float at[3], float strength);

}  // namespace s4vr::foleyaudio
