// ---------------------------------------------------------------------------
// Your Sim's phone (first person, [phone] phone = 1): close your right hand at
// your right hip and your Sim takes its phone out into that hand (control 3 to
// Python; our own phone is drawn in the hand, updatePhone). Its screen shows
// the game's own phone menu (phoneapp.py, hud.cpp drawPhone; user 2026-10-18
// 20:35Z), and its Options app the game menu as below. With Options open it shows
// the game menu: Escape opens it, and
// Python keeps the menu's pause from stopping the game. Poke the screen with
// your left index finger or point the left laser at it. Open the hand and the
// phone goes away (control 4), closing the menu.
static PieMenu g_phone;
static constexpr float kPhoneChin = 0.035f;  // the bands above and below the screen, x the phone's height
static std::atomic<bool> g_gameMenuOpen{false}, g_gameMenuReported{false};
static bool g_phoneOpenedMenu = false;   // our Escape opened the menu (for when Python can't say)
static bool g_vrOptionsAsked = false;   // the phone's VR Options app was pressed: open the debug menu (16_controls)
// A window opened from the phone's own Game Options (user 2026-10-24 01:26Z): the game's UI shows it in front
// of you (the Options' front path below) while the phone keeps our screen; save questions are clientdlg cards.
// The screen is off when the phone comes out, until the holding hand's trigger (user 2026-10-24 01:29Z: hides
// the loading); g_phoneWokeAt fades it on. (02:26Z: the grip, not the trigger.)
static bool g_phoneAsleep = false;
// The phone's Sims Forever app (user 2026-10-24 02:21Z): the phone turned 90 degrees counter-clockwise
// (landscape) and stuck in front of where you looked when you pressed it, Sims 1 on its glass, your real
// mouse and keyboard playing it, until you quit Sims 1; then it is back in your hand. App space: the
// phone's centre, the view's right / up / ahead then; glassW x glassH the portrait glass (updatePhone).
struct Sims1Phone {
    bool on = false;
    XrVector3f at{}, right{}, up{}, ahead{};
    float glassW = 0, glassH = 0, glass = 0;  // glass: its front ahead of the centre
};
static Sims1Phone g_sims1Phone;
static void startSims1Phone() {
    if (g_sims1Phone.on || !g_eyeValid[0] || !g_eyeValid[1]) return;
    const XrVector3f& e0 = g_eyePose[0].position;
    const XrVector3f& e1 = g_eyePose[1].position;
    XrVector3f head{(e0.x + e1.x) * 0.5f, (e0.y + e1.y) * 0.5f, (e0.z + e1.z) * 0.5f};
    XrVector3f ahead = normalize3(rotate3(g_eyePose[0].orientation, {0, 0, -1}));
    XrVector3f right = cross3(ahead, {0, 1, 0});  // level: no head roll
    right = dot3(right, right) > 0.01f ? normalize3(right) : rotate3(g_eyePose[0].orientation, {1, 0, 0});
    const float distance = 0.30f;
    g_sims1Phone.at = {head.x + ahead.x * distance, head.y + ahead.y * distance, head.z + ahead.z * distance};
    g_sims1Phone.right = right;
    g_sims1Phone.up = normalize3(cross3(right, ahead));
    g_sims1Phone.ahead = ahead;
    g_sims1Phone.on = true;
    simsforever::setPhone(true);
    LOG("Phone: Sims Forever, the phone turned sideways in front of you");
}
static ULONGLONG g_phoneWokeAt = 0;
static bool g_phoneWindow = false, g_phoneWindowSeen = false;
static ULONGLONG g_phoneWindowSince = 0;
static bool g_phoneOptions = false;      // the Options app is open: the game menu on the screen (else the game's phone menu)
static bool g_phoneOptionsSeen = false;  // the game menu has been open since Options was pressed
static ULONGLONG g_phoneMenuAt = 0;      // an Escape to open the menu, once the pie's own has gone
static ULONGLONG g_phoneAwayAt = 0;      // tell Python the phone is away (after the menu's Escape)
static ULONGLONG g_phoneMenuSince = 0;   // the menu open since
static bool g_phoneFitted = false;       // measured the menu for this opening
static bool g_phoneFound = false;        // g_phoneCrop holds a measured menu
static int32_t g_phoneCrop[4] = {};      // the menu's box in the UI: x, y, width, height
static ID3D11Texture2D* g_phoneFitCopy = nullptr;
static bool g_phoneFitPending = false;
static int g_phoneFitWait = 0;
static bool g_menuFitOk = false;   // the last fitMenu found the menu
static int g_menuFitDone = 0;      // fits finished (found or not)
// Options' game menu goes on the screen once measured (user 2026-10-23 20:01Z: no half-built screen):
// until then the phone keeps its last screen (hud Frame.phoneWait), then dims and comes up on the menu.
static bool g_optionsShown = false;
static int g_optionsFits = 0;            // g_menuFitDone when Options was asked for
static ULONGLONG g_optionsReadyAt = 0;   // measured: the last screen dims (kScreenDim ms), then the menu fades in
static constexpr ULONGLONG kScreenDim = 120, kScreenUp = 220, kOptionsMaxWait = 2500;
static void optionsAsked() {
    g_optionsShown = false;
    g_optionsFits = g_menuFitDone;
    g_optionsReadyAt = 0;
}

// The game's phone model's size (phone.py), world units; 0 = none.
static std::atomic<float> g_propSize[3] = {};

void setPhoneProp(float width, float height, float depth) {
    g_propSize[0] = width;
    g_propSize[1] = height;
    g_propSize[2] = depth;
    LOG("Phone: the game's phone model is %.3f x %.3f x %.3f", width, height, depth);
}

// Our phone's size: the ini's, or a little larger than the game's model so it hides inside.
static void phoneSize(float& w, float& h, float& d) {
    if (g_tablet) {
        // The screen takes the UI's shape unless [tablet] height says otherwise.
        w = g_config.tabletWidth;
        d = g_config.tabletDepth;
        h = g_config.tabletHeight;
        if (h < 0.08f) {
            // The screen takes the palette's shape. When the whole UI shows
            // instead (a dialog), it stays the size it was and the UI fits inside.
            static float aspect = 1.4f;
            if (g_tabletAspect >= 1.2f) aspect = g_tabletAspect;
            h = (w - 2 * g_config.tabletBezel) / aspect + 2 * g_config.tabletBezel;
        }
        return;
    }
    float s = g_config.worldScale > 0 ? g_config.worldScale : 1.0f;
    w = std::max(g_config.phoneWidth, g_propSize[0].load() / s + 0.004f);
    h = std::max(g_config.phoneHeight, g_propSize[1].load() / s + 0.004f);
    d = std::max(g_config.phoneDepth, g_propSize[2].load() / s + 0.002f);
}

// The phone's turn about its own up axis ([phone] yaw), after the tilt: its
// right (x) and screen (z) axes swing round its up axis. Then [phone] pitch:
// its top tips away from you, about its own left-right axis.
static void pitchPhone(XrVector3f& y, XrVector3f& z) {
    float pitch = g_config.phonePitch * 0.0174533f, c = std::cos(pitch), s = std::sin(pitch);
    XrVector3f y3{y.x * c - z.x * s, y.y * c - z.y * s, y.z * c - z.z * s};
    XrVector3f z3{y.x * s + z.x * c, y.y * s + z.y * c, y.z * s + z.z * c};
    y = y3;
    z = z3;
}

// sign -1: the left hand's phone, the mirror image of the right's.
static void turnPhone(XrVector3f& x, XrVector3f& z, float sign = 1) {
    float yaw = sign * g_config.phoneYaw * 0.0174533f, c = std::cos(yaw), s = std::sin(yaw);
    XrVector3f x3{x.x * c - z.x * s, x.y * c - z.y * s, x.z * c - z.z * s};
    XrVector3f z3{x.x * s + z.x * c, x.y * s + z.y * c, x.z * s + z.z * c};
    x = x3;
    z = z3;
}

// Where the hand holding the phone sits against it ([phone] hand_offset and
// hand_turn, live in the B menu): moved along the phone's own axes (out from
// its middle toward the edge the hand holds, up it, toward you) and turned
// about the palm (about the phone's up axis, then its left-right axis, then
// the line out of its screen). The phone stays where it is. In the
// controller's grip frame (tilted by hand_pitch); the left hand gets the
// mirror image, across the grip's X, as its phone does.
static void phoneHandShift(int side, camera::Vec3& move, camera::Quat& turn) {
    // The right hand's phone axes in its grip frame, turned as updatePhone turns them.
    XrVector3f x{0, 0, 1}, y{0, 1, 0}, z{-1, 0, 0};
    float tilt = g_config.phoneTilt * 0.0174533f, c = std::cos(tilt), s = std::sin(tilt);
    XrVector3f y2{y.x * c - z.x * s, y.y * c - z.y * s, y.z * c - z.z * s};
    XrVector3f z2{y.x * s + z.x * c, y.y * s + z.y * c, y.z * s + z.z * c};
    turnPhone(x, z2);
    pitchPhone(y2, z2);
    const float* o = g_config.phoneHandOffset;
    move = {x.x * o[0] + y2.x * o[1] + z2.x * o[2], x.y * o[0] + y2.y * o[1] + z2.y * o[2],
            x.z * o[0] + y2.z * o[1] + z2.z * o[2]};
    auto about = [](const XrVector3f& v, const XrVector3f& k, float degrees) {
        float a = degrees * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
        XrVector3f kv = cross3(k, v);
        float kd = dot3(k, v) * (1 - ca);
        return XrVector3f{v.x * ca + kv.x * sa + k.x * kd, v.y * ca + kv.y * sa + k.y * kd,
                          v.z * ca + kv.z * sa + k.z * kd};
    };
    const float* t = g_config.phoneHandTurn;
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

bool phoneMount(float out[7]) {
    if (g_phoneHand != 1) return false;  // the game's prop hangs off the right hand only
    camera::Vec3 fwd, thumb, up;
    if (!armik::handAxes(1, fwd, thumb, up)) return false;
    const float* o = g_config.phoneOffset;
    float s = g_config.worldScale;
    float rx = o[0] - 0.02f, ry = o[1], rz = o[2] - 0.08f;
    out[0] = (-rz * fwd.x + ry * thumb.x + rx * up.x) * s;
    out[1] = (-rz * fwd.y + ry * thumb.y + rx * up.y) * s;
    out[2] = (-rz * fwd.z + ry * thumb.z + rx * up.z) * s;
    XrVector3f x{-fwd.x, -fwd.y, -fwd.z}, y{thumb.x, thumb.y, thumb.z}, z{-up.x, -up.y, -up.z};
    float tilt = g_config.phoneTilt * 0.0174533f, c = std::cos(tilt), sn = std::sin(tilt);
    XrVector3f y2{y.x * c - z.x * sn, y.y * c - z.y * sn, y.z * c - z.z * sn};
    XrVector3f z2{y.x * sn + z.x * c, y.y * sn + z.y * c, y.z * sn + z.z * c};
    turnPhone(x, z2);
    pitchPhone(y2, z2);
    XrQuaternionf q = fromAxes(x, y2, z2);
    out[3] = q.x;
    out[4] = q.y;
    out[5] = q.z;
    out[6] = q.w;
    return true;
}

void setGameMenu(bool open) {
    g_gameMenuReported = true;
    if (g_gameMenuOpen.exchange(open) != open) {
        static int logged = 0;
        if (logged++ < 40) LOG("Phone: game menu %s", open ? "open" : "closed");
    }
}

static bool gameMenuOpen() { return g_gameMenuReported ? g_gameMenuOpen.load() : g_phoneOpenedMenu; }

// A game popup (a modal dialog waiting for an answer; phone.py). It takes the
// game's input, so the controllers buzz like a phone (updatePopupBuzz), and
// taking the phone out shows it on the screen instead of opening the menu.
static std::atomic<bool> g_popupOpen{false}, g_popupNew{false};

void setPopup(bool open) {
    if (g_popupOpen.exchange(open) != open) {
        if (open) g_popupNew = true;
        static int logged = 0;
        if (logged++ < 40) LOG("Phone: game popup %s", open ? "open" : "gone");
    }
}

// A phone call of your Sim (nophone.py): the phone stays out until it ends, whatever the hand does.
void setCall(bool on) {
    if (g_onCall.exchange(on) != on) {
        static int logged = 0;
        if (logged++ < 40) LOG("Phone: call %s", on ? "started" : "ended");
    }
}

// Build/buy mode (buildbuy.py): in first person the phone gesture brings out
// the tablet, with the whole build/buy UI on its screen.
static std::atomic<bool> g_buildBuy{false};

void setBuildBuy(bool on) {
    if (g_buildBuy.exchange(on) != on) LOG("Build/buy mode: %s", on ? "on" : "off");
}

// Before the menu is measured: a tall box in the middle of the UI.
static void defaultPhoneCrop(int32_t W, int32_t H, int32_t crop[4]) {
    int32_t cw = std::max(16, std::min(W, static_cast<int32_t>(H * g_config.phoneCropWidth)));
    int32_t ch = std::max(16, std::min(H, static_cast<int32_t>(H * g_config.phoneCropHeight)));
    crop[0] = std::max(0, (W - cw) / 2);
    crop[1] = std::max(0, (H - ch) / 2);
    crop[2] = cw;
    crop[3] = ch;
}

// Finds the menu in a copy of the UI: the solid box in the middle, against
// the dimmed rest of the screen. Rows (and columns) count as menu where a
// band across the middle is mostly more opaque than halfway from the dimming
// to solid; the box grows out from the middle over gaps up to 2% of the height.
static void fitMenu(const uint8_t* pixels, UINT pitch, int W, int H) {
    auto alpha = [&](int x, int y) { return static_cast<int>(pixels[static_cast<size_t>(y) * pitch + x * 4 + 3]); };
    std::vector<uint8_t> edge;
    int step = std::max(1, H / 200);
    for (int y = H * 3 / 10; y < H * 7 / 10; y += step) {
        edge.push_back(static_cast<uint8_t>(alpha(W * 4 / 100, y)));
        edge.push_back(static_cast<uint8_t>(alpha(W * 96 / 100, y)));
    }
    int dim = 0;
    if (!edge.empty()) {
        std::nth_element(edge.begin(), edge.begin() + edge.size() / 2, edge.end());
        dim = edge[edge.size() / 2];
    }
    int threshold = dim + (255 - dim) / 2;
    int xa = W * 45 / 100, xb = W * 55 / 100, ya = H * 40 / 100, yb = H * 60 / 100;
    int needRow = std::max(1, (xb - xa) / 10), needColumn = std::max(1, (yb - ya) / 10);
    int maxGap = std::max(1, H * 2 / 100);
    auto rowOn = [&](int y) {
        int n = 0;
        for (int x = xa; x < xb; ++x) n += alpha(x, y) > threshold;
        return n >= needRow;
    };
    auto columnOn = [&](int x) {
        int n = 0;
        for (int y = ya; y < yb; ++y) n += alpha(x, y) > threshold;
        return n >= needColumn;
    };
    // The run of "on" lines nearest the middle, grown out both ways.
    auto extent = [&](auto on, int size, int& lo, int& hi) {
        int centre = size / 2, start = -1;
        for (int d = 0; d <= size / 10 && start < 0; ++d) {
            if (on(centre - d))
                start = centre - d;
            else if (on(centre + d))
                start = centre + d;
        }
        if (start < 0) return false;
        lo = hi = start;
        for (int i = start - 1, gap = 0; i >= 0; --i) {
            if (on(i))
                lo = i, gap = 0;
            else if (++gap > maxGap)
                break;
        }
        for (int i = start + 1, gap = 0; i < size; ++i) {
            if (on(i))
                hi = i, gap = 0;
            else if (++gap > maxGap)
                break;
        }
        return true;
    };
    int top = 0, bottom = 0, left = 0, right = 0;
    const char* failed = nullptr;
    if (dim > 235)
        failed = "the whole UI is solid";
    else if (!extent(rowOn, H, top, bottom) || !extent(columnOn, W, left, right))
        failed = "nothing solid in the middle";
    int margin = H * 5 / 1000;
    int x0 = std::max(0, left - margin), y0 = std::max(0, top - margin);
    int x1 = std::min(W, right + 1 + margin), y1 = std::min(H, bottom + 1 + margin);
    if (!failed && (x1 - x0 < H / 10 || y1 - y0 < H / 10)) failed = "too small";
    if (!failed && x1 - x0 > W * 8 / 10) failed = "too wide";
    static int logged = 0;
    if (failed) {
        if (logged++ < 20)
            LOG("Phone: couldn't find the menu in the UI (%s; edges alpha %d, threshold %d); showing %s", failed, dim,
                threshold, g_phoneFound ? "the last one found" : "the middle");
        g_menuFitOk = false;
        return;
    }
    g_phoneCrop[0] = x0;
    g_phoneCrop[1] = y0;
    g_phoneCrop[2] = x1 - x0;
    g_phoneCrop[3] = y1 - y0;
    g_phoneFound = true;
    g_menuFitOk = true;
    if (logged++ < 20)
        LOG("Phone: menu found at %d,%d, %dx%d of the %dx%d UI (edges alpha %d, threshold %d)", x0, y0, x1 - x0, y1 - y0,
            W, H, dim, threshold);
}

// Copies the UI for fitMenu; pollPhoneFit reads it once the GPU is done.
static void startPhoneFit() {
    if (!g_uiFrame || g_phoneFitPending) return;
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    if (!alphaReadable(desc)) {
        g_phoneFitted = true;  // can't read it: keep the middle
        return;
    }
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (g_phoneFitCopy) {
        D3D11_TEXTURE2D_DESC have;
        g_phoneFitCopy->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
            g_phoneFitCopy->Release();
            g_phoneFitCopy = nullptr;
        }
    }
    if (!g_phoneFitCopy) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.MipLevels = staging.ArraySize = 1;
        staging.SampleDesc.Count = 1;
        staging.SampleDesc.Quality = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        device->CreateTexture2D(&staging, nullptr, &g_phoneFitCopy);
    }
    if (g_phoneFitCopy) {
        context->CopySubresourceRegion(g_phoneFitCopy, 0, 0, 0, 0, g_uiFrame, 0, nullptr);
        g_phoneFitPending = true;
        g_phoneFitWait = 0;
    } else {
        g_phoneFitted = true;
    }
    context->Release();
    device->Release();
}

static void pollPhoneFit() {
    if (!g_phoneFitPending || !g_phoneFitCopy) return;
    ID3D11Device* device = nullptr;
    g_phoneFitCopy->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(g_phoneFitCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++g_phoneFitWait < 60) {
        // not copied yet; next frame
    } else {
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc;
            g_phoneFitCopy->GetDesc(&desc);
            fitMenu(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, static_cast<int>(desc.Width),
                    static_cast<int>(desc.Height));
            context->Unmap(g_phoneFitCopy, 0);
        }
        g_phoneFitPending = false;
        g_phoneFitted = true;
        ++g_menuFitDone;
    }
    context->Release();
    device->Release();
}

