// ---------------------------------------------------------------------------
// SteamVR: the older eye turned to the newest head pose (eyealign.h).

static bool g_alignBroken = false;  // a turned image couldn't be drawn: off for this session
static bool alignEyes() { return g_steamVR && g_config.alignEyes && !g_alignBroken; }
// Both eyes every frame (deptheye.h): first person, not with SteamVR's eye align.
static bool g_depthEyeBroken = false;  // the pass couldn't be made: off for this session
static bool depthEyeOn() {
    return g_config.depthEye && g_config.depthEyeDebug != 4 && !alignEyes() && !g_depthEyeBroken && camera::bodyActive();
}
// Its test view 4: both eyes drawn by the game, the made one only for screenshots (depthCompareEye).
static bool depthCompare() {
    return g_config.depthEye && g_config.depthEyeDebug == 4 && !alignEyes() && camera::bodyActive();
}
// Your body on its own layer: only the turn smoothing needs it, and not with
// SteamVR's eye align or the depth eye (which redraw the older eye).
static bool bodyLayerOn() { return g_config.bodyLayer && g_config.turnReprojection && !alignEyes() && !depthEyeOn(); }

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

// Where the older eye is now, relative to the fresh eye's render pose (from
// this frame's located views), and the rotation that turns its kept image
// there (a stick turn in between included). Returns the turn (radians).
static float olderEyeTarget(int fresh, const XrView (&located)[2], XrPosef& target, float rotation[9],
                            float tangents[4]) {
    const int old = 1 - fresh;
    const XrPosef& from = g_eyePose[fresh];
    const XrPosef &lf = located[fresh].pose, &lo = located[old].pose;
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
    qmatrix(qnorm(qmul(qconj(was), target.orientation)), rotation);
    const XrFovf& f = g_eyeFov[old];
    tangents[0] = std::tan(f.angleLeft);
    tangents[1] = std::tan(f.angleRight);
    tangents[2] = std::tan(f.angleUp);
    tangents[3] = std::tan(f.angleDown);
    return turn;
}

// `fresh` was just copied: the other eye's kept image is drawn into its
// swapchain turned to where that eye is now relative to the fresh eye's
// render pose, so both views carry one head pose, and its pose and heading
// become the fresh eye's.
static void alignOlderEye(int fresh, const XrView (&located)[2]) {
    const int old = 1 - fresh;
    if (!eyealign::has(old) || !shownChain(old).handle) return;
    XrPosef target;
    float rotation[9], tangents[4];
    const float turn = olderEyeTarget(fresh, located, target, rotation, tangents);

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

// Where the other eye is now, relative to the fresh eye's render pose (this
// frame's located views), and what deptheye::draw needs to make it: a point
// in the fresh view to the other eye's view (x right, y up, z ahead: OpenXR's
// view axes with z flipped), both views' tangents.
static void depthEyeTarget(const XrPosef& from, const XrFovf& f, int fresh, const XrView (&located)[2], XrPosef& target,
                           float rows[12], float srcTan[4], float dstTan[4]) {
    const int old = 1 - fresh;
    const XrPosef &lf = located[fresh].pose, &lo = located[old].pose;
    target.orientation = qnorm(qmul(from.orientation, qmul(qconj(lf.orientation), lo.orientation)));
    const XrVector3f apart = qrot(qconj(lf.orientation), {lo.position.x - lf.position.x, lo.position.y - lf.position.y,
                                                          lo.position.z - lf.position.z});
    const XrVector3f offset = qrot(from.orientation, apart);
    target.position = {from.position.x + offset.x, from.position.y + offset.y, from.position.z + offset.z};

    // The picture is made for the eye distance set by hand (made_eye_distance, 1 = measured); the
    // headset is still told the measured place (depthOtherEye adds made_eye_submit_*).
    const float k = g_config.madeEyeDistance;
    float r[9];
    qmatrix(qnorm(qmul(qconj(target.orientation), from.orientation)), r);
    const XrVector3f t = qrot(qconj(target.orientation), {-offset.x * k, -offset.y * k, -offset.z * k});
    const float flip[3] = {1, 1, -1};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) rows[i * 4 + j] = r[i * 3 + j] * flip[i] * flip[j];
    }
    rows[3] = t.x;
    rows[7] = t.y;
    rows[11] = -t.z;
    srcTan[0] = std::tan(f.angleLeft);
    srcTan[1] = std::tan(f.angleRight);
    srcTan[2] = std::tan(f.angleUp);
    srcTan[3] = std::tan(f.angleDown);
    const XrFovf& g = located[old].fov;
    // made_eye_shift_*: the window the picture is made through moves the other way, so its content
    // moves by the shift (+ right / up).
    const bool right = old == 1;  // the alignment rows are the right eye's
    const float sx = right ? std::tan(g_config.madeEyeShiftX * 0.01745329f) : 0.0f;
    const float sy = right ? std::tan(g_config.madeEyeShiftY * 0.01745329f) : 0.0f;
    dstTan[0] = std::tan(g.angleLeft) - sx;
    dstTan[1] = std::tan(g.angleRight) - sx;
    dstTan[2] = std::tan(g.angleUp) - sy;
    dstTan[3] = std::tan(g.angleDown) - sy;
}

// The made eye's place as the headset is told it: made_eye_submit_x along the eye's own right,
// turned made_eye_submit_yaw right and made_eye_submit_pitch up about itself.
static XrPosef madeEyeSubmitted(const XrPosef& p) {
    const Config& c = g_config;
    if (c.madeEyeSubmitX == 0 && c.madeEyeSubmitYaw == 0 && c.madeEyeSubmitPitch == 0) return p;
    XrPosef out = p;
    const XrVector3f side = qrot(p.orientation, {c.madeEyeSubmitX, 0, 0});
    out.position = {p.position.x + side.x, p.position.y + side.y, p.position.z + side.z};
    const float yaw = -c.madeEyeSubmitYaw * 0.00872665f, pitch = c.madeEyeSubmitPitch * 0.00872665f;  // half angles
    const XrQuaternionf turn{0, std::sin(yaw), 0, std::cos(yaw)};      // about +y: + turns left, so - for right
    const XrQuaternionf tilt{std::sin(pitch), 0, 0, std::cos(pitch)};  // about +x: + tips -z (ahead) up
    out.orientation = qnorm(qmul(p.orientation, qmul(turn, tilt)));
    return out;
}

// Each change to the made eye's alignment, in the log.
static void logMadeEyeAlignment() {
    const Config& c = g_config;
    const float now[7] = {c.madeEyeShiftX, c.madeEyeShiftY, c.madeEyeDistance, c.madeEyeDepth,
                          c.madeEyeSubmitX, c.madeEyeSubmitYaw, c.madeEyeSubmitPitch};
    static float was[7] = {0, 0, 1, 1, 0, 0, 0};
    if (!memcmp(now, was, sizeof now)) return;
    memcpy(was, now, sizeof now);
    LOG("Depth eye alignment: picture shift %+.2f / %+.2f deg (right / up), eye distance %.0f%%, depth %.0f%%; "
        "headset: eye %+.1f mm right, turned %+.2f deg right, %+.2f deg up",
        now[0], now[1], now[2] * 100, now[3] * 100, now[4] * 1000, now[5], now[6]);
}

// Depth eye, eyes taking turns: each eye's last real picture's view (camera::Rendered) and when it
// was kept (performance counter), with its depth (deptheye::keepDepth).
static camera::Rendered g_realView[2];
static bool g_realValid[2] = {false, false};
static LONGLONG g_realAt[2] = {0, 0};

// A point in the older picture's view (b: the other eye's last real one) to the view the other eye is
// made for now: both through the game world, so a stick turn or walk since b was drawn counts too.
// The target is where depthEyeTarget puts it, from the fresh eye's world camera (a). False when
// the world scale changed between them.
static bool olderEyeRows(int fresh, const XrView (&located)[2], const camera::Rendered& a, const camera::Rendered& b,
                         float rows[12]) {
    const float s = a.worldScale;
    if (!(s > 0) || !(b.worldScale > 0) || std::fabs(b.worldScale / s - 1) > 0.01f) return false;
    const int old = 1 - fresh;
    const XrPosef &lf = located[fresh].pose, &lo = located[old].pose;
    const XrQuaternionf qa{a.worldOrientation.x, a.worldOrientation.y, a.worldOrientation.z, a.worldOrientation.w};
    const XrQuaternionf qb{b.worldOrientation.x, b.worldOrientation.y, b.worldOrientation.z, b.worldOrientation.w};
    const XrQuaternionf qt = qnorm(qmul(qa, qmul(qconj(lf.orientation), lo.orientation)));
    const XrVector3f apart = qrot(qconj(lf.orientation), {lo.position.x - lf.position.x, lo.position.y - lf.position.y,
                                                          lo.position.z - lf.position.z});
    const float k = g_config.madeEyeDistance * s;
    const XrVector3f off = qrot(qa, {apart.x * k, apart.y * k, apart.z * k});
    const XrVector3f pt{a.worldPosition.x + off.x, a.worldPosition.y + off.y, a.worldPosition.z + off.z};
    float r[9];
    qmatrix(qnorm(qmul(qconj(qt), qb)), r);
    const XrVector3f t = qrot(qconj(qt), {(b.worldPosition.x - pt.x) / s, (b.worldPosition.y - pt.y) / s,
                                          (b.worldPosition.z - pt.z) / s});
    const float flip[3] = {1, 1, -1};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rows[i * 4 + j] = r[i * 3 + j] * flip[i] * flip[j];
    rows[3] = t.x;
    rows[7] = t.y;
    rows[11] = -t.z;
    return true;
}

// Depth eye (deptheye.h): `fresh` was just copied; the other eye's picture is
// made from it for this frame, moved by its depth to where that eye is now
// (this frame's located views, relative to the fresh render pose). Both views
// then carry the fresh render's head pose and heading.
static void depthOtherEye(int fresh, const XrView (&located)[2], const D3D11_TEXTURE2D_DESC& desc,
                          const camera::Rendered& rendered) {
    const int old = 1 - fresh;
    Swapchain& s = shownChain(old);
    bool recreated = false;
    if (!ensureSwapchain(s, desc, recreated)) return;
    if (recreated) eyealign::forget(old);
    UINT width = 0, height = 0;
    ID3D11ShaderResourceView* source = eyealign::keptView(fresh, width, height);
    if (!source) return;

    XrPosef target;
    float rows[12], srcTan[4], dstTan[4];
    depthEyeTarget(g_eyePose[fresh], g_eyeFov[fresh], fresh, located, target, rows, srcTan, dstTan);
    const XrFovf& g = located[old].fov;

    uint32_t index = 0;
    if (!acquireImage(s, index)) {
        source->Release();
        return;
    }
    // The depth is in game units, the eyes' places in metres.
    const float scale = camera::worldScale() > 0 ? camera::worldScale() : 1.0f;
    deptheye::setDebugView(g_config.depthEyeDebug);
    deptheye::noteDrawnProjection(rendered.drawnNear / scale, rendered.drawnFar / scale);
    logMadeEyeAlignment();
    const float depthScale = g_config.madeEyeDepth > 0.05f ? g_config.madeEyeDepth : 1.0f;
    // Eyes taking turns: what the fresh eye can't see comes from the other eye's own last picture.
    deptheye::Older older{};
    const deptheye::Older* olderUsed = nullptr;
    UINT olderWidth = 0, olderHeight = 0;
    if (g_config.depthEyeAlternate && g_realValid[old]) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        older.ageMs = double(now.QuadPart - g_realAt[old]) * 1000.0 / double(frequency.QuadPart);
        const camera::Rendered& b = g_realView[old];
        if (older.ageMs < 100 && olderEyeRows(fresh, located, rendered, b, older.rows) &&
            (older.colour = eyealign::keptView(old, olderWidth, olderHeight)) != nullptr) {
            if (olderWidth == width && olderHeight == height) {
                older.eye = old;
                older.tan[0] = std::tan(b.fov.left);
                older.tan[1] = std::tan(b.fov.right);
                older.tan[2] = std::tan(b.fov.up);
                older.tan[3] = std::tan(b.fov.down);
                older.nearZ = b.nearZ / b.worldScale * depthScale;
                older.farZ = b.farZ / b.worldScale * depthScale;
                olderUsed = &older;
            }
        }
    }
    const bool drawn = deptheye::draw(source, s.images[index].texture, s.format, width, height, rows, srcTan, dstTan,
                                      rendered.nearZ / scale * depthScale, rendered.farZ / scale * depthScale,
                                      olderUsed);
    if (older.colour) older.colour->Release();
    source->Release();
    // This frame's depth and view, kept as the fresh eye's for when it is the made one next frame.
    g_realValid[fresh] = rendered.worldScale > 0 && deptheye::keepDepth(fresh);
    if (g_realValid[fresh]) {
        g_realView[fresh] = rendered;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_realAt[fresh] = now.QuadPart;
    }
    if (drawn) {
        // A screenshot (right stick click) takes this made eye as the right one.
        ID3D11Device* device = nullptr;
        s.images[index].texture->GetDevice(&device);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        screenshot::offer(context, s.images[index].texture, old == 1 ? screenshot::kRightEye : screenshot::kLeftEye);
        context->Release();
        device->Release();
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage")) return;
    static int misses = 0;  // frames in a row without the other eye
    if (!drawn) {
        // The image shown now holds whatever it held before: leave the eye out
        // (that frame isn't submitted) and try again next frame. Off only when
        // the pass can't be made at all, or nothing was made for 2 s.
        g_eyeValid[old] = false;
        static uint64_t missed = 0;
        if (++missed <= 5 || missed % 1000 == 0)
            LOG("Depth eye: other eye not made this frame (%s; %llu so far)", deptheye::failure(),
                static_cast<unsigned long long>(missed));
        if (deptheye::broken() || ++misses >= 144) {
            g_depthEyeBroken = true;
            LOG("Depth eye: couldn't make the other eye (%s); off (alternate eyes again)", deptheye::failure());
        }
        return;
    }
    misses = 0;
    if (old == 1) target = madeEyeSubmitted(target);
    g_eyePose[old] = g_shownPose[old] = target;
    g_eyeFov[old] = g;
    g_eyeYaw[old] = g_shownYaw[old] = g_eyeYaw[fresh];
    if (old == 1) g_shownLead1 = rendered.lead;
    g_eyeValid[old] = true;
    static uint64_t frames = 0;
    if (++frames == 1 || frames == 1000 || frames % 100000 == 0)
        LOG("Depth eye: %llu frames made from the %s eye", static_cast<unsigned long long>(frames),
            fresh ? "right" : "left");
}

// Depth eye test view 4 ("Compare with the real eye"): the game draws both
// eyes in turn as without the depth eye, and while a screenshot is taken the
// right eye is also made from the left one into a picture of its own, saved
// as the screenshot's _menu.jpg, to set beside the real right eye.
static ID3D11Texture2D* g_compareImage = nullptr;
static void depthCompareEye(const XrView (&located)[2], const camera::Rendered& rendered) {
    const Swapchain& right = shownChain(1);
    UINT width = 0, height = 0;
    ID3D11ShaderResourceView* source = eyealign::keptView(0, width, height);
    if (!source || !right.handle) {
        if (source) source->Release();
        return;
    }
    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    D3D11_TEXTURE2D_DESC have{};
    ID3D11Device* made = nullptr;  // a reset device makes a new one
    if (g_compareImage) {
        g_compareImage->GetDesc(&have);
        g_compareImage->GetDevice(&made);
        made->Release();
    }
    if (!g_compareImage || made != device || have.Width != width || have.Height != height ||
        have.Format != right.format) {
        if (g_compareImage) g_compareImage->Release();
        g_compareImage = nullptr;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = right.format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_compareImage))) g_compareImage = nullptr;
    }
    const XrPosef from{{rendered.orientation.x, rendered.orientation.y, rendered.orientation.z, rendered.orientation.w},
                       {rendered.position.x, rendered.position.y, rendered.position.z}};
    const XrFovf fov{rendered.fov.left, rendered.fov.right, rendered.fov.up, rendered.fov.down};
    XrPosef target;
    float rows[12], srcTan[4], dstTan[4];
    depthEyeTarget(from, fov, 0, located, target, rows, srcTan, dstTan);
    const float scale = camera::worldScale() > 0 ? camera::worldScale() : 1.0f;
    deptheye::setDebugView(0);
    if (g_compareImage &&
        deptheye::draw(source, g_compareImage, right.format, width, height, rows, srcTan, dstTan,
                       rendered.nearZ / scale, rendered.farZ / scale)) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        screenshot::offer(context, g_compareImage, screenshot::kMenu);
        context->Release();
        static int logged = 0;
        if (logged++ < 20) LOG("Depth eye compare: the right eye made from the left one for the screenshot's _menu.jpg");
    }
    device->Release();
    source->Release();
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
    benchmark::turnFrame(newPicture, std::max(std::fabs(turned[0]), std::fabs(turned[1])) * 57.29578f);
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
        const bool hold = eye == 0 && rendered.pairFirst && !alignEyes() && !depthEyeOn();
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
        g_keepEye = alignEyes() || depthEyeOn() || (depthCompare() && eye == 0) ? eye : -1;
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
        if (copied && eye == 0 && depthCompare() && screenshot::taking()) depthCompareEye(located, rendered);
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
            else if (depthEyeOn()) depthOtherEye(eye, located, desc, rendered);
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
    camera::setPairedEyes(g_config.pairedEyes && !alignEyes() && !depthEyeOn());
    camera::setExactFov(g_config.exactFov);
    camera::setPairedHands(g_config.pairedHands);
    singleview::setProbe(g_config.probeDouble);
    camera::setBodyLayer(bodyLayerOn());
    phone3d::setWriteDepth(depthEyeOn());
    // Depth eye: the left eye every frame, its view widened to hold the right
    // eye's too (the right eye is made from it).
    const bool depthEye = depthEyeOn();
    // Eyes taking turns ([stereo] depth_eye_alternate): each widened the same way.
    int next = depthEye && !g_config.depthEyeAlternate ? 0 : g_renderingEye == 0 ? 1 : 0;
    const XrPosef& p = located[next].pose;
    XrFovf f = located[next].fov;
    if (depthEye) {
        const XrFovf& o = located[1 - next].fov;
        f = {std::min(f.angleLeft, o.angleLeft), std::max(f.angleRight, o.angleRight), std::max(f.angleUp, o.angleUp),
             std::min(f.angleDown, o.angleDown)};
    }
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
    if (depthEyeOn()) {
        // What the headset is sent for each eye, against where it says the eyes are now: the right
        // eye's place and turn seen from the left eye, both fields of view, both pictures' sizes.
        static int submitLogs = 0;
        static ULONGLONG lastSubmitLog = 0;
        if (submitLogs < 6 && (!submitLogs || GetTickCount64() - lastSubmitLog > 30000)) {
            ++submitLogs;
            lastSubmitLog = GetTickCount64();
            auto between = [](const XrPosef& a, const XrPosef& b, XrVector3f& d, float& deg) {
                d = qrot(qconj(a.orientation), {b.position.x - a.position.x, b.position.y - a.position.y,
                                                b.position.z - a.position.z});
                const XrQuaternionf m = qmul(qconj(a.orientation), b.orientation);
                deg = 2 * std::acos(std::min(1.0f, std::fabs(m.w))) * 57.29578f;
            };
            XrVector3f sent, now;
            float sentDeg, nowDeg;
            between(views[0].pose, views[1].pose, sent, sentDeg);
            between(located[0].pose, located[1].pose, now, nowDeg);
            const XrFovf &a = views[0].fov, &b = views[1].fov;
            LOG("Depth eye submit: right eye from the left sent %+.1f %+.1f %+.1f mm, turned %.2f deg (headset now "
                "%+.1f %+.1f %+.1f mm, %.2f deg); fov sent left %.1f..%.1f / %.1f..%.1f, right %.1f..%.1f / "
                "%.1f..%.1f deg; pictures %dx%d / %dx%d",
                sent.x * 1000, sent.y * 1000, sent.z * 1000, sentDeg, now.x * 1000, now.y * 1000, now.z * 1000, nowDeg,
                a.angleLeft * 57.29578f, a.angleRight * 57.29578f, a.angleDown * 57.29578f, a.angleUp * 57.29578f,
                b.angleLeft * 57.29578f, b.angleRight * 57.29578f, b.angleDown * 57.29578f, b.angleUp * 57.29578f,
                views[0].subImage.imageRect.extent.width, views[0].subImage.imageRect.extent.height,
                views[1].subImage.imageRect.extent.width, views[1].subImage.imageRect.extent.height);
        }
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

