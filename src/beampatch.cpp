// The phone flashlight's beam inside the game's object pixel shaders (design A, user 2026-11-26): one more term in the
// light sum, diffuse = albedo * (lights + ambient + BEAM), so it is lit before the 8-bit store, keeps the surface's
// true colours and is not touched by Realistic Lighting's darkness (which scales the light inputs, never the sum).
// The matcher and the DXBC rebuild are the ones of Realistic Lighting's shinepatch.cpp (copied, separate code: both
// mods stay standalone and either patch order works, see realistic-lighting/DESIGN.md "the patched form").
//
// Anchor: the final `mad dst.xyz, albedo, light, shine` (shine cube-derived, light through rsq / saturated dp3),
// only in shaders whose input signature has TEXCOORD4 (unnormalised world vector TO the camera, xyz) and TEXCOORD2
// (world normal): re/shaders/FLASHLIGHT-PS.md. Camera-relative fragment position = -v(TEXCOORD4).
// The beam's constants sit in our own cbuffer on slot kSlot (4 vec4, see beampatch.h).
#include "beampatch.h"

#include <cstring>

namespace s4vr::beampatch {
namespace {

uint32_t rd(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void wr(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint32_t bitsOf(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

// ---- DXBC checksum: MD5's block function with the container's own padding ----
void md5Block(uint32_t st[4], const uint8_t* blk) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = rd(blk + 4 * i);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        switch (i >> 4) {
            case 0: f = (b & c) | (~b & d); g = i; break;
            case 1: f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
            case 2: f = b ^ c ^ d; g = (3 * i + 5) & 15; break;
            default: f = c ^ (b | ~d); g = (7 * i) & 15; break;
        }
        const int s = R[(i >> 4) * 4 + (i & 3)];
        f += a + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += (f << s) | (f >> (32 - s));
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

void checksumOf(const std::vector<uint8_t>& dxbc, uint8_t out[16]) {
    const uint8_t* d = dxbc.data() + 20;
    const size_t n = dxbc.size() - 20, full = n - n % 64, left = n - full;
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t i = 0; i < full; i += 64) md5Block(st, d + i);
    uint8_t blk[64] = {};
    if (left >= 56) {
        std::memcpy(blk, d + full, left);
        blk[left] = 0x80;
        md5Block(st, blk);
        std::memset(blk, 0, 64);
        wr(blk, static_cast<uint32_t>(n * 8));
        wr(blk + 60, static_cast<uint32_t>((n * 2) | 1));
        md5Block(st, blk);
    } else {
        wr(blk, static_cast<uint32_t>(n * 8));
        std::memcpy(blk + 4, d + full, left);
        blk[4 + left] = 0x80;
        wr(blk + 60, static_cast<uint32_t>((n * 2) | 1));
        md5Block(st, blk);
    }
    for (int i = 0; i < 4; ++i) wr(out + 4 * i, st[i]);
}

// ---- Shader model 4/5 tokens ----
enum Opcode : uint32_t {
    kAdd = 0x00, kDp2 = 0x0f, kDp3 = 0x10, kDp4 = 0x11, kMad = 0x32, kMul = 0x38, kRsq = 0x44, kCustomData = 0x35,
    kDclConstantBuffer = 0x59, kDclResource = 0x58, kDclTemps = 0x68, kDclFirst = 0x58, kDclLast = 0x6a,
};
constexpr uint32_t kSaturate = 1u << 13;
enum OperandType { kTemp = 0, kInput = 1, kResource = 7, kConstantBuffer = 8 };
constexpr unsigned XYZW = 0xe4, W = 0xff;

struct Op {
    size_t pos = 0, len = 0;
    unsigned type = 0, dim = 0, ncomp = 0, mode = 0;
    unsigned mask = 0, swz = 0, sel = 0;
    uint32_t idx[3] = {0, 0, 0};
    bool neg = false;
};

bool readOp(const std::vector<uint32_t>& t, size_t p, size_t end, Op& o) {
    if (p >= end) return false;
    const uint32_t tok = t[p];
    o = Op();
    o.pos = p;
    size_t q = p + 1;
    o.type = (tok >> 12) & 0xff;
    o.dim = (tok >> 20) & 3;
    o.ncomp = tok & 3;
    o.mode = (tok >> 2) & 3;
    if (o.mode == 0) o.mask = (tok >> 4) & 0xf;
    if (o.mode == 1) o.swz = (tok >> 4) & 0xff;
    if (o.mode == 2) o.sel = (tok >> 4) & 3;
    for (uint32_t ext = tok >> 31; ext;) {
        if (q >= end) return false;
        const uint32_t e = t[q++];
        if ((e & 0x3f) == 1) {
            const uint32_t m = (e >> 6) & 0xff;
            if (m == 1 || m == 3) o.neg = true;
        }
        ext = e >> 31;
    }
    if (o.type == 4 || o.type == 5) {
        q += (o.ncomp == 1 ? 1 : 4) * (o.type == 5 ? 2 : 1);
    } else {
        for (unsigned k = 0; k < o.dim; ++k) {
            const unsigned rep = (tok >> (22 + 3 * k)) & 7;
            if (rep == 0 || rep == 3) {
                if (q >= end) return false;
                o.idx[k] = t[q++];
            } else if (rep == 1 || rep == 4) {
                q += 2;
            } else if (rep != 2) {
                return false;
            }
            if (rep >= 2 && rep <= 4) {
                Op sub;
                if (!readOp(t, q, end, sub)) return false;
                q += sub.len;
            }
        }
    }
    if (q > end) return false;
    o.len = q - p;
    return true;
}

struct Inst {
    size_t pos = 0, len = 0;
    uint32_t op = 0;
    std::vector<Op> ops;
};

bool isDecl(uint32_t op) { return op == kCustomData || (op >= kDclFirst && op <= kDclLast); }

bool readInst(const std::vector<uint32_t>& t, size_t pos, Inst& in) {
    in = Inst();
    in.pos = pos;
    in.op = t[pos] & 0x7ff;
    in.len = in.op == kCustomData ? (pos + 1 < t.size() ? t[pos + 1] : 0) : ((t[pos] >> 24) & 0x7f);
    if (in.len == 0 || pos + in.len > t.size()) return false;
    if (in.op == kCustomData) return true;
    const size_t end = pos + in.len;
    size_t p = pos + 1;
    if (t[pos] >> 31) {
        while (p < end && (t[p++] >> 31)) {
        }
    }
    while (p < end) {
        Op o;
        if (!readOp(t, p, end, o)) return false;
        p += o.len;
        in.ops.push_back(o);
    }
    return true;
}

bool isTemp(const Op& o) { return o.type == kTemp && o.dim == 1; }
// ---- Token builders ----
using Tokens = std::vector<uint32_t>;
Tokens D(unsigned reg, unsigned mask) { return {0x100002u | (mask << 4), reg}; }
Tokens S(unsigned reg, unsigned swz) { return {0x100006u | (swz << 4), reg}; }
Tokens ImV(float a, float b, float c, float d) { return {0x4002u, bitsOf(a), bitsOf(b), bitsOf(c), bitsOf(d)}; }
Tokens Im(float v) { return ImV(v, v, v, v); }

struct Emit {
    Tokens code;
    void op(uint32_t opcode, std::initializer_list<Tokens> operands) {
        size_t n = 1;
        for (const Tokens& o : operands) n += o.size();
        code.push_back(opcode | (static_cast<uint32_t>(n) << 24));
        for (const Tokens& o : operands) code.insert(code.end(), o.begin(), o.end());
    }
};

bool isFlow(uint32_t op) {
    switch (op) {
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x0a: case 0x0d:
        case 0x12: case 0x15: case 0x16: case 0x17: case 0x1f: case 0x2c: case 0x30: case 0x3e: case 0x3f: case 0x4c:
            return true;
        default:
            return false;
    }
}
// Instructions with two destinations (sincos, udiv, imul, umul, uaddc, usubb, swapc).
bool twoDst(uint32_t op) { return op == 0x4d || op == 0x4e || op == 0x26 || op == 0x51 || (op >= 0x89 && op <= 0x8e); }

enum : uint8_t { kCube = 1, kNormal = 2, kOther = 4 };

// ---- Container ----
struct Chunk {
    uint32_t tag;
    std::vector<uint8_t> data;
};

bool readChunks(const uint8_t* code, size_t size, std::vector<Chunk>& chunks) {
    if (size < 32 || std::memcmp(code, "DXBC", 4) != 0 || rd(code + 24) != size) return false;
    const uint32_t count = rd(code + 28);
    if (count == 0 || count > 32 || 32 + 4 * size_t(count) > size) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = rd(code + 32 + 4 * i);
        if (off + 8 > size) return false;
        const uint32_t len = rd(code + off + 4);
        if (off + 8 + size_t(len) > size) return false;
        chunks.push_back({rd(code + off), std::vector<uint8_t>(code + off + 8, code + off + 8 + len)});
    }
    return true;
}

constexpr uint32_t tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

}  // namespace

bool patch(const uint8_t* code, size_t size, std::vector<uint8_t>& out, Info* info) {
    Info local;
    Info& res = info ? *info : local;
    res = Info();
    auto fail = [&](const char* why) {
        res.why = why;
        return false;
    };
    std::vector<Chunk> chunks;
    if (!readChunks(code, size, chunks)) return fail("not a DXBC container");
    int shexAt = -1;
    for (size_t i = 0; i < chunks.size(); ++i)
        if (chunks[i].tag == tag("SHEX") || chunks[i].tag == tag("SHDR")) shexAt = static_cast<int>(i);
    if (shexAt < 0) return fail("no SHEX chunk");
    // Input signature: the registers of TEXCOORD4 (view vector, xyz) and TEXCOORD2 (world normal, xyz).
    int viewReg = -1, nrmReg = -1;
    for (const Chunk& c : chunks) {
        if (c.tag != tag("ISGN")) continue;
        const size_t stride = 24;
        if (c.data.size() < 8) break;
        const uint32_t count = rd(c.data.data());
        for (uint32_t i = 0; i < count && 8 + (i + 1) * stride <= c.data.size(); ++i) {
            const uint8_t* e = c.data.data() + 8 + i * stride;
            const uint32_t nameOff = rd(e);
            if (nameOff + 9 > c.data.size()) continue;
            const char* name = reinterpret_cast<const char*>(c.data.data()) + nameOff;
            if (std::strncmp(name, "TEXCOORD", 9) != 0) continue;
            const uint32_t index = rd(e + 4), reg = rd(e + 16);
            const unsigned mask = e[20];
            if (index == 4 && (mask & 7) == 7) viewReg = static_cast<int>(reg);
            if (index == 2 && (mask & 7) == 7) nrmReg = static_cast<int>(reg);
        }
    }
    if (viewReg < 0 || nrmReg < 0) return fail("no TEXCOORD4 view vector / TEXCOORD2 normal input");
    const std::vector<uint8_t>& shex = chunks[shexAt].data;
    if (shex.size() < 8 || shex.size() % 4) return fail("odd SHEX size");
    std::vector<uint32_t> t(shex.size() / 4);
    std::memcpy(t.data(), shex.data(), shex.size());
    if ((t[0] >> 16) != 0 || ((t[0] >> 4) & 0xf) < 4) return fail("not a pixel shader 4 or 5");
    if (t[1] != t.size()) return fail("SHEX length differs from its chunk");

    std::vector<Inst> insts;
    size_t codeStart = t.size(), tempsPos = 0, cbPos = 0, cbLen = 0;
    unsigned temps = 0;
    bool cube[128] = {};
    for (size_t pos = 2; pos < t.size();) {
        const uint32_t op = t[pos] & 0x7ff;
        if (codeStart == t.size() && (isDecl(op) || (op >= 0x8f && op <= 0xa2))) {
            const size_t len = op == kCustomData ? (pos + 1 < t.size() ? t[pos + 1] : 0) : ((t[pos] >> 24) & 0x7f);
            if (len == 0 || pos + len > t.size()) return fail("cannot read a declaration");
            if (op == kDclTemps && len >= 2) temps = t[pos + 1], tempsPos = pos;
            if (op == kDclConstantBuffer) cbPos = pos, cbLen = len;
            if (op == kDclResource) {
                const unsigned dim = (t[pos] >> 11) & 0x1f;
                Op o;
                if (readOp(t, pos + 1, pos + len, o) && o.type == kResource && o.idx[0] < 128)
                    cube[o.idx[0]] = dim == 6 || dim == 10;  // texturecube, texturecubearray
            }
            pos += len;
            continue;
        }
        if (codeStart == t.size()) codeStart = pos;
        Inst in;
        if (!readInst(t, pos, in)) return fail("cannot read an instruction");
        insts.push_back(in);
        pos += in.len;
    }
    bool anyCube = false;
    for (bool c : cube) anyCube |= c;
    if (!anyCube) return fail("no cube map");
    res.cube = true;
    if (!tempsPos || codeStart == t.size()) return fail("no dcl_temps or no code");
    if (!cbPos || cbLen != 4) return fail("no constant buffer declaration to copy");
    if (t[cbPos + 2] == kSlot) return fail("slot already used");
    if (temps > 250) return fail("too many temps");

    // Data flow, per temp component.
    std::vector<uint8_t> taint(size_t(temps) * 4, 0);
    auto comp = [&](const Op& o, unsigned i) -> uint8_t {  // the tags of a source's i-th selected component
        if (o.type == kTemp && o.dim == 1) {
            if (o.idx[0] >= temps) return kOther;
            unsigned c = o.mode == 1 ? (o.swz >> (2 * i)) & 3 : o.mode == 2 ? o.sel : i;
            return taint[o.idx[0] * 4 + c];
        }
        if (o.type == 4 || o.type == 5) return 0;
        return kOther;
    };
    auto all = [&](const Op& o) -> uint8_t {
        uint8_t r = 0;
        for (unsigned i = 0; i < 4; ++i) r |= comp(o, i);
        return r;
    };
    int found = -1;
    unsigned lightOp = 0;
    int depth = 0, foundDepth = 0;
    for (size_t k = 0; k < insts.size() && found < 0; ++k) {
        const Inst& in = insts[k];
        if (in.op == 0x1f || in.op == 0x30 || in.op == 0x4c) ++depth;
        if (in.op == 0x15 || in.op == 0x16 || in.op == 0x17) --depth;
        if (isFlow(in.op) || in.op == kCustomData || in.ops.empty()) continue;
        const size_t nDst = twoDst(in.op) ? 2 : 1;
        if (in.ops.size() < nDst) continue;
        // The candidate: mad rD.xyz?, a, b, shine.
        if (in.op == kMad && in.ops.size() == 4 && isTemp(in.ops[0]) && in.ops[0].mode == 0 && (in.ops[0].mask & 7) == 7 &&
            isTemp(in.ops[1]) && isTemp(in.ops[2]) && isTemp(in.ops[3])) {
            const uint8_t ta = all(in.ops[1]), tb = all(in.ops[2]), tc = all(in.ops[3]);
            if ((tc & kCube) && !(ta & kCube) && !(tb & kCube)) {
                if (((ta & kNormal) != 0) != ((tb & kNormal) != 0)) {
                    found = static_cast<int>(k);
                    lightOp = (ta & kNormal) ? 1 : 2;
                    foundDepth = depth;
                    break;
                }
                res.why = "light and albedo not told apart";
            }
        }
        // Propagate.
        bool resource = false, cubeRead = false;
        for (size_t i = nDst; i < in.ops.size(); ++i)
            if (in.ops[i].type == kResource) {
                resource = true;
                if (in.ops[i].idx[0] < 128 && cube[in.ops[i].idx[0]]) cubeRead = true;
            }
        uint8_t newT[2][4] = {};
        for (size_t d = 0; d < nDst; ++d) {
            const Op& dst = in.ops[d];
            for (unsigned c = 0; c < 4; ++c) {
                uint8_t v = 0;
                if (resource || in.op == kDp2 || in.op == kDp3 || in.op == kDp4 || nDst == 2) {
                    const unsigned n = in.op == kDp2 ? 2 : in.op == kDp3 ? 3 : 4;
                    for (size_t i = nDst; i < in.ops.size(); ++i)
                        for (unsigned j = 0; j < n; ++j) v |= comp(in.ops[i], j);
                    if (cubeRead) v |= kCube;
                    if (in.op == kDp3 && (t[in.pos] & kSaturate)) v |= kNormal;
                } else {
                    for (size_t i = nDst; i < in.ops.size(); ++i) v |= comp(in.ops[i], c);
                    if (in.op == kRsq) v |= kNormal;
                }
                newT[d][c] = v;
            }
            (void)dst;
        }
        for (size_t d = 0; d < nDst; ++d) {
            const Op& dst = in.ops[d];
            if (!isTemp(dst) || dst.mode != 0 || dst.idx[0] >= temps) continue;
            for (unsigned c = 0; c < 4; ++c)
                if (dst.mask & (1u << c)) taint[dst.idx[0] * 4 + c] = newT[d][c];
        }
    }
    if (found < 0) return fail(res.why.empty() ? "no albedo * light + reflection" : res.why.c_str());
    const Inst& mad = insts[found];
    if (t[mad.pos] >> 31) return fail("extended mad");
    res.inBranch = foundDepth > 0;

    // The new code (temps rB beam, rT, rU scratch, rN the new light operand):
    //   t = v + Lpos (to the light), then the cone, fall-off and N.L of the beam, added to the light operand.
    const unsigned rB = temps, rT = temps + 1, rU = temps + 2, rN = temps + 3;
    const Op& light = mad.ops[lightOp];
    auto CB = [&](unsigned elem, unsigned swz) -> Tokens { return {0x208006u | (swz << 4), kSlot, elem}; };
    auto IN = [&](unsigned reg, unsigned swz) -> Tokens { return {0x101006u | (swz << 4), reg}; };
    constexpr unsigned X = 0x00, Y = 0x55, Z = 0xAA, Wc = 0xFF;
    Emit e;
    e.op(0x36, {D(rB, 15), Im(0)});                                    // mov rB, 0
    {                                                                   // if_nz cb[2].w
        Tokens c = CB(2, Wc);
        e.code.push_back(0x1fu | (1u << 18) | (static_cast<uint32_t>(1 + c.size()) << 24));
        e.code.insert(e.code.end(), c.begin(), c.end());
    }
    e.op(kAdd, {D(rT, 7), IN(viewReg, XYZW), CB(0, XYZW)});             // t = v + Lpos
    e.op(kDp3, {D(rT, 8), S(rT, XYZW), S(rT, XYZW)});                   // t.w = |t|^2
    e.op(kRsq, {D(rU, 1), S(rT, Wc)});                                  // u.x = 1/|t|
    e.op(kDp3, {D(rU, 2), S(rT, XYZW), CB(1, XYZW)});                   // u.y = t . (-Ldir) = cos * |t|
    e.op(kMul, {D(rU, 2), S(rU, Y), S(rU, X)});                         // cos
    e.op(kMad | kSaturate, {D(rU, 2), S(rU, Y), CB(2, X), CB(2, Y)});   // cone ramp
    e.op(kMul, {D(rU, 4), S(rT, Wc), S(rU, X)});                        // u.z = |t|
    e.op(kMad | kSaturate, {D(rU, 4), S(rU, Z), CB(0, Wc), Im(1.0f)});  // sat(1 - |t| / range)  (c0.w = -1/range)
    e.op(kMul, {D(rU, 4), S(rU, Z), S(rU, Z)});
    e.op(kMad, {D(rU, 8), S(rT, Wc), CB(2, Z), Im(1.0f)});              // 1 + |t|^2 / r0^2
    e.op(0x81, {D(rU, 8), S(rU, Wc)});                                  // rcp
    e.op(kMul, {D(rU, 4), S(rU, Z), S(rU, Wc)});
    e.op(kMul, {D(rU, 2), S(rU, Y), S(rU, Z)});                         // cone * fall-off * near
    e.op(kDp3, {D(rU, 8), IN(nrmReg, XYZW), IN(nrmReg, XYZW)});
    e.op(kRsq, {D(rU, 8), S(rU, Wc)});                                  // 1/|N|
    e.op(kDp3, {D(rU, 4), IN(nrmReg, XYZW), S(rT, XYZW)});              // N . t
    e.op(kMul, {D(rU, 4), S(rU, Z), S(rU, Wc)});
    e.op(kMul | kSaturate, {D(rU, 4), S(rU, Z), S(rU, X)});             // N . to-light
    e.op(kMad, {D(rU, 4), S(rU, Z), Im(0.7f), Im(0.3f)});
    e.op(kMul, {D(rU, 2), S(rU, Y), S(rU, Z)});
    e.op(kMul, {D(rB, 7), CB(3, XYZW), S(rU, Y)});                      // beam = colour * factor
    e.code.push_back(0x15u | (1u << 24));                               // endif
    {
        Tokens l(t.begin() + light.pos, t.begin() + light.pos + light.len);
        e.op(kAdd, {D(rN, 15), l, S(rB, XYZW)});                        // light' = light + beam
    }
    {
        Tokens dst(t.begin() + mad.ops[0].pos, t.begin() + mad.ops[0].pos + mad.ops[0].len);
        Tokens ops3[3];
        for (unsigned i = 1; i <= 3; ++i) {
            const Op& o = mad.ops[i];
            ops3[i - 1] = (i == lightOp) ? S(rN, XYZW) : Tokens(t.begin() + o.pos, t.begin() + o.pos + o.len);
        }
        e.op(t[mad.pos] & 0x00ffffffu, {dst, ops3[0], ops3[1], ops3[2]});
    }

    Tokens n;
    n.push_back(t[0]);
    n.push_back(0);
    for (size_t pos = 2; pos < codeStart;) {
        const size_t len = (t[pos] & 0x7ff) == kCustomData ? t[pos + 1] : ((t[pos] >> 24) & 0x7f);
        if (pos == tempsPos) {
            n.push_back(t[pos]);
            n.push_back(temps + 4);
        } else {
            n.insert(n.end(), t.begin() + pos, t.begin() + pos + len);
        }
        if (pos == cbPos) {  // our cbuffer declared right after the last one the shader has
            Tokens d(t.begin() + cbPos, t.begin() + cbPos + cbLen);
            d[2] = kSlot;
            d[3] = 4;
            n.insert(n.end(), d.begin(), d.end());
        }
        pos += len;
    }
    for (size_t i = 0; i < insts.size(); ++i) {
        if (static_cast<int>(i) == found) {
            n.insert(n.end(), e.code.begin(), e.code.end());
            continue;
        }
        n.insert(n.end(), t.begin() + insts[i].pos, t.begin() + insts[i].pos + insts[i].len);
    }
    n[1] = static_cast<uint32_t>(n.size());
    {   // Sanity: every token belongs to an instruction or declaration, and they end exactly at the end.
        size_t pos = 2;
        while (pos < n.size()) {
            const uint32_t op = n[pos] & 0x7ff;
            const size_t len = op == kCustomData ? (pos + 1 < n.size() ? n[pos + 1] : 0) : ((n[pos] >> 24) & 0x7f);
            if (len == 0 || pos + len > n.size()) return fail("rebuilt code does not parse");
            pos += len;
        }
        if (pos != n.size()) return fail("rebuilt code does not parse");
    }

    chunks[shexAt].data.assign(reinterpret_cast<const uint8_t*>(n.data()),
                               reinterpret_cast<const uint8_t*>(n.data()) + n.size() * 4);
    size_t total = 32 + 4 * chunks.size();
    for (const Chunk& c : chunks) total += 8 + c.data.size();
    std::vector<uint8_t> result(total, 0);
    std::memcpy(result.data(), "DXBC", 4);
    wr(result.data() + 20, 1);
    wr(result.data() + 24, static_cast<uint32_t>(total));
    wr(result.data() + 28, static_cast<uint32_t>(chunks.size()));
    size_t off = 32 + 4 * chunks.size();
    for (size_t i = 0; i < chunks.size(); ++i) {
        wr(result.data() + 32 + 4 * i, static_cast<uint32_t>(off));
        wr(result.data() + off, chunks[i].tag);
        wr(result.data() + off + 4, static_cast<uint32_t>(chunks[i].data.size()));
        if (!chunks[i].data.empty()) std::memcpy(result.data() + off + 8, chunks[i].data.data(), chunks[i].data.size());
        off += 8 + chunks[i].data.size();
    }
    uint8_t sum[16];
    checksumOf(result, sum);
    std::memcpy(result.data() + 4, sum, 16);
    out = std::move(result);
    res.patched = true;
    return true;
}

bool checksumMatches(const uint8_t* code, size_t size) {
    if (size < 32) return false;
    std::vector<uint8_t> copy(code, code + size);
    uint8_t sum[16];
    checksumOf(copy, sum);
    return std::memcmp(sum, code + 4, 16) == 0;
}

}  // namespace s4vr::beampatch
