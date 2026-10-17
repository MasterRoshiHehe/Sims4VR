#pragma once
// Sims Forever: memory shared between sims4vr.dll (64-bit, in The Sims 4) and
// sf_hook.dll (32-bit, injected into the user's Sims 1). Fixed-size types only:
// both sides read the same layout.
#include <stdint.h>

#define SF_MAPPING_NAME L"Local\\sims4vr_simsforever_v2"
#define SF_MAGIC 0x32464653u  // "SFF2"
#define SF_KEYS 256  // key message ring
#define SF_MAX_W 2560
#define SF_MAX_H 1600

struct SfShared {
    uint32_t magic;
    uint32_t size;  // sizeof(SfShared), set by sims4vr.dll

    // Sims 4 -> Sims 1
    uint32_t sims4Window;  // The Sims 4's window (an HWND fits in 32 bits): focus goes back to it
    volatile int32_t active;    // 1 while the screen is on show: Sims 1 runs; 0: it is told it lost focus (pauses)
    volatile int32_t inputSeq;  // bumped after mouseX/mouseY/buttons change
    volatile int32_t mouseX, mouseY;  // in Sims 1's client pixels
    volatile int32_t buttons;         // bit 0 left, bit 1 right, bit 2 middle
    volatile int32_t quit;            // 1: close Sims 1 (WM_CLOSE)
    volatile int32_t wheel;           // mouse wheel, summed (WHEEL_DELTA units): the hook posts what's new
    // Your real keyboard while it's locked to Sims 1: messages to post
    // (WM_KEYDOWN / WM_KEYUP / WM_SYSKEY* / WM_CHAR), written at keyWrite++ % SF_KEYS.
    struct Key {
        uint32_t msg, wparam, lparam;
    } keys[SF_KEYS];
    volatile int32_t keyWrite;
    volatile uint8_t keyDown[256];    // which keys are held (GetKeyState / GetAsyncKeyState)

    // Sims 1 -> Sims 4
    volatile int32_t hookAlive;   // GetTickCount of the hook's latest beat
    uint32_t hookPid;
    uint32_t sims1Window;
    volatile int32_t frameSeq;    // bumped after a frame is complete in pixels[frameSeq & 1]
    volatile int32_t width, height;  // of the latest frame (and of the window's client area)
    volatile int32_t source;      // 1 DirectDraw blit, 2 PrintWindow
    volatile int32_t frames;      // frames grabbed in all
    char status[128];             // the hook's latest status line, for the Sims 4 log

    uint8_t pixels[2][SF_MAX_W * SF_MAX_H * 4];  // BGRA, rows tight (width * 4)
};
