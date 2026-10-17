#include "simsforever.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "claudescreen.h"
#include "log.h"
#include "simsforever/sf_shared.h"

namespace s4vr::simsforever {
namespace {

std::mutex g_mutex;
int g_on = 0;
float g_v[11] = {};
SfShared* g_shared = nullptr;
HANDLE g_mapping = nullptr;
HANDLE g_job = nullptr;
HANDLE g_launcher = nullptr;
bool g_launched = false;
ULONGLONG g_launchedAt = 0;
ULONGLONG g_offSince = 0;  // the screen went off: Sims 1 is closed after kCloseAfter
constexpr ULONGLONG kCloseAfter = 2000;

// The screen's shape (screenmesh.py): depths behind its front on a grid.
std::mutex g_surfaceMutex;
int g_surfaceX = 0, g_surfaceY = 0, g_surfaceVersion = 0;
std::vector<float> g_surface;

// The mouse: the laser's or a finger's (absolute) and your real one's (moves,
// buttons) while it's locked to Sims 1.
std::mutex g_inputMutex;
int g_laserButtons = 0, g_realButtons = 0;
float g_vx = -1, g_vy = -1;  // Sims 1's cursor, client pixels
void publishMouse();

// Your real mouse and keyboard locked to Sims 1 (low-level hooks on their own thread).
std::atomic<bool> g_locked{false};
// What the keyboard hook did while locked (logged from poll(), not the hook).
std::atomic<int> g_keysToSims1{0}, g_keysPassed{0}, g_escapesToSims1{0}, g_escapesPassed{0};
DWORD g_hookThread = 0;
HHOOK g_mouseHook = nullptr, g_keyHook = nullptr;
uint8_t g_keys[256] = {};  // your keys' state as Sims 1 sees it (for ToUnicode)

// Render thread only.
ID3D11Texture2D* g_texture = nullptr;
ID3D11ShaderResourceView* g_view = nullptr;
ID3D11Device* g_device = nullptr;
int g_texW = 0, g_texH = 0;
int32_t g_uploadedSeq = -1;

std::wstring widen(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

std::wstring ourFolder() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ourFolder), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

struct FindData {
    DWORD pid;
    HWND best;
    long area;
};
BOOL CALLBACK findWindow(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(hwnd, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > f->area) {
        f->area = area;
        f->best = hwnd;
    }
    return TRUE;
}

bool mapShared() {
    if (g_shared) return true;
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SfShared), SF_MAPPING_NAME);
    if (!g_mapping) {
        LOG("Sims Forever: no shared memory (%lu)", GetLastError());
        return false;
    }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_shared = static_cast<SfShared*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SfShared)));
    if (!g_shared) return false;
    if (!existed) {
        g_shared->magic = SF_MAGIC;
        g_shared->size = sizeof(SfShared);
    }
    FindData f{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    g_shared->sims4Window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f.best));
    return true;
}

// Sims 1 is still there (its hook beat within 5 s).
bool sims1Alive() {
    return g_shared && g_shared->hookPid && static_cast<DWORD>(GetTickCount() - g_shared->hookAlive) < 5000;
}

void launch(const std::wstring& exe, const std::wstring& args) {
    if (!mapShared()) return;
    if (sims1Alive()) return;
    if (g_launcher && WaitForSingleObject(g_launcher, 0) == WAIT_TIMEOUT) return;  // starting
    if (g_launched && GetTickCount64() - g_launchedAt < 150000) return;           // loading (no beat yet)
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG("Sims Forever: Sims 1 not found at \"%ls\" ([simsforever] sims1_path in sims4vr.ini)", exe.c_str());
        return;
    }
    std::wstring launcher = ourFolder() + L"\\sf_launch.exe";
    if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG("Sims Forever: %ls is missing", launcher.c_str());
        return;
    }
    // Sims 1 closes with The Sims 4 (the job is closed when the game exits).
    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    g_shared->frames = 0;
    g_shared->hookPid = 0;
    g_shared->quit = 0;
    std::wstring command = L"\"" + launcher + L"\" \"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    std::wstring folder = ourFolder();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring buffer = command;
    if (!CreateProcessW(launcher.c_str(), &buffer[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        folder.c_str(), &si, &pi)) {
        LOG("Sims Forever: sf_launch.exe didn't start (%lu)", GetLastError());
        return;
    }
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
        LOG("Sims Forever: Sims 1 won't close with the game by itself (job %lu)", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (g_launcher) CloseHandle(g_launcher);
    g_launcher = pi.hProcess;
    g_launched = true;
    g_launchedAt = GetTickCount64();
    LOG("Sims Forever: starting Sims 1: %ls", command.c_str());
}

// Closes Sims 1 outright (no "save?" question): it isn't kept in the background.
void closeSims1(const char* why) {
    if (g_job) {
        TerminateJobObject(g_job, 0);
        CloseHandle(g_job);  // a fresh job for the next start
        g_job = nullptr;
    }
    if (g_shared) {
        g_shared->hookPid = 0;
        g_shared->frames = 0;
    }
    g_launched = false;
    LOG("Sims Forever: Sims 1 closed (%s)", why);
}

void pushKey(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    int32_t at = g_shared->keyWrite;
    SfShared::Key& k = g_shared->keys[at & (SF_KEYS - 1)];
    k.msg = msg;
    k.wparam = wparam;
    k.lparam = lparam;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->keyWrite));
}

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
    if (code != HC_ACTION || !g_locked || !g_shared || (m->flags & LLMHF_INJECTED))
        return CallNextHookEx(nullptr, code, wp, lp);  // ours (the laser's clicks into The Sims 4) go through
    std::lock_guard<std::mutex> lock(g_inputMutex);
    switch (wp) {
    case WM_MOUSEMOVE: {
        POINT now;
        GetCursorPos(&now);  // where the cursor stays (this move is swallowed)
        int w = g_shared->width, h = g_shared->height;
        if (w > 0 && h > 0) {
            if (g_vx < 0) {
                g_vx = w / 2.0f;
                g_vy = h / 2.0f;
            }
            g_vx = std::max(0.0f, std::min(w - 1.0f, g_vx + (m->pt.x - now.x)));
            g_vy = std::max(0.0f, std::min(h - 1.0f, g_vy + (m->pt.y - now.y)));
        }
        break;
    }
    case WM_LBUTTONDOWN: g_realButtons |= 1; break;
    case WM_LBUTTONUP: g_realButtons &= ~1; break;
    case WM_RBUTTONDOWN: g_realButtons |= 2; break;
    case WM_RBUTTONUP: g_realButtons &= ~2; break;
    case WM_MBUTTONDOWN: g_realButtons |= 4; break;
    case WM_MBUTTONUP: g_realButtons &= ~4; break;
    case WM_MOUSEWHEEL:
        InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&g_shared->wheel), static_cast<short>(HIWORD(m->mouseData)));
        break;
    default:
        break;
    }
    publishMouse();
    return 1;  // not for The Sims 4
}

LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp) {
    auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && g_locked && g_shared && (k->flags & LLKHF_INJECTED)) {
        ++g_keysPassed;  // another program's typing (the laser never types)
        if (k->vkCode == VK_ESCAPE && !(k->flags & LLKHF_UP)) ++g_escapesPassed;
    }
    if (code != HC_ACTION || !g_locked || !g_shared || (k->flags & LLKHF_INJECTED))
        return CallNextHookEx(nullptr, code, wp, lp);
    // The Windows keys and Alt+Tab stay Windows'.
    if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || (k->vkCode == VK_TAB && (k->flags & LLKHF_ALTDOWN)))
        return CallNextHookEx(nullptr, code, wp, lp);
    const bool up = (k->flags & LLKHF_UP) != 0, alt = (k->flags & LLKHF_ALTDOWN) != 0;
    ++g_keysToSims1;
    if (k->vkCode == VK_ESCAPE && !up) ++g_escapesToSims1;
    const uint32_t vk = k->vkCode & 0xFF;
    const bool wasDown = g_keys[vk] & 0x80;
    g_keys[vk] = up ? 0 : 0x80;
    // The generic Shift / Ctrl / Alt follow their left and right keys.
    g_keys[VK_SHIFT] = (g_keys[VK_LSHIFT] | g_keys[VK_RSHIFT]) & 0x80;
    g_keys[VK_CONTROL] = (g_keys[VK_LCONTROL] | g_keys[VK_RCONTROL]) & 0x80;
    g_keys[VK_MENU] = (g_keys[VK_LMENU] | g_keys[VK_RMENU]) & 0x80;
    for (int i = 0; i < 256; ++i) g_shared->keyDown[i] = g_keys[i] ? 1 : 0;
    uint32_t lparam = 1u | ((k->scanCode & 0xFF) << 16) | ((k->flags & LLKHF_EXTENDED) ? 1u << 24 : 0) |
                      (alt ? 1u << 29 : 0) | (up ? (1u << 30) | (1u << 31) : (wasDown ? 1u << 30 : 0));
    uint32_t vkMsg = vk == VK_LSHIFT || vk == VK_RSHIFT ? VK_SHIFT
                     : vk == VK_LCONTROL || vk == VK_RCONTROL ? VK_CONTROL
                     : vk == VK_LMENU || vk == VK_RMENU ? VK_MENU
                                                        : vk;
    bool sys = alt || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10;
    pushKey(up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN), vkMsg, lparam);
    if (!up) {
        // The character it types, with your Shift / AltGr held as Sims 1 sees them.
        wchar_t chars[4];
        int n = ToUnicode(vk, k->scanCode, g_keys, chars, 4, 0);
        for (int i = 0; i < n; ++i) {
            char ansi[4];
            int m = WideCharToMultiByte(CP_ACP, 0, &chars[i], 1, ansi, sizeof(ansi), nullptr, nullptr);
            for (int j = 0; j < m; ++j) pushKey(WM_CHAR, static_cast<uint8_t>(ansi[j]), lparam);
        }
    }
    return 1;
}

DWORD WINAPI hookThread(void*) {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // this thread's queue
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP + 1 && !g_mouseHook) {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&hookThread), &self);
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, self, 0);
            g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, self, 0);
            LOG("Sims Forever: your mouse and keyboard go to Sims 1 (%s)", g_mouseHook && g_keyHook ? "locked" : "hooks failed");
        } else if (msg.message == WM_APP + 2 && g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
            g_mouseHook = g_keyHook = nullptr;
            LOG("Sims Forever: your mouse and keyboard are The Sims 4's again");
        }
    }
    return 0;
}

// Laser buttons, real buttons and the cursor to Sims 1. Under g_inputMutex.
void publishMouse() {
    if (!g_shared || g_vx < 0) return;
    int buttons = g_laserButtons | g_realButtons;
    int x = static_cast<int>(g_vx), y = static_cast<int>(g_vy);
    if (x == g_shared->mouseX && y == g_shared->mouseY && buttons == g_shared->buttons) return;
    g_shared->mouseX = x;
    g_shared->mouseY = y;
    g_shared->buttons = buttons;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->inputSeq));
}

}  // namespace

void setLocked(bool locked) {
    if (claudescreen::on()) locked = false;  // Claude's window is in front: your mouse and keyboard reach it as usual
    if (locked == g_locked.load()) return;
    if (!g_hookThread) CreateThread(nullptr, 0, hookThread, nullptr, 0, &g_hookThread);
    g_locked = locked;
    if (!locked) {
        // Nothing stays held in Sims 1.
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_realButtons = 0;
        std::memset(g_keys, 0, sizeof(g_keys));
        if (g_shared) std::memset(const_cast<uint8_t*>(g_shared->keyDown), 0, sizeof(g_shared->keyDown));
        publishMouse();
    }
    for (int i = 0; i < 50 && !PostThreadMessageW(g_hookThread, WM_APP + (locked ? 1 : 2), 0, 0); ++i) Sleep(2);
}

void setSurface(int nx, int ny, const float* depths, int count) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    if (nx < 2 || ny < 2 || nx > 256 || ny > 256 || !depths || count < nx * ny) nx = ny = 0;
    std::vector<float> d(depths, depths + nx * ny);
    if (nx == g_surfaceX && ny == g_surfaceY && d == g_surface) return;
    g_surfaceX = nx;
    g_surfaceY = ny;
    g_surface.swap(d);
    ++g_surfaceVersion;
    float deepest = 0;
    for (float v : g_surface) deepest = std::min(deepest, v);
    static int logged = 0;
    if (logged++ < 20) {
        if (nx)
            LOG("Sims Forever: the screen's shape: %dx%d grid, %.3f deep at most", nx, ny, -deepest);
        else
            LOG("Sims Forever: the screen is flat (no shape read)");
    }
}

int surface(int& nx, int& ny, std::vector<float>& depths) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    nx = g_surfaceX;
    ny = g_surfaceY;
    depths = g_surface;
    return g_surfaceVersion;
}

float depth(float fx, float fy) {
    std::lock_guard<std::mutex> lock(g_surfaceMutex);
    if (!g_surfaceX) return 0;
    float gx = std::max(0.0f, std::min(1.0f, fx)) * (g_surfaceX - 1);
    float gy = std::max(0.0f, std::min(1.0f, fy)) * (g_surfaceY - 1);
    int ix = std::min(static_cast<int>(gx), g_surfaceX - 2), iy = std::min(static_cast<int>(gy), g_surfaceY - 2);
    float tx = gx - ix, ty = gy - iy;
    auto at = [&](int x, int y) { return g_surface[y * g_surfaceX + x]; };
    return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
}

void setScreen(int on, const float v[11], const char* exe, const char* args) {
    std::wstring e = widen(exe), a = widen(args);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_on = on;
        if (on && v) std::memcpy(g_v, v, sizeof(g_v));
        if (on) launch(e, a);
        if (g_shared) g_shared->active = on ? 1 : 0;
        g_offSince = on ? 0 : GetTickCount64();
    }
    static int logged = 0;
    if (logged++ < 40) {
        if (on)
            LOG("Sims Forever: on the screen at %.2f %.2f %.2f facing %.2f %.2f %.2f, %.2f x %.2f", v[0], v[1], v[2],
                v[3], v[4], v[5], v[9], v[10]);
        else
            LOG("Sims Forever: off the screen");
    }
}

bool screen(float v[11]) {
    if (claudescreen::screen(v)) return true;  // Claude on a computer: drawn and clicked the same way
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_on) return false;
    std::memcpy(v, g_v, sizeof(g_v));
    return true;
}

bool frameSize(int& w, int& h) {
    if (claudescreen::on()) return claudescreen::frameSize(w, h);
    if (!g_shared || g_shared->frames <= 0) return false;
    w = g_shared->width;
    h = g_shared->height;
    return w > 0 && h > 0;
}

ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context) {
    if (claudescreen::on()) return claudescreen::frameView(context);
    if (!g_shared || !context) return nullptr;
    int32_t seq = g_shared->frameSeq;
    int w = g_shared->width, h = g_shared->height;
    if (g_shared->frames <= 0 || w <= 0 || h <= 0 || w > SF_MAX_W || h > SF_MAX_H) return g_view;
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
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_texture)) ||
            FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_view))) {
            static int failed = 0;
            if (failed++ < 3) LOG("Sims Forever: no %dx%d texture", w, h);
            return nullptr;
        }
        g_texW = w;
        g_texH = h;
        g_uploadedSeq = -1;
        LOG("Sims Forever: Sims 1 picture %dx%d (%s)", w, h, g_shared->source == 1 ? "DirectDraw" : "PrintWindow");
    }
    if (seq != g_uploadedSeq) {
        // The hook writes the other buffer next; this one stays put while it is copied.
        context->UpdateSubresource(g_texture, 0, nullptr, g_shared->pixels[seq & 1], w * 4, 0);
        g_uploadedSeq = seq;
    }
    return g_view;
}

void mouse(float u, float v, int buttons) {
    if (claudescreen::on()) {
        claudescreen::mouse(u, v, buttons);
        return;
    }
    if (!g_shared || !sims1Alive()) return;
    int w = g_shared->width, h = g_shared->height;
    if (w <= 0 || h <= 0) return;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_vx = std::max(0.0f, std::min(w - 1.0f, u * w));
        g_vy = std::max(0.0f, std::min(h - 1.0f, v * h));
        g_laserButtons = buttons;
        publishMouse();
    }
    int x = static_cast<int>(u * w), y = static_cast<int>(v * h);
    static int prev = 0, logged = 0;
    if (buttons != prev && logged++ < 30) LOG("Sims Forever: mouse %s at %d,%d", buttons ? "down" : "up", x, y);
    prev = buttons;
}

void placeSound(uint32_t pid, float left, float right);  // simsforever_sound.cpp

void setEars(float left, float right) {
    placeSound(sims1Alive() ? g_shared->hookPid : 0, left, right);
}

void poll() {
    if (!g_shared) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_offSince && GetTickCount64() - g_offSince >= kCloseAfter) {
            g_offSince = 0;
            if (g_launched || sims1Alive()) closeSims1("you stopped playing");
        }
    }
    static char last[128] = {};
    static int logged = 0;
    if (std::memcmp(last, g_shared->status, sizeof(last)) != 0) {
        std::memcpy(last, g_shared->status, sizeof(last));
        last[sizeof(last) - 1] = 0;
        if (logged++ < 60 && last[0]) LOG("Sims Forever: Sims 1 says: %s", last);
    }
    static int keysLogged = 0, escapesLogged = 0, passedLogged = 0, passedLines = 0;
    if (g_escapesToSims1 != escapesLogged && escapesLogged < 20) {
        escapesLogged = g_escapesToSims1;
        LOG("Sims Forever: Escape sent to Sims 1 (%d keys sent so far)", g_keysToSims1.load());
    }
    if (g_keysToSims1 / 200 != keysLogged / 200) {
        keysLogged = g_keysToSims1;
        LOG("Sims Forever: %d keys sent to Sims 1", keysLogged);
    }
    if (g_keysPassed != passedLogged && passedLines < 20) {
        passedLogged = g_keysPassed;
        ++passedLines;
        LOG("Sims Forever: %d keys came marked as typed by another program and went to The Sims 4 (%d of them Escape)",
            passedLogged, g_escapesPassed.load());
    }
    if (g_launcher && WaitForSingleObject(g_launcher, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(g_launcher, &code);
        if (code != 0) {
            LOG("Sims Forever: the launcher failed (step %lu)", code);
            g_launched = false;  // may try again on the next use
        }
        CloseHandle(g_launcher);
        g_launcher = nullptr;
    }
}

}  // namespace s4vr::simsforever
