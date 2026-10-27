#include "collide.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

#include "log.h"

namespace s4vr::collide {

namespace {

constexpr float kStepUp = 0.2f;    // metres above your feet: lower things you step over (thresholds, rugs)
constexpr float kBodyTop = 1.0f;   // metres above your feet: higher wall pieces don't stop your feet
constexpr int kPasses = 3;         // push-out passes (a corner pushes twice)

struct Wall {
    float ax, az, bx, bz, y0, y1;
};
struct Shape {
    int first, count;  // into g_points
    float y0, y1;
    uint32_t flags;
    float minX, minZ, maxX, maxZ;
};

std::mutex g_mutex;  // the scene; the game thread copies it only when a new one came
std::vector<Wall> g_walls;
std::vector<Shape> g_shapes;
std::vector<float> g_points;
std::atomic<uint32_t> g_version{0};
std::atomic<bool> g_on{true};
std::atomic<float> g_radius{0.3f};
std::atomic<float> g_furniture{0.12f};

// The game thread's copy.
std::vector<Wall> t_walls;
std::vector<Shape> t_shapes;
std::vector<float> t_points;
uint32_t t_version = 0;
int g_sceneLogs = 0;

void takeScene() {
    uint32_t v = g_version.load(std::memory_order_acquire);
    if (v == t_version) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    t_walls = g_walls;
    t_shapes = g_shapes;
    t_points = g_points;
    t_version = g_version.load(std::memory_order_relaxed);
}

// Closest point on segment ab to p.
void closest(float px, float pz, float ax, float az, float bx, float bz, float& cx, float& cz) {
    float dx = bx - ax, dz = bz - az, l = dx * dx + dz * dz;
    float t = l < 1e-9f ? 0.0f : std::clamp(((px - ax) * dx + (pz - az) * dz) / l, 0.0f, 1.0f);
    cx = ax + dx * t;
    cz = az + dz * t;
}

bool inside(const Shape& s, float x, float z) {
    bool in = false;
    const float* p = &t_points[s.first * 2];
    for (int i = 0, j = s.count - 1; i < s.count; j = i++) {
        float xi = p[i * 2], zi = p[i * 2 + 1], xj = p[j * 2], zj = p[j * 2 + 1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

}  // namespace

void setScene(const float* data, int count) {
    std::vector<Wall> walls;
    std::vector<Shape> shapes;
    std::vector<float> points;
    int i = 0;
    auto take = [&](float& out) {
        if (i >= count) return false;
        out = data[i++];
        return std::isfinite(out);
    };
    float n = 0;
    bool ok = take(n) && n >= 0 && n < 100000;
    for (int k = 0; ok && k < static_cast<int>(n); ++k) {
        Wall w;
        ok = take(w.ax) && take(w.az) && take(w.bx) && take(w.bz) && take(w.y0) && take(w.y1);
        if (ok) walls.push_back(w);
    }
    float m = 0;
    ok = ok && take(m) && m >= 0 && m < 100000;
    for (int k = 0; ok && k < static_cast<int>(m); ++k) {
        float pc = 0, flags = 0;
        Shape s{};
        ok = take(pc) && pc >= 3 && pc < 256 && take(s.y0) && take(s.y1) && take(flags);
        if (!ok) break;
        s.first = static_cast<int>(points.size() / 2);
        s.count = static_cast<int>(pc);
        s.flags = static_cast<uint32_t>(flags);
        s.minX = s.minZ = 1e30f;
        s.maxX = s.maxZ = -1e30f;
        for (int q = 0; ok && q < s.count; ++q) {
            float x, z;
            ok = take(x) && take(z);
            points.push_back(x);
            points.push_back(z);
            s.minX = std::min(s.minX, x), s.maxX = std::max(s.maxX, x);
            s.minZ = std::min(s.minZ, z), s.maxZ = std::max(s.maxZ, z);
        }
        if (ok) shapes.push_back(s);
    }
    if (!ok) {
        walls.clear();
        shapes.clear();
        points.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_walls.swap(walls);
        g_shapes.swap(shapes);
        g_points.swap(points);
        g_version.fetch_add(1, std::memory_order_release);
    }
    if (g_sceneLogs < 40) {
        ++g_sceneLogs;
        LOG("Collision: scene with %d wall sides and %d object shapes%s", static_cast<int>(g_walls.size()),
            static_cast<int>(g_shapes.size()), ok || count <= 1 ? "" : " (malformed: cleared)");
    }
}

void configure(bool on, float bodyRadius) {
    if (!(bodyRadius > 0.05f && bodyRadius < 1.0f)) bodyRadius = 0.3f;
    bool was = g_on.exchange(on);
    float r = g_radius.exchange(bodyRadius);
    if (was != on || r != bodyRadius)
        LOG("Collision: walls and furniture %s, body radius %.2f m", on ? "stop you" : "off", bodyRadius);
}

void setFurnitureRadius(float metres) {
    if (!(metres > 0.02f && metres < 1.0f)) metres = 0.12f;
    if (g_furniture.exchange(metres) != metres) LOG("Collision: your body keeps %.2f m clear of furniture", metres);
}

float furnitureRadius() { return g_furniture.load(std::memory_order_relaxed); }

bool active(float& bodyRadius) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    takeScene();
    bodyRadius = g_radius.load(std::memory_order_relaxed);
    return !t_walls.empty() || !t_shapes.empty();
}

float pushOut(float& x, float& z, float radius, float objectRadius, float feetY, float maxDepth, int& what) {
    takeScene();
    const float x0 = x, z0 = z;
    const float lo = feetY + kStepUp, hi = feetY + kBodyTop;
    for (int pass = 0; pass < kPasses; ++pass) {
        bool moved = false;
        for (const Wall& w : t_walls) {
            if (w.y1 <= lo || w.y0 >= hi) continue;  // another level, or a wall piece above you
            float cx, cz;
            closest(x, z, w.ax, w.az, w.bx, w.bz, cx, cz);
            float dx = x - cx, dz = z - cz, d2 = dx * dx + dz * dz;
            if (d2 >= radius * radius) continue;
            float d = std::sqrt(d2);
            float nx, nz;
            if (d > 1e-4f) {
                nx = dx / d, nz = dz / d;
            } else {  // on the line: out the way you came
                float sx = w.bx - w.ax, sz = w.bz - w.az, l = std::sqrt(sx * sx + sz * sz);
                if (l < 1e-6f) continue;
                nx = -sz / l, nz = sx / l;
                if ((x0 - cx) * nx + (z0 - cz) * nz < 0) nx = -nx, nz = -nz;
            }
            x = cx + nx * radius;
            z = cz + nz * radius;
            what = 0;
            moved = true;
        }
        for (const Shape& s : t_shapes) {
            if (!(s.flags & 1) || s.y1 <= lo || s.y0 >= hi) continue;
            const float ro = objectRadius;
            if (x < s.minX - ro || x > s.maxX + ro || z < s.minZ - ro || z > s.maxZ + ro) continue;
            const float* p = &t_points[s.first * 2];
            float bx = 0, bz = 0, best = 1e30f;
            for (int i = 0, j = s.count - 1; i < s.count; j = i++) {
                float cx, cz;
                closest(x, z, p[j * 2], p[j * 2 + 1], p[i * 2], p[i * 2 + 1], cx, cz);
                float d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                if (d2 < best) best = d2, bx = cx, bz = cz;
            }
            float d = std::sqrt(best);
            bool in = inside(s, x, z);
            if (!in && d >= ro) continue;
            if (in && d + ro > maxDepth) continue;  // deep inside (teleported in): walk out yourself
            float nx = x - bx, nz = z - bz;
            float l = std::sqrt(nx * nx + nz * nz);
            if (l < 1e-5f) continue;
            nx /= l, nz /= l;
            if (in) nx = -nx, nz = -nz;  // the edge is ahead of the centre: out is the other way
            x = bx + nx * ro;
            z = bz + nz * ro;
            what = 1;
            moved = true;
        }
        if (!moved) break;
    }
    float mx = x - x0, mz = z - z0, m = std::sqrt(mx * mx + mz * mz);
    if (m > maxDepth) {  // something odd (inside a wall's thickness): leave it
        x = x0, z = z0;
        return 0;
    }
    return m;
}


// ---------------------------------------------------------------------------
// Your Sim's hands against walls and furniture (phase 3).

namespace {

std::atomic<bool> g_handsOn{true};

struct HandStop {
    float p[3] = {0, 0, 0};  // where the drawn palm is (world)
    ULONGLONG tick = 0;
    bool known = false;
    int logs = 0;
    ULONGLONG logTick = 0;
    unsigned armTries = 0;  // solves caught since the way from the shoulder last helped
};
HandStop g_handStop[2];

struct Near {
    std::vector<const Wall*> walls;
    std::vector<const Shape*> shapes;
    const float* points = nullptr;
};

bool insideOf(const Shape& s, const float* points, float x, float z) {
    bool in = false;
    const float* p = &points[s.first * 2];
    for (int i = 0, j = s.count - 1; i < s.count; j = i++) {
        float xi = p[i * 2], zi = p[i * 2 + 1], xj = p[j * 2], zj = p[j * 2 + 1];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

// A ball of radius r at p pushed out of the nearby walls (upright strips from
// y0 to y1) and object prisms (footprint from y0 to y1). `ref` is on the side
// the ball came from: a wall pushes toward it, even if p went past the wall's
// face line. Returns whether it moved.
bool ballOut(float p[3], float r, const float ref[3], const Near& around) {
    bool moved = false;
    for (const Wall* w : around.walls) {
        float cx, cz;
        closest(p[0], p[2], w->ax, w->az, w->bx, w->bz, cx, cz);
        float cy = std::clamp(p[1], w->y0, w->y1);
        float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz, d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= r * r) continue;
        float sx = w->bx - w->ax, sz = w->bz - w->az, l = std::sqrt(sx * sx + sz * sz);
        float nx = l > 1e-6f ? -sz / l : 0, nz = l > 1e-6f ? sx / l : 0;  // across the wall
        if ((ref[0] - cx) * nx + (ref[2] - cz) * nz < 0) nx = -nx, nz = -nz;
        float across = dx * nx + dz * nz, d = std::sqrt(d2);
        if (across >= 0 && d > 1e-4f) {  // your side of it (its face, its top or an end): straight out
            p[0] = cx + dx / d * r, p[1] = cy + dy / d * r, p[2] = cz + dz / d * r;
        } else {  // past its face line: back out to your side
            p[0] += nx * (r - across), p[2] += nz * (r - across);
        }
        moved = true;
    }
    for (const Shape* s : around.shapes) {
        if (p[1] > s->y1 + r || p[1] < s->y0 - r || p[0] < s->minX - r || p[0] > s->maxX + r ||
            p[2] < s->minZ - r || p[2] > s->maxZ + r)
            continue;
        const float* q = &around.points[s->first * 2];
        float ex = 0, ez = 0, edge = 1e30f;
        for (int i = 0, j = s->count - 1; i < s->count; j = i++) {
            float cx, cz;
            closest(p[0], p[2], q[j * 2], q[j * 2 + 1], q[i * 2], q[i * 2 + 1], cx, cz);
            float d2 = (p[0] - cx) * (p[0] - cx) + (p[2] - cz) * (p[2] - cz);
            if (d2 < edge) edge = d2, ex = cx, ez = cz;
        }
        edge = std::sqrt(edge);
        const bool in = insideOf(*s, around.points, p[0], p[2]);
        if (in && p[1] >= s->y0 && p[1] <= s->y1) {  // inside it: out the nearer way, its top or its side
            float up = s->y1 + r - p[1], side = edge + r;
            if (up <= side || edge < 1e-5f) {
                p[1] = s->y1 + r;
            } else {
                p[0] += (ex - p[0]) / edge * side, p[2] += (ez - p[2]) / edge * side;
            }
            moved = true;
            continue;
        }
        float hx = in ? p[0] : ex, hz = in ? p[2] : ez;
        float cy = std::clamp(p[1], s->y0, s->y1);
        float dx = p[0] - hx, dy = p[1] - cy, dz = p[2] - hz, d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= r * r || d2 < 1e-10f) continue;
        float d = std::sqrt(d2);
        p[0] = hx + dx / d * r, p[1] = cy + dy / d * r, p[2] = hz + dz / d * r;
        moved = true;
    }
    return moved;
}

}  // namespace

void configureHands(bool on) {
    if (g_handsOn.exchange(on) != on) LOG("Collision: your hands %s walls and furniture", on ? "stop at" : "go through");
}

float stopHand(int side, const float target[3], const float* offsets, const float* radii, int count, float snap,
               const float* shoulder, float out[3]) {
    out[0] = target[0], out[1] = target[1], out[2] = target[2];
    if (side < 0 || side > 1 || count <= 0) return 0;
    HandStop& h = g_handStop[side];
    // This thread's own copy of the scene (taken again when a new one came).
    thread_local std::vector<Wall> walls;
    thread_local std::vector<Shape> shapes;
    thread_local std::vector<float> points;
    thread_local uint32_t version = 0;
    uint32_t v = g_version.load(std::memory_order_acquire);
    if (v != version) {
        std::lock_guard<std::mutex> lock(g_mutex);
        walls = g_walls;
        shapes = g_shapes;
        points = g_points;
        version = g_version.load(std::memory_order_relaxed);
    }
    ULONGLONG now = GetTickCount64();
    const float tx = target[0], ty = target[1], tz = target[2];
    if (!g_handsOn.load(std::memory_order_relaxed) || (walls.empty() && shapes.empty()) ||
        !std::isfinite(tx + ty + tz)) {
        h.known = false;
        return 0;
    }
    float gx = tx - h.p[0], gy = ty - h.p[1], gz = tz - h.p[2];
    if (!h.known || now - h.tick > 300 || gx * gx + gy * gy + gz * gz > snap * snap) {
        h.p[0] = tx, h.p[1] = ty, h.p[2] = tz;  // start over at your hand
        h.known = true;
    }
    h.tick = now;
    // What's near the way from the drawn hand to yours, the whole hand.
    float reach = 0, thinnest = 1e30f, centre[3] = {0, 0, 0};
    for (int k = 0; k < count; ++k) {
        const float* o = offsets + k * 3;
        for (int c = 0; c < 3; ++c) centre[c] += o[c] / count;
        reach = std::max(reach, std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + radii[k]);
        thinnest = std::min(thinnest, radii[k]);
    }
    if (!(thinnest > 1e-3f) || !std::isfinite(reach)) return 0;
    const float m = reach + thinnest;
    const bool arm = shoulder && std::isfinite(shoulder[0] + shoulder[1] + shoulder[2]);
    float x0 = std::min(h.p[0], tx) - m, x1 = std::max(h.p[0], tx) + m;
    float y0 = std::min(h.p[1], ty) - m, y1 = std::max(h.p[1], ty) + m;
    float z0 = std::min(h.p[2], tz) - m, z1 = std::max(h.p[2], tz) + m;
    if (arm) {
        x0 = std::min(x0, shoulder[0] - m), x1 = std::max(x1, shoulder[0] + m);
        y0 = std::min(y0, shoulder[1] - m), y1 = std::max(y1, shoulder[1] + m);
        z0 = std::min(z0, shoulder[2] - m), z1 = std::max(z1, shoulder[2] + m);
    }
    thread_local Near around;
    around.walls.clear();
    around.shapes.clear();
    around.points = points.data();
    for (const Wall& w : walls)
        if (w.y1 >= y0 && w.y0 <= y1 && std::max(w.ax, w.bx) >= x0 && std::min(w.ax, w.bx) <= x1 &&
            std::max(w.az, w.bz) >= z0 && std::min(w.az, w.bz) <= z1)
            around.walls.push_back(&w);
    for (const Shape& s : shapes)  // flag 4: a footprint whose model's box (flag 2) is what hands meet
        if (!(s.flags & 4) && s.y1 >= y0 && s.y0 <= y1 && s.maxX >= x0 && s.minX <= x1 && s.maxZ >= z0 &&
            s.minZ <= z1)
            around.shapes.push_back(&s);
    if (around.walls.empty() && around.shapes.empty()) {
        h.p[0] = tx, h.p[1] = ty, h.p[2] = tz;
        return 0;
    }
    // Toward your hand in steps under its thinnest part, so no part gets
    // past a wall's face; each step slides along what it touches. A wall
    // pushes each part back toward the side the middle of the hand is on (a
    // finger turned into the wall comes back out the near side).
    const float step = std::max(thinnest * 0.8f, 0.002f);
    auto sweep = [&](float p[3], int steps) {
        for (int i = 0; i < steps; ++i) {
            float dx = tx - p[0], dy = ty - p[1], dz = tz - p[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
            float from[3] = {p[0], p[1], p[2]};
            if (d > step) dx *= step / d, dy *= step / d, dz *= step / d;
            p[0] += dx, p[1] += dy, p[2] += dz;
            const float middle[3] = {from[0] + centre[0], from[1] + centre[1], from[2] + centre[2]};
            for (int pass = 0; pass < 2; ++pass) {
                bool moved = false;
                for (int k = 0; k < count; ++k) {
                    const float* o = offsets + k * 3;
                    float ball[3] = {p[0] + o[0], p[1] + o[1], p[2] + o[2]};
                    float was[3] = {ball[0], ball[1], ball[2]};
                    if (ballOut(ball, radii[k], middle, around)) {
                        p[0] += ball[0] - was[0], p[1] += ball[1] - was[1], p[2] += ball[2] - was[2];
                        moved = true;
                    }
                }
                if (!moved) break;
            }
            float mx = p[0] - from[0], my = p[1] - from[1], mz = p[2] - from[2];
            if (d <= step || mx * mx + my * my + mz * mz < 1e-8f) break;  // there, or held against something
        }
        float ax = tx - p[0], ay = ty - p[1], az = tz - p[2];
        return std::sqrt(ax * ax + ay * ay + az * az);
    };
    float apart = sweep(h.p, 120);
    // Caught (on a door jamb you walked past, the side of the steps): your
    // arm brings it in from your shoulder instead, if that's nearer.
    // Tried every few solves while it stays caught (it costs a long sweep).
    if (arm && apart > 0.04f && ++h.armTries % 4 == 1) {
        float q[3] = {shoulder[0] - centre[0], shoulder[1] - centre[1], shoulder[2] - centre[2]};
        float other = sweep(q, 200);
        if (other < apart - 0.02f) {
            h.armTries = 0;
            h.p[0] = q[0], h.p[1] = q[1], h.p[2] = q[2];
            apart = other;
            if (h.logs < 20 && now - h.logTick > 2000) {
                ++h.logs;
                h.logTick = now;
                LOG("Collision: your %s hand was caught; brought round from your shoulder (%.0f mm from your hand)",
                    side ? "right" : "left", apart * 1000);
            }
        }
    }
    out[0] = h.p[0], out[1] = h.p[1], out[2] = h.p[2];
    if (apart > 0.03f && h.logs < 20 && now - h.logTick > 2000) {
        ++h.logs;
        h.logTick = now;
        LOG("Collision: your %s hand held at a wall or object, your real hand %.0f mm further", side ? "right" : "left",
            apart * 1000);
    }
    return apart;
}

}  // namespace s4vr::collide
