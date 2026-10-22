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
    bool hasD3D11 = false, hasHands = false, hasHandSource = false, hasDepth = false;
    std::string names;
    for (const auto& e : extensions) {
        hasD3D11 |= std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        hasHands |= std::strcmp(e.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0;
        hasHandSource |= std::strcmp(e.extensionName, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME) == 0;
        hasDepth |= std::strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
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
    const bool wantDepth = config.depthLayer && hasDepth;
    if (wantDepth) enabled.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
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
    g_depthExt = false;
    for (const char* name : enabled) g_depthExt |= std::strcmp(name, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
    if (config.depthLayer)
        LOG("Depth layer: %s", g_depthExt ? "on (each eye's depth goes to the headset)"
                               : hasDepth ? "the runtime refused its extension; off"
                                          : "the runtime has no XR_KHR_composition_layer_depth; off");

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

