#pragma once
#include <cstdint>
#include <string>
#include <vector>

// An object's visible shape for collision (collide.py), from its model's
// triangles rather than its footprint or bounding box: each separate piece of
// the model (a table leg, the top, a parasol's pole, its canopy) is sliced
// into thin layers, and each layer is the outline (convex) of the piece
// there. Layers that look alike are joined, so a pole is one tall prism.
namespace s4vr::meshshape {

// res: a model (MODL resource, unpacked). out gets the prisms in the
// model's own space, one after another: corner count, y0, y1, then x z of
// each corner (anticlockwise seen from above). note says what was found
// (for the log). False: no usable mesh.
bool fromModel(const std::vector<uint8_t>& res, std::vector<float>& out, std::string& note);

}  // namespace s4vr::meshshape
