#include "knock.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace s4vr::knock {
namespace {

struct Target {
    float cx, cz, ux, uz, halfW, halfD, y0, y1;
    int type;
    unsigned long long id;
};
struct Event {
    unsigned long long id;
    int type, hand;
    float into;
};
std::mutex g_events;
std::vector<Event> g_queue;  // g_events; at most 8 waiting
std::mutex g_mutex;
std::vector<Target> g_targets;
std::atomic<uint32_t> g_version{0};

}  // namespace

void setTargets(const float* data, int count) {
    std::vector<Target> next;
    if (data && count >= 1) {
        int n = static_cast<int>(data[0]);
        if (n < 0 || n > 256 || count < 1 + n * 12) n = 0;
        for (int i = 0; i < n; ++i) {
            const float* d = data + 1 + i * 12;
            next.push_back({d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], static_cast<int>(d[8]),
                            static_cast<unsigned long long>(d[9]) | static_cast<unsigned long long>(d[10]) << 24 |
                                static_cast<unsigned long long>(d[11]) << 48});
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_targets.swap(next);
    g_version.fetch_add(1, std::memory_order_release);
}

bool inside(float x, float y, float z, float pad, int& type, float normal[2], unsigned long long& id) {
    thread_local std::vector<Target> targets;
    thread_local uint32_t version = 0;
    uint32_t v = g_version.load(std::memory_order_acquire);
    if (v != version) {
        std::lock_guard<std::mutex> lock(g_mutex);
        targets = g_targets;
        version = g_version.load(std::memory_order_relaxed);
    }
    for (const Target& t : targets) {
        if (y < t.y0 - pad || y > t.y1 + pad) continue;
        float dx = x - t.cx, dz = z - t.cz;
        float along = dx * t.ux + dz * t.uz;
        float across = -dx * t.uz + dz * t.ux;
        if (std::fabs(along) > t.halfW + pad || std::fabs(across) > t.halfD + pad) continue;
        type = t.type;
        id = t.id;
        float s = across < 0 ? -1.0f : 1.0f;
        normal[0] = -t.uz * s;
        normal[1] = t.ux * s;
        return true;
    }
    return false;
}

void pushEvent(unsigned long long id, int type, int hand, float into) {
    std::lock_guard<std::mutex> lock(g_events);
    if (g_queue.size() >= 8) g_queue.erase(g_queue.begin());
    g_queue.push_back({id, type, hand, into});
}

bool takeEvent(unsigned long long& id, int& type, int& hand, float& into) {
    std::lock_guard<std::mutex> lock(g_events);
    if (g_queue.empty()) return false;
    Event e = g_queue.front();
    g_queue.erase(g_queue.begin());
    id = e.id, type = e.type, hand = e.hand, into = e.into;
    return true;
}

}  // namespace s4vr::knock
