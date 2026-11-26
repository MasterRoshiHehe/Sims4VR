// The phone's Photos app (user 2026-10-31, thread "Continue camera mode development"): the photos the Sim took with
// the camera. The game keeps each as an object (crafting/photography.py create_photo_from_photo_data: a photo
// definition with a canvas component holding the picture, put in the photographer's inventory), shown in the
// inventory panel as a small thumbnail and big only when the frame is put down and looked at. Here: the inventory's
// photo objects as a grid, a tap opens one large with Previous / Next and Options (the game's pie menu for that
// object at your hand, as the Inventory app: Place, Display, Sell...). Data: phonesim.py "pho" records.
// Included by hud.cpp after phone_physics.inc.

bool g_photosOpen = false;  // the home screen's Photos app
int g_photoSel = -1;        // the photo open large (row of g_sim.photos), -1 the grid

void drawPhotosLogo(ImDrawList* d, ImVec2 c, float h, ImU32 tint) {
    const ImU32 white = (tint & 0xff000000u) | 0x00ffffffu;
    const ImU32 dark = (tint & 0xff000000u) | 0x00305a96u;
    const float u = h / 30.0f;
    d->AddRectFilled({c.x - 22 * u, c.y - 13 * u}, {c.x + 22 * u, c.y + 17 * u}, white, 5 * u);
    d->AddRectFilled({c.x - 12 * u, c.y - 19 * u}, {c.x - 2 * u, c.y - 12 * u}, white, 2 * u);
    d->AddCircleFilled({c.x, c.y + 2 * u}, 10.5f * u, dark, 32);
    d->AddCircleFilled({c.x, c.y + 2 * u}, 7 * u, white, 32);
    d->AddCircleFilled({c.x, c.y + 2 * u}, 4 * u, dark, 24);
    d->AddCircleFilled({c.x + 15 * u, c.y - 7 * u}, 2 * u, dark, 12);
}

template <class Button>
void drawPhotosApp(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action, int* index,
                   bool swipeY, float dy, const PhoneDrag& drag) {
    constexpr float S = 2.5f;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(28, 72, 150, 255), kGrey = IM_COL32(110, 112, 120, 255);
    const auto& photos = g_sim.photos;
    const int n = static_cast<int>(photos.size());
    if (g_photoSel >= n) g_photoSel = n - 1;
    // The data comes with phonesim's send (asked now and then while the app shows).
    static ULONGLONG asked = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - asked > 3000 && action == kNone) action = kMySimOpened, asked = now;

    d->AddRectFilled(at(0, 0), at(230, 356), IM_COL32(212, 212, 212, 246), 6 * S);
    pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
    {
        const char* head = "Photos";
        ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
        shadowText(d, g_title, 34, {at(115, 24).x - hs.x / 2, at(115, 24).y - hs.y / 2}, kWhite, head);
    }
    if (n == 0) {
        const char* lines[2] = {"No photos yet.", "Take one with the camera."};
        for (int k = 0; k < 2; ++k) {
            ImVec2 ts = g_text->CalcTextSizeA(26, FLT_MAX, 0, lines[k]);
            d->AddText(g_text, 26, {at(115, 0).x - ts.x / 2, at(0, 150 + 18 * k).y - ts.y / 2}, kGrey, lines[k]);
        }
        return;
    }
    if (g_photoSel >= 0) {  // one photo fullscreen on the phone, like a phone's own gallery
        const SimItem& it = photos[g_photoSel];
        static int askedFor = -1;  // the picture is asked for once, again after 3 s if it hasn't come
        static ULONGLONG askedAt = 0;
        if (!photoViewReady(it.picture) && action == kNone && (askedFor != g_photoSel || now - askedAt > 3000)) {
            action = kPhotoView;
            if (index) *index = it.order;
            askedFor = g_photoSel, askedAt = now;
        }
        d->AddRectFilled(at(0, 0), at(230, 356), IM_COL32(8, 8, 10, 255), 6 * S);
        if (photoViewReady(it.picture)) {
            bigPhoto(d, it.picture, at(0, 0), at(230, 356));
        } else {  // while the picture is read: the small one, scaled
            if (it.picture) {
                shown.push_back(it.picture);
                icon(d, it.picture, at(115, 178), 214 * S);
            }
            const char* wait = "Opening...";
            ImVec2 ws = g_text->CalcTextSizeA(24, FLT_MAX, 0, wait);
            d->AddText(g_text, 24, {at(115, 0).x - ws.x / 2, at(0, 178).y - ws.y / 2}, IM_COL32(235, 235, 240, 255), wait);
        }
        // Over the picture: the caption at the top, Previous / Options / Next at the bottom (the footer's Back leaves).
        std::string cap = it.name;
        if (!it.stamp.empty()) cap += cap.empty() ? it.stamp : "  " + it.stamp;
        if (!cap.empty()) {
            ImVec2 cs = g_text->CalcTextSizeA(22, FLT_MAX, 0, cap.c_str());
            d->AddRectFilled({at(115, 0).x - cs.x / 2 - 8 * S, at(0, 6).y}, {at(115, 0).x + cs.x / 2 + 8 * S, at(0, 6).y + cs.y + 6 * S},
                             IM_COL32(0, 0, 0, 120), 4 * S);
            d->AddText(g_text, 22, {at(115, 0).x - cs.x / 2, at(0, 6).y + 3 * S}, IM_COL32(240, 240, 245, 255), cap.c_str());
        }
        struct Btn { const char* label; float x0, x1; int what; };
        const Btn btns[3] = {{"Previous", 10, 74, 0}, {"Options", 78, 152, 1}, {"Next", 156, 220, 2}};
        for (const Btn& b : btns) {
            bool over = false;
            if (button(at(b.x0, 324), at(b.x1, 348), &over)) {
                if (b.what == 0) g_photoSel = (g_photoSel + n - 1) % n;
                else if (b.what == 2) g_photoSel = (g_photoSel + 1) % n;
                else {
                    action = kInventoryPie;
                    if (index) *index = photos[g_photoSel].order;
                }
            }
            d->AddRectFilled(at(b.x0, 324), at(b.x1, 348), over ? IM_COL32(120, 170, 235, 235) : IM_COL32(250, 250, 250, 170), 5 * S);
            ImVec2 ts = g_text->CalcTextSizeA(24, FLT_MAX, 0, b.label);
            d->AddText(g_text, 24, {at((b.x0 + b.x1) / 2, 0).x - ts.x / 2, at(0, 336).y - ts.y / 2}, kBlue, b.label);
        }
        return;
    }
    // The grid, two across, scrolled.
    const float top = 52, bottom = 350, cell = 104, gap = 6, x0 = 6;
    const float total = ((n + 1) / 2) * (cell + gap), room = bottom - top;
    if (swipeY) g_mySimScroll = drag.sim - dy;
    else g_mySimScroll += g_stickPx;
    g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
    d->PushClipRect(at(0, top), at(230, bottom), true);
    for (int j = 0; j < n; ++j) {
        const SimItem& it = photos[j];
        const float x = x0 + (j % 2) * (cell + gap), y = top + (j / 2) * (cell + gap) - g_mySimScroll;
        if (y + cell < top || y > bottom) continue;
        ImVec2 ca = at(x, y), cb = at(x + cell, y + cell);
        ImVec2 ha{ca.x, std::max(ca.y, at(0, top).y)}, hb{cb.x, std::min(cb.y, at(0, bottom).y)};
        bool over = false;
        if (hb.y - ha.y > 1 && button(ha, hb, &over)) g_photoSel = j;
        d->AddRectFilled(ca, cb, IM_COL32(246, 246, 246, 255), 4 * S);
        if (it.picture) {
            shown.push_back(it.picture);
            icon(d, it.picture, at(x + cell / 2, y + cell / 2), (cell - 8) * S);
        }
        if (over) d->AddRect(ca, cb, IM_COL32(120, 190, 80, 230), 4 * S, 0, 1.6f * S);
    }
    d->PopClipRect();
}
