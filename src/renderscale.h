#pragma once

// Makes the game render its 3D view (and back buffer) at a size of our
// choosing, larger than the window, for a sharper headset image.
//
// The engine already supports a render size larger than the window
// (DisplayManager::ApplySettings: render size = max(window, RenderMin)), but
// then letterboxes the frame inside the back buffer and turns MSAA off. We hook
// ApplySettings, raise RenderMin to our size, and afterwards undo the letterbox
// and restore MSAA. Static analysis 2026-10-12 of 1.112.519.1020 (DX11),
// re-checked 2026-10-14 for 1.121.361.1020 (DX11 and DX9); the hook refuses
// to install on other builds. Notes: docs in the research notes.
namespace s4vr::renderscale {

// Verifies the game build and hooks ApplySettings. False if unsupported.
// msaaCap: most edge-smoothing samples while scaled (never more than the player's setting).
bool install(int msaaCap);

// Render at width x height from now on (0, 0 = the game's own size). The game
// re-applies its display settings on its main thread at the next frame.
void request(int width, int height);

// True while a render size is requested (the headset view is on).
bool scaled();

// While a size is requested: its width is used even when narrower than the window (the engine
// otherwise never renders narrower than the window). Re-applies when it changes.
void setFitWidth(bool on);

}  // namespace s4vr::renderscale
