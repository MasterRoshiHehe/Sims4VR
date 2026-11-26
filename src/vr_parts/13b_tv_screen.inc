// A second window on a TV at the same time as Claude / Sims 1 (claudescreen.h slot 1: Discord, thread "Show Discord
// stream on in-game TV"). Always flat, at the window's aspect, as large as fits on the TV's screen. Only the laser
// clicks into it (when [discord] interactive); a viewer otherwise, nothing else of yours changes.
// Screen 1 is the Linear board (claudescreen::kLinear, linearboard.h; thread "Bring the Linear board into the game",
// GLA-46), drawn the same way; the laser and a finger (pokeTv) move its cards.
constexpr int kTvScreens = 2;
static const int kTvSlot[kTvScreens] = {claudescreen::kDiscord, claudescreen::kLinear};
static const char* const kTvName[kTvScreens] = {"Discord screen", "Linear board"};
struct TvScreen {
    bool active = false;
    XrVector3f at{}, x{}, y{}, z{};  // app space; z out of the screen
    float width = 0, height = 0;     // the picture, metres
    float u = 0.5f, v = 0.5f;
    int buttons = 0;
    bool used = false;
    // A finger on it (pokeTv): which hand presses, and which may (came from in front).
    int pressing = -1;
    bool armed[2] = {false, false};
};
static TvScreen g_tvs[kTvScreens];
static int g_tvHitIndex = 0;  // the screen hitTv found (tvMouse clicks into it)

// The Linear scrumboard (linearboard.h cork(): the board on wall art): a note taken off it by a hand closing on it
// (updateBoardGrab, from 16_controls.inc's grab block), drawn in that hand until it opens.
struct BoardHold {
    int hand = -1;
    float at[2] = {0.5f, 0.5f};  // where on the note the hand holds it (0..1 across, down)
    XrVector3f palm{}, head{};
    bool headOk = false;
};
static BoardHold g_boardHold;

// A point in app space on the board: u v across it (0..1), lz in front of it (metres).
static bool boardUv(const XrVector3f& p, float& u, float& v, float& lz) {
    const TvScreen& b = g_tvs[1];
    if (!b.active || b.width <= 0 || b.height <= 0) return false;
    XrVector3f rel = sub3(p, b.at);
    u = dot3(rel, b.x) / b.width + 0.5f;
    v = 0.5f - dot3(rel, b.y) / b.height;
    lz = dot3(rel, b.z);
    return u >= -0.05f && u <= 1.05f && v >= -0.05f && v <= 1.05f;
}

// A flat panel from app space (centre, axes, metres) into the world, as phone3d draws it.
static phone3d::Panel appPanel(const camera::View& view, const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w,
                               float h) {
    const float scale = view.scale > 0 ? view.scale : 1.0f;
    phone3d::Panel panel;
    panel.show = true;
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
    panel.crop[2] = panel.crop[3] = 1;
    return panel;
}

// Every frame in first person: a hand closing on a note (palm within 10 cm in front of the board) takes it; while
// held it follows the hand and the board shows where it would go when the hand is at the board; opening the hand
// pins it there (moved in Linear) or, away from the board, puts it back.
static void updateBoardGrab(const armik::Hand hands[2], const float gripWas[2], const grab::HeadPose& head) {
    g_boardHold.headOk = head.valid;
    if (head.valid) g_boardHold.head = {head.position.x, head.position.y, head.position.z};
    if (!g_tvs[1].active || !linearboard::cork()) {
        if (g_boardHold.hand >= 0) linearboard::drop(0.5f, 0.5f, false);
        g_boardHold.hand = -1;
        return;
    }
    if (g_boardHold.hand < 0) {
        for (int i = 0; i < 2; ++i) {
            if (!hands[i].valid || hands[i].grip <= 0.6f || gripWas[i] > 0.6f || grab::holding(i) ||
                (g_phoneOut && i == g_phoneHand) || (g_bookOut && i == g_bookHand))
                continue;
            const XrVector3f p{hands[i].position.x, hands[i].position.y, hands[i].position.z};
            float u, v, lz;
            if (!boardUv(p, u, v, lz) || lz > 0.10f || lz < -0.06f) continue;
            if (!linearboard::grab(std::max(0.0f, std::min(1.0f, u)), std::max(0.0f, std::min(1.0f, v)), g_boardHold.at))
                continue;
            g_boardHold.hand = i;
            g_boardHold.palm = p;
            buzz(i, 0.35f, 0.04f);
            break;
        }
        return;
    }
    const armik::Hand& h = hands[g_boardHold.hand];
    if (h.valid) g_boardHold.palm = {h.position.x, h.position.y, h.position.z};
    float u = 0.5f, v = 0.5f, lz = 1;
    const bool atBoard = boardUv(g_boardHold.palm, u, v, lz) && lz < 0.15f && lz > -0.08f;
    u = std::max(0.0f, std::min(1.0f, u));
    v = std::max(0.0f, std::min(1.0f, v));
    if (h.grip < 0.35f) {
        linearboard::drop(u, v, atBoard);
        buzz(g_boardHold.hand, atBoard ? 0.5f : 0.2f, 0.05f);
        g_boardHold.hand = -1;
        return;
    }
    linearboard::hold(u, v, atBoard);
}

// The note in your hand (after the board's panel): flat against the board while the hand is at it, else turned
// to your eyes; the spot you took it by stays at your palm, a little in front of your fingers.
static void placeHeldNote(const camera::View& view) {
    float crop[4], size[2];
    const TvScreen& b = g_tvs[1];
    if (g_boardHold.hand < 0 || !b.active || !linearboard::heldCard(crop, size)) {
        phone3d::setTvScreen(phone3d::Panel{}, 2);
        return;
    }
    const float cw = size[0] * b.width, ch = size[1] * b.height;
    XrVector3f cx = b.x, cy = b.y, cz = b.z;
    float u, v, lz;
    const bool atBoard = boardUv(g_boardHold.palm, u, v, lz) && lz < 0.25f;
    if (!atBoard && g_boardHold.headOk) {
        XrVector3f toHead = sub3(g_boardHold.head, g_boardHold.palm);
        if (dot3(toHead, toHead) > 1e-4f) {
            cz = normalize3(toHead);
            XrVector3f side = cross3({0, 1, 0}, cz);
            if (dot3(side, side) > 1e-4f) {
                cx = normalize3(side);
                cy = cross3(cz, cx);
            } else {
                cz = b.z;
            }
        }
    }
    const float ox = (0.5f - g_boardHold.at[0]) * cw, oy = (g_boardHold.at[1] - 0.5f) * ch;
    const XrVector3f& p = g_boardHold.palm;
    XrVector3f c{p.x + cx.x * ox + cy.x * oy + cz.x * 0.03f, p.y + cx.y * ox + cy.y * oy + cz.y * 0.03f,
                 p.z + cx.z * ox + cy.z * oy + cz.z * 0.03f};
    if (atBoard) {  // never into the board: at least 1.5 cm in front of it
        const float front = dot3(sub3(c, b.at), b.z);
        if (front < 0.015f) c = {c.x + b.z.x * (0.015f - front), c.y + b.z.y * (0.015f - front), c.z + b.z.z * (0.015f - front)};
    }
    phone3d::Panel note = appPanel(view, c, cx, cy, cw, ch);
    note.crop[0] = crop[0];
    note.crop[1] = crop[1];
    note.crop[2] = crop[2];
    note.crop[3] = crop[3];
    phone3d::setTvScreen(note, 2);
}

static void hideTvScreen(int i) {
    TvScreen& g_tv = g_tvs[i];
    if (g_tv.active && g_tv.buttons) claudescreen::mouse(g_tv.u, g_tv.v, 0, kTvSlot[i]);
    g_tv.active = false;
    g_tv.buttons = 0;
    g_tv.pressing = -1;
    phone3d::setTvScreen(phone3d::Panel{}, i);
    if (i == 1) phone3d::setTvScreen(phone3d::Panel{}, 2);  // a note in your hand goes with the board
}

static void updateTvScreen(int i, bool firstPerson) {
    TvScreen& g_tv = g_tvs[i];
    const int slot = kTvSlot[i];
    // Neither the laser nor a finger is on it any more: a held button lets go.
    if (!g_tv.used && g_tv.buttons) claudescreen::mouse(g_tv.u, g_tv.v, g_tv.buttons = 0, slot);
    g_tv.used = false;
    float r[11];
    camera::View view;
    if (!firstPerson || !claudescreen::screen(r, slot) || !camera::lastView(view)) {
        if (i == 0) discordaudio::setEars(false, 0, 0);
        if (g_tv.active) hideTvScreen(i);
        return;
    }
    const float scale = view.scale > 0 ? view.scale : 1.0f;
    XrVector3f centre = worldToApp(view, r, false);
    XrVector3f z = normalize3(worldToApp(view, r + 3, true)), up = normalize3(worldToApp(view, r + 6, true));
    XrVector3f x = cross3(up, z);
    if (!(dot3(x, x) > 1e-4f)) return;
    x = normalize3(x);
    XrVector3f y = cross3(z, x);
    float faceW = r[9] / scale, faceH = r[10] / scale;
    int fw = 0, fh = 0;
    float aspect = claudescreen::frameSize(fw, fh, slot) ? static_cast<float>(fw) / static_cast<float>(fh) : 16.0f / 9.0f;
    // A screen standing upright (a monitor turned to portrait, user 2026-10-28 23:32Z) gets the wide picture turned
    // a quarter round (its top toward the screen's right) so it fills it instead of a thin strip.
    const bool cork = i == 1 && linearboard::cork();  // the scrumboard on wall art: its own shape, never turned
    if (!cork && faceH > faceW * 1.3f && aspect > 1.2f) {
        const XrVector3f turned = {-y.x, -y.y, -y.z};
        y = x;
        x = turned;
        std::swap(faceW, faceH);
    }
    float w = faceW, h = w / aspect;
    if (h > faceH) {
        h = faceH;
        w = h * aspect;
    }
    if (!(w > 0.02f && h > 0.02f)) {
        if (i == 0) discordaudio::setEars(false, 0, 0);
        if (g_tv.active) hideTvScreen(i);
        return;
    }
    if (!g_tv.active) {
        static int logged = 0;
        if (logged++ < 20) LOG("%s: shown %.2f x %.2f m on a %.2f x %.2f m screen", kTvName[i], w, h, faceW, faceH);
    }
    const float out = 0.006f;  // just in front of the glass (and of a Claude screen on the same object)
    g_tv.active = true;
    g_tv.at = {centre.x + z.x * out, centre.y + z.y * out, centre.z + z.z * out};
    g_tv.x = x;
    g_tv.y = y;
    g_tv.z = z;
    g_tv.width = w;
    g_tv.height = h;
    phone3d::Panel panel;
    panel.show = true;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {g_tv.at.x * scale, g_tv.at.y * scale, g_tv.at.z * scale});
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
    panel.crop[2] = panel.crop[3] = 1;
    if (i == 1) linearboard::boardCrop(panel.crop);  // the board's part of the picture (the held note's strip is below)
    phone3d::setTvScreen(panel, i);
    if (i == 1) placeHeldNote(view);
    if (i != 0) return;  // the board has no sound
    // Its sound from the TV (user 2026-10-28 19:23Z): quieter with distance, panned to its side, a little duller
    // behind you; the same as Sims 1's (13_sims1_screen.inc).
    if (g_eyeValid[0] && g_eyeValid[1]) {
        const XrVector3f& e0 = g_eyePose[0].position;
        const XrVector3f& e1 = g_eyePose[1].position;
        XrVector3f head{(e0.x + e1.x) * 0.5f, (e0.y + e1.y) * 0.5f, (e0.z + e1.z) * 0.5f};
        XrVector3f ears = sub3(e1, e0);
        ears = dot3(ears, ears) > 1e-6f ? normalize3(ears) : rotate3(g_eyePose[0].orientation, {1, 0, 0});
        XrVector3f ahead = rotate3(g_eyePose[0].orientation, {0, 0, -1});
        XrVector3f to = sub3(g_tv.at, head);
        float d = std::sqrt(dot3(to, to));
        float pan = 0, front = 1;
        if (d > 1e-3f) {
            pan = std::fmax(-1.0f, std::fmin(1.0f, dot3(to, ears) / d));
            front = dot3(to, ahead) / d;
        }
        float g = 1.0f / (1.0f + std::fmax(0.0f, d - 0.5f) * 0.7f);
        if (front < 0) g *= 1.0f + 0.25f * front;
        float side = 1.0f + 0.8f * std::fabs(pan);
        discordaudio::setEars(true, g * std::sqrt((1.0f - 0.8f * pan) / side), g * std::sqrt((1.0f + 0.8f * pan) / side));
    } else {
        discordaudio::setEars(true, 1, 1);
    }
}

static void updateTvScreen(bool firstPerson) {
    for (int i = 0; i < kTvScreens; ++i) updateTvScreen(i, firstPerson);
}

// The laser on a TV picture (only while the laser may click into it): the nearest one hit; g_tvHitIndex says which.
static bool hitTv(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    float best = 1e9f;
    bool any = false;
    for (int i = 0; i < kTvScreens; ++i) {
        const TvScreen& g_tv = g_tvs[i];
        if (!g_tv.active || !claudescreen::interactive(kTvSlot[i])) continue;
        float facing = dot3(d, g_tv.z);
        if (facing > -0.01f) continue;
        float t = -dot3(sub3(o, g_tv.at), g_tv.z) / facing;
        if (t <= 0 || t >= best) continue;
        XrVector3f at = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
        XrVector3f rel = sub3(at, g_tv.at);
        float uu = dot3(rel, g_tv.x) / g_tv.width + 0.5f, vv = 0.5f - dot3(rel, g_tv.y) / g_tv.height;
        if (!(uu >= 0 && uu <= 1 && vv >= 0 && vv <= 1)) continue;
        best = t;
        hit = at;
        u = uu;
        v = vv;
        g_tvHitIndex = i;
        any = true;
    }
    return any;
}

static void tvMouse(float u, float v, int buttons) {
    TvScreen& g_tv = g_tvs[g_tvHitIndex];
    g_tv.u = u;
    g_tv.v = v;
    g_tv.buttons = buttons;
    g_tv.used = true;
    claudescreen::mouse(u, v, buttons, kTvSlot[g_tvHitIndex]);
}

// A finger on the Linear board (user rule: your own hand, not only the laser): touching it presses there, as on
// Sims 1's picture (from in front, let go when pulled back); that hand's trigger presses too. Discord's window is
// left to the laser, as before.
static bool pokeTv(const armik::Hand hands[2], const bool trigger[2]) {
    const int i = 1;
    TvScreen& g_tv = g_tvs[i];
    if (!g_tv.active || !claudescreen::interactive(kTvSlot[i])) return false;
    int best = -1;
    float bestZ = 1e9f, bu = 0, bv = 0, bz = 0;
    for (int k = 0; k < 2; ++k) {
        if (!hands[k].valid) {
            g_tv.armed[k] = false;
            continue;
        }
        XrVector3f rel = sub3(fingertip(hands[k]), g_tv.at);
        float u = dot3(rel, g_tv.x) / g_tv.width + 0.5f, v = 0.5f - dot3(rel, g_tv.y) / g_tv.height, lz = dot3(rel, g_tv.z);
        const float mu = 0.02f / g_tv.width, mv = 0.02f / g_tv.height;
        bool inRange = u > -mu && u < 1 + mu && v > -mv && v < 1 + mv && lz < kPokeHover && lz > -kPokeThrough;
        if (!inRange) {
            g_tv.armed[k] = false;
            continue;
        }
        if (lz > 0.005f) g_tv.armed[k] = true;
        float rank = k == g_tv.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = k;
            bu = u;
            bv = v;
            bz = lz;
        }
    }
    if (best < 0) {
        if (g_tv.pressing >= 0 || g_tv.buttons) claudescreen::mouse(g_tv.u, g_tv.v, g_tv.buttons = 0, kTvSlot[i]);
        g_tv.pressing = -1;
        return false;
    }
    if (g_tv.pressing == best) {
        if (bz > kPokeRelease) g_tv.pressing = -1;
    } else if (bz <= 0 && g_tv.armed[best]) {
        g_tv.pressing = best;
        g_tv.armed[best] = false;
    }
    if (g_tv.pressing != best && g_tv.pressing >= 0) g_tv.pressing = -1;
    bu = std::max(0.0f, std::min(1.0f, bu));
    bv = std::max(0.0f, std::min(1.0f, bv));
    const bool down = g_tv.pressing == best || trigger[best];
    g_tvHitIndex = i;
    tvMouse(bu, bv, down ? 1 : 0);
    const float ox = (bu - 0.5f) * g_tv.width, oy = (0.5f - bv) * g_tv.height;
    XrVector3f onIt{g_tv.at.x + g_tv.x.x * ox + g_tv.y.x * oy, g_tv.at.y + g_tv.x.y * ox + g_tv.y.y * oy,
                    g_tv.at.z + g_tv.x.z * ox + g_tv.y.z * oy};
    g_laserTo = onIt;
    g_laserFrom = {onIt.x + g_tv.z.x * 0.1f, onIt.y + g_tv.z.y * 0.1f, onIt.z + g_tv.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}
