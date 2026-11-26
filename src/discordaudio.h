#pragma once

// Discord's sound in the game (thread "Show Discord stream on in-game TV", user 2026-10-28 19:23Z: a friend's
// stream played in 3D in the game, muted when Discord isn't on a screen). Discord plays through Windows like any
// program: its audio sessions (every process whose program is discord*.exe, on every playing device) get a mute
// and a left and a right volume. Same idea as Sims Forever's sound (simsforever_sound.cpp): the Present thread only
// sets a target, a thread of its own talks to Windows and glides there. Not true 3D (no head-related filtering):
// distance and left/right placement from where the screen is around your head.
namespace s4vr::discordaudio {

// Python: bit 0 = mute Discord while it isn't on a screen you can see (its voice chat too: same program),
// bit 1 = place its sound at the screen. 0 = leave Discord's sound alone (it is given back at once).
void setMode(int mode);
// Present thread, every frame: Discord's screen is shown (false: not shown or not in first person) and, if so,
// the volume for each ear (0..1).
void setEars(bool shown, float left, float right);
// Process exit: Discord's sound is given back.
void shutdown();

}  // namespace s4vr::discordaudio
