// Foliage push shader patch (re/foliage/FOLIAGE_VS_REPORT.md, user 2026-10-28: plants react to your body).
//
// 307 of the game's vertex shaders sway leaves and grass: a bend weight (TEXCOORD1.x) times a sine along the wind
// direction is added to the vertex. This patch adds, right after that wind add, a push away from up to eight spheres
// (the player's head, hands, body; constant buffer slot 6, foliagepatch.h), weighted the same way, so only the free
// ends of leaves and blades move:
//
//     w     = saturate(TEXCOORD1.x * cb1[0].w)                 the bend weight
//     for each sphere:  d = P - C;  t = saturate(1 - |d| / R);  push += d / |d| * t * t
//     P    += (push * cb6[8].x + (0, cb6[8].y, 0)) * w
//
// Two families, found by their instructions:
//   static    the model position is in object space and goes to the clip space through cb0[0..2] (world) and
//             cb0[4..7]. The push is worked out in world space, brought back with the transpose of the world
//             matrix's rotation (divided by its squared scale) and added to the object-space position.
//   instanced the position is rotated into the world and then loses the camera position (add ..., -cb2[0]).
//             The push is added to the world position just before that.
// A shader the patch does not recognise is left alone (the caller creates the original).
//
// Everything is wrapped in `if_nz cb6[8].z`: with nothing to push a vertex pays one branch.
// The container's checksum (DXBC's modified MD5) is recomputed.
#include "foliagepatch.h"

#include <cstring>

namespace s4vr::foliagepatch {
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
    kAdd = 0x00, kDiv = 0x0e, kDp3 = 0x10, kDp4 = 0x11, kEndIf = 0x15, kIf = 0x1f, kMad = 0x32, kMax = 0x34, kMov = 0x36,
    kMul = 0x38, kSqrt = 0x4b, kSincos = 0x4d, kCustomData = 0x35, kDclConstantBuffer = 0x59, kDclTemps = 0x68,
    kDclFirst = 0x58, kDclLast = 0x6a,
};
constexpr uint32_t kSaturate = 1u << 13, kTestNonZero = 1u << 18;
enum OperandType { kTemp = 0, kInput = 1, kConstantBuffer = 8 };
// Swizzles (x y z w, two bits each).
constexpr unsigned X = 0x00, Y = 0x55, Z = 0xaa, W = 0xff, XYZ = 0xe4;

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
bool isCb(const Op& o, unsigned slot, int row) {
    return o.type == kConstantBuffer && o.dim == 2 && o.idx[0] == slot && (row < 0 || o.idx[1] == static_cast<unsigned>(row));
}
// A source's first three swizzle components are x y z.
bool swizzleXyz(const Op& o) { return o.mode == 1 && (o.swz & 0x3f) == 0x24; }

// ---- Token builders ----
using Tokens = std::vector<uint32_t>;
Tokens D(unsigned reg, unsigned mask) { return {0x100002u | (mask << 4), reg}; }
Tokens S(unsigned reg, unsigned swz) { return {0x100006u | (swz << 4), reg}; }
Tokens C(unsigned slot, unsigned row, unsigned swz) { return {0x208006u | (swz << 4), slot, row}; }
Tokens CSel(unsigned slot, unsigned row, unsigned comp) { return {0x20800au | (comp << 4), slot, row}; }
Tokens In(unsigned reg, unsigned swz) { return {0x101006u | (swz << 4), reg}; }
Tokens Im(float v) {
    const uint32_t b = bitsOf(v);
    return {0x4002u, b, b, b, b};
}

struct Emit {
    Tokens code;
    void op(uint32_t opcode, std::initializer_list<Tokens> operands) {
        size_t n = 1;
        for (const Tokens& o : operands) n += o.size();
        code.push_back(opcode | (static_cast<uint32_t>(n) << 24));
        for (const Tokens& o : operands) code.insert(code.end(), o.begin(), o.end());
    }
};

// The push, from the spheres to the displacement `acc` (xyz, world metres, not yet weighted) using `p` as the
// world position. t0 = scratch (w: the bend weight), t1 = acc, t3 = scratch. p is a temp register.
void pushLoop(Emit& e, unsigned t0, unsigned t1, unsigned t3, unsigned p, unsigned texcoord1) {
    e.op(kMul | kSaturate, {D(t0, 8), In(texcoord1, X), C(1, 0, W)});
    e.op(kMov, {D(t1, 7), Im(0)});
    for (unsigned i = 0; i < kSpheres; ++i) {
        e.op(kAdd, {D(t3, 7), S(p, XYZ), C(kSlot, i, XYZ)});          // d = P - C (the buffer holds -C)
        e.op(kDp3, {D(t3, 8), S(t3, XYZ), S(t3, XYZ)});               // |d|^2
        e.op(kSqrt, {D(t0, 1), S(t3, W)});                            // |d|
        e.op(kMad | kSaturate, {D(t0, 2), S(t0, X), C(kSlot, i, W), Im(1)});  // 1 - |d| / R
        e.op(kMul, {D(t0, 2), S(t0, Y), S(t0, Y)});                   // eased
        e.op(kMax, {D(t0, 1), S(t0, X), Im(1e-4f)});
        e.op(kDiv, {D(t0, 4), S(t0, Y), S(t0, X)});                   // t^2 / |d|
        e.op(kMad, {D(t1, 7), S(t3, XYZ), S(t0, Z), S(t1, XYZ)});
    }
    e.op(kMul, {D(t1, 7), S(t1, XYZ), C(kSlot, kSpheres, X)});
    e.op(kAdd, {D(t1, 2), S(t1, Y), C(kSlot, kSpheres, Y)});
    e.op(kMul, {D(t1, 7), S(t1, XYZ), S(t0, W)});
}

void ifPush(Emit& e) { e.op(kIf | kTestNonZero, {CSel(kSlot, kSpheres, 2)}); }
void endIf(Emit& e) { e.op(kEndIf, {}); }

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

// Input signature: (semantic, index) -> register and the components it declares.
bool inputRegister(const std::vector<uint8_t>& chunk, const char* name, unsigned index, unsigned* reg, unsigned* mask) {
    if (chunk.size() < 8) return false;
    const uint32_t n = rd(chunk.data());
    for (uint32_t i = 0; i < n; ++i) {
        const size_t e = 8 + 24 * size_t(i);
        if (e + 24 > chunk.size()) return false;
        const uint32_t nameAt = rd(chunk.data() + e), idx = rd(chunk.data() + e + 4);
        if (nameAt >= chunk.size()) return false;
        const char* s = reinterpret_cast<const char*>(chunk.data() + nameAt);
        if (strnlen(s, chunk.size() - nameAt) == chunk.size() - nameAt) return false;
        if (idx == index && std::strcmp(s, name) == 0) {
            *reg = rd(chunk.data() + e + 16);
            *mask = chunk[e + 20];
            return true;
        }
    }
    return false;
}

}  // namespace

bool checksumMatches(const uint8_t* code, size_t size) {
    if (size < 32) return false;
    std::vector<uint8_t> copy(code, code + size);
    uint8_t sum[16];
    checksumOf(copy, sum);
    return std::memcmp(sum, code + 4, 16) == 0;
}

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
    int shexAt = -1, isgnAt = -1;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].tag == tag("SHEX") || chunks[i].tag == tag("SHDR")) shexAt = static_cast<int>(i);
        if (chunks[i].tag == tag("ISGN")) isgnAt = static_cast<int>(i);
    }
    if (shexAt < 0 || isgnAt < 0) return fail("no SHEX or ISGN chunk");
    const std::vector<uint8_t>& shex = chunks[shexAt].data;
    if (shex.size() < 8 || shex.size() % 4) return fail("odd SHEX size");
    std::vector<uint32_t> t(shex.size() / 4);
    std::memcpy(t.data(), shex.data(), shex.size());
    if ((t[0] >> 16) != 1 || ((t[0] >> 4) & 0xf) < 4) return fail("not a vertex shader 4 or 5");
    if (t[1] != t.size()) return fail("SHEX length differs from its chunk");

    // Declarations, then code.
    std::vector<Inst> insts;
    size_t codeStart = t.size();
    unsigned temps = 0;
    size_t tempsPos = 0;
    bool haveCb1 = false, haveCb4 = false;
    for (size_t pos = 2; pos < t.size();) {
        Inst in;
        if (!readInst(t, pos, in)) return fail("cannot read an instruction");
        if (codeStart == t.size() && !isDecl(in.op)) codeStart = pos;
        if (codeStart == t.size()) {
            if (in.op == kDclTemps && pos + 1 < t.size()) temps = t[pos + 1], tempsPos = pos;
            if (in.op == kDclConstantBuffer && pos + 2 < t.size()) {
                // dcl_constantbuffer: operand token, slot, size
                const unsigned slot = t[pos + 2];
                if (slot == kSlot) return fail("cb6 is already declared");
                if (slot == 1) haveCb1 = true;
                if (slot == 4) haveCb4 = true;
            }
        } else {
            insts.push_back(in);
        }
        pos += in.len;
    }
    if (!tempsPos || codeStart == t.size()) return fail("no dcl_temps or no code");
    if (!haveCb1 || !haveCb4) return fail("cb1 or cb4 not declared");
    if (temps > 250) return fail("too many temps");

    // A wind shader: one sincos, and the frame time (cb4[0]) read.
    int sincosAt = -1, sincosCount = 0;
    bool readsTime = false;
    for (size_t i = 0; i < insts.size(); ++i) {
        if (insts[i].op == kSincos) sincosAt = static_cast<int>(i), ++sincosCount;
        for (const Op& o : insts[i].ops)
            if (isCb(o, 4, 0)) readsTime = true;
    }
    if (sincosCount != 1 || !readsTime) return fail("not a wind shader (sincos / time)");
    res.wind = true;

    // The wind has been added: mad rA.xyz, ..., cb0[n].xxxx, rA.xyz
    int anchor = -1;
    unsigned reg = 0;
    for (size_t i = sincosAt + 1; i < insts.size() && i < size_t(sincosAt) + 14; ++i) {
        const Inst& in = insts[i];
        if (in.op != kMad || in.ops.size() != 4) continue;
        const Op &d = in.ops[0], &b = in.ops[2], &c = in.ops[3];
        if (isTemp(d) && d.mode == 0 && d.mask == 7 && isCb(b, 0, -1) && b.mode == 1 && b.swz == X && isTemp(c) &&
            c.idx[0] == d.idx[0] && swizzleXyz(c)) {
            anchor = static_cast<int>(i);
            reg = d.idx[0];
            break;
        }
    }
    if (anchor < 0) return fail("no wind add (mad rA.xyz, v, cb0.x, rA.xyz)");

    // The family.
    Family family = kNone;
    int insertAt = -1;  // static: after this instruction, instanced: before it
    unsigned subtractReg = 0;
    {
        unsigned found = 0;
        for (size_t i = anchor + 1; i < insts.size() && i < size_t(anchor) + 24 && found != 7; ++i) {
            const Inst& in = insts[i];
            if (in.op != kDp4 || in.ops.size() != 3) continue;
            const Op &d = in.ops[0], &a = in.ops[1], &b = in.ops[2];
            if (!isTemp(d) || d.mode != 0 || !isTemp(a) || a.idx[0] != reg || a.swz != XYZ + 0 || !isCb(b, 0, -1)) continue;
            const int comp = d.mask == 1 ? 0 : d.mask == 2 ? 1 : d.mask == 4 ? 2 : -1;
            if (comp < 0 || b.idx[1] != static_cast<unsigned>(comp)) continue;
            found |= 1u << comp;
        }
        if (found == 7) {
            family = kStatic;
            insertAt = anchor;
        }
    }
    // Hardware instancing: the rotated, scaled vertex plus the instance position read from a vertex stream
    // (add rD.xyz, rS.xyz, vN.xyz): the push goes into rS before it.
    unsigned streamReg = 0;
    if (family == kNone) {
        for (size_t i = anchor + 1; i < insts.size(); ++i) {
            const Inst& in = insts[i];
            if (in.op != kAdd || in.ops.size() != 3) continue;
            const Op &d = in.ops[0], &a = in.ops[1], &b = in.ops[2];
            if (isTemp(d) && d.mode == 0 && d.mask == 7 && isTemp(a) && swizzleXyz(a) && b.type == kInput && b.dim == 1 &&
                swizzleXyz(b) && !b.neg) {
                family = kStreamed;
                insertAt = static_cast<int>(i);
                subtractReg = a.idx[0];
                streamReg = b.idx[0];
                break;
            }
        }
    }
    // The same with the instance position taken from the camera first (add rD.xyz, vN.xyz, -cb2[0].xyz; add rE.xyz,
    // rD.xyz, rR.xyz): the push goes into rR before that second add.
    if (family == kNone) {
        for (size_t i = anchor + 1; i + 1 < insts.size() && family == kNone; ++i) {
            const Inst& in = insts[i];
            if (in.op != kAdd || in.ops.size() != 3) continue;
            const Op &d = in.ops[0], &a = in.ops[1], &b = in.ops[2];
            if (!(isTemp(d) && d.mode == 0 && d.mask == 7 && a.type == kInput && a.dim == 1 && swizzleXyz(a) && isCb(b, 2, 0) &&
                  b.neg && swizzleXyz(b)))
                continue;
            for (size_t j = i + 1; j < insts.size() && j <= i + 3; ++j) {
                const Inst& nx = insts[j];
                if (nx.op != kAdd || nx.ops.size() != 3) continue;
                const Op &d2 = nx.ops[0], &a2 = nx.ops[1], &b2 = nx.ops[2];
                if (!(isTemp(d2) && d2.mode == 0 && d2.mask == 7 && isTemp(a2) && isTemp(b2) && swizzleXyz(a2) && swizzleXyz(b2)))
                    continue;
                const unsigned rd = d.idx[0];
                const Op* rot = a2.idx[0] == rd ? &b2 : b2.idx[0] == rd ? &a2 : nullptr;
                if (!rot || rot->idx[0] == rd) continue;
                family = kStreamed;
                insertAt = static_cast<int>(j);
                subtractReg = rot->idx[0];
                streamReg = a.idx[0];
                break;
            }
        }
    }
    if (family == kNone) {
        for (size_t i = anchor + 1; i < insts.size(); ++i) {
            const Inst& in = insts[i];
            if (in.op != kAdd || in.ops.size() != 3) continue;
            const Op &d = in.ops[0], &a = in.ops[1], &b = in.ops[2];
            if (isTemp(d) && d.mode == 0 && d.mask == 7 && isTemp(a) && swizzleXyz(a) && isCb(b, 2, 0) && b.neg &&
                swizzleXyz(b)) {
                family = kInstanced;
                insertAt = static_cast<int>(i);
                subtractReg = a.idx[0];
                break;
            }
        }
    }
    // The position is already in the world and goes straight to the clip space (dp4 o#, rA.xyzw, cb0[n]).
    if (family == kNone) {
        for (size_t i = anchor + 1; i < insts.size(); ++i) {
            const Inst& in = insts[i];
            if (in.op != kDp4 || in.ops.size() != 3) continue;
            const Op &d = in.ops[0], &a = in.ops[1], &b = in.ops[2];
            if (d.type == 2 && isTemp(a) && a.idx[0] == reg && a.swz == XYZ && isCb(b, 0, -1)) {
                family = kWorld;
                insertAt = anchor;
                break;
            }
        }
    }
    // The camera-relative position is complete and goes to the clip space (mov rA.w, 1; dp4 o, rA.xyzw, cb2[5]): the
    // split-register variants of the streamed family. The push goes into rA.xyz before the mov, from the world position
    // rA + cb2[0]. Only taken when the camera is subtracted somewhere (add .., -cb2[0]) after the wind.
    if (family == kNone) {
        bool camera = false;
        for (size_t i = anchor + 1; i < insts.size() && !camera; ++i)
            if (insts[i].op == kAdd && insts[i].ops.size() == 3 && isCb(insts[i].ops[2], 2, 0) && insts[i].ops[2].neg) camera = true;
        for (size_t i = anchor + 1; camera && i < insts.size(); ++i) {
            const Inst& in = insts[i];
            if (in.op != kDp4 || in.ops.size() != 3 || !isTemp(in.ops[1]) || !isCb(in.ops[2], 2, 5)) continue;
            const unsigned ra = in.ops[1].idx[0];
            for (size_t j = i; j-- > static_cast<size_t>(anchor);) {
                const Inst& m = insts[j];
                if (m.op != kMov || m.ops.size() != 2 || !isTemp(m.ops[0]) || m.ops[0].idx[0] != ra || m.ops[0].mask != 8) continue;
                family = kCamRel;
                insertAt = static_cast<int>(j);
                subtractReg = ra;
                break;
            }
            break;
        }
    }
    if (family == kNone) return fail("no known position path (static / instanced / streamed / world)");

    unsigned tcReg = 0, tcMask = 0;
    if (!inputRegister(chunks[isgnAt].data, "TEXCOORD", 1, &tcReg, &tcMask) || !(tcMask & 1))
        return fail("no TEXCOORD1.x input");

    // The new code.
    const unsigned t0 = temps, t1 = temps + 1, t2 = temps + 2, t3 = temps + 3;
    Emit e;
    ifPush(e);
    if (family == kStatic) {
        // World position of the object-space position (w = 1) in t2.
        e.op(kMov, {D(t1, 7), S(reg, XYZ)});
        e.op(kMov, {D(t1, 8), Im(1)});
        e.op(kDp4, {D(t2, 1), S(t1, XYZ), C(0, 0, XYZ)});
        e.op(kDp4, {D(t2, 2), S(t1, XYZ), C(0, 1, XYZ)});
        e.op(kDp4, {D(t2, 4), S(t1, XYZ), C(0, 2, XYZ)});
        pushLoop(e, t0, t1, t3, t2, tcReg);
        // World displacement to object space: transpose of the rotation, over the squared scale.
        e.op(kMul, {D(t2, 7), S(t1, X), C(0, 0, XYZ)});
        e.op(kMad, {D(t2, 7), S(t1, Y), C(0, 1, XYZ), S(t2, XYZ)});
        e.op(kMad, {D(t2, 7), S(t1, Z), C(0, 2, XYZ), S(t2, XYZ)});
        e.op(kDp3, {D(t0, 1), C(0, 0, XYZ), C(0, 0, XYZ)});
        e.op(kDiv, {D(t0, 1), Im(1), S(t0, X)});
        e.op(kMad, {D(reg, 7), S(t2, XYZ), S(t0, X), S(reg, XYZ)});
    } else if (family == kInstanced) {
        pushLoop(e, t0, t1, t3, subtractReg, tcReg);
        e.op(kAdd, {D(subtractReg, 7), S(subtractReg, XYZ), S(t1, XYZ)});
    } else if (family == kStreamed) {
        e.op(kAdd, {D(t2, 7), S(subtractReg, XYZ), In(streamReg, XYZ)});  // the world position
        pushLoop(e, t0, t1, t3, t2, tcReg);
        e.op(kAdd, {D(subtractReg, 7), S(subtractReg, XYZ), S(t1, XYZ)});
    } else if (family == kCamRel) {
        e.op(kAdd, {D(t2, 7), S(subtractReg, XYZ), C(2, 0, XYZ)});  // the world position
        pushLoop(e, t0, t1, t3, t2, tcReg);
        e.op(kAdd, {D(subtractReg, 7), S(subtractReg, XYZ), S(t1, XYZ)});
    } else {
        pushLoop(e, t0, t1, t3, reg, tcReg);
        e.op(kAdd, {D(reg, 7), S(reg, XYZ), S(t1, XYZ)});
    }
    endIf(e);

    // Rebuild the program: declarations (temps raised, cb6 added), code with the new code in.
    Tokens n;
    n.push_back(t[0]);
    n.push_back(0);
    Tokens dclCb = {kDclConstantBuffer | (4u << 24), 0x00208002u, kSlot, kRows};
    // Copy an existing dcl_constantbuffer's operand token and access pattern, so the declaration matches the game's.
    for (size_t pos = 2; pos < codeStart;) {
        const size_t len = (t[pos] & 0x7ff) == kCustomData ? t[pos + 1] : ((t[pos] >> 24) & 0x7f);
        if ((t[pos] & 0x7ff) == kDclConstantBuffer && len == 4) {
            dclCb[0] = t[pos];
            dclCb[1] = t[pos + 1];
            break;
        }
        pos += len;
    }
    for (size_t pos = 2; pos < codeStart;) {
        const size_t len = (t[pos] & 0x7ff) == kCustomData ? t[pos + 1] : ((t[pos] >> 24) & 0x7f);
        if (pos == tempsPos) {
            n.push_back(t[pos]);
            n.push_back(temps + 4);
        } else {
            n.insert(n.end(), t.begin() + pos, t.begin() + pos + len);
        }
        pos += len;
    }
    n.insert(n.end(), dclCb.begin(), dclCb.end());
    for (size_t i = 0; i < insts.size(); ++i) {
        if ((family == kInstanced || family == kStreamed || family == kCamRel) && static_cast<int>(i) == insertAt) n.insert(n.end(), e.code.begin(), e.code.end());
        n.insert(n.end(), t.begin() + insts[i].pos, t.begin() + insts[i].pos + insts[i].len);
        if ((family == kStatic || family == kWorld) && static_cast<int>(i) == insertAt) n.insert(n.end(), e.code.begin(), e.code.end());
    }
    n[1] = static_cast<uint32_t>(n.size());

    // The container.
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
    res.family = family;
    return true;
}

}  // namespace s4vr::foliagepatch
