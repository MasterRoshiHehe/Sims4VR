#include "gamepad.h"

#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "rtti.h"

namespace s4vr::gamepad {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

// One original per xinput DLL the game might have picked.
const wchar_t* const kDlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
constexpr int kDllCount = 3;
GetStateFn g_getState[kDllCount] = {};
GetCapabilitiesFn g_getCaps[kDllCount] = {};

std::mutex g_mutex;
State g_state;
DWORD g_packet = 1;
std::atomic<bool> g_installed{false};
std::atomic<unsigned long long> g_reads{0};

SHORT axis(float v) {
    if (!(v == v)) return 0;
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<SHORT>(v * 32767);
}

bool deflected(const State& s) {
    return std::fabs(s.leftX) + std::fabs(s.leftY) + std::fabs(s.rightX) + std::fabs(s.rightY) > 0.05f ||
           s.leftTrigger > 0.05f || s.rightTrigger > 0.05f || s.buttons;
}

DWORD fill(DWORD result, XINPUT_STATE* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool real = result == ERROR_SUCCESS;
    if (real && !deflected(g_state)) return result;  // leave a real pad alone
    XINPUT_GAMEPAD pad{};
    pad.wButtons = g_state.buttons;
    pad.bLeftTrigger = static_cast<BYTE>(g_state.leftTrigger * 255);
    pad.bRightTrigger = static_cast<BYTE>(g_state.rightTrigger * 255);
    pad.sThumbLX = axis(g_state.leftX);
    pad.sThumbLY = axis(g_state.leftY);
    pad.sThumbRX = axis(g_state.rightX);
    pad.sThumbRY = axis(g_state.rightY);
    out->Gamepad = pad;
    out->dwPacketNumber = g_packet;  // bumped on every change; the game skips unchanged packets
    return ERROR_SUCCESS;
}

template <int I>
DWORD WINAPI hookedGetState(DWORD user, XINPUT_STATE* out) {
    DWORD result = g_getState[I](user, out);
    if (user != 0 || !out) return result;
    if (g_reads++ == 0) LOG("Game reads gamepad 0 (via %ls)", kDlls[I]);
    return fill(result, out);
}

template <int I>
DWORD WINAPI hookedGetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* out) {
    DWORD result = g_getCaps[I](user, flags, out);
    if (user != 0 || !out || result == ERROR_SUCCESS) return result;
    *out = {};
    out->Type = XINPUT_DEVTYPE_GAMEPAD;
    out->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    out->Gamepad.wButtons = 0xF3FF;
    out->Gamepad.bLeftTrigger = out->Gamepad.bRightTrigger = 255;
    out->Gamepad.sThumbLX = out->Gamepad.sThumbLY = out->Gamepad.sThumbRX = out->Gamepad.sThumbRY = 32767;
    return ERROR_SUCCESS;
}

template <int I>
bool hookDll() {
    HMODULE dll = GetModuleHandleW(kDlls[I]);
    if (!dll) dll = LoadLibraryW(kDlls[I]);
    if (!dll) return false;
    void* getState = reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetState"));
    void* getCaps = reinterpret_cast<void*>(GetProcAddress(dll, "XInputGetCapabilities"));
    bool ok = false;
    if (getState && MH_CreateHook(getState, reinterpret_cast<void*>(&hookedGetState<I>),
                                  reinterpret_cast<void**>(&g_getState[I])) == MH_OK &&
        MH_EnableHook(getState) == MH_OK)
        ok = true;
    if (getCaps && MH_CreateHook(getCaps, reinterpret_cast<void*>(&hookedGetCaps<I>),
                                 reinterpret_cast<void**>(&g_getCaps[I])) == MH_OK)
        MH_EnableHook(getCaps);
    return ok;
}

// The game's input manager (EA::Input::InputMan) probes for pads once, at
// startup, before this mod loads, and never looks for new XInput pads after.
// So mark pad slot 0 as an XInput pad on user index 0 by hand, the way its
// probe does (InputMan init = vtable slot 2, game rva 0x3e3810 in 1.112,
// 0x263610 in 1.121); its per-frame poll then calls XInputGetState(0) and
// announces the pad as connected by itself. Offsets below rechecked for 1.121
// (init and poll unchanged, DX11 and DX9).
constexpr size_t kPad0State = 0xa74;   // int: 4 = XInput pad
constexpr size_t kPad0Present = 0xa70; // int
constexpr size_t kPad0User = 0xb38;    // int64: XInput user index
constexpr size_t kPad0InfoUser = 0x1a60, kPad0InfoType = 0x1a68;
constexpr size_t kLock = 0x618;        // CRITICAL_SECTION

bool safeRead(uintptr_t address, void* out, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &read) && read == size;
}

// The manager may be a global in the game's own data section, which the heap
// scan in rtti::findInstances skips.
std::vector<uintptr_t> findInImage(HMODULE module, uintptr_t vtable) {
    std::vector<uintptr_t> found;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        auto* p = reinterpret_cast<uintptr_t*>(base + section->VirtualAddress);
        size_t count = section->Misc.VirtualSize / 8;
        for (size_t k = 0; k < count; ++k)
            if (p[k] == vtable) found.push_back(reinterpret_cast<uintptr_t>(p + k));
    }
    return found;
}

void attachToGame() {
    HMODULE game = GetModuleHandleW(nullptr);
    uintptr_t vtable = rtti::findVTable(game, ".?AVInputMan@Input@EA@@");
    if (!vtable) {
        LOG("Virtual gamepad: input manager class not found");
        return;
    }
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::vector<uintptr_t> candidates = findInImage(game, vtable);
        for (uintptr_t heap : rtti::findInstances(vtable, 4)) candidates.push_back(heap);
        if (attempt == 0) LOG("Virtual gamepad: %zu input manager candidate(s)", candidates.size());
        for (uintptr_t man : candidates) {
            int state = -1, present = -1;
            uint8_t initialized = 0;
            if (!safeRead(man + kPad0State, &state, 4) || !safeRead(man + kPad0Present, &present, 4) ||
                !safeRead(man + 0x640, &initialized, 1) || !initialized)
                continue;
            LOG("Virtual gamepad: input manager %p, pad 0 state %d present %d", reinterpret_cast<void*>(man), state,
                present);
            if (state == 4) return;  // a real XInput pad already holds slot 0; the hook rides on it
            if (state != 0) {
                LOG("Virtual gamepad: pad slot 0 is in use by another device type; not touching it");
                return;
            }
            auto* lock = reinterpret_cast<CRITICAL_SECTION*>(man + kLock);
            EnterCriticalSection(lock);
            *reinterpret_cast<int*>(man + kPad0Present) = 1;
            *reinterpret_cast<int64_t*>(man + kPad0User) = 0;
            *reinterpret_cast<int64_t*>(man + kPad0InfoUser) = 0;
            *reinterpret_cast<int*>(man + kPad0InfoType) = 4;
            *reinterpret_cast<int*>(man + kPad0State) = 4;
            LeaveCriticalSection(lock);
            LOG("Virtual gamepad: registered as XInput pad 0");
            return;
        }
        Sleep(2000);
    }
    LOG("Virtual gamepad: no initialized input manager found");
}

}  // namespace

bool install() {
    if (g_installed) return true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    int hooked = hookDll<0>() + hookDll<1>() + hookDll<2>();
    LOG("Virtual gamepad: hooked XInputGetState in %d DLL(s)", hooked);
    g_installed = hooked > 0;
    if (g_installed) CreateThread(nullptr, 0, [](void*) -> DWORD { attachToGame(); return 0; }, nullptr, 0, nullptr);
    return g_installed;
}

bool installed() { return g_installed; }

void set(const State& state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (std::memcmp(&state, &g_state, sizeof(State)) != 0) {
        g_state = state;
        ++g_packet;
    }
}

unsigned long long reads() { return g_reads; }

}  // namespace s4vr::gamepad
