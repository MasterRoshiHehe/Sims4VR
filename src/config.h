#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace s4vr {

enum class Mode : int {
    Quad = 0,    // game shown on a world-locked virtual screen (needs no camera hook)
    Stereo = 1,  // 6DoF alternate-eye rendering (needs the camera hook below)
};

struct Config {
    int autostart = 1;  // 0 = vr.start only, 1 = when the main menu appears, 2 = as soon as the mod loads
    Mode mode = Mode::Quad;
    // Stereo while playing a lot, the virtual screen in menus and loading.
    bool autoStereo = true;
    // ...and in first person (body mode) rather than the game camera
    bool playInBody = true;

    // Quad mode
    float quadDistance = 2.0f;  // metres in front of the recentered head
    float quadWidth = 2.4f;     // metres

    // Stereo mode
    float worldScale = 1.0f;  // game units per metre of head movement
    // Also give the game camera the headset's field of view.
    bool fovOverride = true;
    // With fov_override: each eye drawn with its own lopsided field of view (the
    // headset's, wider on the outer side) through the game's off-centre frustum
    // (camera +0x1f0), instead of a centred one wide enough for the wider side,
    // whose extra part the headset never shows (about a third of the picture
    // with Quest 3 under Virtual Desktop). camera.cpp apply, vr.cpp computeRenderSize.
    bool exactFov = true;
    // Contrast-adaptive sharpening of the eye images, 0 (off) .. 1
    float sharpen = 0.6f;
    // [stereo] upscale: 1 = FSR 1 (fsr.h): the game draws at fsr_render_scale of the headset's size and
    // each eye is upscaled to fsr_output_scale and sharpened by `sharpen`; 0 = render_scale and the
    // plain sharpen (sharpen.h). Not with SteamVR's align_eyes or the depth eye.
    bool upscale = true;
    float fsrRenderScale = 0.67f;
    float fsrOutputScale = 1.0f;
    // Stick turns reach the headset like head turns: each eye's pose is turned
    // by how far the world turned since it was rendered (vr.cpp renderStereo).
    bool turnReprojection = true;
    // Stick turns step by the headset's display clock (each shown frame turns
    // by the same amount) instead of when the game happened to hand over a frame.
    bool turnDisplayClock = true;
    bool walkDisplayClock = true;  // [stereo] walk_display_clock: stick walking timed by the headset's refreshes
    // With turnReprojection, in free movement: each pair is drawn at the heading
    // the stick turn will have reached when the headset shows it, so the turn
    // correction stays small and near things (Sims) don't wobble.
    bool turnLead = true;
    // While the headset is on: the game's graphics work goes below normal
    // priority on the card, so Virtual Desktop's streamer (which needs the 3D
    // engine to prepare each frame for the video encoder) is never starved by
    // a game that fills the card (gpuload.cpp yieldToStreamer).
    bool gpuYield = true;
    // First person: the right eye is drawn from the place the left eye of its
    // pair was drawn from, and the headset only gets whole pairs, so moving
    // doesn't put the eyes centimetres apart (vr.cpp renderStereo, camera.cpp applyEye).
    bool pairedEyes = true;
    // Paired eyes while you move freely: the body and the arms are posed from
    // the place the next pair is drawn from, so your hands match in both eyes
    // (camera.cpp applyEye, armik.cpp solve).
    bool pairedHands = true;
    // First person, smooth stick turning: your own body goes to the headset on
    // a layer of its own that the turn smoothing leaves alone (it turns the
    // world under you, and turned your body with it: hands doubled while
    // turning). Where your body was, the world is filled in from around it
    // (vr.cpp renderStereo, bodylayer.cpp).
    bool bodyLayer = true;
    bool frameMap = false;  // [stereo] frame_map: log the render frame's order and call stacks (framemap.cpp)
    // First person: every frame the eye the game didn't draw is made from the
    // one it did, moved by its depth (deptheye.h), so both eyes show the same moment.
    bool depthEye = false;  // off: the game alternates the eyes itself (clean-alternation candidate, user 2026-10-20 18:28Z)
    // [stereo] depth_eye_debug: what the made eye shows, to check it (deptheye.h
    // setDebugView): 0 the picture, 1 its depth, 2 no depth shift, 3 depth edges.
    int depthEyeDebug = 0;
    // [stereo] depth_eye_alternate: with depth_eye the game draws the eyes in turn again, and each frame
    // the other eye is made from the fresh one, where it can't see behind near things from that eye's
    // own last picture (1); 0: the left eye every frame, those spots filled from beside.
    // [stereo] flat_world: with depth_eye, the made (right) eye is the left picture only turned, no depth
    // shift (test view 2 as the mode): no ghost edges, 72 fps, but the world and your hands have no stereo
    // depth (user 2026-10-20 17:57Z: "the best of both worlds when it comes to looks and performance").
    bool flatWorld = true;
    // [stereo] both_eyes: first person, the game draws the left and the right eye every frame (two
    // scene passes, one headset frame), so both are real pictures of the same moment: real depth, no
    // ghost edges. Costs a second scene pass; off until tested (v0.13.304).
    // [stereo] both_eyes_skip: the right eye's pass leaves out a scene part (1 = mode 3, 2 = mode 4, 3 = mode 8;
    // 0 = none) and reuses the left eye's: a test of which part is the shadows (v0.13.307).
    int bothEyesSkip = 0;
    bool bothEyes = true;  // the one mode to make work (user 2026-10-20 18:32Z)
    // [stereo] frame_clock: headset frames go out on their own thread at the headset's rate, each with
    // the newest pictures turned on by the stick turn since they were drawn, so turning stays smooth
    // when the game draws fewer frames than the headset shows (v0.13.307). 0 = the Present sends them.
    bool frameClock = true;
    // [stereo] headset_width: the game draws its 3D view only as wide as the headset needs, even when
    // that is narrower than the game window (2560 -> ~1464 wide at render_scale 0.65), v0.13.309.
    bool headsetWidth = true;
    // [stereo] texture_bias: mip bias added to the game's material samplers in the headset view
    // (texbias.h): sharper textures a few metres out at render_scale under 1. 0 = off. Needs a restart.
    float textureBias = 0;  // -0.6 (v0.13.311) made textures a few metres out shimmer (user 2026-10-21)
    bool depthEyeAlternate = false;  // took turns: edges shook (v0.13.297-298)
    bool depthEyePresentDepth = true;  // [stereo] depth_eye_present_depth (deptheye::setPresentDepth)
    // [stereo] made_eye_*: the made eye's alignment, set by hand from the menu (Advanced › View ›
    // Right eye alignment) to find what's off. Picture: shift_x / shift_y (degrees, + right / up),
    // distance (eye distance used for the depth shift, 1 = measured), depth (scene depth scale).
    // Submitted to the headset: submit_x (metres, + right), submit_yaw / submit_pitch (degrees,
    // + right / up). The picture's own content is left as made by the submit ones.
    float madeEyeShiftX = 0, madeEyeShiftY = 0, madeEyeDistance = 1, madeEyeDepth = 1;
    float madeEyeSubmitX = 0, madeEyeSubmitYaw = 0, madeEyeSubmitPitch = 0;
    bool probeDouble = false;  // [stereo] probe_double: draw the scene twice a frame and time it (singleview.cpp)
    bool alignEyes = true;  // SteamVR: the older eye turned to the newest head pose each frame (eyealign.h)
    // Game render resolution as a fraction of the headset's (0 = the game's own, the monitor's)
    float renderScale = 0.75f;  // both real eyes every frame needs the room (v0.13.304: 0.8 gave ~62 fps)
    int renderMsaa = 2;  // most edge-smoothing samples at that resolution (never more than in-game)
    bool nearFade = false;  // the game's "see-through when the camera is close" (off: never)

    // Controllers: 1 = feed the sticks to the game as a virtual Xbox controller
    bool gamepad = false;
    bool snapTurn = true;   // [controls] snap_turn: right stick turns in fixed steps instead of smoothly (first person)
    float snapAngle = 30;   // [controls] snap_angle: degrees per snap
    float turnSpeed = 900;  // right stick: mouse pixels per second at full deflection

    // UI panel (stereo): the game's UI on a floating panel, in metres from the recentered head
    bool uiPanel = true;
    float uiDistance = 1.0f;
    float uiWidth = 1.3f;
    float uiHeight = -0.15f;  // centre, relative to eye level
    // First person: 0 = no panel, 1 = on your left wrist when you turn it toward you, 2 = in front as above, 3 = both
    int uiFirstPerson = 1;
    float wristWidth = 0.40f;  // metres
    // First person: the pie menu shows where you clicked, within reach, instead of on the panel
    bool pieAtHand = true;
    // First person: your own pie menu, by poking your stomach (grip held) or the laser on your other palm
    bool selfPiePoke = true;
    bool selfPiePalm = true;
    float pieDistance = 0.45f;  // metres from your eyes at most
    float pieWidth = 0.96f;     // metres
    float pieCrop = 0.75f;      // width of the UI cut out around the click, as a fraction of the UI's height
    // First person: a game picker shows on the object it is for (fridge door, TV screen)
    bool objectMenus = true;
    float objectMenuWidth = 0.45f;  // metres at least, on a door or front (screens fit inside)
    // Speech/thought bubbles, skill bars and stat flyaways over Sims: cards in the world (walls hide them)
    bool bubbles3d = true;
    // Bubbles debug category ([bubbles]): your own Sim's speech/thought bubble and skill bar in front of
    // you in first person (metres from your head: right, up, ahead), their size, and hiding bubbles
    bool bubbleOwnFront = true;                        // own_in_front
    float bubbleOwnPos[3] = {0.0f, 0.08f, 0.6f};       // own_bubble_position
    float skillOwnPos[3] = {0.14f, 0.2f, 0.6f};        // own_skill_position: above and a little right
    float bubbleOwnSize = 0.14f;                       // own_size: a bubble's width in front of you (metres)
    float bubbleNpcSize = 0.5f;                        // npc_size: a bubble's width over other Sims (game units)
    bool bubbleHideOwn = false;                        // hide_own: your Sim's bubbles (first person)
    bool bubbleHideNpc = false;                        // hide_npc: everyone else's bubbles
    bool popups3d = true;          // first person: game popups as cards in the world ([ui] popups_3d)
    float popupDistance = 0.75f;   // metres in front of you
    float popupSize = 0.55f;       // card width per metre away (beside a Sim it scales with distance)
    // Pieces of the game's UI on your arms and around you (first person, [wrist]).
    bool wristPieces = true;
    bool noDimming = true;  // [wrist] no_dimming: the game's blue modal dimming off everywhere (at the source)
    // Where each element sits ([wrist] <name>_pos "x y z", <name>_turn "yaw pitch roll", <name>_size).
    // On an arm (watch, build_button, actions, needs, moodlets), from your Sim's
    // wrist joint on its real forearm bones: x metres toward the elbow, y metres
    // sideways round the arm, z metres out from the bone line; <name>_around:
    // degrees round the arm from the palm side. Around you (wants, sleep): x
    // right, y up, z ahead of your eyes; pause: from where popups show.
    // turn: yaw (spins it flat), pitch (tips it about its width), roll (tips it
    // about its height), degrees, after it is placed facing out.
    float wristScale = 1;          // [wrist] scale: every element's size times this
    float wristUiHeight = 1440;    // the game window's height: how big the UI is drawn (its pieces are found by it)
    bool wristTime = true;         // the clock and speed buttons, inside your left wrist
    float wristTimePos[3] = {0.06f, 0, 0.035f}, wristTimeTurn[3] = {}, wristTimeAround = 0, wristTimeSize = 0.09f;
    bool wristBuild = true;        // the round build/buy button, on your left wrist
    float wristBuildPos[3] = {0.03f, 0, 0.028f}, wristBuildTurn[3] = {}, wristBuildAround = 90, wristBuildSize = 0.04f;
    bool wristQueue = true;        // the action queue (bubbles only), along the top of your left forearm
    float wristQueuePos[3] = {0.12f, 0, 0.035f}, wristQueueTurn[3] = {}, wristQueueAround = 180, wristQueueSize = 0.17f;
    bool wristNeeds = true;        // the need bars, inside your right forearm
    float wristNeedsPos[3] = {0.08f, 0, 0.035f}, wristNeedsTurn[3] = {}, wristNeedsAround = 0, wristNeedsSize = 0.12f;
    bool wristMoodlets = true;     // the moodlets, inside your right forearm toward the elbow
    float wristMoodletsPos[3] = {0.17f, 0, 0.045f}, wristMoodletsTurn[3] = {}, wristMoodletsAround = 0,
          wristMoodletsSize = 0.1f;
    bool wristFullButton = true;   // the watch's UI button shows / hides the full UI on your wrist
    bool wristWhims = true;        // the wants above you, seen when you look up
    float wristWhimsPos[3] = {0, 0.45f, 0.30f}, wristWhimsTurn[3] = {}, wristWhimsSize = 0.36f;
    // The money gained / lost (the game's flyaway), in front of you a little up
    // and right (user 2026-10-15 20:46Z): right, up, ahead from your eyes.
    bool wristMoney = true;
    float wristMoneyPos[3] = {0.16f, 0.10f, 0.60f}, wristMoneyTurn[3] = {}, wristMoneySize = 0.30f;
    bool wristPause = true;        // a pause symbol under where popups show, while the game is paused
    float wristPausePos[3] = {0, -0.30f, 0}, wristPauseTurn[3] = {}, wristPauseSize = 0.06f;
    bool wristSleepClock = true;   // the clock and speed buttons low in your view while your Sim sleeps
    float wristSleepPos[3] = {0, -0.22f, 0.6f}, wristSleepTurn[3] = {}, wristSleepSize = 0.16f;

    // Body mode (stereo at the active Sim's eyes)
    float bodyForward = 0.10f;   // metres from the head joint to the eyes, along where you look
    float bodyHeight = 0.15f;    // metres above the head joint
    // Metres your view sits behind the Sim's own eyes (b__L_Eye__/b__R_Eye__):
    // inside its head, so your own face and hair never show (mirrors keep
    // them). forward/height are only used when a rig has no eye joints.
    float bodyEyeInset = 0.02f;
    float bodySmoothing = 0.3f;
    // Seated and other game animations: your view starts at the Sim's eyes when
    // it sits, its spine leans with your real head, and the view never leaves
    // its head (camera.cpp seatView, armik.cpp solveLean).
    bool bodySeatFollow = true;
    bool bodySeatTorso = true;  // seated: your head moves the upper body, not the animation's sway
    float bodyTurnSmoothing = 0.1f;  // 0..1 per frame; how quickly the view turns with the Sim (0 = never)
    float bodyNearClip = 0.08f;      // metres, the lowest near limit (first person)
    bool bodyNearAdaptive = true;    // the near limit follows what is close, less flicker (DX11)
    float bodyNearMax = 0.75f;       // metres, the highest it goes
    bool bodyHideFace = true;        // your own face, eyes and teeth folded out of your view (mirrors keep them)
    bool steadyShadows = true;       // first person: sun shadows that hold still (shadows.h)
    float shadowStep = 0.2f;         // degrees the sun moves before the shadows follow
    float shadowSharp = 5.0f;        // metres the sharpest shadow cascade reaches (0: the game's 10)
    float shadowMap = 4096;            // shadow map pixels while the headset view is on (0: the game's)
    float bodyTurnRate = 150;    // degrees per second, right stick fully pushed
    bool armIk = true;           // the Sim's arms and fingers follow your controllers
    float handPitch = 40;        // degrees the Sim's hand points below the controller's grip axis
    bool handTracking = true;    // controllers put down: the Sim's hands follow your real hands (looks only)
    bool headTracking = true;    // the Sim's head looks where you look (needs arm_ik)
    // Physical touch with other Sims in first person (needs arm_ik)
    bool touchBodies = true;     // their bodies stop your hands and give a little; you can't walk through them
    bool holdHands = true;       // close a hand around a Sim's hand to hold it
    bool lookAtYou = true;       // a Sim you lean in close to looks at you
    bool headRepel = true;       // your head is pushed back out of other Sims instead of passing through
    float headRadius = 0.08f;    // metres your view keeps clear of their bodies
    bool grabBodies = true;      // squeeze grip and trigger on a Sim's body to hold it (they react, bodygrab.py)
    float grabCurl = 0.45f;      // how far the fingers close on a held body (0 open .. 1 fist)
    float grabDrag = 0.15f;      // metres the held place can be dragged (not Claude Jiggle's breasts/butt)
    bool touchHaptics = true;    // controllers vibrate when a hand touches a Sim
    bool touch = true;           // point with your index finger and touch things to click them
    float touchPast = 0.015f;    // metres past the Sim's fingertip that count as the finger's end
    float walkSpeed = 1.5f;      // metres per second, left stick fully pushed
    float runSpeed = 3.0f;       // the same while running (left stick tapped)
    bool bodyKeepHeight = true;  // first person: the game draws your body at your height, not on its level's floor
    bool bodyWalkClips = true;  // first person: the game's own start and stop walk clips
    float bodyStopTime = 0.5f;  // first person: seconds walking eases to rest after you let go
    bool bodySmoothSlide = true;  // first person: slide along walls smoothly, not in 15 degree steps

    // Hands (first person): close a hand near an object to pick it up, open it to put it down
    bool grab = true;

    // Audio: the game's listener turns with your head, and sits at your head in body mode
    bool audioFollowsHead = true;
    // Audio (first person): every sound 1 m or more away is placed where it is; the game spreads
    // those with a small near radius toward the middle of your head (camera.cpp hookedPan)
    bool audioPanAnyDistance = true;
    // Audio (first person): the outdoor ambience fades in over this many metres either side
    // of the outside walls instead of switching at the doorway; 0 = the game's own switch
    float outdoorFade = 4.0f;
    float mirrorInterior = 10;  // max metres a mirror in a room renders (0: the game's 15)
    float mirrorExterior = 50;  // ... outdoors (0: the game's 100)
    bool mirrorClip = true;   // first person lopsided eye: keep the game's clip at the mirror glass
    bool reflectionsOff = false;  // no mirror or water reflections at all
    bool ambientOcclusion = true;  // [stereo] ambient_occlusion: the game's SSAO; 0 = off for speed (GPU pack, perf item 4)
    bool textureProbe = true;     // [perf] texture_probe: time the Sim texture compositing calls, log those over 20 ms (texprobe.cpp)
    bool simCacheBoost = false;   // [perf] sim_cache_boost: Sim body-shape caches x4 (simcache.cpp)
    int simCacheMb = 256;         // [perf] sim_cache_mb: SimCacheSizeLimit used when boosted and the game has none
    bool nearOncePerPair = true;  // [perf] near_once_per_pair: both eyes, near limit's depth copy in the left eye's pass only
    float simLodScale = 2;        // [perf] sim_lod_scale: stretch the Sim detail-change distances (1 = game's)
    bool hitchCatcher = true;     // [perf] hitch_catcher: log what the game's main thread does during stalls over 60 ms (hitch.cpp)

    // Phone (first person): close your right hand at your right hip and your
    // Sim holds its phone, with the game menu on its screen (game not paused)
    bool phone = true;
    float phoneHipDrop = 0.40f;   // metres below your eyes your hand must be
    bool phonePopupBuzz = true;   // the controllers buzz like a phone when the game shows a popup
    float phoneWidth = 0.102f;    // our phone's size, metres
    float phoneHeight = 0.190f;
    float phoneBezel = 0.004f;    // the dark border around its screen, metres
    float phoneDepth = 0.009f;    // its thickness, metres
    float phoneOffset[3] = {-0.064f, 0.042f, 0.024f};  // phone centre from the grip (the Sim's hand frame: -X palm, +Y thumb, +Z wrist)
    float phoneTilt = -15;        // degrees the phone rolls about the hand's line (wrist to fingers)
    float phoneYaw = 90;          // then degrees it turns about its own Y (bottom-top) axis
    float phonePitch = 45;        // then degrees its top tips away from you, about its own X (left-right) axis
    float phoneCurl = 0.8f;       // how far the holding hand's four fingers close (0..1)
    float phoneThumbBend = 0.3f;  // how far its thumb bends toward the palm (0..1)
    float phoneThumbSpread = 0;   // degrees its thumb swings away from the hand (negative = toward it)
    float phoneHandOffset[3] = {0, 0, 0};  // the holding hand moved against the phone (metres): out from its middle, up it, toward you
    float phoneHandTurn[3] = {0, 0, 0};    // and turned about the palm (degrees): about the phone's up axis, its left-right axis, its screen's line
    bool phoneProp = false;       // also put the game's phone model in the Sim's hand
    float phoneCropWidth = 0.45f, phoneCropHeight = 0.80f;  // part of the UI shown before the menu is measured (x UI height)
    // [tablet] In first-person build/buy the phone gesture brings out a tablet
    // with the whole build/buy UI on its screen instead.
    bool tablet = true;
    float tabletWidth = 0.30f;    // metres
    float tabletHeight = 0;       // 0 = the screen takes the palette layout's shape
    float tabletDepth = 0.008f;
    float tabletBezel = 0.010f;   // the dark border around its screen
    float tabletHold = 0.035f;    // how far in from its side edge the hand holds it (metres)

    // Voice: your mic moves your Sim's mouth (voice.cpp)
    bool voiceMic = false;         // [voice] mic = true / false
    std::string voiceDevice;       // part of the mic's name; empty = Windows' default input
    float voiceGateDb = -45;       // quieter than this keeps the mouth shut
    float voiceFullDb = -18;       // this loud opens it all the way
    float voiceMouthOpen = 14;     // degrees, the jaw's widest

    // Developer: run sims4vr_probe.py from the mod folder whenever it changes
    bool devProbe = false;
};

Config loadConfig(const std::wstring& iniPath);

// sims4vr_user.ini next to sims4vr.ini: the settings menu in the headset (B)
// saves there, and its values win over sims4vr.ini's.
std::wstring userIniPath(const std::wstring& iniPath);
// A value as text: sims4vr_user.ini's if it sets it, else sims4vr.ini's, else def.
std::string readSetting(const std::wstring& iniPath, const char* section, const char* key, const char* def);
// sims4vr.ini's own value (ignoring sims4vr_user.ini), else def.
std::string readShippedSetting(const std::wstring& iniPath, const char* section, const char* key, const char* def);

}  // namespace s4vr
