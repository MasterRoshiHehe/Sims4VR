#include "dance.h"

#include <windows.h>
#include <cmath>
#include <mutex>

#include "log.h"

namespace s4vr::dance {

namespace {

constexpr int kRate = 30;               // samples a second
constexpr int kWindow = 4 * kRate;      // 4 s
constexpr ULONGLONG kStepMs = 1000 / kRate;
constexpr int kMinLag = 8, kMaxLag = 45;  // 0.27 .. 1.5 s
constexpr float kWalkMetres = 1.0f;     // head over the floor in 2 s: walking
constexpr ULONGLONG kStickQuietMs = 1000;

struct Sample {
    float headY, headX, headZ;
    float hand[2][3];
    bool handOk[2];
};
Sample g_samples[kWindow];
int g_count = 0, g_next = 0;
ULONGLONG g_lastAt = 0, g_stickAt = 0;

std::mutex g_mutex;
State g_state;
bool g_loggedOn = false;
int g_logs = 0;

const Sample& at(int i) {  // i = 0 the oldest
    return g_samples[(g_next - g_count + i + kWindow) % kWindow];
}

void clear() {
    g_count = g_next = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = State{};
}

void analyse() {
    const float dt = 1.0f / kRate;
    float vy[kWindow] = {}, s[kWindow] = {};
    float bob = 0, hands = 0;
    int handN = 0;
    for (int i = 1; i < g_count; ++i) {
        const Sample &a = at(i - 1), &b = at(i);
        vy[i] = (b.headY - a.headY) / dt;
        bob += vy[i] * vy[i];
        float sp = 0;
        int n = 0;
        for (int h = 0; h < 2; ++h) {
            if (!a.handOk[h] || !b.handOk[h]) continue;
            const float dx = b.hand[h][0] - a.hand[h][0], dy = b.hand[h][1] - a.hand[h][1],
                        dz = b.hand[h][2] - a.hand[h][2];
            sp += std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
            ++n;
        }
        if (n) {
            sp /= n;
            hands += sp;
            ++handN;
        }
        s[i] = vy[i] + 0.25f * sp;
    }
    const int n = g_count - 1;
    bob = std::sqrt(bob / n);
    hands = handN ? hands / handN : 0;
    // The rhythm signal without its mean (the hands' speed is always positive).
    float mean = 0;
    for (int i = 1; i < g_count; ++i) mean += s[i];
    mean /= n;
    for (int i = 1; i < g_count; ++i) s[i] -= mean;
    float best = 0;
    int bestLag = 0;
    for (int lag = kMinLag; lag <= kMaxLag && lag < n / 2; ++lag) {
        float xy = 0, xx = 0, yy = 0;
        for (int i = 1; i + lag < g_count; ++i) {
            xy += s[i] * s[i + lag];
            xx += s[i] * s[i];
            yy += s[i + lag] * s[i + lag];
        }
        const float r = (xx > 1e-6f && yy > 1e-6f) ? xy / std::sqrt(xx * yy) : 0;
        if (r > best) best = r, bestLag = lag;
    }
    // Walking: the head's way over the floor in the last 2 s.
    const Sample &from = at(g_count - 1 - (g_count > 2 * kRate ? 2 * kRate : g_count - 1)), &to = at(g_count - 1);
    const float walked = std::sqrt((to.headX - from.headX) * (to.headX - from.headX) +
                                   (to.headZ - from.headZ) * (to.headZ - from.headZ));
    const bool walking = walked > kWalkMetres || GetTickCount64() - g_stickAt < kStickQuietMs;
    State st;
    st.valid = true;
    st.energy = bob + 0.5f * hands;
    st.rhythm = best < 0 ? 0 : best > 1 ? 1 : best;
    st.bpm = bestLag ? 60.0f * kRate / bestLag : 0;
    st.walking = walking;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = st;
    }
    // One line when it looks like dancing starts or stops (Python decides; this is for the log).
    const bool on = !walking && st.energy > 0.25f && st.rhythm > 0.4f;
    if (on != g_loggedOn && g_logs < 60) {
        ++g_logs;
        g_loggedOn = on;
        LOG("Dance: %s (energy %.2f m/s, rhythm %.2f, %.0f bpm, head bob %.2f, hands %.2f)",
            on ? "moving to a beat" : "stopped", st.energy, st.rhythm, st.bpm, bob, hands);
    }
}

}  // namespace

void update(const armik::Hand handsIn[2], const grab::HeadPose& head, float leftStick, bool firstPerson) {
    if (!firstPerson || !head.valid) {
        if (g_count) clear();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (leftStick > 0.3f) g_stickAt = now;
    if (g_lastAt && now - g_lastAt < kStepMs) return;
    if (g_lastAt && now - g_lastAt > 500) clear();  // a gap (paused, menu): start over
    g_lastAt = now;
    Sample& smp = g_samples[g_next];
    smp.headY = head.position.y;
    smp.headX = head.position.x;
    smp.headZ = head.position.z;
    for (int h = 0; h < 2; ++h) {
        smp.handOk[h] = handsIn[h].valid;
        smp.hand[h][0] = handsIn[h].position.x - head.position.x;
        smp.hand[h][1] = handsIn[h].position.y - head.position.y;
        smp.hand[h][2] = handsIn[h].position.z - head.position.z;
    }
    g_next = (g_next + 1) % kWindow;
    if (g_count < kWindow) ++g_count;
    if (g_count >= kWindow) analyse();
}

State current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

}  // namespace s4vr::dance
