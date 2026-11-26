#pragma once

#include <d3d11.h>
#include <dxgi.h>

// GPU time per render pass ([perf] gpu_timeline, diagnostic, off by default; Low FPS thread 2026-11-03).
// A GPU timestamp is written at every render-target bind (shadow maps, other depth passes, the main scene,
// post/UI) on whatever context the game records into; the timestamps are read back a few frames later,
// sorted by GPU time, and the time to the next stamp is added to the pass kind of the earlier one. Every 10 s
// one log line: ms per presented frame for each kind. Gaps where the card waits for the game's CPU are
// counted in the kind before the gap, so read it together with "Frame pacing" (3D busy).
namespace s4vr::gputimeline {

enum Kind { ShadowMap = 0, OtherDepth = 1, Scene = 2, Colour = 3 };

void setEnabled(bool on);
// A render-target bind was recorded (uihook.cpp setTargets), on any thread and context.
void bind(ID3D11DeviceContext* context, Kind kind);
// The game's Present is about to run (d3d11_hook.cpp), on the thread that presents.
void presented(IDXGISwapChain* swapchain);

}  // namespace s4vr::gputimeline
