#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <string>

#include "config.h"

namespace s4vr::vr {

// Loads openxr_loader.dll from `moduleDir`. The session itself is created
// lazily on the first Present, because it must use the game's D3D11 device.
bool init(const std::wstring& moduleDir, const Config& config);

// Render thread, once per game frame (DX11: the game's swapchain).
void onPresent(IDXGISwapChain* swapchain);

// Render thread, once per game frame: the finished frame as a D3D11 texture
// (the game's backbuffer, or in DX9 a copy on the mod's own D3D11 device) and
// the game window (null once known). ownMirror: the caller keeps the monitor
// steady itself (shows the left eye on right-eye frames); otherwise this does
// it in `backbuffer`. Returns the eye whose image this frame was (0 left,
// 1 right), or -1 outside stereo.
int onFrame(ID3D11Texture2D* backbuffer, HWND window, bool ownMirror);

// Safe to call from any thread; applied on the next frame.
void requestRecenter();
void setMode(Mode mode);
Mode mode();

// Ends the session and instance. Call before the process exits.
void shutdown();

// One-line human-readable state for the vr.status command.
std::string status();

// First-person walking: the left stick as a world direction (x, z) and
// strength 0..1, while body mode is on. False when there is none.
bool walkInput(float& x, float& z, float& strength);

// Any thread: the game built a pie menu for a click into the world (items > 0)
// or you picked from it. In first person the menu then shows at your hand.
void pieOpened(int items);
void pieClosed();
// Any thread (remote.py): the hand of the last click into the world in first
// person (0 left, 1 right; laser or fingertip), -1 if none in the last 5 s; and
// whether the pie menu shows at your hand now.
int clickHand();
bool pieShown();
// Any thread (Python, phone.py): whether a game menu or dialog (a UI modal) is open.
void setGameMenu(bool open);
// The game shows a popup (a modal dialog; phone.py): the phone buzzes, and shows it when taken out.
void setPopup(bool open);
// Python (phone.py): the game clock is paused (the pause symbol in first person).
void setPaused(bool paused);
// Python (controls.py): the game clock for the HUD (hud.h).
void setClock(int hour, int minute, int day, int speed);
// Any thread (buildbuy.py): the lot is in build/buy mode. In first person the
// phone gesture then brings out the tablet: the whole build/buy UI on its screen.
void setBuildBuy(bool on);
// Any thread (objmenu.py): the open picker shows on an object's front face in
// first person. kind 1 = a door or front (a fridge: the upper door), 2 = a
// screen (a TV: fitted inside it), 0 = off. v: centre xyz, outward normal xyz,
// up xyz, width, height of the face (world units).
void setObjectMenu(int kind, const float v[11]);
// Any thread (phone.py): where our phone sits on the Sim's right hand joint,
// in that joint's frame (world units): position xyz and rotation xyzw of the
// phone's frame (x right, y up, z out of the screen). False until the rig is read.
bool phoneMount(float out[7]);
// Any thread (phone.py): the game's phone model's size (width, height,
// thickness, world units), so our phone is drawn around it.
void setPhoneProp(float width, float height, float depth);

}  // namespace s4vr::vr
