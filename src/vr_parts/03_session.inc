// ---------------------------------------------------------------------------
// Session

static void destroySwapchain(Swapchain& s) {
    if (s.handle) sharpen::forgetTargets();
    if (s.handle) eyealign::forgetTargets();
    if (s.handle) bodylayer::forgetTargets();
    if (s.handle) deptheye::forgetTargets();
    if (s.handle) xrDestroySwapchain(s.handle);
    s = Swapchain{};
}

static void resetStereo() {
    camera::release();
    g_renderingEye = -1;
    g_eyeValid[0] = g_eyeValid[1] = false;
    g_leftHeld = false;
    g_mirrorValid = false;
    for (BodyState& state : g_bodyState) state = kBodyUnknown;
    g_bodyLayerReady = false;
}


static void releaseBeam();

static void destroySession() {
    gpuload::yieldToStreamer(false);
    releaseBeam();
    sharpen::release();
    eyealign::release();
    bodylayer::release();
    deptheye::release();
    if (g_mirror) g_mirror->Release();
    g_mirror = nullptr;
    g_mirrorValid = false;
    destroySwapchain(g_quadSwapchain);
    destroySwapchain(g_eyeSwapchains[0]);
    destroySwapchain(g_eyeSwapchains[1]);
    destroySwapchain(g_leftSpare);
    g_leftShown = 0;
    for (Swapchain& chain : g_bodyChains) destroySwapchain(chain);
    destroySwapchain(g_uiSwapchain);
    for (auto*& probe : g_uiProbe) {
        if (probe) probe->Release();
        probe = nullptr;
    }
    resetStereo();
    pointer::releaseAll();
    g_laserValid = false;
    g_actionsAttached = false;
    for (XrSpace* space :
         {&g_aimSpaces[0], &g_aimSpaces[1], &g_gripSpaces[0], &g_gripSpaces[1], &g_appSpace, &g_viewSpace, &g_localSpace}) {
        if (*space) xrDestroySpace(*space);
        *space = XR_NULL_HANDLE;
    }
    for (XrHandTrackerEXT& tracker : g_handTrackers) {
        if (tracker) xrDestroyHandTrackerEXT(tracker);
        tracker = XR_NULL_HANDLE;
    }
    g_handTracked[0] = g_handTracked[1] = false;
    if (g_session) xrDestroySession(g_session);
    g_session = XR_NULL_HANDLE;
    g_sessionRunning = false;
    g_renderSize[0] = g_renderSize[1] = 0;
    g_sessionState = XR_SESSION_STATE_UNKNOWN;
    g_recenterRequested = true;
    g_nextSessionAttempt = GetTickCount64() + 2000;
    g_exitRequested = false;
}

// Ends the session the way OpenXR wants it: ask the runtime to stop it, end it
// when it says it is stopping (pollEvents), then destroy it. Ending a running
// session directly is an error the runtime rejects (Virtual Desktop logged
// "xrEndSession failed with XR_ERROR_SESSION_NOT_STOPPING"), and destroying it
// mid-run can leave the streamer in a bad state.
static void requestSessionEnd(ULONGLONG restartDelay) {
    if (!g_session || g_exitRequested) return;
    if (!g_sessionRunning) {
        destroySession();
        g_nextSessionAttempt = GetTickCount64() + restartDelay;
        return;
    }
    g_exitRequested = check(xrRequestExitSession(g_session), "xrRequestExitSession");
    g_exitDeadline = GetTickCount64() + 3000;
    g_restartDelay = restartDelay;
    if (!g_exitRequested) {
        destroySession();
        g_nextSessionAttempt = GetTickCount64() + restartDelay;
    }
}

// After the headset link died (it stalled until we ended the session, or the
// runtime ended a session itself), Virtual Desktop takes a while to drop the
// headset. No new session until the runtime has said the headset is gone and
// then back (Virtual Desktop restarted in the headset), or kLinkRetry has
// passed. (The crashes this was first meant for, 2026-10-13 16:03 and 17:45,
// were stale events: see dropOldEvents.) Was 60 s; 2026-10-14 14:07 Virtual
// Desktop never said the headset was gone and the user gave up after 44 s.
static constexpr ULONGLONG kLinkRetry = 15000;
static ULONGLONG g_linkLostAt = 0;
static bool g_headsetWentAway = false;

static void linkLost(const char* why) {
    if (!g_linkLostAt) LOG("Headset link lost (%s); reconnecting once the headset is back", why);
    g_linkLostAt = GetTickCount64();
    g_headsetWentAway = false;
}

// Events the runtime still holds from the last session. Virtual Desktop keeps
// its queue across sessions and gives every session the same handle, so its
// IDLE and EXITING from a session we had already ended and destroyed arrived
// as if from the next one: we destroyed that new session at once, and
// destroying a session just made crashed inside Virtual Desktop's runtime
// (heap corruption, 2026-10-13 16:03 and 17:45). Read and dropped before a
// session is made.
static void dropOldEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    for (int i = 0; i < 64 && xrPollEvent(g_instance, &event) == XR_SUCCESS; ++i) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            LOG("Dropped an old event from the last session: %s",
                stateName(reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state));
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static bool createSession(ID3D11Device* device) {
    // Headset off or link not connected yet: retry every 2 s without spamming the log.
    if (GetTickCount64() < g_nextSessionAttempt) return false;
    g_nextSessionAttempt = GetTickCount64() + 2000;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(g_instance, &systemInfo, &g_system);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        static bool logged = false;
        if (!logged) LOG("Waiting for the headset: runtime reports no HMD connected (is the headset streaming?)");
        logged = true;
        if (g_linkLostAt && !g_headsetWentAway) {
            LOG("Headset link: the runtime has let the headset go; waiting for it to come back");
            g_headsetWentAway = true;
        }
        setStatusNote("waiting for headset");
        return false;
    }
    if (!check(result, "xrGetSystem")) return false;
    if (g_linkLostAt) {
        if (!g_headsetWentAway && GetTickCount64() - g_linkLostAt < kLinkRetry) {
            setStatusNote("headset link lost; reconnecting");
            return false;
        }
        LOG("Headset link: %s; making a new session", g_headsetWentAway ? "the headset is back" : "15 s passed");
        g_linkLostAt = 0;
    }
    {
        static bool logged = false;
        uint32_t modes = 0;
        if (!logged && xrEnumerateEnvironmentBlendModes &&
            XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                          0, &modes, nullptr))) {
            std::vector<XrEnvironmentBlendMode> list(modes);
            xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, modes,
                                             &modes, list.data());
            std::string text;
            for (XrEnvironmentBlendMode m : list)
                text += m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE         ? " opaque"
                        : m == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE    ? " additive"
                        : m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? " alpha-blend"
                                                                     : " other";
            LOG("OpenXR environment blend modes:%s", text.c_str());
            logged = true;
        }
    }

    // Must be called before xrCreateSession.
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!check(xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &requirements), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        if (std::memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) != 0)
            LOG("WARNING: game renders on a different GPU than the headset runtime expects");
    }
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    dropOldEvents();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    if (!check(xrCreateSession(g_instance, &sessionInfo, &g_session), "xrCreateSession")) return false;
    setStatusNote("");
    createHandTrackers();

    XrViewConfigurationView configViews[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t viewCount = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       2, &viewCount, configViews)) &&
        viewCount == 2) {
        for (int eye = 0; eye < 2; ++eye) {
            g_recommended[eye][0] = configViews[eye].recommendedImageRectWidth;
            g_recommended[eye][1] = configViews[eye].recommendedImageRectHeight;
        }
        LOG("Headset asks for %ux%u per eye", g_recommended[0][0], g_recommended[0][1]);
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(xrCreateReferenceSpace(g_session, &spaceInfo, &g_localSpace), "xrCreateReferenceSpace(LOCAL)")) {
        destroySession();
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(xrCreateReferenceSpace(g_session, &spaceInfo, &g_viewSpace), "xrCreateReferenceSpace(VIEW)")) {
        destroySession();
        return false;
    }
    attachActions();
    LOG("OpenXR session created");
    return true;
}

static void pollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The user recentred in the headset (e.g. holding the Oculus button).
            LOG("Runtime recenter detected");
            g_recenterRequested = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            g_sessionState = changed->state;
            LOG("Session state: %s", stateName(changed->state));
            if (g_sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_sessionRunning = check(xrBeginSession(g_session, &begin), "xrBeginSession");
            } else if (g_sessionState == XR_SESSION_STATE_STOPPING) {
                // The runtime follows with IDLE and then EXITING if we asked
                // it to end (Virtual Desktop queues both at once); the session
                // is destroyed at EXITING, below, so neither is left behind.
                check(xrEndSession(g_session), "xrEndSession");
                g_sessionRunning = false;
            } else if (g_sessionState == XR_SESSION_STATE_EXITING && g_exitRequested) {
                ULONGLONG delay = g_restartDelay;
                destroySession();
                g_nextSessionAttempt = GetTickCount64() + delay;
                if (g_linkLostAt) LOG("Session ended cleanly");
                else LOG("Session ended cleanly; a new one in %llu s", static_cast<unsigned long long>(delay / 1000));
                return;
            } else if (g_sessionState == XR_SESSION_STATE_EXITING || g_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                // The runtime ended it (ours end at EXITING above): the headset went away.
                linkLost(g_sessionState == XR_SESSION_STATE_EXITING ? "the runtime ended the session"
                                                                    : "the runtime lost the headset");
                destroySession();
                return;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Re-origin the app space at the current head position, facing the head's yaw
// (pitch and roll ignored so the horizon stays level).
static void recenter(XrTime time) {
    // Before the session is focused, runtimes report a placeholder head pose
    // (Virtual Desktop: the origin, i.e. near the floor). Wait for real,
    // actively tracked poses for a short while before trusting one.
    static int settledFrames = 0;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    bool tracked = g_sessionState == XR_SESSION_STATE_FOCUSED &&
                   XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_localSpace, time, &head)) &&
                   (head.locationFlags & needed) == needed;
    settledFrames = tracked ? settledFrames + 1 : 0;
    if (settledFrames < 30) {
        g_recenterRequested = true;  // try again next frame
        return;
    }
    settledFrames = 0;
    const XrQuaternionf& q = head.pose.orientation;
    // Forward = q * (0, 0, -1)
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -1.0f + 2.0f * (q.x * q.x + q.y * q.y);
    float yaw = std::atan2(-fx, -fz);

    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation = {0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2)};
    info.poseInReferenceSpace.position = head.pose.position;
    XrSpace space = XR_NULL_HANDLE;
    if (!check(xrCreateReferenceSpace(g_session, &info, &space), "xrCreateReferenceSpace(recenter)")) return;
    if (g_appSpace) xrDestroySpace(g_appSpace);
    g_appSpace = space;
    resetStereo();
    LOG("Recentered at head %.2f %.2f %.2f (yaw %.1f deg)", head.pose.position.x, head.pose.position.y,
        head.pose.position.z, yaw * 57.2958f);
}

