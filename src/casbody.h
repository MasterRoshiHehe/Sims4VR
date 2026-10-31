#pragma once
#include <cstdint>

// Create a Sim: your own first-person body, a double of the Sim you are editing.
//
// CAS draws one Sim (the one you edit, standing ~1.6 m in front of you). In the passes from your eyes, facehide.cpp
// draws that Sim's scene model a second time, and every bone palette of that second draw is made here: the whole
// skeleton is moved under your headset and turned to where you face, its head and face folded away (as the face is in
// live first person), and both arms reach for your controllers (two-bone IK, hands turned like the controllers). It is
// the same model the game rebuilds on every edit, so clothes, body shape, skin and hair edits show on both at once.
// The torso and legs stand still in one calm pose of the CAS Sim's ([cas] body_still), or follow its idle. Only your eyes
// see it: no shadow, no thumbnails.
namespace s4vr::casbody {

// [cas] body, live.
void setEnabled(bool on);
bool enabled();

// Render threads (facehide.cpp hooks), only while CAS is on (camera::casActive).
// Whether this scene model, drawn from your eyes, is the CAS Sim (its world place at the CAS spot, a Sim skeleton
// with arms and a head). Remembers it for model().
bool isCasSim(const uint8_t* sceneModel);
// The CAS Sim's scene model seen in the last half second, or null.
const uint8_t* model();
// The detail level the CAS Sim was last drawn at (-1: not known).
int lodHint();
void setLodHint(int lod);
// Before the second draw: works out where every bone of the body goes for this pass. False: no body this pass
// (nothing to draw it from, CAS off, the switch off).
bool prepare(const uint8_t* sceneModel);
// In the second draw: whether this mesh is drawn (not one-bone pieces drawn rigidly at the CAS Sim, not eyes/teeth/
// lashes), and its palette (the game built `out` for `pose`; this moves each bone).
bool drawMesh(const uint8_t* mesh);
void palette(const uint8_t* mesh, uint8_t* out, const uint8_t* pose);
// Game thread (camera.cpp, CAS eyes): with [cas] body_eyes on and the body drawn in the last second, the world height
// your eyes go to: the body's own eye height (its head joint plus a little), eased, so you look out of its head as in
// live first person. False: use [cas] eye_height.
bool eyeHeight(float& worldY);
void setEyesAtBody(bool on);
// [cas] body_still: the body stands in one calm pose picked in its first 3 s, not the CAS Sim's idle. Live.
void setStill(bool on);
// The second draw reached no mesh for a while (the game queued them for later): leave the view's batch sink out
// for it (facehide clears view+0x58 around the call).
bool skipBatch();
// After each second draw: how many of its meshes went through drawMesh.
void drawn(uint32_t meshes, bool injected);

}  // namespace s4vr::casbody
