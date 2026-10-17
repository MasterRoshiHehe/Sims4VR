#pragma once
#include <string>

// Finds where the game keeps its camera, using the camera position and target
// that the game reports to Python (camera._camera_position / _target_position).
// Scans every camera-related C++ object (found by RTTI class name) and the
// objects they point to, and logs each field that holds those values.
namespace s4vr::camfind {

// Logs all hits to sims4vr.log and returns a short summary.
std::string run(const float position[3], const float target[3]);

}  // namespace s4vr::camfind
