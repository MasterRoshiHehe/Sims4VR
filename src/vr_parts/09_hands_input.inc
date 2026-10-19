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
static std::atomic<bool> g_onCall{false};  // a phone call of your Sim is running (nophone.py): the phone stays in the hand
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

