#pragma once

// A virtual Xbox controller for the game's built-in XInput support. Hooks
// XInputGetState so the game sees pad 0 connected, with sticks and buttons
// taken from the VR controllers. A real pad on slot 0 still works; the
// virtual sticks override it only while they are deflected.
namespace s4vr::gamepad {

struct State {
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;  // -1..1, up positive
    float leftTrigger = 0, rightTrigger = 0;            // 0..1
    unsigned short buttons = 0;                         // XINPUT_GAMEPAD_* bits
};

bool install();
bool installed();
// Any thread.
void set(const State& state);
// Pad reads by the game since start (to tell whether it polls at all).
unsigned long long reads();

}  // namespace s4vr::gamepad
