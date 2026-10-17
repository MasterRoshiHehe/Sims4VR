#pragma once
#include <cstdint>

#include "camera.h"
#include "config.h"

// Speech and thought bubbles and the skill / purchase bar over Sims as flat
// cards in the 3D world instead of on the flat UI layer (DX11 game only).
//
// The game's ObjectBoundElements widget binds each of these Flash pieces to a
// Sim's bone (service BindDisplayObject, rva 0xe14f70). Every frame its
// updater (0xe16110) projects each anchor with the game camera and moves the
// piece there on the UI. After it, this module moves each lifted piece into a
// slot along the top of the UI at a fixed scale (SetDisplayInfo), turns off the
// game's own wall test for it, and publishes the slot with the piece's world
// anchor. uihook cuts the slots out of the flat UI after it is drawn, and
// phone3d draws them as cards at the anchor, depth-tested, so walls hide them.
// Your own Sim's (first person) are held in front of your head instead. The
// hide switches make balloons invisible in the game itself. RE notes:
// re/bubbles/NOTES.md, re/liveui/components/ObjectBoundElements.md.
namespace s4vr::bubbles {

// Hooks the game. Call once at start; false in the DX9 game or another game version.
bool install();

// Any thread: whether the cards are wanted (the setting, and the UI on its own layer).
void setWanted(bool wanted);
bool wanted();
// Any thread, once a frame: the [bubbles] settings (sizes, positions, hiding).
void setOptions(const Config& config);

// One card: where it is on the UI texture and in the world.
struct Card {
    int rect[4];       // UI texture pixels: left, top, right, bottom (exclusive)
    float centre[3];   // world units
    float x[3], y[3];  // its right and up axes (unit length, the camera's)
    float width, height;  // world units
};
constexpr int kMaxCards = 24;

// The UI's render thread, right after the UI was drawn: this frame's cards for
// a UI texture of this size, seen by `view` (the camera the frame was drawn
// with). Returns how many it wrote.
int frameCards(const camera::View& view, int textureWidth, int textureHeight, Card* out, int max);

}  // namespace s4vr::bubbles
