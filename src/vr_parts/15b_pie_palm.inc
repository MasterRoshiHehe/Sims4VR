// ---------------------------------------------------------------------------
// The pie on your palm (user 2026-10-23 17:25Z, [ui] pie_mode): you click with one
// hand, the other buzzes, and turning that palm up shows the pie over it, facing
// you; the other hand picks with the laser or a poke. A fist on that hand (grip and
// trigger) hides it with a buzz; letting go closes it, thrown away (as the shove
// throws it) when the hand moved fast. The other hand still wipes it away.
// pie_mode 1: always on the palm; 2 (dynamic): in front, but your own pie asked
// for with the laser on a palm opens on that palm. First person only (the pie
// at hand is). Only the pie drawn by us; the game's cut stays in front.

// Right after openPie: which palm, if any, the pie goes to.
static void choosePiePalm(const armik::Hand hands[2]) {
    g_piePalm = -1;
    g_piePalmUp = g_piePalmFist = false;
    const ULONGLONG now = GetTickCount64();
    int palm = -1;
    if (g_config.pieMode == 1)
        palm = clickHand() == 0 ? 1 : 0;  // the hand that didn't click (left when unknown)
    else if (g_config.pieMode == 2 && g_piePalmNext >= 0 && now - g_piePalmNextAt < 3000)
        palm = g_piePalmNext;
    g_piePalmNext = -1;
    if (palm < 0) return;
    const char* why = !g_pie.own                             ? "the game's pie (cut out) stays in front"
                      : !hands[palm].valid                   ? "that hand isn't tracked"
                      : g_phoneOut && g_phoneHand == palm    ? "the phone is in that hand"
                      : grab::holding(palm) || armik::holding(palm) ? "that hand holds something"
                                                             : nullptr;
    static int logged = 0;
    if (why) {
        if (logged++ < 30) LOG("Pie menu at hand: in front of you, not on your palm (%s)", why);
        return;
    }
    g_piePalm = palm;
    buzz(palm, 0.3f, 0.05f);
    if (logged++ < 30) LOG("Pie menu at hand: on your %s palm (turn it up to see it)", palm ? "right" : "left");
}

// Every frame (updateControls): the pie over its palm, shown while the palm faces up; the fist closing it.
static void updatePiePalm(XrTime time, const armik::Hand hands[2]) {
    if (!g_pie.active || g_piePalm < 0) return;
    if (!g_pie.own) {
        g_piePalm = -1;  // its texts weren't found: the game's cut, which stays where it is
        return;
    }
    const int p = g_piePalm;
    const ULONGLONG now = GetTickCount64();
    // The fist: grip and trigger together hide it (closed, a buzz); letting go closes it, thrown if the hand
    // moved fast.
    const float grip = floatState(g_gripAction, p), trigger = floatState(g_triggerAction, p);
    if (!g_piePalmFist) {
        if (grip > 0.6f && trigger > 0.6f && pieSettled()) {
            g_piePalmFist = true;
            g_piePalmFistAt = now;
            g_pieDismissHold = true;  // the trigger clicks nothing
            buzz(p, 0.35f, 0.05f);
            static int logged = 0;
            if (logged++ < 20) LOG("Pie menu at hand: fist on your %s hand, closed", p ? "right" : "left");
        }
    } else if (grip < 0.35f || trigger < 0.35f || now - g_piePalmFistAt > 10000) {
        const XrVector3f v = g_handVelocity[p];
        const bool thrown = dot3(v, v) > 1.2f * 1.2f;
        if (thrown) flingPie(v);
        static int logged = 0;
        if (logged++ < 20)
            LOG("Pie menu at hand: fist let go%s (%.1f m/s)", thrown ? ", thrown away" : "", std::sqrt(dot3(v, v)));
        dismissPie(thrown ? "thrown from your palm" : "fist on your palm");
        return;
    }
    handshape::Shape shape;
    XrPosef head;
    if (!hands[p].valid || !handshape::get(p, hands[p], shape) || !locateHead(time, head)) {
        g_piePalmUp = false;
        return;
    }
    const XrVector3f n{shape.palmNormal.x, shape.palmNormal.y, shape.palmNormal.z};
    const bool upWas = g_piePalmUp;
    g_piePalmUp = upWas ? n.y > 0.35f : n.y > 0.6f;
    if (g_piePalmUp != upWas) {
        static int logged = 0;
        if (logged++ < 30) LOG("Pie menu at hand: palm %s, pie %s", g_piePalmUp ? "up" : "turned away", g_piePalmUp ? "shown" : "hidden");
    }
    // Over the palm, facing your eyes, level.
    const XrVector3f centre{shape.palmCentre.x, shape.palmCentre.y + g_config.piePalmLift, shape.palmCentre.z};
    XrVector3f z = sub3(head.position, centre);
    if (!(dot3(z, z) > 1e-4f)) return;
    z = normalize3(z);
    const XrVector3f x = std::fabs(z.y) > 0.99f ? XrVector3f{1, 0, 0} : normalize3(cross3(XrVector3f{0, 1, 0}, z));
    const XrVector3f y = cross3(z, x);
    g_pie.x = x;
    g_pie.y = y;
    g_pie.z = z;
    g_pie.pose = {fromAxes(x, y, z), centre};
}
