#include "uikit.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace s4vr::uikit {

namespace {
Resources g_res;
}

void bind(const Resources& r) { g_res = r; }
const Resources& res() { return g_res; }

void piece(ImDrawList* d, const art::Piece& p, ImVec2 c, float scale, ImU32 tint) {
    if (!g_res.art) return;
    float w = p.w * scale, h = p.h * scale;
    d->AddImage(g_res.art, {c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2},
                {static_cast<float>(p.x) / art::kAtlasW, static_cast<float>(p.y) / art::kAtlasH},
                {static_cast<float>(p.x + p.w) / art::kAtlasW, static_cast<float>(p.y + p.h) / art::kAtlasH}, tint);
}

void pieceIn(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (!g_res.art) return;
    d->AddImage(g_res.art, a, b, {static_cast<float>(p.x) / art::kAtlasW, static_cast<float>(p.y) / art::kAtlasH},
                {static_cast<float>(p.x + p.w) / art::kAtlasW, static_cast<float>(p.y + p.h) / art::kAtlasH}, tint);
}

void nine(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, float l, float t, float r, float bt, float k,
          ImU32 tint) {
    if (!g_res.art) return;
    const float xs[4] = {a.x, a.x + l * k, b.x - r * k, b.x}, ys[4] = {a.y, a.y + t * k, b.y - bt * k, b.y};
    const float us[4] = {static_cast<float>(p.x), p.x + l, p.x + p.w - r, static_cast<float>(p.x + p.w)};
    const float vs[4] = {static_cast<float>(p.y), p.y + t, p.y + p.h - bt, static_cast<float>(p.y + p.h)};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            d->AddImage(g_res.art, {xs[i], ys[j]}, {xs[i + 1], ys[j + 1]},
                        {us[i] / art::kAtlasW, vs[j] / art::kAtlasH}, {us[i + 1] / art::kAtlasW, vs[j + 1] / art::kAtlasH},
                        tint);
        }
}

void dialogPanel(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 tint) { nine(d, art::kDialogPanel, a, b, 36, 36, 36, 36, 1, tint); }
// (Corners at 0.6: the tail and roundings as the game's are to its text.)
void speechBubble(ImDrawList* d, ImVec2 a, ImVec2 b) { nine(d, art::kSpeechBubble, a, b, 66, 108, 66, 66, 0.6f); }
void answerBubble(ImDrawList* d, ImVec2 a, ImVec2 b) {
    for (int k = 0; k < 2; ++k) nine(d, art::kAnswerBubble, a, b, 60, 60, 66, 138, 0.6f);
}

void shadowText(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
    d->AddText(font, size, {at.x + 2, at.y + 2}, kShadow, text);
    d->AddText(font, size, at, colour, text);
}

ImVec2 text(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const std::string& s, float wrap, bool draw) {
    if (s.empty()) return {0, 0};
    if (draw) d->AddText(font, size, at, colour, s.c_str(), nullptr, wrap);
    return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str());
}

bool dialogButton(ImDrawList* d, int id, const std::string& label, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(300 + id);
    bool pressed = ImGui::InvisibleButton("answer", {b.x - a.x, b.y - a.y});
    bool hover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hover)
        nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.8f);
    else
        nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.8f);
    const float size = g_res.text->FontSize * 0.92f, wrap = b.x - a.x - 60;
    ImVec2 ts = g_res.text->CalcTextSizeA(size, FLT_MAX, wrap, label.c_str());
    ImVec2 at{(a.x + b.x - ts.x) / 2, a.y + (b.y - a.y - 10 - ts.y) / 2};
    d->AddText(g_res.text, size, at, kButtonBlue, label.c_str(), nullptr, wrap);
    return pressed;
}

void icon(ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint) {
    if (g_res.icon) g_res.icon(d, id, c, size, tint);
}

ImU32 moodTint(uint32_t mood, int alpha) {
    ImU32 c = art::kMoodTint[mood < sizeof(art::kMoodTint) / sizeof(art::kMoodTint[0]) ? mood : 0];
    return (c & 0x00ffffffu) | (static_cast<ImU32>(alpha) << 24);
}

void moodDisc(ImDrawList* d, Disc kind, uint32_t mood, ImVec2 c, float diameter, int alpha) {
    const bool queue = kind == Disc::kQueue;
    const size_t n = queue ? sizeof(art::kQueueDisc) / sizeof(art::kQueueDisc[0]) : sizeof(art::kMixerDisc) / sizeof(art::kMixerDisc[0]);
    const art::Piece& disc = (queue ? art::kQueueDisc : art::kMixerDisc)[mood < n ? mood : 0];
    piece(d, disc, c, diameter / disc.w, IM_COL32(255, 255, 255, alpha));
}

// The queue button as the game's InteractionButton.as: disc 0.917 of 60 px
// (1.0 pointed at), 1 px low, faint when queued; picture 44 px (48 over),
// queued at 0.8 alpha (0.9); the X when it can be cancelled and pointed at.
void pictureButton(ImDrawList* d, uint64_t picture, uint32_t mood, ImVec2 c, float px, bool full, bool over, bool cancel) {
    moodDisc(d, Disc::kQueue, mood, {c.x, c.y + 1 * px}, (over ? 1.0f : 0.917f) * 60 * px, full ? 255 : 51);
    icon(d, picture, c, (over ? 48 : 44) * px, IM_COL32(255, 255, 255, full ? 255 : over ? 229 : 204));
    if (over && cancel) piece(d, art::kCancel, c, 36 * px / art::kCancel.w);
}

// MixerInteractionButton: disc 0.889 of 36 px (1.0 over), picture 24 px (28), its own X.
void badge(ImDrawList* d, uint64_t picture, uint32_t mood, ImVec2 c, float px, bool full, bool over, bool cancel) {
    moodDisc(d, Disc::kMixer, mood, c, (over ? 1.0f : 0.889f) * 36 * px, full ? 255 : 51);
    icon(d, picture, c, (over ? 28 : 24) * px, IM_COL32(255, 255, 255, full ? 255 : over ? 229 : 204));
    if (over && cancel) piece(d, art::kMixerCancel, {c.x - 1 * px, c.y}, 22 * px / art::kMixerCancel.w);
}

void progressRing(ImDrawList* d, ImVec2 c, float px, float done) {
    done = std::max(0.0f, std::min(1.0f, done));
    if (!(done > 0) || !g_res.art) return;
    piece(d, art::kProgressShadow, c, 62 * px / art::kProgressShadow.w);
    const art::Piece& s = art::kProgressSpoke;
    const ImVec2 uv0{static_cast<float>(s.x) / art::kAtlasW, static_cast<float>(s.y) / art::kAtlasH};
    const ImVec2 uv1{static_cast<float>(s.x + s.w) / art::kAtlasW, static_cast<float>(s.y + s.h) / art::kAtlasH};
    const float len = 30 * px, half = 1 * px;
    const int spokes = static_cast<int>(done * 100 + 0.5f);
    for (int k = 0; k < spokes; ++k) {
        const float a = k * 6.2831853f / 100;
        const ImVec2 dir{std::sin(a), -std::cos(a)}, side{std::cos(a), std::sin(a)};
        d->AddImageQuad(g_res.art, {c.x - side.x * half, c.y - side.y * half},
                        {c.x + dir.x * len - side.x * half, c.y + dir.y * len - side.y * half},
                        {c.x + dir.x * len + side.x * half, c.y + dir.y * len + side.y * half},
                        {c.x + side.x * half, c.y + side.y * half}, uv0, {uv1.x, uv0.y}, uv1, {uv0.x, uv1.y});
    }
    piece(d, art::kProgressRim, c, 58 * px / art::kProgressRim.w);
}

void portrait(ImDrawList* d, uint64_t picture, const art::Piece& ring, ImVec2 c, float size) {
    // The ring's inner circle is 102 of its 138 pixels.
    piece(d, ring, c, size * (138.0f / 102.0f) / ring.w);
    icon(d, picture, c, size);
}

void tipLines(TipLine out[4], const std::string& title, const std::string& text, const std::string& reason,
              const std::string& left) {
    out[0] = {g_res.small, g_res.small->FontSize, kTitleBlue, &title};
    out[1] = {g_res.text, g_res.text->FontSize * 0.9f, kBodyDark, &text};
    out[2] = {g_res.text, g_res.text->FontSize * 0.8f, kReasonGrey, &reason};
    out[3] = {g_res.text, g_res.text->FontSize * 0.8f, kBodyDark, &left};
}

ImVec2 tooltip(ImDrawList* d, float centreX, float top, const TipLine* lines, int n, float wrap) {
    const float pad = 34;
    float w = 0, h = 0;
    ImVec2 sizes[8];
    n = std::min(n, 8);
    for (int i = 0; i < n; ++i) {
        sizes[i] = lines[i].text->empty() ? ImVec2{0, 0}
                                          : lines[i].font->CalcTextSizeA(lines[i].size, FLT_MAX, wrap, lines[i].text->c_str());
        w = std::max(w, sizes[i].x);
        if (sizes[i].y > 0) h += sizes[i].y + (h > 0 ? 10 : 0);
    }
    w += 2 * pad;
    h += 2 * pad - 6;
    const float x0 = centreX - w / 2;
    dialogPanel(d, {x0, top}, {x0 + w, top + h}, IM_COL32(244, 247, 252, 250));
    float y = top + pad - 4;
    for (int i = 0; i < n; ++i) {
        if (sizes[i].y <= 0) continue;
        d->AddText(lines[i].font, lines[i].size, {x0 + pad, y}, lines[i].colour, lines[i].text->c_str(), nullptr, wrap);
        y += sizes[i].y + 10;
    }
    return {w, h};
}

}  // namespace s4vr::uikit
