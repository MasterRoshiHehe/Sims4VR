#pragma once
#include <windows.h>

// Turns VR controller input into the mouse and keyboard input the game
// already understands: the laser pointer moves the mouse, triggers click and
// the sticks hold the game's camera keys.
namespace s4vr::pointer {

struct Controls {
    // Where the laser meets the game window, 0..1 from the top left.
    bool hasTarget = false;
    float u = 0, v = 0;
    bool leftButton = false;   // held state, not edges
    bool warp = false;         // move the mouse to the target now, even as a button goes down
    bool rightButton = false;
    bool escape = false;
    bool buildMode = false;  // F2, the game's Build Mode key (the HUD's build button)
    // -1..1. Pan: WASD. Rotate: middle-mouse drag. Zoom: scroll wheel.
    float panX = 0, panY = 0, rotate = 0, zoom = 0;
    float turnSpeed = 900;  // mouse pixels per second at full deflection
    // You are holding or using the controllers (a finger on a sensor, a
    // button, a stick): the game window is kept in front meanwhile.
    bool inUse = false;
};

// Render thread, once per frame. Input only goes to the game while its window
// is in front; everything held is released when it is not, and the window is
// brought back to the front while the controllers are in use.
void update(HWND gameWindow, const Controls& controls);
// The game window (or its root) has the focus.
bool inFront(HWND gameWindow);
// Raise the game's view to the top floor, so every floor shows (any thread).
void showAllFloors();
// Page Down this many times (back to the floor god view showed before).
void floorsDown(int count);
// Let go of every key and button we hold.
void releaseAll();

}  // namespace s4vr::pointer
