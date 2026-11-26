#include "teleport.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include "collide.h"
#include "eyelids.h"
#include "log.h"
#include "player.h"

namespace s4vr::teleport {

namespace {

constexpr int kArcPoints = 28;
constexpr float kGravity = 9.8f;       // m/s^2 of the arc
constexpr float kArcTime = 3.0f;       // seconds an arc is followed at most
constexpr float kAskMoved = 0.08f;     // metres the spot moves before Python is asked again
constexpr ULONGLONG kAskEvery = 150;   // ms between asks while it stays
constexpr ULONGLONG kAnswerFresh = 600;// ms an answer stays good for a release
constexpr float kMatch = 0.35f;        // metres an answer may be off the spot you let go at
constexpr ULONGLONG kBlinkClose = 120; // ms the eyes take to shut before the jump
constexpr float kDotSize = 0.035f;     // metres: an arc dot
constexpr float kMarkerGap = 0.07f;    // metres each foot of the marker sits off its centre
constexpr float kMarkerW = 0.12f, kMarkerL = 0.24f;
constexpr float kLift = 0.015f;

std::mutex g_mutex;
Options g_opt;

// The pointer (under g_mutex: the render thread reads it).
bool g_shown = false;
float g_arc[kArcPoints][3];
int g_arcCount = 0;
bool g_hit = false;       // the arc meets the floor within reach
float g_spot[3] = {};     // where (x, y from Python, z)
bool g_valid = false;     // Python and the collision scene say you may go there
float g_from[2] = {};     // the player's feet, for the marker's facing
ULONGLONG g_shownAt = 0;

// Python's check.
uint32_t g_seq = 0, g_asked = 0;
bool g_pending = false;
float g_askSpot[2] = {};
ULONGLONG g_askAt = 0;
bool g_answered = false;
bool g_answerOk = false;
float g_answerY = 0;
float g_answerSpot[2] = {};
ULONGLONG g_answerAt = 0;

// Game thread.
bool g_wasHeld = false;
ULONGLONG g_jumpAt = 0;     // when a blinking jump moves you (0: none waiting)
float g_jump[3] = {};
ULONGLONG g_openAt = 0;     // when the eyes open again (0: not shut by us)
int g_logs = 0;

}  // namespace

void configure(const Options& options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt = options;
    g_opt.maxDistance = std::clamp(g_opt.maxDistance, 2.0f, 30.0f);
    g_opt.blinkSeconds = std::clamp(g_opt.blinkSeconds, 0.05f, 1.5f);
}

bool active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_shown;
}

void stop() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_shown = false;
        g_pending = false;
        g_answered = false;
        g_valid = false;
    }
    g_wasHeld = false;
    g_jumpAt = 0;
    if (g_openAt) {
        eyelids::setCovered(false);
        g_openAt = 0;
    }
}

void update(bool held, const float origin[3], const float dir[3], float scale, const float feet[3]) {
    const ULONGLONG now = GetTickCount64();
    if (scale <= 0) scale = 1;
    // A jump waiting for the eyes to shut, and the eyes opening again.
    if (g_jumpAt && now >= g_jumpAt) {
        player::teleportTo(g_jump[0], g_jump[1], g_jump[2]);
        g_jumpAt = 0;
    }
    if (g_openAt && now >= g_openAt) {
        eyelids::setCovered(false);
        g_openAt = 0;
    }
    Options opt;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        opt = g_opt;
    }
    if (!held) {
        bool go = false;
        float to[3] = {}, from[2] = {};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            from[0] = g_from[0];
            from[1] = g_from[1];
            // Let go: go to the spot if it is open, was checked lately and Python's answer was for this spot.
            if (g_wasHeld && g_shown && g_valid && g_answered && g_answerOk && now - g_answerAt < kAnswerFresh &&
                std::hypot(g_answerSpot[0] - g_spot[0], g_answerSpot[1] - g_spot[2]) < kMatch * scale) {
                go = true;
                to[0] = g_spot[0];
                to[1] = g_answerY;
                to[2] = g_spot[2];
            }
            g_shown = false;
            g_pending = false;
            g_valid = false;
        }
        g_wasHeld = false;
        if (go) {
            if (g_logs++ < 40)
                LOG("Teleport: to %.2f %.2f %.2f (%.1f m)", to[0], to[1], to[2],
                    std::hypot(to[0] - from[0], to[2] - from[1]) / scale);
            if (opt.blink) {
                eyelids::setCovered(true);
                g_jumpAt = now + kBlinkClose;
                g_openAt = now + kBlinkClose + static_cast<ULONGLONG>(opt.blinkSeconds * 1000.0f);
                std::copy(to, to + 3, g_jump);
            } else {
                player::teleportTo(to[0], to[1], to[2]);
            }
        }
        return;
    }
    if (g_jumpAt) return;  // a jump is under way
    g_wasHeld = true;

    // The arc (or laser) from the hand down to the floor; stops at the distance limit. The floor is taken at your
    // feet's height until Python has answered for a spot nearby: a higher or lower floor (a step, a stair's top)
    // then sets the height the arc is cut at, so it lands on that floor.
    float planeY = feet[1];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_answered && g_answerOk && now - g_answerAt < 2000)
            planeY = std::clamp(g_answerY, feet[1] - 3.0f * scale, feet[1] + 3.0f * scale);
    }
    const float maxD = opt.maxDistance * scale;
    float pts[kArcPoints][3];
    int n = 0;
    bool hit = false;
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-4f) return;
    const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
    float spot[2] = {origin[0], origin[2]};
    if (opt.style == 1) {
        // A straight laser: to the floor, or as far as allowed.
        float t = d[1] < -0.02f ? (planeY - origin[1]) / d[1] : 1e9f;
        const float hd = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        const float tMax = hd > 1e-4f ? maxD / hd : 1e9f;
        hit = t > 0 && t <= tMax;
        if (!(t > 0)) t = 0;
        t = std::min({t, tMax, 100.0f * scale});
        for (int i = 0; i < kArcPoints; ++i) {
            const float k = t * (i + 1) / kArcPoints;
            pts[n][0] = origin[0] + d[0] * k;
            pts[n][1] = origin[1] + d[1] * k;
            pts[n][2] = origin[2] + d[2] * k;
            ++n;
        }
    } else {
        // A throw: fast enough to reach maxDistance at the best angle.
        const float g = kGravity * scale;
        const float v = std::sqrt(g * maxD);
        const float dt = kArcTime / 120.0f;
        float px = origin[0], py = origin[1], pz = origin[2];
        float vx = d[0] * v, vy = d[1] * v, vz = d[2] * v;
        float kept[121][3];
        int k = 0;
        for (int i = 0; i < 120; ++i) {
            const float nx = px + vx * dt, ny = py + vy * dt, nz = pz + vz * dt;
            vy -= g * dt;
            const bool floorHit = ny <= planeY && vy < 0;
            const bool tooFar = std::hypot(nx - origin[0], nz - origin[2]) > maxD;
            if (floorHit) {
                const float f = (py - planeY) / std::max(1e-6f, py - ny);
                px = px + (nx - px) * std::clamp(f, 0.0f, 1.0f);
                pz = pz + (nz - pz) * std::clamp(f, 0.0f, 1.0f);
                py = planeY;
                kept[k][0] = px, kept[k][1] = py, kept[k][2] = pz;
                ++k;
                hit = true;
                break;
            }
            px = nx, py = ny, pz = nz;
            kept[k][0] = px, kept[k][1] = py, kept[k][2] = pz;
            ++k;
            if (tooFar) break;
        }
        for (int i = 0; i < kArcPoints && k; ++i) {
            const int idx = std::min(k - 1, static_cast<int>((i + 1) * static_cast<long>(k) / kArcPoints));
            std::copy(kept[idx], kept[idx] + 3, pts[n]);
            ++n;
        }
    }
    if (n == 0) return;
    spot[0] = pts[n - 1][0];
    spot[1] = pts[n - 1][2];
    // Too far: the distance limit ends the arc, not the floor. Short of the floor there is no spot.
    const bool reach = hit && std::hypot(spot[0] - feet[0], spot[1] - feet[2]) <= maxD * 1.02f;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_shown) g_shownAt = now;
    g_shown = true;
    g_arcCount = n;
    std::copy(&pts[0][0], &pts[0][0] + n * 3, &g_arc[0][0]);
    g_hit = reach;
    g_from[0] = feet[0];
    g_from[1] = feet[2];
    g_valid = false;
    if (!reach) {
        g_pending = false;
        return;
    }
    g_spot[0] = spot[0];
    g_spot[2] = spot[1];
    // Ask Python when the spot has moved or the last ask is old.
    if (!g_pending || std::hypot(spot[0] - g_askSpot[0], spot[1] - g_askSpot[1]) > kAskMoved * scale) {
        if (now - g_askAt >= kAskEvery || !g_pending) {
            g_seq++;
            g_asked = g_seq;
            g_pending = true;
            g_askSpot[0] = spot[0];
            g_askSpot[1] = spot[1];
            g_askAt = now;
        }
    }
    // The answer so far, for a spot near this one.
    if (g_answered && now - g_answerAt < 2000 &&
        std::hypot(g_answerSpot[0] - spot[0], g_answerSpot[1] - spot[1]) < kMatch * scale && g_answerOk) {
        float x = spot[0], z = spot[1];
        int what = 0;
        float br = 0;
        bool free = true;
        if (collide::active(br)) {
            // Furniture and walls in the collision scene: the spot must be clear of them.
            free = collide::pushOut(x, z, br * scale, collide::furnitureRadius() * scale, g_answerY, 1e6f, what) <= 0.0f;
        }
        g_spot[1] = g_answerY;
        g_valid = free;
    }
}

bool query(uint32_t& seq, float& x, float& z) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_shown || !g_pending || g_asked == 0) return false;
    static uint32_t given = 0;
    if (given == g_asked) return false;
    given = g_asked;
    seq = g_asked;
    x = g_askSpot[0];
    z = g_askSpot[1];
    return true;
}

void answer(uint32_t seq, bool ok, float y) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (seq != g_asked) return;  // an old spot
    g_answered = true;
    g_answerOk = ok;
    g_answerY = y;
    g_answerSpot[0] = g_askSpot[0];
    g_answerSpot[1] = g_askSpot[1];
    g_answerAt = GetTickCount64();
}

namespace {
// A flat footprint at p facing f (see trail.cpp).
phone3d::Panel foot(const float p[3], const float f[2], bool right, float scale) {
    phone3d::Panel q;
    q.show = true;
    q.clear = true;
    q.opacity = 0.9f;
    q.centre[0] = p[0];
    q.centre[1] = p[1] + kLift * scale;
    q.centre[2] = p[2];
    q.x[0] = -f[1];
    q.x[2] = f[0];
    q.y[0] = f[0];
    q.y[2] = f[1];
    q.width = kMarkerW * scale;
    q.height = kMarkerL * scale;
    q.crop[0] = right ? 1.0f : 0.0f;
    q.crop[2] = right ? 0.0f : 1.0f;
    q.crop[3] = 1;
    return q;
}
}  // namespace

int build(phone3d::Panel* out, int max, const float cam[3], float scale) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_shown || g_arcCount < 2 || max <= 0) return 0;
    if (scale <= 0) scale = 1;
    int count = 0;
    // Arc dots turned to face you, fainter while the spot is closed.
    const float base = g_valid ? 0.9f : 0.35f;
    for (int i = 0; i < g_arcCount - 1 && count < max; ++i) {
        const float* p = g_arc[i];
        float tx = cam[0] - p[0], ty = cam[1] - p[1], tz = cam[2] - p[2];
        const float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (tl < 0.3f * scale) continue;
        tx /= tl, ty /= tl, tz /= tl;
        // x = up cross toward-camera, y = toward-camera cross x.
        float rx = tz, rz = -tx;  // up x toward-camera
        float rl = std::sqrt(rx * rx + rz * rz);
        if (rl < 1e-4f) continue;
        rx /= rl, rz /= rl;
        const float ux = ty * rz, uy = tz * rx - tx * rz, uz = -ty * rx;
        phone3d::Panel q;
        q.show = true;
        q.clear = true;
        q.opacity = base;
        q.centre[0] = p[0], q.centre[1] = p[1], q.centre[2] = p[2];
        q.x[0] = rx, q.x[1] = 0, q.x[2] = rz;
        q.y[0] = ux, q.y[1] = uy, q.y[2] = uz;
        q.width = q.height = kDotSize * scale * (0.7f + 0.3f * i / g_arcCount);
        q.crop[0] = 0, q.crop[1] = 0, q.crop[2] = 1, q.crop[3] = 1;
        out[count++] = q;
    }
    // The marker: a pair of footprints at the spot, facing the way you go there.
    if (g_opt.marker && g_valid && g_hit) {
        float f[2] = {g_spot[0] - g_from[0], g_spot[2] - g_from[1]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
        if (fl > 1e-4f) {
            f[0] /= fl;
            f[1] /= fl;
        } else {
            f[0] = 0;
            f[1] = 1;
        }
        for (int side = 0; side < 2 && count < max; ++side) {
            const float off = (side ? kMarkerGap : -kMarkerGap) * scale;
            const float p[3] = {g_spot[0] - f[1] * off, g_spot[1], g_spot[2] + f[0] * off};
            out[count++] = foot(p, f, side == 1, scale);
        }
    }
    return count;
}

}  // namespace s4vr::teleport
