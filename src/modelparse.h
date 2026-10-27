// Copied from re/client/tools/cpp/parse.h (Claude Bustin Out's model reader); keep the two alike.
#pragma once
// The game's own models and pictures, read from package resources (no Windows calls here, so
// tools/test_parse.cpp can check it against real packages on any machine).
#include <cstdint>
#include <string>
#include <vector>

namespace cbo::parse {

constexpr uint32_t kModel = 0x01661233;    // MODL: the model, its detail levels (LODs)
constexpr uint32_t kModelLod = 0x01D10F34;  // MLOD: one detail level (group = its LOD id)
constexpr uint32_t kImage = 0x00B2D882;     // DDS / DST picture

// A resource as stored -> its bytes (0 none, 0x5A42 zlib, 0xFFFF RefPack). False: broken.
bool unpack(uint16_t compression, const uint8_t* in, size_t stored, uint32_t size, std::vector<uint8_t>& out);

struct Key {
    uint32_t type = 0, group = 0;
    uint64_t instance = 0;
};

// MODL: its box and detail levels. An LOD lives in a chunk of this resource (chunk >= 0) or in
// an MLOD resource of its own (external).
struct Lod {
    uint32_t id = 0;  // 0 the most detailed, 1, 2 ...; 0x10000+ shadows
    float minZ = 0, maxZ = 0;
    int chunk = -1;
    Key external;
};
struct ModelInfo {
    float lo[3] = {}, hi[3] = {};
    std::vector<Lod> lods;
};
bool readModel(const std::vector<uint8_t>& res, ModelInfo& info, std::string& why);

struct Vertex {
    float pos[3], nrm[3], uv[2];
};
struct Param {
    uint32_t hash, type, count;
    float first;  // a number's (first) value
};
struct Mesh {
    uint32_t firstIndex = 0, indexCount = 0;
    bool hasDiffuse = false;
    Key diffuse;
    float alphaClip = 0;  // > 0: pixels with less alpha are left out (leaves)
    uint32_t shader = 0;
    std::vector<Param> params;  // its material's parameters (for the log and tests)
};
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Mesh> meshes;
    std::string skipped;  // meshes left out and why (for the log)
};
// The meshes of one LOD: the MLOD chunk `chunk` of res (-1: its first MLOD). variant: the
// object's material variant (MTST state); 0 = the default.
bool readGeometry(const std::vector<uint8_t>& res, int chunk, uint32_t variant, Geometry& out, std::string& why);

// A picture ready for the graphics card: BC1/2/3 blocks or RGBA8, every mip level back to back,
// the top level no larger than maxSize.
struct Texture {
    int format = 0;  // 1 BC1, 2 BC2, 3 BC3, 4 RGBA8
    uint32_t w = 0, h = 0, mips = 0;
    std::vector<uint8_t> data;
};
bool readTexture(std::vector<uint8_t>& dds, uint32_t maxSize, Texture& out, std::string& why);
uint32_t levelSize(int format, uint32_t w, uint32_t h);

}  // namespace cbo::parse
