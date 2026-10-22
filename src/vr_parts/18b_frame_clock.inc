// ---------------------------------------------------------------------------
// Headset frames on their own clock ([stereo] frame_clock, v0.13.307).
//
// The Present used to send the headset frame itself (xrWaitFrame, xrBeginFrame,
// xrEndFrame), so a game frame that took longer than a refresh left the headset
// showing the last frame again, at the old stick heading: the compositor can't
// see a stick turn, so turning shook whenever the game was under 72 (v0.13.306:
// 5-31 refreshes missed in 1-2 s of turning). Now a thread of its own sends a
// frame every refresh with the newest layers the Present made (a copy, below),
// the scene's eye poses turned on by the stick turn since they were drawn,
// about your head, the way renderStereo turns them back: the compositor then
// reprojects the turn like a head turn every refresh.
//
// g_clockMutex covers the copy and every runtime call but xrWaitFrame: the
// clock holds it from xrBeginFrame to xrEndFrame, the Present while it fills
// swapchains and makes the layers, so a swapchain never gets a new picture
// while a frame showing it with the old pose is being sent. The clock thread
// never takes g_frameMutex (the Present holds it and stops the clock by
// joining it).

struct ClockLayer {
    XrStructureType type = XR_TYPE_UNKNOWN;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
struct ClockSnap {
    ClockLayer layers[10];
    uint32_t count = 0;
    int turnLayer = -1;  // the scene's projection: turned on by the stick at each send
    float turnRate = 0;  // the stick's turn (radians of view yaw a second) when it was made
    XrVector3f head{};   // the head point the turn goes about
    XrTime time = 0;     // the display time it was made for
    uint64_t gen = 0;
};
static std::mutex g_clockMutex;
static std::condition_variable g_clockWake;
static std::thread g_clockThread;
static std::atomic<bool> g_clockStop{false};
static std::atomic<bool> g_clockLost{false};  // xrWaitFrame said the session was lost
static bool g_clockRunning = false;           // Present thread
// Under g_clockMutex:
static ClockSnap g_clockSnap;
static XrTime g_clockTime = 0;  // the newest frame's display time
static XrDuration g_clockPeriod = 0;
static bool g_clockShouldRender = false;
static uint64_t g_clockFrames = 0;  // xrWaitFrame returns
static double g_clockEndMs = 0;     // the slowest xrEndFrame since the Present last asked

static void turnPoseAbout(XrPosef& pose, const XrVector3f& head, float delta) {
    const float c = std::cos(delta / 2), s = std::sin(delta / 2);  // about +y: {0, s, 0, c}
    const XrQuaternionf o = pose.orientation;
    pose.orientation = {c * o.x + s * o.z, c * o.y + s * o.w, c * o.z - s * o.x, c * o.w - s * o.y};
    const float dx = pose.position.x - head.x, dz = pose.position.z - head.z;
    const float cd = std::cos(delta), sd = std::sin(delta);
    pose.position.x = head.x + cd * dx + sd * dz;
    pose.position.z = head.z - sd * dx + cd * dz;
}

static void clockLoop(XrSession session) {
    s4vr::cpuprio::boostClockThread();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    auto now = [] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t;
    };
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return 1000.0 * double(b.QuadPart - a.QuadPart) / double(frequency.QuadPart);
    };
    uint64_t sent = 0, pictures = 0, turnedSent = 0, missed = 0, lastGen = 0;
    double worstEnd = 0, turnedMax = 0;
    XrTime lastTime = 0;
    ULONGLONG since = GetTickCount64();
    while (!g_clockStop.load()) {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        XrResult waited;
        {
            XrCallTimer timer("xrWaitFrame (frame clock)");
            waited = xrWaitFrame(session, &waitInfo, &frame);
        }
        std::unique_lock<std::mutex> lock(g_clockMutex);
        if (waited == XR_ERROR_SESSION_LOST) {
            LOG("Frame clock: the session was lost");
            g_clockLost = true;
            g_clockWake.notify_all();
            break;
        }
        if (!check(waited, "xrWaitFrame (frame clock)")) {
            lock.unlock();
            Sleep(10);
            continue;
        }
        g_clockTime = frame.predictedDisplayTime;
        g_clockPeriod = frame.predictedDisplayPeriod;
        g_clockShouldRender = frame.shouldRender;
        ++g_clockFrames;
        g_clockWake.notify_all();
        if (lastTime && frame.predictedDisplayPeriod > 0) {
            const long long steps =
                std::llround(double(frame.predictedDisplayTime - lastTime) / double(frame.predictedDisplayPeriod));
            if (steps > 1 && steps < 100) missed += steps - 1;
        }
        lastTime = frame.predictedDisplayTime;
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!check(xrBeginFrame(session, &beginInfo), "xrBeginFrame (frame clock)")) continue;

        // The Present's newest layers; the scene's turned on by the stick since they were made.
        const ClockSnap& snap = g_clockSnap;
        const XrCompositionLayerBaseHeader* layers[10] = {};
        XrCompositionLayerProjection turned;
        XrCompositionLayerProjectionView turnedViews[2];
        const uint32_t count = frame.shouldRender ? snap.count : 0;
        for (uint32_t i = 0; i < count; ++i) {
            const ClockLayer& l = snap.layers[i];
            if (l.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l.quad);
                continue;
            }
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l.projection);
            if (int(i) != snap.turnLayer || snap.turnRate == 0 || frame.predictedDisplayPeriod <= 0) continue;
            // Whole refreshes since it was made (the predicted display times wobble), as the stick's
            // turn counts them (updateControls), at most 0.1 s as it does.
            const long long steps =
                std::llround(double(frame.predictedDisplayTime - snap.time) / double(frame.predictedDisplayPeriod));
            if (steps <= 0) continue;
            const float dt = std::min(0.1f, float(double(steps) * double(frame.predictedDisplayPeriod) * 1e-9));
            const float delta = -snap.turnRate * dt;
            turned = l.projection;
            for (uint32_t v = 0; v < l.projection.viewCount && v < 2; ++v) {
                turnedViews[v] = l.views[v];
                turnPoseAbout(turnedViews[v].pose, snap.head, delta);
            }
            turned.views = turnedViews;
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&turned);
            ++turnedSent;
            turnedMax = std::max(turnedMax, double(std::fabs(delta)) * 57.29578);
        }
        if (count && snap.gen != lastGen) ++pictures;
        lastGen = snap.gen;

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frame.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = count;
        endInfo.layers = count ? layers : nullptr;
        const LARGE_INTEGER beforeEnd = now();
        XrResult ended;
        {
            XrCallTimer timer("xrEndFrame (frame clock)");
            ended = xrEndFrame(session, &endInfo);
        }
        check(ended, "xrEndFrame (frame clock)");
        const double endMs = ms(beforeEnd, now());
        g_clockEndMs = std::max(g_clockEndMs, endMs);
        worstEnd = std::max(worstEnd, endMs);
        ++sent;
        const ULONGLONG tick = GetTickCount64();
        if (tick - since >= 10000) {
            LOG("Frame clock: %llu headset frames in %.1f s (%llu refreshes missed), %llu with a new picture, %llu "
                "turned on by the stick (up to %.2f degrees); xrEndFrame up to %.1f ms",
                static_cast<unsigned long long>(sent), (tick - since) / 1000.0, static_cast<unsigned long long>(missed),
                static_cast<unsigned long long>(pictures), static_cast<unsigned long long>(turnedSent), turnedMax,
                worstEnd);
            sent = pictures = turnedSent = missed = 0;
            worstEnd = turnedMax = 0;
            since = tick;
        }
    }
}

// Calls into the game's device context can now come from the runtime on the clock thread too
// (its xrEndFrame), so the context serializes them.
static void protectContext(ID3D11Device* device) {
    static bool done = false;
    if (done || !device) return;
    done = true;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ID3D11Multithread* multithread = nullptr;
    if (context && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        const BOOL was = multithread->SetMultithreadProtected(TRUE);
        LOG("Frame clock: the game's device context is thread-safe (%s)", was ? "it already was" : "turned on");
        multithread->Release();
    } else {
        LOG("Frame clock: the game's device context can't be made thread-safe");
    }
    if (context) context->Release();
}

static void stopFrameClock() {
    if (!g_clockRunning) return;
    g_clockStop = true;
    if (g_clockThread.joinable()) g_clockThread.join();
    g_clockRunning = false;
    std::lock_guard<std::mutex> lock(g_clockMutex);
    g_clockSnap.count = 0;
    g_clockTime = 0;
    LOG("Frame clock: stopped");
}

// Start or stop the clock with the setting (Present thread, frame lock held, session running).
static void followFrameClock(ID3D11Device* device) {
    const bool wanted = g_config.frameClock && !g_exitRequested;
    if (wanted == g_clockRunning) return;
    if (!wanted) {
        stopFrameClock();
        return;
    }
    protectContext(device);
    {
        std::lock_guard<std::mutex> lock(g_clockMutex);
        g_clockSnap.count = 0;
        g_clockTime = 0;
        g_clockEndMs = 0;
    }
    g_clockStop = false;
    g_clockLost = false;
    g_clockThread = std::thread(clockLoop, g_session);
    g_clockRunning = true;
    LOG("Frame clock: on (headset frames sent from their own thread every refresh, turned on by the stick)");
}

// The Present's frame with the clock running: waits for the clock's next refresh (at most 100 ms),
// then holds g_clockMutex in `lock` and gives the display time the layers made now are sent for.
static XrResult clockFrame(std::unique_lock<std::mutex>& lock, XrFrameState& frame) {
    static uint64_t seen = 0;
    lock = std::unique_lock<std::mutex>(g_clockMutex);
    g_clockWake.wait_for(lock, std::chrono::milliseconds(100),
                         [] { return g_clockFrames != seen || g_clockLost.load(); });
    if (g_clockLost.load()) {
        lock.unlock();
        return XR_ERROR_SESSION_LOST;
    }
    seen = g_clockFrames;
    frame.predictedDisplayPeriod = g_clockPeriod;
    frame.predictedDisplayTime = g_clockTime ? g_clockTime + g_clockPeriod : 0;
    frame.shouldRender = g_clockShouldRender && g_clockTime;
    return XR_SUCCESS;
}

// The layers the Present made, copied for the clock to send (g_clockMutex held). `scene`: the
// stereo scene's projection, turned on by the stick when sent later.
static void clockPublish(const XrCompositionLayerBaseHeader* const* layers, uint32_t count,
                         const XrCompositionLayerProjection* scene, XrTime time) {
    ClockSnap& s = g_clockSnap;
    s.count = 0;
    s.turnLayer = -1;
    for (uint32_t i = 0; i < count && s.count < 10; ++i) {
        ClockLayer& l = s.layers[s.count];
        l.type = layers[i]->type;
        if (l.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            l.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layers[i]);
            l.quad.next = nullptr;
        } else if (l.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(layers[i]);
            l.projection = *p;
            l.projection.next = nullptr;
            l.projection.viewCount = std::min<uint32_t>(p->viewCount, 2);
            for (uint32_t v = 0; v < l.projection.viewCount; ++v) {
                l.views[v] = p->views[v];
                l.views[v].next = nullptr;
            }
            l.projection.views = l.views;
            if (p == scene) s.turnLayer = int(s.count);
        } else {
            continue;
        }
        ++s.count;
    }
    s.turnRate = scene && g_config.turnReprojection ? g_stickRate : 0;
    s.head = g_shownHead;
    s.time = time;
    ++s.gen;
}

static double clockTakeEndMs() {
    const double worst = g_clockEndMs;
    g_clockEndMs = 0;
    return worst;
}
