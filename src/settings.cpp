#include "settings.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <thread>
#include <deque>
#include <string>
#include <vector>

#include "armik.h"
#include "benchmark.h"
#include "ambience.h"
#include "camera.h"
#include "facehide.h"
#include "gputimeline.h"
#include "godhands.h"
#include "casbody.h"
#include "collide.h"
#include "walkstop.h"
#include "gesture.h"
#include "grab.h"
#include "hud.h"
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
    othermods::Mod mod = othermods::kBodyPhysics;  // with jiggle: the separate mod the key belongs to
    const wchar_t* tip = nullptr;  // what it does: shown in a box under the row while it's picked
    const char* when = nullptr;       // another mod's Link row: it opens only while this key of the mod is not 0
    const wchar_t* offTip = nullptr;  // with when: the tip while it can't be opened (greyed out)
    bool dev = false;  // developer-only: hidden unless Options > Developer options is on (settings_dev.inc)
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
    {L"Logging", L"Logging (testing)", L"Options"},
    // Options: the five categories (user 2026-10-23: the menu got hard to navigate).
    {L"Options", L"Options", L""},
    {L"Game", L"Game settings", L"Options"},
    {L"Passive objects", L"Objects around you", L"Game"},
    {L"Influence", L"Influencing other Sims", L"Game"},
    {L"Gaze watch", L"Looking through windows and at Sims", L"Game"},
    {L"Light switches", L"Touching light switches", L"Game"},
    {L"Drone", L"Flying the Streaming Drone", L"Game"},
    {L"CAS", L"Create a Sim (CAS)", L"Game"},
    {L"Outfits", L"Live outfit planning", L"Game"},
    {L"Reach", L"Reach and walking to things", L"Game"},
    {L"Trail", L"Footprint trail", L"Reach"},
    {L"Other Sims", L"Other Sims", L"Game"},
    {L"Other Sims talk", L"Talking and following", L"Other Sims"},
    {L"Other Sims hugs", L"Hugs and kisses", L"Other Sims"},
    {L"Other Sims touch", L"Touching and grabbing", L"Other Sims"},
    {L"Other Sims hits", L"Hits and fights", L"Other Sims"},
    {L"Knockout", L"Knock out", L"Other Sims hits"},
    {L"Other Sims grab", L"Grabbing", L"Other Sims touch"},
    {L"Other Sims reactions", L"How they react", L"Other Sims touch"},
    {L"Other Sims look at hand", L"They look at your hand", L"Other Sims touch"},
    {L"Other Sims hit strength", L"How hard a touch counts", L"Other Sims hits"},
    {L"Other Sims gestures", L"Hand gestures", L"Controls"},
    {L"Body physics app", L"Body Physics phone app", L"Controls"},
    {L"Body app frozen Sim", L"Her Settings", L"Body physics app"},
    {L"Body app size", L"Size preset", L"Body app frozen Sim"},
    {L"Body app gravity", L"Gravity preset", L"Body app frozen Sim"},
    {L"Body app spacing", L"Spacing preset", L"Body app frozen Sim"},
    {L"Relationships", L"Relationships", L"Testing"},
    {L"Body app friendship", L"Friendship with the Body app Sim", L"Relationships"},
    {L"Body app romance", L"Romance with the Body app Sim", L"Relationships"},
    {L"Gesture follow", L"Follow me", L"Other Sims gestures"},
    {L"Gesture shoo", L"Shoo", L"Other Sims gestures"},
    {L"Other Sims high five", L"High five and taps", L"Other Sims gestures"},
    {L"Touch sounds", L"Touch and hit sounds", L"Sound"},
    {L"Touch sounds cloth", L"On clothes", L"Touch sounds"},
    {L"Touch sounds skin", L"On bare skin", L"Touch sounds"},
    {L"Touch sounds volume", L"Volume of your own sounds", L"Touch sounds"},
    {L"Touch sounds objects", L"Hitting objects", L"Touch sounds"},
    {L"Knocking", L"Knocking on doors and windows", L"Touch sounds"},
    {L"Ragdoll", L"Ragdoll falls", L"Other Sims hits"},
    {L"Shoves", L"Shoves", L"Other Sims hits"},
    {L"Loading", L"Loading and saving", L"Game"},
    {L"Graphics", L"Graphics & Performance", L"Options"},
    {L"View picture", L"Picture", L"Graphics"},
    {L"View mirrors", L"Mirrors and reflections", L"Graphics"},
    {L"View near", L"Near clipping", L"Graphics"},
    {L"View shadows", L"Shadows", L"Graphics"},
    {L"View motion", L"Smooth motion", L"Graphics"},
    {L"View eyes", L"Depth eye and tests", L"Graphics"},
    {L"View align", L"Right eye alignment", L"View eyes"},
    {L"View speed", L"Performance and tests", L"Graphics"},
    {L"Sound", L"Sound", L"Options"},
    {L"Controls", L"Controls", L"Options"},
    {L"Moving", L"Moving", L"Controls"},
    {L"Collision", L"Walls and furniture", L"Controls"},
    {L"Foliage", L"Plants and leaves", L"Controls"},
    {L"Crouch", L"Crouch", L"Moving"},
    {L"Teleport", L"Teleport", L"Moving"},
    {L"Turning", L"Turning", L"Controls"},
    {L"Hands", L"Hands and touch", L"Controls"},
    {L"Doors", L"Doors", L"Hands"},
    {L"View head", L"Head and body", L"Controls"},
    {L"View place", L"Place your view", L"View head"},
    {L"Seat place", L"Seated: where you sit", L"View head"},
    {L"Interface", L"Interface", L"Options"},
    {L"Handhelds", L"Phone, tablet and notebook", L"Interface"},
    {L"Menus pie", L"Pie menu", L"Interface"},
    {L"Menus pickers", L"Pickers on objects", L"Interface"},
    {L"Menus popups", L"Popups", L"Interface"},
    {L"Menus panels", L"Panels and flat screen", L"Interface"},
    {L"HUD", L"Wrist HUD", L"Interface"},
    {L"HUD watch", L"Watch", L"HUD"},
    {L"HUD needs", L"Needs", L"HUD"},
    {L"HUD moodlets", L"Moodlets", L"HUD"},
    {L"HUD queue", L"Action queue", L"HUD"},
    {L"HUD build", L"Build button", L"HUD"},
    {L"HUD wants", L"Wants", L"HUD"},
    {L"HUD pause", L"Pause symbol", L"HUD"},
    {L"HUD sleep", L"Clock while asleep", L"HUD"},
    {L"HUD lot intro", L"Lot name and traits on arrival", L"HUD"},
    {L"HUD work card", L"Work card (eyes closed at work)", L"HUD"},
    {L"HUD note", L"Notification card", L"Interface"},
    {L"Bubbles", L"Speech bubbles and skill bars", L"Interface"},
    {L"Bubbles yours", L"Your bubble", L"Bubbles"},
    {L"Bubbles skill", L"Your skill bar", L"Bubbles"},
    {L"Bubbles pops", L"Your stat and relationship pops", L"Bubbles"},
    {L"Bubbles scandal", L"Your scandal mark", L"Bubbles"},
    {L"Bubbles others", L"Other Sims' bubbles", L"Bubbles"},
    {L"Phone", L"Phone", L"Handhelds"},
    {L"Phone position", L"Position", L"Phone"},
    {L"Phone hand", L"Hand", L"Phone"},
    {L"Phone size", L"Size and screen", L"Phone"},
    {L"Phone model", L"Game's phone model", L"Phone"},
    {L"Phone sounds", L"Sounds", L"Phone"},
    {L"Phone notes", L"Notifications", L"Phone"},
    {L"Phone flashlight", L"Flashlight", L"Phone"},
    {L"Phone sf", L"Sims Forever", L"Phone"},
    {L"Phone sf left", L"Left hand", L"Phone sf"},
    {L"Phone sf right", L"Right hand", L"Phone sf"},
    {L"Tablet", L"Tablet", L"Handhelds"},
    {L"Notebook", L"Notebook", L"Handhelds"},
    {L"Notebook position", L"Position", L"Notebook"},
    {L"Notebook hand", L"Hand", L"Notebook"},
    {L"Notebook size", L"Size and turning pages", L"Notebook"},
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

// A touch sound (foley.py): picking one plays it at your right hand (controls.py 7650000 + kind * 100 + n).
Row foley(const wchar_t* group, const wchar_t* label, const char* key, int Config::*m, int kind,
          std::vector<const wchar_t*> names) {
    Row r = choice(group, label, "foley", key, m, std::move(names));
    r.action = 7650000 + kind * 100;
    return r;
}

// A script number shown as a percentage (1 = 100%).
Row percent(Row r) {
    r.shown = 100;
    return r;
}

#include "settings_tips.inc"
#include "settings_dev.inc"

std::vector<Row> makeRows() {
    const wchar_t* moving = L"Moving";
    const wchar_t* hands = L"Hands";
    const wchar_t* sims = L"Other Sims";
    const wchar_t* front = L"Settings";  // the first page under Play: the way into Options
    const wchar_t* sound = L"Sound";
    const wchar_t* phone = L"Phone";
    const wchar_t* phonePosition = L"Phone position";
    const wchar_t* phoneHand = L"Phone hand";
    const wchar_t* phoneSize = L"Phone size";
    const wchar_t* phoneModel = L"Phone model";
    const wchar_t* phoneSounds = L"Phone sounds";
    const wchar_t* phoneNotes = L"Phone notes";
    const wchar_t* phoneFlash = L"Phone flashlight";
    const wchar_t* tablet = L"Tablet";
    const wchar_t* notebook = L"Notebook";
    const wchar_t* notebookPosition = L"Notebook position";
    const wchar_t* notebookHand = L"Notebook hand";
    const wchar_t* notebookSize = L"Notebook size";
    const wchar_t* testing = L"Testing";
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
    auto relButton = [](const wchar_t* label, int action) {
        Row r;
        r.group = L"Relationships";
        r.label = label;
        r.kind = Kind::Action;
        r.action = action;
        return r;
    };
    auto benchButton = [](const wchar_t* label, int action, const wchar_t* tip) {
        Row r;
        r.group = L"View speed";
        r.label = label;
        r.kind = Kind::Action;
        r.action = action;
        r.tip = tip;
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
    Row keyboardButton;  // native: opens / closes the VR keyboard by hand (the auto-detect is not sure of every box)
    keyboardButton.group = L"Menus popups";
    keyboardButton.label = L"Open / close the keyboard";
    keyboardButton.kind = Kind::Action;
    keyboardButton.action = 204;
    keyboardButton.tip = L"Opens the keyboard in front of you for a text box it did not open for, or closes it.";
    // physapp.py (thread "Add body physics phone app"): the frozen Sim's outfit and WickedWhims pose menu by control.
    auto physicsButton = [](const wchar_t* label, int control, const wchar_t* tip, const wchar_t* group = L"Body app frozen Sim") {
        Row r;
        r.group = group;
        r.label = label;
        r.kind = Kind::Action;
        r.action = 8400000 + control;
        r.tip = tip;
        return r;
    };
    // The frozen Sim's pie menu options (physapp.py _groups / control): presets by control 300 + 30 * group + index.
    std::vector<Row> physicsRows;
    physicsRows.push_back(physicsButton(L"Previous outfit", 10, L"The Sim frozen in the Body Physics app wears her previous outfit (the pie menu's list: Everyday, Formal, Athletic, ...).", L"Body physics app"));
    physicsRows.push_back(physicsButton(L"Next outfit", 11, L"She wears her next outfit.", L"Body physics app"));
    physicsRows.push_back(physicsButton(L"WickedWhims pose menu", 12, L"Opens WickedWhims' own pose menu for her (needs WickedWhims). The physics mod's freeze hold on her is dropped while it runs her.", L"Body physics app"));
    physicsRows.push_back(physicsButton(L"Freeze her again", 13, L"Holds her in the neutral pose again after WickedWhims' poses.", L"Body physics app"));
    physicsRows.push_back(physicsButton(L"Top: switch Clothed / Topless", 14, L"Sets her top part as nude or as clothed for the physics, as the pie menu's Set as nude / Set as clothed."));
    physicsRows.push_back(physicsButton(L"Breasts: switch Enhanced / Natural", 15, L"Switches her breasts between natural and enhanced, as the pie menu."));
    struct Preset { const wchar_t* group; const wchar_t* labels[8]; };
    static const Preset kPresets[5] = {
        {L"Body app size", {L"Auto (from CAS)", L"Small", L"Medium", L"Bigger", L"Huge", nullptr}},
        {L"Body app gravity", {L"Bolt on", L"Enhanced", L"Natural", L"Heavy", L"Saggy", L"Follow the debug menu", nullptr}},
        {L"Body app spacing", {L"Very together", L"Together", L"Natural", L"Apart", L"Very apart", L"Follow the debug menu", nullptr}},
        {L"Body app friendship", {L"Enemy (-100)", L"Disliked (-50)", L"Neutral (0)", L"Acquaintance (20)", L"Friend (40)", L"Good friend (70)", L"Best friend (100)"}},
        {L"Body app romance", {L"Hostile (-50)", L"None (0)", L"Flirty (25)", L"Romantic (50)", L"Lovers (75)", L"Soulmates (100)", nullptr}},
    };
    for (int g = 0; g < 5; ++g)
        for (int i = 0; i < 8 && kPresets[g].labels[i]; ++i)
            physicsRows.push_back(physicsButton(kPresets[g].labels[i], 300 + g * 30 + i,
                                                L"Applies to the Sim frozen in the Body Physics app (the pie menu's choice).", kPresets[g].group));
    Row alignReset;
    alignReset.group = L"View align";
    alignReset.label = L"Reset the alignment";
    alignReset.kind = Kind::Action;
    alignReset.action = 203;  // native: every made_eye_* row back to as measured
    alignReset.tip = L"Picture and headset rows back to 0 (eye distance and depth to 100%). Each change logs "
                     L"\"Depth eye alignment\" and is saved in your ini.";
    Row outfitProbe;  // outfit.py control 90000 (controls.py 8600000 + code)
    outfitProbe.group = L"Outfits";
    outfitProbe.label = L"Outfit probe (testing)";
    outfitProbe.kind = Kind::Action;
    outfitProbe.action = 8690000;
    outfitProbe.dev = true;
    outfitProbe.tip = L"First person: puts a catalog part on your Sim the panel's way and logs each step (outfit: lines in "
                      L"the python log). Save, reload and press it again to see whether the part stayed.";
    Row reset;
    reset.group = L"Options";
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
        number(L"View picture", L"Render resolution", "stereo", "render_scale", &Config::renderScale, 0.4f, 1.5f, 0.05f, 100,
               L"%.0f%%", kRenderScale),
        choice(L"Moving", L"Movement mode", "controls", "move_mode", &Config::moveMode,
               {L"Smooth walking (left stick)", L"Point to teleport"}),
        number(L"Moving", L"Walk speed", "body", "walk_speed", &Config::walkSpeed, 0.5f, 5, 0.1f, 1, L"%.1f m/s", kWalk),
        number(L"Turning", L"Turn speed", "body", "turn_rate", &Config::bodyTurnRate, 30, 360, 10, 1, L"%.0f°/s"),
        toggle(L"Turning", L"Snap turning (off: smooth turning)", "controls", "snap_turn", &Config::snapTurn),
        number(L"Turning", L"Snap turn angle (small push)", "controls", "snap_angle", &Config::snapAngle, 15, 90, 15, 1, L"%.0f°"),
        number(L"Turning", L"Snap turn angle (stick all the way, 0 = same)", "controls", "snap_angle_big", &Config::snapAngleBig, 0,
               180, 15, 1, L"%.0f°"),
        flag(L"Trail", L"Walk there yourself (footprint trail)", "body", "trail", true, kLive),  // trail.py
        number(L"Trail", L"Trail: how near its end starts the action", "body", "trail_arrive", &Config::trailArrive, 0.3f, 2,
               0.1f, 100, L"%.0f cm"),  // trail.py
        number(L"Trail", L"Trail: the same for touching socials", "body", "trail_arrive_contact", &Config::trailArriveContact,
               0.1f, 1, 0.05f, 100, L"%.0f cm"),  // trail.py
        flag(L"Trail", L"Trail: the game takes you through elevators", "body", "trail_elevator", true, kLive),  // trail.py
        flag(L"Trail", L"Trail: the game climbs ladders with you", "body", "trail_ladder", true, kLive),  // trail.py
        flag(L"Trail", L"Elevator: walk into the car yourself, eyes shut for the ride", "body", "elevator_ride", true, kLive),  // trail.py
        flag(L"Trail", L"Elevator: long ride (off: short, only the end of the sound)", "body", "elevator_long", true, kLive),  // trail.py
        percent(scriptNumber(L"Trail", L"Elevator: sound volume", "foley", "volume_elevator", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        flag(L"Trail", L"Click a need to find a way to fulfil it", "body", "need_click", true, kLive),  // needclick.py
        flag(L"Trail", L"Trail: Sims you walk to stop and face you", "body", "trail_stop", true, kLive),  // trail.py
        number(L"Trail", L"Trail: how near you are when they stop", "body", "trail_stop_dist", &Config::trailStopDist, 1, 10, 0.5f, 1,
               L"%.1f m"),  // trail.py
        toggle(L"Turning", L"Recenter turns your body to your view (in actions: view to the body)", "controls",
               "recenter_turns_body", &Config::recenterTurnsBody),
        flag(L"Loading", L"Start with eyes closed and time paused until you can move", "body", "load_eyes_closed", true,
             kLive),  // loadshut.py
        flag(L"Loading", L"Eyes closed while the game saves", "body", "exit_eyes_closed", true, kLive),  // loadshut.py
        toggle(L"Hands", L"Pick things up", "hands", "grab", &Config::grab, kNextStart),
        toggle(L"Hands", L"Arms follow your controllers (next start)", "body", "arm_ik", &Config::armIk, kNextStart),
        toggle(L"Hands", L"Real hands when controllers are put down (next start)", "body", "hand_tracking", &Config::handTracking, kNextStart),
        toggle(L"Hands", L"Touch to click", "body", "touch", &Config::touch, kNextStart),
        toggle(L"Hands", L"Selected Sim's hands in god mode", "body", "god_hands", &Config::bodyGodHands, kView),
        toggle(L"Menus pie", L"Pie menu at your hand", "ui", "pie_at_hand", &Config::pieAtHand),
        toggle(L"Menus popups", L"Popups in the world", "ui", "popups_3d", &Config::popups3d),
        flag(L"Menus popups", L"Phone-ring Sim pickers as cards (Invite over)", "ui", "ring_pickers_3d", true, kLive),  // objmenu.py
        toggle(L"HUD", L"HUD on (all of it)", "wrist", "hud", &Config::wristPieces),
        number(L"HUD", L"HUD size (all of it)", "wrist", "scale", &Config::wristScale, 0.3f, 3, 0.05f, 100, L"%.0f%%"),
        toggle(L"Phone", L"Phone at your hip", "phone", "phone", &Config::phone),
        flag(L"Phone", L"Notifications about one Sim over their head (off: phone only)", "phone", "notes_overhead", true, kLive),  // notify.py
        flag(L"Phone", L"Game camera for photos and selfies (off: no photo session, testing)", "photo", "game_camera", true, kLive),  // photowatch.py
        flag(L"Phone", L"Camera app buttons on the phone during photos", "photo", "camera_app", true, kLive),  // photowatch.py control()
        toggle(L"View head", L"Mic moves your Sim's mouth", "voice", "mic", &Config::voiceMic, kNextStart),
        link(front, L"Options", L"Options"),
        reset,
        link(L"Options", L"Game settings", L"Game"),
        link(L"Options", L"Graphics & Performance", L"Graphics"),
        link(L"Options", L"Sound", L"Sound"),
        link(L"Options", L"Controls", L"Controls"),
        link(L"Options", L"Interface", L"Interface"),
        flag(L"Options", L"Developer options (testing only)", "debug", "dev_options", kDevDefault, kLive),
        link(L"Options", L"Logging (testing)", L"Logging"),
        flag(L"Logging", L"Flight recorder (last seconds kept in memory)", "logging", "recorder", true, kLive),
        flag(L"Logging", L"Decision (why) lines", "logging", "why", true, kLive),
        flag(L"Logging", L"Noisy tags (frame map, photo, sound probes)", "logging", "noisy", false, kLive),
        scriptNumber(L"Logging", L"Recorder length", "logging", "ring_seconds", 20, 10, 60, 10, L"%.0f s"),
        scriptNumber(L"Logging", L"Trace files kept", "logging", "keep_dumps", 20, 5, 50, 5, L"%.0f"),
        [] {
            Row r;
            r.group = L"Logging";
            r.label = L"Dump trace now";
            r.kind = Kind::Action;
            r.action = 205;  // native: log::dumpTrace
            return r;
        }(),
        link(L"Game", L"Reach and walking to things", L"Reach"),
        link(L"Game", L"Other Sims", L"Other Sims"),
        link(L"Game", L"Loading and saving", L"Loading"),
        link(L"Game", L"Objects around you", L"Passive objects"),
        link(L"Game", L"Looking through windows and at Sims", L"Gaze watch"),
        link(L"Game", L"Influencing other Sims", L"Influence"),
        link(L"Game", L"Touching light switches", L"Light switches"),
        link(L"Game", L"Flying the Streaming Drone", L"Drone"),
        link(L"Game", L"Create a Sim (CAS)", L"CAS"),
        toggle(L"CAS", L"CAS in 3D with your eyes", "cas", "stereo", &Config::casStereo, kView),
        number(L"CAS", L"Distance to the Sim", "cas", "distance", &Config::casDistance, 0.8f, 4.0f, 0.1f, 1, L"%.1f m", kView),
        number(L"CAS", L"Eye height", "cas", "eye_height", &Config::casEyeHeight, 0.5f, 2.5f, 0.1f, 1, L"%.1f m", kView),
        number(L"CAS", L"Eye separation", "cas", "eye_separation", &Config::casEyeSeparation, 0.0f, 2.0f, 0.1f, 1, L"%.1f x", kView),
        toggle(L"CAS", L"Your body in Create a Sim", "cas", "body", &Config::casBody, kView),
        toggle(L"CAS", L"Eyes at your body's height", "cas", "body_eyes", &Config::casBodyEyes, kView),
        toggle(L"CAS", L"Your body stands still", "cas", "body_still", &Config::casBodyStill, kView),
        // outfit.py (+ hud_outfit.inc, 14c_outfit.inc): Plan Outfit on a mirror, dresser or wardrobe opens our panel in
        // the room (first person); a dresser's drawers pull out by hand; Store options on its pie
        link(L"Game", L"Live outfit planning", L"Outfits"),
        flag(L"Outfits", L"Live outfit planning", "outfit", "on", true, kLive),  // the whole feature; off = the game's CAS
        flag(L"Outfits", L"Dressers and wardrobes open by hand", "outfit", "drawers", true, kLive),
        flag(L"Outfits", L"Open by hand without Plan Outfit", "outfit", "always", true, kLive),
        scriptNumber(L"Outfits", L"How far to pull a drawer", "outfit", "drawer_pull", 0.18f, 0.05f, 0.5f, 0.01f, L"%.2f m"),
        flag(L"Outfits", L"Drawer sounds", "outfit", "sounds", true, kLive),
        flag(L"Outfits", L"Store options on dressers and wardrobes", "outfit", "tags", true, kLive),
        flag(L"Outfits", L"Every colour as its own part (testing)", "outfit", "all_variants", false, kLive),
        scriptNumber(L"Outfits", L"How a change is put on (testing)", "outfit", "apply_route", 0, 0, 2, 1, L"%.0f"),
        flag(L"Outfits", L"Log every step (testing)", "outfit", "log", false, kLive),
        outfitProbe,
        // drone.py (+ drone.cpp): start a drone recording/stream and you fly it yourself, the view from the drone
        flag(L"Drone", L"Fly the drone yourself", "drone", "enabled", true, kLive),
        scriptNumber(L"Drone", L"Speed", "drone", "speed", 6, 1, 20, 0.5f, L"%.1f m/s"),
        scriptNumber(L"Drone", L"Climb speed", "drone", "lift_speed", 3, 0.5f, 10, 0.5f, L"%.1f m/s"),
        scriptNumber(L"Drone", L"Smoothing", "drone", "smoothing", 0.25f, 0, 1, 0.05f, L"%.2f s"),
        scriptNumber(L"Drone", L"Stick fully pushed speeds up by", "drone", "boost", 2, 1, 4, 0.25f, L"x%.2f"),
        scriptNumber(L"Drone", L"Lowest height above the ground", "drone", "min_height", 1, 0.3f, 5, 0.1f, L"%.1f m"),
        scriptNumber(L"Drone", L"Highest height above the ground", "drone", "max_height", 60, 5, 200, 5, L"%.0f m"),
        // influence.py: "Influence to..." on a household Sim's pie; Try... then picks what they do
        flag(L"Influence", L"Influence to... on household Sims", "influence", "on", true, kLive),
        scriptNumber(L"Influence", L"Willing from friendship", "influence", "min_friendship", -20, -100, 100, 5, L"%.0f"),
        scriptNumber(L"Influence", L"A close friend ignores a bad mood from", "influence", "mood_override", 60, -100, 100, 5, L"%.0f"),
        scriptNumber(L"Influence", L"They only look with their eyes within", "influence", "look_eyes", 0.6f, 0, 3, 0.1f, L"%.1f m"),
        scriptNumber(L"Influence", L"They turn their head from", "influence", "look_head", 1.5f, 0, 5, 0.1f, L"%.1f m"),
        scriptNumber(L"Influence", L"They turn their body past", "influence", "body_angle", 75, 30, 150, 5, L"%.0f°"),
        scriptNumber(L"Influence", L"They never turn further from you than", "influence", "face_keep", 60, 20, 120, 5, L"%.0f°"),
        flag(L"Influence", L"Ignore willingness and consent (testing)", "influence", "ignore_willingness", false, kLive),
        flag(L"Influence", L"Only household Sims", "influence", "household_only", true, kLive),
        flag(L"Influence", L"Social picks need their consent", "influence", "gate_socials", true, kLive),
        scriptNumber(L"Influence", L"Romance needed for flirting and kissing", "influence", "romance_min", 15, -100, 100, 5, L"%.0f"),
        scriptNumber(L"Influence", L"Romance needed for sex and making a move", "influence", "intimate_min", 40, -100, 100, 5, L"%.0f"),
        scriptNumber(L"Influence", L"Friendship needed for friendly and pranks", "influence", "friendly_min", -10, -100, 100, 5, L"%.0f"),
        scriptNumber(L"Influence", L"Choosing ends after", "influence", "timeout", 45, 10, 300, 5, L"%.0f s"),
        // gazewatch.py: looking steadily peeps through a window / watches a Sim (WickedWhims interactions)
        flag(L"Gaze watch", L"Looking does things (peeping, watching)", "gazewatch", "on", true, kLive),
        flag(L"Gaze watch", L"Peep by looking through a window", "gazewatch", "peep", true, kLive),
        scriptNumber(L"Gaze watch", L"Look through it for", "gazewatch", "peep_seconds", 4, 1, 30, 0.5f, L"%.1f s"),
        scriptNumber(L"Gaze watch", L"Window may be off-centre by", "gazewatch", "peep_angle", 25, 5, 60, 5, L"%.0f°"),
        scriptNumber(L"Gaze watch", L"Window must be within", "gazewatch", "peep_range", 3, 1, 8, 0.5f, L"%.1f m"),
        flag(L"Gaze watch", L"Watch Sims by looking at them", "gazewatch", "watch", true, kLive),
        scriptNumber(L"Gaze watch", L"Look at them for", "gazewatch", "watch_seconds", 1.5f, 0.5f, 10, 0.5f, L"%.1f s"),
        scriptNumber(L"Gaze watch", L"Sim may be off-centre by", "gazewatch", "watch_angle", 12, 3, 40, 1, L"%.0f°"),
        scriptNumber(L"Gaze watch", L"Sim must be within", "gazewatch", "watch_range", 12, 2, 30, 1, L"%.0f m"),
        scriptNumber(L"Gaze watch", L"Looking away stops it after", "gazewatch", "grace", 2, 0.5f, 10, 0.5f, L"%.1f s"),
        // lightswitch.py: touching a placeable light switch flips its room's lights
        flag(L"Light switches", L"Touch a light switch to flip the room's lights", "lightswitch", "on", true, kLive),
        scriptNumber(L"Light switches", L"Touch distance (palm to switch)", "lightswitch", "touch_radius", 0.10f, 0.03f, 0.30f, 0.01f, L"%.2f m"),
        scriptNumber(L"Light switches", L"Wait before the same hand flips it again", "lightswitch", "cooldown", 0.8f, 0.2f, 3, 0.1f, L"%.1f s"),
        flag(L"Light switches", L"Buzz the hand on a flip", "lightswitch", "haptics", true, kLive),
        flag(L"Light switches", L"Log the switches found and flips", "lightswitch", "log", true, kLive),
        link(L"Other Sims", L"Talking and following", L"Other Sims talk"),
        link(L"Other Sims", L"Hugs and kisses", L"Other Sims hugs"),
        link(L"Other Sims", L"Touching and grabbing", L"Other Sims touch"),
        link(L"Other Sims", L"Hits and fights", L"Other Sims hits"),
        link(L"Other Sims hits", L"How hard a touch counts", L"Other Sims hit strength"),
        link(L"Other Sims hits", L"Knock out", L"Knockout"),
        link(L"Other Sims hits", L"Ragdoll falls", L"Ragdoll"),
        link(L"Other Sims hits", L"Shoves", L"Shoves"),
        link(L"Reach", L"Footprint trail", L"Trail"),
        link(L"Hands", L"Doors", L"Doors"),
        link(L"Moving", L"Crouch", L"Crouch"),
        link(L"Moving", L"Teleport (point to move)", L"Teleport"),
        choice(L"Teleport", L"Stick that aims (push forward)", "teleport", "stick", &Config::teleStick,
               {L"Left stick", L"Right stick"}),
        choice(L"Teleport", L"Pointer style", "teleport", "style", &Config::teleStyle, {L"Arc", L"Straight laser"}),
        number(L"Teleport", L"Longest jump", "teleport", "max_distance", &Config::teleDistance, 2, 30, 1, 1, L"%.0f m"),
        flag(L"Teleport", L"Jump over stairs to other levels", "teleport", "other_levels", true, kLive),  // walk.py
        number(L"Teleport", L"Biggest height difference", "teleport", "max_rise", &Config::teleMaxRise, 0.2f, 3, 0.1f, 1,
               L"%.1f m"),  // walk.py
        toggle(L"Teleport", L"Footprints at the spot", "teleport", "marker", &Config::teleMarker),
        toggle(L"Teleport", L"Blink around the jump", "teleport", "blink", &Config::teleBlink),
        number(L"Teleport", L"Blink length", "teleport", "blink_seconds", &Config::teleBlinkTime, 0.05f, 1, 0.05f, 1000,
               L"%.0f ms"),
        link(L"Other Sims gestures", L"Follow me", L"Gesture follow"),
        link(L"Other Sims gestures", L"Shoo", L"Gesture shoo"),
        link(L"Other Sims gestures", L"High five and taps", L"Other Sims high five"),
        link(L"Sound", L"Touch and hit sounds", L"Touch sounds"),
        link(L"Interface", L"Wrist HUD", L"HUD"),
        link(L"Interface", L"Notification card", L"HUD note"),
        link(L"Interface", L"Pie menu", L"Menus pie"),
        link(L"Interface", L"Pickers on objects", L"Menus pickers"),
        link(L"Interface", L"Popups", L"Menus popups"),
        link(L"Interface", L"Speech bubbles and skill bars", L"Bubbles"),
        link(L"Interface", L"Panels and flat screen", L"Menus panels"),
        link(L"Interface", L"Phone, tablet and notebook", L"Handhelds"),
        link(L"Handhelds", L"Phone", L"Phone"),
        link(L"Handhelds", L"Tablet", L"Tablet"),
        link(L"Handhelds", L"Notebook", L"Notebook"),
        // passive.py / tvwatch.py: objects affect you without using them (thread "Build passive object effects system")
        flag(L"Passive objects", L"Objects around you affect you", "passive", "on", true, kLive),
        flag(L"Passive objects", L"Log what the game's objects do to you", "passive", "log", true, kLive),
        flag(L"Passive objects", L"Music nearby gives fun", "passive", "music", true, kLive),
        scriptNumber(L"Passive objects", L"Music heard up to (high volume)", "passive", "music_range", 12, 1, 30, 1, L"%.0f m"),
        scriptNumber(L"Passive objects", L"Fun a game minute from music", "passive", "music_rate", 0.15f, 0, 1, 0.05f, L"%.2f"),
        flag(L"Passive objects", L"Watch a TV by looking at it", "tv", "gaze", true, kLive),
        scriptNumber(L"Passive objects", L"TV heard up to (high volume)", "tv", "nearby", 10, 1, 30, 1, L"%.0f m"),
        scriptNumber(L"Passive objects", L"Fun from a TV nearby (share of watching)", "tv", "passive_share", 0.3f, 0, 1, 0.05f, L"%.2f"),
        // passive_gain.py: an interaction's gains without the interaction (thread "Research passive actions framework")
        flag(L"Passive objects", L"TV gives its gains without the watch action", "tv", "engine", true, kLive),
        scriptNumber(L"Passive objects", L"Speed of passive gains", "passive", "gain_scale", 1, 0, 3, 0.1f, L"%.1fx"),
        flag(L"Passive objects", L"Skill from things nearby too", "passive", "nearby_skill", false, kLive),
        scriptNumber(L"Passive objects", L"Heard through a wall", "passive", "wall_factor", 0.4f, 0, 1, 0.05f, L"%.2f"),
        scriptNumber(L"Passive objects", L"Heard through a floor", "passive", "floor_factor", 0.2f, 0, 1, 0.05f, L"%.2f"),
        flag(L"Passive objects", L"Music gives its gains without the listen action", "passive", "music_engine", true, kLive),
        // dance.cpp + passive.py: dancing in real life to music you hear
        flag(L"Passive objects", L"Dance in real life to music", "dance", "on", true, kLive),
        scriptNumber(L"Passive objects", L"Dancing starts at movement", "dance", "energy", 0.25f, 0.05f, 1, 0.05f, L"%.2f m/s"),
        scriptNumber(L"Passive objects", L"Full dancing at movement", "dance", "full_energy", 0.7f, 0.2f, 2, 0.05f, L"%.2f m/s"),
        scriptNumber(L"Passive objects", L"Dancing needs a beat of", "dance", "rhythm", 0.35f, 0, 0.9f, 0.05f, L"%.2f"),
        scriptNumber(L"Passive objects", L"Dancing needs the music heard at", "dance", "hearing", 0.3f, 0, 1, 0.05f, L"%.2f"),
        link(L"Other Sims touch", L"How they react", L"Other Sims reactions"),
        link(L"Other Sims touch", L"Grabbing", L"Other Sims grab"),
        link(L"Other Sims touch", L"They look at your hand", L"Other Sims look at hand"),
        link(L"Touch sounds", L"On clothes", L"Touch sounds cloth"),
        link(L"Touch sounds", L"On bare skin", L"Touch sounds skin"),
        link(L"Touch sounds", L"Volume of your own sounds", L"Touch sounds volume"),
        link(L"Touch sounds", L"Hitting objects", L"Touch sounds objects"),
        link(L"Touch sounds", L"Knocking on doors and windows", L"Knocking"),
        link(L"Graphics", L"Picture", L"View picture"),
        link(L"Graphics", L"Mirrors and reflections", L"View mirrors"),
        link(L"Graphics", L"Near clipping", L"View near"),
        link(L"Graphics", L"Shadows", L"View shadows"),
        link(L"Graphics", L"Smooth motion", L"View motion"),
        link(L"Graphics", L"Depth eye and tests", L"View eyes"),
        link(L"Graphics", L"Performance and tests", L"View speed"),
        link(L"View eyes", L"Right eye alignment (depth eye)", L"View align"),
        link(L"Controls", L"Moving", L"Moving"),
        link(L"Controls", L"Walls and furniture", L"Collision"),
        link(L"Controls", L"Plants and leaves", L"Foliage"),
        link(L"Controls", L"Turning", L"Turning"),
        link(L"Controls", L"Hands and touch", L"Hands"),
        link(L"Controls", L"Hand gestures", L"Other Sims gestures"),
        link(L"Controls", L"Body Physics phone app", L"Body physics app"),
        link(L"Body physics app", L"Global settings", L"ibp:main"),
        link(L"Body physics app", L"Her Settings", L"Body app frozen Sim"),
        link(L"Body app frozen Sim", L"Size preset", L"Body app size"),
        link(L"Body app frozen Sim", L"Gravity preset", L"Body app gravity"),
        link(L"Body app frozen Sim", L"Spacing preset", L"Body app spacing"),
        link(L"Body app frozen Sim", L"More of her settings", L"ibp:sim"),
        link(testing, L"Relationships", L"Relationships"),
        link(L"Relationships", L"Friendship with the Sim picked in the Body Physics app", L"Body app friendship"),
        link(L"Relationships", L"Romance with the Sim picked in the Body Physics app", L"Body app romance"),
        link(L"Controls", L"Head and body", L"View head"),
        // Their pages come from their own menu exports once they are loaded (mergeMenus, v0.13.236/237).
        link(L"Other mods", L"Integrated Body Physics", L"ibp:main"),
        link(L"Other mods", L"Extended Sound Control", L"esc:main"),
        link(L"Other mods", L"Claude Performance", L"Performance"),
        link(L"Other mods", L"Claude Bustin Out", L"cbo:main"),
        link(L"Other mods", L"Claude Realistic Lighting", L"crl:main"),

        cheats,
        cheatPies,
        button(L"Fill needs", 5),                        // the played Sim's needs to full
        button(L"Motherlode (+50,000 simoleons)", 7),    // motherlode
        button(L"Move objects on", 8),                   // bb.moveobjects on
        button(L"CAS full edit mode", 10),               // cas.fulleditmode
        relButton(L"Everyone here hates me", 14),           // controls.py: every Sim on the lot, friendship + romance lowest
        relButton(L"Everyone here loves me", 15),           // controls.py: highest
        button(L"Fill nearby Sims' needs", 16),          // controls.py: every other Sim within cheats.fill_radius
        scriptNumber(testing, L"Fill nearby Sims' needs: radius", "cheats", "fill_radius", 3, 1, 15, 0.5f, L"%.1f m"),

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

        number(L"View picture", L"Sharpness", "stereo", "sharpen", &Config::sharpen, 0, 1, 0.05f, 100, L"%.0f%%"),
        number(L"View picture", L"Texture sharpness (mip bias, next start; 0 = off)", "stereo", "texture_bias",
               &Config::textureBias, -1, 0, 0.1f, 1, L"%.1f", kNextStart),
        toggle(L"View picture", L"FSR upscaling (off: plain sharpen at Render resolution)", "stereo", "upscale",
               &Config::upscale, kRenderScale),
        number(L"View picture", L"FSR: game draws at", "stereo", "fsr_render_scale", &Config::fsrRenderScale, 0.4f, 1,
               0.05f, 100, L"%.0f%%", kRenderScale),
        number(L"View picture", L"FSR: headset picture size", "stereo", "fsr_output_scale", &Config::fsrOutputScale,
               0.6f, 1.5f, 0.05f, 100, L"%.0f%%", kRenderScale),
        toggle(L"View picture", L"Card goes to the headset stream first", "stereo", "gpu_yield", &Config::gpuYield),
        toggle(L"View picture", L"Align the eyes (SteamVR)", "stereo", "align_eyes", &Config::alignEyes),
        number(L"View picture", L"World scale (bigger = world feels smaller)", "stereo", "world_scale", &Config::worldScale, 0.2f, 5,
               0.05f, 100, L"%.2f x", kView),
        toggle(L"View picture", L"Lopsided eye view (draws less unseen picture)", "stereo", "exact_fov", &Config::exactFov,
               kRenderScale),
        toggle(L"View motion", L"Smooth stick turning", "stereo", "turn_reprojection", &Config::turnReprojection),
        toggle(L"View motion", L"Head pose for the shown moment", "stereo", "late_pose", &Config::latePose),
        toggle(L"View motion", L"Game waits for the headset frame (less delay)", "stereo", "cap_lead", &Config::capLead),
        toggle(L"View motion", L"Stick turn on the headset's clock", "stereo", "turn_display_clock", &Config::turnDisplayClock),
        toggle(L"View motion", L"Stick walk on the headset's clock", "stereo", "walk_display_clock", &Config::walkDisplayClock),
        toggle(L"View motion", L"Draw ahead of the stick turn", "stereo", "turn_lead", &Config::turnLead),
        number(L"View motion", L"Draw-ahead strength", "stereo", "turn_lead_scale", &Config::turnLeadScale, 0, 2, 0.1f, 100, L"%.0f%%"),
        toggle(L"View motion", L"Body waits out stick turns", "body", "turn_body_lazy", &Config::bodyLazyTurn, kWalk),
        number(L"View motion", L"Body turn steps a second", "body", "turn_body_rate", &Config::bodyStandRate, 0, 60, 5, 1, L"%.0f", kWalk),
        number(L"View motion", L"Ease into stick turns", "body", "turn_ease", &Config::turnEase, 0, 1, 0.1f, 100, L"%.0f%%"),
        toggle(L"View motion", L"Both eyes drawn from one place (walking)", "stereo", "paired_eyes", &Config::pairedEyes),
        toggle(L"View motion", L"Next eye waits until the last was drawn", "stereo", "eye_wait_drawn", &Config::eyeWaitDrawn),
        toggle(L"Phone", L"Photo: phone lens as a third picture (off: the phone shows your own view; selfie looks out the glass)", "photo", "lens", &Config::photoLens),
        number(L"View motion", L"Longest wait for a drawn eye (ms)", "stereo", "eye_wait_ms", &Config::eyeWaitMs, 20, 1000, 10, 1, L"%.0f"),
        toggle(L"View motion", L"Hands match in both eyes (walking)", "stereo", "paired_hands", &Config::pairedHands),
        toggle(L"View eyes", L"Own body steady while turning", "stereo", "body_layer", &Config::bodyLayer),
        toggle(L"View eyes", L"Both eyes every frame (depth)", "stereo", "depth_eye", &Config::depthEye),
        toggle(L"View eyes", L"Flat world (clean, fast; no depth in the picture)", "stereo", "flat_world",
               &Config::flatWorld),
        toggle(L"View eyes", L"Both real eyes every frame (real depth; test)", "stereo", "both_eyes",
               &Config::bothEyes),
        toggle(L"View motion", L"Headset frames on their own clock (smooth turning)", "stereo", "frame_clock",
               &Config::frameClock),
        toggle(L"View motion", L"Draw only the width the headset needs (faster)", "stereo", "headset_width",
               &Config::headsetWidth),
        choice(L"View eyes", L"Eye shown on the desktop window", "stereo", "monitor_eye", &Config::monitorEye,
               {L"Left", L"Right"}),
        choice(L"View eyes", L"Desktop window picture shape", "stereo", "monitor_fit", &Config::monitorFit,
               {L"Stretched", L"Cropped to window", L"Whole, with bars"}),
        number(L"View eyes", L"Desktop window zoom (cropped; higher = narrower view)", "stereo", "monitor_zoom",
               &Config::monitorZoom, 1.0f, 2.5f, 0.05f, 1, L"%.2fx"),
        choice(L"View eyes", L"Right eye reuses a scene part (test)", "stereo", "both_eyes_skip", &Config::bothEyesSkip,
               {L"Off", L"Part 3", L"Part 4", L"Part 8"}),
        choice(L"View eyes", L"Depth eye test view", "stereo", "depth_eye_debug", &Config::depthEyeDebug,
               {L"Off", L"Depth", L"No shift", L"Depth edges", L"Compare with the real eye"}),
        toggle(L"View align", L"Eyes take turns (real picture behind near things)", "stereo", "depth_eye_alternate",
               &Config::depthEyeAlternate),
        // The made eye lined up by hand (user 2026-10-20 15:29Z): picture, then what the headset is told.
        number(L"View align", L"Picture: shift sideways (+ right)", "stereo", "made_eye_shift_x", &Config::madeEyeShiftX,
               -3, 3, 0.05f, 1, L"%+.2f\u00b0"),
        number(L"View align", L"Picture: shift up/down (+ up)", "stereo", "made_eye_shift_y", &Config::madeEyeShiftY,
               -3, 3, 0.05f, 1, L"%+.2f\u00b0"),
        number(L"View align", L"Picture: eye distance", "stereo", "made_eye_distance", &Config::madeEyeDistance,
               0, 2, 0.05f, 100, L"%.0f%%"),
        number(L"View align", L"Picture: depth scale", "stereo", "made_eye_depth", &Config::madeEyeDepth,
               0.5f, 2, 0.05f, 100, L"%.0f%%"),
        number(L"View align", L"Headset: eye position sideways (+ right)", "stereo", "made_eye_submit_x",
               &Config::madeEyeSubmitX, -0.02f, 0.02f, 0.0005f, 1000, L"%+.1f mm"),
        number(L"View align", L"Headset: eye turned sideways (+ right)", "stereo", "made_eye_submit_yaw",
               &Config::madeEyeSubmitYaw, -3, 3, 0.05f, 1, L"%+.2f\u00b0"),
        number(L"View align", L"Headset: eye tilted up/down (+ up)", "stereo", "made_eye_submit_pitch",
               &Config::madeEyeSubmitPitch, -3, 3, 0.05f, 1, L"%+.2f\u00b0"),
        alignReset,
        toggle(L"View speed", L"Test: draw the scene twice (log)", "stereo", "probe_double", &Config::probeDouble),
        toggle(L"View picture", L"Ambient occlusion (SSAO, next start)", "stereo", "ambient_occlusion", &Config::ambientOcclusion, kNextStart),
        benchButton(L"Run turn test (game paused)", 201,
                    L"Stand still, close this menu. After 8 s: 5 s still, 3 turns each way, 5 s still. One 'Benchmark:' line in the log. Pause the game first."),
        benchButton(L"Run turn test (game running)", 202,
                    L"Same test with the game running at speed 1 (the sim runs too). Compare it with the paused run."),
        number(L"View speed", L"Far Sims left out of shadows and mirrors (0 = off)", "perf", "far_sim_cull", &Config::farSimCull, 0, 150, 5, 1,
               L"%.0f m", kView),
        number(L"View speed", L"Far Sims not drawn in your view (0 = off)", "perf", "far_sim_cull_eyes", &Config::eyeSimCull, 0, 300, 10, 1,
               L"%.0f m", kView),
        toggle(L"View speed", L"GPU timeline (log GPU time per pass)", "perf", "gpu_timeline", &Config::gpuTimeline, kView),
        toggle(L"View speed", L"Bigger Sim caches (next start)", "perf", "sim_cache_boost", &Config::simCacheBoost, kNextStart),
        number(L"View speed", L"Sim detail distance (next start)", "perf", "sim_lod_scale", &Config::simLodScale, 0.25f, 3, 0.25f, 1, L"x%.2f", kNextStart),
        choice(L"View speed", L"Edge smoothing (MSAA) when scaled (next start)", "stereo", "render_msaa", &Config::renderMsaa,
               {L"Off", L"Off", L"2x", L"3x", L"4x"}),
        toggle(L"View speed", L"Hitch catcher (log stalls, next start)", "perf", "hitch_catcher", &Config::hitchCatcher, kNextStart),
        toggle(L"View speed", L"Try an unrecognised game build (next start)", "patch", "try_unknown_build", &Config::tryUnknownBuild, kNextStart),
        number(L"View head", L"Head follow speed", "body", "smoothing", &Config::bodySmoothing, 0.05f, 1, 0.05f, 1, L"%.2f",
               kView),
        number(L"View head", L"Turn with your Sim", "body", "turn_smoothing", &Config::bodyTurnSmoothing, 0, 1, 0.05f, 1,
               L"%.2f", kView),
        number(L"View head", L"View behind the eyes", "body", "eye_inset", &Config::bodyEyeInset, 0, 0.1f, 0.005f, 100,
               L"%.1f cm", kView),
        link(L"View head", L"Place your view (try out)", L"View place"),
        number(L"View place", L"Right (+) / left (-)", "body", "head_x", &Config::bodyHeadX, -0.15f, 0.15f, 0.005f, 100, L"%.1f cm", kView),
        number(L"View place", L"Up (+) / down (-)", "body", "head_y", &Config::bodyHeadY, -0.15f, 0.15f, 0.005f, 100, L"%.1f cm", kView),
        number(L"View place", L"Ahead (+) / back (-)", "body", "head_z", &Config::bodyHeadZ, -0.15f, 0.15f, 0.005f, 100, L"%.1f cm", kView),
        toggle(L"View head", L"Your Sim's head follows your look", "body", "head_tracking", &Config::headTracking, kHands),
        toggle(L"View head", L"Seated: your Sim leans with you", "body", "seat_follow", &Config::bodySeatFollow, kView),
        toggle(L"View head", L"Seated: your head moves the upper body", "body", "seat_torso", &Config::bodySeatTorso, kView),
        link(L"View head", L"Seated: where you sit", L"Seat place"),
        number(L"Seat place", L"Back from screen (+) / closer (-)", "body", "seat_back", &Config::bodySeatBack, -0.3f, 1, 0.01f, 100, L"%.0f cm", kView),
        number(L"Seat place", L"Up (+) / down (-)", "body", "seat_up", &Config::bodySeatUp, -0.3f, 0.3f, 0.01f, 100, L"%.0f cm", kView),
        number(L"Seat place", L"Your view may leave the Sim's eyes by", "body", "seat_hold", &Config::bodySeatHold, 0.02f, 1, 0.02f, 100, L"%.0f cm", kView),
        number(L"Seat place", L"Ignore the Sim's idle sway under", "body", "seat_still", &Config::bodySeatStill, 0, 0.2f, 0.01f, 100, L"%.0f cm", kView),
        toggle(L"View head", L"Hide your own face (mirrors keep it)", "body", "hide_face", &Config::bodyHideFace, kView),
        toggle(L"View head", L"Steady wrist HUD (idle doesn't move it)", "body", "steady_shoulders",
               &Config::bodySteadyShoulders, kView),
        number(L"View near", L"Near clipping", "body", "near_clip", &Config::bodyNearClip, 0.02f, 0.5f, 0.01f, 100, L"%.0f cm",
               kView),
        toggle(L"View near", L"Near clipping follows what's close", "body", "near_adaptive", &Config::bodyNearAdaptive, kView),
        number(L"View near", L"Near clipping at most", "body", "near_max", &Config::bodyNearMax, 0.1f, 2, 0.05f, 100, L"%.0f cm",
               kView),
        flag(L"View near", L"Rooms in doorways stay whole", "body", "rooms_whole", true, kLive),  // gamefix.cpp
        flag(L"View near", L"Every room always drawn whole", "body", "rooms_all", true, kLive),  // gamefix.cpp
        number(L"View mirrors", L"Mirrors reach in rooms (0 = game's)", "stereo", "mirror_far_inside", &Config::mirrorInterior, 0, 15,
               1, 1, L"%.0f m", kNextStart),
        number(L"View mirrors", L"Mirrors and water reach outdoors (0 = game's)", "stereo", "mirror_far_outside",
               &Config::mirrorExterior, 0, 100, 5, 1, L"%.0f m", kNextStart),
        toggle(L"View mirrors", L"Mirrors clip at the glass (may pop objects)", "stereo", "mirror_clip", &Config::mirrorClip,
               kLive),
        toggle(L"View mirrors", L"Pool reflection clipped at the water (nothing below it)", "stereo", "water_clip", &Config::waterClip,
               kLive),
        toggle(L"View mirrors", L"Pool reflection in the main pool's plane (not a mean of all)", "stereo", "water_one_plane",
               &Config::waterOnePlane, kLive),
        toggle(L"View mirrors", L"No mirror or water reflections", "stereo", "reflections_off", &Config::reflectionsOff,
               kNextStart),
        toggle(L"View shadows", L"Right eye of a pair reuses the left eye's shadows (walking/turning, faster)", "stereo", "pair_shadows",
               &Config::pairShadows),
        toggle(L"View shadows", L"Right eye reuses the left eye's shadows (both real eyes, faster)", "stereo", "reuse_shadows",
               &Config::reuseShadows),
        toggle(L"View shadows", L"Right eye skips preparing shadows (faster)", "stereo",
               "skip_shadow_prep", &Config::skipShadowLists),
        toggle(L"View shadows", L"Steady shadows", "body", "steady_shadows", &Config::steadyShadows, kShadows),
        number(L"View shadows", L"Shadow map size (0 = game's, next start)", "stereo", "shadow_map", &Config::shadowMap, 0, 4096,
               1024, 1, L"%.0f px", kNextStart),
        number(L"View shadows", L"Shadows follow the sun every", "body", "shadow_step", &Config::shadowStep, 0, 2, 0.05f, 1,
               L"%.2f°", kShadows),
        number(L"View shadows", L"Sharpest shadows reach", "body", "shadow_sharp", &Config::shadowSharp, 0, 10, 0.5f, 1,
               L"%.1f m", kShadows),

        number(moving, L"Run speed (tap left stick)", "body", "run_speed", &Config::runSpeed, 1, 8, 0.1f, 1, L"%.1f m/s",
               kWalk),
        number(L"Turning", L"Game camera turn speed", "controls", "turn_speed", &Config::turnSpeed, 200, 3000, 100, 1,
               L"%.0f"),
        flag(moving, L"Stairs and ladders carry you", "body", "carried_stairs", true),
        flag(L"Swimming", L"Stick takes you into and out of the water", "swim", "enter_exit", true),  // swim.py
        toggle(moving, L"Body drawn at your height (stairs)", "body", "keep_height", &Config::bodyKeepHeight, kWalk),
        toggle(moving, L"Slide smoothly along walls", "body", "smooth_slide", &Config::bodySmoothSlide, kWalk),
        flag(L"Collision", L"Map walls and furniture around you", "body", "collision_scene", true, kLive),  // collide.py
        toggle(L"Collision", L"Walls and furniture stop you", "body", "collision", &Config::bodyCollision, kWalk),
        number(L"Collision", L"Body keeps clear of walls by", "body", "body_radius", &Config::bodyRadius, 0.1f, 0.5f, 0.025f, 100,
               L"%.1f cm", kWalk),
        number(L"Collision", L"Body keeps clear of furniture by", "body", "furniture_radius", &Config::furnitureRadius, 0.05f, 0.5f,
               0.01f, 100, L"%.0f cm", kWalk),
        toggle(L"Collision", L"Hands stop at walls and furniture", "body", "hand_collision", &Config::handCollision, kWalk),
        number(L"Collision", L"Hand gap to furniture", "body", "hand_object_gap", &Config::handObjectGap, -0.05f, 0.05f,
               0.0025f, 100, L"%.2f cm", kWalk),
        number(L"Collision", L"Hand gap to walls", "body", "hand_wall_gap", &Config::handWallGap, -0.05f, 0.05f, 0.0025f, 100,
               L"%.2f cm", kWalk),
        flag(L"Collision", L"Neighbourhood scenery stops you", "body", "world_scenery", true, kLive),  // collide.py
        toggle(L"Collision", L"Walk through bushes and flowers", "body", "soft_plants", &Config::softPlants, kWalk),
        number(L"Collision", L"Bushes slow you by up to", "body", "plant_slowdown", &Config::plantSlowdown, 0, 0.9f, 0.05f,
               100, L"%.0f%%", kWalk),
        toggle(L"Collision", L"Step over low objects", "body", "step_over", &Config::stepOver, kWalk),
        toggle(L"Collision", L"Step over any low object, not only rocks", "body", "step_all", &Config::stepAll, kWalk),  // collide.py
        number(L"Collision", L"Tallest object you step over", "body", "step_height", &Config::stepHeight, 0.1f, 1.0f, 0.05f, 100,
               L"%.0f cm", kWalk),
        toggle(L"Foliage", L"Plants and leaves give way to your body", "foliage", "push", &Config::foliagePush),
        toggle(L"Foliage", L"Prepare the shaders for it (next start)", "foliage", "patch_shaders", &Config::foliagePatch,
               kNextStart),
        number(L"Foliage", L"Push at the closest", "foliage", "push_cm", &Config::foliagePushCm, 5, 80, 5, 1, L"%.0f cm"),
        toggle(L"Foliage", L"Your head pushes", "foliage", "head", &Config::foliageHead),
        number(L"Foliage", L"Head reach", "foliage", "head_radius", &Config::foliageHeadRadius, 0.1f, 0.6f, 0.025f, 100,
               L"%.1f cm"),
        toggle(L"Foliage", L"Your hands push", "foliage", "hands", &Config::foliageHands),
        number(L"Foliage", L"Hand reach", "foliage", "hand_radius", &Config::foliageHandRadius, 0.05f, 0.4f, 0.01f, 100,
               L"%.0f cm"),
        toggle(L"Foliage", L"Your body pushes", "foliage", "body", &Config::foliageBody),
        number(L"Foliage", L"Body reach", "foliage", "body_radius", &Config::foliageBodyRadius, 0.1f, 0.8f, 0.025f, 100,
               L"%.1f cm"),
        toggle(L"Foliage", L"Other Sims push", "foliage", "sims", &Config::foliageSims),
        number(L"Foliage", L"Test: every leaf rises by (0 = off)", "foliage", "lift_cm", &Config::foliageLiftCm, 0, 100, 5, 1,
               L"%.0f cm"),
        flag(moving, L"Walking animation on stairs", "body", "stairs_walk", true, kLive),  // stride.py
        toggle(moving, L"Start and stop walking animations", "body", "walk_clips", &Config::bodyWalkClips, kWalk),
        number(moving, L"Stop smoothness (0 = instant)", "body", "stop_time", &Config::bodyStopTime, 0, 1.5f, 0.05f, 1,
               L"%.2f s", kWalk),
        toggle(L"Crouch", L"Crouch (push right stick down)", "body", "crouch", &Config::crouch, kWalk),
        number(L"Crouch", L"Crouch depth", "body", "crouch_depth", &Config::crouchDepth, 0.1f, 0.9f, 0.05f, 100, L"%.0f cm",
               kWalk),
        number(L"Crouch", L"Crouch walking speed", "body", "crouch_speed", &Config::crouchSpeed, 0.1f, 1, 0.05f, 100,
               L"%.0f%%", kWalk),
        number(L"Crouch", L"Crouch down/up time", "body", "crouch_seconds", &Config::crouchSeconds, 0.1f, 1.5f, 0.05f, 1,
               L"%.2f s", kWalk),
        toggle(moving, L"Glide out over the stop animation", "body", "stop_glide", &Config::bodyStopGlide, kWalk),

        flag(L"Reach", L"Only use what's within 1 m", "body", "reach_only", true, kLive),  // reach.py
        flag(L"Doors", L"Doors open by hand", "body", "manual_doors", true, kLive),  // doors.py, collide.py
        number(L"Doors", L"Door push: hand speed that opens it", "body", "door_push_speed", &Config::doorPushSpeed, 0.3f, 3,
               0.1f, 1, L"%.1f m/s"),  // doors.py
        flag(L"Doors", L"Doors need a handle grab", "body", "door_handle", true, kLive),  // doors.py
        number(L"Doors", L"Door handle reach", "body", "door_handle_reach", &Config::doorHandleReach, 0.1f, 1.0f, 0.05f, 100,
               L"%.0f cm"),  // doors.py
        number(L"Doors", L"Sliding door: how far to slide it", "body", "door_slide", &Config::doorSlide, 0.05f, 0.5f, 0.05f, 100,
               L"%.0f cm"),  // doors.py
        flag(L"Doors", L"Doors swing the other way", "body", "door_flip", false, kLive),  // doors.py
        flag(L"Doors", L"Open the fridge door to cook", "body", "fridge_door", true, kLive),  // objmenu.py
        number(L"Doors", L"Fridge door: how far to pull it open", "body", "fridge_door_pull", &Config::fridgeDoorPull, 0.05f, 0.6f,
               0.05f, 100, L"%.0f cm"),  // objmenu.py
        flag(L"Reach", L"Grey out what is too far", "body", "grey_far", true, kLive),  // reach.py
        flag(L"Reach", L"Reactions don't stop you", "body", "reactions_in_place", true, kLive),  // scripted.py
        flag(L"Reach", L"The game can't send you to things", "body", "no_errands", true, kLive),  // scripted.py
        flag(L"Reach", L"Nothing violent happens on its own", "body", "no_auto_violence", true, kLive),  // scripted.py
        flag(L"Reach", L"Stop walks you didn't ask for", "body", "route_guard", true, kLive),  // halt.py

        number(hands, L"Hand angle", "body", "hand_pitch", &Config::handPitch, 0, 90, 5, 1, L"%.0f°", kHands),
        flag(hands, L"Eat with your hands", "hands", "eat", true),
        toggle(hands, L"Vibrate on touch", "physical", "haptics", &Config::touchHaptics),

        toggle(L"Other Sims touch", L"Their bodies stop your hands", "physical", "bodies", &Config::touchBodies, kTouch),
        toggle(L"Other Sims touch", L"Hold hands", "physical", "hold_hands", &Config::holdHands, kTouch),
        toggle(L"Other Sims talk", L"They look at you", "physical", "look_at_you", &Config::lookAtYou, kTouch),
        toggle(L"Other Sims touch", L"Your head stays out of them", "physical", "head", &Config::headRepel, kTouch),
        number(L"Other Sims touch", L"Head keeps clear by", "physical", "head_radius", &Config::headRadius, 0.02f, 0.2f, 0.01f, 100, L"%.0f cm", kTouch),
        toggle(L"Other Sims grab", L"Grab their bodies (grip + trigger)", "physical", "grab_bodies", &Config::grabBodies, kTouch),
        number(L"Other Sims grab", L"Grab: finger bend", "physical", "grab_curl", &Config::grabCurl, 0, 1, 0.05f, 100, L"%.0f%%", kTouch),
        number(L"Other Sims grab", L"Holding a breast: finger bend", "physical", "breast_grab_curl", &Config::breastCurl, 0, 1, 0.05f, 100, L"%.0f%%", kTouch),
        number(L"Other Sims grab", L"Holding the butt: finger bend", "physical", "butt_grab_curl", &Config::buttCurl, 0, 1, 0.05f, 100, L"%.0f%%", kTouch),
        number(L"Other Sims grab", L"Grab: how far you drag them", "physical", "grab_drag", &Config::grabDrag, 0, 0.6f, 0.025f, 100,
               L"%.1f cm", kTouch),
        flag(L"Other Sims grab", L"They react to being grabbed", "physical", "grab_reactions", true, kLive),  // bodygrab.py
        flag(L"Other Sims reactions", L"They react to being touched", "physical", "touch_reactions", true, kLive),  // bodygrab.py
        scriptNumber(L"Other Sims reactions", L"Touch counts only within your look", "physical", "touch_look_angle", 50, 10, 180, 5,
                     L"%.0f°"),  // bodygrab.py: 180 = anywhere
        scriptNumber(L"Other Sims reactions", L"Touch counts from (lighter is ignored)", "physical", "touch_min_speed", 0.15f, 0, 1.5f,
                     0.05f, L"%.2f m/s"),  // bodygrab.py
        flag(L"Other Sims reactions", L"Touches change your relationship", "physical", "touch_relationship", true, kLive),  // bodygrab.py
        // reactions.py: what they do about your touches, and how they show what they feel about you
        flag(L"Other Sims reactions", L"A hit or unwelcome touch stops what they do", "physical", "touch_interrupts", true, kLive),
        flag(L"Other Sims reactions", L"They act on it (slap back, yell, hug, kiss...)", "physical", "touch_consequences", true, kLive),
        flag(L"Other Sims reactions", L"Hits leave moodlets (angry, hurt, scared)", "physical", "touch_moodlets", true, kLive),
        scriptNumber(L"Other Sims reactions", L"How often they act on it", "physical", "consequence_chance", 1, 0, 3, 0.25f, L"%.2fx"),
        flag(L"Other Sims reactions", L"Hits can start fights", "physical", "touch_fights", true, kLive),
        flag(L"Other Sims reactions", L"Sims who see it call you out", "physical", "touch_witnesses", true, kLive),
        flag(L"Other Sims reactions", L"They show how they feel about you", "physical", "show_feelings", true, kLive),
        // bodygrab.force_of: how hard a touch was and how much it hurts there
        scriptNumber(L"Other Sims hit strength", L"A slap from", "physical", "slap_speed", 1.8f, 0.8f, 4, 0.1f, L"%.1f m/s"),
        scriptNumber(L"Other Sims hit strength", L"A hard hit from", "physical", "hard_hit_speed", 3, 1.5f, 6, 0.1f, L"%.1f m/s"),
        scriptNumber(L"Other Sims hit strength", L"How much hits hurt", "physical", "hit_weight", 0.9f, 0, 2, 0.05f, L"%.2fx"),
        scriptNumber(L"Other Sims hit strength", L"Counts as a hit from (pain)", "physical", "hit_from", 0.3f, 0, 1.5f, 0.05f, L"%.2f"),
        scriptNumber(L"Other Sims hit strength", L"Counts as a hard hit from (pain)", "physical", "hard_pain", 0.8f, 0.2f, 2, 0.05f, L"%.2f"),
        scriptNumber(L"Other Sims hit strength", L"How much a hard pull hurts", "physical", "pull_pain", 0.5f, 0, 1.5f, 0.05f, L"%.2fx"),
        scriptNumber(L"Other Sims hit strength", L"How close you must be for intimate places", "physical", "intimacy_weight", 0.6f, 0, 1.5f, 0.05f, L"%.2fx"),
        flag(L"Other Sims hit strength", L"Head slaps: surprised (or angry) first", "physical", "head_hit_first", true, kLive),
        scriptNumber(L"Other Sims hit strength", L"Head slaps: what they do next comes after", "physical", "head_hit_delay", 1.5f, 0, 4, 0.25f, L"%.2f s"),
        flag(L"Other Sims hit strength", L"Light hits can be friendly", "physical", "friendly_hits", true, kLive),
        scriptNumber(L"Other Sims hit strength", L"Friendly up to", "physical", "friendly_max_speed", 2.4f, 0.5f, 4, 0.1f, L"%.1f m/s"),
        toggle(L"Other Sims look at hand", L"They look at the hand that touches them", "physical", "look_at_hand", &Config::lookAtHand, kTouch),
        number(L"Other Sims look at hand", L"Look: how fast it moves", "physical", "look_speed", &Config::lookSpeed, 1, 20, 0.5f, 1, L"%.1f", kTouch),
        number(L"Other Sims look at hand", L"Look: eyes only within", "physical", "look_eyes_within", &Config::lookEyesWithin, 0, 1, 0.05f, 100, L"%.0f cm", kTouch),
        number(L"Other Sims look at hand", L"Look: whole head from", "physical", "look_head_from", &Config::lookHeadFrom, 0.1f, 2, 0.05f, 100, L"%.0f cm", kTouch),
        scriptNumber(L"Other Sims talk", L"They follow you from (0 off)", "body", "follow_distance", 2.5f, 0, 10, 0.5f,
                     L"%.1f m"),  // hint.py: the Sims you talk with follow you
        scriptNumber(L"Other Sims talk", L"Talk starts within", "body", "talk_distance", 3, 1, 10, 0.5f,
                     L"%.1f m"),  // scripted.py, reach.py, trail.py: closer, a picked social just starts
        flag(L"Other Sims talk", L"Their chats with you stay where you are", "body", "socials_in_place", true, kLive),  // scripted.py
        flag(L"Other Sims talk", L"Sims turn to face you in interactions", "body", "face_you", true, kLive),  // facing.py
        scriptNumber(L"Other Sims talk", L"They turn when facing away more than", "body", "face_angle", 45, 15, 120, 5,
                     L"%.0f°"),  // facing.py
        flag(L"Other Sims hugs", L"Hugs and kisses fit your body", "pair", "anchor", true, kLive),  // pair.py
        flag(L"Other Sims hugs", L"Partner steps onto its spot at the start", "pair", "snap", true, kLive),  // pair.py
        flag(L"Other Sims hugs", L"Hugs and kisses hold you in place", "pair", "lock", true, kLive),  // pair.py
        scriptNumber(L"Other Sims hugs", L"Kiss: partner steps in by", "pair", "kiss_in", 10, 0, 40, 1, L"%.0f cm"),  // pair.py
        // physapp.py: the phone's Body Physics app picks a Sim by touch or by looking (thread "Add body physics phone app")
        flag(L"Body physics app", L"Touching a Sim picks her while picking", "bodyphysics", "pick_by_touch", true, kLive),
        scriptNumber(L"Body physics app", L"Looking at a Sim picks her after", "bodyphysics", "dwell_seconds", 1.2f, 0.4f, 4, 0.1f,
                     L"%.1f s"),
        scriptNumber(L"Body physics app", L"Looking: within (of the middle of your view)", "bodyphysics", "dwell_angle", 8, 2, 25, 1,
                     L"%.0f deg"),
        flag(L"Body physics app", L"Outfit changes play the change spin", "bodyphysics", "outfit_spin", true, kLive),
        scriptNumber(L"Body physics app", L"Picking mode ends after", "bodyphysics", "pick_timeout", 60, 10, 300, 10, L"%.0f s"),
        scriptNumber(L"Body physics app", L"Pick the Sim I look at: within", "bodyphysics", "look_angle", 15, 4, 40, 1, L"%.0f deg"),
        // gestures.py: hand gestures at other Sims (thread "Hand gesture interactions with Sims")
        flag(L"Other Sims gestures", L"Wave: they wave back", "gestures", "wave", true, kLive),
        flag(L"Other Sims gestures", L"Open-palm come hither: they walk to you", "gestures", "come_hither", true, kLive),
        flag(L"Gesture follow", L"Crooked finger (grip + trigger twice): Follow me", "gestures", "follow_me", true, kLive),
        flag(L"Gesture follow", L"Follow me: they react first (mood, relationship)", "gestures", "follow_reactions", true, kLive),
        flag(L"Gesture follow", L"Follow me: Sims who dislike you refuse", "gestures", "follow_refuse", true, kLive),
        flag(L"Other Sims high five", L"High five: raise an open hand and hold it", "gestures", "high_five", true, kLive),
        number(L"Other Sims high five", L"High five: hold your hand up for", "gestures", "high_five_hold", &Config::highFiveHold, 0.5f, 6, 0.25f, 1, L"%.2f s", kTouch),
        flag(L"Other Sims high five", L"Tap or poke twice from behind: they turn to you", "gestures", "tap_to_turn", true, kLive),
        flag(L"Gesture follow", L"Wave at a follower: they wave and stop following", "gestures", "wave_dismiss", true, kLive),
        flag(L"Gesture shoo", L"Shoo (both palms down, flick up): they go away", "gestures", "shoo", true, kLive),
        number(L"Gesture shoo", L"Shoo: how far the fingers swing up", "gestures", "shoo_angle", &Config::shooAngle, 20, 80, 5, 1, L"%.0f deg", kTouch),
        number(L"Gesture shoo", L"Shoo: flicks needed", "gestures", "shoo_flicks", &Config::shooFlicks, 1, 4, 1, 1, L"%.0f", kTouch),
        flag(L"Gesture follow", L"They nod as they come or start following", "gestures", "nod", true, kLive),
        flag(L"Other Sims gestures", L"Vibrate when a hand gesture is seen", "gestures", "haptics", false, kLive),
        // foley.py: the game's foley when your hands touch other Sims (thread "Add touch foley sounds for Sim contact");
        // the names must match foley.py's lists.
        flag(L"Touch sounds", L"Play touch sounds", "foley", "on", true, kLive),
        scriptNumber(L"Touch sounds", L"A pat from (into them)", "foley", "pat_speed", 0.5f, 0.1f, 3, 0.1f, L"%.1f m/s"),
        scriptNumber(L"Touch sounds", L"A slap from (into them)", "foley", "slap_speed", 2.2f, 0.5f, 6, 0.1f, L"%.1f m/s"),
        // armik.cpp foleyTrack / knockBack, foleyaudio.cpp (user 2026-10-26 21:30Z)
        scriptNumber(L"Touch sounds", L"A punch from (fist, into them)", "foley", "punch_speed", 1.8f, 0.5f, 6, 0.1f,
                     L"%.1f m/s"),
        flag(L"Other Sims hits", L"Hits while you walk with the stick", "foley", "walk_hits", true, kLive),
        percent(scriptNumber(L"Other Sims hits", L"Knockback where you hit", "foley", "knockback", 1.0f, 0, 2, 0.05f,
                             L"%.0f%%")),
        // Shoves (thread "Build aggressive shove gesture"): armik.cpp shove(), bodygrab.py shove()
        flag(L"Shoves", L"Shoving Sims on", "foley", "shove", true, kLive),
        scriptNumber(L"Shoves", L"A shove from (palm straight into them)", "foley", "shove_speed", 1.3f, 0.5f, 5, 0.1f,
                     L"%.1f m/s"),
        scriptNumber(L"Shoves", L"Sideways share allowed (more = swipes count too)", "foley", "shove_ratio", 0.7f, 0.2f, 2,
                     0.05f, L"%.2f"),
        scriptNumber(L"Shoves", L"Push: share of your hand's speed they take", "foley", "shove_force", 1.5f, 0.2f, 4, 0.05f,
                     L"%.2f"),
        scriptNumber(L"Shoves", L"They fall from (else they stumble)", "foley", "shove_fall_speed", 2.8f, 1, 8, 0.1f,
                     L"%.1f m/s"),
        scriptNumber(L"Shoves", L"A fallen Sim lies limp for", "foley", "shove_limp", 3.0f, 0.5f, 15, 0.5f, L"%.1f s"),
        scriptNumber(L"Shoves", L"How much a shove hurts", "physical", "shove_pain", 0.45f, 0, 1.5f, 0.05f, L"%.2f"),
        flag(L"Shoves", L"Light shoves from friends are play", "physical", "friendly_shoves", true, kLive),
        // ragdoll.cpp (thread "Build ragdoll system for punching", docs/RAGDOLL.md)
        flag(L"Ragdoll", L"Test mode: ragdoll showcase (flinch, soft limits, relaxed pose, hit spin at test values)", "ragdoll",
             "showcase", true, kLive),
        flag(L"Ragdoll", L"Ragdoll falls on", "ragdoll", "on", true, kLive),
        flag(L"Ragdoll", L"Test: your slaps and punches drop the Sim limp", "ragdoll", "test_on_hit", false, kLive),
        scriptNumber(L"Ragdoll", L"Blow: share of your hand's speed they take", "ragdoll", "strength", 0.5f, 0, 3, 0.05f,
                     L"%.2f"),
        scriptNumber(L"Ragdoll", L"Limp for (then they blend back)", "ragdoll", "limp_seconds", 4, 0.5f, 30, 0.5f,
                     L"%.1f s"),
        scriptNumber(L"Ragdoll", L"Gravity", "ragdoll", "gravity", 1.0f, 0.2f, 2, 0.1f, L"%.1f x"),
        flag(L"Ragdoll", L"Knocked out Sims fall as ragdolls", "ragdoll", "on_knockout", true, kLive),
        scriptNumber(L"Ragdoll", L"Knocked out: ragdoll lasts", "ragdoll", "knockout_limp", 3.0f, 0.5f, 15, 0.5f,
                     L"%.1f s"),
        scriptNumber(L"Ragdoll", L"Soft joint limits (limbs and spine slow before the end)", "ragdoll", "soft_limits", 0.8f, 0, 1, 0.1f,
                     L"%.1f"),
        scriptNumber(L"Ragdoll", L"Relaxed pose (knees and elbows settle slightly bent)", "ragdoll", "relaxed", 0.5f, 0, 2, 0.1f,
                     L"%.1f"),
        scriptNumber(L"Ragdoll", L"Hit spin (arms trail the blow, legs lag)", "ragdoll", "hit_spin", 0.5f, 0, 2, 0.1f, L"%.1f"),
        flag(L"Ragdoll", L"Flinch: hits make the struck part give way and spring back", "ragdoll", "flinch", true, kLive),
        scriptNumber(L"Ragdoll", L"Flinch: share of your hand's speed they take", "ragdoll", "flinch_strength", 0.35f, 0, 2, 0.05f,
                     L"%.2f"),
        scriptNumber(L"Ragdoll", L"Flinch: back to the pose in", "ragdoll", "flinch_recover", 0.4f, 0.1f, 2, 0.05f,
                     L"%.2f s"),
        scriptNumber(L"Ragdoll", L"Flinch: stiffness at the hit part", "ragdoll", "flinch_local", 0.1f, 0, 1, 0.05f, L"%.2f"),
        scriptNumber(L"Ragdoll", L"Flinch: stiffness of the rest", "ragdoll", "flinch_body", 0.9f, 0, 1, 0.05f, L"%.2f"),
        scriptNumber(L"Ragdoll", L"Flinch: spread from the hit", "ragdoll", "flinch_spread", 0.45f, 0.1f, 1.5f, 0.05f,
                     L"%.2f m"),
        scriptNumber(L"Ragdoll", L"Stagger: how fast the body returns", "ragdoll", "stagger_omega", 5.0f, 2, 12, 0.5f, L"%.1f /s"),
        scriptNumber(L"Ragdoll", L"Stagger: upper body leans further by", "ragdoll", "stagger_lean", 0.7f, 0, 2, 0.05f, L"%.2f"),
        scriptNumber(L"Ragdoll", L"Stagger: a foot steps when the body is this far", "ragdoll", "stagger_step", 0.08f, 0.02f, 0.4f, 0.01f,
                     L"%.2f m"),
        scriptNumber(L"Ragdoll", L"Stagger: one step takes", "ragdoll", "stagger_step_time", 0.22f, 0.08f, 0.6f, 0.02f, L"%.2f s"),
        flag(L"Ragdoll", L"Bumps into walls and furniture", "ragdoll", "world", true, kLive),
        scriptNumber(L"Ragdoll", L"Stays within (of where the Sim stands, 0 = free)", "ragdoll", "tether", 1.2f, 0, 4, 0.1f,
                     L"%.1f m"),
        scriptNumber(L"Ragdoll", L"Blend back takes", "ragdoll", "blend_out", 1.0f, 0.1f, 3, 0.1f, L"%.1f s"),
        scriptNumber(L"Ragdoll", L"Damping (how fast motion dies away)", "ragdoll", "damping", 1.5f, 0, 4, 0.1f,
                     L"%.1f"),
        // knockout.py (user 2026-10-26 22:03Z)
        flag(L"Knockout", L"Knock Sims out", "knockout", "on", true, kLive),
        scriptNumber(L"Knockout", L"Knocked out at (daze)", "knockout", "score", 40, 5, 200, 5, L"%.0f"),
        scriptNumber(L"Knockout", L"One blow knocks out from (0 = never)", "knockout", "one_blow", 6.0f, 0, 12,
                     0.5f, L"%.1f m/s"),
        scriptNumber(L"Knockout", L"Daze halves every", "knockout", "recover", 8, 1, 60, 1, L"%.0f s"),
        flag(L"Knockout", L"Knocked out unconscious (off: asleep)", "knockout", "unconscious", true, kLive),
        scriptNumber(L"Knockout", L"Knocked out for", "knockout", "minutes", 20, 1, 180, 1, L"%.0f Sim min"),
        scriptNumber(L"Touch sounds", L"Rub sounds from (0 = none)", "foley", "rub_speed", 0.3f, 0, 2, 0.05f,
                     L"%.2f m/s"),
        // foleyaudio.cpp: the user's own sounds (sounds/touch), before the game's
        flag(L"Touch sounds volume", L"Your own sounds first (sounds folder)", "foley", "own_sounds", true, kLive),
        percent(scriptNumber(L"Touch sounds volume", L"All your own sounds", "foley", "own_volume", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Light touch", "foley", "volume_touch", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Pat", "foley", "volume_pat", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Slap", "foley", "volume_slap", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Punch (fist)", "foley", "volume_punch", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Grab (grip + trigger)", "foley", "volume_grab", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Objects: tap", "foley", "volume_object_tap", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Objects: slap", "foley", "volume_object_slap", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        percent(scriptNumber(L"Touch sounds volume", L"Objects: punch", "foley", "volume_object_punch", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        // Knocking (user 2026-11-01): fist (grip + trigger) into a door or window; armik.cpp objectTrack, knock.py, sounds\\knock
        flag(L"Knocking", L"Knock on doors and windows", "foley", "knock", true, kLive),
        scriptNumber(L"Knocking", L"A knock from (into it)", "foley", "knock_speed", 0.2f, 0.05f, 3, 0.05f, L"%.2f m/s"),
        percent(scriptNumber(L"Knocking", L"Knock volume", "foley", "volume_knock", 1.0f, 0, 2, 0.05f, L"%.0f%%")),
        flag(L"Knocking", L"Log the doors and windows found and knocks", "knock", "log", true, kLive),
        // Your hand hitting objects (armik.cpp objectTrack, collide.cpp handContact, foley.py; user 2026-10-27)
        flag(L"Touch sounds objects", L"Sounds when you hit objects and walls", "foley", "objects", true, kLive),
        scriptNumber(L"Touch sounds objects", L"A tap from (into it)", "foley", "object_tap_speed", 0.8f, 0.2f, 4, 0.1f, L"%.1f m/s"),
        foley(L"Touch sounds objects", L"Tap (game sound)", "object_tap", &Config::foleyObjectTap, 12,
              {L"None", L"Hand on tile", L"Hand on door frame", L"Hand plant", L"Quiet tap", L"Put down",
               L"Finger tap", L"Clay thump", L"Wall knocks (spooky)"}),
        foley(L"Touch sounds objects", L"Slap (game sound)", "object_slap", &Config::foleyObjectSlap, 13,
              {L"None", L"Table slam", L"Hand hit", L"Game piece slam", L"Bang on keyboard", L"Hand on door frame"}),
        foley(L"Touch sounds objects", L"Punch (game sound)", "object_punch", &Config::foleyObjectPunch, 14,
              {L"None", L"Table slam", L"Fist pound", L"Head bonk", L"Bang fist on door", L"Hand hit"}),
        foley(L"Touch sounds objects", L"Metal tap (game sound)", "object_metal_tap", &Config::foleyObjectMetalTap, 17,
              {L"None", L"Metal ladder hand", L"Ladder grab", L"Hand on tile"}),
        foley(L"Touch sounds objects", L"Metal slap or punch (game sound)", "object_metal", &Config::foleyObjectMetal, 15,
              {L"None", L"Locker punch", L"Bang fist on door", L"Locker kick"}),
        foley(L"Touch sounds objects", L"Soft (game sound)", "object_soft", &Config::foleyObjectSoft, 16,
              {L"None", L"Hand on blanket", L"Cloth rustle", L"Back pat", L"Clay thump", L"Body grab"}),
        foley(L"Touch sounds", L"Hands meet (pat or faster)", "hands", &Config::foleyHands, 10,
              {L"None", L"High five", L"Hand clasp", L"Clasp (soft)"}),
        foley(L"Touch sounds", L"Taking their hand", "hold_hands", &Config::foleyHoldHands, 11,
              {L"None", L"Hand clasp", L"Clasp (soft)", L"Handshake"}),
        foley(L"Touch sounds cloth", L"Light touch", "light_cloth", &Config::foleyLightCloth, 0,
              {L"None", L"Cloth rustle", L"Cloth brush", L"Arm squeeze"}),
        foley(L"Touch sounds cloth", L"Pat", "pat_cloth", &Config::foleyPatCloth, 2,
              {L"None", L"Shoulder pat", L"Back pat", L"Arm punch", L"Cloth rustle"}),
        foley(L"Touch sounds cloth", L"Slap", "slap_cloth", &Config::foleySlapCloth, 4,
              {L"None", L"Leg slap", L"Knee slap", L"Arm punch"}),
        foley(L"Touch sounds cloth", L"Rub", "rub_cloth", &Config::foleyRubCloth, 6,
              {L"None", L"Cloth brush", L"Arm squeeze", L"Cloth rustle"}),
        foley(L"Touch sounds cloth", L"Grab (grip + trigger)", "grab_cloth", &Config::foleyGrabCloth, 8,
              {L"None", L"Cloth grab", L"Body grab", L"Hug squeeze"}),
        foley(L"Touch sounds skin", L"Light touch", "light_skin", &Config::foleyLightSkin, 1,
              {L"None", L"Soft touch", L"Knee touch", L"Cuddle touch", L"Hand pat"}),
        foley(L"Touch sounds skin", L"Pat", "pat_skin", &Config::foleyPatSkin, 3,
              {L"None", L"Hand pat", L"Thigh pat", L"Knee pat", L"Leg pat"}),
        foley(L"Touch sounds skin", L"Slap", "slap_skin", &Config::foleySlapSkin, 5,
              {L"None", L"Hand slap", L"Quick slap", L"Spin slap", L"Slap fight", L"Werewolf slap"}),
        foley(L"Touch sounds skin", L"Rub", "rub_skin", &Config::foleyRubSkin, 7,
              {L"None", L"Arm rub", L"Snuggle rub", L"Back rub"}),
        foley(L"Touch sounds skin", L"Grab (grip + trigger)", "grab_skin", &Config::foleyGrabSkin, 9,
              {L"None", L"Arm squeeze", L"Soft touch", L"Hand pat"}),

        choice(L"Interface", L"Game menus in first person", "ui", "first_person", &Config::uiFirstPerson,
               {L"Hidden", L"Wrist HUD only", L"Game UI in front only", L"Both"}),
        // The wrist HUD's size here too (user 18:24Z: the size on this page should size what's on the wrist).
        flag(L"Menus pie", L"Pie drawn by the mod (off: cut from the game's UI)", "ui", "own_pie", true, kLive),  // pie.py
        flag(L"Menus pie", L"A Sim's question: answers on the palm pie (palm / dynamic)", "ui", "chat_pie", true,
             kLive),  // objmenu.py
        choice(L"Menus pie", L"Pie menu shows", "ui", "pie_mode", &Config::pieMode,
               {L"In front", L"On your other palm", L"Dynamic"}),
        number(L"Menus pie", L"Pie on the palm: size", "ui", "pie_palm_size", &Config::piePalmSize, 0.15f, 1, 0.05f, 100,
               L"%.0f%%"),
        number(L"Menus pie", L"Pie on the palm: height above it", "ui", "pie_palm_lift", &Config::piePalmLift, 0, 0.4f, 0.02f,
               100, L"%.0f cm"),
        toggle(L"Menus pie", L"Pie on a TV: hover in front of its screen", "ui", "pie_tv", &Config::pieTvFront),
        number(L"Menus pie", L"Pie on a TV: size against in front of you", "ui", "pie_tv_size", &Config::pieTvSize, 0.5f, 6, 0.25f,
               100, L"%.0f%%"),
        number(L"Menus pie", L"Pie on a TV: distance from the screen", "ui", "pie_tv_offset", &Config::pieTvOffset, 0, 2, 0.05f,
               100, L"%.0f cm"),
        toggle(L"Menus pie", L"Own pie menu: poke your stomach", "ui", "self_pie_poke", &Config::selfPiePoke),
        toggle(L"Menus pie", L"Own pie menu: laser on your palm", "ui", "self_pie_palm", &Config::selfPiePalm),
        number(L"Menus pie", L"Pie menu distance", "ui", "pie_distance", &Config::pieDistance, 0.25f, 1, 0.05f, 100,
               L"%.0f cm"),
        number(L"Menus pie", L"Pie menu size", "ui", "pie_width", &Config::pieWidth, 0.4f, 1.5f, 0.04f, 100, L"%.0f cm"),
        toggle(L"Menus pickers", L"Pickers on the object (fridge, TV)", "ui", "object_menus", &Config::objectMenus),
        number(L"Menus pickers", L"Picker size on fronts", "ui", "object_menu_width", &Config::objectMenuWidth, 0.4f, 1.5f, 0.05f,
               100, L"%.0f cm"),
        flag(L"Menus pickers", L"Open Claude on TVs too", "claude", "tvs", false, kLive),  // claude.py
        flag(L"Menus pickers", L"Open Discord on screens", "discord", "on", true, kLive),  // discord.py
        flag(L"Menus pickers", L"Discord screen: laser clicks", "discord", "interactive", true, kLive),
        flag(L"Menus pickers", L"Discord sound: mute off screen", "discord", "mute_off_screen", false, kLive),
        flag(L"Menus pickers", L"Discord sound: from the screen", "discord", "sound_3d", false, kLive),
        scriptNumber(L"Menus pickers", L"Discord screen: pictures a second", "discord", "fps", 30, 5, 60, 5, L"%.0f"),
        scriptNumber(L"Menus pickers", L"Discord screen: widest picture", "discord", "max_width", 1920, 640, 3840, 160,
                     L"%.0f px"),
        flag(L"Menus pickers", L"Open Linear board on screens", "linear", "on", true, kLive),  // linearboard.py
        scriptNumber(L"Menus pickers", L"Linear board: refresh every", "linear", "poll", 20, 10, 120, 5, L"%.0f s"),
        scriptNumber(L"Menus pickers", L"Linear board: Done shows last", "linear", "done_days", 14, 0, 60, 1, L"%.0f days"),
        flag(L"Menus pickers", L"Linear board: Canceled column", "linear", "canceled", false, kLive),
        flag(L"Menus pickers", L"Linear scrumboard on wall art", "linear", "cork", true, kLive),
        scriptNumber(L"Menus pickers", L"Linear board: picture width", "linear", "width", 1920, 1280, 3840, 320, L"%.0f px"),
        flag(L"Menus popups", L"Other Sims' decisions: picked at random", "ui", "others_decide", true, kLive),  // objmenu.py
        number(L"Menus popups", L"Popup distance", "ui", "popup_distance", &Config::popupDistance, 0.4f, 1.5f, 0.05f, 100,
               L"%.0f cm"),
        number(L"Menus popups", L"Popup size", "ui", "popup_size", &Config::popupSize, 0.3f, 1.2f, 0.05f, 100, L"%.0f cm a metre"),
        toggle(L"Menus popups", L"No blue dimming behind dialogs", "wrist", "no_dimming", &Config::noDimming),
        toggle(L"Menus popups", L"Keyboard for text boxes", "ui", "keyboard", &Config::keyboard),
        number(L"Menus popups", L"Keyboard size", "ui", "keyboard_size", &Config::keyboardSize, 0.3f, 1, 0.05f, 100, L"%.0f cm"),
        keyboardButton,
        number(L"Menus panels", L"Menu panel distance", "ui", "distance", &Config::uiDistance, 0.5f, 3, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Menu panel size", "ui", "width", &Config::uiWidth, 0.5f, 3, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Menu panel height", "ui", "height", &Config::uiHeight, -1, 1, 0.05f, 1, L"%+.2f m"),
        number(L"Menus panels", L"Flat screen distance", "quad", "distance", &Config::quadDistance, 1, 5, 0.1f, 1, L"%.1f m"),
        number(L"Menus panels", L"Flat screen size", "quad", "width", &Config::quadWidth, 1, 5, 0.1f, 1, L"%.1f m"),

        // Our own HUD (first person, hud.cpp: drawn with Dear ImGui).
        number(L"Menus panels", L"Game UI on the wrist (UI button): size", "ui", "wrist_width", &Config::wristWidth, 0.2f, 0.8f,
               0.02f, 100, L"%.0f cm"),
        // The notification card in front of you (user 2026-10-23): where it sits and its size; also set by dragging it.
        component(L"HUD note", L"Notification card: right", "phone", "note_place", &Config::notePlace, 0, -1, 1, 0.02f, 100,
                  L"%+.0f cm"),
        component(L"HUD note", L"Notification card: up", "phone", "note_place", &Config::notePlace, 1, -1, 1, 0.02f, 100,
                  L"%+.0f cm"),
        component(L"HUD note", L"Notification card: ahead", "phone", "note_place", &Config::notePlace, 2, 0.25f, 2, 0.05f,
                  100, L"%.0f cm"),
        number(L"HUD note", L"Notification card: size", "phone", "note_size", &Config::noteSize, 0.2f, 1, 0.02f, 100,
               L"%.0f cm wide"),
        toggle(L"HUD watch", L"Watch on your left wrist", "wrist", "watch", &Config::wristTime),
        toggle(L"HUD pause", L"Pause symbol while paused", "wrist", "pause", &Config::wristPause),
        toggle(L"HUD sleep", L"Clock in view while asleep", "wrist", "sleep_clock", &Config::wristSleepClock),
        toggle(L"HUD lot intro", L"Cut it out where the game puts the banner (screen centre, UI pixels)", "wrist",
               "lot_intro_crop_auto", &Config::wristLotCropAuto),
        number(L"HUD lot intro", L"Banner width (game UI pixels, automatic cut-out)", "wrist", "lot_intro_banner_w",
               &Config::wristLotBannerW, 200, 2000, 20, 1, L"%.0f px"),
        number(L"HUD lot intro", L"Banner shift down (game UI pixels, automatic cut-out)", "wrist", "lot_intro_banner_y",
               &Config::wristLotBannerY, -400, 400, 10, 1, L"%+.0f px"),
        number(L"HUD lot intro", L"Cut-out width (share of the UI)", "wrist", "lot_intro_crop_w", &Config::wristLotCropW, 0.1f, 1, 0.02f, 100,
               L"%.0f %%"),
        number(L"HUD lot intro", L"Cut-out height (share of the UI)", "wrist", "lot_intro_crop_h", &Config::wristLotCropH, 0.05f, 1, 0.01f, 100,
               L"%.0f %%"),
        number(L"HUD lot intro", L"Cut-out shift down (share of the UI)", "wrist", "lot_intro_crop_y", &Config::wristLotCropY, -0.5f, 0.5f, 0.01f,
               100, L"%+.0f %%"),
        toggle(L"HUD lot intro", L"Show the lot name and traits while your eyes are shut for a load", "wrist", "lot_intro",
               &Config::wristLotIntro),  // lotintro.py
        toggle(L"HUD work card", L"Close your eyes and show the work card when your Sim goes to work", "wrist", "work_card",
               &Config::wristWork),  // work.py
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
        number(L"Bubbles others", L"Other Sims' bubble size", "bubbles", "npc_size", &Config::bubbleNpcSize, 0.2f, 1.5f, 0.05f,
               100, L"%.0f cm wide"),
        toggle(L"Bubbles", L"Hide your Sim's bubbles", "bubbles", "hide_own", &Config::bubbleHideOwn),
        toggle(L"Bubbles others", L"Hide other Sims' bubbles", "bubbles", "hide_npc", &Config::bubbleHideNpc),
        toggle(L"Bubbles others", L"Skill bars, flyaways, nameplates (old cut, stray boxes)", "bubbles", "cut_others", &Config::bubbleCutOthers),
        toggle(L"Bubbles", L"Balloons drawn by the mod (off: cut from the game's UI)", "bubbles", "own_balloons", &Config::bubbleOwnDraw),
        link(L"Bubbles", L"Your bubble", L"Bubbles yours"),
        link(L"Bubbles", L"Your skill bar", L"Bubbles skill"),
        link(L"Bubbles", L"Other Sims' bubbles", L"Bubbles others"),
        link(L"Bubbles", L"Your scandal mark (Royalty & Legacy)", L"Bubbles scandal"),
        // Your Sim's stat / relationship pops (user 2026-10-23): their own place and size.
        link(L"Bubbles", L"Your stat and relationship pops", L"Bubbles pops"),
        toggle(L"Bubbles scandal", L"Hide your scandal mark", "bubbles", "scandal_hide", &Config::scandalHide),  // scandal.py
        component(L"Bubbles scandal", L"Your scandal mark: right", "bubbles", "scandal_position", &Config::scandalPos, 0, -1, 1,
                  0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles scandal", L"Your scandal mark: up", "bubbles", "scandal_position", &Config::scandalPos, 1, -1, 1,
                  0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles scandal", L"Your scandal mark: ahead", "bubbles", "scandal_position", &Config::scandalPos, 2, -1, 1,
                  0.05f, 100, L"%+.0f cm"),
        toggle(L"Bubbles pops", L"Hide your pops", "bubbles", "hide_pops", &Config::popHideOwn),
        component(L"Bubbles pops", L"Your pops: right", "bubbles", "pop_position", &Config::popOwnPos, 0, -1, 1, 0.05f, 100,
                  L"%+.0f cm"),
        component(L"Bubbles pops", L"Your pops: up", "bubbles", "pop_position", &Config::popOwnPos, 1, -1, 1, 0.05f, 100,
                  L"%+.0f cm"),
        component(L"Bubbles pops", L"Your pops: ahead", "bubbles", "pop_position", &Config::popOwnPos, 2, 0.1f, 4, 0.05f, 100,
                  L"%.0f cm"),
        number(L"Bubbles pops", L"Your pops: size", "bubbles", "pop_size", &Config::popOwnSize, 0.05f, 0.6f, 0.01f, 100,
               L"%.0f cm wide"),
        component(L"Bubbles yours", L"Your bubble: right", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 0, -1,
                  1, 0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your bubble: up", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 1, -1, 1,
                  0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles yours", L"Your bubble: ahead", "bubbles", "own_bubble_position", &Config::bubbleOwnPos, 2, 0.1f,
                  4, 0.05f, 100, L"%.0f cm"),
        component(L"Bubbles skill", L"Your skill bar: horizontal", "bubbles", "skill_position", &Config::skillOwnPos, 0, -0.5f,
                  0.5f, 0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles skill", L"Your skill bar: up", "bubbles", "skill_position", &Config::skillOwnPos, 1, -0.5f,
                  1.0f, 0.05f, 100, L"%+.0f cm"),
        component(L"Bubbles skill", L"Your skill bar: ahead", "bubbles", "skill_position", &Config::skillOwnPos, 2, 0.2f,
                  2, 0.05f, 100, L"%.0f cm"),
        number(L"Bubbles skill", L"Your skill bar: size", "bubbles", "skill_size", &Config::skillOwnSize, 0.05f, 0.6f, 0.01f,
               100, L"%.0f cm wide"),

        choice(phone, L"Phone: where it comes out", "slots", "phone", &Config::phoneSlot,
               {L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        choice(phone, L"Phone: this Sim only", "slots", "this_phone", &Config::slotThisPhone,
               {L"Same as everyone", L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        number(phone, L"Slots: pocket, shoulder and bra reach", "slots", "radius", &Config::slotRadius, 0.05f, 0.4f, 0.01f, 100, L"%.0f cm"),
        number(phone, L"Hip: hand below your eyes", "phone", "hip_drop", &Config::phoneHipDrop, 0.1f, 0.9f, 0.02f, 100,
               L"%.0f cm"),
        toggle(phone, L"Pass the phone to the other hand", "phone", "pass", &Config::phonePass),
        number(phone, L"Pass: hands closer than", "phone", "pass_reach", &Config::phonePassReach, 0.08f, 0.5f, 0.02f, 100,
               L"%.0f cm"),
        choice(phone, L"Text size on the phone", "phone", "text_size", &Config::phoneTextSize, {L"Small", L"Medium", L"Large"}),
        number(phone, L"Right stick scroll speed (0 off)", "phone", "stick_scroll", &Config::phoneStickScroll, 0, 1500, 20, 1,
               L"%.0f px/s"),
        number(phone, L"Right stick dead zone", "phone", "stick_deadzone", &Config::phoneStickDeadzone, 0.05f, 0.9f, 0.05f, 100,
               L"%.0f %%"),
        number(phone, L"Swipe starts after", "phone", "swipe_start", &Config::phoneSwipeStart, 1, 20, 1, 1, L"%.0f px"),
        number(phone, L"Swipe scroll speed", "phone", "swipe_speed", &Config::phoneSwipeSpeed, 0.3f, 3, 0.1f, 100, L"%.0f %%"),
        link(phone, L"Position", phonePosition),
        link(phone, L"Hand", phoneHand),
        link(phone, L"Size and screen", phoneSize),
        link(phone, L"Game's phone model", phoneModel),
        link(phone, L"Sounds", phoneSounds),
        link(phone, L"Notifications", phoneNotes),
        link(phone, L"Flashlight", phoneFlash),
        link(phone, L"Sims Forever (phone in front of you)", L"Phone sf"),
        link(L"Phone sf", L"Left hand", L"Phone sf left"),
        link(L"Phone sf", L"Right hand", L"Phone sf right"),
        number(L"Phone sf left", L"Finger curl", "simsforever", "phone_finger_curl_left", &Config::sfCurlL, 0, 1, 0.05f, 1, L"%.2f"),
        number(L"Phone sf left", L"Thumb bend", "simsforever", "phone_thumb_bend_left", &Config::sfThumbBendL, 0, 1, 0.05f, 1, L"%.2f"),
        number(L"Phone sf left", L"Thumb spread", "simsforever", "phone_thumb_spread_left", &Config::sfThumbSpreadL, -60, 60, 5, 1,
               L"%+.0f\u00b0"),
        component(L"Phone sf left", L"Hand to your right", "simsforever", "phone_hand_offset_left", &Config::sfHandOffsetL, 0, -0.3f,
                  0.3f, 0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf left", L"Hand up", "simsforever", "phone_hand_offset_left", &Config::sfHandOffsetL, 1, -0.2f, 0.2f,
                  0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf left", L"Hand toward you", "simsforever", "phone_hand_offset_left", &Config::sfHandOffsetL, 2, -0.2f,
                  0.2f, 0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf left", L"Hand turn", "simsforever", "phone_hand_turn_left", &Config::sfHandTurnL, 0, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        component(L"Phone sf left", L"Hand tip", "simsforever", "phone_hand_turn_left", &Config::sfHandTurnL, 1, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        component(L"Phone sf left", L"Hand roll", "simsforever", "phone_hand_turn_left", &Config::sfHandTurnL, 2, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        number(L"Phone sf right", L"Finger curl", "simsforever", "phone_finger_curl_right", &Config::sfCurlR, 0, 1, 0.05f, 1, L"%.2f"),
        number(L"Phone sf right", L"Thumb bend", "simsforever", "phone_thumb_bend_right", &Config::sfThumbBendR, 0, 1, 0.05f, 1, L"%.2f"),
        number(L"Phone sf right", L"Thumb spread", "simsforever", "phone_thumb_spread_right", &Config::sfThumbSpreadR, -60, 60, 5, 1,
               L"%+.0f\u00b0"),
        component(L"Phone sf right", L"Hand to your right", "simsforever", "phone_hand_offset_right", &Config::sfHandOffsetR, 0, -0.3f,
                  0.3f, 0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf right", L"Hand up", "simsforever", "phone_hand_offset_right", &Config::sfHandOffsetR, 1, -0.2f, 0.2f,
                  0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf right", L"Hand toward you", "simsforever", "phone_hand_offset_right", &Config::sfHandOffsetR, 2, -0.2f,
                  0.2f, 0.005f, 100, L"%+.1f cm"),
        component(L"Phone sf right", L"Hand turn", "simsforever", "phone_hand_turn_right", &Config::sfHandTurnR, 0, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        component(L"Phone sf right", L"Hand tip", "simsforever", "phone_hand_turn_right", &Config::sfHandTurnR, 1, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        component(L"Phone sf right", L"Hand roll", "simsforever", "phone_hand_turn_right", &Config::sfHandTurnR, 2, -180, 180, 5, 1,
                  L"%+.0f\u00b0"),
        number(phoneFlash, L"Brightness", "phone", "flash_brightness", &Config::flashBrightness, 0, 100, 1, 1, L"x%.0f"),
        number(phoneFlash, L"Fill light (black surfaces)", "phone", "flash_fill", &Config::flashFill, 0, 0.3f, 0.01f, 100,
               L"%.0f %%"),
        number(phoneFlash, L"Range", "phone", "flash_range", &Config::flashRange, 1, 40, 1, 1, L"%.0f m"),
        number(phoneFlash, L"Cone width", "phone", "flash_cone", &Config::flashCone, 10, 120, 5, 1, L"%.0f\u00b0"),
        // The phone's Sounds screen has these too (phonesound.py: the names match its tone lists).
        choice(phoneSounds, L"Phone", "phone", "sound_mode", &Config::phoneSoundMode, {L"Sound", L"Vibrate", L"Mute"}),
        choice(phoneSounds, L"Ringtone", "phone", "ringtone_file", &Config::phoneRingtone,
               {L"None", L"Bustin\'", L"Old Phone 1", L"Old Phone 2", L"Outin\'"}),
        choice(phoneSounds, L"Text and notification tone", "phone", "text_tone_file", &Config::phoneTextTone,
               {L"None", L"Click", L"Promo", L"Slap", L"Success", L"Upgrade"}),
        scriptNumber(phoneSounds, L"Phone speaker: high-pass (0 = off)", "foley", "phone_highpass", 600, 0, 2000, 50, L"%.0f Hz"),
        toggle(phoneSounds, L"Buzz when the game shows a popup", "phone", "popup_buzz", &Config::phonePopupBuzz),
        // Calls (phonecall.py): the phone's own incoming call and in-call screens; the game's ring is left out.
        flag(phoneSounds, L"Calls on the phone (own screens, no game ring)", "phone", "call_screen", true, kLive),
        flag(phoneSounds, L"Animated text message screen", "phone", "text_screen", true, kLive),
        // Notifications (notify.py, phonesound.py): which show live in front of you, and their sound on the phone.
        flag(phoneNotes, L"Show notifications live", "phone", "notes_live", true, kLive),
        flag(phoneNotes, L"Live: urgent", "phone", "notes_live_urgent", true, kLive),
        flag(phoneNotes, L"Live: special moments", "phone", "notes_live_moment", true, kLive),
        flag(phoneNotes, L"Live: household and game", "phone", "notes_live_player", true, kLive),
        flag(phoneNotes, L"Live: about a Sim", "phone", "notes_live_sim", true, kLive),
        flag(phoneNotes, L"Live: other mods", "phone", "notes_live_mod", true, kLive),
        flag(phoneNotes, L"Live: Claude mods", "phone", "notes_live_claude", true, kLive),
        flag(phoneNotes, L"Hide mods' notices while a lot loads", "phone", "notes_hide_startup", true, kLive),
        scriptNumber(phoneNotes, L"Loading window", "phone", "notes_startup_seconds", 120, 0, 600, 15, L"%.0f s"),
        flag(phoneNotes, L"Phone-only notices play a sound from the phone", "phone", "notes_sound_phone", true, kLive),
        flag(phoneNotes, L"The game's own notification sound", "phone", "notes_sound_vanilla", false, kLive),
        toggle(tablet, L"Tablet in first-person build/buy", "tablet", "tablet", &Config::tablet),
        number(tablet, L"Width", "tablet", "width", &Config::tabletWidth, 0.15f, 0.5f, 0.01f, 100, L"%.0f cm"),
        number(tablet, L"Height (0 = fits the layout)", "tablet", "height", &Config::tabletHeight, 0, 0.5f, 0.01f, 100,
               L"%.0f cm"),
        choice(tablet, L"Tablet: where it comes out", "slots", "tablet", &Config::tabletSlot,
               {L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        choice(tablet, L"Tablet: this Sim only", "slots", "this_tablet", &Config::slotThisTablet,
               {L"Same as everyone", L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        number(tablet, L"Thickness", "tablet", "depth", &Config::tabletDepth, 0.003f, 0.03f, 0.001f, 100, L"%.1f cm"),
        number(tablet, L"Screen border", "tablet", "bezel", &Config::tabletBezel, 0, 0.03f, 0.001f, 100, L"%.1f cm"),
        number(tablet, L"Weight (steadies the tablet in your hand)", "tablet", "steady", &Config::tabletSteady, 0, 1, 0.05f, 100,
               L"%.0f%%"),
        number(tablet, L"Steady laser point (hand shake)", "tablet", "pointer_steady", &Config::tabletPointer, 0, 1, 0.05f, 100,
               L"%.0f%%"),
        number(tablet, L"Build click: wait before pressing", "tablet", "click_hover", &Config::tabletClickHover, 0, 600, 25, 1,
               L"%.0f ms"),
        number(tablet, L"Build click: press length (0 = held)", "tablet", "click_press", &Config::tabletClickPress, 0, 1000, 25, 1,
               L"%.0f ms"),
        number(tablet, L"Hand in from the edge", "tablet", "hold", &Config::tabletHold, 0, 0.12f, 0.005f, 100,
               L"%.1f cm"),
        number(tablet, L"Build menu size", "tablet", "menu_size", &Config::tabletUiScale, 0.3f, 4.0f, 0.05f, 100,
               L"%.0f%%"),
        number(tablet, L"Tool tray size", "tablet", "tray_scale", &Config::tabletTrayScale, 0.3f, 1.45f, 0.05f, 100,
               L"%.0f%%"),
        toggle(tablet, L"Eyes close when entering/leaving build mode", "buildbuy", "eyes", &Config::buildEyes),
        number(tablet, L"Eyes: steady frames before opening", "buildbuy", "eyes_frames", &Config::buildEyesFrames, 1, 300, 5, 1,
               L"%.0f"),
        number(tablet, L"Eyes: open anyway after", "buildbuy", "eyes_max_wait", &Config::buildEyesMaxWait, 0.5f, 30, 0.5f, 1,
               L"%.1f s"),
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
        toggle(notebook, L"Notebook at your left hip", "book", "book", &Config::book),
        toggle(notebook, L"Poke and laser on the pages (hold the grip)", "book", "poke", &Config::bookPoke),
        toggle(notebook, L"Page-flip sound", "book", "sound", &Config::bookSound),
        choice(notebook, L"Notebook: where it comes out", "slots", "book", &Config::bookSlot,
               {L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        choice(notebook, L"Notebook: this Sim only", "slots", "this_book", &Config::slotThisBook,
               {L"Same as everyone", L"Right hip", L"Left hip", L"Right front pocket", L"Left front pocket", L"Right back pocket", L"Left back pocket", L"Right shoulder", L"Left shoulder", L"Right bra", L"Left bra", L"Center bra"}),
        number(notebook, L"Hip: hand below your eyes", "book", "hip_drop", &Config::bookHipDrop, 0.1f, 0.9f, 0.02f, 100,
               L"%.0f cm"),
        link(notebook, L"Position", notebookPosition),
        link(notebook, L"Hand", notebookHand),
        link(notebook, L"Size and turning pages", notebookSize),
        angle(notebookPosition, L"Tilt", "book", "tilt", &Config::bookTilt),
        angle(notebookPosition, L"Turn", "book", "yaw", &Config::bookYaw),
        angle(notebookPosition, L"Tip back / forward", "book", "pitch", &Config::bookPitch),
        component(notebookPosition, L"Toward the back of the hand", "book", "book_offset", &Config::bookOffset, 0, -0.15f,
                  0.15f, 0.002f, 100, L"%+.1f cm"),
        component(notebookPosition, L"Toward the thumb", "book", "book_offset", &Config::bookOffset, 1, -0.15f, 0.15f,
                  0.002f, 100, L"%+.1f cm"),
        component(notebookPosition, L"Toward the wrist", "book", "book_offset", &Config::bookOffset, 2, -0.15f, 0.15f,
                  0.002f, 100, L"%+.1f cm"),
        number(notebookHand, L"Finger curl", "book", "finger_curl", &Config::bookCurl, 0, 1, 0.05f, 1, L"%.2f"),
        number(notebookHand, L"Thumb bend", "book", "thumb_bend", &Config::bookThumbBend, 0, 1, 0.05f, 1, L"%.2f"),
        number(notebookHand, L"Thumb spread", "book", "thumb_spread", &Config::bookThumbSpread, -60, 60, 5, 1,
               L"%+.0f\u00b0"),
        component(notebookHand, L"Hand out from the book", "book", "hand_offset", &Config::bookHandOffset, 0, -0.1f, 0.1f,
                  0.002f, 100, L"%+.1f cm"),
        component(notebookHand, L"Hand up the book", "book", "hand_offset", &Config::bookHandOffset, 1, -0.1f, 0.1f,
                  0.002f, 100, L"%+.1f cm"),
        component(notebookHand, L"Hand toward you", "book", "hand_offset", &Config::bookHandOffset, 2, -0.1f, 0.1f,
                  0.002f, 100, L"%+.1f cm"),
        component(notebookHand, L"Hand turn", "book", "hand_turn", &Config::bookHandTurn, 0, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        component(notebookHand, L"Hand tip", "book", "hand_turn", &Config::bookHandTurn, 1, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        component(notebookHand, L"Hand roll", "book", "hand_turn", &Config::bookHandTurn, 2, -90, 90, 5, 1,
                  L"%+.0f\u00b0"),
        number(notebookSize, L"Width (both pages)", "book", "book_width", &Config::bookWidth, 0.1f, 0.6f, 0.01f, 100,
               L"%.0f cm"),
        number(notebookSize, L"Page art crop: outer edge", "book", "crop_outer", &Config::bookCropOuter, 0, 0.3f, 0.005f, 100,
               L"%.1f%%"),
        number(notebookSize, L"Page art crop: spine edge", "book", "crop_spine", &Config::bookCropSpine, 0, 0.3f, 0.005f, 100,
               L"%.1f%%"),
        number(notebookSize, L"Page art crop: top", "book", "crop_top", &Config::bookCropTop, 0, 0.3f, 0.005f, 100,
               L"%.1f%%"),
        number(notebookSize, L"Page art crop: bottom", "book", "crop_bottom", &Config::bookCropBottom, 0, 0.3f, 0.005f, 100,
               L"%.1f%%"),
        number(notebookSize, L"Cover opening time", "book", "open_seconds", &Config::bookOpenSeconds, 0.1f, 3, 0.1f, 1,
               L"%.1f s"),
        number(notebookSize, L"Page turn: swipe length", "book", "swipe", &Config::bookSwipe, 0.03f, 0.3f, 0.01f, 100,
               L"%.0f cm"),
        number(phoneSize, L"Width", "phone", "phone_width", &Config::phoneWidth, 0.04f, 0.2f, 0.002f, 100, L"%.1f cm"),
        number(phoneSize, L"Height", "phone", "phone_height", &Config::phoneHeight, 0.08f, 0.3f, 0.002f, 100, L"%.1f cm"),
        number(phoneSize, L"Thickness", "phone", "phone_depth", &Config::phoneDepth, 0.003f, 0.03f, 0.001f, 100,
               L"%.1f cm"),
        number(phoneSize, L"Screen border", "phone", "bezel", &Config::phoneBezel, 0, 0.015f, 0.0005f, 100, L"%.2f cm"),
        number(phoneSize, L"Menu crop width", "phone", "crop_width", &Config::phoneCropWidth, 0.1f, 2, 0.05f, 1, L"%.2f"),
        number(phoneSize, L"Menu crop height", "phone", "crop_height", &Config::phoneCropHeight, 0.1f, 1, 0.05f, 1,
               L"%.2f"),
        flag(phoneModel, L"Game's phone model (seen in mirrors)", "phone", "game_phone", true, kNextPhone),
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
        choice(sound, L"Flat sounds made 3D (phone jingles)", "audio", "flat_to_3d", &Config::audioFlatTo3D,
               {L"Off", L"Phone jingles", L"All flat UI sounds (test)"}),
        number(sound, L"Flat voices: full volume up to", "audio", "voice_near", &Config::audioVoiceNear, 0.5f, 10, 0.5f, 1,
               L"%.1f m", kAudio),
        number(sound, L"Flat voices: silent beyond (0 = their own)", "audio", "voice_far", &Config::audioVoiceFar, 0, 60, 1,
               1, L"%.0f m", kAudio),
        flag(sound, L"Outdoor sound comes in through doorways", "audio", "doors_matter", true, kLive),  // outdoors.py
        flag(sound, L"Porches with railings count as outside", "audio", "railings_outside", true, kLive),  // outdoors.py
        number(sound, L"Outdoor sound heard inside, at least", "audio", "outdoor_floor", &Config::outdoorFloor, 0, 1, 0.05f, 2,
               L"%.2f", kAudio),
        number(sound, L"In a basement the outdoors is (1 = same)", "audio", "outdoor_basement", &Config::outdoorBasement, 0, 1,
               0.05f, 2, L"%.2f", kAudio),
        scriptNumber(sound, L"A closed door lets through", "audio", "closed_door", 0.35f, 0, 1, 0.05f, L"%.2f"),
        scriptNumber(sound, L"Walls let through", "audio", "wall_leak", 0.25f, 0, 1, 0.05f, L"%.2f"),
        flag(sound, L"Muffle sounds through walls and floors", "audio", "muffle_feed", true, kLive),  // collide.py -> Extended Sound Control
        scriptNumber(sound, L"Muffle: a wall blocks", "audio", "muffle_wall", 0.5f, 0, 0.95f, 0.05f, L"%.2f"),
        scriptNumber(sound, L"Muffle: a closed door blocks", "audio", "muffle_door", 0.35f, 0, 0.95f, 0.05f, L"%.2f"),
        scriptNumber(sound, L"Muffle: a half wall blocks", "audio", "muffle_half_wall", 0.25f, 0, 0.95f, 0.05f, L"%.2f"),
        scriptNumber(sound, L"Muffle: a floor or ceiling blocks", "audio", "muffle_floor", 0.6f, 0, 0.95f, 0.05f, L"%.2f"),
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
        {L"HUD lot intro", L"Lot name and traits on arrival", "lot_intro", &Config::wristLotPos, &Config::wristLotTurn,
         nullptr, &Config::wristLotSize, 0.3f, 3.0f},
        {L"HUD work card", L"Work card (eyes closed at work)", "work_card", &Config::wristWorkPos, &Config::wristWorkTurn,
         nullptr, &Config::wristWorkSize, 0.2f, 1.5f},
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
    // The Body Physics phone app's rows join before pages are assigned (they were added after, so every one sat on the front page).
    rows.insert(rows.end(), physicsRows.begin(), physicsRows.end());
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
    applyDev(rows);
    applyTips(rows);
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
size_t g_devRow = SIZE_MAX;   // the Developer options switch's row
bool devOn() { return g_devRow >= g_flags.size() || g_flags[g_devRow] != 0; }
bool rowGone(const Row& r, int depth);
// A page whose rows are all hidden (it has rows): its link goes too.
bool pageAllGone(int page, int depth) {
    bool any = false;
    for (const Row& r : g_rows) {
        if (r.page != page || r.kind == Kind::Back) continue;
        any = true;
        if (!rowGone(r, depth + 1)) return false;
    }
    return any;
}
// Developer-only rows (and links to pages with nothing else) hide while Developer options is off.
bool rowGone(const Row& r, int depth) {
    if (devOn()) return false;
    if (r.dev) return true;
    return r.kind == Kind::Link && r.linkPage > 0 && depth < 8 && pageAllGone(r.linkPage, depth);
}
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
constexpr ULONGLONG kTipDelay = 1000;
const wchar_t* const kCloseTip = L"Closes the menu and saves your changes. B closes it too.";
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
    if (g_page > 0 && pageAllGone(g_page, 0)) g_page = 0;  // developer-only page, Developer options off
    g_lines.clear();
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].kind == Kind::Back && g_rows[i].page == g_page) g_lines.push_back({static_cast<int>(i), nullptr});
    const wchar_t* group = nullptr;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.page != g_page || r.kind == Kind::Back) continue;
        if (r.kind == Kind::Link && (r.linkPage <= 0 || !pageShown(r.linkPage))) continue;
        if (rowGone(r, 0)) continue;
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
        if (rowGone(r, 0)) continue;
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
    if (i == g_devRow) buildLines();  // rows appear or go
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
            camera::setHeadOffset(c.bodyHeadX, c.bodyHeadY, c.bodyHeadZ);
            camera::setSeatFollow(c.bodySeatFollow);
            camera::setCas(c.casStereo, c.casDistance, c.casEyeSeparation, c.casEyeHeight);
            camera::setWorldScale(c.worldScale);
            armik::setSeatTorso(c.bodySeatTorso);
            armik::setSteadyShoulders(c.bodySteadyShoulders);
            facehide::setEnabled(c.bodyHideFace);
            facehide::setFarSimCull(c.farSimCull);
            facehide::setEyeSimCull(c.eyeSimCull);
            gputimeline::setEnabled(c.gpuTimeline);
            godhands::setEnabled(c.bodyGodHands);
            casbody::setEnabled(c.casBody);
            casbody::setEyesAtBody(c.casBodyEyes);
            casbody::setStill(c.casBodyStill);
            break;
        case kWalk:
            player::configure(c.walkSpeed, c.runSpeed, c.bodyForward, c.bodyHeight, c.touchBodies && c.armIk);
            player::setKeepHeight(c.bodyKeepHeight);
            player::setLazyTurn(c.bodyLazyTurn);
            player::setStandRate(c.bodyStandRate);
            player::setSmoothSlide(c.bodySmoothSlide);
            collide::configure(c.bodyCollision, c.bodyRadius);
            collide::setFurnitureRadius(c.furnitureRadius);
            collide::configureHands(c.handCollision);
            collide::setHandGaps(c.handWallGap, c.handObjectGap);
            collide::configurePlants(c.softPlants, c.plantSlowdown);
            collide::configureStep(c.stepOver, c.stepHeight);
            player::setStopTime(c.bodyStopTime);
            player::configureCrouch(c.crouch, c.crouchDepth, c.crouchSpeed, c.crouchSeconds);
            player::setStopGlide(c.bodyStopGlide);
            walkstop::setClips(c.bodyWalkClips);
            break;
        case kHands: armik::configureHands(c.handPitch, c.headTracking); break;
        case kTouch:
            armik::configureTouch(c.touchBodies, c.holdHands, c.lookAtYou, c.grabBodies);
            armik::configureGrab(c.grabCurl, c.grabDrag, c.breastCurl, c.buttCurl);
            armik::setLookAtHand(c.lookAtHand, c.lookSpeed, c.lookEyesWithin, c.lookHeadFrom);
            gesture::setHighFiveHold(c.highFiveHold);
            gesture::setShoo(c.shooAngle, static_cast<int>(c.shooFlicks + 0.5f));
            player::setHeadRepel(c.headRepel, c.headRadius);
            player::configure(c.walkSpeed, c.runSpeed, c.bodyForward, c.bodyHeight, c.touchBodies && c.armIk);
            break;
        case kRenderScale: g_renderScaleChanged = true; break;
        case kShadows: shadows::configure(c.steadyShadows, c.shadowStep, c.shadowSharp); break;
        case kAudio:
            ambience::setFloor(c.outdoorFloor);
            ambience::setBasement(c.outdoorBasement);
            camera::setPanAnyDistance(c.audioPanAnyDistance);
            camera::setVoiceRadii(c.audioVoiceNear, c.audioVoiceFar);
            camera::setFlatTo3D(c.audioFlatTo3D, c.audioFlatIds, c.audioFlatGraph);
            break;
        default: break;
    }
}

using JiggleGetFn = othermods::GetFn;
using JiggleSetFn = othermods::SetFn;

// A separate mod's settings calls (Integrated Body Physics unless the row names another), while its DLL is loaded.
bool jiggleApi(JiggleGetFn& get, JiggleSetFn& set, othermods::Mod mod = othermods::kBodyPhysics) {
    return othermods::api(mod, get, set);
}

const char* modName(othermods::Mod mod) {
    switch (mod) {
        case othermods::kSoundControl: return "Extended Sound Control";
        case othermods::kBustinOut: return "Claude Bustin Out";
        case othermods::kRealisticLighting: return "Claude Realistic Lighting";
        default: return "Integrated Body Physics";
    }
}

// A link of another mod's menu with when=<key> opens only while the mod's
// value for that key is not 0 (Integrated Body Physics "Configure current
// Sim" while a Sim is frozen); till then it is greyed out with its off tip.
bool linkOpen(const Row& r) {
    if (!r.when) return true;
    JiggleGetFn get;
    JiggleSetFn set;
    float v = 0;
    return jiggleApi(get, set, r.mod) && get(r.when, &v) && v != 0;
}

// Its rows show what the mod has now.
void readJiggle() {
    JiggleGetFn get[othermods::kModCount] = {};
    JiggleSetFn set[othermods::kModCount] = {};
    bool have[othermods::kModCount] = {};
    for (int m = 0; m < othermods::kModCount; ++m) have[m] = jiggleApi(get[m], set[m], static_cast<othermods::Mod>(m));
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        float v = 0;
        if (!r.jiggle || !have[r.mod] || !get[r.mod](r.jiggle, &v)) continue;
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
            r.mod = mod;
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
        rows.back().mod = mod;
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
    if (!merged[othermods::kBustinOut] && mergeMenu(othermods::kBustinOut, L"cbo:"))
        any = merged[othermods::kBustinOut] = true;
    if (!merged[othermods::kRealisticLighting] && mergeMenu(othermods::kRealisticLighting, L"crl:"))
        any = merged[othermods::kRealisticLighting] = true;
    return any;
}

void changed(size_t i) {
    if (const char* key = g_rows[i].jiggle) {
        JiggleGetFn get;
        JiggleSetFn set;
        float v = g_rows[i].kind == Kind::Toggle ? (g_flags[i] ? 1.0f : 0.0f) : g_values[i];
        if (jiggleApi(get, set, g_rows[i].mod))
            set(key, v);
        else
            LOG("Settings: %s %s = %g, but that mod isn't loaded", modName(g_rows[i].mod), key, v);
        g_redraw = true;
        return;
    }
    if (g_rows[i].section && !strcmp(g_rows[i].section, "logging") && g_rows[i].key)
        s4vr::log::setOption(g_rows[i].key, g_rows[i].kind == Kind::Toggle ? (g_flags[i] ? 1.0f : 0.0f) : g_values[i]);
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
            if (r.action && c > 0) grab::pushControl(r.action + c);  // a touch sound: heard as it is picked
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
                    if (g_confirmYes && jiggleApi(get, set, r.mod)) {
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
            if (r.action == 203) {  // native: the made eye's alignment back to as measured
                static const Config defaults;
                for (size_t k = 0; k < g_rows.size(); ++k) {
                    const Row& a = g_rows[k];
                    if (a.kind != Kind::Number || !a.number || !a.key || strncmp(a.key, "made_eye_", 9) != 0) continue;
                    {
                        std::lock_guard<std::mutex> lock(g_valueMutex);
                        numberRef(k) = defaults.*a.number;
                    }
                    changed(k);
                }
                g_redraw = true;
                g_doneAt = GetTickCount64();
                g_doneRow = i;
                LOG("Settings: %ls", r.label);
                return true;
            }
            if (r.action == 204) {  // native: the VR keyboard by hand
                hud::toggleKeyboard();
                g_redraw = true;
                g_doneAt = GetTickCount64();
                g_doneRow = i;
                LOG("Settings: %ls", r.label);
                return true;
            }
            if (r.action == 205) {  // native: the flight recorder's last seconds to a trace file
                const std::string file = s4vr::log::dumpTrace("menu", true);
                LOG("Settings: %ls -> %s", r.label, file.empty() ? "nothing to dump (recorder off?)" : file.c_str());
                g_redraw = true;
                g_doneAt = GetTickCount64();
                g_doneRow = i;
                return true;
            }
            if (r.action == 201 || r.action == 202) {  // native: the turn benchmark (benchmark.cpp), not a script control
                benchmark::start(r.action == 201);
                g_redraw = true;
                g_doneAt = GetTickCount64();
                g_doneRow = i;
                LOG("Settings: %ls", r.label);
                return true;
            }
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
    const bool onClose = g_selected == kCloseRow;
    if (!onClose && (g_selected < 0 || g_selected >= static_cast<int>(g_rows.size()))) return;
    const Row* picked = onClose ? nullptr : &g_rows[g_selected];
    const wchar_t* tip = onClose ? kCloseTip : picked->kind == Kind::Link && !linkOpen(*picked) ? picked->offTip : picked->tip;
    if (!tip || GetTickCount64() - g_selectedAt < kTipDelay) return;
    int line = onClose ? 0 : lineOf(g_selected);
    if (!onClose && (line < g_top || line >= g_top + kShown)) return;
    g_tipDrawn = true;
    constexpr int kLeft = 60, kRight = kWidth - 60, kPad = 18;
    SelectObject(g_canvas.dc, g_canvas.small);
    RECT size{kLeft + kPad, 0, kRight - kPad, 0};
    DrawTextW(g_canvas.dc, tip, -1, &size, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int height = static_cast<int>(size.bottom - size.top) + 2 * kPad;
    int rowTop = kListTop + (line - g_top) * kRowHeight, rowBottom = rowTop + kRowHeight;
    int y0 = rowBottom - 2;
    if (onClose) y0 = kCloseBottom + 8;  // under the Close button
    else if (y0 + height > kHeight - 12) y0 = rowTop + 2 - height;
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
    {   // A row whose group has no entry in kFixedPages lands on the main page: say so in the log (2026-11-04).
        static std::vector<std::wstring> reported;
        for (const Row& r : g_rows) {
            if (!r.group || pageOf(r.group) != 0 || !wcscmp(r.group, g_pages[0].group)) continue;
            if (std::find(reported.begin(), reported.end(), r.group) != reported.end()) continue;
            reported.push_back(r.group);
            LOG("Settings: rows in group '%ls' have no page in kFixedPages (they would show on the main page)", r.group);
        }
    }
    g_flags.assign(g_rows.size(), 0);
    g_values.assign(g_rows.size(), 0.0f);
    g_changed.assign(g_rows.size(), 0);
    g_removed.assign(g_rows.size(), 0);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.key && !strcmp(r.key, "dev_options")) g_devRow = i;
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
    if (g_live) {  // the menu's rows (HUD > Notification card), which dragging the card sets too
        std::copy(g_live->notePlace, g_live->notePlace + 3, place);
        return true;
    }
    if (!g_hasNotePlace) return false;
    std::copy(g_notePlace, g_notePlace + 3, place);
    return true;
}

void setNotePlacement(const float place[3]) {
    std::copy(place, place + 3, g_notePlace);
    g_hasNotePlace = true;
    if (g_live) std::copy(place, place + 3, g_live->notePlace);
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

int testingCheats() { return g_cheats; }

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

const wchar_t* openPage() { return g_open && g_page > 0 && g_page < pageCount() ? g_pages[g_page].group : nullptr; }

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

std::vector<PageRow> pageRows(const wchar_t* group, std::wstring* title, std::wstring* parent) {
    std::vector<PageRow> out;
    // Another mod's pages (ibp:/esc:/cbo:) on the phone without the menu ever opened: merge their export now and
    // keep the values current (the menu does both on open; the phone apps of those mods show them closed).
    if (!g_open && group && group[0] && group[3] == L':') {
        static ULONGLONG lastRead = 0;
        const ULONGLONG now = GetTickCount64();
        mergeMenus();
        if (now - lastRead > 400) readJiggle(), lastRead = now;
    }
    const int page = pageOf(group);
    if (page <= 0) return out;
    if (title) *title = g_pages[page].name;
    if (parent) *parent = g_pages[page].parent ? g_pages[page].parent : L"";
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        if (r.page != page || r.kind == Kind::Back || r.kind == Kind::Reset) continue;
        PageRow p;
        p.label = r.label;
        p.row = static_cast<int>(i);
        if (r.kind == Kind::Link) {
            if (!pageShown(r.linkPage) || !linkOpen(r)) continue;
            p.kind = 2;
            p.target = r.target ? r.target : L"";
        } else if (r.kind == Kind::Toggle) {
            p.kind = 1;
            p.value = toggleValue(i) ? L"On" : L"Off";
        } else if (r.kind == Kind::Action) {
            p.kind = 3;
        } else {
            wchar_t v[96];
            shownValue(i, v, 96);
            p.value = v;
        }
        out.push_back(std::move(p));
    }
    return out;
}

bool pageChange(int row, int dir) {
    if (row < 0 || row >= static_cast<int>(g_rows.size())) return false;
    const Kind k = g_rows[row].kind;
    if (k == Kind::Link || k == Kind::Back || k == Kind::Reset) return false;
    const bool done = change(static_cast<size_t>(row), dir);
    if (done && !g_open) flush();  // the menu isn't open to save it a second later
    return done;
}

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
        return selectClose(), click ? kCloseRequested : kNothing;  // hovering it shows its tip
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
    if (row != hovered || click || g_selected == kCloseRow) result = select(row) ? kChanged : kNothing;
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
    if (g_open && !g_tipDrawn && GetTickCount64() - g_selectedAt >= kTipDelay &&
        (g_selected == kCloseRow ||
         (g_selected >= 0 && g_selected < static_cast<int>(g_rows.size()) && g_rows[g_selected].tip)))
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

std::wstring userIni() { return g_ini.empty() ? std::wstring() : userIniPath(g_ini); }

}  // namespace s4vr::settings
