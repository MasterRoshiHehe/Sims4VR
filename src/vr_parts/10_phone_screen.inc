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

// The tablet's layout, like a painter's palette (user's mockup, 2026-10-14):
// the build/buy UI's pieces (they sit against the screen's edges with the
// world between them) packed together:
//   the tool bar and the mode buttons along the top;
//   the catalog's search box and the lot name under them;
//   the catalog's left panel (categories, the house), the item tooltip beside it;
//   the catalog's items across the whole width, in two halves one above the other;
//   a list popped up from the left panel (categories) beside it, over the items.
// The phone's background picture (or a soft blue) shows around the pieces.
// Measured on the 2808 px high build/buy UI (game 1.121), scaled with the
// UI's height; the left panel's and the items' tops are found in the UI
// (the build catalog's panel is taller than buy's). When something solid
// stands in the middle of the UI (a dialog, the game menu), the tablet shows
// the whole UI as it is until it's gone.
struct TabletScan {
    bool middle = false;     // something solid in the middle of the UI
    bool tip = false;        // the item tooltip, in UI pixels:
    int tipBox[4] = {};      // x0, y0, x1, y1
    bool popup = false;      // a list popped up over the catalog's left panel (categories), in UI pixels:
    int popBox[4] = {};      // x0, y0, x1, y1
    int leftTop = 0;         // the catalog's left panel's top (its search box), 0 = not found
    int gridTop = 0;         // the catalog items' top, 0 = not found
    int gaps[64] = {};       // the gaps between the catalog's items (their middles), left to right
    int gapCount = 0;
};
static TabletScan g_tabletScan;
static ID3D11Texture2D* g_tabletScanCopy = nullptr;
static bool g_tabletScanPending = false;
static int g_tabletScanY0 = 0, g_tabletScanWait = 0, g_tabletMiddleRuns = 0;
static int g_tabletLeftSeen = 0, g_tabletGridSeen = 0;  // a new top, seen once so far
static ULONGLONG g_tabletScanAt = 0;

// The tablet's band of the UI: from 15% down to the bottom.
static void tabletScanBand(int W, int H, int& y0, int& y1) {
    y0 = H * 15 / 100;
    y1 = H;
    (void)W;
}

static void readTabletScan(const uint8_t* px, UINT pitch, int W, int H) {
    float s = H / 2808.0f;
    int y0, y1;
    tabletScanBand(W, H, y0, y1);
    auto alpha = [&](int x, int y) { return px[static_cast<size_t>(y - y0) * pitch + static_cast<size_t>(x) * 4 + 3]; };
    // The middle: rows above the tooltip's band, clear of the catalog's left panel, every 16 px.
    int midEnd = H - static_cast<int>(600 * s), solid = 0, total = 0;
    for (int y = y0; y < midEnd; y += 16)
        for (int x = static_cast<int>(600 * s); x < W - static_cast<int>(600 * s); x += 16, ++total)
            solid += alpha(x, y) > 200;
    bool middle = total > 0 && solid * 25 > total;  // over 4% of it solid
    g_tabletMiddleRuns = middle == g_tabletScan.middle ? 0 : g_tabletMiddleRuns + 1;
    if (g_tabletMiddleRuns >= 2) {  // twice in a row before it changes
        g_tabletScan.middle = middle;
        g_tabletMiddleRuns = 0;
        static int logged = 0;
        if (logged++ < 20)
            LOG("Tablet: %s", middle ? "something solid in the middle of the UI: the whole UI on the tablet"
                                     : "the middle is clear again: the palette layout");
    }
    // A panel's top: up from the bottom at column x while the UI is solid (gaps up to 12 px).
    auto topAt = [&](int x) {
        int top = H, gap = 0;
        for (int y = H - 1; y >= y0; --y) {
            if (alpha(x, y) > 120) {
                top = y;
                gap = 0;
            } else if (++gap > static_cast<int>(12 * s) + 1) {
                break;
            }
        }
        return top;
    };
    // A top changes only when found the same twice in a row; not found (something
    // standing on it, like the tooltip) keeps the last one.
    auto settle = [](int found, int& kept, int& seen) {
        if (!found || found == kept) {
            seen = 0;
            return;
        }
        if (seen && std::abs(found - seen) <= 2) {
            kept = found;
            seen = 0;
        } else {
            seen = found;
        }
    };
    int leftTop = topAt(static_cast<int>(40 * s));    // through the search box
    int gridTop = topAt(static_cast<int>(700 * s));   // through the first item's column
    int oldLeft = g_tabletScan.leftTop, oldGrid = g_tabletScan.gridTop;
    settle(H - leftTop >= 150 * s && H - leftTop <= H / 2 ? leftTop : 0, g_tabletScan.leftTop, g_tabletLeftSeen);
    settle(H - gridTop >= 120 * s && H - gridTop <= 400 * s ? gridTop : 0, g_tabletScan.gridTop, g_tabletGridSeen);
    if (g_tabletScan.leftTop != oldLeft || g_tabletScan.gridTop != oldGrid) {
        static int logged = 0;
        if (logged++ < 30)
            LOG("Tablet: catalog's left panel from %d px up, items from %d px up (of %d; 0 = not found)",
                g_tabletScan.leftTop ? H - g_tabletScan.leftTop : 0, g_tabletScan.gridTop ? H - g_tabletScan.gridTop : 0, H);
    }
    // The gaps between the items, along their first row (runs of 3+ clear pixels).
    g_tabletScan.gapCount = 0;
    if (g_tabletScan.gridTop) {
        int gy = std::min(H - 1, g_tabletScan.gridTop + static_cast<int>(30 * s)), run = 0;
        for (int x = static_cast<int>(512 * s); x < W; ++x) {
            if (alpha(x, gy) < 60) {
                ++run;
            } else {
                if (run >= 3 && g_tabletScan.gapCount < 64) g_tabletScan.gaps[g_tabletScan.gapCount++] = x - (run + 1) / 2;
                run = 0;
            }
        }
    }
    // The tooltip: the solid box in the band above the catalog, right of its left panel.
    int ta = H - static_cast<int>(600 * s), tb = H - static_cast<int>(300 * s), xa = static_cast<int>(512 * s);
    int bx0 = W, bx1 = -1, by0 = H, by1 = -1;
    for (int y = std::max(ta, y0); y < std::min(tb, y1); y += 4)
        for (int x = xa; x < W; x += 4)
            if (alpha(x, y) > 200) {
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
    bool tip = bx1 - bx0 > 100 * s && by1 - by0 > 60 * s;
    if (tip) {
        // Its bottom reaches down past the band: follow its middle down, to the items' top.
        int cx = (bx0 + bx1) / 2, stop = H - static_cast<int>(238 * s);
        while (by1 + 2 < stop && alpha(cx, by1 + 2) > 200) by1 += 2;
        g_tabletScan.tipBox[0] = bx0;
        g_tabletScan.tipBox[1] = by0;
        g_tabletScan.tipBox[2] = bx1 + 1;
        g_tabletScan.tipBox[3] = by1 + 1;
    }
    g_tabletScan.tip = tip;
    // A popped-up list (the categories' "Show All"): solid above the left panel's search box,
    // within the panel's width; it reaches down to the panel along its right side.
    int panelTop = g_tabletScan.leftTop ? g_tabletScan.leftTop : H - static_cast<int>(298 * s);
    int px0 = W, px1 = -1, py0 = H, py1 = -1;
    for (int y = y0; y < panelTop - static_cast<int>(4 * s); y += 4)
        for (int x = static_cast<int>(8 * s); x < static_cast<int>(504 * s); x += 4)
            if (alpha(x, y) > 200) {
                px0 = std::min(px0, x);
                px1 = std::max(px1, x);
                py0 = std::min(py0, y);
                py1 = std::max(py1, y);
            }
    bool popup = px1 - px0 > 150 * s && py1 - py0 > 60 * s;
    if (popup) {
        int cx = px1 - static_cast<int>(10 * s), stop = std::min(H - 1, panelTop + static_cast<int>(60 * s)), gap = 0;
        for (int y = py1 + 1; y < stop; ++y) {
            if (alpha(cx, y) > 120) {
                py1 = y;
                gap = 0;
            } else if (++gap > 3) {
                break;
            }
        }
        g_tabletScan.popBox[0] = px0;
        g_tabletScan.popBox[1] = py0;
        g_tabletScan.popBox[2] = px1 + 1;
        g_tabletScan.popBox[3] = py1 + 1;
    }
    if (popup != g_tabletScan.popup) {
        static int logged = 0;
        if (logged++ < 20) {
            if (popup)
                LOG("Tablet: a list popped up over the catalog's left panel (%d,%d to %d,%d): beside the panel",
                    px0, py0, px1 + 1, py1 + 1);
            else
                LOG("Tablet: the popped-up list is gone");
        }
    }
    g_tabletScan.popup = popup;
}

// While the tablet is out: a copy of the UI's band about 3 times a second, read a frame or two later.
static void updateTabletScan(ULONGLONG now) {
    if (!g_uiFrame) return;
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    if (!alphaReadable(desc)) return;
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    int W = static_cast<int>(desc.Width), H = static_cast<int>(desc.Height), y0, y1;
    tabletScanBand(W, H, y0, y1);
    if (g_tabletScanPending && g_tabletScanCopy) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(g_tabletScanCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++g_tabletScanWait < 30) {
            // next frame
        } else {
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC have;
                g_tabletScanCopy->GetDesc(&have);
                if (static_cast<int>(have.Width) == W && g_tabletScanY0 == y0 &&
                    static_cast<int>(have.Height) == y1 - y0)
                    readTabletScan(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, W, H);
                context->Unmap(g_tabletScanCopy, 0);
            }
            g_tabletScanPending = false;
        }
    } else if (now >= g_tabletScanAt) {
        g_tabletScanAt = now + 300;
        if (g_tabletScanCopy) {
            D3D11_TEXTURE2D_DESC have;
            g_tabletScanCopy->GetDesc(&have);
            if (static_cast<int>(have.Width) != W || static_cast<int>(have.Height) != y1 - y0 ||
                have.Format != desc.Format) {
                g_tabletScanCopy->Release();
                g_tabletScanCopy = nullptr;
            }
        }
        if (!g_tabletScanCopy) {
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = static_cast<UINT>(W);
            staging.Height = static_cast<UINT>(y1 - y0);
            staging.MipLevels = staging.ArraySize = 1;
            staging.Format = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            device->CreateTexture2D(&staging, nullptr, &g_tabletScanCopy);
        }
        if (g_tabletScanCopy) {
            D3D11_BOX box{0, static_cast<UINT>(y0), 0, static_cast<UINT>(W), static_cast<UINT>(y1), 1};
            context->CopySubresourceRegion(g_tabletScanCopy, 0, 0, 0, 0, g_uiFrame, 0, &box);
            g_tabletScanY0 = y0;
            g_tabletScanPending = true;
            g_tabletScanWait = 0;
        }
    }
    context->Release();
    device->Release();
}

// The palette: tiles (place on the sheet 0..1 from its top left, part of the
// UI in pixels) and the sheet's width / height. 0 tiles = the whole UI.
// Later tiles lie over earlier ones (drawn and clicked on top).
static int tabletLayout(int W, int H, float dst[][4], float src[][4], float& aspect) {
    aspect = static_cast<float>(W) / static_cast<float>(H);
    if (g_tabletScan.middle || W < H / 2) return 0;
    const float s = H / 2808.0f, Wf = static_cast<float>(W), Hf = static_cast<float>(H);
    const float pad = 18 * s, gap = 12 * s;  // around the edge, between pieces
    const float left = 512 * s;
    const float leftTop = g_tabletScan.leftTop ? static_cast<float>(g_tabletScan.leftTop) : Hf - 298 * s;
    const float gridTop = g_tabletScan.gridTop ? static_cast<float>(g_tabletScan.gridTop) : Hf - 238 * s;
    const float search = 52 * s, gridH = Hf - gridTop, panelH = Hf - leftTop - search;
    // The items in two halves, split in the gap between two items nearest the middle
    // (measured; else they start 524 px in, 116 px apart).
    float middleX = left + (Wf - left) / 2;
    float split = 524 * s + std::round((middleX - left - 12 * s) / (116 * s)) * 116 * s - 4 * s;
    for (int k = 0, best = -1; k < g_tabletScan.gapCount; ++k) {
        float x = static_cast<float>(g_tabletScan.gaps[k]);
        if (x <= left + 116 * s || x >= Wf - 116 * s) continue;
        if (best < 0 || std::fabs(x - middleX) < std::fabs(split - middleX)) {
            split = x;
            best = k;
        }
    }
    split = std::max(left + 116 * s, std::min(Wf - 116 * s, split));
    const float inner = std::max(std::max(split - left, Wf - split), 1000 * s);  // the pieces' width
    const float CW = inner + 2 * pad;
    struct Box { float x0, y0, x1, y1; };
    std::vector<std::pair<Box, Box>> tiles;  // place (pixels on the sheet), part (UI pixels)
    auto put = [&](float dx, float dy, Box from, float k) {
        tiles.push_back({{dx, dy, dx + (from.x1 - from.x0) * k, dy + (from.y1 - from.y0) * k}, from});
    };
    auto add = [&](float dx, float dy, Box from) { put(dx, dy, from, 1); };
    const float x0 = pad, x1 = pad + inner;
    // The top: the tool bar at the left, the mode buttons at the right.
    float y = pad;
    add(x0, y, {Wf / 2 - 260 * s, 0, Wf / 2 + 260 * s, 56 * s});
    add(x1 - 454 * s, y, {Wf - 454 * s, 0, Wf, 64 * s});
    y += 64 * s + gap;
    // The search box, the lot name (made smaller to fit), the items' filter at the right.
    const float rowH = 60 * s;
    add(x0, y + (rowH - search) / 2, {0, leftTop, 290 * s, leftTop + search});
    Box filter{Wf - 344 * s, gridTop - 60 * s, Wf, gridTop - 8 * s};
    add(x1 - 344 * s, y + (rowH - (filter.y1 - filter.y0)) / 2, filter);
    {
        Box lot{0, 0, std::min(520 * s, Wf), 60 * s};
        float room = inner - 290 * s - 344 * s - 2 * gap;
        float k = std::max(0.3f, std::min(1.0f, room / (lot.x1 - lot.x0)));
        float w = (lot.x1 - lot.x0) * k;
        put(x0 + 290 * s + gap + (room - w) / 2, y + (rowH - rowH * k) / 2, lot, k);
    }
    y += rowH + gap;
    // The left panel, and the item tooltip beside it.
    const float panelY = y;
    add(x0, y, {0, leftTop + search, left, Hf});
    const float room = inner - left - gap;
    if (g_tabletScan.tip && !g_tabletScan.popup) {
        const int* b = g_tabletScan.tipBox;
        Box tip{static_cast<float>(b[0]), static_cast<float>(b[1]), static_cast<float>(b[2]), static_cast<float>(b[3])};
        float tw = tip.x1 - tip.x0, th = tip.y1 - tip.y0;
        float k = std::min(1.0f, std::min(room / tw, panelH / th));
        put(x0 + left + gap + (room - tw * k) / 2, y + (panelH - th * k) / 2, tip, k);
    }
    y += panelH + gap;
    // The items: first half, then the second.
    add(x0, y, {left, gridTop, split, Hf});
    y += gridH + gap;
    add(x0, y, {split, gridTop, Wf, Hf});
    y += gridH + pad;
    const float CH = y;
    // A popped-up list (the categories): beside the left panel, opening down over the items.
    if (g_tabletScan.popup) {
        const int* b = g_tabletScan.popBox;
        Box pop{static_cast<float>(b[0]), static_cast<float>(b[1]), static_cast<float>(b[2]), static_cast<float>(b[3])};
        float pw = pop.x1 - pop.x0, ph = pop.y1 - pop.y0;
        float k = std::min(1.0f, std::min(room / pw, (CH - pad - panelY) / ph));
        put(x0 + left + gap, panelY, pop, k);
    }
    int n = 0;
    for (const auto& t : tiles) {
        if (n >= phone3d::kMaxTiles) break;
        dst[n][0] = t.first.x0 / CW;
        dst[n][1] = t.first.y0 / CH;
        dst[n][2] = t.first.x1 / CW;
        dst[n][3] = t.first.y1 / CH;
        src[n][0] = t.second.x0;
        src[n][1] = t.second.y0;
        src[n][2] = t.second.x1;
        src[n][3] = t.second.y1;
        ++n;
    }
    aspect = CW / CH;
    return n;
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

