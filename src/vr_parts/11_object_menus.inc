// ---------------------------------------------------------------------------
// Menus on objects (first person, [ui] object_menus): a game picker (Cook...
// on a fridge, a TV's channel or movie picker) shows on the object it is for,
// not on the wrist or the phone. objmenu.py says where the object's front face
// is in the world; the picker is found in the UI as for the phone (fitMenu)
// and that part of the UI goes on the face: a door or front (kind 1) gets it
// near its top, at least [ui] object_menu_width wide, like a smart fridge; a
// screen (kind 2) gets it fitted inside, like the TV showing it. Point the
// laser at it or poke it, as with the pie menu.
struct ObjectMenuRequest {
    int kind = 0;
    float v[11] = {};  // centre xyz, normal xyz, up xyz, width, height (world units)
};
static std::mutex g_objMenuMutex;
static ObjectMenuRequest g_objMenuRequest;
static std::atomic<int> g_objMenuSerial{0};
static std::atomic<bool> g_objMenuWanted{false};  // a picker is on an object (no popup buzz for it)
// A game popup goes into the world as a card (kinds 4 in front of you, 5
// beside the Sim it is about; objmenu.py): the wrist gives way to it at once.
static std::atomic<bool> g_popupCardWanted{false};
static PieMenu g_objMenu;

void setObjectMenu(int kind, const float v[11]) {
    {
        std::lock_guard<std::mutex> lock(g_objMenuMutex);
        g_objMenuRequest.kind = kind;
        std::memcpy(g_objMenuRequest.v, v, sizeof(g_objMenuRequest.v));
    }
    g_objMenuWanted = kind > 0 && kind < 4;
    g_popupCardWanted = kind >= 4;
    ++g_objMenuSerial;
    static int logged = 0;
    static int lastKind = 0;
    bool follow = kind == 5 && lastKind == 5;  // the Sim's head followed: no log line each time
    lastKind = kind;
    if (follow) return;
    if (logged++ < 40) {
        if (kind == 4)
            LOG("Popup: a card in front of you");
        else if (kind == 5)
            LOG("Popup: a card beside the Sim it is about, head at %.2f %.2f %.2f", v[0], v[1], v[2]);
        else if (kind)
            LOG("Object menu: picker on a %s at %.2f %.2f %.2f facing %.2f %.2f %.2f, face %.2f x %.2f",
                kind >= 2 ? "screen" : "front", v[0], v[1], v[2], v[3], v[4], v[5], v[9], v[10]);
        else
            LOG("Object menu: off");
    }
}

static void hidePopupHud();
static bool popupOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w, bool onTop);
static void hidePickerHud();
static void pickerOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w);

static void hideObjectMenu() {
    hidePopupHud();
    hidePickerHud();
    g_objMenu.active = false;
    g_objMenu.armed[0] = g_objMenu.armed[1] = false;
    g_objMenu.pressing = -1;
    phone3d::setPanel(phone3d::Panel{}, nullptr);
}

// World (game units) to app space (metres): world = anchor + heading * (xr * scale).
static XrVector3f worldToApp(const camera::View& view, const float* w, bool direction) {
    XrVector3f d{w[0], w[1], w[2]};
    if (!direction) d = {w[0] - view.anchor.x, w[1] - view.anchor.y, w[2] - view.anchor.z};
    XrQuaternionf back{-view.heading.x, -view.heading.y, -view.heading.z, view.heading.w};
    XrVector3f r = rotate3(back, d);
    float k = direction || !(view.scale > 0) ? 1.0f : 1.0f / view.scale;
    return {r.x * k, r.y * k, r.z * k};
}

// ---------------------------------------------------------------------------
// Your own pie menu (first person, user 2026-10-17 13:44Z): poke your stomach
// with the grip held (the trigger too or not), or point the laser (grip + A /
// X) at your other hand's palm and pull the trigger. It is the menu the game
// opens for a click on your own Sim: Python sends the PieMenu widget's own
// request, interactions.choices for your Sim with a Sim pick (pie.py
// open_self, the round trip in re/liveui/components/PieMenu.md), the game
// builds it, and the widget draws it at the mouse, which waits in the middle
// of the window meanwhile. The pie at hand then shows it in front of you.
struct SelfPie {
    bool pending = false;
    ULONGLONG asked = 0, until = 0, shownAt = 0;
};
static SelfPie g_selfPie;
static constexpr float kSelfPieU = 0.5f, kSelfPieV = 0.5f;  // where the mouse waits for it
static constexpr ULONGLONG kSelfPieWait = 2000;              // ms for the game to answer
static constexpr ULONGLONG kSelfPieHold = 300;               // ms the mouse stays after it showed (the widget places it then)

static void requestSelfPie(int hand, const char* how, const XrPosef& head) {
    ULONGLONG now = GetTickCount64();
    if (g_pie.active || (g_selfPie.pending && now < g_selfPie.until)) return;
    // In front of you, a little below your eyes, wherever you look up or down.
    XrVector3f f = forwardOf(head.orientation);
    f.y = 0;
    if (!(dot3(f, f) > 1e-4f)) return;
    f = normalize3(f);
    g_lastWorldClick.valid = true;
    g_lastWorldClick.u = kSelfPieU;
    g_lastWorldClick.v = kSelfPieV;
    g_lastWorldClick.point = {head.position.x + f.x * 0.45f, head.position.y - 0.12f, head.position.z + f.z * 0.45f};
    g_lastWorldClick.tick = now;
    g_clickHand.store(hand, std::memory_order_relaxed);
    g_clickHandTick.store(now, std::memory_order_relaxed);
    g_selfPie.pending = true;
    g_selfPie.asked = now;
    g_selfPie.until = now + kSelfPieWait;
    g_selfPie.shownAt = 0;
    g_pieDismissHold = true;  // the trigger held for it clicks nothing until it lets go
    grab::pushControl(61);
    buzz(hand, 0.5f, 0.05f);
    static int logged = 0;
    if (logged++ < 30) LOG("Own pie menu: asked for (%s hand, %s)", hand ? "right" : "left", how);
}

// Every frame (updateControls), before anything else takes the mouse: while
// the game is opening your own pie, the mouse waits where it will be drawn.
static void holdSelfPieMouse(pointer::Controls& c, bool firstPerson) {
    if (!g_selfPie.pending) return;
    ULONGLONG now = GetTickCount64();
    if (g_pie.active && !g_selfPie.shownAt) g_selfPie.shownAt = now;
    bool done = !firstPerson || now >= g_selfPie.until || (g_selfPie.shownAt && now - g_selfPie.shownAt >= kSelfPieHold);
    if (done) {
        static int logged = 0;
        if (logged++ < 30)
            LOG("Own pie menu: %s after %llu ms", g_selfPie.shownAt ? "shown" : "the game showed none",
                static_cast<unsigned long long>((g_selfPie.shownAt ? g_selfPie.shownAt : now) - g_selfPie.asked));
        g_selfPie.pending = false;
        return;
    }
    c.hasTarget = true;
    c.warp = true;
    c.u = kSelfPieU;
    c.v = kSelfPieV;
    c.leftButton = false;
}

// Poking your stomach: a hand with its grip held whose Sim index fingertip
// comes from away from your Sim's belly (in front, or off to its side) and
// reaches it. The spot is between spine 1 and spine 2 (lower chest), as drawn; its front
// is kBellySurface out from them.
static constexpr float kBellySurface = 0.14f;  // metres in front of the spine that touch the belly
static constexpr float kBellyBehind = 0.05f;   // ... and behind it (a finger through it)
static constexpr float kBellyAway = 0.22f;     // in front of the spine: far enough out to poke again
static constexpr float kBellyHalfWidth = 0.15f, kBellyBelow = 0.14f, kBellyAbove = 0.16f;

static void pokeStomach(const armik::Hand hands[2], int laserHand, bool firstPerson, const XrPosef& head, bool headOk) {
    static bool armed[2] = {false, false};
    camera::View view;
    float t[6][3];
    bool ok = firstPerson && headOk && g_config.selfPiePoke && !g_pie.active && !g_selfPie.pending &&
              camera::lastView(view) && camera::torso(t);
    if (!ok) {
        armed[0] = armed[1] = false;
        return;
    }
    XrVector3f j[6];
    for (int k = 0; k < 6; ++k) j[k] = worldToApp(view, t[k], false);
    XrVector3f up = sub3(j[3], j[0]), across = sub3(j[5], j[4]);  // pelvis to spine 2; left to right shoulder
    if (!(dot3(up, up) > 1e-4f)) return;
    up = normalize3(up);
    across = sub3(across, {up.x * dot3(across, up), up.y * dot3(across, up), up.z * dot3(across, up)});
    if (!(dot3(across, across) > 1e-4f)) return;
    across = normalize3(across);
    XrVector3f ahead = cross3(up, across);
    const XrVector3f look = forwardOf(head.orientation);
    if (dot3(ahead, look) < 0) ahead = {-ahead.x, -ahead.y, -ahead.z};  // you face the way your Sim does
    // Between spine 1 and spine 2: the lower chest (user 2026-10-17 15:08Z: higher, more like the chest).
    const XrVector3f belly{(j[2].x + j[3].x) * 0.5f, (j[2].y + j[3].y) * 0.5f, (j[2].z + j[3].z) * 0.5f};
    for (int i = 0; i < 2; ++i) {
        bool can = hands[i].valid && i != laserHand && !(g_phoneOut && i == g_phoneHand) && !grab::holding(i) &&
                   !armik::holding(i) && hands[i].grip > 0.6f;
        if (!can) {
            armed[i] = false;  // a grip closed on the belly isn't a poke
            continue;
        }
        XrVector3f tip = simFingertip(hands[i], i);
        {
            // The index fingertip as the hand is drawn (handshape.h), curled or not.
            handshape::Shape shape;
            camera::Vec3 t;
            if (handshape::get(i, hands[i], shape) && handshape::tip(shape, 1, t)) tip = {t.x, t.y, t.z};
        }
        XrVector3f r = sub3(tip, belly);
        float f = dot3(r, ahead), h = dot3(r, up), s = dot3(r, across);
        bool over = std::fabs(s) < kBellyHalfWidth && h > -kBellyBelow && h < kBellyAbove;
        if (!over || f > kBellyAway) {
            armed[i] = true;
            continue;
        }
        if (!armed[i] || f > kBellySurface || f < -kBellyBehind) continue;
        armed[i] = false;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Own pie menu: your %s finger poked your stomach (%.1f cm in front of the spine, %.1f cm up, %.1f cm "
                "across, torso %.0f cm)",
                i ? "right" : "left", f * 100, h * 100, s * 100, std::sqrt(dot3(sub3(j[3], j[0]), sub3(j[3], j[0]))) * 100);
        requestSelfPie(i, "stomach poke", head);
        return;
    }
}

// The laser on your other hand (first person): anywhere on that hand as your
// Sim's hand is drawn this frame, palm, back or fingers (handshape.h; user
// 2026-10-17 14:49Z: only a small square of the hand worked). False if it misses.
static constexpr float kHandPad = 0.01f;  // metres added around the hand, for forgiving aim
static bool laserOnPalm(const armik::Hand hands[2], int laserHand, const XrVector3f& o, const XrVector3f& d,
                        XrVector3f& hit) {
    if (laserHand < 0 || !g_config.selfPiePalm || g_pie.active) return false;
    const int other = 1 - laserHand;
    if (!hands[other].valid || (g_phoneOut && other == g_phoneHand) || grab::holding(other)) return false;
    handshape::Shape shape;
    if (!handshape::get(other, hands[other], shape)) return false;
    handshape::Hit h;
    if (!handshape::raycast(shape, {o.x, o.y, o.z}, {d.x, d.y, d.z}, 3.0f, h, kHandPad)) return false;
    hit = {h.point.x, h.point.y, h.point.z};
    return true;
}

// Every frame (updateControls). True while the picker is shown on its object.
static int32_t g_popupCrop[4] = {};  // the popup's box in the UI
static bool g_popupPlace = true;     // the card in front of you: put it in front of you again
static bool updatePopupCard(const ObjectMenuRequest& r, bool firstPerson, bool headOk, const XrPosef& head);

static bool updateObjectMenu(bool firstPerson, bool headOk, const XrPosef& head) {
    const float headY = head.position.y;
    ObjectMenuRequest r;
    int serial;
    {
        std::lock_guard<std::mutex> lock(g_objMenuMutex);
        r = g_objMenuRequest;
        serial = g_objMenuSerial.load();
    }
    static int seen = 0, fitsAtOpen = 0;
    static ULONGLONG since = 0, refitAt = 0;
    ULONGLONG now = GetTickCount64();
    bool newRequest = false;
    if (serial != seen) {
        seen = serial;
        // The same popup's card following its Sim's head isn't a new one.
        static int lastKind = 0;
        bool follow = r.kind == 5 && lastKind == 5;
        lastKind = r.kind;
        newRequest = !follow;
    }
    if (newRequest) {
        hideObjectMenu();
        g_popupPlace = true;
        if (r.kind < 0) {
            // You walked away from the object: Escape answers the game's
            // picker as its Cancel would, so it closes everywhere (the wrist
            // too), and the game hears "cancel".
            g_escapeFrames = 3;
            LOG("Object menu: walked away, Escape cancels the picker");
        }
        if (r.kind > 0) {
            // The picker is modal: a pie menu still on show here is stale (a
            // second click while picking opened one), and it would hide the
            // picker until it timed out.
            closePie("a picker opened on an object");
            // Measure the picker once it has opened (fitMenu, as for the phone).
            since = g_phoneMenuSince = now;
            g_phoneFitted = false;
            fitsAtOpen = g_menuFitDone;
            refitAt = now + 2500;
        }
    }
    bool want = r.kind > 0 && (r.kind >= 4 ? g_config.popups3d : g_config.objectMenus) && firstPerson && g_uiFrame &&
                !g_pie.active && !(g_phoneOut && (g_tablet || g_phoneOptions));  // the phone's apps: popups stay in front of you
    camera::View view;
    if (!want || !camera::lastView(view)) {
        hideObjectMenu();
        phone3d::setPanel(phone3d::Panel{}, nullptr);
        return false;
    }
    // A picker on a front (the fridge) is our own card, drawn from the game's
    // message as the game's recipe picker lays it out (hud.cpp drawPicker; user
    // 2026-10-16 09:57Z: the cut of the game's flat picker looked badly
    // cropped). The cut stays for TV and computer screens, and when the
    // card's texts are not found.
    const float card = r.kind == 1 ? hud::pickerHeight() : 0.0f;
    if (r.kind == 1 && !(card > 0) && hud::pickerPending() && now - since < 3000) return false;  // its texts are being found
    // Measured again every 2.5 s: the picker can change size (tabs, more options).
    if (!(card > 0) && now >= refitAt && g_phoneFitted && !g_phoneFitPending) {
        g_phoneFitted = false;
        refitAt = now + 2500;
    }
    bool fitted = g_menuFitDone != fitsAtOpen;
    if (!(card > 0) && !fitted && now - since < 1500) return false;  // not measured yet
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t crop[4] = {W / 5, H / 5, W * 3 / 5, H * 3 / 5};  // not found: the middle of the UI
    if (fitted && g_menuFitOk) std::memcpy(crop, g_phoneCrop, sizeof(crop));
    crop[2] = std::max(16, std::min(crop[2], W));
    crop[3] = std::max(16, std::min(crop[3], H));
    crop[0] = std::max(0, std::min(W - crop[2], crop[0]));
    crop[1] = std::max(0, std::min(H - crop[3], crop[1]));
    if (r.kind >= 4) {
        // A popup: only the dialog itself (fitMenu's margin holds the game's
        // dimming around it, which stays behind on the flat UI).
        if (fitted && g_menuFitOk) {
            int32_t m = H * 5 / 1000;
            if (crop[2] > 4 * m && crop[3] > 4 * m) {
                crop[0] += m;
                crop[1] += m;
                crop[2] -= 2 * m;
                crop[3] -= 2 * m;
            }
        }
        std::memcpy(g_popupCrop, crop, sizeof(crop));
        return updatePopupCard(r, firstPerson, headOk, head);
    }

    float scale = view.scale > 0 ? view.scale : 1.0f;
    XrVector3f centre = worldToApp(view, r.v, false);
    XrVector3f z = normalize3(worldToApp(view, r.v + 3, true)), up = normalize3(worldToApp(view, r.v + 6, true));
    XrVector3f x = cross3(up, z);
    if (!(dot3(x, x) > 1e-4f)) return false;
    x = normalize3(x);
    XrVector3f y = cross3(z, x);
    float faceW = r.v[9] / scale, faceH = r.v[10] / scale;
    float aspect = card > 0 ? hud::kPicker.w / card : static_cast<float>(crop[2]) / static_cast<float>(crop[3]);
    float w, h, lift = 0;
    float screenW = 0, screenH = 0;  // a TV: its screen, the menu inside it
    if (r.kind == 3) {
        // The TV's own video screen (the model's VideoSurface mesh): the menu
        // fills it, as large as it fits, where the TV plays its pictures.
        screenW = faceW;
        screenH = faceH;
        w = screenW;
        h = w / aspect;
        if (h > screenH) {
            h = screenH;
            w = h * aspect;
        }
    } else if (r.kind == 2) {
        // No screen found in the model: the model's front is the whole TV, frame and stand too: the screen
        // is taken as a 16:9 glass 90% of its width, up against its top (where
        // the screen is on a TV with a stand). The menu sits inside it with
        // dark glass around, like the phone's.
        screenW = faceW * 0.9f;
        screenH = screenW * 9 / 16;
        if (screenH > faceH * 0.85f) {
            screenH = faceH * 0.85f;
            screenW = screenH * 16 / 9;
        }
        lift = faceH / 2 - faceH * 0.05f - screenH / 2;
        w = screenW * 0.96f;
        h = w / aspect;
        if (h > screenH * 0.94f) {
            h = screenH * 0.94f;
            w = h * aspect;
        }
    } else {
        // On a door or front, its middle at your eye height, at least
        // [ui] object_menu_width wide. Rule (user): never wider or taller than
        // the object, and always inside its front.
        w = std::max(faceW * 0.63f, g_config.objectMenuWidth);
        h = w / aspect;
        const float margin = std::min(faceW, faceH) * 0.03f;
        float maxW = faceW - 2 * margin, maxH = faceH - 2 * margin;
        if (w > maxW) {
            w = maxW;
            h = w / aspect;
        }
        if (h > maxH) {
            h = maxH;
            w = h * aspect;
        }
        lift = headOk ? (headY - centre.y) / std::max(0.3f, y.y) : faceH / 2 - margin - h / 2;
        float room = std::max(0.0f, faceH / 2 - margin - h / 2);
        lift = std::max(-room, std::min(lift, room));
    }
    if (!(w > 0.05f && h > 0.05f)) {
        hideObjectMenu();
        phone3d::setPanel(phone3d::Panel{}, nullptr);
        return false;
    }
    const float out = 0.01f;  // just in front of the face
    XrVector3f at{centre.x + y.x * lift + z.x * out, centre.y + y.y * lift + z.y * out,
                  centre.z + y.z * lift + z.z * out};
    if (card > 0) {
        // The card: one of the HUD's sheets, in the world on the front (depth
        // tested, so your hands pass in front of it); poked or pointed at as the HUD is.
        if (g_objMenu.active) {
            g_objMenu.active = false;
            phone3d::setPanel(phone3d::Panel{}, nullptr);
        }
        pickerOnHud(at, x, y, w);
        return true;
    }
    hidePickerHud();
    if (!g_objMenu.active) {
        g_objMenu.opened = now;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Object menu: shown %.2f x %.2f m on the %s (UI %dx%d at %d,%d, %s)", w, h,
                r.kind >= 2 ? "screen" : "front", crop[2], crop[3], crop[0], crop[1],
                fitted && g_menuFitOk ? "measured" : "not found, the middle");
    }
    g_objMenu.active = true;
    g_objMenu.x = x;
    g_objMenu.y = y;
    g_objMenu.z = z;
    g_objMenu.pose = {fromAxes(x, y, z), at};
    g_objMenu.width = w;
    g_objMenu.height = h;
    g_objMenu.x0 = crop[0];
    g_objMenu.y0 = crop[1];
    g_objMenu.cw = crop[2];
    g_objMenu.ch = crop[3];
    g_objMenu.uiW = ui.Width;
    g_objMenu.uiH = ui.Height;
    // Drawn inside the game's 3D view (phone3d), so your hands and anything
    // nearer pass in front of it; the flat layer only if that can't be drawn.
    // Back to world units: world = anchor + heading * (app * scale).
    phone3d::Panel panel;
    panel.show = true;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, x), wy = rotate3(heading, y);
    const float wcs[3] = {view.anchor.x + wc.x, view.anchor.y + wc.y, view.anchor.z + wc.z};
    std::memcpy(panel.centre, wcs, sizeof(wcs));
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = (screenW > 0 ? screenW : w) * scale;
    panel.height = (screenH > 0 ? screenH : h) * scale;
    if (screenW > 0) {
        panel.menu[2] = w / screenW;
        panel.menu[3] = h / screenH;
    }
    panel.crop[0] = static_cast<float>(crop[0]) / W;
    panel.crop[1] = static_cast<float>(crop[1]) / H;
    panel.crop[2] = static_cast<float>(crop[0] + crop[2]) / W;
    panel.crop[3] = static_cast<float>(crop[1] + crop[3]) / H;
    phone3d::setPanel(panel, g_uiFrame);
    return true;
}

// A game popup as a card in the world (first person, [ui] popups_3d; user
// 2026-10-15): beside the Sim it is about (kind 5: objmenu.py sends that Sim's
// head; the Sim is within 5 m, on your floor, in your room), hidden by walls
// like a speech bubble; else in front of you (kind 4), drawn over the world
// like the pie so it never ends up inside a wall, staying where it is until
// you turn or walk well away from it. Only the dialog shows (g_popupCrop),
// not the game's blue dimming around it. Point or poke it like the pie.
static bool updatePopupCard(const ObjectMenuRequest& r, bool firstPerson, bool headOk, const XrPosef& head) {
    camera::View view;
    if (!firstPerson || !headOk || !g_uiFrame || !camera::lastView(view)) {
        hideObjectMenu();
        return false;
    }
    ULONGLONG now = GetTickCount64();
    float scale = view.scale > 0 ? view.scale : 1.0f;
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    const int32_t* crop = g_popupCrop;
    float aspect = static_cast<float>(crop[2]) / static_cast<float>(std::max(1, crop[3]));
    // Drawn from the game's own message in its dialog art (hud.cpp), once its
    // texts are found: that card's shape; else the cut of the game's dialog.
    const float drawn = hud::popupHeight();
    if (drawn > 0) aspect = hud::kPopup.w / drawn;
    const XrVector3f up{0, 1, 0};
    XrVector3f eye = head.position;
    XrVector3f look = rotate3(head.orientation, {0, 0, -1});
    XrVector3f flat = normalize3({look.x, 0, look.z});
    if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
    XrVector3f at{};
    float w = 0;
    bool onTop = false;
    if (r.kind == 5) {
        // Beside the Sim's head, to its right as you see it, like a speech
        // bubble; as wide as [ui] popup_size per metre away, so it reads the same.
        XrVector3f sim = worldToApp(view, r.v, false);
        XrVector3f to{sim.x - eye.x, 0, sim.z - eye.z};
        float away = std::sqrt(dot3(to, to));
        if (!(away > 0.05f)) to = flat, away = 0.05f;
        XrVector3f d = normalize3(to), right{-d.z, 0, d.x};
        w = std::max(0.25f, std::min(1.4f, g_config.popupSize * std::max(away, 0.5f)));
        float h = w / std::max(aspect, 0.2f);
        at = {sim.x + right.x * (0.20f + w / 2), sim.y + h / 2 - 0.10f, sim.z + right.z * (0.20f + w / 2)};
    } else {
        // In front of you, a little below your eyes, fixed in the world; put
        // in front of you again once you have turned or walked away from it.
        static float world[3] = {};
        static ULONGLONG outSince = 0;
        float distance = std::max(0.3f, g_config.popupDistance);
        if (!g_popupPlace) {
            XrVector3f was = worldToApp(view, world, false);
            XrVector3f to{was.x - eye.x, 0, was.z - eye.z};
            float gap = std::sqrt(dot3(to, to));
            bool out = gap > distance + 1.5f || gap < 0.2f ||
                       (gap > 0.01f && dot3(normalize3(to), flat) < std::cos(55 * 0.0174533f));
            if (!out)
                outSince = 0;
            else if (!outSince)
                outSince = now;
            else if (now - outSince > 800)
                g_popupPlace = true;
        }
        if (g_popupPlace) {
            g_popupPlace = false;
            outSince = 0;
            XrVector3f a{eye.x + flat.x * distance, eye.y - 0.08f, eye.z + flat.z * distance};
            XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
            XrVector3f wa = rotate3(heading, {a.x * scale, a.y * scale, a.z * scale});
            world[0] = view.anchor.x + wa.x;
            world[1] = view.anchor.y + wa.y;
            world[2] = view.anchor.z + wa.z;
        }
        at = worldToApp(view, world, false);
        XrVector3f to{at.x - eye.x, at.y - eye.y, at.z - eye.z};
        w = std::max(0.15f, g_config.popupSize * std::sqrt(dot3(to, to)));
        onTop = true;
    }
    float h = w / std::max(aspect, 0.2f);
    // Facing you, upright.
    XrVector3f z = normalize3({eye.x - at.x, eye.y - at.y, eye.z - at.z});
    XrVector3f y = normalize3({up.x - z.x * z.y, up.y - z.y * z.y, up.z - z.z * z.y});
    if (!(dot3(y, y) > 0.5f)) y = up;
    XrVector3f x = cross3(y, z);
    if (drawn > 0) {
        if (g_objMenu.active) hideObjectMenu();
        return popupOnHud(at, x, y, w, onTop);
    }
    hidePopupHud();
    if (!g_objMenu.active) {
        g_objMenu.opened = now;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Popup: shown %.2f x %.2f m %s (UI %dx%d at %d,%d)", w, h,
                r.kind == 5 ? "beside the Sim" : "in front of you", crop[2], crop[3], crop[0], crop[1]);
    }
    g_objMenu.active = true;
    g_objMenu.x = x;
    g_objMenu.y = y;
    g_objMenu.z = z;
    g_objMenu.pose = {fromAxes(x, y, z), at};
    g_objMenu.width = w;
    g_objMenu.height = h;
    g_objMenu.x0 = crop[0];
    g_objMenu.y0 = crop[1];
    g_objMenu.cw = crop[2];
    g_objMenu.ch = crop[3];
    g_objMenu.uiW = ui.Width;
    g_objMenu.uiH = ui.Height;
    phone3d::Panel panel;
    panel.show = true;
    panel.clear = true;
    panel.opacity = 1;
    panel.onTop = onTop;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, x), wy = rotate3(heading, y);
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = w * scale;
    panel.height = h * scale;
    if (r.kind == 4) {
        // In front of you (app space): placed with each frame's camera.
        panel.app = true;
        panel.appAt[0] = at.x, panel.appAt[1] = at.y, panel.appAt[2] = at.z;
        panel.appX[0] = x.x, panel.appX[1] = x.y, panel.appX[2] = x.z;
        panel.appY[0] = y.x, panel.appY[1] = y.y, panel.appY[2] = y.z;
        panel.appW = w;
        panel.appH = h;
    }
    float W = static_cast<float>(ui.Width), H = static_cast<float>(ui.Height);
    panel.crop[0] = crop[0] / W;
    panel.crop[1] = crop[1] / H;
    panel.crop[2] = (crop[0] + crop[2]) / W;
    panel.crop[3] = (crop[1] + crop[3]) / H;
    phone3d::setPanel(panel, g_uiFrame);
    return true;
}

