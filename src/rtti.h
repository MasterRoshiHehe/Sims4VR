#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// Finds game objects by their C++ class name (MSVC RTTI), so the mod keeps
// working across game patches without hard-coded addresses.
namespace s4vr::rtti {

// Address of the primary vtable (the one at object offset 0) for a mangled
// class name such as ".?AVCameraControllerManager@ClientApp@Sims4@EA@@".
// Returns 0 if not found.
uintptr_t findVTable(HMODULE module, const char* mangledName);

// Heap objects whose first pointer is `vtable`. Scans private read-write memory.
std::vector<uintptr_t> findInstances(uintptr_t vtable, size_t maxResults = 16);

// The vtable of an object, or 0 if the address is not readable.
uintptr_t vtableOf(uintptr_t object);

// Mangled class name of a polymorphic object (".?AV...") or false.
bool className(uintptr_t object, char* out, size_t size);

bool readable(uintptr_t address, size_t size);

}  // namespace s4vr::rtti
