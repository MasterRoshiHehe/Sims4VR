// The build tablet's tool tray (user 2026-10-24 01:51Z, categories 02:00Z): build mode's keyboard-only hotkeys as
// buttons, right middle of the tablet (docs/BUILD-TRAY.md). Category tabs down the left, that category's buttons
// in two columns beside them, in the game's dialog art. Toggles light up (Frame.trayLit, bit per TrayTool);
// rotate / raise / scale repeat while held. Included by hud.cpp.

namespace {

struct TrayButton {
    int tool;
    const char* label;
    bool repeat;
};
struct TrayCategory {
    const char* name;
    TrayButton buttons[12];
    int count;
};
const TrayCategory kTrayCategories[] = {
    {"Objects",
     {{kTrayAlt, "Free place", false},
      {kTrayShift, "Place copies", false},
      {kTraySnap, "Snap size", false},
      {kTrayGrid, "Grid", false},
      {kTrayRotateLeft, "Turn left", true},
      {kTrayRotateRight, "Turn right", true},
      {kTrayRaise, "Raise", true},
      {kTrayLower, "Lower", true},
      {kTrayBigger, "Bigger", true},
      {kTraySmaller, "Smaller", true},
      {kTraySwatch, "Next swatch", false},
      {kTraySlot, "Next slot", false}},
     12},
    {"Walls & floors",
     {{kTrayShift, "Fill room", false},
      {kTrayAlt, "One wall", false},
      {kTrayQuarter, "Quarter tiles", false},
      {kTrayGrid, "Grid", false}},
     4},
    {"Roofs",
     {{kTrayShift, "One side eaves", false}, {kTrayAlt, "Free height", false}, {kTrayRoofCurve, "Curved roof", false}},
     3},
    {"Terrain", {{kTrayEraser, "Eraser", false}}, 1},
    {"Cheats",
     {{kTrayMoveObjects, "Move objects", false},
      {kTrayHidden, "Hidden objects", false},
      {kTrayLiveEdit, "Live-edit objects", false}},
     3},
};
constexpr int kTrayCategoryCount = sizeof(kTrayCategories) / sizeof(kTrayCategories[0]);
int g_trayCategory = 0;
int g_trayHeld = -1;            // the repeating button held down
ULONGLONG g_trayRepeatAt = 0;   // its next repeat

// One button: the game's dialog button, green-tinted while lit; true on the frames it acts.
bool trayButton(ImDrawList* d, int id, const char* label, ImVec2 a, ImVec2 b, bool lit, bool repeat) {
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
    const float size = g_text->FontSize * 0.72f, wrap = b.x - a.x - 24;
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, label);
    d->AddText(g_text, size, {(a.x + b.x - ts.x) / 2, a.y + (b.y - a.y - 8 - ts.y) / 2}, kButtonBlue, label, nullptr, wrap);
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
    uikit::dialogPanel(d, {r.x, r.y}, {r.x + r.w, r.y + r.h});
    const float pad = 14, tabW = 136;
    Action action = kNone;
    // The category tabs, top to bottom, and under them leaving first person (user 2026-10-24 02:02Z).
    const float tabH = (r.h - 2 * pad) / (kTrayCategoryCount + 1);
    for (int k = 0; k < kTrayCategoryCount; ++k) {
        ImVec2 a{r.x + pad, r.y + pad + k * tabH}, b{r.x + pad + tabW, r.y + pad + (k + 1) * tabH - 6};
        if (trayButton(d, 100 + k, kTrayCategories[k].name, a, b, k == g_trayCategory, false)) g_trayCategory = k;
    }
    {
        const int k = kTrayCategoryCount;
        ImVec2 a{r.x + pad, r.y + pad + k * tabH}, b{r.x + pad + tabW, r.y + pad + (k + 1) * tabH - 6};
        if (trayButton(d, 120, "Exit first person", a, b, false, false)) {
            action = kBuildTool;
            if (index) *index = kTrayExitFirstPerson;
        }
    }
    // Its buttons, two columns, rows as tall as twelve would allow.
    const TrayCategory& cat = kTrayCategories[g_trayCategory];
    const float x0 = r.x + pad + tabW + 10, colW = (r.x + r.w - pad - x0 - 8) / 2, rowH = (r.h - 2 * pad) / 6;
    for (int i = 0; i < cat.count; ++i) {
        const TrayButton& t = cat.buttons[i];
        ImVec2 a{x0 + (i % 2) * (colW + 8), r.y + pad + (i / 2) * rowH};
        ImVec2 b{a.x + colW, a.y + rowH - 6};
        if (trayButton(d, g_trayCategory * 16 + i, t.label, a, b, (f.trayLit >> t.tool) & 1, t.repeat)) {
            action = kBuildTool;
            if (index) *index = t.tool;
        }
    }
    return action;
}

}  // namespace
