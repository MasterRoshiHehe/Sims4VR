#pragma once
#include <string>

// The game's own text (its STBL string tables), so the DLL can draw game
// popups with their real words.
//
// A background thread reads every Strings_ENG_US.package of the game and its
// packs (delta builds win), then the English string tables of every package
// in the Mods folder (a mod wins over the game on the same key). Python sends
// a LocalizedString as JSON and resolve() fills in its tokens: Sim names,
// he/she, numbers, money, object names, nested strings.
namespace s4vr::strings {

// Starts loading the strings (once; any thread).
void start();
// The strings are loaded.
bool ready();
// A LocalizedString as JSON to plain UTF-8 text:
//   {"hash": 12345, "tokens": [{"type": "SIM", "first_name": "Bella", "is_female": true}, ...]}
// or {"raw": "text"} for plain text as is. Markup is stripped (<br> and </li>
// become new lines, <li> a bullet). "" when the hash isn't known or the
// strings aren't loaded yet.
std::string resolve(const std::string& json);

}  // namespace s4vr::strings
