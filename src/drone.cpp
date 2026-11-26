#include "drone.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace s4vr::drone {

namespace {
std::mutex g_mutex;
bool g_on = false;
float g_pos[3] = {};
float g_vel[3] = {};  // metres per second
float g_lowest = -1e9f, g_highest = 1e9f;
float g_speed = 6.0f, g_liftSpeed = 3.0f, g_smooth = 0.25f, g_boost = 2.0f;

struct In {
    float x = 0, z = 0, strength = 0, lift = 0;
} g_in;
bool g_land = false;
bool g_landWas = false;
std::atomic<bool> g_active{false};
}  // namespace

void start(const float position[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < 3; ++i) g_pos[i] = position[i], g_vel[i] = 0;
    g_in = In{};
    g_land = g_landWas = false;
    g_on = true;
    g_active.store(true);
}

void stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on = false;
    g_land = g_landWas = false;
    g_active.store(false);
}

bool active() { return g_active.load(); }

void setLimits(float lowest, float highest) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lowest = lowest;
    g_highest = highest < lowest ? lowest : highest;
}

void input(float x, float z, float strength, float lift, bool land) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_in.x = x;
    g_in.z = z;
    g_in.strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
    g_in.lift = lift < -1 ? -1 : lift > 1 ? 1 : lift;
    if (land && !g_landWas) g_land = true;
    g_landWas = land;
}

bool update(float dt, float scale, float out[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_on) return false;
    if (dt > 0) {
        if (dt > 0.1f) dt = 0.1f;
        constexpr float kDead = 0.15f;
        float s = g_in.strength;
        s = s < kDead ? 0 : (s - kDead) / (1 - kDead);
        const float boost = g_in.strength > 0.97f ? g_boost : 1.0f;
        float lift = std::fabs(g_in.lift) < kDead ? 0 : (std::fabs(g_in.lift) - kDead) / (1 - kDead) * (g_in.lift < 0 ? -1.f : 1.f);
        float tx = g_in.x * s * g_speed * boost, tz = g_in.z * s * g_speed * boost, ty = lift * g_liftSpeed;
        const float k = g_smooth > 0.01f ? 1.0f - std::exp(-dt / g_smooth) : 1.0f;
        g_vel[0] += (tx - g_vel[0]) * k;
        g_vel[1] += (ty - g_vel[1]) * k;
        g_vel[2] += (tz - g_vel[2]) * k;
        for (int i = 0; i < 3; ++i) g_pos[i] += g_vel[i] * dt * scale;
        if (g_pos[1] < g_lowest) g_pos[1] = g_lowest, g_vel[1] = g_vel[1] < 0 ? 0 : g_vel[1];
        if (g_pos[1] > g_highest) g_pos[1] = g_highest, g_vel[1] = g_vel[1] > 0 ? 0 : g_vel[1];
    }
    for (int i = 0; i < 3; ++i) out[i] = g_pos[i];
    return true;
}

bool pose(float out[5]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_on) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_pos[i];
    out[3] = std::sqrt(g_vel[0] * g_vel[0] + g_vel[1] * g_vel[1] + g_vel[2] * g_vel[2]);
    out[4] = g_land ? 1.0f : 0.0f;
    return true;
}

void configure(float speed, float liftSpeed, float smoothing, float boost) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (speed > 0.1f && speed < 60) g_speed = speed;
    if (liftSpeed > 0.1f && liftSpeed < 30) g_liftSpeed = liftSpeed;
    if (smoothing >= 0 && smoothing < 3) g_smooth = smoothing;
    if (boost >= 1 && boost < 10) g_boost = boost;
}

}  // namespace s4vr::drone
