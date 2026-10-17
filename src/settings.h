#pragma once
#include <cstdint>
#include <string>

#include "config.h"

// The VR settings menu (B): the sims4vr.ini values you tune while playing, on
// a panel in front of you. Right stick up / down picks a row, left / right
// changes it, A switches it; the laser works too (pull the trigger on a
// value's arrows). Most apply at once. Everything you changed is saved to
// sims4vr_user.ini when the menu closes; it wins over sims4vr.ini, so a new
// release keeps your choices.
namespace s4vr::settings {

// vr.cpp's live config: most rows change it in place.
void init(const std::wstring& moduleDir, Config* live);

// The menu's current value for a key it has a row for (what the script mod
// reads, so a change counts before the menu closes and saves). Any thread.
bool liveValue(const char* section, const char* key, std::string& out);

// The game's testing cheats as the script mod sees them (on: every pie menu is
// the Shift+click cheat menu). Shown on the menu's switch. Any thread.
void setTestingCheats(int state);  // 1: testing cheats on, 2: cheat pie menus on (v0.13.127)

// B / Y: up a page. False on the first page (B / Y close the menu there).
bool back();

bool isOpen();
void open();
// Closes and saves. True if render_scale changed (vr.cpp applies it then).
bool close();

// Render thread, while open: a stick (x changes the row, y picks one) and A.
// True when the row or a value changed (for a haptic tick).
bool update(float stickX, float stickY, bool press);

// The laser on the panel at u, v (0..1 from its top left) picks the row under
// it; a click acts on it.
enum PointResult { kNothing = 0, kChanged = 1, kCloseRequested = 2 };
int point(float u, float v, bool click);

// Moving the panel by its title bar (vr.cpp, v0.13.117): u, v on the title bar
// (not its close button), and where the panel opens once it was moved: metres
// right, up and forward of your head facing level, and degrees it is turned
// from facing you. Saved in sims4vr_user.ini, [ui] settings_place.
bool onTitleBar(float u, float v);
bool placement(float place[4]);
void setPlacement(const float place[4]);

// The picture: premultiplied 0xAARRGGBB pixels, top-down. version changes
// whenever the picture does.
const uint32_t* image(int& width, int& height, uint64_t& version);

}  // namespace s4vr::settings
