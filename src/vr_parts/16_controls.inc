static void updateControls(XrTime time, const D3D11_TEXTURE2D_DESC& desc) {
    g_laserValid = false;
    g_tipOnly = false;
    armik::Hand hands[2];
    if (!g_actionsAttached || g_sessionState != XR_SESSION_STATE_FOCUSED || !g_appSpace) {
        pointer::releaseAll();
        armik::setHands(hands);
        hideWrist();
        closePie("headset not focused");
        return;
    }
    XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!XR_SUCCEEDED(xrSyncActions(g_session, &sync))) {
        armik::setHands(hands);
        return;
    }
    updateHands(time, hands);

    float trigger[2] = {floatState(g_triggerAction, 0), floatState(g_triggerAction, 1)};
    static bool triggerDown[2] = {false, false};
    const bool triggerWas[2] = {triggerDown[0], triggerDown[1]};
    for (int hand = 0; hand < 2; ++hand) {
        bool down = triggerDown[hand] ? trigger[hand] > 0.35f : trigger[hand] > 0.6f;
        triggerDown[hand] = down;
    }
    // Picking things up (grab.cpp): a hand that closes near an object holds
    // it, and points or shows the laser no more until it opens.
    {
        bool indexOut[2];
        for (int i = 0; i < 2; ++i) indexOut[i] = !boolState(g_indexTouchAction, i) && trigger[i] < 0.15f;
        grab::HeadPose head;
        XrSpaceLocation view{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags both = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        bool firstPerson = mode() == Mode::Stereo && camera::bodyActive();
        if (firstPerson && g_viewSpace && XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &view)) &&
            (view.locationFlags & both) == both) {
            const XrPosef& v = view.pose;
            head.valid = true;
            head.position = {v.position.x, v.position.y, v.position.z};
            head.orientation = {v.orientation.x, v.orientation.y, v.orientation.z, v.orientation.w};
        }
        // The settings menu held by its title bar with a hand (v0.13.117): the hand
        // closing within 10 cm of the bar holds it until it opens.
        {
            static float gripWas[2] = {0, 0};
            SettingsDrag& drag = g_settingsDrag;
            const SettingsPanel& p = g_settingsPanel;
            if (drag.hand < 0 && p.placed && settings::isOpen()) {
                for (int i = 0; i < 2; ++i) {
                    if (!hands[i].valid || hands[i].grip <= 0.6f || gripWas[i] > 0.6f || grab::holding(i)) continue;
                    const XrVector3f at{hands[i].position.x, hands[i].position.y, hands[i].position.z};
                    const XrVector3f rel = sub3(at, p.pose.position);
                    const float out = dot3(rel, p.z);
                    if (std::fabs(out) > 0.1f) continue;
                    if (!settings::onTitleBar(dot3(rel, p.x) / p.width + 0.5f, 0.5f - dot3(rel, p.y) / p.height))
                        continue;
                    const XrVector3f held{at.x - p.z.x * out, at.y - p.z.y * out, at.z - p.z.z * out};
                    drag.grip = true;
                    startSettingsDrag(i, false, held);
                    drag.fromHand = sub3(held, at);
                    break;
                }
            }
            for (int i = 0; i < 2; ++i) gripWas[i] = hands[i].grip;
            if (drag.hand >= 0 && !drag.laser) {
                const armik::Hand& h = hands[drag.hand];
                XrPosef head;
                if (!settings::isOpen() || h.grip < 0.35f)
                    endSettingsDrag(time);
                else if (h.valid && locateHead(time, head))
                    moveSettings({h.position.x + drag.fromHand.x, h.position.y + drag.fromHand.y,
                                  h.position.z + drag.fromHand.z},
                                 head.position);
            }
        }
        // The notification card in front of you held by a hand closing on it
        // (within 12 cm of it): it moves with the hand until the hand opens.
        {
            static float gripWas[2] = {0, 0};
            if (g_noteDrag.hand < 0 && g_noteCardFront && g_settingsDrag.hand < 0) {
                for (int i = 0; i < 2; ++i) {
                    if (!hands[i].valid || hands[i].grip <= 0.6f || gripWas[i] > 0.6f || grab::holding(i) ||
                        (g_phoneOut && i == g_phoneHand))
                        continue;
                    const XrVector3f at{hands[i].position.x, hands[i].position.y, hands[i].position.z};
                    if (!onNoteCard(at, 0.12f, 0.04f)) continue;
                    startNoteDrag(i, false, at);
                    break;
                }
            }
            for (int i = 0; i < 2; ++i) gripWas[i] = hands[i].grip;
            if (g_noteDrag.hand >= 0 && !g_noteDrag.laser) {
                const armik::Hand& h = hands[g_noteDrag.hand];
                if (h.grip < 0.35f || !g_noteCardFront) {
                    XrPosef head;
                    const bool headOk = locateHead(time, head);
                    endNoteDrag(head, headOk);
                } else if (h.valid) {
                    g_noteDrag.held = {h.position.x, h.position.y, h.position.z};
                }
            }
        }
        // The hand holding the phone (or the settings menu) picks nothing up.
        armik::Hand grabHands[2] = {hands[0], hands[1]};
        if (g_noteDrag.hand >= 0 && !g_noteDrag.laser) grabHands[g_noteDrag.hand].grip = 0;
        if (g_phoneOut) grabHands[g_phoneHand].grip = 0;
        if (g_settingsDrag.hand >= 0 && !g_settingsDrag.laser) grabHands[g_settingsDrag.hand].grip = 0;
        grab::update(grabHands, trigger, indexOut, head, firstPerson, g_config.grab);
        for (int i = 0; i < 2; ++i) {
            float amplitude = 0, seconds = 0;
            if (grab::takeHaptic(i, amplitude, seconds)) buzz(i, amplitude, seconds);
        }
        // Your phone: either hand closing at your right hip (and not on
        // something to pick up) takes it out into that hand; opening that
        // hand puts it away.
        static bool closed[2] = {false, false};
        const bool closedWas[2] = {closed[0], closed[1]};
        for (int i = 0; i < 2; ++i) closed[i] = closed[i] ? hands[i].grip > 0.35f : hands[i].grip > 0.6f;
        // Into or out of build/buy with it out: put away (it comes back as the other).
        const bool tabletNow = g_buildBuy && g_config.tablet;
        // A phone call of your Sim keeps the phone in its hand (opening the hand doesn't put it away); every
        // 2 s the script is asked whether the call still runs (nophone.call_check).
        const bool callHold = g_onCall.load(std::memory_order_relaxed) && firstPerson && g_config.phone && !tabletNow;
        if (callHold) {
            static ULONGLONG askedAt = 0;
            const ULONGLONG t = GetTickCount64();
            if (t - askedAt > 2000) {
                askedAt = t;
                grab::pushControl(82);
            }
        }
        if (callHold && !g_phoneOut && head.valid) {
            phoneOpen(1);  // the call brings the phone out in your right hand
        } else if (g_phoneOut && !callHold &&
                   (!closed[g_phoneHand] || !firstPerson || !g_config.phone || g_tablet != tabletNow)) {
            phoneClose(!firstPerson ? "left first person" : g_tablet != tabletNow ? "build/buy changed" : "hand opened");
        } else if (!g_phoneOut && !callHold && firstPerson && g_config.phone && head.valid) {
            for (int i : {1, 0}) {
                if (closed[i] && !closedWas[i] && hands[i].valid && !grab::holding(i) && !armik::holding(i) &&
                    atRightHip(hands[i], i, head)) {
                    phoneOpen(i);
                    break;
                }
            }
        }
    }

    // Touching other Sims (armik): a tap when a hand meets a body or takes a
    // hand, then a soft hum while it presses, stronger the deeper.
    if (g_config.touchHaptics && mode() == Mode::Stereo && camera::bodyActive()) {
        static bool touched[2] = {false, false}, held[2] = {false, false};
        for (int i = 0; i < 2; ++i) {
            float depth = armik::contact(i);
            bool touching = depth > 0.003f, holds = armik::holdingSim(i);
            if (holds && !held[i])
                buzz(i, 0.7f, 0.06f);
            else if (touching && !touched[i])
                buzz(i, 0.45f, 0.03f);
            else if (touching)
                buzz(i, std::min(0.35f, 0.08f + depth * 4.0f), 0.02f);
            touched[i] = touching;
            held[i] = holds;
        }
    }

    // Holding a stick down: the left switches between first person and the
    // game camera, the right to the next Sim of your household (Python does both).
    // A short click of the right stick takes a screenshot of both eyes; of the
    // left, in first person, switches between walking and running.
    {
        static ULONGLONG downSince[2] = {0, 0};
        static bool fired[2] = {false, false};
        ULONGLONG tick = GetTickCount64();
        for (int i = 0; i < 2; ++i) {
            if (!boolState(g_stickClickAction, i)) {
                if (i == 1 && downSince[i] && !fired[i] && tick - downSince[i] < 500) {
                    screenshot::request();
                    buzz(i, 0.35f, 0.04f);
                }
                if (i == 0 && downSince[i] && !fired[i] && tick - downSince[i] < 500 && mode() == Mode::Stereo &&
                    camera::bodyActive()) {
                    bool run = !player::running();
                    player::setRunning(run);
                    buzz(i, run ? 0.5f : 0.3f, run ? 0.08f : 0.04f);
                    LOG("Controls: left stick tapped: %s", run ? "running" : "walking");
                }
                downSince[i] = 0;
                fired[i] = false;
                continue;
            }
            if (!downSince[i]) downSince[i] = tick;
            if (!fired[i] && tick - downSince[i] >= 500 && mode() == Mode::Stereo) {
                fired[i] = true;
                grab::pushControl(i == 0 ? 1 : 2);
                buzz(i, 0.6f, 0.08f);
                LOG("Controls: %s stick held: %s", i ? "right" : "left", i ? "next Sim" : "switch view");
            }
        }
    }

    int laserHand = -1;
    bool swallowClick = false;
    static bool laserButton[2] = {false, false};  // A / X held down as the laser's button (until released)
    if (!(mode() == Mode::Stereo && camera::bodyActive())) {
        // Main menu, loading screens, god camera: the laser is always on.
        // Pulling the other hand's trigger moves it to that hand (no click).
        for (int i = 0; i < 2; ++i) {
            if (i != g_pointerHand && triggerDown[i] && !triggerWas[i]) {
                g_pointerHand = i;
                swallowClick = true;
            }
        }
        laserHand = g_pointerHand;
    } else {
        // First person: the laser shows while a hand holds the grip and its
        // A (right) or X (left) button; pulling that trigger clicks. Otherwise
        // the triggers are just your index fingers.
        for (int i : {g_pointerHand, 1 - g_pointerHand}) {
            if (floatState(g_gripAction, i) > 0.6f && boolState(g_primaryAction, i) && !grab::holding(i) &&
                !(g_phoneOut && i == g_phoneHand)) {
                laserHand = i;
                break;
            }
        }
        if (laserHand >= 0) laserButton[laserHand] = true;
    }
    for (int i = 0; i < 2; ++i)
        if (!boolState(g_primaryAction, i)) laserButton[i] = false;
    if (laserHand >= 0) g_pointerHand = laserHand;

    pointer::Controls c;
    c.leftButton = laserHand >= 0 && triggerDown[laserHand] && !swallowClick;
    static bool swallowing = false;  // a hand switch's pull stays unclicked until released
    if (swallowClick) swallowing = true;
    if (swallowing && !(laserHand >= 0 && triggerDown[laserHand])) swallowing = false;
    if (swallowing) c.leftButton = false;
    static bool laserClickWas = false;
    const bool laserClick = c.leftButton && !laserClickWas;  // the laser's trigger just pulled
    laserClickWas = c.leftButton;
    // A / X right-click, except while held as the laser's button.
    c.rightButton = (!laserButton[0] && boolState(g_primaryAction, 0)) || (!laserButton[1] && boolState(g_primaryAction, 1));
    // The left menu button is the game's Escape (its menu). Recentering is
    // the headset's own (hold the Meta button): the runtime's recenter is
    // picked up as ours. In first person with no game UI in front (menus on
    // the wrist only, or hidden) the game's menu would open unseen, so the
    // button leaves it shut (user 2026-10-17 13:00Z); it still closes the
    // menu if something else opened it.
    const bool menuButton = boolState(g_menuAction, 0) || boolState(g_menuAction, 1);
    c.escape = menuButton && (frontPanelShown() || gameMenuOpen());
    const bool cancel = boolState(g_secondaryAction, 0) || boolState(g_secondaryAction, 1);
    XrVector2f left = stickState(0), right = stickState(1);

    // The pie menu at your hand: opened for a click into the world in first
    // person, closed when you pick, press Escape, or leave first person.
    const XrSpaceLocationFlags located = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    bool firstPerson = mode() == Mode::Stereo && camera::bodyActive() && g_config.pieAtHand;
    ULONGLONG now = GetTickCount64();
    if (g_pieCloseRequest.exchange(false)) closePie("picked");
    if (g_pieOpenRequest.exchange(false) && firstPerson && g_lastWorldClick.valid &&
        now - g_lastWorldClick.tick < 3000) {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) &&
            (head.locationFlags & located) == located)
            openPie(head.pose.position);
        g_lastWorldClick.valid = false;
    }
    if (g_pie.active && (!firstPerson || !g_uiFrame)) closePie("left first person");
    if (g_pie.active && now - g_pie.opened > 120000) closePie("timed out");
    static bool escapeWas = false;
    if (menuButton && !escapeWas) closePie("escape");
    escapeWas = menuButton;
    // B / Y close the pie at your hand (and the game's, with its Escape);
    // otherwise B opens the VR settings menu, and B or Y goes back a page
    // (closing it on its first page).
    static bool cancelWas = false;
    if (cancel && !cancelWas) {
        if (g_pie.active)
            dismissPie("B / Y");
        else if (settings::isOpen()) {
            if (settings::back())
                buzz(1, 0.15f, 0.015f);
            else
                closeSettings();
        }
        else if (boolState(g_secondaryAction, 1))
            openSettings(time);
    }
    cancelWas = cancel;
    // While the settings menu is open it has the right stick (no turning or
    // zooming) and A (no right click), except A held with the grip for the laser.
    if (settings::isOpen()) {
        if (settings::update(right.x, right.y, boolState(g_primaryAction, 1) && !laserButton[1])) buzz(1, 0.15f, 0.015f);
        if (settings::closeWanted()) closeSettings();
        right = {0, 0};
        c.rightButton = false;
    }

    // A picker on the object it is for (first person); the wrist menus give way to it.
    {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        bool headOk = XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) &&
                      (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        updateObjectMenu(mode() == Mode::Stereo && camera::bodyActive(), headOk, head.pose);
    }
    updateSims1Screen(mode() == Mode::Stereo && camera::bodyActive());
    // The menus on your left wrist (first person, while no pie menu is open).
    {
        // The game's full UI on your wrist: only with "Both", from the watch's UI button
        // (user 17:13Z: "wrist" = our HUD only, "in front" = the game's UI only).
        bool wristMode = mode() == Mode::Stereo && camera::bodyActive() && g_config.uiFirstPerson == 3 && !g_pie.active &&
                         !g_phoneOut && !g_objMenu.active && !(g_popupCardWanted && g_config.popups3d);
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        bool headOk = wristMode && XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) &&
                      (head.locationFlags & located) == located;
        bool fullUi = g_fullUi;
        updateWrist(hands[0], head.pose, headOk && fullUi && !grab::holding(0));
    }
    // Pieces of the UI on your arms and around you (first person).
    {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        bool firstPerson = mode() == Mode::Stereo && camera::bodyActive();
        bool hudMode = g_config.uiFirstPerson == 1 || g_config.uiFirstPerson == 3;  // our HUD: "Wrist" or "Both"
        bool headOk = firstPerson && XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) &&
                      (head.locationFlags & located) == located;
        updateHud(hands, firstPerson && hudMode, headOk, head.pose);
        // The game's blue modal dimming never shows in first person: nodim takes it
        // out of the UI; the dialogs drawn over it keep everything but bare dimming.
        int boxes[3][4];
        int count = 0;
        auto box = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
            if (w <= 0 || h <= 0) return;
            boxes[count][0] = x;
            boxes[count][1] = y;
            boxes[count][2] = w;
            boxes[count][3] = h;
            ++count;
        };
        if (g_popupCardWanted) box(g_popupCrop[0], g_popupCrop[1], g_popupCrop[2], g_popupCrop[3]);
        if (g_objMenu.active) box(g_objMenu.x0, g_objMenu.y0, g_objMenu.cw, g_objMenu.ch);
        if (g_phoneOut && g_phoneFound) box(g_phoneCrop[0], g_phoneCrop[1], g_phoneCrop[2], g_phoneCrop[3]);
        nodim::set(firstPerson && g_config.noDimming, boxes, count);
        nodim::setSource(g_config.noDimming);
    }
    // The phone's screen follows the hand holding it; the menu is measured once it has opened.
    updatePhone(hands[g_phoneHand], g_phoneHand);
    if (g_tablet && g_phone.active) updateTabletScan(now);
    // A game popup (first person): the controllers buzz like a phone, two short pulses, again
    // every 5 s while the phone is away (3 rings); with the phone out, only
    // the hand holding it buzzes, once, and its screen is measured again.
    {
        static ULONGLONG pulseAt = 0;
        static int rings = 0, pulse = 0;
        static bool popupWas = false;
        bool popup = g_popupOpen.load();
        if (g_popupNew.exchange(false)) {
            pulseAt = now;
            rings = pulse = 0;
            g_phoneMenuSince = now;
            g_phoneFitted = false;
        }
        if (!popup && popupWas && g_phoneOut && !g_tablet && g_phoneOptions && !gameMenuOpen())
            g_phoneMenuAt = now + 300;  // answered on the phone in Options: the menu comes back
        popupWas = popup;
        // First person only: the game camera (god view) keeps the game's own behaviour.
        if (!popup || !g_config.phonePopupBuzz || !(mode() == Mode::Stereo && camera::bodyActive())) pulseAt = 0;
        // A picker on its object is right in front of you: no ring.
        if (g_objMenuWanted && g_config.objectMenus && g_uiFrame) pulseAt = 0;
        if (pulseAt && now >= pulseAt) {
            if (g_phoneOut) {
                buzz(g_phoneHand, 0.6f, 0.12f);
            } else {
                buzz(0, 0.8f, 0.12f);
                buzz(1, 0.8f, 0.12f);
            }
            if (++pulse < 2) {
                pulseAt = now + 250;
            } else {
                pulse = 0;
                // A popup card in the world is in sight already: one ring.
                bool card = g_popupCardWanted && g_config.popups3d;
                pulseAt = !g_phoneOut && !card && ++rings < 3 ? now + 5000 : 0;
            }
        }
    }
    {
        static bool menuWas = false;
        bool menu = gameMenuOpen() || g_popupOpen;
        if (menu && !menuWas) {
            g_phoneMenuSince = now;
            g_phoneFitted = false;
        }
        menuWas = menu;
        // Options: once the game menu has closed (Resume, a choice in it), or
        // never opened, the phone shows the game's phone menu again.
        if (g_phoneOptions && g_phoneOut && !g_tablet) {
            if (gameMenuOpen()) {
                g_phoneOptionsSeen = true;
            } else if (!g_popupOpen && (g_phoneOptionsSeen || now - g_phoneMenuSince > 3000) && !g_phoneMenuAt &&
                       g_escapeFrames == 0) {
                g_phoneOptions = g_phoneOptionsSeen = false;
                hud::phoneHome();
                LOG("Phone: the game menu closed, back to the phone's home screen");
            }
        }
        {
            // Options: the first fit is the game menu itself; a fit of another
            // size later is a window it opened (game options, a Save or Exit
            // question): that goes in front of you. Measured again every second.
            static bool wasOptions = false;
            static int32_t menuW = 0, menuH = 0;
            static int seen = 0;
            static ULONGLONG refitAt = 0;
            const bool options = g_phoneOptions && g_phoneOut && !g_tablet && menu;
            if (options && !wasOptions) menuW = menuH = 0, seen = g_menuFitDone;
            wasOptions = options;
            if (!options) {
                if (g_optionsFront) LOG("Phone: Options' window gone, the game's UI leaves the front");
                g_optionsFront = false;
                g_frontCropOn = false;
            } else {
                if (g_menuFitDone != seen) {
                    seen = g_menuFitDone;
                    // Not found (the game options fill the screen: "too wide") is another window too.
                    if (menuW || (g_menuFitOk && g_phoneFound)) {
                        const int32_t w = g_menuFitOk ? g_phoneCrop[2] : 0, h = g_menuFitOk ? g_phoneCrop[3] : 0;
                        if (!menuW) menuW = w, menuH = h;
                        const bool other = !g_menuFitOk || std::abs(w - menuW) > menuW / 6 || std::abs(h - menuH) > menuH / 6;
                        if (other != g_optionsFront)
                            LOG("Phone: %s (%dx%d, the game menu %dx%d)",
                                other ? "Options opened a window: the game's UI in front of you"
                                      : "back to the game menu on the phone",
                                w, h, menuW, menuH);
                        g_optionsFront = other;
                        // That window alone in front (fitMenu's box less its margin of the
                        // game's dimming); a window it can't box (filling the screen): all the UI.
                        const bool crop = other && g_menuFitOk && g_uiFrame;
                        if (crop) {
                            D3D11_TEXTURE2D_DESC ui;
                            g_uiFrame->GetDesc(&ui);
                            const int32_t m = static_cast<int32_t>(ui.Height) * 5 / 1000;
                            std::memcpy(g_frontCrop, g_phoneCrop, sizeof(g_frontCrop));
                            if (g_frontCrop[2] > 4 * m && g_frontCrop[3] > 4 * m) {
                                g_frontCrop[0] += m, g_frontCrop[1] += m;
                                g_frontCrop[2] -= 2 * m, g_frontCrop[3] -= 2 * m;
                            }
                        }
                        if (crop != g_frontCropOn)
                            LOG("Phone: %s", crop ? "the window alone in front of you" : "the whole UI in front of you");
                        g_frontCropOn = crop;
                    }
                }
                if (!g_phoneFitPending && now >= refitAt && now - g_phoneMenuSince >= 600) {
                    refitAt = now + 1000;
                    startPhoneFit();
                }
            }
        }
        if (((g_phone.active && !g_tablet && menu) || (g_objMenuWanted && g_config.objectMenus && !g_phone.active) ||
             (g_popupCardWanted && g_config.popups3d && !g_phone.active)) &&
            !g_phoneFitted &&
            !g_phoneFitPending && now - g_phoneMenuSince >= 600)
            startPhoneFit();
        pollPhoneFit();
    }

    // Your own pie menu: poke your stomach with the grip held.
    {
        XrPosef head{};
        const bool firstPerson = mode() == Mode::Stereo && camera::bodyActive();
        const bool headOk = firstPerson && locateHead(time, head);
        pokeStomach(hands, laserHand, firstPerson, head, headOk);
    }
    // Touch to interact: a hand points (grip held, index finger off the
    // trigger, nothing in that hand) and what its fingertip touches is
    // clicked once (touch.cpp says what it touches).
    int pointing = -1;
    if (mode() == Mode::Stereo && camera::bodyActive()) {
        for (int i : {g_pointerHand, 1 - g_pointerHand}) {
            if (i != laserHand && !(g_phoneOut && i == g_phoneHand) && hands[i].valid && hands[i].grip > 0.6f &&
                !boolState(g_indexTouchAction, i) &&
                trigger[i] < 0.15f && !armik::holding(i) && !grab::holding(i)) {
                pointing = i;
                break;
            }
        }
    }
    touch::setPointing(pointing);
    g_turnChecking = false;
    g_stickRate = 0;
    if (!(mode() == Mode::Stereo && camera::bodyActive())) camera::holdBodyTurn(false);
    if (mode() == Mode::Stereo && camera::bodyActive()) {
        // First person: the left stick walks the Sim (Python reads walkInput),
        // in the direction you are looking.
        updateWalk(left, time);
        camera::holdBodyTurn(std::sqrt(left.x * left.x + left.y * left.y) > 0.25f);
        // Right stick turns you (smooth turn): gentle near the centre, fast at the edge.
        // Timed by the frames' display times, so every frame you see is turned
        // by the same step; the PC clock here jitters with xrWaitFrame's return.
        // In whole refreshes: Virtual Desktop's predicted display times wobble
        // from frame to frame, the headset's refreshes don't.
        static LARGE_INTEGER freq{}, last{};
        static XrTime lastShown = 0;
        static double refresh = 0;  // seconds between the headset's refreshes
        static double slot = 0;     // the refresh (display clock, seconds) the last frame was turned for
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        float dt = last.QuadPart ? float(t.QuadPart - last.QuadPart) / float(freq.QuadPart) : 0;
        const double step = lastShown > 0 && time > lastShown ? double(time - lastShown) * 1e-9 : 0;
        int refreshes = 1;
        if (step > 0 && step < 0.1) {
            // The period doubles while Virtual Desktop's frame smoothing runs;
            // the refreshes stay as they were.
            const double period = g_displayPeriod > 0 ? double(g_displayPeriod) * 1e-9 : step;
            if (refresh <= 0 || std::fabs(period - 2 * refresh) > 0.05 * refresh) refresh = period;
            // Whole refreshes since the last frame's, at least one: the predicted
            // display times bunch up (two frames 1 ms apart, then 27 ms), so each
            // frame is counted against the refresh the one before was turned for.
            const double now = double(time) * 1e-9;
            if (slot <= 0 || std::fabs(now - slot) > 0.1) slot = now - refresh;
            refreshes = std::max(1, int(std::lround((now - slot) / refresh)));
            slot += refreshes * refresh;
            slot += (now - slot) * 0.05;  // follows the display clock, not its wobble
            if (g_config.turnDisplayClock) dt = float(refreshes * refresh);
            // Walking too: each step moves you by the time its pictures are shown.
            if (g_config.walkDisplayClock) player::addShownTime(float(refreshes * refresh));
        }
        last = t;
        lastShown = time;
        if (dt > 0.1f) dt = 0.1f;
        benchmark::stick(right.x);  // turn benchmark: the stick pushed fully (benchmark.cpp)
        float a = std::fabs(right.x);
        g_turnChecking = a > 0.15f;
        if (g_turnChecking && step > 0 && step < 0.1) {
            TurnCheck& c = g_turnCheck;
            c.stepMin = std::min(c.stepMin, step * 1000);
            c.stepMax = std::max(c.stepMax, step * 1000);
            c.stepSum += step * 1000;
            ++c.steps;
            c.missed += refreshes - 1;
            benchmark::missedRefreshes(refreshes - 1);
        }
        static bool snapArmed = true;
        if (g_config.snapTurn) {
            // Snap turning: one fixed step per push past 0.6, re-armed when the stick comes back under 0.3.
            // No smooth turning, so the world never slides under you.
            if (a < 0.3f) snapArmed = true;
            if (snapArmed && a > 0.6f) {
                snapArmed = false;
                const float step = std::clamp(g_config.snapAngle, 5.f, 180.f) * 0.0174533f * (right.x > 0 ? -1 : 1);
                camera::turnBody(step);
                benchmark::turned(step);
            }
            g_stickRate = 0;
        } else if (a > 0.15f) {
            float shaped = (a - 0.15f) / 0.85f;
            shaped *= shaped;
            g_stickRate = (right.x > 0 ? -1 : 1) * shaped * g_config.bodyTurnRate * 0.0174533f;
            camera::turnBody(g_stickRate * dt);
            benchmark::turned(g_stickRate * dt);
        }
        // Up/down tilts the view, only while it lies with your Sim's body
        // (WickedWhims animations and the like); the camera drops it otherwise.
        float b = std::fabs(right.y);
        if (b > 0.15f) {
            float shaped = (b - 0.15f) / 0.85f;
            shaped *= shaped;
            camera::pitchBody((right.y > 0 ? 1 : -1) * shaped * g_config.bodyTurnRate * 0.0174533f * dt);
        }
        // Up/down would scroll the game's camera, and scrolling it out far
        // enough opens the map view (the Travel button).
        right = {0, 0};
    } else if (gamepad::installed() && gamepad::reads() > 0) {
        // Test build: the left stick goes through the game's own gamepad
        // support (analog); the right stick still uses keys and the wheel.
        gamepad::State pad;
        pad.leftX = left.x;
        pad.leftY = left.y;
        gamepad::set(pad);
    } else {
        c.panX = left.x;
        c.panY = left.y;
    }
    c.rotate = -right.x;  // push right to turn the view right
    c.zoom = right.y;
    c.turnSpeed = g_config.turnSpeed;

    XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    // A finger near the pie (either hand), the phone (the hand not holding it)
    // or the wrist menus (the right) has the mouse.
    const bool bothHands[2] = {true, true}, rightHand[2] = {false, true},
               freeHand[2] = {g_phoneHand == 1, g_phoneHand == 0};
    // While the laser is held on (grip + A / X), it has the mouse, not a finger.
    const bool pulled[2] = {triggerDown[0] && !triggerWas[0], triggerDown[1] && !triggerWas[1]};
    // A hand swept fast across the pie closes it (each hand's speed is kept up every frame).
    shovePie(hands);
    // In first person the pie is drawn in the world beside your Sim's hands, so the
    // finger you see pokes it (as on the phone), not the 10 cm guess.
    // Sims 1 on a computer screen first (nothing of The Sims 4's is in front of it then).
    const bool pokingSims1 = laserHand < 0 && !g_pie.active && !g_objMenu.active && pokeSims1(hands, triggerDown, c.rightButton);
    if (pokingSims1) {
        c.leftButton = c.rightButton = false;
        c.hasTarget = false;
    }
    bool poking = pokingSims1 || (laserHand < 0 && ((g_pie.active && pokeSheet(g_pie, hands, bothHands, triggerDown, pulled, c, camera::bodyActive())) ||
                                    (g_phone.active && pokeSheet(g_phone, hands, freeHand, triggerDown, pulled, c, true)) ||
                                    (g_phoneSheet.m.active && pokeHud(g_phoneSheet, hands, freeHand)) ||
                                    (g_objMenu.active && pokeSheet(g_objMenu, hands, bothHands, triggerDown, pulled, c)) ||
                                    (g_wrist.active && pokeSheet(g_wrist, hands, rightHand, triggerDown, pulled, c)) ||
                                    pokeHuds(hands)));
    bool onPie = poking, onWorld = false;
    // The grip pressed while the laser is on (the game camera; in first person the
    // grip holds the laser itself): it can take the settings menu by its title bar.
    static bool laserGripWas = false;
    const bool laserGrip = laserHand >= 0 && floatState(g_gripAction, laserHand) > 0.6f;
    const bool laserGripPress = laserGrip && !laserGripWas && !(mode() == Mode::Stereo && camera::bodyActive());
    laserGripWas = laserGrip;
    bool settingsDragged = false;  // the laser moved the settings menu this frame
    bool laserIntoWorld = false;   // the laser points into the world: the game's pick finds where it ends
    // Pointing turns the laser off: the finger alone drives the game's pick.
    if (!poking && pointing < 0 && laserHand >= 0 && g_aimSpaces[g_pointerHand] &&
        XR_SUCCEEDED(xrLocateSpace(g_aimSpaces[g_pointerHand], g_appSpace, time, &aim)) &&
        (aim.locationFlags & needed) == needed) {
        XrVector3f o = aim.pose.position, d = forwardOf(aim.pose.orientation);
        smoothAim(o, d);
        XrVector3f panelHit;
        float pu = 0, pv = 0;
        bool onPanel = false, onTabletBlank = false;
        // The notification card in front of you: the trigger held on it a moment takes it.
        {
            static ULONGLONG holdSince = 0;
            XrVector3f hit{};
            if (g_noteDrag.hand >= 0 && g_noteDrag.laser) {
                if (g_noteDrag.hand == laserHand && triggerDown[laserHand] && g_noteCardFront) {
                    g_noteDrag.held = {o.x + d.x * g_noteDrag.distance, o.y + d.y * g_noteDrag.distance,
                                       o.z + d.z * g_noteDrag.distance};
                    g_laserFrom = o;
                    g_laserTo = g_noteDrag.held;
                    g_laserValid = true;
                    settingsDragged = true;  // nothing else takes the laser while it moves the card
                } else {
                    XrPosef head;
                    const bool headOk = locateHead(time, head);
                    endNoteDrag(head, headOk);
                }
                holdSince = 0;
            } else if (g_noteDrag.hand < 0 && g_settingsDrag.hand < 0 && laserHand >= 0 && triggerDown[laserHand] &&
                       laserOnNoteCard(o, d, hit)) {
                const ULONGLONG now = GetTickCount64();
                if (!holdSince) {
                    holdSince = now;
                } else if (now - holdSince > 350) {
                    startNoteDrag(laserHand, true, hit);
                    const XrVector3f along = sub3(hit, o);
                    g_noteDrag.distance = std::sqrt(dot3(along, along));
                    holdSince = 0;
                    settingsDragged = true;
                }
            } else {
                holdSince = 0;
            }
        }
        SettingsDrag& drag = g_settingsDrag;
        if (drag.hand >= 0 && drag.laser && drag.hand == laserHand && settings::isOpen() &&
            (drag.grip ? floatState(g_gripAction, laserHand) > 0.35f : triggerDown[laserHand])) {
            // Moving the settings menu: the held point stays on the laser.
            const XrVector3f at{o.x + d.x * drag.distance, o.y + d.y * drag.distance, o.z + d.z * drag.distance};
            XrPosef head;
            if (locateHead(time, head)) moveSettings(at, head.position);
            g_laserFrom = o;
            g_laserTo = at;
            g_laserValid = true;
            if (c.leftButton) g_pieDismissHold = true;
            c.leftButton = false;
            settingsDragged = true;
        }
        const bool onSettings = !settingsDragged && hitSettings(o, d, panelHit, pu, pv);  // in front of everything else
        // The laser on your other palm: the trigger opens your own pie menu.
        XrVector3f palmHit{};
        const bool onPalm = !settingsDragged && !onSettings && mode() == Mode::Stereo && camera::bodyActive() &&
                            !g_phone.active && !g_phoneSheet.m.active && !g_objMenu.active &&
                            laserOnPalm(hands, laserHand, o, d, palmHit);
        const bool onScreen = !settingsDragged && !onSettings && !onPalm && !g_pie.active && !g_objMenu.active &&
                              hitSims1(o, d, panelHit, pu, pv);
        // A / X on Claude's screen is a click on the TV or computer itself, so its
        // pie menu (with Close Claude) opens (user 2026-10-15 00:53Z). Sims 1 keeps
        // A / X as its right button.
        const bool claudeWorldClick = onScreen && claudescreen::on() && c.rightButton;
        g_laserOnClaude = onScreen && claudescreen::on() && !claudeWorldClick;
        const bool onSims1 = onScreen && !claudeWorldClick;
        if (claudeWorldClick) {
            c.leftButton = true;
            c.rightButton = false;
        }
        if (settingsDragged || onSettings || onSims1 || onPalm) {
        } else if (!g_pie.active && g_phoneSheet.m.active && laserPhone(o, d, c.leftButton, panelHit)) {
            // The phone's screen in your hand, in front of a popup card behind it.
            onPanel = onPie = true;
            onTabletBlank = true;
        } else if (g_pie.active || g_phone.active || g_objMenu.active || g_wrist.active) {
            // The pie, the phone, a picker on its object or the wrist menus replace the panel;
            // transparent parts let the laser through.
            if (hitSheet(g_pie.active     ? g_pie
                         : g_phone.active ? g_phone
                         : g_objMenu.active ? g_objMenu
                                            : g_wrist,
                         o, d, panelHit, pu, pv)) {
                probeUiAlpha(g_uiFrame, pu, pv, g_pie.active ? pieNearPixels() : 0);
                if (g_pie.active) {
                    // The whole pie sheet holds the laser: a click in a gap
                    // went to the world behind it, which opened another menu.
                    // A click off the pie closes it; a near miss does nothing.
                    onPanel = onPie = true;
                    if (g_uiAlphaUnderLaser <= kPieOpaque) {
                        c.leftButton = false;
                        if (laserClick && g_uiAlphaNearLaser <= kPieOpaque && pieSettled()) dismissPie("clicked outside");
                    }
                } else if (g_phone.active && g_phone.solid) {
                    // The tablet: all of it is in front of the world.
                    onPanel = true;
                    onPie = g_uiAlphaUnderLaser > kPieOpaque;
                    onTabletBlank = !onPie;
                } else {
                    onPanel = onPie = g_uiAlphaUnderLaser > kPieOpaque;
                }
            }
        } else if (laserHud(o, d, c.leftButton, panelHit)) {
            // Our HUD: the laser stops there; the trigger presses its buttons, not the game's.
            onPanel = onPie = true;
            onTabletBlank = true;
        } else {
            onPanel = mode() == Mode::Stereo && g_uiFrame && frontPanelShown() && hitPanel(o, d, desc, panelHit, pu, pv);
            if (onPanel) probeUiAlpha(g_uiFrame, pu, pv);
        }
        if (settingsDragged) {
        } else if (onPalm) {
            g_laserFrom = o;
            g_laserTo = palmHit;
            g_laserValid = true;
            onPie = true;
            c.leftButton = false;
            XrPosef head;
            if (laserClick && locateHead(time, head)) requestSelfPie(laserHand, "laser on the palm", head);
        } else if (onSims1) {
            // Sims 1's mouse: the trigger is its left button, A / X its right (Claude: the trigger only).
            g_laserFrom = o;
            g_laserTo = panelHit;
            g_laserValid = true;
            sims1Mouse(pu, pv, (c.leftButton ? 1 : 0) | (c.rightButton ? 2 : 0));
            c.leftButton = c.rightButton = false;
        } else if (onSettings) {
            // Nothing behind the settings menu gets the trigger, until it is let go.
            g_laserFrom = o;
            g_laserTo = panelHit;
            g_laserValid = true;
            if (c.leftButton) g_pieDismissHold = true;
            c.leftButton = false;
            if ((laserClick || laserGripPress) && settings::onTitleBar(pu, pv)) {
                drag.grip = !laserClick;
                startSettingsDrag(laserHand, true, panelHit);
                const XrVector3f along = sub3(panelHit, o);
                drag.distance = std::sqrt(dot3(along, along));
                settingsDragged = true;
            }
            int result = settingsDragged ? settings::kNothing : settings::point(pu, pv, laserClick);
            if (result & settings::kCloseRequested)
                closeSettings();
            else if (result & settings::kChanged)
                buzz(g_pointerHand, 0.15f, 0.015f);
        } else if (onTabletBlank) {
            // An empty part of the tablet: the laser stops there; no click, the mouse stays.
            g_laserFrom = o;
            g_laserTo = panelHit;
            g_laserValid = true;
            c.leftButton = false;
        } else if (onPanel && (onPie || g_uiAlphaUnderLaser > 0.04f)) {
            // Pointing at a menu or button on the UI panel.
            g_laserFrom = o;
            g_laserTo = panelHit;
            g_laserValid = true;
            c.hasTarget = true;
            c.u = pu;
            c.v = pv;
        } else if (mode() == Mode::Stereo && camera::available()) {
            const float scale = camera::worldScale(), longest = 30.0f / scale;
            float floor = 0, t = longest;
            if (camera::floorInXr(floor) && d.y < -0.01f) {
                float hit = (floor - o.y) / d.y;
                if (hit > 0 && hit < t * 4) t = hit;
            }
            // Where it really ends: the game's own pick along the laser, made
            // in each frame's eye camera (touch.h), so it ends on what it
            // points at, the same in both eyes. The floor or 30 m is only the
            // start, and what is used while no answer is fresh.
            camera::Vec3 wo, wd;
            if (scale > 0 && camera::xrToWorld({o.x, o.y, o.z}, wo) && camera::xrDirectionToWorld({d.x, d.y, d.z}, wd)) {
                touch::setLaser(wo, wd, std::max(longest, t) * scale, t * scale);
                laserIntoWorld = true;
                const touch::LaserEnd end = touch::laserEnd();
                if (end.valid && now - end.tick < 250) t = end.length / scale;
            }
            XrVector3f p{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
            g_laserFrom = o;
            g_laserTo = p;
            g_laserValid = true;
            // The game picks through the camera it last rendered with, so put
            // the mouse where the laser end sits in the latest eye's pick frame.
            float u, v;
            if (g_lastEye >= 0 && g_eyeValid[g_lastEye] &&
                projectNormalized(p, g_eyePose[g_lastEye], pickFov(g_eyeFov[g_lastEye]), u, v) && u > 0.015f &&
                u < 0.985f && v > 0.015f && v < 0.985f) {
                c.hasTarget = true;
                c.u = u;
                c.v = v;
                onWorld = true;
            }
        } else if (mode() == Mode::Quad && d.z < -0.01f) {
            float t = (-g_config.quadDistance - o.z) / d.z;
            float width = g_config.quadWidth, height = width * desc.Height / desc.Width;
            float u = (o.x + d.x * t + width / 2) / width, v = (height / 2 - (o.y + d.y * t)) / height;
            if (t > 0 && u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                g_laserFrom = o;
                g_laserTo = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
                g_laserValid = true;
                c.hasTarget = true;
                c.u = u;
                c.v = v;
            }
        }
    }
    if (!laserIntoWorld) touch::laserOff();
    // The laser let go of the settings menu (its button opened, or the laser went off).
    if (g_settingsDrag.hand >= 0 && g_settingsDrag.laser && !settingsDragged) endSettingsDrag(time);
    if (laserClick) {
        static int logged = 0, loggedPhone = 0;  // the phone-out clicks count apart (A7 diagnostics)
        if (g_phoneOut ? loggedPhone++ < 60 : logged++ < 60)
            LOG("Laser: click %s (pie %s, UI alpha %.2f, near %.2f%s%s%s)",
                poking ? "taken by a finger poke" : pointing >= 0 ? "ignored: the other hand points"
                : onPie ? "on a menu" : onWorld ? "into the world" : c.hasTarget ? "on the UI" : "on nothing",
                g_pie.active ? "open" : "closed", g_uiAlphaUnderLaser, g_uiAlphaNearLaser,
                gameMenuOpen() ? ", game menu open" : "", g_pieDismissHold ? ", held after a pie close" : "",
                g_phoneOut ? (g_phoneSheet.m.active ? ", phone out (its screen up)" : ", phone out") : "");
    }
    // A touch clicks once, where the game picked through the fingertip; the
    // finger has to come away before it clicks again.
    bool touchClick = false;
    static int touchFrames = 0;  // frames the touch click stays held
    static float touchU = 0, touchV = 0;
    static bool touchArmed = true;
    static ULONGLONG apartSince = 0;
    if (pointing >= 0 && !poking) {
        touch::Contact t = touch::latest();
        bool contact = t.valid && t.touching && now - t.tick < 200;
        c.leftButton = false;
        if (touchFrames > 0) {
            --touchFrames;
            c.hasTarget = true;
            c.u = touchU;
            c.v = touchV;
            c.leftButton = true;
        } else if (contact && touchArmed) {
            touchArmed = false;
            touchFrames = 3;
            touchU = t.u;
            touchV = t.v;
            c.hasTarget = true;
            c.u = t.u;
            c.v = t.v;
            c.warp = true;  // the mouse goes there as the button goes down: the click uses that pick
            c.leftButton = true;
            touchClick = true;
            static int logged = 0;
            if (logged++ < 30)
                LOG("Touch: clicked object %016llx (pick type %d, %.3f m from its surface) at %.3f %.3f",
                    static_cast<unsigned long long>(t.objectId), t.pickType, t.gap, t.u, t.v);
        }
        if (contact) {
            apartSince = 0;
        } else {
            if (!apartSince) apartSince = now;
            if (now - apartSince > 250) touchArmed = true;
        }
    } else {
        touchFrames = 0;
        touchArmed = true;
        apartSince = 0;
    }

    // The press that closed the pie clicks nothing while held.
    if (g_pieDismissHold) {
        if (c.leftButton || triggerDown[0] || triggerDown[1])
            c.leftButton = false;
        else
            g_pieDismissHold = false;
    }
    // First person: a press into the world is only ever a click. The button
    // goes up kWorldClickMs after it went down, however long the trigger is
    // held, so the game's live mode never starts dragging the object under
    // it (user 2026-10-16 12:13Z: clicking the grilled cheese still lifted it
    // and put it down elsewhere; the server already refuses the pick-up,
    // scriptmod livedrag.py, but the client moved it meanwhile).
    {
        // The mouse also stays where the press went down until well after the
        // button is up again: the laser's shake moved it a few pixels while the
        // button was down, which is the game's own drag, and small objects
        // still got lifted (user 2026-10-17 12:08Z, grip + A laser + trigger).
        constexpr ULONGLONG kWorldClickMs = 80, kWorldHoldMs = 300;
        static bool rawWas = false, worldPress = false;
        static ULONGLONG worldPressAt = 0;
        static float pressU = 0, pressV = 0;
        const bool raw = c.leftButton;
        if (!raw) {
            worldPress = false;
        } else if (!rawWas && onWorld && !onPie && camera::bodyActive()) {
            worldPress = true;
            worldPressAt = now;
            pressU = c.u;
            pressV = c.v;
        }
        rawWas = raw;
        if (worldPress && now - worldPressAt > kWorldClickMs) c.leftButton = false;
        if (worldPressAt && now - worldPressAt < kWorldHoldMs && camera::bodyActive() && c.hasTarget && !onPie) {
            c.u = pressU;
            c.v = pressV;
        }
    }
    // A click into the world may open a pie menu there; a click off the pie
    // (laser or touch) only closes it.
    static bool leftWas = false;
    if (c.leftButton && !leftWas && g_pie.active && !onPie && pieSettled()) {
        dismissPie("clicked elsewhere");
        c.leftButton = false;
    }
    if (c.leftButton && !leftWas) {
        if (touchClick) {
            g_lastWorldClick.valid = true;
            g_lastWorldClick.u = c.u;
            g_lastWorldClick.v = c.v;
            g_lastWorldClick.point = fingertip(hands[pointing]);
            g_lastWorldClick.tick = now;
            g_clickHand.store(pointing, std::memory_order_relaxed);
            g_clickHandTick.store(now, std::memory_order_relaxed);
        } else if (onWorld) {
            g_lastWorldClick.valid = true;
            g_lastWorldClick.u = c.u;
            g_lastWorldClick.v = c.v;
            g_lastWorldClick.point = g_laserTo;
            g_lastWorldClick.tick = now;
            g_clickHand.store(g_pointerHand, std::memory_order_relaxed);
            g_clickHandTick.store(now, std::memory_order_relaxed);
        } else {
            g_lastWorldClick.valid = false;
        }
    }
    leftWas = c.leftButton;
    // The phone: the menu's Escape after the pie's, and telling Python it's away after the menu closed.
    if (g_phoneMenuAt && now >= g_phoneMenuAt && g_escapeFrames == 0) {
        g_phoneMenuAt = 0;
        if (g_phoneOut && !g_tablet && !gameMenuOpen() && !g_popupOpen) {
            g_escapeFrames = 3;
            g_phoneOpenedMenu = true;
        }
    }
    if (g_phoneAwayAt && now >= g_phoneAwayAt) {
        g_phoneAwayAt = 0;
        grab::pushControl(4);
    }
    // When the game's pie was already gone, the pie's Escape opens the game's
    // menu instead: unseen in first person, it takes every click until closed.
    // Close it again (unless the phone or the tablet is out, which show it).
    // The game may ignore an Escape while its menu is still opening, so keep
    // pressing it (every 0.6 s, for up to 3 s) until the menu says it closed.
    {
        static ULONGLONG closingUntil = 0, lastPress = 0;
        static int presses = 0;
        const bool menuNow = g_gameMenuReported && g_gameMenuOpen.load();
        if (menuNow && !closingUntil && g_pieEscapeTick && now - g_pieEscapeTick < 1500 && !g_phoneOut && !g_buildBuy) {
            closingUntil = now + 3000;
            lastPress = 0;
            presses = 0;
            static int logged = 0;
            if (logged++ < 20) LOG("Pie: its Escape opened the game menu (the game's pie was already gone); closing it again");
        }
        if (closingUntil) {
            if (!menuNow || g_phoneOut || now > closingUntil) {
                static int logged = 0;
                if (logged++ < 20)
                    LOG("Pie: the game menu %s after %d Escape presses", menuNow ? "stayed open" : "closed again", presses);
                closingUntil = 0;
            } else if (g_escapeFrames == 0 && now - lastPress >= 600) {
                g_escapeFrames = 3;
                lastPress = now;
                ++presses;
            }
        }
        if (g_pieEscapeTick && now - g_pieEscapeTick >= 1500) g_pieEscapeTick = 0;
    }
    // Escape closes the game's own pie menu (and opens or closes the game menu for the phone).
    if (g_escapeFrames > 0) {
        --g_escapeFrames;
        c.escape = true;
    }
    if (g_buildKeyFrames > 0) {
        --g_buildKeyFrames;
        c.buildMode = true;
    }
    // Your Sim's eyes look where the grip + A / X laser lands (first person).
    {
        const bool look = mode() == Mode::Stereo && camera::bodyActive() && laserHand >= 0 && g_laserValid && !g_tipOnly;
        const float at[3] = {g_laserTo.x, g_laserTo.y, g_laserTo.z};
        armik::setLookTarget(look, at);
        // The hand pointing the laser lines its index finger up with it (armik).
        XrVector3f along = normalize3(sub3(g_laserTo, g_laserFrom));
        for (int i = 0; i < 2; ++i)
            armik::setPointing(i, look && i == laserHand && dot3(along, along) > 0.5f,
                               {g_laserFrom.x, g_laserFrom.y, g_laserFrom.z}, {along.x, along.y, along.z});
    }
    // Holding the controllers (a finger on a sensor, a button, a stick) keeps
    // the game window in front; only asked while something else has it.
    if (!pointer::inFront(g_gameWindow))
        for (int i = 0; i < 2 && !c.inUse; ++i) {
            XrVector2f stick = stickState(i);
            c.inUse = boolState(g_indexTouchAction, i) || boolState(g_thumbTouchAction, i) ||
                      floatState(g_triggerAction, i) > 0.1f || floatState(g_gripAction, i) > 0.3f ||
                      boolState(g_primaryAction, i) || boolState(g_secondaryAction, i) || boolState(g_menuAction, i) ||
                      std::fabs(stick.x) + std::fabs(stick.y) > 0.3f;
        }
    // Claude's window keeps the front (your real keyboard types into it) while
    // the laser is on its screen or the controllers are put down; using the
    // controllers anywhere else gives the game the front back, so clicks work
    // (user 2026-10-15 01:48Z: nothing could be clicked while Claude was open).
    if (claudescreen::on() && g_laserOnClaude) c.inUse = false;
    if (!claudescreen::on()) g_laserOnClaude = false;
    claudescreen::setGameWindow(g_gameWindow);
    holdSelfPieMouse(c, mode() == Mode::Stereo && camera::bodyActive());
    pointer::update(g_gameWindow, c);
}

