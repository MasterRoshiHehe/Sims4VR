#pragma once

#include <cstdint>
#include <string>

#include "hud_art.h"
#include "imgui.h"

// The native UI kit: the game's own UI pieces, drawn by us (user rule
// 2026-10-17: never crop the game's rendered UI; docs/UI-KIT.md). Everything
// here draws into an ImGui draw list from the game's art atlas (hud_art.bin),
// the game's icons / thumbnails and the game's fonts' stand-ins, in atlas
// pixels. hud.cpp owns the textures and calls bind() once; new wrist, phone,
// popup and HUD elements compose these instead of drawing from scratch.
namespace s4vr::uikit {

namespace art = hud::art;

const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kShadow = IM_COL32(0, 0, 0, 110);
const ImU32 kTextOn = IM_COL32(120, 214, 82, 255);      // the game's selected-speed green, for text
const ImU32 kTitleBlue = IM_COL32(29, 58, 102, 255);    // titles on the light dialog panel
const ImU32 kBodyDark = IM_COL32(44, 52, 66, 255);      // body text on the light dialog panel
const ImU32 kReasonGrey = IM_COL32(110, 118, 130, 255);
const ImU32 kButtonBlue = IM_COL32(30, 70, 128, 255);   // the dialog button's text

// What the kit draws with. Set by hud.cpp (after its fonts, art and icon
// slots exist); `icon` draws the game's icon or thumbnail `id` centred in a
// box (nothing until it is read).
struct Resources {
    ImTextureID art = 0;
    ImFont *big = nullptr, *small = nullptr, *text = nullptr, *title = nullptr;
    void (*icon)(ImDrawList*, uint64_t id, ImVec2 centre, float size, ImU32 tint) = nullptr;
};
void bind(const Resources& r);
const Resources& res();

// --- art ---------------------------------------------------------------
// A piece of the game's art, its own size times `scale`, centred on c.
void piece(ImDrawList* d, const art::Piece& p, ImVec2 c, float scale = 1, ImU32 tint = kWhite);
// Stretched into a box.
void pieceIn(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, ImU32 tint = kWhite);
// Nine-slice: corners (insets l, t, r, b in the piece's own pixels, drawn k
// times that) kept, edges and middle stretched.
void nine(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, float l, float t, float r, float bt, float k = 1,
          ImU32 tint = kWhite);

// --- panels ------------------------------------------------------------
// The game's light dialog panel (DialogPanel art), tinted `tint`.
void dialogPanel(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32(244, 247, 252, 255));
// SimInteractionDialog's blue speech bubble (tail on the left, at the top)
// and the light answer bubble (tail on the right, low; drawn twice over, as
// the game draws it over its dimming, here it is over the world).
void speechBubble(ImDrawList* d, ImVec2 a, ImVec2 b);
void answerBubble(ImDrawList* d, ImVec2 a, ImVec2 b);

// --- text --------------------------------------------------------------
void shadowText(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text);
// Wrapped text in `font`; returns its size (draw=false: measure only).
ImVec2 text(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const std::string& s, float wrap,
            bool draw = true);

// --- buttons -----------------------------------------------------------
// One dialog button (DialogButton: the white pill, its over picture under the
// pointer) with `label` in the game's button blue; true on release. `id` must
// be unique among the frame's buttons.
bool dialogButton(ImDrawList* d, int id, const std::string& label, ImVec2 a, ImVec2 b);

// --- discs, pictures, badges --------------------------------------------
// The mood disc of the action queue (InteractionButtonEmotionBg) or of its
// side action (MixerInteractionButton): `mood` indexes hud's MOODS (0 the
// default frame), `diameter` in pixels, drawn `alpha` 0..255.
enum class Disc { kQueue, kMixer };
void moodDisc(ImDrawList* d, Disc kind, uint32_t mood, ImVec2 c, float diameter, int alpha = 255);
// A Sim's or object's picture on a mood disc, the way the game's queue draws a
// button: `px` atlas pixels per game pixel, `full` running / starting (else
// queued: faint disc, dimmer picture), `over` pointed at, `cancel` shows the X.
// `picture` is an icon resource or a thumbnail id (thumbs.cpp).
void pictureButton(ImDrawList* d, uint64_t picture, uint32_t mood, ImVec2 c, float px, bool full, bool over,
                   bool cancel);
// The small side-action button (mixer badge) of a picture button.
void badge(ImDrawList* d, uint64_t picture, uint32_t mood, ImVec2 c, float px, bool full, bool over, bool cancel);
// The progress ring under a running button: dark back, green spokes
// (100 round, clockwise from the top) up to `done` 0..1, and the rim.
void progressRing(ImDrawList* d, ImVec2 c, float px, float done);
// A Sim's picture in a portrait ring (SimSpeechHeader / Footer): `ring` is
// art::kPortraitRing or art::kYourRing.
void portrait(ImDrawList* d, uint64_t picture, const art::Piece& ring, ImVec2 c, float size);
// The icon / thumbnail `id` centred in a box.
void icon(ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint = kWhite);
// The tint of mood `mood` (art::kMoodTint) at `alpha`.
ImU32 moodTint(uint32_t mood, int alpha = 255);

// --- tooltip -----------------------------------------------------------
struct TipLine {
    ImFont* font;
    float size;
    ImU32 colour;
    const std::string* text;
};
// The game's tooltip: lines on the dialog panel, centred on `centreX` with
// its top at `top`; returns its size. Empty lines are skipped.
ImVec2 tooltip(ImDrawList* d, float centreX, float top, const TipLine* lines, int n, float wrap);
// The standard need / moodlet lines (name, meaning, reason, time left) in
// the game's tooltip styling.
void tipLines(TipLine out[4], const std::string& title, const std::string& text, const std::string& reason,
              const std::string& left);

}  // namespace s4vr::uikit
