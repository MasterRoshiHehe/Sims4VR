// Your Sim's notebook (user 2026-10-23, thread "Notebook in VR"): a hand closing at your LEFT hip takes it out
// (the phone's gesture mirrored; either hand takes it, the same hand closing there again puts it away). A
// leather-bound book in that hand whose cover swings open on the game's notebook pages (hud.cpp notebook
// pages in its atlas, the game's own art; notebook.py sends the data); your other hand swept across it turns
// the page. First person only, read only. Three leaves (phone3d::setBook): the right page, the cover's
// leather, and the cover's inside (the left page), placed on the render thread from the Sim's hand joints.

// A hand down at your left hip (head-relative, like atRightHip).
static bool atLeftHip(const armik::Hand& hand, int side, const grab::HeadPose& head) {
    XrQuaternionf q{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
    XrVector3f right = rotate3(q, {1, 0, 0});
    float len = std::sqrt(right.x * right.x + right.z * right.z);
    if (len < 0.2f) return false;
    right = {right.x / len, 0, right.z / len};
    XrVector3f forward{right.z, 0, -right.x};
    XrVector3f rel{hand.position.x - head.position.x, hand.position.y - head.position.y,
                   hand.position.z - head.position.z};
    float down = -rel.y, toRight = dot3(rel, right), ahead = dot3(rel, forward);
    bool hip = down > g_config.bookHipDrop && toRight < -0.05f && ahead > -0.35f && ahead < 0.30f;
    static int logged = 0;
    if (logged++ < 20)
        LOG("Book: %s hand closed %.2f m below your eyes, %.2f m right, %.2f m ahead: %s", side ? "right" : "left", down,
            toRight, ahead, hip ? "takes the notebook out" : "not at the hip");
    return hip;
}

static ULONGLONG g_bookOpenedAt = 0;

static void bookOpen(int hand) {
    g_bookOut = true;
    g_bookHand = hand;
    g_bookSpread = 0;
    g_bookAnim = 0;
    g_bookOpenedAt = GetTickCount64();
    grab::pushControl(83);  // notebook.py builds the pages and hands them over
    buzz(hand, 0.4f, 0.05f);
    static int logged = 0;
    if (logged++ < 30) LOG("Book: out in the %s hand", hand ? "right" : "left");
}

static void bookClose(const char* why) {
    if (!g_bookOut) return;
    g_bookOut = false;
    phone3d::setBook(nullptr, 0, nullptr);
    grab::pushControl(84);
    static int logged = 0;
    if (logged++ < 30) LOG("Book: away (%s)", why);
}

// The book's frame in the world, from the Sim's hand as armik last posed it (the phone's frame math with
// [book] values; the left hand is the mirror of the right): its centre (the spine's middle), x across the
// spread, y up the spine, z out of the pages toward you, and the metre in world units.
static bool bookFrameWorld(int side, float c[3], XrVector3f& x, XrVector3f& y, XrVector3f& z, float& metre) {
    camera::Vec3 wrist;
    camera::Quat q;
    float hs = 1;
    if (!armik::handPose(side, wrist, q, hs)) return false;
    const XrQuaternionf grip{q.x, q.y, q.z, q.w};
    const float m = side ? 1.0f : -1.0f;
    const float* o = g_config.bookOffset;
    XrVector3f w = rotate3(grip, {(o[0] * m - 0.02f * m) * hs, o[1] * hs, (o[2] - 0.08f) * hs});
    c[0] = wrist.x + w.x;
    c[1] = wrist.y + w.y;
    c[2] = wrist.z + w.z;
    XrVector3f ax = rotate3(grip, {0, 0, m}), ay = rotate3(grip, {0, 1, 0}), az = rotate3(grip, {-m, 0, 0});
    const float tilt = g_config.bookTilt * 0.0174533f, ct = std::cos(tilt), st = std::sin(tilt);
    XrVector3f ay2{ay.x * ct - az.x * st, ay.y * ct - az.y * st, ay.z * ct - az.z * st};
    XrVector3f az2{ay.x * st + az.x * ct, ay.y * st + az.y * ct, ay.z * st + az.z * ct};
    const float yaw = m * g_config.bookYaw * 0.0174533f, cy = std::cos(yaw), sy = std::sin(yaw);
    XrVector3f ax3{ax.x * cy - az2.x * sy, ax.y * cy - az2.y * sy, ax.z * cy - az2.z * sy};
    XrVector3f az3{ax.x * sy + az2.x * cy, ax.y * sy + az2.y * cy, ax.z * sy + az2.z * cy};
    const float pitch = g_config.bookPitch * 0.0174533f, cp = std::cos(pitch), sp = std::sin(pitch);
    XrVector3f ay3{ay2.x * cp - az3.x * sp, ay2.y * cp - az3.y * sp, ay2.z * cp - az3.z * sp};
    XrVector3f az4{ay2.x * sp + az3.x * cp, ay2.y * sp + az3.y * cp, ay2.z * sp + az3.z * cp};
    x = ax3;
    y = ay3;
    z = az4;
    metre = hs;
    return true;
}

// The hand holding the book against it ([book] hand_offset / hand_turn), as phoneHandShift does for the phone.
static void bookHandShift(int side, camera::Vec3& move, camera::Quat& turn) {
    XrVector3f x{0, 0, 1}, y{0, 1, 0}, z{-1, 0, 0};
    float tilt = g_config.bookTilt * 0.0174533f, c = std::cos(tilt), s = std::sin(tilt);
    XrVector3f y2{y.x * c - z.x * s, y.y * c - z.y * s, y.z * c - z.z * s};
    XrVector3f z2{y.x * s + z.x * c, y.y * s + z.y * c, y.z * s + z.z * c};
    {
        float yaw = g_config.bookYaw * 0.0174533f, cy = std::cos(yaw), sy = std::sin(yaw);
        XrVector3f x3{x.x * cy - z2.x * sy, x.y * cy - z2.y * sy, x.z * cy - z2.z * sy};
        XrVector3f z3{x.x * sy + z2.x * cy, x.y * sy + z2.y * cy, x.z * sy + z2.z * cy};
        x = x3;
        z2 = z3;
        float pitch = g_config.bookPitch * 0.0174533f, cp = std::cos(pitch), sp = std::sin(pitch);
        XrVector3f y3{y2.x * cp - z2.x * sp, y2.y * cp - z2.y * sp, y2.z * cp - z2.z * sp};
        XrVector3f z4{y2.x * sp + z2.x * cp, y2.y * sp + z2.y * cp, y2.z * sp + z2.z * cp};
        y2 = y3;
        z2 = z4;
    }
    const float* o = g_config.bookHandOffset;
    move = {x.x * o[0] + y2.x * o[1] + z2.x * o[2], x.y * o[0] + y2.y * o[1] + z2.y * o[2],
            x.z * o[0] + y2.z * o[1] + z2.z * o[2]};
    auto about = [](const XrVector3f& v, const XrVector3f& k, float degrees) {
        float a = degrees * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
        XrVector3f kv = cross3(k, v);
        float kd = dot3(k, v) * (1 - ca);
        return XrVector3f{v.x * ca + kv.x * sa + k.x * kd, v.y * ca + kv.y * sa + k.y * kd,
                          v.z * ca + kv.z * sa + k.z * kd};
    };
    const float* t = g_config.bookHandTurn;
    XrVector3f basis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (XrVector3f& b : basis) b = about(about(about(b, z2, t[2]), x, t[1]), y2, t[0]);
    XrQuaternionf q = fromAxes(basis[0], basis[1], basis[2]);
    turn = {q.x, q.y, q.z, q.w};
    if (!side) {
        move.x = -move.x;
        turn.y = -turn.y;
        turn.z = -turn.z;
    }
}

// The leaves' places, render thread (phone3d::setBookPlacer): 0 the right page, 1 the cover's leather, 2 its
// inside. The cover hinges on the spine (the book frame's y axis) by g_bookAngle (0 closed .. pi open).
static std::atomic<float> g_bookAngle{0};
static std::atomic<int> g_bookSide{0};
constexpr float kBookPageAspect = 568.0f / 422.0f;  // the game's page art, height over width
constexpr float kBookLeaf = 0.0015f;                 // half the cover's thickness, metres

static void placeBook(phone3d::Panel* leaves, int count) {
    float c[3], metre = 1;
    XrVector3f x, y, z;
    if (!bookFrameWorld(g_bookSide.load(), c, x, y, z, metre)) {
        for (int i = 0; i < count; ++i) leaves[i].width = leaves[i].height = 0;
        return;
    }
    const float pw = g_config.bookWidth / 2, ph = pw * kBookPageAspect;
    const float th = g_bookAngle.load(), ct = std::cos(th), st = std::sin(th);
    auto put = [&](phone3d::Panel& p, const XrVector3f& at, const XrVector3f& ax, const XrVector3f& ay) {
        auto world = [&](const XrVector3f& v) {
            return XrVector3f{x.x * v.x + y.x * v.y + z.x * v.z, x.y * v.x + y.y * v.y + z.y * v.z,
                              x.z * v.x + y.z * v.y + z.z * v.z};
        };
        XrVector3f wc = world(at), wx = world(ax), wy = world(ay);
        p.centre[0] = c[0] + wc.x * metre;
        p.centre[1] = c[1] + wc.y * metre;
        p.centre[2] = c[2] + wc.z * metre;
        p.x[0] = wx.x, p.x[1] = wx.y, p.x[2] = wx.z;
        p.y[0] = wy.x, p.y[1] = wy.y, p.y[2] = wy.z;
        p.width = pw * metre;
        p.height = ph * metre;
    };
    const XrVector3f up{0, 1, 0};
    for (int i = 0; i < count; ++i) {
        phone3d::Panel& p = leaves[i];
        if (p.tag == 0) {  // the right page, flat on the spine's right
            put(p, {pw / 2, 0, 0}, {1, 0, 0}, up);
        } else if (p.tag == 1) {  // the cover's leather, facing out of the book while it lies on the page
            put(p, {ct * pw / 2 - st * kBookLeaf, 0, st * pw / 2 + ct * kBookLeaf}, {ct, 0, st}, up);
        } else if (p.tag == 2) {  // its inside, the left page, facing the other way
            put(p, {ct * pw / 2 + st * kBookLeaf, 0, st * pw / 2 - ct * kBookLeaf}, {-ct, 0, -st}, up);
        }
    }
}

// Present thread, every frame while the book is out: the cover's angle, and the leaves handed to phone3d.
static void updateBookLeaves() {
    if (!g_bookOut) return;
    const float seconds = std::max(0.1f, g_config.bookOpenSeconds);
    float t = std::min(1.0f, (GetTickCount64() - g_bookOpenedAt) / 1000.0f / seconds);
    t = 1 - (1 - t) * (1 - t) * (1 - t);  // eases out
    g_bookAnim = t;
    g_bookAngle = 3.14159265f * t;
    g_bookSide = g_bookHand;
    ID3D11Texture2D* atlas = hud::texture();
    if (!atlas) return;
    phone3d::Panel leaves[3];
    const hud::Rect* crops[3] = {&hud::kBookRight, &hud::kBookCover, &hud::kBookLeft};
    for (int i = 0; i < 3; ++i) {
        phone3d::Panel& p = leaves[i];
        p.show = true;
        p.clear = true;
        p.opacity = 1;
        p.tag = i;
        const hud::Rect& r = *crops[i];
        p.crop[0] = r.x / hud::kWidth;
        p.crop[1] = r.y / hud::kHeight;
        p.crop[2] = (r.x + r.w) / hud::kWidth;
        p.crop[3] = (r.y + r.h) / hud::kHeight;
    }
    phone3d::setBookPlacer(placeBook);
    phone3d::setBook(leaves, 3, atlas);
}

// Controls thread: the other hand's sweep across the book turns a page (a swipe toward the left, the way a page
// turns forward in a book you read, is the next page). The book in app space comes from the Sim's hand.
static void updateBookSwipe(const armik::Hand hands[2]) {
    static bool over = false;
    static float anchor = 0;
    static ULONGLONG lastTurn = 0;
    const int other = 1 - g_bookHand;
    camera::View view;
    float c[3], metre = 1;
    XrVector3f wx, wy, wz;
    if (!g_bookOut || !hands[other].valid || !camera::lastView(view) || !bookFrameWorld(g_bookHand, c, wx, wy, wz, metre)) {
        over = false;
        return;
    }
    const XrVector3f centre = worldToApp(view, c, false);
    const float dx[3] = {wx.x, wx.y, wx.z}, dy[3] = {wy.x, wy.y, wy.z}, dz[3] = {wz.x, wz.y, wz.z};
    const XrVector3f x = normalize3(worldToApp(view, dx, true)), y = normalize3(worldToApp(view, dy, true)),
                     z = normalize3(worldToApp(view, dz, true));
    const XrVector3f rel = sub3({hands[other].position.x, hands[other].position.y, hands[other].position.z}, centre);
    const float u = dot3(rel, x), v = dot3(rel, y), n = dot3(rel, z);
    const float halfW = g_config.bookWidth / 2, halfH = halfW * kBookPageAspect / 2;
    const bool inside = std::fabs(u) < halfW + 0.04f && std::fabs(v) < halfH + 0.05f && n > -0.03f && n < 0.12f;
    const ULONGLONG now = GetTickCount64();
    if (!inside || g_bookAnim < 1) {
        over = false;
        return;
    }
    if (!over) {
        over = true;
        anchor = u;
        return;
    }
    if (now - lastTurn < 350) {
        anchor = u;
        return;
    }
    const float moved = u - anchor;
    if (std::fabs(moved) < g_config.bookSwipe) return;
    const int spreads = std::max(1, hud::notebookSpreads());
    const int was = g_bookSpread;
    g_bookSpread = std::max(0, std::min(spreads - 1, g_bookSpread + (moved < 0 ? 1 : -1)));
    anchor = u;
    lastTurn = now;
    if (g_bookSpread != was) {
        buzz(other, 0.3f, 0.04f);
        static int logged = 0;
        if (logged++ < 40) LOG("Book: turned to spread %d of %d", g_bookSpread + 1, spreads);
    }
}
