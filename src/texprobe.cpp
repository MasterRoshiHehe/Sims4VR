#include "texprobe.h"

#include <windows.h>
#include "MinHook.h"
#include <cstdint>
#include <cstdio>
#include <string>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::texprobe {
namespace {

constexpr uint32_t kOuter = 0x19f3210, kInner = 0x1a03f10;  // exe stamp 0x6aa08454
constexpr double kLogMs = 20;
constexpr int kMaxLines = 200;

using Fn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
Fn g_outer = nullptr, g_inner = nullptr;
uint8_t* g_base = nullptr;
double g_msPerTick = 0;
int g_lines = 0;
thread_local int t_depth = 0;

int64_t ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

bool readable(uint64_t p, size_t n) {
    if (p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return p + n <= reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// The object's vtable as a game rva, or 0.
uint64_t vtableOf(uint64_t obj) {
    if (!readable(obj, 8)) return 0;
    const uint64_t vt = *reinterpret_cast<const uint64_t*>(obj);
    const uint64_t base = reinterpret_cast<uint64_t>(g_base);
    return vt >= base && vt < base + 0x4000000 ? vt - base : 0;
}

uint64_t __fastcall outerHook(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    // The outer function steps a state machine on a job object: a -> [a+8] -> [+0x10] holds the state at +0x5d8.
    uint64_t job = 0;
    int state = -1;
    if (readable(a + 8, 8)) {
        const uint64_t x = *reinterpret_cast<const uint64_t*>(a + 8);
        if (readable(x + 0x10, 8)) job = *reinterpret_cast<const uint64_t*>(x + 0x10);
        if (job && readable(job + 0x5d8, 4)) state = *reinterpret_cast<const int*>(job + 0x5d8);
    }
    ++t_depth;
    const int64_t t0 = ticks();
    const uint64_t r = g_outer(a, b, c, d);
    const double ms = (ticks() - t0) * g_msPerTick;
    --t_depth;
    if (ms >= kLogMs && g_lines < kMaxLines) {
        ++g_lines;
        LOGT("texprobe", "Texture outer step %.0f ms: this vt game+0x%llx, job vt game+0x%llx, job state before %d", ms,
             static_cast<unsigned long long>(vtableOf(a)), static_cast<unsigned long long>(vtableOf(job)), state);
    }
    return r;
}

// Hex of n bytes at p (8 bytes a group), "unreadable" if not mapped; plus printable characters.
std::string dump(uint64_t p, size_t n) {
    if (!readable(p, n)) return "unreadable";
    std::string out, text;
    char b[8];
    const uint8_t* d = reinterpret_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", d[i]);
        out += b;
        if ((i & 7) == 7 && i + 1 < n) out += ' ';
        text += d[i] >= 32 && d[i] < 127 ? static_cast<char>(d[i]) : '.';
    }
    return out + " |" + text + "|";
}

uint64_t __fastcall innerHook(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    const int64_t t0 = ticks();
    const uint64_t r = g_inner(a, b, c, d);
    const double ms = (ticks() - t0) * g_msPerTick;
    if (ms >= kLogMs && g_lines < kMaxLines) {
        ++g_lines;
        LOGT("texprobe", "Texture inner call %.0f ms: this vt game+0x%llx, arg2 vt game+0x%llx, inside outer step: %s", ms,
             static_cast<unsigned long long>(vtableOf(a)), static_cast<unsigned long long>(vtableOf(b)),
             t_depth > 0 ? "yes" : "no");
        static int details = 0;
        if (ms >= 50 && details < 60) {  // what was built: raw bytes (the resource key sits in the ModelLOD's first 0x60)
            ++details;
            uint64_t name = readable(a + 0x1c0, 8) ? *reinterpret_cast<const uint64_t*>(a + 0x1c0) : 0;
            LOGT("texprobe", "Texture build detail: ModelLOD %s | owner+0: %s | owner+0x1c0: %s | *(owner+0x1c0): %s",
                 dump(b, 0x60).c_str(), dump(a, 0x40).c_str(), dump(a + 0x1c0, 0x20).c_str(),
                 name ? dump(name, 0x30).c_str() : "none");
        }
    }
    return r;
}

}  // namespace

void install(bool on) {
    if (!on) return;
    if (!gameaddr::recognised() || gameaddr::dx9()) {
        LOGT("texprobe", "Texture probe: game build not recognised; off");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    uint8_t* o = g_base + kOuter;
    uint8_t* i = g_base + kInner;
    if (MH_CreateHook(o, reinterpret_cast<void*>(&outerHook), reinterpret_cast<void**>(&g_outer)) != MH_OK ||
        MH_CreateHook(i, reinterpret_cast<void*>(&innerHook), reinterpret_cast<void**>(&g_inner)) != MH_OK ||
        MH_EnableHook(o) != MH_OK || MH_EnableHook(i) != MH_OK) {
        LOGT("texprobe", "Texture probe: hook failed; off");
        return;
    }
    LOGT("texprobe", "Texture probe on: game+0x%x and game+0x%x timed, calls over %.0f ms logged", kOuter, kInner, kLogMs);
}

}  // namespace s4vr::texprobe
