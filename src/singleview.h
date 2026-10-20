#pragma once

// Both eyes in one game frame, step 2 probe ([stereo] probe_double, diagnostic;
// docs/handoffs/SINGLE-VIEW-HANDOFF.md). Hooks the game's scene render (rva
// 0x137a710, called once per main-thread frame by the renderer's frame
// function 0x137b830, which GameRenderSystemService::Update 0xa5b60 calls).
// Always logs the main thread's ms in it every 10 s ("Single view:"); with the
// probe on it runs it a second time with the same camera and no time step, and
// logs that pass's ms too, so we know whether two real passes fit the frame.
namespace s4vr::singleview {

void install();
void setProbe(bool on);  // live, from the settings
void presented();        // a picture was presented (Present thread)
void culled(int kind);   // the game culled rooms for a view (job threads)

}  // namespace s4vr::singleview
