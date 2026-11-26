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
//
// Two windows can be on two screens at once (thread "Show Discord stream on
// in-game TV", user 2026-10-28): slot 0 is Claude (a computer or a TV: the
// window comes to the front and is the real mouse and keyboard's), slot 1 is
// Discord (a TV: a viewer; the window stays where it is, behind the game, and
// only the laser clicks into it when `interactive`). Each slot has its own
// capture thread; a slot nobody looked at for a second stops capturing, and a
// picture wider than `maxWidth` is scaled down on that thread, so a stream
// costs the game nothing but one texture upload per new picture.
namespace s4vr::claudescreen {

constexpr int kSlots = 2;
constexpr int kClaude = 0, kDiscord = 1;
// Not a window: the Linear board (linearboard.h), drawn by us and shown on a screen the way Discord's window is.
// Every function below forwards slot kLinear there; set() takes exe = the team's key, title = options,
// fps = seconds between refreshes, maxWidth = the picture's width.
constexpr int kLinear = 2;

// Python: on, the screen (world units: centre, outward normal, up, width,
// height) and the app to start when it isn't running (empty: the usual place).
// Slot 1 also takes the window title it must contain (empty: any), the
// pictures a second, the widest picture and whether the laser clicks into it;
// calling it again with on = 1 only changes these.
void set(int on, const float v[11], const char* exe, int slot = 0, const char* title = "", int fps = 20,
         int maxWidth = 2560, int interactive = 1);
// Any thread: on show.
bool on(int slot = 0);
// The laser clicks into this slot's window.
bool interactive(int slot = 0);
// The screen, as simsforever::screen.
bool screen(float v[11], int slot = 0);
bool frameSize(int& w, int& h, int slot = 0);
// The UI render thread: the latest picture as a shader view (not owned).
ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context, int slot = 0);
// Present thread: the laser or a finger at u v (0..1 across the picture); buttons bit 0 left, bit 1 right.
void mouse(float u, float v, int buttons, int slot = 0);
// The game window's procedure: true while a loss of focus is kept from the game.
bool keepsGameFocused();
// Present thread: the game's window (to give the front back to).
void setGameWindow(HWND game);

}  // namespace s4vr::claudescreen
