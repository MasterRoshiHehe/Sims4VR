#include "claudescreen.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "linearboard.h"
#include "log.h"

namespace s4vr::claudescreen {
namespace {

constexpr int kMaxCaptureW = 3840, kMaxCaptureH = 2160;
constexpr ULONGLONG kUnseenMs = 1000;  // nobody drew the slot for this long: no pictures taken

// One window on one screen. Slot 0 is Claude, slot 1 Discord (a viewer).
struct Inst {
    int slot = 0;
    const char* name = "Claude screen";

    std::mutex mutex;
    std::atomic<bool> on{false};
    float v[11] = {};
    std::wstring exe, title;
    std::atomic<int> fps{20}, maxWidth{2560};
    std::atomic<bool> interactive{true};
    std::atomic<HWND> window{nullptr};  // the window on show (capture thread)
    std::atomic<ULONGLONG> viewedAt{0};  // the last time the render thread asked for the picture
    std::atomic<ULONGLONG> returnFrontAt{0};  // after a click: the game gets the front back then
    bool threadStarted = false;

    // The latest picture (BGRA), written by the capture thread.
    std::mutex frameMutex;
    std::vector<uint32_t> frame;
    int frameW = 0, frameH = 0, frameSeq = 0;

    // Render thread only.
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Device* device = nullptr;
    int texW = 0, texH = 0, uploadedSeq = -1;

    // The laser's mouse (present thread).
    int buttons = 0;
    POINT lastSet{-1, -1};

    bool claude() const { return slot == kClaude; }
};
Inst g_inst[kSlots];
std::atomic<HWND> g_game{nullptr};
std::once_flag g_names;

Inst& inst(int slot) {
    std::call_once(g_names, [] {
        g_inst[kClaude].slot = kClaude;
        g_inst[kClaude].name = "Claude screen";
        g_inst[kDiscord].slot = kDiscord;
        g_inst[kDiscord].name = "Discord screen";
        g_inst[kDiscord].fps = 30;
        g_inst[kDiscord].maxWidth = 1920;
        g_inst[kDiscord].interactive = true;
    });
    return g_inst[slot < 0 || slot >= kSlots ? 0 : slot];
}

std::wstring widen(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// The app's main window: its largest visible top-level window (of its process; with `title` in its title).
struct Find {
    int slot = 0;
    std::wstring title;
    HWND best = nullptr;
    long area = 0;
};
BOOL CALLBACK findWindowProc(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<Find*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0) return TRUE;
    if (!f->title.empty()) {
        wchar_t text[256] = {};
        GetWindowTextW(hwnd, text, 256);
        if (lower(text).find(f->title) == std::wstring::npos) return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring p = lower(path);
        size_t slash = p.find_last_of(L'\\');
        std::wstring base = slash == std::wstring::npos ? p : p.substr(slash + 1);
        if (f->slot == kClaude)
            match = base == L"claude.exe";
        else  // Discord, its public test and canary builds
            match = base.size() >= 11 && base.compare(0, 7, L"discord") == 0 && base.compare(base.size() - 4, 4, L".exe") == 0;
    }
    CloseHandle(process);
    if (!match) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    long w = r.right - r.left, h = r.bottom - r.top;
    if (f->slot == kDiscord && (w < 200 || h < 150) && !IsIconic(hwnd)) return TRUE;  // helper windows
    long area = w * h;
    if (IsIconic(hwnd)) area = 1;  // a minimised one still counts, last
    if (area > f->area) {
        f->area = area;
        f->best = hwnd;
    }
    return TRUE;
}
HWND findWindow(Inst& s) {
    Find f;
    f.slot = s.slot;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        f.title = lower(s.title);
    }
    EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

void launch(Inst& s, const std::wstring& configured) {
    std::wstring exe = configured;
    wchar_t local[MAX_PATH] = {};
    const bool haveLocal = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0;
    if (s.claude()) {
        if (exe.empty() && haveLocal) exe = std::wstring(local) + L"\\AnthropicClaude\\claude.exe";
        if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            LOG("%s: starting %ls (%s)", s.name, exe.c_str(), reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
            return;
        }
        // Installed another way: its link handler starts it.
        HINSTANCE r = ShellExecuteW(nullptr, L"open", L"claude://", nullptr, nullptr, SW_SHOWNORMAL);
        LOG("%s: the app isn't at \"%ls\"; asked Windows to open claude:// (%s)", s.name, exe.c_str(),
            reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
        return;
    }
    // Discord: its updater starts the current build.
    if (exe.empty() && haveLocal) exe = std::wstring(local) + L"\\Discord\\Update.exe";
    if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const bool updater = lower(exe).find(L"update.exe") != std::wstring::npos;
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), updater ? L"--processStart Discord.exe" : nullptr,
                                    nullptr, SW_SHOWMINNOACTIVE);
        LOG("%s: starting %ls (%s)", s.name, exe.c_str(), reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
        return;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"discord://", nullptr, nullptr, SW_SHOWMINNOACTIVE);
    LOG("%s: Discord isn't at \"%ls\"; asked Windows to open discord:// (%s)", s.name, exe.c_str(),
        reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
}

// Brings `window` to the front from this process (the game, in front now or
// recently): the usual Alt-key nudge lets SetForegroundWindow through.
bool toFront(HWND window) {
    if (!window || !IsWindow(window)) return false;
    if (GetForegroundWindow() == window) return true;
    INPUT alt[2] = {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(window);
    if (GetForegroundWindow() == window) return true;
    // Windows refused (the other window, Discord's, is in front and owned by another process): join its input
    // queue for the moment, which lets the front change hands (692 log: "Windows refused to bring it to the front").
    HWND front = GetForegroundWindow();
    DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = frontThread && frontThread != me && AttachThreadInput(me, frontThread, TRUE);
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    if (attached) AttachThreadInput(me, frontThread, FALSE);
    return GetForegroundWindow() == window;
}

// Draws the mouse cursor (as Windows shows it now) into a picture of `window`'s client area.
void drawCursor(HDC dc, HWND window) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    POINT p = ci.ptScreenPos;
    ScreenToClient(window, &p);
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    int hx = static_cast<int>(ii.xHotspot), hy = static_cast<int>(ii.yHotspot);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DrawIconEx(dc, p.x - hx, p.y - hy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

// A 32-bit top-down bitmap of w x h selected into `dc`.
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    void init() { dc = CreateCompatibleDC(nullptr); }
    bool size(int nw, int nh) {
        if (nw == w && nh == h) return bmp != nullptr;
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) SelectObject(dc, bmp);
        w = bmp ? nw : 0;
        h = bmp ? nh : 0;
        return bmp != nullptr;
    }
};

// Every sampled pixel black: a window the capture gets nothing of.
bool looksBlack(const uint32_t* px, int w, int h) {
    for (int j = 1; j < 9; ++j)
        for (int i = 1; i < 9; ++i)
            if (px[static_cast<size_t>(h * j / 10) * w + static_cast<size_t>(w * i / 10)] & 0x00FFFFFFu) return false;
    return true;
}

void captureThread(Inst* sp) {
    Inst& s = *sp;
    Dib full, scaled;
    full.init();
    scaled.init();
    SetStretchBltMode(scaled.dc, COLORONCOLOR);  // HALFTONE took 48 ms a picture (692 log); the GPU's mip chain smooths it
    SetBrushOrgEx(scaled.dc, 0, 0, nullptr);
    std::vector<uint32_t> scratch;
    HWND window = nullptr;
    bool placed = false, launched = false, fronted = false, clipped = false, wasIconic = false;
    WINDOWPLACEMENT saved{};
    saved.length = sizeof(saved);
    DWORD lastLook = 0, startedAt = 0;
    int captures = 0, failures = 0, blackRun = 0;
    ULONGLONG raisedAt = 0;
    bool bitBlt = false, bitBltLogged = false;
    auto release = [&]() {
        if (clipped) ClipCursor(nullptr);
        clipped = false;
        if (window && placed && IsWindow(window)) {
            if (s.claude())
                SetWindowPlacement(window, &saved);
            else if (wasIconic)
                ShowWindow(window, SW_SHOWMINNOACTIVE);  // as it was: minimised
        }
        if (placed) LOG("%s: off; the window has its old place back", s.name);
        if (window && bitBlt && IsWindow(window))  // back behind the game
            SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        placed = fronted = launched = wasIconic = bitBlt = bitBltLogged = false;
        blackRun = 0;
        window = nullptr;
        s.window = nullptr;
        if (s.claude() || s.buttons) {
            if (HWND game = g_game.load()) toFront(game);
        }
    };
    for (;;) {
        DWORD now = GetTickCount();
        if (!s.on) {
            if (window || placed || launched) release();
            Sleep(100);
            continue;
        }
        // The game's front back, a moment after a click into the window.
        ULONGLONG back = s.returnFrontAt.load();
        if (back && GetTickCount64() >= back) {
            s.returnFrontAt = 0;
            if (HWND game = g_game.load()) toFront(game);
        }
        if (!window || !IsWindow(window)) {
            if (now - lastLook < 500) {
                Sleep(50);
                continue;
            }
            lastLook = now;
            window = findWindow(s);
            if (!window) {
                if (!launched) {
                    std::wstring exe;
                    {
                        std::lock_guard<std::mutex> lock(s.mutex);
                        exe = s.exe;
                    }
                    launch(s, exe);
                    launched = true;
                    startedAt = now;
                } else if (now - startedAt > 30000 && now - startedAt < 30600) {
                    LOG("%s: no window 30 s after starting the app", s.name);
                }
                continue;
            }
            LOG("%s: found the window %p", s.name, static_cast<void*>(window));
            s.window = window;
            placed = false;
        }
        if (!placed) {
            if (s.claude()) {
                // Its place kept for later; restored to a normal window shaped like the screen.
                GetWindowPlacement(window, &saved);
                float aspect;
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    aspect = s.v[10] > 0 ? s.v[9] / s.v[10] : 16.0f / 9.0f;
                }
                if (IsIconic(window) || IsZoomed(window)) ShowWindow(window, SW_RESTORE);
                int h = 900, w = std::max(700, std::min(2400, static_cast<int>(h * aspect + 0.5f)));
                RECT want{0, 0, w, h};
                AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                                   static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
                RECT r;
                GetWindowRect(window, &r);
                HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(monitor, &mi);
                int ww = want.right - want.left, wh = want.bottom - want.top;
                int x = std::max(static_cast<int>(mi.rcWork.left), std::min(static_cast<int>(r.left), static_cast<int>(mi.rcWork.right) - ww));
                int y = std::max(static_cast<int>(mi.rcWork.top), std::min(static_cast<int>(r.top), static_cast<int>(mi.rcWork.bottom) - wh));
                SetWindowPos(window, HWND_TOP, x, y, ww, wh, SWP_NOACTIVATE);
                LOG("%s: window sized %dx%d for a %.2f:1 screen", s.name, w, h, aspect);
            } else {
                // A viewer: the window stays as it is (a stream you set up on your desktop), only brought back
                // from the taskbar without taking the front, since nothing is drawn for a minimised window.
                wasIconic = IsIconic(window) != FALSE;
                if (wasIconic) ShowWindow(window, SW_SHOWNOACTIVATE);
                RECT r;
                GetClientRect(window, &r);
                LOG("%s: viewing the window as it is, %ldx%ld%s", s.name, r.right, r.bottom,
                    wasIconic ? " (restored from the taskbar)" : "");
            }
            placed = true;
        }
        // Nobody is looking at the screen: no pictures (the last one stays up).
        if (s.frameW > 0 && GetTickCount64() - s.viewedAt.load() > kUnseenMs) {
            Sleep(150);
            continue;
        }
        if (s.claude()) {
            bool front = GetForegroundWindow() == window;
            if (!front && !fronted) {
                fronted = true;  // once: after that you may click elsewhere (it comes back with the laser)
                front = toFront(window);
                LOG("%s: the Claude window %s the front (your mouse and keyboard type into it)", s.name,
                    front ? "has" : "didn't get");
            }
            // The real mouse stays on the window while it's in front.
            if (front) {
                RECT wr;
                GetWindowRect(window, &wr);
                ClipCursor(&wr);
                clipped = true;
            } else if (clipped) {
                ClipCursor(nullptr);
                clipped = false;
            }
        }
        RECT rc;
        GetClientRect(window, &rc);
        int w = std::min(static_cast<int>(rc.right), kMaxCaptureW), h = std::min(static_cast<int>(rc.bottom), kMaxCaptureH);
        if (w > 0 && h > 0 && full.size(w, h)) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            bool ok;
            if (bitBlt) {
                // PrintWindow gave black pictures (a stream drawn by the graphics card): the window as it is on the
                // screen, which needs nothing in front of it. The game's own window is, so Discord goes on top of
                // it without taking the focus (the game keeps the keyboard; 700 log 20:39: black at once).
                if (GetTickCount64() - raisedAt > 500) {
                    raisedAt = GetTickCount64();
                    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                HDC wdc = GetDC(window);
                ok = wdc && BitBlt(full.dc, 0, 0, w, h, wdc, 0, 0, SRCCOPY | CAPTUREBLT);
                if (wdc) ReleaseDC(window, wdc);
            } else {
                ok = PrintWindow(window, full.dc, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */) != FALSE;
            }
            if (ok) {
                if (s.claude() || s.interactive) drawCursor(full.dc, window);
                GdiFlush();
                // Wider than allowed: scaled down here, on this thread.
                const int cap = std::max(320, s.maxWidth.load());
                int ow = w, oh = h;
                const uint32_t* in = static_cast<const uint32_t*>(full.bits);
                if (w > cap) {
                    ow = cap;
                    oh = std::max(1, static_cast<int>(static_cast<long long>(h) * cap / w));
                    if (scaled.size(ow, oh)) {
                        StretchBlt(scaled.dc, 0, 0, ow, oh, full.dc, 0, 0, w, h, SRCCOPY);
                        GdiFlush();
                        in = static_cast<const uint32_t*>(scaled.bits);
                    } else {
                        ow = w;
                        oh = h;
                    }
                }
                scratch.resize(static_cast<size_t>(ow) * oh);
                for (size_t i = 0; i < scratch.size(); ++i) scratch[i] = in[i] | 0xFF000000u;
                // A window the capture gets nothing of (a GPU-drawn stream): the on-screen copy instead.
                if (!s.claude() && !bitBlt) {
                    if (looksBlack(scratch.data(), ow, oh)) {
                        if (++blackRun == 15) {
                            bitBlt = true;
                            if (!bitBltLogged) {
                                bitBltLogged = true;
                                LOG("%s: PrintWindow pictures are black; copying from the screen instead (the Discord "
                                    "window is put on top of the game's, without the focus)", s.name);
                            }
                        }
                    } else {
                        blackRun = 0;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(s.frameMutex);
                    s.frame.swap(scratch);
                    s.frameW = ow;
                    s.frameH = oh;
                    ++s.frameSeq;
                }
                if (captures++ == 0) LOG("%s: picture %dx%d (window %dx%d)", s.name, ow, oh, w, h);
                // What a picture costs (user 2026-10-15 00:53Z asked about the
                // performance): logged every 30 s while a window is on a screen.
                static LARGE_INTEGER freq{};
                static double total[kSlots] = {}, most[kSlots] = {};
                static int pictures[kSlots] = {};
                static DWORD since[kSlots] = {};
                if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
                if (!since[s.slot]) since[s.slot] = GetTickCount();
                QueryPerformanceCounter(&t1);
                double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
                total[s.slot] += ms;
                most[s.slot] = std::max(most[s.slot], ms);
                ++pictures[s.slot];
                if (GetTickCount() - since[s.slot] >= 30000) {
                    LOG("%s: %d pictures of %dx%d in the last %lu s, %.1f ms each on average, %.1f ms at most (its own "
                        "thread, not the game's)",
                        s.name, pictures[s.slot], ow, oh, (GetTickCount() - since[s.slot]) / 1000,
                        total[s.slot] / pictures[s.slot], most[s.slot]);
                    total[s.slot] = most[s.slot] = 0;
                    pictures[s.slot] = 0;
                    since[s.slot] = GetTickCount();
                }
            } else if (failures++ < 5) {
                LOG("%s: no picture of the window (%lu)", s.name, GetLastError());
            }
        }
        DWORD frameMs = 1000u / static_cast<DWORD>(std::max(1, std::min(60, s.fps.load())));
        DWORD spent = GetTickCount() - now;
        Sleep(spent < frameMs ? frameMs - spent : 1);
    }
}

}  // namespace

void set(int on, const float v[11], const char* exe, int slot, const char* title, int fps, int maxWidth, int interactive) {
    if (slot == kLinear) return linearboard::set(on, v, exe, title, fps, maxWidth);
    Inst& s = inst(slot);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (on && v) {
            if (s.on && std::fabs(v[0] - s.v[0]) + std::fabs(v[1] - s.v[1]) + std::fabs(v[2] - s.v[2]) > 0.05f)
                LOG("%s: moved to another screen at %.2f %.2f %.2f, %.2f x %.2f", s.name, v[0], v[1], v[2], v[9], v[10]);
            std::memcpy(s.v, v, sizeof(s.v));
        }
        s.exe = widen(exe);
        s.title = widen(title);
        if (on) {
            s.fps = std::max(1, std::min(60, fps));
            s.maxWidth = std::max(320, std::min(kMaxCaptureW, maxWidth));
            s.interactive = interactive != 0;
        }
        if (on && !s.threadStarted) {
            s.threadStarted = true;
            std::thread(captureThread, &s).detach();
        }
    }
    if (on != static_cast<int>(s.on.load())) {
        LOG("%s: %s", s.name, on ? "on a screen" : "off the screen");
        if (on) {
            std::lock_guard<std::mutex> lock(s.frameMutex);
            s.frameW = s.frameH = 0;  // no old picture
            s.viewedAt = GetTickCount64();
        }
    }
    s.on = on != 0;
    if (!on && s.buttons) mouse(0.5f, 0.5f, 0, slot);
}

bool on(int slot) { return slot == kLinear ? linearboard::on() : inst(slot).on.load(); }

bool interactive(int slot) {
    if (slot == kLinear) return linearboard::on();
    Inst& s = inst(slot);
    return s.on && s.interactive;
}

bool screen(float v[11], int slot) {
    if (slot == kLinear) return linearboard::screen(v);
    Inst& s = inst(slot);
    if (!s.on) return false;
    std::lock_guard<std::mutex> lock(s.mutex);
    std::memcpy(v, s.v, sizeof(s.v));
    return true;
}

bool frameSize(int& w, int& h, int slot) {
    if (slot == kLinear) return linearboard::frameSize(w, h);
    Inst& s = inst(slot);
    std::lock_guard<std::mutex> lock(s.frameMutex);
    w = s.frameW;
    h = s.frameH;
    return w > 0 && h > 0;
}

ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context, int slot) {
    if (slot == kLinear) return linearboard::frameView(context);
    Inst& s = inst(slot);
    if (!context) return nullptr;
    s.viewedAt = GetTickCount64();
    std::lock_guard<std::mutex> lock(s.frameMutex);
    int w = s.frameW, h = s.frameH;
    if (w <= 0 || h <= 0) return nullptr;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return nullptr;
    device->Release();  // the context holds it
    if (device != s.device || w != s.texW || h != s.texH) {
        if (s.view) s.view->Release();
        if (s.texture) s.texture->Release();
        s.view = nullptr;
        s.texture = nullptr;
        s.device = device;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        // A full mip chain: seen from a few metres the window is many texels per pixel, and
        // one level shimmered into unreadable text (user 2026-10-15 17:13Z).
        d.MipLevels = 0;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &s.texture)) ||
            FAILED(device->CreateShaderResourceView(s.texture, nullptr, &s.view))) {
            if (s.texture) s.texture->Release();
            s.texture = nullptr;
            static int failed = 0;
            if (failed++ < 3) LOG("%s: no %dx%d texture", s.name, w, h);
            return nullptr;
        }
        s.texW = w;
        s.texH = h;
        s.uploadedSeq = -1;
    }
    if (s.frameSeq != s.uploadedSeq) {
        context->UpdateSubresource(s.texture, 0, nullptr, s.frame.data(), w * 4, 0);
        context->GenerateMips(s.view);
        s.uploadedSeq = s.frameSeq;
    }
    return s.view;
}

void mouse(float u, float v, int buttons, int slot) {
    if (slot == kLinear) return linearboard::pointer(u, v, buttons);
    Inst& s = inst(slot);
    if (!s.claude() && !s.interactive) return;
    HWND window = s.window.load();
    if (!window || !IsWindow(window)) return;
    RECT rc;
    GetClientRect(window, &rc);
    POINT p{static_cast<LONG>(std::max(0.0f, std::min(1.0f, u)) * (rc.right - 1)),
            static_cast<LONG>(std::max(0.0f, std::min(1.0f, v)) * (rc.bottom - 1))};
    // Only when the laser moved, so the real mouse still works.
    if (std::abs(p.x - s.lastSet.x) > 1 || std::abs(p.y - s.lastSet.y) > 1) {
        s.lastSet = p;
        POINT pt = p;
        ClientToScreen(window, &pt);
        SetCursorPos(pt.x, pt.y);
    }
    int changed = buttons ^ s.buttons;
    if (changed) {
        if (buttons && GetForegroundWindow() != window) toFront(window);
        INPUT in{};
        in.type = INPUT_MOUSE;
        if (changed & 1) {
            in.mi.dwFlags = (buttons & 1) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &in, sizeof(in));
        }
        if (changed & 2) {
            in.mi.dwFlags = (buttons & 2) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            SendInput(1, &in, sizeof(in));
        }
        // A viewer doesn't keep the front: the game gets it back a moment after the click.
        if (!s.claude() && !buttons) s.returnFrontAt = GetTickCount64() + 150;
        s.buttons = buttons;
    }
}

bool keepsGameFocused() { return inst(kClaude).on || (inst(kDiscord).on && inst(kDiscord).interactive); }

void setGameWindow(HWND game) { g_game = game; }

}  // namespace s4vr::claudescreen
