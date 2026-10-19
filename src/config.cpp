#include "config.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace s4vr {

std::wstring userIniPath(const std::wstring& ini) {
    size_t slash = ini.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash + 1)) + L"sims4vr_user.ini";
}

// One file's value; false if that file doesn't set it.
static bool readFrom(const std::wstring& file, const char* section, const char* key, std::string& out) {
    char buf[512];
    std::string path(file.begin(), file.end());
    GetPrivateProfileStringA(section, key, "\x01", buf, sizeof(buf), path.c_str());
    if (buf[0] == '\x01' && buf[1] == 0) return false;
    out = buf;
    return true;
}

std::string readSetting(const std::wstring& ini, const char* section, const char* key, const char* def) {
    std::string value;
    if (readFrom(userIniPath(ini), section, key, value) || readFrom(ini, section, key, value)) return value;
    return def;
}

std::string readShippedSetting(const std::wstring& ini, const char* section, const char* key, const char* def) {
    std::string value;
    return readFrom(ini, section, key, value) ? value : def;
}

static std::string readString(const std::wstring& ini, const char* section, const char* key, const char* def) {
    return readSetting(ini, section, key, def);
}

static float readFloat(const std::wstring& ini, const char* section, const char* key, float def) {
    std::string s = readString(ini, section, key, "");
    return s.empty() ? def : static_cast<float>(atof(s.c_str()));
}

// true/false, yes/no, on/off or a number.
static bool readBool(const std::wstring& ini, const char* section, const char* key, bool def) {
    std::string s = readString(ini, section, key, "");
    for (char& ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (s.empty()) return def;
    if (s == "true" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "no" || s == "off") return false;
    return strtol(s.c_str(), nullptr, 0) != 0;
}

static long readInt(const std::wstring& ini, const char* section, const char* key, long def) {
    std::string s = readString(ini, section, key, "");
    return s.empty() ? def : strtol(s.c_str(), nullptr, 0);  // base 0 accepts 0x... hex
}

Config loadConfig(const std::wstring& ini) {
    Config c;
    c.autostart = readInt(ini, "general", "autostart", 1);
    c.autoStereo = readInt(ini, "general", "auto_stereo", 1) != 0;
    c.playInBody = readString(ini, "general", "play_mode", "body") != "stereo";
    c.mode = readString(ini, "general", "mode", "quad") == "stereo" ? Mode::Stereo : Mode::Quad;

    c.quadDistance = readFloat(ini, "quad", "distance", c.quadDistance);
    c.quadWidth = readFloat(ini, "quad", "width", c.quadWidth);

    c.worldScale = readFloat(ini, "stereo", "world_scale", c.worldScale);

    c.fovOverride = readInt(ini, "stereo", "fov_override", 1) != 0;
    c.exactFov = readBool(ini, "stereo", "exact_fov", c.exactFov);
    c.sharpen = readFloat(ini, "stereo", "sharpen", c.sharpen);
    if (!(c.sharpen >= 0 && c.sharpen <= 1)) c.sharpen = 0.6f;
    c.turnReprojection = readBool(ini, "stereo", "turn_reprojection", c.turnReprojection);
    c.turnDisplayClock = readBool(ini, "stereo", "turn_display_clock", c.turnDisplayClock);
    c.walkDisplayClock = readBool(ini, "stereo", "walk_display_clock", c.walkDisplayClock);
    c.turnLead = readBool(ini, "stereo", "turn_lead", c.turnLead);
    c.gpuYield = readBool(ini, "stereo", "gpu_yield", c.gpuYield);
    c.pairedEyes = readBool(ini, "stereo", "paired_eyes", c.pairedEyes);
    c.pairedHands = readBool(ini, "stereo", "paired_hands", c.pairedHands);
    c.bodyLayer = readBool(ini, "stereo", "body_layer", c.bodyLayer);
    c.alignEyes = readBool(ini, "stereo", "align_eyes", c.alignEyes);
    c.frameMap = readBool(ini, "stereo", "frame_map", c.frameMap);
    c.renderScale = readFloat(ini, "stereo", "render_scale", c.renderScale);
    if (!(c.renderScale >= 0 && c.renderScale <= 1.5f)) c.renderScale = 0.8f;
    c.renderMsaa = static_cast<int>(readInt(ini, "stereo", "render_msaa", c.renderMsaa));
    c.nearFade = readInt(ini, "stereo", "near_fade", 0) != 0;

    c.uiPanel = readInt(ini, "ui", "panel", 1) != 0;
    c.uiDistance = readFloat(ini, "ui", "distance", c.uiDistance);
    c.uiWidth = readFloat(ini, "ui", "width", c.uiWidth);
    c.uiHeight = readFloat(ini, "ui", "height", c.uiHeight);
    c.uiFirstPerson = readInt(ini, "ui", "first_person", c.uiFirstPerson);
    c.wristWidth = readFloat(ini, "ui", "wrist_width", c.wristWidth);
    c.pieAtHand = readInt(ini, "ui", "pie_at_hand", 1) != 0;
    c.selfPiePoke = readInt(ini, "ui", "self_pie_poke", 1) != 0;
    c.selfPiePalm = readInt(ini, "ui", "self_pie_palm", 1) != 0;
    c.pieDistance = readFloat(ini, "ui", "pie_distance", c.pieDistance);
    c.pieWidth = readFloat(ini, "ui", "pie_width", c.pieWidth);
    c.pieCrop = readFloat(ini, "ui", "pie_crop", c.pieCrop);
    c.objectMenus = readInt(ini, "ui", "object_menus", 1) != 0;
    c.objectMenuWidth = readFloat(ini, "ui", "object_menu_width", c.objectMenuWidth);
    c.bubbles3d = readInt(ini, "ui", "bubbles_3d", 1) != 0;
    c.bubbleOwnFront = readInt(ini, "bubbles", "own_in_front", 1) != 0;
    for (auto [key, out] : {std::pair<const char*, float*>{"own_bubble_position", c.bubbleOwnPos},
                            std::pair<const char*, float*>{"own_skill_position", c.skillOwnPos}}) {
        std::string s = readString(ini, "bubbles", key, "");
        float v[3];
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) out[i] = v[i];
    }
    c.bubbleOwnSize = readFloat(ini, "bubbles", "own_size", c.bubbleOwnSize);
    c.bubbleNpcSize = readFloat(ini, "bubbles", "npc_size", c.bubbleNpcSize);
    c.bubbleHideOwn = readInt(ini, "bubbles", "hide_own", 0) != 0;
    c.bubbleHideNpc = readInt(ini, "bubbles", "hide_npc", 0) != 0;
    c.popups3d = readInt(ini, "ui", "popups_3d", 1) != 0;
    c.popupDistance = readFloat(ini, "ui", "popup_distance", c.popupDistance);
    c.popupSize = readFloat(ini, "ui", "popup_size", c.popupSize);
    c.wristPieces = readInt(ini, "wrist", "hud", 1) != 0;
    c.noDimming = readInt(ini, "wrist", "no_dimming", 1) != 0;
    c.wristUiHeight = readFloat(ini, "wrist", "ui_height", c.wristUiHeight);
    c.wristScale = readFloat(ini, "wrist", "scale", c.wristScale);
    auto three = [&](const char* key, float* v) {
        float r[3];
        std::string s = readString(ini, "wrist", key, "");
        if (sscanf(s.c_str(), "%f %f %f", &r[0], &r[1], &r[2]) == 3)
            for (int i = 0; i < 3; ++i) v[i] = r[i];
    };
    auto arm = [&](const char* name, bool& on, float* pos, float* turn, float& around, float& size) {
        std::string n(name);
        on = readInt(ini, "wrist", name, 1) != 0;
        three((n + "_pos").c_str(), pos);
        three((n + "_turn").c_str(), turn);
        around = readFloat(ini, "wrist", (n + "_around").c_str(), around);
        size = readFloat(ini, "wrist", (n + "_size").c_str(), size);
    };
    arm("watch", c.wristTime, c.wristTimePos, c.wristTimeTurn, c.wristTimeAround, c.wristTimeSize);
    arm("build_button", c.wristBuild, c.wristBuildPos, c.wristBuildTurn, c.wristBuildAround, c.wristBuildSize);
    arm("actions", c.wristQueue, c.wristQueuePos, c.wristQueueTurn, c.wristQueueAround, c.wristQueueSize);
    arm("needs", c.wristNeeds, c.wristNeedsPos, c.wristNeedsTurn, c.wristNeedsAround, c.wristNeedsSize);
    arm("moodlets", c.wristMoodlets, c.wristMoodletsPos, c.wristMoodletsTurn, c.wristMoodletsAround,
        c.wristMoodletsSize);
    c.wristFullButton = readInt(ini, "wrist", "full_ui_button", 1) != 0;
    c.wristWhims = readInt(ini, "wrist", "wants", 1) != 0;
    three("wants_pos", c.wristWhimsPos);
    three("wants_turn", c.wristWhimsTurn);
    c.wristWhimsSize = readFloat(ini, "wrist", "wants_size", c.wristWhimsSize);
    c.wristMoney = readInt(ini, "wrist", "money", 1) != 0;
    three("money_pos", c.wristMoneyPos);
    three("money_turn", c.wristMoneyTurn);
    c.wristMoneySize = readFloat(ini, "wrist", "money_size", c.wristMoneySize);
    c.wristPause = readInt(ini, "wrist", "pause", 1) != 0;
    three("pause_pos", c.wristPausePos);
    three("pause_turn", c.wristPauseTurn);
    c.wristPauseSize = readFloat(ini, "wrist", "pause_size", c.wristPauseSize);
    c.wristSleepClock = readInt(ini, "wrist", "sleep_clock", 1) != 0;
    three("sleep_pos", c.wristSleepPos);
    three("sleep_turn", c.wristSleepTurn);
    c.wristSleepSize = readFloat(ini, "wrist", "sleep_size", c.wristSleepSize);
    if (!(c.pieCrop >= 0.2f && c.pieCrop <= 3.0f)) c.pieCrop = 0.75f;
    if (!(c.pieWidth >= 0.1f && c.pieWidth <= 1.5f)) c.pieWidth = 0.96f;
    if (!(c.pieDistance >= 0.2f && c.pieDistance <= 3.0f)) c.pieDistance = 0.45f;
    c.gamepad = readInt(ini, "controls", "gamepad", 0) != 0;
    c.turnSpeed = readFloat(ini, "controls", "turn_speed", c.turnSpeed);
    c.audioFollowsHead = readInt(ini, "audio", "follow_head", 1) != 0;
    c.audioPanAnyDistance = readInt(ini, "audio", "pan_any_distance", 1) != 0;
    c.mirrorInterior = readFloat(ini, "stereo", "mirror_far_inside", c.mirrorInterior);
    c.mirrorExterior = readFloat(ini, "stereo", "mirror_far_outside", c.mirrorExterior);
    c.mirrorClip = readInt(ini, "stereo", "mirror_clip", 1) != 0;
    c.reflectionsOff = readInt(ini, "stereo", "reflections_off", 0) != 0;
    c.outdoorFade = readFloat(ini, "audio", "outdoor_fade", c.outdoorFade);
    if (!(c.outdoorFade >= 0 && c.outdoorFade <= 20)) c.outdoorFade = 4.0f;
    c.phone = readBool(ini, "phone", "phone", true);
    c.phoneHipDrop = readFloat(ini, "phone", "hip_drop", c.phoneHipDrop);
    c.phonePopupBuzz = readBool(ini, "phone", "popup_buzz", c.phonePopupBuzz);
    c.phoneWidth = readFloat(ini, "phone", "phone_width", c.phoneWidth);
    if (!(c.phoneWidth >= 0.03f && c.phoneWidth <= 0.5f)) c.phoneWidth = 0.102f;
    c.phoneHeight = readFloat(ini, "phone", "phone_height", c.phoneHeight);
    if (!(c.phoneHeight >= 0.03f && c.phoneHeight <= 0.5f)) c.phoneHeight = 0.190f;
    c.phoneBezel = readFloat(ini, "phone", "bezel", c.phoneBezel);
    if (!(c.phoneBezel >= 0 && c.phoneBezel * 4 < std::min(c.phoneWidth, c.phoneHeight))) c.phoneBezel = 0.004f;
    c.phoneDepth = readFloat(ini, "phone", "phone_depth", c.phoneDepth);
    if (!(c.phoneDepth >= 0.002f && c.phoneDepth <= 0.05f)) c.phoneDepth = 0.009f;
    c.phoneCurl = readFloat(ini, "phone", "finger_curl", c.phoneCurl);
    if (!(c.phoneCurl >= 0 && c.phoneCurl <= 1)) c.phoneCurl = 0.8f;
    c.phoneThumbBend = readFloat(ini, "phone", "thumb_bend", c.phoneThumbBend);
    if (!(c.phoneThumbBend >= 0 && c.phoneThumbBend <= 1)) c.phoneThumbBend = 0.3f;
    c.phoneThumbSpread = readFloat(ini, "phone", "thumb_spread", c.phoneThumbSpread);
    if (!(c.phoneThumbSpread >= -90 && c.phoneThumbSpread <= 90)) c.phoneThumbSpread = 0;
    c.phoneProp = readBool(ini, "phone", "game_phone", false);
    {
        float v[3];
        std::string s = readString(ini, "phone", "hand_offset", "");
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.phoneHandOffset[i] = v[i] >= -0.2f && v[i] <= 0.2f ? v[i] : 0;
        s = readString(ini, "phone", "hand_turn", "");
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.phoneHandTurn[i] = v[i] >= -180 && v[i] <= 180 ? v[i] : 0;
    }
    {
        std::string s = readString(ini, "phone", "phone_offset", "");
        float v[3];
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.phoneOffset[i] = v[i];
    }
    c.phoneTilt = readFloat(ini, "phone", "tilt", c.phoneTilt);
    c.phoneYaw = readFloat(ini, "phone", "yaw", c.phoneYaw);
    c.phonePitch = readFloat(ini, "phone", "pitch", c.phonePitch);
    c.tablet = readBool(ini, "tablet", "tablet", true);
    c.tabletWidth = readFloat(ini, "tablet", "width", c.tabletWidth);
    if (!(c.tabletWidth >= 0.1f && c.tabletWidth <= 0.6f)) c.tabletWidth = 0.30f;
    c.tabletHeight = readFloat(ini, "tablet", "height", c.tabletHeight);
    if (!(c.tabletHeight == 0 || (c.tabletHeight >= 0.08f && c.tabletHeight <= 0.6f))) c.tabletHeight = 0;
    c.tabletDepth = readFloat(ini, "tablet", "depth", c.tabletDepth);
    if (!(c.tabletDepth >= 0.002f && c.tabletDepth <= 0.05f)) c.tabletDepth = 0.008f;
    c.tabletBezel = readFloat(ini, "tablet", "bezel", c.tabletBezel);
    if (!(c.tabletBezel >= 0 && c.tabletBezel * 8 < c.tabletWidth)) c.tabletBezel = 0.010f;
    c.tabletHold = readFloat(ini, "tablet", "hold", c.tabletHold);
    if (!(c.tabletHold >= 0 && c.tabletHold < c.tabletWidth / 2)) c.tabletHold = 0.035f;
    c.phoneCropWidth = readFloat(ini, "phone", "crop_width", c.phoneCropWidth);
    c.phoneCropHeight = readFloat(ini, "phone", "crop_height", c.phoneCropHeight);
    if (!(c.phoneCropWidth > 0.05f && c.phoneCropWidth <= 2.0f)) c.phoneCropWidth = 0.45f;
    if (!(c.phoneCropHeight > 0.05f && c.phoneCropHeight <= 1.0f)) c.phoneCropHeight = 0.80f;
    c.voiceMic = readBool(ini, "voice", "mic", false);
    c.voiceDevice = readString(ini, "voice", "mic_device", "");
    c.voiceGateDb = readFloat(ini, "voice", "mic_gate", c.voiceGateDb);
    c.voiceFullDb = readFloat(ini, "voice", "mic_full", c.voiceFullDb);
    c.voiceMouthOpen = readFloat(ini, "voice", "mouth_open", c.voiceMouthOpen);
    c.devProbe = readInt(ini, "dev", "probe", 0) != 0;

    c.bodyForward = readFloat(ini, "body", "forward", c.bodyForward);
    c.bodyHeight = readFloat(ini, "body", "height", c.bodyHeight);
    c.bodyEyeInset = readFloat(ini, "body", "eye_inset", c.bodyEyeInset);
    c.bodySmoothing = readFloat(ini, "body", "smoothing", c.bodySmoothing);
    c.bodySeatFollow = readInt(ini, "body", "seat_follow", 1) != 0;
    c.bodySeatTorso = readInt(ini, "body", "seat_torso", 1) != 0;
    c.bodyNearClip = readFloat(ini, "body", "near_clip", c.bodyNearClip);
    c.bodyNearAdaptive = readInt(ini, "body", "near_adaptive", 1) != 0;
    c.bodyHideFace = readInt(ini, "body", "hide_face", 1) != 0;
    c.bodyNearMax = readFloat(ini, "body", "near_max", c.bodyNearMax);
    c.steadyShadows = readInt(ini, "body", "steady_shadows", 1) != 0;
    c.shadowStep = readFloat(ini, "body", "shadow_step", c.shadowStep);
    c.shadowSharp = readFloat(ini, "body", "shadow_sharp", c.shadowSharp);
    c.shadowMap = static_cast<int>(readInt(ini, "stereo", "shadow_map", c.shadowMap));
    c.bodyTurnRate = readFloat(ini, "body", "turn_rate", c.bodyTurnRate);
    c.walkSpeed = readFloat(ini, "body", "walk_speed", c.walkSpeed);
    c.runSpeed = readFloat(ini, "body", "run_speed", c.runSpeed);
    c.bodyKeepHeight = readInt(ini, "body", "keep_height", 1) != 0;
    c.bodySmoothSlide = readInt(ini, "body", "smooth_slide", 1) != 0;
    c.bodyStopTime = readFloat(ini, "body", "stop_time", c.bodyStopTime);
    c.bodyWalkClips = readInt(ini, "body", "walk_clips", 1) != 0;
    c.armIk = readInt(ini, "body", "arm_ik", 1) != 0;
    c.handPitch = readFloat(ini, "body", "hand_pitch", c.handPitch);
    c.handTracking = readInt(ini, "body", "hand_tracking", 1) != 0;
    c.touch = readInt(ini, "body", "touch", 1) != 0;
    c.touchPast = readFloat(ini, "body", "touch_past", c.touchPast);
    if (!(c.touchPast >= -0.03f && c.touchPast <= 0.1f)) c.touchPast = 0.015f;
    c.headTracking = readInt(ini, "body", "head_tracking", 1) != 0;
    c.touchBodies = readInt(ini, "physical", "bodies", 1) != 0;
    c.holdHands = readInt(ini, "physical", "hold_hands", 1) != 0;
    c.lookAtYou = readInt(ini, "physical", "look_at_you", 1) != 0;
    c.headRepel = readInt(ini, "physical", "head", 1) != 0;
    c.headRadius = readFloat(ini, "physical", "head_radius", 0.08f);
    c.grabBodies = readInt(ini, "physical", "grab_bodies", 1) != 0;
    c.grabCurl = readFloat(ini, "physical", "grab_curl", c.grabCurl);
    c.grabDrag = readFloat(ini, "physical", "grab_drag", c.grabDrag);
    c.touchHaptics = readInt(ini, "physical", "haptics", 1) != 0;
    c.grab = readInt(ini, "hands", "grab", 1) != 0;
    c.bodyTurnSmoothing = readFloat(ini, "body", "turn_smoothing", c.bodyTurnSmoothing);
    if (!(c.bodyTurnSmoothing >= 0 && c.bodyTurnSmoothing <= 1.0f)) c.bodyTurnSmoothing = 0.1f;
    if (!(c.bodySmoothing > 0.01f && c.bodySmoothing <= 1.0f)) c.bodySmoothing = 0.3f;
    return c;
}

}  // namespace s4vr
