#pragma once
#include <cstdint>

#include "phone3d.h"

// First person: point-to-teleport movement ([controls] move_mode = 1, user 2026-10-28). Instead of the left
// stick walking you, a stick pushed forward (a [teleport] option) shows an arc or a laser from that hand; where it
// meets the floor a pair of footprints marks the spot (gone when the spot is closed). Letting go moves the player
// there (player::teleportTo; the Sim's body follows like it does for any step), after an optional blink.
//
// What may be reached is decided twice: scriptmod walk.py checks the straight line from your feet to the spot
// against the navmesh (walls, closed-off areas, other levels, water), and gives the floor height there; the DLL then
// checks the spot against the collision scene (furniture). Nothing is routed, no interaction runs.
namespace s4vr::teleport {

struct Options {
    int style = 0;            // 0 arc, 1 straight laser
    float maxDistance = 8;    // metres from you
    bool blink = true;        // the eyes shut for a moment around the jump
    float blinkSeconds = 0.25f;
    bool marker = true;
};
void configure(const Options& options);

// Game thread, every frame in first person while teleport movement is on: held says the button is down; origin and
// dir are the pointing hand's place and direction in the world (units), scale the game units per metre, feet the
// player's feet (x, y, z). Finishes a jump in progress as well, so call it every frame (held false when idle).
void update(bool held, const float origin[3], const float dir[3], float scale, const float feet[3]);
// Teleport movement is off or first person ended: drop the pointer and any jump in progress.
void stop();
// Any thread: the pointer shows.
bool active();

// Python (walk.py, ten times a second): the spot waiting to be checked. True with seq and x, z when there is a new
// one; answer it with answer(seq, ok, y) (y: the floor height there).
bool query(uint32_t& seq, float& x, float& z);
void answer(uint32_t seq, bool ok, float y);

constexpr int kMaxPanels = 32;
// Render thread (phone3d::draw): this frame's arc dots and marker as clear panels; the camera at cam.
int build(phone3d::Panel* out, int max, const float cam[3], float scale);

}  // namespace s4vr::teleport
