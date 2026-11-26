// The build tablet's tool tray (user 2026-10-24 01:51Z, categories 02:00Z): build mode's keyboard-only hotkeys as
// buttons, right middle of the tablet (docs/BUILD-TRAY.md). Category tabs down the left, that category's buttons
// in two columns beside them, in the game's dialog art. Toggles light up (Frame.trayLit, bit per TrayTool);
// rotate / raise / scale repeat while held. Included by hud.cpp.

namespace {

struct TrayButton {
    int tool;
    const char* label;
    bool repeat;
    const art::Piece* icon;
};
struct TrayCategory {
    const char* name;
    const art::Piece* icon;
    TrayButton buttons[12];
    int count;
};
const TrayCategory kTrayCategories[] = {
    {"Objects", &art::kTrayCatObjects,
     {{kTrayFreePlace, "Free place", false, &art::kTrayFreePlace},
      {kTrayShift, "Place copies", false, &art::kTrayCopies},
      {kTraySnap, "Snap size", false, &art::kTraySnap},
      {kTrayGrid, "Grid", false, &art::kTrayGrid},
      {kTrayRotateLeft, "Turn left", true, &art::kTrayTurnLeft},
      {kTrayRotateRight, "Turn right", true, &art::kTrayTurnRight},
      {kTrayRaise, "Raise", true, &art::kTrayRaise},
      {kTrayLower, "Lower", true, &art::kTrayLower},
      {kTrayBigger, "Bigger", true, &art::kTrayBigger},
      {kTraySmaller, "Smaller", true, &art::kTraySmaller},
      {kTraySwatch, "Next swatch", false, &art::kTraySwatch},
      {kTraySlot, "Next slot", false, &art::kTraySlot}},
     12},
    {"Walls & floors", &art::kTrayCatWalls,
     {{kTrayShift, "Fill room", false, &art::kTrayFillRoom},
      {kTrayAlt, "One wall", false, &art::kTrayOneWall},
      {kTrayQuarter, "Quarter tiles", false, &art::kTrayQuarter},
      {kTrayGrid, "Grid", false, &art::kTrayGrid}},
     4},
    {"Roofs", &art::kTrayCatRoofs,
     {{kTrayShift, "One side eaves", false, &art::kTrayRoofEaves}, {kTrayAlt, "Free height", false, &art::kTrayRoofHeight}, {kTrayRoofCurve, "Curved roof", false, &art::kTrayRoofCurve}},
     3},
    {"Terrain", &art::kTrayCatTerrain, {{kTrayEraser, "Eraser", false, &art::kTrayEraser}}, 1},
    {"Cheats", &art::kTrayCatCheats,
     {{kTrayMoveObjects, "Move objects", false, &art::kTrayMoveObjects},
      {kTrayHidden, "Hidden objects", false, &art::kTrayHidden},
      {kTrayLiveEdit, "Live-edit objects", false, &art::kTrayLiveEdit}},
     3},
};
constexpr int kTrayCategoryCount = sizeof(kTrayCategories) / sizeof(kTrayCategories[0]);
int g_trayCategory = 0;  // the open category (starts on Objects: the laser trigger is a click there), -1 = closed (the tabs alone)
int g_trayHeld = -1;            // the repeating button held down
ULONGLONG g_trayRepeatAt = 0;   // its next repeat

// One button: the game's dialog button, green-tinted while lit; true on the frames it acts.
bool trayButton(ImDrawList* d, int id, const char* label, const art::Piece* icon, ImVec2 a, ImVec2 b, bool lit,
                bool repeat) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(9000 + id);
    bool pressed = ImGui::InvisibleButton("tray", {b.x - a.x, b.y - a.y});
    const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PopID();
    const ImU32 tint = lit ? IM_COL32(150, 235, 140, 255) : kWhite;
    if (hover || held)
        nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.6f, tint);
    else
        nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.6f, tint);
    // The icon (tools/make_tray_icons.py, the vanilla tool icons' look) over its name; green while on, as the
    // game's selected tools.
    const float size = g_text->FontSize * 0.56f, wrap = b.x - a.x - 16;
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, label);
    const float h = b.y - a.y - 8, iconPx = std::min(h - ts.y - 10, b.x - a.x - 24);
    const float top = a.y + (h - iconPx - ts.y - 2) / 2;
    if (icon && iconPx > 8) {
        const ImU32 glyph = lit ? IM_COL32(120, 214, 82, 255) : hover || held ? IM_COL32(200, 240, 170, 255) : kWhite;
        // The vanilla icons are white on the dark tool bar: a dark disc keeps them readable on the light button.
        d->AddCircleFilled({(a.x + b.x) / 2, top + iconPx / 2}, iconPx * 0.56f, IM_COL32(40, 70, 120, 235), 32);
        piece(d, *icon, {(a.x + b.x) / 2, top + iconPx / 2}, iconPx * 0.86f / icon->w, glyph);
    }
    d->AddText(g_text, size, {(a.x + b.x - ts.x) / 2, top + iconPx + 2}, kButtonBlue, label, nullptr, wrap);
    if (!repeat) return pressed;
    // Repeats: once on the press, then every 120 ms after the first 350 ms held.
    const ULONGLONG now = GetTickCount64();
    if (held && ImGui::IsMouseClicked(0)) {
        g_trayHeld = id;
        g_trayRepeatAt = now + 350;
        return true;
    }
    if (held && g_trayHeld == id && now >= g_trayRepeatAt) {
        g_trayRepeatAt = now + 120;
        return true;
    }
    if (!held && g_trayHeld == id) g_trayHeld = -1;
    return false;
}

Action drawBuildTray(const Frame& f, int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kBuildTray;
    // The tray sits at the tablet's far right (user 2026-10-24 03:14Z): its tabs down the right edge, the
    // category's buttons to their left. Closed (15:30Z: a category tapped again): the tabs alone.
    const float pad = 14, tabW = 136, tabX = r.x + r.w - pad - tabW;
    uikit::dialogPanel(d, {g_trayCategory < 0 ? tabX - pad : r.x, r.y}, {r.x + r.w, r.y + r.h});
    Action action = kNone;
    // The category tabs, top to bottom, and under them leaving first person (user 2026-10-24 02:02Z).
    const float tabH = (r.h - 2 * pad) / (kTrayCategoryCount + 1);
    for (int k = 0; k < kTrayCategoryCount; ++k) {
        ImVec2 a{tabX, r.y + pad + k * tabH}, b{tabX + tabW, r.y + pad + (k + 1) * tabH - 6};
        if (trayButton(d, 100 + k, kTrayCategories[k].name, kTrayCategories[k].icon, a, b, k == g_trayCategory, false))
            g_trayCategory = k == g_trayCategory ? -1 : k;
    }
    {
        const int k = kTrayCategoryCount;
        ImVec2 a{tabX, r.y + pad + k * tabH}, b{tabX + tabW, r.y + pad + (k + 1) * tabH - 6};
        if (trayButton(d, 120, "Exit first person", &art::kTrayExitFirstPerson, a, b, false, false)) {
            action = kBuildTool;
            if (index) *index = kTrayExitFirstPerson;
        }
    }
    // Its buttons, two columns, rows as tall as twelve would allow.
    if (g_trayCategory < 0) return action;
    const TrayCategory& cat = kTrayCategories[g_trayCategory];
    const float x0 = r.x + pad, colW = (tabX - 10 - x0 - 8) / 2, rowH = (r.h - 2 * pad) / 6;
    for (int i = 0; i < cat.count; ++i) {
        const TrayButton& t = cat.buttons[i];
        ImVec2 a{x0 + (i % 2) * (colW + 8), r.y + pad + (i / 2) * rowH};
        ImVec2 b{a.x + colW, a.y + rowH - 6};
        if (trayButton(d, g_trayCategory * 16 + i, t.label, t.icon, a, b, (f.trayLit >> t.tool) & 1, t.repeat)) {
            action = kBuildTool;
            if (index) *index = t.tool;
        }
    }
    return action;
}

// The phone camera app (a photo session): shutter, zoom, filter, flash, orientation, photo size and Close in the
// tray's place. Every button is a CameraTool; zoom repeats while held.
Action drawCameraTray(const Frame& f, int* index) {
    // No panel: small see-through round buttons over the picture (user 2026-11-02: the big white menu hid it).
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kBuildTray;
    Action action = kNone;
    auto lit = [&](int tool) { return ((f.trayLit >> (20 + tool - kCamShutter)) & 1) != 0; };
    // One round button at the centre (cx, cy) in tray pixels; true on the frames it acts (zoom repeats while held).
    auto circleButton = [&](int tool, const char* label, float cx, float cy, float radius, bool on, bool repeat) {
        const ImVec2 c{r.x + cx, r.y + cy};
        ImGui::SetCursorScreenPos({c.x - radius, c.y - radius});
        ImGui::PushID(9300 + tool);
        const bool pressed = ImGui::InvisibleButton("cam", {radius * 2, radius * 2});
        const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        ImGui::PopID();
        const ImU32 fill = on ? IM_COL32(120, 214, 82, 200) : hover || held ? IM_COL32(255, 255, 255, 120) : IM_COL32(0, 0, 0, 120);
        d->AddCircleFilled(c, radius, fill, 32);
        d->AddCircle(c, radius, IM_COL32(255, 255, 255, 230), 32, 3);
        const float size = std::min(g_text->FontSize * 0.62f, radius * 0.9f);
        const ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, 0, label);
        d->AddText(g_text, size, {c.x - ts.x / 2, c.y - ts.y / 2}, kWhite, label);
        bool act = pressed;
        if (repeat) {
            act = false;
            const ULONGLONG now = GetTickCount64();
            if (held && ImGui::IsMouseClicked(0)) {
                g_trayHeld = tool;
                g_trayRepeatAt = now + 350;
                act = true;
            } else if (held && g_trayHeld == tool && now >= g_trayRepeatAt) {
                g_trayRepeatAt = now + 120;
                act = true;
            }
            if (!held && g_trayHeld == tool) g_trayHeld = -1;
        }
        if (act) {
            action = kBuildTool;
            if (index) *index = tool;
        }
    };
    const float w = r.w, h = r.h;
    circleButton(kCamClose, "X", 50, 50, 34, false, false);
    circleButton(kCamFlash, "Fl", w / 2, 50, 34, lit(kCamFlash), false);
    circleButton(kCamOrient, "Rot", w - 50, 50, 34, false, false);
    circleButton(kCamFilterPrev, "<", 46, h * 0.40f, 34, false, false);
    circleButton(kCamFilterNext, ">", 46, h * 0.40f + 90, 34, false, false);
    circleButton(kCamZoomIn, "+", w - 46, h * 0.40f, 34, false, true);
    circleButton(kCamZoomOut, "-", w - 46, h * 0.40f + 90, 34, false, true);
    circleButton(kCamSmall, "S", w / 2 - 80, h - 150, 26, lit(kCamSmall), false);
    circleButton(kCamMedium, "M", w / 2, h - 150, 26, lit(kCamMedium), false);
    circleButton(kCamLarge, "L", w / 2 + 80, h - 150, 26, lit(kCamLarge), false);
    // The shutter: a big white ring.
    {
        const ImVec2 c{r.x + w / 2, r.y + h - 62};
        ImGui::SetCursorScreenPos({c.x - 56, c.y - 56});
        ImGui::PushID(9399);
        const bool pressed = ImGui::InvisibleButton("shutter", {112, 112});
        const bool down = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::PopID();
        d->AddCircle(c, 54, kWhite, 40, 6);
        d->AddCircleFilled(c, down ? 40.0f : 44.0f, kWhite, 40);
        if (pressed) {
            action = kBuildTool;
            if (index) *index = kCamShutter;
        }
    }
    return action;
}

}  // namespace

std::atomic<bool> g_cameraTrayWanted{false};
void setCameraTrayWanted(bool on) { g_cameraTrayWanted.store(on); }

// The tray closed (the tabs alone): their part of kBuildTray, 0..1 from its left. The camera app's is always open.
float buildTrayLeft() {
    if (g_cameraTrayWanted.load()) return 0;
    return g_trayCategory < 0 ? (kBuildTray.w - 14 - 136 - 14) / kBuildTray.w : 0;
}
