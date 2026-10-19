// Our own HUD (first person, [wrist]; user 2026-10-15 05:34Z: drawn with a
// third-party UI library, not crops of the game's UI). hud.cpp draws it with
// Dear ImGui from the game's values; here each part of it is placed: the watch
// (clock, day, speed buttons, the full-UI button) lying on your Sim's left
// forearm, inside the wrist; the pause symbol 30 cm under where popups show
// while the game is paused; and while your Sim sleeps the watch low in your
// view over the closed eyelids (a headset layer: the eyelids are drawn into
// the eye images). Poke with your right index finger or point the laser.

enum { kHudWatch, kHudPause, kHudSleep, kHudBuild, kHudNeeds, kHudMoodlets, kHudQueue, kHudWants, kHudPopup, kHudTip, kHudMoney,
       kHudPicker, kHudNote, kHudCount };
struct HudSheet {
    PieMenu m;  // x0, y0, cw, ch: its part of the atlas; uiW, uiH: the atlas
    bool onTop = false;
};
static HudSheet g_hud[kHudCount];
static HudSheet g_phoneSheet;  // the phone's screen with the game's phone menu (hud::kPhone), while it shows
static bool g_fullUi = false;  // the full UI over your palm, shown with the watch's button
static std::atomic<bool> g_paused{false};
static hud::Frame g_hudInput;  // this frame's pointer on the atlas (a poke or the laser)
static Swapchain g_hudSwapchain;  // the asleep watch's layer

void setPaused(bool paused) {
    if (g_paused.exchange(paused) != paused) {
        static int logged = 0;
        if (logged++ < 40) LOG("Game clock %s", paused ? "paused" : "running");
    }
}

void setClock(int hour, int minute, int day, int speed) { hud::setClock(hour, minute, day, speed); }

static void hideHud(HudSheet& s) {
    s.m.active = false;
    s.m.armed[0] = s.m.armed[1] = false;
    s.m.pressing = -1;
}

// Puts a sheet at `centre` (app space), axes x, y (z = x × y, toward you),
// `width` metres wide, showing `area` of the atlas.
static void placeHud(HudSheet& s, const XrVector3f& centre, const XrVector3f& x, const XrVector3f& y, float width,
                     const hud::Rect& area) {
    PieMenu& m = s.m;
    if (!m.active) m.opened = GetTickCount64();
    m.active = true;
    m.x = x;
    m.y = y;
    m.z = cross3(x, y);
    m.pose = {fromAxes(m.x, m.y, m.z), centre};
    m.width = width;
    m.height = width * area.h / area.w;
    m.x0 = static_cast<int32_t>(area.x);
    m.y0 = static_cast<int32_t>(area.y);
    m.cw = static_cast<int32_t>(area.w);
    m.ch = static_cast<int32_t>(area.h);
    m.uiW = hud::kWidth;
    m.uiH = hud::kHeight;
}

// The popup card drawn from the game's message (hud::kPopup, as tall as it
// is drawn), `w` metres wide at `at`; pointed at and poked as the HUD is.
static void hidePopupHud() { hideHud(g_hud[kHudPopup]); }
static bool popupOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w, bool onTop) {
    HudSheet& s = g_hud[kHudPopup];
    const float used = hud::popupHeight();
    if (!(used > 0)) {
        hideHud(s);
        return false;
    }
    if (!s.m.active) {
        static int logged = 0;
        if (logged++ < 30) LOG("Popup: drawn from the game's message, %.2f x %.2f m", w, w * used / hud::kPopup.w);
    }
    s.onTop = onTop;
    placeHud(s, at, x, y, w, {hud::kPopup.x, hud::kPopup.y, hud::kPopup.w, used});
    return true;
}

// The picker card drawn from the game's message (hud::kPicker, as tall as it
// is drawn) on its object's front, `w` metres wide at `at`.
static void hidePickerHud() { hideHud(g_hud[kHudPicker]); }
static void pickerOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w) {
    HudSheet& s = g_hud[kHudPicker];
    const float used = hud::pickerHeight();
    if (!(used > 0)) {
        hideHud(s);
        return;
    }
    if (!s.m.active) {
        static int logged = 0;
        if (logged++ < 30) LOG("Object menu: the picker drawn from the game's message, %.2f x %.2f m on the front", w, w * used / hud::kPicker.w);
    }
    s.onTop = false;
    placeHud(s, at, x, y, w, {hud::kPicker.x, hud::kPicker.y, hud::kPicker.w, used});
}

// Facing your eyes from `at`, upright.
static void facingAxes(const XrVector3f& at, const XrVector3f& eye, XrVector3f& x, XrVector3f& y) {
    XrVector3f z = normalize3(sub3(eye, at));
    y = normalize3({-z.x * z.y, 1 - z.y * z.y, -z.z * z.y});
    if (!(dot3(y, y) > 0.5f)) y = {0, 0, -1};
    x = cross3(y, z);
}

static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b);

// Turns a placed sheet (x right, y up, facing x * y) by yaw (about its
// facing), pitch (about its width) and roll (about its height), degrees.
static void turnSheet(XrVector3f& x, XrVector3f& y, const float turn[3]) {
    if (!turn[0] && !turn[1] && !turn[2]) return;
    const XrVector3f z = cross3(x, y);
    auto about = [](const XrVector3f& axis, float degrees) {
        float h = degrees * 0.0174533f * 0.5f, s = std::sin(h);
        return XrQuaternionf{axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
    };
    XrQuaternionf q = about({0, 0, 1}, turn[0]);
    q = qmul(q, about({1, 0, 0}, turn[1]));
    q = qmul(q, about({0, 1, 0}, turn[2]));
    auto out = [&](const XrVector3f& v) {
        XrVector3f l = rotate3(q, v);
        return XrVector3f{x.x * l.x + y.x * l.y + z.x * l.z, x.y * l.x + y.y * l.y + z.y * l.z,
                          x.z * l.x + y.z * l.y + z.z * l.z};
    };
    XrVector3f nx = out({1, 0, 0}), ny = out({0, 1, 0});
    x = nx;
    y = ny;
}

// A sheet on one of your Sim's forearms, built on its real bones: the forearm
// line from its elbow (b__X_Forearm__) to its wrist (b__X_Hand__) joint as the
// game posed them this frame, and the palm side from the Sim's hand as armik
// posed it (v0.13.145 read the hand joint's turn: 130 deg off), so it sits on
// the arm you see wherever the IK put it. pos: x metres from the wrist toward the elbow, y
// sideways round the arm, z out from the bone line; `around` degrees round the
// arm from the palm side; then turned by `turn`. It reads left to right toward
// your hand (the watch, with your left arm across you), or with `toElbow`
// toward your elbow (the action queue, running first at the wrist; the right
// arm palm up across you). False while it faces away from you or the arm isn't
// read (nothing is shown off the arm).
// What each arm sheet was placed by (onArm), for placing it again as it is
// drawn (placeArmPieces), from the joints of that very frame.
struct ArmPlace {
    bool on = false;
    int side = 0;
    float pos[3]{}, turn[3]{}, around = 0;
    bool toElbow = false;
};
static std::mutex g_armPlaceMutex;
static ArmPlace g_armPlace[16];
// The sheets around you (the wants, the pause symbol): where they sit in app
// space, put into the world again with the camera of the very frame drawn
// (placeArmPieces), so they don't jiggle per eye while you walk (user 19:15Z).
struct AppPlace {
    bool on = false;
    XrVector3f at{}, x{}, y{};
};
static AppPlace g_appPlace[16];  // g_armPlaceMutex

// A sheet on one of your Sim's forearms, in the world: its centre and axes
// (x along its width, y up it) from the forearm's joints and the posed hand,
// with `metres` world units a metre. False without the arm.
static bool armSheetWorld(const ArmPlace& a, float metres, XrVector3f& centre, XrVector3f& x, XrVector3f& y) {
    float e[3], w[3];
    if (!camera::forearm(a.side, e, w)) return false;
    camera::Vec3 handWrist;
    camera::Quat grip;
    float handScale;
    if (!armik::handPose(a.side, handWrist, grip, handScale)) return false;
    XrVector3f wrist{w[0], w[1], w[2]};
    XrVector3f axis = sub3({e[0], e[1], e[2]}, wrist);
    if (!(dot3(axis, axis) > 1e-8f)) return false;
    axis = normalize3(axis);
    const float k = a.side == 0 ? 1.0f : -1.0f;
    XrVector3f palm = rotate3(XrQuaternionf{grip.x, grip.y, grip.z, grip.w}, {k, 0, 0});
    palm = sub3(palm, {axis.x * dot3(palm, axis), axis.y * dot3(palm, axis), axis.z * dot3(palm, axis)});
    if (!(dot3(palm, palm) > 1e-6f)) return false;
    palm = normalize3(palm);
    XrVector3f sideways = cross3(axis, palm);
    float r = a.around * 0.0174533f;
    XrVector3f n = normalize3({palm.x * std::cos(r) + sideways.x * std::sin(r),
                               palm.y * std::cos(r) + sideways.y * std::sin(r),
                               palm.z * std::cos(r) + sideways.z * std::sin(r)});
    XrVector3f round = cross3(axis, n);
    const float* p = a.pos;
    centre = {wrist.x + (axis.x * p[0] + round.x * p[1] + n.x * p[2]) * metres,
              wrist.y + (axis.y * p[0] + round.y * p[1] + n.y * p[2]) * metres,
              wrist.z + (axis.z * p[0] + round.z * p[1] + n.z * p[2]) * metres};
    x = a.toElbow ? axis : XrVector3f{-axis.x, -axis.y, -axis.z};
    y = normalize3(cross3(n, x));
    turnSheet(x, y, a.turn);
    return true;
}

// Render thread (phone3d::setPiecePlacer): the arm sheets placed again from
// the joints of the frame being drawn, so they stay on the arm in both eyes.
static void placeArmPieces(phone3d::Panel* pieces, int count) {
    camera::View view;
    if (!camera::lastView(view)) return;
    const float metres = view.scale > 0 ? view.scale : 1.0f;
    ArmPlace places[16];
    AppPlace apps[16];
    {
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        std::copy(g_armPlace, g_armPlace + 16, places);
        std::copy(g_appPlace, g_appPlace + 16, apps);
    }
    const XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    for (int i = 0; i < count; ++i) {
        phone3d::Panel& p = pieces[i];
        if (p.tag < 0 || p.tag >= 16) continue;
        XrVector3f c, x, y;
        if (apps[p.tag].on) {
            const AppPlace& a = apps[p.tag];
            c = rotate3(heading, {a.at.x * metres, a.at.y * metres, a.at.z * metres});
            c = {view.anchor.x + c.x, view.anchor.y + c.y, view.anchor.z + c.z};
            x = rotate3(heading, a.x);
            y = rotate3(heading, a.y);
        } else if (!places[p.tag].on || !armSheetWorld(places[p.tag], metres, c, x, y)) {
            continue;
        }
        p.centre[0] = c.x;
        p.centre[1] = c.y;
        p.centre[2] = c.z;
        p.x[0] = x.x;
        p.x[1] = x.y;
        p.x[2] = x.z;
        p.y[0] = y.x;
        p.y[1] = y.y;
        p.y[2] = y.z;
    }
}

static bool onArm(HudSheet& s, int side, const armik::Hand& controller, const camera::View& view,
                  const XrVector3f& eye, const float pos[3], const float turn[3], float around, float width,
                  const hud::Rect& area, bool toElbow = false) {
    float e[3], w[3];
    if (!camera::forearm(side, e, w)) return false;
    XrVector3f wrist = worldToApp(view, w, false);
    XrVector3f axis = sub3(worldToApp(view, e, false), wrist);
    if (!(dot3(axis, axis) > 1e-4f)) return false;
    axis = normalize3(axis);
    // The palm side: the Sim's hand as armik last posed it (its grip frame in
    // the world; the left palm faces its +X, the right one its -X), in app space.
    camera::Vec3 handWrist;
    camera::Quat handGrip;
    float handScale;
    if (!armik::handPose(side, handWrist, handGrip, handScale)) return false;
    const float k = side == 0 ? 1.0f : -1.0f;
    XrVector3f palmWorld = rotate3(XrQuaternionf{handGrip.x, handGrip.y, handGrip.z, handGrip.w}, {k, 0, 0});
    const float pw[3] = {palmWorld.x, palmWorld.y, palmWorld.z};
    XrVector3f palm = worldToApp(view, pw, true);
    palm = sub3(palm, {axis.x * dot3(palm, axis), axis.y * dot3(palm, axis), axis.z * dot3(palm, axis)});
    if (!(dot3(palm, palm) > 1e-4f)) return false;
    palm = normalize3(palm);
    XrVector3f sideways = cross3(axis, palm);
    float r = around * 0.0174533f;
    XrVector3f n = normalize3({palm.x * std::cos(r) + sideways.x * std::sin(r),
                               palm.y * std::cos(r) + sideways.y * std::sin(r),
                               palm.z * std::cos(r) + sideways.z * std::sin(r)});
    XrVector3f round = cross3(axis, n);  // y: sideways round the arm
    XrVector3f centre{wrist.x + axis.x * pos[0] + round.x * pos[1] + n.x * pos[2],
                      wrist.y + axis.y * pos[0] + round.y * pos[1] + n.y * pos[2],
                      wrist.z + axis.z * pos[0] + round.z * pos[1] + n.z * pos[2]};
    XrVector3f toEye = sub3(eye, centre);
    float distance = std::sqrt(dot3(toEye, toEye));
    if (!(distance > 0.05f)) return false;
    XrVector3f x = toElbow ? axis : XrVector3f{-axis.x, -axis.y, -axis.z};
    XrVector3f y = normalize3(cross3(n, x));
    turnSheet(x, y, turn);
    XrVector3f facingDir = cross3(x, y);
    float facing = dot3(facingDir, {toEye.x / distance, toEye.y / distance, toEye.z / distance});
    bool show = s.m.active ? facing > 0.2f && distance < 0.9f : facing > 0.4f && distance < 0.8f;
    if (!show) return false;
    static bool logged[2] = {};
    if (!logged[side] && controller.valid) {
        // Once: how far the Sim's arm sits from your controller, for the log.
        logged[side] = true;
        const camera::Quat& c = controller.orientation;
        XrVector3f grip = rotate3(XrQuaternionf{c.x, c.y, c.z, c.w}, {k, 0, 0});
        XrVector3f elbow = worldToApp(view, e, false);
        XrVector3f fore = sub3(elbow, wrist);
        XrVector3f off = sub3(wrist, {controller.position.x, controller.position.y, controller.position.z});
        LOG("HUD: %s forearm from the Sim's joints (%.1f cm long, wrist %.1f cm from the controller); its palm side "
            "%.0f deg from the controller's",
            side ? "right" : "left", std::sqrt(dot3(fore, fore)) * 100, std::sqrt(dot3(off, off)) * 100,
            std::acos(std::clamp(dot3(palm, normalize3(grip)), -1.0f, 1.0f)) * 57.2958f);
    }
    placeHud(s, centre, x, y, width * g_config.wristScale, area);
    {
        const int index = static_cast<int>(&s - g_hud);
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        if (index >= 0 && index < 16) {
            ArmPlace& a = g_armPlace[index];
            a.on = true;
            a.side = side;
            std::copy(pos, pos + 3, a.pos);
            std::copy(turn, turn + 3, a.turn);
            a.around = around;
            a.toElbow = toElbow;
        }
    }
    return true;
}

// A sheet around you: `pos` from `origin` along right / up / ahead, facing
// your eyes, then turned.
static void aroundYou(HudSheet& s, const XrVector3f& origin, const XrVector3f& ahead, const XrVector3f& eye,
                      const float pos[3], const float turn[3], float width, const hud::Rect& area) {
    XrVector3f right = normalize3(cross3(ahead, {0, 1, 0}));
    XrVector3f at{origin.x + right.x * pos[0] + ahead.x * pos[2], origin.y + pos[1] + ahead.y * pos[2],
                  origin.z + right.z * pos[0] + ahead.z * pos[2]};
    XrVector3f x, y;
    facingAxes(at, eye, x, y);
    turnSheet(x, y, turn);
    placeHud(s, at, x, y, width * g_config.wristScale, area);
}

// A sheet drawn in the game's 3D view (phone3d): back to world units.
static phone3d::Panel hudPanel(const HudSheet& s, const camera::View& view) {
    const PieMenu& m = s.m;
    float scale = view.scale > 0 ? view.scale : 1.0f;
    phone3d::Panel panel;
    panel.show = true;
    panel.clear = true;
    panel.opacity = 1;
    panel.onTop = s.onTop;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    const XrVector3f& at = m.pose.position;
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, m.x), wy = rotate3(heading, m.y);
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = m.width * scale;
    panel.height = m.height * scale;
    panel.crop[0] = static_cast<float>(m.x0) / hud::kWidth;
    panel.crop[1] = static_cast<float>(m.y0) / hud::kHeight;
    panel.crop[2] = static_cast<float>(m.x0 + m.cw) / hud::kWidth;
    panel.crop[3] = static_cast<float>(m.y0 + m.ch) / hud::kHeight;
    return panel;
}

// A notification about one Sim as a card over their head (notify.py; [phone]
// notes_overhead), or in front of you for your own Sim. Python follows the
// head and ends it; a press dismisses it here.
struct NoteOver {
    uint64_t id = 0;
    float at[3] = {};
    bool front = false;
};
static std::mutex g_noteMutex;
static NoteOver g_noteOver;               // g_noteMutex
static std::atomic<uint64_t> g_noteDismissed{0};

void setNoteOver(uint64_t id, const float at[3], bool front) {
    if (id && id == g_noteDismissed.load()) return;
    std::lock_guard<std::mutex> lock(g_noteMutex);
    if (id != g_noteOver.id) {
        static int logged = 0;
        if (logged++ < 40) {
            if (id)
                LOG("Notification %llu: a card %s", static_cast<unsigned long long>(id),
                    front ? "in front of you (your Sim)" : "over the Sim's head");
            else
                LOG("Notification card gone");
        }
    }
    g_noteOver.id = id;
    std::memcpy(g_noteOver.at, at, sizeof(g_noteOver.at));
    g_noteOver.front = front;
}

// Moving the card in front of you (user 2026-10-18 23:52Z): close your hand on
// it, or hold the laser's trigger on it a moment, and move it; let go and the
// cards in front of you show there from then on, relative to your head (settings.cpp
// saves it, [phone] note_place).
struct NoteDrag {
    int hand = -1;          // the hand moving it; -1 none
    bool laser = false;     // by that hand's laser (else by the hand itself)
    float distance = 0;     // laser: the held point's distance along it
    XrVector3f offset{};    // the held point from the card's centre
    XrVector3f held{};      // the held point now
};
static NoteDrag g_noteDrag;
static ULONGLONG g_noteDragEnded = 0;  // a press let go after a move doesn't dismiss or answer
// The card in front of you as last placed (render thread): centre, axes, size.
static bool g_noteCardFront = false;
static XrVector3f g_noteCardAt{}, g_noteCardX{}, g_noteCardY{};
static float g_noteCardW = 0, g_noteCardH = 0;

// Where on the front card (within `slack` metres of its rectangle) a point is, or false.
static bool onNoteCard(const XrVector3f& p, float depth, float slack) {
    if (!g_noteCardFront) return false;
    const XrVector3f z = cross3(g_noteCardX, g_noteCardY), rel = sub3(p, g_noteCardAt);
    return std::fabs(dot3(rel, z)) <= depth && std::fabs(dot3(rel, g_noteCardX)) <= g_noteCardW / 2 + slack &&
           std::fabs(dot3(rel, g_noteCardY)) <= g_noteCardH / 2 + slack;
}

static bool laserOnNoteCard(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit) {
    if (!g_noteCardFront) return false;
    const XrVector3f z = cross3(g_noteCardX, g_noteCardY);
    const float den = dot3(d, z);
    if (std::fabs(den) < 1e-4f) return false;
    const float t = dot3(sub3(g_noteCardAt, o), z) / den;
    if (!(t > 0.05f && t < 10)) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    return onNoteCard(hit, 0.01f, 0);
}

static void startNoteDrag(int hand, bool laser, const XrVector3f& held) {
    g_noteDrag.hand = hand;
    g_noteDrag.laser = laser;
    g_noteDrag.offset = sub3(held, g_noteCardAt);
    g_noteDrag.held = held;
    buzz(hand, 0.4f, 0.03f);
    LOG("Notification card: moving it (%s, %s hand)", laser ? "laser" : "hand", hand ? "right" : "left");
}

static void endNoteDrag(const XrPosef& head, bool headOk) {
    if (g_noteDrag.hand < 0) return;
    buzz(g_noteDrag.hand, 0.25f, 0.02f);
    g_noteDrag.hand = -1;
    g_noteDragEnded = GetTickCount64();
    if (!headOk) return;
    XrVector3f f = rotate3(head.orientation, {0, 0, -1});
    f.y = 0;
    f = dot3(f, f) > 1e-4f ? normalize3(f) : XrVector3f{0, 0, -1};
    const XrVector3f r = cross3(f, XrVector3f{0, 1, 0});
    const XrVector3f delta = sub3(sub3(g_noteDrag.held, g_noteDrag.offset), head.position);
    const float place[3] = {dot3(delta, r), delta.y, std::max(0.25f, dot3(delta, f))};
    settings::setNotePlacement(place);
}

static uint64_t noteShown() {
    std::lock_guard<std::mutex> lock(g_noteMutex);
    return g_noteOver.id;
}

static void updateNoteCard(const camera::View& view, bool firstPerson, bool headOk, const XrPosef& head) {
    HudSheet& s = g_hud[kHudNote];
    NoteOver n;
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        n = g_noteOver;
    }
    const float used = hud::noteHeight();
    if (!n.id || !firstPerson || !headOk || !(used > 0)) {
        g_noteCardFront = false;
        g_noteDrag.hand = -1;
        hideHud(s);
        return;
    }
    const XrVector3f eye = head.position;
    XrVector3f at, x, y;
    float w;
    if (n.front) {
        XrVector3f look = rotate3(head.orientation, {0, 0, -1});
        XrVector3f flat = normalize3({look.x, 0, look.z});
        if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
        const XrVector3f right = cross3(flat, XrVector3f{0, 1, 0});
        // Where you put it (right, up, forward of your head facing level), else 0.75 m ahead, a little low.
        float place[3] = {0, -0.12f, 0.75f};
        settings::notePlacement(place);
        if (g_noteDrag.hand >= 0)
            at = sub3(g_noteDrag.held, g_noteDrag.offset);
        else
            at = {eye.x + right.x * place[0] + flat.x * place[2], eye.y + place[1],
                  eye.z + right.z * place[0] + flat.z * place[2]};
        w = 0.42f;
    } else {
        XrVector3f headAt = worldToApp(view, n.at, false);
        XrVector3f to = sub3(headAt, eye);
        const float away = std::sqrt(dot3(to, to));
        w = std::max(0.3f, std::min(1.2f, g_config.popupSize * 0.8f * std::max(away, 0.6f)));
        const float h = w * used / hud::kNote.w;
        at = {headAt.x, headAt.y + 0.28f + h / 2, headAt.z};
    }
    facingAxes(at, eye, x, y);
    g_noteCardFront = n.front;
    g_noteCardAt = at, g_noteCardX = x, g_noteCardY = y;
    g_noteCardW = w, g_noteCardH = w * used / hud::kNote.w;
    if (!n.front) g_noteDrag.hand = -1;
    s.onTop = true;
    placeHud(s, at, x, y, w, {hud::kNote.x, hud::kNote.y, hud::kNote.w, used});
}

// All but the popup, picker and notification cards (they show whatever the HUD setting).
static void hideAllHud() {
    bool any = false;
    for (int i = 0; i < kHudCount; ++i) {
        if (i == kHudPopup || i == kHudPicker || i == kHudNote || i == kHudTip) continue;  // the tip: updateTip
        any = any || g_hud[i].m.active;
        hideHud(g_hud[i]);
    }
    if (any) phone3d::setPieces(nullptr, 0, nullptr);
}

// The tooltip of the need, moodlet, want, queue button or popup answer the
// laser points at (user 18:34Z: as the game's UI shows one), just above that
// sheet (under the wants), turned as it is; outside the sheet, so never cut.
static void updateTip() {
    HudSheet& tip = g_hud[kHudTip];
    const int kind = hud::tipKind();
    const float used = hud::tipHeight();
    const int from = kind == hud::kNeedItems      ? kHudNeeds
                     : kind == hud::kMoodletItems ? kHudMoodlets
                     : kind == hud::kWantItems    ? kHudWants
                     : kind == hud::kQueueItems   ? kHudQueue
                     : kind == hud::kPopupButtons ? kHudPopup
                                                  : -1;
    if (from >= 0 && g_hud[from].m.active && used > 0) {
        const PieMenu& m = g_hud[from].m;
        // The wants' tip under them (they are above you), as wide as they are.
        const bool wants = from == kHudWants;
        const float w = wants ? m.width * 0.75f
                        : from == kHudPopup ? std::max(0.22f * g_config.wristScale, m.width * 0.7f)
                                            : 0.22f * g_config.wristScale,
                    h = w * used / hud::kTip.w;
        const float up = (m.height / 2 + h / 2 + 0.006f) * (wants ? -1 : 1);
        const XrVector3f at{m.pose.position.x + m.y.x * up + m.z.x * 0.006f,
                            m.pose.position.y + m.y.y * up + m.z.y * 0.006f,
                            m.pose.position.z + m.y.z * up + m.z.z * 0.006f};
        tip.onTop = true;
        placeHud(tip, at, m.x, m.y, w, {hud::kTip.x, hud::kTip.y, hud::kTip.w, used});
    } else {
        hideHud(tip);
    }
}

// Every frame (updateControls), before the pointer is worked out.
static void updateHud(const armik::Hand hands[2], bool firstPerson, bool headOk, const XrPosef& head) {
    g_hudInput = hud::Frame{};
    camera::View view;
    const bool viewOk = camera::lastView(view);
    if (viewOk)
        updateNoteCard(view, firstPerson, headOk, head);
    else
        hideHud(g_hud[kHudNote]);
    if (!firstPerson || !headOk || !g_config.wristPieces || !viewOk) {
        hideAllHud();
        // The popup, picker and notification cards alone (taken away again once they are gone).
        static bool alone = false;
        ID3D11Texture2D* atlas = hud::texture();
        phone3d::Panel panels[4];
        int count = 0;
        updateTip();
        for (int i : {kHudPopup, kHudPicker, kHudNote, kHudTip})
            if (viewOk && atlas && g_hud[i].m.active) {
                panels[count] = hudPanel(g_hud[i], view);
                panels[count++].tag = i;
            }
        if (count || alone) phone3d::setPieces(panels, count, count ? atlas : nullptr);
        alone = count > 0;
        return;
    }
    const XrVector3f eye = head.position;
    XrVector3f look = rotate3(head.orientation, {0, 0, -1});
    XrVector3f flat = normalize3({look.x, 0, look.z});
    if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
    const bool asleep = eyelids::sleeping();
    const bool leftFree = !grab::holding(0) && !(g_phoneOut && g_phoneHand == 0);

    const bool rightFree = !grab::holding(1) && !(g_phoneOut && g_phoneHand == 1);
    const Config& c = g_config;
    struct ArmPiece {
        int sheet;
        bool on;
        int side;
        const float *pos, *turn;
        float around, size;
        hud::Rect area;
        bool toElbow;
    };
    // Left wrist: the watch inside, the build/buy button on the thumb side,
    // the action queue along the top (running first at the wrist). Inside the
    // right forearm: the needs, and the moodlets toward the elbow.
    const ArmPiece pieces[] = {
        {kHudWatch, c.wristTime && leftFree, 0, c.wristTimePos, c.wristTimeTurn, c.wristTimeAround, c.wristTimeSize,
         hud::kWatch, false},
        {kHudBuild, c.wristBuild && leftFree, 0, c.wristBuildPos, c.wristBuildTurn, c.wristBuildAround,
         c.wristBuildSize, hud::kBuild, false},
        {kHudQueue, c.wristQueue && leftFree, 0, c.wristQueuePos, c.wristQueueTurn, c.wristQueueAround,
         c.wristQueueSize, hud::kQueue, true},
        {kHudNeeds, c.wristNeeds && rightFree, 1, c.wristNeedsPos, c.wristNeedsTurn, c.wristNeedsAround,
         c.wristNeedsSize, hud::kNeeds, true},
        {kHudMoodlets, c.wristMoodlets && rightFree, 1, c.wristMoodletsPos, c.wristMoodletsTurn,
         c.wristMoodletsAround, c.wristMoodletsSize, hud::kMoodlets, true},
    };
    for (const ArmPiece& p : pieces) {
        HudSheet& sheet = g_hud[p.sheet];
        if (!(p.on && !asleep &&
              onArm(sheet, p.side, hands[p.side], view, eye, p.pos, p.turn, p.around, p.size, p.area, p.toElbow)))
            hideHud(sheet);
    }
    // The wants above you, ahead: seen when you look up.
    {
        HudSheet& wants = g_hud[kHudWants];
        // Drawn over the world: tested against its depth, ceilings and walls
        // near you cut it up or hid it (user 18:44Z).
        wants.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!wants.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(40 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.05f, 0, dir.z + (flat.z - dir.z) * 0.05f});
        if (c.wristWhims && !asleep && hud::hasItems(hud::kWantItems))
            aroundYou(wants, eye, dir, eye, c.wristWhimsPos, c.wristWhimsTurn, c.wristWhimsSize, hud::kWants);
        else
            hideHud(wants);
    }

    // The money gained or lost, in front of you a little up and right, turning
    // with you lazily; over the world.
    {
        HudSheet& money = g_hud[kHudMoney];
        money.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!money.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(35 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.06f, 0, dir.z + (flat.z - dir.z) * 0.06f});
        if (c.wristMoney && !asleep && hud::moneyShowing())
            aroundYou(money, eye, dir, eye, c.wristMoneyPos, c.wristMoneyTurn, c.wristMoneySize, hud::kMoney);
        else
            hideHud(money);
    }

    // The pause symbol, 30 cm under where popups show in front of you (under
    // the popup itself while one is there), turning with you lazily. Not
    // pointable: it only shows that the game is paused.
    {
        HudSheet& pause = g_hud[kHudPause];
        pause.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!pause.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(30 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.06f, 0, dir.z + (flat.z - dir.z) * 0.06f});
        if (c.wristPause && g_paused.load() && !asleep) {
            float distance = std::max(0.3f, c.popupDistance);
            XrVector3f at{eye.x + dir.x * distance, eye.y - 0.08f, eye.z + dir.z * distance};
            int kind;
            {
                std::lock_guard<std::mutex> lock(g_objMenuMutex);
                kind = g_objMenuRequest.kind;
            }
            if (g_objMenu.active && kind == 4) at = g_objMenu.pose.position;
            if (g_hud[kHudPopup].m.active && kind == 4) at = g_hud[kHudPopup].m.pose.position;
            aroundYou(pause, at, dir, eye, c.wristPausePos, c.wristPauseTurn, c.wristPauseSize, hud::kPauseArea);
        } else {
            hideHud(pause);
        }
    }

    updateTip();

    // Asleep: the watch low in your view, over the closed eyelids (ahead =
    // where you look, half its tilt up or down).
    {
        HudSheet& sleep = g_hud[kHudSleep];
        if (c.wristSleepClock && asleep) {
            XrVector3f fwd = normalize3({look.x, look.y * 0.5f, look.z});
            aroundYou(sleep, eye, fwd, eye, c.wristSleepPos, c.wristSleepTurn, c.wristSleepSize, hud::kWatch);
        } else {
            hideHud(sleep);
        }
    }

    phone3d::Panel panels[kHudCount];
    int count = 0;
    ID3D11Texture2D* atlas = hud::texture();
    if (atlas)
        for (int i = 0; i < kHudCount; ++i)
            if (i != kHudSleep && g_hud[i].m.active) {
                panels[count] = hudPanel(g_hud[i], view);
                panels[count++].tag = i;
            }
    {
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        for (int i : {kHudWants, kHudPause, kHudTip, kHudMoney}) {
            const PieMenu& m = g_hud[i].m;
            const bool around = i != kHudTip || hud::tipKind() == hud::kWantItems;
            g_appPlace[i] = {m.active && around, m.pose.position, m.x, m.y};
        }
    }
    {
        // How often the wants come and go (user 19:15Z: they pop in and out).
        static bool was = false;
        static int changes = 0;
        static ULONGLONG since = GetTickCount64();
        const bool now = g_hud[kHudWants].m.active;
        if (now != was) ++changes, was = now;
        if (GetTickCount64() - since > 10000) {
            if (changes > 2) LOG("HUD: the wants came and went %d times in 10 s", changes);
            changes = 0;
            since = GetTickCount64();
        }
    }
    static int shown = -1;
    if (count != shown) {
        static int logged = 0;
        if (logged++ < 40) LOG("HUD: %d shown in the world%s", count, g_paused.load() ? " (paused)" : "");
        shown = count;
    }
    phone3d::setPiecePlacer(placeArmPieces);
    phone3d::setPieces(panels, count, count ? atlas : nullptr);
}

// A finger on a HUD sheet: the pointer there, pressing once it has gone
// through the surface from in front (and until it comes back out).
static bool pokeHud(HudSheet& s, const armik::Hand hands[2], const bool use[2]) {
    PieMenu& m = s.m;
    if (!m.active) return false;
    int best = -1;
    float bestZ = 1e9f, bx = 0, by = 0, bz = 0;
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid || !use[i]) {
            m.armed[i] = false;
            continue;
        }
        float lx, ly, lz;
        sheetLocal(m, simFingertip(hands[i], i), lx, ly, lz);
        if (!insideSheet(m, lx, ly, 0.01f) || lz > kPokeHover || lz < -kPokeThrough) {
            m.armed[i] = false;
            if (m.pressing == i) m.pressing = -1;
            continue;
        }
        if (lz > 0.005f) m.armed[i] = true;
        float rank = i == m.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = i;
            bx = lx;
            by = ly;
            bz = lz;
        }
    }
    if (best < 0) {
        m.pressing = -1;
        return false;
    }
    if (m.pressing == best && bz > kPokeRelease) m.pressing = -1;
    if (m.pressing != best && m.armed[best] && bz <= 0) {
        m.pressing = best;
        m.armed[best] = false;
        buzz(best, 0.2f, 0.015f);
    }
    float a = std::max(0.0f, std::min(1.0f, bx / m.width + 0.5f)), b = std::max(0.0f, std::min(1.0f, 0.5f - by / m.height));
    g_hudInput.px = m.x0 + a * m.cw;
    g_hudInput.py = m.y0 + b * m.ch;
    g_hudInput.down = m.pressing == best;
    XrVector3f onSheet{m.pose.position.x + m.x.x * bx + m.y.x * by, m.pose.position.y + m.x.y * bx + m.y.y * by,
                       m.pose.position.z + m.x.z * bx + m.y.z * by};
    g_laserTo = onSheet;
    g_laserFrom = {onSheet.x + m.z.x * 0.1f, onSheet.y + m.z.y * 0.1f, onSheet.z + m.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}

// Pokes on the watch (your right hand) or the asleep watch (either hand). They
// never reach the game.
static bool pokeHuds(const armik::Hand hands[2]) {
    static const bool right[2] = {false, true}, both[2] = {true, true};
    return pokeHud(g_hud[kHudPopup], hands, both) || pokeHud(g_hud[kHudPicker], hands, both) ||
           pokeHud(g_hud[kHudNote], hands, both) ||
           pokeHud(g_hud[kHudWatch], hands, right) ||
           pokeHud(g_hud[kHudBuild], hands, right) ||
           pokeHud(g_hud[kHudQueue], hands, right) || pokeHud(g_hud[kHudSleep], hands, both);
}

// The laser on the watch, the asleep watch, the build button, the queue or
// the wants above you: true with the hit, and the pointer for the HUD (the
// trigger presses).
static bool laserPhone(const XrVector3f& o, const XrVector3f& d, bool trigger, XrVector3f& hit) {
    const PieMenu& m = g_phoneSheet.m;
    float u, v;
    if (!m.active || !hitSheet(m, o, d, hit, u, v)) return false;
    g_hudInput.px = u * hud::kWidth;
    g_hudInput.py = v * hud::kHeight;
    g_hudInput.down = trigger;
    return true;
}

static bool laserHud(const XrVector3f& o, const XrVector3f& d, bool trigger, XrVector3f& hit) {
    if (laserPhone(o, d, trigger, hit)) return true;  // the phone's screen first (in your hand, in front of the rest)
    for (int i : {kHudPopup, kHudPicker, kHudNote, kHudSleep, kHudWatch, kHudBuild, kHudQueue, kHudWants, kHudNeeds,
                  kHudMoodlets}) {
        const PieMenu& m = g_hud[i].m;
        float u, v;
        if (!m.active || !hitSheet(m, o, d, hit, u, v)) continue;
        g_hudInput.px = u * hud::kWidth;
        g_hudInput.py = v * hud::kHeight;
        g_hudInput.down = trigger;
        return true;
    }
    return false;
}

// Present thread, after updateControls: draws the HUD and acts on its buttons.
static void drawHud(ID3D11Texture2D* backbuffer) {
    hud::Frame f = g_hudInput;
    f.watch = g_hud[kHudWatch].m.active || g_hud[kHudSleep].m.active;
    f.pause = g_hud[kHudPause].m.active;
    f.build = g_hud[kHudBuild].m.active;
    f.needs = g_hud[kHudNeeds].m.active;
    f.moodlets = g_hud[kHudMoodlets].m.active;
    f.queue = g_hud[kHudQueue].m.active;
    f.wants = g_hud[kHudWants].m.active;
    f.money = g_hud[kHudMoney].m.active;
    f.fullUi = g_fullUi;
    f.uiButton = g_config.uiFirstPerson == 3 && g_config.wristFullButton;
    // The popup's texts are looked up and it is measured before its card shows.
    f.popup = hud::popupPending();
    f.picker = hud::pickerPending();
    f.phone = g_phoneSheet.m.active;
    f.note = noteShown();
    if (!f.watch && !f.pause && !f.build && !f.needs && !f.moodlets && !f.queue && !f.wants && !f.popup && !f.money &&
        !f.picker && !f.phone && !f.note)
        return;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    device->GetImmediateContext(&context);
    int index = -1;
    hud::Action action = hud::render(device, context, f, &index);
    context->Release();
    device->Release();
    switch (action) {
        case hud::kFullUi:
            g_fullUi = !g_fullUi;
            LOG("HUD: full UI on your wrist %s", g_fullUi ? "on" : "off");
            break;
        case hud::kPausePressed:
        case hud::kSpeed1:
        case hud::kSpeed2:
        case hud::kSpeed3:
            // controls.py: 20 pause (or back to the last speed), 21..23 speed 1..3.
            grab::pushControl(20 + (action - hud::kPausePressed));
            LOG("HUD: speed button %d", action - hud::kPausePressed);
            break;
        case hud::kBuildPressed:
            g_buildKeyFrames = 3;
            LOG("HUD: build button (F2)");
            break;
        case hud::kCancel:
            // controls.py: 30 + n cancels the n-th item of the queue it sent,
            // 90 + n the side action on it.
            if (index >= 0 && index < hud::kMaxItems) grab::pushControl(30 + index);
            if (index >= hud::kMaxItems && index < 2 * hud::kMaxItems) grab::pushControl(90 + index - hud::kMaxItems);
            LOG("HUD: cancel queue item %d%s", index % hud::kMaxItems, index >= hud::kMaxItems ? " (its side action)" : "");
            break;
        case hud::kAnswer:
            // controls.py: 50 + n answers the popup with its n-th button.
            if (index >= 0 && index < 10) grab::pushControl(50 + index);
            LOG("HUD: popup button %d", index);
            break;
        case hud::kPick:
            // controls.py: 1000 + row * 4 + the ingredient boxes picks the picker's row.
            if (index >= 0) grab::pushControl(1000 + index);
            LOG("HUD: picker row %d (boxes %d)", index / 4, index % 4);
            break;
        case hud::kPickerClose:
            // controls.py: 60 cancels the picker (the game's close button).
            grab::pushControl(60);
            LOG("HUD: picker closed");
            break;
        case hud::kPhonePick:
            // controls.py: 100000 + n picks the phone menu's entry n (phoneapp.py, the game's interactions.select).
            if (index >= 0) grab::pushControl(100000 + index);
            buzz(1 - g_phoneHand, 0.2f, 0.015f);
            LOG("Phone: picked entry %d of the game's phone menu", index);
            break;
        case hud::kNotesOpened:
            grab::pushControl(78);  // notify.opened: all read
            LOG("Phone: notifications opened");
            break;
        case hud::kNoteAnswer:
            if (GetTickCount64() - g_noteDragEnded < 400) break;  // the press that moved the card
            // notify.answer: the notification's button (index = its id * 8 + button).
            if (index >= 0) grab::pushControl(10000000 + index);
            if (const uint64_t shownId = noteShown(); index >= 0 && shownId == static_cast<uint64_t>(index / 8)) {
                g_noteDismissed = shownId;  // answered on the card: it goes
                const float none[3] = {};
                setNoteOver(0, none, false);
            }
            LOG("Notification %d: button %d", index / 8, index % 8);
            break;
        case hud::kCalendarOpened:
            grab::pushControl(81);  // phonecal.send
            LOG("Phone: calendar opened");
            break;
        case hud::kNotesClear:
            grab::pushControl(79);
            LOG("Phone: notifications cleared");
            break;
        case hud::kNoteDelete:
            if (index > 0 && index < 10000) grab::pushControl(80000 + index);
            break;
        case hud::kNoteDismiss: {
            if (GetTickCount64() - g_noteDragEnded < 400) break;  // the press that moved the card
            const uint64_t id = noteShown();
            g_noteDismissed = id;
            const float none[3] = {};
            setNoteOver(0, none, false);
            LOG("Notification %llu dismissed", static_cast<unsigned long long>(id));
            break;
        }
        case hud::kPhoneColor:
            if (index >= 0 && index < 100) grab::pushControl(90000 + index);  // phoneapp.set_color
            LOG("Phone: wallpaper colour %d", index);
            break;
        case hud::kPhoneSilence:
            grab::pushControl(77);  // controls.py: the phone's silence (ui.toggle_silence_phone)
            LOG("Phone: silence button");
            break;
        case hud::kPhoneOptions:
            // The Options app: the game menu (Escape) on the phone's screen, as before.
            if (g_phoneOut && !g_tablet && g_popupOpen) {
                LOG("Phone: Options while a popup is up: answer the popup first (it is in front of you)");
            } else if (g_phoneOut && !g_tablet) {
                g_phoneOptions = true;
                g_phoneOptionsSeen = gameMenuOpen();
                if (!gameMenuOpen() && !g_popupOpen) {
                    g_escapeFrames = 3;
                    g_phoneOpenedMenu = true;
                }
                g_phoneFitted = false;
                g_phoneMenuSince = GetTickCount64();
                LOG("Phone: Options (the game menu)%s", g_popupOpen ? ", a popup is up: no Escape" : "");
            }
            break;
        case hud::kDismissWant:
            // controls.py: 40 + n dismisses the n-th want it sent.
            if (index >= 0 && index < 4) grab::pushControl(40 + index);
            LOG("HUD: dismiss want %d", index);
            break;
        default:
            break;
    }
}

// The asleep watch's headset layer: the atlas copied into its swapchain.
static bool hudSleepLayer(ID3D11Texture2D* atlas, XrCompositionLayerQuad& layer) {
    const PieMenu& m = g_hud[kHudSleep].m;
    if (!m.active || !atlas) return false;
    D3D11_TEXTURE2D_DESC desc;
    atlas->GetDesc(&desc);
    bool recreated;
    if (!ensureSwapchain(g_hudSwapchain, desc, recreated) || !copyIntoSwapchain(g_hudSwapchain, atlas, desc))
        return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_hudSwapchain);
    layer.subImage.imageRect.offset = {m.x0, m.y0};
    layer.subImage.imageRect.extent = {m.cw, m.ch};
    layer.pose = m.pose;
    layer.size = {m.width, m.height};
    return true;
}

