#include "gputimeline.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "log.h"

namespace s4vr::gputimeline {

namespace {

constexpr int kKinds = 4;
constexpr int kPool = 2048;
constexpr uint32_t kDelay = 3;      // frames to wait before reading a frame's stamps
constexpr uint32_t kGiveUp = 90;    // frames after which unread stamps are dropped
const char* const kNames[kKinds] = {"shadow maps", "other depth passes (reflections, water)", "main scene (both eyes)",
                                    "post and UI"};

const char* const kShort[kKinds] = {"shadow", "other-depth", "scene", "post"};

struct Stamp {
    ID3D11Query* query = nullptr;
    int kind = 0;
    uint32_t frame = 0;
};

std::atomic<bool> g_on{false};
std::atomic<bool> g_ready{false};
std::mutex g_mutex;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_imm = nullptr;
std::vector<ID3D11Query*> g_free;
std::deque<Stamp> g_issued;
uint32_t g_frame = 0;
ID3D11Query* g_disjoint[3] = {};
bool g_ended[3] = {};
int g_open = -1;
UINT64 g_freq = 0;
constexpr int kSlots = 48;
double g_slotMs[kKinds][kSlots] = {};
double g_sumMs[kKinds] = {};
uint32_t g_stamps[kKinds] = {};
uint32_t g_framesRead = 0;
uint32_t g_dropped = 0;
ULONGLONG g_logTick = 0;
bool g_failed = false;

bool setup(IDXGISwapChain* swapchain) {
    if (g_failed) return false;
    ID3D11Device* device = nullptr;
    if (!swapchain || FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) {
        g_failed = true;
        return false;
    }
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
    bool ok = imm != nullptr;
    for (auto& q : g_disjoint) ok = ok && SUCCEEDED(device->CreateQuery(&dj, &q));
    for (int i = 0; ok && i < kPool; ++i) {
        ID3D11Query* q = nullptr;
        if (FAILED(device->CreateQuery(&ts, &q))) {
            ok = false;
            break;
        }
        g_free.push_back(q);
    }
    if (!ok) {
        LOG("GPU timeline: could not make its queries; off");
        g_failed = true;
        device->Release();
        if (imm) imm->Release();
        return false;
    }
    g_device = device;  // kept for the session (the game's device lives as long as the process)
    g_imm = imm;
    LOG("GPU timeline: on (a GPU timestamp at every render-target bind; one log line every 10 s)");
    g_ready.store(true, std::memory_order_release);
    return true;
}

void recycle(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        g_free.push_back(g_issued.front().query);
        g_issued.pop_front();
    }
}

// Read the oldest frames whose stamps are all in. g_mutex held, on the presenting thread.
void collect() {
    while (!g_issued.empty() && g_issued.front().frame + kDelay <= g_frame) {
        const uint32_t frame = g_issued.front().frame;
        size_t count = 0;
        while (count < g_issued.size() && g_issued[count].frame == frame) ++count;
        std::vector<std::pair<UINT64, int>> ticks;
        ticks.reserve(count);
        bool ready = true;
        for (size_t i = 0; i < count; ++i) {
            UINT64 value = 0;
            if (g_imm->GetData(g_issued[i].query, &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                ready = false;
                break;
            }
            ticks.emplace_back(value, g_issued[i].kind);
        }
        if (!ready) {
            if (g_frame < frame + kGiveUp) return;  // wait for the card
            g_dropped += static_cast<uint32_t>(count);
            recycle(count);
            continue;
        }
        recycle(count);
        if (!g_freq || ticks.size() < 2) continue;
        std::sort(ticks.begin(), ticks.end());
        int ordinal[kKinds] = {};
        for (size_t i = 0; i + 1 < ticks.size(); ++i) {
            const double ms = static_cast<double>(ticks[i + 1].first - ticks[i].first) * 1000.0 / static_cast<double>(g_freq);
            if (ms < 0 || ms > 100) continue;  // a gap while the game was stalled is not a pass
            g_sumMs[ticks[i].second] += ms;
            const int o = ordinal[ticks[i].second]++;
            if (o < kSlots) g_slotMs[ticks[i].second][o] += ms;
            ++g_stamps[ticks[i].second];
        }
        ++g_framesRead;
    }
}

void report() {
    ULONGLONG now = GetTickCount64();
    if (!g_logTick) g_logTick = now;
    if (now - g_logTick < 10000) return;
    g_logTick = now;
    if (!g_framesRead) return;
    char line[600];
    int n = 0;
    double total = 0;
    for (int k = 0; k < kKinds; ++k) {
        const double per = g_sumMs[k] / g_framesRead;
        total += per;
        n += snprintf(line + n, sizeof(line) - n, "%s%s %.2f ms (%u binds)", k ? "; " : "", kNames[k], per,
                      g_stamps[k] / g_framesRead);
    }
    LOG("GPU timeline (last 10 s, %u frames read, %u stamps dropped): %s; total %.2f ms a frame", g_framesRead, g_dropped, line,
        total);
    // The five single passes that cost most: "kind #n" is the n-th bind of that kind in the frame (0-based).
    struct Slot { double ms; int kind, ord; } top[5] = {};
    for (int k = 0; k < kKinds; ++k)
        for (int o = 0; o < kSlots; ++o) {
            const double per = g_slotMs[k][o] / g_framesRead;
            for (int t = 0; t < 5; ++t)
                if (per > top[t].ms) {
                    for (int u = 4; u > t; --u) top[u] = top[u - 1];
                    top[t] = {per, k, o};
                    break;
                }
        }
    char big[300];
    int m = 0;
    for (const Slot& t : top)
        if (t.ms > 0) m += snprintf(big + m, sizeof(big) - m, "%s%s #%d %.2f ms", m ? "; " : "", kShort[t.kind], t.ord, t.ms);
    LOG("GPU timeline biggest single passes: %s", big);
    std::fill(&g_slotMs[0][0], &g_slotMs[0][0] + kKinds * kSlots, 0.0);
    std::fill(std::begin(g_sumMs), std::end(g_sumMs), 0.0);
    std::fill(std::begin(g_stamps), std::end(g_stamps), 0u);
    g_framesRead = 0;
    g_dropped = 0;
}

}  // namespace

void setEnabled(bool on) {
    if (g_on.exchange(on) != on) LOG("GPU timeline %s", on ? "requested" : "off");
}

void bind(ID3D11DeviceContext* context, Kind kind) {
    if (!g_on.load(std::memory_order_relaxed) || !g_ready.load(std::memory_order_acquire) || !context) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_free.empty()) return;
    ID3D11Query* q = g_free.back();
    g_free.pop_back();
    context->End(q);
    g_issued.push_back({q, static_cast<int>(kind), g_frame});
}

void presented(IDXGISwapChain* swapchain) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    if (!g_ready.load(std::memory_order_acquire) && !setup(swapchain)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    // One disjoint window per present on the immediate context; only its frequency is used.
    if (g_open >= 0) {
        g_imm->End(g_disjoint[g_open]);
        g_ended[g_open] = true;
    }
    if (!g_freq) {
        for (int i = 0; i < 3; ++i) {
            if (!g_ended[i] || i == g_open) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            if (g_imm->GetData(g_disjoint[i], &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !data.Disjoint)
                g_freq = data.Frequency;
        }
    }
    int next = (g_open + 1) % 3;
    if (g_open < 0) next = 0;
    g_ended[next] = false;
    g_imm->Begin(g_disjoint[next]);
    g_open = next;
    ++g_frame;
    collect();
    report();
}

}  // namespace s4vr::gputimeline
