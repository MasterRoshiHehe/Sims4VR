#pragma once
#include <cstdint>

// God mode: the hands (no arms) of the Sim you have selected, drawn at your controllers.
//
// The game skins Sims on the graphics card: each mesh's draw (game+0x121dcf0) builds a bone palette
// (game+0x121d850) and draws. For the selected Sim, in the passes from your eyes only, facehide.cpp
// draws every mesh that follows a hand joint one more time per hand with a palette made here: the
// bones of that hand and its fingers are moved from where the Sim's animation has them to where the
// controller is, every other bone is folded into the wrist (no area, nothing drawn). The Sim stays
// where it is in the world. The mesh still follows the Sim's own finger pose, so the hand shows the
// skin tone, nails, rings and gloves it has. Not used in first person, where your own arms follow you.
namespace s4vr::godhands {

// [body] god_hands, the B menu, live.
void setEnabled(bool on);

// Game thread, every camera copy: the Sim that is selected (its client Actor) in god mode, or null
// (first person, other modes).
void setActor(void* actor);

// Render threads (facehide hooks).
// The selected Sim's scene model, found through its scene-object component (null: not found). Drawn by
// facehide.cpp itself, so its hands show even when the game leaves the Sim out (another floor).
const uint8_t* model();
// The detail level that last gave hands (-1: not known) and a count of hand draws made, for facehide's try-out.
int lodHint();
void setLodHint(int lod);
// Whether this scene model (a SceneModel the game draws) is the selected Sim's.
bool isSelected(const uint8_t* sceneModel);
// Which hands (bit 0 left, bit 1 right) this mesh draw (the mesh and its draw's pose pointer) needs an
// extra draw for: the mesh follows that hand and the controller is tracked. 0: none.
int sidesFor(const uint8_t* mesh, const void* pose);
// After the game built `out`, the bone palette of `mesh` for `pose`, in an extra draw for `side`:
// moves that hand's bones to its controller and folds the rest into the wrist.
void palette(int side, const uint8_t* mesh, uint8_t* out, const uint8_t* pose);

}  // namespace s4vr::godhands
