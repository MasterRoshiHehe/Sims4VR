#include "cpuprio.h"

#include <windows.h>
#include <atomic>
#include <vector>

#include "log.h"

namespace s4vr::cpuprio {

namespace {

bool g_on = false;
std::atomic<bool> g_mainDone{false};

using SetCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, USHORT);
using GetCpuSetInfoFn = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
using SetThreadInfoFn = BOOL(WINAPI*)(HANDLE, int, void*, DWORD);
using AvSetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);

// SYSTEM_CPU_SET_INFORMATION's leading fields (winnt.h, Windows 10): Size, Type, then CpuSet with Id at +8 and
// EfficiencyClass at +0x12 (after Group u16 +0xc, LogicalProcessorIndex u8 +0xe, CoreIndex +0xf,
// LastLevelCacheIndex +0x10, NumaNodeIndex +0x11).
constexpr int kThreadPowerThrottling = 3;  // THREAD_INFORMATION_CLASS ThreadPowerThrottling
struct PowerThrottling {
    ULONG version, controlMask, stateMask;
};

// The CPU set ids of the fastest cores (highest efficiency class); empty when all cores are alike.
const std::vector<ULONG>& fastCores(int& fast, int& all) {
    static std::vector<ULONG> ids;
    static int fastCount = 0, allCount = 0;
    static bool done = false;
    if (!done) {
        done = true;
        auto get = reinterpret_cast<GetCpuSetInfoFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
        ULONG size = 0;
        if (get) get(nullptr, 0, &size, GetCurrentProcess(), 0);
        if (get && size) {
            std::vector<uint8_t> buffer(size);
            if (get(buffer.data(), size, &size, GetCurrentProcess(), 0)) {
                uint8_t top = 0;
                for (ULONG at = 0; at + 0x14 <= size;) {
                    const ULONG entry = *reinterpret_cast<ULONG*>(&buffer[at]);
                    if (entry < 0x14) break;
                    if (*reinterpret_cast<ULONG*>(&buffer[at + 4]) == 0) if (buffer[at + 0x12] > top) top = buffer[at + 0x12];
                    at += entry;
                }
                for (ULONG at = 0; at + 0x14 <= size;) {
                    const ULONG entry = *reinterpret_cast<ULONG*>(&buffer[at]);
                    if (entry < 0x14) break;
                    if (*reinterpret_cast<ULONG*>(&buffer[at + 4]) == 0) {
                        ++allCount;
                        if (buffer[at + 0x12] == top) ids.push_back(*reinterpret_cast<ULONG*>(&buffer[at + 8]));
                    }
                    at += entry;
                }
                fastCount = static_cast<int>(ids.size());
                if (fastCount == allCount) ids.clear();  // no slow cores: no preference needed
            }
        }
    }
    fast = fastCount;
    all = allCount;
    return ids;
}

void boost(const char* who, int priority, bool games) {
    HANDLE thread = GetCurrentThread();
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    int fast = 0, all = 0;
    const std::vector<ULONG>& ids = fastCores(fast, all);
    bool cores = false;
    if (!ids.empty())
        if (auto set = reinterpret_cast<SetCpuSetsFn>(GetProcAddress(kernel, "SetThreadSelectedCpuSets")))
            cores = set(thread, ids.data(), static_cast<USHORT>(ids.size())) != 0;
    bool unthrottled = false;
    if (auto info = reinterpret_cast<SetThreadInfoFn>(GetProcAddress(kernel, "SetThreadInformation"))) {
        PowerThrottling state{1, 1, 0};  // THREAD_POWER_THROTTLING_EXECUTION_SPEED controlled, off
        unthrottled = info(thread, kThreadPowerThrottling, &state, sizeof(state)) != 0;
    }
    const bool raised = SetThreadPriority(thread, priority) != 0;
    bool mmcss = false;
    if (games) {
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        auto av = avrt ? reinterpret_cast<AvSetFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
        DWORD task = 0;
        mmcss = av && av(L"Games", &task) != nullptr;
    }
    LOG("Fast cores: %s thread %lu: %s (%d of %d logical cores fast), power throttling %s, priority %s%s", who,
        GetCurrentThreadId(), cores ? "prefers the fast cores" : ids.empty() ? "all cores alike, no preference" : "core preference failed",
        fast, all, unthrottled ? "off" : "unchanged", raised ? (priority == THREAD_PRIORITY_HIGHEST ? "highest" : "above normal") : "unchanged",
        games ? (mmcss ? ", multimedia class Games" : ", multimedia class failed") : "");
}

}  // namespace

void configure(bool on) {
    g_on = on;
    if (!on) LOG("Fast cores: off (Windows places the game's threads)");
}

void boostMainThread() {
    if (!g_on || g_mainDone.exchange(true)) return;
    boost("game main", THREAD_PRIORITY_ABOVE_NORMAL, false);
}

void boostClockThread() {
    if (!g_on) return;
    boost("frame clock", THREAD_PRIORITY_HIGHEST, true);
}

}  // namespace s4vr::cpuprio
