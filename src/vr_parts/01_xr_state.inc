// ---------------------------------------------------------------------------
// OpenXR entry points, loaded at runtime from openxr_loader.dll so the DLL has
// no link-time dependency and the loader can sit next to it in the Mods folder.

#define S4VR_XR_FUNCTIONS(X)                \
    X(xrDestroyInstance)                    \
    X(xrGetInstanceProperties)              \
    X(xrGetSystem)                          \
    X(xrGetD3D11GraphicsRequirementsKHR)    \
    X(xrCreateSession)                      \
    X(xrDestroySession)                     \
    X(xrBeginSession)                       \
    X(xrEndSession)                         \
    X(xrRequestExitSession)                 \
    X(xrPollEvent)                          \
    X(xrCreateReferenceSpace)               \
    X(xrDestroySpace)                       \
    X(xrLocateSpace)                        \
    X(xrLocateViews)                        \
    X(xrEnumerateViewConfigurationViews)    \
    X(xrEnumerateSwapchainFormats)          \
    X(xrCreateSwapchain)                    \
    X(xrDestroySwapchain)                   \
    X(xrEnumerateSwapchainImages)           \
    X(xrAcquireSwapchainImage)              \
    X(xrWaitSwapchainImage)                 \
    X(xrReleaseSwapchainImage)              \
    X(xrWaitFrame)                          \
    X(xrBeginFrame)                         \
    X(xrEndFrame)                           \
    X(xrResultToString)                     \
    X(xrStringToPath)                       \
    X(xrCreateActionSet)                    \
    X(xrDestroyActionSet)                   \
    X(xrCreateAction)                       \
    X(xrSuggestInteractionProfileBindings)  \
    X(xrAttachSessionActionSets)            \
    X(xrCreateActionSpace)                  \
    X(xrSyncActions)                        \
    X(xrGetActionStateBoolean)              \
    X(xrGetActionStateFloat)                \
    X(xrGetActionStateVector2f)             \
    X(xrGetActionStatePose)                 \
    X(xrApplyHapticFeedback)                \
    X(xrEnumerateEnvironmentBlendModes)

static PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
static PFN_xrCreateInstance xrCreateInstance = nullptr;
static PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
#define S4VR_DECLARE(name) static PFN_##name name = nullptr;
S4VR_XR_FUNCTIONS(S4VR_DECLARE)
#undef S4VR_DECLARE

// ---------------------------------------------------------------------------
// State. Everything below is touched only on the render thread, except the
// atomics and the status fields guarded by g_statusMutex.

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;  // backbuffer format it was made for
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int waiting = -1;  // an acquired image whose wait ran out: waited for again next frame
};

static Config g_config;
static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static std::atomic<XrSessionState> g_sessionState{XR_SESSION_STATE_UNKNOWN};
static bool g_sessionRunning = false;
static XrSpace g_localSpace = XR_NULL_HANDLE;
static XrSpace g_viewSpace = XR_NULL_HANDLE;
static XrSpace g_appSpace = XR_NULL_HANDLE;  // LOCAL, re-originated at the recentered head pose
static ULONGLONG g_nextSessionAttempt = 0;
// A session restart under way: exit requested from the runtime, the session
// is destroyed once it has stopped (or at the deadline), then a new one is
// made after the delay.
static bool g_exitRequested = false;
static ULONGLONG g_exitDeadline = 0, g_restartDelay = 0;

static Swapchain g_quadSwapchain;
static Swapchain g_eyeSwapchains[2];
static Swapchain g_uiSwapchain;

// UI panel: the game's UI, split off the 3D view, on a quad in front of you.
static ID3D11Texture2D* g_uiFrame = nullptr;  // this frame's UI, owned by uihook
static constexpr int kProbeMax = 32;           // pixels around the laser's point the probe also reads
static ID3D11Texture2D* g_uiProbe[3] = {};   // staging copies of the pixels under the laser
static int g_uiProbeFrame = 0;
static bool g_uiProbePending[3] = {};
static UINT g_uiProbeBox[3][4] = {};          // each copy's width, height and laser pixel in it
static float g_uiAlphaUnderLaser = 0;
static float g_uiAlphaNearLaser = 0;          // the most opaque UI around that point (a near miss)

// Alternate-eye rendering: the game renders one eye per frame with the pose we
// wrote into its camera on the previous Present.
static int g_renderingEye = -1;
static XrPosef g_eyePose[2]{};
static XrFovf g_eyeFov[2]{};
static bool g_fovLogged = false;  // the eyes' FOV went to the log (once per game run)
static bool g_eyeValid[2] = {false, false};
static float g_eyeYaw[2] = {NAN, NAN};  // app space's heading in the world when each eye was rendered
// What each eye's swapchain holds now: the image as rendered (the above), or
// under SteamVR the older eye turned to the newest head pose (eyealign.h).
static XrPosef g_shownPose[2]{};
static float g_shownYaw[2] = {NAN, NAN};
// The headset's frame period (xrWaitFrame), for the stick turn's steps.
static XrDuration g_displayPeriod = 0;
// Stick turn check: how evenly each frame turned while the right stick turns
// (logged every 2 s of turning).
struct TurnCheck {
    ULONGLONG start = 0;
    int frames = 0, missed = 0, pictures = 0;
    double stepMin = 1e9, stepMax = 0, stepSum = 0;  // display clock steps, ms
    int steps = 0;
    double turnMin = 1e9, turnMax = -1e9, turnSum = 0;  // view heading turned per frame, degrees
    int turns = 0;
    float leadMax = 0;  // drawn ahead of the turn, degrees
    float backMin = 1e9f, backMax = -1e9f;  // eyes turned back to the newest heading, degrees
    float lastHeading = NAN;
};
static TurnCheck g_turnCheck;
static bool g_turnChecking = false;  // the right stick turns this frame
// Turn lead (camera::setTurnLead): the stick's turn rate now (radians of yaw a
// second, 0 when still), how long a pair's heading is set before the headset
// shows it (learned while turning), and the lead the shown right eye was drawn with.
static float g_stickRate = 0;
static float g_turnLatency = 0.05f;
static float g_shownLead1 = 0;
static int g_turnCheckLogs = 0;
static int g_keepEye = -1;  // the eye copyIntoSwapchain is drawing, when its image is to be kept
// Paired eyes (first person, camera.h): the right eye is drawn from where its
// left eye was. A shown left eye from the pair before would be one pair
// behind, so the new left eye waits in the other of two left swapchains until
// its right eye arrives, and then both are shown together.
static Swapchain g_leftSpare;
static int g_leftShown = 0;      // the left swapchain shown: 0 g_eyeSwapchains[0], 1 g_leftSpare
static bool g_leftHeld = false;  // the other one holds a left eye waiting for its right eye
static XrPosef g_heldPose{};
static XrFovf g_heldFov{};
static float g_heldYaw = NAN;
static uint64_t g_pairsShown = 0;
static Swapchain& shownChain(int eye) {
    return eye == 0 && g_leftShown ? g_leftSpare : g_eyeSwapchains[eye];
}
static Swapchain& heldChain() { return g_leftShown ? g_eyeSwapchains[0] : g_leftSpare; }
// Body layer (bodylayer.h): each eye swapchain has a body swapchain beside it
// (slot 0 the left, 1 the right, 2 the spare left), written in the same copy
// as its eye picture: your body split off it, or all transparent.
enum BodyState { kBodyUnknown, kBodyEmpty, kBodyDrawn };
static Swapchain g_bodyChains[3];
static BodyState g_bodyState[3] = {};
static int chainSlot(const Swapchain& s) {
    return &s == &g_eyeSwapchains[0] ? 0 : &s == &g_eyeSwapchains[1] ? 1 : &s == &g_leftSpare ? 2 : -1;
}
static int g_splitSlot = -1;                         // the eye swapchain copyIntoSwapchain is drawing, to split
static const camera::Rendered* g_splitFrom = nullptr;  // what that eye picture was drawn with
static XrCompositionLayerProjection g_bodyProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
static XrCompositionLayerProjectionView g_bodyViews[2]{};
static bool g_bodyLayerReady = false;  // renderStereo made g_bodyProjection for this frame
// Only around turns (user 2026-10-14 22:04Z: the body layer costs
// performance; 22:06Z: think outside the box; v0.13.111): the split and the
// second layer are only needed while the turn smoothing turns the eye
// pictures. Eye pictures are split while your right thumb is on the stick
// (the controller senses it, so pictures are already split when a turn
// starts) or the view's heading changed within kBodyLinger; otherwise the
// body swapchains go transparent once and the layer is left out.
constexpr ULONGLONG kBodyLinger = 300;  // ms after the heading last changed or the thumb left the stick
static ULONGLONG g_lastTurnTick = 0;
static float g_lastTurnYaw = NAN;
static bool g_splitWanted = false;  // split the eye picture being copied (else its body image goes transparent)
static bool g_steamVR = false;
// The desktop window would alternate between the eyes every frame, which
// flickers for anyone watching. Keep the last left-eye frame and show it on
// right-eye frames instead: one GPU copy per frame.
static ID3D11Texture2D* g_mirror = nullptr;
static bool g_mirrorValid = false;
// The caller keeps the monitor steady itself (DX9: the D3D11 image is a copy).
static bool g_ownMirror = false;
static int g_frameEye = -1;  // eye of the image this frame sent to the headset

// Headset render size (renderscale): the runtime's recommended eye size, and
// the game size computed from it for this session.
static uint32_t g_recommended[2][2] = {};
static int g_renderSize[2] = {};

// Controllers. Actions belong to the instance; spaces to the session.
static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction g_aimAction = XR_NULL_HANDLE, g_triggerAction = XR_NULL_HANDLE, g_gripAction = XR_NULL_HANDLE,
                g_stickAction = XR_NULL_HANDLE, g_primaryAction = XR_NULL_HANDLE, g_secondaryAction = XR_NULL_HANDLE,
                g_menuAction = XR_NULL_HANDLE, g_handAction = XR_NULL_HANDLE, g_indexTouchAction = XR_NULL_HANDLE,
                g_thumbTouchAction = XR_NULL_HANDLE, g_hapticAction = XR_NULL_HANDLE, g_stickClickAction = XR_NULL_HANDLE,
                g_stickTouchAction = XR_NULL_HANDLE;
static XrPath g_handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
static XrSpace g_aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrSpace g_gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};  // where the hand holds it (arm IK)
static bool g_actionsAttached = false;

// Hand tracking (XR_EXT_hand_tracking, [body] hand_tracking): your real hands,
// with the controllers put down, in first person. Looks only: they move the
// Sim's arms and fingers (armik) and nothing else. A tracked hand's buttons
// read as released and its controller as lost, so it presses, points, pokes,
// touches and picks up nothing.
static bool g_handTrackingExt = false;  // the extension is on and its functions loaded
static bool g_handSourceExt = false;    // XR_EXT_hand_tracking_data_source: real hands only
static PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT = nullptr;
static PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT = nullptr;
static PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT = nullptr;
static XrHandTrackerEXT g_handTrackers[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static bool g_handTracked[2] = {false, false};  // this frame: that hand is your real hand, not a controller
static void createHandTrackers();

// The laser, in app space; shown as two quad layers (beam and end point).
static bool g_laserValid = false;
static XrVector3f g_laserFrom{}, g_laserTo{};
static int g_pointerHand = 1;  // the hand that showed the laser last; right by default

static std::atomic<int> g_mode{static_cast<int>(Mode::Quad)};
static std::atomic<bool> g_recenterRequested{true};

static std::mutex g_statusMutex;
static std::mutex g_frameMutex;
static std::string g_runtimeName = "none";
static std::string g_lastError;
static std::string g_statusNote;

static void setStatusNote(const char* note) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusNote = note;
}

static void setError(const std::string& error) {
    LOG("ERROR: %s", error.c_str());
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_lastError = error;
}

static bool check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return true;
    char text[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_instance && xrResultToString) xrResultToString(g_instance, result, text);
    setError(std::string(what) + " failed: " + text + " (" + std::to_string(result) + ")");
    return false;
}

// The runtime call the Present thread is in, for the watchdog: a runtime that
// lost its headset can block a call for good, and the game freezes with it.
// Logged, so the log says where it hung.
static std::atomic<const char*> g_xrCall{nullptr};
static std::atomic<ULONGLONG> g_xrCallSince{0};

struct XrCallTimer {
    explicit XrCallTimer(const char* name) {
        g_xrCallSince.store(GetTickCount64(), std::memory_order_relaxed);
        g_xrCall.store(name, std::memory_order_release);
    }
    ~XrCallTimer() { g_xrCall.store(nullptr, std::memory_order_release); }
};

static void startXrWatchdog() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        const char* reported = nullptr;
        ULONGLONG reportedSince = 0;
        for (;;) {
            Sleep(1000);
            const char* call = g_xrCall.load(std::memory_order_acquire);
            ULONGLONG since = g_xrCallSince.load(std::memory_order_relaxed), now = GetTickCount64();
            if (call && now - since > 3000 && !(call == reported && since == reportedSince)) {
                LOG("Headset link: stuck in %s for %.0f s (the headset's runtime isn't answering; the game waits on it)",
                    call, (now - since) / 1000.0);
                reported = call;
                reportedSince = since;
            } else if (reported && !(call == reported && since == reportedSince)) {
                LOG("Headset link: %s returned", reported);
                reported = nullptr;
            }
        }
    }).detach();
}

// What went to the headset in the last frame, and how loaded the graphics
// card is: logged when the link turns slow, so a freeze can be matched to
// what the mod was sending and whether the card was out of time or memory.
struct SentLayer {
    XrStructureType type;
    XrExtent2Di size;
};
static SentLayer g_sentLayer[10];
static uint32_t g_sentLayers = 0;

static void logLoad() {
    std::string sent;
    for (uint32_t i = 0; i < g_sentLayers; ++i) {
        const SentLayer& l = g_sentLayer[i];
        const char* kind = l.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION ? "both eyes"
                           : l.type == XR_TYPE_COMPOSITION_LAYER_QUAD     ? "panel"
                                                                          : "other";
        char item[64];
        std::snprintf(item, sizeof(item), "%s%s %dx%d", i ? ", " : "", kind, l.size.width, l.size.height);
        sent += item;
    }
    LOG("  It was sent %u layers (%s); phone drawn in 3D %s, settings menu %s; graphics card done %.0f ms "
        "after the last frame, busy: %s; video memory %s",
        g_sentLayers, sent.empty() ? "none" : sent.c_str(), phone3d::drawnRecently() ? "yes" : "no",
        settings::isOpen() ? "open" : "closed", gpuload::latestMs(), gpuload::busyNowText().c_str(),
        gpuload::memoryText().c_str());
}

// Link check: when the headset link stalls, which way did it die? Virtual
// Desktop's frame clock (what xrEndFrame waits on) falls back to 100 ms steps
// when the headset stops asking for frames. Head tracking and the headset's
// mic come the other way over the same link, so each second this counts how
// often the head data was new (its velocity changes with every real sample
// and stays bit-for-bit the same when the runtime is only extrapolating an
// old one) and how much mic sound arrived. Still flowing in a stall = the
// headset and Wi-Fi were alive and only the video side stuck; stopped = the
// headset or the network went first. Logged for the 5 s before a stall and
// each second during it.
struct LinkSecond {
    int frames = 0, fresh = 0, located = 0;
    float moveMm = 0, turnDeg = 0;
    uint64_t micFrames = 0, micLive = 0;
    bool micOpen = false;
    float worstFrameMs = 0;
};
static LinkSecond g_linkNow, g_linkRing[5];
static int g_linkRingCount = 0;
static ULONGLONG g_linkSecondStart = 0;
static voice::Flow g_linkMicAt;

static std::string linkText(const LinkSecond& s) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "%d frames (worst %.0f ms), head data new in %d of %d, moved %.0f mm / %.1f deg; mic %s",
                  s.frames, s.worstFrameMs, s.fresh, s.located, s.moveMm, s.turnDeg,
                  !s.micOpen ? "not in use" : "");
    std::string out = text;
    if (s.micOpen) {
        std::snprintf(text, sizeof(text), "%llu samples in, %llu of them 10 ms blocks with sound",
                      static_cast<unsigned long long>(s.micFrames), static_cast<unsigned long long>(s.micLive));
        out += text;
    }
    return out;
}

static void linkSample(XrTime time) {
    static XrPosef last{};
    static XrVector3f lastVel{}, lastAng{};
    static bool have = false;
    if (!g_session || !g_viewSpace || !g_localSpace) return;
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION, &velocity};
    if (!XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_localSpace, time, &head))) return;
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((head.locationFlags & valid) != valid) return;
    ++g_linkNow.located;
    if (have) {
        bool fresh = velocity.velocityFlags
                         ? std::memcmp(&velocity.linearVelocity, &lastVel, sizeof(lastVel)) != 0 ||
                               std::memcmp(&velocity.angularVelocity, &lastAng, sizeof(lastAng)) != 0
                         : std::memcmp(&head.pose, &last, sizeof(last)) != 0;  // no velocity: any change
        if (fresh) ++g_linkNow.fresh;
        const XrVector3f &a = last.position, &b = head.pose.position;
        g_linkNow.moveMm += 1000.0f * std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                                (a.z - b.z) * (a.z - b.z));
        const XrQuaternionf &p = last.orientation, &q = head.pose.orientation;
        float dot = std::fabs(p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w);
        g_linkNow.turnDeg += 2.0f * std::acos(std::min(1.0f, dot)) * 57.29578f;
    }
    last = head.pose;
    lastVel = velocity.linearVelocity;
    lastAng = velocity.angularVelocity;
    have = true;
}

// Once a frame, after the stall detector: closes a second when one has passed.
static void linkTick(ULONGLONG tick, double frameMs, bool stalled, bool onset) {
    ++g_linkNow.frames;
    if (frameMs > g_linkNow.worstFrameMs) g_linkNow.worstFrameMs = static_cast<float>(frameMs);
    if (onset) {
        LOG("  Link check, the seconds before (oldest first):");
        for (int i = 0; i < g_linkRingCount; ++i) LOG("    %s", linkText(g_linkRing[i]).c_str());
    }
    if (!g_linkSecondStart) {
        g_linkSecondStart = tick;
        g_linkMicAt = voice::flow();
    }
    if (tick - g_linkSecondStart < 1000) return;
    voice::Flow mic = voice::flow();
    g_linkNow.micOpen = mic.open && g_linkMicAt.open;
    g_linkNow.micFrames = mic.frames - g_linkMicAt.frames;
    g_linkNow.micLive = mic.live - g_linkMicAt.live;
    static int stallLines = 0;
    if (stalled) {
        if (stallLines++ < 15) LOG("  Link check during the stall: %s", linkText(g_linkNow).c_str());
    } else {
        stallLines = 0;
    }
    // No snapshots of Virtual Desktop's programs any more (2026-10-15): the
    // dump suspends VD's streamer, and twice it never finished (0-byte file),
    // leaving VD paused until it was restarted. The freezes turned out to be
    // the Intel USB link to the Quest.
    if (g_linkRingCount < 5) {
        g_linkRing[g_linkRingCount++] = g_linkNow;
    } else {
        for (int i = 0; i < 4; ++i) g_linkRing[i] = g_linkRing[i + 1];
        g_linkRing[4] = g_linkNow;
    }
    g_linkNow = {};
    g_linkSecondStart = tick;
    g_linkMicAt = mic;
}

// Over the video memory budget Windows moves things out of the card's memory,
// and everything that draws (the game, the headset's stream) stalls: said once
// each time it happens.
static void watchVideoMemory() {
    static ULONGLONG next = 0;
    static bool over = false;
    ULONGLONG tick = GetTickCount64();
    if (tick < next) return;
    next = tick + 2000;
    gpuload::Memory m;
    if (!gpuload::memory(m) || !m.budgetMb) return;
    bool now = m.usedMb > m.budgetMb;
    if (now && !over) LOG("Video memory over budget: %s", gpuload::memoryText().c_str());
    else if (!now && over) LOG("Video memory back under budget: %s", gpuload::memoryText().c_str());
    over = now;
}

// The swapchain's next image, waited for at most 0.5 s: a runtime whose link to
// the headset died can hold an image forever. A wait that runs out is finished
// next frame (the runtime has the same image waited for again); that layer is
// left out meanwhile.
static bool acquireImage(Swapchain& s, uint32_t& index) {
    if (s.waiting < 0) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult acquired;
        {
            XrCallTimer timer("xrAcquireSwapchainImage");
            acquired = xrAcquireSwapchainImage(s.handle, &acquire, &index);
        }
        if (!check(acquired, "xrAcquireSwapchainImage")) return false;
        s.waiting = static_cast<int>(index);
    }
    index = static_cast<uint32_t>(s.waiting);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 500000000;  // nanoseconds
    XrResult waited;
    {
        XrCallTimer timer("xrWaitSwapchainImage");
        waited = xrWaitSwapchainImage(s.handle, &wait);
    }
    if (waited == XR_TIMEOUT_EXPIRED) {
        static int logged = 0;
        if (logged++ < 10) LOG("Headset link: a %ux%u image wasn't free after 0.5 s; left out this frame", s.width, s.height);
        return false;
    }
    s.waiting = -1;
    return check(waited, "xrWaitSwapchainImage");
}

static const char* stateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "no session";
    }
}

