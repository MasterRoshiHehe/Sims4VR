#pragma once
#include <d3d11.h>

#include <vector>

// Sims Forever (first person, DX11): the user's real Sims 1 on an in-game
// computer's screen. The script mod (simsforever.py) says when "Play Sims
// Forever" runs on a computer and where that computer's screen is; Sims 1 is
// started (sf_launch.exe loads sf_hook.dll into it, src/simsforever/), its
// frames come through shared memory, are drawn on the screen (phone3d) and the
// laser or a finger on the screen is its mouse (vr.cpp).
namespace s4vr::simsforever {

// Python: the screen (world units: centre, outward normal, up, width, height),
// on = 0 takes it off (Sims 1 is closed outright 2 s later, no save question).
// exe / args: Sims 1, started on the first `on`.
void setScreen(int on, const float v[11], const char* exe, const char* args);
// The screen request: false if off. v as above.
bool screen(float v[11]);
// The latest frame's size (0 before the first frame).
bool frameSize(int& w, int& h);
// The UI render thread (phone3d::draw): the frame as a shader view, brought up to date.
// Not owned; valid until the next call.
ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context);
// Present thread: the mouse on the screen, u v 0..1 across the frame; buttons bit 0 left, bit 1 right.
void mouse(float u, float v, int buttons);
// Present thread: Sims 1 is on show in front of you; your real mouse and
// keyboard go to it meanwhile (the controllers stay The Sims 4's).
void setLocked(bool locked);
// Python (screenmesh.py): the screen's shape, an nx x ny grid of depths behind
// its front (world units, rows from the bottom left); nx or ny < 2 = flat.
void setSurface(int nx, int ny, const float* depths, int count);
// Any thread: the shape (empty = flat) and a number that changes with it.
// depth(): behind the front at fractions fx, fy (0..1 across the whole screen), bilinear.
int surface(int& nx, int& ny, std::vector<float>& depths);
float depth(float fx, float fy);

// Present thread: Sims 1's sound volumes for your left and right ear (0..1,
// from where the computer is around your head; simsforever_sound.cpp).
void setEars(float left, float right);

// Present thread, every frame: logs the Sims 1 side's news; closes Sims 1 when
// the screen has been off for 2 s.
void poll();

}  // namespace s4vr::simsforever
