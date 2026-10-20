#include "settings.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <thread>
#include <deque>
#include <string>
#include <vector>

#include "armik.h"
#include "camera.h"
#include "facehide.h"
#include "walkstop.h"
#include "grab.h"
#include "log.h"
#include "othermods.h"
#include "player.h"
#include "shadows.h"

namespace s4vr::settings {
namespace {

enum class Kind { Number, Toggle, Choice, Reset, Action, Link, Back };
// What a change needs besides the new value in vr.cpp's config.
enum Apply { kLive, kView, kWalk, kHands, kTouch, kRenderScale, kNextStart, kNextPhone, kShadows, kAudio };

struct Row {
    const wchar_t* group = L"";
    const wchar_t* label = L"";
    const char* section = nullptr;
    const char* key = nullptr;
    Kind kind = Kind::Number;
    float Config::*number = nullptr;
    float (Config::*triple)[3] = nullptr;  // a value of three (x y z in the ini): this row is triple[index]
    int index = -1;                        // >= 0: one of three values under one key
    bool Config::*toggle = nullptr;  // nullptr: a script mod switch, its value kept here
    int Config::*choice = nullptr;
    float min = 0, max = 1, step = 0.1f, shown = 1;  // shown: what the value is multiplied by on screen
    bool wrap = false;    // an angle: past max comes round to min
    float fallbackNumber = 0;  // a script mod number's value when no ini sets it
    const wchar_t* format = L"%.2f";
    std::vector<const wchar_t*> names;  // a choice's options
    Apply apply = kLive;
    bool fallback = true;  // a script mod switch's value when no ini sets it
    int action = 0;        // an Action row: the control it sends the script mod (grab::pushControl);
                           // a Toggle row with one: a switch in the game (testing cheats), not saved
    const wchar_t* target = nullptr;  // a Link row: the page it opens (a group name)
    int page = 0;                     // the page the row is on (from its group); a Link's: the one it opens
    int linkPage = 0;
    const char* jiggle = nullptr;  // a setting of the separate Integrated Body Physics mod (its key), not saved here
    bool sound = false;            // with jiggle: the key belongs to the separate Extended Sound Control mod instead
    const wchar_t* tip = nullptr;  // what it does: shown in a box under the row while it's picked
    const char* when = nullptr;       // another mod's Link row: it opens only while this key of the mod is not 0
    const wchar_t* offTip = nullptr;  // with when: the tip while it can't be opened (greyed out)
};

// The menu's pages (structure: user 2026-10-15 18:25Z, v0.13.153). Rows sit on
// the page named by their group; Play, Settings and Other mods are the first
// page's groups. Settings holds the everyday settings and a link to Advanced,
// whose pages hold the rest (user 2026-10-17 16:09Z, v0.13.235). A page's
// title is its parents' names and its own (Advanced › Wrist and HUD › Watch).
// No page holds more rows than the panel shows (kShown, Back included), so
// only the first page scrolls.
struct Page {
    const wchar_t* group;
    const wchar_t* name;
    const wchar_t* parent;  // its parent page's group; nullptr: the first page itself
    int mod = -1;           // another mod's page (othermods::Mod): it and its pages show only while that mod is loaded
};
const Page kFixedPages[] = {
    {L"", L"Sims4VR", nullptr},
    {L"Testing", L"Testing cheats", L""},
    {L"Advanced", L"Advanced", L""},
    {L"View", L"View", L"Advanced"},
    {L"View picture", L"Picture", L"View"},
    {L"View motion", L"Smooth motion", L"View"},
    {L"View head", L"Head and body", L"View"},
    {L"View near", L"Near clipping", L"View"},
    {L"View shadows", L"Shadows", L"View"},
    {L"Moving", L"Moving", L"Advanced"},
    {L"Hands", L"Hands", L"Advanced"},
    {L"Reach", L"Reach and game control", L"Advanced"},
    {L"Other Sims", L"Other Sims", L"Advanced"},
    {L"Menus", L"Menus and popups", L"Advanced"},
    {L"Menus pie", L"Pie menu", L"Menus"},
    {L"Menus pickers", L"Pickers on objects", L"Menus"},
    {L"Menus popups", L"Popups", L"Menus"},
    {L"Menus panels", L"Panels and flat screen", L"Menus"},
    {L"HUD", L"Wrist and HUD", L"Advanced"},
    {L"HUD watch", L"Watch", L"HUD"},
    {L"HUD needs", L"Needs", L"HUD"},
    {L"HUD moodlets", L"Moodlets", L"HUD"},
    {L"HUD queue", L"Action queue", L"HUD"},
    {L"HUD build", L"Build button", L"HUD"},
    {L"HUD wants", L"Wants", L"HUD"},
    {L"HUD pause", L"Pause symbol", L"HUD"},
    {L"HUD sleep", L"Clock while asleep", L"HUD"},
    {L"Bubbles", L"Speech bubbles and skill bars", L"Advanced"},
    {L"Bubbles yours", L"Where yours sit", L"Bubbles"},
    {L"Phone", L"Phone", L"Advanced"},
    {L"Phone position", L"Position", L"Phone"},
    {L"Phone hand", L"Hand", L"Phone"},
    {L"Phone size", L"Size and screen", L"Phone"},
    {L"Phone model", L"Game's phone model", L"Phone"},
    {L"Tablet", L"Tablet (build/buy)", L"Phone"},
    {L"Sound", L"Sound", L"Advanced"},
    {L"Performance", L"Claude Performance", L"", othermods::kPerformance},
};
// The pages: ours, then those other mods' menu exports add (Integrated Body Physics, Extended Sound Control: mergeMenus).
std::vector<Page> g_pages(std::begin(kFixedPages), std::end(kFixedPages));
int pageCount() { return static_cast<int>(g_pages.size()); }

int pageOf(const wchar_t* group) {
    for (int p = 1; p < pageCount(); ++p)
        if (group && !wcscmp(group, g_pages[p].group)) return p;
    return 0;
}

// The page above this one (0 for the first page's own).
int parentOf(int page) {
    return page > 0 && page < pageCount() && g_pages[page].parent ? pageOf(g_pages[page].parent) : 0;
}

// Another mod's pages, and the rows that open them, show only while that mod
// is loaded (user 2026-10-17 16:09Z).
bool pageShown(int page) {
    for (int p = page, guard = 0; p > 0 && p < pageCount() && guard < 8; p = parentOf(p), ++guard)
        if (g_pages[p].mod >= 0) return othermods::loaded(static_cast<othermods::Mod>(g_pages[p].mod));
    return true;
}

Row link(const wchar_t* group, const wchar_t* label, const wchar_t* target) {
    Row r;
    r.group = group;
    r.label = label;
    r.kind = Kind::Link;
    r.target = target;
    return r;
}

Row number(const wchar_t* group, const wchar_t* label, const char* section, const char* key, float Config::*m,
           float min, float max, float step, float shown, const wchar_t* format, Apply apply = kLive) {
    Row r;
    r.group = group;
    r.label = label;
    r.section = section;
    r.key = key;
    r.number = m;
    r.min = min;
    r.max = max;
    r.step = step;
    r.shown = shown;
    r.format = format;
    r.apply = apply;
    return r;
}

// One of the three numbers under one key ("x y z"); m nullptr: only the script mod reads it.
Row component(const wchar_t* group, const wchar_t* label, const char* section, const char* key,
              float (Config::*m)[3], int index, float min, float max, float step, float shown,
              const wchar_t* format, Apply apply = kLive) {
    Row r = number(group, label, section, key, nullptr, min, max, step, shown, format, apply);
    r.triple = m;
    r.index = index;
    return r;
}

Row angle(const wchar_t* group, const wchar_t* label, const char* section, const char* key, float Config::*m) {
    Row r = number(group, label, section, key, m, -180, 180, 5, 1, L"%.0f°");
    r.wrap = true;
    return r;
}

Row toggle(const wchar_t* group, const wchar_t* label, const char* section, const char* key, bool Config::*m,
           Apply apply = kLive) {
    Row r;
    r.group = group;
    r.label = label;
    r.section = section;
    r.key = key;
    r.kind = Kind::Toggle;
    r.toggle = m;
    r.apply = apply;
    return r;
}

// A switch only the script mod reads (when first person starts, or when it's used).
Row flag(const wchar_t* group, const wchar_t* label, const char* section, const char* key, bool fallback,
         Apply apply = kNextStart) {
    Row r = toggle(group, label, section, key, nullptr, apply);
    r.fallback = fallback;
    return r;
}

// A number only the script mod reads (fallback: its value when no ini sets it).
Row scriptNumber(const wchar_t* group, const wchar_t* label, const char* section, const char* key, float fallback,
                 float min, float max, float step, const wchar_t* format) {
    Row r = number(group, label, section, key, nullptr, min, max, step, 1, format);
    r.fallbackNumber = fallback;
    return r;
}

// A setting of the separate Integrated Body Physics mod: read from and changed
// in its DLL while that is loaded (it saves its own settings).
Row jiggleNumber(const wchar_t* group, const wchar_t* label, const char* key, float min, float max, float step,
                 float shown, const wchar_t* format, const wchar_t* tip) {
    Row r = number(group, label, nullptr, nullptr, nullptr, min, max, step, shown, format);
    r.jiggle = key;
    r.tip = tip;
    return r;
}
// An Integrated Body Physics button that asks "are you sure?" (No / Yes) before it sets its key to 1.
Row jiggleConfirm(const wchar_t* group, const wchar_t* label, const char* key, const wchar_t* tip) {
    Row r;
    r.group = group;
    r.label = label;
    r.kind = Kind::Action;
    r.jiggle = key;
    r.tip = tip;
    return r;
}
Row jiggleSwitch(const wchar_t* group, const wchar_t* label, const char* key, const wchar_t* tip) {
    Row r = toggle(group, label, nullptr, nullptr, nullptr);
    r.jiggle = key;
    r.tip = tip;
    return r;
}

Row choice(const wchar_t* group, const wchar_t* label, const char* section, const char* key, int Config::*m,
           std::vector<const wchar_t*> names) {
    Row r;
    r.group = group;
    r.label = label;
    r.section = section;
    r.key = key;
    r.kind = Kind::Choice;
    r.choice = m;
    r.names = std::move(names);
    return r;
}

std::vector<Row> makeRows() {
    const wchar_t* view = L"View";
    const wchar_t* moving = L"Moving";
    const wchar_t* hands = L"Hands";
    const wchar_t* sims = L"Other Sims";
    const wchar_t* menus = L"Menus";
    const wchar_t* sound = L"Sound";
    const wchar_t* phone = L"Phone";
    const wchar_t* phonePosition = L"Phone position";
    const wchar_t* phoneHand = L"Phone hand";
    const wchar_t* phoneSize = L"Phone size";
    const wchar_t* phoneModel = L"Phone model";
    const wchar_t* tablet = L"Tablet";
    const wchar_t* testing = L"Testing";
    const wchar_t* front = L"Settings";  // the first page, under Play: the everyday settings
    const wchar_t* advanced = L"Advanced";
    Row view3;
    view3.group = L"Play";
    view3.label = L"First person / game camera";
    view3.kind = Kind::Action;
    view3.action = 1;  // controls.py: switch between first person and the game camera
    // Buttons for testing (controls.py runs them in the game, once a press).
    auto button = [testing](const wchar_t* label, int action) {
        Row r;
        r.group = testing;
        r.label = label;
        r.kind = Kind::Action;
        r.action = action;
        return r;
    };
    auto perfButton = [](const wchar_t* label, int action, const wchar_t* tip) {
        Row r;
        r.group = L"Performance";
        r.label = label;
        r.kind = Kind::Action;
        r.action = action;
        r.tip = tip;
        return r;
    };
    // Testing cheats: on sends 6 (testingcheats true), off sends 9. Cheat pie menus
    // (every pie menu opens as the Shift+click cheat menu, pie.py; they need testing
    // cheats, so on turns those on too): on sends 11, off 12. Separate switches since
    // the user's 2026-10-15 01:06Z ask (v0.13.127). The game's state comes back
    // through setTestingCheats; both start off each time the game does.
    Row cheats;
    cheats.group = testing;
    cheats.label = L"Testing cheats";
    cheats.kind = Kind::Toggle;
    cheats.action = 6;
    Row cheatPies = cheats;
    cheatPies.label = L"Cheat pie menus (every pie menu is the cheat menu)";
    cheatPies.action = 11;
    // A hard cancel (controls.py action 13 -> halt.py, first person only; user
    // 2026-10-15 18:54Z, v0.13.159).
    Row stopAll;
    stopAll.group = L"Play";
    stopAll.label = L"Stop everything (hard cancel)";
    stopAll.kind = Kind::Action;
    stopAll.action = 13;
    stopAll.tip = L"Your Sim stops whatever it is doing at once, walks too; standing up or putting something down "
                  L"still plays. Still busy a few seconds later, the game resets it where it stands. First person only.";
    Row reset;
    reset.group = advanced;
    reset.label = L"Use sims4vr.ini for everything";
    reset.kind = Kind::Reset;
    std::vector<Row> rows = {
        view3,
        // The HUD / game UI switch on the first page too (user 2026-10-15 20:28Z, v0.13.163): the same
        // [ui] first_person as the Menus page's row.
        choice(L"Play", L"Game menus in first person", "ui", "first_person", &Config::uiFirstPerson,
               {L"Hidden", L"Wrist HUD only", L"Game UI in front only", L"Both"}),
        stopAll,
        link(L"Play", L"Testing cheats", testing),
        // The everyday settings on the first page, the rest under Advanced (user 2026-10-17 16:09Z, v0.13.235;
        // these rows moved here from their pages).
        number(front, L"Render resolution", "stereo", "render_scale", &Config::renderScale, 0.4f, 1.5f, 0.05f, 100,
               L"%.0f%%", kRenderScale),
        number(front, L"Walk speed", "body", "walk_speed", &Config::walkSpeed, 0.5f, 5, 0.1f, 1, L"%.1f m/s", kWalk),
        number(front, L"Turn speed", "body", "turn_rate", &Config::bodyTurnRate, 30, 360, 10, 1, L"%.0f°/s"),
        flag(front, L"Walk there yourself (footprint trail)", "body", "trail", true, kLive),  // trail.py
        toggle(front, L"Pick things up", "hands", "grab", &Config::grab, kNextStart),
        toggle(front, L"Touch to click", "body", "touch", &Config::touch, kNextStart),
        toggle(front, L"Pie menu at your hand", "ui", "pie_at_hand", &Config::pieAtHand),
        toggle(front, L"Popups in the world", "ui", "popups_3d", &Config::popups3d),
        toggle(front, L"HUD on (all of it)", "wrist", "hud", &Config::wristPieces),
        number(front, L"HUD size (all of it)", "wrist", "scale", &Config::wristScale, 0.3f, 3, 0.05f, 100, L"%.0f%%"),
        toggle(front, L"Phone at your hip", "phone", "phone", &Config::phone),
        flag(front, L"Notifications about one Sim over their head (off: phone only)", "phone", "notes_overhead", true, kLive),  // notify.py
        toggle(front, L"Mic moves your Sim's mouth", "voice", "mic", &Config::voiceMic, kNextStart),
        link(front, L"Advanced settings", advanced),
        link(advanced, L"View", view),
        link(advanced, L"Moving", moving),
        link(advanced, L"Hands", hands),
        link(advanced, L"Reach and game control", L"Reach"),
        link(advanced, L"Other Sims", sims),
        link(advanced, L"Menus and popups", menus),
        link(advanced, L"Wrist and HUD", L"HUD"),
        link(advanced, L"Speech bubbles and skill bars", L"Bubbles"),
        link(advanced, L"Phone", phone),
        link(advanced, L"Sound", sound),
        reset,
        // Their pages come from their own menu exports once they are loaded (mergeMenus, v0.13.236/237).
        link(L"Other mods", L"Integrated Body Physics", L"ibp:main"),
        link(L"Other mods", L"Extended Sound Control", L"esc:main"),
        link(L"Other mods", L"Claude Performance", L"Performance"),

        cheats,
        cheatPies,
        button(L"Fill needs", 5),                        // the played Sim's needs to full
        button(L"Motherlode (+50,000 simoleons)", 7),    // motherlode
        button(L"Move objects on", 8),                   // bb.moveobjects on
        button(L"CAS full edit mode", 10),               // cas.fulleditmode

        // The separate Claude Performance mod's console commands (user 2026-10-16 19:13Z, v0.13.184):
        // controls.py actions 70-76 run perf.* as if typed; that mod answers with a game notification.
        perfButton(L"Status", 70, L"Shows Claude Performance's state as a notification: freeze on or off, how many "
                                  L"objects are frozen, the game's own garbage-collector mode, recent sim update times."),
        perfButton(L"Freeze on", 71, L"Freezes the lot's objects now and after every lot load, so the game's garbage "
                                     L"collector skips them. Until the game restarts; claudeperf.ini sets the default."),
        perfButton(L"Freeze off", 72, L"Stops freezing at the next lot loads (what is frozen now stays until you leave "
                                      L"the lot). Until the game restarts; claudeperf.ini sets the default."),
        perfButton(L"Timing log on", 73, L"Writes garbage-collector and sim update times to claudeperf.log every 60 s."),
        perfButton(L"Timing log off", 74, L"Stops the timing log until the game restarts."),
        perfButton(L"Profile 30 s", 75, L"Records for 30 s which game functions take the most time and writes them to "
                                        L"claudeperf_profile.txt. The game runs slower while it records."),
        perfButton(L"Profile 60 s", 76, L"Records for 60 s which game functions take the most time and writes them to "
                                        L"claudeperf_profile.txt. The game runs slower while it records."),

        link(view, L"Picture", L"View picture"),
        link(view, L"Smooth motion", L"View motion"),
        link(view, L"Head and body", L"View head"),
        link(view, L"Near clipping", L"View near"),
        link(view, L"Shadows", L"View shadows"),
        number(L"View picture", L"Sharpness", "stereo", "sharpen", &Config::sharpen, 0, 1, 0.05f, 100, L"%.0f%%"),
        toggle(L"View picture", L"Card goes to the headset stream first", "stereo", "gpu_yield", &Config::gpuYield),
        toggle(L"View picture", L"Align the eyes (SteamVR)", "stereo", "align_eyes", &Config::alignEyes),
        toggle(L"View picture", L"Lopsided eye view (draws less unseen picture)", "stereo", "exact_fov", &Config::exactFov,
               kRenderScale),
        toggle(L"View motion", L"Smooth stick turning", "stereo", "turn_reprojection", &Config::turnReprojection),
        toggle(L"View motion", L"Stick turn on the headset's clock", "stereo", "turn_display_clock", &Config::turnDisplayClock),
        toggle(L"View motion", L"Stick walk on the headset's clock", "stereo", "walk_display_clock", &Config::walkDisplayClock),
        toggle(L"View motion", L"Draw ahead of the stick turn", "stereo", "turn_lead", &Config::turnLead),
        toggle(L"View motion", L"Both eyes drawn from one place (walking)", "stereo", "paired_eyes", &Config::pairedEyes),
        toggle(L"View motion", L"Hands match in both eyes (walking)", "stereo", "paired_hands", &Config::pairedHands),
        toggle(L"View motion", L"Own body steady while turning", "stereo", "body_layer", &Config::bodyLayer),
        toggle(L"View picture", L"Test: draw the scene twice (log)", "stereo", "probe_double", &Config::probeDouble),
        number(L"View head", L"Head follow speed", "body", "smoothing", &Config::bodySmoothing, 0.05f, 1, 0.05f, 1, L"%.2f",
               kView),
        number(L"View head", L"Turn with your Sim", "body", "turn_smoothing", &Config::bodyTurnSmoothing, 0, 1, 0.05f, 1,
               L"%.2f", kView),
        number(L"View head", L"View behind the eyes", "body", "eye_inset", &Config::bodyEyeInset, 0, 0.1f, 0.005f, 100,
               L"%.1f cm", kView),
        toggle(L"View head", L"Your Sim's head follows your look", "body", "head_tracking", &Config::headTracking, kHands),
        toggle(L"View head", L"Seated: your Sim leans with you", "body", "seat_follow", &Config::bodySeatFollow, kView),
        toggle(L"View head", L"Seated: your head moves the upper body", "body", "seat_torso", &Config::bodySeatTorso, kView),
        toggle(L"View head", L"Hide your own face (mirrors keep it)", "body", "hide_face", &Config::bodyHideFace, kView),
        number(L"View near", L"Near clipping", "body", "near_clip", &Config::bodyNearClip, 0.02f, 0.5f, 0.01f, 100, L"%.0f cm",
               kView),
        toggle(L"View near", L"Near clipping follows what's close", "body", "near_adaptive", &Config::bodyNearAdaptive, kView),
        number(L"View near", L"Near clipping at most", "body", "near_max", &Config::bodyNearMax, 0.1f, 2, 0.05f, 100, L"%.0f cm",
               kView),
        flag(L"View near", L"Rooms in doorways stay whole", "body", "rooms_whole", true, kLive),  // gamefix.cpp
        number(L"View picture", L"Mirrors reach in rooms (0 = game's)", "stereo", "mirror_far_inside", &Config::mirrorInterior, 0, 15,
               1, 1, L"%.0f m", kNextStart),
        number(L"View picture", L"Mirrors and water reach outdoors (0 = game's)", "stereo", "mirror_far_outside",
               &Config::mirrorExterior, 0, 100, 5, 1, L"%.0f m", kNextStart),
        toggle(L"View picture", L"Mirrors clip at the glass (may pop objects)", "stereo", "mirror_clip", &Config::mirrorClip,
               kLive),
        toggle(L"View picture", L"No mirror or water reflections", "stereo", "reflections_off", &Config::reflectionsOff,
               kNextStart),
        toggle(L"View shadows", L"Steady shadows", "body", "steady_shadows", &Config::steadyShadows, kShadows),
        number(L"View shadows", L"Shadows follow the sun every", "body", "shadow_step", &Config::shadowStep, 0, 2, 0.05f, 1,
               L"%.2f°", kShadows),
        number(L"View shadows", L"Sharpest shadows reach", "body", "shadow_sharp", &Config::shadowSharp, 0, 10, 0.5f, 1,
               L"%.1f m", kShadows),

        number(moving, L"Run speed (tap left stick)", "body", "run_speed", &Config::runSpeed, 1, 8, 0.1f, 1, L"%.1f m/s",
               kWalk),
        number(moving, L"Game camera turn speed", "controls", "turn_speed", &Config::turnSpeed, 200, 3000, 100, 1,
               L"%.0f"),
        flag(moving, L"Stairs and ladders carry you", "body", "carried_stairs", true),
        toggle(moving, L"Body drawn at your height (stairs)", "body", "keep_height", &Config::bodyKeepHeight, kWalk),
        toggle(moving, L"Slide smoothly along walls", "body", "smooth_slide", &Config::bodySmoothSlide, kWalk),
        toggle(moving, L"Start and stop walking animations", "body", "walk_clips", &Config::bodyWalkClips, kWalk),
        number(moving, L"Stop smoothness (0 = instant)", "body", "stop_time", &Config::bodyStopTime, 0, 1.5f, 0.1f, 1, L"%.1f s",
               kWalk),

        flag(L"Reach", L"Only use what's within 1 m", "body", "reach_only", true, kLive),  // reach.py
        flag(L"Reach", L"Grey out what is too far", "body", "grey_far", true, kLive),  // reach.py
        flag(L"Reach", L"Reactions don't stop you", "body", "reactions_in_place", true, kLive),  // scripted.py
        flag(L"Reach", L"The game can't send you to things", "body", "no_errands", true, kLive),  // scripted.py
        flag(L"Reach", L"Stop walks you didn't ask for", "body", "route_guard", true, kLive),  // halt.py

        number(hands, L"Hand angle", "body", "hand_pitch", &Config::handPitch, 0, 90, 5, 1, L"%.0f°", kHands),
        flag(hands, L"Eat with your hands", "hands", "eat", true),
        toggle(hands, L"Vibrate on touch", "physical", "haptics", &Config::touchHaptics),

        toggle(sims, L"Their bodies stop your hands", "physical", "bodies", &Config::touchBodies, kTouch),
        toggle(sims, L"Hold hands", "physical", "hold_hands", &Config::holdHands, kTouch),
        toggle(sims, L"They look at you", "physical", "look_at_you", &Config::lookAtYou, kTouch),
        toggle(sims, L"Your head stays out of them", "physical", "head", &Config::headRepel, kTouch),
        number(sims, L"Head keeps clear by", "physical", "head_radius", &Config::headRadius, 0.02f, 0.2f, 0.01f, 100, L"%.0f cm", kTouch),
        toggle(sims, L"Grab their bodies (grip + trigger)", "physical", "grab_bodies", &Config::grabBodies, kTouch),
        number(sims, L"Grab: finger bend", "physical", "grab_curl", &Config::grabCurl, 0, 1, 0.05f, 100, L"%.0f%%", kTouch),
        number(sims, L"Grab: how far you drag them", "physical", "grab_drag", &Config::grabDrag, 0, 0.6f, 0.025f, 100,
               L"%.1f cm", kTouch),
        flag(sims, L"They react to being grabbed", "physical", "grab_reactions", true, kLive),  // bodygrab.py
        scriptNumber(sims, L"They follow you from (0 off)", "body", "follow_distance", 2.5f, 0, 10, 0.5f,
                     L"%.1f m"),  // hint.py: the Sims you talk with follow you
        scriptNumber(sims, L"Talk starts within", "body", "talk_distance", 3, 1, 10, 0.5f,
                     L"%.1f m"),  // scripted.py, reach.py, trail.py: closer, a picked social just starts
        flag(sims, L"Their chats with you stay where you are", "body", "socials_in_place", true, kLive),  // scripted.py

        choice(menus, L"Game menus in first person", "ui", "first_person", &Config::uiFirstPerson,
               {L"Hidden", L"Wrist HUD only", L"Game UI in front only", L"Both"}),
        // The wrist HUD's size here too (user 18:24Z: the size on this page should size what's on the wrist).
        number(menus, L"Wrist HUD size", "wrist", "scale", &Config::wristScale, 0.3f, 3, 0.05f, 100, L"%.0f%%"),
        link(menus, L"Pie menu", L"Menus pie"),
        link(menus, L"Pickers on objects", L"Menus pickers"),
        link(menus, L"Popups", L"Menus popups"),
        link(menus, L"Panels and flat screen", L"Menus panels"),
        toggle(L"Menus pie", L"Own pie menu: poke your stomach", "ui", "self_pie_poke", &Config::selfPiePoke),
        toggle(L"Menus pie", L"Own pie menu: laser on your palm", "ui", "self_pie_palm", &Config::selfPiePalm),
        number(L"Menus pie", L"Pie menu distance", "ui", "pie_distance", &Config::pieDistance, 0.25f, 1, 0.05f, 100,
               L"%.0f cm"),
        number(L"Menus pie", L"Pie menu size", "ui", "pie_width", &Config::pieWidth, 0.4f, 1.5f, 0.04f, 100, L"%.0f cm"),
        toggle(L"Menus pickers", L"Pickers on the object (fridge, TV)", "ui", "object_menus", &Config::objectMenus),
        number(L"Menus pickers", L"Picker size on fronts", "ui", "object_menu_width", &Config::objectMenuWidth, 0.4f, 1.5f, 0.05f,
               100, L"%.0f cm"),
        flag(L"Menus pickers", L"Open Claude on TVs too", "claude", "tvs", false, kLive),  // claude.py
        flag(L"Menus popups", L"Other Sims' decisions: picked at random", "ui", "others_decide", true, kLive),  // objmenu.py
        number(L"Menus popups", L"Popup distance", "ui", "popup_distance", &Config::popupDistance, 0.4f, 1.5f, 0.05f, 100,
               L"%.0f cm"),
        number(L"Menus popups", L"Popup size", "ui", "popup_size", &Config::popupSize, 0.3f, 1.2f, 0.05f, 100, L"%.0f cm a metre"),
        toggle(L"Menus popups", L"No blue dimming behind dialogs", "wrist", "no_dimming", &Config::noDimming),
        number(L"Menus panels", L"Menu panel distance", "ui", "distance", &Config::uiDistance, 0.5f, 3, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Menu panel size", "ui", "width", &Config::uiWidth, 0.5f, 3, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Menu panel height", "ui", "height", &Config::uiHeight, -1, 1, 0.05f, 1, L"%+.2f m"),
        number(L"Menus panels", L"Flat screen distance", "quad", "distance", &Config::quadDistance, 1, 5, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Flat screen size", "quad", "width", &Config::quadWidth, 1, 5, 0.1f, 1, L"%.1f m"),

        // Our own HUD (first person, hud.cpp: drawn with Dear ImGui).
        number(L"HUD", L"Game UI on the wrist (UI button): size", "ui", "wrist_width", &Config::wristWidth, 0.2f, 0.8f,
               0.02f, 100, L"%.0f cm"),
        toggle(L"HUD watch", L"Watch on your left wrist", "wrist", "watch", &Config::wristTime),
        toggle(L"HUD pause", L"Pause symbol while paused", "wrist", "pause", &Config::wristPause),
        toggle(L"HUD sleep", L"Clock in view while asleep", "wrist", "sleep_clock", &Config::wristSleepClock),
        toggle(L"HUD needs", L"Needs inside your right forearm", "wrist", "needs", &Config::wristNeeds),
        toggle(L"HUD moodlets", L"Moodlets inside your right forearm", "wrist", "moodlets", &Config::wristMoodlets),
        toggle(L"HUD queue", L"Action queue on your left forearm", "wrist", "actions", &Config::wristQueue),
        toggle(L"HUD build", L"Build button on your left wrist (off: phone only)", "wrist", "build_button", &Config::wristBuild),
        toggle(L"HUD wants", L"Wants above you", "wrist", "wants", &Config::wristWhims),

        // Bubbles over Sims as cards in the world; your own in front of you (first person).
        toggle(L"Bubbles", L"Bubbles and skill bars in the world", "ui", "bubbles_3d", &Config::bubbles3d),
        toggle(L"Bubbles", L"Yours in front of you (first person)", "bubbles", "own_in_front", &Config::bubbleOwnFront),
        number(L"Bubbles", L"Your bubble size", "bubbles", "own_size", &Config::bubbleOwnSize, 0.05f, 0.5f, 0.01f, 100,
               L"%.0f cm wide"),
        number(L"Bubbles", L"Other Sims' bubble size", "bubbles", "npc_size", &Config::bubbleNpcSize, 0.2f, 1.5f, 0.05f,
               100, L"%.0f cm wide"),
        toggle(L"Bubbles", L"Hide your Sim's bubbles", "bubbles", "hide_own", &Config::bubbleHideOwn),
        toggle(L"Bubbles", L"Hide other Sims' bubbles", "bubbles", "hide_npc", &Config::bubbleHideNpc),
        link(L"Bubbles", L"Where yours sit", L"Bubbles yours"),
        component(L"Bubbles yours", L"Your bubble: right", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 0, -0.5f,
                  0.5f, 0.01f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your bubble: up", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 1, -0.5f, 0.5f,
                  0.01f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your bubble: ahead", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 2, 0.2f,
                  2, 0.02f, 100, L"%.0f cm"),
        component(L"Bubbles yours", L"Your skill bar: right", "bubbles", "own_skill_position", &Config::skillOwnPos, 0, -0.5f,
                  0.5f, 0.01f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your skill bar: up", "bubbles", "own_skill_position", &Config::skillOwnPos, 1, -0.5f,
                  0.5f, 0.01f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your skill bar: ahead", "bubbles", "own_skill_position", &Config::skillOwnPos, 2, 0.2f,
                  2, 0.02f, 100, L"%.0f cm"),

        number(phone, L"Hip: hand below your eyes", "phone", "hip_drop", &Config::phoneHipDrop, 0.1f, 0.9f, 0.02f, 100,
               L"%.0f cm"),
        toggle(phone, L"Buzz when the game shows a popup", "phone", "popup_buzz", &Config::phonePopupBuzz),
        link(phone, L"Position", phonePosition),
        link(phone, L"Hand", phoneHand),
        link(phone, L"Size and screen", phoneSize),
        link(phone, L"Game's phone model", phoneModel),
        link(phone, L"Tablet (build/buy)", tablet),
        toggle(tablet, L"Tablet in first-person build/buy", "tablet", "tablet", &Config::tablet),
        number(tablet, L"Width", "tablet", "width", &Config::tabletWidth, 0.15f, 0.5f, 0.01f, 100, L"%.0f cm"),
        number(tablet, L"Height (0 = fits the layout)", "tablet", "height", &Config::tabletHeight, 0, 0.5f, 0.01f, 100,
               L"%.0f cm"),
        number(tablet, L"Thickness", "tablet", "depth", &Config::tabletDepth, 0.003f, 0.03f, 0.001f, 100, L"%.1f cm"),
        number(tablet, L"Screen border", "tablet", "bezel", &Config::tabletBezel, 0, 0.03f, 0.001f, 100, L"%.1f cm"),
        number(tablet, L"Hand in from the edge", "tablet", "hold", &Config::tabletHold, 0, 0.12f, 0.005f, 100,
               L"%.1f cm"),
        angle(phonePosition, L"Tilt", "phone", "tilt", &Config::phoneTilt),
        angle(phonePosition, L"Turn", "phone", "yaw", &Config::phoneYaw),
        angle(phonePosition, L"Tip back / forward", "phone", "pitch", &Config::phonePitch),
        component(phonePosition, L"Toward the back of the hand", "phone", "phone_offset", &Config::phoneOffset, 0, -0.1f, 0.1f,
                  0.002f, 100, L"%+.1f cm"),
        component(phonePosition, L"Toward the thumb", "phone", "phone_offset", &Config::phoneOffset, 1, -0.1f, 0.15f, 0.002f,
                  100, L"%+.1f cm"),
        component(phonePosition, L"Toward the wrist", "phone", "phone_offset", &Config::phoneOffset, 2, -0.15f, 0.15f, 0.002f,
                  100, L"%+.1f cm"),
        number(phoneHand, L"Finger curl", "phone", "finger_curl", &Config::phoneCurl, 0, 1, 0.05f, 1, L"%.2f"),
        number(phoneHand, L"Thumb bend", "phone", "thumb_bend", &Config::phoneThumbBend, 0, 1, 0.05f, 1, L"%.2f"),
        number(phoneHand, L"Thumb spread", "phone", "thumb_spread", &Config::phoneThumbSpread, -60, 60, 5, 1,
               L"%+.0f\u00b0"),
        component(phoneHand, L"Hand out from the phone", "phone", "hand_offset", &Config::phoneHandOffset, 0, -0.08f, 0.08f,
                  0.002f, 100, L"%+.1f cm"),
        component(phoneHand, L"Hand up the phone", "phone", "hand_offset", &Config::phoneHandOffset, 1, -0.08f, 0.08f,
                  0.002f, 100, L"%+.1f cm"),
        component(phoneHand, L"Hand toward you", "phone", "hand_offset", &Config::phoneHandOffset, 2, -0.08f, 0.08f,
                  0.002f, 100, L"%+.1f cm"),
        component(phoneHand, L"Hand turn", "phone", "hand_turn", &Config::phoneHandTurn, 0, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        component(phoneHand, L"Hand tip", "phone", "hand_turn", &Config::phoneHandTurn, 1, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        component(phoneHand, L"Hand roll", "phone", "hand_turn", &Config::phoneHandTurn, 2, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        number(phoneSize, L"Width", "phone", "phone_width", &Config::phoneWidth, 0.04f, 0.2f, 0.002f, 100, L"%.1f cm"),
        number(phoneSize, L"Height", "phone", "phone_height", &Config::phoneHeight, 0.08f, 0.3f, 0.002f, 100, L"%.1f cm"),
        number(phoneSize, L"Thickness", "phone", "phone_depth", &Config::phoneDepth, 0.003f, 0.03f, 0.001f, 100,
               L"%.1f cm"),
        number(phoneSize, L"Screen border", "phone", "bezel", &Config::phoneBezel, 0, 0.015f, 0.0005f, 100, L"%.2f cm"),
        number(phoneSize, L"Menu crop width", "phone", "crop_width", &Config::phoneCropWidth, 0.1f, 2, 0.05f, 1, L"%.2f"),
        number(phoneSize, L"Menu crop height", "phone", "crop_height", &Config::phoneCropHeight, 0.1f, 1, 0.05f, 1,
               L"%.2f"),
        flag(phoneModel, L"Game's phone model too", "phone", "game_phone", false, kNextPhone),
        component(phoneModel, L"Model: right", "phone", "prop_offset", nullptr, 0, -0.1f, 0.1f, 0.002f, 100, L"%+.1f cm",
                  kNextPhone),
        component(phoneModel, L"Model: up", "phone", "prop_offset", nullptr, 1, -0.1f, 0.1f, 0.002f, 100, L"%+.1f cm",
                  kNextPhone),
        component(phoneModel, L"Model: out of the screen", "phone", "prop_offset", nullptr, 2, -0.1f, 0.1f, 0.002f, 100,
                  L"%+.1f cm", kNextPhone),
        component(phoneModel, L"Model: turn about X", "phone", "prop_rotation", nullptr, 0, -180, 180, 5, 1, L"%.0f°",
                  kNextPhone),
        component(phoneModel, L"Model: turn about Y", "phone", "prop_rotation", nullptr, 1, -180, 180, 5, 1, L"%.0f°",
                  kNextPhone),
        component(phoneModel, L"Model: turn about Z", "phone", "prop_rotation", nullptr, 2, -180, 180, 5, 1, L"%.0f°",
                  kNextPhone),

        toggle(sound, L"Sound follows your head", "audio", "follow_head", &Config::audioFollowsHead, kNextStart),
        toggle(sound, L"Far sounds come from where they are", "audio", "pan_any_distance",
               &Config::audioPanAnyDistance, kAudio),
        flag(sound, L"Outdoor sound comes in through doorways", "audio", "doors_matter", true, kLive),  // outdoors.py
        flag(sound, L"Porches with railings count as outside", "audio", "railings_outside", true, kLive),  // outdoors.py
        scriptNumber(sound, L"A closed door lets through", "audio", "closed_door", 0.35f, 0, 1, 0.05f, L"%.2f"),
        scriptNumber(sound, L"Walls let through", "audio", "wall_leak", 0.25f, 0, 1, 0.05f, L"%.2f"),
    };
    // Each HUD element's own page: where it sits (x y z), how it turns (yaw
    // pitch roll), its size; on an arm also how far round the arm.
    struct HudRows {
        const wchar_t* page;
        const wchar_t* name;
        const char* key;
        float (Config::*pos)[3];
        float (Config::*turn)[3];
        float Config::*around;  // nullptr: around you, not on an arm
        float Config::*size;
        float sizeMin, sizeMax;
    };
    static const HudRows kHudRows[] = {
        {L"HUD watch", L"Watch", "watch", &Config::wristTimePos, &Config::wristTimeTurn, &Config::wristTimeAround,
         &Config::wristTimeSize, 0.03f, 0.3f},
        {L"HUD needs", L"Needs", "needs", &Config::wristNeedsPos, &Config::wristNeedsTurn, &Config::wristNeedsAround,
         &Config::wristNeedsSize, 0.03f, 0.4f},
        {L"HUD moodlets", L"Moodlets", "moodlets", &Config::wristMoodletsPos, &Config::wristMoodletsTurn,
         &Config::wristMoodletsAround, &Config::wristMoodletsSize, 0.03f, 0.4f},
        {L"HUD queue", L"Action queue", "actions", &Config::wristQueuePos, &Config::wristQueueTurn,
         &Config::wristQueueAround, &Config::wristQueueSize, 0.04f, 0.5f},
        {L"HUD build", L"Build button", "build_button", &Config::wristBuildPos, &Config::wristBuildTurn,
         &Config::wristBuildAround, &Config::wristBuildSize, 0.015f, 0.15f},
        {L"HUD wants", L"Wants", "wants", &Config::wristWhimsPos, &Config::wristWhimsTurn, nullptr,
         &Config::wristWhimsSize, 0.05f, 1.2f},
        {L"HUD pause", L"Pause symbol", "pause", &Config::wristPausePos, &Config::wristPauseTurn, nullptr,
         &Config::wristPauseSize, 0.02f, 0.3f},
        {L"HUD sleep", L"Clock while asleep", "sleep", &Config::wristSleepPos, &Config::wristSleepTurn, nullptr,
         &Config::wristSleepSize, 0.05f, 0.6f},
    };
    static std::string keys[sizeof(kHudRows) / sizeof(kHudRows[0])][4];
    for (size_t e = 0; e < sizeof(kHudRows) / sizeof(kHudRows[0]); ++e) {
        const HudRows& h = kHudRows[e];
        rows.push_back(link(L"HUD", h.name, h.page));
        keys[e][0] = std::string(h.key) + "_pos";
        keys[e][1] = std::string(h.key) + "_turn";
        keys[e][2] = std::string(h.key) + "_around";
        keys[e][3] = std::string(h.key) + "_size";
        const bool arm = h.around != nullptr;
        static const wchar_t* const kArmAxes[3] = {L"X: toward the elbow", L"Y: sideways round the arm",
                                                   L"Z: out from the arm bone"};
        static const wchar_t* const kAroundAxes[3] = {L"X: right", L"Y: up", L"Z: ahead"};
        for (int i = 0; i < 3; ++i) {
            float lo = arm ? (i == 2 ? 0.0f : -0.3f) : -1.5f, hi = arm ? (i == 2 ? 0.15f : 0.4f) : 2.0f;
            rows.push_back(component(h.page, arm ? kArmAxes[i] : kAroundAxes[i], "wrist", keys[e][0].c_str(), h.pos, i,
                                     lo, hi, arm ? 0.0025f : 0.01f, 100, arm ? L"%+.2f cm" : L"%+.0f cm"));
        }
        static const wchar_t* const kTurns[3] = {L"Yaw (spin it flat)", L"Pitch (tip it about its width)",
                                                 L"Roll (tip it about its height)"};
        for (int i = 0; i < 3; ++i) {
            Row r = component(h.page, kTurns[i], "wrist", keys[e][1].c_str(), h.turn, i, -180, 180, 5, 1, L"%+.0f\u00b0");
            r.wrap = true;
            rows.push_back(r);
        }
        if (arm) rows.push_back(angle(h.page, L"Round the arm (from the palm side)", "wrist", keys[e][2].c_str(), h.around));
        rows.push_back(number(h.page, L"Size", "wrist", keys[e][3].c_str(), h.size, h.sizeMin, h.sizeMax, 0.005f, 100,
                              L"%.1f cm"));
    }
    for (Row& r : rows) {
        if (r.index >= 0 && r.min <= -180 && r.max >= 180) r.wrap = true;  // the model's turns
        r.page = pageOf(r.group);
        if (r.kind == Kind::Link) r.linkPage = pageOf(r.target);
    }
    // Every page but the first starts with a way back.
    for (int p = 1; p < pageCount(); ++p) {
        Row back;
        back.group = g_pages[p].group;
        back.label = L"\u2039  Back";
        back.kind = Kind::Back;
        back.page = p;
        back.linkPage = parentOf(p);
        rows.push_back(back);
    }
    return rows;
}

std::wstring g_ini;
float g_place[4] = {};  // where the panel opens after it was moved (setPlacement)
bool g_hasPlace = false;
float g_notePlace[3] = {};
bool g_hasNotePlace = false;
Config* g_live = nullptr;
std::vector<Row> g_rows;
std::vector<char> g_flags;    // script mod switches' values
std::vector<float> g_values;  // script mod numbers' values
std::mutex g_valueMutex;      // the values, for the script mod reading them (liveValue) on its thread
std::vector<char> g_changed;  // saved when the menu closes
std::vector<char> g_removed;  // taken out of sims4vr_user.ini when it closes
bool g_open = false;
bool g_renderScaleChanged = false;
ULONGLONG g_lastChange = 0;  // unsaved changes since then (saved a second later, or when the menu closes)
int g_selected = 0;
ULONGLONG g_selectedAt = 0;  // when the picked row changed (its tip shows a moment later)
// The Close button picked with the stick (up past the page's first row); A or
// right on it closes the menu (user 2026-10-18 22:49Z: only B spam or the
// laser reached it).
constexpr int kCloseRow = -2;
bool g_closeWanted = false;
bool g_tipDrawn = false;
constexpr ULONGLONG kTipDelay = 500;
int g_top = 0;  // first line shown
ULONGLONG g_resetArmed = 0;
ULONGLONG g_switchedAt = 0;  // the view switch was asked for (until first person comes or goes)
ULONGLONG g_doneAt = 0;      // a testing button was pressed (its row shows "Done" for a moment)
size_t g_doneRow = 0;
// An Integrated Body Physics button that asks first (Scale the others from Medium): its row, when it was pressed, and
// whether Yes is picked (No until the stick says otherwise).
size_t g_confirmRow = SIZE_MAX;
ULONGLONG g_confirmAt = 0;
bool g_confirmYes = false;
bool confirming(size_t row) { return g_confirmRow == row && GetTickCount64() - g_confirmAt < 15000; }
// The game's switches as the script mod reports them: kCheatsOn testing cheats,
// kCheatPiesOn every pie menu the cheat menu.
constexpr int kCheatsOn = 1, kCheatPiesOn = 2;
std::atomic<int> g_cheats{0};
int g_cheatsShown = 0;
int cheatBit(const Row& r) { return r.action == 11 ? kCheatPiesOn : kCheatsOn; }
bool g_bodyShown = false;    // first person, as the picture shows it

// The page's list as shown: on the first page a heading line (row -1) before
// each group; other pages start with their Back row.
struct Line {
    int row;
    const wchar_t* heading;
};
std::vector<Line> g_lines;
int g_page = 0;  // kept while the menu is closed: it opens where you left it
// The page the menu opens on next (user 2026-10-15 01:06Z, v0.13.127): the one
// you closed it on, or, when you closed it with B on the first page after going
// back with B, the page you went back from (with its row). Kept across game
// starts in sims4vr_user.ini, [ui] settings_page = page|row.
int g_backFrom = -1, g_backFromRow = -1;  // where the latest run of B presses started
bool g_backing = false;                   // the last move in the menu was B going back a page
int g_reopenPage = -1, g_reopenRow = -1;
std::string g_savedPage;                  // what settings_page holds

void buildLines() {
    if (!pageShown(g_page)) g_page = 0;  // its mod isn't loaded (any more)
    g_lines.clear();
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].kind == Kind::Back && g_rows[i].page == g_page) g_lines.push_back({static_cast<int>(i), nullptr});
    const wchar_t* group = nullptr;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.page != g_page || r.kind == Kind::Back) continue;
        if (r.kind == Kind::Link && (r.linkPage <= 0 || !pageShown(r.linkPage))) continue;
        if (g_page == 0 && (!group || wcscmp(group, r.group) != 0)) {
            group = r.group;
            g_lines.push_back({-1, group});
        }
        g_lines.push_back({static_cast<int>(i), nullptr});
    }
}

// How many settings a page holds, its own pages' too (shown on the rows that open it).
int pageSize(int page, int depth = 0) {
    int n = 0;
    for (const Row& r : g_rows) {
        if (r.page != page || r.kind == Kind::Back) continue;
        if (r.kind == Kind::Link && (r.linkPage <= 0 || !pageShown(r.linkPage))) continue;
        // depth: another mod's menu could link round in a circle
        n += r.kind == Kind::Link && r.linkPage != page ? depth < 8 ? pageSize(r.linkPage, depth + 1) : 0 : 1;
    }
    return n;
}

// Stick repeat: a step when pushed, then again while held. 9 = wait for the
// stick to come back to the centre (it was already pushed when the menu opened).
int g_dirX = 9, g_dirY = 9;
ULONGLONG g_nextX = 0, g_nextY = 0;
int g_repeatsX = 0, g_repeatsY = 0;
bool g_pressWas = true;

constexpr int kWidth = 1024, kHeight = 820;
constexpr int kTitleHeight = 88, kListTop = 100, kRowHeight = 52, kShown = 12;
constexpr int kListBottom = kListTop + kRowHeight * kShown;
constexpr int kValueLeft = 640, kArrow = 44, kValueRight = 960;
constexpr int kCloseLeft = 836, kCloseTop = 22, kCloseRight = 990, kCloseBottom = 66;

std::vector<uint32_t> g_pixels;
uint64_t g_version = 0;
bool g_redraw = true;

float& numberRef(size_t i) {
    const Row& r = g_rows[i];
    if (r.number) return g_live->*r.number;
    if (r.triple && r.index >= 0 && r.index < 3) return (g_live->*r.triple)[r.index];
    return g_values[i];
}

// The rows that share a three-number key, by index (-1 where missing).
void siblings(size_t i, int out[3]) {
    out[0] = out[1] = out[2] = -1;
    const Row& r = g_rows[i];
    for (size_t j = 0; j < g_rows.size(); ++j) {
        const Row& o = g_rows[j];
        if (o.index >= 0 && o.index < 3 && o.key && !strcmp(o.section, r.section) && !strcmp(o.key, r.key))
            out[o.index] = static_cast<int>(j);
    }
}

bool toggleValue(size_t i) {
    const Row& r = g_rows[i];
    if (r.action) return (g_cheats & cheatBit(r)) != 0;
    return r.toggle ? g_live->*r.toggle : g_flags[i] != 0;
}

void setToggle(size_t i, bool on) {
    const Row& r = g_rows[i];
    if (r.action) {
        // Cheat pie menus need testing cheats: on turns both on, testing cheats off turns both off.
        const int bit = cheatBit(r);
        if (on)
            g_cheats |= bit == kCheatPiesOn ? kCheatsOn | kCheatPiesOn : kCheatsOn;
        else
            g_cheats &= bit == kCheatsOn ? 0 : ~kCheatPiesOn;
    }
    else if (r.toggle)
        g_live->*r.toggle = on;
    else
        g_flags[i] = on;
}

// true/false, yes/no, on/off or a number, as config.cpp reads them.
bool parseFlag(std::string s, bool fallback) {
    for (char& ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (s.empty()) return fallback;
    if (s == "true" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "no" || s == "off") return false;
    return strtol(s.c_str(), nullptr, 0) != 0;
}

std::string valueText(size_t i) {
    const Row& r = g_rows[i];
    char buf[96];
    switch (r.kind) {
        case Kind::Number:
            if (r.index >= 0) {
                int rows[3];
                siblings(i, rows);
                double v[3];
                for (int k = 0; k < 3; ++k) v[k] = rows[k] >= 0 ? numberRef(rows[k]) : 0.0;
                snprintf(buf, sizeof(buf), "%.4g %.4g %.4g", v[0], v[1], v[2]);
            } else {
                snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(numberRef(i)));
            }
            break;
        case Kind::Toggle: snprintf(buf, sizeof(buf), "%d", toggleValue(i) ? 1 : 0); break;
        case Kind::Choice: snprintf(buf, sizeof(buf), "%d", g_live->*r.choice); break;
        default: buf[0] = 0; break;
    }
    return buf;
}

void applyChange(Apply apply) {
    Config& c = *g_live;
    switch (apply) {
        case kView:
            camera::configureView(c.bodySmoothing, c.bodyTurnSmoothing, c.bodyEyeInset, c.bodyNearClip,
                                  c.bodyNearAdaptive, c.bodyNearMax);
            camera::setSeatFollow(c.bodySeatFollow);
            armik::setSeatTorso(c.bodySeatTorso);
            facehide::setEnabled(c.bodyHideFace);
            break;
        case kWalk:
            player::configure(c.walkSpeed, c.runSpeed, c.bodyForward, c.bodyHeight, c.touchBodies && c.armIk);
            player::setKeepHeight(c.bodyKeepHeight);
            player::setSmoothSlide(c.bodySmoothSlide);
            player::setStopTime(c.bodyStopTime);
            walkstop::setClips(c.bodyWalkClips);
            break;
        case kHands: armik::configureHands(c.handPitch, c.headTracking); break;
        case kTouch:
            armik::configureTouch(c.touchBodies, c.holdHands, c.lookAtYou, c.grabBodies);
            armik::configureGrab(c.grabCurl, c.grabDrag);
            player::setHeadRepel(c.headRepel, c.headRadius);
            player::configure(c.walkSpeed, c.runSpeed, c.bodyForward, c.bodyHeight, c.touchBodies && c.armIk);
            break;
        case kRenderScale: g_renderScaleChanged = true; break;
        case kShadows: shadows::configure(c.steadyShadows, c.shadowStep, c.shadowSharp); break;
        case kAudio: camera::setPanAnyDistance(c.audioPanAnyDistance); break;
        default: break;
    }
}

using JiggleGetFn = othermods::GetFn;
using JiggleSetFn = othermods::SetFn;

// Integrated Body Physics' (or, for a sound row, Extended Sound Control's) settings calls, while its DLL is loaded.
bool jiggleApi(JiggleGetFn& get, JiggleSetFn& set, bool sound = false) {
    return othermods::api(sound ? othermods::kSoundControl : othermods::kBodyPhysics, get, set);
}

// A link of another mod's menu with when=<key> opens only while the mod's
// value for that key is not 0 (Integrated Body Physics "Configure current
// Sim" while a Sim is frozen); till then it is greyed out with its off tip.
bool linkOpen(const Row& r) {
    if (!r.when) return true;
    JiggleGetFn get;
    JiggleSetFn set;
    float v = 0;
    return jiggleApi(get, set, r.sound) && get(r.when, &v) && v != 0;
}

// Its rows show what the mod has now.
void readJiggle() {
    JiggleGetFn get[2];
    JiggleSetFn set[2];
    bool have[2] = {jiggleApi(get[0], set[0], false), jiggleApi(get[1], set[1], true)};
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        float v = 0;
        if (!r.jiggle || !have[r.sound] || !get[r.sound](r.jiggle, &v)) continue;
        if (r.kind == Kind::Toggle)
            g_flags[i] = v != 0;
        else
            g_values[i] = v;
    }
}

// A number field of a menu export ("." the decimal point, whatever the locale).
float menuNumber(const std::wstring& s) {
    size_t i = 0;
    float sign = 1, value = 0, scale = 0;
    if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) sign = s[i++] == L'-' ? -1.0f : 1.0f;
    for (; i < s.size(); ++i) {
        if (s[i] == L'.' && scale == 0) {
            scale = 1;
        } else if (s[i] >= L'0' && s[i] <= L'9') {
            value = value * 10 + (s[i] - L'0');
            scale *= 10;
        } else {
            break;
        }
    }
    return sign * (scale > 1 ? value / scale : value);
}

// A number format from a menu export: exactly one %f/%g/%e (with flags, width,
// precision) and any %%; anything else could read past the one value we pass.
bool safeFormat(const std::wstring& f) {
    int conversions = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != L'%') continue;
        if (++i < f.size() && f[i] == L'%') continue;
        while (i < f.size() && wcschr(L"-+ 0#", f[i])) ++i;
        while (i < f.size() && iswdigit(f[i])) ++i;
        if (i < f.size() && f[i] == L'.')
            for (++i; i < f.size() && iswdigit(f[i]);) ++i;
        if (i >= f.size() || !wcschr(L"fFgGeE", f[i])) return false;
        ++conversions;
    }
    return conversions == 1;
}

// Another mod's menu export (othermods::menu, Integrated Body Physics'
// docs/MENU-EXPORT.md; user 2026-10-17 16:09Z): its pages and rows join ours
// the first time the menu opens with that mod loaded, so its layout changes
// with no Sims4VR build. Rows flagged novr (only without VR), key rows (its
// keyboard hotkey, off while Sims4VR is loaded) and kinds or flags we don't
// know are left out.
bool mergeMenu(othermods::Mod mod, const wchar_t* prefix) {
    const wchar_t* text = othermods::menu(mod);
    if (!text) return false;
    static std::deque<std::wstring> texts;  // pages and rows keep pointers to these
    static std::deque<std::string> keys;
    auto keep = [](std::wstring s) {
        texts.push_back(std::move(s));
        return texts.back().c_str();
    };
    auto pageId = [&](const std::wstring& id) { return keep(prefix + id); };
    auto key = [](const std::wstring& k) {
        std::string s;
        for (wchar_t c : k) s += c < 128 ? static_cast<char>(c) : '?';
        keys.push_back(s);
        return keys.back().c_str();
    };
    const int pagesBefore = pageCount();
    std::vector<Row> rows;
    int skipped = 0;
    for (const wchar_t* at = text; *at;) {
        const wchar_t* end = wcschr(at, L'\n');
        std::wstring line(at, end ? end : at + wcslen(at));
        at = end ? end + 1 : at + line.size();
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::wstring> f;
        for (size_t a = 0;;) {
            size_t b = line.find(L'\t', a);
            f.push_back(line.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
        f.resize(std::max<size_t>(f.size(), 11));
        const std::wstring& kind = f[0];
        if (kind == L"page") {
            if (f[1].empty()) continue;
            Page page{pageId(f[1]), keep(f[3]), f[2].empty() ? L"" : pageId(f[2])};
            if (f[2].empty()) page.mod = mod;  // its first page: shown while the mod is loaded
            g_pages.push_back(page);
            continue;
        }
        // A row's flags: vr shows, novr or one we don't know leaves the row out;
        // when=<key> (links only) is handed back in `when`.
        auto flagsShow = [](const std::wstring& flags, std::wstring* when) {
            bool show = true;
            for (size_t a = 0; a <= flags.size();) {
                size_t b = flags.find(L',', a);
                std::wstring flag = flags.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
                flag.erase(0, flag.find_first_not_of(L' '));
                flag.erase(flag.find_last_not_of(L' ') + 1);
                if (when && flag.compare(0, 5, L"when=") == 0 && flag.size() > 5)
                    *when = flag.substr(5);
                else if (!flag.empty() && flag != L"vr")
                    show = false;
                if (b == std::wstring::npos) break;
                a = b + 1;
            }
            return show;
        };
        if (kind == L"link") {
            // page, label, target[, flags, tip, off tip]
            std::wstring when;
            if (!flagsShow(f[4], &when)) {
                ++skipped;
                continue;
            }
            Row r = link(pageId(f[1]), keep(f[2]), pageId(f[3]));
            r.tip = f[5].empty() ? nullptr : keep(f[5]);
            if (!when.empty()) {
                r.when = key(when);
                r.offTip = f[6].empty() ? nullptr : keep(f[6]);
            }
            r.sound = mod == othermods::kSoundControl;
            rows.push_back(r);
            continue;
        }
        const bool isNumber = kind == L"number";
        if (!isNumber && kind != L"switch" && kind != L"confirm") {
            ++skipped;  // key rows and kinds we don't know
            continue;
        }
        const bool show = flagsShow(f[isNumber ? 9 : 4], nullptr);
        if (!show || f[3].empty()) {
            ++skipped;
            continue;
        }
        const wchar_t* tip = f[isNumber ? 10 : 5].empty() ? nullptr : keep(f[isNumber ? 10 : 5]);
        if (isNumber) {
            float min = menuNumber(f[4]), max = menuNumber(f[5]), step = menuNumber(f[6]), shown = menuNumber(f[7]);
            if (!(max > min) || !(step > 0)) {
                ++skipped;
                continue;
            }
            rows.push_back(jiggleNumber(pageId(f[1]), keep(f[2]), key(f[3]), min, max, step, shown != 0 ? shown : 1,
                                        safeFormat(f[8]) ? keep(f[8]) : L"%.2f", tip));
        } else if (kind == L"switch") {
            rows.push_back(jiggleSwitch(pageId(f[1]), keep(f[2]), key(f[3]), tip));
        } else {
            rows.push_back(jiggleConfirm(pageId(f[1]), keep(f[2]), key(f[3]), tip));
        }
        rows.back().sound = mod == othermods::kSoundControl;
    }
    std::lock_guard<std::mutex> lock(g_valueMutex);  // liveValue reads the rows on the script mod's thread
    size_t added = 0;
    for (Row& r : rows) {
        r.page = pageOf(r.group);
        if (r.page < pagesBefore) {  // not one of its own pages
            ++skipped;
            continue;
        }
        if (r.kind == Kind::Link) r.linkPage = pageOf(r.target);
        g_rows.push_back(r);
        ++added;
    }
    for (int p = pagesBefore; p < pageCount(); ++p) {
        Row back;
        back.group = g_pages[p].group;
        back.label = L"‹  Back";
        back.kind = Kind::Back;
        back.page = p;
        back.linkPage = parentOf(p);
        g_rows.push_back(back);
    }
    for (Row& r : g_rows)
        if (r.kind == Kind::Link) r.linkPage = pageOf(r.target);  // ours to its pages (Other mods)
    g_flags.resize(g_rows.size(), 0);
    g_values.resize(g_rows.size(), 0.0f);
    g_changed.resize(g_rows.size(), 0);
    g_removed.resize(g_rows.size(), 0);
    LOG("Settings: %ls's menu: %d pages, %zu rows (%d left out)", prefix, pageCount() - pagesBefore, added, skipped);
    return true;
}

// The other mods' menus, once each, as soon as they are loaded. True when one came in now.
bool mergeMenus() {
    static bool merged[othermods::kModCount] = {};
    bool any = false;
    if (!merged[othermods::kBodyPhysics] && mergeMenu(othermods::kBodyPhysics, L"ibp:"))
        any = merged[othermods::kBodyPhysics] = true;
    if (!merged[othermods::kSoundControl] && mergeMenu(othermods::kSoundControl, L"esc:"))
        any = merged[othermods::kSoundControl] = true;
    return any;
}

void changed(size_t i) {
    if (const char* key = g_rows[i].jiggle) {
        JiggleGetFn get;
        JiggleSetFn set;
        float v = g_rows[i].kind == Kind::Toggle ? (g_flags[i] ? 1.0f : 0.0f) : g_values[i];
        if (jiggleApi(get, set, g_rows[i].sound))
            set(key, v);
        else
            LOG("Settings: %s %s = %g, but that mod isn't loaded", g_rows[i].sound ? "Extended Sound Control" :
                "Integrated Body Physics", key, v);
        g_redraw = true;
        return;
    }
    g_lastChange = GetTickCount64();
    // A three-number key is saved whole: its other rows count as changed too.
    if (g_rows[i].index >= 0) {
        int rows[3];
        siblings(i, rows);
        for (int k : rows)
            if (k >= 0) {
                g_changed[k] = 1;
                g_removed[k] = 0;
            }
    }
    g_changed[i] = 1;
    g_removed[i] = 0;
    applyChange(g_rows[i].apply);
    g_redraw = true;
}

// Every row back to sims4vr.ini's value (the built-in one if it has none).
void resetAll() {
    static const Config defaults;
    std::lock_guard<std::mutex> lock(g_valueMutex);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (!r.key) continue;
        std::string s = readShippedSetting(g_ini, r.section, r.key, "");
        switch (r.kind) {
            case Kind::Number: {
                float fallback = r.number ? defaults.*r.number : r.triple ? (defaults.*r.triple)[r.index] : r.fallbackNumber;
                float v[3];
                if (r.index >= 0)
                    numberRef(i) = sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3 ? v[r.index] : fallback;
                else
                    numberRef(i) = s.empty() ? fallback : static_cast<float>(atof(s.c_str()));
                break;
            }
            case Kind::Toggle: setToggle(i, parseFlag(s, r.toggle ? defaults.*r.toggle : r.fallback)); break;
            case Kind::Choice: g_live->*r.choice = s.empty() ? defaults.*r.choice : static_cast<int>(strtol(s.c_str(), nullptr, 0)); break;
            default: break;
        }
        g_changed[i] = 0;
        g_removed[i] = 1;
    }
    for (Apply a : {kView, kWalk, kHands, kTouch, kRenderScale, kShadows, kAudio}) applyChange(a);
    g_lastChange = GetTickCount64();
    g_redraw = true;
    LOG("Settings: everything back to sims4vr.ini's values");
}

void openPage(int page, int selectRow);
void goBack();

// dir -1 / +1: down / up a step; 0: A (switch, or the next option).
bool change(size_t i, int dir) {
    const Row& r = g_rows[i];
    g_backing = false;  // something done here: the menu reopens on this page
    switch (r.kind) {
        case Kind::Number: {
            if (!dir) return false;
            float& v = numberRef(i);
            float k = std::round((v - r.min) / r.step) + static_cast<float>(dir);
            float next = r.min + k * r.step;
            if (r.wrap) {  // round the circle: 180 then -175
                float span = r.max - r.min;
                if (next > r.max + 1e-4f) next -= span;
                if (next <= r.min + 1e-4f && dir < 0 && v <= r.min + 1e-4f) next += span;
            }
            next = std::clamp(next, r.min, r.max);
            if (std::fabs(next - v) < 1e-6f) return false;
            std::lock_guard<std::mutex> lock(g_valueMutex);
            v = next;
            break;
        }
        case Kind::Toggle: {
            bool on = toggleValue(i), want = dir ? dir > 0 : !on;
            if (want == on) return false;
            std::lock_guard<std::mutex> lock(g_valueMutex);
            setToggle(i, want);
            if (r.action) {  // the game's switch: the script mod does it (and reports back)
                grab::pushControl(want ? r.action : r.action == 11 ? 12 : 9);
                g_redraw = true;
                LOG("Settings: %s %s", r.action == 11 ? "cheat pie menus" : "testing cheats", want ? "on" : "off");
                return true;
            }
            break;
        }
        case Kind::Choice: {
            int& c = g_live->*r.choice;
            int n = static_cast<int>(r.names.size());
            int next = dir ? std::clamp(c + dir, 0, n - 1) : (c + 1) % n;
            if (next == c) return false;
            c = next;
            break;
        }
        case Kind::Reset: {
            if (dir) return false;
            ULONGLONG now = GetTickCount64();
            if (g_resetArmed && now - g_resetArmed < 4000) {
                g_resetArmed = 0;
                resetAll();
            } else {
                g_resetArmed = now;
            }
            g_redraw = true;
            return true;
        }
        case Kind::Link:
            if (dir < 0) return false;
            if (!linkOpen(r)) {  // greyed out: its tip says why
                g_redraw = true;
                return true;
            }
            openPage(r.linkPage, -1);
            return true;
        case Kind::Back:
            if (dir > 0) return false;
            goBack();
            return true;
        case Kind::Action: {
            if (r.jiggle) {  // asks first: A, then the stick to Yes, then A
                if (!confirming(i)) {
                    if (dir) return false;
                    g_confirmRow = i;
                    g_confirmAt = GetTickCount64();
                    g_confirmYes = false;
                } else if (dir) {
                    if ((dir > 0) == g_confirmYes) return false;
                    g_confirmYes = dir > 0;
                    g_confirmAt = GetTickCount64();
                } else {
                    JiggleGetFn get;
                    JiggleSetFn set;
                    if (g_confirmYes && jiggleApi(get, set, r.sound)) {
                        set(r.jiggle, 1);
                        readJiggle();
                        g_doneAt = GetTickCount64();
                        g_doneRow = i;
                        LOG("Settings: %ls", r.label);
                    }
                    g_confirmRow = SIZE_MAX;
                }
                g_redraw = true;
                return true;
            }
            if (r.action == 1) {
                // The view: stick left for the game camera, right for first
                // person, like the other rows; A no longer switches it (user
                // 2026-10-14 22:08Z, v0.13.113).
                if (!dir || g_switchedAt || (dir > 0) == camera::bodyActive()) return false;
                grab::pushControl(r.action);
                g_redraw = true;
                g_switchedAt = GetTickCount64();
                LOG("Settings: %ls", dir < 0 ? L"to the game camera" : L"to first person");
                return true;
            }
            if (dir) return false;
            grab::pushControl(r.action);
            g_redraw = true;
            if (r.action != 1) {
                if (r.action == 5 || r.action == 7 || r.action == 8 || r.action == 10)
                    g_cheats |= kCheatsOn;  // the testing buttons turn testing cheats on too (controls.py), not cheat pie menus
                g_doneAt = GetTickCount64();
                g_doneRow = i;
                LOG("Settings: %ls", r.label);
                return true;
            }
            return true;
        }
    }
    changed(i);
    return true;
}

int lineOf(int row) {
    for (size_t j = 0; j < g_lines.size(); ++j)
        if (g_lines[j].row == row) return static_cast<int>(j);
    return 0;
}

// Scrolls so the picked row shows (with its heading when it's the group's first).
void reveal() {
    int line = lineOf(g_selected);
    int want = line > 0 && g_lines[line - 1].row < 0 ? line - 1 : line;
    if (want < g_top) g_top = want;
    if (line >= g_top + kShown) g_top = line - kShown + 1;
    g_top = std::clamp(g_top, 0, std::max(0, static_cast<int>(g_lines.size()) - kShown));
}

bool select(int row) {
    row = std::clamp(row, 0, static_cast<int>(g_rows.size()) - 1);
    if (row == g_selected) return false;
    g_selected = row;
    g_selectedAt = GetTickCount64();
    g_tipDrawn = false;
    g_resetArmed = 0;
    reveal();
    g_redraw = true;
    return true;
}

// The row `by` lines below this one on the page (headings skipped), or itself at the ends.
int stepRow(int row, int by) {
    int line = lineOf(row);
    for (int j = line + by; j >= 0 && j < static_cast<int>(g_lines.size()); j += by)
        if (g_lines[j].row >= 0) return g_lines[j].row;
    return row;
}

// The page's top row (its Back row on other pages).
int firstRow() {
    for (const Line& line : g_lines)
        if (line.row >= 0) return line.row;
    return 0;
}

bool selectClose() {
    if (g_selected == kCloseRow) return false;
    g_selected = kCloseRow;
    g_selectedAt = GetTickCount64();
    g_tipDrawn = false;
    g_resetArmed = 0;
    g_top = 0;
    g_redraw = true;
    return true;
}

// Shows a page with selectRow picked (-1: its first setting, past Back).
void openPage(int page, int selectRow) {
    g_backing = false;
    g_page = std::clamp(page, 0, pageCount() - 1);
    buildLines();
    g_top = 0;
    g_resetArmed = 0;
    g_confirmRow = SIZE_MAX;
    bool listed = false;  // a hidden row (another mod's link) can't stay picked
    for (const Line& line : g_lines) listed = listed || line.row == selectRow;
    if (selectRow < 0 || g_rows[selectRow].page != g_page || !listed) {
        selectRow = 0;
        for (const Line& line : g_lines)
            if (line.row >= 0) {
                selectRow = line.row;
                if (g_rows[line.row].kind != Kind::Back) break;
            }
    }
    g_selected = selectRow;
    g_selectedAt = GetTickCount64();
    g_tipDrawn = false;
    reveal();
    g_redraw = true;
    LOG("Settings: page %ls", g_page ? g_pages[g_page].group : L"first");
}

// Up a page, with the row that opened this one picked.
void goBack() {
    int from = g_page, to = parentOf(from), opener = -1;
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].kind == Kind::Link && g_rows[i].linkPage == from && g_rows[i].page == to) opener = static_cast<int>(i);
    if (!g_backing) {
        g_backFrom = from;
        g_backFromRow = g_selected;
    }
    openPage(to, opener);
    g_backing = true;
}

// -1 / +1 when the stick pushes that way now (a step, then repeats while held).
// Held longer, it repeats faster.
int stickStep(float v, int& dir, ULONGLONG& next, int& repeats, ULONGLONG now) {
    float a = std::fabs(v);
    if (a < 0.35f) {
        dir = 0;
        return 0;
    }
    if (dir == 9 || a < 0.6f) return 0;
    int want = v > 0 ? 1 : -1;
    if (want != dir) {
        dir = want;
        next = now + 400;
        repeats = 0;
        return want;
    }
    if (now >= next) {
        next = now + (++repeats > 8 ? 50 : 110);
        return want;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Drawing: shapes straight into the pixels (antialiased), text with GDI on top.

struct Canvas {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    uint32_t* bits = nullptr;
    HFONT title = nullptr, row = nullptr, small = nullptr, heading = nullptr;
};
Canvas g_canvas;

bool makeCanvas() {
    if (g_canvas.bits) return true;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_canvas.dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    void* bits = nullptr;
    g_canvas.bitmap = CreateDIBSection(g_canvas.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_canvas.dc || !g_canvas.bitmap || !bits) {
        LOG("Settings: couldn't make the menu's picture (GDI)");
        return false;
    }
    SelectObject(g_canvas.dc, g_canvas.bitmap);
    auto font = [](int pixels, int weight) {
        return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    g_canvas.title = font(40, FW_SEMIBOLD);
    g_canvas.row = font(30, FW_NORMAL);
    g_canvas.small = font(23, FW_NORMAL);
    g_canvas.heading = font(24, FW_BOLD);
    SetBkMode(g_canvas.dc, TRANSPARENT);
    g_canvas.bits = static_cast<uint32_t*>(bits);
    return true;
}

constexpr uint32_t rgb(int r, int g, int b) {
    return 0xff000000u | static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b);
}
constexpr uint32_t kBackground = rgb(22, 26, 34), kSelected = rgb(46, 74, 122), kText = rgb(236, 238, 242),
                   kDim = rgb(140, 148, 162), kAccent = rgb(120, 176, 255), kOn = rgb(56, 166, 92),
                   kOff = rgb(74, 80, 92), kBorder = rgb(84, 104, 140), kWarn = rgb(255, 190, 90),
                   kTrack = rgb(38, 44, 56), kThumb = rgb(112, 122, 142), kTipBack = rgb(30, 36, 48);

uint32_t blend(uint32_t under, uint32_t over, float a) {
    auto ch = [&](int shift) {
        float u = static_cast<float>(under >> shift & 0xff), o = static_cast<float>(over >> shift & 0xff);
        return static_cast<uint32_t>(u + (o - u) * a + 0.5f) << shift;
    };
    return 0xff000000u | ch(16) | ch(8) | ch(0);
}

// Signed distance from a rounded rectangle's edge (negative inside).
float roundedDistance(float x, float y, float x0, float y0, float x1, float y1, float radius) {
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    float qx = std::fabs(x - cx) - ((x1 - x0) / 2 - radius), qy = std::fabs(y - cy) - ((y1 - y0) / 2 - radius);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

void fillRounded(int x0, int y0, int x1, int y1, float radius, uint32_t color) {
    for (int y = std::max(0, y0); y < std::min(kHeight, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(kWidth, x1); ++x) {
            float d = roundedDistance(x + 0.5f, y + 0.5f, static_cast<float>(x0), static_cast<float>(y0),
                                      static_cast<float>(x1), static_cast<float>(y1), radius);
            float a = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (a > 0) g_canvas.bits[y * kWidth + x] = blend(g_canvas.bits[y * kWidth + x], color, a);
        }
}

// A small triangle pointing left (dir -1) or right (+1), centred in the box.
void arrow(int x0, int y0, int x1, int y1, int dir, uint32_t color) {
    float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f, half = 9, depth = 8;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float px = (x + (sx + 0.5f) / 4 - cx) * dir, py = y + (sy + 0.5f) / 4 - cy;
                    // tip at +depth, base at -depth spanning +-half
                    if (px >= -depth && px <= depth && std::fabs(py) <= half * (depth - px) / (2 * depth)) ++inside;
                }
            if (inside) g_canvas.bits[y * kWidth + x] = blend(g_canvas.bits[y * kWidth + x], color, inside / 16.0f);
        }
}

void text(const wchar_t* s, HFONT font, uint32_t color, int x0, int y0, int x1, int y1, UINT align) {
    SelectObject(g_canvas.dc, font);
    SetTextColor(g_canvas.dc, RGB(color >> 16 & 0xff, color >> 8 & 0xff, color & 0xff));
    RECT r{x0, y0, x1, y1};
    DrawTextW(g_canvas.dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

int textWidth(const wchar_t* s, HFONT font) {
    SelectObject(g_canvas.dc, font);
    SIZE size{};
    GetTextExtentPoint32W(g_canvas.dc, s, static_cast<int>(wcslen(s)), &size);
    return size.cx;
}

// A page's title: its parents' names and its own ("Wrist and HUD › Watch"); the
// top ones give way to "…" while it is wider than width.
std::wstring pageTitle(int page, int width) {
    if (page <= 0 || page >= pageCount()) return g_pages[0].name;
    std::vector<const wchar_t*> names;
    for (int p = page, guard = 0; p > 0 && guard < 8; p = parentOf(p), ++guard) names.insert(names.begin(), g_pages[p].name);
    for (size_t first = 0;; ++first) {
        std::wstring title = first ? L"\u2026" : L"";
        for (size_t i = first; i < names.size(); ++i) {
            if (!title.empty()) title += L" \u203a ";
            title += names[i];
        }
        if (first + 1 >= names.size() || textWidth(title.c_str(), g_canvas.title) <= width) return title;
    }
}

const wchar_t* noteOf(const Row& r) {
    switch (r.apply) {
        case kRenderScale: return L"when you close this";
        case kNextStart: return L"next start";
        case kNextPhone: return L"next time it's out";
        default: return nullptr;
    }
}

void shownValue(size_t i, wchar_t* out, size_t size) {
    const Row& r = g_rows[i];
    if (r.kind == Kind::Number)
        swprintf(out, size, r.format, static_cast<double>(numberRef(i) * r.shown));
    else if (r.kind == Kind::Choice)
        swprintf(out, size, L"%ls", r.names[std::clamp(g_live->*r.choice, 0, static_cast<int>(r.names.size()) - 1)]);
    else
        out[0] = 0;
}

// The picked row's tip: a box under the row (over it when there's no room),
// once the row has stayed picked for a moment.
void drawTip() {
    if (g_selected < 0 || g_selected >= static_cast<int>(g_rows.size())) return;
    const Row& row = g_rows[g_selected];
    const wchar_t* tip = row.kind == Kind::Link && !linkOpen(row) ? row.offTip : row.tip;
    if (!tip || GetTickCount64() - g_selectedAt < kTipDelay) return;
    int line = lineOf(g_selected);
    if (line < g_top || line >= g_top + kShown) return;
    g_tipDrawn = true;
    constexpr int kLeft = 60, kRight = kWidth - 60, kPad = 18;
    SelectObject(g_canvas.dc, g_canvas.small);
    RECT size{kLeft + kPad, 0, kRight - kPad, 0};
    DrawTextW(g_canvas.dc, tip, -1, &size, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int height = static_cast<int>(size.bottom - size.top) + 2 * kPad;
    int rowTop = kListTop + (line - g_top) * kRowHeight, rowBottom = rowTop + kRowHeight;
    int y0 = rowBottom - 2;
    if (y0 + height > kHeight - 12) y0 = rowTop + 2 - height;
    fillRounded(kLeft - 2, y0 - 2, kRight + 2, y0 + height + 2, 14, kBorder);
    fillRounded(kLeft, y0, kRight, y0 + height, 12, kTipBack);
    GdiFlush();
    RECT r{kLeft + kPad, y0 + kPad, kRight - kPad, y0 + height - kPad};
    SetTextColor(g_canvas.dc, RGB(kText >> 16 & 0xff, kText >> 8 & 0xff, kText & 0xff));
    DrawTextW(g_canvas.dc, tip, -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
    GdiFlush();
}

void draw() {
    uint32_t* px = g_canvas.bits;
    std::fill(px, px + kWidth * kHeight, kBackground);
    const int last = std::min(static_cast<int>(g_lines.size()), g_top + kShown);
    const bool resetArmed = g_resetArmed && GetTickCount64() - g_resetArmed < 4000;

    // Shapes.
    fillRounded(kCloseLeft, kCloseTop, kCloseRight, kCloseBottom, 12, g_selected == kCloseRow ? kSelected : kOff);
    fillRounded(24, kTitleHeight, kWidth - 24, kTitleHeight + 2, 1, kTrack);
    fillRounded(kWidth / 2 - 56, 9, kWidth / 2 + 56, 15, 3, kTrack);  // a handle: the title bar moves the menu
    for (int j = g_top; j < last; ++j) {
        const Line& line = g_lines[j];
        int y0 = kListTop + (j - g_top) * kRowHeight, y1 = y0 + kRowHeight;
        if (line.row < 0) continue;
        const Row& r = g_rows[line.row];
        if (line.row == g_selected) fillRounded(24, y0 + 3, kWidth - 34, y1 - 3, 10, kSelected);
        if (r.kind == Kind::Number || r.kind == Kind::Choice) {
            bool low, high;
            if (r.kind == Kind::Number) {
                float v = numberRef(line.row);
                low = r.wrap || v > r.min + 1e-6f;
                high = r.wrap || v < r.max - 1e-6f;
            } else {
                int c = g_live->*r.choice;
                low = c > 0;
                high = c + 1 < static_cast<int>(r.names.size());
            }
            arrow(kValueLeft, y0, kValueLeft + kArrow, y1, -1, low ? kText : kOff);
            arrow(kValueRight - kArrow, y0, kValueRight, y1, 1, high ? kText : kOff);
        } else if (r.kind == Kind::Action && r.jiggle && confirming(line.row)) {
            arrow(kValueLeft, y0, kValueLeft + kArrow, y1, -1, g_confirmYes ? kText : kOff);
            arrow(kValueRight - kArrow, y0, kValueRight, y1, 1, g_confirmYes ? kOff : kText);
        } else if (r.kind == Kind::Action && r.action == 1) {
            const bool body = camera::bodyActive();
            arrow(kValueLeft, y0, kValueLeft + kArrow, y1, -1, body && !g_switchedAt ? kText : kOff);
            arrow(kValueRight - kArrow, y0, kValueRight, y1, 1, !body && !g_switchedAt ? kText : kOff);
        } else if (r.kind == Kind::Toggle) {
            fillRounded(kValueRight - 124, y0 + 8, kValueRight, y1 - 8, 18, toggleValue(line.row) ? kOn : kOff);
        }
    }
    if (static_cast<int>(g_lines.size()) > kShown) {
        int track = kListBottom - kListTop;
        int thumb = std::max(40, track * kShown / static_cast<int>(g_lines.size()));
        int range = static_cast<int>(g_lines.size()) - kShown;
        int at = kListTop + (track - thumb) * g_top / range;
        fillRounded(kWidth - 26, kListTop, kWidth - 18, kListBottom, 4, kTrack);
        fillRounded(kWidth - 26, at, kWidth - 18, at + thumb, 4, kThumb);
    }
    GdiFlush();

    // Text.
    text(pageTitle(g_page, kCloseLeft - 60).c_str(), g_canvas.title, kText, 40, 0, kCloseLeft - 20, kTitleHeight, DT_LEFT);
    text(L"Close  ×", g_canvas.row, kText, kCloseLeft, kCloseTop, kCloseRight, kCloseBottom, DT_CENTER);
    for (int j = g_top; j < last; ++j) {
        const Line& line = g_lines[j];
        int y0 = kListTop + (j - g_top) * kRowHeight, y1 = y0 + kRowHeight;
        if (line.row < 0) {
            text(line.heading, g_canvas.heading, kAccent, 40, y0 + 10, 600, y1, DT_LEFT);
            continue;
        }
        const Row& r = g_rows[line.row];
        int right = kValueLeft - 12;
        const bool closed = r.kind == Kind::Link && !linkOpen(r);  // greyed out till it can be opened
        text(r.label, g_canvas.row, closed ? kDim : kText, 48, y0, right, y1, DT_LEFT);
        if (const wchar_t* note = noteOf(r)) {
            int after = 48 + textWidth(r.label, g_canvas.row) + 14;
            if (after + textWidth(note, g_canvas.small) <= right) text(note, g_canvas.small, kDim, after, y0 + 3, right, y1, DT_LEFT);
        }
        wchar_t value[64];
        switch (r.kind) {
            case Kind::Number:
            case Kind::Choice:
                shownValue(line.row, value, 64);
                text(value, g_canvas.row, kText, kValueLeft + kArrow, y0, kValueRight - kArrow, y1, DT_CENTER);
                break;
            case Kind::Toggle:
                text(toggleValue(line.row) ? L"On" : L"Off", g_canvas.row, kText, kValueRight - 124, y0, kValueRight,
                     y1, DT_CENTER);
                break;
            case Kind::Reset:
                text(resetArmed ? L"Press A again" : L"Press A", g_canvas.row, resetArmed ? kWarn : kDim, kValueLeft,
                     y0, kValueRight, y1, DT_RIGHT);
                break;
            case Kind::Link: {
                wchar_t count[48];
                int n = pageSize(r.linkPage);
                swprintf(count, 48, L"%d setting%ls", n, n == 1 ? L"" : L"s");
                text(count, g_canvas.small, kDim, kValueLeft, y0 + 3, kValueRight - 40, y1, DT_RIGHT);
                text(L"\u203a", g_canvas.title, closed ? kDim : kText, kValueRight - 30, y0 - 4, kValueRight, y1, DT_RIGHT);
                break;
            }
            case Kind::Back:
                text(L"B", g_canvas.row, kDim, kValueLeft, y0, kValueRight, y1, DT_RIGHT);
                break;
            case Kind::Action:
                if (r.jiggle && confirming(line.row)) {
                    text(g_confirmYes ? L"Yes, overwrite" : L"No", g_canvas.row, g_confirmYes ? kWarn : kText,
                         kValueLeft + kArrow, y0, kValueRight - kArrow, y1, DT_CENTER);
                } else if (r.action != 1) {
                    const bool done = g_doneAt && g_doneRow == static_cast<size_t>(line.row);
                    text(done ? L"Done" : L"Press A", g_canvas.row, done ? kWarn : kDim, kValueLeft, y0, kValueRight,
                         y1, DT_RIGHT);
                } else {
                    text(g_switchedAt                  ? L"Switching..."
                         : camera::bodyActive() ? L"First person"
                                                : L"Game camera",
                         g_canvas.row, g_switchedAt ? kWarn : kText, kValueLeft + kArrow, y0, kValueRight - kArrow, y1,
                         DT_CENTER);
                }
                break;
        }
    }
    text(g_page ? L"Right stick: up / down picks, left / right changes.   A: press.   B: back."
                : L"Right stick: up / down picks, left / right changes or opens.   A: open.   B: save and close.",
         g_canvas.small, kDim, 40, kListBottom + 14, kWidth - 40, kListBottom + 50, DT_LEFT);
    text(L"Laser: the trigger on a row's arrows or button. Hold the title bar (laser or hand) to move the menu.",
         g_canvas.small, kDim, 40, kListBottom + 50, kWidth - 40, kListBottom + 86, DT_LEFT);
    GdiFlush();
    drawTip();

    // The panel's rounded outline and border; premultiplied alpha.
    if (g_pixels.size() != static_cast<size_t>(kWidth * kHeight)) g_pixels.resize(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            float d = roundedDistance(x + 0.5f, y + 0.5f, 0, 0, kWidth, kHeight, 28);
            float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
            uint32_t c = px[y * kWidth + x];
            float edge = std::clamp(d + 3.5f, 0.0f, 1.0f);  // the outer 3 px
            if (edge > 0) c = blend(c, kBorder, edge);
            uint32_t a = static_cast<uint32_t>(cover * 255 + 0.5f);
            auto ch = [&](int shift) { return static_cast<uint32_t>((c >> shift & 0xff) * cover + 0.5f) << shift; };
            g_pixels[y * kWidth + x] = a << 24 | ch(16) | ch(8) | ch(0);
        }
    ++g_version;
}

// The settings file: written on a worker thread, in the order the changes came.
struct Entry {
    std::string section, key, value;
    bool remove;
};
void queueSave(std::vector<Entry> entries);

std::string utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// [ui] settings_page: "page|row", by their names (they keep across builds that add rows).
void savePage(int page, int row) {
    if (page < 0 || page >= pageCount()) return;
    std::string text = utf8(g_pages[page].group);
    if (row >= 0 && row < static_cast<int>(g_rows.size()) && g_rows[row].page == page) text += "|" + utf8(g_rows[row].label);
    if (text == g_savedPage) return;
    g_savedPage = text;
    queueSave({{"ui", "settings_page", text, false}});
}

void loadPage(const std::string& text) {
    g_savedPage = text;
    const size_t bar = text.find('|');
    const std::string group = text.substr(0, bar), label = bar == std::string::npos ? "" : text.substr(bar + 1);
    for (int p = 1; p < pageCount(); ++p) {
        if (utf8(g_pages[p].group) != group) continue;
        g_reopenPage = p;
        g_reopenRow = -1;
        for (size_t i = 0; i < g_rows.size(); ++i)
            if (g_rows[i].page == p && !label.empty() && utf8(g_rows[i].label) == label) g_reopenRow = static_cast<int>(i);
        return;
    }
}

void save(const std::wstring& path, const std::vector<Entry>& entries) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const char header[] =
                "; Sims4VR: what you changed in the headset's settings menu (B). These win over\r\n"
                "; sims4vr.ini; delete a line (or this file) to use sims4vr.ini's value again.\r\n\r\n";
            DWORD written = 0;
            WriteFile(file, header, sizeof(header) - 1, &written, nullptr);
            CloseHandle(file);
        }
    }
    std::string narrow(path.begin(), path.end());
    int failed = 0;
    for (const Entry& e : entries)
        if (!WritePrivateProfileStringA(e.section.c_str(), e.key.c_str(), e.remove ? nullptr : e.value.c_str(),
                                        narrow.c_str()) &&
            !e.remove)
            ++failed;
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, narrow.c_str());  // flush
    if (failed)
        LOG("Settings: %d of %zu values couldn't be written to %ls (error %lu)", failed, entries.size(), path.c_str(),
            GetLastError());
    else
        LOG("Settings: saved to %ls", path.c_str());
}

}  // namespace

void init(const std::wstring& moduleDir, Config* live) {
    g_ini = moduleDir + L"\\sims4vr.ini";
    g_live = live;
    g_rows = makeRows();
    g_flags.assign(g_rows.size(), 0);
    g_values.assign(g_rows.size(), 0.0f);
    g_changed.assign(g_rows.size(), 0);
    g_removed.assign(g_rows.size(), 0);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.kind == Kind::Toggle && !r.toggle && r.key)
            g_flags[i] = parseFlag(readSetting(g_ini, r.section, r.key, ""), r.fallback);
        if (r.kind == Kind::Number && !r.number && !r.triple && r.key) {
            std::string text = readSetting(g_ini, r.section, r.key, "");
            float v[3];
            if (r.index >= 0)
                g_values[i] = sscanf(text.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3 ? v[r.index] : r.fallbackNumber;
            else
                g_values[i] = text.empty() ? r.fallbackNumber : static_cast<float>(atof(text.c_str()));
        }
    }
    float place[4];
    if (sscanf(readSetting(g_ini, "ui", "settings_place", "").c_str(), "%f %f %f %f", &place[0], &place[1], &place[2],
               &place[3]) == 4 &&
        std::isfinite(place[0] + place[1] + place[2] + place[3])) {
        std::copy(place, place + 4, g_place);
        g_hasPlace = true;
    }
    float note[3];
    if (sscanf(readSetting(g_ini, "phone", "note_place", "").c_str(), "%f %f %f", &note[0], &note[1], &note[2]) == 3 &&
        std::isfinite(note[0] + note[1] + note[2])) {
        std::copy(note, note + 3, g_notePlace);
        g_hasNotePlace = true;
    }
    g_page = 0;
    buildLines();
    loadPage(readSetting(g_ini, "ui", "settings_page", ""));
}

bool placement(float place[4]) {
    if (!g_hasPlace) return false;
    std::copy(g_place, g_place + 4, place);
    return true;
}

void setPlacement(const float place[4]) {
    std::copy(place, place + 4, g_place);
    g_hasPlace = true;
    char text[96];
    snprintf(text, sizeof(text), "%.3f %.3f %.3f %.1f", place[0], place[1], place[2], place[3]);
    LOG("Settings: moved; [ui] settings_place = %s (right, up, forward of your head, turn)", text);
    queueSave({{"ui", "settings_place", text, false}});
}

bool notePlacement(float place[3]) {
    if (!g_hasNotePlace) return false;
    std::copy(g_notePlace, g_notePlace + 3, place);
    return true;
}

void setNotePlacement(const float place[3]) {
    std::copy(place, place + 3, g_notePlace);
    g_hasNotePlace = true;
    char text[96];
    snprintf(text, sizeof(text), "%.3f %.3f %.3f", place[0], place[1], place[2]);
    LOG("Notification card moved; [phone] note_place = %s (right, up, forward of your head)", text);
    queueSave({{"phone", "note_place", text, false}});
}

bool onTitleBar(float u, float v) {
    if (!g_open) return false;
    const float x = u * kWidth, y = v * kHeight;
    if (x >= kCloseLeft - 12 && x < kCloseRight + 12 && y >= kCloseTop - 12 && y < kCloseBottom + 12) return false;
    return x >= -30 && x < kWidth + 30 && y >= -40 && y < kTitleHeight;
}

bool liveValue(const char* section, const char* key, std::string& out) {
    if (!section || !key || !g_live) return false;
    std::lock_guard<std::mutex> lock(g_valueMutex);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.key && !_stricmp(r.section, section) && !_stricmp(r.key, key)) {
            out = valueText(i);
            return true;
        }
    }
    return false;
}

void setTestingCheats(int state) {
    if (g_cheats.exchange(state) != state)
        LOG("Testing cheats are %s in the game, cheat pie menus %s", state & kCheatsOn ? "on" : "off",
            state & kCheatPiesOn ? "on" : "off");
}

bool back() {
    if (!g_open || g_page == 0) return false;
    goBack();
    return true;
}

bool isOpen() { return g_open; }

bool closeWanted() {
    const bool wanted = g_open && g_closeWanted;
    g_closeWanted = false;
    return wanted;
}

void open() {
    if (g_open || !g_live) return;
    g_open = true;
    g_closeWanted = false;
    g_dirX = g_dirY = 9;
    g_pressWas = true;
    g_resetArmed = 0;
    if (mergeMenus() && g_reopenPage < 0 && !g_savedPage.empty())
        loadPage(g_savedPage);  // the page you left was one of its pages
    readJiggle();
    if (g_reopenPage >= 0) {
        openPage(g_reopenPage, g_reopenRow);
        g_reopenPage = -1;
    } else {
        openPage(g_page, g_selected);  // other mods' pages show or hide as they are loaded now
    }
    reveal();
    g_redraw = true;
    LOG("Settings: opened");
}

namespace {

// Writes what changed to sims4vr_user.ini (on a worker thread).
void flush() {
    g_lastChange = 0;
    std::vector<Entry> entries;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        bool done = false;  // a three-number key already written for its first row
        for (const Entry& e : entries) done |= r.key && e.section == r.section && e.key == r.key;
        if (done) {
        } else if (g_removed[i]) {
            entries.push_back({r.section, r.key, "", true});
        } else if (g_changed[i]) {
            entries.push_back({r.section, r.key, valueText(i), false});
            LOG("Settings: [%s] %s = %s", r.section, r.key, valueText(i).c_str());
        }
        g_changed[i] = g_removed[i] = 0;
    }
    queueSave(std::move(entries));
}

// Writes entries to sims4vr_user.ini on a worker thread, one batch after another.
void queueSave(std::vector<Entry> entries) {
    if (entries.empty()) return;
    static std::mutex mutex;
    static std::vector<std::vector<Entry>> queue;
    static bool writing = false;
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(std::move(entries));
    if (writing) return;
    writing = true;
    std::thread([path = userIniPath(g_ini)] {
        for (;;) {
            std::vector<Entry> next;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (queue.empty()) {
                    writing = false;
                    return;
                }
                next = std::move(queue.front());
                queue.erase(queue.begin());
            }
            save(path, next);
        }
    }).detach();
}

}  // namespace

bool close() {
    if (!g_open) return false;
    g_open = false;
    g_reopenPage = g_page;
    g_reopenRow = g_selected == kCloseRow ? -1 : g_selected;
    g_closeWanted = false;
    if (g_page == 0 && g_backing && g_backFrom > 0) {
        g_reopenPage = g_backFrom;
        g_reopenRow = g_backFromRow;
    }
    savePage(g_reopenPage, g_reopenRow);
    flush();
    LOG("Settings: closed");
    bool renderScale = g_renderScaleChanged;
    g_renderScaleChanged = false;
    return renderScale;
}

bool update(float stickX, float stickY, bool press) {
    if (!g_open) return false;
    ULONGLONG now = GetTickCount64();
    bool any = false;
    // The stronger axis wins, so a diagonal push does one thing.
    bool vertical = std::fabs(stickY) >= std::fabs(stickX);
    int dy = stickStep(vertical ? stickY : 0, g_dirY, g_nextY, g_repeatsY, now);
    int dx = stickStep(vertical ? 0 : stickX, g_dirX, g_nextX, g_repeatsX, now);
    if (dy) {
        if (g_selected == kCloseRow)
            any |= dy < 0 && select(firstRow());  // down: back onto the list
        else if (dy > 0 && stepRow(g_selected, -1) == g_selected)
            any |= selectClose();  // up past the first row
        else
            any |= select(stepRow(g_selected, -dy));  // up = the row above
    }
    if (g_selected == kCloseRow) {
        if (dx > 0 || (press && !g_pressWas)) g_closeWanted = any = true;
    } else {
        if (dx) any |= change(g_selected, dx);
        if (press && !g_pressWas) any |= change(g_selected, 0);
    }
    g_pressWas = press;
    // Saved a second after the last change, so a crash or a lost headset link
    // doesn't take your settings with it.
    if (g_lastChange && now - g_lastChange >= 1000) flush();
    if (g_resetArmed && now - g_resetArmed >= 4000) {
        g_resetArmed = 0;
        g_redraw = true;
    }
    // The view switch row shows the view you're in once it has changed (or
    // after 3 s, if it didn't: not on a lot, or the game didn't take it).
    bool body = camera::bodyActive();
    if (body != g_bodyShown || (g_switchedAt && now - g_switchedAt >= 3000)) {
        g_bodyShown = body;
        g_switchedAt = 0;
        g_redraw = true;
    }
    if (g_cheats != g_cheatsShown) {
        g_cheatsShown = g_cheats;
        g_redraw = true;
    }
    if (g_doneAt && now - g_doneAt >= 1500) {
        g_doneAt = 0;
        g_redraw = true;
    }
    return any;
}

int point(float u, float v, bool click) {
    if (!g_open) return kNothing;
    int x = static_cast<int>(u * kWidth), y = static_cast<int>(v * kHeight);
    if (x >= kCloseLeft && x < kCloseRight && y >= kCloseTop && y < kCloseBottom)
        return click ? kCloseRequested : kNothing;
    if (y < kListTop || y >= kListBottom) return kNothing;
    int j = g_top + (y - kListTop) / kRowHeight;
    if (j >= static_cast<int>(g_lines.size()) || g_lines[j].row < 0) return kNothing;
    int row = g_lines[j].row;
    // The laser picks the row it moves onto; resting there leaves the stick free to pick another.
    static int hovered = -1;
    static ULONGLONG hoveredAt = 0;
    ULONGLONG now = GetTickCount64();
    if (now - hoveredAt > 150) hovered = -1;
    hoveredAt = now;
    int result = kNothing;
    if (row != hovered || click) result = select(row) ? kChanged : kNothing;
    hovered = row;
    if (!click) return result;
    const Row& r = g_rows[row];
    int dir = 0;
    if (r.kind == Kind::Number || r.kind == Kind::Choice || (r.kind == Kind::Action && r.action == 1)) {
        if (x >= kValueLeft - 20 && x < kValueLeft + kArrow + 20) dir = -1;
        else if (x >= kValueRight - kArrow - 20 && x < kValueRight + 20) dir = 1;
        else return result;
    }
    return change(row, dir) ? kChanged : result;
}

const uint32_t* image(int& width, int& height, uint64_t& version) {
    if (g_open && !g_tipDrawn && g_selected >= 0 && g_selected < static_cast<int>(g_rows.size()) &&
        g_rows[g_selected].tip && GetTickCount64() - g_selectedAt >= kTipDelay)
        g_redraw = true;  // the tip is due
    if (g_redraw && makeCanvas()) {
        g_redraw = false;
        draw();
    }
    width = kWidth;
    height = kHeight;
    version = g_version;
    return g_pixels.empty() ? nullptr : g_pixels.data();
}

}  // namespace s4vr::settings
