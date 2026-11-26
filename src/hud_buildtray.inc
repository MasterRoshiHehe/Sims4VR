// The build tablet's tool tray (user 2026-10-24 01:51Z, categories 02:00Z): build mode's keyboard-only hotkeys as
// buttons, right middle of the tablet (docs/BUILD-TRAY.md). Category tabs down the left, that category's buttons
// in two columns beside them, in the game's dialog art. Toggles light up (Frame.trayLit, bit per TrayTool);
// rotate / raise / scale repeat while held. Included by hud.cpp.

std::atomic<float> g_cameraAspect{0.5f};

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
    // The camera app like a phone's: the picture fills the glass, everything floats over it, see-through (user
    // 2026-11-26). Laid out on the glass' own shape: the crop of kBuildTray that placeTray puts across the glass.
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect full = kBuildTray;
    const float aspect = g_cameraAspect.load();
    float w = full.w, h = full.h;
    if (aspect <= full.w / full.h) w = h * aspect;
    else h = w / aspect;
    const ImVec2 o{full.x + (full.w - w) / 2, full.y + (full.h - h) / 2};
    Action action = kNone;
    auto press = [&](int tool) {
        action = kBuildTool;
        if (index) *index = tool;
    };
    auto label = [&](ImVec2 c, const char* text, float size, ImU32 colour) {
        const ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, 0, text);
        d->AddText(g_text, size, {c.x - ts.x / 2, c.y - ts.y / 2}, colour, text);
    };
    // A see-through round button at (cx, cy) in glass pixels.
    auto circle = [&](int tool, const char* text, float cx, float cy, float radius, bool on) {
        const ImVec2 c{o.x + cx, o.y + cy};
        ImGui::SetCursorScreenPos({c.x - radius, c.y - radius});
        ImGui::PushID(9300 + tool);
        const bool pressed = ImGui::InvisibleButton("cam", {radius * 2, radius * 2});
        const bool hover = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::PopID();
        d->AddCircleFilled(c, radius, hover ? IM_COL32(255, 255, 255, 120) : IM_COL32(0, 0, 0, 110), 32);
        d->AddCircle(c, radius, on ? IM_COL32(255, 214, 60, 255) : IM_COL32(255, 255, 255, 220), 32, 2.5f);
        label(c, text, std::min(g_text->FontSize * 0.62f, radius * 0.8f), on ? IM_COL32(255, 214, 60, 255) : kWhite);
        if (pressed) press(tool);
    };
    // Top: back (leaves the camera) left, home right.
    circle(kCamClose, "<", 48, 48, 30, false);
    circle(kCamHome, "Home", w - 48, 48, 30, false);
    // Zoom chips just above the footer; the front camera has none.
    const float stripY = h - 84;
    if (!f.camSelfie) {
        static const char* kChips[3] = {"1x", "2x", "3x"};
        for (int k = 0; k < 3; ++k) circle(kCamZoom1 + k, kChips[k], w / 2 + (k - 1) * 70.0f, stripY - 124, 24, f.camZoom == k);
    }
    // Footer: last photo bottom left, flip bottom right.
    {
        const ImVec2 a{o.x + 30, o.y + stripY - 28}, b{o.x + 86, o.y + stripY + 28};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(9390);
        if (ImGui::InvisibleButton("last", {b.x - a.x, b.y - a.y})) press(kCamLastPhoto);
        const bool hover = ImGui::IsItemHovered();
        ImGui::PopID();
        d->AddRectFilled(a, b, hover ? IM_COL32(255, 255, 255, 120) : IM_COL32(0, 0, 0, 110), 10);
        d->AddRect(a, b, kWhite, 10, 0, 2.5f);
        label({(a.x + b.x) / 2, (a.y + b.y) / 2}, "Last", g_text->FontSize * 0.5f, kWhite);
    }
    circle(kCamFlip, "Flip", w - 58, stripY, 30, f.camSelfie);
    // The strip: the chosen filter is the shutter; the others sit either side and slide as you swipe (left = next),
    // like Snapchat. Tap a neighbour to pick it, tap the shutter to take the photo.
    static const char* kNames[kCamFilters] = {"Normal", "Gray", "Sepia", "Vivid", "Muted", "Vignette",
                                              "Glow",   "Invert", "Warm", "Cool", "Bright", "Dark"};
    {
        const float cx = w / 2, spacing = 96;
        const ImVec2 a{o.x + 100, o.y + stripY - 62}, b{o.x + w - 100, o.y + stripY + 62};
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(9399);
        ImGui::InvisibleButton("strip", {b.x - a.x, b.y - a.y});
        static float startX = 0, stepped = 0;
        static bool dragged = false;
        const bool active = ImGui::IsItemActive();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated()) startX = mouse.x, stepped = 0, dragged = false;
        if (active) {
            const float dx = mouse.x - startX - stepped;
            if (std::fabs(dx) > 8) dragged = true;
            if (std::fabs(dx) > spacing * 0.6f) {
                const int step = dx < 0 ? 1 : -1;
                const int next = f.camFilter + step;
                if (next >= 0 && next < kCamFilters) press(kCamFilter0 + next);
                stepped += dx < 0 ? -spacing * 0.6f : spacing * 0.6f;
                dragged = true;
            }
        }
        if (ImGui::IsItemDeactivated() && !dragged) {
            const float rel = (mouse.x - (o.x + cx)) / spacing;
            const int k = static_cast<int>(std::lround(rel));
            if (k == 0) press(kCamShutter);
            else if (f.camFilter + k >= 0 && f.camFilter + k < kCamFilters) press(kCamFilter0 + f.camFilter + k);
        }
        ImGui::PopID();
        for (int k = -2; k <= 2; ++k) {
            const int fi = f.camFilter + k;
            if (fi < 0 || fi >= kCamFilters) continue;
            const ImVec2 c{o.x + cx + k * spacing, o.y + stripY};
            if (k == 0) {
                const bool down = active && !dragged;
                d->AddCircle(c, 54, kWhite, 40, 6);
                d->AddCircleFilled(c, down ? 40.0f : 44.0f, kWhite, 40);
            } else {
                const float r = k == 1 || k == -1 ? 28.0f : 20.0f;
                d->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 110), 32);
                d->AddCircle(c, r, IM_COL32(255, 255, 255, 200), 32, 2.5f);
                if (r > 25) label(c, std::string(kNames[fi]).substr(0, 3).c_str(), g_text->FontSize * 0.5f, kWhite);
            }
        }
        label({o.x + cx, o.y + stripY - 66}, kNames[std::clamp(f.camFilter, 0, kCamFilters - 1)], g_text->FontSize * 0.6f, kWhite);
    }
    return action;
}

}  // namespace

std::atomic<bool> g_cameraTrayWanted{false};
void setCameraTrayWanted(bool on) { g_cameraTrayWanted.store(on); }
void setCameraAspect(float a) { g_cameraAspect.store(a); }

// The tray closed (the tabs alone): their part of kBuildTray, 0..1 from its left. The camera app's is always open.
float buildTrayLeft() {
    if (g_cameraTrayWanted.load()) return 0;
    return g_trayCategory < 0 ? (kBuildTray.w - 14 - 136 - 14) / kBuildTray.w : 0;
}
