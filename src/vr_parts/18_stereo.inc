// ---------------------------------------------------------------------------
// SteamVR: the older eye turned to the newest head pose (eyealign.h).

static bool g_alignBroken = false;  // a turned image couldn't be drawn: off for this session
static bool alignEyes() { return g_steamVR && g_config.alignEyes && !g_alignBroken; }
// Your body on its own layer: only the turn smoothing needs it, and not with
// SteamVR's eye align (which redraws the older eye).
static bool bodyLayerOn() { return g_config.bodyLayer && g_config.turnReprojection && !alignEyes(); }

static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static XrQuaternionf qconj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

static XrQuaternionf qnorm(const XrQuaternionf& q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? XrQuaternionf{q.x / n, q.y / n, q.z / n, q.w / n} : XrQuaternionf{0, 0, 0, 1};
}

static XrVector3f qrot(const XrQuaternionf& q, const XrVector3f& v) {
    XrQuaternionf r = qmul(qmul(q, {v.x, v.y, v.z, 0}), qconj(q));
    return {r.x, r.y, r.z};
}

// Row-major rotation matrix of a unit quaternion.
static void qmatrix(const XrQuaternionf& q, float m[9]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}

// `fresh` was just copied: the other eye's kept image is drawn into its
// swapchain turned to where that eye is now relative to the fresh eye's
// render pose (from this frame's located views), so both views carry one
// head pose, and its pose and heading become the fresh eye's.
static void alignOlderEye(int fresh, const XrView (&located)[2]) {
    const int old = 1 - fresh;
    if (!eyealign::has(old) || !shownChain(old).handle) return;
    const XrPosef& from = g_eyePose[fresh];
    const XrPosef &lf = located[fresh].pose, &lo = located[old].pose;
    XrPosef target;
    target.orientation = qnorm(qmul(from.orientation, qmul(qconj(lf.orientation), lo.orientation)));
    const XrVector3f apart = qrot(qconj(lf.orientation), {lo.position.x - lf.position.x, lo.position.y - lf.position.y,
                                                          lo.position.z - lf.position.z});
    const XrVector3f offset = qrot(from.orientation, apart);
    target.position = {from.position.x + offset.x, from.position.y + offset.y, from.position.z + offset.z};

    // The old image as if rendered at the fresh one's heading in the world
    // (a stick turn in between turns the world, like the turn reprojection).
    XrQuaternionf was = g_eyePose[old].orientation;
    float turn = g_config.turnReprojection && std::isfinite(g_eyeYaw[old]) && std::isfinite(g_eyeYaw[fresh])
                     ? std::remainder(g_eyeYaw[old] - g_eyeYaw[fresh], 6.2831853f)
                     : 0.0f;
    if (std::fabs(turn) > 1e-5f && std::fabs(turn) < 0.5f)
        was = qmul({0, std::sin(turn / 2), 0, std::cos(turn / 2)}, was);
    else
        turn = 0;
    float rotation[9];
    qmatrix(qnorm(qmul(qconj(was), target.orientation)), rotation);
    const XrFovf& f = g_eyeFov[old];
    const float tangents[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};

    Swapchain& s = shownChain(old);
    uint32_t index = 0;
    if (!acquireImage(s, index)) return;
    bool drawn = eyealign::draw(old, s.images[index].texture, s.format, rotation, tangents);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    bool released = check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
    // The image released now is what the swapchain shows: turned, or (if the
    // draw failed) whatever that image held before, so the pose that goes
    // with it is unknown: leave the eye out until it is rendered again.
    if (!released) return;
    if (!drawn) {
        g_eyeValid[old] = false;
        eyealign::forget(old);
        g_alignBroken = true;
        LOG("Eye align: couldn't draw the turned eye; off");
        return;
    }
    g_shownPose[old] = target;
    g_shownYaw[old] = g_eyeYaw[fresh];

    static int logs = 0;
    static ULONGLONG lastLog = 0;
    if (logs < 10 && GetTickCount64() - lastLog > 30000) {
        ++logs;
        lastLog = GetTickCount64();
        XrQuaternionf moved = qmul(qconj(g_eyePose[old].orientation), target.orientation);
        float angle = 2 * std::acos(std::min(1.0f, std::fabs(moved.w))) * 57.29578f;
        LOG("Eye align: the %s eye turned %.2f degrees to the newest head pose (turn %.2f)", old ? "right" : "left",
            angle, turn * 57.29578f);
    }
}

// The stick turn check: each frame while the right stick turns, how far the
// view heading the eyes are corrected to moved on, and how far the eyes were
// turned back; a line every 2 s of turning (and when you let go).
static void turnCheck(float nowYaw, const float (&turned)[2], bool newPicture) {
    TurnCheck& c = g_turnCheck;
    auto flush = [&] {
        if (c.frames >= 20 && g_turnCheckLogs < 60) {
            ++g_turnCheckLogs;
            const double seconds = double(GetTickCount64() - c.start) / 1000.0;
            LOG("Turning check: %.1f s, %d frames (%d refreshes missed), %d new pictures; display clock steps %.2f to "
                "%.2f ms (%.2f on average); the view turned %.2f to %.2f degrees a frame (%.2f on average); eyes "
                "turned back %.2f to %.2f degrees; drawn up to %.1f degrees ahead (%.0f ms)",
                seconds, c.frames, c.missed, c.pictures, c.steps ? c.stepMin : 0.0, c.steps ? c.stepMax : 0.0,
                c.steps ? c.stepSum / c.steps : 0.0, c.turns ? c.turnMin : 0.0, c.turns ? c.turnMax : 0.0,
                c.turns ? c.turnSum / c.turns : 0.0, c.frames && c.backMin < 1e8f ? c.backMin : 0.0f,
                c.frames && c.backMax > -1e8f ? c.backMax : 0.0f, c.leadMax, g_turnLatency * 1000);
        }
        c = TurnCheck{};
    };
    if (!g_turnChecking || !std::isfinite(nowYaw)) {
        flush();
        return;
    }
    if (!c.start) c.start = GetTickCount64();
    ++c.frames;
    if (newPicture) ++c.pictures;
    if (std::isfinite(c.lastHeading)) {
        const double step = std::fabs(std::remainder(nowYaw - c.lastHeading, 6.2831853f)) * 57.29578;
        c.turnMin = std::min(c.turnMin, step);
        c.turnMax = std::max(c.turnMax, step);
        c.turnSum += step;
        ++c.turns;
    }
    c.lastHeading = nowYaw;
    for (float back : turned) {
        if (back == 0) continue;
        c.backMin = std::min(c.backMin, std::fabs(back) * 57.29578f);
        c.backMax = std::max(c.backMax, std::fabs(back) * 57.29578f);
    }
    if (GetTickCount64() - c.start >= 2000) {
        const float heading = c.lastHeading;
        flush();
        c.start = GetTickCount64();
        c.lastHeading = heading;
    }
}

static bool renderStereo(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, XrTime time,
                         XrCompositionLayerProjection& layer, XrCompositionLayerProjectionView (&views)[2]) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = g_appSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!check(xrLocateViews(g_session, &locate, &viewState, 2, &count, located), "xrLocateViews")) return false;
    computeRenderSize(located);
    bool newPicture = false;  // a new eye picture (or pair) shown from this frame on

    // Turning now, or about to? (kBodyLinger)
    if (bodyLayerOn()) {
        const float yaw = camera::viewYaw();
        if ((std::isfinite(yaw) && std::isfinite(g_lastTurnYaw) &&
             std::fabs(std::remainder(yaw - g_lastTurnYaw, 6.2831853f)) > 1e-4f) ||
            boolState(g_stickTouchAction, 1) || std::fabs(stickState(1).x) > 0.05f)
            g_lastTurnTick = GetTickCount64();
        g_lastTurnYaw = yaw;
    }
    const bool turning = g_lastTurnTick && GetTickCount64() - g_lastTurnTick < kBodyLinger;

    // 1. The backbuffer holds what the camera hook rendered: the oldest
    //    entry, since the game thread can run a frame ahead of Present.
    camera::Rendered rendered;
    if (camera::takeRendered(rendered)) {
        int eye = rendered.eye;
        // Paired eyes: a left eye waits for its right eye (in the left
        // swapchain not shown now).
        const bool hold = eye == 0 && rendered.pairFirst && !alignEyes();
        Swapchain& chain = hold ? heldChain() : shownChain(eye);
        bool recreated;
        if (!ensureSwapchain(chain, desc, recreated)) return false;
        if (recreated && hold) {
            g_leftHeld = false;
        } else if (recreated) {
            g_eyeValid[eye] = false;
            eyealign::forget(eye);
        }
        const camera::Vec3& p = rendered.position;
        const camera::Quat& q = rendered.orientation;
        g_overlayPose = {{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}};
        g_overlayFov = {rendered.fov.left, rendered.fov.right, rendered.fov.up, rendered.fov.down};
        g_overlayEye = true;
        g_captureSlot = eye;  // screenshot::kLeftEye / kRightEye
        g_keepEye = alignEyes() ? eye : -1;
        const int slot = chainSlot(chain);
        // Not with the phone or the pie drawn into the view at your hand: they
        // are drawn after the depth that finds your body, so only the pieces
        // in front of your hand would go with it.
        const bool handUi = g_pie.active || ((g_phone.active || g_phoneSheet.m.active) && phone3d::drawnRecently());
        g_splitSlot = bodyLayerOn() && !handUi ? slot : -1;
        g_splitWanted = turning;
        g_splitFrom = &rendered;
        if (slot >= 0 && g_splitSlot < 0) g_bodyState[slot] = kBodyUnknown;  // no longer goes with it
        bool copied = copyIntoSwapchain(chain, backbuffer, desc);
        g_splitSlot = -1;
        g_splitFrom = nullptr;
        g_keepEye = -1;
        g_captureSlot = -1;
        g_overlayEye = false;
        if (copied && hold) {
            g_heldPose = g_overlayPose;
            g_heldFov = g_overlayFov;
            g_heldYaw = rendered.yaw;
            g_leftHeld = true;
        } else if (hold) {
            g_leftHeld = false;
        } else if (copied) {
            if (eye == 0) g_leftHeld = false;  // shown straight away: nothing waits
            if (eye == 1 && g_leftHeld) {
                // Its left eye: show it now, together with this one.
                g_leftShown ^= 1;
                g_leftHeld = false;
                g_eyePose[0] = g_shownPose[0] = g_heldPose;
                g_eyeFov[0] = g_heldFov;
                g_eyeYaw[0] = g_shownYaw[0] = g_heldYaw;
                g_eyeValid[0] = true;
                if (++g_pairsShown == 1) LOG("Paired eyes: the headset shows both eyes of a pair together");
            }
            newPicture = true;
            g_eyePose[eye] = g_overlayPose;
            g_eyeFov[eye] = g_overlayFov;
            g_eyeYaw[eye] = rendered.yaw;
            g_shownPose[eye] = g_overlayPose;
            g_shownYaw[eye] = rendered.yaw;
            if (eye == 1) g_shownLead1 = rendered.lead;
            g_eyeValid[eye] = true;
            g_lastEye = eye;
            if (alignEyes() && g_eyeValid[1 - eye]) alignOlderEye(eye, located);
        }
        g_frameEye = eye;
        if (!g_ownMirror) steadyMirror(backbuffer, desc, eye);
    }

    // 2. Point the game camera at the other eye for the frames it renders next.
    //    Body mode also wants the headset itself, between the eyes.
    const XrPosef &l = located[0].pose, &r = located[1].pose;
    camera::setHead({(l.position.x + r.position.x) / 2, (l.position.y + r.position.y) / 2,
                     (l.position.z + r.position.z) / 2},
                    {l.orientation.x, l.orientation.y, l.orientation.z, l.orientation.w});
    camera::setPairedEyes(g_config.pairedEyes && !alignEyes());
    camera::setExactFov(g_config.exactFov);
    camera::setPairedHands(g_config.pairedHands);
    camera::setBodyLayer(bodyLayerOn());
    int next = g_renderingEye == 0 ? 1 : 0;
    const XrPosef& p = located[next].pose;
    const XrFovf& f = located[next].fov;
    camera::apply(next, {p.position.x, p.position.y, p.position.z},
                  {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                  {f.angleLeft, f.angleRight, f.angleUp, f.angleDown});
    g_renderingEye = next;

    // 3. Submit once both eyes hold an image. Each view carries the pose its
    //    image was rendered with, so the runtime reprojects it correctly.
    if (!g_eyeValid[0] || !g_eyeValid[1]) return false;
    // A stick turn (or the game camera turning) turns the world, not your
    // head, so the runtime can't see it: the eyes, rendered a frame apart,
    // showed the world at two headings (a blur while turning), and each
    // turned only every other frame. Each eye's pose is turned back by how
    // far the world has turned since it was rendered, about your head, so
    // the runtime reprojects that turn like a head turn, every refresh.
    float nowYaw = g_config.turnReprojection ? camera::viewYaw() : NAN;
    // Turn lead: how long a pair's heading is set before it's shown, from how
    // far the shown right eye trails the turn (its lead taken out), averaged
    // while the stick turns; the next pair is drawn that far ahead.
    {
        const bool leadOn = g_config.turnLead && g_config.turnReprojection && !alignEyes();
        if (leadOn && std::isfinite(nowYaw) && std::isfinite(g_shownYaw[1]) && g_eyeValid[1] &&
            std::fabs(g_stickRate) > 0.3f) {
            const float behind = g_shownLead1 - std::remainder(g_shownYaw[1] - nowYaw, 6.2831853f);
            const float seconds = behind / g_stickRate;
            if (seconds > 0 && seconds < 0.2f) g_turnLatency += (seconds - g_turnLatency) * 0.03f;
        }
        const float lead = leadOn ? std::clamp(g_stickRate * g_turnLatency, -0.3f, 0.3f) : 0.0f;
        camera::setTurnLead(lead);
        if (g_turnChecking) g_turnCheck.leadMax = std::max(g_turnCheck.leadMax, std::fabs(lead) * 57.29578f);
    }
    const XrVector3f head{(l.position.x + r.position.x) / 2, (l.position.y + r.position.y) / 2,
                          (l.position.z + r.position.z) / 2};
    float turned[2] = {0, 0};
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = g_shownPose[eye];
        views[eye].fov = g_eyeFov[eye];
        views[eye].subImage = subImage(shownChain(eye));
        float delta = std::isfinite(nowYaw) && std::isfinite(g_shownYaw[eye])
                          ? std::remainder(g_shownYaw[eye] - nowYaw, 6.2831853f)
                          : 0.0f;
        if (std::fabs(delta) < 1e-5f || std::fabs(delta) > 0.5f) continue;  // still, or a jump (a new view)
        turned[eye] = delta;
        XrPosef& pose = views[eye].pose;
        const float c = std::cos(delta / 2), s = std::sin(delta / 2);  // about +y: {0, s, 0, c}
        const XrQuaternionf o = pose.orientation;
        pose.orientation = {c * o.x + s * o.z, c * o.y + s * o.w, c * o.z - s * o.x, c * o.w - s * o.y};
        const float dx = pose.position.x - head.x, dz = pose.position.z - head.z;
        const float cd = std::cos(delta), sd = std::sin(delta);
        pose.position.x = head.x + cd * dx + sd * dz;
        pose.position.z = head.z - sd * dx + cd * dz;
    }
    // Your body, which turns with you, on its own layer that the turn leaves
    // alone (bodylayer.h): each eye's as it was drawn.
    g_bodyLayerReady = false;
    if (bodyLayerOn()) {
        const int slots[2] = {chainSlot(shownChain(0)), chainSlot(shownChain(1))};
        if (slots[0] >= 0 && slots[1] >= 0 && g_bodyState[slots[0]] != kBodyUnknown &&
            g_bodyState[slots[1]] != kBodyUnknown &&
            (g_bodyState[slots[0]] == kBodyDrawn || g_bodyState[slots[1]] == kBodyDrawn)) {
            for (int eye = 0; eye < 2; ++eye) {
                g_bodyViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                g_bodyViews[eye].pose = g_shownPose[eye];
                g_bodyViews[eye].fov = g_eyeFov[eye];
                g_bodyViews[eye].subImage = subImage(g_bodyChains[slots[eye]]);
            }
            g_bodyProjection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            g_bodyProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
            g_bodyProjection.space = g_appSpace;
            g_bodyProjection.viewCount = 2;
            g_bodyProjection.views = g_bodyViews;
            g_bodyLayerReady = true;
        }
    }
    turnCheck(nowYaw, turned, newPicture);
    static int turnLogs = 0;
    static ULONGLONG lastTurnLog = 0;
    if ((turned[0] != 0 || turned[1] != 0) && turnLogs < 20 && GetTickCount64() - lastTurnLog > 2000) {
        ++turnLogs;
        lastTurnLog = GetTickCount64();
        LOG("Turning: eyes turned back %.2f / %.2f degrees (left / right) to the newest heading%s",
            turned[0] * 57.29578f, turned[1] * 57.29578f,
            g_bodyLayerReady ? "; your body on its own layer, not turned" : "");
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

