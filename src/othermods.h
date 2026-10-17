#pragma once

// The separate mods the debug menu (B) has pages for and that sims4vr reads
// settings from while they are loaded: Integrated Body Physics (was Claude
// Jiggle) and Extended Sound Control (was Claude Sound), both DLLs, and the
// Claude Performance script mod. Each is optional: none of their pages or
// rows show while it isn't loaded (user 2026-10-17 16:09Z).
namespace s4vr::othermods {

enum Mod { kBodyPhysics, kSoundControl, kPerformance, kModCount };

using GetFn = int (*)(const char* key, float* out);
using SetFn = int (*)(const char* key, float value);

// A DLL mod's settings calls (<prefix>_get / _set); false while it isn't loaded.
bool api(Mod mod, GetFn& get, SetFn& set);
bool loaded(Mod mod);
// A DLL mod's menu export (<prefix>_menu: UTF-16 rows, its docs/MENU-EXPORT.md); nullptr without one.
const wchar_t* menu(Mod mod);

// The script mods the script mod found loaded (bit 1 << kPerformance). Any thread.
void setScriptMods(int bits);

}  // namespace s4vr::othermods
