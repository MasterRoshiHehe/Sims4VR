#include "hitch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "log.h"

namespace s4vr::hitch {
namespace {

constexpr double kStallMs = 60;
constexpr int kMaxSamples = 10, kMaxLines = 300, kStackBytes = 4096, kMaxFrames = 10;

std::atomic<int64_t> g_lastEnd{0}, g_lastStart{0};
std::atomic<bool> g_inPresent{false}, g_on{false};
std::atomic<DWORD> g_tid{0};
double g_msPerTick = 0;
int g_lines = 0;
uint64_t g_stalls = 0;

int64_t ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

struct Sample {
    uint64_t rip = 0;
    uint64_t returns[kMaxFrames] = {};
    int count = 0;
};

// Stack bytes copied while the thread is suspended (static: the watchdog is the only user).
uint8_t g_stack[kStackBytes];

bool isCallBefore(uint64_t q) {
    // The bytes before a return address: E8 rel32, FF /2 (call r/m) forms. q is inside a mapped module here.
    const uint8_t* p = reinterpret_cast<const uint8_t*>(q);
    if (p[-5] == 0xE8) return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;                      // call reg / [reg]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;                      // call [reg+disp8]
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;                      // call [rip+disp32] / [reg+disp32]
    if (p[-3] == 0x41 && p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;     // call r8..r15
    return false;
}

// Suspend, read the registers and the top of the stack, resume. Nothing else happens in between.
bool grab(HANDLE thread, uint64_t& rip, uint64_t& rsp, size_t& copied) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    bool ok = GetThreadContext(thread, &ctx) != 0;
    copied = 0;
    if (ok) {
        rip = ctx.Rip;
        rsp = ctx.Rsp;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(rsp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            copied = static_cast<size_t>(std::min<uint64_t>(kStackBytes, end - rsp));
            std::memcpy(g_stack, reinterpret_cast<void*>(rsp), copied);
        }
    }
    ResumeThread(thread);
    return ok;
}

bool inCode(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & kExec)) return false;
    return a - reinterpret_cast<uint64_t>(mbi.BaseAddress) >= 6;  // p[-6] stays in the region
}

void analyse(Sample& s, size_t copied) {
    s.count = 0;
    for (size_t i = 0; i + 8 <= copied && s.count < kMaxFrames; i += 8) {
        uint64_t q;
        std::memcpy(&q, g_stack + i, 8);
        if (q < 0x10000 || q > 0x7fffffffffffull) continue;
        if (inCode(q) && isCallBefore(q)) s.returns[s.count++] = q;
    }
}

std::map<uint64_t, std::string> g_names;  // module base -> "game" or file name

std::string where(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        char b[32];
        std::snprintf(b, sizeof(b), "?0x%llx", static_cast<unsigned long long>(a));
        return b;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(mbi.AllocationBase);
    auto it = g_names.find(base);
    if (it == g_names.end()) {
        std::string name = "?";
        wchar_t path[MAX_PATH];
        if (base == reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))) {
            name = "game";
        } else if (GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH)) {
            const wchar_t* slash = std::wcsrchr(path, L'\\');
            for (const wchar_t* c = slash ? slash + 1 : path; *c; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
        }
        it = g_names.emplace(base, name).first;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%s+0x%llx", it->second.c_str(), static_cast<unsigned long long>(a - base));
    return b;
}

std::string describe(const Sample& s) {
    std::string out = where(s.rip);
    for (int i = 0; i < s.count; ++i) out += " < " + where(s.returns[i]);
    return out;
}

void logStall(double ms, const std::vector<Sample>& samples) {
    ++g_stalls;
    if (g_lines >= kMaxLines) return;
    ++g_lines;
    std::map<std::string, int> groups;
    for (const Sample& s : samples) ++groups[describe(s)];
    std::vector<std::pair<int, std::string>> order;
    for (auto& g : groups) order.emplace_back(g.second, g.first);
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < order.size() && i < 3; ++i)
        text += (i ? "; " : "") + std::string("x") + std::to_string(order[i].first) + " " + order[i].second;
    LOGT("hitch", "Hitch #%llu: %.0f ms between pictures, %d samples (stack scan, innermost first): %s",
         static_cast<unsigned long long>(g_stalls), ms, static_cast<int>(samples.size()),
         samples.empty() ? "stall over before the first sample" : text.c_str());
}

void wait5ms(HANDLE timer) {
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -50000;  // 5 ms
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 50);
    } else {
        Sleep(5);
    }
}

DWORD WINAPI watchdog(void*) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    HANDLE thread = nullptr;
    DWORD openedFor = 0;
    int64_t episodeEnd = 0;
    std::vector<Sample> samples;
    double lastMs = 0;
    for (;;) {
        wait5ms(timer);
        const DWORD tid = g_tid.load();
        if (!g_on.load() || !tid) continue;
        if (tid != openedFor) {
            if (thread) CloseHandle(thread);
            thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            openedFor = tid;
        }
        const int64_t end = g_lastEnd.load();
        if (!end || !thread) continue;
        if (episodeEnd && end != episodeEnd) {  // the picture thread came back: log the stall that ended
            const int64_t start = g_lastStart.load();
            logStall(start > episodeEnd ? (start - episodeEnd) * g_msPerTick : lastMs, samples);
            episodeEnd = 0;
            samples.clear();
        }
        if (g_inPresent.load()) continue;
        const double ms = (ticks() - end) * g_msPerTick;
        if (ms < kStallMs) continue;
        lastMs = ms;
        if (!episodeEnd) episodeEnd = end;
        if (static_cast<int>(samples.size()) >= kMaxSamples) continue;
        uint64_t rip = 0, rsp = 0;
        size_t copied = 0;
        if (!grab(thread, rip, rsp, copied)) continue;
        Sample s;
        s.rip = rip;
        analyse(s, copied);
        samples.push_back(s);
    }
    return 0;
}

}  // namespace

void install(bool on) {
    g_on.store(on);
    static bool started = false;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
    if (!on || started) return;
    started = true;
    HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    LOGT("hitch", "Hitch catcher on: stalls over %.0f ms between pictures are sampled every 5 ms", kStallMs);
}

void presentStart() {
    g_lastStart.store(ticks());
    g_inPresent.store(true);
}

void presentEnd() {
    g_tid.store(GetCurrentThreadId());
    g_lastEnd.store(ticks());
    g_inPresent.store(false);
}

}  // namespace s4vr::hitch
