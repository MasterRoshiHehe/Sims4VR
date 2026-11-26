#pragma once
// Doors and windows you can knock on (user 2026-11-01): knock.py sends the doors and windows
// around you as upright boxes (world metres); armik.cpp objectTrack asks whether your fist is in
// one and plays the knock there (sounds\knock\door, sounds\knock\window).
namespace s4vr::knock {

enum Type { kDoor = 0, kWindow = 1 };

// Little-endian floats: count, then per target 12: centre x, z; along unit x, z; half width along; half depth
// across; y bottom, y top; type (kDoor, kWindow); the object id in three floats (low 24 bits, middle 24, top 16). Any thread.
void setTargets(const float* data, int count);
// Any thread: is (x, y, z) within `pad` metres of a target? Then its type and the unit normal (x, z) on the
// side the point is.
bool inside(float x, float y, float z, float pad, int& type, float normal[2], unsigned long long& id);
// A knock that played (armik.cpp): which target, which hand, how fast (m/s). Any thread.
void pushEvent(unsigned long long id, int type, int hand, float into);
// The oldest knock not yet taken (knock.py polls it).
bool takeEvent(unsigned long long& id, int& type, int& hand, float& into);

}  // namespace s4vr::knock
