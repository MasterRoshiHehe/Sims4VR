// ---------------------------------------------------------------------------
// Pie menu at your hand (first person). The game draws its pie menu into the
// UI around where the mouse was when you clicked. That part of the UI gets its
// own quad where you clicked, within reach, and the full panel hides while it
// is open: point at an option and pull the trigger, or poke it with a finger.
// Python says when the game builds a pie menu for a click in the world and
// when you pick from it; a click anywhere else or Escape also closes it.

struct PieMenu {
    bool active = false;
    XrPosef pose{};              // app space, facing you
    XrVector3f x{}, y{}, z{};    // its axes; z points at you
    float width = 0, height = 0; // metres
    int32_t x0 = 0, y0 = 0, cw = 0, ch = 0;  // the part of the UI it shows, in pixels
    uint32_t uiW = 0, uiH = 0;
    ULONGLONG opened = 0;
    bool armed[2] = {false, false};  // a finger came from in front (pokes)
    int pressing = -1;               // the finger pressing it
    bool solid = false;              // the tablet: the world is behind all of it, so its empty parts take
                                     // the laser and fingers but don't click or move the mouse
    // The tablet: pieces of the UI laid out on the sheet (phone3d::Phone has
    // the same); 0 = the crop above. Places 0..1 from the top left, parts in UI pixels.
    int tiles = 0;
    float tileDst[phone3d::kMaxTiles][4]{}, tileSrc[phone3d::kMaxTiles][4]{};
    bool own = false;  // the pie drawn by us (hud kPie, the HUD sheet kHudPie in its place), not the game's cut
};
static PieMenu g_pie;
static bool g_pieWorldSet = false;  // pieInWorld holds the open pie in the world
static std::atomic<bool> g_pieOpenRequest{false}, g_pieCloseRequest{false};

// The last left click into the world (not on the UI): where the mouse was and
// where the laser ended.
struct WorldClick {
    bool valid = false;
    float u = 0, v = 0;
    XrVector3f point{};
    ULONGLONG tick = 0;
};
static WorldClick g_lastWorldClick;

// A finger near the pie: the laser marker shows where it would press, without the beam.
static bool g_tipOnly = false;

static constexpr float kPieOpaque = 0.04f;    // UI alpha that counts as part of the menu
static constexpr float kPieNearMiss = 0.02f;  // metres off an option that still count as on the menu
static constexpr float kPokeHover = 0.06f;    // metres in front of the pie at which a finger takes the mouse
static constexpr float kPokeRelease = 0.015f; // metres back out in front that let go
static constexpr float kPokeThrough = 0.12f;  // metres behind it that let go too

static std::atomic<int> g_clickHand{-1};
static std::atomic<ULONGLONG> g_clickHandTick{0};
static std::atomic<bool> g_pieShown{false};

void pieOpened(int items) {
    if (items > 0) g_pieOpenRequest = true;
}
int clickHand() {
    return GetTickCount64() - g_clickHandTick.load(std::memory_order_relaxed) < 5000
               ? g_clickHand.load(std::memory_order_relaxed)
               : -1;
}
bool pieShown() { return g_pieShown.load(std::memory_order_relaxed); }
void pieClosed() { g_pieCloseRequest = true; }

// A click within this long of the pie opening closes nothing: it is still
// the press that opened it (the trigger flickering as it is held).
static constexpr ULONGLONG kPieClickSettle = 300;  // ms
static bool pieSettled() { return GetTickCount64() - g_pie.opened >= kPieClickSettle; }

static void closePie(const char* why) {
    if (!g_pie.active) return;
    g_pie.active = false;
    g_pieShown.store(false, std::memory_order_relaxed);
    if (g_pie.own) hud::clearPie();
    g_pie.own = false;
    static int logged = 0;
    if (logged++ < 20) LOG("Pie menu at hand: closed (%s)", why);
}

// A press off the pie closes it: the sheet here and, with Escape, the game's
// menu. The press itself never reaches the world (it would open a new menu).
static int g_escapeFrames = 0;
static int g_buildKeyFrames = 0;  // the HUD's build button: F2, the game's Build Mode key
static bool g_pieDismissHold = false;  // that press, until the triggers let go
static ULONGLONG g_pieEscapeTick = 0;  // when dismissPie last pressed Escape
static void dismissPie(const char* why) {
    if (!g_pie.active) return;
    closePie(why);
    g_escapeFrames = 3;
    g_pieEscapeTick = GetTickCount64();
    g_pieDismissHold = true;
}

// The near-miss distance in UI pixels on the pie sheet.
static int pieNearPixels() {
    if (!(g_pie.width > 0)) return 0;
    return static_cast<int>(kPieNearMiss * g_pie.cw / g_pie.width + 0.5f);
}

// The UI part the pie was opened on (x0, y0, w, h) and the pie's centre in it (the click), in UI pixels.
static int32_t g_pieFull[4] = {}, g_pieCentre[2] = {};

// Opens the pie quad for the last world click, seen from your head.
static bool openPie(const XrVector3f& head) {
    const WorldClick& click = g_lastWorldClick;
    if (!g_uiFrame || !click.valid) return false;
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t cw = std::min(W, static_cast<int32_t>(H * g_config.pieCrop));
    // PieMenuBase: the nubs sit at most 140 stage px above/below the centre (re/liveui/components/PieMenu.md),
    // plus a button's height: about a quarter of the stage's height in all; trimPie fits the rest to what is drawn.
    int32_t ch = std::min(H, std::min(cw * 2 / 3, H * 30 / 100));
    if (cw < 16 || ch < 16) return false;
    int32_t x0 = static_cast<int32_t>(click.u * W) - cw / 2, y0 = static_cast<int32_t>(click.v * H) - ch / 2;
    x0 = std::max(0, std::min(W - cw, x0));
    y0 = std::max(0, std::min(H - ch, y0));

    XrVector3f to = sub3(click.point, head);
    float distance = std::sqrt(dot3(to, to));
    if (!(distance > 0.05f)) return false;
    XrVector3f dir{to.x / distance, to.y / distance, to.z / distance};
    float d = std::max(0.25f, std::min(g_config.pieDistance, distance - 0.08f));
    XrVector3f z{-dir.x, -dir.y, -dir.z};
    XrVector3f x = std::fabs(z.y) > 0.99f ? XrVector3f{1, 0, 0} : normalize3(cross3(XrVector3f{0, 1, 0}, z));
    XrVector3f y = cross3(z, x);

    g_pie.active = true;
    g_pieWorldSet = false;
    g_pieShown.store(true, std::memory_order_relaxed);
    g_pie.x = x;
    g_pie.y = y;
    g_pie.z = z;
    g_pie.pose = {fromAxes(x, y, z), {head.x + dir.x * d, head.y + dir.y * d, head.z + dir.z * d}};
    g_pie.width = g_config.pieWidth;
    g_pie.height = g_config.pieWidth * ch / cw;
    g_pie.x0 = x0;
    g_pie.y0 = y0;
    g_pie.cw = cw;
    g_pie.ch = ch;
    g_pie.uiW = ui.Width;
    g_pie.uiH = ui.Height;
    g_pie.opened = GetTickCount64();
    g_pieFull[0] = x0, g_pieFull[1] = y0, g_pieFull[2] = cw, g_pieFull[3] = ch;
    // Drawn by us when pie.py sent the game's options for it (its texts may still be looked up).
    g_pie.own = hud::pieState() == 1 || hud::pieState() == 2;
    g_pieCentre[0] = std::max(0, std::min(W - 1, static_cast<int32_t>(click.u * W)));
    g_pieCentre[1] = std::max(0, std::min(H - 1, static_cast<int32_t>(click.v * H)));
    static int logged = 0;
    if (logged++ < 20)
        LOG("Pie menu at hand: opened %.2f m away (click %.0f m away), UI %dx%d at %d,%d of %dx%d", d, distance, cw,
            ch, x0, y0, W, H);
    return true;
}

// The pie's sheet fitted to what the game actually drew (user 2026-10-22: boxes in the screen's lower
// corners bled into the cut): every 200 ms the opened part's alpha is read back (a frame late, no wait),
// the opaque 8 px cells joined to the ring around the click are kept (gaps up to 2 cells, so labels stay
// whole) and the sheet shrinks to their box, keeping its metres-per-pixel and where each pixel sits.
static void trimPie() {
    static ID3D11Texture2D* staging = nullptr;
    static UINT stagingW = 0, stagingH = 0;
    static bool pending = false;
    static ULONGLONG lastCopy = 0;
    static int32_t copied[4] = {};
    if (!g_pie.active || !g_uiFrame || g_pie.tiles || g_pie.own) {
        pending = false;
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    if (!alphaReadable(desc)) return;
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const int32_t* f = g_pieFull;
    if (pending && staging) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            pending = false;
            constexpr int kCell = 8;
            const int gw = (copied[2] + kCell - 1) / kCell, gh = (copied[3] + kCell - 1) / kCell;
            std::vector<uint8_t> cell(static_cast<size_t>(gw) * gh, 0);
            const uint8_t* px = static_cast<const uint8_t*>(mapped.pData);
            const uint8_t opaque = static_cast<uint8_t>(kPieOpaque * 255);
            for (int y = 0; y < copied[3]; ++y)
                for (int x = 0; x < copied[2]; ++x)
                    if (px[y * mapped.RowPitch + x * 4 + 3] > opaque) cell[(y / kCell) * gw + x / kCell] = 1;
            context->Unmap(staging, 0);
            // Seeds: opaque cells within the ring (about 15% of the UI's height from the centre).
            const int cx = (g_pieCentre[0] - copied[0]) / kCell, cy = (g_pieCentre[1] - copied[1]) / kCell;
            const int ring = static_cast<int>(desc.Height * 0.15f) / kCell;
            std::vector<int> stack;
            for (int y = std::max(0, cy - ring); y <= std::min(gh - 1, cy + ring); ++y)
                for (int x = std::max(0, cx - ring); x <= std::min(gw - 1, cx + ring); ++x)
                    if (cell[y * gw + x] == 1) cell[y * gw + x] = 2, stack.push_back(y * gw + x);
            int bx0 = gw, by0 = gh, bx1 = -1, by1 = -1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % gw, y = i / gw;
                bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || cell[ny * gw + nx] != 1) continue;
                        cell[ny * gw + nx] = 2;
                        stack.push_back(ny * gw + nx);
                    }
            }
            if (bx1 >= 0 && copied[0] == f[0] && copied[1] == f[1]) {
                const int pad = 12;
                int32_t nx0 = std::max(f[0], copied[0] + bx0 * kCell - pad), ny0 = std::max(f[1], copied[1] + by0 * kCell - pad);
                int32_t nx1 = std::min(f[0] + f[2], copied[0] + (bx1 + 1) * kCell + pad);
                int32_t ny1 = std::min(f[1] + f[3], copied[1] + (by1 + 1) * kCell + pad);
                if (nx1 - nx0 >= 32 && ny1 - ny0 >= 32 &&
                    (nx0 != g_pie.x0 || ny0 != g_pie.y0 || nx1 - nx0 != g_pie.cw || ny1 - ny0 != g_pie.ch)) {
                    // Same metres per pixel; the sheet's centre moves with the part's centre.
                    const float k = g_pie.width / std::max(1, g_pie.cw);
                    const float sx = ((nx0 + nx1) * 0.5f - (g_pie.x0 + g_pie.cw * 0.5f)) * k;
                    const float sy = ((ny0 + ny1) * 0.5f - (g_pie.y0 + g_pie.ch * 0.5f)) * k;
                    XrVector3f& c = g_pie.pose.position;
                    c = {c.x + g_pie.x.x * sx - g_pie.y.x * sy, c.y + g_pie.x.y * sx - g_pie.y.y * sy,
                         c.z + g_pie.x.z * sx - g_pie.y.z * sy};
                    g_pie.x0 = nx0, g_pie.y0 = ny0, g_pie.cw = nx1 - nx0, g_pie.ch = ny1 - ny0;
                    g_pie.width = g_pie.cw * k;
                    g_pie.height = g_pie.ch * k;
                    static int logged = 0;
                    if (logged++ < 20) LOG("Pie menu at hand: fitted to what is drawn, UI %dx%d at %d,%d", g_pie.cw, g_pie.ch, nx0, ny0);
                }
            }
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (!pending && now - lastCopy >= 200 && f[2] > 0 && f[3] > 0) {
        if (!staging || stagingW != static_cast<UINT>(f[2]) || stagingH != static_cast<UINT>(f[3]) ) {
            if (staging) staging->Release(), staging = nullptr;
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = stagingW = static_cast<UINT>(f[2]);
            sd.Height = stagingH = static_cast<UINT>(f[3]);
            sd.MipLevels = sd.ArraySize = 1;
            sd.Format = desc.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            device->CreateTexture2D(&sd, nullptr, &staging);
        }
        if (staging) {
            D3D11_BOX box{static_cast<UINT>(f[0]), static_cast<UINT>(f[1]), 0, static_cast<UINT>(f[0] + f[2]),
                          static_cast<UINT>(f[1] + f[3]), 1};
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, g_uiFrame, 0, &box);
            std::copy(f, f + 4, copied);
            pending = true;
            lastCopy = now;
        }
    }
    context->Release();
    device->Release();
}

// A point in a sheet's frame: x right, y up, z toward you (metres).
static void sheetLocal(const PieMenu& m, const XrVector3f& p, float& lx, float& ly, float& lz) {
    XrVector3f r = sub3(p, m.pose.position);
    lx = dot3(r, m.x);
    ly = dot3(r, m.y);
    lz = dot3(r, m.z);
}

static bool insideSheet(const PieMenu& m, float lx, float ly, float margin) {
    return std::fabs(lx) <= m.width / 2 + margin && std::fabs(ly) <= m.height / 2 + margin;
}

// Sheet position to 0..1 window coordinates (the UI covers the window).
// On the tablet, between its pieces: u = v = -1 (nothing there).
static void sheetToWindow(const PieMenu& m, float lx, float ly, float& u, float& v) {
    float a = std::max(0.0f, std::min(1.0f, lx / m.width + 0.5f));
    float b = std::max(0.0f, std::min(1.0f, 0.5f - ly / m.height));
    if (m.tiles > 0) {
        u = v = -1;
        for (int k = 0; k < m.tiles; ++k) {
            const float* d = m.tileDst[k];
            const float* s = m.tileSrc[k];
            if (a < d[0] || a > d[2] || b < d[1] || b > d[3]) continue;
            float fa = (a - d[0]) / std::max(d[2] - d[0], 1e-6f), fb = (b - d[1]) / std::max(d[3] - d[1], 1e-6f);
            u = (s[0] + fa * (s[2] - s[0])) / static_cast<float>(m.uiW);
            v = (s[1] + fb * (s[3] - s[1])) / static_cast<float>(m.uiH);
        }
        return;
    }
    u = (m.x0 + a * m.cw) / static_cast<float>(m.uiW);
    v = (m.y0 + b * m.ch) / static_cast<float>(m.uiH);
}

// The laser against a sheet: true with the hit point and window coordinates.
static bool hitSheet(const PieMenu& m, const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    float facing = dot3(d, m.z);
    if (facing > -0.01f) return false;
    float t = -dot3(sub3(o, m.pose.position), m.z) / facing;
    if (t <= 0) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    float lx, ly, lz;
    sheetLocal(m, hit, lx, ly, lz);
    if (!insideSheet(m, lx, ly, 0)) return false;
    sheetToWindow(m, lx, ly, u, v);
    return true;
}

// Where your index fingertip is: 10 cm ahead of the grip, along the hand
// (the grip's -Z tilted down by hand_pitch, as armik poses the Sim's hand).
static XrVector3f fingertip(const armik::Hand& hand) {
    float half = -g_config.handPitch * 0.5f * 3.14159265f / 180.0f;
    float s = std::sin(half), c = std::cos(half);
    const camera::Quat& q = hand.orientation;
    XrQuaternionf r{q.w * s + q.x * c, q.y * c + q.z * s, q.z * c - q.y * s, q.w * c - q.x * s};  // q * rotX(-pitch)
    XrVector3f f = forwardOf(r);
    return {hand.position.x + f.x * 0.10f, hand.position.y + f.y * 0.10f, hand.position.z + f.z * 0.10f};
}

// Your Sim's own index fingertip, where armik last posed it, carried along
// with the controller (so it doesn't lag behind): a poke comes when the
// finger you see reaches the glass. The 10 cm guess while the Sim's hand
// isn't yours (god cam) or hasn't been posed.
static XrVector3f simFingertip(const armik::Hand& hand, int side) {
    camera::Vec3 local;
    if (!armik::fingertip(side, local)) return fingertip(hand);
    float half = -g_config.handPitch * 0.5f * 3.14159265f / 180.0f;
    float s = std::sin(half), c = std::cos(half);
    const camera::Quat& q = hand.orientation;
    XrQuaternionf r{q.w * s + q.x * c, q.y * c + q.z * s, q.z * c - q.y * s, q.w * c - q.x * s};  // q * rotX(-pitch)
    XrVector3f v = rotate3(r, {local.x, local.y, local.z});
    static bool logged[2] = {};
    if (!logged[side]) {
        logged[side] = true;
        LOG("Poke: your Sim's %s index fingertip is %.1f cm ahead of the grip, %.1f cm toward the thumb, %.1f cm "
            "toward the back of the hand (the guess was 10 cm ahead)",
            side ? "right" : "left", -local.z * 100, local.y * 100, local.x * (side ? 100 : -100));
    }
    return {hand.position.x + v.x, hand.position.y + v.y, hand.position.z + v.z};
}

// Each hand's speed (app space, m/s), kept up every frame by shovePie.
static XrVector3f g_handVelocity[2];
static void flingPie(const XrVector3f& velocity);
static void buzz(int hand, float amplitude, float seconds);

// An empty hand: no grip, trigger or button held (a slap, not pointing to press).
static bool emptyHand(int i) {
    return floatState(g_gripAction, i) <= 0.2f && floatState(g_triggerAction, i) <= 0.2f &&
           !boolState(g_primaryAction, i) && !boolState(g_secondaryAction, i);
}
static constexpr float kSlapThrough = 0.6f;  // m/s: an empty hand going through the pie this fast flings it

// Pokes: a finger that comes from in front of a sheet and crosses it presses
// the left button there, and lets go when it comes back out. Fills the mouse
// target and button; false when no finger is near the sheet. `use` says which
// hands may poke (the hand carrying a wrist sheet can't); `pulled`, which
// triggers were just pulled; simTip, poke with the Sim's own fingertip (the
// phone, drawn in the game beside it). A poke or pull off the pie closes it.
static bool pokeSheet(PieMenu& m, const armik::Hand hands[2], const bool use[2], const bool trigger[2],
                      const bool pulled[2], pointer::Controls& c, bool simTip = false) {
    int best = -1;
    float bestZ = 1e9f, bx = 0, by = 0;
    XrVector3f bestTip{};
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid || !use[i]) {
            m.armed[i] = false;
            continue;
        }
        XrVector3f tip = simTip ? simFingertip(hands[i], i) : fingertip(hands[i]);
        float lx, ly, lz;
        sheetLocal(m, tip, lx, ly, lz);
        bool inRange = insideSheet(m, lx, ly, 0.02f) && lz < kPokeHover && lz > -kPokeThrough;
        if (!inRange) {
            m.armed[i] = false;
            continue;
        }
        if (lz > 0.005f) m.armed[i] = true;
        // The pressing finger keeps the mouse; otherwise the one closest to the sheet.
        float rank = i == m.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = i;
            bx = lx;
            by = ly;
            bestTip = tip;
        }
    }
    if (best < 0) {
        m.pressing = -1;
        return false;
    }
    float lx, ly, lz;
    sheetLocal(m, bestTip, lx, ly, lz);
    float u, v;
    sheetToWindow(m, bx, by, u, v);
    const bool pie = &m == &g_pie;
    probeUiAlpha(g_uiFrame, u, v, pie ? pieNearPixels() : 0);
    // Off the pie: not even a near miss of an option.
    const bool offPie = pie && g_uiAlphaNearLaser <= kPieOpaque;
    if (m.pressing == best) {
        if (lz > kPokeRelease) m.pressing = -1;
    } else if (lz <= 0 && m.armed[best] && g_uiAlphaUnderLaser > kPieOpaque) {
        m.pressing = best;  // only on the menu itself
        m.armed[best] = false;
    } else if (lz <= 0 && m.armed[best] && offPie) {
        m.armed[best] = false;
        // An empty hand swung through it (a backhand reaches it with the
        // fingers before the palm is fast enough to count as a shove) flings it.
        const XrVector3f& hv = g_handVelocity[best];
        if (pie && emptyHand(best) && dot3(hv, hv) > kSlapThrough * kSlapThrough) {
            flingPie(hv);
            dismissPie("slapped through");
            buzz(best, 0.4f, 0.04f);
        } else {
            dismissPie("poked outside");
        }
    }
    if (pulled[best] && offPie) dismissPie("pulled outside");
    if (m.pressing != best && m.pressing >= 0) m.pressing = -1;
    // An empty part of the tablet: the mouse stays where it was (over the
    // world, where whatever you are placing waits).
    const bool blank = m.solid && g_uiAlphaUnderLaser <= kPieOpaque;
    c.hasTarget = !blank;
    c.u = u;
    c.v = v;
    // Pulling that hand's trigger clicks where the finger is, too (a poke
    // can miss); not between a pie's options, where it would close the pie.
    c.leftButton = m.pressing == best || (trigger[best] && !blank && (!pie || g_uiAlphaUnderLaser > kPieOpaque));
    // The marker sits on the sheet under your finger.
    XrVector3f onSheet{m.pose.position.x + m.x.x * bx + m.y.x * by, m.pose.position.y + m.x.y * bx + m.y.y * by,
                       m.pose.position.z + m.x.z * bx + m.y.z * by};
    g_laserTo = onSheet;
    g_laserFrom = {onSheet.x + m.z.x * 0.1f, onSheet.y + m.z.y * 0.1f, onSheet.z + m.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}

// A sheet's layer: the panel's swapchain, cropped to the part it shows.
static void sheetLayer(const PieMenu& m, const XrCompositionLayerQuad& panel, XrCompositionLayerQuad& layer) {
    layer = panel;
    int32_t W = static_cast<int32_t>(g_uiSwapchain.width), H = static_cast<int32_t>(g_uiSwapchain.height);
    int32_t cw = std::min(m.cw, W), ch = std::min(m.ch, H);
    layer.subImage.imageRect.offset = {std::max(0, std::min(W - cw, m.x0)), std::max(0, std::min(H - ch, m.y0))};
    layer.subImage.imageRect.extent = {cw, ch};
    layer.pose = m.pose;
    layer.size = {m.width, m.height};
}

// The pie shoved away (first person): a still of it flies off the way your
// hand went, tumbling and fading, while the game's own closes.
struct PieFling {
    bool active = false;
    ULONGLONG start = 0;
    XrVector3f centre{}, x{}, y{}, z{}, velocity{};  // app space
    float width = 0, height = 0;
};
static PieFling g_fling;
static ID3D11Texture2D* g_flingStill = nullptr;  // the pie's part of the UI when it was shoved
static constexpr float kFlingSeconds = 0.4f;

// Keep a still of the pie as it is now and start it flying (velocity: the
// shoving hand's, m/s, app space). Present thread, before the game's pie goes.
static void flingPie(const XrVector3f& velocity) {
    g_fling.active = false;
    // The pie drawn by us: its part of the HUD atlas; else the game's UI cut.
    ID3D11Texture2D* source = g_pie.own ? hud::texture() : g_uiFrame;
    const int32_t sx = g_pie.own ? static_cast<int32_t>(hud::kPie.x) : g_pie.x0,
                  sy = g_pie.own ? static_cast<int32_t>(hud::kPie.y) : g_pie.y0,
                  sw = g_pie.own ? static_cast<int32_t>(hud::kPie.w) : g_pie.cw,
                  sh = g_pie.own ? static_cast<int32_t>(hud::kPie.h) : g_pie.ch;
    if (!source || !camera::bodyActive() || sw <= 0 || sh <= 0) return;
    D3D11_TEXTURE2D_DESC ui;
    source->GetDesc(&ui);
    if (ui.SampleDesc.Count != 1) return;
    const UINT x0 = static_cast<UINT>(std::max(0, sx)), y0 = static_cast<UINT>(std::max(0, sy));
    const UINT w = std::min(static_cast<UINT>(sw), ui.Width - std::min(x0, ui.Width));
    const UINT h = std::min(static_cast<UINT>(sh), ui.Height - std::min(y0, ui.Height));
    if (!w || !h) return;
    ID3D11Device* device = nullptr;
    source->GetDevice(&device);
    if (!device) return;
    D3D11_TEXTURE2D_DESC have{};
    if (g_flingStill) g_flingStill->GetDesc(&have);
    if (!g_flingStill || have.Width != w || have.Height != h || have.Format != ui.Format) {
        if (g_flingStill) g_flingStill->Release();
        g_flingStill = nullptr;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = ui.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g_flingStill))) g_flingStill = nullptr;
    }
    if (g_flingStill) {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        D3D11_BOX box{x0, y0, 0, x0 + w, y0 + h, 1};
        context->CopySubresourceRegion(g_flingStill, 0, 0, 0, 0, source, 0, &box);
        context->Release();
        g_fling.active = true;
        g_fling.start = GetTickCount64();
        g_fling.centre = g_pie.pose.position;
        g_fling.x = g_pie.x;
        g_fling.y = g_pie.y;
        g_fling.z = g_pie.z;
        g_fling.width = g_pie.width * w / static_cast<float>(sw);
        g_fling.height = g_pie.height * h / static_cast<float>(sh);
        // At least a brisk throw, at most a fast one, the way the hand went.
        float speed = std::sqrt(dot3(velocity, velocity));
        float k = speed > 0.01f ? std::min(std::max(speed, 2.0f), 4.0f) / speed : 0.0f;
        g_fling.velocity = {velocity.x * k, velocity.y * k, velocity.z * k};
    }
    device->Release();
}

// Rotate v about the unit axis a by angle (radians).
static XrVector3f turnAbout(const XrVector3f& v, const XrVector3f& a, float angle) {
    float c = std::cos(angle), s = std::sin(angle);
    XrVector3f axv = cross3(a, v);
    float d = dot3(a, v) * (1 - c);
    return {v.x * c + axv.x * s + a.x * d, v.y * c + axv.y * s + a.y * d, v.z * c + axv.z * s + a.z * d};
}

// First person: the pie drawn inside the game's 3D view (phone3d), clear
// around it, so your hands and anything nearer pass in front of it like the
// phone and the menus on objects; then, shoved away, its still flying off.
// Elsewhere, or when it can't be drawn there, it stays the flat layer.
// Present thread, each frame.
static bool pieInWorld(bool show) {
    camera::View view;
    float t = g_fling.active ? (GetTickCount64() - g_fling.start) / 1000.0f : 0.0f;
    if (show || t >= kFlingSeconds) g_fling.active = false;
    const bool flying = g_fling.active && g_flingStill;
    // The pie drawn by us comes from the HUD atlas (hud::kPie), the game's cut from its UI.
    const bool own = show && g_pie.own;
    ID3D11Texture2D* source = own ? hud::texture() : g_uiFrame;
    if ((!show && !flying) || !camera::bodyActive() || !camera::lastView(view) ||
        (show && (!source || (!own && (!g_pie.uiW || !g_pie.uiH)) || !(g_pie.width > 0)))) {
        phone3d::setPie(phone3d::Panel{}, nullptr);
        return false;
    }
    XrVector3f at = g_pie.pose.position, ax = g_pie.x, ay = g_pie.y;
    float width = g_pie.width, height = g_pie.height, opacity = 1;
    if (flying) {
        // Out along the throw, slowing; tumbling about the axis across it; fading out.
        float f = t / kFlingSeconds;
        float travel = t * (1 - 0.5f * f);
        const XrVector3f& v = g_fling.velocity;
        at = {g_fling.centre.x + v.x * travel, g_fling.centre.y + v.y * travel, g_fling.centre.z + v.z * travel};
        XrVector3f spin = cross3(g_fling.z, v);
        float n = std::sqrt(dot3(spin, spin));
        ax = g_fling.x;
        ay = g_fling.y;
        if (n > 1e-4f) {
            spin = {spin.x / n, spin.y / n, spin.z / n};
            ax = turnAbout(ax, spin, 2.2f * f);
            ay = turnAbout(ay, spin, 2.2f * f);
        }
        width = g_fling.width * (1 - 0.25f * f);
        height = g_fling.height * (1 - 0.25f * f);
        opacity = (1 - f) * (1 - f);
    }
    const float scale = view.scale;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, ax), wy = rotate3(heading, ay);
    // Open (not flying): held in the world where it first showed (user 2026-10-22 on 344: a two-step
    // shake when a turn stops, app space put into the world with a camera a frame or two off the
    // scene's). Drawn in world coordinates it moves with the scene exactly; its app-space pose (laser,
    // pokes, shove, the HUD sheet) follows from this frame's camera.
    static XrVector3f worldAt, worldX, worldY;
    static float worldScale = 1;
    const bool held = show && !flying && g_pieWorldSet;
    if (show && !flying && !held) {
        worldAt = {view.anchor.x + wc.x, view.anchor.y + wc.y, view.anchor.z + wc.z};
        worldX = wx;
        worldY = wy;
        worldScale = scale;
        g_pieWorldSet = true;
    } else if (held && scale > 0) {
        const XrQuaternionf back{-heading.x, -heading.y, -heading.z, heading.w};
        const XrVector3f rel = rotate3(back, {worldAt.x - view.anchor.x, worldAt.y - view.anchor.y, worldAt.z - view.anchor.z});
        g_pie.x = rotate3(back, worldX);
        g_pie.y = rotate3(back, worldY);
        g_pie.z = cross3(g_pie.x, g_pie.y);
        g_pie.pose = {fromAxes(g_pie.x, g_pie.y, g_pie.z), {rel.x / scale, rel.y / scale, rel.z / scale}};
    }
    phone3d::Panel panel;
    panel.show = true;
    panel.clear = true;
    panel.opacity = opacity;
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = width * scale;
    panel.height = height * scale;
    if (show && !flying) {
        // In the world as held (the size too: a scale change doesn't resize it under your hand).
        const float k = scale > 0 ? worldScale / scale : 1.0f;
        panel.centre[0] = worldAt.x, panel.centre[1] = worldAt.y, panel.centre[2] = worldAt.z;
        panel.x[0] = worldX.x, panel.x[1] = worldX.y, panel.x[2] = worldX.z;
        panel.y[0] = worldY.x, panel.y[1] = worldY.y, panel.y[2] = worldY.z;
        panel.width = width * scale * k;
        panel.height = height * scale * k;
    } else
        panel.app = true;
    panel.appAt[0] = at.x, panel.appAt[1] = at.y, panel.appAt[2] = at.z;
    panel.appX[0] = ax.x, panel.appX[1] = ax.y, panel.appX[2] = ax.z;
    panel.appY[0] = ay.x, panel.appY[1] = ay.y, panel.appY[2] = ay.z;
    panel.appW = width;
    panel.appH = height;
    if (flying) {
        const float whole[4] = {0, 0, 1, 1};
        std::memcpy(panel.crop, whole, sizeof(whole));
        phone3d::setPie(panel, g_flingStill);
        return false;
    }
    if (own) {
        const float W = static_cast<float>(hud::kWidth), H = static_cast<float>(hud::kHeight);
        panel.crop[0] = hud::kPie.x / W;
        panel.crop[1] = hud::kPie.y / H;
        panel.crop[2] = (hud::kPie.x + hud::kPie.w) / W;
        panel.crop[3] = (hud::kPie.y + hud::kPie.h) / H;
    } else {
        const float W = static_cast<float>(g_pie.uiW), H = static_cast<float>(g_pie.uiH);
        panel.crop[0] = g_pie.x0 / W;
        panel.crop[1] = g_pie.y0 / H;
        panel.crop[2] = (g_pie.x0 + g_pie.cw) / W;
        panel.crop[3] = (g_pie.y0 + g_pie.ch) / H;
    }
    phone3d::setPie(panel, source);
    static int logged = 0;
    bool drawn = phone3d::pieDrawnRecently();
    if (drawn && logged++ < 5) LOG("Pie: drawn in the world (your hands in front of it)");
    return drawn;
}

