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

// A page turning: a short swish of paper and a soft tap, made once as a wave and played through the system's
// default output (the headset's audio follows it).
static bool bookFrameWorld(int side, float c[3], XrVector3f& x, XrVector3f& y, XrVector3f& z, float& metre);

static void bookFlipSound() {
    if (!g_config.bookSound) return;
    static std::vector<int16_t> pcm;
    const int rate = 22050;
    if (pcm.empty()) {
        const int n = static_cast<int>(rate * 0.42f);
        pcm.resize(n);
        uint32_t seed = 0x1234567u;
        float lp = 0, lp2 = 0;
        for (int i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / rate;
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const float noise = (seed & 0xffff) / 32768.0f - 1;
            const float sweep = 0.10f + 0.55f * std::sin(std::min(1.0f, t / 0.22f) * 3.14159f);  // the brightness rises and falls
            lp += (noise - lp) * sweep;
            lp2 += (lp - lp2) * 0.6f;
            const float env = std::min(1.0f, t / 0.03f) * std::exp(-std::max(0.0f, t - 0.08f) * 9.0f);
            float v = (lp - lp2) * 2.2f * env;
            const float tap = t - 0.27f;  // the page landing
            if (tap > 0) v += std::sin(tap * 2 * 3.14159f * 140) * std::exp(-tap * 60) * 0.5f;
            pcm[i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v * 0.7f)) * 32000);
        }
    }
    // Placed at the book: louder on the ear side it is at, a little later in the far ear, softer with distance.
    float left = 1, right = 1;
    int delay = 0;  // samples the far ear lags
    camera::View view;
    float c[3], metre = 1;
    XrVector3f wx, wy, wz;
    if (camera::lastView(view) && bookFrameWorld(g_bookHand, c, wx, wy, wz, metre)) {
        const float rx = c[0] - view.position.x, ry = c[1] - view.position.y, rz = c[2] - view.position.z;
        const float m = metre > 0.01f ? metre : 1;
        const float dist = std::sqrt(rx * rx + ry * ry + rz * rz) / m;
        if (dist > 0.01f) {
            const float side = (rx * view.right.x + ry * view.right.y + rz * view.right.z) / (dist * m);  // -1 left .. 1 right
            const float gain = 1.0f / (1.0f + std::max(0.0f, dist - 0.3f) * 1.5f);
            const float pan = std::max(-1.0f, std::min(1.0f, side));
            left = gain * std::sqrt(0.5f * (1 - pan) + 0.08f);
            right = gain * std::sqrt(0.5f * (1 + pan) + 0.08f);
            delay = static_cast<int>(std::fabs(pan) * 0.0006f * rate);
        }
    }
    static std::vector<uint8_t> wave[2];
    static int which = 0;
    std::vector<uint8_t>& w = wave[which ^= 1];
    w.clear();
    const int n = static_cast<int>(pcm.size()) + delay;
    auto put32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) w.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    auto put16 = [&](uint16_t v) { for (int k = 0; k < 2; ++k) w.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    for (char ch : {'R', 'I', 'F', 'F'}) w.push_back(static_cast<uint8_t>(ch));
    put32(36 + n * 4);
    for (char ch : {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}) w.push_back(static_cast<uint8_t>(ch));
    put32(16), put16(1), put16(2), put32(rate), put32(rate * 4), put16(4), put16(16);
    for (char ch : {'d', 'a', 't', 'a'}) w.push_back(static_cast<uint8_t>(ch));
    put32(n * 4);
    const bool rightFar = right < left;
    for (int i = 0; i < n; ++i) {
        const int il = rightFar ? i : i - delay, ir = rightFar ? i - delay : i;
        const int16_t sl = il >= 0 && il < static_cast<int>(pcm.size()) ? pcm[il] : 0;
        const int16_t sr = ir >= 0 && ir < static_cast<int>(pcm.size()) ? pcm[ir] : 0;
        put16(static_cast<uint16_t>(static_cast<int16_t>(sl * left)));
        put16(static_cast<uint16_t>(static_cast<int16_t>(sr * right)));
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

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
    hideHud(g_bookSheet[0]);
    hideHud(g_bookSheet[1]);
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

// The book as a real object made of flat faces (a hardcover: a back board, a spine, a page block under each
// side, and a front board that hinges on the spine carrying the left block), each a textured quad of the HUD
// atlas. Faces turned away are not drawn (phone3d), the rest write depth, so the pieces hide each other right.
// Book frame (metres): x across the spread from the spine, y up the spine, z out of the pages toward you; the
// pages' plane is z = 0. The cover swings by g_bookAngle (0 closed .. pi open) about the spine.
static std::atomic<float> g_bookAngle{0};
static std::atomic<int> g_bookSide{0};
constexpr float kBookPageAspect = 568.0f / 422.0f;  // the game's page art, height over width
enum BookCrop { kCropRight, kCropLeft, kCropLeather, kCropPlain, kCropPaper, kCropEdge };
struct BookLeaf {
    bool turns;       // rides on the front board (turned by the angle) or stays with the back board
    float c[3];       // its centre (the board's frame when it turns)
    float ax[3], ay[3];
    float w, h;       // along ax, along ay
    int crop;
};
constexpr int kBookLeaves = 19;

// Leaf i of the book for page width pw, height ph and cover angle: the numbers are what the comments at each
// face say. Normal = ax x ay.
static BookLeaf bookLeaf(int i, float pw, float ph) {
    const float m = pw * 0.05f, t = pw * 0.028f, pb = pw * 0.10f, W = pw + m, H = ph + 2 * m;
    switch (i) {
        // The front board and the left block under it (they turn).
        case 0: return {true, {W / 2, 0, t}, {1, 0, 0}, {0, 1, 0}, W, H, kCropLeather};              // outside
        case 1: return {true, {W / 2, 0, 0}, {-1, 0, 0}, {0, 1, 0}, W, H, kCropPaper};               // inside (endpaper)
        case 2: return {true, {pw / 2, 0, -pb}, {-1, 0, 0}, {0, 1, 0}, pw, ph, kCropLeft};           // the left page
        case 3: return {true, {W / 2, H / 2, t / 2}, {0, 0, 1}, {1, 0, 0}, t, W, kCropPlain};        // top edge
        case 4: return {true, {W / 2, -H / 2, t / 2}, {1, 0, 0}, {0, 0, 1}, W, t, kCropPlain};       // bottom edge
        case 5: return {true, {W, 0, t / 2}, {0, 1, 0}, {0, 0, 1}, H, t, kCropPlain};                // outer edge
        case 6: return {true, {pw / 2, ph / 2, -pb / 2}, {0, 0, 1}, {1, 0, 0}, pb, pw, kCropEdge};   // block: top
        case 7: return {true, {pw / 2, -ph / 2, -pb / 2}, {1, 0, 0}, {0, 0, 1}, pw, pb, kCropEdge};  // bottom
        case 8: return {true, {pw, 0, -pb / 2}, {0, 1, 0}, {0, 0, 1}, ph, pb, kCropEdge};            // outer
        // The back board, the spine and the right block (they stay).
        case 9: return {false, {W / 2, 0, 0}, {1, 0, 0}, {0, 1, 0}, W, H, kCropPaper};               // its inside
        case 10: return {false, {W / 2, 0, -t}, {-1, 0, 0}, {0, 1, 0}, W, H, kCropPlain};            // outside
        case 11: return {false, {W / 2, H / 2, -t / 2}, {0, 0, 1}, {1, 0, 0}, t, W, kCropPlain};
        case 12: return {false, {W / 2, -H / 2, -t / 2}, {1, 0, 0}, {0, 0, 1}, W, t, kCropPlain};
        case 13: return {false, {W, 0, -t / 2}, {0, 1, 0}, {0, 0, 1}, H, t, kCropPlain};
        case 14: return {false, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 0, H, kCropLeather};                // the spine (sized live)
        case 15: return {false, {pw / 2, 0, pb}, {1, 0, 0}, {0, 1, 0}, pw, ph, kCropRight};          // the right page
        case 16: return {false, {pw / 2, ph / 2, pb / 2}, {0, 0, 1}, {1, 0, 0}, pb, pw, kCropEdge};
        case 17: return {false, {pw / 2, -ph / 2, pb / 2}, {1, 0, 0}, {0, 0, 1}, pw, pb, kCropEdge};
        default: return {false, {pw, 0, pb / 2}, {0, 1, 0}, {0, 0, 1}, ph, pb, kCropEdge};
    }
}

static void placeBook(phone3d::Panel* leaves, int count) {
    float c[3], metre = 1;
    XrVector3f x, y, z;
    if (!bookFrameWorld(g_bookSide.load(), c, x, y, z, metre)) {
        for (int i = 0; i < count; ++i) leaves[i].width = leaves[i].height = 0;
        return;
    }
    const float pw = g_config.bookWidth / 2, ph = pw * kBookPageAspect;
    const float pb = pw * 0.10f, t = pw * 0.028f;
    const float th = g_bookAngle.load(), ct = std::cos(th), st = std::sin(th);
    const float hinge = pb * (1 + ct);  // the front board's hinge rides down as the cover opens
    auto world = [&](const XrVector3f& v) {
        return XrVector3f{x.x * v.x + y.x * v.y + z.x * v.z, x.y * v.x + y.y * v.y + z.y * v.z,
                          x.z * v.x + y.z * v.y + z.z * v.z};
    };
    for (int i = 0; i < count; ++i) {
        phone3d::Panel& p = leaves[i];
        if (p.tag < 0 || p.tag >= kBookLeaves) {
            p.width = p.height = 0;
            continue;
        }
        BookLeaf l = bookLeaf(p.tag, pw, ph);
        XrVector3f at{l.c[0], l.c[1], l.c[2]}, ax{l.ax[0], l.ax[1], l.ax[2]}, ay{l.ay[0], l.ay[1], l.ay[2]};
        if (p.tag == 14) {  // the spine spans the boards' gap
            l.w = hinge + 2 * t;
            at.z = hinge / 2;
        }
        if (l.turns) {
            auto turn = [&](const XrVector3f& v) { return XrVector3f{v.x * ct - v.z * st, v.y, v.x * st + v.z * ct}; };
            at = turn(at);
            at.z += hinge;
            ax = turn(ax);
            ay = turn(ay);
        }
        at.z -= pb;  // the pages' plane stays where the flat leaves were
        XrVector3f wc = world(at), wx = world(ax), wy = world(ay);
        p.centre[0] = c[0] + wc.x * metre;
        p.centre[1] = c[1] + wc.y * metre;
        p.centre[2] = c[2] + wc.z * metre;
        p.x[0] = wx.x, p.x[1] = wx.y, p.x[2] = wx.z;
        p.y[0] = wy.x, p.y[1] = wy.y, p.y[2] = wy.z;
        p.width = l.w * metre;
        p.height = l.h * metre;
    }
}

// Present thread, every frame while the book is out: the cover's angle, and the faces handed to phone3d.
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
    static const hud::Rect* const kRects[6] = {&hud::kBookRight, &hud::kBookLeft, &hud::kBookLeatherAll,
                                               &hud::kBookLeatherPlain, &hud::kBookPaper, &hud::kBookEdge};
    phone3d::Panel leaves[kBookLeaves];
    for (int i = 0; i < kBookLeaves; ++i) {
        phone3d::Panel& p = leaves[i];
        p.show = true;
        p.clear = true;
        p.opacity = 1;
        p.tag = i;
        const hud::Rect& r = *kRects[bookLeaf(i, 0.13f, 0.175f).crop];
        p.crop[0] = r.x / hud::kWidth;
        p.crop[1] = r.y / hud::kHeight;
        p.crop[2] = (r.x + r.w) / hud::kWidth;
        p.crop[3] = (r.y + r.h) / hud::kHeight;
    }
    phone3d::setBookPlacer(placeBook);
    phone3d::setBook(leaves, kBookLeaves, atlas);
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
    if (!g_bookOut || !camera::lastView(view) || !bookFrameWorld(g_bookHand, c, wx, wy, wz, metre)) {
        hideHud(g_bookSheet[0]);
        hideHud(g_bookSheet[1]);
        over = false;
        return;
    }
    const XrVector3f centre = worldToApp(view, c, false);
    const float dx[3] = {wx.x, wx.y, wx.z}, dy[3] = {wy.x, wy.y, wy.z}, dz[3] = {wz.x, wz.y, wz.z};
    const XrVector3f x = normalize3(worldToApp(view, dx, true)), y = normalize3(worldToApp(view, dy, true)),
                     z = normalize3(worldToApp(view, dz, true));
    // The pages as pokeable sheets (app space), once the cover is open: poke and laser reach their buttons.
    {
        const float pw = g_config.bookWidth / 2;
        const hud::Rect* areas[2] = {&hud::kBookLeft, &hud::kBookRight};
        for (int i = 0; i < 2; ++i) {
            if (!g_config.bookPoke || g_bookAnim < 1) {
                hideHud(g_bookSheet[i]);
                continue;
            }
            const float k = i ? 0.5f * pw : -0.5f * pw;
            placeHud(g_bookSheet[i], {centre.x + x.x * k, centre.y + x.y * k, centre.z + x.z * k}, x, y, pw, *areas[i]);
        }
    }
    if (!hands[other].valid) {
        over = false;
        return;
    }
    if (g_bookSheet[0].m.pressing >= 0 || g_bookSheet[1].m.pressing >= 0 || g_bookSheet[0].m.armed[other] ||
        g_bookSheet[1].m.armed[other]) {  // a finger at a page is poking, not sweeping
        over = false;
        return;
    }
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
        bookFlipSound();
        static int logged = 0;
        if (logged++ < 40) LOG("Book: turned to spread %d of %d", g_bookSpread + 1, spreads);
    }
}

// Present thread: a spread the pointer picked (a category cell, a page corner).
static void bookApplyPick() {
    if (!g_bookOut) return;
    const int want = hud::notebookTakeSpread();
    if (want >= 0) {
        const int was = g_bookSpread;
        g_bookSpread = std::max(0, std::min(std::max(1, hud::notebookSpreads()) - 1, want));
        if (g_bookSpread != was) bookFlipSound();
        LOG("Book: pointer turned to spread %d", g_bookSpread + 1);
    }
}
