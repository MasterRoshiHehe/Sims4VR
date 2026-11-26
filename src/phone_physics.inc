// The phone's Body Physics app (user 2026-10-28 13:50Z; scriptmod physapp.py): pick a Sim by touching or
// looking at her, freeze her motor functions, hold her in a pose, and open the Integrated Body Physics
// mod's pages (its ibp_menu table, merged into settings.cpp as "ibp:<page>") in the Phone Options list.
// Included in hud.cpp before drawWeather; the home-screen entry, the icon call and the pages' Back are
// hooked in drawPhone. Own-drawn logo (no atlas art): works on every phone theme.

bool g_bpOpen = false;      // the Body Physics app's hub
bool g_bpPoses = false;     // its pose list
bool g_bpOutfits = false;   // its outfit list
int g_bpGroup = -1;         // an option list (0 size, 1 gravity, 2 spacing), -1 none
bool g_bpHer = false;       // its "Her Settings" list (the presets, top and breasts)
bool g_bpFromHub = false;   // a physics page (Phone Options list) was opened from the hub: Back returns here
float g_bpScroll = 0;  // the pose / outfit list, scrolled by the pixel (phone swipe + right stick)

// The app's logo on the dark tile of the apps without art (hud.cpp appIcon): two soft round shapes with
// motion arcs beside them, in the phone's white-on-dark style.
void drawPhysicsLogo(ImDrawList* d, ImVec2 c, float h, ImU32 tint) {
    const ImU32 white = (tint & 0xff000000u) | 0x00ffffffu;
    const ImU32 soft = (tint & 0xff000000u) | 0x00bfd6ff;
    const float u = h / 30.0f;
    // two rounded masses
    d->AddCircleFilled({c.x - 7 * u, c.y - 4 * u}, 8.5f * u, white, 32);
    d->AddCircleFilled({c.x + 7 * u, c.y - 4 * u}, 8.5f * u, white, 32);
    // the dip between them and the lower pair's shadow
    d->AddCircleFilled({c.x - 6 * u, c.y + 10 * u}, 6.5f * u, soft, 28);
    d->AddCircleFilled({c.x + 6 * u, c.y + 10 * u}, 6.5f * u, soft, 28);
    // motion arcs on both sides
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 2; ++k) {
            const float rr = (21 + 4.5f * k) * u;
            d->PathArcTo({c.x, c.y + 2 * u}, rr, side < 0 ? 3.14159f - 0.55f : -0.55f, side < 0 ? 3.14159f + 0.55f : 0.55f, 14);
            d->PathStroke((tint & 0xff000000u) | 0x00ffffffu, 0, 1.6f * u * (k ? 0.8f : 1.0f));
        }
}

template <class Button>
void drawPhysicsApp(ImDrawList* d, const Rect& r, Button& button, Action& action, int* index, bool swipeY, float dy,
                    const PhoneDrag& drag) {
    constexpr float S = 2.5f;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(28, 72, 150, 255), kGreen = IM_COL32(60, 150, 40, 255), kGrey = IM_COL32(120, 128, 140, 255);
    const SimPhysics& bp = g_sim.physics;
    static ULONGLONG asked = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - asked > 1000 && action == kNone) action = kMySimOpened, asked = now;  // fresh state while open
    pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
    {
        static const char* const kHeads[3] = {"Size preset", "Gravity preset", "Spacing preset"};
        const char* head = g_bpPoses ? "Poses" : g_bpOutfits ? "Outfits" : g_bpGroup >= 0 ? kHeads[g_bpGroup] : g_bpHer ? "Her Settings" : "Body Physics";
        ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
        shadowText(d, g_title, 34, {at(115, 22).x - hs.x / 2, at(115, 22).y - hs.y / 2}, kWhite, head);
    }
    static bool listScrolls = false;  // the list scrolled last frame: cards leave room for its slider
    const float cardRight = listScrolls ? 206.0f : 222.0f;
    float clipTop = 0, clipBottom = 400;  // game pixels: a card half out of the scrolled list takes presses only where it shows
    auto card = [&](float top, float rowH, const char* label, bool on, bool enabled, const char* hint, bool chevron) {
        const ImVec2 ba = at(8, top), bb = at(cardRight, top + rowH);
        bool over = false, held = false;
        if (rowH < 34) hint = nullptr;
        const bool clicked = enabled && button({ba.x, std::max(ba.y, at(0, clipTop).y)}, {bb.x, std::min(bb.y, at(0, clipBottom).y)}, &over, &held);
        const ImU32 fill = !enabled ? IM_COL32(215, 220, 228, 200) : held ? IM_COL32(205, 222, 246, 245)
                         : on ? IM_COL32(214, 240, 206, 245) : over ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, 225);
        d->AddRectFilled(ba, bb, fill, 8 * S);
        if (over || held || on) d->AddRect(ba, bb, on ? kGreen : IM_COL32(66, 140, 214, 255), 8 * S, 0, 1.2f * S);
        const float size = 24 * g_phoneText;
        const std::string text = label;
        ImVec2 ls = g_title->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        const float fit = std::min(1.0f, (chevron ? 176.0f : 196.0f) * S / std::max(1.0f, ls.x));
        const float ty = hint && *hint ? ba.y + 3 * S : (ba.y + bb.y - ls.y * fit) / 2;
        d->AddText(g_title, size * fit, {at(16, 0).x, ty}, enabled ? kBlue : kGrey, text.c_str());
        if (hint && *hint) {
            const float hs = 17 * g_phoneText;
            ImVec2 hz = g_text->CalcTextSizeA(hs, FLT_MAX, 0, hint);
            const float hk = std::min(1.0f, 196.0f * S / std::max(1.0f, hz.x));
            d->AddText(g_text, hs * hk, {at(16, 0).x, bb.y - hz.y * hk - 2.5f * S}, kGrey, hint);
        }
        if (chevron) {
            const ImVec2 c = at(cardRight - 10, top + rowH / 2);
            d->AddTriangleFilled({c.x - 3 * S, c.y - 6 * S}, {c.x - 3 * S, c.y + 6 * S}, {c.x + 4 * S, c.y}, kBlue);
        }
        return clicked;
    };
    auto control = [&](int n, const char* log) {
        action = kPhysics, *index = n;
        LOG("Phone: Body Physics %s", log);
    };
    // One scrolling list for the hub and its sub-lists: poses, outfits, and the pie menu's option groups
    // (size preset, gravity, spacing). A press either sends a control or opens a list.
    struct Item {
        std::string label;
        bool on = false, chevron = false, enabled = true;
        int control = -1;  // sent to physapp.control
        int nav = 0;       // 1 poses, 2 outfits, 3 + g option group g, 8 her settings pages, 9 all settings pages, 10 Her Settings list
    };
    static const char* const kGroupNames[3] = {"Size preset", "Gravity preset", "Spacing preset"};
    const bool hub = !g_bpPoses && !g_bpOutfits && g_bpGroup < 0 && !g_bpHer;
    const bool mod = bp.sent && bp.mod;
    std::vector<Item> items;
    auto listItems = [&](const std::vector<PhysicsPose>& list, int base) {
        for (const PhysicsPose& p : list) {
            Item it;
            it.label = p.name, it.on = p.held, it.chevron = p.source == 1, it.control = base + p.index;
            items.push_back(it);
        }
    };
    if (g_bpPoses) listItems(bp.poses, 100);
    else if (g_bpOutfits) listItems(bp.outfits, 200);
    else if (g_bpGroup >= 0) listItems(bp.groups[g_bpGroup], 300 + g_bpGroup * 30);
    else if (g_bpHer) {
        auto add = [&](const std::string& label, bool chevron, int control, int nav) {
            Item it;
            it.label = label, it.chevron = chevron, it.control = control, it.nav = nav;
            items.push_back(it);
        };
        for (int g = 0; g < 3; ++g) add(kGroupNames[g], true, -1, 3 + g);
        if (!bp.topLabel.empty()) add(bp.topLabel, false, 14, 0);
        if (!bp.enhLabel.empty()) add(bp.enhLabel, false, 15, 0);
        add("More of her settings", true, -1, 8);
    } else {
        auto add = [&](const std::string& label, bool on, bool chevron, bool enabled, int control, int nav) {
            Item it;
            it.label = label, it.on = on, it.chevron = chevron, it.enabled = enabled, it.control = control, it.nav = nav;
            items.push_back(it);
        };
        add(bp.armed ? "Cancel picking" : "Pick a Sim", bp.armed, false, mod, 0, 0);
        add("Pick the Sim I look at", false, false, mod, 2, 0);
        if (bp.frozen) {
            add("Let her go", false, false, true, 1, 0);
            if (bp.wwFree) add("Freeze her again", false, false, true, 13, 0);
            add("Poses", false, true, true, -1, 1);
            add("Outfit", false, true, true, -1, 2);
        }
        add("Global settings", false, true, mod, -1, 9);
        if (bp.frozen) add("Her Settings", false, true, true, -1, 10);
    }
    const float top = hub ? 90.0f : 52.0f, rowH = 34, bottom = 352;
    const int total = static_cast<int>(items.size());
    const float stride = rowH + 4, maxSc = std::max(0.0f, total * stride - (bottom - top));
    if (hub) {
        const std::string who = !bp.sent ? "..." : !bp.mod ? "Integrated Body Physics is not loaded"
                              : bp.frozen ? bp.sim + " is frozen" : bp.armed ? "Touch her or look at her..." : "No Sim picked";
        const float size = 24 * g_phoneText;
        ImVec2 ws = g_title->CalcTextSizeA(size, FLT_MAX, 0, who.c_str());
        const float fit = std::min(1.0f, 210.0f * S / std::max(1.0f, ws.x));
        shadowText(d, g_title, size * fit, {at(115, 62).x - ws.x * fit / 2, at(115, 62).y - ws.y * fit / 2}, kWhite, who.c_str());
        if (!bp.status.empty()) {
            const float s2 = 18 * g_phoneText;
            ImVec2 ss = g_text->CalcTextSizeA(s2, FLT_MAX, 0, bp.status.c_str());
            const float f2 = std::min(1.0f, 210.0f * S / std::max(1.0f, ss.x));
            d->AddText(g_text, s2 * f2, {at(115, 0).x - ss.x * f2 / 2, at(0, 78).y}, IM_COL32(255, 220, 120, 255), bp.status.c_str());
        }
    }
    if (swipeY) g_bpScroll = drag.bp - dy;
    else g_bpScroll += g_stickPx;
    g_bpScroll = std::max(0.0f, std::min(maxSc, g_bpScroll));
    if (total == 0) {
        const char* none = bp.frozen ? "Nothing to show" : "Pick a Sim first";
        ImVec2 ns = g_text->CalcTextSizeA(24 * g_phoneText, FLT_MAX, 0, none);
        d->AddText(g_text, 24 * g_phoneText, {at(115, 0).x - ns.x / 2, at(0, 90).y}, kWhite, none);
    }
    clipTop = top, clipBottom = bottom;
    d->PushClipRect(at(0, top), at(230, bottom), true);
    for (int i = static_cast<int>(g_bpScroll / stride); i < total; ++i) {
        const float y = top + i * stride - g_bpScroll;
        if (y >= bottom) break;
        const Item& it = items[i];
        if (!card(y, rowH, it.label.c_str(), it.on, it.enabled, nullptr, it.chevron)) continue;
        if (it.control >= 0) {
            control(it.control, it.label.c_str());
        } else if (it.nav == 1) {
            g_bpPoses = true, g_bpScroll = 0;
        } else if (it.nav == 2) {
            g_bpOutfits = true, g_bpScroll = 0;
        } else if (it.nav == 10) {
            g_bpHer = true, g_bpScroll = 0;
        } else if (it.nav >= 3 && it.nav <= 5) {
            g_bpGroup = it.nav - 3, g_bpScroll = 0;
        } else if (it.nav == 8 || it.nav == 9) {
            g_phoneOptsGroup = it.nav == 8 ? L"ibp:sim" : L"ibp:main", g_phoneOptsScroll = 0, g_bpOpen = false, g_bpFromHub = true;
        }
    }
    d->PopClipRect();
    clipTop = 0, clipBottom = 400;
    listScrolls = maxSc > 0;
    if (maxSc > 0) {
        // The slider on the right, as the other phone lists (Phone Options): track, thumb, dragged by sliderFollow.
        const float trackH = bottom - top, thumbH = std::max(24.0f, trackH * trackH / (total * stride));
        const float thumbY = top + (trackH - thumbH) * g_bpScroll / maxSc;
        d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
        pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
        const float t = sliderFollow(r, top, bottom, thumbH);
        if (t >= 0) g_bpScroll = t * maxSc;
    }
    bool over = false;
    if (button(at(kBackX, 361), at(kBackX + 33, 392), &over)) {
        if (hub) g_bpOpen = false;
        else if (g_bpGroup >= 0) g_bpGroup = -1, g_bpScroll = 0;
        else g_bpPoses = g_bpOutfits = g_bpHer = false, g_bpScroll = 0;
    }
    piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(kBackX + 16.5f, 361 + 15.5f), 2.5f / art::kScale);
}
