#pragma once

#include <cstdint>

// Pictures from the game's own thumbnail system (EA::Sims4::Thumbnails), asked
// for the way the game's UI asks for them: its image loader
// (EA::Sims4::UI::ImageLoader) takes "img://thumbs/<path>" URLs such as
// "img://thumbs/sims/f_<sim id>_m_0" (a Sim's face) or
// "img://thumbs/objects/<object id>_m" (an object's picture), built by the UI's
// ImageUtils.as. The game renders or reads the picture and calls back; its
// pixels become the HUD picture `id` (hud::setPicture), drawn like any icon.
// (User 2026-10-17 13:34Z: no more cuts of the game's UI; draw what it draws.)
namespace s4vr::thumbs {

// Any thread (Python's): the picture "img://thumbs/<path>" as HUD picture
// `id`. Asked once per id; a failed one is asked again after a while. False:
// not asked (game build not recognised, or a bad path).
bool request(uint64_t id, const char* path);

// The game's main thread (the camera hook), where the game's UI asks too:
// sends the requests waiting to the game's thumbnail system.
void pump();

}  // namespace s4vr::thumbs
