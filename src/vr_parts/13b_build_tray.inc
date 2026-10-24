// The build tablet's tool tray (user 2026-10-24 01:51Z; docs/BUILD-TRAY.md): build mode's keyboard-only hotkeys as
// buttons on the tablet's glass, right middle (hud.cpp draws it in kBuildTray, phone3d puts it over the UI). Most
// go to the same game services the game's controller build mode calls (clientdlg::gameService); Alt and Shift are
// held keys while their toggles are on; Next slot (M) and Curved roof (Shift+C) are key taps; the bb.* cheats run
// in Python (controls.py 7700000+). With the laser on and the tablet out, the laser hand's stick turns (X) and
// raises / lowers (Y) the item you are placing instead of turning or walking you.

// Where it sits on the tablet's screen: 0..1 from its top left. Its width follows from the height and the
// screen's shape (the atlas area's shape kept).
// [tablet] tray_scale (live) sizes it: 1 = two thirds of the screen's height, centred a little above the middle.
static constexpr float kTrayRight = 0.995f, kTrayMiddle = 0.485f, kTrayHeight = 0.67f;

static bool trayOn(int tool) { return (g_trayLit >> tool) & 1; }
static void setTrayLit(int tool, bool on) {
    if (on) g_trayLit |= 1u << tool;
    else g_trayLit &= ~(1u << tool);
}

// Every frame the tablet is drawn: the tray on its glass (p3's overlay) and its sheet for pokes and the laser.
static void placeTray(phone3d::Phone& p3, const XrVector3f& centre, const XrVector3f& x, const XrVector3f& y,
                      const XrVector3f& z, float menuW, float menuH) {
    const float h = std::min(0.96f, kTrayHeight * g_config.tabletTrayScale);
    const float top = std::max(0.02f, std::min(0.98f - h, kTrayMiddle - h / 2)), bottom = top + h;
    const float w = h * menuH / menuW * hud::kBuildTray.w / hud::kBuildTray.h;
    const float x0 = kTrayRight - w, x1 = kTrayRight;
    p3.overlay = true;
    p3.overlayDst[0] = x0, p3.overlayDst[1] = top, p3.overlayDst[2] = x1, p3.overlayDst[3] = bottom;
    p3.overlayCrop[0] = hud::kBuildTray.x / hud::kWidth;
    p3.overlayCrop[1] = hud::kBuildTray.y / hud::kHeight;
    p3.overlayCrop[2] = (hud::kBuildTray.x + hud::kBuildTray.w) / hud::kWidth;
    p3.overlayCrop[3] = (hud::kBuildTray.y + hud::kBuildTray.h) / hud::kHeight;
    const float lx = ((x0 + x1) / 2 - 0.5f) * menuW, ly = (0.5f - (top + bottom) / 2) * menuH, lz = 0.0005f;
    const XrVector3f at{centre.x + x.x * lx + y.x * ly + z.x * lz, centre.y + x.y * lx + y.y * ly + z.y * lz,
                        centre.z + x.z * lx + y.z * ly + z.z * lz};
    static bool logged = false;
    if (!g_traySheet.m.active && !logged) {
        logged = true;
        LOG("Build tray: on the tablet, %.1f x %.1f cm", w * menuW * 100, h * menuH * 100);
    }
    placeHud(g_traySheet, at, x, y, w * menuW, hud::kBuildTray);
}

// Raise / lower / bigger / smaller work with bb.moveobjects on (the game's controller build mode only sends them
// with cheats on): turned on with them.
static void trayNeedsMoveObjects() {
    if (trayOn(hud::kTrayMoveObjects)) return;
    setTrayLit(hud::kTrayMoveObjects, true);
    grab::pushControl(7700000);
}

static void trayTool(int tool) {
    using namespace hud;
    switch (tool) {
        case kTrayAlt: setTrayLit(tool, !trayOn(tool)); break;
        case kTrayShift: setTrayLit(tool, !trayOn(tool)); break;
        case kTraySnap: clientdlg::gameService("BBCycleSnapSize"); break;
        case kTrayGrid: clientdlg::gameService("ToggleBuildBuyGrid"); break;
        case kTrayRotateLeft: clientdlg::gameService("BBRotateSelectedObject", "{\"value\":-1}"); break;
        case kTrayRotateRight: clientdlg::gameService("BBRotateSelectedObject", "{\"value\":1}"); break;
        case kTrayRaise: trayNeedsMoveObjects(); clientdlg::gameService("BBMoveObjectVerticallyUp"); break;
        case kTrayLower: trayNeedsMoveObjects(); clientdlg::gameService("BBMoveObjectVerticallyDown"); break;
        case kTrayBigger: trayNeedsMoveObjects(); clientdlg::gameService("BBIncreaseObjectScale"); break;
        case kTraySmaller: trayNeedsMoveObjects(); clientdlg::gameService("BBDecreaseObjectScale"); break;
        case kTraySwatch: clientdlg::gameService("BBCycleVariant"); break;
        case kTraySlot: pointer::tapKey(0x32); break;  // M
        case kTrayQuarter:
            clientdlg::gameService("BBToggleQuarterTile");
            setTrayLit(tool, !trayOn(tool));
            break;
        case kTrayRoofCurve: pointer::tapKey(0x2E, true); break;  // Shift+C
        case kTrayEraser:
            clientdlg::gameService("TerrainPaintToggleTool", "", true);
            setTrayLit(tool, !trayOn(tool));
            break;
        case kTrayMoveObjects:
            setTrayLit(tool, !trayOn(tool));
            grab::pushControl(trayOn(tool) ? 7700000 : 7700001);
            break;
        case kTrayHidden:
            setTrayLit(tool, !trayOn(tool));
            grab::pushControl(7700002);
            break;
        case kTrayLiveEdit:
            setTrayLit(tool, !trayOn(tool));
            grab::pushControl(7700003);
            break;
        case kTrayExitFirstPerson: grab::pushControl(1); break;  // as the left stick held: switch view
        default: return;
    }
    static int logged = 0;
    if (logged++ < 200) LOG("Build tray: tool %d (lit 0x%x)", tool, g_trayLit);
}

// The tray is gone (the tablet put away, build mode left): Alt and Shift let go.
static void hideTray() {
    if (!g_traySheet.m.active) return;
    hideHud(g_traySheet);
    setTrayLit(hud::kTrayAlt, false);
    setTrayLit(hud::kTrayShift, false);
    LOG("Build tray: away");
}

// The laser on the tray.
static bool laserTray(const XrVector3f& o, const XrVector3f& d, bool trigger, XrVector3f& hit) {
    const PieMenu& m = g_traySheet.m;
    float u, v;
    if (!m.active || !hitSheet(m, o, d, hit, u, v)) return false;
    g_hudInput.px = u * hud::kWidth;
    g_hudInput.py = v * hud::kHeight;
    g_hudInput.down = trigger;
    return true;
}

// The laser hand's stick while the tray is out and the laser on: X turns the held item (freely with Free place
// on, as the controller build mode's right stick; else a 45 degree step per push, repeating while held), Y
// raises / lowers it. Returns true when it took the stick.
static bool trayStick(XrVector2f stick, float dt) {
    if (!g_traySheet.m.active) return false;
    static ULONGLONG nextTurn = 0, nextLift = 0;
    const ULONGLONG now = GetTickCount64();
    const float ax = std::fabs(stick.x), ay = std::fabs(stick.y);
    if (ax > 0.2f && trayOn(hud::kTrayAlt)) {
        // BuildBuyCenterAdapterBase.Tick: -stick.x * deltaTime (ms) * CONTROLLER_FREE_ROTATE_SPEED (0.01).
        char value[32];
        std::snprintf(value, sizeof value, "%.4f", -stick.x * dt * 1000.0f * 0.01f);
        clientdlg::gameService("BBGamepadFreeRotate", value);
    } else if (ax > 0.6f) {
        if (now >= nextTurn) {
            trayTool(stick.x > 0 ? hud::kTrayRotateRight : hud::kTrayRotateLeft);
            nextTurn = now + 400;
        }
    } else if (ax < 0.3f) {
        nextTurn = 0;
    }
    if (ay > 0.6f) {
        if (now >= nextLift) {
            trayTool(stick.y > 0 ? hud::kTrayRaise : hud::kTrayLower);
            nextLift = now + 120;
        }
    } else if (ay < 0.3f) {
        nextLift = 0;
    }
    return true;
}
