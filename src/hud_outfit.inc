// Planning outfits in the room (outfit.py; user 2026-10-29 19:36Z / 19:43Z, research/CAS-VR.md 4b; built
// 2026-10-31, docs/OUTFIT-LIVE-PROGRESS.md): the outfit panel, drawn by us in the game's dialog art (never a crop
// of the game's UI). The CAS categories with the game's own names and icons (the CASMenuItem tuning), the catalog
// as the game's CAS thumbnails (thumbs/cas/c_<part>_l_<f|m>, what CASCatalogProduct.LoadThumbnail asks for), the
// worn part's colours (the game's GetCatalogColorFamilies) and Undo / Redo / Remove / Cancel / Done. Laid out for
// where it shows (Frame.outfitAspect, its height / width): on a mirror's glass or in front of you the categories
// run down the left; lying in a drawer the tabs run along the top and the buttons down the right. A button's
// index is outfit.py's control code (kOutfitButton). Included by hud.cpp.

namespace {

struct OutfitButton {  // a category (the CAS context menu's group) or a tab over the grid
    std::string key, nameJson;
    uint64_t icon = 0, selected = 0;
};
struct OutfitPart {
    uint64_t id = 0, picture = 0;  // the CAS part, its thumbnail's HUD picture id (FNV-1a64 of "thumbs/" + path)
    std::string path;              // "cas/c_<id>_l_<f|m>"
    bool worn = false, locked = false;
    std::string tipJson;  // why it is locked (its locked tooltip)
};
struct OutfitColour {
    uint64_t id = 0;  // the colour's own part
    uint32_t argb = 0;
    bool worn = false;
};
struct OutfitSaveCat {  // Save As: an outfit category (Everyday, Formal...)
    int cat = 0;
    std::string nameJson;
    bool on = false;
};
struct OutfitContent {
    bool valid = false;
    int opened = 0;  // which opening of the panel (a new one starts at the worn part's page)
    int kind = 0, flags = 0, tab = 0, serial = 0, hint = -1, group = -1;
    std::string titleJson, messageJson;
    std::vector<OutfitButton> groups, tabs;
    std::vector<OutfitPart> parts;
    std::vector<OutfitColour> colours;
    std::vector<OutfitSaveCat> saveCats;  // Save As
    int saveCat = 0, saveSlot = -1, saveCount = 0, saveMax = 5, saveWorn = -1, saveDeleted = 0;
};
// The header's flags (outfit.py _send).
constexpr int kOutfitCanUndo = 1, kOutfitCanRedo = 2, kOutfitCanRemove = 4, kOutfitDrawer = 16, kOutfitGlass = 32,
              kOutfitSaving = 64;  // glass: on a mirror, no background (user 2026-11-04); saving: the Save As screen
// outfit.py's control codes (C_*).
constexpr int kOcTab = 0, kOcGroup = 100, kOcPart = 1000, kOcColour = 70000, kOcUndo = 80000, kOcRedo = 80001,
              kOcRemove = 80002, kOcDone = 80003, kOcCancel = 80004, kOcSaveAs = 80005, kOcSaveHere = 80006,
              kOcDelete = 80007, kOcBack = 80008, kOcSaveCat = 81000, kOcSlot = 82000;
std::mutex g_outfitMutex;
OutfitContent g_outfitIn;  // set by Python (g_outfitMutex)
bool g_outfitNew = false;  // g_outfitMutex
OutfitContent g_outfit;    // present thread
int g_outfitPage = 0;            // present thread: the grid's page
bool g_outfitToHint = false;     // present thread: a new list: its page with the worn part comes first
int g_outfitAskedPage = -1;      // present thread: the page whose thumbnails (and the next page's) were asked for
ULONGLONG g_outfitAskedAt = 0;
std::atomic<float> g_outfitDrawn{0};  // the panel's height in kOutfitArea; 0 while not drawn
std::vector<uint64_t> g_outfitIcons;  // the categories' icons (g_itemMutex; missingIcons)

uint64_t outfitHash(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

// A pressable area: true on release over it.
bool outfitHit(int id, ImVec2 a, ImVec2 b, bool* over = nullptr, bool* held = nullptr) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(0x0F000000 + id);
    const bool pressed = ImGui::InvisibleButton("outfit", {std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)});
    if (over) *over = ImGui::IsItemHovered();
    if (held) *held = ImGui::IsItemActive();
    ImGui::PopID();
    return pressed;
}

// The game's dialog button (uikit::dialogButton's look, our own ids); greyed and not pressable while `on` is false.
bool outfitButton(ImDrawList* d, int id, const char* label, ImVec2 a, ImVec2 b, bool on) {
    bool over = false;
    const bool pressed = on && outfitHit(id, a, b, &over);
    const float k = std::min(0.8f, (b.y - a.y) / 100.0f);
    if (over)
        nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, k);
    else
        nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, k, on ? kWhite : IM_COL32(255, 255, 255, 120));
    const float size = g_text->FontSize * std::min(0.92f, (b.y - a.y) / 92.0f), wrap = b.x - a.x - 16;
    const ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, label);
    d->AddText(g_text, size, {(a.x + b.x - ts.x) / 2, a.y + (b.y - a.y - 8 - ts.y) / 2},
               on ? kButtonBlue : IM_COL32(30, 70, 128, 100), label, nullptr, wrap);
    return pressed;
}

// A category's round button (the CAS context menu's): its game icon on a white disc; the open one on blue with
// its selected icon [guess: the icon is a dark glyph and the selected icon a light one].
bool outfitDisc(ImDrawList* d, int id, const OutfitButton& button, ImVec2 c, float size, bool lit) {
    bool over = false, held = false;
    const bool pressed = outfitHit(id, {c.x - size / 2, c.y - size / 2}, {c.x + size / 2, c.y + size / 2}, &over, &held);
    const float r = size / 2;
    d->AddCircleFilled({c.x + 2, c.y + 4}, r, IM_COL32(0, 0, 0, 50), 48);
    d->AddCircleFilled(c, r, lit ? IM_COL32(58, 128, 210, 255) : held ? IM_COL32(220, 232, 246, 255) : kWhite, 48);
    const ImU32 ring = lit ? IM_COL32(36, 92, 168, 255) : over ? IM_COL32(120, 190, 80, 255) : IM_COL32(176, 188, 206, 255);
    d->AddCircle(c, r - 1.5f, ring, 48, over && !lit ? 5.0f : 2.5f);
    icon(d, lit && button.selected ? button.selected : button.icon, c, size * 0.74f);
    return pressed;
}

// A catalog cell (CASClothingPanelCellView): the part's CAS thumbnail on a white card, the worn one outlined in
// the selection green, a locked one dimmed with the lock (it can't be picked).
bool outfitCell(ImDrawList* d, int id, const OutfitPart& p, ImVec2 a, ImVec2 b, bool* over) {
    bool held = false;
    const bool pressed = outfitHit(id, a, b, over, &held);
    const float w = b.x - a.x, h = b.y - a.y, round = w * 0.07f;
    d->AddRectFilled({a.x + 2, a.y + 4}, {b.x + 2, b.y + 4}, IM_COL32(0, 0, 0, 45), round);
    d->AddRectFilled(a, b, held && !p.locked ? IM_COL32(226, 236, 248, 255) : kWhite, round);
    icon(d, p.picture, {a.x + w / 2, a.y + h / 2}, std::min(w, h) - 10, p.locked ? IM_COL32(255, 255, 255, 110) : kWhite);
    if (p.worn)
        d->AddRect({a.x + 2, a.y + 2}, {b.x - 2, b.y - 2}, IM_COL32(110, 200, 60, 255), round, 0, 6.0f);
    else if (*over && !p.locked)
        d->AddRect({a.x + 1, a.y + 1}, {b.x - 1, b.y - 1}, IM_COL32(120, 190, 80, 220), round, 0, 3.5f);
    else
        d->AddRect(a, b, IM_COL32(196, 204, 218, 255), round, 0, 1.5f);
    if (p.locked) piece(d, art::kInvLock, {b.x - w * 0.17f, a.y + w * 0.17f}, w * 0.24f / art::kInvLock.w);
    return pressed && !p.locked;
}

// A colour of the worn part: a chip in its colour (ColorSwatchPanel's cells), the worn one ringed green.
bool outfitChip(ImDrawList* d, int id, const OutfitColour& c, ImVec2 at, float size) {
    bool over = false;
    const bool pressed = outfitHit(id, {at.x - size / 2, at.y - size / 2}, {at.x + size / 2, at.y + size / 2}, &over);
    d->AddCircleFilled({at.x + 1, at.y + 3}, size / 2, IM_COL32(0, 0, 0, 50), 32);
    d->AddCircleFilled(at, size / 2, kWhite, 32);
    d->AddCircleFilled(at, size / 2 - 4, fromArgb(c.argb | 0xff000000u), 32);
    if (c.worn)
        d->AddCircle(at, size / 2 + 3, IM_COL32(110, 200, 60, 255), 32, 5.0f);
    else if (over)
        d->AddCircle(at, size / 2 + 2, IM_COL32(120, 190, 80, 220), 32, 3.5f);
    return pressed;
}

// A page arrow: a white disc with a blue triangle (left or right); greyed at the ends.
bool outfitArrow(ImDrawList* d, int id, ImVec2 c, float size, bool right, bool on) {
    bool over = false;
    const bool pressed = on && outfitHit(id, {c.x - size / 2, c.y - size / 2}, {c.x + size / 2, c.y + size / 2}, &over);
    d->AddCircleFilled({c.x + 2, c.y + 4}, size / 2, IM_COL32(0, 0, 0, on ? 50 : 20), 40);
    d->AddCircleFilled(c, size / 2, on ? (over ? IM_COL32(224, 240, 214, 255) : kWhite) : IM_COL32(255, 255, 255, 110), 40);
    const float s = size * 0.22f, dir = right ? 1.0f : -1.0f;
    const ImU32 ink = on ? kButtonBlue : IM_COL32(30, 70, 128, 80);
    d->AddTriangleFilled({c.x + dir * s * 1.2f, c.y}, {c.x - dir * s * 0.8f, c.y - s * 1.3f}, {c.x - dir * s * 0.8f, c.y + s * 1.3f}, ink);
    return pressed;
}

// A button's chosen look: the selection green round it (as a worn catalog cell).
void outfitChosen(ImDrawList* d, ImVec2 a, ImVec2 b) {
    d->AddRect({a.x - 3, a.y - 3}, {b.x + 3, b.y + 3}, IM_COL32(110, 200, 60, 255), 14.0f, 0, 6.0f);
}

// The Save As screen (outfit.py _save_records): the outfit categories, the chosen one's slots (1..n and New) and
// Save here / Delete (Keep on a slot marked) / Back, where the panel's buttons are.
// The code of the button pressed, or -1.
int drawOutfitSave(ImDrawList* d, const OutfitContent& c, bool drawer, float x0, float y0, float x1, float y1, float H) {
    int pressed = -1;
    auto send = [&](int code) { pressed = code; };
    const float gap = 12;
    const bool picked = c.saveSlot >= 0, existing = picked && c.saveSlot < c.saveCount;
    const bool marked = existing && (c.saveDeleted >> c.saveSlot & 1) != 0;
    const char* labels[3] = {"Save here", marked ? "Keep" : "Delete", "Back"};
    const int codes[3] = {kOcSaveHere, kOcDelete, kOcBack};
    const bool on[3] = {picked && !marked, existing, true};
    float ax0 = x0, ax1 = x1, ay0 = y0, ay1 = y1;
    if (drawer) {
        const float bw = 196, bh = std::min(88.0f, (y1 - y0 - 5 * gap) / 6);
        for (int k = 0; k < 3; ++k) {
            const ImVec2 a{x1 - bw, y0 + k * (bh + gap)}, b{x1, y0 + k * (bh + gap) + bh};
            if (outfitButton(d, 30 + k, labels[k], a, b, on[k])) send(codes[k]);
        }
        ax1 = x1 - bw - 2 * gap;
    } else {
        const float bh = std::max(64.0f, std::min(96.0f, H * 0.07f)), bw = (x1 - x0 - 2 * gap) / 3;
        for (int k = 0; k < 3; ++k) {
            const ImVec2 a{x0 + k * (bw + gap), y1 - bh}, b{x0 + k * (bw + gap) + bw, y1};
            if (outfitButton(d, 30 + k, labels[k], a, b, on[k])) send(codes[k]);
        }
        ay1 = y1 - bh - gap;
    }
    const float titleSize = g_title->FontSize * (drawer ? 0.9f : 1.05f);
    d->AddText(g_title, titleSize, {ax0 + 4, ay0}, kTitleBlue, found(c.titleJson).c_str());
    ay0 += titleSize + gap;
    // The message line at the bottom.
    const float lineH = g_text->FontSize * 0.85f;
    ay1 -= lineH + 6;
    const std::string message = found(c.messageJson);
    if (!message.empty()) {
        d->PushClipRect({ax0, ay1 + 2}, {ax1, ay1 + lineH + 10}, true);
        d->AddText(g_text, g_text->FontSize * 0.8f, {ax0 + 4, ay1 + 4}, kBodyDark, message.c_str());
        d->PopClipRect();
    }
    // The categories: four to a row.
    const int n = static_cast<int>(c.saveCats.size()), perRow = 4;
    const int rows = (n + perRow - 1) / perRow;
    const float room = ay1 - ay0;
    const float ch = std::max(48.0f, std::min(drawer ? 72.0f : 88.0f, room / (rows + 2.6f) - gap));
    const float cw = (ax1 - ax0 - (perRow - 1) * gap) / perRow;
    for (int i = 0; i < n; ++i) {
        const ImVec2 a{ax0 + (i % perRow) * (cw + gap), ay0 + (i / perRow) * (ch + gap)};
        const ImVec2 b{a.x + cw, a.y + ch};
        if (outfitButton(d, 400 + i, found(c.saveCats[i].nameJson).c_str(), a, b, true) && !c.saveCats[i].on)
            send(kOcSaveCat + c.saveCats[i].cat);
        if (c.saveCats[i].on) outfitChosen(d, a, b);
    }
    ay0 += rows * (ch + gap) + gap;
    // The chosen category's slots, and New while it has room.
    const int slots = c.saveCount + (c.saveCount < c.saveMax ? 1 : 0);
    const int columns = std::max(c.saveMax + 1, slots);
    const float sw = (ax1 - ax0 - (columns - 1) * gap) / columns;
    const float sh = std::max(56.0f, std::min(ch * 1.6f, ay1 - ay0 - gap));
    for (int i = 0; i < slots; ++i) {
        char label[32];
        if (i == c.saveCount)
            std::snprintf(label, sizeof label, "+ New");
        else if (c.saveDeleted >> i & 1)
            std::snprintf(label, sizeof label, "%d (deleted)", i + 1);
        else if (i == c.saveWorn)
            std::snprintf(label, sizeof label, "%d (wearing)", i + 1);
        else
            std::snprintf(label, sizeof label, "%d", i + 1);
        const ImVec2 a{ax0 + i * (sw + gap), ay0}, b{a.x + sw, ay0 + sh};
        if (outfitButton(d, 500 + i, label, a, b, true) && i != c.saveSlot) send(kOcSlot + i);
        if (i == c.saveSlot) outfitChosen(d, a, b);
    }
    return pressed;
}

void resolveOutfit() {
    std::lock_guard<std::mutex> lock(g_outfitMutex);
    if (!g_outfitNew) return;
    g_outfitNew = false;
    if (!g_outfit.valid || g_outfitIn.opened != g_outfit.opened || g_outfitIn.serial != g_outfit.serial ||
        g_outfitIn.tab != g_outfit.tab || g_outfitIn.group != g_outfit.group || g_outfitIn.kind != g_outfit.kind) {
        g_outfitToHint = true;
        g_outfitAskedPage = -1;
    }
    g_outfit = g_outfitIn;
}

// Present thread (render): the panel in kOutfitArea, `aspect` (height / width) the shape of its place.
Action drawOutfit(float aspect, int* index) {
    resolveOutfit();
    const OutfitContent& c = g_outfit;
    if (!c.valid) {
        g_outfitDrawn = 0;
        return kNone;
    }
    const Rect r = kOutfitArea;
    const float W = r.w, H = std::floor(W * std::max(0.375f, std::min(1.5f, aspect)));
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    if (!(c.flags & kOutfitGlass)) uikit::dialogPanel(d, {r.x + 4, r.y + 4}, {r.x + W - 4, r.y + H - 4});
    Action action = kNone;
    auto send = [&](int code) {
        action = kOutfitButton;
        if (index) *index = code;
    };
    const bool drawer = (c.flags & kOutfitDrawer) != 0;
    const float pad = 26, gap = 12;
    const float x0 = r.x + pad, x1 = r.x + W - pad, y0 = r.y + pad, y1 = r.y + H - pad;
    if (c.flags & kOutfitSaving) {
        const int code = drawOutfitSave(d, c, drawer, x0, y0, x1, y1, H);
        if (code >= 0) send(code);
        g_outfitDrawn = H;
        return action;
    }
    std::string message = found(c.messageJson);

    // The buttons: along the bottom (mirror, in front) or down the right (drawer).
    static const char* const kLabels[6] = {"Undo", "Redo", "Remove", "Save As", "Cancel", "Done"};
    static const int kCodes[6] = {kOcUndo, kOcRedo, kOcRemove, kOcSaveAs, kOcCancel, kOcDone};
    const bool on[6] = {(c.flags & kOutfitCanUndo) != 0, (c.flags & kOutfitCanRedo) != 0, (c.flags & kOutfitCanRemove) != 0,
                        true, true, true};
    float ax0 = x0, ax1 = x1, ay0 = y0, ay1 = y1;  // what is left for the rest
    if (drawer) {
        const float bw = 196, bh = std::min(88.0f, (y1 - y0 - 5 * gap) / 6);
        for (int k = 0; k < 6; ++k) {
            const ImVec2 a{x1 - bw, y0 + k * (bh + gap)}, b{x1, y0 + k * (bh + gap) + bh};
            if (outfitButton(d, 10 + k, kLabels[k], a, b, on[k])) send(kCodes[k]);
        }
        ax1 = x1 - bw - 2 * gap;
    } else {
        const float bh = std::max(64.0f, std::min(96.0f, H * 0.07f)), bw = (x1 - x0 - 5 * gap) / 6;
        for (int k = 0; k < 6; ++k) {
            const ImVec2 a{x0 + k * (bw + gap), y1 - bh}, b{x0 + k * (bw + gap) + bw, y1};
            if (outfitButton(d, 10 + k, kLabels[k], a, b, on[k])) send(kCodes[k]);
        }
        ay1 = y1 - bh - gap;
    }

    // The categories: a column of round buttons down the left (the CAS context menu's), on a mirror or in front.
    if (!c.groups.empty()) {
        const int n = static_cast<int>(c.groups.size());
        const float disc = std::max(48.0f, std::min(132.0f, (ay1 - ay0) / n - 14));
        for (int g = 0; g < n; ++g) {
            const ImVec2 at{ax0 + disc / 2 + 4, ay0 + disc / 2 + 4 + g * (disc + 14)};
            if (outfitDisc(d, 100 + g, c.groups[g], at, disc, g == c.group) && g != c.group) send(kOcGroup + g);
        }
        ax0 += disc + 8 + 2 * gap;
    }

    // The title (the open category's name) or, with several categories here, their tabs.
    if (c.tabs.size() > 1) {
        const int n = static_cast<int>(c.tabs.size());
        const float disc = std::max(44.0f, std::min(drawer ? 84.0f : 96.0f, (ax1 - ax0) / n - 12));
        for (int t = 0; t < n; ++t) {
            const ImVec2 at{ax0 + disc / 2 + t * (disc + 12), ay0 + disc / 2 + 2};
            if (outfitDisc(d, 200 + t, c.tabs[t], at, disc, t == c.tab) && t != c.tab) send(kOcTab + t);
        }
        ay0 += disc + gap + 4;
        if (!drawer) {
            const std::string& title = found(c.titleJson);
            const float size = g_title->FontSize * 0.9f;
            d->AddText(g_title, size, {ax0 + 4, ay0}, kTitleBlue, title.c_str());
            ay0 += size + gap;
        }
    } else {
        const std::string& title = found(c.titleJson);
        const float size = g_title->FontSize * (drawer ? 0.9f : 1.05f);
        d->AddText(g_title, size, {ax0 + 4, ay0}, kTitleBlue, title.c_str());
        ay0 += size + gap;
    }

    // The worn part's colours: a row (two at most) of chips, as the CAS swatch panel.
    const int colours = static_cast<int>(c.colours.size());
    if (colours > 0) {
        const float chip = drawer ? 48 : 56, step = chip + 14;
        const int perRow = std::max(1, static_cast<int>((ax1 - ax0 + 14) / step));
        const int rows = std::min(2, (colours + perRow - 1) / perRow);
        const float cy0 = drawer ? ay1 - rows * step : ay0;
        for (int i = 0; i < std::min(colours, rows * perRow); ++i) {
            const ImVec2 at{ax0 + chip / 2 + 4 + (i % perRow) * step, cy0 + chip / 2 + 4 + (i / perRow) * step};
            if (outfitChip(d, 300 + i, c.colours[i], at, chip) && !c.colours[i].worn) send(kOcColour + i);
        }
        if (drawer)
            ay1 -= rows * step + 4;
        else
            ay0 += rows * step + 4;
    }

    // A line for the message (a part that can't be worn) or the lock of the cell pointed at.
    const float lineH = g_text->FontSize * 0.85f;
    ay1 -= lineH + 6;
    const float lineY = ay1 + 4;

    // The grid with its page arrows: below them (mirror, in front) or at its ends (drawer).
    const float arrow = drawer ? 72 : 80;
    float gx0 = ax0, gx1 = ax1, gy0 = ay0, gy1 = ay1;
    if (drawer)
        gx0 += arrow + gap, gx1 -= arrow + gap;
    else
        gy1 -= arrow + gap;
    const float cellGap = 14, ratio = 1.22f;  // a cell's height / width
    int cols, rows;
    float cw, ch;
    if (drawer) {
        rows = std::max(1, std::min(3, static_cast<int>(std::lround((gy1 - gy0) / 140.0f))));
        ch = (gy1 - gy0 - (rows - 1) * cellGap) / rows;
        cw = ch / ratio;
        cols = std::max(1, static_cast<int>((gx1 - gx0 + cellGap) / (cw + cellGap)));
    } else {
        cols = gx1 - gx0 > 700 ? 4 : 3;
        cw = (gx1 - gx0 - (cols - 1) * cellGap) / cols;
        ch = cw * ratio;
        rows = std::max(1, static_cast<int>((gy1 - gy0 + cellGap) / (ch + cellGap)));
        if (rows * (ch + cellGap) - cellGap > gy1 - gy0) {
            ch = gy1 - gy0;
            cw = std::min(cw, ch / ratio);
        }
    }
    const int perPage = std::max(1, cols * rows), parts = static_cast<int>(c.parts.size());
    const int pages = std::max(1, (parts + perPage - 1) / perPage);
    if (g_outfitToHint) {
        g_outfitPage = c.hint >= 0 ? c.hint / perPage : 0;
        g_outfitToHint = false;
    }
    g_outfitPage = std::max(0, std::min(pages - 1, g_outfitPage));
    // The thumbnails of this page and the next: asked for once per page shown, and again every few seconds (a
    // picture dropped from the HUD's slots comes back).
    const ULONGLONG now = GetTickCount64();
    if (g_outfitAskedPage != g_outfitPage || now - g_outfitAskedAt > 3000) {
        g_outfitAskedPage = g_outfitPage;
        g_outfitAskedAt = now;
        for (int i = g_outfitPage * perPage; i < std::min(parts, (g_outfitPage + 2) * perPage); ++i)
            thumbs::request(c.parts[i].picture, c.parts[i].path.c_str());
    }
    const float usedW = cols * cw + (cols - 1) * cellGap, left = gx0 + (gx1 - gx0 - usedW) / 2;
    int pointed = -1;
    for (int k = 0; k < perPage; ++k) {
        const int i = g_outfitPage * perPage + k;
        if (i >= parts) break;
        const ImVec2 a{left + (k % cols) * (cw + cellGap), gy0 + (k / cols) * (ch + cellGap)};
        const ImVec2 b{a.x + cw, a.y + ch};
        bool over = false;
        if (outfitCell(d, 1000 + k, c.parts[i], a, b, &over) && !c.parts[i].worn) send(kOcPart + i);
        if (over) pointed = i;
    }
    if (!parts) {
        const char* none = "Nothing here to wear";
        const ImVec2 ns = g_text->CalcTextSizeA(g_text->FontSize, FLT_MAX, 0, none);
        d->AddText(g_text, g_text->FontSize, {(gx0 + gx1 - ns.x) / 2, (gy0 + gy1 - ns.y) / 2}, kBodyDark, none);
    }
    // The page arrows and "page / pages".
    {
        char text[24];
        std::snprintf(text, sizeof text, "%d / %d", g_outfitPage + 1, pages);
        const float size = g_small->FontSize * 0.9f;
        const ImVec2 ts = g_small->CalcTextSizeA(size, FLT_MAX, 0, text);
        ImVec2 back, next, at;
        if (drawer) {
            const float my = (gy0 + gy1) / 2;
            back = {gx0 - gap - arrow / 2, my}, next = {gx1 + gap + arrow / 2, my};
            at = {gx1 + gap + arrow / 2 - ts.x / 2, my + arrow / 2 + 6};
        } else {
            const float my = gy1 + gap + arrow / 2;
            back = {gx0 + arrow / 2, my}, next = {gx1 - arrow / 2, my};
            at = {(gx0 + gx1 - ts.x) / 2, my - ts.y / 2};
        }
        if (outfitArrow(d, 20, back, arrow, false, g_outfitPage > 0)) --g_outfitPage;
        if (outfitArrow(d, 21, next, arrow, true, g_outfitPage + 1 < pages)) ++g_outfitPage;
        if (pages > 1 && (!drawer || at.y + ts.y < gy1 + 4)) d->AddText(g_small, size, at, kTitleBlue, text);
    }
    // The message, or why the cell pointed at is locked.
    if (pointed >= 0 && c.parts[pointed].locked) message = found(c.parts[pointed].tipJson);
    if (!message.empty()) {
        const float size = g_text->FontSize * 0.8f;
        d->PushClipRect({ax0, lineY - 2}, {ax1, lineY + lineH + 4}, true);
        d->AddText(g_text, size, {ax0 + 4, lineY}, IM_COL32(176, 52, 40, 255), message.c_str());
        d->PopClipRect();
    }
    g_outfitDrawn = H;
    return action;
}

}  // namespace

void setOutfitContent(const char* data, size_t size) {
    OutfitContent c;
    const std::string all(data ? data : "", data ? size : 0);
    bool female = false;
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        const size_t end = all.find('\x1e', at);
        const std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? all.size() + 1 : end + 1;
        if (f[0] == "outfit" && f.size() >= 10) {
            c.valid = true;
            c.kind = std::atoi(f[1].c_str());
            female = f[2] == "f";
            c.flags = std::atoi(f[3].c_str());
            c.titleJson = f[4];
            c.tab = std::atoi(f[5].c_str());
            c.serial = std::atoi(f[6].c_str());
            c.hint = std::atoi(f[7].c_str());
            c.group = std::atoi(f[8].c_str());
            c.messageJson = f[9];
        } else if ((f[0] == "group" || f[0] == "tab") && f.size() >= 5) {
            OutfitButton b;
            b.key = f[1];
            b.nameJson = f[2];
            b.icon = std::strtoull(f[3].c_str(), nullptr, 10);
            b.selected = std::strtoull(f[4].c_str(), nullptr, 10);
            (f[0] == "group" ? c.groups : c.tabs).push_back(b);
        } else if (f[0] == "part" && f.size() >= 3) {
            OutfitPart p;
            p.id = std::strtoull(f[1].c_str(), nullptr, 10);
            const int bits = std::atoi(f[2].c_str());
            p.worn = (bits & 1) != 0;
            p.locked = (bits & 2) != 0;
            if (f.size() >= 4) p.tipJson = f[3];
            c.parts.push_back(p);
        } else if (f[0] == "savecat" && f.size() >= 4) {
            OutfitSaveCat k;
            k.cat = std::atoi(f[1].c_str());
            k.nameJson = f[2];
            k.on = f[3] == "1";
            c.saveCats.push_back(k);
        } else if (f[0] == "save" && f.size() >= 7) {
            c.saveCat = std::atoi(f[1].c_str());
            c.saveSlot = std::atoi(f[2].c_str());
            c.saveCount = std::atoi(f[3].c_str());
            c.saveMax = std::max(1, std::min(8, std::atoi(f[4].c_str())));
            c.saveWorn = std::atoi(f[5].c_str());
            c.saveDeleted = std::atoi(f[6].c_str());
        } else if (f[0] == "colour" && f.size() >= 4) {
            OutfitColour k;
            k.id = std::strtoull(f[1].c_str(), nullptr, 10);
            k.argb = static_cast<uint32_t>(std::strtoull(f[2].c_str(), nullptr, 10));
            k.worn = f[3] == "1";
            c.colours.push_back(k);
        }
        if (end == std::string::npos) break;
    }
    for (OutfitPart& p : c.parts) {
        p.path = "cas/c_" + std::to_string(p.id) + "_l_" + (female ? "f" : "m");
        p.picture = outfitHash("thumbs/" + p.path);
    }
    // (Python's thread only.)
    static int opened = 0;
    static bool was = false;
    if (c.valid && !was) ++opened;
    was = c.valid;
    c.opened = opened;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_outfitIcons.clear();
        for (const std::vector<OutfitButton>* list : {&c.groups, &c.tabs})
            for (const OutfitButton& b : *list) {
                g_outfitIcons.push_back(b.icon);
                if (b.selected) g_outfitIcons.push_back(b.selected);
            }
    }
    static int logged = 0;
    if (c.valid && logged++ < 40)
        LOG("HUD: outfit panel (kind %d): %zu categories, %zu tabs, %zu parts, %zu colours", c.kind, c.groups.size(),
            c.tabs.size(), c.parts.size(), c.colours.size());
    std::lock_guard<std::mutex> lock(g_outfitMutex);
    g_outfitIn = std::move(c);
    g_outfitNew = true;
}

bool outfitPending() {
    std::lock_guard<std::mutex> lock(g_outfitMutex);
    return g_outfitIn.valid;
}

float outfitHeight() { return g_outfitDrawn.load(); }
