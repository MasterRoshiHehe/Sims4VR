// Rewrites a game foliage vertex shader (DXBC) so leaves and grass can be pushed away from a few spheres:
// see foliagepatch.cpp. Pure bytes in, bytes out (no Windows or D3D calls), so tools/test_foliagepatch.cpp checks it
// offline against shaders unpacked from Shaders_DX11.precomp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace s4vr::foliagepatch {

// The patched shader reads this constant buffer slot (declared as cb6[kRows]); the game's wind shaders
// use none of slots 3 and 6 (re/foliage/FOLIAGE_VS_REPORT.md).
constexpr unsigned kSlot = 6;
constexpr unsigned kSpheres = 8;
constexpr unsigned kRows = kSpheres + 1;
// Rows 0..7 : xyz = MINUS the sphere centre (world, metres), w = MINUS 1 / radius. A sphere that is not there:
//             centre far away (xyz = +1e6), w = -1.
// Row 8     : x = push (metres at a sphere's centre), y = lift (metres, every leaf: the test),
//             z = non-zero while anything is to be pushed (otherwise the patch costs one branch), w unused.
// All zero rows leave the shader drawing as before.

enum Family { kNone = 0, kStatic = 1, kInstanced = 2, kStreamed = 3, kWorld = 4, kCamRel = 5 };

struct Info {
    Family family = kNone;
    bool wind = false;  // a wind shader (one sincos, frame time read), whether or not the patch took
    std::string why;  // when family is kNone: what did not match
};

// True when `code` is a wind vertex shader the patch understands: `out` is the patched shader (checksum fixed).
// Otherwise `out` is untouched and info->why says why.
bool patch(const uint8_t* code, size_t size, std::vector<uint8_t>& out, Info* info);

// The container's stored checksum recomputed: true when it matches (used by the offline test).
bool checksumMatches(const uint8_t* code, size_t size);

}  // namespace s4vr::foliagepatch
