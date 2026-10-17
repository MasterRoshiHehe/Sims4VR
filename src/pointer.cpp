#include "pointer.h"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"

namespace s4vr::pointer {

namespace {

// Keyboard scan codes (set 1); games often read scan codes, not virtual keys.
// 0xE0xx: an extended key (E0 prefix). Page Up/Down without it are the number
// pad's 9 and 3, which with Num Lock on set the game to speed 3.
enum Scan : WORD { kEscape = 0x01, kF2 = 0x3C, kW = 0x11, kA = 0x1E, kS = 0x1F, kD = 0x20, kPageUp = 0xE049, kPageDown = 0xE051 };
const WORD kHeldKeys[] = {kW, kA, kS, kD, kEscape, kF2};
constexpr int kKeyCount = sizeof(kHeldKeys) / sizeof(kHeldKeys[0]);

bool g_keyDown[kKeyCount] = {};
bool g_leftDown = false, g_rightDown = false;
ULONGLONG g_holdCursorUntil = 0;  // keep the mouse still around a click
float g_wheel = 0;
ULONGLONG g_lastUpdate = 0;
POINT g_lastSet{-1, -1};
bool g_loggedFirst = false;

void sendKey(WORD scan, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan & 0xFF;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0) |
                    (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

void sendMouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
}

void setKey(int index, bool down) {
    if (g_keyDown[index] == down) return;
    g_keyDown[index] = down;
    sendKey(kHeldKeys[index], down);
}

int indexOf(WORD scan) {
    for (int i = 0; i < kKeyCount; ++i)
        if (kHeldKeys[i] == scan) return i;
    return 0;
}

// Stick deflection past the dead zone, 0..1, curved for finer control near
// the centre and speed at the edge.
float shaped(float magnitude, float power) {
    constexpr float kDeadZone = 0.15f;
    float m = magnitude <= kDeadZone ? 0 : (magnitude - kDeadZone) / (1 - kDeadZone);
    return std::pow(m > 1 ? 1 : m, power);
}

// WASD from the stick. The direction snaps to the nearest of the eight the
// keys can make, and both keys of a diagonal go down and up together, so the
// camera glides instead of zig-zagging. Partial deflection pulses the keys
// for a share of each short cycle; near full deflection holds them.
void panKeys(float x, float y) {
    constexpr ULONGLONG kCycleMs = 120;
    float duty = shaped(std::sqrt(x * x + y * y), 1.5f);
    bool hold = duty >= 0.85f || (duty > 0.05f && GetTickCount64() % kCycleMs < static_cast<ULONGLONG>(duty * kCycleMs));
    int sector = -1;  // 0 = right, counter-clockwise in 45 degree steps
    if (duty > 0.05f) {
        float angle = std::atan2(y, x);
        sector = static_cast<int>(std::lround(angle / 0.785398f) + 8) % 8;
    }
    bool right = sector == 7 || sector == 0 || sector == 1, up = sector >= 1 && sector <= 3,
         left = sector >= 3 && sector <= 5, down = sector >= 5 && sector <= 7;
    setKey(indexOf(kD), hold && right);
    setKey(indexOf(kW), hold && up);
    setKey(indexOf(kA), hold && left);
    setKey(indexOf(kS), hold && down);
}

// Turning: the game's own middle-mouse drag, which turns smoothly at any
// speed. The drag runs along the bottom edge of the window, where the game's
// rotate icon falls outside what you see, and restarts in the middle before
// the mouse could reach a side.
bool g_turning = false;
float g_turnPixels = 0;   // fractional pixels not sent yet
LONG g_turnOffset = 0;    // pixels moved since the drag started

void startTurn(HWND game) {
    RECT client;
    if (!GetClientRect(game, &client)) return;
    POINT centre{client.right / 2, client.bottom - 1};
    ClientToScreen(game, &centre);
    SetCursorPos(centre.x, centre.y);
    sendMouse(MOUSEEVENTF_MIDDLEDOWN);
    g_turning = true;
    g_turnOffset = 0;
}

void stopTurn() {
    if (!g_turning) return;
    sendMouse(MOUSEEVENTF_MIDDLEUP);
    g_turning = false;
    g_turnPixels = 0;
    g_lastSet = {-1, -1};  // let the laser put the mouse back
}

void turn(HWND game, float value, float speed, float dt) {
    float amount = shaped(std::fabs(value), 2.0f);
    if (amount <= 0) {
        stopTurn();
        return;
    }
    if (!g_turning) startTurn(game);
    if (!g_turning) return;
    RECT client;
    GetClientRect(game, &client);
    if (std::abs(g_turnOffset) > client.right / 3) {
        stopTurn();
        startTurn(game);
    }
    g_turnPixels += (value < 0 ? -1 : 1) * amount * speed * dt;
    LONG dx = static_cast<LONG>(g_turnPixels);
    if (dx == 0) return;
    g_turnPixels -= dx;
    g_turnOffset += dx;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// Single key presses queued from other threads, sent one per frame.
std::mutex g_tapMutex;
std::vector<WORD> g_taps;
WORD g_tapDown = 0;

void sendTaps() {
    if (g_tapDown) {
        sendKey(g_tapDown, false);
        g_tapDown = 0;
        return;
    }
    std::lock_guard<std::mutex> lock(g_tapMutex);
    if (g_taps.empty()) return;
    g_tapDown = g_taps.front();
    g_taps.erase(g_taps.begin());
    sendKey(g_tapDown, true);
}

bool gameInFront(HWND game) {
    HWND front = GetForegroundWindow();
    return game && front && (front == game || GetAncestor(game, GA_ROOT) == front);
}

// Keeping the game in front. Mouse and keys go to the window in front, so
// once something else took the focus (a click outside the game, a pop-up)
// your hands still moved but clicked nothing until you alt-tabbed back. While
// you use the controllers with the headset on, the game window is brought
// back to the front instead. A window of the game's own (a message box) is
// left alone. Done on a thread of its own: taking the front talks to other
// windows, which must never hold up a frame.
std::mutex g_frontMutex;
std::condition_variable g_frontWake;
HWND g_frontWanted = nullptr;
bool g_frontStarted = false;
int g_frontLogs = 0;

std::string exeOf(HWND window) {
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid) || !pid) return "nothing";
    std::string name = "an unknown program";
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        char path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(process, 0, path, &size)) {
            name = path;
            size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
        }
        CloseHandle(process);
    }
    return name;
}

bool raise(HWND game) {
    if (IsIconic(game)) ShowWindow(game, SW_RESTORE);
    // Windows lets a program in the background take the front right after it
    // sent input itself, so a mouse move of nothing goes first.
    sendMouse(MOUSEEVENTF_MOVE);
    SetForegroundWindow(game);
    if (gameInFront(game)) return true;
    // Second try: share the input state of the window in front for a moment.
    HWND front = GetForegroundWindow();
    DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0;
    DWORD self = GetCurrentThreadId();
    if (frontThread && frontThread != self && AttachThreadInput(self, frontThread, TRUE)) {
        BringWindowToTop(game);
        SetForegroundWindow(game);
        AttachThreadInput(self, frontThread, FALSE);
    }
    return gameInFront(game);
}

void frontLoop() {
    for (;;) {
        HWND game;
        {
            std::unique_lock<std::mutex> lock(g_frontMutex);
            g_frontWake.wait(lock, [] { return g_frontWanted != nullptr; });
            game = g_frontWanted;
            g_frontWanted = nullptr;
        }
        std::string from = exeOf(GetForegroundWindow());
        bool ok = raise(game);
        if (g_frontLogs < 30) {
            ++g_frontLogs;
            if (ok)
                LOG("Game window was behind %s while you used the controllers: brought it back to the front", from.c_str());
            else
                LOG("Game window is behind %s and Windows refused to bring it to the front (tried again each second)",
                    from.c_str());
        }
    }
}

void keepInFront(HWND game) {
    static ULONGLONG lastTry = 0;
    ULONGLONG now = GetTickCount64();
    if (!game || now - lastTry < 1000) return;
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    if (pid == GetCurrentProcessId()) return;  // the game's own window
    lastTry = now;
    std::lock_guard<std::mutex> lock(g_frontMutex);
    if (!g_frontStarted) {
        g_frontStarted = true;
        std::thread(frontLoop).detach();
    }
    g_frontWanted = game;
    g_frontWake.notify_one();
}

}  // namespace

bool inFront(HWND game) { return gameInFront(game); }

void showAllFloors() {
    // Page Up moves the view up a floor; the game stops at the top one.
    std::lock_guard<std::mutex> lock(g_tapMutex);
    g_taps.assign(6, kPageUp);
}

void floorsDown(int count) {
    if (count <= 0) return;
    std::lock_guard<std::mutex> lock(g_tapMutex);
    g_taps.assign(count < 6 ? count : 6, kPageDown);
}

void releaseAll() {
    for (int i = 0; i < kKeyCount; ++i) setKey(i, false);
    stopTurn();
    if (g_leftDown) sendMouse(MOUSEEVENTF_LEFTUP);
    if (g_rightDown) sendMouse(MOUSEEVENTF_RIGHTUP);
    g_leftDown = g_rightDown = false;
}

void update(HWND game, const Controls& c) {
    if (!gameInFront(game)) {
        releaseAll();
        g_lastSet = {-1, -1};
        if (c.inUse) keepInFront(game);
        return;
    }
    ULONGLONG now = GetTickCount64();
    float dt = g_lastUpdate ? (now - g_lastUpdate) / 1000.0f : 0;
    if (dt > 0.1f) dt = 0.1f;
    g_lastUpdate = now;
    // A press holds the mouse still briefly so a shaky hand still clicks;
    // holding longer lets it move again for dragging.
    if (c.leftButton != g_leftDown || c.rightButton != g_rightDown) g_holdCursorUntil = now + (c.leftButton || c.rightButton ? 250 : 120);
    if (c.hasTarget && (c.warp || (now >= g_holdCursorUntil && !g_turning))) {
        RECT client;
        if (GetClientRect(game, &client) && client.right > 0 && client.bottom > 0) {
            POINT p{static_cast<LONG>(c.u * client.right), static_cast<LONG>(c.v * client.bottom)};
            // Only move the mouse when the laser moved, so the real mouse still works.
            if (std::abs(p.x - g_lastSet.x) > 1 || std::abs(p.y - g_lastSet.y) > 1) {
                g_lastSet = p;
                ClientToScreen(game, &p);
                SetCursorPos(p.x, p.y);
                if (!g_loggedFirst) {
                    g_loggedFirst = true;
                    LOG("Laser pointer moved the mouse to %.3f %.3f of the window", c.u, c.v);
                }
            }
        }
    }
    if (c.leftButton != g_leftDown) {
        g_leftDown = c.leftButton;
        sendMouse(g_leftDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    }
    if (c.rightButton != g_rightDown) {
        g_rightDown = c.rightButton;
        sendMouse(g_rightDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    }
    setKey(indexOf(kEscape), c.escape);
    setKey(indexOf(kF2), c.buildMode);
    sendTaps();
    panKeys(c.panX, c.panY);
    turn(game, c.rotate, c.turnSpeed, dt);
    // Zoom with the scroll wheel; the Z/X keys also click UI buttons.
    float zoom = std::fabs(c.zoom) < 0.3f ? 0 : c.zoom;
    g_wheel += zoom * 10.0f * dt;  // up to 10 wheel notches a second
    while (g_wheel >= 1) {
        sendMouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA);
        g_wheel -= 1;
    }
    while (g_wheel <= -1) {
        sendMouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-WHEEL_DELTA));
        g_wheel += 1;
    }
    if (zoom == 0) g_wheel = 0;
}

}  // namespace s4vr::pointer
