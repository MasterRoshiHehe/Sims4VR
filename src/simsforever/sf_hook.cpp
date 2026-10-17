// Sims Forever, the Sims 1 side (32-bit, built by tools/build_simsforever.sh).
//
// sf_launch.exe starts the user's Sims 1 and loads this DLL into it. Here:
// - Sims 1 keeps running behind The Sims 4: losing focus is kept from it
//   (WM_ACTIVATEAPP / WM_ACTIVATE / WM_KILLFOCUS), focus goes back to The
//   Sims 4 right away, and Sims 1 can't take it again (SetForegroundWindow).
//   While the computer screen isn't on show, Sims 4 says so (active = 0) and
//   Sims 1 is told it lost focus, so it pauses itself.
// - Its picture: every frame it blits to the screen (IDirectDrawSurface Blt /
//   BltFast / Flip onto the primary) is copied to the shared memory as BGRA,
//   at most ~30 times a second. If no blit is seen, PrintWindow is used.
// - Its mouse: Sims 4 writes a cursor and buttons (the laser or your finger
//   on the screen). GetCursorPos / GetAsyncKeyState / GetKeyState answer with
//   them and mouse messages are posted to the window. While Sims 1 is on the
//   screen your real mouse and keyboard are locked to it: Sims 4 catches them
//   and they arrive here (moves, buttons, wheel, key and character messages).
// - Its sound: DirectSound mutes an app that isn't in front; dsound.dll is told
//   Sims 1 is in front (GetForegroundWindow) and new sound buffers get
//   DSBCAPS_GLOBALFOCUS, so you hear Sims 1 while The Sims 4 has the focus.
// - Its mouse cursor: Sims 1 uses Windows cursors (SetCursor with its .cur
//   files), which Windows draws on the desktop, not into the picture. The
//   cursor it set is drawn into each copied frame at the virtual mouse.
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <mmsystem.h>
#include <dsound.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sf_shared.h"

namespace {

SfShared* g_shared = nullptr;
HWND g_window = nullptr;
WNDPROC g_original = nullptr;
volatile LONG g_letDeactivate = 0;  // our own WM_ACTIVATEAPP(FALSE) goes through
volatile LONG g_inputUsed = 0;
volatile LONG g_vx = 0, g_vy = 0, g_buttons = 0;
volatile DWORD g_lastBlitFrame = 0;
volatile DWORD g_lastCapture = 0;
CRITICAL_SECTION g_captureLock;

void status(const char* fmt, ...) {
    if (!g_shared) return;
    char line[128];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    memcpy(g_shared->status, line, sizeof(line));
}

// ---- input ---------------------------------------------------------------

BOOL(WINAPI* realGetCursorPos)(LPPOINT) = GetCursorPos;
BOOL(WINAPI* realSetCursorPos)(int, int) = SetCursorPos;
BOOL(WINAPI* realSetForegroundWindow)(HWND) = SetForegroundWindow;

BOOL WINAPI myGetCursorPos(LPPOINT p) {
    if (!p) return FALSE;
    if (!g_inputUsed || !g_window) return realGetCursorPos(p);
    p->x = g_vx;
    p->y = g_vy;
    ClientToScreen(g_window, p);
    return TRUE;
}

BOOL WINAPI mySetCursorPos(int x, int y) {
    // Never moves the real cursor (it is The Sims 4's): the virtual one moves.
    if (!g_window) return realSetCursorPos(x, y);
    POINT p{x, y};
    ScreenToClient(g_window, &p);
    g_vx = p.x;
    g_vy = p.y;
    return TRUE;
}

SHORT buttonState(int vk) {
    if (vk == VK_LBUTTON) return (g_buttons & 1) ? (SHORT)0x8000 : 0;
    if (vk == VK_RBUTTON) return (g_buttons & 2) ? (SHORT)0x8000 : 0;
    if (vk == VK_MBUTTON) return (g_buttons & 4) ? (SHORT)0x8000 : 0;
    // Only keys The Sims 4 handed over (your keyboard while it's locked to Sims 1).
    if (g_shared && vk >= 0 && vk < 256 && g_shared->keyDown[vk]) return (SHORT)0x8000;
    return 0;
}
SHORT WINAPI myGetAsyncKeyState(int vk) { return buttonState(vk); }
SHORT WINAPI myGetKeyState(int vk) { return buttonState(vk); }
BOOL WINAPI mySetForegroundWindow(HWND) { return TRUE; }
HWND WINAPI myGetForegroundWindow() { return g_window ? g_window : GetForegroundWindow(); }

// The cursor Sims 1 asked for, and whether it hid it (ShowCursor's count).
volatile HCURSOR g_cursor = nullptr;
volatile LONG g_cursorShown = 0, g_cursorSets = 0;
HCURSOR WINAPI mySetCursor(HCURSOR c) {
    g_cursor = c;
    InterlockedIncrement(&g_cursorSets);
    return SetCursor(c);
}
int WINAPI myShowCursor(BOOL show) {
    int count = ShowCursor(show);
    g_cursorShown = count;
    return count;
}

// Draws Sims 1's cursor into a copied frame (BGRA, w x h) at the virtual mouse.
void drawCursor(uint32_t* frame, int w, int h) {
    if (g_cursorShown < 0 || !g_inputUsed) return;
    HCURSOR cursor = g_cursor;
    if (!cursor) cursor = LoadCursorA(nullptr, IDC_ARROW);
    static HCURSOR known = nullptr;
    static int hotX = 0, hotY = 0;
    if (cursor != known) {
        ICONINFO ii{};
        if (!GetIconInfo(cursor, &ii)) return;
        hotX = static_cast<int>(ii.xHotspot);
        hotY = static_cast<int>(ii.yHotspot);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        known = cursor;
    }
    const int size = 64;
    static HDC mem = nullptr;
    static uint32_t* bits = nullptr;
    if (!mem) {
        mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* p = nullptr;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!bmp) return;
        SelectObject(mem, bmp);
        bits = static_cast<uint32_t*>(p);
    }
    if (!bits) return;
    const int x0 = g_vx - hotX, y0 = g_vy - hotY;
    // The frame under it, the cursor drawn over that (its mask and colours as
    // Windows draws them), and back.
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int fx = x0 + x, fy = y0 + y;
            bits[y * size + x] = fx >= 0 && fx < w && fy >= 0 && fy < h ? frame[fy * w + fx] : 0;
        }
    GdiFlush();
    if (!DrawIconEx(mem, 0, 0, cursor, 0, 0, 0, nullptr, DI_NORMAL)) return;
    GdiFlush();
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int fx = x0 + x, fy = y0 + y;
            if (fx >= 0 && fx < w && fy >= 0 && fy < h) frame[fy * w + fx] = bits[y * size + x] | 0xFF000000u;
        }
}

// Replaces an import of `module` (by name, from user32) with `hook`.
bool patchImport(HMODULE module, const char* dll, const char* name, void* hook) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(byName->Name), name) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// Replaces `name` in every import table entry of `module`, whichever DLL it comes from.
int patchImportAny(HMODULE module, const char* name, void* hook) {
    int patched = 0;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(byName->Name), name) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<uintptr_t>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    return patched;
}

// ---- sound ----------------------------------------------------------------

typedef HRESULT(WINAPI* CreateBufferFn)(IUnknown*, LPCDSBUFFERDESC, IUnknown**, IUnknown*);
struct SoundVt {
    void** table;
    CreateBufferFn create;
};
SoundVt g_soundVts[2] = {};
int g_soundVtCount = 0;

HRESULT WINAPI myCreateSoundBuffer(IUnknown* self, LPCDSBUFFERDESC desc, IUnknown** out, IUnknown* outer) {
    void** table = *reinterpret_cast<void***>(self);
    CreateBufferFn create = nullptr;
    for (int i = 0; i < g_soundVtCount; ++i)
        if (g_soundVts[i].table == table) create = g_soundVts[i].create;
    if (!create) return E_FAIL;
    if (desc && !(desc->dwFlags & DSBCAPS_PRIMARYBUFFER)) {
        DSBUFFERDESC copy{};
        memcpy(&copy, desc, desc->dwSize < sizeof(copy) ? desc->dwSize : sizeof(copy));
        copy.dwFlags = (copy.dwFlags & ~DSBCAPS_STICKYFOCUS) | DSBCAPS_GLOBALFOCUS;
        return create(self, &copy, out, outer);
    }
    return create(self, desc, out, outer);
}

void hookSoundTable(IUnknown* ds) {
    void** table = *reinterpret_cast<void***>(ds);
    for (int i = 0; i < g_soundVtCount; ++i)
        if (g_soundVts[i].table == table) return;
    if (g_soundVtCount >= 2) return;
    g_soundVts[g_soundVtCount].table = table;
    g_soundVts[g_soundVtCount].create = reinterpret_cast<CreateBufferFn>(table[3]);
    ++g_soundVtCount;
    DWORD old;
    VirtualProtect(table + 3, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    table[3] = reinterpret_cast<void*>(myCreateSoundBuffer);
    VirtualProtect(table + 3, sizeof(void*), old, &old);
}

// Sims 1's sound keeps playing while The Sims 4 has the focus. False until dsound.dll is loaded.
bool hookSound() {
    HMODULE dsound = GetModuleHandleA("dsound.dll");
    if (!dsound) return false;
    // DirectSound mutes buffers of an app that isn't in front: tell it Sims 1 is
    // (covers the buffers made before we were loaded).
    int fg = patchImportAny(dsound, "GetForegroundWindow", reinterpret_cast<void*>(myGetForegroundWindow));
    typedef HRESULT(WINAPI * CreateFn)(LPCGUID, IUnknown**, IUnknown*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dsound, "DirectSoundCreate"));
    auto create8 = reinterpret_cast<CreateFn>(GetProcAddress(dsound, "DirectSoundCreate8"));
    IUnknown* ds = nullptr;
    if (create && SUCCEEDED(create(nullptr, &ds, nullptr)) && ds) hookSoundTable(ds);  // kept: the table must stay
    IUnknown* ds8 = nullptr;
    if (create8 && SUCCEEDED(create8(nullptr, &ds8, nullptr)) && ds8) hookSoundTable(ds8);
    status("sound: plays without focus (%d foreground checks, %d buffer tables)", fg, g_soundVtCount);
    return true;
}

void sendInput() {
    static int32_t seen = -1;
    static LONG was = 0;
    if (!g_window || g_shared->inputSeq == seen) return;
    seen = g_shared->inputSeq;
    g_vx = g_shared->mouseX;
    g_vy = g_shared->mouseY;
    LONG b = g_shared->buttons;
    g_buttons = b;
    g_inputUsed = 1;
    LPARAM at = MAKELPARAM((short)g_vx, (short)g_vy);
    WPARAM keys = ((b & 1) ? MK_LBUTTON : 0) | ((b & 2) ? MK_RBUTTON : 0) | ((b & 4) ? MK_MBUTTON : 0);
    PostMessageA(g_window, WM_MOUSEMOVE, keys, at);
    if ((b & 1) != (was & 1)) PostMessageA(g_window, (b & 1) ? WM_LBUTTONDOWN : WM_LBUTTONUP, keys, at);
    if ((b & 2) != (was & 2)) PostMessageA(g_window, (b & 2) ? WM_RBUTTONDOWN : WM_RBUTTONUP, keys, at);
    if ((b & 4) != (was & 4)) PostMessageA(g_window, (b & 4) ? WM_MBUTTONDOWN : WM_MBUTTONUP, keys, at);
    was = b;
}

// The wheel and your keyboard, as The Sims 4 handed them over.
void sendKeys() {
    static int32_t wheelSeen = 0x7fffffff, keyRead = -1;
    if (!g_window) return;
    if (wheelSeen == 0x7fffffff) wheelSeen = g_shared->wheel;
    int32_t wheel = g_shared->wheel;
    if (wheel != wheelSeen) {
        POINT p{g_vx, g_vy};
        ClientToScreen(g_window, &p);  // the wheel message carries screen coordinates
        PostMessageA(g_window, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(wheel - wheelSeen)), MAKELPARAM((short)p.x, (short)p.y));
        wheelSeen = wheel;
    }
    int32_t write = g_shared->keyWrite;
    if (keyRead < 0 || write - keyRead > SF_KEYS) keyRead = write;  // first look, or overrun: from now on
    for (; keyRead != write; ++keyRead) {
        const SfShared::Key& k = g_shared->keys[keyRead & (SF_KEYS - 1)];
        PostMessageA(g_window, k.msg, k.wparam, k.lparam);
    }
}

// ---- focus ---------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (!wp && !g_letDeactivate) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_letDeactivate) return 0;
        break;
    case WM_KILLFOCUS:
        if (!g_letDeactivate) return 0;
        break;
    case WM_NCACTIVATE:
        if (!wp) return TRUE;
        break;
    default:
        break;
    }
    return CallWindowProcA(g_original, hwnd, msg, wp, lp);
}

struct FindData {
    DWORD pid;
    HWND best;
    int area;
};
BOOL CALLBACK findWindow(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(hwnd, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > f->area) {
        f->area = area;
        f->best = hwnd;
    }
    return TRUE;
}

void takeWindow() {
    FindData f{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    if (!f.best || f.best == g_window) return;
    if (g_window && g_original) SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
    g_window = f.best;
    g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc)));
    g_shared->sims1Window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_window));
    // Behind The Sims 4, which gets its focus back.
    HWND sims4 = reinterpret_cast<HWND>(static_cast<uintptr_t>(g_shared->sims4Window));
    if (sims4 && IsWindow(sims4)) {
        SetWindowPos(g_window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        realSetForegroundWindow(sims4);
    }
    status("window %p taken (%d px), focus back to The Sims 4", (void*)g_window, f.area);
}

void setActive(bool active) {
    if (!g_window) return;
    DWORD_PTR result;
    if (active) {
        SendMessageTimeoutA(g_window, WM_ACTIVATEAPP, TRUE, 0, SMTO_ABORTIFHUNG, 500, &result);
        SendMessageTimeoutA(g_window, WM_ACTIVATE, WA_ACTIVE, 0, SMTO_ABORTIFHUNG, 500, &result);
    } else {
        InterlockedExchange(&g_letDeactivate, 1);
        SendMessageTimeoutA(g_window, WM_ACTIVATEAPP, FALSE, 0, SMTO_ABORTIFHUNG, 500, &result);
        InterlockedExchange(&g_letDeactivate, 0);
    }
    status("Sims 1 %s", active ? "running (on the screen)" : "told it lost focus (paused)");
}

// ---- picture -------------------------------------------------------------

uint32_t g_lut16[65536];
DWORD g_lutMasks[3] = {0, 0, 0};

int shiftOf(DWORD mask) {
    int s = 0;
    if (!mask) return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        ++s;
    }
    return s;
}
uint32_t expand(DWORD v, DWORD mask) {
    if (!mask) return 0;
    int s = shiftOf(mask);
    DWORD m = mask >> s;
    return ((v & mask) >> s) * 255 / m;
}
uint32_t toBgra(DWORD v, const DDPIXELFORMAT& pf) {
    return 0xFF000000u | (expand(v, pf.dwRBitMask) << 16) | (expand(v, pf.dwGBitMask) << 8) | expand(v, pf.dwBBitMask);
}

// Copies a locked surface (part `r` of it) to the next shared buffer.
void publish(const DDSURFACEDESC2& d, const RECT& r, IDirectDrawPalette* palette, int source) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (w > SF_MAX_W) w = SF_MAX_W;
    if (h > SF_MAX_H) h = SF_MAX_H;
    const DDPIXELFORMAT& pf = d.ddpfPixelFormat;
    int bits = pf.dwRGBBitCount;
    uint32_t pal[256];
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
        PALETTEENTRY e[256] = {};
        if (!palette || FAILED(palette->GetEntries(0, 0, 256, e))) return;
        for (int i = 0; i < 256; ++i) pal[i] = 0xFF000000u | (e[i].peRed << 16) | (e[i].peGreen << 8) | e[i].peBlue;
        bits = 8;
    } else if (!(pf.dwFlags & DDPF_RGB)) {
        return;
    }
    if (bits == 16 && (g_lutMasks[0] != pf.dwRBitMask || g_lutMasks[1] != pf.dwGBitMask || g_lutMasks[2] != pf.dwBBitMask)) {
        for (int v = 0; v < 65536; ++v) g_lut16[v] = toBgra(v, pf);
        g_lutMasks[0] = pf.dwRBitMask;
        g_lutMasks[1] = pf.dwGBitMask;
        g_lutMasks[2] = pf.dwBBitMask;
    }
    int next = (g_shared->frameSeq + 1) & 1;
    uint32_t* out = reinterpret_cast<uint32_t*>(g_shared->pixels[next]);
    const uint8_t* base = static_cast<const uint8_t*>(d.lpSurface);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = base + (r.top + y) * d.lPitch;
        uint32_t* o = out + y * w;
        switch (bits) {
        case 8:
            for (int x = 0; x < w; ++x) o[x] = pal[row[r.left + x]];
            break;
        case 16: {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + r.left;
            for (int x = 0; x < w; ++x) o[x] = g_lut16[p[x]];
            break;
        }
        case 24:
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = row + (r.left + x) * 3;
                o[x] = toBgra(p[0] | (p[1] << 8) | (p[2] << 16), pf);
            }
            break;
        case 32: {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(row) + r.left;
            if (pf.dwRBitMask == 0xFF0000 && pf.dwGBitMask == 0xFF00 && pf.dwBBitMask == 0xFF)
                for (int x = 0; x < w; ++x) o[x] = p[x] | 0xFF000000u;
            else
                for (int x = 0; x < w; ++x) o[x] = toBgra(p[x], pf);
            break;
        }
        default:
            return;
        }
    }
    drawCursor(out, w, h);
    g_shared->width = w;
    g_shared->height = h;
    g_shared->source = source;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->frameSeq));
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->frames));
}

bool isPrimary(IUnknown* surface) {
    IDirectDrawSurface7* s7 = nullptr;
    if (!surface || FAILED(surface->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&s7))) || !s7)
        return false;
    DDSCAPS2 caps{};
    bool primary = SUCCEEDED(s7->GetCaps(&caps)) && (caps.dwCaps & DDSCAPS_PRIMARYSURFACE);
    s7->Release();
    return primary;
}

// A frame went to the screen from `source` (part `r`, or all of it): copy it.
void grab(IUnknown* source, const RECT* r, IUnknown* primary) {
    if (!g_shared || !source) return;
    DWORD now = GetTickCount();
    g_lastBlitFrame = now;
    if (!g_shared->active || now - g_lastCapture < 30) return;
    if (!TryEnterCriticalSection(&g_captureLock)) return;
    g_lastCapture = now;
    IDirectDrawSurface7* s7 = nullptr;
    if (SUCCEEDED(source->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&s7))) && s7) {
        DDSURFACEDESC2 d{};
        d.dwSize = sizeof(d);
        if (SUCCEEDED(s7->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR, nullptr))) {
            RECT whole{0, 0, (LONG)d.dwWidth, (LONG)d.dwHeight};
            RECT part = r ? *r : whole;
            IntersectRect(&part, &part, &whole);
            IDirectDrawPalette* palette = nullptr;
            if (d.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) {
                if (FAILED(s7->GetPalette(&palette)) && primary) {
                    IDirectDrawSurface7* p7 = nullptr;
                    if (SUCCEEDED(primary->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&p7))) && p7) {
                        p7->GetPalette(&palette);
                        p7->Release();
                    }
                }
            }
            publish(d, part, palette, 1);
            if (palette) palette->Release();
            s7->Unlock(nullptr);
        }
        s7->Release();
    }
    LeaveCriticalSection(&g_captureLock);
}

// The primary surfaces seen (a few; the game may make it again).
IUnknown* g_primaries[8] = {};
IUnknown* g_notPrimary[64] = {};
int g_notPrimaryNext = 0;
DWORD g_cacheCleared = 0;

bool knownPrimary(IUnknown* s) {
    DWORD now = GetTickCount();
    if (now - g_cacheCleared > 3000) {
        memset(g_notPrimary, 0, sizeof(g_notPrimary));
        memset(g_primaries, 0, sizeof(g_primaries));
        g_cacheCleared = now;
    }
    for (IUnknown* p : g_primaries)
        if (p == s) return true;
    for (IUnknown* p : g_notPrimary)
        if (p == s) return false;
    if (isPrimary(s)) {
        for (IUnknown*& p : g_primaries)
            if (!p) {
                p = s;
                break;
            }
        return true;
    }
    g_notPrimary[g_notPrimaryNext++ & 63] = s;
    return false;
}

typedef HRESULT(WINAPI* BltFn)(IUnknown*, LPRECT, IUnknown*, LPRECT, DWORD, void*);
typedef HRESULT(WINAPI* BltFastFn)(IUnknown*, DWORD, DWORD, IUnknown*, LPRECT, DWORD);
typedef HRESULT(WINAPI* FlipFn)(IUnknown*, IUnknown*, DWORD);

struct Vt {
    void** table;
    BltFn blt;
    BltFastFn bltFast;
    FlipFn flip;
};
Vt g_vts[5] = {};
int g_vtCount = 0;

Vt* vtOf(IUnknown* self) {
    void** table = *reinterpret_cast<void***>(self);
    for (int i = 0; i < g_vtCount; ++i)
        if (g_vts[i].table == table) return &g_vts[i];
    return nullptr;
}

HRESULT WINAPI myBlt(IUnknown* self, LPRECT dest, IUnknown* src, LPRECT srcRect, DWORD flags, void* fx) {
    Vt* vt = vtOf(self);
    if (!vt) return E_FAIL;
    if (src && knownPrimary(self)) grab(src, srcRect, self);
    return vt->blt(self, dest, src, srcRect, flags, fx);
}

HRESULT WINAPI myBltFast(IUnknown* self, DWORD x, DWORD y, IUnknown* src, LPRECT srcRect, DWORD flags) {
    Vt* vt = vtOf(self);
    if (!vt) return E_FAIL;
    if (src && knownPrimary(self)) grab(src, srcRect, self);
    return vt->bltFast(self, x, y, src, srcRect, flags);
}

HRESULT WINAPI myFlip(IUnknown* self, IUnknown* target, DWORD flags) {
    Vt* vt = vtOf(self);
    if (!vt) return E_FAIL;
    HRESULT hr = vt->flip(self, target, flags);
    // After the flip the front buffer holds the new frame.
    if (SUCCEEDED(hr)) grab(self, nullptr, self);
    return hr;
}

void hookTable(IUnknown* surface) {
    void** table = *reinterpret_cast<void***>(surface);
    for (int i = 0; i < g_vtCount; ++i)
        if (g_vts[i].table == table) return;
    if (g_vtCount >= 5) return;
    Vt& vt = g_vts[g_vtCount];
    vt.table = table;
    vt.blt = reinterpret_cast<BltFn>(table[5]);
    vt.bltFast = reinterpret_cast<BltFastFn>(table[7]);
    vt.flip = reinterpret_cast<FlipFn>(table[11]);
    ++g_vtCount;  // before the table points at us
    DWORD old;
    VirtualProtect(table + 5, 7 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    table[5] = reinterpret_cast<void*>(myBlt);
    table[7] = reinterpret_cast<void*>(myBltFast);
    table[11] = reinterpret_cast<void*>(myFlip);
    VirtualProtect(table + 5, 7 * sizeof(void*), old, &old);
}

// Every IDirectDrawSurface version's method table, from a small surface of our own.
bool hookDirectDraw() {
    HMODULE ddraw = GetModuleHandleA("ddraw.dll");
    if (!ddraw) return false;
    typedef HRESULT(WINAPI * CreateFn)(GUID*, LPDIRECTDRAW*, IUnknown*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(ddraw, "DirectDrawCreate"));
    if (!create) return false;
    IDirectDraw* dd = nullptr;
    if (FAILED(create(nullptr, &dd, nullptr)) || !dd) {
        status("DirectDrawCreate failed: PrintWindow only");
        return true;
    }
    IDirectDraw7* dd7 = nullptr;
    if (SUCCEEDED(dd->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&dd7))) && dd7) {
        dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        DDSURFACEDESC2 d{};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = d.dwHeight = 16;
        IDirectDrawSurface7* s7 = nullptr;
        if (SUCCEEDED(dd7->CreateSurface(&d, &s7, nullptr)) && s7) {
            const IID* iids[] = {&IID_IDirectDrawSurface, &IID_IDirectDrawSurface2, &IID_IDirectDrawSurface3,
                                 &IID_IDirectDrawSurface4, &IID_IDirectDrawSurface7};
            for (const IID* iid : iids) {
                IUnknown* s = nullptr;
                if (SUCCEEDED(s7->QueryInterface(*iid, reinterpret_cast<void**>(&s))) && s) {
                    hookTable(s);
                    s->Release();
                }
            }
            // Kept: the method tables must stay valid.
        }
        // dd7 kept too, for the same reason.
    }
    status("DirectDraw blits hooked (%d method tables)", g_vtCount);
    return true;
}

// Fallback: the window's picture from the compositor.
void printWindowFrame() {
    if (!g_window) return;
    RECT rc;
    GetClientRect(g_window, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    if (w > SF_MAX_W) w = SF_MAX_W;
    if (h > SF_MAX_H) h = SF_MAX_H;
    static HDC mem = nullptr;
    static HBITMAP bmp = nullptr;
    static void* bits = nullptr;
    static int bw = 0, bh = 0;
    if (!mem) mem = CreateCompatibleDC(nullptr);
    if (w != bw || h != bh) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return;
        SelectObject(mem, bmp);
        bw = w;
        bh = h;
    }
    if (!PrintWindow(g_window, mem, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */)) return;
    GdiFlush();
    int next = (g_shared->frameSeq + 1) & 1;
    uint32_t* out = reinterpret_cast<uint32_t*>(g_shared->pixels[next]);
    const uint32_t* in = static_cast<const uint32_t*>(bits);
    for (int i = 0; i < w * h; ++i) out[i] = in[i] | 0xFF000000u;
    drawCursor(out, w, h);
    g_shared->width = w;
    g_shared->height = h;
    g_shared->source = 2;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->frameSeq));
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->frames));
}

DWORD WINAPI worker(void*) {
    HANDLE mapping = nullptr;
    for (int i = 0; i < 100 && !mapping; ++i) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SF_MAPPING_NAME);
        if (!mapping) Sleep(100);
    }
    if (!mapping) return 0;
    g_shared = static_cast<SfShared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SfShared)));
    if (!g_shared || g_shared->magic != SF_MAGIC || g_shared->size != sizeof(SfShared)) return 0;
    g_shared->hookPid = GetCurrentProcessId();
    status("loaded");

    HMODULE exe = GetModuleHandleA(nullptr);
    patchImport(exe, "user32.dll", "GetCursorPos", reinterpret_cast<void*>(myGetCursorPos));
    patchImport(exe, "user32.dll", "SetCursorPos", reinterpret_cast<void*>(mySetCursorPos));
    patchImport(exe, "user32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(myGetAsyncKeyState));
    patchImport(exe, "user32.dll", "GetKeyState", reinterpret_cast<void*>(myGetKeyState));
    patchImport(exe, "user32.dll", "SetForegroundWindow", reinterpret_cast<void*>(mySetForegroundWindow));
    patchImport(exe, "user32.dll", "SetCursor", reinterpret_cast<void*>(mySetCursor));
    patchImport(exe, "user32.dll", "ShowCursor", reinterpret_cast<void*>(myShowCursor));

    bool hooked = false, soundHooked = false;
    int wasActive = -1;
    DWORD lastWindowLook = 0, lastPrint = 0;
    for (;;) {
        DWORD now = GetTickCount();
        g_shared->hookAlive = static_cast<int32_t>(now);
        if (!hooked) hooked = hookDirectDraw();
        if (!soundHooked) soundHooked = hookSound();
        if (now - lastWindowLook > 1000) {
            lastWindowLook = now;
            takeWindow();
        }
        int active = g_shared->active ? 1 : 0;
        if (g_window && active != wasActive) {
            if (wasActive >= 0 || !active) setActive(active != 0);
            wasActive = active;
        }
        sendInput();
        sendKeys();
        static LONG reportedSets = -1;
        static DWORD lastCursorReport = 0;
        if (g_inputUsed && now - lastCursorReport > 10000 && (g_cursorSets > 0) != (reportedSets > 0)) {
            lastCursorReport = now;
            reportedSets = g_cursorSets;
            status("cursor: Sims 1 set it %ld times, shown count %ld; drawn into the picture", g_cursorSets,
                   g_cursorShown);
        }
        if (g_shared->quit == 1 && g_window) {
            g_shared->quit = 2;
            PostMessageA(g_window, WM_CLOSE, 0, 0);
        }
        // No blit seen for 2 s while on show: take the picture from the compositor.
        if (active && now - g_lastBlitFrame > 2000 && now - lastPrint >= 50) {
            lastPrint = now;
            printWindowFrame();
        }
        Sleep(4);
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_captureLock);
        CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
