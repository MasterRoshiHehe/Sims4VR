#include "renderscale.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "crashlog.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"
#include "rtti.h"

namespace s4vr::renderscale {

namespace {

constexpr char kDisplayManagerClass[] = ".?AVDisplayManager@UI@Sims4@EA@@";

// DisplayManager fields
constexpr size_t kRenderMinWidth = 0x10, kRenderMinHeight = 0x14;  // config RenderMinWidth/Height
constexpr size_t kPolledSize = 0x18;                              // int2; 0 makes Update re-apply
constexpr size_t kAppliedWindowSize = 0x20;                       // int2; -1 forces the size branch
// gfx device fields: back-buffer width/height, "scaled", MSAA samples. The DX11
// device grew in 1.121 (was 0x40/0x44/0x2f/0x4c; GetDisplayMode 0x125e670,
// GetScaled 0x125e690, SetMsaa 0x125e8b0); the DX9 one kept the old layout.
struct GfxFields {
    size_t width, height, scaled, msaa;
};
constexpr GfxFields kGfxDx11{0x48, 0x4c, 0x35, 0x54}, kGfxDx9{0x40, 0x44, 0x2f, 0x4c};
GfxFields g_f = kGfxDx11;  // this executable's, set in tryInstall
// Canvas fields and vtable
constexpr size_t kCanvasExclusiveFullscreen = 0x4b;  // the engine refuses scaled modes there
constexpr size_t kClearViewportSlot = 0x168 / 8, kSetVirtualSizeSlot = 0x178 / 8;

using ApplySettingsFn = uint64_t (*)(void* dm, void* settings, char a3, char a4, char a5, char a6);
using GetterFn = uint8_t* (*)();
using SetLetterboxFn = void (*)(void* rs, int x, int y, int w, int h, int windowW, int windowH);
using SetMsaaFn = char (*)(void* gfx, int samples, char validate);
using ResetFn = char (*)(void* gfx, char a2);
using ClearViewportFn = void (*)(void* canvas);
using SetVirtualSizeFn = void (*)(void* canvas, int w, int h);

ApplySettingsFn g_original = nullptr;
GetterFn g_gfx = nullptr, g_renderSystem = nullptr, g_canvas = nullptr;
SetLetterboxFn g_setLetterbox = nullptr;
SetMsaaFn g_setMsaa = nullptr;
ResetFn g_reset = nullptr;
uint8_t* g_dm = nullptr;

std::atomic<int> g_width{0}, g_height{0};
// Headset width (v0.13.309): ApplySettings takes render width = max(window width, RenderMinWidth * dpi)
// (0xdaf977..0xdaf98e in 1.128: cmp r14d, eax; cmovl ecx, eax), so the 3D view was never narrower
// than the window: 2560 wide where the headset needs ~1464. While on, the cmovl becomes a mov
// (ApplySettings + 0x17e: 0f 4c c8 -> 89 c1 90): the width is ours alone. Restored when off.
std::atomic<bool> g_fitWidth{false};
uint8_t* g_widthPatch = nullptr;  // ApplySettings + 0x17e once the bytes there were recognised
bool g_widthPatched = false;
constexpr uint8_t kWidthMax[3] = {0x0f, 0x4c, 0xc8}, kWidthOurs[3] = {0x89, 0xc1, 0x90};

void patchWidth(bool ours) {
    if (!g_widthPatch || g_widthPatched == ours) return;
    DWORD old = 0;
    if (!VirtualProtect(g_widthPatch, 3, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(g_widthPatch, ours ? kWidthOurs : kWidthMax, 3);
    VirtualProtect(g_widthPatch, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_widthPatch, 3);
    g_widthPatched = ours;
}
int g_savedMinW = -1, g_savedMinH = -1;  // the game's RenderMin, restored when we stop
int g_msaa = 0;                          // the player's edge smoothing (MSAA samples) before we scaled
int g_msaaCap = 4;                       // ini: most samples while scaled (never more than the player's)

template <typename T>
T at(uint8_t* p, size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

uint64_t hookedApplySettings(void* dm, void* settings, char a3, char a4, char a5, char a6) {
    crashlog::note(crashlog::kRenderScale);
    auto* d = static_cast<uint8_t*>(dm);
    int w = g_width.load(), h = g_height.load();
    uint8_t* canvas = g_canvas();
    if (w > 0 && canvas && at<uint8_t>(canvas, kCanvasExclusiveFullscreen)) {
        static bool logged = false;
        if (!logged) LOG("Render scale: off in exclusive fullscreen (use windowed fullscreen)");
        logged = true;
        w = h = 0;
    }
    uint8_t* gfx = g_gfx();
    if (gfx) {
        int samples = at<int>(gfx, g_f.msaa);
        if (samples > 1 && !at<uint8_t>(gfx, g_f.scaled)) g_msaa = samples;
    }
    if (w > 0 && h > 0) {
        if (g_savedMinW < 0) {
            g_savedMinW = at<int>(d, kRenderMinWidth);
            g_savedMinH = at<int>(d, kRenderMinHeight);
        }
        std::memcpy(d + kRenderMinWidth, &w, sizeof(int));
        std::memcpy(d + kRenderMinHeight, &h, sizeof(int));
        patchWidth(g_fitWidth.load());
    } else if (g_savedMinW >= 0) {
        patchWidth(false);
        std::memcpy(d + kRenderMinWidth, &g_savedMinW, sizeof(int));
        std::memcpy(d + kRenderMinHeight, &g_savedMinH, sizeof(int));
        g_savedMinW = g_savedMinH = -1;
    }

    if (!(w > 0 && h > 0)) patchWidth(false);
    uint64_t result = g_original(dm, settings, a3, a4, a5, a6);

    gfx = g_gfx();
    if ((result & 0xff) && gfx && at<uint8_t>(gfx, g_f.scaled)) {
        int bw = at<int>(gfx, g_f.width), bh = at<int>(gfx, g_f.height);
        // No letterbox: the whole back buffer is the frame (the desktop window
        // shows it stretched), and the mouse maps straight onto it.
        if (uint8_t* rs = g_renderSystem()) g_setLetterbox(rs, 0, 0, 0, 0, 0, 0);
        if ((canvas = g_canvas())) {
            auto vtable = *reinterpret_cast<void***>(canvas);
            reinterpret_cast<ClearViewportFn>(vtable[kClearViewportSlot])(canvas);
            reinterpret_cast<SetVirtualSizeFn>(vtable[kSetVirtualSizeSlot])(canvas, bw, bh);
        }
        // The engine turns edge smoothing off for scaled rendering; put it back.
        // Not in DX9: scaled frames there need the COPY swap effect, which
        // Direct3D 9 can't multisample (the reset would fail, the screen go black).
        int samples = g_msaa < g_msaaCap ? g_msaa : g_msaaCap;
        bool msaa = false;
        if (!gameaddr::dx9() && samples > 1 && at<int>(gfx, g_f.msaa) != samples)
            msaa = g_setMsaa(gfx, samples, 1) && g_reset(gfx, 0);
        LOG("Render size %dx%d (window stays as it is), MSAA %d%s", bw, bh, at<int>(gfx, g_f.msaa),
            msaa ? " restored" : gameaddr::dx9() && g_msaa > 1 ? " (DX9: off while scaled)" : "");
    } else if (result & 0xff) {
        LOG("Render size back to the game's own (%dx%d)", gfx ? at<int>(gfx, g_f.width) : 0,
            gfx ? at<int>(gfx, g_f.height) : 0);
    }
    return result;
}

// The display manager only exists once the game has set up its display, which
// can be after the script mod loads; request() retries until it is found.
bool g_wanted = false;
bool g_unsupported = false;
ULONGLONG g_nextTry = 0;

bool tryInstall() {
    if (g_original) return true;
    if (g_unsupported) return false;
    for (const char* k : {"ApplySettings", "SetLetterbox", "SetMsaa", "Reset", "Gfx", "RenderSystem", "Canvas"}) {
        if (!addrs::fn(k)) {
            LOG("Render scale: game build not recognised (no match for %s); off", k);
            g_unsupported = true;
            return false;
        }
    }
    uint8_t* dm = nullptr;
    uint8_t* dmGlobal = addrs::global("DisplayManagerGlobal");
    if (!dmGlobal) {
        g_unsupported = true;
        return false;
    }
    std::memcpy(&dm, dmGlobal, sizeof(dm));
    char name[64] = {};
    if (!dm) return false;  // not created yet
    if (!rtti::className(reinterpret_cast<uintptr_t>(dm), name, sizeof(name)) ||
        std::strcmp(name, kDisplayManagerClass) != 0) {
        LOG("Render scale: display manager not recognised (%p %s); off", static_cast<void*>(dm), name);
        g_unsupported = true;
        return false;
    }
    g_dm = dm;
    g_f = gameaddr::dx9() ? kGfxDx9 : kGfxDx11;
    auto fn = [](const char* name) { return addrs::fn(name); };
    g_gfx = reinterpret_cast<GetterFn>(fn("Gfx"));
    g_renderSystem = reinterpret_cast<GetterFn>(fn("RenderSystem"));
    g_canvas = reinterpret_cast<GetterFn>(fn("Canvas"));
    g_setLetterbox = reinterpret_cast<SetLetterboxFn>(fn("SetLetterbox"));
    g_setMsaa = reinterpret_cast<SetMsaaFn>(fn("SetMsaa"));
    g_reset = reinterpret_cast<ResetFn>(fn("Reset"));

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* target = fn("ApplySettings");
    {
        auto* at = static_cast<uint8_t*>(target) + 0x17b;  // cmp r14d, eax; cmovl ecx, eax
        const uint8_t expected[6] = {0x44, 0x3b, 0xf0, 0x0f, 0x4c, 0xc8};
        if (std::memcmp(at, expected, sizeof(expected)) == 0) g_widthPatch = at + 3;
        else LOG("Render scale: render width can't go under the window's on this game build");
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedApplySettings), reinterpret_cast<void**>(&g_original)) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Render scale: hook failed; off");
        g_original = nullptr;
        g_unsupported = true;
        return false;
    }
    LOG("Render scale: hooked display settings (display manager %p)", static_cast<void*>(dm));
    return true;
}

}  // namespace

bool install(int msaaCap) {
    g_msaaCap = msaaCap;
    g_wanted = true;
    if (tryInstall()) return true;
    if (!g_unsupported) LOG("Render scale: display not set up yet; will hook it once it is");
    return !g_unsupported;
}

void request(int width, int height) {
    if (!g_original && g_wanted && (width > 0 || height > 0) && GetTickCount64() >= g_nextTry) {
        g_nextTry = GetTickCount64() + 2000;
        tryInstall();
    }
    if (!g_original || !g_dm) return;
    if (width < 0 || height < 0) width = height = 0;
    int oldWidth = g_width.exchange(width), oldHeight = g_height.exchange(height);
    if (oldWidth == width && oldHeight == height) return;
    // Make DisplayManager::Update re-apply on the game's main thread next frame.
    int64_t zero = 0, minusOne = -1;
    std::memcpy(g_dm + kAppliedWindowSize, &minusOne, sizeof(minusOne));
    std::memcpy(g_dm + kPolledSize, &zero, sizeof(zero));
    LOG("Render scale: requested %dx%d", width, height);
}

bool scaled() { return g_width.load() > 0 || g_height.load() > 0; }

void setFitWidth(bool on) {
    if (g_fitWidth.exchange(on) == on) return;
    LOG("Render scale: the 3D view %s", on ? "as wide as the headset needs (may be narrower than the window)"
                                           : "at least as wide as the window");
    if (!g_original || !g_dm || !scaled()) return;
    int64_t zero = 0, minusOne = -1;  // re-apply next frame, as request does
    std::memcpy(g_dm + kAppliedWindowSize, &minusOne, sizeof(minusOne));
    std::memcpy(g_dm + kPolledSize, &zero, sizeof(zero));
}

}  // namespace s4vr::renderscale
