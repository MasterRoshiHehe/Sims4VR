#include "handshape.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "log.h"

namespace s4vr::handshape {

const char* const kPartNames[kParts] = {"palm", "thumb", "index finger", "middle finger", "ring finger", "little finger"};

namespace {

using camera::Quat;
using camera::Vec3;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-8f ? mul(a, 1 / l) : Vec3{0, 0, 0};
}
bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
Quat qmul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
Vec3 rotate(const Quat& q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = mul(Vec3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x}, 2);
    return add(add(v, mul(t, q.w)), Vec3{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x});
}
Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = sub(b, a);
    float l2 = dot(ab, ab);
    float t = l2 > 1e-12f ? dot(sub(p, a), ab) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return add(a, mul(ab, t));
}

// Published poses (armik's job threads write, anyone reads; never blocks the writer for long).
struct Stored {
    Raw raw;
    ULONGLONG tick;
};
struct Slot {
    std::atomic<uint32_t> seq{0};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    Stored data{};
};
Slot g_slot[2];

bool readSlot(int side, Stored& out) {
    const Slot& s = g_slot[side];
    for (int tries = 0; tries < 16; ++tries) {
        uint32_t before = s.seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        std::memcpy(&out, &s.data, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) == before) return before != 0;
    }
    return false;
}

// Capsule radii for an adult hand (metres): the palm's half thickness, and
// each finger's, from the knuckle segment out (thumb, index, middle, ring, little).
constexpr float kPalmRadius = 0.016f;
constexpr float kHeelHalfWidth = 0.02f;  // the palm's heel either side of the wrist joint
constexpr float kFingerRadius[5] = {0.0105f, 0.009f, 0.0092f, 0.0087f, 0.0076f};
constexpr float kTaper = 0.08f;           // each segment out is this much thinner
constexpr float kAdultPalm = 0.085f;      // wrist joint to the middle knuckle, adult
// Bend from straight to a fist, radians summed over a finger's joints (armik's kFist), thumb across the palm.
constexpr float kFistBend = 3.9f, kThumbBend = 2.1f;

void addSegment(Shape& s, Vec3 a, Vec3 b, float radius, int part, int bone) {
    if (s.segmentCount >= 24 || !finite(a) || !finite(b)) return;
    Segment& g = s.segment[s.segmentCount++];
    g.a = a;
    g.b = b;
    g.radius = radius;
    g.part = part;
    g.bone = bone;
}

// The shape from a stored pose, carried by the rigid move from the solve's
// controller pose to `pos`/`rot`.
void build(int side, const Stored& st, const Vec3& pos, const Quat& rot, Shape& s) {
    const Raw& r = st.raw;
    const Quat move = qmul(rot, conj(r.gripRot));
    auto P = [&](Vec3 p) { return add(pos, rotate(move, sub(p, r.gripPos))); };
    s = Shape{};
    s.side = side;
    s.tracked = r.tracked;
    s.tick = st.tick;
    s.wrist = P(r.wrist);
    const Quat hand = qmul(move, r.handRot);
    s.forward = normalize(rotate(hand, r.forward));
    s.thumbSide = normalize(rotate(hand, r.thumbSide));
    s.palmNormal = normalize(rotate(hand, r.palmNormal));
    for (int f = 0; f < 5; ++f) {
        s.jointCount[f] = r.jointCount[f];
        for (int k = 0; k < r.jointCount[f] && k < 4; ++k) s.joint[f][k] = P(r.joint[f][k]);
    }
    const Vec3 midKnuckle = s.jointCount[2] ? s.joint[2][0] : s.jointCount[1] ? s.joint[1][0] : add(s.wrist, mul(s.forward, kAdultPalm));
    float palmLength = length(sub(midKnuckle, s.wrist));
    s.size = palmLength / kAdultPalm;
    if (!(s.size > 0.5f)) s.size = 0.5f;
    if (s.size > 1.5f) s.size = 1.5f;
    const float k = s.size;
    const float pr = kPalmRadius * k;

    // The palm: a fan of capsules from either side of the heel to each
    // knuckle, a bar across the knuckles and one across the heel.
    const Vec3 heelThumb = add(s.wrist, mul(s.thumbSide, kHeelHalfWidth * k));
    const Vec3 heelLittle = sub(s.wrist, mul(s.thumbSide, kHeelHalfWidth * k));
    addSegment(s, heelThumb, heelLittle, pr, kPalm, 0);
    for (int f = 1; f < 5; ++f)
        if (s.jointCount[f]) addSegment(s, f <= 2 ? heelThumb : heelLittle, s.joint[f][0], pr, kPalm, 0);
    int first = s.jointCount[1] ? 1 : 2, last = s.jointCount[4] ? 4 : 3;
    if (s.jointCount[first] && s.jointCount[last]) addSegment(s, s.joint[first][0], s.joint[last][0], pr, kPalm, 0);
    // The fingers: one capsule per segment, root out to the tip.
    for (int f = 0; f < 5; ++f)
        for (int j = 0; j + 1 < s.jointCount[f]; ++j)
            addSegment(s, s.joint[f][j], s.joint[f][j + 1], kFingerRadius[f] * k * (1 - kTaper * j), kThumb + f, j + 1);
    // The palm's surface, between wrist and middle knuckle.
    s.palmCentre = add(mul(add(s.wrist, midKnuckle), 0.5f), mul(s.palmNormal, pr));
    s.valid = s.segmentCount > 0;
}

}  // namespace

void publish(int side, const Raw& raw) {
    if (side < 0 || side > 1) return;
    Slot& s = g_slot[side];
    while (s.writing.test_and_set(std::memory_order_acquire)) {
    }
    uint32_t q = s.seq.load(std::memory_order_relaxed);
    s.seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.data.raw = raw;
    s.data.tick = GetTickCount64();
    std::atomic_thread_fence(std::memory_order_release);
    s.seq.store(q + 2, std::memory_order_release);
    s.writing.clear(std::memory_order_release);

    static std::atomic<int> logged[2];
    if (logged[side].fetch_add(1, std::memory_order_relaxed) == 0) {
        float len[5] = {};
        for (int f = 0; f < 5; ++f)
            for (int k = 0; k + 1 < raw.jointCount[f]; ++k) len[f] += length(sub(raw.joint[f][k + 1], raw.joint[f][k]));
        Vec3 mid = raw.jointCount[2] ? raw.joint[2][0] : raw.wrist;
        LOG("Hand shape: %s hand from the posed skeleton: palm %.1f cm (wrist to middle knuckle), fingers %.1f %.1f %.1f "
            "%.1f %.1f cm (thumb..little), joints %d %d %d %d %d, %s",
            side ? "right" : "left", length(sub(mid, raw.wrist)) * 100, len[0] * 100, len[1] * 100, len[2] * 100,
            len[3] * 100, len[4] * 100, raw.jointCount[0], raw.jointCount[1], raw.jointCount[2], raw.jointCount[3],
            raw.jointCount[4], raw.tracked ? "hand tracking" : "controller");
    }
}

bool latest(int side, Shape& out) {
    out = Shape{};
    Stored st;
    if (side < 0 || side > 1 || !readSlot(side, st) || GetTickCount64() - st.tick >= 300) return false;
    build(side, st, st.raw.gripPos, st.raw.gripRot, out);
    return out.valid;
}

bool get(int side, const armik::Hand& live, Shape& out) {
    out = Shape{};
    Stored st;
    if (side < 0 || side > 1 || !readSlot(side, st) || GetTickCount64() - st.tick >= 300) return false;
    if (live.valid && live.tracked == st.raw.tracked)
        build(side, st, live.position, live.orientation, out);
    else
        build(side, st, st.raw.gripPos, st.raw.gripRot, out);
    return out.valid;
}

namespace {
// Ray (unit dir) against a capsule: distance along the ray to its surface, or -1.
float rayCapsule(Vec3 ro, Vec3 rd, Vec3 pa, Vec3 pb, float r) {
    Vec3 ba = sub(pb, pa), oa = sub(ro, pa);
    float baba = dot(ba, ba), bard = dot(ba, rd), baoa = dot(ba, oa), rdoa = dot(rd, oa), oaoa = dot(oa, oa);
    float a = baba - bard * bard;
    if (a > 1e-10f) {
        float b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba;
        float h = b * b - a * c;
        if (h < 0) return -1;
        float t = (-b - std::sqrt(h)) / a;
        float y = baoa + t * bard;
        if (y > 0 && y < baba) return t;
    }
    // The end caps (spheres).
    float best = -1;
    for (Vec3 c : {pa, pb}) {
        Vec3 oc = sub(ro, c);
        float b = dot(rd, oc), cc = dot(oc, oc) - r * r, h = b * b - cc;
        if (h < 0) continue;
        float t = -b - std::sqrt(h);
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    return best;
}

void finishHit(const Shape& s, const Segment& g, Vec3 point, Hit& hit) {
    Vec3 c = closestOnSegment(point, g.a, g.b);
    hit.side = s.side;
    hit.part = g.part;
    hit.bone = g.bone;
    hit.normal = normalize(sub(point, c));
    hit.point = add(c, mul(hit.normal, g.radius));
    hit.palmSide = dot(hit.normal, s.palmNormal) > 0.2f;
}
}  // namespace

bool raycast(const Shape& s, const Vec3& origin, const Vec3& dir, float maxDistance, Hit& hit, float pad) {
    if (!s.valid) return false;
    int best = -1;
    float bestT = maxDistance;
    for (int i = 0; i < s.segmentCount; ++i) {
        const Segment& g = s.segment[i];
        float t = rayCapsule(origin, dir, g.a, g.b, g.radius + pad);
        if (t >= 0 && t < bestT) {
            bestT = t;
            best = i;
        }
    }
    if (best < 0) return false;
    finishHit(s, s.segment[best], add(origin, mul(dir, bestT)), hit);
    hit.distance = bestT;
    return true;
}

bool closest(const Shape& s, const Vec3& point, Hit& hit) {
    if (!s.valid) return false;
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < s.segmentCount; ++i) {
        const Segment& g = s.segment[i];
        float d = length(sub(point, closestOnSegment(point, g.a, g.b))) - g.radius;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    if (best < 0) return false;
    const Segment& g = s.segment[best];
    Vec3 c = closestOnSegment(point, g.a, g.b);
    Vec3 n = normalize(sub(point, c));
    if (length(n) < 0.5f) n = s.palmNormal;  // right on the axis
    finishHit(s, g, add(c, mul(n, g.radius)), hit);
    hit.distance = bestD;
    return true;
}

bool touches(const Shape& s, const Vec3& centre, float radius, Hit& hit) {
    return closest(s, centre, hit) && hit.distance <= radius;
}

bool tip(const Shape& s, int finger, Vec3& out) {
    if (!s.valid || finger < 0 || finger > 4 || s.jointCount[finger] < 2) return false;
    out = s.joint[finger][s.jointCount[finger] - 1];
    return true;
}

bool curls(const Shape& s, float out[5]) {
    if (!s.valid) return false;
    for (int f = 0; f < 5; ++f) {
        out[f] = 0;
        int n = s.jointCount[f];
        if (n < 2) continue;
        // The bend at each joint: between the way in (from the wrist, or the
        // segment before) and the segment out.
        float bend = 0;
        Vec3 in = f == 0 ? normalize(sub(s.joint[f][0], s.wrist)) : s.forward;
        for (int j = 0; j + 1 < n; ++j) {
            Vec3 seg = normalize(sub(s.joint[f][j + 1], s.joint[f][j]));
            float d = dot(in, seg);
            bend += std::acos(d > 1 ? 1 : d < -1 ? -1 : d);
            in = seg;
        }
        float c = bend / (f == 0 ? kThumbBend : kFistBend);
        out[f] = c < 0 ? 0 : c > 1 ? 1 : c;
    }
    return true;
}

}  // namespace s4vr::handshape
