#include "trail.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "log.h"

namespace s4vr::trail {

namespace {

constexpr float kStep = 0.36f;        // metres between footprints (left, right, left ...)
constexpr float kSide = 0.09f;        // metres each print sits off the route
constexpr float kPrintW = 0.12f;      // metres: a print's quad (the picture has room for its glow)
constexpr float kPrintL = 0.24f;
constexpr float kLift = 0.015f;       // metres above the floor (no flicker against it)
constexpr float kStartAhead = 0.5f;   // metres past you before prints show (none under your own feet)
constexpr float kFadeIn = 0.9f;       // metres over which they fade in from there
constexpr float kFarFade = 30.0f;     // metres ahead where they start fading out
constexpr float kFarEnd = 40.0f;      // and are gone
constexpr float kGoalGap = 0.07f;     // metres each foot of the pair at the goal sits off its centre

std::mutex g_mutex;
std::vector<float> g_points;  // x, y, z ...
float g_goal[5] = {};
std::vector<float> g_along;   // distance along the route at each point (world units)
ULONGLONG g_setAt = 0;
int g_logs = 0;

// Under g_mutex.
void measure() {
    const size_t n = g_points.size() / 3;
    g_along.assign(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        const float* a = &g_points[(i - 1) * 3];
        const float* b = &g_points[i * 3];
        g_along[i] = g_along[i - 1] + std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) +
                                                (b[2] - a[2]) * (b[2] - a[2]));
    }
}

// How far along the route (world units) the point nearest (x, z) on it is.
float along(float x, float z) {
    const size_t n = g_points.size() / 3;
    float best = 1e30f, at = 0;
    for (size_t i = 1; i < n; ++i) {
        const float* a = &g_points[(i - 1) * 3];
        const float* b = &g_points[i * 3];
        const float dx = b[0] - a[0], dz = b[2] - a[2];
        const float len2 = dx * dx + dz * dz;
        float t = len2 > 1e-8f ? ((x - a[0]) * dx + (z - a[2]) * dz) / len2 : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        const float px = a[0] + t * dx - x, pz = a[2] + t * dz - z;
        const float d = px * px + pz * pz;
        if (d < best) {
            best = d;
            at = g_along[i - 1] + t * (g_along[i] - g_along[i - 1]);
        }
    }
    return at;
}

// The point and the floor direction at distance s along the route.
void sample(float s, float p[3], float f[2]) {
    const size_t n = g_points.size() / 3;
    size_t i = 1;
    while (i < n - 1 && g_along[i] < s) ++i;
    const float* a = &g_points[(i - 1) * 3];
    const float* b = &g_points[i * 3];
    const float seg = g_along[i] - g_along[i - 1];
    const float t = seg > 1e-6f ? std::max(0.0f, std::min(1.0f, (s - g_along[i - 1]) / seg)) : 0.0f;
    for (int k = 0; k < 3; ++k) p[k] = a[k] + t * (b[k] - a[k]);
    float dx = b[0] - a[0], dz = b[2] - a[2];
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) {
        dx = g_goal[3];
        dz = g_goal[4];
        len = std::sqrt(dx * dx + dz * dz);
    }
    f[0] = len > 1e-6f ? dx / len : 0.0f;
    f[1] = len > 1e-6f ? dz / len : 1.0f;
}

// A print lying flat on the floor at p, its toes along f (x, z), as a clear
// panel: x = f cross up (the walker's right), y = f, so its face points up.
phone3d::Panel print(const float p[3], const float f[2], bool right, float opacity, float scale) {
    phone3d::Panel q;
    q.show = true;
    q.clear = true;
    q.opacity = opacity;
    q.centre[0] = p[0];
    q.centre[1] = p[1] + kLift * scale;
    q.centre[2] = p[2];
    q.x[0] = -f[1];
    q.x[1] = 0;
    q.x[2] = f[0];
    q.y[0] = f[0];
    q.y[1] = 0;
    q.y[2] = f[1];
    q.width = kPrintW * scale;
    q.height = kPrintL * scale;
    // The picture is a left foot; the right one is its mirror image.
    q.crop[0] = right ? 1.0f : 0.0f;
    q.crop[1] = 0;
    q.crop[2] = right ? 0.0f : 1.0f;
    q.crop[3] = 1;
    return q;
}

}  // namespace

void setPath(const float* points, int count, const float goal[5]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    count = std::max(0, std::min(count, kMaxPoints));
    if (count < 2 || !points || !goal) {
        g_points.clear();
        g_along.clear();
        return;
    }
    g_points.assign(points, points + count * 3);
    std::copy(goal, goal + 5, g_goal);
    measure();
    g_setAt = GetTickCount64();
    if (g_logs++ < 20)
        LOG("Trail: %d points, %.1f units long, goal %.2f,%.2f,%.2f", count, g_along.back(), goal[0], goal[1], goal[2]);
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_points.empty() && g_logs++ < 40) LOG("Trail: cleared");
    g_points.clear();
    g_along.clear();
}

bool active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_points.empty();
}

int build(phone3d::Panel* out, int max, float x, float z, float scale) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_points.size() < 6 || max <= 0) return 0;
    if (scale <= 0) scale = 1;
    const float total = g_along.back();
    const float you = along(x, z);
    const float now = static_cast<float>(GetTickCount64() - g_setAt) / 1000.0f;
    int count = 0;
    // The pair at the goal: where you stand for the action, facing its way; a slow pulse.
    {
        float f[2] = {g_goal[3], g_goal[4]};
        const float len = std::sqrt(f[0] * f[0] + f[1] * f[1]);
        if (len > 1e-6f) {
            f[0] /= len;
            f[1] /= len;
        } else {
            f[0] = 0;
            f[1] = 1;
        }
        const float pulse = 0.8f + 0.2f * std::sin(now * 3.0f);
        for (int side = 0; side < 2 && count < max; ++side) {
            const float off = (side ? kGoalGap : -kGoalGap) * scale;
            const float p[3] = {g_goal[0] + -f[1] * off, g_goal[1], g_goal[2] + f[0] * off};
            out[count++] = print(p, f, side == 1, pulse, scale);
        }
    }
    // Along the route, from just past you to the goal: left, right, left ...
    const float step = kStep * scale, start = you + kStartAhead * scale;
    const float last = total - 0.6f * scale;  // the goal's pair takes over there
    int index = static_cast<int>(std::ceil(start / step));
    for (float s = index * step; s < last && count < max; s += step, ++index) {
        const float ahead = s - you;
        float a = std::min(1.0f, (ahead - kStartAhead * scale) / (kFadeIn * scale));
        if (ahead > kFarFade * scale) a *= std::max(0.0f, 1.0f - (ahead - kFarFade * scale) / ((kFarEnd - kFarFade) * scale));
        if (a <= 0.01f) continue;
        // A soft shimmer running toward the goal.
        const float wave = 0.5f + 0.5f * std::sin((s / scale) * 2.1f - now * 4.0f);
        a *= 0.65f + 0.35f * wave;
        float p[3], f[2];
        sample(s, p, f);
        const bool right = (index & 1) != 0;
        const float off = (right ? kSide : -kSide) * scale;
        p[0] += -f[1] * off;
        p[2] += f[0] * off;
        out[count++] = print(p, f, right, a, scale);
    }
    return count;
}

}  // namespace s4vr::trail
