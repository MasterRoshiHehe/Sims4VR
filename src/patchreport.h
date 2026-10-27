#pragma once
#include <cstdint>
#include <string>

// What this mod found in the running game build, written to sims4vr.log and rewritten as a short file next to it
// (sims4vr_patch_report.txt) every time something is added: exe, every address-table row, which features that
// leaves usable, RTTI lookups and their fallbacks, MinHook results and log-only probes. Meant to be read after
// a game patch to see what moved without anything crashing.
namespace s4vr::patchreport {

// First thing in s4vr_init: where the file goes, and the exe's stamp, size and sections.
void init(const std::wstring& dir);

// An unrecognised game build is about to be tried. False (safe mode for this run) if the last such run did not
// get 120 s in or end cleanly, i.e. probably crashed: one run later it tries again. The marker file is removed
// after 120 s and at shutdown.
bool armUnknownBuild();

// After the address table has resolved: rows, features.
void tableDone();

// A line in a group: "rtti", "probe" or "note".
void note(const char* group, const char* fmt, ...);

// Names a hook target for the hook list (MinHook calls are recorded by the linker wrap).
void nameHook(const void* target, const char* name);

// First `slots` entries of a vtable as game rvas, to compare with re/exe vtable_slots.tsv.
void vtableProbe(const char* name, uintptr_t vtable, int slots);

// MinHook results (called by the linker wrap in patchreport.cpp).
void hookCreated(void* target, uint32_t caller, int status);
void hookEnabled(void* target, int status);

void shutdown();

}  // namespace s4vr::patchreport
