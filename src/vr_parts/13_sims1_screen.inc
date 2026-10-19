// Sims Forever (first person): Sims 1's picture on a computer's screen
// (simsforever.h). It fills the screen the script mod found in the computer's
// model (as large as it fits, at Sims 1's aspect), drawn in the world like
// the menus on objects; the laser or a finger on it is Sims 1's mouse.
struct Sims1Screen {
    bool active = false;
    XrVector3f at{}, x{}, y{}, z{};  // app space; z out of the screen
    float width = 0, height = 0;     // the picture, metres
    float faceW = 0, faceH = 0;      // the whole screen it's on, metres
    float scale = 1;                 // world units a metre
    int pressing = -1;               // the finger pressing it
    bool armed[2] = {false, false};
    float u = 0.5f, v = 0.5f;
    int buttons = 0;
    bool used = false;  // the laser or a finger gave it the mouse this frame
};
static Sims1Screen g_sims1;

static void hideSims1Screen() {
    if (g_sims1.active || g_sims1.buttons) simsforever::mouse(g_sims1.u, g_sims1.v, 0);
    g_sims1.active = false;
    g_sims1.buttons = 0;
    g_sims1.pressing = -1;
    simsforever::setLocked(false);
    phone3d::setSims1Screen(phone3d::Panel{});
}

static bool g_laserOnClaude = false;  // the laser is on Claude's screen (it keeps the front then)

static void updateSims1Screen(bool firstPerson) {
    simsforever::poll();
    // Neither the laser nor a finger is on it any more: a held button lets go.
    if (!g_sims1.used && g_sims1.buttons) simsforever::mouse(g_sims1.u, g_sims1.v, g_sims1.buttons = 0);
    g_sims1.used = false;
    float r[11];
    camera::View view;
    if (!firstPerson || !simsforever::screen(r) || !camera::lastView(view)) {
        if (g_sims1.active) hideSims1Screen();
        return;
    }
    float scale = view.scale > 0 ? view.scale : 1.0f;
    XrVector3f centre = worldToApp(view, r, false);
    XrVector3f z = normalize3(worldToApp(view, r + 3, true)), up = normalize3(worldToApp(view, r + 6, true));
    XrVector3f x = cross3(up, z);
    if (!(dot3(x, x) > 1e-4f)) return;
    x = normalize3(x);
    XrVector3f y = cross3(z, x);
    float faceW = r[9] / scale, faceH = r[10] / scale;
    int fw = 0, fh = 0;
    float aspect = simsforever::frameSize(fw, fh) ? static_cast<float>(fw) / static_cast<float>(fh) : 4.0f / 3.0f;
    float w = faceW, h = w / aspect;
    if (h > faceH) {
        h = faceH;
        w = h * aspect;
    }
    if (!(w > 0.02f && h > 0.02f)) {
        hideSims1Screen();
        return;
    }
    const float out = 0.005f;  // just in front of the glass
    XrVector3f at{centre.x + z.x * out, centre.y + z.y * out, centre.z + z.z * out};
    if (!g_sims1.active) {
        static int logged = 0;
        if (logged++ < 20) LOG("Sims Forever: shown %.2f x %.2f m on a %.2f x %.2f m screen", w, h, faceW, faceH);
    }
    g_sims1.active = true;
    g_sims1.at = at;
    g_sims1.x = x;
    g_sims1.y = y;
    g_sims1.z = z;
    g_sims1.width = w;
    g_sims1.height = h;
    g_sims1.faceW = faceW;
    g_sims1.faceH = faceH;
    g_sims1.scale = scale;
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
    panel.width = faceW * scale;
    panel.height = faceH * scale;
    panel.menu[2] = w / faceW;
    panel.menu[3] = h / faceH;
    panel.crop[2] = panel.crop[3] = 1;
    phone3d::setSims1Screen(panel);
    simsforever::setLocked(true);  // your real mouse and keyboard play Sims 1 meanwhile
    // Sims 1's sound from the computer (user 18:30Z): quieter with distance,
    // panned to its side, a little duller behind you.
    if (g_eyeValid[0] && g_eyeValid[1]) {
        const XrVector3f& e0 = g_eyePose[0].position;
        const XrVector3f& e1 = g_eyePose[1].position;
        XrVector3f head{(e0.x + e1.x) * 0.5f, (e0.y + e1.y) * 0.5f, (e0.z + e1.z) * 0.5f};
        XrVector3f ears = sub3(e1, e0);
        ears = dot3(ears, ears) > 1e-6f ? normalize3(ears) : rotate3(g_eyePose[0].orientation, {1, 0, 0});
        XrVector3f ahead = rotate3(g_eyePose[0].orientation, {0, 0, -1});
        XrVector3f to = sub3(at, head);
        float d = std::sqrt(dot3(to, to));
        float pan = 0, front = 1;
        if (d > 1e-3f) {
            pan = std::fmax(-1.0f, std::fmin(1.0f, dot3(to, ears) / d));
            front = dot3(to, ahead) / d;
        }
        float g = 1.0f / (1.0f + std::fmax(0.0f, d - 0.5f) * 0.7f);
        if (front < 0) g *= 1.0f + 0.25f * front;
        float side = 1.0f + 0.8f * std::fabs(pan);
        simsforever::setEars(g * std::sqrt((1.0f - 0.8f * pan) / side), g * std::sqrt((1.0f + 0.8f * pan) / side));
    }
}

// How far the screen's glass is behind the panel's plane at u v of the picture
// (metres, <= 0; a curved screen's shape, simsforever::depth).
static float sims1Depth(float u, float v) {
    if (!(g_sims1.faceW > 0 && g_sims1.faceH > 0)) return 0;
    float fx = 0.5f + (u - 0.5f) * g_sims1.width / g_sims1.faceW;
    float fy = 0.5f + (0.5f - v) * g_sims1.height / g_sims1.faceH;
    return simsforever::depth(fx, fy) / g_sims1.scale;
}

// Where a point is on Sims 1's picture: u v 0..1 across it, lz metres in front of the glass.
static void sims1Local(const XrVector3f& p, float& u, float& v, float& lz) {
    XrVector3f rel = sub3(p, g_sims1.at);
    u = dot3(rel, g_sims1.x) / g_sims1.width + 0.5f;
    v = 0.5f - dot3(rel, g_sims1.y) / g_sims1.height;
    lz = dot3(rel, g_sims1.z) - sims1Depth(u, v);
}

static bool hitSims1(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    if (!g_sims1.active) return false;
    float facing = dot3(d, g_sims1.z);
    if (facing > -0.01f) return false;
    // The plane, then the glass's depth where it hit, a few times (curved screens).
    float shift = 0, lz = 0;
    for (int i = 0; i < 4; ++i) {
        float t = (shift - dot3(sub3(o, g_sims1.at), g_sims1.z)) / facing;
        if (t <= 0) return false;
        hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
        sims1Local(hit, u, v, lz);
        shift = sims1Depth(u, v);
    }
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

static void sims1Mouse(float u, float v, int buttons) {
    g_sims1.u = u;
    g_sims1.v = v;
    g_sims1.buttons = buttons;
    g_sims1.used = true;
    simsforever::mouse(u, v, buttons);
}

// A finger on Sims 1's picture: touching it presses the mouse there, as on the
// pie (from in front, let go when pulled back); that hand's trigger clicks too.
static bool pokeSims1(const armik::Hand hands[2], const bool trigger[2], bool rightButton) {
    if (!g_sims1.active) return false;
    int best = -1;
    float bestZ = 1e9f, bu = 0, bv = 0, bz = 0;
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid) {
            g_sims1.armed[i] = false;
            continue;
        }
        float u, v, lz;
        sims1Local(fingertip(hands[i]), u, v, lz);
        const float mu = 0.02f / g_sims1.width, mv = 0.02f / g_sims1.height;
        bool inRange = u > -mu && u < 1 + mu && v > -mv && v < 1 + mv && lz < kPokeHover && lz > -kPokeThrough;
        if (!inRange) {
            g_sims1.armed[i] = false;
            continue;
        }
        if (lz > 0.005f) g_sims1.armed[i] = true;
        float rank = i == g_sims1.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = i;
            bu = u;
            bv = v;
            bz = lz;
        }
    }
    if (best < 0) {
        if (g_sims1.pressing >= 0 || g_sims1.buttons) sims1Mouse(g_sims1.u, g_sims1.v, 0);
        g_sims1.pressing = -1;
        return false;
    }
    if (g_sims1.pressing == best) {
        if (bz > kPokeRelease) g_sims1.pressing = -1;
    } else if (bz <= 0 && g_sims1.armed[best]) {
        g_sims1.pressing = best;
        g_sims1.armed[best] = false;
    }
    if (g_sims1.pressing != best && g_sims1.pressing >= 0) g_sims1.pressing = -1;
    bu = std::max(0.0f, std::min(1.0f, bu));
    bv = std::max(0.0f, std::min(1.0f, bv));
    bool left = g_sims1.pressing == best || trigger[best];
    sims1Mouse(bu, bv, (left ? 1 : 0) | (rightButton ? 2 : 0));
    const float ox = (bu - 0.5f) * g_sims1.width, oy = (0.5f - bv) * g_sims1.height, oz = sims1Depth(bu, bv);
    XrVector3f onIt{g_sims1.at.x + g_sims1.x.x * ox + g_sims1.y.x * oy + g_sims1.z.x * oz,
                    g_sims1.at.y + g_sims1.x.y * ox + g_sims1.y.y * oy + g_sims1.z.y * oz,
                    g_sims1.at.z + g_sims1.x.z * ox + g_sims1.y.z * oy + g_sims1.z.z * oz};
    g_laserTo = onIt;
    g_laserFrom = {onIt.x + g_sims1.z.x * 0.1f, onIt.y + g_sims1.z.y * 0.1f, onIt.z + g_sims1.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}

