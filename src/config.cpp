#include "config.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
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
    // v3 (user 2026-10-20 18:32Z, one mode): both real eyes every frame at render scale 0.65. Before: the smoothest setup (16:45Z: "whatever option will make the game
    // run smoothest"): the depth eye on, the eyes not taking turns, no test view, the alignment as measured.
    // Once: the menu's saved values for these keys are taken out of sims4vr_user.ini so sims4vr.ini's apply.
    {
        const std::wstring user = userIniPath(ini);
        std::string path(user.begin(), user.end());
        std::string done;
        if (!readFrom(user, "stereo", "smooth_defaults", done) || atoi(done.c_str()) < 3) {
            for (const char* key : {"depth_eye", "depth_eye_alternate", "depth_eye_debug", "depth_eye_present_depth",
                                    "flat_world", "both_eyes", "render_scale", "made_eye_shift_x", "made_eye_shift_y", "made_eye_distance", "made_eye_depth",
                                    "made_eye_submit_x", "made_eye_submit_yaw", "made_eye_submit_pitch"})
                WritePrivateProfileStringA("stereo", key, nullptr, path.c_str());
            WritePrivateProfileStringA("stereo", "smooth_defaults", "3", path.c_str());
            WritePrivateProfileStringA(nullptr, nullptr, nullptr, path.c_str());  // flush
        }
    }
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
    c.upscale = readBool(ini, "stereo", "upscale", c.upscale);
    c.fsrRenderScale = readFloat(ini, "stereo", "fsr_render_scale", c.fsrRenderScale);
    if (!(c.fsrRenderScale >= 0.3f && c.fsrRenderScale <= 1.5f)) c.fsrRenderScale = 0.67f;
    c.fsrOutputScale = readFloat(ini, "stereo", "fsr_output_scale", c.fsrOutputScale);
    if (!(c.fsrOutputScale >= 0.3f && c.fsrOutputScale <= 2.0f)) c.fsrOutputScale = 1.0f;
    c.turnReprojection = readBool(ini, "stereo", "turn_reprojection", c.turnReprojection);
    c.latePose = readBool(ini, "stereo", "late_pose", c.latePose);
    c.gpuWait = readBool(ini, "stereo", "gpu_wait", c.gpuWait);
    c.capLead = readBool(ini, "stereo", "cap_lead", c.capLead);
    c.turnDisplayClock = readBool(ini, "stereo", "turn_display_clock", c.turnDisplayClock);
    c.walkDisplayClock = readBool(ini, "stereo", "walk_display_clock", c.walkDisplayClock);
    c.turnLead = readBool(ini, "stereo", "turn_lead", c.turnLead);
    c.turnLeadScale = std::clamp(readFloat(ini, "stereo", "turn_lead_scale", c.turnLeadScale), 0.f, 2.f);
    c.turnEase = std::clamp(readFloat(ini, "body", "turn_ease", c.turnEase), 0.f, 1.f);
    c.gpuYield = readBool(ini, "stereo", "gpu_yield", c.gpuYield);
    c.pairedEyes = readBool(ini, "stereo", "paired_eyes", c.pairedEyes);
    c.eyeWaitDrawn = readBool(ini, "stereo", "eye_wait_drawn", c.eyeWaitDrawn);
    c.photoLens = readBool(ini, "photo", "lens", c.photoLens);
    c.eyeWaitMs = std::clamp(readFloat(ini, "stereo", "eye_wait_ms", c.eyeWaitMs), 20.f, 1000.f);
    c.pairedHands = readBool(ini, "stereo", "paired_hands", c.pairedHands);
    c.bodyLayer = readBool(ini, "stereo", "body_layer", c.bodyLayer);
    c.alignEyes = readBool(ini, "stereo", "align_eyes", c.alignEyes);
    c.frameMap = readBool(ini, "stereo", "frame_map", c.frameMap);
    c.probeDouble = readBool(ini, "stereo", "probe_double", c.probeDouble);
    c.depthEye = readBool(ini, "stereo", "depth_eye", c.depthEye);
    c.depthEyeDebug = static_cast<int>(readInt(ini, "stereo", "depth_eye_debug", c.depthEyeDebug));
    c.depthEyeAlternate = readBool(ini, "stereo", "depth_eye_alternate", c.depthEyeAlternate);
    c.flatWorld = readBool(ini, "stereo", "flat_world", c.flatWorld);
    c.bothEyes = readBool(ini, "stereo", "both_eyes", c.bothEyes);
    // both_eyes_skip is a live test: every start begins with it off (a saved 4 was "Frame begin",
    // which crashed in 317; 1-3 leave a scene part out of the right eye).
    c.bothEyesSkip = 0;
    c.pairShadows = readBool(ini, "stereo", "pair_shadows", c.pairShadows);
    c.reuseShadows = readBool(ini, "stereo", "reuse_shadows", c.reuseShadows);
    c.skipShadowLists = readBool(ini, "stereo", "skip_shadow_prep", c.skipShadowLists);
    c.frameClock = readBool(ini, "stereo", "frame_clock", c.frameClock);
    c.headsetWidth = readBool(ini, "stereo", "headset_width", c.headsetWidth);
    c.textureBias = readFloat(ini, "stereo", "texture_bias", c.textureBias);
    if (!(c.textureBias >= -3 && c.textureBias <= 0)) c.textureBias = 0;
    c.depthEyePresentDepth = readBool(ini, "stereo", "depth_eye_present_depth", c.depthEyePresentDepth);
    c.madeEyeShiftX = readFloat(ini, "stereo", "made_eye_shift_x", c.madeEyeShiftX);
    c.madeEyeShiftY = readFloat(ini, "stereo", "made_eye_shift_y", c.madeEyeShiftY);
    c.madeEyeDistance = readFloat(ini, "stereo", "made_eye_distance", c.madeEyeDistance);
    c.madeEyeDepth = readFloat(ini, "stereo", "made_eye_depth", c.madeEyeDepth);
    c.madeEyeSubmitX = readFloat(ini, "stereo", "made_eye_submit_x", c.madeEyeSubmitX);
    c.madeEyeSubmitYaw = readFloat(ini, "stereo", "made_eye_submit_yaw", c.madeEyeSubmitYaw);
    c.madeEyeSubmitPitch = readFloat(ini, "stereo", "made_eye_submit_pitch", c.madeEyeSubmitPitch);
    c.renderScale = readFloat(ini, "stereo", "render_scale", c.renderScale);
    if (!(c.renderScale >= 0 && c.renderScale <= 1.5f)) c.renderScale = 0.75f;
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
    c.pieMode = readInt(ini, "ui", "pie_mode", c.pieMode);
    c.scandalHide = readInt(ini, "bubbles", "scandal_hide", 0) != 0;
    c.piePalmSize = readFloat(ini, "ui", "pie_palm_size", c.piePalmSize);
    c.pieTvFront = readInt(ini, "ui", "pie_tv", 1) != 0;
    c.pieTvSize = readFloat(ini, "ui", "pie_tv_size", c.pieTvSize);
    c.pieTvOffset = readFloat(ini, "ui", "pie_tv_offset", c.pieTvOffset);
    c.piePalmLift = readFloat(ini, "ui", "pie_palm_lift", c.piePalmLift);
    c.pieCrop = readFloat(ini, "ui", "pie_crop", c.pieCrop);
    c.objectMenus = readInt(ini, "ui", "object_menus", 1) != 0;
    c.objectMenuWidth = readFloat(ini, "ui", "object_menu_width", c.objectMenuWidth);
    c.bubbles3d = readInt(ini, "ui", "bubbles_3d", 1) != 0;
    c.bubbleOwnFront = readInt(ini, "bubbles", "own_in_front", 1) != 0;
    c.skillOwnSize = readFloat(ini, "bubbles", "skill_size", c.skillOwnSize);
    for (auto [key, out] : {std::pair<const char*, float*>{"own_bubble_position", c.bubbleOwnPos},
                            std::pair<const char*, float*>{"skill_position", c.skillOwnPos},
                            std::pair<const char*, float*>{"pop_position", c.popOwnPos},
                            std::pair<const char*, float*>{"scandal_position", c.scandalPos}}) {
        std::string s = readString(ini, "bubbles", key, "");
        float v[3];
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) out[i] = v[i];
    }
    {
        std::string s = readString(ini, "phone", "note_place", "");
        float v[3];
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.notePlace[i] = v[i];
    }
    c.noteSize = readFloat(ini, "phone", "note_size", c.noteSize);
    c.bubbleOwnSize = readFloat(ini, "bubbles", "own_size", c.bubbleOwnSize);
    c.popOwnSize = readFloat(ini, "bubbles", "pop_size", c.popOwnSize);
    c.popHideOwn = readInt(ini, "bubbles", "hide_pops", 0) != 0;
    c.bubbleNpcSize = readFloat(ini, "bubbles", "npc_size", c.bubbleNpcSize);
    c.bubbleHideOwn = readInt(ini, "bubbles", "hide_own", 0) != 0;
    c.bubbleHideNpc = readInt(ini, "bubbles", "hide_npc", 0) != 0;
    c.bubbleCutOthers = readInt(ini, "bubbles", "cut_others", 0) != 0;
    c.bubbleOwnDraw = readInt(ini, "bubbles", "own_balloons", 1) != 0;
    c.popups3d = readInt(ini, "ui", "popups_3d", 1) != 0;
    c.popupDistance = readFloat(ini, "ui", "popup_distance", c.popupDistance);
    c.popupSize = readFloat(ini, "ui", "popup_size", c.popupSize);
    c.keyboard = readInt(ini, "ui", "keyboard", 1) != 0;
    c.keyboardSize = readFloat(ini, "ui", "keyboard_size", c.keyboardSize);
    c.keyboardDistance = readFloat(ini, "ui", "keyboard_distance", c.keyboardDistance);
    c.keyboardDrop = readFloat(ini, "ui", "keyboard_drop", c.keyboardDrop);
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
    c.wristWork = readInt(ini, "wrist", "work_card", 1) != 0;
    three("work_card_pos", c.wristWorkPos);
    three("work_card_turn", c.wristWorkTurn);
    c.wristWorkSize = readFloat(ini, "wrist", "work_card_size", c.wristWorkSize);
    c.wristLotIntro = readInt(ini, "wrist", "lot_intro", 1) != 0;
    three("lot_intro_pos", c.wristLotPos);
    three("lot_intro_turn", c.wristLotTurn);
    c.wristLotSize = readFloat(ini, "wrist", "lot_intro_size", c.wristLotSize);
    c.wristLotCropAuto = readInt(ini, "wrist", "lot_intro_crop_auto", 1) != 0;
    c.wristLotBannerW = readFloat(ini, "wrist", "lot_intro_banner_w", c.wristLotBannerW);
    c.wristLotBannerY = readFloat(ini, "wrist", "lot_intro_banner_y", c.wristLotBannerY);
    if (!(c.wristLotBannerW >= 200 && c.wristLotBannerW <= 2000)) c.wristLotBannerW = 1000;
    if (!(c.wristLotBannerY >= -400 && c.wristLotBannerY <= 400)) c.wristLotBannerY = 0;
    c.wristLotCropW = readFloat(ini, "wrist", "lot_intro_crop_w", c.wristLotCropW);
    c.wristLotCropH = readFloat(ini, "wrist", "lot_intro_crop_h", c.wristLotCropH);
    c.wristLotCropY = readFloat(ini, "wrist", "lot_intro_crop_y", c.wristLotCropY);
    if (!(c.wristLotCropW >= 0.1f && c.wristLotCropW <= 1)) c.wristLotCropW = 0.42f;
    if (!(c.wristLotCropH >= 0.05f && c.wristLotCropH <= 1)) c.wristLotCropH = 0.26f;
    if (!(c.wristLotCropY >= -0.5f && c.wristLotCropY <= 0.5f)) c.wristLotCropY = 0;
    if (!(c.pieCrop >= 0.2f && c.pieCrop <= 3.0f)) c.pieCrop = 0.75f;
    if (!(c.pieWidth >= 0.1f && c.pieWidth <= 1.5f)) c.pieWidth = 0.96f;
    if (c.pieMode < 0 || c.pieMode > 2) c.pieMode = 0;
    if (!(c.piePalmSize >= 0.15f && c.piePalmSize <= 1.0f)) c.piePalmSize = 0.45f;
    if (!(c.pieTvSize >= 0.5f && c.pieTvSize <= 6.0f)) c.pieTvSize = 3.0f;
    if (!(c.pieTvOffset >= 0.0f && c.pieTvOffset <= 2.0f)) c.pieTvOffset = 0.30f;
    if (!(c.piePalmLift >= 0.0f && c.piePalmLift <= 0.4f)) c.piePalmLift = 0.10f;
    if (!(c.pieDistance >= 0.2f && c.pieDistance <= 3.0f)) c.pieDistance = 0.45f;
    c.gamepad = readInt(ini, "controls", "gamepad", 0) != 0;
    c.moveMode = readInt(ini, "controls", "move_mode", c.moveMode);
    c.teleStick = readInt(ini, "teleport", "stick", c.teleStick);
    c.teleStyle = readInt(ini, "teleport", "style", c.teleStyle);
    c.teleDistance = readFloat(ini, "teleport", "max_distance", c.teleDistance);
    c.teleBlink = readBool(ini, "teleport", "blink", c.teleBlink);
    c.teleBlinkTime = readFloat(ini, "teleport", "blink_seconds", c.teleBlinkTime);
    c.teleMaxRise = readFloat(ini, "teleport", "max_rise", c.teleMaxRise);
    c.teleMarker = readBool(ini, "teleport", "marker", c.teleMarker);
    c.snapTurn = readBool(ini, "controls", "snap_turn", c.snapTurn);
    c.snapAngle = readFloat(ini, "controls", "snap_angle", c.snapAngle);
    c.snapAngleBig = readFloat(ini, "controls", "snap_angle_big", c.snapAngleBig);
    c.recenterTurnsBody = readBool(ini, "controls", "recenter_turns_body", c.recenterTurnsBody);
    c.doorSlide = readFloat(ini, "body", "door_slide", c.doorSlide);
    c.fridgeDoorPull = readFloat(ini, "body", "fridge_door_pull", c.fridgeDoorPull);
    c.doorHandleReach = readFloat(ini, "body", "door_handle_reach", c.doorHandleReach);
    c.doorPushSpeed = readFloat(ini, "body", "door_push_speed", c.doorPushSpeed);
    c.trailArrive = readFloat(ini, "body", "trail_arrive", c.trailArrive);
    c.trailStopDist = readFloat(ini, "body", "trail_stop_dist", c.trailStopDist);
    c.trailArriveContact = readFloat(ini, "body", "trail_arrive_contact", c.trailArriveContact);
    c.turnSpeed = readFloat(ini, "controls", "turn_speed", c.turnSpeed);
    c.audioFollowsHead = readInt(ini, "audio", "follow_head", 1) != 0;
    c.audioPanAnyDistance = readInt(ini, "audio", "pan_any_distance", 1) != 0;
    c.audioFlatTo3D = std::clamp(static_cast<int>(readInt(ini, "audio", "flat_to_3d", 1)), 0, 2);
    c.audioFlatIds = readString(ini, "audio", "flat_to_3d_ids", "");
    c.audioFlatGraph = std::strtoull(readString(ini, "audio", "flat_to_3d_graph", "e29d705fca6cd456").c_str(), nullptr, 16);
    c.audioVoiceNear = readFloat(ini, "audio", "voice_near", c.audioVoiceNear);
    c.audioVoiceFar = readFloat(ini, "audio", "voice_far", c.audioVoiceFar);
    c.mirrorInterior = readFloat(ini, "stereo", "mirror_far_inside", c.mirrorInterior);
    c.mirrorExterior = readFloat(ini, "stereo", "mirror_far_outside", c.mirrorExterior);
    c.monitorEye = std::clamp(static_cast<int>(readInt(ini, "stereo", "monitor_eye", 0)), 0, 1);
    c.monitorZoom = std::clamp(readFloat(ini, "stereo", "monitor_zoom", 1.4f), 1.0f, 3.0f);
    c.monitorFit = std::clamp(static_cast<int>(readInt(ini, "stereo", "monitor_fit", 1)), 0, 2);
    c.mirrorClip = readInt(ini, "stereo", "mirror_clip", 1) != 0;
    c.waterClip = readInt(ini, "stereo", "water_clip", 1) != 0;
    c.waterOnePlane = readInt(ini, "stereo", "water_one_plane", 1) != 0;
    c.reflectionsOff = readInt(ini, "stereo", "reflections_off", 0) != 0;
    c.ambientOcclusion = readBool(ini, "stereo", "ambient_occlusion", c.ambientOcclusion);
    if (readBool(ini, "stereo", "ssao_off", false)) c.ambientOcclusion = false;  // the key of v0.13.279
    c.textureProbe = readBool(ini, "perf", "texture_probe", c.textureProbe);
    c.tryUnknownBuild = readBool(ini, "patch", "try_unknown_build", c.tryUnknownBuild);
    c.simCacheBoost = readBool(ini, "perf", "sim_cache_boost", c.simCacheBoost);
    c.simCacheMb = static_cast<int>(readFloat(ini, "perf", "sim_cache_mb", static_cast<float>(c.simCacheMb)));
    c.simLodScale = readFloat(ini, "perf", "sim_lod_scale", c.simLodScale);
    c.eyeSimCull = readFloat(ini, "perf", "far_sim_cull_eyes", c.eyeSimCull);
    c.gpuTimeline = readBool(ini, "perf", "gpu_timeline", c.gpuTimeline);
    c.farSimCull = readFloat(ini, "perf", "far_sim_cull", c.farSimCull);
    c.nearOncePerPair = readBool(ini, "perf", "near_once_per_pair", c.nearOncePerPair);
    c.hitchCatcher = readBool(ini, "perf", "hitch_catcher", c.hitchCatcher);
    c.outdoorFade = readFloat(ini, "audio", "outdoor_fade", c.outdoorFade);
    if (!(c.outdoorFade >= 0 && c.outdoorFade <= 20)) c.outdoorFade = 4.0f;
    c.outdoorBasement = readFloat(ini, "audio", "outdoor_basement", c.outdoorBasement);
    if (!(c.outdoorBasement >= 0 && c.outdoorBasement <= 1)) c.outdoorBasement = 0.4f;
    c.outdoorFloor = readFloat(ini, "audio", "outdoor_floor", c.outdoorFloor);
    if (!(c.outdoorFloor >= 0 && c.outdoorFloor <= 1)) c.outdoorFloor = 0.15f;
    c.phone = readBool(ini, "phone", "phone", true);
    c.phoneSlot = (int)std::clamp<long>(readInt(ini, "slots", "phone", c.phoneSlot), 0, 10);
    c.tabletSlot = (int)std::clamp<long>(readInt(ini, "slots", "tablet", c.tabletSlot), 0, 10);
    c.bookSlot = (int)std::clamp<long>(readInt(ini, "slots", "book", c.bookSlot), 0, 10);
    c.slotThisPhone = (int)std::clamp<long>(readInt(ini, "slots", "this_phone", 0), 0, 11);
    c.slotThisTablet = (int)std::clamp<long>(readInt(ini, "slots", "this_tablet", 0), 0, 11);
    c.slotThisBook = (int)std::clamp<long>(readInt(ini, "slots", "this_book", 0), 0, 11);
    c.slotRadius = std::max(0.05f, std::min(0.4f, readFloat(ini, "slots", "radius", c.slotRadius)));
    c.phoneHipDrop = readFloat(ini, "phone", "hip_drop", c.phoneHipDrop);
    c.flashBrightness = std::max(0.0f, std::min(100.0f, readFloat(ini, "phone", "flash_brightness", c.flashBrightness)));
    c.flashFill = std::max(0.0f, std::min(0.3f, readFloat(ini, "phone", "flash_fill", c.flashFill)));
    c.flashRange = std::max(1.0f, std::min(40.0f, readFloat(ini, "phone", "flash_range", c.flashRange)));
    c.flashInShader = readBool(ini, "phone", "flash_in_shader", c.flashInShader);
    c.flashCone = std::max(10.0f, std::min(120.0f, readFloat(ini, "phone", "flash_cone", c.flashCone)));
    c.phonePass = readBool(ini, "phone", "pass", c.phonePass);
    c.phonePassReach = std::max(0.08f, std::min(0.5f, readFloat(ini, "phone", "pass_reach", c.phonePassReach)));
    c.phonePopupBuzz = readBool(ini, "phone", "popup_buzz", c.phonePopupBuzz);
    c.phoneLock = readBool(ini, "phone", "lock", c.phoneLock);
    c.phoneLockGrace = std::max(0.0f, std::min(300.0f, readFloat(ini, "phone", "lock_grace", c.phoneLockGrace)));
    c.phoneStickScroll = std::clamp(readFloat(ini, "phone", "stick_scroll", c.phoneStickScroll), 0.f, 1500.f);
    c.phoneStickDeadzone = std::clamp(readFloat(ini, "phone", "stick_deadzone", c.phoneStickDeadzone), 0.05f, 0.9f);
    c.phoneSwipeStart = std::clamp(readFloat(ini, "phone", "swipe_start", c.phoneSwipeStart), 1.f, 20.f);
    c.phoneSwipeSpeed = std::clamp(readFloat(ini, "phone", "swipe_speed", c.phoneSwipeSpeed), 0.3f, 3.f);
    c.phoneTextSize = readInt(ini, "phone", "text_size", c.phoneTextSize);
    c.phoneSoundMode = readInt(ini, "phone", "sound_mode", c.phoneSoundMode);
    c.phoneRingtone = readInt(ini, "phone", "ringtone_file", c.phoneRingtone);
    c.phoneTextTone = readInt(ini, "phone", "text_tone_file", c.phoneTextTone);
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
    c.phoneProp = readBool(ini, "phone", "game_phone", true);
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
        // Sims Forever on the phone: each hand on it.
        auto triple = [&](const char* key, float out[3], float lim) {
            float v[3];
            std::string s = readString(ini, "simsforever", key, "");
            if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
                for (int i = 0; i < 3; ++i) out[i] = v[i] >= -lim && v[i] <= lim ? v[i] : 0;
        };
        triple("phone_hand_offset_left", c.sfHandOffsetL, 0.4f);
        triple("phone_hand_offset_right", c.sfHandOffsetR, 0.4f);
        triple("phone_hand_turn_left", c.sfHandTurnL, 180);
        triple("phone_hand_turn_right", c.sfHandTurnR, 180);
        auto unit = [&](const char* key, float& out, float lo, float hi) {
            float v = readFloat(ini, "simsforever", key, out);
            if (v >= lo && v <= hi) out = v;
        };
        unit("phone_finger_curl_left", c.sfCurlL, 0, 1);
        unit("phone_finger_curl_right", c.sfCurlR, 0, 1);
        unit("phone_thumb_bend_left", c.sfThumbBendL, 0, 1);
        unit("phone_thumb_bend_right", c.sfThumbBendR, 0, 1);
        unit("phone_thumb_spread_left", c.sfThumbSpreadL, -90, 90);
        unit("phone_thumb_spread_right", c.sfThumbSpreadR, -90, 90);
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
    c.book = readBool(ini, "book", "book", c.book);
    c.bookHipDrop = readFloat(ini, "book", "hip_drop", c.bookHipDrop);
    c.bookWidth = readFloat(ini, "book", "book_width", c.bookWidth);
    if (!(c.bookWidth >= 0.08f && c.bookWidth <= 0.8f)) c.bookWidth = 0.26f;
    c.bookTilt = readFloat(ini, "book", "tilt", c.bookTilt);
    c.bookYaw = readFloat(ini, "book", "yaw", c.bookYaw);
    c.bookPitch = readFloat(ini, "book", "pitch", c.bookPitch);
    c.bookCurl = readFloat(ini, "book", "finger_curl", c.bookCurl);
    if (!(c.bookCurl >= 0 && c.bookCurl <= 1)) c.bookCurl = 0.5f;
    c.bookThumbBend = readFloat(ini, "book", "thumb_bend", c.bookThumbBend);
    if (!(c.bookThumbBend >= 0 && c.bookThumbBend <= 1)) c.bookThumbBend = 0.3f;
    c.bookThumbSpread = readFloat(ini, "book", "thumb_spread", c.bookThumbSpread);
    if (!(c.bookThumbSpread >= -90 && c.bookThumbSpread <= 90)) c.bookThumbSpread = 0;
    auto crop = [&](const char* key, float& v, float def) {
        v = readFloat(ini, "book", key, def);
        if (!(v >= 0 && v <= 0.3f)) v = def;
    };
    crop("crop_outer", c.bookCropOuter, 0.07f);
    crop("crop_spine", c.bookCropSpine, 0.04f);
    crop("crop_top", c.bookCropTop, 0.02f);
    crop("crop_bottom", c.bookCropBottom, 0.05f);
    c.bookSound = readBool(ini, "book", "sound", c.bookSound);
    c.bookPoke = readBool(ini, "book", "poke", c.bookPoke);
    c.bookOpenSeconds = readFloat(ini, "book", "open_seconds", c.bookOpenSeconds);
    if (!(c.bookOpenSeconds >= 0.1f && c.bookOpenSeconds <= 3)) c.bookOpenSeconds = 0.7f;
    c.bookSwipe = readFloat(ini, "book", "swipe", c.bookSwipe);
    if (!(c.bookSwipe >= 0.03f && c.bookSwipe <= 0.4f)) c.bookSwipe = 0.10f;
    {
        float v[3];
        std::string s = readString(ini, "book", "book_offset", "");
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.bookOffset[i] = v[i];
        s = readString(ini, "book", "hand_offset", "");
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.bookHandOffset[i] = v[i] >= -0.2f && v[i] <= 0.2f ? v[i] : 0;
        s = readString(ini, "book", "hand_turn", "");
        if (sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3)
            for (int i = 0; i < 3; ++i) c.bookHandTurn[i] = v[i] >= -180 && v[i] <= 180 ? v[i] : 0;
    }
    c.tablet = readBool(ini, "tablet", "tablet", true);
    c.tabletWidth = readFloat(ini, "tablet", "width", c.tabletWidth);
    if (!(c.tabletWidth >= 0.1f && c.tabletWidth <= 0.6f)) c.tabletWidth = 0.30f;
    c.tabletHeight = readFloat(ini, "tablet", "height", c.tabletHeight);
    if (!(c.tabletHeight == 0 || (c.tabletHeight >= 0.08f && c.tabletHeight <= 0.6f))) c.tabletHeight = 0;
    c.tabletDepth = readFloat(ini, "tablet", "depth", c.tabletDepth);
    if (!(c.tabletDepth >= 0.002f && c.tabletDepth <= 0.05f)) c.tabletDepth = 0.008f;
    c.tabletBezel = readFloat(ini, "tablet", "bezel", c.tabletBezel);
    if (!(c.tabletBezel >= 0 && c.tabletBezel * 8 < c.tabletWidth)) c.tabletBezel = 0.010f;
    c.tabletSteady = std::max(0.0f, std::min(1.0f, readFloat(ini, "tablet", "steady", c.tabletSteady)));
    c.tabletClickHover = std::clamp(readFloat(ini, "tablet", "click_hover", c.tabletClickHover), 0.0f, 600.0f);
    c.tabletClickPress = std::clamp(readFloat(ini, "tablet", "click_press", c.tabletClickPress), 0.0f, 1000.0f);
    c.tabletPointer = std::max(0.0f, std::min(1.0f, readFloat(ini, "tablet", "pointer_steady", c.tabletPointer)));
    c.tabletHold = readFloat(ini, "tablet", "hold", c.tabletHold);
    if (!(c.tabletHold >= 0 && c.tabletHold < c.tabletWidth / 2)) c.tabletHold = 0.035f;
    c.tabletUiScale = readFloat(ini, "tablet", "menu_size", c.tabletUiScale);
    if (!(c.tabletUiScale >= 0.3f && c.tabletUiScale <= 4.0f)) c.tabletUiScale = 2.0f;
    c.tabletTrayScale = readFloat(ini, "tablet", "tray_scale", c.tabletTrayScale);
    if (!(c.tabletTrayScale >= 0.3f && c.tabletTrayScale <= 1.45f)) c.tabletTrayScale = 1.0f;
    c.buildEyes = readBool(ini, "buildbuy", "eyes", true);
    c.buildEyesFrames = readFloat(ini, "buildbuy", "eyes_frames", c.buildEyesFrames);
    if (!(c.buildEyesFrames >= 1 && c.buildEyesFrames <= 600)) c.buildEyesFrames = 45;
    c.buildEyesFrameMs = readFloat(ini, "buildbuy", "eyes_frame_ms", c.buildEyesFrameMs);
    if (!(c.buildEyesFrameMs >= 5 && c.buildEyesFrameMs <= 200)) c.buildEyesFrameMs = 30;
    c.buildEyesMaxWait = readFloat(ini, "buildbuy", "eyes_max_wait", c.buildEyesMaxWait);
    if (!(c.buildEyesMaxWait >= 0.5f && c.buildEyesMaxWait <= 30)) c.buildEyesMaxWait = 6;
    c.phoneCropWidth = readFloat(ini, "phone", "crop_width", c.phoneCropWidth);
    c.phoneCropHeight = readFloat(ini, "phone", "crop_height", c.phoneCropHeight);
    if (!(c.phoneCropWidth > 0.05f && c.phoneCropWidth <= 2.0f)) c.phoneCropWidth = 0.45f;
    if (!(c.phoneCropHeight > 0.05f && c.phoneCropHeight <= 1.0f)) c.phoneCropHeight = 0.80f;
    c.voiceMic = readBool(ini, "voice", "mic", false);
    c.voiceDevice = readString(ini, "voice", "mic_device", "");
    c.voiceGateDb = readFloat(ini, "voice", "mic_gate", c.voiceGateDb);
    c.voiceFullDb = readFloat(ini, "voice", "mic_full", c.voiceFullDb);
    c.voiceMouthOpen = readFloat(ini, "voice", "mouth_open", c.voiceMouthOpen);
    c.casStereo = readInt(ini, "cas", "stereo", 1) != 0;
    c.casDistance = std::clamp(readFloat(ini, "cas", "distance", c.casDistance), 0.8f, 4.0f);
    c.casEyeHeight = std::clamp(readFloat(ini, "cas", "eye_height", c.casEyeHeight), 0.5f, 2.5f);
    c.casEyeSeparation = std::clamp(readFloat(ini, "cas", "eye_separation", c.casEyeSeparation), 0.0f, 2.0f);
    c.casBody = readInt(ini, "cas", "body", 1) != 0;
    c.casBodyEyes = readInt(ini, "cas", "body_eyes", 1) != 0;
    c.casBodyStill = readInt(ini, "cas", "body_still", 1) != 0;
    c.devProbe = readInt(ini, "dev", "probe", 0) != 0;

    c.bodyForward = readFloat(ini, "body", "forward", c.bodyForward);
    c.bodyHeight = readFloat(ini, "body", "height", c.bodyHeight);
    c.bodyEyeInset = readFloat(ini, "body", "eye_inset", c.bodyEyeInset);
    c.bodyHeadX = readFloat(ini, "body", "head_x", c.bodyHeadX);
    c.bodyHeadY = readFloat(ini, "body", "head_y", c.bodyHeadY);
    c.bodyHeadZ = readFloat(ini, "body", "head_z", c.bodyHeadZ);
    c.bodySmoothing = readFloat(ini, "body", "smoothing", c.bodySmoothing);
    c.bodySeatFollow = readInt(ini, "body", "seat_follow", 1) != 0;
    c.bodySeatBack = readFloat(ini, "body", "seat_back", c.bodySeatBack);
    c.bodySeatUp = readFloat(ini, "body", "seat_up", c.bodySeatUp);
    c.bodySeatHold = readFloat(ini, "body", "seat_hold", c.bodySeatHold);
    c.bodySeatStill = readFloat(ini, "body", "seat_still", c.bodySeatStill);
    if (!(c.bodySeatHold >= 0.02f && c.bodySeatHold <= 1)) c.bodySeatHold = 0.08f;
    if (!(c.bodySeatStill >= 0 && c.bodySeatStill <= 0.2f)) c.bodySeatStill = 0.03f;
    if (!(std::fabs(c.bodySeatBack) <= 1)) c.bodySeatBack = 0;
    if (!(std::fabs(c.bodySeatUp) <= 1)) c.bodySeatUp = 0;
    c.bodySeatTorso = readInt(ini, "body", "seat_torso", 1) != 0;
    c.bodyNearClip = readFloat(ini, "body", "near_clip", c.bodyNearClip);
    c.bodyNearAdaptive = readInt(ini, "body", "near_adaptive", 1) != 0;
    c.bodyHideFace = readInt(ini, "body", "hide_face", 1) != 0;
    c.bodyGodHands = readInt(ini, "body", "god_hands", 1) != 0;
    c.bodySteadyShoulders = readInt(ini, "body", "steady_shoulders", 1) != 0;
    c.bodyNearMax = readFloat(ini, "body", "near_max", c.bodyNearMax);
    c.steadyShadows = readInt(ini, "body", "steady_shadows", 1) != 0;
    c.shadowStep = readFloat(ini, "body", "shadow_step", c.shadowStep);
    c.shadowSharp = readFloat(ini, "body", "shadow_sharp", c.shadowSharp);
    c.shadowMap = readFloat(ini, "stereo", "shadow_map", c.shadowMap);
    c.bodyTurnRate = readFloat(ini, "body", "turn_rate", c.bodyTurnRate);
    c.walkSpeed = readFloat(ini, "body", "walk_speed", c.walkSpeed);
    c.runSpeed = readFloat(ini, "body", "run_speed", c.runSpeed);
    c.bodyLazyTurn = readInt(ini, "body", "turn_body_lazy", 1) != 0;
    c.bodyStandRate = std::clamp(readFloat(ini, "body", "turn_body_rate", c.bodyStandRate), 0.f, 60.f);
    c.bodyKeepHeight = readInt(ini, "body", "keep_height", 1) != 0;
    c.bodySmoothSlide = readInt(ini, "body", "smooth_slide", 1) != 0;
    c.bodyCollision = readInt(ini, "body", "collision", 1) != 0;
    c.bodyRadius = readFloat(ini, "body", "body_radius", c.bodyRadius);
    c.furnitureRadius = readFloat(ini, "body", "furniture_radius", c.furnitureRadius);
    c.handCollision = readInt(ini, "body", "hand_collision", 1) != 0;
    c.handWallGap = readFloat(ini, "body", "hand_wall_gap", c.handWallGap);
    c.handObjectGap = readFloat(ini, "body", "hand_object_gap", c.handObjectGap);
    c.softPlants = readInt(ini, "body", "soft_plants", 1) != 0;
    c.plantSlowdown = readFloat(ini, "body", "plant_slowdown", c.plantSlowdown);
    c.foliagePatch = readInt(ini, "foliage", "patch_shaders", 1) != 0;
    c.foliagePush = readInt(ini, "foliage", "push", 1) != 0;
    c.foliagePushCm = readFloat(ini, "foliage", "push_cm", c.foliagePushCm);
    c.foliageHeadRadius = readFloat(ini, "foliage", "head_radius", c.foliageHeadRadius);
    c.foliageHandRadius = readFloat(ini, "foliage", "hand_radius", c.foliageHandRadius);
    c.foliageBodyRadius = readFloat(ini, "foliage", "body_radius", c.foliageBodyRadius);
    c.foliageHead = readInt(ini, "foliage", "head", 1) != 0;
    c.foliageHands = readInt(ini, "foliage", "hands", 1) != 0;
    c.foliageBody = readInt(ini, "foliage", "body", 1) != 0;
    c.foliageSims = readInt(ini, "foliage", "sims", 1) != 0;
    c.foliageLiftCm = readFloat(ini, "foliage", "lift_cm", c.foliageLiftCm);
    c.stepOver = readInt(ini, "body", "step_over", 1) != 0;
    c.stepAll = readInt(ini, "body", "step_all", 0) != 0;
    c.stepHeight = readFloat(ini, "body", "step_height", c.stepHeight);
    c.crouch = readInt(ini, "body", "crouch", 1) != 0;
    c.crouchDepth = readFloat(ini, "body", "crouch_depth", c.crouchDepth);
    c.crouchSpeed = readFloat(ini, "body", "crouch_speed", c.crouchSpeed);
    c.crouchSeconds = readFloat(ini, "body", "crouch_seconds", c.crouchSeconds);
    c.bodyStopTime = readFloat(ini, "body", "stop_time", c.bodyStopTime);
    c.bodyWalkClips = readInt(ini, "body", "walk_clips", 1) != 0;
    c.bodyStopGlide = readInt(ini, "body", "stop_glide", 0) != 0;
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
    c.lookAtHand = readInt(ini, "physical", "look_at_hand", 1) != 0;
    c.lookSpeed = readFloat(ini, "physical", "look_speed", 5.0f);
    c.lookEyesWithin = readFloat(ini, "physical", "look_eyes_within", 0.3f);
    c.lookHeadFrom = readFloat(ini, "physical", "look_head_from", 0.8f);
    c.shooAngle = readFloat(ini, "gestures", "shoo_angle", 45.0f);
    c.shooFlicks = readFloat(ini, "gestures", "shoo_flicks", 2.0f);
    c.highFiveHold = readFloat(ini, "gestures", "high_five_hold", 2.5f);
    c.headRepel = readInt(ini, "physical", "head", 1) != 0;
    c.headRadius = readFloat(ini, "physical", "head_radius", 0.08f);
    c.grabBodies = readInt(ini, "physical", "grab_bodies", 1) != 0;
    c.grabCurl = readFloat(ini, "physical", "grab_curl", c.grabCurl);
    c.breastCurl = readFloat(ini, "physical", "breast_grab_curl", c.breastCurl);
    c.buttCurl = readFloat(ini, "physical", "butt_grab_curl", c.buttCurl);
    c.grabDrag = readFloat(ini, "physical", "grab_drag", c.grabDrag);
    c.touchHaptics = readInt(ini, "physical", "haptics", 1) != 0;
    c.foleyLightCloth = readInt(ini, "foley", "light_cloth", c.foleyLightCloth);
    c.foleyLightSkin = readInt(ini, "foley", "light_skin", c.foleyLightSkin);
    c.foleyPatCloth = readInt(ini, "foley", "pat_cloth", c.foleyPatCloth);
    c.foleyPatSkin = readInt(ini, "foley", "pat_skin", c.foleyPatSkin);
    c.foleySlapCloth = readInt(ini, "foley", "slap_cloth", c.foleySlapCloth);
    c.foleySlapSkin = readInt(ini, "foley", "slap_skin", c.foleySlapSkin);
    c.foleyRubCloth = readInt(ini, "foley", "rub_cloth", c.foleyRubCloth);
    c.foleyRubSkin = readInt(ini, "foley", "rub_skin", c.foleyRubSkin);
    c.foleyGrabCloth = readInt(ini, "foley", "grab_cloth", c.foleyGrabCloth);
    c.foleyGrabSkin = readInt(ini, "foley", "grab_skin", c.foleyGrabSkin);
    c.foleyHands = readInt(ini, "foley", "hands", c.foleyHands);
    c.foleyHoldHands = readInt(ini, "foley", "hold_hands", c.foleyHoldHands);
    c.foleyObjectTap = readInt(ini, "foley", "object_tap", c.foleyObjectTap);
    c.foleyObjectSlap = readInt(ini, "foley", "object_slap", c.foleyObjectSlap);
    c.foleyObjectPunch = readInt(ini, "foley", "object_punch", c.foleyObjectPunch);
    c.foleyObjectMetal = readInt(ini, "foley", "object_metal", c.foleyObjectMetal);
    c.foleyObjectSoft = readInt(ini, "foley", "object_soft", c.foleyObjectSoft);
    c.foleyObjectMetalTap = readInt(ini, "foley", "object_metal_tap", c.foleyObjectMetalTap);
    c.grab = readInt(ini, "hands", "grab", 1) != 0;
    c.bodyTurnSmoothing = readFloat(ini, "body", "turn_smoothing", c.bodyTurnSmoothing);
    if (!(c.bodyTurnSmoothing >= 0 && c.bodyTurnSmoothing <= 1.0f)) c.bodyTurnSmoothing = 0.1f;
    if (!(c.bodySmoothing > 0.01f && c.bodySmoothing <= 1.0f)) c.bodySmoothing = 0.3f;
    return c;
}

}  // namespace s4vr
