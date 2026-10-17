#pragma once

// Outdoor ambience fades in as you get near the outside (first person).
//
// The game's ambience has an inside/outside control (AmbienceInsideOutsideGC).
// Five times a second, when the listener has moved, the client works out how
// far the listener is from the nearest wall between inside and outside
// (FUN_14185da30 -> FUN_14185bbf0, which only looks a short way around) and
// sends that distance, negative inside, to the audio side (controller field
// +0x334c, message 0xf3ac34b3 -> manager +0x3350). Each sound's GC turns it
// into a value: x = (T + d) / 2T through the sound's curve (FUN_141868e80;
// T is the GC's tuned width), so the outdoor sounds switched on within a step
// of the doorway.
//
// In first person the wall search looks out to `fadeMetres`; outside the GC's
// value is its deep-outside one right up to the wall, and inside it goes
// smoothly to its deep-inside value over that distance (v0.13.72; it used to
// fade over the distance either side of the wall, so a porch was quieter).
// God view keeps the game's.
// Inside, outdoors.py says how the way out goes (v0.13.68): through an arch or
// an open door the outdoors is heard fully, through a closed door muffled, and
// only a little through solid walls.
//
// Static analysis of 1.121.361.1020 (DX11 and DX9); other builds get none.
namespace s4vr::ambience {

// fadeMetres 0: the game's own ambience everywhere.
void install(float fadeMetres);
// The game's main thread, every frame the VR camera runs and when first
// person ends.
void setFirstPerson(bool on);
// outdoors.py, a few times a second in first person: how much of the outdoors
// reaches you inside through doors and arches, 0..1 (negative: not known, the
// wall distance alone), and how much comes through plain walls, 0..1.
void setOpenings(float through, float wallLeak);

}  // namespace s4vr::ambience
