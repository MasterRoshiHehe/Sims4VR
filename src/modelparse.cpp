// Copied from re/client/tools/cpp/parse.cpp (Claude Bustin Out's model reader); keep the two alike.
// The game's model and picture resources (formats checked on real packages, see tools/test_parse.cpp):
// RCOL containers with MODL / MLOD / MTST / MATD / VRTF / VBUF / IBUF chunks, DST-shuffled DDS pictures,
// zlib and RefPack compression.
#include "modelparse.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace cbo::parse {
namespace {

// ---------------------------------------------------------------------------
// Inflate (RFC 1951), canonical Huffman decoded a bit at a time: small and plain, fast enough for a
// background thread.

struct Inflater {
    const uint8_t* in;
    size_t n, pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    bool bad = false;
    std::vector<uint8_t>& out;
    Inflater(const uint8_t* i, size_t c, std::vector<uint8_t>& o) : in(i), n(c), out(o) {}

    int bits(int need) {
        uint32_t v = bitBuf;
        while (bitCount < need) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            v |= static_cast<uint32_t>(in[pos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = v >> need;
        bitCount -= need;
        return static_cast<int>(v & ((1u << need) - 1));
    }

    struct Huffman {
        short count[16];
        short symbol[320];
    };
    // 0 complete, < 0 over-subscribed, > 0 incomplete (allowed for one-code distance sets)
    static int build(Huffman& h, const short* length, int n) {
        std::memset(h.count, 0, sizeof(h.count));
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s]) h.symbol[offs[length[s]]++] = static_cast<short>(s);
        return left;
    }
    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= bits(1);
            if (bad) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        bad = true;
        return -1;
    }

    bool stored() {
        bitBuf = 0;
        bitCount = 0;
        if (pos + 4 > n) return false;
        unsigned len = in[pos] | in[pos + 1] << 8;
        unsigned nlen = in[pos + 2] | in[pos + 3] << 8;
        pos += 4;
        if (len != (~nlen & 0xffff) || pos + len > n) return false;
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (bad || symbol < 0) return false;
            if (symbol < 256) {
                out.push_back(static_cast<uint8_t>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) return false;
                int len = lbase[symbol] + bits(lext[symbol]);
                int ds = decode(distcode);
                if (bad || ds < 0 || ds >= 30) return false;
                size_t dist = static_cast<size_t>(dbase[ds] + bits(dext[ds]));
                if (bad || dist > out.size()) return false;
                size_t from = out.size() - dist;
                for (int k = 0; k < len; ++k) out.push_back(out[from + k]);
            }
        }
    }

    bool fixed() {
        static Huffman lencode, distcode;
        static bool made = false;
        if (!made) {
            short lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            build(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            build(distcode, lengths, 30);
            made = true;
        }
        return codes(lencode, distcode);
    }

    bool dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        short lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(bits(3));
        Huffman lencode, distcode;
        if (build(lencode, lengths, 19) != 0) return false;
        int index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (bad || symbol < 0) return false;
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
            } else {
                short len = 0;
                int repeat;
                if (symbol == 16) {
                    if (index == 0) return false;
                    len = lengths[index - 1];
                    repeat = 3 + bits(2);
                } else if (symbol == 17) {
                    repeat = 3 + bits(3);
                } else {
                    repeat = 11 + bits(7);
                }
                if (index + repeat > nlen + ndist) return false;
                while (repeat--) lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) return false;
        int err = build(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
        err = build(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
        return codes(lencode, distcode);
    }

    bool run() {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            if (bad) return false;
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }
};

bool inflateZlib(const uint8_t* in, size_t n, std::vector<uint8_t>& out, uint32_t size) {
    if (n < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return false;
    out.clear();
    out.reserve(size);
    Inflater f(in + 2, n - 2, out);
    return f.run() && (size == 0 || out.size() == size);
}

// EA's RefPack (QFS).
bool refpack(const uint8_t* in, size_t n, std::vector<uint8_t>& out, uint32_t size) {
    if (n < 2) return false;
    const uint8_t flags = in[0];
    size_t at = 2 + ((flags & 0x80) ? 4 : 3);
    if (flags & 0x01) at += (flags & 0x80) ? 4 : 3;
    out.clear();
    out.reserve(size);
    while (at < n) {
        const uint8_t b0 = in[at];
        size_t plain, copy, offset;
        if (b0 < 0x80) {
            if (at + 2 > n) return false;
            const uint8_t b1 = in[at + 1];
            plain = b0 & 3;
            copy = ((b0 >> 2) & 7) + 3;
            offset = ((b0 & 0x60) << 3) + b1 + 1;
            at += 2;
        } else if (b0 < 0xC0) {
            if (at + 3 > n) return false;
            const uint8_t b1 = in[at + 1], b2 = in[at + 2];
            plain = b1 >> 6;
            copy = (b0 & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
            at += 3;
        } else if (b0 < 0xE0) {
            if (at + 4 > n) return false;
            const uint8_t b1 = in[at + 1], b2 = in[at + 2], b3 = in[at + 3];
            plain = b0 & 3;
            copy = ((b0 & 0x0C) << 6) + b3 + 5;
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
            at += 4;
        } else if (b0 < 0xFC) {
            plain = ((b0 & 0x1F) << 2) + 4;
            copy = 0;
            offset = 0;
            at += 1;
        } else {
            plain = b0 & 3;
            copy = 0;
            offset = 0;
            at += 1;
        }
        if (at + plain > n) return false;
        out.insert(out.end(), in + at, in + at + plain);
        at += plain;
        if (copy) {
            if (offset > out.size()) return false;
            size_t from = out.size() - offset;
            for (size_t k = 0; k < copy; ++k) out.push_back(out[from + k]);
        }
        if (b0 >= 0xFC) break;
    }
    return size == 0 || out.size() == size;
}

// ---------------------------------------------------------------------------
// RCOL: chunks (tag, position, size) and the external keys.

struct Chunk {
    uint32_t tag = 0;
    size_t pos = 0, size = 0;
};
struct Rcol {
    uint32_t publicCount = 0;
    std::vector<Chunk> chunks;
    std::vector<Key> external;
};

uint32_t tagOf(const char* t) { return static_cast<uint32_t>(t[0]) | t[1] << 8 | t[2] << 16 | static_cast<uint32_t>(t[3]) << 24; }

template <typename T>
bool get(const std::vector<uint8_t>& b, size_t at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return true;
}
template <typename T>
T val(const std::vector<uint8_t>& b, size_t at) {
    T v{};
    get(b, at, v);
    return v;
}

bool readRcol(const std::vector<uint8_t>& b, Rcol& r, std::string& why) {
    uint32_t version, pub, unused, external, internal;
    if (!get(b, 0, version) || !get(b, 4, pub) || !get(b, 8, unused) || !get(b, 12, external) || !get(b, 16, internal) ||
        internal == 0 || internal > 4096 || external > 4096) {
        why = "not an RCOL resource";
        return false;
    }
    r.publicCount = pub;
    size_t at = 20 + static_cast<size_t>(internal) * 16;
    for (uint32_t i = 0; i < external; ++i, at += 16) {
        Key k;
        if (!get(b, at, k.instance) || !get(b, at + 8, k.type) || !get(b, at + 12, k.group)) {
            why = "RCOL external keys cut off";
            return false;
        }
        r.external.push_back(k);
    }
    for (uint32_t i = 0; i < internal; ++i, at += 8) {
        uint32_t pos, size;
        if (!get(b, at, pos) || !get(b, at + 4, size) || static_cast<size_t>(pos) + size > b.size() || size < 4) {
            why = "RCOL chunk past the end";
            return false;
        }
        Chunk c;
        c.tag = val<uint32_t>(b, pos);
        c.pos = pos;
        c.size = size;
        r.chunks.push_back(c);
    }
    return true;
}

// A chunk reference (kind in the top 4 bits: 0 public, 1 private, 3 external) -> chunk index with one
// of the tags (or -1); external: -2 - its index in r.external.
int resolve(const Rcol& r, uint32_t ref, std::initializer_list<const char*> tags) {
    if (!ref) return -1;
    const uint32_t kind = ref >> 28, index = (ref & 0x0FFFFFFF) - 1;
    if (kind == 3) return index < r.external.size() ? -2 - static_cast<int>(index) : -1;
    if (kind > 1) return -1;
    auto fits = [&](size_t i) {
        if (i >= r.chunks.size()) return false;
        for (const char* t : tags)
            if (r.chunks[i].tag == tagOf(t)) return true;
        return false;
    };
    size_t first = kind == 1 ? index + r.publicCount : index, other = kind == 1 ? index : index + r.publicCount;
    if (fits(first)) return static_cast<int>(first);
    if (fits(other)) return static_cast<int>(other);
    return -1;
}

// ---------------------------------------------------------------------------
// Materials (MATD) and material sets (MTST).

constexpr uint32_t kDiffuseMap = 0x6CC0FD85;      // fnv32("diffusemap")
constexpr uint32_t kAlphaThreshold = 0xE77A2B60;  // fnv32("alphamaskthreshold")
constexpr uint32_t kUvScale = 0x159BA53E;         // fnv32("uvscale")
constexpr uint32_t kUvOffset = 0x57582869;        // fnv32("uvoffset")
constexpr uint32_t kDropShadow = 0xC09C7582;      // shader fnv32("dropshadow")
constexpr uint32_t kImpostor = 0x2F2F11B9;        // a tree's last detail level: one picture card (2 triangles)

struct Material {
    uint32_t shader = 0;
    bool hasDiffuse = false;
    Key diffuse;
    float alphaClip = 0;
    float uvScale[2] = {1, 1}, uvOffset[2] = {0, 0};
    bool hasUvScale = false;
    std::vector<Param> params;
};

bool readMaterial(const std::vector<uint8_t>& b, const Chunk& c, Material& m) {
    uint32_t version = val<uint32_t>(b, c.pos + 4);
    m.shader = val<uint32_t>(b, c.pos + 12);
    size_t block = c.pos + 20 + (version >= 0x103 ? 8 : 0);
    uint32_t count = val<uint32_t>(b, block + 12);
    if (block + 16 > c.pos + c.size || count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t p = block + 16 + i * 16;
        if (p + 16 > c.pos + c.size) return false;
        uint32_t hash = val<uint32_t>(b, p), type = val<uint32_t>(b, p + 4), n = val<uint32_t>(b, p + 8),
                 offset = val<uint32_t>(b, p + 12);
        size_t data = block + offset;
        if (data >= c.pos + c.size) continue;
        m.params.push_back({hash, type, n, type == 1 ? val<float>(b, data) : 0.0f});
        if (hash == kDiffuseMap && (type & 0xFFFF) == 4 && n == 4 && data + 16 <= c.pos + c.size) {
            m.diffuse.instance = val<uint64_t>(b, data);
            m.diffuse.type = val<uint32_t>(b, data + 8);
            m.diffuse.group = val<uint32_t>(b, data + 12);
            m.hasDiffuse = m.diffuse.instance != 0;
        } else if (hash == kAlphaThreshold && type == 1 && n >= 1) {
            m.alphaClip = val<float>(b, data);
        } else if (hash == kUvScale && type == 1 && n >= 2) {
            m.uvScale[0] = val<float>(b, data);
            m.uvScale[1] = val<float>(b, data + 4);
            m.hasUvScale = true;
        } else if (hash == kUvOffset && type == 1 && n >= 2) {
            m.uvOffset[0] = val<float>(b, data);
            m.uvOffset[1] = val<float>(b, data + 4);
        }
    }
    return true;
}

// A mesh's material: a MATD, or the MTST entry for this variant (else its default state, else its first).
bool materialFor(const std::vector<uint8_t>& b, const Rcol& r, uint32_t ref, uint32_t variant, Material& m) {
    int i = resolve(r, ref, {"MATD", "MTST"});
    if (i < 0) return false;
    if (r.chunks[i].tag == tagOf("MTST")) {
        const Chunk& c = r.chunks[i];
        uint32_t fallbackState = val<uint32_t>(b, c.pos + 12), count = val<uint32_t>(b, c.pos + 16);
        if (count == 0 || count > 64 || c.pos + 20 + count * 12 > c.pos + c.size) return false;
        uint32_t chosen = 0, byDefault = 0;
        bool haveVariant = false, haveDefault = false;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t mref = val<uint32_t>(b, c.pos + 20 + k * 12), state = val<uint32_t>(b, c.pos + 28 + k * 12);
            if (k == 0) chosen = byDefault = mref;
            if (variant && state == variant && !haveVariant) chosen = mref, haveVariant = true;
            if (state == fallbackState && !haveDefault) byDefault = mref, haveDefault = true;
        }
        i = resolve(r, haveVariant ? chosen : byDefault, {"MATD"});
        if (i < 0) return false;
    }
    return readMaterial(b, r.chunks[i], m);
}

// ---------------------------------------------------------------------------
// Vertices.

struct Element {
    int format = -1;
    uint32_t offset = 0;
};

bool decodePosition(const uint8_t* p, int format, float* out) {
    if (format == 2 || format == 3) {
        std::memcpy(out, p, 12);
        return true;
    }
    if (format == 7 || format == 10 || format == 12) {
        int16_t x, y, z;
        uint16_t w;
        std::memcpy(&x, p, 2);
        std::memcpy(&y, p + 2, 2);
        std::memcpy(&z, p + 4, 2);
        std::memcpy(&w, p + 6, 2);
        float s = w ? static_cast<float>(w) : 32767.0f;
        out[0] = x / s, out[1] = y / s, out[2] = z / s;
        return true;
    }
    return false;
}

void decodeNormal(const uint8_t* p, int format, float* out) {
    out[0] = 0, out[1] = 1, out[2] = 0;
    if (format == 2 || format == 3) {
        std::memcpy(out, p, 12);
    } else if (format == 8 || format == 4) {
        for (int k = 0; k < 3; ++k) out[k] = p[k] / 127.5f - 1;
    } else if (format == 5) {  // colour order BGRA
        out[0] = p[2] / 127.5f - 1, out[1] = p[1] / 127.5f - 1, out[2] = p[0] / 127.5f - 1;
    } else if (format == 13 || format == 14) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        for (int k = 0; k < 3; ++k) out[k] = ((v >> (10 * k)) & 1023) / 511.5f - 1;
    }
    float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len > 1e-4f)
        for (int k = 0; k < 3; ++k) out[k] /= len;
    else
        out[0] = 0, out[1] = 1, out[2] = 0;
}

float half(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24) : exp == 31 ? 65504.0f : std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

void decodeUv(const uint8_t* p, int format, float* out) {
    out[0] = out[1] = 0;
    if (format == 1 || format == 2 || format == 3) {
        std::memcpy(out, p, 8);
    } else if (format == 6 || format == 9 || format == 7 || format == 10) {
        int16_t u, v;
        std::memcpy(&u, p, 2);
        std::memcpy(&v, p + 2, 2);
        out[0] = u / 32767.0f, out[1] = v / 32767.0f;
    } else if (format == 11 || format == 12) {
        uint16_t u, v;
        std::memcpy(&u, p, 2);
        std::memcpy(&v, p + 2, 2);
        out[0] = u / 65535.0f, out[1] = v / 65535.0f;
    } else if (format == 15 || format == 16) {
        uint16_t u, v;
        std::memcpy(&u, p, 2);
        std::memcpy(&v, p + 2, 2);
        out[0] = half(u), out[1] = half(v);
    }
}

}  // namespace

bool unpack(uint16_t compression, const uint8_t* in, size_t stored, uint32_t size, std::vector<uint8_t>& out) {
    if (compression == 0) {
        out.assign(in, in + stored);
        return true;
    }
    if (compression == 0x5A42) return inflateZlib(in, stored, out, size);
    if (compression == 0xFFFF) return refpack(in, stored, out, size);
    return false;
}

bool readModel(const std::vector<uint8_t>& res, ModelInfo& info, std::string& why) {
    Rcol r;
    if (!readRcol(res, r, why)) return false;
    int at = -1;
    for (size_t i = 0; i < r.chunks.size(); ++i)
        if (r.chunks[i].tag == tagOf("MODL")) {
            at = static_cast<int>(i);
            break;
        }
    if (at < 0) {
        why = "no MODL chunk";
        return false;
    }
    const Chunk& c = r.chunks[at];
    uint32_t count = val<uint32_t>(res, c.pos + 8);
    if (count == 0 || count > 32 || c.size < 36 + count * 20) {
        why = "MODL with " + std::to_string(count) + " detail levels";
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        info.lo[k] = val<float>(res, c.pos + 12 + k * 4);
        info.hi[k] = val<float>(res, c.pos + 24 + k * 4);
    }
    // The LOD entries end the chunk: reference, flags, id, min and max distance.
    size_t first = c.pos + c.size - count * 20;
    for (uint32_t k = 0; k < count; ++k) {
        size_t e = first + k * 20;
        Lod lod;
        uint32_t ref = val<uint32_t>(res, e);
        lod.id = val<uint32_t>(res, e + 8);
        lod.minZ = val<float>(res, e + 12);
        lod.maxZ = val<float>(res, e + 16);
        int i = resolve(r, ref, {"MLOD"});
        if (i >= 0)
            lod.chunk = i;
        else if (i <= -2)
            lod.external = r.external[static_cast<size_t>(-2 - i)];
        else
            continue;
        info.lods.push_back(lod);
    }
    if (info.lods.empty()) why = "no readable detail level";
    return !info.lods.empty();
}

bool readGeometry(const std::vector<uint8_t>& res, int chunk, uint32_t variant, Geometry& out, std::string& why) {
    Rcol r;
    if (!readRcol(res, r, why)) return false;
    if (chunk < 0)
        for (size_t i = 0; i < r.chunks.size() && chunk < 0; ++i)
            if (r.chunks[i].tag == tagOf("MLOD")) chunk = static_cast<int>(i);
    if (chunk < 0 || chunk >= static_cast<int>(r.chunks.size()) || r.chunks[chunk].tag != tagOf("MLOD")) {
        why = "no MLOD chunk";
        return false;
    }
    const Chunk& lod = r.chunks[chunk];
    const uint32_t count = val<uint32_t>(res, lod.pos + 8);
    if (count > 256) {
        why = "MLOD with " + std::to_string(count) + " meshes";
        return false;
    }
    size_t at = lod.pos + 12;
    for (uint32_t m = 0; m < count; ++m) {
        const uint32_t length = val<uint32_t>(res, at);
        const size_t e = at + 4;
        at = e + length;
        if (length < 72 || at > lod.pos + lod.size) {
            why = "MLOD mesh cut off";
            return false;
        }
        const uint32_t matRef = val<uint32_t>(res, e + 4), vrtfRef = val<uint32_t>(res, e + 8),
                       vbufRef = val<uint32_t>(res, e + 12), ibufRef = val<uint32_t>(res, e + 16),
                       flags = val<uint32_t>(res, e + 20);
        const int32_t stream = val<int32_t>(res, e + 24), startVertex = val<int32_t>(res, e + 28),
                      startIndex = val<int32_t>(res, e + 32), minVertex = val<int32_t>(res, e + 36),
                      vertexCount = val<int32_t>(res, e + 40), primitives = val<int32_t>(res, e + 44);
        float box[6];
        for (int k = 0; k < 6; ++k) box[k] = val<float>(res, e + 48 + k * 4);
        char note[96];
        auto skip = [&](const char* reason) {
            std::snprintf(note, sizeof(note), "mesh %u: %s; ", m, reason);
            out.skipped += note;
        };
        if ((flags & 0xFF) != 3) {
            skip("not a triangle list");
            continue;
        }
        if (vertexCount <= 0 || vertexCount > 200000 || primitives <= 0 || primitives > 400000) {
            skip("bad counts");
            continue;
        }
        Material mat;
        bool haveMat = materialFor(res, r, matRef, variant, mat);
        if (haveMat && mat.shader == kDropShadow) {
            skip("drop shadow");
            continue;
        }
        if (haveMat && mat.shader == kImpostor && vertexCount <= 8) {
            skip("picture card");
            continue;
        }
        if (vertexCount <= 16 && box[4] - box[1] < 0.02f && box[1] < 0.05f) {
            skip("flat shadow card");
            continue;
        }
        const int vf = resolve(r, vrtfRef, {"VRTF"}), vb = resolve(r, vbufRef, {"VBUF"}), ib = resolve(r, ibufRef, {"IBUF"});
        if (vf < 0 || vb < 0 || ib < 0) {
            skip("no vertex format or buffers");
            continue;
        }
        // Vertex format.
        const Chunk& fc = r.chunks[vf];
        const uint32_t stride = val<uint32_t>(res, fc.pos + 8), elements = val<uint32_t>(res, fc.pos + 12),
                       extended = val<uint32_t>(res, fc.pos + 16);
        if (stride < 4 || stride > 256 || elements == 0 || elements > 32) {
            skip("bad vertex format");
            continue;
        }
        Element pos, nrm, uv;
        size_t ea = fc.pos + 20;
        for (uint32_t k = 0; k < elements; ++k) {
            uint32_t usage, index, format, offset;
            if (extended) {
                usage = val<uint32_t>(res, ea), index = val<uint32_t>(res, ea + 4), format = val<uint32_t>(res, ea + 8),
                offset = val<uint32_t>(res, ea + 12);
                ea += 16;
            } else {
                usage = res[ea], index = res[ea + 1], format = res[ea + 2], offset = res[ea + 3];
                ea += 4;
            }
            if (offset >= stride) continue;
            Element el{static_cast<int>(format), offset};
            if (usage == 0 && pos.format < 0) pos = el;
            if (usage == 1 && nrm.format < 0) nrm = el;
            if (usage == 2 && index == 0 && uv.format < 0) uv = el;
        }
        if (pos.format < 0) {
            skip("no positions");
            continue;
        }
        // Vertices: past the VBUF header at the mesh's stream offset (else the other layouts some models use).
        const Chunk& vc = r.chunks[vb];
        const size_t need = static_cast<size_t>(vertexCount) * stride;
        const float slack = 0.02f + 0.02f * std::fmax(box[3] - box[0], std::fmax(box[4] - box[1], box[5] - box[2]));
        size_t base = 0;
        bool found = false;
        for (size_t candidate : {vc.pos + 16 + stream, vc.pos + 16 + stream + static_cast<size_t>(startVertex) * stride,
                                 vc.pos + 16 + stream + static_cast<size_t>(minVertex) * stride, vc.pos + stream}) {
            if (candidate < vc.pos || candidate + need > vc.pos + vc.size) continue;
            int inside = 0;
            for (int v = 0; v < vertexCount; ++v) {
                float p[3];
                if (!decodePosition(res.data() + candidate + v * stride + pos.offset, pos.format, p)) break;
                bool in = true;
                for (int k = 0; k < 3; ++k) in = in && p[k] >= box[k] - slack && p[k] <= box[3 + k] + slack;
                inside += in;
            }
            if (inside >= vertexCount * 95 / 100) {
                base = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            skip("positions don't fit the mesh's box");
            continue;
        }
        // Indices: 16 or 32 bit, maybe each the difference to the one before (from the buffer's start).
        const Chunk& ic = r.chunks[ib];
        const uint32_t iflags = val<uint32_t>(res, ic.pos + 8);
        const bool wide = iflags & 2, differenced = iflags & 1;
        const size_t isize = wide ? 4 : 2, total = (ic.size - 16) / isize;
        const size_t from = static_cast<size_t>(startIndex), take = static_cast<size_t>(primitives) * 3;
        if (startIndex < 0 || from + take > total) {
            skip("indices past the buffer");
            continue;
        }
        std::vector<uint32_t> idx(take);
        int64_t acc = 0;
        const uint8_t* ip = res.data() + ic.pos + 16;
        for (size_t k = 0; k < from + take; ++k) {
            int64_t v;
            if (wide) {
                int32_t x;
                std::memcpy(&x, ip + k * 4, 4);
                v = x;
            } else {
                int16_t x;
                std::memcpy(&x, ip + k * 2, 2);
                v = x;
            }
            acc = differenced ? acc + v : (wide ? v : static_cast<uint16_t>(v));
            if (!wide) acc &= 0xFFFF;
            if (k >= from) idx[k - from] = static_cast<uint32_t>(acc);
        }
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t v : idx) lo = v < lo ? v : lo, hi = v > hi ? v : hi;
        uint32_t shift = 0;
        if (hi >= static_cast<uint32_t>(vertexCount)) {
            if (lo >= static_cast<uint32_t>(minVertex) && hi - static_cast<uint32_t>(minVertex) < static_cast<uint32_t>(vertexCount))
                shift = static_cast<uint32_t>(minVertex);
            else {
                skip("indices out of range");
                continue;
            }
        }
        // Into the model.
        const uint32_t vbase = static_cast<uint32_t>(out.vertices.size());
        const float us = haveMat && mat.hasUvScale ? mat.uvScale[0] : 1, vs = haveMat && mat.hasUvScale ? mat.uvScale[1] : 1;
        for (int v = 0; v < vertexCount; ++v) {
            const uint8_t* p = res.data() + base + v * stride;
            Vertex vx;
            decodePosition(p + pos.offset, pos.format, vx.pos);
            if (nrm.format >= 0)
                decodeNormal(p + nrm.offset, nrm.format, vx.nrm);
            else
                vx.nrm[0] = 0, vx.nrm[1] = 1, vx.nrm[2] = 0;
            if (uv.format >= 0) {
                decodeUv(p + uv.offset, uv.format, vx.uv);
                vx.uv[0] = vx.uv[0] * us + (haveMat ? mat.uvOffset[0] : 0);
                vx.uv[1] = vx.uv[1] * vs + (haveMat ? mat.uvOffset[1] : 0);
            } else {
                vx.uv[0] = vx.uv[1] = 0;
            }
            out.vertices.push_back(vx);
        }
        Mesh mesh;
        mesh.firstIndex = static_cast<uint32_t>(out.indices.size());
        mesh.indexCount = static_cast<uint32_t>(take);
        for (uint32_t v : idx) out.indices.push_back(vbase + v - shift);
        if (haveMat) {
            mesh.shader = mat.shader;
            mesh.hasDiffuse = mat.hasDiffuse;
            mesh.diffuse = mat.diffuse;
            // alphamaskthreshold is on the 0-255 scale (leaves: 128)
            const float clip = mat.alphaClip > 1 ? mat.alphaClip / 255.0f : mat.alphaClip;
            mesh.alphaClip = clip > 0 && clip < 1 ? clip : 0;
            mesh.params = mat.params;
        }
        out.meshes.push_back(mesh);
    }
    if (out.meshes.empty()) {
        why = "no drawable mesh (" + out.skipped + ")";
        return false;
    }
    return true;
}

uint32_t levelSize(int format, uint32_t w, uint32_t h) {
    if (format == 4) return w * h * 4;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    return bw * bh * (format == 1 ? 8 : 16);
}

bool readTexture(std::vector<uint8_t>& dds, uint32_t maxSize, Texture& out, std::string& why) {
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) {
        why = "not a DDS picture";
        return false;
    }
    uint32_t h = val<uint32_t>(dds, 12), w = val<uint32_t>(dds, 16), mips = val<uint32_t>(dds, 28),
             pfFlags = val<uint32_t>(dds, 80), bits = val<uint32_t>(dds, 88);
    char fourcc[5] = {};
    std::memcpy(fourcc, dds.data() + 84, 4);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        why = "bad picture size";
        return false;
    }
    if (mips == 0) mips = 1;
    const size_t n = dds.size() - 128;
    uint8_t* data = dds.data() + 128;
    int format = 0;
    if (!std::strcmp(fourcc, "DST1") || !std::strcmp(fourcc, "DXT1")) format = 1;
    if (!std::strcmp(fourcc, "DST3") || !std::strcmp(fourcc, "DXT3")) format = 2;
    if (!std::strcmp(fourcc, "DST5") || !std::strcmp(fourcc, "DXT5")) format = 3;
    // DST: each block's parts are stored in separate runs over the whole picture (all mip levels):
    // put the blocks back together (s4pi DSTResource.Unshuffle).
    if (fourcc[0] == 'D' && fourcc[1] == 'S' && fourcc[2] == 'T') {
        std::vector<uint8_t> blocks(n);
        if (format == 1) {
            const size_t count = n / 8;
            for (size_t k = 0; k < count; ++k) {
                std::memcpy(&blocks[k * 8], data + k * 4, 4);
                std::memcpy(&blocks[k * 8 + 4], data + count * 4 + k * 4, 4);
            }
        } else if (format) {
            const size_t count = n / 16;
            const uint8_t *a0 = data, *c0 = data + count * 2, *a1 = c0 + count * 4, *c1 = a1 + count * 6;
            for (size_t k = 0; k < count; ++k) {
                std::memcpy(&blocks[k * 16], a0 + k * 2, 2);
                std::memcpy(&blocks[k * 16 + 2], a1 + k * 6, 6);
                std::memcpy(&blocks[k * 16 + 8], c0 + k * 4, 4);
                std::memcpy(&blocks[k * 16 + 12], c1 + k * 4, 4);
            }
        }
        std::memcpy(data, blocks.data(), n);
    }
    std::vector<uint8_t> rgba;
    if (!format) {
        // Uncompressed 24/32 bit (by its channel masks): only the top level, as RGBA.
        if (!(pfFlags & 0x40) || (bits != 32 && bits != 24)) {
            why = std::string("picture format '") + fourcc + "'";
            return false;
        }
        const size_t step = bits / 8;
        if (n < static_cast<size_t>(w) * h * step) {
            why = "picture cut off";
            return false;
        }
        uint32_t mask[4];
        std::memcpy(mask, dds.data() + 92, 16);
        auto channel = [](uint32_t v, uint32_t m) -> uint32_t {
            if (!m) return 255;
            int shift = 0;
            while (!((m >> shift) & 1)) ++shift;
            const uint32_t top = m >> shift;
            return top ? ((v & m) >> shift) * 255 / top : 255;
        };
        const bool alpha = (pfFlags & 1) && mask[3];
        rgba.resize(static_cast<size_t>(w) * h * 4);
        for (size_t k = 0; k < static_cast<size_t>(w) * h; ++k) {
            const uint8_t* p = data + k * step;
            uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (step == 4 ? static_cast<uint32_t>(p[3]) << 24 : 0);
            rgba[k * 4] = static_cast<uint8_t>(channel(v, mask[0]));
            rgba[k * 4 + 1] = static_cast<uint8_t>(channel(v, mask[1]));
            rgba[k * 4 + 2] = static_cast<uint8_t>(channel(v, mask[2]));
            rgba[k * 4 + 3] = static_cast<uint8_t>(alpha ? channel(v, mask[3]) : 255);
        }
        out.format = 4;
        out.w = w, out.h = h, out.mips = 1;
        out.data.swap(rgba);
        return true;
    }
    // Skip the top levels larger than maxSize, keep the rest of the chain (down to 4 x 4).
    size_t at = 0;
    uint32_t level = 0;
    while (level + 1 < mips && (w > maxSize || h > maxSize) && w / 2 >= 4 && h / 2 >= 4) {
        at += levelSize(format, w, h);
        w /= 2, h /= 2;
        ++level;
    }
    if (w % 4 || h % 4) {
        why = "picture not a multiple of 4";
        return false;
    }
    uint32_t kept = 0;
    size_t end = at;
    for (uint32_t lw = w, lh = h; level + kept < mips && lw >= 1 && lh >= 1; lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        size_t sz = levelSize(format, lw, lh);
        if (end + sz > n) break;
        end += sz;
        ++kept;
        if (lw == 1 && lh == 1) break;
    }
    if (!kept) {
        why = "picture cut off";
        return false;
    }
    out.format = format;
    out.w = w, out.h = h, out.mips = kept;
    out.data.assign(data + at, data + end);
    return true;
}

}  // namespace cbo::parse
