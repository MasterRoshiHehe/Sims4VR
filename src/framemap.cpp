#include "framemap.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "log.h"
#include "singleview.h"

namespace s4vr::framemap {
namespace {

const char* const kNames[SiteCount] = {"CameraCopy", "SceneBind", "DepthOnlyBind", "OtherBind", "ColourBind",
                                       "SceneDraw",  "CullRooms", "UiRender",      "Present",     "ShadowFit"};

// Presents at which a window of kWindowFrames frames is recorded.
// Both real eyes present twice a game frame: 4 Presents = two frames, both passes each.
const uint64_t kWindowAt[] = {3000, 9000, 20000, 40000, 70000};
constexpr int kWindowFrames = 4;
constexpr int kMaxEvents = 4000;
constexpr int kMaxStacks = 128;
constexpr int kStackDepth = 40;

struct Event {
    int64_t when;
    uint32_t thread;
    uint16_t site;
    int16_t extra;
    uint32_t count;  // the same event repeated back to back
};
struct Stack {
    uint16_t site;
    int16_t extra;
    uint32_t thread;
    uint16_t depth;
    void* frames[kStackDepth];
};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_recording{false};
std::atomic<uint64_t> g_presents{0};
int g_window = 0;        // next window index (Present thread)
int g_framesLeft = 0;    // Presents left in the current window (Present thread)
int64_t g_windowStart = 0;
std::mutex g_mutex;      // the event and stack tables while recording
Event g_events[kMaxEvents];
int g_eventCount = 0;
Stack g_stacks[kMaxStacks];
int g_stackCount = 0;
int g_dropped = 0;

int64_t ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

const double g_msPerTick = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 / static_cast<double>(f.QuadPart);
}();

void record(Site site, int extra) {
    const int64_t now = ticks();
    const uint32_t thread = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recording.load(std::memory_order_relaxed)) return;
    // A new kind of event for this thread in this window: keep its call stack.
    bool known = false;
    for (int i = 0; i < g_stackCount; ++i) {
        if (g_stacks[i].site == site && g_stacks[i].extra == extra && g_stacks[i].thread == thread) {
            known = true;
            break;
        }
    }
    if (!known && g_stackCount < kMaxStacks) {
        Stack& s = g_stacks[g_stackCount++];
        s.site = static_cast<uint16_t>(site);
        s.extra = static_cast<int16_t>(extra);
        s.thread = thread;
        s.depth = static_cast<uint16_t>(RtlCaptureStackBackTrace(1, kStackDepth, s.frames, nullptr));
    }
    if (g_eventCount) {
        Event& last = g_events[g_eventCount - 1];
        if (last.site == site && last.extra == extra && last.thread == thread) {
            ++last.count;
            return;
        }
    }
    if (g_eventCount == kMaxEvents) {
        ++g_dropped;
        return;
    }
    g_events[g_eventCount++] = Event{now, thread, static_cast<uint16_t>(site), static_cast<int16_t>(extra), 1};
}

void describe(void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        const uintptr_t rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
        if (module == GetModuleHandleW(nullptr)) {
            std::snprintf(out, size, "game+0x%llx", static_cast<unsigned long long>(rva));
            return;
        }
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        std::snprintf(out, size, "%ls+0x%llx", name, static_cast<unsigned long long>(rva));
        return;
    }
    std::snprintf(out, size, "%p", address);
}

// Writes the window to the log (Present thread, after recording stopped).
void dump(int window) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LOG("Frame map: window %d (%d frames from Present #%llu): %d event runs%s, %d call stacks", window, kWindowFrames,
        static_cast<unsigned long long>(kWindowAt[window]), g_eventCount, g_dropped ? " (some dropped)" : "",
        g_stackCount);
    for (int i = 0; i < g_eventCount; ++i) {
        const Event& e = g_events[i];
        LOG("Frame map: %8.3f ms  thread %5lu  %-13s %3d  x%u", (e.when - g_windowStart) * g_msPerTick,
            static_cast<unsigned long>(e.thread), kNames[e.site], static_cast<int>(e.extra), e.count);
    }
    char line[2048];
    char one[96];
    for (int i = 0; i < g_stackCount; ++i) {
        const Stack& s = g_stacks[i];
        line[0] = 0;
        size_t used = 0;
        for (int f = 0; f < s.depth; ++f) {
            describe(s.frames[f], one, sizeof(one));
            int n = std::snprintf(line + used, sizeof(line) - used, "%s%s", f ? " < " : "", one);
            if (n < 0 || used + static_cast<size_t>(n) >= sizeof(line)) break;
            used += static_cast<size_t>(n);
        }
        LOG("Frame map stack: %s %d on thread %lu: %s", kNames[s.site], static_cast<int>(s.extra),
            static_cast<unsigned long>(s.thread), line);
    }
    g_eventCount = 0;
    g_stackCount = 0;
    g_dropped = 0;
}

}  // namespace

void init(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) LOG("Frame map: on; %d windows of %d frames will be logged", static_cast<int>(sizeof(kWindowAt) / sizeof(kWindowAt[0])), kWindowFrames);
}

void mark(Site site, int extra) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (site == Present) {
        const uint64_t n = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_recording.load(std::memory_order_relaxed)) {
            record(site, extra);
            if (--g_framesLeft <= 0) {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_recording.store(false, std::memory_order_relaxed);
                }
                dump(g_window++);
            }
            return;
        }
        if (g_window < static_cast<int>(sizeof(kWindowAt) / sizeof(kWindowAt[0])) && n >= kWindowAt[g_window]) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_windowStart = ticks();
            g_framesLeft = kWindowFrames;
            g_eventCount = 0;
            g_stackCount = 0;
            g_dropped = 0;
            g_recording.store(true, std::memory_order_relaxed);
            // The Present that opens the window is its first event.
            g_events[g_eventCount++] = Event{g_windowStart, GetCurrentThreadId(), static_cast<uint16_t>(Present), 0, 1};
        }
        return;
    }
    if (g_recording.load(std::memory_order_relaxed)) record(site, singleview::inSecond() ? extra + 100 : extra);
}

}  // namespace s4vr::framemap
