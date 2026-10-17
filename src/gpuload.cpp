#include "gpuload.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"

namespace s4vr::gpuload {

namespace {

constexpr int kQueries = 8;
ID3D11Device* g_device = nullptr;  // not owned: the queries' device
ID3D11Query* g_queries[kQueries] = {};
LARGE_INTEGER g_issued[kQueries] = {};
bool g_pending[kQueries] = {};
int g_next = 0;
LARGE_INTEGER g_frequency{};

double g_total = 0, g_worst = 0, g_latest = -1;
int g_count = 0;

IDXGIAdapter3* g_adapter = nullptr;

// GPU busy meter (Windows' "GPU Engine" counters), filled by its own thread.
std::mutex g_busyMutex;
Busy g_busy;            // sums since the last takeBusy (averaged there)
Busy g_busyNow;         // the latest sample
bool g_busyKnown = false;
std::atomic<bool> g_samplerStarted{false};

// Instance names look like "pid_1234_luid_0x00000000_0x0000D1A3_phys_0_eng_0_engtype_3D".
void sample(PDH_HCOUNTER counter, const std::wstring& luid, DWORD pid, std::vector<unsigned char>& buffer) {
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || !size) return;
    buffer.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    // Per engine: everyone's share added up, and the game's.
    std::map<std::wstring, double> all3d, game3d, encode;
    wchar_t ownPid[32];
    std::swprintf(ownPid, 32, L"pid_%lu_", static_cast<unsigned long>(pid));
    for (DWORD i = 0; i < count; ++i) {
        const PDH_FMT_COUNTERVALUE& v = items[i].FmtValue;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        std::wstring name = items[i].szName ? items[i].szName : L"";
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        if (name.find(luid) == std::wstring::npos) continue;  // another card
        size_t phys = name.find(L"_phys_");
        size_t type = name.find(L"_engtype_");
        if (phys == std::wstring::npos || type == std::wstring::npos) continue;
        std::wstring engine = name.substr(phys);
        std::wstring kind = name.substr(type + 9);
        bool own = name.compare(0, wcslen(ownPid), ownPid) == 0;
        if (kind == L"3d") {
            all3d[engine] += v.doubleValue;
            if (own) game3d[engine] += v.doubleValue;
        } else if (kind.find(L"encode") != std::wstring::npos || kind.find(L"codec") != std::wstring::npos) {
            encode[engine] += v.doubleValue;
        }
    }
    // The busiest engine of each kind, like Task Manager.
    auto busiest = [](const std::map<std::wstring, double>& m) {
        double most = 0;
        for (const auto& e : m) most = e.second > most ? e.second : most;
        return most > 100 ? 100.0 : most;
    };
    Busy now;
    now.all3d = busiest(all3d);
    now.game3d = busiest(game3d);
    now.encode = busiest(encode);
    now.worst3d = now.all3d;
    now.samples = 1;
    std::lock_guard<std::mutex> lock(g_busyMutex);
    g_busyNow = now;
    g_busyKnown = true;
    g_busy.all3d += now.all3d;
    g_busy.game3d += now.game3d;
    g_busy.encode += now.encode;
    if (now.all3d > g_busy.worst3d) g_busy.worst3d = now.all3d;
    ++g_busy.samples;
}

void sampler(LUID luid) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
    if (status == ERROR_SUCCESS)
        status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
    if (status == ERROR_SUCCESS) status = PdhCollectQueryData(query);
    if (status != ERROR_SUCCESS) {
        LOG("GPU busy meter: Windows' GPU counters unavailable (0x%08lx)", static_cast<unsigned long>(status));
        if (query) PdhCloseQuery(query);
        return;
    }
    wchar_t text[48];
    std::swprintf(text, 48, L"luid_0x%08lx_0x%08lx", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    const std::wstring luidText = text;
    const DWORD pid = GetCurrentProcessId();
    std::vector<unsigned char> buffer;
    LOG("GPU busy meter: on (Windows' GPU counters, every 2 s)");
    for (;;) {
        Sleep(2000);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
        sample(counter, luidText, pid, buffer);
    }
}

void startSampler(const LUID& luid) {
    if (g_samplerStarted.exchange(true)) return;
    std::thread(sampler, luid).detach();
}

void release() {
    for (auto*& q : g_queries) {
        if (q) q->Release();
        q = nullptr;
    }
    for (bool& p : g_pending) p = false;
    if (g_adapter) g_adapter->Release();
    g_adapter = nullptr;
    g_device = nullptr;
}

void findAdapter(ID3D11Device* device) {
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            LOG("Graphics card: %s, %llu MB of its own memory", name,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
        }
        if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter))))
            g_adapter = nullptr;
        if (desc.AdapterLuid.LowPart || desc.AdapterLuid.HighPart) startSampler(desc.AdapterLuid);
        adapter->Release();
    }
    dxgi->Release();
    if (g_adapter) LOG("Video memory: %s", memoryText().c_str());
}

}  // namespace

void onFrame(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return;
    if (device != g_device) {
        release();
        g_device = device;
        if (!g_frequency.QuadPart) QueryPerformanceFrequency(&g_frequency);
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        for (auto*& q : g_queries)
            if (FAILED(device->CreateQuery(&desc, &q))) q = nullptr;
        findAdapter(device);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // The oldest first: a finished one tells how long the card took to get there.
    for (int k = 0; k < kQueries; ++k) {
        int i = (g_next + k) % kQueries;
        if (!g_pending[i] || !g_queries[i]) continue;
        BOOL done = FALSE;
        if (context->GetData(g_queries[i], &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        g_pending[i] = false;
        double ms = 1000.0 * static_cast<double>(now.QuadPart - g_issued[i].QuadPart) /
                    static_cast<double>(g_frequency.QuadPart);
        g_latest = ms;
        g_total += ms;
        if (ms > g_worst) g_worst = ms;
        ++g_count;
    }
    // A slot still waiting after a full ring: the card is that far behind;
    // it is counted as at least that late, and the slot reused.
    if (g_pending[g_next]) {
        double ms = 1000.0 * static_cast<double>(now.QuadPart - g_issued[g_next].QuadPart) /
                    static_cast<double>(g_frequency.QuadPart);
        g_latest = ms;
        g_total += ms;
        if (ms > g_worst) g_worst = ms;
        ++g_count;
    }
    if (ID3D11Query* q = g_queries[g_next]) {
        context->End(q);
        g_issued[g_next] = now;
        g_pending[g_next] = true;
    }
    g_next = (g_next + 1) % kQueries;
}

Lag take() {
    Lag lag;
    if (g_count) {
        lag.averageMs = g_total / g_count;
        lag.worstMs = g_worst;
        lag.frames = g_count;
    }
    g_total = g_worst = 0;
    g_count = 0;
    return lag;
}

double latestMs() { return g_latest; }

bool memory(Memory& out) {
    if (!g_adapter) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
    if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return false;
    if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) shared = {};
    out.usedMb = local.CurrentUsage >> 20;
    out.budgetMb = local.Budget >> 20;
    out.sharedMb = shared.CurrentUsage >> 20;
    return true;
}

std::string memoryText() {
    Memory m;
    if (!memory(m)) return "unknown";
    char text[128];
    std::snprintf(text, sizeof(text), "%llu of %llu MB, %llu MB more in system memory", m.usedMb, m.budgetMb,
                  m.sharedMb);
    return text;
}

Busy takeBusy() {
    std::lock_guard<std::mutex> lock(g_busyMutex);
    Busy busy = g_busy;
    if (busy.samples) {
        busy.all3d /= busy.samples;
        busy.game3d /= busy.samples;
        busy.encode /= busy.samples;
    }
    g_busy = Busy{};
    return busy;
}

std::string busyText(const Busy& busy) {
    if (!busy.samples) return "unknown";
    char text[160];
    std::snprintf(text, sizeof(text), "3D %.0f%% (the game %.0f%%), worst %.0f%%; video encoder %.0f%%", busy.all3d,
                  busy.game3d, busy.worst3d, busy.encode);
    return text;
}

std::string busyNowText() {
    Busy now;
    {
        std::lock_guard<std::mutex> lock(g_busyMutex);
        if (g_busyKnown) now = g_busyNow;
    }
    return busyText(now);
}

namespace {

// gdi32's D3DKMT scheduling class calls (d3dkmthk.h isn't in mingw).
enum SchedulingClass { kIdle, kBelowNormal, kNormal, kAboveNormal, kHigh, kRealtime };
using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
using GetClassFn = LONG(APIENTRY*)(HANDLE, int*);

const char* className(int c) {
    static const char* names[] = {"idle", "below normal", "normal", "above normal", "high", "realtime"};
    return c >= 0 && c <= 5 ? names[c] : "unknown";
}

// The streamer's class, or "not running" / "unknown".
std::string streamerClass(GetClassFn get) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "unknown";
    std::string result = "not running";
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"VirtualDesktop.Streamer.exe") != 0) continue;
        result = "unknown";
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process) {
            int c = -1;
            if (get && get(process, &c) == 0) result = className(c);
            CloseHandle(process);
        }
        break;
    }
    CloseHandle(snap);
    return result;
}

int g_yield = -1;  // -1 never set, 0 normal, 1 below normal

}  // namespace

void yieldToStreamer(bool on) {
    if (g_yield == (on ? 1 : 0)) return;
    if (g_yield == -1 && !on) {  // never lowered: nothing to put back
        g_yield = 0;
        return;
    }
    g_yield = on ? 1 : 0;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    auto set = gdi ? reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
    auto get = gdi ? reinterpret_cast<GetClassFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass")) : nullptr;
    if (!set) {
        LOG("Graphics card priority: Windows doesn't offer it; left as it is");
        return;
    }
    LONG status = set(GetCurrentProcess(), on ? kBelowNormal : kNormal);
    int now = -1;
    if (get) get(GetCurrentProcess(), &now);
    if (status != 0)
        LOG("Graphics card priority: couldn't set %s (status %08lx); the game is %s", on ? "below normal" : "normal",
            static_cast<unsigned long>(status), className(now));
    else
        LOG("Graphics card priority: the game %s (%s); Virtual Desktop's streamer is %s", className(now),
            on ? "the headset stream goes first" : "headset off", streamerClass(get).c_str());
}

}  // namespace s4vr::gpuload
