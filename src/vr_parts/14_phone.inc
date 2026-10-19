// A hand down at your right hip (head-relative, so it works seated or
// standing and wherever you face). The left hand reaches across for it.
static bool atRightHip(const armik::Hand& hand, int side, const grab::HeadPose& head) {
    XrQuaternionf q{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
    XrVector3f right = rotate3(q, {1, 0, 0});  // stays level while you look down at your hip
    float len = std::sqrt(right.x * right.x + right.z * right.z);
    if (len < 0.2f) return false;
    right = {right.x / len, 0, right.z / len};
    XrVector3f forward{right.z, 0, -right.x};
    XrVector3f rel{hand.position.x - head.position.x, hand.position.y - head.position.y,
                   hand.position.z - head.position.z};
    float down = -rel.y, toRight = dot3(rel, right), ahead = dot3(rel, forward);
    bool hip = down > g_config.phoneHipDrop && toRight > 0.05f && ahead > -0.35f && ahead < 0.30f;
    static int logged = 0;
    if (logged++ < 20)
        LOG("Phone: %s hand closed %.2f m below your eyes, %.2f m right, %.2f m ahead: %s", side ? "right" : "left",
            down, toRight, ahead, hip ? "takes the phone out" : "not at the hip");
    return hip;
}

static void phoneOpen(int hand) {
    g_phoneOut = true;
    g_phoneHand = hand;
    g_phoneAwayAt = 0;
    g_tablet = g_buildBuy && g_config.tablet;
    g_phone.solid = g_tablet;
    g_phone.tiles = 0;
    g_tabletScan = TabletScan{};
    g_tabletLeftSeen = g_tabletGridSeen = 0;
    g_tabletMiddleRuns = 0;
    if (g_tablet) {
        // Build/buy: the tablet shows the UI as it is; no Escape, no game menu,
        // nothing for Python (the game is paused in build/buy anyway).
        hideWrist();
        if (g_pie.active) dismissPie("tablet out");
        g_phoneFitted = true;
        buzz(hand, 0.4f, 0.05f);
        static int logged = 0;
        if (logged++ < 30) LOG("Tablet: out in the %s hand (build/buy)", hand ? "right" : "left");
        return;
    }
    grab::pushControl(3);
    hideWrist();
    if (g_pie.active) dismissPie("phone out");
    // The game's phone menu shows (Python sends it); a game menu already open
    // shows as the Options app. No Escape: the menu opens from Options.
    g_phoneOptions = gameMenuOpen() && !g_popupOpen;
    g_phoneOptionsSeen = g_phoneOptions;
    hud::phoneHome();
    g_phoneFitted = false;
    g_phoneMenuSince = GetTickCount64();
    buzz(hand, 0.4f, 0.05f);
    static int logged = 0;
    if (logged++ < 30)
        LOG("Phone: out in the %s hand (%s)", hand ? "right" : "left",
            g_phoneOptions ? "the game menu was open: Options" : "the game's phone menu");
}

static void phoneClose(const char* why) {
    if (!g_phoneOut) return;
    g_phoneOut = false;
    g_phoneMenuAt = 0;
    if (g_tablet) {
        g_tablet = false;
        g_phone.solid = false;
        g_phone.tiles = 0;
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        static int logged = 0;
        if (logged++ < 30) LOG("Tablet: away (%s)", why);
        return;
    }
    hideHud(g_phoneSheet);
    if (g_phoneOptions && gameMenuOpen() && !g_popupOpen) {
        // Close the menu first, so it doesn't pause the game for a moment.
        // (Not with a popup up: Escape would answer it for you.)
        g_escapeFrames = 3;
        g_phoneAwayAt = GetTickCount64() + 250;
    } else {
        grab::pushControl(4);
    }
    g_phoneOpenedMenu = false;
    g_phoneOptions = g_phoneOptionsSeen = false;
    g_phone.active = false;
    g_phone.armed[0] = g_phone.armed[1] = false;
    g_phone.pressing = -1;
    static int logged = 0;
    if (logged++ < 30) LOG("Phone: away (%s)", why);
}

// Our phone in your hand, held like a real one: it lies in the palm, its
// screen facing where the palm faces, its top toward the thumb. The frame is
// the Sim's hand (the grip pose tilted down by hand_pitch, as armik poses it),
// so with the fingers pointing left the phone stands upright. [phone] sets it
// for the right hand; in the left it sits as the mirror image (the grip
// frames of the two hands mirror each other across X). The phone's
// body is our own picture (phoneBodyLayer); the menu sits inside its screen,
// as large as fits without stretching.
static XrPosef g_phoneBodyPose{};

// The phone where phone3d draws it: in the Sim's hand (armik's latest pose),
// the same frame and offsets as phone3d::draw, in app space. The controller's
// frame (updatePhone's fallback) sits a few cm off the Sim's hand, so the
// laser and pokes would miss what you see (user 2026-10-18 21:22Z).
static bool simPhoneFrame(int side, const float o[3], float tilt, float yaw, float pitch, XrVector3f& centre,
                          XrVector3f& x, XrVector3f& y, XrVector3f& z) {
    camera::View view;
    camera::Vec3 wrist;
    camera::Quat q;
    float handScale = 1;
    if (!camera::lastView(view) || !armik::handPose(side, wrist, q, handScale)) return false;
    const XrQuaternionf grip{q.x, q.y, q.z, q.w};
    const float m = side ? 1.0f : -1.0f;
    XrVector3f w = rotate3(grip, {(o[0] - 0.02f * m) * handScale, o[1] * handScale, (o[2] - 0.08f) * handScale});
    const float c[3] = {wrist.x + w.x, wrist.y + w.y, wrist.z + w.z};
    XrVector3f ax = rotate3(grip, {0, 0, m}), ay = rotate3(grip, {0, 1, 0}), az = rotate3(grip, {-m, 0, 0});
    const float ct = std::cos(tilt), st = std::sin(tilt);
    XrVector3f ay2{ay.x * ct - az.x * st, ay.y * ct - az.y * st, ay.z * ct - az.z * st};
    XrVector3f az2{ay.x * st + az.x * ct, ay.y * st + az.y * ct, ay.z * st + az.z * ct};
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    XrVector3f ax3{ax.x * cy - az2.x * sy, ax.y * cy - az2.y * sy, ax.z * cy - az2.z * sy};
    XrVector3f az3{ax.x * sy + az2.x * cy, ax.y * sy + az2.y * cy, ax.z * sy + az2.z * cy};
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    XrVector3f ay3{ay2.x * cp - az3.x * sp, ay2.y * cp - az3.y * sp, ay2.z * cp - az3.z * sp};
    XrVector3f az4{ay2.x * sp + az3.x * cp, ay2.y * sp + az3.y * cp, ay2.z * sp + az3.z * cp};
    const float dx[3] = {ax3.x, ax3.y, ax3.z}, dy[3] = {ay3.x, ay3.y, ay3.z}, dz[3] = {az4.x, az4.y, az4.z};
    centre = worldToApp(view, c, false);
    x = normalize3(worldToApp(view, dx, true));
    y = normalize3(worldToApp(view, dy, true));
    z = normalize3(worldToApp(view, dz, true));
    return true;
}

static void updatePhone(const armik::Hand& hand, int side) {
    // The game's phone menu (drawn by hud.cpp), or the UI's game menu (Options) / the tablet.
    const bool app = g_phoneOut && !g_tablet && (!g_phoneOptions || g_optionsFront);
    if (!g_phoneOut || !hand.valid || (!app && !g_uiFrame)) {
        phone3d::setPhone(phone3d::Phone{}, nullptr);
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        hideHud(g_phoneSheet);
        return;
    }
    D3D11_TEXTURE2D_DESC ui{};
    if (app) {
        ui.Width = hud::kWidth;
        ui.Height = hud::kHeight;
    } else {
        g_uiFrame->GetDesc(&ui);
    }
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t crop[4];
    defaultPhoneCrop(W, H, crop);
    if (g_phoneFound) std::memcpy(crop, g_phoneCrop, sizeof(crop));
    if (app) {
        crop[0] = static_cast<int32_t>(hud::kPhone.x);
        crop[1] = static_cast<int32_t>(hud::kPhone.y);
        crop[2] = static_cast<int32_t>(hud::kPhone.w);
        crop[3] = static_cast<int32_t>(hud::kPhone.h);
    }
    float tileDst[phone3d::kMaxTiles][4], tileSrc[phone3d::kMaxTiles][4];
    int tiles = 0;
    if (g_tablet) {  // the tablet: the build/buy UI's pieces packed together, or all of it
        crop[0] = crop[1] = 0;
        crop[2] = W;
        crop[3] = H;
        tiles = tabletLayout(W, H, tileDst, tileSrc, g_tabletAspect);
    }
    crop[2] = std::min(crop[2], W);
    crop[3] = std::min(crop[3], H);
    crop[0] = std::max(0, std::min(W - crop[2], crop[0]));
    crop[1] = std::max(0, std::min(H - crop[3], crop[1]));
    // The Sim's hand: the grip turned down by hand_pitch about its X.
    float half = -g_config.handPitch * 0.5f * 3.14159265f / 180.0f;
    float hs = std::sin(half), hc = std::cos(half);
    const camera::Quat& q = hand.orientation;
    XrQuaternionf grip{q.w * hs + q.x * hc, q.y * hc + q.z * hs, q.z * hc - q.y * hs, q.w * hc - q.x * hs};
    XrVector3f gx = rotate3(grip, {1, 0, 0}), gy = rotate3(grip, {0, 1, 0}), gz = rotate3(grip, {0, 0, 1});
    // The right palm faces -X; the fingers point along -Z, so +Z is the phone's
    // right. The left hand mirrors it (m = -1): its palm faces +X.
    const float m = side ? 1.0f : -1.0f;
    XrVector3f x{gz.x * m, gz.y * m, gz.z * m}, y = gy, z{-gx.x * m, -gx.y * m, -gx.z * m};
    float tilt = g_config.phoneTilt * 0.0174533f, c = std::cos(tilt), s = std::sin(tilt);
    XrVector3f y2{y.x * c - z.x * s, y.y * c - z.y * s, y.z * c - z.z * s};
    XrVector3f z2{y.x * s + z.x * c, y.y * s + z.y * c, y.z * s + z.z * c};
    turnPhone(x, z2, m);
    pitchPhone(y2, z2);
    float o[3] = {g_config.phoneOffset[0] * m, g_config.phoneOffset[1], g_config.phoneOffset[2]};
    float b = g_tablet ? g_config.tabletBezel : g_config.phoneBezel, w, h, depth;
    phoneSize(w, h, depth);
    if (g_tablet) {
        // The hand holds the tablet near its outer side edge (the right hand
        // its right edge), so the tablet reaches across in front of you:
        // its centre moves along its own width, in the hand's grip frame.
        float k = -m * std::max(0.0f, w / 2 - g_config.tabletHold);
        XrVector3f shift{x.x * k, x.y * k, x.z * k};
        o[0] += dot3(shift, gx);
        o[1] += dot3(shift, gy);
        o[2] += dot3(shift, gz);
    }
    XrVector3f centre{hand.position.x + gx.x * o[0] + gy.x * o[1] + gz.x * o[2],
                      hand.position.y + gx.y * o[0] + gy.y * o[1] + gz.y * o[2],
                      hand.position.z + gx.z * o[0] + gy.z * o[1] + gz.z * o[2]};
    {
        // Where it is drawn: in the Sim's hand (phone3d), so the laser and pokes hit what you see.
        XrVector3f sc, sx, sy, sz;
        static bool logged = false;
        if (simPhoneFrame(side, o, tilt, m * g_config.phoneYaw * 0.0174533f, g_config.phonePitch * 0.0174533f, sc, sx, sy,
                          sz)) {
            if (!logged) {
                logged = true;
                XrVector3f off = sub3(sc, centre);
                LOG("Phone: placed on the Sim's hand for the laser and pokes, %.1f cm from the controller's place",
                    std::sqrt(dot3(off, off)) * 100);
            }
            centre = sc;
            x = sx;
            y2 = sy;
            z2 = sz;
        }
    }
    XrQuaternionf facing = fromAxes(x, y2, z2);
    g_phoneBodyPose = {facing, centre};

    // The screen: the body less the bezel, with a wider band at the bottom and
    // top (not on the tablet). The menu fills its width (or its height, if the menu is the taller).
    float screenW = w - 2 * b, screenH = h - 2 * b - (g_tablet ? 0 : kPhoneChin * h * 2);
    float aspect = tiles ? g_tabletAspect : static_cast<float>(crop[2]) / static_cast<float>(crop[3]);
    float menuW = screenW, menuH = screenW / aspect;
    if (menuH > screenH) {
        menuH = screenH;
        menuW = screenH * aspect;
    }
    // A little above the middle of the screen, on the glass (the game's phone menu: in the middle).
    float up = g_tablet || app ? 0 : (screenH - menuH) * 0.25f, glass = depth / 2 + 0.001f;
    XrVector3f menuCentre{centre.x + y2.x * up + z2.x * glass, centre.y + y2.y * up + z2.y * glass,
                          centre.z + y2.z * up + z2.z * glass};
    // The same phone in 3D, in the game's view (phone3d), when it can be drawn there.
    phone3d::Phone p3;
    p3.show = true;
    const XrVector3f* axes[3] = {&x, &y2, &z2};
    p3.centre[0] = centre.x;
    p3.centre[1] = centre.y;
    p3.centre[2] = centre.z;
    float* out[3] = {p3.x, p3.y, p3.z};
    for (int i = 0; i < 3; ++i) {
        out[i][0] = axes[i]->x;
        out[i][1] = axes[i]->y;
        out[i][2] = axes[i]->z;
    }
    p3.grip[0] = o[0];
    p3.grip[1] = o[1];
    p3.grip[2] = o[2];
    p3.side = side;
    p3.tilt = tilt;
    p3.yaw = m * g_config.phoneYaw * 0.0174533f;
    p3.pitch = g_config.phonePitch * 0.0174533f;
    p3.width = w;
    p3.height = h;
    p3.depth = depth;
    p3.radius = g_tablet ? std::min(w, h) * 0.05f : w * 0.14f;
    p3.screen[0] = 0;
    p3.screen[1] = 0;
    p3.screen[2] = screenW;
    p3.screen[3] = screenH;
    p3.menu[0] = 0;
    p3.menu[1] = up;
    p3.menu[2] = menuW;
    p3.menu[3] = menuH;
    p3.crop[0] = static_cast<float>(crop[0]) / W;
    p3.crop[1] = static_cast<float>(crop[1]) / H;
    p3.crop[2] = static_cast<float>(crop[0] + crop[2]) / W;
    p3.crop[3] = static_cast<float>(crop[1] + crop[3]) / H;
    p3.tiles = tiles;
    p3.wallpaper = g_tablet || app;  // the Sim's phone background (buildbuy.py) behind the game's phone menu
    g_phone.tiles = tiles;
    for (int k = 0; k < tiles; ++k) {
        for (int i = 0; i < 4; ++i) {
            p3.tileDst[k][i] = g_phone.tileDst[k][i] = tileDst[k][i];
            g_phone.tileSrc[k][i] = tileSrc[k][i];
            p3.tileSrc[k][i] = tileSrc[k][i] / static_cast<float>(i % 2 ? H : W);
        }
    }
    if (app) {
        // The game's phone menu: our HUD atlas on the glass; pokes and the laser
        // go to its buttons (pokeHud / laserHud), not to the game's UI.
        phone3d::setPhone(p3, hud::texture());
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        PieMenu& m = g_phoneSheet.m;
        if (!m.active) m.opened = GetTickCount64();
        m.active = true;
        m.x = x;
        m.y = y2;
        m.z = z2;
        m.pose = {facing, menuCentre};
        m.width = menuW;
        m.height = menuH;
        m.x0 = crop[0];
        m.y0 = crop[1];
        m.cw = crop[2];
        m.ch = crop[3];
        m.uiW = hud::kWidth;
        m.uiH = hud::kHeight;
        return;
    }
    hideHud(g_phoneSheet);
    phone3d::setPhone(p3, g_uiFrame);
    if (!g_phone.active) g_phone.opened = GetTickCount64();
    g_phone.active = true;
    g_phone.x = x;
    g_phone.y = y2;
    g_phone.z = z2;
    g_phone.pose = {facing, menuCentre};
    g_phone.width = menuW;
    g_phone.height = menuH;
    g_phone.x0 = crop[0];
    g_phone.y0 = crop[1];
    g_phone.cw = crop[2];
    g_phone.ch = crop[3];
    g_phone.uiW = ui.Width;
    g_phone.uiH = ui.Height;
}

// The phone's body: a dark rounded slab with a thin light rim, a black screen
// (rounded too) and a camera dot in the band above it. Made once, at the
// proportions in sims4vr.ini.
static bool phoneBodyLayer(ID3D11Device* device, XrCompositionLayerQuad& layer) {
    const float w = g_config.phoneWidth, h = g_config.phoneHeight, b = g_config.phoneBezel;
    const uint32_t texH = 512, texW = std::max<uint32_t>(64, static_cast<uint32_t>(texH * w / h + 0.5f));
    bool ok = ensureLayerImage(g_phoneBodyImage, device, texW, texH, [w, h, b](float u, float v, float* c) {
        // In metres from the centre: px right, py down.
        float px = (u - 0.5f) * w, py = (v - 0.5f) * h;
        auto roundedBox = [](float x, float y, float hw, float hh, float r) {  // signed distance, metres
            float qx = std::fabs(x) - (hw - r), qy = std::fabs(y) - (hh - r);
            float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
        };
        float pixel = h / 512.0f;
        float body = roundedBox(px, py, w / 2, h / 2, w * 0.14f);
        float a = std::max(0.0f, std::min(1.0f, 0.5f - body / pixel));
        float chin = kPhoneChin * h;
        float screen = roundedBox(px, py, w / 2 - b, h / 2 - b - chin, w * 0.14f - b);
        float inScreen = std::max(0.0f, std::min(1.0f, 0.5f - screen / pixel));
        float rim = body > -0.0012f ? 1.0f : 0.0f;
        float frame = rim ? 0.32f : 0.07f;
        float cam = std::sqrt(px * px + (py + h / 2 - b - chin / 2) * (py + h / 2 - b - chin / 2));
        if (cam < chin * 0.22f) frame = 0.015f;
        float shade = frame + (0.012f - frame) * inScreen;
        c[0] = shade * a;
        c[1] = shade * a;
        c[2] = (shade + (rim ? 0.03f : 0.01f) * (1 - inScreen)) * a;
        c[3] = a;
    });
    if (!ok || !g_phoneBodyImage.source) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ok = submitLayerImage(g_phoneBodyImage, context);
    context->Release();
    if (!ok) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_phoneBodyImage.swapchain);
    layer.pose = g_phoneBodyPose;
    layer.size = {w, h};
    return true;
}

// Where the laser lands. Stereo: on the floor the game camera looks at (or
// far away), then through the game's own camera into a window position, since
// that camera is what the game picks with. Quad: on the virtual screen.
