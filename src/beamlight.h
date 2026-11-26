#pragma once
// The phone flashlight's beam as a real light in the game's own shaders (design A, user 2026-11-26): the game's
// object pixel shaders are patched when they are created (beampatch.cpp), and the phone's beam goes to them in a
// cbuffer of ours (slot beampatch::kSlot), bound whenever a patched shader is set. DX11 only.
namespace s4vr::beamlight {

// Early (before the game makes its shaders), once. false: left alone ([phone] flash_in_shader = 0, or no hook).
bool install(bool enable);

// Any thread: the beam for the next frames, from phone3d. World space, the camera's world units.
// pos: where it starts; dir: unit vector it points along; range: world units; the cone's full angle in radians;
// r0: the distance (world units) where the near fall-off is half; rgb: colour * gain (added to the light sum).
struct Beam {
    bool on = false;
    float pos[3]{}, dir[3]{};
    float range = 10, cone = 0.9f, r0 = 1;
    float rgb[3]{1, 0.95f, 0.85f};
};
void set(const Beam& beam);

// Shaders patched so far, and whether the beam can work in the shaders (installed and at least one patched).
int patchedCount();
bool active();

}  // namespace s4vr::beamlight
