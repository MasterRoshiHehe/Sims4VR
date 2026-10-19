#pragma once
#include <d3d11.h>

// No blue dimming in first person (user 2026-10-15: "I DONT WANT IT TO SHOW IN
// THE WIDGETS"). While a modal widget is open, the game's LayoutManager puts
// mModalitySprite under it: a full-screen vertical gradient, #161D3A at 55% to
// #12152F at 85% (olympus DisplayObjectUtils.DrawModality). It lands in the UI
// layer with everything else, so every piece lifted from it (popup cards,
// skill bars, the wrist) carried it. This takes it back out of the UI layer:
// the dimming's colour per row is read off the UI's left and right edges
// (empty there but for the dimming), then every pixel under it is un-blended
// (what was there before the dimming was drawn over it) and the dimming alone
// becomes transparent. Inside the open dialog's own box (drawn over the
// dimming) only the bare dimming is cleared.
namespace s4vr::nodim {

// At the source, in every mode (user 2026-10-18: "completely disable the blue
// overlay"): the game's gradient colour ramp gets the dimming's two stops fully
// see-through. The modality sprite stays (clicks behind dialogs stay blocked)
// but draws nothing. Hooked once at load; `on` counts for dimming pictures made
// after it (the game makes one when the dimming is first drawn). The first-
// person un-blend below stays as the fallback (it finds no dimming then).
void installSource(bool on);
void setSource(bool on);

// Any thread: on (first person) or off, and the boxes of dialogs drawn over the
// dimming (UI pixels: x, y, width, height; up to 4).
void set(bool on, const int (*boxes)[4], int count);

// Present thread (uihook): `raw` is this frame's UI (single sample, shader
// resource), `out` gets it without the dimming (render target, same size).
// False: nothing done (off, no dimming seen, or not available); copy as before.
bool apply(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* raw, ID3D11RenderTargetView* out,
           const D3D11_TEXTURE2D_DESC& desc);

}  // namespace s4vr::nodim
