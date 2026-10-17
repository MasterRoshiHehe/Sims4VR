#pragma once
#include <cstdint>

// The game's own functions and globals the mod calls or hooks, in whichever of
// the two game executables is running. They were found in the DX11 build
// (TS4_x64.exe); the DX9 build (TS4_DX9_x64.exe) is the same game code
// compiled again for the other renderer, and tools/map_dx9.py found each one
// there by its code with relative addresses masked out (functions) or by the
// matching code that uses it (globals, vtables). Renderer code itself differs
// between the two and has no DX9 entries (render resolution, the UI HAL).
namespace s4vr::gameaddr {

// Running in TS4_DX9_x64.exe.
bool dx9();

// The running executable's address of a function known by its DX11 rva and
// its first 16 bytes there. Null if this executable has no known equivalent,
// its first bytes differ, or the game build is not the one the addresses are
// for (game 1.128.90.1030; any other build gets none).
uint8_t* function(uint32_t dx11Rva, const uint8_t (&dx11Bytes)[16]);

// The running executable's address of a global variable or a vtable known by
// its DX11 rva, or null (also for any other game build).
uint8_t* global(uint32_t dx11Rva);

}  // namespace s4vr::gameaddr
