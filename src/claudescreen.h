#pragma once
#include <d3d11.h>

// "Open Claude" on an in-game computer (user 2026-10-14 16:35Z: write bug
// reports and ideas without taking the headset off). The script mod
// (claude.py) says when your Sim opened Claude on a computer and where its
// screen is; the Claude desktop app's window is brought to the front (so your
// real mouse and keyboard type into it), sized to the screen's shape, and its
// picture (with the mouse cursor) is drawn on the computer's screen the same
// way as Sims Forever's (vr.cpp, phone3d). The laser or a finger on the
// screen moves and clicks the mouse there. The Sims 4 isn't told it lost the
// focus meanwhile (it keeps running); the controllers stay The Sims 4's.
// When it goes off, the window gets its old place and size back and The Sims
// 4 the front. The app is never closed.
namespace s4vr::claudescreen {

// Python: on, the screen (world units: centre, outward normal, up, width,
// height) and the app to start when it isn't running (empty: the usual place).
void set(int on, const float v[11], const char* exe);
// Any thread: on show.
bool on();
// The screen, as simsforever::screen.
bool screen(float v[11]);
bool frameSize(int& w, int& h);
// The UI render thread: the latest picture as a shader view (not owned).
ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context);
// Present thread: the laser or a finger at u v (0..1 across the picture); buttons bit 0 left, bit 1 right.
void mouse(float u, float v, int buttons);
// The game window's procedure: true while a loss of focus is kept from the game.
bool keepsGameFocused();
// Present thread: the game's window (to give the front back to).
void setGameWindow(HWND game);

}  // namespace s4vr::claudescreen
