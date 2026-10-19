#include "vr.h"

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "armik.h"
#include "camera.h"
#include "player.h"
#include "crashlog.h"
#include "log.h"
#include "gamepad.h"
#include "gpuload.h"
#include "nearplane.h"
#include "pointer.h"
#include "renderscale.h"
#include "screenshot.h"
#include "settings.h"
#include "sharpen.h"
#include "bodylayer.h"
#include "eyelids.h"
#include "hud.h"
#include "nodim.h"
#include "eyealign.h"
#include "grab.h"
#include "handshape.h"
#include "touch.h"
#include "bubbles.h"
#include "uihook.h"
#include "phone3d.h"
#include "simsforever.h"
#include "claudescreen.h"
#include "voice.h"

namespace s4vr::vr {

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

// ---------------------------------------------------------------------------
// Controller actions

static XrPath path(const char* text) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, text, &p);
    return p;
}

static XrAction makeAction(const char* name, const char* localized, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, localized);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g_handPaths;
    XrAction action = XR_NULL_HANDLE;
    check(xrCreateAction(g_actionSet, &info, &action), "xrCreateAction");
    return action;
}

// Without controllers VR still works (head only), so failures here only log.
static void createActions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gameplay");
    std::strcpy(setInfo.localizedActionSetName, "Gameplay");
    if (!check(xrCreateActionSet(g_instance, &setInfo, &g_actionSet), "xrCreateActionSet")) return;
    g_handPaths[0] = path("/user/hand/left");
    g_handPaths[1] = path("/user/hand/right");
    g_aimAction = makeAction("aim", "Laser pointer", XR_ACTION_TYPE_POSE_INPUT);
    g_triggerAction = makeAction("trigger", "Click", XR_ACTION_TYPE_FLOAT_INPUT);
    g_gripAction = makeAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
    g_stickAction = makeAction("stick", "Move camera", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_primaryAction = makeAction("primary", "Right click (A / X)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_secondaryAction = makeAction("secondary", "VR settings (B); close the pie menu (B / Y)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_menuAction = makeAction("menu", "Game menu (Escape)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_handAction = makeAction("hand", "Hand", XR_ACTION_TYPE_POSE_INPUT);
    g_indexTouchAction = makeAction("index_touch", "Index finger on the trigger", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_thumbTouchAction = makeAction("thumb_touch", "Thumb on a button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_stickTouchAction = makeAction("stick_touch", "Thumb on the stick", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_hapticAction = makeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    g_stickClickAction = makeAction("stick_click", "Switch view / Sim (hold a stick); screenshot (click the right stick)",
                                    XR_ACTION_TYPE_BOOLEAN_INPUT);

    std::vector<XrActionSuggestedBinding> touch;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        std::string h = hand;
        touch.push_back({g_aimAction, path((h + "/input/aim/pose").c_str())});
        touch.push_back({g_triggerAction, path((h + "/input/trigger/value").c_str())});
        touch.push_back({g_gripAction, path((h + "/input/squeeze/value").c_str())});
        touch.push_back({g_stickAction, path((h + "/input/thumbstick").c_str())});
        touch.push_back({g_hapticAction, path((h + "/output/haptic").c_str())});
        touch.push_back({g_stickClickAction, path((h + "/input/thumbstick/click").c_str())});
    }
    touch.push_back({g_primaryAction, path("/user/hand/right/input/a/click")});
    touch.push_back({g_primaryAction, path("/user/hand/left/input/x/click")});
    touch.push_back({g_secondaryAction, path("/user/hand/right/input/b/click")});
    touch.push_back({g_secondaryAction, path("/user/hand/left/input/y/click")});
    touch.push_back({g_menuAction, path("/user/hand/left/input/menu/click")});
    for (const char* hand : {"/user/hand/left", "/user/hand/right"})
        touch.push_back({g_handAction, path((std::string(hand) + "/input/grip/pose").c_str())});
    // Finger tracking from the capacitive sensors. A runtime that rejects any
    // of these rejects the whole list, so they are tried on top.
    std::vector<XrActionSuggestedBinding> fingers = touch;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        std::string h = hand;
        fingers.push_back({g_indexTouchAction, path((h + "/input/trigger/touch").c_str())});
        fingers.push_back({g_thumbTouchAction, path((h + "/input/thumbstick/touch").c_str())});
        fingers.push_back({g_thumbTouchAction, path((h + "/input/thumbrest/touch").c_str())});
        fingers.push_back({g_stickTouchAction, path((h + "/input/thumbstick/touch").c_str())});
    }
    for (const char* button : {"/user/hand/left/input/x/touch", "/user/hand/left/input/y/touch",
                               "/user/hand/right/input/a/touch", "/user/hand/right/input/b/touch"})
        fingers.push_back({g_thumbTouchAction, path(button)});
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(fingers.size());
    suggested.suggestedBindings = fingers.data();
    if (XR_FAILED(xrSuggestInteractionProfileBindings(g_instance, &suggested))) {
        LOG("Touch controller finger sensors not accepted; fingers follow the buttons only");
        suggested.countSuggestedBindings = static_cast<uint32_t>(touch.size());
        suggested.suggestedBindings = touch.data();
        check(xrSuggestInteractionProfileBindings(g_instance, &suggested), "suggest Touch bindings");
    }

    std::vector<XrActionSuggestedBinding> simple;
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        std::string h = hand;
        simple.push_back({g_aimAction, path((h + "/input/aim/pose").c_str())});
        simple.push_back({g_triggerAction, path((h + "/input/select/click").c_str())});
        simple.push_back({g_menuAction, path((h + "/input/menu/click").c_str())});
        simple.push_back({g_handAction, path((h + "/input/grip/pose").c_str())});
        simple.push_back({g_hapticAction, path((h + "/output/haptic").c_str())});
    }
    suggested.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(simple.size());
    suggested.suggestedBindings = simple.data();
    check(xrSuggestInteractionProfileBindings(g_instance, &suggested), "suggest simple controller bindings");
    LOG("Controller actions created");
}

static void attachActions() {
    if (!g_actionSet) return;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    g_actionsAttached = check(xrAttachSessionActionSets(g_session, &attach), "xrAttachSessionActionSets");
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.action = g_aimAction;
        info.subactionPath = g_handPaths[hand];
        info.poseInActionSpace.orientation.w = 1.0f;
        check(xrCreateActionSpace(g_session, &info, &g_aimSpaces[hand]), "xrCreateActionSpace");
        info.action = g_handAction;
        check(xrCreateActionSpace(g_session, &info, &g_gripSpaces[hand]), "xrCreateActionSpace (grip)");
    }
}

static float floatState(XrAction action, int hand) {
    if (g_handTracked[hand]) return 0.0f;
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_handPaths[hand];
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &info, &state)) && state.isActive ? state.currentState : 0.0f;
}

static bool boolState(XrAction action, int hand) {
    if (g_handTracked[hand]) return false;
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_handPaths[hand];
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &info, &state)) && state.isActive && state.currentState;
}

static XrVector2f stickState(int hand) {
    if (g_handTracked[hand]) return {0, 0};
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = g_stickAction;
    info.subactionPath = g_handPaths[hand];
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &info, &state)) && state.isActive) return state.currentState;
    return {0, 0};
}

// Forward of a pose: orientation * (0, 0, -1).
static XrVector3f forwardOf(const XrQuaternionf& q) {
    return {-2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.w * q.x - q.y * q.z), -1.0f + 2.0f * (q.x * q.x + q.y * q.y)};
}

// q * v
static XrVector3f rotate3(const XrQuaternionf& q, const XrVector3f& v) {
    XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

// ---------------------------------------------------------------------------
// Instance

bool init(const std::wstring& moduleDir, const Config& config) {
    g_config = config;
    startXrWatchdog();
    settings::init(moduleDir, &g_config);
    g_mode = static_cast<int>(config.mode);

    HMODULE loader = LoadLibraryW((moduleDir + L"\\openxr_loader.dll").c_str());
    if (!loader) loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) {
        setError("openxr_loader.dll not found next to sims4vr.dll");
        return false;
    }
    xrGetInstanceProcAddr =
        reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) {
        setError("openxr_loader.dll has no xrGetInstanceProcAddr");
        return false;
    }
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance));
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties));

    uint32_t count = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    bool hasD3D11 = false, hasHands = false, hasHandSource = false;
    std::string names;
    for (const auto& e : extensions) {
        hasD3D11 |= std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        hasHands |= std::strcmp(e.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0;
        hasHandSource |= std::strcmp(e.extensionName, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME) == 0;
        names += names.empty() ? "" : " ";
        names += e.extensionName;
    }
    // What the runtime offers (passthrough for a tabletop mode, among others).
    LOG("OpenXR extensions: %s", names.c_str());
    if (!hasD3D11) {
        setError("the active OpenXR runtime does not support D3D11");
        return false;
    }

    std::vector<const char*> enabled{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    const bool wantHands = config.armIk && config.handTracking && hasHands;
    if (wantHands) enabled.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (wantHands && hasHandSource) enabled.push_back(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(info.applicationInfo.applicationName, "Sims4VR");
    std::strcpy(info.applicationInfo.engineName, "Sims4VR");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.enabledExtensionNames = enabled.data();
    XrResult created = xrCreateInstance(&info, &g_instance);
    if (XR_FAILED(created) && enabled.size() > 1) {
        LOG("Hand tracking: the runtime refused its extensions (%d); carrying on without", static_cast<int>(created));
        enabled.resize(1);
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = enabled.data();
        created = xrCreateInstance(&info, &g_instance);
    }
    if (!check(created, "xrCreateInstance")) return false;

#define S4VR_LOAD(name) \
    xrGetInstanceProcAddr(g_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name));
    S4VR_XR_FUNCTIONS(S4VR_LOAD)
#undef S4VR_LOAD
    if (enabled.size() > 1) {
        xrGetInstanceProcAddr(g_instance, "xrCreateHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateHandTrackerEXT));
        xrGetInstanceProcAddr(g_instance, "xrDestroyHandTrackerEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyHandTrackerEXT));
        xrGetInstanceProcAddr(g_instance, "xrLocateHandJointsEXT",
                              reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateHandJointsEXT));
        g_handTrackingExt = xrCreateHandTrackerEXT && xrDestroyHandTrackerEXT && xrLocateHandJointsEXT;
        g_handSourceExt = g_handTrackingExt && enabled.size() > 2;
    }
    LOG("Hand tracking: %s", g_handTrackingExt ? (g_handSourceExt ? "on, real hands only" : "on, while a controller is lost")
                             : !config.armIk || !config.handTracking ? "off ([body] arm_ik / hand_tracking)"
                             : "the runtime has none");

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props))) {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_runtimeName = props.runtimeName;
    }
    g_steamVR = g_runtimeName.rfind("SteamVR", 0) == 0;
    LOG("OpenXR runtime: %s", g_runtimeName.c_str());
    createActions();
    return true;
}

// ---------------------------------------------------------------------------
// Session

static void destroySwapchain(Swapchain& s) {
    if (s.handle) sharpen::forgetTargets();
    if (s.handle) eyealign::forgetTargets();
    if (s.handle) bodylayer::forgetTargets();
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

// ---------------------------------------------------------------------------
// Swapchains and copies

static DXGI_FORMAT srgbVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

static DXGI_FORMAT unormVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// The game writes gamma-encoded colour into a UNORM backbuffer, so the sRGB
// variant tells the runtime how to read those same bytes. Copies between the
// two are legal because they share a typeless family.
static DXGI_FORMAT chooseFormat(DXGI_FORMAT backbuffer) {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());
    for (DXGI_FORMAT wanted : {srgbVariant(backbuffer), unormVariant(backbuffer), backbuffer}) {
        for (int64_t f : formats)
            if (f == wanted) return wanted;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// Returns false if no usable swapchain; sets `recreated` when images were replaced.
static bool ensureSwapchain(Swapchain& s, const D3D11_TEXTURE2D_DESC& source, bool& recreated) {
    recreated = false;
    if (s.handle && s.width == source.Width && s.height == source.Height && s.sourceFormat == source.Format)
        return true;
    destroySwapchain(s);
    recreated = true;

    DXGI_FORMAT format = chooseFormat(source.Format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        setError("runtime has no swapchain format compatible with backbuffer format " + std::to_string(source.Format));
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = source.Width;
    info.height = source.Height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(xrCreateSwapchain(g_session, &info, &s.handle), "xrCreateSwapchain")) return false;

    uint32_t count = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &count, nullptr);
    s.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data()));
    s.width = source.Width;
    s.height = source.Height;
    s.sourceFormat = source.Format;
    s.format = format;
    LOG("Swapchain %ux%u format %d (%u images)", s.width, s.height, static_cast<int>(format), count);
    return true;
}


// ---------------------------------------------------------------------------
// Game window, for turning the laser into mouse input.
static HWND g_gameWindow = nullptr;

// ---------------------------------------------------------------------------
// Laser. Drawn into each eye image as dots along the ray, projected with the
// pose that image was rendered from, so it sits at the right depth.

static bool g_overlayEye = false;  // the next copy is an eye image
static XrPosef g_overlayPose{};
static XrFovf g_overlayFov{};

// App-space point to pixel in an image rendered with this pose and FOV.
static bool projectNormalized(const XrVector3f& p, const XrPosef& pose, const XrFovf& fov, float& nx, float& ny) {
    // v = conj(q) * (p - position)
    const XrQuaternionf& q = pose.orientation;
    camera::Vec3 d{p.x - pose.position.x, p.y - pose.position.y, p.z - pose.position.z};
    camera::Vec3 u{-q.x, -q.y, -q.z};
    camera::Vec3 t{2 * (u.y * d.z - u.z * d.y), 2 * (u.z * d.x - u.x * d.z), 2 * (u.x * d.y - u.y * d.x)};
    camera::Vec3 v{d.x + q.w * t.x + (u.y * t.z - u.z * t.y), d.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                   d.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    if (v.z > -0.01f) return false;
    float x = v.x / -v.z, y = v.y / -v.z;
    float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), up = std::tan(fov.angleUp), dn = std::tan(fov.angleDown);
    nx = (x - l) / (r - l);
    ny = (up - y) / (up - dn);
    return nx >= 0 && ny >= 0 && nx < 1 && ny < 1;
}

// The frame the game picks in: a centred frustum as wide as the drawn one
// (touch.cpp: its pick camera gets the pixel's centred direction). The same
// as the picture with a centred eye view; with the lopsided one the mouse
// went where the laser shows in the picture and picked about 18 degrees off.
static XrFovf pickFov(const XrFovf& f) {
    const float halfX = (std::tan(f.angleRight) - std::tan(f.angleLeft)) / 2,
                halfY = (std::tan(f.angleUp) - std::tan(f.angleDown)) / 2;
    return {-std::atan(halfX), std::atan(halfX), std::atan(halfY), -std::atan(halfY)};
}

static int g_captureSlot = -1;  // what copyIntoSwapchain is drawing, for a screenshot (screenshot::Slot)

// The body swapchain beside the eye swapchain `eye` (g_splitSlot), whose new
// picture `image` was just drawn: your body split off it into its own image
// (bodylayer.h), or that image made transparent. An image already
// transparent stays shown as it is.
static void splitBody(const Swapchain& eye, ID3D11Texture2D* image) {
    const int slot = g_splitSlot;
    Swapchain& chain = g_bodyChains[slot];
    const bool want = g_splitWanted && g_splitFrom && g_splitFrom->body.count > 0;
    if (!want && g_bodyState[slot] == kBodyEmpty) return;
    D3D11_TEXTURE2D_DESC like{};
    like.Width = eye.width;
    like.Height = eye.height;
    like.Format = eye.sourceFormat;
    bool recreated = false;
    uint32_t index = 0;
    if (!ensureSwapchain(chain, like, recreated) || chain.format != eye.format || !acquireImage(chain, index)) {
        g_bodyState[slot] = kBodyUnknown;
        return;
    }
    ID3D11Texture2D* body = chain.images[index].texture;
    const bool drawn =
        want && bodylayer::split(image, body, eye.format, eye.width, eye.height, g_splitFrom->fov, g_splitFrom->body);
    const bool empty = !drawn && bodylayer::clear(body, chain.format);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released = check(xrReleaseSwapchainImage(chain.handle, &release), "xrReleaseSwapchainImage");
    g_bodyState[slot] = !released ? kBodyUnknown : drawn ? kBodyDrawn : empty ? kBodyEmpty : kBodyUnknown;
}

static bool copyIntoSwapchain(Swapchain& s, ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc) {
    uint32_t index = 0;
    if (!acquireImage(s, index)) return false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    device->GetImmediateContext(&context);
    ID3D11Texture2D* target = s.images[index].texture;
    eyelids::State lids = eyelids::advance();
    if (g_overlayEye && sharpen::draw(backbuffer, desc, target, s.format, g_config.sharpen, lids.closure, lids.dark)) {
        // sharpened (and eyelids drawn) into the eye image
    } else if (desc.SampleDesc.Count > 1)
        context->ResolveSubresource(target, 0, backbuffer, 0, s.format);
    else
        context->CopySubresourceRegion(target, 0, 0, 0, 0, backbuffer, 0, nullptr);
    if (g_keepEye >= 0) eyealign::keep(g_keepEye, target, s.format, s.width, s.height);
    if (g_captureSlot >= 0) screenshot::offer(context, target, g_captureSlot);
    if (g_splitSlot >= 0) splitBody(s, target);
    context->Release();
    device->Release();

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
}

// ---------------------------------------------------------------------------
// Laser beam: a thin quad along the ray that turns to face the head, plus a
// round marker where it lands. Both are compositor layers, so they are sharp
// and steady whatever the game renders.

static XrSwapchainSubImage subImage(const Swapchain& s);

struct LayerImage {
    Swapchain swapchain;
    ID3D11Texture2D* source = nullptr;  // our picture, copied into the swapchain when it changes
    // The picture is in the swapchain's last released image. A layer shows the
    // last released image, so an unchanged picture needs no acquire, copy or
    // release (each a call into the runtime, and a wait) every frame.
    bool uploaded = false;
};
static LayerImage g_beamImage, g_tipImage, g_phoneBodyImage, g_settingsImage, g_voidImage;

static void releaseLayerImage(LayerImage& image) {
    destroySwapchain(image.swapchain);
    if (image.source) image.source->Release();
    image.source = nullptr;
    image.uploaded = false;
}

static void releaseBeam() {
    releaseLayerImage(g_beamImage);
    releaseLayerImage(g_tipImage);
    releaseLayerImage(g_phoneBodyImage);
    releaseLayerImage(g_settingsImage);
    releaseLayerImage(g_voidImage);
}

// rgba(x, y) returns premultiplied {r, g, b, a} in 0..1.
template <typename Fn>
static bool ensureLayerImage(LayerImage& image, ID3D11Device* device, uint32_t width, uint32_t height, Fn rgba) {
    D3D11_TEXTURE2D_DESC want{};
    want.Width = width;
    want.Height = height;
    want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    want.SampleDesc.Count = 1;
    bool recreated;
    if (!ensureSwapchain(image.swapchain, want, recreated)) return false;
    if (image.source && !recreated) return true;
    if (image.source) image.source->Release();
    image.source = nullptr;
    image.uploaded = false;
    const DXGI_FORMAT f = image.swapchain.format;
    const bool rgbaOrder = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                           f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool srgb = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    auto encode = [srgb](float v) {
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        if (srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        return static_cast<uint32_t>(v * 255 + 0.5f);
    };
    std::vector<uint32_t> pixels(width * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            float c[4];
            rgba((x + 0.5f) / width, (y + 0.5f) / height, c);
            uint32_t r = encode(c[0]), g = encode(c[1]), b = encode(c[2]);
            uint32_t a = static_cast<uint32_t>((c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3]) * 255 + 0.5f);
            pixels[y * width + x] = rgbaOrder ? (a << 24 | b << 16 | g << 8 | r) : (a << 24 | r << 16 | g << 8 | b);
        }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = f;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4, 0};
    if (FAILED(device->CreateTexture2D(&desc, &init, &image.source))) {
        image.source = nullptr;
        return false;
    }
    return true;
}

static bool submitLayerImage(LayerImage& image, ID3D11DeviceContext* context) {
    if (image.uploaded) return true;
    Swapchain& s = image.swapchain;
    uint32_t index = 0;
    if (!acquireImage(s, index)) return false;
    context->CopyResource(s.images[index].texture, image.source);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    image.uploaded = check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
    return image.uploaded;
}

static XrVector3f sub3(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot3(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static XrVector3f cross3(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static XrVector3f normalize3(const XrVector3f& v) {
    float l = std::sqrt(dot3(v, v));
    return l > 1e-6f ? XrVector3f{v.x / l, v.y / l, v.z / l} : XrVector3f{0, 0, 1};
}

// Rotation whose x, y and z axes are the given orthonormal vectors.
static XrQuaternionf fromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z) {
    float trace = x.x + y.y + z.z;
    XrQuaternionf q;
    if (trace > 0) {
        float s = std::sqrt(trace + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    } else if (x.x > y.y && x.x > z.z) {
        float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return q;
}

static bool renderBeam(ID3D11Device* device, XrTime time, XrCompositionLayerQuad& beam, XrCompositionLayerQuad& tip) {
    if (!g_laserValid) return false;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) ||
        !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return false;
    // Beam: bright core with a soft blue glow, thinning and fading with distance.
    bool ok = ensureLayerImage(g_beamImage, device, 256, 32, [](float u, float v, float* c) {
        float across = std::fabs(v - 0.5f) * 2;  // 0 centre, 1 edge
        float width = 1.0f - 0.55f * u;          // taper
        float r = across / width;
        float core = r < 0.25f ? 1.0f : r < 0.45f ? 1.0f - (r - 0.25f) / 0.2f : 0.0f;
        float glow = r < 1 ? (1 - r) * (1 - r) : 0;
        float fade = (u < 0.04f ? u / 0.04f : 1.0f) * (1.0f - 0.6f * u);
        float a = (core * 0.9f + glow * 0.45f) * fade;
        a = a > 1 ? 1 : a;
        float white = core * fade;
        c[0] = (0.35f * a + 0.65f * white);
        c[1] = (0.7f * a + 0.3f * white);
        c[2] = a;
        for (int i = 0; i < 3; ++i) c[i] = c[i] > a ? a : c[i];  // keep premultiplied
        c[3] = a;
    });
    ok = ok && ensureLayerImage(g_tipImage, device, 64, 64, [](float u, float v, float* c) {
        float r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2;  // 0 centre, 1 edge
        float ring = r > 0.7f && r < 0.95f ? 1.0f : 0.0f;
        float dot = r < 0.35f ? 1.0f : r < 0.45f ? (0.45f - r) / 0.1f : 0.0f;
        float a = std::max(ring * 0.9f, dot);
        float glow = r < 1 ? (1 - r) * 0.25f : 0;
        a = std::max(a, glow);
        c[0] = a * (ring + dot > 0 ? 1.0f : 0.35f);
        c[1] = a * (ring + dot > 0 ? 1.0f : 0.7f);
        c[2] = a;
        c[3] = a;
    });
    if (!ok || !g_beamImage.source || !g_tipImage.source) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ok = submitLayerImage(g_beamImage, context) && submitLayerImage(g_tipImage, context);
    context->Release();
    if (!ok) return false;

    const XrVector3f eye = head.pose.position;
    XrVector3f along = sub3(g_laserTo, g_laserFrom);
    float length = std::sqrt(dot3(along, along));
    if (!(length >= 0.01f)) return false;
    along = normalize3(along);
    XrVector3f to = g_laserTo;
    if (length > 50) {  // pointing at the horizon: draw a long beam, not a kilometre-wide one
        length = 50;
        to = {g_laserFrom.x + along.x * length, g_laserFrom.y + along.y * length, g_laserFrom.z + along.z * length};
    }
    XrVector3f mid{(g_laserFrom.x + to.x) / 2, (g_laserFrom.y + to.y) / 2, (g_laserFrom.z + to.z) / 2};
    XrVector3f toEye = sub3(eye, mid);
    XrVector3f facing = normalize3(sub3(toEye, XrVector3f{along.x * dot3(toEye, along), along.y * dot3(toEye, along),
                                                          along.z * dot3(toEye, along)}));
    XrVector3f up = cross3(facing, along);

    beam = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    beam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    beam.space = g_appSpace;
    beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    beam.subImage = subImage(g_beamImage.swapchain);
    beam.pose = {fromAxes(along, up, facing), mid};
    beam.size = {length, 0.012f};

    // The marker keeps the same apparent size at any distance.
    XrVector3f tipToEye = sub3(eye, to);
    float distance = std::sqrt(dot3(tipToEye, tipToEye));
    XrVector3f z = normalize3(tipToEye);
    XrVector3f x = normalize3(cross3(XrVector3f{0, 1, 0}, z));
    if (std::fabs(z.y) > 0.99f) x = {1, 0, 0};
    XrVector3f y = cross3(z, x);
    tip = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    tip.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    tip.space = g_appSpace;
    tip.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    tip.subImage = subImage(g_tipImage.swapchain);
    tip.pose = {fromAxes(x, y, z), to};
    float tipSize = 0.025f * distance;
    tip.size = {tipSize, tipSize};
    return true;
}

// ---------------------------------------------------------------------------
// UI panel

static XrSwapchainSubImage subImage(const Swapchain& s);

static float uiPanelHeight(const D3D11_TEXTURE2D_DESC& desc) { return g_config.uiWidth * desc.Height / desc.Width; }

// A window the phone's Options app (the game's menu) opened shows in front of
// you on its own (user 2026-10-19 01:08Z: the game's exact window, not the whole
// UI and not on the phone): the panel shows only its box in the UI (fitMenu),
// larger than on the whole panel. Off: the whole UI (a window filling the screen).
static bool g_frontCropOn = false;
static int32_t g_frontCrop[4] = {};  // x, y, width, height in the UI
constexpr float kFrontCropScale = 1.8f;

// The panel's size in metres (the window alone while g_frontCropOn).
static void uiPanelSize(const D3D11_TEXTURE2D_DESC& desc, float& w, float& h) {
    w = g_config.uiWidth;
    h = uiPanelHeight(desc);
    if (g_frontCropOn && g_frontCrop[2] > 0 && g_frontCrop[3] > 0 && desc.Width > 0) {
        w = std::min(g_config.uiWidth, g_config.uiWidth * g_frontCrop[2] / desc.Width * kFrontCropScale);
        h = w * g_frontCrop[3] / g_frontCrop[2];
    }
}

// Laser against the panel: true with the hit point and 0..1 coordinates (of the whole UI).
static bool hitPanel(const XrVector3f& o, const XrVector3f& d, const D3D11_TEXTURE2D_DESC& desc, XrVector3f& hit,
                     float& u, float& v) {
    if (d.z > -0.01f) return false;
    float t = (-g_config.uiDistance - o.z) / d.z;
    if (t <= 0) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, -g_config.uiDistance};
    float w, h;
    uiPanelSize(desc, w, h);
    u = (hit.x + w / 2) / w;
    v = (g_config.uiHeight + h / 2 - hit.y) / h;
    const bool in = u >= 0 && u <= 1 && v >= 0 && v <= 1;
    if (g_frontCropOn && desc.Width > 0 && desc.Height > 0) {
        u = (g_frontCrop[0] + u * g_frontCrop[2]) / static_cast<float>(desc.Width);
        v = (g_frontCrop[1] + v * g_frontCrop[3]) / static_cast<float>(desc.Height);
    }
    return in;
}

// Reads back the UI alpha under the laser, a couple of frames late, so the
// laser can pass through transparent parts of the panel to the world.
// The UI's alpha under the laser (two frames late, so the GPU never waits),
// and the most opaque pixel within `nearPx` pixels of it.
// A UI texture whose alpha the CPU can read: 8 bits a channel, alpha in byte 3, no MSAA.
static bool alphaReadable(const D3D11_TEXTURE2D_DESC& desc) {
    bool eightBit = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    return eightBit && desc.SampleDesc.Count == 1;
}

static void probeUiAlpha(ID3D11Texture2D* ui, float u, float v, int nearPx = 0) {
    if (u < 0 || v < 0) {  // between the tablet's pieces: nothing
        g_uiAlphaUnderLaser = g_uiAlphaNearLaser = 0;
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    ui->GetDesc(&desc);
    if (!alphaReadable(desc)) {
        g_uiAlphaUnderLaser = g_uiAlphaNearLaser = 1;  // can't tell; treat the whole panel as UI
        return;
    }
    ID3D11Device* device = nullptr;
    ui->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    int slot = g_uiProbeFrame++ % 3;
    if (!g_uiProbe[slot]) {
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = staging.Height = 2 * kProbeMax + 1;
        staging.MipLevels = staging.ArraySize = 1;
        staging.Format = desc.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&staging, nullptr, &g_uiProbe[slot]);
    }
    // Read the oldest copy (two frames back), then reuse its slot.
    if (g_uiProbe[slot] && g_uiProbePending[slot]) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(g_uiProbe[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            const UINT* box = g_uiProbeBox[slot];
            const uint8_t* pixels = static_cast<const uint8_t*>(mapped.pData);
            uint8_t most = 0;
            for (UINT y = 0; y < box[1]; ++y)
                for (UINT x = 0; x < box[0]; ++x) most = std::max(most, pixels[y * mapped.RowPitch + x * 4 + 3]);
            g_uiAlphaUnderLaser = pixels[box[3] * mapped.RowPitch + box[2] * 4 + 3] / 255.0f;
            g_uiAlphaNearLaser = most / 255.0f;
            context->Unmap(g_uiProbe[slot], 0);
        }
        g_uiProbePending[slot] = false;
    }
    if (g_uiProbe[slot]) {
        UINT x = static_cast<UINT>(u * (desc.Width - 1)), y = static_cast<UINT>(v * (desc.Height - 1));
        UINT r = static_cast<UINT>(std::max(0, std::min(kProbeMax, nearPx)));
        UINT x0 = x > r ? x - r : 0, y0 = y > r ? y - r : 0;
        D3D11_BOX box{x0, y0, 0, std::min(desc.Width, x + r + 1), std::min(desc.Height, y + r + 1), 1};
        context->CopySubresourceRegion(g_uiProbe[slot], 0, 0, 0, 0, ui, 0, &box);
        UINT* kept = g_uiProbeBox[slot];
        kept[0] = box.right - box.left;
        kept[1] = box.bottom - box.top;
        kept[2] = x - x0;
        kept[3] = y - y0;
        g_uiProbePending[slot] = true;
    }
    context->Release();
    device->Release();
}

static bool renderUiPanel(XrCompositionLayerQuad& layer) {
    if (!g_uiFrame) return false;
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    bool recreated;
    if (!ensureSwapchain(g_uiSwapchain, desc, recreated)) return false;
    g_captureSlot = screenshot::kMenu;
    bool copied = copyIntoSwapchain(g_uiSwapchain, g_uiFrame, desc);
    g_captureSlot = -1;
    if (!copied) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied, as Scaleform blends
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_uiSwapchain);
    if (g_frontCropOn && g_frontCrop[2] > 0 && g_frontCrop[3] > 0)
        layer.subImage.imageRect = {{g_frontCrop[0], g_frontCrop[1]}, {g_frontCrop[2], g_frontCrop[3]}};
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, g_config.uiHeight, -g_config.uiDistance};
    uiPanelSize(desc, layer.size.width, layer.size.height);
    return true;
}

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
};
static PieMenu g_pie;
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

// Opens the pie quad for the last world click, seen from your head.
static bool openPie(const XrVector3f& head) {
    const WorldClick& click = g_lastWorldClick;
    if (!g_uiFrame || !click.valid) return false;
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t cw = std::min(W, static_cast<int32_t>(H * g_config.pieCrop));
    int32_t ch = std::min(H, cw * 2 / 3);
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
    static int logged = 0;
    if (logged++ < 20)
        LOG("Pie menu at hand: opened %.2f m away (click %.0f m away), UI %dx%d at %d,%d of %dx%d", d, distance, cw,
            ch, x0, y0, W, H);
    return true;
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
    if (!g_uiFrame || !camera::bodyActive() || g_pie.cw <= 0 || g_pie.ch <= 0) return;
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    if (ui.SampleDesc.Count != 1) return;
    const UINT x0 = static_cast<UINT>(std::max(0, g_pie.x0)), y0 = static_cast<UINT>(std::max(0, g_pie.y0));
    const UINT w = std::min(static_cast<UINT>(g_pie.cw), ui.Width - std::min(x0, ui.Width));
    const UINT h = std::min(static_cast<UINT>(g_pie.ch), ui.Height - std::min(y0, ui.Height));
    if (!w || !h) return;
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
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
        context->CopySubresourceRegion(g_flingStill, 0, 0, 0, 0, g_uiFrame, 0, &box);
        context->Release();
        g_fling.active = true;
        g_fling.start = GetTickCount64();
        g_fling.centre = g_pie.pose.position;
        g_fling.x = g_pie.x;
        g_fling.y = g_pie.y;
        g_fling.z = g_pie.z;
        g_fling.width = g_pie.width * w / static_cast<float>(g_pie.cw);
        g_fling.height = g_pie.height * h / static_cast<float>(g_pie.ch);
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
    if ((!show && !flying) || !camera::bodyActive() || !camera::lastView(view) ||
        (show && (!g_uiFrame || !g_pie.uiW || !g_pie.uiH))) {
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
    const float W = static_cast<float>(g_pie.uiW), H = static_cast<float>(g_pie.uiH);
    panel.crop[0] = g_pie.x0 / W;
    panel.crop[1] = g_pie.y0 / H;
    panel.crop[2] = (g_pie.x0 + g_pie.cw) / W;
    panel.crop[3] = (g_pie.y0 + g_pie.ch) / H;
    phone3d::setPie(panel, g_uiFrame);
    static int logged = 0;
    bool drawn = phone3d::pieDrawnRecently();
    if (drawn && logged++ < 5) LOG("Pie: drawn in the world (your hands in front of it)");
    return drawn;
}

// ---------------------------------------------------------------------------
// The menus on your left wrist (first person, [ui] first_person = 1): turn the
// inside of your left wrist toward your face and the whole UI shows as a small
// screen over that hand, facing you; point at it with the right hand's laser
// or touch it with your right index finger. Lower the hand and it goes away.
static PieMenu g_wrist;

// The full panel in front of you: always outside first person, and in it with [ui] first_person = 2.
// A window the phone's Options app opened (the game options, Save, Exit...):
// too big for the phone, so the game's UI shows in front of you meanwhile and
// the phone keeps its apps (user 2026-10-18 21:55Z).
static bool g_optionsFront = false;
static bool frontPanelShown() { return !camera::bodyActive() || g_config.uiFirstPerson >= 2 || g_optionsFront; }

static void hideWrist() {
    g_wrist.active = false;
    g_wrist.armed[0] = g_wrist.armed[1] = false;
    g_wrist.pressing = -1;
}

static void updateWrist(const armik::Hand& left, const XrPosef& head, bool allowed) {
    if (!allowed || !left.valid || !g_uiFrame) {
        hideWrist();
        return;
    }
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    const camera::Quat& q = left.orientation;
    XrQuaternionf grip{q.x, q.y, q.z, q.w};
    // The left palm faces the grip pose's +X.
    XrVector3f palm = rotate3(grip, {1, 0, 0});
    XrVector3f p{left.position.x, left.position.y, left.position.z};
    XrVector3f toHead = sub3(head.position, p);
    float distance = std::sqrt(dot3(toHead, toHead));
    if (!(distance > 0.05f)) {
        hideWrist();
        return;
    }
    toHead = {toHead.x / distance, toHead.y / distance, toHead.z / distance};
    XrVector3f look = forwardOf(head.orientation);
    float facing = dot3(palm, toHead);         // the palm turned toward your face
    float inView = -dot3(look, toHead);        // the hand ahead of your eyes
    bool show = g_wrist.active ? facing > 0.25f && inView > 0.55f && distance < 0.85f
                               : facing > 0.5f && inView > 0.75f && distance < 0.7f;
    if (show && !g_wrist.active) {
        static int logged = 0;
        if (logged++ < 10) LOG("Wrist menus: shown (palm %.2f toward you, %.2f m away)", facing, distance);
    }
    if (!show) {
        hideWrist();
        return;
    }
    g_wrist.active = true;
    // A screen just over the palm, facing your eyes, upright.
    float width = g_config.wristWidth, height = width * ui.Height / ui.Width;
    XrVector3f centre{p.x + palm.x * 0.05f, p.y + palm.y * 0.05f + height * 0.5f + 0.03f, p.z + palm.z * 0.05f};
    XrVector3f z = normalize3(sub3(head.position, centre));
    XrVector3f x = std::fabs(z.y) > 0.99f ? XrVector3f{1, 0, 0} : normalize3(cross3(XrVector3f{0, 1, 0}, z));
    XrVector3f y = cross3(z, x);
    g_wrist.x = x;
    g_wrist.y = y;
    g_wrist.z = z;
    g_wrist.pose = {fromAxes(x, y, z), centre};
    g_wrist.width = width;
    g_wrist.height = height;
    g_wrist.x0 = 0;
    g_wrist.y0 = 0;
    g_wrist.cw = static_cast<int32_t>(ui.Width);
    g_wrist.ch = static_cast<int32_t>(ui.Height);
    g_wrist.uiW = ui.Width;
    g_wrist.uiH = ui.Height;
}

// ---------------------------------------------------------------------------
// Controllers to game input

static bool projectNormalized(const XrVector3f& p, const XrPosef& pose, const XrFovf& fov, float& nx, float& ny);
static int g_lastEye = -1;  // eye of the latest image copied from the game

// First-person walking, handed to Python through walkInput().
static std::mutex g_walkMutex;
static float g_walk[3] = {};
static ULONGLONG g_walkTick = 0;

static void updateWalk(const XrVector2f& stick, XrTime time) {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) ||
        !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    XrVector3f f = forwardOf(head.pose.orientation);
    float len = std::sqrt(f.x * f.x + f.z * f.z);
    if (len < 0.1f) return;  // looking straight up or down: keep the last direction
    f = {f.x / len, 0, f.z / len};
    camera::Vec3 xr{f.x * stick.y - f.z * stick.x, 0, f.z * stick.y + f.x * stick.x};
    camera::Vec3 world;
    if (!camera::xrDirectionToWorld(xr, world)) return;
    float strength = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    float wl = std::sqrt(world.x * world.x + world.z * world.z);
    std::lock_guard<std::mutex> lock(g_walkMutex);
    g_walk[0] = wl > 1e-4f ? world.x / wl : 0;
    g_walk[1] = wl > 1e-4f ? world.z / wl : 0;
    g_walk[2] = strength > 1 ? 1 : strength;
    g_walkTick = GetTickCount64();
}

bool walkInput(float& x, float& z, float& strength) {
    std::lock_guard<std::mutex> lock(g_walkMutex);
    if (!g_walkTick || GetTickCount64() - g_walkTick > 500) return false;
    x = g_walk[0];
    z = g_walk[1];
    strength = g_walk[2];
    return true;
}

// Steadies the laser: strong smoothing for small hand tremor, none for fast moves.
static void smoothAim(XrVector3f& origin, XrVector3f& dir) {
    static bool have = false;
    static XrVector3f o{}, d{};
    if (!have) {
        o = origin;
        d = dir;
        have = true;
        return;
    }
    float dot = d.x * dir.x + d.y * dir.y + d.z * dir.z;
    float degrees = std::acos(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 57.2958f;
    float a = degrees / 4.0f;
    a = a < 0.08f ? 0.08f : a > 1 ? 1 : a;
    o = {o.x + (origin.x - o.x) * a, o.y + (origin.y - o.y) * a, o.z + (origin.z - o.z) * a};
    d = {d.x + (dir.x - d.x) * a, d.y + (dir.y - d.y) * a, d.z + (dir.z - d.z) * a};
    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 1e-4f) d = {d.x / len, d.y / len, d.z / len};
    origin = o;
    dir = d;
}

// Your Sim's phone (see "Your Sim's phone" below), and the hand holding it
// (0 left, 1 right).
static bool g_phoneOut = false;
static int g_phoneHand = 1;
static bool g_tablet = false;  // what's out is the tablet (first-person build/buy), not the phone
static float g_tabletAspect = 1;  // its screen's width / height (the palette's, or the whole UI's)
static void phoneSize(float& w, float& h, float& d);
static void phoneHandShift(int side, camera::Vec3& move, camera::Quat& turn);

// Hand tracking: one hand tracker each (left, right), on the new session.
// With the data source extension they report real hands only, never hands
// the runtime makes up from a controller you hold.
static void createHandTrackers() {
    if (!g_handTrackingExt || !g_session) return;
    for (int i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrHandTrackingDataSourceEXT sources[] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
        XrHandTrackingDataSourceInfoEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
        source.requestedDataSourceCount = 1;
        source.requestedDataSources = sources;
        if (g_handSourceExt) info.next = &source;
        XrResult r = xrCreateHandTrackerEXT(g_session, &info, &g_handTrackers[i]);
        if (XR_FAILED(r)) {
            g_handTrackers[i] = XR_NULL_HANDLE;
            LOG("Hand tracking: no %s hand tracker (%d)", i ? "right" : "left", static_cast<int>(r));
        }
    }
    if (g_handTrackers[0] || g_handTrackers[1]) LOG("Hand tracking: hand trackers ready");
    // Virtual Desktop hands the headset's own hand tracking to its OpenXR
    // runtime through this shared memory, only while its setting to forward
    // tracking data to the PC is on; without it the trackers never see a hand.
    if (g_handTrackers[0] || g_handTrackers[1]) {
        if (HANDLE body = OpenFileMappingW(FILE_MAP_READ, FALSE, L"VirtualDesktop.BodyState")) {
            CloseHandle(body);
            LOG("Hand tracking: Virtual Desktop forwards the headset's tracking data");
        } else if (g_runtimeName.find("VirtualDesktop") != std::string::npos) {
            LOG("Hand tracking: Virtual Desktop does NOT forward the headset's tracking data (its setting to forward "
                "hand tracking to the PC is off): no hands will come");
        }
    }
}

static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b);
static XrQuaternionf qconj(const XrQuaternionf& q);

// Your real hand (hand tracking) as armik takes a controller: the grip pose
// (in the palm, armik turns it by hand_pitch and puts the Sim's wrist 8 cm
// behind it and 2 cm toward the back of the hand), and each finger joint's
// bend. False while the runtime doesn't see that hand (or, with the data
// source extension, only makes it up from a controller).
static char g_handMiss[2][96] = {};  // why the latest trackedHand found no hand, for the log

static bool trackedHand(XrTime time, int i, armik::Hand& out) {
    if (!g_handTrackers[i] || !g_appSpace) {
        std::snprintf(g_handMiss[i], sizeof(g_handMiss[i]), "no hand tracker");
        return false;
    }
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT located{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    located.jointCount = XR_HAND_JOINT_COUNT_EXT;
    located.jointLocations = joints;
    XrHandTrackingDataSourceStateEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
    if (g_handSourceExt) located.next = &source;
    XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    info.baseSpace = g_appSpace;
    info.time = time;
    XrResult r = xrLocateHandJointsEXT(g_handTrackers[i], &info, &located);
    if (XR_FAILED(r) || !located.isActive) {
        std::snprintf(g_handMiss[i], sizeof(g_handMiss[i]), "the runtime sees no hand (result %d, active %d)",
                      static_cast<int>(r), static_cast<int>(located.isActive));
        return false;
    }
    if (g_handSourceExt && !(source.isActive && source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT)) {
        std::snprintf(g_handMiss[i], sizeof(g_handMiss[i]), "not a real hand (source active %d, kind %d)",
                      static_cast<int>(source.isActive), static_cast<int>(source.dataSource));
        return false;
    }
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; ++j) {
        if ((joints[j].locationFlags & needed) != needed) {
            std::snprintf(g_handMiss[i], sizeof(g_handMiss[i]), "joint %d not located (flags %llx)", j,
                          static_cast<unsigned long long>(joints[j].locationFlags));
            return false;
        }
    }

    // The tracked palm points -Z along the hand with +Y out of its back; the
    // Sim's hand frame (armik's tilted grip) points -Z along the hand with +Y
    // toward the thumb and the palm along +X (left) or -X (right): a quarter
    // turn about Z, the other way for each hand.
    const float s = i == 0 ? 1.0f : -1.0f;
    const float h = 0.70710678f;
    XrQuaternionf g = qmul(joints[XR_HAND_JOINT_PALM_EXT].pose.orientation, XrQuaternionf{0, 0, -s * h, h});
    // armik turns the grip down by hand_pitch about its X: undo that.
    float half = g_config.handPitch * 0.5f * 3.14159265f / 180.0f;
    XrQuaternionf o = qmul(g, XrQuaternionf{std::sin(half), 0, 0, std::cos(half)});
    // The grip where armik's wrist offset puts the Sim's wrist on yours.
    XrVector3f back = rotate3(g, {-s * 0.02f, 0, 0.08f});
    const XrVector3f& wrist = joints[XR_HAND_JOINT_WRIST_EXT].pose.position;
    out = armik::Hand{};
    out.valid = true;
    out.tracked = true;
    out.position = {wrist.x - back.x, wrist.y - back.y, wrist.z - back.z};
    out.orientation = {o.x, o.y, o.z, o.w};

    // Each joint bends by how far the next bone turns toward the palm, in
    // this bone's frame (-Z along it, +Y out of its back).
    auto bend = [&](int a, int b) {
        XrQuaternionf rel = qmul(qconj(joints[a].pose.orientation), joints[b].pose.orientation);
        XrVector3f v = rotate3(rel, {0, 0, -1});
        float angle = std::atan2(-v.y, -v.z);
        return angle < -0.35f ? -0.35f : angle > 1.9f ? 1.9f : angle;
    };
    for (int f = 1; f < 5; ++f) {
        int m = XR_HAND_JOINT_INDEX_METACARPAL_EXT + (f - 1) * 5;  // metacarpal, proximal, intermediate, distal
        for (int k = 0; k < 3; ++k) out.curl[f][k] = bend(m + k, m + k + 1);
    }
    out.curl[0][1] = bend(XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_THUMB_PROXIMAL_EXT);
    out.curl[0][2] = bend(XR_HAND_JOINT_THUMB_PROXIMAL_EXT, XR_HAND_JOINT_THUMB_DISTAL_EXT);
    // The thumb's root folds a little with the rest of it.
    float root = 0.25f * (out.curl[0][1] + out.curl[0][2]);
    out.curl[0][0] = root < 0 ? 0 : root > 0.5f ? 0.5f : root;
    return true;
}

// Your hands for the Sim (armik): where each controller is held, and how far
// each finger is curled: index on the trigger, the other three on the grip,
// the thumb down on any button, the stick or the thumb rest. In first person
// a hand the runtime tracks (controller put down) moves the Sim's hand
// instead, for looks only: here it reads as a lost controller with nothing
// pressed.
static void updateHands(XrTime time, armik::Hand hands[2]) {
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    const bool firstPerson = mode() == Mode::Stereo && camera::bodyActive();
    armik::Hand tracked[2];
    for (int i = 0; i < 2; ++i) {
        XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
        armik::Hand& h = hands[i];
        h.valid = g_gripSpaces[i] && XR_SUCCEEDED(xrLocateSpace(g_gripSpaces[i], g_appSpace, time, &grip)) &&
                  (grip.locationFlags & needed) == needed;
        // A tracked hand counts only while its controller is lost: a controller the
        // runtime still reports always wins, so a hand seen next to it (hands and
        // controllers at once) never mutes its buttons (user 2026-10-17 02:57Z).
        const bool was = g_handTracked[i];
        g_handTracked[i] = firstPerson && trackedHand(time, i, tracked[i]) && !h.valid;
        if (g_handTracked[i] != was) {
            static int logs = 0;
            if (logs++ < 40)
                LOG("Hand tracking: %s hand %s", i ? "right" : "left",
                    g_handTracked[i] ? "tracked (looks only)" : "back on its controller, or out of sight");
        }
        // A controller lost for 2 s in first person with no hand in its place: say why, once each time.
        static ULONGLONG lostSince[2] = {0, 0};
        static bool told[2] = {false, false};
        if (firstPerson && g_handTrackingExt && !h.valid && !g_handTracked[i]) {
            ULONGLONG now = GetTickCount64();
            if (!lostSince[i]) lostSince[i] = now;
            static int missLogs = 0;
            if (!told[i] && now - lostSince[i] > 2000 && missLogs++ < 20) {
                told[i] = true;
                LOG("Hand tracking: %s controller lost for 2 s, no hand in its place: %s", i ? "right" : "left",
                    g_handMiss[i][0] ? g_handMiss[i] : "?");
            }
        } else {
            lostSince[i] = 0;
            told[i] = false;
        }
        if (g_handTracked[i]) {
            h.valid = false;
            static int curlLogs = 0;
            if (curlLogs++ < 6)
                LOG("Hand tracking: %s curls thumb %.2f %.2f %.2f, index %.2f %.2f %.2f, middle %.2f %.2f %.2f",
                    i ? "R" : "L", tracked[i].curl[0][0], tracked[i].curl[0][1], tracked[i].curl[0][2],
                    tracked[i].curl[1][0], tracked[i].curl[1][1], tracked[i].curl[1][2], tracked[i].curl[2][0],
                    tracked[i].curl[2][1], tracked[i].curl[2][2]);
        }
        h.grip = floatState(g_gripAction, i);  // also while tracking is lost: a held object stays held
        if (!h.valid) continue;
        const XrPosef& p = grip.pose;
        h.position = {p.position.x, p.position.y, p.position.z};
        h.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        float trigger = floatState(g_triggerAction, i);
        h.index = boolState(g_indexTouchAction, i) ? 0.3f + 0.7f * trigger : trigger;
        h.trigger = trigger;
        bool thumb = boolState(g_thumbTouchAction, i) || boolState(g_primaryAction, i) || boolState(g_secondaryAction, i);
        h.thumb = thumb ? 1.0f : 0.0f;
    }
    // The hand holding the phone keeps one fixed pose, whatever the buttons
    // do or the hand turns: the four fingers curled by finger_curl, the thumb
    // bent by thumb_bend and swung out by thumb_spread, the hand moved and
    // turned against the phone by hand_offset and hand_turn ([phone], live in
    // the B menu).
    armik::Hand posed[2] = {hands[0], hands[1]};
    for (int i = 0; i < 2; ++i)
        if (g_handTracked[i]) posed[i] = tracked[i];
    camera::Vec3 move{0, 0, 0};
    camera::Quat turn{0, 0, 0, 1};
    const bool phoneHeld = g_phoneOut && !g_handTracked[g_phoneHand];
    if (phoneHeld) {
        posed[g_phoneHand].grip = g_config.phoneCurl;
        posed[g_phoneHand].index = g_config.phoneCurl;
        posed[g_phoneHand].trigger = 0;  // tapping the phone squeezes no Sim
        posed[g_phoneHand].thumb = 0;
        phoneHandShift(g_phoneHand, move, turn);
    }
    for (int i = 0; i < 2; ++i)
        armik::setHoldPose(i, phoneHeld && i == g_phoneHand, g_config.phoneCurl, g_config.phoneThumbBend,
                           g_config.phoneThumbSpread * 0.0174533f, move, turn);
    armik::setHands(posed);
}

// A short vibration on one controller.
static void buzz(int hand, float amplitude, float seconds) {
    if (!g_hapticAction || !g_session) return;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = g_hapticAction;
    info.subactionPath = g_handPaths[hand];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.duration = static_cast<XrDuration>(seconds * 1e9);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    xrApplyHapticFeedback(g_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

// Shoving the pie away: a hand swept fast across the pie (a wave) or pushed
// hard through it closes it, as B does. A push into the sheet needs more
// speed than a wave, so a quick poke at an option still picks it; a hand
// pulled back toward you never counts. Measured at the palm (the grip pose),
// from a short running average so one tracking jump can't do it.
static constexpr float kShoveWave = 1.0f;       // m/s along the sheet (palm or backhand)
static constexpr float kShovePush = 1.5f;       // m/s into it, away from you
static constexpr float kShoveMargin = 0.15f;    // metres past the sheet's edges
static constexpr float kShoveDepth = 0.30f;     // metres in front of or behind it
static constexpr ULONGLONG kShoveSettle = 400;  // ms after the pie opens

static bool shovePie(const armik::Hand hands[2]) {
    static XrVector3f last[2];
    XrVector3f* velocity = g_handVelocity;
    static LARGE_INTEGER lastTime[2];
    static bool tracked[2] = {false, false};
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    int shover = -1;
    float wave = 0, push = 0;
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid) {
            tracked[i] = false;
            continue;
        }
        XrVector3f p{hands[i].position.x, hands[i].position.y, hands[i].position.z};
        float dt = tracked[i] ? float(t.QuadPart - lastTime[i].QuadPart) / float(freq.QuadPart) : 0;
        XrVector3f step = tracked[i] ? sub3(p, last[i]) : XrVector3f{0, 0, 0};
        last[i] = p;
        lastTime[i] = t;
        if (!tracked[i] || dt <= 0.001f || dt > 0.1f || dot3(step, step) > 0.3f * 0.3f) {
            // First sight, a stall or a jump: start over.
            tracked[i] = true;
            velocity[i] = {0, 0, 0};
            continue;
        }
        XrVector3f v{step.x / dt, step.y / dt, step.z / dt};
        float k = std::min(1.0f, dt / 0.04f);  // about 40 ms of averaging
        velocity[i] = {velocity[i].x + (v.x - velocity[i].x) * k, velocity[i].y + (v.y - velocity[i].y) * k,
                       velocity[i].z + (v.z - velocity[i].z) * k};
        if (!g_pie.active || GetTickCount64() - g_pie.opened < kShoveSettle || g_pie.pressing == i) continue;
        // Only an empty hand slaps: no grip, trigger or button held (pointing
        // with the grip to press an option is not a slap).
        if (!emptyHand(i)) continue;
        float lx, ly, lz;
        sheetLocal(g_pie, p, lx, ly, lz);
        if (!insideSheet(g_pie, lx, ly, kShoveMargin) || std::fabs(lz) > kShoveDepth) continue;
        const XrVector3f& u = velocity[i];
        float toward = dot3(u, g_pie.z);  // + toward you
        XrVector3f along{u.x - g_pie.z.x * toward, u.y - g_pie.z.y * toward, u.z - g_pie.z.z * toward};
        float a = std::sqrt(dot3(along, along));
        if (toward > 0.5f * std::sqrt(dot3(u, u))) continue;  // pulled back toward you
        if (a > kShoveWave || -toward > kShovePush) {
            shover = i;
            wave = a;
            push = -toward;
            break;
        }
    }
    if (shover < 0) return false;
    static int logged = 0;
    if (logged++ < 20)
        LOG("Pie menu at hand: your %s hand shoved it away (%.1f m/s across it, %.1f m/s into it)",
            shover ? "right" : "left", wave, push);
    flingPie(velocity[shover]);
    dismissPie("shoved away");
    buzz(shover, 0.4f, 0.04f);
    return true;
}

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

// ---------------------------------------------------------------------------
// Menus on objects (first person, [ui] object_menus): a game picker (Cook...
// on a fridge, a TV's channel or movie picker) shows on the object it is for,
// not on the wrist or the phone. objmenu.py says where the object's front face
// is in the world; the picker is found in the UI as for the phone (fitMenu)
// and that part of the UI goes on the face: a door or front (kind 1) gets it
// near its top, at least [ui] object_menu_width wide, like a smart fridge; a
// screen (kind 2) gets it fitted inside, like the TV showing it. Point the
// laser at it or poke it, as with the pie menu.
struct ObjectMenuRequest {
    int kind = 0;
    float v[11] = {};  // centre xyz, normal xyz, up xyz, width, height (world units)
};
static std::mutex g_objMenuMutex;
static ObjectMenuRequest g_objMenuRequest;
static std::atomic<int> g_objMenuSerial{0};
static std::atomic<bool> g_objMenuWanted{false};  // a picker is on an object (no popup buzz for it)
// A game popup goes into the world as a card (kinds 4 in front of you, 5
// beside the Sim it is about; objmenu.py): the wrist gives way to it at once.
static std::atomic<bool> g_popupCardWanted{false};
static PieMenu g_objMenu;

void setObjectMenu(int kind, const float v[11]) {
    {
        std::lock_guard<std::mutex> lock(g_objMenuMutex);
        g_objMenuRequest.kind = kind;
        std::memcpy(g_objMenuRequest.v, v, sizeof(g_objMenuRequest.v));
    }
    g_objMenuWanted = kind > 0 && kind < 4;
    g_popupCardWanted = kind >= 4;
    ++g_objMenuSerial;
    static int logged = 0;
    static int lastKind = 0;
    bool follow = kind == 5 && lastKind == 5;  // the Sim's head followed: no log line each time
    lastKind = kind;
    if (follow) return;
    if (logged++ < 40) {
        if (kind == 4)
            LOG("Popup: a card in front of you");
        else if (kind == 5)
            LOG("Popup: a card beside the Sim it is about, head at %.2f %.2f %.2f", v[0], v[1], v[2]);
        else if (kind)
            LOG("Object menu: picker on a %s at %.2f %.2f %.2f facing %.2f %.2f %.2f, face %.2f x %.2f",
                kind >= 2 ? "screen" : "front", v[0], v[1], v[2], v[3], v[4], v[5], v[9], v[10]);
        else
            LOG("Object menu: off");
    }
}

static void hidePopupHud();
static bool popupOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w, bool onTop);
static void hidePickerHud();
static void pickerOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w);

static void hideObjectMenu() {
    hidePopupHud();
    hidePickerHud();
    g_objMenu.active = false;
    g_objMenu.armed[0] = g_objMenu.armed[1] = false;
    g_objMenu.pressing = -1;
    phone3d::setPanel(phone3d::Panel{}, nullptr);
}

// World (game units) to app space (metres): world = anchor + heading * (xr * scale).
static XrVector3f worldToApp(const camera::View& view, const float* w, bool direction) {
    XrVector3f d{w[0], w[1], w[2]};
    if (!direction) d = {w[0] - view.anchor.x, w[1] - view.anchor.y, w[2] - view.anchor.z};
    XrQuaternionf back{-view.heading.x, -view.heading.y, -view.heading.z, view.heading.w};
    XrVector3f r = rotate3(back, d);
    float k = direction || !(view.scale > 0) ? 1.0f : 1.0f / view.scale;
    return {r.x * k, r.y * k, r.z * k};
}

// ---------------------------------------------------------------------------
// Your own pie menu (first person, user 2026-10-17 13:44Z): poke your stomach
// with the grip held (the trigger too or not), or point the laser (grip + A /
// X) at your other hand's palm and pull the trigger. It is the menu the game
// opens for a click on your own Sim: Python sends the PieMenu widget's own
// request, interactions.choices for your Sim with a Sim pick (pie.py
// open_self, the round trip in re/liveui/components/PieMenu.md), the game
// builds it, and the widget draws it at the mouse, which waits in the middle
// of the window meanwhile. The pie at hand then shows it in front of you.
struct SelfPie {
    bool pending = false;
    ULONGLONG asked = 0, until = 0, shownAt = 0;
};
static SelfPie g_selfPie;
static constexpr float kSelfPieU = 0.5f, kSelfPieV = 0.5f;  // where the mouse waits for it
static constexpr ULONGLONG kSelfPieWait = 2000;              // ms for the game to answer
static constexpr ULONGLONG kSelfPieHold = 300;               // ms the mouse stays after it showed (the widget places it then)

static void requestSelfPie(int hand, const char* how, const XrPosef& head) {
    ULONGLONG now = GetTickCount64();
    if (g_pie.active || (g_selfPie.pending && now < g_selfPie.until)) return;
    // In front of you, a little below your eyes, wherever you look up or down.
    XrVector3f f = forwardOf(head.orientation);
    f.y = 0;
    if (!(dot3(f, f) > 1e-4f)) return;
    f = normalize3(f);
    g_lastWorldClick.valid = true;
    g_lastWorldClick.u = kSelfPieU;
    g_lastWorldClick.v = kSelfPieV;
    g_lastWorldClick.point = {head.position.x + f.x * 0.45f, head.position.y - 0.12f, head.position.z + f.z * 0.45f};
    g_lastWorldClick.tick = now;
    g_clickHand.store(hand, std::memory_order_relaxed);
    g_clickHandTick.store(now, std::memory_order_relaxed);
    g_selfPie.pending = true;
    g_selfPie.asked = now;
    g_selfPie.until = now + kSelfPieWait;
    g_selfPie.shownAt = 0;
    g_pieDismissHold = true;  // the trigger held for it clicks nothing until it lets go
    grab::pushControl(61);
    buzz(hand, 0.5f, 0.05f);
    static int logged = 0;
    if (logged++ < 30) LOG("Own pie menu: asked for (%s hand, %s)", hand ? "right" : "left", how);
}

// Every frame (updateControls), before anything else takes the mouse: while
// the game is opening your own pie, the mouse waits where it will be drawn.
static void holdSelfPieMouse(pointer::Controls& c, bool firstPerson) {
    if (!g_selfPie.pending) return;
    ULONGLONG now = GetTickCount64();
    if (g_pie.active && !g_selfPie.shownAt) g_selfPie.shownAt = now;
    bool done = !firstPerson || now >= g_selfPie.until || (g_selfPie.shownAt && now - g_selfPie.shownAt >= kSelfPieHold);
    if (done) {
        static int logged = 0;
        if (logged++ < 30)
            LOG("Own pie menu: %s after %llu ms", g_selfPie.shownAt ? "shown" : "the game showed none",
                static_cast<unsigned long long>((g_selfPie.shownAt ? g_selfPie.shownAt : now) - g_selfPie.asked));
        g_selfPie.pending = false;
        return;
    }
    c.hasTarget = true;
    c.warp = true;
    c.u = kSelfPieU;
    c.v = kSelfPieV;
    c.leftButton = false;
}

// Poking your stomach: a hand with its grip held whose Sim index fingertip
// comes from away from your Sim's belly (in front, or off to its side) and
// reaches it. The spot is between spine 1 and spine 2 (lower chest), as drawn; its front
// is kBellySurface out from them.
static constexpr float kBellySurface = 0.14f;  // metres in front of the spine that touch the belly
static constexpr float kBellyBehind = 0.05f;   // ... and behind it (a finger through it)
static constexpr float kBellyAway = 0.22f;     // in front of the spine: far enough out to poke again
static constexpr float kBellyHalfWidth = 0.15f, kBellyBelow = 0.14f, kBellyAbove = 0.16f;

static void pokeStomach(const armik::Hand hands[2], int laserHand, bool firstPerson, const XrPosef& head, bool headOk) {
    static bool armed[2] = {false, false};
    camera::View view;
    float t[6][3];
    bool ok = firstPerson && headOk && g_config.selfPiePoke && !g_pie.active && !g_selfPie.pending &&
              camera::lastView(view) && camera::torso(t);
    if (!ok) {
        armed[0] = armed[1] = false;
        return;
    }
    XrVector3f j[6];
    for (int k = 0; k < 6; ++k) j[k] = worldToApp(view, t[k], false);
    XrVector3f up = sub3(j[3], j[0]), across = sub3(j[5], j[4]);  // pelvis to spine 2; left to right shoulder
    if (!(dot3(up, up) > 1e-4f)) return;
    up = normalize3(up);
    across = sub3(across, {up.x * dot3(across, up), up.y * dot3(across, up), up.z * dot3(across, up)});
    if (!(dot3(across, across) > 1e-4f)) return;
    across = normalize3(across);
    XrVector3f ahead = cross3(up, across);
    const XrVector3f look = forwardOf(head.orientation);
    if (dot3(ahead, look) < 0) ahead = {-ahead.x, -ahead.y, -ahead.z};  // you face the way your Sim does
    // Between spine 1 and spine 2: the lower chest (user 2026-10-17 15:08Z: higher, more like the chest).
    const XrVector3f belly{(j[2].x + j[3].x) * 0.5f, (j[2].y + j[3].y) * 0.5f, (j[2].z + j[3].z) * 0.5f};
    for (int i = 0; i < 2; ++i) {
        bool can = hands[i].valid && i != laserHand && !(g_phoneOut && i == g_phoneHand) && !grab::holding(i) &&
                   !armik::holding(i) && hands[i].grip > 0.6f;
        if (!can) {
            armed[i] = false;  // a grip closed on the belly isn't a poke
            continue;
        }
        XrVector3f tip = simFingertip(hands[i], i);
        {
            // The index fingertip as the hand is drawn (handshape.h), curled or not.
            handshape::Shape shape;
            camera::Vec3 t;
            if (handshape::get(i, hands[i], shape) && handshape::tip(shape, 1, t)) tip = {t.x, t.y, t.z};
        }
        XrVector3f r = sub3(tip, belly);
        float f = dot3(r, ahead), h = dot3(r, up), s = dot3(r, across);
        bool over = std::fabs(s) < kBellyHalfWidth && h > -kBellyBelow && h < kBellyAbove;
        if (!over || f > kBellyAway) {
            armed[i] = true;
            continue;
        }
        if (!armed[i] || f > kBellySurface || f < -kBellyBehind) continue;
        armed[i] = false;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Own pie menu: your %s finger poked your stomach (%.1f cm in front of the spine, %.1f cm up, %.1f cm "
                "across, torso %.0f cm)",
                i ? "right" : "left", f * 100, h * 100, s * 100, std::sqrt(dot3(sub3(j[3], j[0]), sub3(j[3], j[0]))) * 100);
        requestSelfPie(i, "stomach poke", head);
        return;
    }
}

// The laser on your other hand (first person): anywhere on that hand as your
// Sim's hand is drawn this frame, palm, back or fingers (handshape.h; user
// 2026-10-17 14:49Z: only a small square of the hand worked). False if it misses.
static constexpr float kHandPad = 0.01f;  // metres added around the hand, for forgiving aim
static bool laserOnPalm(const armik::Hand hands[2], int laserHand, const XrVector3f& o, const XrVector3f& d,
                        XrVector3f& hit) {
    if (laserHand < 0 || !g_config.selfPiePalm || g_pie.active) return false;
    const int other = 1 - laserHand;
    if (!hands[other].valid || (g_phoneOut && other == g_phoneHand) || grab::holding(other)) return false;
    handshape::Shape shape;
    if (!handshape::get(other, hands[other], shape)) return false;
    handshape::Hit h;
    if (!handshape::raycast(shape, {o.x, o.y, o.z}, {d.x, d.y, d.z}, 3.0f, h, kHandPad)) return false;
    hit = {h.point.x, h.point.y, h.point.z};
    return true;
}

// Every frame (updateControls). True while the picker is shown on its object.
static int32_t g_popupCrop[4] = {};  // the popup's box in the UI
static bool g_popupPlace = true;     // the card in front of you: put it in front of you again
static bool updatePopupCard(const ObjectMenuRequest& r, bool firstPerson, bool headOk, const XrPosef& head);

static bool updateObjectMenu(bool firstPerson, bool headOk, const XrPosef& head) {
    const float headY = head.position.y;
    ObjectMenuRequest r;
    int serial;
    {
        std::lock_guard<std::mutex> lock(g_objMenuMutex);
        r = g_objMenuRequest;
        serial = g_objMenuSerial.load();
    }
    static int seen = 0, fitsAtOpen = 0;
    static ULONGLONG since = 0, refitAt = 0;
    ULONGLONG now = GetTickCount64();
    bool newRequest = false;
    if (serial != seen) {
        seen = serial;
        // The same popup's card following its Sim's head isn't a new one.
        static int lastKind = 0;
        bool follow = r.kind == 5 && lastKind == 5;
        lastKind = r.kind;
        newRequest = !follow;
    }
    if (newRequest) {
        hideObjectMenu();
        g_popupPlace = true;
        if (r.kind < 0) {
            // You walked away from the object: Escape answers the game's
            // picker as its Cancel would, so it closes everywhere (the wrist
            // too), and the game hears "cancel".
            g_escapeFrames = 3;
            LOG("Object menu: walked away, Escape cancels the picker");
        }
        if (r.kind > 0) {
            // The picker is modal: a pie menu still on show here is stale (a
            // second click while picking opened one), and it would hide the
            // picker until it timed out.
            closePie("a picker opened on an object");
            // Measure the picker once it has opened (fitMenu, as for the phone).
            since = g_phoneMenuSince = now;
            g_phoneFitted = false;
            fitsAtOpen = g_menuFitDone;
            refitAt = now + 2500;
        }
    }
    bool want = r.kind > 0 && (r.kind >= 4 ? g_config.popups3d : g_config.objectMenus) && firstPerson && g_uiFrame &&
                !g_pie.active && !(g_phoneOut && (g_tablet || g_phoneOptions));  // the phone's apps: popups stay in front of you
    camera::View view;
    if (!want || !camera::lastView(view)) {
        hideObjectMenu();
        phone3d::setPanel(phone3d::Panel{}, nullptr);
        return false;
    }
    // A picker on a front (the fridge) is our own card, drawn from the game's
    // message as the game's recipe picker lays it out (hud.cpp drawPicker; user
    // 2026-10-16 09:57Z: the cut of the game's flat picker looked badly
    // cropped). The cut stays for TV and computer screens, and when the
    // card's texts are not found.
    const float card = r.kind == 1 ? hud::pickerHeight() : 0.0f;
    if (r.kind == 1 && !(card > 0) && hud::pickerPending() && now - since < 3000) return false;  // its texts are being found
    // Measured again every 2.5 s: the picker can change size (tabs, more options).
    if (!(card > 0) && now >= refitAt && g_phoneFitted && !g_phoneFitPending) {
        g_phoneFitted = false;
        refitAt = now + 2500;
    }
    bool fitted = g_menuFitDone != fitsAtOpen;
    if (!(card > 0) && !fitted && now - since < 1500) return false;  // not measured yet
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t crop[4] = {W / 5, H / 5, W * 3 / 5, H * 3 / 5};  // not found: the middle of the UI
    if (fitted && g_menuFitOk) std::memcpy(crop, g_phoneCrop, sizeof(crop));
    crop[2] = std::max(16, std::min(crop[2], W));
    crop[3] = std::max(16, std::min(crop[3], H));
    crop[0] = std::max(0, std::min(W - crop[2], crop[0]));
    crop[1] = std::max(0, std::min(H - crop[3], crop[1]));
    if (r.kind >= 4) {
        // A popup: only the dialog itself (fitMenu's margin holds the game's
        // dimming around it, which stays behind on the flat UI).
        if (fitted && g_menuFitOk) {
            int32_t m = H * 5 / 1000;
            if (crop[2] > 4 * m && crop[3] > 4 * m) {
                crop[0] += m;
                crop[1] += m;
                crop[2] -= 2 * m;
                crop[3] -= 2 * m;
            }
        }
        std::memcpy(g_popupCrop, crop, sizeof(crop));
        return updatePopupCard(r, firstPerson, headOk, head);
    }

    float scale = view.scale > 0 ? view.scale : 1.0f;
    XrVector3f centre = worldToApp(view, r.v, false);
    XrVector3f z = normalize3(worldToApp(view, r.v + 3, true)), up = normalize3(worldToApp(view, r.v + 6, true));
    XrVector3f x = cross3(up, z);
    if (!(dot3(x, x) > 1e-4f)) return false;
    x = normalize3(x);
    XrVector3f y = cross3(z, x);
    float faceW = r.v[9] / scale, faceH = r.v[10] / scale;
    float aspect = card > 0 ? hud::kPicker.w / card : static_cast<float>(crop[2]) / static_cast<float>(crop[3]);
    float w, h, lift = 0;
    float screenW = 0, screenH = 0;  // a TV: its screen, the menu inside it
    if (r.kind == 3) {
        // The TV's own video screen (the model's VideoSurface mesh): the menu
        // fills it, as large as it fits, where the TV plays its pictures.
        screenW = faceW;
        screenH = faceH;
        w = screenW;
        h = w / aspect;
        if (h > screenH) {
            h = screenH;
            w = h * aspect;
        }
    } else if (r.kind == 2) {
        // No screen found in the model: the model's front is the whole TV, frame and stand too: the screen
        // is taken as a 16:9 glass 90% of its width, up against its top (where
        // the screen is on a TV with a stand). The menu sits inside it with
        // dark glass around, like the phone's.
        screenW = faceW * 0.9f;
        screenH = screenW * 9 / 16;
        if (screenH > faceH * 0.85f) {
            screenH = faceH * 0.85f;
            screenW = screenH * 16 / 9;
        }
        lift = faceH / 2 - faceH * 0.05f - screenH / 2;
        w = screenW * 0.96f;
        h = w / aspect;
        if (h > screenH * 0.94f) {
            h = screenH * 0.94f;
            w = h * aspect;
        }
    } else {
        // On a door or front, its middle at your eye height, at least
        // [ui] object_menu_width wide. Rule (user): never wider or taller than
        // the object, and always inside its front.
        w = std::max(faceW * 0.63f, g_config.objectMenuWidth);
        h = w / aspect;
        const float margin = std::min(faceW, faceH) * 0.03f;
        float maxW = faceW - 2 * margin, maxH = faceH - 2 * margin;
        if (w > maxW) {
            w = maxW;
            h = w / aspect;
        }
        if (h > maxH) {
            h = maxH;
            w = h * aspect;
        }
        lift = headOk ? (headY - centre.y) / std::max(0.3f, y.y) : faceH / 2 - margin - h / 2;
        float room = std::max(0.0f, faceH / 2 - margin - h / 2);
        lift = std::max(-room, std::min(lift, room));
    }
    if (!(w > 0.05f && h > 0.05f)) {
        hideObjectMenu();
        phone3d::setPanel(phone3d::Panel{}, nullptr);
        return false;
    }
    const float out = 0.01f;  // just in front of the face
    XrVector3f at{centre.x + y.x * lift + z.x * out, centre.y + y.y * lift + z.y * out,
                  centre.z + y.z * lift + z.z * out};
    if (card > 0) {
        // The card: one of the HUD's sheets, in the world on the front (depth
        // tested, so your hands pass in front of it); poked or pointed at as the HUD is.
        if (g_objMenu.active) {
            g_objMenu.active = false;
            phone3d::setPanel(phone3d::Panel{}, nullptr);
        }
        pickerOnHud(at, x, y, w);
        return true;
    }
    hidePickerHud();
    if (!g_objMenu.active) {
        g_objMenu.opened = now;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Object menu: shown %.2f x %.2f m on the %s (UI %dx%d at %d,%d, %s)", w, h,
                r.kind >= 2 ? "screen" : "front", crop[2], crop[3], crop[0], crop[1],
                fitted && g_menuFitOk ? "measured" : "not found, the middle");
    }
    g_objMenu.active = true;
    g_objMenu.x = x;
    g_objMenu.y = y;
    g_objMenu.z = z;
    g_objMenu.pose = {fromAxes(x, y, z), at};
    g_objMenu.width = w;
    g_objMenu.height = h;
    g_objMenu.x0 = crop[0];
    g_objMenu.y0 = crop[1];
    g_objMenu.cw = crop[2];
    g_objMenu.ch = crop[3];
    g_objMenu.uiW = ui.Width;
    g_objMenu.uiH = ui.Height;
    // Drawn inside the game's 3D view (phone3d), so your hands and anything
    // nearer pass in front of it; the flat layer only if that can't be drawn.
    // Back to world units: world = anchor + heading * (app * scale).
    phone3d::Panel panel;
    panel.show = true;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, x), wy = rotate3(heading, y);
    const float wcs[3] = {view.anchor.x + wc.x, view.anchor.y + wc.y, view.anchor.z + wc.z};
    std::memcpy(panel.centre, wcs, sizeof(wcs));
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = (screenW > 0 ? screenW : w) * scale;
    panel.height = (screenH > 0 ? screenH : h) * scale;
    if (screenW > 0) {
        panel.menu[2] = w / screenW;
        panel.menu[3] = h / screenH;
    }
    panel.crop[0] = static_cast<float>(crop[0]) / W;
    panel.crop[1] = static_cast<float>(crop[1]) / H;
    panel.crop[2] = static_cast<float>(crop[0] + crop[2]) / W;
    panel.crop[3] = static_cast<float>(crop[1] + crop[3]) / H;
    phone3d::setPanel(panel, g_uiFrame);
    return true;
}

// A game popup as a card in the world (first person, [ui] popups_3d; user
// 2026-10-15): beside the Sim it is about (kind 5: objmenu.py sends that Sim's
// head; the Sim is within 5 m, on your floor, in your room), hidden by walls
// like a speech bubble; else in front of you (kind 4), drawn over the world
// like the pie so it never ends up inside a wall, staying where it is until
// you turn or walk well away from it. Only the dialog shows (g_popupCrop),
// not the game's blue dimming around it. Point or poke it like the pie.
static bool updatePopupCard(const ObjectMenuRequest& r, bool firstPerson, bool headOk, const XrPosef& head) {
    camera::View view;
    if (!firstPerson || !headOk || !g_uiFrame || !camera::lastView(view)) {
        hideObjectMenu();
        return false;
    }
    ULONGLONG now = GetTickCount64();
    float scale = view.scale > 0 ? view.scale : 1.0f;
    D3D11_TEXTURE2D_DESC ui;
    g_uiFrame->GetDesc(&ui);
    const int32_t* crop = g_popupCrop;
    float aspect = static_cast<float>(crop[2]) / static_cast<float>(std::max(1, crop[3]));
    // Drawn from the game's own message in its dialog art (hud.cpp), once its
    // texts are found: that card's shape; else the cut of the game's dialog.
    const float drawn = hud::popupHeight();
    if (drawn > 0) aspect = hud::kPopup.w / drawn;
    const XrVector3f up{0, 1, 0};
    XrVector3f eye = head.position;
    XrVector3f look = rotate3(head.orientation, {0, 0, -1});
    XrVector3f flat = normalize3({look.x, 0, look.z});
    if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
    XrVector3f at{};
    float w = 0;
    bool onTop = false;
    if (r.kind == 5) {
        // Beside the Sim's head, to its right as you see it, like a speech
        // bubble; as wide as [ui] popup_size per metre away, so it reads the same.
        XrVector3f sim = worldToApp(view, r.v, false);
        XrVector3f to{sim.x - eye.x, 0, sim.z - eye.z};
        float away = std::sqrt(dot3(to, to));
        if (!(away > 0.05f)) to = flat, away = 0.05f;
        XrVector3f d = normalize3(to), right{-d.z, 0, d.x};
        w = std::max(0.25f, std::min(1.4f, g_config.popupSize * std::max(away, 0.5f)));
        float h = w / std::max(aspect, 0.2f);
        at = {sim.x + right.x * (0.20f + w / 2), sim.y + h / 2 - 0.10f, sim.z + right.z * (0.20f + w / 2)};
    } else {
        // In front of you, a little below your eyes, fixed in the world; put
        // in front of you again once you have turned or walked away from it.
        static float world[3] = {};
        static ULONGLONG outSince = 0;
        float distance = std::max(0.3f, g_config.popupDistance);
        if (!g_popupPlace) {
            XrVector3f was = worldToApp(view, world, false);
            XrVector3f to{was.x - eye.x, 0, was.z - eye.z};
            float gap = std::sqrt(dot3(to, to));
            bool out = gap > distance + 1.5f || gap < 0.2f ||
                       (gap > 0.01f && dot3(normalize3(to), flat) < std::cos(55 * 0.0174533f));
            if (!out)
                outSince = 0;
            else if (!outSince)
                outSince = now;
            else if (now - outSince > 800)
                g_popupPlace = true;
        }
        if (g_popupPlace) {
            g_popupPlace = false;
            outSince = 0;
            XrVector3f a{eye.x + flat.x * distance, eye.y - 0.08f, eye.z + flat.z * distance};
            XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
            XrVector3f wa = rotate3(heading, {a.x * scale, a.y * scale, a.z * scale});
            world[0] = view.anchor.x + wa.x;
            world[1] = view.anchor.y + wa.y;
            world[2] = view.anchor.z + wa.z;
        }
        at = worldToApp(view, world, false);
        XrVector3f to{at.x - eye.x, at.y - eye.y, at.z - eye.z};
        w = std::max(0.15f, g_config.popupSize * std::sqrt(dot3(to, to)));
        onTop = true;
    }
    float h = w / std::max(aspect, 0.2f);
    // Facing you, upright.
    XrVector3f z = normalize3({eye.x - at.x, eye.y - at.y, eye.z - at.z});
    XrVector3f y = normalize3({up.x - z.x * z.y, up.y - z.y * z.y, up.z - z.z * z.y});
    if (!(dot3(y, y) > 0.5f)) y = up;
    XrVector3f x = cross3(y, z);
    if (drawn > 0) {
        if (g_objMenu.active) hideObjectMenu();
        return popupOnHud(at, x, y, w, onTop);
    }
    hidePopupHud();
    if (!g_objMenu.active) {
        g_objMenu.opened = now;
        static int logged = 0;
        if (logged++ < 30)
            LOG("Popup: shown %.2f x %.2f m %s (UI %dx%d at %d,%d)", w, h,
                r.kind == 5 ? "beside the Sim" : "in front of you", crop[2], crop[3], crop[0], crop[1]);
    }
    g_objMenu.active = true;
    g_objMenu.x = x;
    g_objMenu.y = y;
    g_objMenu.z = z;
    g_objMenu.pose = {fromAxes(x, y, z), at};
    g_objMenu.width = w;
    g_objMenu.height = h;
    g_objMenu.x0 = crop[0];
    g_objMenu.y0 = crop[1];
    g_objMenu.cw = crop[2];
    g_objMenu.ch = crop[3];
    g_objMenu.uiW = ui.Width;
    g_objMenu.uiH = ui.Height;
    phone3d::Panel panel;
    panel.show = true;
    panel.clear = true;
    panel.opacity = 1;
    panel.onTop = onTop;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, x), wy = rotate3(heading, y);
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = w * scale;
    panel.height = h * scale;
    if (r.kind == 4) {
        // In front of you (app space): placed with each frame's camera.
        panel.app = true;
        panel.appAt[0] = at.x, panel.appAt[1] = at.y, panel.appAt[2] = at.z;
        panel.appX[0] = x.x, panel.appX[1] = x.y, panel.appX[2] = x.z;
        panel.appY[0] = y.x, panel.appY[1] = y.y, panel.appY[2] = y.z;
        panel.appW = w;
        panel.appH = h;
    }
    float W = static_cast<float>(ui.Width), H = static_cast<float>(ui.Height);
    panel.crop[0] = crop[0] / W;
    panel.crop[1] = crop[1] / H;
    panel.crop[2] = (crop[0] + crop[2]) / W;
    panel.crop[3] = (crop[1] + crop[3]) / H;
    phone3d::setPanel(panel, g_uiFrame);
    return true;
}

// Our own HUD (first person, [wrist]; user 2026-10-15 05:34Z: drawn with a
// third-party UI library, not crops of the game's UI). hud.cpp draws it with
// Dear ImGui from the game's values; here each part of it is placed: the watch
// (clock, day, speed buttons, the full-UI button) lying on your Sim's left
// forearm, inside the wrist; the pause symbol 30 cm under where popups show
// while the game is paused; and while your Sim sleeps the watch low in your
// view over the closed eyelids (a headset layer: the eyelids are drawn into
// the eye images). Poke with your right index finger or point the laser.

enum { kHudWatch, kHudPause, kHudSleep, kHudBuild, kHudNeeds, kHudMoodlets, kHudQueue, kHudWants, kHudPopup, kHudTip, kHudMoney,
       kHudPicker, kHudNote, kHudCount };
struct HudSheet {
    PieMenu m;  // x0, y0, cw, ch: its part of the atlas; uiW, uiH: the atlas
    bool onTop = false;
};
static HudSheet g_hud[kHudCount];
static HudSheet g_phoneSheet;  // the phone's screen with the game's phone menu (hud::kPhone), while it shows
static bool g_fullUi = false;  // the full UI over your palm, shown with the watch's button
static std::atomic<bool> g_paused{false};
static hud::Frame g_hudInput;  // this frame's pointer on the atlas (a poke or the laser)
static Swapchain g_hudSwapchain;  // the asleep watch's layer

void setPaused(bool paused) {
    if (g_paused.exchange(paused) != paused) {
        static int logged = 0;
        if (logged++ < 40) LOG("Game clock %s", paused ? "paused" : "running");
    }
}

void setClock(int hour, int minute, int day, int speed) { hud::setClock(hour, minute, day, speed); }

static void hideHud(HudSheet& s) {
    s.m.active = false;
    s.m.armed[0] = s.m.armed[1] = false;
    s.m.pressing = -1;
}

// Puts a sheet at `centre` (app space), axes x, y (z = x × y, toward you),
// `width` metres wide, showing `area` of the atlas.
static void placeHud(HudSheet& s, const XrVector3f& centre, const XrVector3f& x, const XrVector3f& y, float width,
                     const hud::Rect& area) {
    PieMenu& m = s.m;
    if (!m.active) m.opened = GetTickCount64();
    m.active = true;
    m.x = x;
    m.y = y;
    m.z = cross3(x, y);
    m.pose = {fromAxes(m.x, m.y, m.z), centre};
    m.width = width;
    m.height = width * area.h / area.w;
    m.x0 = static_cast<int32_t>(area.x);
    m.y0 = static_cast<int32_t>(area.y);
    m.cw = static_cast<int32_t>(area.w);
    m.ch = static_cast<int32_t>(area.h);
    m.uiW = hud::kWidth;
    m.uiH = hud::kHeight;
}

// The popup card drawn from the game's message (hud::kPopup, as tall as it
// is drawn), `w` metres wide at `at`; pointed at and poked as the HUD is.
static void hidePopupHud() { hideHud(g_hud[kHudPopup]); }
static bool popupOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w, bool onTop) {
    HudSheet& s = g_hud[kHudPopup];
    const float used = hud::popupHeight();
    if (!(used > 0)) {
        hideHud(s);
        return false;
    }
    if (!s.m.active) {
        static int logged = 0;
        if (logged++ < 30) LOG("Popup: drawn from the game's message, %.2f x %.2f m", w, w * used / hud::kPopup.w);
    }
    s.onTop = onTop;
    placeHud(s, at, x, y, w, {hud::kPopup.x, hud::kPopup.y, hud::kPopup.w, used});
    return true;
}

// The picker card drawn from the game's message (hud::kPicker, as tall as it
// is drawn) on its object's front, `w` metres wide at `at`.
static void hidePickerHud() { hideHud(g_hud[kHudPicker]); }
static void pickerOnHud(const XrVector3f& at, const XrVector3f& x, const XrVector3f& y, float w) {
    HudSheet& s = g_hud[kHudPicker];
    const float used = hud::pickerHeight();
    if (!(used > 0)) {
        hideHud(s);
        return;
    }
    if (!s.m.active) {
        static int logged = 0;
        if (logged++ < 30) LOG("Object menu: the picker drawn from the game's message, %.2f x %.2f m on the front", w, w * used / hud::kPicker.w);
    }
    s.onTop = false;
    placeHud(s, at, x, y, w, {hud::kPicker.x, hud::kPicker.y, hud::kPicker.w, used});
}

// Facing your eyes from `at`, upright.
static void facingAxes(const XrVector3f& at, const XrVector3f& eye, XrVector3f& x, XrVector3f& y) {
    XrVector3f z = normalize3(sub3(eye, at));
    y = normalize3({-z.x * z.y, 1 - z.y * z.y, -z.z * z.y});
    if (!(dot3(y, y) > 0.5f)) y = {0, 0, -1};
    x = cross3(y, z);
}

static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b);

// Turns a placed sheet (x right, y up, facing x * y) by yaw (about its
// facing), pitch (about its width) and roll (about its height), degrees.
static void turnSheet(XrVector3f& x, XrVector3f& y, const float turn[3]) {
    if (!turn[0] && !turn[1] && !turn[2]) return;
    const XrVector3f z = cross3(x, y);
    auto about = [](const XrVector3f& axis, float degrees) {
        float h = degrees * 0.0174533f * 0.5f, s = std::sin(h);
        return XrQuaternionf{axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
    };
    XrQuaternionf q = about({0, 0, 1}, turn[0]);
    q = qmul(q, about({1, 0, 0}, turn[1]));
    q = qmul(q, about({0, 1, 0}, turn[2]));
    auto out = [&](const XrVector3f& v) {
        XrVector3f l = rotate3(q, v);
        return XrVector3f{x.x * l.x + y.x * l.y + z.x * l.z, x.y * l.x + y.y * l.y + z.y * l.z,
                          x.z * l.x + y.z * l.y + z.z * l.z};
    };
    XrVector3f nx = out({1, 0, 0}), ny = out({0, 1, 0});
    x = nx;
    y = ny;
}

// A sheet on one of your Sim's forearms, built on its real bones: the forearm
// line from its elbow (b__X_Forearm__) to its wrist (b__X_Hand__) joint as the
// game posed them this frame, and the palm side from the Sim's hand as armik
// posed it (v0.13.145 read the hand joint's turn: 130 deg off), so it sits on
// the arm you see wherever the IK put it. pos: x metres from the wrist toward the elbow, y
// sideways round the arm, z out from the bone line; `around` degrees round the
// arm from the palm side; then turned by `turn`. It reads left to right toward
// your hand (the watch, with your left arm across you), or with `toElbow`
// toward your elbow (the action queue, running first at the wrist; the right
// arm palm up across you). False while it faces away from you or the arm isn't
// read (nothing is shown off the arm).
// What each arm sheet was placed by (onArm), for placing it again as it is
// drawn (placeArmPieces), from the joints of that very frame.
struct ArmPlace {
    bool on = false;
    int side = 0;
    float pos[3]{}, turn[3]{}, around = 0;
    bool toElbow = false;
};
static std::mutex g_armPlaceMutex;
static ArmPlace g_armPlace[16];
// The sheets around you (the wants, the pause symbol): where they sit in app
// space, put into the world again with the camera of the very frame drawn
// (placeArmPieces), so they don't jiggle per eye while you walk (user 19:15Z).
struct AppPlace {
    bool on = false;
    XrVector3f at{}, x{}, y{};
};
static AppPlace g_appPlace[16];  // g_armPlaceMutex

// A sheet on one of your Sim's forearms, in the world: its centre and axes
// (x along its width, y up it) from the forearm's joints and the posed hand,
// with `metres` world units a metre. False without the arm.
static bool armSheetWorld(const ArmPlace& a, float metres, XrVector3f& centre, XrVector3f& x, XrVector3f& y) {
    float e[3], w[3];
    if (!camera::forearm(a.side, e, w)) return false;
    camera::Vec3 handWrist;
    camera::Quat grip;
    float handScale;
    if (!armik::handPose(a.side, handWrist, grip, handScale)) return false;
    XrVector3f wrist{w[0], w[1], w[2]};
    XrVector3f axis = sub3({e[0], e[1], e[2]}, wrist);
    if (!(dot3(axis, axis) > 1e-8f)) return false;
    axis = normalize3(axis);
    const float k = a.side == 0 ? 1.0f : -1.0f;
    XrVector3f palm = rotate3(XrQuaternionf{grip.x, grip.y, grip.z, grip.w}, {k, 0, 0});
    palm = sub3(palm, {axis.x * dot3(palm, axis), axis.y * dot3(palm, axis), axis.z * dot3(palm, axis)});
    if (!(dot3(palm, palm) > 1e-6f)) return false;
    palm = normalize3(palm);
    XrVector3f sideways = cross3(axis, palm);
    float r = a.around * 0.0174533f;
    XrVector3f n = normalize3({palm.x * std::cos(r) + sideways.x * std::sin(r),
                               palm.y * std::cos(r) + sideways.y * std::sin(r),
                               palm.z * std::cos(r) + sideways.z * std::sin(r)});
    XrVector3f round = cross3(axis, n);
    const float* p = a.pos;
    centre = {wrist.x + (axis.x * p[0] + round.x * p[1] + n.x * p[2]) * metres,
              wrist.y + (axis.y * p[0] + round.y * p[1] + n.y * p[2]) * metres,
              wrist.z + (axis.z * p[0] + round.z * p[1] + n.z * p[2]) * metres};
    x = a.toElbow ? axis : XrVector3f{-axis.x, -axis.y, -axis.z};
    y = normalize3(cross3(n, x));
    turnSheet(x, y, a.turn);
    return true;
}

// Render thread (phone3d::setPiecePlacer): the arm sheets placed again from
// the joints of the frame being drawn, so they stay on the arm in both eyes.
static void placeArmPieces(phone3d::Panel* pieces, int count) {
    camera::View view;
    if (!camera::lastView(view)) return;
    const float metres = view.scale > 0 ? view.scale : 1.0f;
    ArmPlace places[16];
    AppPlace apps[16];
    {
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        std::copy(g_armPlace, g_armPlace + 16, places);
        std::copy(g_appPlace, g_appPlace + 16, apps);
    }
    const XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    for (int i = 0; i < count; ++i) {
        phone3d::Panel& p = pieces[i];
        if (p.tag < 0 || p.tag >= 16) continue;
        XrVector3f c, x, y;
        if (apps[p.tag].on) {
            const AppPlace& a = apps[p.tag];
            c = rotate3(heading, {a.at.x * metres, a.at.y * metres, a.at.z * metres});
            c = {view.anchor.x + c.x, view.anchor.y + c.y, view.anchor.z + c.z};
            x = rotate3(heading, a.x);
            y = rotate3(heading, a.y);
        } else if (!places[p.tag].on || !armSheetWorld(places[p.tag], metres, c, x, y)) {
            continue;
        }
        p.centre[0] = c.x;
        p.centre[1] = c.y;
        p.centre[2] = c.z;
        p.x[0] = x.x;
        p.x[1] = x.y;
        p.x[2] = x.z;
        p.y[0] = y.x;
        p.y[1] = y.y;
        p.y[2] = y.z;
    }
}

static bool onArm(HudSheet& s, int side, const armik::Hand& controller, const camera::View& view,
                  const XrVector3f& eye, const float pos[3], const float turn[3], float around, float width,
                  const hud::Rect& area, bool toElbow = false) {
    float e[3], w[3];
    if (!camera::forearm(side, e, w)) return false;
    XrVector3f wrist = worldToApp(view, w, false);
    XrVector3f axis = sub3(worldToApp(view, e, false), wrist);
    if (!(dot3(axis, axis) > 1e-4f)) return false;
    axis = normalize3(axis);
    // The palm side: the Sim's hand as armik last posed it (its grip frame in
    // the world; the left palm faces its +X, the right one its -X), in app space.
    camera::Vec3 handWrist;
    camera::Quat handGrip;
    float handScale;
    if (!armik::handPose(side, handWrist, handGrip, handScale)) return false;
    const float k = side == 0 ? 1.0f : -1.0f;
    XrVector3f palmWorld = rotate3(XrQuaternionf{handGrip.x, handGrip.y, handGrip.z, handGrip.w}, {k, 0, 0});
    const float pw[3] = {palmWorld.x, palmWorld.y, palmWorld.z};
    XrVector3f palm = worldToApp(view, pw, true);
    palm = sub3(palm, {axis.x * dot3(palm, axis), axis.y * dot3(palm, axis), axis.z * dot3(palm, axis)});
    if (!(dot3(palm, palm) > 1e-4f)) return false;
    palm = normalize3(palm);
    XrVector3f sideways = cross3(axis, palm);
    float r = around * 0.0174533f;
    XrVector3f n = normalize3({palm.x * std::cos(r) + sideways.x * std::sin(r),
                               palm.y * std::cos(r) + sideways.y * std::sin(r),
                               palm.z * std::cos(r) + sideways.z * std::sin(r)});
    XrVector3f round = cross3(axis, n);  // y: sideways round the arm
    XrVector3f centre{wrist.x + axis.x * pos[0] + round.x * pos[1] + n.x * pos[2],
                      wrist.y + axis.y * pos[0] + round.y * pos[1] + n.y * pos[2],
                      wrist.z + axis.z * pos[0] + round.z * pos[1] + n.z * pos[2]};
    XrVector3f toEye = sub3(eye, centre);
    float distance = std::sqrt(dot3(toEye, toEye));
    if (!(distance > 0.05f)) return false;
    XrVector3f x = toElbow ? axis : XrVector3f{-axis.x, -axis.y, -axis.z};
    XrVector3f y = normalize3(cross3(n, x));
    turnSheet(x, y, turn);
    XrVector3f facingDir = cross3(x, y);
    float facing = dot3(facingDir, {toEye.x / distance, toEye.y / distance, toEye.z / distance});
    bool show = s.m.active ? facing > 0.2f && distance < 0.9f : facing > 0.4f && distance < 0.8f;
    if (!show) return false;
    static bool logged[2] = {};
    if (!logged[side] && controller.valid) {
        // Once: how far the Sim's arm sits from your controller, for the log.
        logged[side] = true;
        const camera::Quat& c = controller.orientation;
        XrVector3f grip = rotate3(XrQuaternionf{c.x, c.y, c.z, c.w}, {k, 0, 0});
        XrVector3f elbow = worldToApp(view, e, false);
        XrVector3f fore = sub3(elbow, wrist);
        XrVector3f off = sub3(wrist, {controller.position.x, controller.position.y, controller.position.z});
        LOG("HUD: %s forearm from the Sim's joints (%.1f cm long, wrist %.1f cm from the controller); its palm side "
            "%.0f deg from the controller's",
            side ? "right" : "left", std::sqrt(dot3(fore, fore)) * 100, std::sqrt(dot3(off, off)) * 100,
            std::acos(std::clamp(dot3(palm, normalize3(grip)), -1.0f, 1.0f)) * 57.2958f);
    }
    placeHud(s, centre, x, y, width * g_config.wristScale, area);
    {
        const int index = static_cast<int>(&s - g_hud);
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        if (index >= 0 && index < 16) {
            ArmPlace& a = g_armPlace[index];
            a.on = true;
            a.side = side;
            std::copy(pos, pos + 3, a.pos);
            std::copy(turn, turn + 3, a.turn);
            a.around = around;
            a.toElbow = toElbow;
        }
    }
    return true;
}

// A sheet around you: `pos` from `origin` along right / up / ahead, facing
// your eyes, then turned.
static void aroundYou(HudSheet& s, const XrVector3f& origin, const XrVector3f& ahead, const XrVector3f& eye,
                      const float pos[3], const float turn[3], float width, const hud::Rect& area) {
    XrVector3f right = normalize3(cross3(ahead, {0, 1, 0}));
    XrVector3f at{origin.x + right.x * pos[0] + ahead.x * pos[2], origin.y + pos[1] + ahead.y * pos[2],
                  origin.z + right.z * pos[0] + ahead.z * pos[2]};
    XrVector3f x, y;
    facingAxes(at, eye, x, y);
    turnSheet(x, y, turn);
    placeHud(s, at, x, y, width * g_config.wristScale, area);
}

// A sheet drawn in the game's 3D view (phone3d): back to world units.
static phone3d::Panel hudPanel(const HudSheet& s, const camera::View& view) {
    const PieMenu& m = s.m;
    float scale = view.scale > 0 ? view.scale : 1.0f;
    phone3d::Panel panel;
    panel.show = true;
    panel.clear = true;
    panel.opacity = 1;
    panel.onTop = s.onTop;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    const XrVector3f& at = m.pose.position;
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, m.x), wy = rotate3(heading, m.y);
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = m.width * scale;
    panel.height = m.height * scale;
    panel.crop[0] = static_cast<float>(m.x0) / hud::kWidth;
    panel.crop[1] = static_cast<float>(m.y0) / hud::kHeight;
    panel.crop[2] = static_cast<float>(m.x0 + m.cw) / hud::kWidth;
    panel.crop[3] = static_cast<float>(m.y0 + m.ch) / hud::kHeight;
    return panel;
}

// A notification about one Sim as a card over their head (notify.py; [phone]
// notes_overhead), or in front of you for your own Sim. Python follows the
// head and ends it; a press dismisses it here.
struct NoteOver {
    uint64_t id = 0;
    float at[3] = {};
    bool front = false;
};
static std::mutex g_noteMutex;
static NoteOver g_noteOver;               // g_noteMutex
static std::atomic<uint64_t> g_noteDismissed{0};

void setNoteOver(uint64_t id, const float at[3], bool front) {
    if (id && id == g_noteDismissed.load()) return;
    std::lock_guard<std::mutex> lock(g_noteMutex);
    if (id != g_noteOver.id) {
        static int logged = 0;
        if (logged++ < 40) {
            if (id)
                LOG("Notification %llu: a card %s", static_cast<unsigned long long>(id),
                    front ? "in front of you (your Sim)" : "over the Sim's head");
            else
                LOG("Notification card gone");
        }
    }
    g_noteOver.id = id;
    std::memcpy(g_noteOver.at, at, sizeof(g_noteOver.at));
    g_noteOver.front = front;
}

// Moving the card in front of you (user 2026-10-18 23:52Z): close your hand on
// it, or hold the laser's trigger on it a moment, and move it; let go and the
// cards in front of you show there from then on, relative to your head (settings.cpp
// saves it, [phone] note_place).
struct NoteDrag {
    int hand = -1;          // the hand moving it; -1 none
    bool laser = false;     // by that hand's laser (else by the hand itself)
    float distance = 0;     // laser: the held point's distance along it
    XrVector3f offset{};    // the held point from the card's centre
    XrVector3f held{};      // the held point now
};
static NoteDrag g_noteDrag;
static ULONGLONG g_noteDragEnded = 0;  // a press let go after a move doesn't dismiss or answer
// The card in front of you as last placed (render thread): centre, axes, size.
static bool g_noteCardFront = false;
static XrVector3f g_noteCardAt{}, g_noteCardX{}, g_noteCardY{};
static float g_noteCardW = 0, g_noteCardH = 0;

// Where on the front card (within `slack` metres of its rectangle) a point is, or false.
static bool onNoteCard(const XrVector3f& p, float depth, float slack) {
    if (!g_noteCardFront) return false;
    const XrVector3f z = cross3(g_noteCardX, g_noteCardY), rel = sub3(p, g_noteCardAt);
    return std::fabs(dot3(rel, z)) <= depth && std::fabs(dot3(rel, g_noteCardX)) <= g_noteCardW / 2 + slack &&
           std::fabs(dot3(rel, g_noteCardY)) <= g_noteCardH / 2 + slack;
}

static bool laserOnNoteCard(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit) {
    if (!g_noteCardFront) return false;
    const XrVector3f z = cross3(g_noteCardX, g_noteCardY);
    const float den = dot3(d, z);
    if (std::fabs(den) < 1e-4f) return false;
    const float t = dot3(sub3(g_noteCardAt, o), z) / den;
    if (!(t > 0.05f && t < 10)) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    return onNoteCard(hit, 0.01f, 0);
}

static void startNoteDrag(int hand, bool laser, const XrVector3f& held) {
    g_noteDrag.hand = hand;
    g_noteDrag.laser = laser;
    g_noteDrag.offset = sub3(held, g_noteCardAt);
    g_noteDrag.held = held;
    buzz(hand, 0.4f, 0.03f);
    LOG("Notification card: moving it (%s, %s hand)", laser ? "laser" : "hand", hand ? "right" : "left");
}

static void endNoteDrag(const XrPosef& head, bool headOk) {
    if (g_noteDrag.hand < 0) return;
    buzz(g_noteDrag.hand, 0.25f, 0.02f);
    g_noteDrag.hand = -1;
    g_noteDragEnded = GetTickCount64();
    if (!headOk) return;
    XrVector3f f = rotate3(head.orientation, {0, 0, -1});
    f.y = 0;
    f = dot3(f, f) > 1e-4f ? normalize3(f) : XrVector3f{0, 0, -1};
    const XrVector3f r = cross3(f, XrVector3f{0, 1, 0});
    const XrVector3f delta = sub3(sub3(g_noteDrag.held, g_noteDrag.offset), head.position);
    const float place[3] = {dot3(delta, r), delta.y, std::max(0.25f, dot3(delta, f))};
    settings::setNotePlacement(place);
}

static uint64_t noteShown() {
    std::lock_guard<std::mutex> lock(g_noteMutex);
    return g_noteOver.id;
}

static void updateNoteCard(const camera::View& view, bool firstPerson, bool headOk, const XrPosef& head) {
    HudSheet& s = g_hud[kHudNote];
    NoteOver n;
    {
        std::lock_guard<std::mutex> lock(g_noteMutex);
        n = g_noteOver;
    }
    const float used = hud::noteHeight();
    if (!n.id || !firstPerson || !headOk || !(used > 0)) {
        g_noteCardFront = false;
        g_noteDrag.hand = -1;
        hideHud(s);
        return;
    }
    const XrVector3f eye = head.position;
    XrVector3f at, x, y;
    float w;
    if (n.front) {
        XrVector3f look = rotate3(head.orientation, {0, 0, -1});
        XrVector3f flat = normalize3({look.x, 0, look.z});
        if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
        const XrVector3f right = cross3(flat, XrVector3f{0, 1, 0});
        // Where you put it (right, up, forward of your head facing level), else 0.75 m ahead, a little low.
        float place[3] = {0, -0.12f, 0.75f};
        settings::notePlacement(place);
        if (g_noteDrag.hand >= 0)
            at = sub3(g_noteDrag.held, g_noteDrag.offset);
        else
            at = {eye.x + right.x * place[0] + flat.x * place[2], eye.y + place[1],
                  eye.z + right.z * place[0] + flat.z * place[2]};
        w = 0.42f;
    } else {
        XrVector3f headAt = worldToApp(view, n.at, false);
        XrVector3f to = sub3(headAt, eye);
        const float away = std::sqrt(dot3(to, to));
        w = std::max(0.3f, std::min(1.2f, g_config.popupSize * 0.8f * std::max(away, 0.6f)));
        const float h = w * used / hud::kNote.w;
        at = {headAt.x, headAt.y + 0.28f + h / 2, headAt.z};
    }
    facingAxes(at, eye, x, y);
    g_noteCardFront = n.front;
    g_noteCardAt = at, g_noteCardX = x, g_noteCardY = y;
    g_noteCardW = w, g_noteCardH = w * used / hud::kNote.w;
    if (!n.front) g_noteDrag.hand = -1;
    s.onTop = true;
    placeHud(s, at, x, y, w, {hud::kNote.x, hud::kNote.y, hud::kNote.w, used});
}

// All but the popup, picker and notification cards (they show whatever the HUD setting).
static void hideAllHud() {
    bool any = false;
    for (int i = 0; i < kHudCount; ++i) {
        if (i == kHudPopup || i == kHudPicker || i == kHudNote) continue;
        any = any || g_hud[i].m.active;
        hideHud(g_hud[i]);
    }
    if (any) phone3d::setPieces(nullptr, 0, nullptr);
}

// Every frame (updateControls), before the pointer is worked out.
static void updateHud(const armik::Hand hands[2], bool firstPerson, bool headOk, const XrPosef& head) {
    g_hudInput = hud::Frame{};
    camera::View view;
    const bool viewOk = camera::lastView(view);
    if (viewOk)
        updateNoteCard(view, firstPerson, headOk, head);
    else
        hideHud(g_hud[kHudNote]);
    if (!firstPerson || !headOk || !g_config.wristPieces || !viewOk) {
        hideAllHud();
        // The popup, picker and notification cards alone (taken away again once they are gone).
        static bool alone = false;
        ID3D11Texture2D* atlas = hud::texture();
        phone3d::Panel panels[3];
        int count = 0;
        for (int i : {kHudPopup, kHudPicker, kHudNote})
            if (viewOk && atlas && g_hud[i].m.active) {
                panels[count] = hudPanel(g_hud[i], view);
                panels[count++].tag = i;
            }
        if (count || alone) phone3d::setPieces(panels, count, count ? atlas : nullptr);
        alone = count > 0;
        return;
    }
    const XrVector3f eye = head.position;
    XrVector3f look = rotate3(head.orientation, {0, 0, -1});
    XrVector3f flat = normalize3({look.x, 0, look.z});
    if (!(dot3(flat, flat) > 0.5f)) flat = {0, 0, -1};
    const bool asleep = eyelids::sleeping();
    const bool leftFree = !grab::holding(0) && !(g_phoneOut && g_phoneHand == 0);

    const bool rightFree = !grab::holding(1) && !(g_phoneOut && g_phoneHand == 1);
    const Config& c = g_config;
    struct ArmPiece {
        int sheet;
        bool on;
        int side;
        const float *pos, *turn;
        float around, size;
        hud::Rect area;
        bool toElbow;
    };
    // Left wrist: the watch inside, the build/buy button on the thumb side,
    // the action queue along the top (running first at the wrist). Inside the
    // right forearm: the needs, and the moodlets toward the elbow.
    const ArmPiece pieces[] = {
        {kHudWatch, c.wristTime && leftFree, 0, c.wristTimePos, c.wristTimeTurn, c.wristTimeAround, c.wristTimeSize,
         hud::kWatch, false},
        {kHudBuild, c.wristBuild && leftFree, 0, c.wristBuildPos, c.wristBuildTurn, c.wristBuildAround,
         c.wristBuildSize, hud::kBuild, false},
        {kHudQueue, c.wristQueue && leftFree, 0, c.wristQueuePos, c.wristQueueTurn, c.wristQueueAround,
         c.wristQueueSize, hud::kQueue, true},
        {kHudNeeds, c.wristNeeds && rightFree, 1, c.wristNeedsPos, c.wristNeedsTurn, c.wristNeedsAround,
         c.wristNeedsSize, hud::kNeeds, true},
        {kHudMoodlets, c.wristMoodlets && rightFree, 1, c.wristMoodletsPos, c.wristMoodletsTurn,
         c.wristMoodletsAround, c.wristMoodletsSize, hud::kMoodlets, true},
    };
    for (const ArmPiece& p : pieces) {
        HudSheet& sheet = g_hud[p.sheet];
        if (!(p.on && !asleep &&
              onArm(sheet, p.side, hands[p.side], view, eye, p.pos, p.turn, p.around, p.size, p.area, p.toElbow)))
            hideHud(sheet);
    }
    // The wants above you, ahead: seen when you look up.
    {
        HudSheet& wants = g_hud[kHudWants];
        // Drawn over the world: tested against its depth, ceilings and walls
        // near you cut it up or hid it (user 18:44Z).
        wants.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!wants.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(40 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.05f, 0, dir.z + (flat.z - dir.z) * 0.05f});
        if (c.wristWhims && !asleep && hud::hasItems(hud::kWantItems))
            aroundYou(wants, eye, dir, eye, c.wristWhimsPos, c.wristWhimsTurn, c.wristWhimsSize, hud::kWants);
        else
            hideHud(wants);
    }

    // The money gained or lost, in front of you a little up and right, turning
    // with you lazily; over the world.
    {
        HudSheet& money = g_hud[kHudMoney];
        money.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!money.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(35 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.06f, 0, dir.z + (flat.z - dir.z) * 0.06f});
        if (c.wristMoney && !asleep && hud::moneyShowing())
            aroundYou(money, eye, dir, eye, c.wristMoneyPos, c.wristMoneyTurn, c.wristMoneySize, hud::kMoney);
        else
            hideHud(money);
    }

    // The pause symbol, 30 cm under where popups show in front of you (under
    // the popup itself while one is there), turning with you lazily. Not
    // pointable: it only shows that the game is paused.
    {
        HudSheet& pause = g_hud[kHudPause];
        pause.onTop = true;
        static XrVector3f dir{0, 0, -1};
        if (!pause.m.active)
            dir = flat;
        else if (dot3(dir, flat) < std::cos(30 * 0.0174533f))
            dir = normalize3({dir.x + (flat.x - dir.x) * 0.06f, 0, dir.z + (flat.z - dir.z) * 0.06f});
        if (c.wristPause && g_paused.load() && !asleep) {
            float distance = std::max(0.3f, c.popupDistance);
            XrVector3f at{eye.x + dir.x * distance, eye.y - 0.08f, eye.z + dir.z * distance};
            int kind;
            {
                std::lock_guard<std::mutex> lock(g_objMenuMutex);
                kind = g_objMenuRequest.kind;
            }
            if (g_objMenu.active && kind == 4) at = g_objMenu.pose.position;
            if (g_hud[kHudPopup].m.active && kind == 4) at = g_hud[kHudPopup].m.pose.position;
            aroundYou(pause, at, dir, eye, c.wristPausePos, c.wristPauseTurn, c.wristPauseSize, hud::kPauseArea);
        } else {
            hideHud(pause);
        }
    }

    // The tooltip of the need or moodlet the laser points at (user 18:34Z: as
    // the game's UI shows one), just above that sheet, turned as it is.
    {
        HudSheet& tip = g_hud[kHudTip];
        const int kind = hud::tipKind();
        const float used = hud::tipHeight();
        const int from = kind == hud::kNeedItems      ? kHudNeeds
                         : kind == hud::kMoodletItems ? kHudMoodlets
                         : kind == hud::kWantItems    ? kHudWants
                         : kind == hud::kQueueItems   ? kHudQueue
                                                      : -1;
        if (from >= 0 && g_hud[from].m.active && used > 0) {
            const PieMenu& m = g_hud[from].m;
            // The wants' tip under them (they are above you), as wide as they are.
            const bool wants = from == kHudWants;
            const float w = wants ? m.width * 0.75f : 0.22f * g_config.wristScale, h = w * used / hud::kTip.w;
            const float up = (m.height / 2 + h / 2 + 0.006f) * (wants ? -1 : 1);
            const XrVector3f at{m.pose.position.x + m.y.x * up + m.z.x * 0.006f,
                                m.pose.position.y + m.y.y * up + m.z.y * 0.006f,
                                m.pose.position.z + m.y.z * up + m.z.z * 0.006f};
            tip.onTop = true;
            placeHud(tip, at, m.x, m.y, w, {hud::kTip.x, hud::kTip.y, hud::kTip.w, used});
        } else {
            hideHud(tip);
        }
    }

    // Asleep: the watch low in your view, over the closed eyelids (ahead =
    // where you look, half its tilt up or down).
    {
        HudSheet& sleep = g_hud[kHudSleep];
        if (c.wristSleepClock && asleep) {
            XrVector3f fwd = normalize3({look.x, look.y * 0.5f, look.z});
            aroundYou(sleep, eye, fwd, eye, c.wristSleepPos, c.wristSleepTurn, c.wristSleepSize, hud::kWatch);
        } else {
            hideHud(sleep);
        }
    }

    phone3d::Panel panels[kHudCount];
    int count = 0;
    ID3D11Texture2D* atlas = hud::texture();
    if (atlas)
        for (int i = 0; i < kHudCount; ++i)
            if (i != kHudSleep && g_hud[i].m.active) {
                panels[count] = hudPanel(g_hud[i], view);
                panels[count++].tag = i;
            }
    {
        std::lock_guard<std::mutex> lock(g_armPlaceMutex);
        for (int i : {kHudWants, kHudPause, kHudTip, kHudMoney}) {
            const PieMenu& m = g_hud[i].m;
            const bool around = i != kHudTip || hud::tipKind() == hud::kWantItems;
            g_appPlace[i] = {m.active && around, m.pose.position, m.x, m.y};
        }
    }
    {
        // How often the wants come and go (user 19:15Z: they pop in and out).
        static bool was = false;
        static int changes = 0;
        static ULONGLONG since = GetTickCount64();
        const bool now = g_hud[kHudWants].m.active;
        if (now != was) ++changes, was = now;
        if (GetTickCount64() - since > 10000) {
            if (changes > 2) LOG("HUD: the wants came and went %d times in 10 s", changes);
            changes = 0;
            since = GetTickCount64();
        }
    }
    static int shown = -1;
    if (count != shown) {
        static int logged = 0;
        if (logged++ < 40) LOG("HUD: %d shown in the world%s", count, g_paused.load() ? " (paused)" : "");
        shown = count;
    }
    phone3d::setPiecePlacer(placeArmPieces);
    phone3d::setPieces(panels, count, count ? atlas : nullptr);
}

// A finger on a HUD sheet: the pointer there, pressing once it has gone
// through the surface from in front (and until it comes back out).
static bool pokeHud(HudSheet& s, const armik::Hand hands[2], const bool use[2]) {
    PieMenu& m = s.m;
    if (!m.active) return false;
    int best = -1;
    float bestZ = 1e9f, bx = 0, by = 0, bz = 0;
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid || !use[i]) {
            m.armed[i] = false;
            continue;
        }
        float lx, ly, lz;
        sheetLocal(m, simFingertip(hands[i], i), lx, ly, lz);
        if (!insideSheet(m, lx, ly, 0.01f) || lz > kPokeHover || lz < -kPokeThrough) {
            m.armed[i] = false;
            if (m.pressing == i) m.pressing = -1;
            continue;
        }
        if (lz > 0.005f) m.armed[i] = true;
        float rank = i == m.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = i;
            bx = lx;
            by = ly;
            bz = lz;
        }
    }
    if (best < 0) {
        m.pressing = -1;
        return false;
    }
    if (m.pressing == best && bz > kPokeRelease) m.pressing = -1;
    if (m.pressing != best && m.armed[best] && bz <= 0) {
        m.pressing = best;
        m.armed[best] = false;
        buzz(best, 0.2f, 0.015f);
    }
    float a = std::max(0.0f, std::min(1.0f, bx / m.width + 0.5f)), b = std::max(0.0f, std::min(1.0f, 0.5f - by / m.height));
    g_hudInput.px = m.x0 + a * m.cw;
    g_hudInput.py = m.y0 + b * m.ch;
    g_hudInput.down = m.pressing == best;
    XrVector3f onSheet{m.pose.position.x + m.x.x * bx + m.y.x * by, m.pose.position.y + m.x.y * bx + m.y.y * by,
                       m.pose.position.z + m.x.z * bx + m.y.z * by};
    g_laserTo = onSheet;
    g_laserFrom = {onSheet.x + m.z.x * 0.1f, onSheet.y + m.z.y * 0.1f, onSheet.z + m.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}

// Pokes on the watch (your right hand) or the asleep watch (either hand). They
// never reach the game.
static bool pokeHuds(const armik::Hand hands[2]) {
    static const bool right[2] = {false, true}, both[2] = {true, true};
    return pokeHud(g_hud[kHudPopup], hands, both) || pokeHud(g_hud[kHudPicker], hands, both) ||
           pokeHud(g_hud[kHudNote], hands, both) ||
           pokeHud(g_hud[kHudWatch], hands, right) ||
           pokeHud(g_hud[kHudBuild], hands, right) ||
           pokeHud(g_hud[kHudQueue], hands, right) || pokeHud(g_hud[kHudSleep], hands, both);
}

// The laser on the watch, the asleep watch, the build button, the queue or
// the wants above you: true with the hit, and the pointer for the HUD (the
// trigger presses).
static bool laserPhone(const XrVector3f& o, const XrVector3f& d, bool trigger, XrVector3f& hit) {
    const PieMenu& m = g_phoneSheet.m;
    float u, v;
    if (!m.active || !hitSheet(m, o, d, hit, u, v)) return false;
    g_hudInput.px = u * hud::kWidth;
    g_hudInput.py = v * hud::kHeight;
    g_hudInput.down = trigger;
    return true;
}

static bool laserHud(const XrVector3f& o, const XrVector3f& d, bool trigger, XrVector3f& hit) {
    if (laserPhone(o, d, trigger, hit)) return true;  // the phone's screen first (in your hand, in front of the rest)
    for (int i : {kHudPopup, kHudPicker, kHudNote, kHudSleep, kHudWatch, kHudBuild, kHudQueue, kHudWants, kHudNeeds,
                  kHudMoodlets}) {
        const PieMenu& m = g_hud[i].m;
        float u, v;
        if (!m.active || !hitSheet(m, o, d, hit, u, v)) continue;
        g_hudInput.px = u * hud::kWidth;
        g_hudInput.py = v * hud::kHeight;
        g_hudInput.down = trigger;
        return true;
    }
    return false;
}

// Present thread, after updateControls: draws the HUD and acts on its buttons.
static void drawHud(ID3D11Texture2D* backbuffer) {
    hud::Frame f = g_hudInput;
    f.watch = g_hud[kHudWatch].m.active || g_hud[kHudSleep].m.active;
    f.pause = g_hud[kHudPause].m.active;
    f.build = g_hud[kHudBuild].m.active;
    f.needs = g_hud[kHudNeeds].m.active;
    f.moodlets = g_hud[kHudMoodlets].m.active;
    f.queue = g_hud[kHudQueue].m.active;
    f.wants = g_hud[kHudWants].m.active;
    f.money = g_hud[kHudMoney].m.active;
    f.fullUi = g_fullUi;
    f.uiButton = g_config.uiFirstPerson == 3 && g_config.wristFullButton;
    // The popup's texts are looked up and it is measured before its card shows.
    f.popup = hud::popupPending();
    f.picker = hud::pickerPending();
    f.phone = g_phoneSheet.m.active;
    f.note = noteShown();
    if (!f.watch && !f.pause && !f.build && !f.needs && !f.moodlets && !f.queue && !f.wants && !f.popup && !f.money &&
        !f.picker && !f.phone && !f.note)
        return;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    device->GetImmediateContext(&context);
    int index = -1;
    hud::Action action = hud::render(device, context, f, &index);
    context->Release();
    device->Release();
    switch (action) {
        case hud::kFullUi:
            g_fullUi = !g_fullUi;
            LOG("HUD: full UI on your wrist %s", g_fullUi ? "on" : "off");
            break;
        case hud::kPausePressed:
        case hud::kSpeed1:
        case hud::kSpeed2:
        case hud::kSpeed3:
            // controls.py: 20 pause (or back to the last speed), 21..23 speed 1..3.
            grab::pushControl(20 + (action - hud::kPausePressed));
            LOG("HUD: speed button %d", action - hud::kPausePressed);
            break;
        case hud::kBuildPressed:
            g_buildKeyFrames = 3;
            LOG("HUD: build button (F2)");
            break;
        case hud::kCancel:
            // controls.py: 30 + n cancels the n-th item of the queue it sent,
            // 90 + n the side action on it.
            if (index >= 0 && index < hud::kMaxItems) grab::pushControl(30 + index);
            if (index >= hud::kMaxItems && index < 2 * hud::kMaxItems) grab::pushControl(90 + index - hud::kMaxItems);
            LOG("HUD: cancel queue item %d%s", index % hud::kMaxItems, index >= hud::kMaxItems ? " (its side action)" : "");
            break;
        case hud::kAnswer:
            // controls.py: 50 + n answers the popup with its n-th button.
            if (index >= 0 && index < 10) grab::pushControl(50 + index);
            LOG("HUD: popup button %d", index);
            break;
        case hud::kPick:
            // controls.py: 1000 + row * 4 + the ingredient boxes picks the picker's row.
            if (index >= 0) grab::pushControl(1000 + index);
            LOG("HUD: picker row %d (boxes %d)", index / 4, index % 4);
            break;
        case hud::kPickerClose:
            // controls.py: 60 cancels the picker (the game's close button).
            grab::pushControl(60);
            LOG("HUD: picker closed");
            break;
        case hud::kPhonePick:
            // controls.py: 100000 + n picks the phone menu's entry n (phoneapp.py, the game's interactions.select).
            if (index >= 0) grab::pushControl(100000 + index);
            buzz(1 - g_phoneHand, 0.2f, 0.015f);
            LOG("Phone: picked entry %d of the game's phone menu", index);
            break;
        case hud::kNotesOpened:
            grab::pushControl(78);  // notify.opened: all read
            LOG("Phone: notifications opened");
            break;
        case hud::kNoteAnswer:
            if (GetTickCount64() - g_noteDragEnded < 400) break;  // the press that moved the card
            // notify.answer: the notification's button (index = its id * 8 + button).
            if (index >= 0) grab::pushControl(10000000 + index);
            if (const uint64_t shownId = noteShown(); index >= 0 && shownId == static_cast<uint64_t>(index / 8)) {
                g_noteDismissed = shownId;  // answered on the card: it goes
                const float none[3] = {};
                setNoteOver(0, none, false);
            }
            LOG("Notification %d: button %d", index / 8, index % 8);
            break;
        case hud::kCalendarOpened:
            grab::pushControl(81);  // phonecal.send
            LOG("Phone: calendar opened");
            break;
        case hud::kNotesClear:
            grab::pushControl(79);
            LOG("Phone: notifications cleared");
            break;
        case hud::kNoteDelete:
            if (index > 0 && index < 10000) grab::pushControl(80000 + index);
            break;
        case hud::kNoteDismiss: {
            if (GetTickCount64() - g_noteDragEnded < 400) break;  // the press that moved the card
            const uint64_t id = noteShown();
            g_noteDismissed = id;
            const float none[3] = {};
            setNoteOver(0, none, false);
            LOG("Notification %llu dismissed", static_cast<unsigned long long>(id));
            break;
        }
        case hud::kPhoneColor:
            if (index >= 0 && index < 100) grab::pushControl(90000 + index);  // phoneapp.set_color
            LOG("Phone: wallpaper colour %d", index);
            break;
        case hud::kPhoneSilence:
            grab::pushControl(77);  // controls.py: the phone's silence (ui.toggle_silence_phone)
            LOG("Phone: silence button");
            break;
        case hud::kPhoneOptions:
            // The Options app: the game menu (Escape) on the phone's screen, as before.
            if (g_phoneOut && !g_tablet && g_popupOpen) {
                LOG("Phone: Options while a popup is up: answer the popup first (it is in front of you)");
            } else if (g_phoneOut && !g_tablet) {
                g_phoneOptions = true;
                g_phoneOptionsSeen = gameMenuOpen();
                if (!gameMenuOpen() && !g_popupOpen) {
                    g_escapeFrames = 3;
                    g_phoneOpenedMenu = true;
                }
                g_phoneFitted = false;
                g_phoneMenuSince = GetTickCount64();
                LOG("Phone: Options (the game menu)%s", g_popupOpen ? ", a popup is up: no Escape" : "");
            }
            break;
        case hud::kDismissWant:
            // controls.py: 40 + n dismisses the n-th want it sent.
            if (index >= 0 && index < 4) grab::pushControl(40 + index);
            LOG("HUD: dismiss want %d", index);
            break;
        default:
            break;
    }
}

// The asleep watch's headset layer: the atlas copied into its swapchain.
static bool hudSleepLayer(ID3D11Texture2D* atlas, XrCompositionLayerQuad& layer) {
    const PieMenu& m = g_hud[kHudSleep].m;
    if (!m.active || !atlas) return false;
    D3D11_TEXTURE2D_DESC desc;
    atlas->GetDesc(&desc);
    bool recreated;
    if (!ensureSwapchain(g_hudSwapchain, desc, recreated) || !copyIntoSwapchain(g_hudSwapchain, atlas, desc))
        return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_hudSwapchain);
    layer.subImage.imageRect.offset = {m.x0, m.y0};
    layer.subImage.imageRect.extent = {m.cw, m.ch};
    layer.pose = m.pose;
    layer.size = {m.width, m.height};
    return true;
}

// Sims Forever (first person): Sims 1's picture on a computer's screen
// (simsforever.h). It fills the screen the script mod found in the computer's
// model (as large as it fits, at Sims 1's aspect), drawn in the world like
// the menus on objects; the laser or a finger on it is Sims 1's mouse.
struct Sims1Screen {
    bool active = false;
    XrVector3f at{}, x{}, y{}, z{};  // app space; z out of the screen
    float width = 0, height = 0;     // the picture, metres
    float faceW = 0, faceH = 0;      // the whole screen it's on, metres
    float scale = 1;                 // world units a metre
    int pressing = -1;               // the finger pressing it
    bool armed[2] = {false, false};
    float u = 0.5f, v = 0.5f;
    int buttons = 0;
    bool used = false;  // the laser or a finger gave it the mouse this frame
};
static Sims1Screen g_sims1;

static void hideSims1Screen() {
    if (g_sims1.active || g_sims1.buttons) simsforever::mouse(g_sims1.u, g_sims1.v, 0);
    g_sims1.active = false;
    g_sims1.buttons = 0;
    g_sims1.pressing = -1;
    simsforever::setLocked(false);
    phone3d::setSims1Screen(phone3d::Panel{});
}

static bool g_laserOnClaude = false;  // the laser is on Claude's screen (it keeps the front then)

static void updateSims1Screen(bool firstPerson) {
    simsforever::poll();
    // Neither the laser nor a finger is on it any more: a held button lets go.
    if (!g_sims1.used && g_sims1.buttons) simsforever::mouse(g_sims1.u, g_sims1.v, g_sims1.buttons = 0);
    g_sims1.used = false;
    float r[11];
    camera::View view;
    if (!firstPerson || !simsforever::screen(r) || !camera::lastView(view)) {
        if (g_sims1.active) hideSims1Screen();
        return;
    }
    float scale = view.scale > 0 ? view.scale : 1.0f;
    XrVector3f centre = worldToApp(view, r, false);
    XrVector3f z = normalize3(worldToApp(view, r + 3, true)), up = normalize3(worldToApp(view, r + 6, true));
    XrVector3f x = cross3(up, z);
    if (!(dot3(x, x) > 1e-4f)) return;
    x = normalize3(x);
    XrVector3f y = cross3(z, x);
    float faceW = r[9] / scale, faceH = r[10] / scale;
    int fw = 0, fh = 0;
    float aspect = simsforever::frameSize(fw, fh) ? static_cast<float>(fw) / static_cast<float>(fh) : 4.0f / 3.0f;
    float w = faceW, h = w / aspect;
    if (h > faceH) {
        h = faceH;
        w = h * aspect;
    }
    if (!(w > 0.02f && h > 0.02f)) {
        hideSims1Screen();
        return;
    }
    const float out = 0.005f;  // just in front of the glass
    XrVector3f at{centre.x + z.x * out, centre.y + z.y * out, centre.z + z.z * out};
    if (!g_sims1.active) {
        static int logged = 0;
        if (logged++ < 20) LOG("Sims Forever: shown %.2f x %.2f m on a %.2f x %.2f m screen", w, h, faceW, faceH);
    }
    g_sims1.active = true;
    g_sims1.at = at;
    g_sims1.x = x;
    g_sims1.y = y;
    g_sims1.z = z;
    g_sims1.width = w;
    g_sims1.height = h;
    g_sims1.faceW = faceW;
    g_sims1.faceH = faceH;
    g_sims1.scale = scale;
    phone3d::Panel panel;
    panel.show = true;
    XrQuaternionf heading{view.heading.x, view.heading.y, view.heading.z, view.heading.w};
    XrVector3f wc = rotate3(heading, {at.x * scale, at.y * scale, at.z * scale});
    XrVector3f wx = rotate3(heading, x), wy = rotate3(heading, y);
    panel.centre[0] = view.anchor.x + wc.x;
    panel.centre[1] = view.anchor.y + wc.y;
    panel.centre[2] = view.anchor.z + wc.z;
    panel.x[0] = wx.x;
    panel.x[1] = wx.y;
    panel.x[2] = wx.z;
    panel.y[0] = wy.x;
    panel.y[1] = wy.y;
    panel.y[2] = wy.z;
    panel.width = faceW * scale;
    panel.height = faceH * scale;
    panel.menu[2] = w / faceW;
    panel.menu[3] = h / faceH;
    panel.crop[2] = panel.crop[3] = 1;
    phone3d::setSims1Screen(panel);
    simsforever::setLocked(true);  // your real mouse and keyboard play Sims 1 meanwhile
    // Sims 1's sound from the computer (user 18:30Z): quieter with distance,
    // panned to its side, a little duller behind you.
    if (g_eyeValid[0] && g_eyeValid[1]) {
        const XrVector3f& e0 = g_eyePose[0].position;
        const XrVector3f& e1 = g_eyePose[1].position;
        XrVector3f head{(e0.x + e1.x) * 0.5f, (e0.y + e1.y) * 0.5f, (e0.z + e1.z) * 0.5f};
        XrVector3f ears = sub3(e1, e0);
        ears = dot3(ears, ears) > 1e-6f ? normalize3(ears) : rotate3(g_eyePose[0].orientation, {1, 0, 0});
        XrVector3f ahead = rotate3(g_eyePose[0].orientation, {0, 0, -1});
        XrVector3f to = sub3(at, head);
        float d = std::sqrt(dot3(to, to));
        float pan = 0, front = 1;
        if (d > 1e-3f) {
            pan = std::fmax(-1.0f, std::fmin(1.0f, dot3(to, ears) / d));
            front = dot3(to, ahead) / d;
        }
        float g = 1.0f / (1.0f + std::fmax(0.0f, d - 0.5f) * 0.7f);
        if (front < 0) g *= 1.0f + 0.25f * front;
        float side = 1.0f + 0.8f * std::fabs(pan);
        simsforever::setEars(g * std::sqrt((1.0f - 0.8f * pan) / side), g * std::sqrt((1.0f + 0.8f * pan) / side));
    }
}

// How far the screen's glass is behind the panel's plane at u v of the picture
// (metres, <= 0; a curved screen's shape, simsforever::depth).
static float sims1Depth(float u, float v) {
    if (!(g_sims1.faceW > 0 && g_sims1.faceH > 0)) return 0;
    float fx = 0.5f + (u - 0.5f) * g_sims1.width / g_sims1.faceW;
    float fy = 0.5f + (0.5f - v) * g_sims1.height / g_sims1.faceH;
    return simsforever::depth(fx, fy) / g_sims1.scale;
}

// Where a point is on Sims 1's picture: u v 0..1 across it, lz metres in front of the glass.
static void sims1Local(const XrVector3f& p, float& u, float& v, float& lz) {
    XrVector3f rel = sub3(p, g_sims1.at);
    u = dot3(rel, g_sims1.x) / g_sims1.width + 0.5f;
    v = 0.5f - dot3(rel, g_sims1.y) / g_sims1.height;
    lz = dot3(rel, g_sims1.z) - sims1Depth(u, v);
}

static bool hitSims1(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    if (!g_sims1.active) return false;
    float facing = dot3(d, g_sims1.z);
    if (facing > -0.01f) return false;
    // The plane, then the glass's depth where it hit, a few times (curved screens).
    float shift = 0, lz = 0;
    for (int i = 0; i < 4; ++i) {
        float t = (shift - dot3(sub3(o, g_sims1.at), g_sims1.z)) / facing;
        if (t <= 0) return false;
        hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
        sims1Local(hit, u, v, lz);
        shift = sims1Depth(u, v);
    }
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

static void sims1Mouse(float u, float v, int buttons) {
    g_sims1.u = u;
    g_sims1.v = v;
    g_sims1.buttons = buttons;
    g_sims1.used = true;
    simsforever::mouse(u, v, buttons);
}

// A finger on Sims 1's picture: touching it presses the mouse there, as on the
// pie (from in front, let go when pulled back); that hand's trigger clicks too.
static bool pokeSims1(const armik::Hand hands[2], const bool trigger[2], bool rightButton) {
    if (!g_sims1.active) return false;
    int best = -1;
    float bestZ = 1e9f, bu = 0, bv = 0, bz = 0;
    for (int i = 0; i < 2; ++i) {
        if (!hands[i].valid) {
            g_sims1.armed[i] = false;
            continue;
        }
        float u, v, lz;
        sims1Local(fingertip(hands[i]), u, v, lz);
        const float mu = 0.02f / g_sims1.width, mv = 0.02f / g_sims1.height;
        bool inRange = u > -mu && u < 1 + mu && v > -mv && v < 1 + mv && lz < kPokeHover && lz > -kPokeThrough;
        if (!inRange) {
            g_sims1.armed[i] = false;
            continue;
        }
        if (lz > 0.005f) g_sims1.armed[i] = true;
        float rank = i == g_sims1.pressing ? -1e9f : std::fabs(lz);
        if (rank < bestZ) {
            bestZ = rank;
            best = i;
            bu = u;
            bv = v;
            bz = lz;
        }
    }
    if (best < 0) {
        if (g_sims1.pressing >= 0 || g_sims1.buttons) sims1Mouse(g_sims1.u, g_sims1.v, 0);
        g_sims1.pressing = -1;
        return false;
    }
    if (g_sims1.pressing == best) {
        if (bz > kPokeRelease) g_sims1.pressing = -1;
    } else if (bz <= 0 && g_sims1.armed[best]) {
        g_sims1.pressing = best;
        g_sims1.armed[best] = false;
    }
    if (g_sims1.pressing != best && g_sims1.pressing >= 0) g_sims1.pressing = -1;
    bu = std::max(0.0f, std::min(1.0f, bu));
    bv = std::max(0.0f, std::min(1.0f, bv));
    bool left = g_sims1.pressing == best || trigger[best];
    sims1Mouse(bu, bv, (left ? 1 : 0) | (rightButton ? 2 : 0));
    const float ox = (bu - 0.5f) * g_sims1.width, oy = (0.5f - bv) * g_sims1.height, oz = sims1Depth(bu, bv);
    XrVector3f onIt{g_sims1.at.x + g_sims1.x.x * ox + g_sims1.y.x * oy + g_sims1.z.x * oz,
                    g_sims1.at.y + g_sims1.x.y * ox + g_sims1.y.y * oy + g_sims1.z.y * oz,
                    g_sims1.at.z + g_sims1.x.z * ox + g_sims1.y.z * oy + g_sims1.z.z * oz};
    g_laserTo = onIt;
    g_laserFrom = {onIt.x + g_sims1.z.x * 0.1f, onIt.y + g_sims1.z.y * 0.1f, onIt.z + g_sims1.z.z * 0.1f};
    g_laserValid = true;
    g_tipOnly = true;
    return true;
}

// A hand down at your right hip (head-relative, so it works seated or
// standing and wherever you face). The left hand reaches across for it.
static bool atRightHip(const armik::Hand& hand, int side, const grab::HeadPose& head) {
    XrQuaternionf q{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
    XrVector3f right = rotate3(q, {1, 0, 0});  // stays level while you look down at your hip
    float len = std::sqrt(right.x * right.x + right.z * right.z);
    if (len < 0.2f) return false;
    right = {right.x / len, 0, right.z / len};
    XrVector3f forward{right.z, 0, -right.x};
    XrVector3f rel{hand.position.x - head.position.x, hand.position.y - head.position.y,
                   hand.position.z - head.position.z};
    float down = -rel.y, toRight = dot3(rel, right), ahead = dot3(rel, forward);
    bool hip = down > g_config.phoneHipDrop && toRight > 0.05f && ahead > -0.35f && ahead < 0.30f;
    static int logged = 0;
    if (logged++ < 20)
        LOG("Phone: %s hand closed %.2f m below your eyes, %.2f m right, %.2f m ahead: %s", side ? "right" : "left",
            down, toRight, ahead, hip ? "takes the phone out" : "not at the hip");
    return hip;
}

static void phoneOpen(int hand) {
    g_phoneOut = true;
    g_phoneHand = hand;
    g_phoneAwayAt = 0;
    g_tablet = g_buildBuy && g_config.tablet;
    g_phone.solid = g_tablet;
    g_phone.tiles = 0;
    g_tabletScan = TabletScan{};
    g_tabletLeftSeen = g_tabletGridSeen = 0;
    g_tabletMiddleRuns = 0;
    if (g_tablet) {
        // Build/buy: the tablet shows the UI as it is; no Escape, no game menu,
        // nothing for Python (the game is paused in build/buy anyway).
        hideWrist();
        if (g_pie.active) dismissPie("tablet out");
        g_phoneFitted = true;
        buzz(hand, 0.4f, 0.05f);
        static int logged = 0;
        if (logged++ < 30) LOG("Tablet: out in the %s hand (build/buy)", hand ? "right" : "left");
        return;
    }
    grab::pushControl(3);
    hideWrist();
    if (g_pie.active) dismissPie("phone out");
    // The game's phone menu shows (Python sends it); a game menu already open
    // shows as the Options app. No Escape: the menu opens from Options.
    g_phoneOptions = gameMenuOpen() && !g_popupOpen;
    g_phoneOptionsSeen = g_phoneOptions;
    hud::phoneHome();
    g_phoneFitted = false;
    g_phoneMenuSince = GetTickCount64();
    buzz(hand, 0.4f, 0.05f);
    static int logged = 0;
    if (logged++ < 30)
        LOG("Phone: out in the %s hand (%s)", hand ? "right" : "left",
            g_phoneOptions ? "the game menu was open: Options" : "the game's phone menu");
}

static void phoneClose(const char* why) {
    if (!g_phoneOut) return;
    g_phoneOut = false;
    g_phoneMenuAt = 0;
    if (g_tablet) {
        g_tablet = false;
        g_phone.solid = false;
        g_phone.tiles = 0;
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        static int logged = 0;
        if (logged++ < 30) LOG("Tablet: away (%s)", why);
        return;
    }
    hideHud(g_phoneSheet);
    if (g_phoneOptions && gameMenuOpen() && !g_popupOpen) {
        // Close the menu first, so it doesn't pause the game for a moment.
        // (Not with a popup up: Escape would answer it for you.)
        g_escapeFrames = 3;
        g_phoneAwayAt = GetTickCount64() + 250;
    } else {
        grab::pushControl(4);
    }
    g_phoneOpenedMenu = false;
    g_phoneOptions = g_phoneOptionsSeen = false;
    g_phone.active = false;
    g_phone.armed[0] = g_phone.armed[1] = false;
    g_phone.pressing = -1;
    static int logged = 0;
    if (logged++ < 30) LOG("Phone: away (%s)", why);
}

// Our phone in your hand, held like a real one: it lies in the palm, its
// screen facing where the palm faces, its top toward the thumb. The frame is
// the Sim's hand (the grip pose tilted down by hand_pitch, as armik poses it),
// so with the fingers pointing left the phone stands upright. [phone] sets it
// for the right hand; in the left it sits as the mirror image (the grip
// frames of the two hands mirror each other across X). The phone's
// body is our own picture (phoneBodyLayer); the menu sits inside its screen,
// as large as fits without stretching.
static XrPosef g_phoneBodyPose{};

// The phone where phone3d draws it: in the Sim's hand (armik's latest pose),
// the same frame and offsets as phone3d::draw, in app space. The controller's
// frame (updatePhone's fallback) sits a few cm off the Sim's hand, so the
// laser and pokes would miss what you see (user 2026-10-18 21:22Z).
static bool simPhoneFrame(int side, const float o[3], float tilt, float yaw, float pitch, XrVector3f& centre,
                          XrVector3f& x, XrVector3f& y, XrVector3f& z) {
    camera::View view;
    camera::Vec3 wrist;
    camera::Quat q;
    float handScale = 1;
    if (!camera::lastView(view) || !armik::handPose(side, wrist, q, handScale)) return false;
    const XrQuaternionf grip{q.x, q.y, q.z, q.w};
    const float m = side ? 1.0f : -1.0f;
    XrVector3f w = rotate3(grip, {(o[0] - 0.02f * m) * handScale, o[1] * handScale, (o[2] - 0.08f) * handScale});
    const float c[3] = {wrist.x + w.x, wrist.y + w.y, wrist.z + w.z};
    XrVector3f ax = rotate3(grip, {0, 0, m}), ay = rotate3(grip, {0, 1, 0}), az = rotate3(grip, {-m, 0, 0});
    const float ct = std::cos(tilt), st = std::sin(tilt);
    XrVector3f ay2{ay.x * ct - az.x * st, ay.y * ct - az.y * st, ay.z * ct - az.z * st};
    XrVector3f az2{ay.x * st + az.x * ct, ay.y * st + az.y * ct, ay.z * st + az.z * ct};
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    XrVector3f ax3{ax.x * cy - az2.x * sy, ax.y * cy - az2.y * sy, ax.z * cy - az2.z * sy};
    XrVector3f az3{ax.x * sy + az2.x * cy, ax.y * sy + az2.y * cy, ax.z * sy + az2.z * cy};
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    XrVector3f ay3{ay2.x * cp - az3.x * sp, ay2.y * cp - az3.y * sp, ay2.z * cp - az3.z * sp};
    XrVector3f az4{ay2.x * sp + az3.x * cp, ay2.y * sp + az3.y * cp, ay2.z * sp + az3.z * cp};
    const float dx[3] = {ax3.x, ax3.y, ax3.z}, dy[3] = {ay3.x, ay3.y, ay3.z}, dz[3] = {az4.x, az4.y, az4.z};
    centre = worldToApp(view, c, false);
    x = normalize3(worldToApp(view, dx, true));
    y = normalize3(worldToApp(view, dy, true));
    z = normalize3(worldToApp(view, dz, true));
    return true;
}

static void updatePhone(const armik::Hand& hand, int side) {
    // The game's phone menu (drawn by hud.cpp), or the UI's game menu (Options) / the tablet.
    const bool app = g_phoneOut && !g_tablet && (!g_phoneOptions || g_optionsFront);
    if (!g_phoneOut || !hand.valid || (!app && !g_uiFrame)) {
        phone3d::setPhone(phone3d::Phone{}, nullptr);
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        hideHud(g_phoneSheet);
        return;
    }
    D3D11_TEXTURE2D_DESC ui{};
    if (app) {
        ui.Width = hud::kWidth;
        ui.Height = hud::kHeight;
    } else {
        g_uiFrame->GetDesc(&ui);
    }
    int32_t W = static_cast<int32_t>(ui.Width), H = static_cast<int32_t>(ui.Height);
    int32_t crop[4];
    defaultPhoneCrop(W, H, crop);
    if (g_phoneFound) std::memcpy(crop, g_phoneCrop, sizeof(crop));
    if (app) {
        crop[0] = static_cast<int32_t>(hud::kPhone.x);
        crop[1] = static_cast<int32_t>(hud::kPhone.y);
        crop[2] = static_cast<int32_t>(hud::kPhone.w);
        crop[3] = static_cast<int32_t>(hud::kPhone.h);
    }
    float tileDst[phone3d::kMaxTiles][4], tileSrc[phone3d::kMaxTiles][4];
    int tiles = 0;
    if (g_tablet) {  // the tablet: the build/buy UI's pieces packed together, or all of it
        crop[0] = crop[1] = 0;
        crop[2] = W;
        crop[3] = H;
        tiles = tabletLayout(W, H, tileDst, tileSrc, g_tabletAspect);
    }
    crop[2] = std::min(crop[2], W);
    crop[3] = std::min(crop[3], H);
    crop[0] = std::max(0, std::min(W - crop[2], crop[0]));
    crop[1] = std::max(0, std::min(H - crop[3], crop[1]));
    // The Sim's hand: the grip turned down by hand_pitch about its X.
    float half = -g_config.handPitch * 0.5f * 3.14159265f / 180.0f;
    float hs = std::sin(half), hc = std::cos(half);
    const camera::Quat& q = hand.orientation;
    XrQuaternionf grip{q.w * hs + q.x * hc, q.y * hc + q.z * hs, q.z * hc - q.y * hs, q.w * hc - q.x * hs};
    XrVector3f gx = rotate3(grip, {1, 0, 0}), gy = rotate3(grip, {0, 1, 0}), gz = rotate3(grip, {0, 0, 1});
    // The right palm faces -X; the fingers point along -Z, so +Z is the phone's
    // right. The left hand mirrors it (m = -1): its palm faces +X.
    const float m = side ? 1.0f : -1.0f;
    XrVector3f x{gz.x * m, gz.y * m, gz.z * m}, y = gy, z{-gx.x * m, -gx.y * m, -gx.z * m};
    float tilt = g_config.phoneTilt * 0.0174533f, c = std::cos(tilt), s = std::sin(tilt);
    XrVector3f y2{y.x * c - z.x * s, y.y * c - z.y * s, y.z * c - z.z * s};
    XrVector3f z2{y.x * s + z.x * c, y.y * s + z.y * c, y.z * s + z.z * c};
    turnPhone(x, z2, m);
    pitchPhone(y2, z2);
    float o[3] = {g_config.phoneOffset[0] * m, g_config.phoneOffset[1], g_config.phoneOffset[2]};
    float b = g_tablet ? g_config.tabletBezel : g_config.phoneBezel, w, h, depth;
    phoneSize(w, h, depth);
    if (g_tablet) {
        // The hand holds the tablet near its outer side edge (the right hand
        // its right edge), so the tablet reaches across in front of you:
        // its centre moves along its own width, in the hand's grip frame.
        float k = -m * std::max(0.0f, w / 2 - g_config.tabletHold);
        XrVector3f shift{x.x * k, x.y * k, x.z * k};
        o[0] += dot3(shift, gx);
        o[1] += dot3(shift, gy);
        o[2] += dot3(shift, gz);
    }
    XrVector3f centre{hand.position.x + gx.x * o[0] + gy.x * o[1] + gz.x * o[2],
                      hand.position.y + gx.y * o[0] + gy.y * o[1] + gz.y * o[2],
                      hand.position.z + gx.z * o[0] + gy.z * o[1] + gz.z * o[2]};
    {
        // Where it is drawn: in the Sim's hand (phone3d), so the laser and pokes hit what you see.
        XrVector3f sc, sx, sy, sz;
        static bool logged = false;
        if (simPhoneFrame(side, o, tilt, m * g_config.phoneYaw * 0.0174533f, g_config.phonePitch * 0.0174533f, sc, sx, sy,
                          sz)) {
            if (!logged) {
                logged = true;
                XrVector3f off = sub3(sc, centre);
                LOG("Phone: placed on the Sim's hand for the laser and pokes, %.1f cm from the controller's place",
                    std::sqrt(dot3(off, off)) * 100);
            }
            centre = sc;
            x = sx;
            y2 = sy;
            z2 = sz;
        }
    }
    XrQuaternionf facing = fromAxes(x, y2, z2);
    g_phoneBodyPose = {facing, centre};

    // The screen: the body less the bezel, with a wider band at the bottom and
    // top (not on the tablet). The menu fills its width (or its height, if the menu is the taller).
    float screenW = w - 2 * b, screenH = h - 2 * b - (g_tablet ? 0 : kPhoneChin * h * 2);
    float aspect = tiles ? g_tabletAspect : static_cast<float>(crop[2]) / static_cast<float>(crop[3]);
    float menuW = screenW, menuH = screenW / aspect;
    if (menuH > screenH) {
        menuH = screenH;
        menuW = screenH * aspect;
    }
    // A little above the middle of the screen, on the glass (the game's phone menu: in the middle).
    float up = g_tablet || app ? 0 : (screenH - menuH) * 0.25f, glass = depth / 2 + 0.001f;
    XrVector3f menuCentre{centre.x + y2.x * up + z2.x * glass, centre.y + y2.y * up + z2.y * glass,
                          centre.z + y2.z * up + z2.z * glass};
    // The same phone in 3D, in the game's view (phone3d), when it can be drawn there.
    phone3d::Phone p3;
    p3.show = true;
    const XrVector3f* axes[3] = {&x, &y2, &z2};
    p3.centre[0] = centre.x;
    p3.centre[1] = centre.y;
    p3.centre[2] = centre.z;
    float* out[3] = {p3.x, p3.y, p3.z};
    for (int i = 0; i < 3; ++i) {
        out[i][0] = axes[i]->x;
        out[i][1] = axes[i]->y;
        out[i][2] = axes[i]->z;
    }
    p3.grip[0] = o[0];
    p3.grip[1] = o[1];
    p3.grip[2] = o[2];
    p3.side = side;
    p3.tilt = tilt;
    p3.yaw = m * g_config.phoneYaw * 0.0174533f;
    p3.pitch = g_config.phonePitch * 0.0174533f;
    p3.width = w;
    p3.height = h;
    p3.depth = depth;
    p3.radius = g_tablet ? std::min(w, h) * 0.05f : w * 0.14f;
    p3.screen[0] = 0;
    p3.screen[1] = 0;
    p3.screen[2] = screenW;
    p3.screen[3] = screenH;
    p3.menu[0] = 0;
    p3.menu[1] = up;
    p3.menu[2] = menuW;
    p3.menu[3] = menuH;
    p3.crop[0] = static_cast<float>(crop[0]) / W;
    p3.crop[1] = static_cast<float>(crop[1]) / H;
    p3.crop[2] = static_cast<float>(crop[0] + crop[2]) / W;
    p3.crop[3] = static_cast<float>(crop[1] + crop[3]) / H;
    p3.tiles = tiles;
    p3.wallpaper = g_tablet || app;  // the Sim's phone background (buildbuy.py) behind the game's phone menu
    g_phone.tiles = tiles;
    for (int k = 0; k < tiles; ++k) {
        for (int i = 0; i < 4; ++i) {
            p3.tileDst[k][i] = g_phone.tileDst[k][i] = tileDst[k][i];
            g_phone.tileSrc[k][i] = tileSrc[k][i];
            p3.tileSrc[k][i] = tileSrc[k][i] / static_cast<float>(i % 2 ? H : W);
        }
    }
    if (app) {
        // The game's phone menu: our HUD atlas on the glass; pokes and the laser
        // go to its buttons (pokeHud / laserHud), not to the game's UI.
        phone3d::setPhone(p3, hud::texture());
        g_phone.active = false;
        g_phone.armed[0] = g_phone.armed[1] = false;
        g_phone.pressing = -1;
        PieMenu& m = g_phoneSheet.m;
        if (!m.active) m.opened = GetTickCount64();
        m.active = true;
        m.x = x;
        m.y = y2;
        m.z = z2;
        m.pose = {facing, menuCentre};
        m.width = menuW;
        m.height = menuH;
        m.x0 = crop[0];
        m.y0 = crop[1];
        m.cw = crop[2];
        m.ch = crop[3];
        m.uiW = hud::kWidth;
        m.uiH = hud::kHeight;
        return;
    }
    hideHud(g_phoneSheet);
    phone3d::setPhone(p3, g_uiFrame);
    if (!g_phone.active) g_phone.opened = GetTickCount64();
    g_phone.active = true;
    g_phone.x = x;
    g_phone.y = y2;
    g_phone.z = z2;
    g_phone.pose = {facing, menuCentre};
    g_phone.width = menuW;
    g_phone.height = menuH;
    g_phone.x0 = crop[0];
    g_phone.y0 = crop[1];
    g_phone.cw = crop[2];
    g_phone.ch = crop[3];
    g_phone.uiW = ui.Width;
    g_phone.uiH = ui.Height;
}

// The phone's body: a dark rounded slab with a thin light rim, a black screen
// (rounded too) and a camera dot in the band above it. Made once, at the
// proportions in sims4vr.ini.
static bool phoneBodyLayer(ID3D11Device* device, XrCompositionLayerQuad& layer) {
    const float w = g_config.phoneWidth, h = g_config.phoneHeight, b = g_config.phoneBezel;
    const uint32_t texH = 512, texW = std::max<uint32_t>(64, static_cast<uint32_t>(texH * w / h + 0.5f));
    bool ok = ensureLayerImage(g_phoneBodyImage, device, texW, texH, [w, h, b](float u, float v, float* c) {
        // In metres from the centre: px right, py down.
        float px = (u - 0.5f) * w, py = (v - 0.5f) * h;
        auto roundedBox = [](float x, float y, float hw, float hh, float r) {  // signed distance, metres
            float qx = std::fabs(x) - (hw - r), qy = std::fabs(y) - (hh - r);
            float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
        };
        float pixel = h / 512.0f;
        float body = roundedBox(px, py, w / 2, h / 2, w * 0.14f);
        float a = std::max(0.0f, std::min(1.0f, 0.5f - body / pixel));
        float chin = kPhoneChin * h;
        float screen = roundedBox(px, py, w / 2 - b, h / 2 - b - chin, w * 0.14f - b);
        float inScreen = std::max(0.0f, std::min(1.0f, 0.5f - screen / pixel));
        float rim = body > -0.0012f ? 1.0f : 0.0f;
        float frame = rim ? 0.32f : 0.07f;
        float cam = std::sqrt(px * px + (py + h / 2 - b - chin / 2) * (py + h / 2 - b - chin / 2));
        if (cam < chin * 0.22f) frame = 0.015f;
        float shade = frame + (0.012f - frame) * inScreen;
        c[0] = shade * a;
        c[1] = shade * a;
        c[2] = (shade + (rim ? 0.03f : 0.01f) * (1 - inScreen)) * a;
        c[3] = a;
    });
    if (!ok || !g_phoneBodyImage.source) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ok = submitLayerImage(g_phoneBodyImage, context);
    context->Release();
    if (!ok) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_phoneBodyImage.swapchain);
    layer.pose = g_phoneBodyPose;
    layer.size = {w, h};
    return true;
}

// Where the laser lands. Stereo: on the floor the game camera looks at (or
// far away), then through the game's own camera into a window position, since
// that camera is what the game picks with. Quad: on the virtual screen.
// ---------------------------------------------------------------------------
// The VR settings menu (B; settings.cpp has the rows): a panel set down in
// front of you when it opens. The right stick and A drive it while it's open,
// and the laser points at it.
struct SettingsPanel {
    bool placed = false;
    XrPosef pose{};
    XrVector3f x{}, y{}, z{};  // z points at you
    float width = 0.62f, height = 0;
};
static SettingsPanel g_settingsPanel;
static uint64_t g_settingsUploaded = 0;  // the picture's version in g_settingsImage

// Moving the menu (user 2026-10-14 22:34Z, v0.13.117): hold its title bar with
// the laser (its trigger, or in the game camera also the grip) or close your hand
// on it, and move it; it stays upright and turns to face you. Let go and it opens
// there, relative to your head, from then on (settings.cpp saves it).
struct SettingsDrag {
    int hand = -1;              // the hand holding the menu; -1 none
    bool laser = false;         // held by that hand's laser (else by the hand itself)
    bool grip = false;          // the laser's grip holds it (else its trigger)
    float distance = 0;         // laser: the held point's distance along it
    XrVector3f fromHand{};      // hand: the held point from the hand
    float offX = 0, offY = 0;   // the held point from the panel's centre, along its x and y
};
static SettingsDrag g_settingsDrag;

static bool locateHead(XrTime time, XrPosef& pose) {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags both = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (!g_viewSpace || !XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) ||
        (head.locationFlags & both) != both)
        return false;
    pose = head.pose;
    return true;
}

// Level forward and right of the head.
static void headAxes(const XrPosef& head, XrVector3f& f, XrVector3f& r) {
    f = forwardOf(head.orientation);
    f.y = 0;
    f = dot3(f, f) > 1e-4f ? normalize3(f) : XrVector3f{0, 0, -1};
    r = cross3(f, XrVector3f{0, 1, 0});
}

static void startSettingsDrag(int hand, bool laser, const XrVector3f& held) {
    SettingsDrag& d = g_settingsDrag;
    const SettingsPanel& p = g_settingsPanel;
    d.hand = hand;
    d.laser = laser;
    XrVector3f r = sub3(held, p.pose.position);
    d.offX = dot3(r, p.x);
    d.offY = dot3(r, p.y);
    buzz(hand, 0.4f, 0.03f);
    LOG("Settings: moving the menu (%s, %s hand)", laser ? (d.grip ? "laser, grip" : "laser, trigger") : "hand",
        hand ? "right" : "left");
}

// The held point to `at`; upright, facing the head.
static void moveSettings(const XrVector3f& at, const XrVector3f& head) {
    SettingsPanel& p = g_settingsPanel;
    const SettingsDrag& d = g_settingsDrag;
    XrVector3f z{head.x - at.x, 0, head.z - at.z};
    if (dot3(z, z) > 1e-4f) p.z = normalize3(z);
    p.x = normalize3(cross3(XrVector3f{0, 1, 0}, p.z));
    p.y = cross3(p.z, p.x);
    const XrVector3f c{at.x - p.x.x * d.offX - p.y.x * d.offY, at.y - p.x.y * d.offX - p.y.y * d.offY,
                       at.z - p.x.z * d.offX - p.y.z * d.offY};
    p.pose = {fromAxes(p.x, p.y, p.z), c};
}

static void endSettingsDrag(XrTime time) {
    SettingsDrag& d = g_settingsDrag;
    if (d.hand < 0) return;
    buzz(d.hand, 0.25f, 0.02f);
    d.hand = -1;
    XrPosef head;
    if (!locateHead(time, head)) return;
    XrVector3f f, r;
    headAxes(head, f, r);
    const SettingsPanel& p = g_settingsPanel;
    const XrVector3f delta = sub3(p.pose.position, head.position);
    const XrVector3f facing{-f.x, 0, -f.z};
    const float turn = std::atan2(facing.z * p.z.x - facing.x * p.z.z, dot3(facing, p.z)) * 57.29578f;
    const float place[4] = {dot3(delta, r), delta.y, dot3(delta, f), turn};
    settings::setPlacement(place);
}

static void openSettings(XrTime time) {
    XrPosef head;
    if (!locateHead(time, head)) return;
    XrVector3f f, right;
    headAxes(head, f, right);
    // Where you last put it (relative to your head), or 0.75 m ahead, a little low.
    float place[4] = {0, -0.08f, 0.75f, 0};
    settings::placement(place);
    SettingsPanel& p = g_settingsPanel;
    const float a = place[3] / 57.29578f, ca = std::cos(a), sa = std::sin(a);
    p.z = {-f.x * ca - f.z * sa, 0, f.x * sa - f.z * ca};  // facing you, turned by place[3] about up
    p.x = normalize3(cross3(XrVector3f{0, 1, 0}, p.z));
    p.y = cross3(p.z, p.x);
    const XrVector3f& h = head.position;
    p.pose = {fromAxes(p.x, p.y, p.z),
              {h.x + right.x * place[0] + f.x * place[2], h.y + place[1], h.z + right.z * place[0] + f.z * place[2]}};
    g_settingsDrag.hand = -1;
    int w = 0, hgt = 0;
    uint64_t version = 0;
    settings::image(w, hgt, version);
    p.height = w > 0 ? p.width * hgt / w : p.width * 0.8f;
    p.placed = true;
    settings::open();
    buzz(1, 0.3f, 0.03f);
}

static void closeSettings() {
    g_settingsDrag.hand = -1;  // a move in progress isn't kept
    g_settingsPanel.placed = false;
    // A new render_scale: the size is worked out again from the next views.
    if (settings::close()) g_renderSize[0] = g_renderSize[1] = 0;
    buzz(1, 0.3f, 0.03f);
}

static bool hitSettings(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    const SettingsPanel& p = g_settingsPanel;
    if (!p.placed || !settings::isOpen()) return false;
    float facing = dot3(d, p.z);
    if (facing > -0.01f) return false;
    float t = -dot3(sub3(o, p.pose.position), p.z) / facing;
    if (t <= 0) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    XrVector3f r = sub3(hit, p.pose.position);
    float lx = dot3(r, p.x), ly = dot3(r, p.y);
    if (std::fabs(lx) > p.width / 2 || std::fabs(ly) > p.height / 2) return false;
    u = lx / p.width + 0.5f;
    v = 0.5f - ly / p.height;
    return true;
}

static bool settingsLayer(ID3D11Device* device, XrCompositionLayerQuad& layer) {
    if (!g_settingsPanel.placed || !settings::isOpen()) return false;
    int w = 0, h = 0;
    uint64_t version = 0;
    const uint32_t* pixels = settings::image(w, h, version);
    if (!pixels || w <= 0 || h <= 0) return false;
    D3D11_TEXTURE2D_DESC want{};
    want.Width = static_cast<UINT>(w);
    want.Height = static_cast<UINT>(h);
    want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    want.SampleDesc.Count = 1;
    bool recreated;
    LayerImage& image = g_settingsImage;
    if (!ensureSwapchain(image.swapchain, want, recreated)) return false;
    const DXGI_FORMAT f = image.swapchain.format;
    if (recreated || !image.source) {
        if (image.source) image.source->Release();
        image.source = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = want.Width;
        desc.Height = want.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = f;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &image.source))) {
            image.source = nullptr;
            return false;
        }
        g_settingsUploaded = 0;
        image.uploaded = false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (version != g_settingsUploaded) {
        // The picture is sRGB, premultiplied BGRA: reorder for an RGBA swapchain,
        // and linearise for one the runtime reads as linear.
        const bool rgbaOrder = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                               f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const bool srgb = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        static std::vector<uint32_t> converted;
        const uint32_t* data = pixels;
        if (rgbaOrder || !srgb) {
            static uint8_t linear[256];
            static bool table = false;
            if (!table) {
                for (int i = 0; i < 256; ++i) {
                    float c = i / 255.0f;
                    c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                    linear[i] = static_cast<uint8_t>(c * 255 + 0.5f);
                }
                table = true;
            }
            converted.resize(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < converted.size(); ++i) {
                uint32_t p = pixels[i], a = p >> 24, r = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
                if (!srgb) {
                    r = linear[r];
                    g = linear[g];
                    b = linear[b];
                }
                converted[i] = rgbaOrder ? (a << 24 | b << 16 | g << 8 | r) : (a << 24 | r << 16 | g << 8 | b);
            }
            data = converted.data();
        }
        context->UpdateSubresource(image.source, 0, nullptr, data, static_cast<UINT>(w) * 4, 0);
        g_settingsUploaded = version;
        image.uploaded = false;
    }
    bool ok = submitLayerImage(image, context);
    context->Release();
    if (!ok) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(image.swapchain);
    layer.pose = g_settingsPanel.pose;
    layer.size = {g_settingsPanel.width, g_settingsPanel.height};
    return true;
}

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
        if (g_phoneOut && (!closed[g_phoneHand] || !firstPerson || !g_config.phone || g_tablet != tabletNow)) {
            phoneClose(!firstPerson ? "left first person" : g_tablet != tabletNow ? "build/buy changed" : "hand opened");
        } else if (!g_phoneOut && firstPerson && g_config.phone && head.valid) {
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
        float a = std::fabs(right.x);
        g_turnChecking = a > 0.15f;
        if (g_turnChecking && step > 0 && step < 0.1) {
            TurnCheck& c = g_turnCheck;
            c.stepMin = std::min(c.stepMin, step * 1000);
            c.stepMax = std::max(c.stepMax, step * 1000);
            c.stepSum += step * 1000;
            ++c.steps;
            c.missed += refreshes - 1;
        }
        if (a > 0.15f) {
            float shaped = (a - 0.15f) / 0.85f;
            shaped *= shaped;
            g_stickRate = (right.x > 0 ? -1 : 1) * shaped * g_config.bodyTurnRate * 0.0174533f;
            camera::turnBody(g_stickRate * dt);
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
        static int logged = 0;
        if (logged++ < 60)
            LOG("Laser: click %s (pie %s, UI alpha %.2f, near %.2f%s%s)",
                poking ? "taken by a finger poke" : pointing >= 0 ? "ignored: the other hand points"
                : onPie ? "on a menu" : onWorld ? "into the world" : c.hasTarget ? "on the UI" : "on nothing",
                g_pie.active ? "open" : "closed", g_uiAlphaUnderLaser, g_uiAlphaNearLaser,
                gameMenuOpen() ? ", game menu open" : "", g_pieDismissHold ? ", held after a pie close" : "");
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

// ---------------------------------------------------------------------------
// Per-frame rendering

static XrSwapchainSubImage subImage(const Swapchain& s) {
    XrSwapchainSubImage sub{};
    sub.swapchain = s.handle;
    sub.imageRect.extent = {static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)};
    return sub;
}

static bool renderUiPanel(XrCompositionLayerQuad& layer);

static bool renderQuad(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, XrCompositionLayerQuad& layer) {
    bool recreated;
    if (!ensureSwapchain(g_quadSwapchain, desc, recreated)) return false;
    g_captureSlot = screenshot::kFlatScreen;
    bool copied = copyIntoSwapchain(g_quadSwapchain, backbuffer, desc);
    g_captureSlot = -1;
    if (!copied) return false;

    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_quadSwapchain);
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, 0.0f, -g_config.quadDistance};
    layer.size = {g_config.quadWidth, g_config.quadWidth * desc.Height / desc.Width};
    return true;
}


// The flat screen (main menu, loading) floats in a black room. Sent as quad
// layers alone, it swam with your head in Virtual Desktop: turn your head
// and it slid along a little, then caught up. Frames that carry one
// projection layer as well kept their quads still (the Crash Team Racing VR
// mod's "one-picture fix" notes, and our own first person view), so the black
// room is now that projection layer: plain black, at this frame's views.
static bool voidLayer(ID3D11Device* device, XrTime time, XrCompositionLayerProjection& layer,
                      XrCompositionLayerProjectionView (&views)[2]) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = g_appSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!XR_SUCCEEDED(xrLocateViews(g_session, &locate, &viewState, 2, &count, located)) || count != 2 ||
        !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;
    if (!ensureLayerImage(g_voidImage, device, 256, 256, [](float, float, float* c) {
            c[0] = c[1] = c[2] = 0;
            c[3] = 1;
        }))
        return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    bool ok = submitLayerImage(g_voidImage, context);
    context->Release();
    if (!ok) return false;
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage = subImage(g_voidImage.swapchain);
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

// Every layer is checked before it goes to the runtime: a NaN or absurd pose
// or size can take the headset's compositor down with it.
static bool saneFloat(float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

static bool sanePose(XrPosef& pose) {
    XrQuaternionf& q = pose.orientation;
    float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(n) || n < 0.5f || n > 1.5f) return false;
    n = 1 / std::sqrt(n);
    q = {q.x * n, q.y * n, q.z * n, q.w * n};
    const XrVector3f& p = pose.position;
    return saneFloat(p.x, 1000) && saneFloat(p.y, 1000) && saneFloat(p.z, 1000);
}

__attribute__((noinline)) static bool saneLayer(const XrCompositionLayerBaseHeader* header) {
    if (header->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        auto* quad = const_cast<XrCompositionLayerQuad*>(reinterpret_cast<const XrCompositionLayerQuad*>(header));
        return sanePose(quad->pose) && saneFloat(quad->size.width, 200) && saneFloat(quad->size.height, 200) &&
               quad->size.width > 0 && quad->size.height > 0;
    }
    if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        for (uint32_t i = 0; i < projection->viewCount; ++i) {
            auto& view = const_cast<XrCompositionLayerProjectionView&>(projection->views[i]);
            const XrFovf& f = view.fov;
            if (!sanePose(view.pose) || !saneFloat(f.angleLeft, 1.55f) || !saneFloat(f.angleRight, 1.55f) ||
                !saneFloat(f.angleUp, 1.55f) || !saneFloat(f.angleDown, 1.55f) || f.angleLeft >= f.angleRight ||
                f.angleDown >= f.angleUp)
                return false;
        }
    }
    return true;
}

static void steadyMirror(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, int eye) {
    ID3D11Device* device = nullptr;
    backbuffer->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (g_mirror) {
        D3D11_TEXTURE2D_DESC have;
        g_mirror->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format ||
            have.SampleDesc.Count != desc.SampleDesc.Count) {
            g_mirror->Release();
            g_mirror = nullptr;
            g_mirrorValid = false;
        }
    }
    if (!g_mirror) {
        D3D11_TEXTURE2D_DESC m = desc;
        m.Usage = D3D11_USAGE_DEFAULT;
        m.BindFlags = desc.SampleDesc.Count > 1 ? D3D11_BIND_RENDER_TARGET : 0;
        m.CPUAccessFlags = 0;
        m.MiscFlags = 0;
        HRESULT hr = device->CreateTexture2D(&m, nullptr, &g_mirror);
        if (FAILED(hr)) g_mirror = nullptr;
        LOG("Monitor mirror %ux%u format %d samples %u bind 0x%x misc 0x%x: %s (0x%08lx)", desc.Width, desc.Height,
            static_cast<int>(desc.Format), desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags,
            g_mirror ? "created" : "FAILED", static_cast<unsigned long>(hr));
    }
    static uint64_t counts[2] = {};
    if (eye == 0 || eye == 1) {
        if (++counts[eye] % 3000 == 0)
            LOG("Monitor mirror: %llu left, %llu right frames", static_cast<unsigned long long>(counts[0]),
                static_cast<unsigned long long>(counts[1]));
    }
    if (g_mirror) {
        if (eye == 0) {
            context->CopyResource(g_mirror, backbuffer);
            g_mirrorValid = true;
        } else if (g_mirrorValid) {
            context->CopyResource(backbuffer, g_mirror);
        }
    }
    context->Release();
    device->Release();
}

// Game render size for the headset: the runtime's pixel density (recommended
// size over the eye's FOV) across the frustum the game renders (the eye's own,
// or with exact_fov off a centred one covering its wider side), times
// render_scale. Computed once per session from the first located views, and
// again when the settings menu closes after a change to these.
static void computeRenderSize(const XrView (&views)[2]) {
    if (g_renderSize[0] || g_config.renderScale <= 0 || !g_recommended[0][0]) return;
    const bool exact = g_config.fovOverride && g_config.exactFov;
    float densityX = 0, densityY = 0, width = 0, height = 0;
    for (int eye = 0; eye < 2; ++eye) {
        const XrFovf& f = views[eye].fov;
        float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
        if (r - l < 0.1f || u - d < 0.1f) return;
        densityX = std::fmax(densityX, g_recommended[eye][0] / (r - l));
        densityY = std::fmax(densityY, g_recommended[eye][1] / (u - d));
        width = std::fmax(width, exact ? r - l : 2 * std::fmax(-l, r));
        height = std::fmax(height, exact ? u - d : 2 * std::fmax(-d, u));
        if (!g_fovLogged)
            LOG("Headset %s eye FOV: left %.1f, right %.1f, up %.1f, down %.1f degrees", eye ? "right" : "left",
                f.angleLeft * 57.29578f, f.angleRight * 57.29578f, f.angleUp * 57.29578f, f.angleDown * 57.29578f);
    }
    g_fovLogged = true;
    auto size = [](float pixels) {
        int n = static_cast<int>(pixels * g_config.renderScale / 8 + 0.5f) * 8;
        return n < 64 ? 64 : n > 4096 ? 4096 : n;
    };
    g_renderSize[0] = size(densityX * width);
    g_renderSize[1] = size(densityY * height);
    LOG("Headset render size %dx%d (render_scale %.2f, %s eye view)", g_renderSize[0], g_renderSize[1],
        g_config.renderScale, exact ? "lopsided" : "centred");
}

// Ask the game for the headset size while stereo runs; its own size otherwise.
// Switching resets the game's device, so brief gaps don't switch back.
static void updateRenderSize(bool stereo) {
    static ULONGLONG lastStereo = 0;
    ULONGLONG now = GetTickCount64();
    if (stereo && g_renderSize[0]) {
        lastStereo = now;
        renderscale::request(g_renderSize[0], g_renderSize[1]);
    } else if (now - lastStereo > 1500) {
        renderscale::request(0, 0);
    }
}

// ---------------------------------------------------------------------------
// SteamVR: the older eye turned to the newest head pose (eyealign.h).

static bool g_alignBroken = false;  // a turned image couldn't be drawn: off for this session
static bool alignEyes() { return g_steamVR && g_config.alignEyes && !g_alignBroken; }
// Your body on its own layer: only the turn smoothing needs it, and not with
// SteamVR's eye align (which redraws the older eye).
static bool bodyLayerOn() { return g_config.bodyLayer && g_config.turnReprojection && !alignEyes(); }

static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static XrQuaternionf qconj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

static XrQuaternionf qnorm(const XrQuaternionf& q) {
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? XrQuaternionf{q.x / n, q.y / n, q.z / n, q.w / n} : XrQuaternionf{0, 0, 0, 1};
}

static XrVector3f qrot(const XrQuaternionf& q, const XrVector3f& v) {
    XrQuaternionf r = qmul(qmul(q, {v.x, v.y, v.z, 0}), qconj(q));
    return {r.x, r.y, r.z};
}

// Row-major rotation matrix of a unit quaternion.
static void qmatrix(const XrQuaternionf& q, float m[9]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}

// `fresh` was just copied: the other eye's kept image is drawn into its
// swapchain turned to where that eye is now relative to the fresh eye's
// render pose (from this frame's located views), so both views carry one
// head pose, and its pose and heading become the fresh eye's.
static void alignOlderEye(int fresh, const XrView (&located)[2]) {
    const int old = 1 - fresh;
    if (!eyealign::has(old) || !shownChain(old).handle) return;
    const XrPosef& from = g_eyePose[fresh];
    const XrPosef &lf = located[fresh].pose, &lo = located[old].pose;
    XrPosef target;
    target.orientation = qnorm(qmul(from.orientation, qmul(qconj(lf.orientation), lo.orientation)));
    const XrVector3f apart = qrot(qconj(lf.orientation), {lo.position.x - lf.position.x, lo.position.y - lf.position.y,
                                                          lo.position.z - lf.position.z});
    const XrVector3f offset = qrot(from.orientation, apart);
    target.position = {from.position.x + offset.x, from.position.y + offset.y, from.position.z + offset.z};

    // The old image as if rendered at the fresh one's heading in the world
    // (a stick turn in between turns the world, like the turn reprojection).
    XrQuaternionf was = g_eyePose[old].orientation;
    float turn = g_config.turnReprojection && std::isfinite(g_eyeYaw[old]) && std::isfinite(g_eyeYaw[fresh])
                     ? std::remainder(g_eyeYaw[old] - g_eyeYaw[fresh], 6.2831853f)
                     : 0.0f;
    if (std::fabs(turn) > 1e-5f && std::fabs(turn) < 0.5f)
        was = qmul({0, std::sin(turn / 2), 0, std::cos(turn / 2)}, was);
    else
        turn = 0;
    float rotation[9];
    qmatrix(qnorm(qmul(qconj(was), target.orientation)), rotation);
    const XrFovf& f = g_eyeFov[old];
    const float tangents[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};

    Swapchain& s = shownChain(old);
    uint32_t index = 0;
    if (!acquireImage(s, index)) return;
    bool drawn = eyealign::draw(old, s.images[index].texture, s.format, rotation, tangents);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    bool released = check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
    // The image released now is what the swapchain shows: turned, or (if the
    // draw failed) whatever that image held before, so the pose that goes
    // with it is unknown: leave the eye out until it is rendered again.
    if (!released) return;
    if (!drawn) {
        g_eyeValid[old] = false;
        eyealign::forget(old);
        g_alignBroken = true;
        LOG("Eye align: couldn't draw the turned eye; off");
        return;
    }
    g_shownPose[old] = target;
    g_shownYaw[old] = g_eyeYaw[fresh];

    static int logs = 0;
    static ULONGLONG lastLog = 0;
    if (logs < 10 && GetTickCount64() - lastLog > 30000) {
        ++logs;
        lastLog = GetTickCount64();
        XrQuaternionf moved = qmul(qconj(g_eyePose[old].orientation), target.orientation);
        float angle = 2 * std::acos(std::min(1.0f, std::fabs(moved.w))) * 57.29578f;
        LOG("Eye align: the %s eye turned %.2f degrees to the newest head pose (turn %.2f)", old ? "right" : "left",
            angle, turn * 57.29578f);
    }
}

// The stick turn check: each frame while the right stick turns, how far the
// view heading the eyes are corrected to moved on, and how far the eyes were
// turned back; a line every 2 s of turning (and when you let go).
static void turnCheck(float nowYaw, const float (&turned)[2], bool newPicture) {
    TurnCheck& c = g_turnCheck;
    auto flush = [&] {
        if (c.frames >= 20 && g_turnCheckLogs < 60) {
            ++g_turnCheckLogs;
            const double seconds = double(GetTickCount64() - c.start) / 1000.0;
            LOG("Turning check: %.1f s, %d frames (%d refreshes missed), %d new pictures; display clock steps %.2f to "
                "%.2f ms (%.2f on average); the view turned %.2f to %.2f degrees a frame (%.2f on average); eyes "
                "turned back %.2f to %.2f degrees; drawn up to %.1f degrees ahead (%.0f ms)",
                seconds, c.frames, c.missed, c.pictures, c.steps ? c.stepMin : 0.0, c.steps ? c.stepMax : 0.0,
                c.steps ? c.stepSum / c.steps : 0.0, c.turns ? c.turnMin : 0.0, c.turns ? c.turnMax : 0.0,
                c.turns ? c.turnSum / c.turns : 0.0, c.frames && c.backMin < 1e8f ? c.backMin : 0.0f,
                c.frames && c.backMax > -1e8f ? c.backMax : 0.0f, c.leadMax, g_turnLatency * 1000);
        }
        c = TurnCheck{};
    };
    if (!g_turnChecking || !std::isfinite(nowYaw)) {
        flush();
        return;
    }
    if (!c.start) c.start = GetTickCount64();
    ++c.frames;
    if (newPicture) ++c.pictures;
    if (std::isfinite(c.lastHeading)) {
        const double step = std::fabs(std::remainder(nowYaw - c.lastHeading, 6.2831853f)) * 57.29578;
        c.turnMin = std::min(c.turnMin, step);
        c.turnMax = std::max(c.turnMax, step);
        c.turnSum += step;
        ++c.turns;
    }
    c.lastHeading = nowYaw;
    for (float back : turned) {
        if (back == 0) continue;
        c.backMin = std::min(c.backMin, std::fabs(back) * 57.29578f);
        c.backMax = std::max(c.backMax, std::fabs(back) * 57.29578f);
    }
    if (GetTickCount64() - c.start >= 2000) {
        const float heading = c.lastHeading;
        flush();
        c.start = GetTickCount64();
        c.lastHeading = heading;
    }
}

static bool renderStereo(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, XrTime time,
                         XrCompositionLayerProjection& layer, XrCompositionLayerProjectionView (&views)[2]) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = g_appSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!check(xrLocateViews(g_session, &locate, &viewState, 2, &count, located), "xrLocateViews")) return false;
    computeRenderSize(located);
    bool newPicture = false;  // a new eye picture (or pair) shown from this frame on

    // Turning now, or about to? (kBodyLinger)
    if (bodyLayerOn()) {
        const float yaw = camera::viewYaw();
        if ((std::isfinite(yaw) && std::isfinite(g_lastTurnYaw) &&
             std::fabs(std::remainder(yaw - g_lastTurnYaw, 6.2831853f)) > 1e-4f) ||
            boolState(g_stickTouchAction, 1) || std::fabs(stickState(1).x) > 0.05f)
            g_lastTurnTick = GetTickCount64();
        g_lastTurnYaw = yaw;
    }
    const bool turning = g_lastTurnTick && GetTickCount64() - g_lastTurnTick < kBodyLinger;

    // 1. The backbuffer holds what the camera hook rendered: the oldest
    //    entry, since the game thread can run a frame ahead of Present.
    camera::Rendered rendered;
    if (camera::takeRendered(rendered)) {
        int eye = rendered.eye;
        // Paired eyes: a left eye waits for its right eye (in the left
        // swapchain not shown now).
        const bool hold = eye == 0 && rendered.pairFirst && !alignEyes();
        Swapchain& chain = hold ? heldChain() : shownChain(eye);
        bool recreated;
        if (!ensureSwapchain(chain, desc, recreated)) return false;
        if (recreated && hold) {
            g_leftHeld = false;
        } else if (recreated) {
            g_eyeValid[eye] = false;
            eyealign::forget(eye);
        }
        const camera::Vec3& p = rendered.position;
        const camera::Quat& q = rendered.orientation;
        g_overlayPose = {{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}};
        g_overlayFov = {rendered.fov.left, rendered.fov.right, rendered.fov.up, rendered.fov.down};
        g_overlayEye = true;
        g_captureSlot = eye;  // screenshot::kLeftEye / kRightEye
        g_keepEye = alignEyes() ? eye : -1;
        const int slot = chainSlot(chain);
        // Not with the phone or the pie drawn into the view at your hand: they
        // are drawn after the depth that finds your body, so only the pieces
        // in front of your hand would go with it.
        const bool handUi = g_pie.active || ((g_phone.active || g_phoneSheet.m.active) && phone3d::drawnRecently());
        g_splitSlot = bodyLayerOn() && !handUi ? slot : -1;
        g_splitWanted = turning;
        g_splitFrom = &rendered;
        if (slot >= 0 && g_splitSlot < 0) g_bodyState[slot] = kBodyUnknown;  // no longer goes with it
        bool copied = copyIntoSwapchain(chain, backbuffer, desc);
        g_splitSlot = -1;
        g_splitFrom = nullptr;
        g_keepEye = -1;
        g_captureSlot = -1;
        g_overlayEye = false;
        if (copied && hold) {
            g_heldPose = g_overlayPose;
            g_heldFov = g_overlayFov;
            g_heldYaw = rendered.yaw;
            g_leftHeld = true;
        } else if (hold) {
            g_leftHeld = false;
        } else if (copied) {
            if (eye == 0) g_leftHeld = false;  // shown straight away: nothing waits
            if (eye == 1 && g_leftHeld) {
                // Its left eye: show it now, together with this one.
                g_leftShown ^= 1;
                g_leftHeld = false;
                g_eyePose[0] = g_shownPose[0] = g_heldPose;
                g_eyeFov[0] = g_heldFov;
                g_eyeYaw[0] = g_shownYaw[0] = g_heldYaw;
                g_eyeValid[0] = true;
                if (++g_pairsShown == 1) LOG("Paired eyes: the headset shows both eyes of a pair together");
            }
            newPicture = true;
            g_eyePose[eye] = g_overlayPose;
            g_eyeFov[eye] = g_overlayFov;
            g_eyeYaw[eye] = rendered.yaw;
            g_shownPose[eye] = g_overlayPose;
            g_shownYaw[eye] = rendered.yaw;
            if (eye == 1) g_shownLead1 = rendered.lead;
            g_eyeValid[eye] = true;
            g_lastEye = eye;
            if (alignEyes() && g_eyeValid[1 - eye]) alignOlderEye(eye, located);
        }
        g_frameEye = eye;
        if (!g_ownMirror) steadyMirror(backbuffer, desc, eye);
    }

    // 2. Point the game camera at the other eye for the frames it renders next.
    //    Body mode also wants the headset itself, between the eyes.
    const XrPosef &l = located[0].pose, &r = located[1].pose;
    camera::setHead({(l.position.x + r.position.x) / 2, (l.position.y + r.position.y) / 2,
                     (l.position.z + r.position.z) / 2},
                    {l.orientation.x, l.orientation.y, l.orientation.z, l.orientation.w});
    camera::setPairedEyes(g_config.pairedEyes && !alignEyes());
    camera::setExactFov(g_config.exactFov);
    camera::setPairedHands(g_config.pairedHands);
    camera::setBodyLayer(bodyLayerOn());
    int next = g_renderingEye == 0 ? 1 : 0;
    const XrPosef& p = located[next].pose;
    const XrFovf& f = located[next].fov;
    camera::apply(next, {p.position.x, p.position.y, p.position.z},
                  {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
                  {f.angleLeft, f.angleRight, f.angleUp, f.angleDown});
    g_renderingEye = next;

    // 3. Submit once both eyes hold an image. Each view carries the pose its
    //    image was rendered with, so the runtime reprojects it correctly.
    if (!g_eyeValid[0] || !g_eyeValid[1]) return false;
    // A stick turn (or the game camera turning) turns the world, not your
    // head, so the runtime can't see it: the eyes, rendered a frame apart,
    // showed the world at two headings (a blur while turning), and each
    // turned only every other frame. Each eye's pose is turned back by how
    // far the world has turned since it was rendered, about your head, so
    // the runtime reprojects that turn like a head turn, every refresh.
    float nowYaw = g_config.turnReprojection ? camera::viewYaw() : NAN;
    // Turn lead: how long a pair's heading is set before it's shown, from how
    // far the shown right eye trails the turn (its lead taken out), averaged
    // while the stick turns; the next pair is drawn that far ahead.
    {
        const bool leadOn = g_config.turnLead && g_config.turnReprojection && !alignEyes();
        if (leadOn && std::isfinite(nowYaw) && std::isfinite(g_shownYaw[1]) && g_eyeValid[1] &&
            std::fabs(g_stickRate) > 0.3f) {
            const float behind = g_shownLead1 - std::remainder(g_shownYaw[1] - nowYaw, 6.2831853f);
            const float seconds = behind / g_stickRate;
            if (seconds > 0 && seconds < 0.2f) g_turnLatency += (seconds - g_turnLatency) * 0.03f;
        }
        const float lead = leadOn ? std::clamp(g_stickRate * g_turnLatency, -0.3f, 0.3f) : 0.0f;
        camera::setTurnLead(lead);
        if (g_turnChecking) g_turnCheck.leadMax = std::max(g_turnCheck.leadMax, std::fabs(lead) * 57.29578f);
    }
    const XrVector3f head{(l.position.x + r.position.x) / 2, (l.position.y + r.position.y) / 2,
                          (l.position.z + r.position.z) / 2};
    float turned[2] = {0, 0};
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = g_shownPose[eye];
        views[eye].fov = g_eyeFov[eye];
        views[eye].subImage = subImage(shownChain(eye));
        float delta = std::isfinite(nowYaw) && std::isfinite(g_shownYaw[eye])
                          ? std::remainder(g_shownYaw[eye] - nowYaw, 6.2831853f)
                          : 0.0f;
        if (std::fabs(delta) < 1e-5f || std::fabs(delta) > 0.5f) continue;  // still, or a jump (a new view)
        turned[eye] = delta;
        XrPosef& pose = views[eye].pose;
        const float c = std::cos(delta / 2), s = std::sin(delta / 2);  // about +y: {0, s, 0, c}
        const XrQuaternionf o = pose.orientation;
        pose.orientation = {c * o.x + s * o.z, c * o.y + s * o.w, c * o.z - s * o.x, c * o.w - s * o.y};
        const float dx = pose.position.x - head.x, dz = pose.position.z - head.z;
        const float cd = std::cos(delta), sd = std::sin(delta);
        pose.position.x = head.x + cd * dx + sd * dz;
        pose.position.z = head.z - sd * dx + cd * dz;
    }
    // Your body, which turns with you, on its own layer that the turn leaves
    // alone (bodylayer.h): each eye's as it was drawn.
    g_bodyLayerReady = false;
    if (bodyLayerOn()) {
        const int slots[2] = {chainSlot(shownChain(0)), chainSlot(shownChain(1))};
        if (slots[0] >= 0 && slots[1] >= 0 && g_bodyState[slots[0]] != kBodyUnknown &&
            g_bodyState[slots[1]] != kBodyUnknown &&
            (g_bodyState[slots[0]] == kBodyDrawn || g_bodyState[slots[1]] == kBodyDrawn)) {
            for (int eye = 0; eye < 2; ++eye) {
                g_bodyViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                g_bodyViews[eye].pose = g_shownPose[eye];
                g_bodyViews[eye].fov = g_eyeFov[eye];
                g_bodyViews[eye].subImage = subImage(g_bodyChains[slots[eye]]);
            }
            g_bodyProjection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            g_bodyProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
            g_bodyProjection.space = g_appSpace;
            g_bodyProjection.viewCount = 2;
            g_bodyProjection.views = g_bodyViews;
            g_bodyLayerReady = true;
        }
    }
    turnCheck(nowYaw, turned, newPicture);
    static int turnLogs = 0;
    static ULONGLONG lastTurnLog = 0;
    if ((turned[0] != 0 || turned[1] != 0) && turnLogs < 20 && GetTickCount64() - lastTurnLog > 2000) {
        ++turnLogs;
        lastTurnLog = GetTickCount64();
        LOG("Turning: eyes turned back %.2f / %.2f degrees (left / right) to the newest heading%s",
            turned[0] * 57.29578f, turned[1] * 57.29578f,
            g_bodyLayerReady ? "; your body on its own layer, not turned" : "");
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

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

}  // namespace s4vr::vr
