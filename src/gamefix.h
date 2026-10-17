#pragma once

// Fixes for game bugs that VR play runs into.
//
// Room culling: the game culls each lot's rooms and portals once per frame per
// view kind (FUN_140a4bed0, rva 0xa4bed0; 0xadbef0 in 1.112). Views of the
// same kind (shadow cascades, reflections) run as parallel jobs, and up to
// 1.112 only guarded the work with a plain frame-number check, so two threads
// could rebuild the same room lists at once and write past the end of a
// buffer. That corrupted the game's heap and it crashed later in its allocator
// (dump 2026-10-12 03:36: two worker threads in the same object and view
// kind). 1.121 takes a lock of its own around it; the hook runs under that
// same lock. In first person it keeps rooms you just saw drawn for half a
// second, so a doorway the culling briefly loses doesn't flash the room beyond
// empty; and ([body] rooms_whole) a room seen through a doorway is drawn
// whole and stays drawn while you stand where you saw it, so standing in a
// door frame doesn't hide the things in the room ahead.
//
// Near fading: the game hides objects when the camera is inside their bounds
// (plus 1 m on the active lot), and fades objects, walls and Sims close to the
// camera. With near_fade = 0 neither happens in first person.
//
// All of it changes the game only in first person; god view keeps the game's
// own behaviour.
//
// Static analysis of 1.112.519.1020, rechecked on 1.121.361.1020 (DX11);
// refuses to install on other builds.
namespace s4vr::gamefix {

void install(bool nearFade);

// The game's main thread: whether you are in first person (every frame the VR
// camera runs, and when first person ends).
void setFirstPerson(bool on);

// The game's main thread: the level the active lot shows (Page Up / Page
// Down), or kNoLevel.
constexpr int kNoLevel = -1000;
int shownLevel();

// First person, the game's main thread, every frame: put walls-up / all
// floors back if the game dropped it (cheap check; re-applies at most twice
// a second).
void keepTopFloor();

}  // namespace s4vr::gamefix
