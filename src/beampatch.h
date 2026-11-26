// The phone flashlight's beam inside the game's object pixel shaders: see beampatch.cpp. Pure bytes in, bytes out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace s4vr::beampatch {

// The cbuffer slot our 4 vec4 are declared on in a patched shader (D3D11 has 14: 0..13; the game's shaders use <= 7).
//   c0: xyz = light position relative to the camera (world axes, world units), w = -1 / range
//   c1: xyz = -(beam direction, unit), w = unused
//   c2: x = 1 / (cosInner - cosOuter), y = -cosOuter * x, z = 1 / r0^2, w = on (> 0)
//   c3: rgb = beam colour * gain
constexpr unsigned kSlot = 13;

struct Info {
    bool cube = false;      // the shader reads a cube map
    bool patched = false;
    bool inBranch = false;  // the patched spot sits inside an if/loop
    std::string why;        // when not patched
};

bool patch(const uint8_t* code, size_t size, std::vector<uint8_t>& out, Info* info);
bool checksumMatches(const uint8_t* code, size_t size);

}  // namespace s4vr::beampatch
