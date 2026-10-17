#include "claudescreen.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"

namespace s4vr::claudescreen {
namespace {

constexpr int kMaxW = 2560, kMaxH = 1600;
constexpr DWORD kFrameMs = 50;  // 20 pictures a second

std::mutex g_mutex;
std::atomic<bool> g_on{false};
float g_v[11] = {};
std::wstring g_exe;
std::atomic<HWND> g_game{nullptr};
std::atomic<HWND> g_window{nullptr};  // the Claude window on show (capture thread)
bool g_threadStarted = false;

// The latest picture (BGRA), written by the capture thread.
std::mutex g_frameMutex;
std::vector<uint32_t> g_frame;
int g_frameW = 0, g_frameH = 0, g_frameSeq = 0;

// Render thread only.
ID3D11Texture2D* g_texture = nullptr;
ID3D11ShaderResourceView* g_view = nullptr;
ID3D11Device* g_device = nullptr;
int g_texW = 0, g_texH = 0, g_uploadedSeq = -1;

// The laser's mouse (present thread).
int g_buttons = 0;
POINT g_lastSet{-1, -1};

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

// The Claude app's main window: its largest visible top-level window.
struct Find {
    HWND best = nullptr;
    long area = 0;
};
BOOL CALLBACK findClaude(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<Find*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool claude = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring p = lower(path);
        claude = p.size() >= 11 && p.compare(p.size() - 11, 11, L"\\claude.exe") == 0;
    }
    CloseHandle(process);
    if (!claude) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > f->area) {
        f->area = area;
        f->best = hwnd;
    }
    return TRUE;
}
HWND findWindow() {
    Find f;
    EnumWindows(findClaude, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

void launch(const std::wstring& configured) {
    std::wstring exe = configured;
    if (exe.empty()) {
        wchar_t local[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) exe = std::wstring(local) + L"\\AnthropicClaude\\claude.exe";
    }
    if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        LOG("Claude screen: starting %ls (%s)", exe.c_str(), reinterpret_cast<INT_PTR>(r) > 32 ? "ok" : "failed");
        return;
    }
    // Installed another way: its link handler starts it.
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"claude://", nullptr, nullptr, SW_SHOWNORMAL);
    LOG("Claude screen: the app isn't at \"%ls\"; asked Windows to open claude:// (%s)", exe.c_str(),
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

void captureThread() {
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    int bw = 0, bh = 0;
    HWND window = nullptr;
    bool placed = false, launched = false, fronted = false, clipped = false;
    WINDOWPLACEMENT saved{};
    saved.length = sizeof(saved);
    DWORD lastLook = 0, startedAt = 0;
    int captures = 0, failures = 0;
    auto release = [&]() {
        if (clipped) ClipCursor(nullptr);
        clipped = false;
        if (window && placed && IsWindow(window)) SetWindowPlacement(window, &saved);
        if (placed) LOG("Claude screen: off; the window has its old place back");
        placed = fronted = launched = false;
        window = nullptr;
        g_window = nullptr;
        if (HWND game = g_game.load()) toFront(game);
    };
    for (;;) {
        DWORD now = GetTickCount();
        if (!g_on) {
            if (window || placed || launched) release();
            Sleep(100);
            continue;
        }
        if (!window || !IsWindow(window)) {
            if (now - lastLook < 500) {
                Sleep(50);
                continue;
            }
            lastLook = now;
            window = findWindow();
            if (!window) {
                if (!launched) {
                    std::wstring exe;
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        exe = g_exe;
                    }
                    launch(exe);
                    launched = true;
                    startedAt = now;
                } else if (now - startedAt > 30000 && now - startedAt < 30600) {
                    LOG("Claude screen: no Claude window 30 s after starting the app");
                }
                continue;
            }
            LOG("Claude screen: found the Claude window %p", static_cast<void*>(window));
            g_window = window;
        }
        if (!placed) {
            // Its place kept for later; restored to a normal window shaped like the screen.
            GetWindowPlacement(window, &saved);
            float aspect;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                aspect = g_v[10] > 0 ? g_v[9] / g_v[10] : 16.0f / 9.0f;
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
            placed = true;
            LOG("Claude screen: window sized %dx%d for a %.2f:1 screen", w, h, aspect);
        }
        bool front = GetForegroundWindow() == window;
        if (!front && !fronted) {
            fronted = true;  // once: after that you may click elsewhere (it comes back with the laser)
            front = toFront(window);
            LOG("Claude screen: the Claude window %s the front (your mouse and keyboard type into it)",
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
        RECT rc;
        GetClientRect(window, &rc);
        int w = std::min(static_cast<int>(rc.right), kMaxW), h = std::min(static_cast<int>(rc.bottom), kMaxH);
        if (w > 0 && h > 0) {
            if (w != bw || h != bh) {
                if (bmp) DeleteObject(bmp);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bmp) SelectObject(mem, bmp);
                bw = bmp ? w : 0;
                bh = bmp ? h : 0;
            }
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            if (bmp && PrintWindow(window, mem, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */)) {
                drawCursor(mem, window);
                GdiFlush();
                std::lock_guard<std::mutex> lock(g_frameMutex);
                g_frame.resize(static_cast<size_t>(w) * h);
                const uint32_t* in = static_cast<const uint32_t*>(bits);
                for (size_t i = 0; i < g_frame.size(); ++i) g_frame[i] = in[i] | 0xFF000000u;
                g_frameW = w;
                g_frameH = h;
                ++g_frameSeq;
                if (captures++ == 0) LOG("Claude screen: picture %dx%d", w, h);
                // What a picture costs (user 2026-10-15 00:53Z asked about the
                // performance): logged every 30 s while Claude is on a screen.
                static LARGE_INTEGER freq{};
                static double total = 0, most = 0;
                static int pictures = 0;
                static DWORD since = GetTickCount();
                if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&t1);
                double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
                total += ms;
                most = std::max(most, ms);
                ++pictures;
                if (GetTickCount() - since >= 30000) {
                    LOG("Claude screen: %d pictures of %dx%d in the last %lu s, %.1f ms each on average, %.1f ms at "
                        "most (its own thread, not the game's)",
                        pictures, w, h, (GetTickCount() - since) / 1000, total / pictures, most);
                    total = most = 0;
                    pictures = 0;
                    since = GetTickCount();
                }
            } else if (failures++ < 5) {
                LOG("Claude screen: no picture of the window (%lu)", GetLastError());
            }
        }
        DWORD spent = GetTickCount() - now;
        Sleep(spent < kFrameMs ? kFrameMs - spent : 1);
    }
}

}  // namespace

void set(int on, const float v[11], const char* exe) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (on && v) std::memcpy(g_v, v, sizeof(g_v));
        g_exe = widen(exe);
        if (on && !g_threadStarted) {
            g_threadStarted = true;
            std::thread(captureThread).detach();
        }
    }
    if (on != static_cast<int>(g_on.load())) {
        LOG("Claude screen: %s", on ? "on the computer's screen" : "off the screen");
        if (on) {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_frameW = g_frameH = 0;  // no old picture
        }
    }
    g_on = on != 0;
    if (!on && g_buttons) mouse(0.5f, 0.5f, 0);
}

bool on() { return g_on; }

bool screen(float v[11]) {
    if (!g_on) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(v, g_v, sizeof(g_v));
    return true;
}

bool frameSize(int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    w = g_frameW;
    h = g_frameH;
    return w > 0 && h > 0;
}

ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context) {
    if (!context) return nullptr;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    int w = g_frameW, h = g_frameH;
    if (w <= 0 || h <= 0) return nullptr;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return nullptr;
    device->Release();  // the context holds it
    if (device != g_device || w != g_texW || h != g_texH) {
        if (g_view) g_view->Release();
        if (g_texture) g_texture->Release();
        g_view = nullptr;
        g_texture = nullptr;
        g_device = device;
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
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_texture)) ||
            FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_view))) {
            if (g_texture) g_texture->Release();
            g_texture = nullptr;
            static int failed = 0;
            if (failed++ < 3) LOG("Claude screen: no %dx%d texture", w, h);
            return nullptr;
        }
        g_texW = w;
        g_texH = h;
        g_uploadedSeq = -1;
    }
    if (g_frameSeq != g_uploadedSeq) {
        context->UpdateSubresource(g_texture, 0, nullptr, g_frame.data(), w * 4, 0);
        context->GenerateMips(g_view);
        g_uploadedSeq = g_frameSeq;
    }
    return g_view;
}

void mouse(float u, float v, int buttons) {
    HWND window = g_window.load();
    if (!window || !IsWindow(window)) return;
    RECT rc;
    GetClientRect(window, &rc);
    POINT p{static_cast<LONG>(std::max(0.0f, std::min(1.0f, u)) * (rc.right - 1)),
            static_cast<LONG>(std::max(0.0f, std::min(1.0f, v)) * (rc.bottom - 1))};
    // Only when the laser moved, so the real mouse still works.
    if (std::abs(p.x - g_lastSet.x) > 1 || std::abs(p.y - g_lastSet.y) > 1) {
        g_lastSet = p;
        POINT s = p;
        ClientToScreen(window, &s);
        SetCursorPos(s.x, s.y);
    }
    int changed = buttons ^ g_buttons;
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
        g_buttons = buttons;
    }
}

bool keepsGameFocused() { return g_on; }

void setGameWindow(HWND game) { g_game = game; }

}  // namespace s4vr::claudescreen
