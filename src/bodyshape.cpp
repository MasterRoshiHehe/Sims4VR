#include "bodyshape.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace s4vr::bodyshape {

const char* const kRegionNames[kRegions] = {"torso", "head", "hair", "upper arm", "forearm", "thigh", "calf", "foot", "other"};

namespace {

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
Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = sub(b, a);
    float l2 = dot(ab, ab);
    float t = l2 > 1e-12f ? dot(sub(p, a), ab) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return add(a, mul(ab, t));
}

struct Slot {
    std::atomic<uint32_t> seq{0};
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    Shape data{};
};
Slot g_slot[kMaxSims];

bool readSlot(int i, Shape& out) {
    const Slot& s = g_slot[i];
    for (int tries = 0; tries < 16; ++tries) {
        uint32_t before = s.seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        std::memcpy(&out, &s.data, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) == before) return before != 0 && out.valid;
    }
    return false;
}

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

void finish(const Shape& s, int i, Vec3 point, Hit& hit) {
    const Segment& g = s.segment[i];
    Vec3 c = closestOnSegment(point, g.a, g.b);
    hit.actor = s.actor;
    hit.player = s.player;
    hit.region = g.region;
    hit.side = g.side;
    hit.joint = g.joint;
    hit.segment = i;
    hit.normal = normalize(sub(point, c));
    hit.point = add(c, mul(hit.normal, g.radius));
}

bool nearBounds(const Shape& s, Vec3 p, float reach) {
    return length(sub(p, s.centre)) <= s.boundRadius + reach;
}

bool closestIn(const Shape& s, const Vec3& point, Hit& hit, bool torsoOnly) {
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < s.segmentCount; ++i) {
        const Segment& g = s.segment[i];
        if (torsoOnly && g.region != kTorso) continue;
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
    if (length(n) < 0.5f) n = s.forward;
    finish(s, best, add(c, mul(n, g.radius)), hit);
    hit.distance = bestD;
    return true;
}

}  // namespace

void publish(const Shape& shape) {
    int i = -1;
    ULONGLONG now = GetTickCount64();
    if (shape.player) {
        i = 0;
    } else {
        // The slot this actor had, else one that is free or stale (never slot 0).
        for (int k = 1; k < kMaxSims && i < 0; ++k)
            if (g_slot[k].data.actor == shape.actor) i = k;
        for (int k = 1; k < kMaxSims && i < 0; ++k)
            if (!g_slot[k].data.valid || now - g_slot[k].data.tick > 1000) i = k;
        if (i < 0) return;
    }
    Slot& s = g_slot[i];
    if (s.writing.test_and_set(std::memory_order_acquire)) return;  // another job thread is on it
    uint32_t q = s.seq.load(std::memory_order_relaxed);
    s.seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.data = shape;
    s.data.tick = now;
    std::atomic_thread_fence(std::memory_order_release);
    s.seq.store(q + 2, std::memory_order_release);
    s.writing.clear(std::memory_order_release);
}

int count() {
    int n = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kMaxSims; ++i)
        if (g_slot[i].data.valid && now - g_slot[i].data.tick < 300) ++n;
    return n;
}

bool get(int index, Shape& out) {
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (int i = 0; i < kMaxSims; ++i) {
        if (!g_slot[i].data.valid || now - g_slot[i].data.tick >= 300) continue;
        if (n++ != index) continue;
        return readSlot(i, out) && now - out.tick < 300;
    }
    return false;
}

bool forActor(const void* actor, Shape& out) {
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kMaxSims; ++i)
        if (g_slot[i].data.actor == actor && readSlot(i, out)) return now - out.tick < 300;
    return false;
}

bool forPlayer(Shape& out) { return readSlot(0, out) && GetTickCount64() - out.tick < 300; }

bool raycast(const Shape& s, const Vec3& origin, const Vec3& dir, float maxDistance, Hit& hit, float pad) {
    if (!s.valid) return false;
    // Skip a Sim whose bounds the ray misses.
    Vec3 oc = sub(s.centre, origin);
    float along = dot(oc, dir);
    float r = s.boundRadius + pad;
    if (along < -r || along > maxDistance + r) return false;
    Vec3 off = sub(oc, mul(dir, along));
    if (dot(off, off) > r * r) return false;
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
    finish(s, best, add(origin, mul(dir, bestT)), hit);
    hit.distance = bestT;
    return true;
}

bool closest(const Shape& s, const Vec3& point, Hit& hit) { return s.valid && closestIn(s, point, hit, false); }

bool touches(const Shape& s, const Vec3& centre, float radius, Hit& hit) {
    return s.valid && nearBounds(s, centre, radius) && closestIn(s, centre, hit, false) && hit.distance <= radius;
}

bool raycastAll(const Vec3& origin, const Vec3& dir, float maxDistance, Hit& hit, bool includePlayer, float pad) {
    bool any = false;
    float limit = maxDistance;
    for (int i = includePlayer ? 0 : 1; i < kMaxSims; ++i) {
        static thread_local Shape s;
        if (!readSlot(i, s) || GetTickCount64() - s.tick >= 300) continue;
        Hit h;
        if (raycast(s, origin, dir, limit, h, pad)) {
            hit = h;
            limit = h.distance;
            any = true;
        }
    }
    return any;
}

bool closestAll(const Vec3& point, Hit& hit, bool includePlayer) {
    bool any = false;
    for (int i = includePlayer ? 0 : 1; i < kMaxSims; ++i) {
        static thread_local Shape s;
        if (!readSlot(i, s) || GetTickCount64() - s.tick >= 300) continue;
        Hit h;
        if (closestIn(s, point, h, false) && (!any || h.distance < hit.distance)) {
            hit = h;
            any = true;
        }
    }
    return any;
}

bool touchesAll(const Vec3& centre, float radius, Hit& hit, bool includePlayer) {
    return closestAll(centre, hit, includePlayer) && hit.distance <= radius;
}

bool bellyTouch(const Vec3& centre, float radius, Hit& hit) {
    static thread_local Shape s;
    return forPlayer(s) && closestIn(s, centre, hit, true) && hit.distance <= radius;
}

}  // namespace s4vr::bodyshape
