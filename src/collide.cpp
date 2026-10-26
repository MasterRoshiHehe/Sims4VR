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

bool active(float& bodyRadius) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    takeScene();
    bodyRadius = g_radius.load(std::memory_order_relaxed);
    return !t_walls.empty() || !t_shapes.empty();
}

float pushOut(float& x, float& z, float radius, float feetY, float maxDepth, int& what) {
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
            if (x < s.minX - radius || x > s.maxX + radius || z < s.minZ - radius || z > s.maxZ + radius) continue;
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
            if (!in && d >= radius) continue;
            if (in && d + radius > maxDepth) continue;  // deep inside (teleported in): walk out yourself
            float nx = x - bx, nz = z - bz;
            float l = std::sqrt(nx * nx + nz * nz);
            if (l < 1e-5f) continue;
            nx /= l, nz /= l;
            if (in) nx = -nx, nz = -nz;  // the edge is ahead of the centre: out is the other way
            x = bx + nx * radius;
            z = bz + nz * radius;
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

}  // namespace s4vr::collide
