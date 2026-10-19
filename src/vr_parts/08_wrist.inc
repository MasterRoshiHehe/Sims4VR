// ---------------------------------------------------------------------------
// The menus on your left wrist (first person, [ui] first_person = 1): turn the
// inside of your left wrist toward your face and the whole UI shows as a small
// screen over that hand, facing you; point at it with the right hand's laser
// or touch it with your right index finger. Lower the hand and it goes away.
static PieMenu g_wrist;

// The full panel in front of you: always outside first person, and in it with [ui] first_person = 2.
// A window the phone's Options app opened (the game options, Save, Exit...):
// too big for the phone, so the game's UI shows in front of you meanwhile and
// the phone keeps its apps (user 2026-10-18 21:55Z).
static bool g_optionsFront = false;
static bool frontPanelShown() { return !camera::bodyActive() || g_config.uiFirstPerson >= 2 || g_optionsFront; }

static void hideWrist() {
    g_wrist.active = false;
    g_wrist.armed[0] = g_wrist.armed[1] = false;
    g_wrist.pressing = -1;
}

static void updateWrist(const armik::Hand& left, const XrPosef& head, bool allowed) {
    if (!allowed || !left.valid || !g_uiFrame) {
        hideWrist();
        return;
    }
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    const camera::Quat& q = left.orientation;
    XrQuaternionf grip{q.x, q.y, q.z, q.w};
    // The left palm faces the grip pose's +X.
    XrVector3f palm = rotate3(grip, {1, 0, 0});
    XrVector3f p{left.position.x, left.position.y, left.position.z};
    XrVector3f toHead = sub3(head.position, p);
    float distance = std::sqrt(dot3(toHead, toHead));
    if (!(distance > 0.05f)) {
        hideWrist();
        return;
    }
    toHead = {toHead.x / distance, toHead.y / distance, toHead.z / distance};
    XrVector3f look = forwardOf(head.orientation);
    float facing = dot3(palm, toHead);         // the palm turned toward your face
    float inView = -dot3(look, toHead);        // the hand ahead of your eyes
    bool show = g_wrist.active ? facing > 0.25f && inView > 0.55f && distance < 0.85f
                               : facing > 0.5f && inView > 0.75f && distance < 0.7f;
    if (show && !g_wrist.active) {
        static int logged = 0;
        if (logged++ < 10) LOG("Wrist menus: shown (palm %.2f toward you, %.2f m away)", facing, distance);
    }
    if (!show) {
        hideWrist();
        return;
    }
    g_wrist.active = true;
    // A screen just over the palm, facing your eyes, upright.
    float width = g_config.wristWidth, height = width * ui.Height / ui.Width;
    XrVector3f centre{p.x + palm.x * 0.05f, p.y + palm.y * 0.05f + height * 0.5f + 0.03f, p.z + palm.z * 0.05f};
    XrVector3f z = normalize3(sub3(head.position, centre));
    XrVector3f x = std::fabs(z.y) > 0.99f ? XrVector3f{1, 0, 0} : normalize3(cross3(XrVector3f{0, 1, 0}, z));
    XrVector3f y = cross3(z, x);
    g_wrist.x = x;
    g_wrist.y = y;
    g_wrist.z = z;
    g_wrist.pose = {fromAxes(x, y, z), centre};
    g_wrist.width = width;
    g_wrist.height = height;
    g_wrist.x0 = 0;
    g_wrist.y0 = 0;
    g_wrist.cw = static_cast<int32_t>(ui.Width);
    g_wrist.ch = static_cast<int32_t>(ui.Height);
    g_wrist.uiW = ui.Width;
    g_wrist.uiH = ui.Height;
}

