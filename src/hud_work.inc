// The card shown over closed eyes while your Sim is at work or in another rabbit hole (work.py; user 2026-11-03
// 22:11Z, go 2026-11-04 15:17Z; docs/WORK-RABBIT-HOLE.md option A). Drawn by us in the game's dialog art (never a crop
// of the game's UI) into the outfit panel's atlas area (it never shows together with the outfit panel; the work card
// wins). A headset layer (vr_parts/12_hud.inc hudWorkLayer) so the closed eyelids don't dim it. Included by hud.cpp
// after hud_outfit.inc (it uses outfitButton).
//
// Content (work.py _send): records split by 0x1e, fields by 0x1f:
//   "work" mode (0 leaving, 1 away, 2 back: the offer to swap back, shown over open eyes) title shift clock speed flags
//          (1 leave early, 2 swap back, 4 camera button, 8 dismiss)
//   "sim"  index name flags (1 the Sim you are in now)
// Buttons: work.py control codes (controls.py 8700000 + code): 1..3 speed, 4 pause, 5 leave early, 6 game camera,
// 7 swap back, 8 dismiss, 100 + index a household Sim.

namespace {

struct WorkSim {
    int index = 0, flags = 0;
    std::string name;
};
struct WorkContent {
    bool valid = false;
    int mode = 1, speed = 1, flags = 0;
    std::string title, shift, clock;
    std::vector<WorkSim> sims;
};
constexpr int kWkSpeed1 = 1, kWkSpeed2 = 2, kWkSpeed3 = 3, kWkPause = 4, kWkLeave = 5, kWkCamera = 6, kWkSwapBack = 7, kWkDismiss = 8,
              kWkSim = 100;
constexpr int kWorkLeaveEarly = 1, kWorkSwapBack = 2, kWorkCamera = 4, kWorkDismiss = 8;
std::mutex g_workMutex;
WorkContent g_workIn;  // g_workMutex

}  // namespace

void setWorkContent(const char* data, size_t size) {
    WorkContent c;
    const std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        const size_t end = all.find('\x1e', at);
        const std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? all.size() + 1 : end + 1;
        if (f.empty()) continue;
        if (f[0] == "work" && f.size() >= 7) {
            c.valid = true;
            c.mode = std::atoi(f[1].c_str());
            c.title = f[2];
            c.shift = f[3];
            c.clock = f[4];
            c.speed = std::atoi(f[5].c_str());
            c.flags = std::atoi(f[6].c_str());
        } else if (f[0] == "sim" && f.size() >= 4 && c.sims.size() < 8) {
            WorkSim s;
            s.index = std::atoi(f[1].c_str());
            s.name = f[2];
            s.flags = std::atoi(f[3].c_str());
            c.sims.push_back(s);
        }
    }
    std::lock_guard<std::mutex> lock(g_workMutex);
    g_workIn = c;
}

bool workPending() {
    std::lock_guard<std::mutex> lock(g_workMutex);
    return g_workIn.valid;
}

namespace {

// A label in the middle of a rectangle (the dialog's blue ink).
void workText(ImDrawList* d, const std::string& text, ImVec2 a, ImVec2 b, float size, bool left) {
    const float wrap = b.x - a.x;
    const ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
    d->AddText(g_text, size, {left ? a.x : (a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2}, kButtonBlue, text.c_str(), nullptr, wrap);
}

}  // namespace

// Present thread (render): the card in the top of kOutfitArea; the action's code in *index.
Action drawWork(int* index) {
    WorkContent c;
    {
        std::lock_guard<std::mutex> lock(g_workMutex);
        c = g_workIn;
    }
    if (!c.valid) return kNone;
    const Rect r = kWorkArea;
    const float W = r.w, H = r.h;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    uikit::dialogPanel(d, {r.x + 4, r.y + 4}, {r.x + W - 4, r.y + H - 4});
    Action action = kNone;
    auto send = [&](int code) {
        action = kWorkButton;
        if (index) *index = code;
    };
    const float pad = 30, gap = 14;
    const float x0 = r.x + pad, x1 = r.x + W - pad;
    float y = r.y + pad;

    // Title and clock.
    workText(d, c.title, {x0, y}, {x1 - 230, y + 70}, 54, true);
    workText(d, c.clock, {x1 - 220, y}, {x1, y + 70}, 54, false);
    y += 76;
    workText(d, c.shift, {x0, y}, {x1, y + 46}, 38, true);
    y += 62;

    // Time: pause and the game's three speeds (vanilla speeds only).
    if (c.mode != 2) {
        static const char* const kLabels[4] = {"Pause", "1x", "2x", "3x"};
        static const int kCodes[4] = {kWkPause, kWkSpeed1, kWkSpeed2, kWkSpeed3};
        const float bh = 84, bw = (x1 - x0 - 3 * gap) / 4;
        for (int k = 0; k < 4; ++k) {
            const ImVec2 a{x0 + k * (bw + gap), y}, b{x0 + k * (bw + gap) + bw, y + bh};
            const bool lit = (k == 0 && c.speed == 0) || (k > 0 && c.speed == k);
            if (lit) d->AddRectFilled({a.x - 6, a.y - 6}, {b.x + 6, b.y + 6}, IM_COL32(120, 190, 80, 200), 18.0f);
            if (outfitButton(d, 200 + k, kLabels[k], a, b, true)) send(kCodes[k]);
        }
        y += bh + gap * 2;
    }

    // The household's other Sims: tap one to play as them.
    if (!c.sims.empty() && c.mode != 2) {
        workText(d, "Play as", {x0, y}, {x1, y + 40}, 34, true);
        y += 46;
        const int n = static_cast<int>(c.sims.size()), perRow = 3;
        const float bh = 72, bw = (x1 - x0 - (perRow - 1) * gap) / perRow;
        for (int k = 0; k < n; ++k) {
            const int row = k / perRow, col = k % perRow;
            const ImVec2 a{x0 + col * (bw + gap), y + row * (bh + gap)}, b{a.x + bw, a.y + bh};
            if (outfitButton(d, 210 + k, c.sims[k].name.c_str(), a, b, (c.sims[k].flags & 1) == 0))
                send(kWkSim + c.sims[k].index);
        }
        y += ((n + perRow - 1) / perRow) * (bh + gap) + gap;
    }

    // Along the bottom: leave work early, swap back, the game camera.
    {
        const float bh = 88;
        const float by1 = r.y + H - pad, by0 = by1 - bh;
        std::vector<std::pair<const char*, int>> buttons;
        if (c.flags & kWorkLeaveEarly) buttons.push_back({c.mode == 0 ? "Stay home" : "Leave early", kWkLeave});
        if (c.flags & kWorkSwapBack) buttons.push_back({"Swap back", kWkSwapBack});
        if (c.flags & kWorkDismiss) buttons.push_back({"Stay here", kWkDismiss});
        if (c.flags & kWorkCamera) buttons.push_back({"Game camera", kWkCamera});
        const int n = static_cast<int>(buttons.size());
        if (n > 0) {
            const float bw = (x1 - x0 - (n - 1) * gap) / n;
            for (int k = 0; k < n; ++k) {
                const ImVec2 a{x0 + k * (bw + gap), by0}, b{a.x + bw, by1};
                if (outfitButton(d, 230 + k, buttons[k].first, a, b, true)) send(buttons[k].second);
            }
        }
    }
    return action;
}
