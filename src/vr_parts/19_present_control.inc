// The game exits without running Python's atexit, so OpenXR was never shut
// down and Virtual Desktop treated it as a crash (the headset disconnected).
// End the session cleanly when the game window closes instead.
static WNDPROC g_originalWndProc = nullptr;

static LRESULT CALLBACK closingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Claude open on an in-game computer: its window has the front, the game
    // isn't told (so it keeps running as if in front).
    if (claudescreen::keepsGameFocused() &&
        ((msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || msg == WM_KILLFOCUS))
        return 0;
    if (msg == WM_DESTROY) {
        LOG("Game window closing: shutting OpenXR down");
        shutdown();
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wp, lp);
}

static void watchWindowClose(HWND window) {
    if (!window || g_originalWndProc) return;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&closingWndProc)));
}

void onPresent(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) return;
    HWND window = nullptr;
    DXGI_SWAP_CHAIN_DESC scDesc;
    if (!g_gameWindow && SUCCEEDED(swapchain->GetDesc(&scDesc))) window = scDesc.OutputWindow;
    onFrame(backbuffer, window, false);
    backbuffer->Release();
}

int onFrame(ID3D11Texture2D* backbuffer, HWND window, bool ownMirror) {
    static uint64_t frames = 0;
    crashlog::note(crashlog::kPresent);
    if (frames++ == 0) LOG("First Present seen");
    if (frames % 3000 == 0) LOG("Present #%llu, session %s", static_cast<unsigned long long>(frames), stateName(g_sessionState));
    // Shutdown runs on another thread (the game's Python); never tear down mid-frame.
    std::lock_guard<std::mutex> frameLock(g_frameMutex);
    if (!g_instance) return -1;
    g_ownMirror = ownMirror;
    g_frameEye = -1;
    // The UI drawn this frame went to the panel texture if redirection was on.
    const bool uiOwnLayer = g_config.uiPanel && g_session && g_sessionRunning && mode() == Mode::Stereo && camera::available();
    bubbles::setOptions(g_config);
    bubbles::setWanted(uiOwnLayer && g_config.bubbles3d);
    g_uiFrame = uihook::onPresent(backbuffer, uiOwnLayer);
    ID3D11Device* device = nullptr;
    backbuffer->GetDevice(&device);
    {
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        gpuload::onFrame(device, context);
        nearplane::collect(context);
        context->Release();
        watchVideoMemory();
    }
    if (!g_gameWindow && window) {
        g_gameWindow = window;
        watchWindowClose(g_gameWindow);
    }

    if (!g_session) createSession(device);
    if (g_session) pollEvents();

    // Stall diagnostics: was a slow frame the game's own work, or a runtime call?
    static LARGE_INTEGER frequency{}, lastPresent{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    auto now = [] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t;
    };
    auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b) {
        return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(frequency.QuadPart);
    };
    LARGE_INTEGER frameStart = now();
    double gameMs = lastPresent.QuadPart ? ms(lastPresent, frameStart) : 0, waitMs = 0, renderMs = 0, endMs = 0;

    bool stereoFrame = false;
    if (g_session && g_sessionRunning) {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        XrResult waited;
        {
            XrCallTimer timer("xrWaitFrame");
            waited = xrWaitFrame(g_session, &waitInfo, &frame);
        }
        LARGE_INTEGER afterWait = now();
        waitMs = ms(frameStart, afterWait);
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (waited == XR_ERROR_SESSION_LOST) {
            destroySession();
        } else if (check(waited, "xrWaitFrame") && check(xrBeginFrame(g_session, &beginInfo), "xrBeginFrame")) {
            static XrDuration loggedPeriod = 0;
            static int periodLogs = 0;
            if (frame.predictedDisplayPeriod > 0 && frame.predictedDisplayPeriod != loggedPeriod && periodLogs < 20) {
                // Virtual Desktop doubles the period while its frame smoothing
                // (Synchronous Spacewarp) runs.
                if (!loggedPeriod)
                    LOG("Headset refresh: %.1f Hz (a frame every %.2f ms)", 1e9 / frame.predictedDisplayPeriod,
                        frame.predictedDisplayPeriod / 1e6);
                else
                    LOG("Headset frame period now %.2f ms (was %.2f): frame smoothing %s", frame.predictedDisplayPeriod / 1e6,
                        loggedPeriod / 1e6, frame.predictedDisplayPeriod > loggedPeriod ? "on" : "off");
                ++periodLogs;
                loggedPeriod = frame.predictedDisplayPeriod;
            }
            g_displayPeriod = frame.predictedDisplayPeriod;
            linkSample(frame.predictedDisplayTime);
            if (g_recenterRequested.exchange(false) || !g_appSpace) recenter(frame.predictedDisplayTime);

            D3D11_TEXTURE2D_DESC desc;
            backbuffer->GetDesc(&desc);
            updateControls(frame.predictedDisplayTime, desc);
            drawHud(backbuffer);
            XrCompositionLayerQuad quad, uiQuad, pieQuad, beamQuad, tipQuad, phoneQuad, settingsQuad, sleepQuad;
            XrCompositionLayerProjection projection;
            XrCompositionLayerProjectionView views[2];
            const XrCompositionLayerBaseHeader* layers[10] = {};
            uint32_t layerCount = 0;
            auto add = [&](const void* layer) {
                auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(layer);
                if (saneLayer(header)) {
                    layers[layerCount++] = header;
                    return;
                }
                static int dropped = 0;
                if (dropped++ < 10) LOG("Dropped a broken layer (type %d) instead of sending it to the headset", header->type);
            };

            bool stereo = frame.shouldRender && g_appSpace && mode() == Mode::Stereo && camera::available();
            stereoFrame = stereo;
            if (!stereo) pieInWorld(false);
            if (frame.shouldRender && g_appSpace) {
                if (!stereo && g_renderingEye >= 0) resetStereo();
                bool scene = false;
                if (stereo) {
                    if (renderStereo(backbuffer, desc, frame.predictedDisplayTime, projection, views)) {
                        add(&projection);
                        if (g_bodyLayerReady) add(&g_bodyProjection);
                        scene = true;
                        // One UI layer at most: the pie at your hand, the phone, a picker on its object,
                        // the wrist menus or the panel.
                        bool showPie = g_pie.active, showPhone = !showPie && g_phone.active,
                             showObject = !showPie && !showPhone && g_objMenu.active,
                             showWrist = !showPie && !showPhone && !showObject && g_wrist.active,
                             showPanel = !showPie && !showPhone && !showObject && !showWrist && frontPanelShown();
                        bool pieIn3d = pieInWorld(showPie);
                        // Asleep (first person): the clock over the closed eyelids.
                        if (hudSleepLayer(hud::texture(), sleepQuad)) add(&sleepQuad);
                        if ((showPie || showPhone || showObject || showWrist || showPanel) && renderUiPanel(uiQuad)) {
                            bool phoneIn3d = showPhone && phone3d::drawnRecently();
                            if (showPhone && !phoneIn3d && !g_tablet) {
                                // Flat fallback: our phone's body first, its screen's menu on top.
                                ID3D11Device* device = nullptr;
                                backbuffer->GetDevice(&device);
                                if (phoneBodyLayer(device, phoneQuad)) add(&phoneQuad);
                                device->Release();
                            }
                            if ((showObject && phone3d::panelDrawnRecently()) || (showPie && pieIn3d)) {
                                // drawn inside the 3D view (phone3d): no flat layer over your hands
                            } else if (showPie || (showPhone && !phoneIn3d) || showObject || showWrist) {
                                sheetLayer(showPie ? g_pie : showPhone ? g_phone : showObject ? g_objMenu : g_wrist, uiQuad,
                                           pieQuad);
                                add(&pieQuad);
                            } else if (showPanel) {  // not with the phone drawn in 3D: its screen has the menu
                                add(&uiQuad);
                            }
                        }
                    }
                } else if (renderQuad(backbuffer, desc, quad)) {
                    // The black room as one projection layer under the screen (voidLayer).
                    ID3D11Device* device = nullptr;
                    backbuffer->GetDevice(&device);
                    bool room = voidLayer(device, frame.predictedDisplayTime, projection, views);
                    device->Release();
                    if (room) add(&projection);
                    static int roomLogged = -1;
                    if (roomLogged != (room ? 1 : 0)) {
                        roomLogged = room ? 1 : 0;
                        LOG("Flat screen: %s", room ? "sent over a black projection layer (1 projection + quads)"
                                                    : "black projection layer FAILED; quads only");
                    }
                    add(&quad);
                    scene = true;
                }
                if (scene) {
                    ID3D11Device* device = nullptr;
                    backbuffer->GetDevice(&device);
                    if (settingsLayer(device, settingsQuad)) add(&settingsQuad);
                    if (renderBeam(device, frame.predictedDisplayTime, beamQuad, tipQuad)) {
                        // First person: the marker only with the laser, not under a poking finger.
                        if (!g_tipOnly) add(&beamQuad);
                        if (!g_tipOnly || !camera::bodyActive()) add(&tipQuad);
                    }
                    device->Release();
                }
            }

            {
                ID3D11Device* device = nullptr;
                ID3D11DeviceContext* context = nullptr;
                backbuffer->GetDevice(&device);
                device->GetImmediateContext(&context);
                screenshot::pump(context);
                context->Release();
                device->Release();
            }

            g_sentLayers = layerCount;
            for (uint32_t i = 0; i < layerCount; ++i) {
                const XrCompositionLayerBaseHeader* layer = layers[i];
                g_sentLayer[i] = {layer->type, {}};
                if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                    g_sentLayer[i].size = reinterpret_cast<const XrCompositionLayerProjection*>(layer)
                                              ->views[0].subImage.imageRect.extent;
                else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                    g_sentLayer[i].size =
                        reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage.imageRect.extent;
            }

            LARGE_INTEGER beforeEnd = now();
            renderMs = ms(afterWait, beforeEnd);
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frame.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = layerCount;
            endInfo.layers = layerCount ? layers : nullptr;
            XrResult ended;
            {
                XrCallTimer timer("xrEndFrame");
                ended = xrEndFrame(g_session, &endInfo);
            }
            check(ended, "xrEndFrame");
            endMs = ms(beforeEnd, now());
        }
        // A runtime that blocks every frame: the link to the headset is
        // struggling (Wi-Fi, the stream backing up) or gone (the headset app
        // died). Short stalls pass on their own, so they are ridden out; one
        // that lasts 12 s ends the session (cleanly) to reconnect later, so
        // the game isn't left waiting on a runtime whose headset is gone (on
        // 2026-10-13 the link crawled at 190 ms a frame, Virtual Desktop
        // dropped the headset and the game died). It only counts as recovered
        // after a second of normal frames: a fast frame or two in a crawl
        // doesn't start the clock again.
        static ULONGLONG stalledSince = 0, backoff = 5000, smoothSince = 0;
        static int stalledFrames = 0;
        ULONGLONG tick = GetTickCount64();
        bool onset = false;
        // Headset off your head (pushed up on your forehead, taken off): the
        // runtime drops the session from focused to visible and the headset
        // stops asking for frames, so frames take 100 ms. That is a pause,
        // not a broken link: other VR games just wait, and ending the session
        // there is what turned taking the headset off into "VR crashed".
        // The stall clock only runs while the headset is on (focused).
        bool headsetOn = g_sessionState == XR_SESSION_STATE_FOCUSED;
        static bool offLogged = false;
        if (!headsetOn && g_sessionState == XR_SESSION_STATE_VISIBLE && waitMs + endMs > 80 && !offLogged) {
            offLogged = true;
            LOG("Headset link: slow while the headset is off your head (session visible); waiting, not ending it");
        }
        if (headsetOn) offLogged = false;
        if (headsetOn && waitMs + endMs > 80) {
            if (!stalledSince) stalledSince = tick;
            if (stalledFrames++ == 0) {
                LOG("Headset link slow (%.0f ms for a frame); waiting for it to recover", waitMs + endMs);
                logLoad();
                onset = true;
            }
            smoothSince = 0;
        } else {
            if (!smoothSince) smoothSince = tick;
            if (stalledSince && tick - smoothSince >= 1000) {
                if (stalledFrames >= 5)
                    LOG("Headset link recovered after %d slow frames over %.1f s", stalledFrames,
                        (smoothSince - stalledSince) / 1000.0);
                stalledSince = 0;
                stalledFrames = 0;
            }
            if (tick - smoothSince > 30000) backoff = 5000;
        }
        linkTick(tick, waitMs + endMs, stalledSince != 0, onset);
        gpuload::yieldToStreamer(g_config.gpuYield);
        if (g_session && !g_exitRequested && stalledSince && tick - stalledSince > 12000) {
            LOG("Headset link stalled for 12 s (%.0f ms per frame); ending the session", waitMs + endMs);
            stalledSince = 0;
            stalledFrames = 0;
            requestSessionEnd(backoff);
            linkLost("stalled for 12 s");
            if (backoff < 60000) backoff *= 2;
            setStatusNote("headset link lost; reconnecting");
        }
    }
    // A requested end the runtime never acted on: destroy the session anyway.
    if (g_exitRequested && GetTickCount64() > g_exitDeadline) {
        LOG("The runtime didn't stop the session in time; closing it anyway");
        ULONGLONG delay = g_restartDelay;
        if (g_sessionRunning) xrEndSession(g_session);
        destroySession();
        g_nextSessionAttempt = GetTickCount64() + delay;
    }
    updateRenderSize(stereoFrame);
    lastPresent = now();
    // Frame pacing: frames that took far longer than the ones before them are
    // what you notice as a stutter. Counted, with the worst, every 3000 frames.
    {
        static double average = 0, worst = 0, oursWorst = 0, oursTotal = 0;
        static int spikes = 0, counted = 0;
        if (gameMs > 0 && gameMs < 1000) {
            if (average > 0 && gameMs > 2.0 * average && gameMs > 20) {
                ++spikes;
                if (gameMs > worst) worst = gameMs;
            }
            average = average > 0 ? average * 0.95 + gameMs * 0.05 : gameMs;
        }
        double ours = renderMs;
        oursTotal += ours;
        if (ours > oursWorst) oursWorst = ours;
        if (++counted == 3000) {
            gpuload::Lag lag = gpuload::take();
            LOG("Frame pacing: %d stutters (a frame over twice the average, worst %.0f ms), game %.1f ms a frame; "
                "VR work %.2f ms a frame, worst %.1f ms; graphics card done %.0f ms after a frame (worst %.0f), "
                "busy: %s; video memory %s",
                spikes, worst, average, oursTotal / counted, oursWorst, lag.averageMs, lag.worstMs,
                gpuload::busyText(gpuload::takeBusy()).c_str(), gpuload::memoryText().c_str());
            spikes = counted = 0;
            worst = oursWorst = oursTotal = 0;
        }
    }
    static uint64_t slowFrames = 0;
    static ULONGLONG lastSlowLog = 0;
    if (gameMs + waitMs + renderMs + endMs > 100) {
        ++slowFrames;
        ULONGLONG tick = GetTickCount64();
        if (tick - lastSlowLog > 2000) {
            lastSlowLog = tick;
            LOG("Slow frame #%llu: game %.0f ms, xrWaitFrame %.0f ms, copy %.0f ms, xrEndFrame %.0f ms, session %s",
                static_cast<unsigned long long>(slowFrames), gameMs, waitMs, renderMs, endMs, stateName(g_sessionState));
        }
    }

    device->Release();
    return g_frameEye;
}

// ---------------------------------------------------------------------------
// Control

void shutdown() {
    std::lock_guard<std::mutex> frameLock(g_frameMutex);
    if (!g_instance) return;
    if (g_session && g_sessionRunning) {
        // Ask the runtime to stop the session and wait (briefly) until it has.
        if (!g_exitRequested) {
            g_exitRequested = XR_SUCCEEDED(xrRequestExitSession(g_session));
            g_restartDelay = 0;
        }
        for (int i = 0; i < 100 && g_session && g_sessionRunning && g_exitRequested; ++i) {
            pollEvents();
            if (g_session && g_sessionRunning) Sleep(10);
        }
        if (g_session && g_sessionRunning) xrEndSession(g_session);
    }
    // Runtimes keep threads that touch the session; leaving it alive until the
    // process tears down crashes inside them (seen with Virtual Desktop).
    destroySession();
    xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    LOG("OpenXR shut down");
}

void requestRecenter() { g_recenterRequested = true; }

void setMode(Mode mode) {
    g_mode = static_cast<int>(mode);
    g_recenterRequested = true;
}

Mode mode() { return static_cast<Mode>(g_mode.load()); }

std::string status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    std::string s = "runtime=" + g_runtimeName;
    s += " session=" + std::string(stateName(g_sessionState));
    s += " mode=" + std::string(mode() == Mode::Stereo ? "stereo" : "quad");
    s += " camera=" + std::string(camera::available() ? "found" : "not found");
    s += " gamepad=" + std::string(!gamepad::installed() ? "off" : gamepad::reads() ? "read by game" : "not read yet");
    char scale[32];
    std::snprintf(scale, sizeof(scale), " scale=%.2f", camera::worldScale());
    s += scale;
    if (!g_statusNote.empty()) s += " note=\"" + g_statusNote + "\"";
    if (!g_lastError.empty()) s += " last_error=\"" + g_lastError + "\"";
    return s;
}

