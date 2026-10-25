#include "hud.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hud_art.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "log.h"
#include "phone3d.h"
#include "settings.h"
#include "simsforever.h"
#include "strings.h"
#include "uikit.h"

// The game's own UI art (tools/pack_hud_art.py: colour re-cut of re/liveui/art),
// in the DLL: u32 width, height, then run-length coded straight-alpha BGRA.
__asm__(".section .rdata,\"dr\"\n"
        ".p2align 4\n"
        "s4vr_hud_art:\n"
        ".incbin \"" S4VR_HUD_ART "\"\n"
        "s4vr_hud_art_end:\n"
        ".text\n");
extern "C" const unsigned char s4vr_hud_art[], s4vr_hud_art_end[];

// imgui_impl_dx11.cpp calls this for D3DCompile (CMakeLists renames it):
// d3dcompiler_47.dll is loaded at runtime, as bodylayer.cpp does, not linked.
extern "C" HRESULT WINAPI s4vr_D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                               ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                               ID3DBlob** code, ID3DBlob** errors) {
    static pD3DCompile fn = [] {
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        return dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }();
    if (!fn) return E_FAIL;
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace s4vr::hud {
namespace {

std::atomic<int> g_hour{-1}, g_minute{0}, g_day{0}, g_speed{1};
ImGuiContext* g_imgui = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11Texture2D* g_texture = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
ID3D11ShaderResourceView* g_atlasView = nullptr;  // all its levels, for GenerateMips
constexpr UINT kAtlasMips = 4;
ImFont* g_big = nullptr;    // the time
ImFont* g_small = nullptr;  // the day, labels
ImFont* g_text = nullptr;   // popup text (regular)
ImFont* g_title = nullptr;  // popup titles
ID3D11ShaderResourceView* g_art = nullptr;  // the game's art
void bindKit();  // hands the UI kit (uikit.h) the art, fonts and icons, once they exist

// The popup on show (objmenu.py), its texts once the strings are loaded.
struct PopupContent {
    int layout = 0;                     // 1: two Sims talking
    uint64_t pictures[2] = {};          // the other Sim's, yours (icon or thumbnail ids)
    std::string titleJson, textJson;
    std::vector<std::string> buttonJson;
    std::vector<std::string> tipJson;   // each button's tooltip ("" none)
    std::vector<long long> choices;
    bool resolved = false, valid = false;
    std::string title, text;
    std::vector<std::string> buttons;
    // Layout 2: a picker of rows (the game's GUIPicker / SimPicker: objmenu.py _send_list_content).
    struct Row {
        std::string nameJson, descJson, name, desc;
        uint64_t icon = 0;
        bool enabled = true, selected = false;
    };
    std::vector<Row> rows;
    std::string subtitleJson, subtitle;
    bool multi = false, canClose = true;
    int minSel = 0, maxSel = 1;
};
int g_listPage = 0;                 // the list picker's page (present thread)
int g_textPage = 0;                 // a long popup text's page (present thread)
std::vector<char> g_listChosen;     // its rows ticked (several to pick)
std::vector<int> g_listPicked;      // what Done sent
std::mutex g_popupMutex;
PopupContent g_popupIn;      // set by Python (g_popupMutex)
bool g_popupNew = false;     // g_popupIn changed (g_popupMutex)
PopupContent g_popup;        // present thread
bool g_popupOn = false;      // present thread: there is one
std::atomic<float> g_popupHeight{0};
std::atomic<int> g_popupState{0};  // 0 none, 1 waiting for its texts, 2 drawn from them, 3 texts missing

// The picker on its object (objmenu.py; user 2026-10-16 09:57Z: the cut of
// the game's flat picker on the fridge looked badly cropped), its texts once
// the strings are loaded.
struct PickerRow {
    bool top = true;  // in the list (else only one of a top row's servings)
    std::string nameJson, servingJson, name, serving;
    uint64_t icon = 0;
    unsigned enabled = 0;     // 1 plain, 2 with ingredients, 4 prepped, 8 both
    unsigned price[4] = {};   // plain, with ingredients, prepped, both
    unsigned discounted = 0;  // 0: none
    int skill = 0;
    std::vector<int> servings;  // a top row's linked rows (may hold itself)
    std::vector<unsigned> tags;
};
struct PickerFilter {
    unsigned tag = 0;
    uint64_t icon = 0;
    std::string json, text;
};
struct PickerContent {
    long long dialog = 0;
    int flags = 0;     // hud.h setPickerContent
    std::string sort;  // the column it is sorted by ("" the message's order)
    std::string titleJson, title;
    std::vector<PickerFilter> filters;
    std::vector<PickerRow> rows;
    std::vector<int> order;  // the top rows, sorted
    size_t mostServings = 0;
    bool resolved = false, valid = false;
};
std::mutex g_pickerMutex;
PickerContent g_pickerIn;  // set by Python (g_pickerMutex)
bool g_pickerNew = false;  // g_pickerIn changed (g_pickerMutex)
PickerContent g_picker;    // present thread
bool g_pickerOn = false;   // present thread: there is one
std::atomic<float> g_pickerHeight{0};
std::atomic<int> g_pickerState{0};  // as g_popupState
// What you did on it (present thread): the page, the filter tab (-1 all), the
// top row whose servings are open (-1 none), the two ingredient boxes.
int g_pickerPage = 0, g_pickerTab = -1, g_pickerOpen = -1;
bool g_pickerFresh = false, g_pickerPrepped = false;
std::vector<uint64_t> g_pickerIcons;  // the icons it shows (g_itemMutex)
std::vector<uint64_t> g_pieIcons;     // the own-drawn pie's icons (g_itemMutex; hud_pie.inc)
std::vector<uint64_t> g_popupIcons;   // the popup's pictures (g_itemMutex)

// The game's phone menu (phoneapp.py; user 2026-10-18 20:35Z: the game's own
// phone on our phone), drawn as the game's PhonePanel draws it.
struct PhoneNode {
    bool category = false;
    int parent = -1;  // the node above (-1: the home screen)
    uint64_t icon = 0;
    std::string nameJson, disabledJson, name, disabled;
    bool notification = false;  // an item's own badge, or (a category) any of its items'
    int priority = 0;
    std::vector<int> children;  // sorted as the game sorts them, once the texts are found
};
struct PhoneContent {
    bool silenced = false;
    int color = -1;                // the wallpaper colour chosen (index into colors)
    std::vector<uint64_t> colors;  // the wallpaper colours' swatch icons (PhoneBackgroundColorSelector)
    std::string disabledJson, disabled;  // the phone can't be used now: why (the game's fly-away text)
    std::vector<PhoneNode> nodes;
    std::vector<int> home;  // the home screen's apps (categories under -1)
    bool resolved = false, valid = false;
};
std::mutex g_phoneMutex;
PhoneContent g_phoneIn;     // set by Python (g_phoneMutex)
bool g_phoneNew = false;    // g_phoneIn changed (g_phoneMutex)
PhoneContent g_phone;       // present thread
bool g_phoneOn = false;     // present thread: there is one
std::vector<uint64_t> g_phoneIcons;  // its apps' icons (g_itemMutex)
std::vector<uint64_t> g_nbIcons;     // the notebook's icons, read ahead (g_itemMutex)
// What you did on it (present thread): the screen (the home screen, or the
// list of the app `g_phoneApp`, opened from the home screen's app
// g_phoneRoot), the home screen's page, the list's first row shown.
std::atomic<bool> g_phoneHomeAsked{false};
bool g_phoneColorsOpen = false;  // the wallpaper colour selector is up (present thread)
// The game's notifications (notify.py): the phone's bell and list, a card over a Sim's head.
struct Note {
    uint64_t id = 0, picture = 0;
    bool read = false;
    int style = 0;  // notify.py: 0 standard, 1 player, 2 critical, 3 moment; +4 strip
    std::string titleJson, textJson, time;
    std::vector<std::string> buttons;  // its answers' texts (json) while the game waits for one
};
struct NotesContent {
    int unread = 0;
    std::vector<Note> notes;  // newest first
};
std::mutex g_notesMutex;
NotesContent g_notesIn;  // g_notesMutex
bool g_notesNew = false;  // g_notesMutex
NotesContent g_notes;     // present thread
struct Balloon {
    uint64_t sim, picture, overlay, category;
    int type, priority, slot;
    ULONGLONG start, until;
};
std::vector<Balloon> g_balloons;  // (g_itemMutex)
struct Bar {
    uint64_t sim, icon;
    double start, end, current, rate;
    int level, maxLevel, slot;
    ULONGLONG at, until;
};
std::vector<Bar> g_bars;  // (g_itemMutex)
std::vector<uint64_t> g_noteIcons;  // their pictures (g_itemMutex)
bool g_phoneNotesOpen = false;  // the phone shows the notification list (present thread)
int g_notesScroll = 0;
float g_noteHeight = 0;
// The game's calendar as a phone app (phonecal.py, the game's Calendar widget):
// the days of the year's weeks from this week's first, their entries.
struct CalDay {
    int season = -1, moon = -1, main = 0;  // main: the entry kind its big picture shows
    uint64_t weather = 0, mainPicture = 0;
    std::vector<int> small;  // the small activity icons (0: more)
    std::string forecastJson;
};
struct CalItem {
    int day = 0, kind = 0;
    uint64_t picture = 0;
    std::string nameJson, timeJson;
};
struct CalendarContent {
    bool valid = false;
    int today = 0, count = 0;
    std::string titleJson, emptyJson;
    std::vector<CalDay> days;
    std::vector<CalItem> items;
};
std::mutex g_calMutex;
CalendarContent g_calIn;  // g_calMutex
bool g_calNew = false;    // g_calMutex
CalendarContent g_cal;    // present thread
bool g_phoneCalOpen = false;  // the phone shows the calendar (present thread)
// The My Sim app (phonesim.py; user 2026-10-23 21:35Z: the HUD's lower right panels on the phone).
struct SimSkill {
    int category = 0, level = 0, maxLevel = 0;
    uint64_t icon = 0;
    float progress = 0;
    bool mastered = false, rising = false;
    std::string nameJson;
};
struct SimGoal {
    std::string textJson;
    bool done = false, showProgress = false;
    int value = 0, goal = 0;
};
struct SimTrack {
    int category = 0;
    uint64_t icon = 0;
    bool current = false;
    std::string nameJson;
};
struct SimAspiration {
    bool valid = false, done = false;
    uint64_t icon = 0;
    int current = 0, count = 0;
    float progress = 0;
    std::string nameJson, textJson;
    std::vector<std::pair<std::string, bool>> milestones;  // name json, done
    std::vector<SimGoal> goals;                             // the current milestone's
};
struct SimJob {
    uint64_t icon = 0;
    int level = 0, levels = 0;
    float performance = 0;
    std::string nameJson, titleJson, pay, companyJson, shift;
};
struct SimMember {  // the Household app: one of the household tray's Sims
    int index = 0, age = 0;
    bool here = false, active = false;
    uint64_t face = 0, bust = 0;
    std::string first, name;
};
struct SimContact {  // Contacts: one Sim your Sim has a relationship with (phonesim.py "ct")
    std::string name, friendBitJson, romanceBitJson, familyBitJson;  // family: Husband, Daughter... (other visible bits)
    uint64_t face = 0;
    float friendship = 0, romance = 0;
    bool hasRomance = false, here = false;
    std::string tags;  // the filters it is in (phonesim._contact_tagger): H F f R E W S L C P
};
struct ContactPage {  // the picked contact: what your Sim knows of them ("cd", "cdt", "cdk", "cds", "cdb")
    int index = -1, unknown = 0;
    uint64_t bust = 0;
    struct Trait { std::string group, nameJson; uint64_t icon = 0; };
    struct Fact { std::string label, textJson; };
    struct Bit { uint64_t icon = 0; std::string nameJson, textJson; };
    std::vector<Trait> traits;
    std::vector<Fact> facts;
    std::vector<Bit> sentiments, bits;
    // The game's relationship tooltip ("cdg", "cdj", "cdc", "cdx"): age, known careers, compatibility, attraction.
    Bit age, compatibility, attraction;
    struct Job { uint64_t icon = 0; std::string name, company, hours, days; };
    std::vector<Job> jobs;
    struct Act { int node = 0; std::string nameJson, disabledJson; };
    std::vector<Act> actions;  // the phone's Social app entries, run for this contact
    std::string note;  // "cdn": the last of them the game doesn't offer with this Sim
    int calls = 0, texts = 0;  // "cdo": how many of them are calls / texts the game offers with this Sim
};
struct SimItem {  // Inventory: one stack (phonesim.py "inv")
    uint64_t picture = 0;
    int count = 1, value = 0, order = 0;
    bool favorite = false, locked = false;  // locked: non_deletable_by_user (can't be sold)
    std::string name;
};
struct SimTraitRow {  // Simology: a trait, like or turn-on (phonesim.py "sy")
    char group = 'T';    // T traits, L likes, D dislikes, O turn-ons, F turn-offs
    int style = 0;       // the cell: 0 personality, 1 reward, 2 phase, 3 tradition, 4 fear
    uint64_t icon = 0;
    std::string name;
};
struct SimSouls {  // Soul's Journey ("sj", "sjg": phonesim._souls_journey)
    bool sent = false, ghost = false, complete = false;
    int rank = 0;
    float progress = 0;  // in the rank, 0..1
    std::string first;
    struct Goal { std::string text; bool done = false; int value = 0, goal = 1; bool show = true; std::string tip; };
    std::vector<Goal> goals;
    std::string tips[12];  // "sjt": 0-7 the pips', 8 the ghost lock's, 9-11 the help button's parts
};
struct SimClub {  // Social Groups: one club ("club", "cmem", "crule")
    uint64_t icon = 0;
    bool gathering = false;
    std::string name, description, leader;
    struct Member { uint64_t face = 0; std::string first; bool leader = false; };
    std::vector<Member> members;
    std::vector<std::pair<bool, std::string>> rules;  // encouraged, text
};
struct SimBusiness {  // Owned Businesses ("biz")
    int type = 0, funds = 0, revenue = 0, employees = 0;
    float stars = 0;
    bool open = false;
    std::string name;
};
struct SimWeather {  // the Weather app ("wx2": what the game's weather and season tooltip shows)
    bool sent = false, enabled = false;
    int season = -1, day = 0, length = 0, phase = -1, eco = -1;
    uint64_t icon[3] = {};          // the weather now, the temperature, today's forecast (large icons)
    std::string nameJson[3];
    std::string effectJson, ecoNameJson, ecoTextJson;  // the lunar effect; the eco footprint state and its text
    float ecoProgress = 0.5f, ecoDelta = 0;
};
struct SimContent {
    SimWeather weather;
    std::vector<SimBusiness> businesses;
    std::vector<SimClub> clubs;
    std::vector<SimItem> inventory;
    std::vector<SimTraitRow> simology;
    SimSouls souls;
    std::vector<SimContact> contacts;
    ContactPage contact;
    std::vector<SimMember> household;
    int age = 0;  // sims.sim_info_types.Age (0: not sent)
    std::string name;  // first and last
    long long funds = -1;  // the household's ("sim" f[3]; -1: not sent)
    int mood = 0;  // hudinfo MOODS index
    std::string moodJson;
    std::vector<SimJob> jobs;
    SimAspiration aspiration;
    std::vector<std::string> trackCategories;  // Change Aspiration: category names (json)
    std::vector<SimTrack> tracks;
    std::vector<std::pair<int, std::string>> categories;  // skill category, its title json
    std::vector<SimSkill> skills;
};
std::mutex g_simMutex;
SimContent g_simIn;      // g_simMutex
bool g_simNew = false;   // g_simMutex
SimContent g_sim;        // present thread
std::atomic<int> g_phoneTaps{0};    // presses (selects) on the phone's screen since the last phoneTaps()
std::atomic<int> g_phoneHovers{0};  // a new thing pointed at (highlighted) on it since the last phoneHovers()
std::atomic<int> g_phoneModeSet{-1};  // the Sounds screen's mode just picked (0 sound, 1 vibrate, 2 mute)
uint32_t g_hoverKey = 0, g_hoverKeySeen = 0;  // the button under the pointer last frame / this frame (its rect)
int g_mySimPage = 0;  // My Sim's page of apps (9 a page, as the phone's home)
bool g_phoneSwipeX = false, g_phoneSwipeEnd = false;  // this frame's sideways swipe on the phone, and its end
float g_phoneDx = 0;                                    // how far it went (phone units)  // presses on the phone's screen since the last phoneTaps()
int g_mySim = -1;        // present thread: -1 closed, 0 its apps, 1 + k sub-app k (kMySimApps)
float g_mySimScroll = 0; // game pixels the list is scrolled
ULONGLONG g_mySimAsked = 0;
int g_mySimMoodlet = -1;   // My Sim shows this moodlet's page
bool g_aspChange = false;   // Aspirations shows the tracks to change to
std::atomic<bool> g_phoneAsleep{false};  // the phone's screen is off (setPhoneAsleep)
// Rearranging the home screen (user 2026-10-24 02:06Z): the order saved by app (ours "#-n", the game's and mods'
// by their names) in sims4vr_user.ini [phone] app_order, '|' between; apps not in it follow in the game's order.
bool g_reorder = false;
std::string g_reorderPick;  // the app picked to move (its key), empty: none
std::vector<std::string> g_appOrder;
bool g_appOrderRead = false;
std::string utf8(const std::wstring& w);  // below
std::string appKey(const PhoneContent& p, int node) {
    return node < 0 ? "#" + std::to_string(node) : node < static_cast<int>(p.nodes.size()) ? p.nodes[node].name : std::string();
}
// An app order kept in sims4vr_user.ini [phone] (app_order: the home screen's; mysim_order: My Sim's).
void readOrder(const wchar_t* key, std::vector<std::string>& order, bool& read) {
    if (read) return;
    const std::wstring ini = settings::userIni();
    if (ini.empty()) return;
    read = true;
    char buf[8192] = {};
    GetPrivateProfileStringW(L"phone", key, L"", reinterpret_cast<wchar_t*>(buf), sizeof buf / 2, ini.c_str());
    std::string text = utf8(reinterpret_cast<const wchar_t*>(buf));
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('|', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) order.push_back(text.substr(at, end - at));
        at = end + 1;
    }
}
void saveOrder(const wchar_t* key, const std::vector<std::string>& order) {
    const std::wstring ini = settings::userIni();
    if (ini.empty()) return;
    std::string text;
    for (const std::string& k : order) text += (text.empty() ? "" : "|") + k;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(1, n)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n);
    WritePrivateProfileStringW(L"phone", key, w.c_str(), ini.c_str());
    LOG("Phone: app order saved (%zu apps)", order.size());
}
void readAppOrder() { readOrder(L"app_order", g_appOrder, g_appOrderRead); }
void saveAppOrder() { saveOrder(L"app_order", g_appOrder); }
std::vector<std::string> g_mySimOrder;  // My Sim's apps by name (user 2026-10-25 00:17Z: movable as the home screen's)
bool g_mySimOrderRead = false;
bool g_weatherOpen = false;    // the home screen's Weather app
bool g_cheatsOpen = false;     // the home screen's Cheats app (the debug menu's testing cheats)
bool g_gameMenuPhone = false;  // Settings > Game Options: the game's Escape menu drawn on the phone
int g_contacts = -1;        // Contacts: -1 closed, 0 the list, 1 a contact's page
int g_contactFilter = 0;     // Contacts: the filter (All, Household, Family...)
bool g_contactFilterOpen = false;
int g_contactSel = -1;      // the contact whose page is open
ULONGLONG g_contactsAsked = 0;
int g_hhSim = -1;           // Household shows this member's page
ULONGLONG g_playAsAt = 0;   // Play as pressed (the button waits while the switch runs)
int g_aspPicked = -1;       // the track pressed once (a second press changes to it)
ULONGLONG g_aspPickedAt = 0, g_aspHoverSince = 0;
// A press held and moved on the phone (laser or finger, user 2026-10-18 23:43Z):
// a swipe. Pages and lists follow it while held; a swipe presses no button.
struct PhoneDrag {
    bool on = false, moved = false, horizontal = false;
    ImVec2 start{};
    float startY = 0;  // game pixels down the screen where it began
    int page = 0, scroll = 0, notes = 0, cal = 0, week = 0;
    float sim = 0;
};
PhoneDrag g_drag;  // present thread
int g_calWeek = 0, g_calSel = -1, g_calScroll = 0;
int g_phoneApp = -1, g_phoneRoot = -1, g_phonePage = 0, g_phoneScroll = 0;
// The phone's Settings app (user 2026-10-23 20:55Z): its screen of apps (Game Options, Phone Options,
// VR Options), and Phone Options: the debug menu's phone pages on the phone (settings::pageRows).
bool g_phoneSettingsOpen = false;
bool g_phoneSoundsOpen = false;  // the Sounds screen (the header's top left button; phonesound.py)
std::wstring g_phoneOptsGroup;  // the debug menu page Phone Options shows ("" closed)
int g_phoneOptsScroll = 0;
float g_phoneText = 1;          // the phone's text size (Frame.phoneText)
std::vector<uint64_t> g_phonePreload;  // every picture the phone can show (apps, colours, notifications), read ahead (g_itemMutex)
std::vector<uint64_t> g_notesPreload;  // the notifications' pictures, read ahead (g_itemMutex)
std::string g_phoneRaw;                // the menu as Python sent it last (g_phoneMutex): the same again changes nothing
int g_phoneSerial = 0;                 // present thread: the menu's tree changed (a new screen to fade to)
// The phone's screen while it builds (user 2026-10-23 20:01Z): taken out, it is black with the game's
// spinning plumbob until its menu, texts and pictures are in, then fades in; a press that leads to a
// screen still building keeps a still of the last ready one (the press's screen waits) until the new
// one is ready, then fades over to it.
struct PhoneCover {
    bool loading = true;    // nothing ready shown since taken out: the black loading screen
    bool still = false;     // g_stillTexture holds the last ready screen
    float fade = 1;         // how much of the cover shows (1 while waiting, to 0 once ready)
    ULONGLONG waiting = 0;  // not ready since (0: ready)
    bool blocked = false;   // the cover is up: presses on the phone wait
    bool gaveUp = false;    // waited kCoverMaxWait: shown as it is
    int readyKey = -1;      // the screen shown ready last: pictures coming in later don't hold it again (no flicker)
};
// A screen opened that waits for fresh values from Python (the calendar, My Sim; user 2026-10-24 00:22Z:
// fully there before it opens, nothing filling in while you scroll): until they come, or this time.
ULONGLONG g_phoneDataUntil = 0;  // present thread
PhoneCover g_cover;                       // present thread
float sliderFollow(const Rect& r, float top, float bottom, float thumbH);  // below
std::atomic<bool> g_phoneTakenOut{false};
ImDrawList* g_phoneList = nullptr;        // the phone's screen, drawn apart (shown or not, after the rest)
ImDrawList* g_coverList = nullptr;        // over it: the black, the still, the plumbob
ID3D11Texture2D* g_stillTexture = nullptr;
ID3D11ShaderResourceView* g_stillView = nullptr;
ID3D11BlendState* g_premultiplied = nullptr;  // the still is premultiplied (as the atlas it is copied from)
ID3D11DeviceContext* g_drawContext = nullptr;  // render(): for the draw callbacks
constexpr float kCoverFade = 0.22f;      // seconds from the cover to the screen
constexpr ULONGLONG kCoverMaxWait = 4000;  // ms: then shown as it is (a picture that never comes)
constexpr ULONGLONG kSpinAfter = 150;
constexpr ULONGLONG kPictureWait = 700;    // ms a screen waits for its pictures      // ms waiting before the plumbob shows (no flash when it is quick)

// The tooltips of the needs and moodlets (hudinfo.py), and their texts once found.
struct Tip {
    std::string json[3];  // title, text, reason
    int minutes = -1;
};
std::vector<Tip> g_tips[kKinds];  // g_itemMutex
std::map<std::string, std::string> g_found;  // present thread: LocalizedString JSON -> text
int g_hoverKind = -1, g_hoverIndex = -1;    // present thread: the item pointed at
std::atomic<int> g_tipKind{-1};
std::atomic<float> g_tipHeight{0};

// The game's values for the needs, moodlets, queue and wants (hudinfo.py).
std::mutex g_itemMutex;
Item g_items[kKinds][kMaxItems];
int g_counts[kKinds] = {};

// The game's icons (buffs, needs, wants, interactions), 128 px slots in one
// texture, read on demand (missingIcons) and kept while used.
// 16 x 16 (2048 px): the phone's app icons and colours stay in beside the HUD's (they are read ahead, phone away).
constexpr int kIconSize = 128, kIconsAcross = 16, kIconSlots = kIconsAcross * kIconsAcross;
struct PendingIcon {
    uint64_t id;
    std::vector<uint32_t> pixels;  // kIconSize^2, RGBA straight
};
std::vector<PendingIcon> g_pendingIcons;  // decoded, to upload (g_itemMutex)
std::set<uint64_t> g_loadedIcons, g_badIcons;  // in a slot / could not be read (g_itemMutex)
std::set<uint64_t> g_pictureIds;  // thumbnails (thumbs.cpp), not asked of Python (g_itemMutex)
struct IconSlot {
    uint64_t id = 0;
    unsigned used = 0;  // frame last drawn
};
IconSlot g_slots[kIconSlots];  // present thread
unsigned g_frameNo = 0;
ID3D11Texture2D* g_iconTexture = nullptr;
ID3D11ShaderResourceView* g_icons = nullptr;
LARGE_INTEGER g_last{};
bool g_failed = false;

// The embedded art into a texture.
bool loadArt(ID3D11Device* device) {
    const size_t packed = static_cast<size_t>(s4vr_hud_art_end - s4vr_hud_art);
    if (packed < 12) return false;
    uint32_t w, h, runs;
    std::memcpy(&w, s4vr_hud_art, 4);
    std::memcpy(&h, s4vr_hud_art + 4, 4);
    std::memcpy(&runs, s4vr_hud_art + 8, 4);
    if (w != art::kAtlasW || h != art::kAtlasH) return false;
    std::vector<unsigned char> unpacked;
    if (!strings::unzlib(s4vr_hud_art + 12, packed - 12, unpacked, runs) || unpacked.size() != runs) return false;
    const unsigned char* p = unpacked.data();
    const size_t size = unpacked.size();
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0);
    size_t at = 0, i = 0;
    while (at + 4 <= size && i < pixels.size()) {
        uint32_t n;
        std::memcpy(&n, p + at, 4);
        at += 4;
        size_t count = n & 0x7fffffffu;
        if (count > pixels.size() - i) return false;
        if (!(n & 0x80000000u)) {
            if (at + count * 4 > size) return false;
            std::memcpy(&pixels[i], p + at, count * 4);
            at += count * 4;
        }
        i += count;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), w * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return false;
    HRESULT hr = device->CreateShaderResourceView(texture, nullptr, &g_art);
    texture->Release();
    return SUCCEEDED(hr);
}

bool init(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    // Smaller copies of it (mipmaps, user 2026-10-23: the cards looked low quality): a card seen smaller
    // than its pixels is drawn from them instead of skipping pixels (jagged, shimmering). Four levels: a
    // level-3 texel is 8 atlas pixels, so neighbouring areas bleed in at most a few pixels.
    desc.MipLevels = kAtlasMips;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_texture)) ||
        FAILED(device->CreateRenderTargetView(g_texture, nullptr, &g_target)) ||
        FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_atlasView))) {
        LOG("HUD: could not make its texture");
        release();
        return false;
    }
    IMGUI_CHECKVERSION();
    g_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    // Windows' own UI font, large: the atlas is seen up close in the headset.
    ImFontConfig config;
    config.OversampleH = 2;
    g_big = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 46.0f, &config);
    g_small = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 34.0f, &config);
    // Popup texts: Latin with accents, the typographic quotes, dashes, ellipsis
    // and bullet the game's strings use, and the Simoleon sign.
    static const ImWchar kTextRanges[] = {0x0020, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x20AC, 0x20AC,
                                          0x2122, 0x2122, 0};
    g_text = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 36.0f, &config, kTextRanges);
    g_title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 44.0f, &config, kTextRanges);
    if (!g_big || !g_small) {
        ImFontConfig fallback;
        fallback.SizePixels = 40;
        g_small = g_big = io.Fonts->AddFontDefault(&fallback);
        LOG("HUD: Segoe UI not found, the built-in font");
    }
    if (!g_text) g_text = g_small;
    if (!g_title) g_title = g_big;
    if (!ImGui_ImplDX11_Init(device, context)) {
        LOG("HUD: Dear ImGui's DirectX 11 renderer did not start");
        release();
        return false;
    }
    if (!loadArt(device)) LOG("HUD: the game's art did not load; shapes only");
    bindKit();
    {
        D3D11_TEXTURE2D_DESC icons{};
        icons.Width = icons.Height = kIconSize * kIconsAcross;
        icons.MipLevels = 1;
        icons.ArraySize = 1;
        icons.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        icons.SampleDesc.Count = 1;
        icons.Usage = D3D11_USAGE_DEFAULT;
        icons.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&icons, nullptr, &g_iconTexture)) ||
            FAILED(device->CreateShaderResourceView(g_iconTexture, nullptr, &g_icons)))
            LOG("HUD: no texture for the game's icons");
    }
    {
        // The phone's screen apart, its cover and the still of its last ready screen (PhoneCover).
        g_phoneList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
        g_coverList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
        D3D11_TEXTURE2D_DESC still{};
        still.Width = static_cast<UINT>(kPhone.w);
        still.Height = static_cast<UINT>(kPhone.h);
        still.MipLevels = 1;
        still.ArraySize = 1;
        still.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        still.SampleDesc.Count = 1;
        still.Usage = D3D11_USAGE_DEFAULT;
        still.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateTexture2D(&still, nullptr, &g_stillTexture)) ||
            FAILED(device->CreateShaderResourceView(g_stillTexture, nullptr, &g_stillView)) ||
            FAILED(device->CreateBlendState(&blend, &g_premultiplied)))
            LOG("HUD: no still for the phone's screen (it shows as it builds)");
    }
    g_device = device;
    g_device->AddRef();
    QueryPerformanceCounter(&g_last);
    LOG("HUD: Dear ImGui %s ready (%dx%d atlas)", IMGUI_VERSION, kWidth, kHeight);
    return true;
}

// The generic pieces live in the UI kit (uikit.h, docs/UI-KIT.md).
using uikit::kBodyDark;
using uikit::kButtonBlue;
using uikit::kReasonGrey;
using uikit::kShadow;
using uikit::kTextOn;
using uikit::kTitleBlue;
using uikit::kWhite;
using uikit::nine;
using uikit::piece;
using uikit::pieceIn;
using uikit::shadowText;
void icon(ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint = kWhite);  // below

// The game's TimeControls in its own art (TimeControlTray, the speed buttons'
// up / over / selected_up pictures; selected = running speed, pause red when
// paused) with the time below in the weather pill, as on the game's HUD. The
// pill's darker end is our full-UI-on-the-wrist button.
Action drawWatch(const Frame& f) {
    Action action = kNone;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kWatch;
    const float cx = r.x + r.w / 2;
    // The tray: the game's TimeControlTray, as drawn (light glass dome).
    piece(d, art::kTray, {cx, r.y + 20 + art::kTray.h / 2.0f});
    // The speed buttons: the running one in its selected picture, the one
    // under your finger with the game's over picture and green disc.
    int speed = g_speed.load();
    int lit = speed == 0 ? 0 : speed == 5 ? 3 : speed >= 1 && speed <= 3 ? speed : 1;
    static const art::Piece* const kUp[4] = {&art::kPauseUp, &art::kOneUp, &art::kTwoUp, &art::kThreeUp};
    static const art::Piece* const kOver[4] = {&art::kPauseOver, &art::kOneOver, &art::kTwoOver, &art::kThreeOver};
    static const art::Piece* const kOn[4] = {&art::kPauseOn, &art::kOneOn, &art::kTwoOn, &art::kThreeOn};
    const float by = r.y + 83, gap = 96, br = 46;
    for (int i = 0; i < 4; ++i) {
        ImVec2 c{cx + gap * (i - 1.5f), by};
        ImGui::SetCursorScreenPos({c.x - br, c.y - br});
        ImGui::PushID(i);
        bool pressed = ImGui::InvisibleButton("speed", {br * 2, br * 2});
        bool hover = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hover && i != lit) piece(d, art::kHoverDisc, c, 0.85f);
        piece(d, i == lit ? *kOn[i] : hover ? *kOver[i] : *kUp[i], c);
        if (pressed) action = i == 0 ? kPausePressed : static_cast<Action>(kSpeed1 + i - 1);
    }
    // The pill: day and time on its light part, "UI" on its dark end.
    const ImVec2 pa{cx - 235, r.y + 160}, pb{cx + 235, r.y + 226};
    pieceIn(d, art::kTimePill, pa, pb);
    int hour = g_hour.load(), minute = g_minute.load();
    char text[32] = "--:--";
    if (hour >= 0) {
        static const char* const kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int h12 = hour % 12 == 0 ? 12 : hour % 12;
        std::snprintf(text, sizeof text, "%s %d:%02d %s", kDays[(g_day.load() % 7 + 7) % 7], h12, minute,
                      hour < 12 ? "AM" : "PM");
    }
    const float split = pa.x + (pb.x - pa.x) * 0.635f;  // where the pill's dark end starts
    ImVec2 ts = g_big->CalcTextSizeA(g_big->FontSize, 1e9f, 0, text);
    shadowText(d, g_big, g_big->FontSize, {pa.x + (split - pa.x - ts.x) / 2, (pa.y + pb.y - ts.y) / 2}, kWhite, text);
    if (f.uiButton) {
        ImVec2 a{split, pa.y}, b{pb.x, pb.y};
        ImGui::SetCursorScreenPos(a);
        bool pressed = ImGui::InvisibleButton("fullui", {b.x - a.x, b.y - a.y});
        bool hover = ImGui::IsItemHovered();
        if (hover) d->AddRectFilled({a.x + 6, a.y + 6}, {b.x - 6, b.y - 6}, IM_COL32(255, 255, 255, 40), 26);
        const char* label = "UI";
        ImVec2 ls = g_small->CalcTextSizeA(g_small->FontSize, 1e9f, 0, label);
        shadowText(d, g_small, g_small->FontSize, {(a.x + b.x - ls.x) / 2, (a.y + b.y - ls.y) / 2},
                   f.fullUi ? kTextOn : kWhite, label);
        if (pressed) action = kFullUi;
    }
    return action;
}

// While paused: the game's selected pause picture (red), large.
void drawPause() {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kPauseArea;
    piece(d, art::kPauseOn, {r.x + r.w / 2, r.y + r.h / 2}, (r.h - 16) / art::kPauseOn.h);
}

// ARGB (Python's colours) to ImGui's.
ImU32 fromArgb(uint32_t c) {
    return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, (c >> 24) & 0xff);
}
using uikit::moodTint;

// The game's icon `id` in a box centred on c (nothing until it is read).
void icon(ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint) {
    if (!id || !g_icons) return;
    for (int i = 0; i < kIconSlots; ++i) {
        IconSlot& slot = g_slots[i];
        if (slot.id != id) continue;
        slot.used = g_frameNo;
        float u = static_cast<float>(i % kIconsAcross) / kIconsAcross, v = static_cast<float>(i / kIconsAcross) / kIconsAcross;
        float step = 1.0f / kIconsAcross;
        d->AddImage(reinterpret_cast<ImTextureID>(g_icons), {c.x - size / 2, c.y - size / 2},
                    {c.x + size / 2, c.y + size / 2}, {u, v}, {u + step, v + step}, tint);
        return;
    }
}

void bindKit() {
    uikit::Resources kit;
    kit.art = reinterpret_cast<ImTextureID>(g_art);
    kit.big = g_big, kit.small = g_small, kit.text = g_text, kit.title = g_title;
    kit.icon = [](ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint) { icon(d, id, c, size, tint); };
    uikit::bind(kit);
}

// Decoded icons into free (or the longest unused) slots.
void uploadIcons(ID3D11DeviceContext* context) {
    std::vector<PendingIcon> pending;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        pending.swap(g_pendingIcons);
    }
    if (!g_iconTexture) return;
    for (PendingIcon& p : pending) {
        int best = -1;
        for (int i = 0; i < kIconSlots; ++i) {
            if (g_slots[i].id == p.id) {
                best = i;
                break;
            }
            if (g_slots[i].used == g_frameNo && g_slots[i].id) continue;  // drawn this frame
            if (best < 0 || (g_slots[best].id && (!g_slots[i].id || g_slots[i].used < g_slots[best].used))) best = i;
        }
        if (best < 0) continue;
        uint64_t old = g_slots[best].id;
        D3D11_BOX box{static_cast<UINT>(best % kIconsAcross * kIconSize), static_cast<UINT>(best / kIconsAcross * kIconSize),
                      0, static_cast<UINT>(best % kIconsAcross * kIconSize + kIconSize),
                      static_cast<UINT>(best / kIconsAcross * kIconSize + kIconSize), 1};
        context->UpdateSubresource(g_iconTexture, 0, &box, p.pixels.data(), kIconSize * 4, 0);
        g_slots[best] = {p.id, g_frameNo};
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (old && old != p.id) g_loadedIcons.erase(old);
        g_loadedIcons.insert(p.id);
    }
}

// The needs: the game's motive panel grid (2 x 3, its order), each the need's
// icon and its bar in the tuned fill and background colours of its state
// (CommodityState.fill_color / background_color), a sliver even when empty
// as the game pads it.
void drawNeeds(const Item* items, int count) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kNeeds;
    const float cw = r.w / 2, ch = r.h / 3;
    for (int i = 0; i < count && i < 6; ++i) {
        const Item& it = items[i];
        float x = r.x + cw * (i % 2), y = r.y + ch * (i / 2), cy = y + ch / 2;
        ImGui::SetCursorScreenPos({x, y});
        ImGui::PushID(400 + i);
        ImGui::InvisibleButton("need", {cw, ch});
        if (ImGui::IsItemHovered()) g_hoverKind = kNeedItems, g_hoverIndex = i;
        ImGui::PopID();
        icon(d, it.icon, {x + 44, cy}, 66);
        ImVec2 a{x + 88, cy - 15}, b{x + cw - 12, cy + 15};
        ImU32 back = (it.flags >> 24) ? fromArgb(it.flags) : IM_COL32(40, 44, 52, 200);
        d->AddRectFilled({a.x - 3, a.y - 3}, {b.x + 3, b.y + 3}, IM_COL32(20, 22, 28, 150), 18);
        d->AddRectFilled(a, b, back, 15);
        float fill = 0.07f + 0.93f * std::max(0.0f, std::min(1.0f, it.value));
        d->AddRectFilled(a, {a.x + (b.x - a.x) * fill, b.y}, fromArgb(it.colour | 0xff000000u), 15);
        d->AddRectFilled({a.x + 4, a.y + 3}, {a.x + (b.x - a.x) * fill - 4, a.y + 10}, IM_COL32(255, 255, 255, 60), 6);
    }
}

// The moodlets: the game's BuffBGClip tile in the buff's mood colour, the
// buff's icon on it (SimHub), 4 across, 2 down.
void drawMoodlets(const Item* items, int count) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kMoodlets;
    const float cell = r.w / 4;
    for (int i = 0; i < count && i < 8; ++i) {
        ImVec2 c{r.x + cell * (i % 4) + cell / 2, r.y + cell * (i / 4) + cell / 2};
        ImGui::SetCursorScreenPos({c.x - cell / 2, c.y - cell / 2});
        ImGui::PushID(500 + i);
        ImGui::InvisibleButton("moodlet", {cell, cell});
        bool hover = ImGui::IsItemHovered();
        if (hover) g_hoverKind = kMoodletItems, g_hoverIndex = i;
        ImGui::PopID();
        piece(d, art::kMoodTile, c, (hover ? 120.0f : 112.0f) / art::kMoodTile.w, moodTint(items[i].colour));
        icon(d, items[i].icon, c, 84);
    }
}

// The action queue as the game's InteractionQueue draws it (its SWF:
// NormalInteractionButton, MixerInteractionButton; InteractionButton.as),
// one game pixel kQueuePx atlas pixels, the buttons side by side (the game
// stacks them), running first:
// - the disc: the Sim's mood frame of InteractionButtonEmotionBg when the
//   interaction lists that mood, else its Default (Fine) frame, at 0.917
//   (1.0 under the pointer), 1 px low; queued ones at 0.2 alpha, running or
//   starting ones full;
// - the picture: 44 px (48 under the pointer), queued at 0.8 alpha (0.9);
// - its progress, when the game sends one: the dark back, the green spokes
//   (100 round, clockwise from the top) and the rim, under the disc;
// - the X (mcCancel) under the pointer, if it can be cancelled, and on the
//   buttons the one pointed at would cancel (interactions_to_be_canceled);
// - a side action (a mixer): its small button 20 px right and 10 px down,
//   disc at 0.889 (32 px), picture 24 px (28), its own X.
constexpr float kQueuePx = 2.1f;
Action drawQueue(const Item* items, const Item* badges, int count, int* index) {
    Action action = kNone;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kQueue;
    count = std::min(count, kMaxItems);
    // Each button 58 game px apart (BUTTON_HEIGHT), more after one with a side
    // action (its badge reaches 36 px out); all made smaller if they don't fit.
    float centres[kMaxItems] = {}, x = 0;
    for (int i = 0; i < count; ++i) {
        centres[i] = x + 31;
        x += 58 + ((badges[i].flags & 1) ? 10 : 0);
    }
    const float px = std::min(kQueuePx, (r.w - 16) / std::max(1.0f, x + 4));
    const float cy = r.y + 72;
    auto at = [&](int i) { return ImVec2{r.x + 8 + centres[i] * px, cy}; };
    auto badgeAt = [&](int i) { return ImVec2{at(i).x + 20 * px, cy + 10 * px}; };
    // The side actions' buttons first: over their main button, they get the pointer.
    bool badgeHover[kMaxItems] = {}, hover[kMaxItems] = {};
    for (int i = 0; i < count; ++i) {
        if (!(badges[i].flags & 1)) continue;
        const ImVec2 c = badgeAt(i);
        ImGui::SetCursorScreenPos({c.x - 16 * px, c.y - 16 * px});
        ImGui::PushID(140 + i);
        const bool pressed = ImGui::InvisibleButton("mixer", {32 * px, 32 * px});
        badgeHover[i] = ImGui::IsItemHovered();
        ImGui::PopID();
        if (badgeHover[i]) g_hoverKind = kQueueItems, g_hoverIndex = 2 * i + 1;
        if (pressed && (badges[i].flags & 2)) {
            action = kCancel;
            if (index) *index = kMaxItems + i;
        }
    }
    unsigned preview = 0;  // the buttons the one pointed at would cancel
    for (int i = 0; i < count; ++i) {
        const ImVec2 c = at(i);
        ImGui::SetCursorScreenPos({c.x - 30 * px, c.y - 30 * px});
        ImGui::PushID(100 + i);
        const bool pressed = ImGui::InvisibleButton("queue", {60 * px, 60 * px});
        hover[i] = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hover[i]) {
            g_hoverKind = kQueueItems, g_hoverIndex = 2 * i;
            preview = (items[i].flags >> 8) & 0xff;
        }
        if (pressed && (items[i].flags & 2)) {
            action = kCancel;
            if (index) *index = i;
        }
    }
    for (int i = 0; i < count; ++i) {
        const Item& it = items[i];
        const ImVec2 c = at(i);
        const bool full = it.flags & 1;  // running or starting
        const bool over = hover[i] || ((preview >> i) & 1);
        if (full) uikit::progressRing(d, c, px, it.value);
        uikit::pictureButton(d, it.icon, it.colour, c, px, full, over, it.flags & 2);
        const Item& b = badges[i];
        if (b.flags & 1) uikit::badge(d, b.icon, b.colour, badgeAt(i), px, b.flags & 4, badgeHover[i], b.flags & 2);
    }
    return action;
}

// The money gained or lost, as the game's flyaway over its funds: "+§100"
// in green, "-§100" in red, rising and fading over kMoneySeconds; the newest
// lowest, up to three at once.
constexpr float kMoneySeconds = 2.6f;
struct MoneyChange {
    long long amount;
    ULONGLONG at;
};
std::mutex g_moneyMutex;
std::vector<MoneyChange> g_money;  // g_moneyMutex

std::string moneyText(long long amount) {
    unsigned long long v = amount < 0 ? 0ull - static_cast<unsigned long long>(amount) : amount;
    std::string digits = std::to_string(v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return std::string(amount < 0 ? "-" : "+") + "\xC2\xA7" + out;
}

void drawMoney() {
    std::vector<MoneyChange> shown;
    {
        std::lock_guard<std::mutex> lock(g_moneyMutex);
        const ULONGLONG now = GetTickCount64();
        g_money.erase(std::remove_if(g_money.begin(), g_money.end(),
                                     [&](const MoneyChange& m) { return now - m.at > kMoneySeconds * 1000; }),
                      g_money.end());
        shown = g_money;
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kMoney;
    const ULONGLONG now = GetTickCount64();
    const int n = static_cast<int>(shown.size());
    for (int i = std::max(0, n - 3); i < n; ++i) {
        const float t = (now - shown[i].at) / 1000.0f / kMoneySeconds;
        const float alpha = t < 0.75f ? 1.0f : std::max(0.0f, (1 - t) / 0.25f);
        const std::string text = moneyText(shown[i].amount);
        const float size = g_title->FontSize * 1.25f;
        const ImVec2 ts = g_title->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        const float y = r.y + r.h - ts.y - 4 - t * (r.h - ts.y - 8);
        const float x = r.x + (r.w - ts.x) / 2;
        const bool gain = shown[i].amount >= 0;
        const ImU32 fill = gain ? IM_COL32(118, 200, 64, static_cast<int>(255 * alpha))
                                : IM_COL32(222, 62, 52, static_cast<int>(255 * alpha));
        const ImU32 edge = IM_COL32(20, 30, 20, static_cast<int>(200 * alpha));
        for (int dx = -2; dx <= 2; dx += 2)
            for (int dy = -2; dy <= 2; dy += 2)
                if (dx || dy) d->AddText(g_title, size, {x + dx, y + dy}, edge, text.c_str());
        d->AddText(g_title, size, {x, y}, fill, text.c_str());
    }
}

// The wants: the game's whim cloud with the want's icon, up to three.
Action drawWants(const Item* items, int count, int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kWants;
    // The game's wants panel: each want in a cloud, a fear (flags bit 0) in
    // the spiky bubble. Pointed at, a want shows the game's X; pressed, it is
    // dismissed (whims.refresh), as in the game.
    const float cell = r.w / 4;
    Action action = kNone;
    for (int i = 0; i < count && i < 4; ++i) {
        ImVec2 c{r.x + cell * i + cell / 2, r.y + r.h / 2};
        const bool fear = items[i].flags & 1;
        ImGui::SetCursorScreenPos({c.x - cell / 2, r.y});
        ImGui::PushID(200 + i);
        bool pressed = ImGui::InvisibleButton("want", {cell, r.h});
        bool hover = ImGui::IsItemHovered();
        if (hover) g_hoverKind = kWantItems, g_hoverIndex = i;
        ImGui::PopID();
        const art::Piece& back = fear ? art::kFearBubble : art::kWantCloud;
        float scale = (cell - 12) / back.w * (hover ? 1.06f : 1.0f);
        piece(d, back, c, scale);
        icon(d, items[i].icon, {c.x, c.y + (fear ? 0 : 5)}, fear ? 92 : 84);
        if (hover && !fear) piece(d, art::kCancel, {c.x + cell * 0.3f, c.y - r.h * 0.3f}, 56.0f / art::kCancel.w);
        if (pressed && !fear) {
            action = kDismissWant;
            if (index) *index = i;
        }
    }
    return action;
}

// The texts of the popup from the game's string tables, once they are loaded.
void resolvePopup() {
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (g_popupNew) {
            g_popup = g_popupIn;
            g_popupOn = !g_popupIn.textJson.empty() || !g_popupIn.titleJson.empty() || !g_popupIn.buttonJson.empty() ||
                        !g_popupIn.rows.empty();
            g_popupNew = false;
            g_listPage = 0;
            g_textPage = 0;
            g_listChosen.assign(g_popup.rows.size(), 0);
            for (size_t i = 0; i < g_popup.rows.size(); ++i) g_listChosen[i] = g_popup.rows[i].selected && g_popup.multi;
            g_popupHeight = 0;
        }
    }
    if (!g_popupOn || g_popup.resolved || !strings::ready()) return;
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (g_popupNew) return;  // a newer one came meanwhile
    }
    PopupContent& p = g_popup;
    p.resolved = true;
    if (!p.titleJson.empty()) p.title = strings::resolve(p.titleJson);
    if (!p.textJson.empty()) p.text = strings::resolve(p.textJson);
    if (p.layout == 2) {
        if (!p.subtitleJson.empty()) p.subtitle = strings::resolve(p.subtitleJson);
        int named = 0;
        for (PopupContent::Row& r : p.rows) {
            if (r.nameJson.size() > 2) r.name = strings::resolve(r.nameJson);
            if (r.descJson.size() > 2) r.desc = strings::resolve(r.descJson);
            if (!r.name.empty()) ++named;
        }
        // Every row needs its words: an unknown one keeps the cut of the game's picker.
        p.valid = !p.rows.empty() && named == static_cast<int>(p.rows.size());
        {
            std::lock_guard<std::mutex> lock(g_popupMutex);
            if (!g_popupNew) g_popupState = p.valid ? 2 : 3;
        }
        static int logged = 0;
        if (logged++ < 40)
            LOG("HUD: list picker texts %s (%zu rows, %d named, %s)", p.valid ? "found" : "missing: the game's picker is shown cut out",
                p.rows.size(), named, p.multi ? "several" : "one");
        return;
    }
    int named = 0;
    for (size_t i = 0; i < p.buttonJson.size(); ++i) {
        std::string label = p.buttonJson[i].size() > 2 ? strings::resolve(p.buttonJson[i]) : std::string();
        if (!label.empty()) {
            ++named;
        } else {
            // A button without a text of its own: the game's OK / Cancel.
            long long id = i < p.choices.size() ? p.choices[i] : 0;
            label = id == 10002 ? "Cancel" : id == -1 ? "Close" : "OK";
        }
        p.buttons.push_back(label);
    }
    {
        // The buttons' tooltips (the game's response tooltip_text), for drawTip.
        std::vector<Tip> tips(p.buttonJson.size());
        for (size_t i = 0; i < tips.size() && i < p.tipJson.size(); ++i)
            if (p.tipJson[i].size() > 2) tips[i].json[1] = p.tipJson[i];
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_tips[kPopupButtons] = tips;
    }
    // Its words must be there: an unknown text (a string we don't have) keeps
    // the cut of the game's own dialog.
    bool textOk = p.textJson.empty() || !p.text.empty() || !p.title.empty();  // a title-only warning (2026-10-23)
    p.valid = textOk && (!p.text.empty() || !p.title.empty()) &&
              (p.buttonJson.empty() || named > 0 || p.buttonJson.size() <= 2);
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (!g_popupNew) g_popupState = p.valid ? 2 : 3;
    }
    static int logged = 0;
    if (logged++ < 40)
        LOG("HUD: popup texts %s (layout %d, title %zu chars, text %zu chars, %zu buttons, %d with own text)",
            p.valid ? "found" : "missing: the game's dialog is shown cut out", p.layout, p.title.size(), p.text.size(),
            p.buttons.size(), named);
}

using uikit::dialogButton;
using uikit::portrait;

// The popup as the game draws it. Two Sims talking (SimInteractionDialog):
// the other Sim's portrait with what they say in the blue speech bubble, your
// answers as buttons in the light bubble beside your portrait. Otherwise the
// plain dialog: its panel, title, text and buttons. Returns kAnswer with the
// button's index; its height goes to popupHeight().
// A picker of rows (the game's GUIPicker / SimPicker, re/liveui/components/Pickers.md) as its card: the dialog
// panel, title and subtitle, the X (unless the picker has none), a page of rows (icon, name, description;
// greyed when not enabled) and the page arrows. One to pick: a row press answers. Several: rows tick, Done
// (from min_selectable, up to max_selectable; 0 = any) answers with the ticked ones.
Action drawList(int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const PopupContent& p = g_popup;
    const Rect r = kPopup;
    const ImU32 kTitle = IM_COL32(29, 58, 102, 255), kBody = IM_COL32(44, 52, 66, 255), kGrey = IM_COL32(130, 136, 146, 255);
    const float x0 = r.x + 8, x1 = r.x + r.w - 8, top = r.y + 8, pad = 34, in0 = x0 + pad, in1 = x1 - pad;
    const float titleSize = g_title->FontSize, textSize = g_text->FontSize * 0.8f, descSize = g_text->FontSize * 0.62f;
    Action action = kNone;
    std::vector<uint64_t> shown;
    for (uint64_t pic : {p.pictures[0], p.pictures[1]})
        if (pic) shown.push_back(pic);
    int id = 0;
    auto button = [&](ImVec2 a, ImVec2 b, bool* hover) {
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(3000 + id++);
        bool pressed = ImGui::InvisibleButton("row", {std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)});
        if (hover) *hover = ImGui::IsItemHovered();
        ImGui::PopID();
        return pressed;
    };
    const int n = static_cast<int>(p.rows.size());
    if (static_cast<int>(g_listChosen.size()) != n) g_listChosen.assign(n, 0);
    int ticked = 0;
    for (char c : g_listChosen) ticked += c ? 1 : 0;

    const float wrapTitle = in1 - in0 - 160;
    ImVec2 tt = p.title.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(titleSize, FLT_MAX, wrapTitle, p.title.c_str());
    ImVec2 st = p.subtitle.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(descSize, FLT_MAX, in1 - in0, p.subtitle.c_str());
    const float rowH = 96, rowGap = 8, arrowsH = 76, doneH = 76;
    const float headH = 30 + std::max(tt.y, 60.0f) + (st.y > 0 ? 12 + st.y : 0) + 22;
    const float footFixed = 22 + std::max(p.multi ? doneH : 0.0f, 0.0f) + 30 + 16;
    int perPage = std::max(3, std::min(8, static_cast<int>((r.h - headH - footFixed - arrowsH - 22 + rowGap) / (rowH + rowGap))));
    const bool paged = n > perPage;
    if (!paged) perPage = std::max(1, n);
    const int pages = std::max(1, (n + perPage - 1) / perPage);
    g_listPage = std::max(0, std::min(g_listPage, pages - 1));
    const int linesShown = paged ? perPage : n;
    const float listY = top + headH;
    const float listH = linesShown * (rowH + rowGap) - rowGap;
    const float footY = listY + listH + 22;
    const float footH = std::max(paged ? arrowsH : 0.0f, p.multi ? doneH : 0.0f);
    const float bottom = footY + footH + 30;
    nine(d, art::kDialogPanel, {x0, top}, {x1, bottom}, 36, 36, 36, 36, 1, IM_COL32(244, 247, 252, 255));

    float y = top + 30;
    if (tt.y > 0) d->AddText(g_title, titleSize, {(in0 + in1 - tt.x) / 2, y}, kTitle, p.title.c_str(), nullptr, wrapTitle);
    if (p.canClose) {
        const float s = 76;
        ImVec2 c{in1 - s / 2 + 8, y + titleSize / 2};
        bool hover = false;
        if (button({c.x - s / 2, c.y - s / 2}, {c.x + s / 2, c.y + s / 2}, &hover)) action = kListClose;
        piece(d, art::kCancel, c, (hover ? s * 1.08f : s) / art::kCancel.w);
    }
    y += std::max(tt.y, 60.0f);
    if (st.y > 0) {
        d->AddText(g_text, descSize, {in0, y + 12}, kBody, p.subtitle.c_str(), nullptr, in1 - in0);
    }

    const int first = g_listPage * perPage;
    int tipRow = -1;  // the row pointed at whose description doesn't fit its one line
    float tipY0 = 0, tipY1 = 0, tipX = 0;
    for (int k = first; k < std::min(first + perPage, n); ++k) {
        const PopupContent::Row& row = p.rows[k];
        const float ry = listY + (k - first) * (rowH + rowGap);
        const ImVec2 a{in0, ry}, b{in1, ry + rowH};
        bool hover = false;
        const bool full = p.multi && p.maxSel > 0 && ticked >= p.maxSel && !g_listChosen[k];
        const bool usable = row.enabled && !(full && p.maxSel > 1);
        if (button(a, b, &hover) && usable) {
            if (!p.multi) {
                action = kListPick;
                if (index) *index = k;
            } else if (p.maxSel == 1) {
                const bool was = g_listChosen[k] != 0;
                std::fill(g_listChosen.begin(), g_listChosen.end(), 0);
                g_listChosen[k] = was ? 0 : 1;
            } else {
                g_listChosen[k] = g_listChosen[k] ? 0 : 1;
            }
        }
        const bool on = p.multi && g_listChosen[k];
        nine(d, art::kDialogPanel, a, b, 36, 36, 36, 36, 0.5f,
             !usable ? IM_COL32(234, 236, 240, 255) : on ? IM_COL32(170, 208, 244, 255)
                                                   : hover ? IM_COL32(214, 230, 250, 255) : kWhite);
        float tx = a.x + 18;
        if (p.multi) {
            const float s = 40;
            const ImVec2 ca{tx, (a.y + b.y - s) / 2}, cb{tx + s, (a.y + b.y + s) / 2};
            d->AddRectFilled(ca, cb, kWhite, 6);
            d->AddRect(ca, cb, kTitle, 6, 0, 3);
            if (on) {
                const ImVec2 tick[3] = {{ca.x + 8, ca.y + s * 0.52f}, {ca.x + s * 0.42f, cb.y - 9}, {cb.x - 7, ca.y + 9}};
                d->AddPolyline(tick, 3, kTitle, 0, 5);
            }
            tx += s + 16;
        }
        if (row.icon) {
            const float is = rowH - 20;
            icon(d, row.icon, {tx + is / 2, (a.y + b.y) / 2}, is, usable ? kWhite : IM_COL32(255, 255, 255, 110));
            shown.push_back(row.icon);
            tx += is + 16;
        }
        const ImVec4 clip{tx, a.y + 4, b.x - 12, b.y - 4};
        const float nameY = row.desc.empty() ? a.y + (rowH - textSize) / 2 - 4 : a.y + 12;
        d->AddText(g_text, textSize, {tx, nameY}, usable ? kBody : kGrey, row.name.c_str(), nullptr, 0, &clip);
        if (!row.desc.empty()) {
            // One line of it, as the game's ObjectPickerCellView.
            std::string line = row.desc.substr(0, row.desc.find('\n'));
            d->AddText(g_text, descSize, {tx, a.y + 16 + textSize}, kGrey, line.c_str(), nullptr, 0, &clip);
            if (hover && (line.size() < row.desc.size() ||
                          g_text->CalcTextSizeA(descSize, FLT_MAX, 0, line.c_str()).x > clip.z - tx))
                tipRow = k, tipY0 = a.y, tipY1 = b.y, tipX = tx;
        }
    }
    if (tipRow >= 0) {
        // The whole description of the row pointed at (user 2026-10-23: long ones were cut off), in a box over
        // the rows under it, or over those above when it doesn't fit below; never past the card.
        const std::string& desc = p.rows[tipRow].desc;
        const float wrap = in1 - 24 - tipX, m = 16;
        ImVec2 ds = g_text->CalcTextSizeA(descSize, FLT_MAX, wrap, desc.c_str());
        const float boxH = ds.y + 2 * m, limit = std::max(footY + 76, listY + listH);
        float by = tipY1 + 4;
        if (by + boxH > limit) by = std::max(listY, tipY0 - 4 - boxH);
        const ImVec2 ba{tipX - m, by}, bb{in1 - 8, by + boxH};
        d->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 250), 10);
        d->AddRect(ba, bb, kTitle, 10, 0, 2);
        d->AddText(g_text, descSize, {tipX, by + m}, kBody, desc.c_str(), nullptr, wrap);
    }

    // The footer: Done (several to pick) on the left, the page arrows on the right.
    if (p.multi) {
        const bool can = ticked >= std::max(0, p.minSel) && (ticked > 0 || p.minSel <= 0);
        char label[48];
        if (p.maxSel > 1)
            std::snprintf(label, sizeof label, "Done (%d/%d)", ticked, p.maxSel);
        else
            std::snprintf(label, sizeof label, "Done");
        if (dialogButton(d, 63, label, {in0, footY}, {in0 + 330, footY + doneH}) && can) {
            g_listPicked.clear();
            for (int k = 0; k < n; ++k)
                if (g_listChosen[k]) g_listPicked.push_back(k);
            action = kListDone;
        }
    }
    if (paged) {
        const float bw = 104, bh = 76;
        char where[32];
        std::snprintf(where, sizeof where, "%d / %d", g_listPage + 1, pages);
        ImVec2 ws = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, where);
        const float nx = in1 - bw, px = nx - 24 - ws.x - 24 - bw;
        if (dialogButton(d, 64, "<", {px, footY}, {px + bw, footY + bh})) g_listPage = (g_listPage + pages - 1) % pages;
        d->AddText(g_text, textSize, {px + bw + 24, footY + (bh - 10 - ws.y) / 2}, kBody, where);
        if (dialogButton(d, 65, ">", {nx, footY}, {nx + bw, footY + bh})) g_listPage = (g_listPage + 1) % pages;
    }
    std::sort(shown.begin(), shown.end());
    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (shown != g_popupIcons) g_popupIcons.swap(shown);
    }
    g_popupHeight = std::min(r.h, bottom - r.y + 8);
    return action;
}

Action drawPopup(int* index) {
    if (g_popup.layout == 2) return drawList(index);
    if (g_popup.layout == 3) {
        // A Sim's question with its answers on the pie: just their speech bubble (the game's, as layout 1 draws it).
        ImDrawList* d = ImGui::GetBackgroundDrawList();
        const Rect r = kPopup;
        const std::string& said = g_popup.text.empty() ? g_popup.title : g_popup.text;
        const float size = g_text->FontSize, wrap = r.w - 16 - 40 - 44 - 40;
        ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, said.c_str());
        const float y0 = r.y + 8, h = std::max(132.0f, ts.y + 64);
        uikit::speechBubble(d, {r.x + 8, y0}, {r.x + r.w - 8, y0 + h});
        d->AddText(g_text, size, {r.x + 8 + 40 + 16, y0 + 5 + (h - 9 - ts.y) / 2}, uikit::kWhite, said.c_str(), nullptr, wrap);
        g_popupHeight = std::min(r.h, h + 20);
        return kNone;
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const PopupContent& p = g_popup;
    const Rect r = kPopup;
    Action action = kNone;
    float bottom = r.y;
    const int n = static_cast<int>(p.buttons.size());
    auto answer = [&](int i, ImVec2 a, ImVec2 b) {
        if (dialogButton(d, i, p.buttons[i], a, b)) {
            action = kAnswer;
            if (index) *index = i;
        }
        if (ImGui::IsItemHovered()) g_hoverKind = kPopupButtons, g_hoverIndex = i;
    };
    if (p.layout == 1) {
        const float P = 176, gap = 26;
        const float bx0 = r.x + 8 + P + gap, bx1 = r.x + r.w - 8 - P - gap;
        // What they say: the speech bubble (its tail on the left, at the top).
        const std::string& said = p.text.empty() ? p.title : p.text;
        // (Its corners at 0.6: the tail and roundings as the game's are to its text.)
        const float size = g_text->FontSize, wrap = bx1 - bx0 - 40 - 44;
        ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, said.c_str());
        const float y0 = r.y + 8, h = std::max(P * 0.75f, ts.y + 64);
        uikit::speechBubble(d, {bx0, y0}, {bx1, y0 + h});
        d->AddText(g_text, size, {bx0 + 40 + 16, y0 + 5 + (h - 9 - ts.y) / 2}, kWhite, said.c_str(), nullptr, wrap);
        portrait(d, p.pictures[0], art::kPortraitRing, {r.x + 8 + P / 2, y0 + P / 2}, P);
        // Your answers: the light bubble (its tail on the right, low), twice
        // over: the game draws it over its dimming, here it is over the world.
        const float bh = 76, bgap = 12;
        const float y1 = y0 + h + 18;
        const float fh = std::max(130.0f, 30 + n * bh + std::max(0, n - 1) * bgap + 34);
        uikit::answerBubble(d, {bx0, y1}, {bx1 + 40, y1 + fh});
        for (int i = 0; i < n; ++i) {
            float y = y1 + 30 + i * (bh + bgap);
            answer(i, {bx0 + 24, y}, {bx1 + 40 - 40 - 18, y + bh});
        }
        portrait(d, p.pictures[1], art::kYourRing, {r.x + r.w - 8 - P / 2, y1 + P / 2 + 6}, P);
        bottom = std::max(y1 + fh, y1 + P + 20);
    } else {
        // The plain dialog: the panel, the title, the text, the buttons (side
        // by side for two short ones, else one above the other).
        const float x0 = r.x + 40, x1 = r.x + r.w - 40, pad = 56, wrap = x1 - x0 - 2 * pad;
        float y = r.y + 8 + 44;
        const ImU32 kTitle = IM_COL32(29, 58, 102, 255), kBody = IM_COL32(44, 52, 66, 255);
        ImVec2 tt = p.title.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(g_title->FontSize, FLT_MAX, wrap, p.title.c_str());
        ImVec2 xs = p.text.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(g_text->FontSize, FLT_MAX, wrap, p.text.c_str());
        bool row = n == 2 && p.buttons[0].size() < 18 && p.buttons[1].size() < 18;
        const float bh = 76, bgap = 16;
        float buttonsH = n == 0 ? 0 : row ? bh : n * bh + (n - 1) * bgap;
        // A text longer than the card holds (a mod's release notes): shown a page at a time with arrows under it,
        // so the buttons always stay on the card.
        const float arrowsH = 76, fixedH = 44 + tt.y + (tt.y > 0 && xs.y > 0 ? 20 : 0) + (n ? 40 : 0) + buttonsH + 50 + 20;
        float textH = xs.y;
        int pages = 1;
        if (xs.y > 0 && fixedH + xs.y > r.h) {
            textH = std::max(120.0f, r.h - fixedH - arrowsH - 20);
            const float line = g_text->FontSize * 1.2f, step = std::max(line, std::floor((textH - line) / line) * line);
            pages = 1 + static_cast<int>(std::ceil((xs.y - textH) / step));
            textH = std::floor(textH / line) * line;
            g_textPage = std::max(0, std::min(g_textPage, pages - 1));
        }
        float h = fixedH - 20 + textH + (pages > 1 ? arrowsH + 20 : 0);
        uikit::dialogPanel(d, {x0, r.y + 8}, {x1, r.y + 8 + h});
        if (tt.y > 0) {
            d->AddText(g_title, g_title->FontSize, {(x0 + x1 - tt.x) / 2, y}, kTitle, p.title.c_str(), nullptr, wrap);
            y += tt.y + (xs.y > 0 ? 20 : 0);
        }
        if (xs.y > 0 && pages > 1) {
            const float line = g_text->FontSize * 1.2f, step = std::max(line, std::floor((textH - line) / line) * line);
            d->PushClipRect({x0, y}, {x1, y + textH}, true);
            d->AddText(g_text, g_text->FontSize, {x0 + pad, y - g_textPage * step}, kBody, p.text.c_str(), nullptr, wrap);
            d->PopClipRect();
            y += textH + 20;
            const float bw = 104;
            char where[32];
            std::snprintf(where, sizeof where, "%d / %d", g_textPage + 1, pages);
            ImVec2 ws = g_text->CalcTextSizeA(g_text->FontSize, FLT_MAX, 0, where);
            const float nx = x1 - pad - bw, px = nx - 24 - ws.x - 24 - bw;
            if (dialogButton(d, 66, "<", {px, y}, {px + bw, y + arrowsH})) g_textPage = (g_textPage + pages - 1) % pages;
            d->AddText(g_text, g_text->FontSize, {px + bw + 24, y + (arrowsH - 10 - ws.y) / 2}, kBody, where);
            if (dialogButton(d, 67, ">", {nx, y}, {nx + bw, y + arrowsH})) g_textPage = (g_textPage + 1) % pages;
            y += arrowsH;
        } else if (xs.y > 0) {
            d->AddText(g_text, g_text->FontSize, {x0 + pad, y}, kBody, p.text.c_str(), nullptr, wrap);
            y += xs.y;
        }
        y += n ? 40 : 0;
        if (row) {
            float w = (wrap - 30) / 2;
            answer(0, {x0 + pad, y}, {x0 + pad + w, y + bh});
            answer(1, {x1 - pad - w, y}, {x1 - pad, y + bh});
        } else {
            for (int i = 0; i < n; ++i, y += bh + bgap) answer(i, {x0 + pad + 40, y}, {x1 - pad - 40, y + bh});
        }
        bottom = r.y + 8 + h;
    }
    g_popupHeight = std::min(r.h, bottom - r.y + 12);
    return action;
}

// A LocalizedString's text (strings.cpp), kept once found.
const std::string& found(const std::string& json) {
    static const std::string kNothing;
    if (json.empty()) return kNothing;
    auto it = g_found.find(json);
    if (it != g_found.end()) return it->second;
    if (!strings::ready()) return kNothing;
    if (g_found.size() > 2000) g_found.clear();
    return g_found[json] = strings::resolve(json);
}

// The tooltip of the need or moodlet pointed at, as the game's: its name,
// what it means, why you have it (grey) and the time it has left, on the
// dialog panel. False with no texts for it.
bool drawTip(const Tip& tip) {
    const std::string &title = found(tip.json[0]), &text = found(tip.json[1]), &reason = found(tip.json[2]);
    std::string left;
    if (tip.minutes > 0) {
        char buffer[48];
        if (tip.minutes >= 120)
            std::snprintf(buffer, sizeof buffer, "%d hours left", (tip.minutes + 30) / 60);
        else if (tip.minutes >= 60)
            std::snprintf(buffer, sizeof buffer, "1 hour left");
        else
            std::snprintf(buffer, sizeof buffer, "%d minutes left", std::max(1, tip.minutes));
        left = buffer;
    }
    if (title.empty() && text.empty()) return false;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kTip;
    uikit::TipLine lines[4];
    uikit::tipLines(lines, title, text, reason, left);
    const ImVec2 size = uikit::tooltip(d, r.x + r.w / 2, r.y + 8, lines, 4, r.w - 16 - 2 * 34);
    const float h = size.y;
    g_tipHeight = std::min(r.h, h + 16);
    return true;
}

// A record's fields (0x1f between them).
std::vector<std::string> fieldsOf(const std::string& record) {
    std::vector<std::string> f;
    for (size_t k = 0;;) {
        size_t e = record.find('\x1f', k);
        f.push_back(record.substr(k, e == std::string::npos ? std::string::npos : e - k));
        if (e == std::string::npos) break;
        k = e + 1;
    }
    return f;
}

// The picker's row state with the boxes as they are (the game's
// RecipePickerRowData.is_enable / GetDiscountedPrice).
int pickerVariant() { return g_pickerFresh && g_pickerPrepped ? 3 : g_pickerFresh ? 1 : g_pickerPrepped ? 2 : 0; }
bool rowEnabled(const PickerRow& r) {
    static const unsigned kBit[4] = {1, 2, 4, 8};
    return (r.enabled & kBit[pickerVariant()]) != 0;
}
unsigned rowPrice(const PickerRow& r) {
    const int v = pickerVariant();
    if (v) return r.price[v];
    return r.discounted ? r.discounted : r.price[0];
}

// The picker's texts from the game's string tables, once they are loaded,
// and its top rows in the order of its sort column.
void resolvePicker() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (g_pickerNew) {
            changed = true;
            g_picker = g_pickerIn;
            g_pickerOn = !g_pickerIn.rows.empty();
            g_pickerNew = false;
            g_pickerHeight = 0;
            g_pickerPage = 0;
            g_pickerTab = g_pickerOpen = -1;
            // GridPicker: Use Ingredients is ticked whenever the picker has an
            // ingredients column, Use Prepped Ingredients whenever it shows.
            g_pickerFresh = (g_picker.flags & 4) != 0;
            g_pickerPrepped = (g_picker.flags & 2) != 0;
        }
    }
    if (changed) {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_pickerIcons.clear();  // a new picker (or none): its icons are asked for as it draws them
    }
    if (!g_pickerOn || g_picker.resolved || !strings::ready()) return;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (g_pickerNew) return;  // a newer one came meanwhile
    }
    PickerContent& p = g_picker;
    p.resolved = true;
    if (!p.titleJson.empty()) p.title = strings::resolve(p.titleJson);
    for (PickerFilter& f : p.filters)
        if (!f.json.empty()) f.text = strings::resolve(f.json);
    int tops = 0, named = 0;
    const int n = static_cast<int>(p.rows.size());
    for (int i = 0; i < n; ++i) {
        PickerRow& r = p.rows[i];
        if (!r.nameJson.empty()) r.name = strings::resolve(r.nameJson);
        if (!r.servingJson.empty()) r.serving = strings::resolve(r.servingJson);
        r.servings.erase(std::remove_if(r.servings.begin(), r.servings.end(), [n](int k) { return k < 0 || k >= n; }),
                         r.servings.end());
        if (!r.top) continue;
        ++tops;
        if (!r.name.empty()) ++named;
        p.order.push_back(i);
        p.mostServings = std::max(p.mostServings, r.servings.size());
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (p.sort == "name")
        std::stable_sort(p.order.begin(), p.order.end(),
                         [&](int a, int b) { return lower(p.rows[a].name) < lower(p.rows[b].name); });
    else if (p.sort == "price")
        std::stable_sort(p.order.begin(), p.order.end(), [&](int a, int b) { return p.rows[a].price[0] < p.rows[b].price[0]; });
    else if (p.sort == "skill_level")
        std::stable_sort(p.order.begin(), p.order.end(), [&](int a, int b) { return p.rows[a].skill < p.rows[b].skill; });
    // Every row's name must be there: a missing one keeps the cut of the game's picker.
    p.valid = tops > 0 && named == tops;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (!g_pickerNew) g_pickerState = p.valid ? 2 : 3;
    }
    static int logged = 0;
    if (logged++ < 40)
        LOG("HUD: picker texts %s (%d rows, %d in the list, %d named, %zu filters, sorted by %s, boxes %d)",
            p.valid ? "found" : "missing: the game's picker is shown cut out", n, tops, named, p.filters.size(),
            p.sort.empty() ? "the message's order" : p.sort.c_str(), p.flags);
}

// The price as the game shows it ("§45"); a changed one (ingredients, a
// discount) the old one struck through, the new one green (cheaper) or red.
void pickerPrice(ImDrawList* d, const PickerRow& r, ImVec2 rightTop, float size) {
    const unsigned now = rowPrice(r), was = r.price[0];
    if (!now && !was) return;
    char text[32], old[32];
    std::snprintf(text, sizeof text, "\xc2\xa7%u", now);
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, 0, text);
    const ImU32 colour = now == was ? IM_COL32(44, 52, 66, 255) : now < was ? IM_COL32(40, 140, 60, 255) : IM_COL32(190, 60, 50, 255);
    d->AddText(g_text, size, {rightTop.x - ts.x, rightTop.y}, colour, text);
    if (now == was) return;
    std::snprintf(old, sizeof old, "\xc2\xa7%u", was);
    ImVec2 os = g_text->CalcTextSizeA(size * 0.85f, FLT_MAX, 0, old);
    ImVec2 at{rightTop.x - ts.x - 12 - os.x, rightTop.y + size * 0.1f};
    const ImU32 grey = IM_COL32(130, 136, 146, 255);
    d->AddText(g_text, size * 0.85f, at, grey, old);
    d->AddLine({at.x - 2, at.y + os.y * 0.55f}, {at.x + os.x + 2, at.y + os.y * 0.55f}, grey, 2.5f);
}

// The game's recipe picker (GridPicker) as a card in the game's dialog art:
// the title and the close X, the filter tabs, the top rows two by two (icon,
// name, price; greyed when they can't be made now), the page arrows and the
// Use Ingredients boxes. A row with servings (the game's linked rows) opens
// them; a row or serving picks it. Returns kPick (index: row * 4 + the boxes)
// or kPickerClose; its height goes to pickerHeight().
Action drawPicker(int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const PickerContent& p = g_picker;
    const Rect r = kPicker;
    const ImU32 kTitle = IM_COL32(29, 58, 102, 255), kBody = IM_COL32(44, 52, 66, 255), kGrey = IM_COL32(130, 136, 146, 255);
    const float x0 = r.x + 8, x1 = r.x + r.w - 8, top = r.y + 8, pad = 34;
    const float in0 = x0 + pad, in1 = x1 - pad;
    Action action = kNone;
    int pressedRow = -1;
    std::vector<uint64_t> shown;
    int id = 0;
    auto button = [&](ImVec2 a, ImVec2 b, bool* hover) {
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(2000 + id++);
        bool pressed = ImGui::InvisibleButton("pick", {std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)});
        if (hover) *hover = ImGui::IsItemHovered();
        ImGui::PopID();
        return pressed;
    };
    auto boxes = [&]() {
        return ((p.flags & 1) && g_pickerFresh ? 1 : 0) + ((p.flags & 2) && g_pickerPrepped ? 2 : 0);
    };
    auto anyEnabled = [&](const PickerRow& row) {
        if (rowEnabled(row)) return true;
        for (int k : row.servings)
            if (rowEnabled(p.rows[k])) return true;
        return false;
    };

    // Its height first (the panel goes under everything): the same for every
    // page, tab and opened row, so the card keeps its size; as many lines of
    // rows a page as fit in kPicker (up to 6).
    const float titleSize = g_title->FontSize, textSize = g_text->FontSize * 0.8f;
    ImVec2 tt = p.title.empty() ? ImVec2{0, titleSize} : g_title->CalcTextSizeA(titleSize, FLT_MAX, in1 - in0 - 140, p.title.c_str());
    const float tabW = 96, tabH = 80, tabGap = 10;
    const int tabsPerLine = std::max(1, static_cast<int>((in1 - in0 + tabGap) / (tabW + tabGap)));
    const int tabCount = p.filters.empty() ? 0 : static_cast<int>(p.filters.size()) + 1;
    const int tabLines = (tabCount + tabsPerLine - 1) / tabsPerLine;
    const bool freshBox = (p.flags & 1) != 0, preppedBox = (p.flags & 2) != 0;
    const float boxLine = 56, arrowsH = 76;
    const float boxesH = ((freshBox ? 1 : 0) + (preppedBox ? 1 : 0)) * boxLine;
    const int kCols = 2;
    const float cellGap = 10, cellH = 112, cellW = (in1 - in0 - cellGap * (kCols - 1)) / kCols;
    const float fixedH = 30 + tt.y + 18 + (tabLines ? tabLines * (tabH + tabGap) + 8 : 0) + 22 +
                         std::max(arrowsH, boxesH) + 30 + 16;
    const int rowsPerPage = std::max(2, std::min(6, static_cast<int>((r.h - fixedH + cellGap) / (cellH + cellGap))));
    const int perPage = kCols * rowsPerPage;
    const int totalTop = static_cast<int>(p.order.size());
    const int gridLines = std::max(1, std::min(rowsPerPage, (totalTop + kCols - 1) / kCols));
    const float gridH = gridLines * (cellH + cellGap) - cellGap;
    const float servH = 76, servGap = 12;
    const float openH = p.mostServings ? cellH + 16 + p.mostServings * (servH + servGap) + servH : 0;
    const float contentH = std::max(gridH, openH);
    const bool paged = totalTop > perPage;  // (all rows: the card keeps its size whatever the tab)
    const float footerH = std::max(paged ? arrowsH : 0.0f, boxesH);

    // The rows of the chosen tab, and the page of them.
    std::vector<int> list;
    for (int i : p.order) {
        const PickerRow& row = p.rows[i];
        if (g_pickerTab >= 0 && g_pickerTab < static_cast<int>(p.filters.size()) &&
            std::find(row.tags.begin(), row.tags.end(), p.filters[g_pickerTab].tag) == row.tags.end())
            continue;
        list.push_back(i);
    }
    const int pages = std::max(1, (static_cast<int>(list.size()) + perPage - 1) / perPage);
    g_pickerPage = std::max(0, std::min(g_pickerPage, pages - 1));
    if (g_pickerOpen >= static_cast<int>(p.rows.size())) g_pickerOpen = -1;

    float y = top + 30;
    const float titleY = y;
    y += tt.y + 18;
    const float tabsY = y;
    if (tabLines) y += tabLines * (tabH + tabGap) + 8;
    const float gridY = y;
    y += contentH;
    const float footerY = y + (footerH > 0 ? 22 : 0);
    const float bottom = footerY + footerH + 30;
    nine(d, art::kDialogPanel, {x0, top}, {x1, bottom}, 36, 36, 36, 36, 1, IM_COL32(244, 247, 252, 255));

    // The title, and the close X in the corner (the game's Cancel).
    if (!p.title.empty())
        d->AddText(g_title, titleSize, {(in0 + in1 - tt.x) / 2, titleY}, kTitle, p.title.c_str(), nullptr, in1 - in0 - 140);
    {
        const float s = 76;
        ImVec2 c{in1 - s / 2 + 8, titleY + titleSize / 2};
        bool hover = false;
        if (button({c.x - s / 2, c.y - s / 2}, {c.x + s / 2, c.y + s / 2}, &hover)) action = kPickerClose;
        piece(d, art::kCancel, c, (hover ? s * 1.08f : s) / art::kCancel.w);
    }

    // The filter tabs: All, then the picker's categories (their icons).
    for (int t = 0; t < tabCount; ++t) {
        const int tab = t - 1;
        const float tx = in0 + (t % tabsPerLine) * (tabW + tabGap), ty = tabsY + (t / tabsPerLine) * (tabH + tabGap);
        bool hover = false;
        if (button({tx, ty}, {tx + tabW, ty + tabH}, &hover)) {
            g_pickerTab = tab;
            g_pickerPage = 0;
            g_pickerOpen = -1;
        }
        const bool on = g_pickerTab == tab;
        nine(d, art::kDialogPanel, {tx, ty}, {tx + tabW, ty + tabH}, 36, 36, 36, 36, 0.4f,
             on ? IM_COL32(150, 196, 240, 255) : hover ? IM_COL32(218, 232, 250, 255) : IM_COL32(228, 233, 241, 255));
        const PickerFilter* f = tab >= 0 ? &p.filters[tab] : nullptr;
        if (f && f->icon) {
            icon(d, f->icon, {tx + tabW / 2, ty + tabH / 2}, 60);
            shown.push_back(f->icon);
        } else {
            const char* label = f ? f->text.c_str() : "All";
            const float size = g_small->FontSize * 0.7f;
            ImVec2 ls = g_small->CalcTextSizeA(size, FLT_MAX, tabW - 8, label);
            d->AddText(g_small, size, {tx + (tabW - ls.x) / 2, ty + (tabH - ls.y) / 2}, kTitle, label, nullptr, tabW - 8);
        }
    }

    // A row's cell: its icon, name and price.
    auto cell = [&](const PickerRow& row, ImVec2 a, ImVec2 b, bool enabled, bool hover, bool more) {
        nine(d, art::kDialogPanel, a, b, 36, 36, 36, 36, 0.5f,
             !enabled ? IM_COL32(234, 236, 240, 255) : hover ? IM_COL32(214, 230, 250, 255) : kWhite);
        const float is = b.y - a.y - 24;
        if (row.icon) {
            icon(d, row.icon, {a.x + 14 + is / 2, (a.y + b.y) / 2}, is, enabled ? kWhite : IM_COL32(255, 255, 255, 110));
            shown.push_back(row.icon);
        }
        const float tx = a.x + 14 + is + 14, wrap = b.x - tx - (more ? 34 : 14);
        const ImVec4 clip{tx, a.y + 6, b.x - 8, b.y - 6};
        d->AddText(g_text, textSize, {tx, a.y + 12}, enabled ? kBody : kGrey, row.name.c_str(), nullptr, wrap, &clip);
        if (p.flags & 8) pickerPrice(d, row, {b.x - (more ? 34 : 16), b.y - 14 - textSize}, textSize);
        if (more) {
            // It opens its servings: the game's expand arrow.
            const float mx = b.x - 22, my = (a.y + b.y) / 2;
            d->AddTriangleFilled({mx - 7, my - 12}, {mx - 7, my + 12}, {mx + 7, my}, enabled ? kTitle : kGrey);
        }
    };

    if (g_pickerOpen < 0) {
        // The page of rows, two by two.
        const int first = g_pickerPage * perPage;
        for (int k = first; k < std::min(first + perPage, static_cast<int>(list.size())); ++k) {
            const int i = list[k], slot = k - first;
            const PickerRow& row = p.rows[i];
            const float cx = in0 + (slot % kCols) * (cellW + cellGap), cy = gridY + (slot / kCols) * (cellH + cellGap);
            const bool enabled = anyEnabled(row), more = !row.servings.empty();
            bool hover = false;
            if (button({cx, cy}, {cx + cellW, cy + cellH}, &hover) && enabled) {
                if (more) {
                    g_pickerOpen = i;
                } else {
                    pressedRow = i;
                }
            }
            cell(row, {cx, cy}, {cx + cellW, cy + cellH}, enabled, hover && enabled, more);
        }
        if (list.empty()) {
            const char* none = "Nothing here";
            ImVec2 ns = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, none);
            d->AddText(g_text, textSize, {(in0 + in1 - ns.x) / 2, gridY + 20}, kGrey, none);
        }
    } else {
        // The opened row: its servings, each picked as the game's linked rows are, and Back.
        const PickerRow& row = p.rows[g_pickerOpen];
        cell(row, {in0, gridY}, {in1, gridY + cellH}, anyEnabled(row), false, false);
        float sy = gridY + cellH + 16;
        for (int k : row.servings) {
            const PickerRow& s = p.rows[k];
            const bool enabled = rowEnabled(s);
            const ImVec2 a{in0 + 40, sy}, b{in1 - 40, sy + servH};
            bool hover = false;
            if (button(a, b, &hover) && enabled) pressedRow = k;
            if (hover && enabled)
                nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.8f);
            else
                nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.8f, enabled ? kWhite : IM_COL32(255, 255, 255, 120));
            const std::string& label = s.serving.empty() ? s.name : s.serving;
            const ImVec4 clip{a.x + 30, a.y, b.x - 150, b.y};
            ImVec2 ls = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, label.c_str());
            d->AddText(g_text, textSize, {a.x + 36, a.y + (b.y - a.y - 8 - ls.y) / 2},
                       enabled ? IM_COL32(30, 70, 128, 255) : kGrey, label.c_str(), nullptr, 0, &clip);
            if (p.flags & 8) pickerPrice(d, s, {b.x - 36, a.y + (b.y - a.y - 8 - textSize) / 2}, textSize);
            sy += servH + servGap;
        }
        if (dialogButton(d, 60, "Back", {in0 + 40, sy}, {in0 + 300, sy + servH})) g_pickerOpen = -1;
    }

    // The footer: the ingredient boxes (left) and the page arrows (right).
    float by = footerY;
    auto box = [&](const char* label, bool& on) {
        const float s = 40;
        ImVec2 a{in0, by + (boxLine - s) / 2}, b{in0 + s, by + (boxLine + s) / 2};
        ImVec2 ls = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, label);
        bool hover = false;
        if (button({a.x - 6, by}, {b.x + 16 + ls.x + 6, by + boxLine}, &hover)) on = !on;
        d->AddRectFilled(a, b, hover ? IM_COL32(218, 232, 250, 255) : kWhite, 6);
        d->AddRect(a, b, kTitle, 6, 0, 3);
        if (on) {
            const ImVec2 tick[3] = {{a.x + 8, a.y + s * 0.52f}, {a.x + s * 0.42f, b.y - 9}, {b.x - 7, a.y + 9}};
            d->AddPolyline(tick, 3, kTitle, 0, 5);
        }
        d->AddText(g_text, textSize, {b.x + 16, by + (boxLine - ls.y) / 2}, kBody, label);
        by += boxLine;
    };
    if (freshBox) box("Use Ingredients", g_pickerFresh);
    if (preppedBox) box("Use Prepped Ingredients", g_pickerPrepped);
    if (pages > 1 && g_pickerOpen < 0) {
        const float bw = 104, bh = 76;
        char where[32];
        std::snprintf(where, sizeof where, "%d / %d", g_pickerPage + 1, pages);
        ImVec2 ws = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, where);
        const float right = in1, nx = right - bw, px = nx - 24 - ws.x - 24 - bw;
        if (dialogButton(d, 61, "<", {px, footerY}, {px + bw, footerY + bh})) g_pickerPage = (g_pickerPage + pages - 1) % pages;
        d->AddText(g_text, textSize, {px + bw + 24, footerY + (bh - 10 - ws.y) / 2}, kBody, where);
        if (dialogButton(d, 62, ">", {nx, footerY}, {nx + bw, footerY + bh})) g_pickerPage = (g_pickerPage + 1) % pages;
    }

    if (pressedRow >= 0) {
        action = kPick;
        if (index) *index = pressedRow * 4 + boxes();
    }
    std::sort(shown.begin(), shown.end());
    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (shown != g_pickerIcons) g_pickerIcons.swap(shown);
    }
    g_pickerHeight = std::min(r.h, bottom - r.y + 8);
    return action;
}

// The round build/buy button: the game's GameTray icon on a light disc.
// The phone's texts from the game's string tables, then its entries sorted as
// the game sorts them (InteractionMenuData.OptionsSortingFunction: higher
// priority first, then by name; a category's list again by priority,
// PhoneInteractionScreen.PopulateItems).
void resolvePhone() {
    {
        std::lock_guard<std::mutex> lock(g_notesMutex);
        if (g_notesNew) {
            g_notes = g_notesIn;
            g_notesNew = false;
            std::vector<uint64_t> pictures;
            for (const Note& n : g_notes.notes)
                if (n.picture && pictures.size() < 24) pictures.push_back(n.picture);
            std::lock_guard<std::mutex> items(g_itemMutex);
            g_notesPreload.swap(pictures);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_calMutex);
        if (g_calNew) {
            g_cal = g_calIn;
            g_calNew = false;
            g_phoneDataUntil = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_simMutex);
        if (g_simNew) {
            g_sim = g_simIn;
            g_simNew = false;
            g_phoneDataUntil = 0;
            if (g_contacts >= 1 && g_sim.contact.index >= 0) g_contactSel = g_sim.contact.index;  // the list re-sorts
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_phoneMutex);
        if (g_phoneNew) {
            // A node's place in the tree (the names above it and its own): the screen you are on is
            // found again in a fresh menu, whose entries may be numbered differently.
            auto place = [](const PhoneContent& c, int node) {
                std::string key;
                for (int n = node; n >= 0 && n < static_cast<int>(c.nodes.size()); n = c.nodes[n].parent)
                    key = c.nodes[n].nameJson + '\x1d' + key;
                return key;
            };
            auto find = [](const PhoneContent& c, const std::string& key, auto& placeOf) {
                if (key.empty()) return -1;
                for (int n = 0; n < static_cast<int>(c.nodes.size()); ++n)
                    if (c.nodes[n].category && placeOf(c, n) == key) return n;
                return -1;
            };
            const std::string app = g_phoneApp >= 0 ? place(g_phone, g_phoneApp) : std::string(),
                              root = g_phoneRoot >= 0 ? place(g_phone, g_phoneRoot) : std::string();
            bool same = g_phone.nodes.size() == g_phoneIn.nodes.size() && g_phone.home == g_phoneIn.home;
            for (size_t i = 0; same && i < g_phone.nodes.size(); ++i) {
                const PhoneNode &a = g_phone.nodes[i], &b = g_phoneIn.nodes[i];
                same = a.parent == b.parent && a.icon == b.icon && a.nameJson == b.nameJson &&
                       a.disabledJson == b.disabledJson && a.notification == b.notification && a.category == b.category;
            }
            g_phone = g_phoneIn;
            g_phoneOn = !g_phoneIn.nodes.empty() || !g_phoneIn.disabledJson.empty() || g_phoneIn.valid;
            g_phoneNew = false;
            if (!same) {
                ++g_phoneSerial;
                if (g_phoneApp >= 0) {
                    g_phoneApp = find(g_phone, app, place);
                    g_phoneRoot = find(g_phone, root, place);
                    if (g_phoneApp < 0 || g_phoneRoot < 0) g_phoneApp = g_phoneRoot = -1, g_phoneScroll = 0;
                }
            }
            const int n = static_cast<int>(g_phone.nodes.size());
            if (g_phoneApp >= n || g_phoneRoot >= n) g_phoneApp = g_phoneRoot = -1;
            // Its pictures read now, while it is still in your pocket.
            std::vector<uint64_t> preload;
            for (const PhoneNode& node : g_phone.nodes)
                if (node.icon) preload.push_back(node.icon);
            preload.insert(preload.end(), g_phone.colors.begin(), g_phone.colors.end());
            std::lock_guard<std::mutex> items(g_itemMutex);
            g_phonePreload.swap(preload);
        }
    }
    if (!g_phoneOn || g_phone.resolved || !strings::ready()) return;
    PhoneContent& p = g_phone;
    if (!p.disabledJson.empty()) p.disabled = strings::resolve(p.disabledJson);
    for (PhoneNode& node : p.nodes) {
        if (!node.nameJson.empty()) node.name = strings::resolve(node.nameJson);
        if (!node.disabledJson.empty()) node.disabled = strings::resolve(node.disabledJson);
    }
    // A category is badged when any entry in it is (InteractionCategory.ShouldDisplayNotification).
    for (int i = static_cast<int>(p.nodes.size()) - 1; i >= 0; --i)
        if (p.nodes[i].notification && p.nodes[i].parent >= 0) p.nodes[p.nodes[i].parent].notification = true;
    auto byPriorityThenName = [&](int a, int b) {
        const PhoneNode &x = p.nodes[a], &y = p.nodes[b];
        if (x.priority != y.priority) return x.priority > y.priority;
        return x.name < y.name;
    };
    std::stable_sort(p.home.begin(), p.home.end(), byPriorityThenName);
    for (PhoneNode& node : p.nodes) {
        std::stable_sort(node.children.begin(), node.children.end(), byPriorityThenName);
        std::stable_sort(node.children.begin(), node.children.end(),
                         [&](int a, int b) { return p.nodes[a].priority < p.nodes[b].priority; });
    }
    p.resolved = true;
}

// A notification's background as the game's (NotificationView.SetBackgroundFrame:
// standard blue, player green, critical orange, special moment purple; the
// "_strip" frames when it has a secondary icon), corners kept, its gradient stretched.
void noteBackground(ImDrawList* d, int style, ImVec2 a, ImVec2 b, float k) {
    static const art::Piece* kFrames[8] = {&art::kNoteStandard, &art::kNotePlayer, &art::kNoteCritical,
                                           &art::kNoteMomentStrip, &art::kNoteStandardStrip, &art::kNotePlayerStrip,
                                           &art::kNoteCriticalStrip, &art::kNoteMomentStrip};
    nine(d, *kFrames[style & 7], a, b, 18, 18, 18, 18, k);
}

// The calendar's art per entry kind (CalendarDayView.GetLabelForType: its
// main picture's background and its activity icon; vacations and stayovers
// have no main picture of their own).
const art::Piece* calendarBackground(int kind) {
    switch (kind) {
        case 1: case 5: case 9: return &art::kCalBgEvent;
        case 2: case 3: return &art::kCalBgHoliday;
        case 6: return &art::kCalBgFestival;
        case 7: return &art::kCalBgWedding;
        case 8: case 21: return &art::kCalBgWork;
        case 10: return &art::kCalBgFuneral;
        case 11: return &art::kCalBgGetaway;
        case 20: return &art::kCalBgBirthday;
        default: return nullptr;
    }
}
const art::Piece* calendarIcon(int kind) {
    switch (kind) {
        case 1: return &art::kCalIconEvent;
        case 2: case 3: return &art::kCalIconHoliday;
        case 5: return &art::kCalIconVacation;
        case 6: return &art::kCalIconFestival;
        case 7: return &art::kCalIconWedding;
        case 8: case 21: return &art::kCalIconWork;
        case 9: return &art::kCalIconStayover;
        case 10: return &art::kCalIconFuneral;
        case 11: return &art::kCalIconGetaway;
        case 20: return &art::kCalIconBirthday;
        default: return nullptr;
    }
}

// The calendar app (the game's Calendar widget on the phone's narrow screen):
// the family's title; a week of day cells (CalendarDayView: the season's
// background, the weather forecast and moon phase, the main entry's picture
// on its kind's background, the other activities' icons; past days dimmed,
// today outlined, the selected day in the green frame) with arrows through
// the year's weeks; under it the selected day's schedule (SchedulePane: the
// weekday, the forecast, each entry with its picture, name and time).
template <class Button>
void drawCalendar(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action) {
    constexpr float S = 2.5f;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const CalendarContent& c = g_cal;
    static const char* kShort[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char* kLong[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
    {
        std::string title = found(c.titleJson);
        if (title.empty()) title = "Calendar";
        ImVec2 ts = g_title->CalcTextSizeA(34, FLT_MAX, 0, title.c_str());
        const float k = std::min(1.0f, 210 * S / std::max(1.0f, ts.x));
        shadowText(d, g_title, 34 * k, {at(115, 22).x - ts.x * k / 2, at(115, 22).y - ts.y * k / 2}, kWhite, title.c_str());
    }
    if (!c.valid || c.count <= 0) {
        const char* wait = "...";
        ImVec2 ws = g_text->CalcTextSizeA(30, FLT_MAX, 0, wait);
        shadowText(d, g_text, 30, {at(115, 180).x - ws.x / 2, at(115, 180).y - ws.y / 2}, kWhite, wait);
        return;
    }
    const int weeks = (c.count + 6) / 7;
    if (g_calSel < 0 || g_calSel >= c.count) g_calSel = c.today, g_calWeek = c.today / 7, g_calScroll = 0;
    {
        // Swipes: sideways turns the week, up and down scrolls the day's schedule.
        const PhoneDrag& drag = g_drag;
        const ImGuiIO& io = ImGui::GetIO();
        const float dx = (io.MousePos.x - drag.start.x) / S, dy = (io.MousePos.y - drag.start.y) / S;
        if (drag.on && drag.moved && drag.horizontal && !io.MouseDown[0]) {
            if (dx < -40) g_calWeek = drag.week + 1;
            if (dx > 40) g_calWeek = drag.week - 1;
        }
        if (drag.on && drag.moved && !drag.horizontal && drag.startY > 120)
            g_calScroll = drag.cal - static_cast<int>(std::lround(dy / 37));
    }
    g_calWeek = std::max(0, std::min(weeks - 1, g_calWeek));
    for (int col = 0; col < 7; ++col) {
        ImVec2 ns = g_small->CalcTextSizeA(20, FLT_MAX, 0, kShort[col]);
        const ImVec2 m = at(4 + col * 32 + 15, 51);
        shadowText(d, g_small, 20, {m.x - ns.x / 2, m.y - ns.y / 2}, kWhite, kShort[col]);
    }
    for (int col = 0; col < 7; ++col) {
        const int i = g_calWeek * 7 + col;
        if (i >= c.count) break;
        const CalDay& day = c.days[i];
        const float x = 4 + col * 32, y = 58, w = 30, h = 41;
        const ImVec2 a = at(x, y), b = at(x + w, y + h);
        const art::Piece& season = day.season == 0   ? art::kCalSummer
                                   : day.season == 1 ? art::kCalFall
                                   : day.season == 2 ? art::kCalWinter
                                                     : art::kCalSpring;
        pieceIn(d, season, a, b);
        if (const art::Piece* bg = calendarBackground(day.main)) {
            const ImVec2 ma = at(x + 1.5f, y + 11), mb = at(x + w - 1.5f, y + 32);
            pieceIn(d, *bg, ma, mb);
            if (day.mainPicture) {
                shown.push_back(day.mainPicture);
                icon(d, day.mainPicture, {(ma.x + mb.x) / 2, (ma.y + mb.y) / 2}, 19 * S);
            }
            if (const art::Piece* ic = calendarIcon(day.main)) pieceIn(d, *ic, at(x + 1, y + 9), at(x + 9, y + 17));
        }
        if (day.weather) {
            shown.push_back(day.weather);
            icon(d, day.weather, at(x + 6, y + 6), 10 * S);
        }
        if (day.moon >= 0 && day.moon < 8) {
            static const art::Piece* kMoons[8] = {&art::kCalMoon0, &art::kCalMoon1, &art::kCalMoon2, &art::kCalMoon3,
                                                  &art::kCalMoon4, &art::kCalMoon5, &art::kCalMoon6, &art::kCalMoon7};
            pieceIn(d, *kMoons[day.moon], at(x + w - 9, y + 1.5f), at(x + w - 1.5f, y + 9));
        }
        float ix = x + w - 1.5f;
        for (int kind : day.small) {
            const ImVec2 ia = at(ix - 8, y + h - 9), ib = at(ix, y + h - 1);
            if (const art::Piece* ic = calendarIcon(kind)) {
                pieceIn(d, *ic, ia, ib);
            } else {  // more
                ImVec2 ms = g_small->CalcTextSizeA(18, FLT_MAX, 0, "+");
                shadowText(d, g_small, 18, {(ia.x + ib.x - ms.x) / 2, (ia.y + ib.y - ms.y) / 2}, kWhite, "+");
            }
            ix -= 9;
        }
        if (i < c.today) pieceIn(d, art::kCalDayPast, a, b, IM_COL32(255, 255, 255, 170));
        if (i == c.today) d->AddRect(a, b, IM_COL32(255, 255, 255, 230), 2 * S, 0, 1.2f * S);
        bool over = false;
        if (i >= c.today && button(a, b, &over)) g_calSel = i, g_calScroll = 0;
        if (i == g_calSel) {
            pieceIn(d, art::kCalDaySelected, a, b);
        } else if (over) {
            d->AddRectFilled(a, b, IM_COL32(255, 255, 255, 40), 2 * S);
        }
    }
    // The weeks: back and on.
    {
        auto arrow = [&](float cx, bool left, bool enabled) {
            bool over = false;
            const bool pressed = button(at(cx - 12, 102), at(cx + 12, 120), &over) && enabled;
            const ImU32 ink = !enabled ? IM_COL32(255, 255, 255, 60) : over ? IM_COL32(186, 240, 120, 255) : kWhite;
            const ImVec2 m = at(cx, 111);
            const float u = 5 * S;
            if (left)
                d->AddTriangleFilled({m.x - u, m.y}, {m.x + u * 0.7f, m.y - u}, {m.x + u * 0.7f, m.y + u}, ink);
            else
                d->AddTriangleFilled({m.x + u, m.y}, {m.x - u * 0.7f, m.y - u}, {m.x - u * 0.7f, m.y + u}, ink);
            return pressed;
        };
        if (arrow(18, true, g_calWeek > 0)) --g_calWeek;
        if (arrow(212, false, g_calWeek + 1 < weeks)) ++g_calWeek;
        char label[32];
        const int ahead = g_calWeek - c.today / 7;
        if (ahead == 0) std::snprintf(label, sizeof label, "This week");
        else if (ahead == 1) std::snprintf(label, sizeof label, "Next week");
        else std::snprintf(label, sizeof label, "In %d weeks", ahead);
        ImVec2 ls = g_small->CalcTextSizeA(24, FLT_MAX, 0, label);
        shadowText(d, g_small, 24, {at(115, 111).x - ls.x / 2, at(115, 111).y - ls.y / 2}, kWhite, label);
    }
    // The selected day's schedule.
    const CalDay& sel = c.days[g_calSel];
    pieceIn(d, art::kPhoneTint, at(0, 123), at(230, 149));
    {
        std::string head = kLong[g_calSel % 7];
        if (g_calSel == c.today) head += "  (Today)";
        ImVec2 hs = g_title->CalcTextSizeA(28, FLT_MAX, 0, head.c_str());
        shadowText(d, g_title, 28, {at(8, 136).x, at(8, 136).y - hs.y / 2}, kWhite, head.c_str());
        if (sel.weather) {
            const std::string& fname = found(sel.forecastJson);
            ImVec2 fs = g_text->CalcTextSizeA(22, FLT_MAX, 0, fname.c_str());
            const float room = 222 * S - (at(8, 0).x - r.x) - hs.x - 30 * S;
            const float k = fs.x > 0 ? std::min(1.0f, std::max(0.5f, room / fs.x)) : 1;
            const float right = at(224, 0).x;
            shadowText(d, g_text, 22 * k, {right - fs.x * k, at(0, 136).y - fs.y * k / 2}, kWhite, fname.c_str());
            shown.push_back(sel.weather);
            icon(d, sel.weather, {right - fs.x * k - 10 * S, at(0, 136).y}, 16 * S);
        }
    }
    std::vector<const CalItem*> list;
    for (const CalItem& it : c.items)
        if (it.day == g_calSel) list.push_back(&it);
    const float top = 152, bottom = 356, left = 6, right = 210, rowH = 34;
    const int fit = static_cast<int>((bottom - top) / (rowH + 3)), total = static_cast<int>(list.size());
    g_calScroll = std::max(0, std::min(std::max(0, total - fit), g_calScroll));
    const ImU32 kName = IM_COL32(29, 58, 102, 255), kTime = IM_COL32(80, 92, 110, 255);
    for (int k = 0; k < fit && g_calScroll + k < total; ++k) {
        const CalItem& it = *list[g_calScroll + k];
        const float y = top + k * (rowH + 3);
        nine(d, art::kDialogPanel, at(left, y), at(right, y + rowH), 36, 36, 36, 36, 0.5f, IM_COL32(246, 249, 253, 250));
        const ImVec2 pa = at(left + 3, y + 3), pb = at(left + 3 + 36, y + rowH - 3);
        if (const art::Piece* bg = calendarBackground(it.kind)) pieceIn(d, *bg, pa, pb);
        if (it.picture) {
            shown.push_back(it.picture);
            icon(d, it.picture, {(pa.x + pb.x) / 2, (pa.y + pb.y) / 2}, 26 * S);
        }
        if (const art::Piece* ic = calendarIcon(it.kind)) pieceIn(d, *ic, at(right - 15, y + 3), at(right - 3, y + 15));
        const float tx = at(left + 44, 0).x, wrap = (right - left - 44 - 18) * S;
        const std::string &name = found(it.nameJson), &time = found(it.timeJson);
        ImVec2 ns = g_title->CalcTextSizeA(24, FLT_MAX, 0, name.c_str());
        const float nk = ns.x > wrap ? std::max(0.6f, wrap / ns.x) : 1;
        d->PushClipRect(at(left, y), at(right - 16, y + rowH), true);
        d->AddText(g_title, 24 * nk, {tx, at(0, y + 5).y}, kName, name.c_str());
        if (!time.empty()) d->AddText(g_text, 21, {tx, at(0, y + 19).y}, kTime, time.c_str());
        d->PopClipRect();
    }
    if (list.empty()) {
        std::string none = found(c.emptyJson);
        if (none.empty()) none = "Nothing scheduled.";
        const float wrap = 200 * S;
        ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, wrap, none.c_str());
        const ImVec2 m = at(115, 200);
        d->AddText(g_text, 26, {m.x - ns.x / 2 + 2, m.y - ns.y / 2 + 2}, kShadow, none.c_str(), nullptr, wrap);
        d->AddText(g_text, 26, {m.x - ns.x / 2, m.y - ns.y / 2}, kWhite, none.c_str(), nullptr, wrap);
    }
    if (total > fit) {
        const float trackH = bottom - top, thumbH = std::max(24.0f, trackH * fit / total);
        const float thumbY = top + (trackH - thumbH) * g_calScroll / std::max(1, total - fit);
        d->AddRectFilled(at(218, top), at(222, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
        pieceIn(d, art::kPhoneSlider, at(214, thumbY), at(226, thumbY + thumbH));
        const float t = sliderFollow(r, top, bottom, thumbH);
        if (t >= 0) g_calScroll = static_cast<int>(std::lround(t * (total - fit)));
    }
    (void)action;
}

// A phone list's slider held (user 2026-10-24 00:22Z: the sliders didn't work): a press that began on
// its column (right of the list) moves the thumb to the pointer while held; where along the track
// (0..1), or -1 when it isn't held.
float sliderFollow(const Rect& r, float top, float bottom, float thumbH) {
    constexpr float S = 2.5f;
    const ImGuiIO& io = ImGui::GetIO();
    if (!g_drag.on || !io.MouseDown[0] || g_cover.blocked) return -1;
    const float sx = (g_drag.start.x - r.x) / S, sy = (g_drag.start.y - r.y) / S;
    if (sx < 203 || sy < top - 6 || sy > bottom + 6) return -1;
    const float t = ((io.MousePos.y - r.y) / S - top - thumbH / 2) / std::max(1.0f, bottom - top - thumbH);
    return std::max(0.0f, std::min(1.0f, t));
}

// The My Sim app's sub-apps, in its grid's order (user 2026-10-23 21:35Z); their icons are kApp*.
constexpr int kMySimApps = 11;
const char* const kMySimNames[kMySimApps] = {"Aspirations", "Career", "Wants", "Skills", "Needs", "Simology",
                                             "Inventory", "Social Groups", "Owned Businesses", "Household", "Soul's Journey"};
constexpr int kSimAspirations = 0, kSimCareer = 1, kSimWants = 2, kSimSkills = 3, kSimNeeds = 4, kSimSimology = 5, kSimInventory = 6,
              kSimClubs = 7, kSimBusinesses = 8, kSimHousehold = 9, kSimSouls = 10;
bool mySimBuilt(int k) { return k >= 0 && k <= kSimSouls; }
// sims.sim_info_types.Age's names, as the game shows them.
const char* ageName(int age) {
    switch (age) {
        case 1: return "Baby";
        case 2: return "Toddler";
        case 4: return "Child";
        case 8: return "Teen";
        case 16: return "Young Adult";
        case 32: return "Adult";
        case 64: return "Elder";
        case 128: return "Infant";
        default: return "";
    }
}
// The age's name for the Career app: School for children and teens (sims.sim_info_types.Age), none younger.
const char* careerName(int age) {
    if (age == 4 || age == 8) return "School";
    if (age != 0 && age < 4) return nullptr;
    if (age == 128) return nullptr;  // infants
    return "Career";
}

// The phone's clock: the game's time (controls.py _report_clock), in the header's middle.
void drawPhoneClock(ImDrawList* d, const Rect& r, bool home) {
    constexpr float S = 2.5f;
    const int hour = g_hour.load(), minute = g_minute.load();
    if (hour < 0) return;
    static const char* const kDays[7] = {"Sun.", "Mon.", "Tue.", "Wed.", "Thu.", "Fri.", "Sat."};
    char text[24];
    std::snprintf(text, sizeof text, "%s %d:%02d %s", kDays[(g_day.load() % 7 + 7) % 7], hour % 12 == 0 ? 12 : hour % 12, minute,
                  hour < 12 ? "AM" : "PM");
    const float size = home ? 30.0f : 19.0f, y = home ? 20.5f : 6.0f;
    ImVec2 ts = g_title->CalcTextSizeA(size, FLT_MAX, 0, text);
    shadowText(d, g_title, size, {r.x + 115 * S - ts.x / 2, r.y + y * S - ts.y / 2}, kWhite, text);
}

// My Sim: its apps (named under them, the ones still to come dimmed), Skills (the game's
// SimInfoSkillPanel: per category its title, each skill's icon, name, level and its progress
// through the level, gold once mastered, an arrow while rising) and Needs (the wrist's needs,
// hudinfo.py: each one's icon, name and bar in its state's colours).
// Inventory (user 2026-10-24 02:42Z: as the game's inventory panel, vertical): SimInfoInventoryPanel laid out
// for the phone (mark 2026-10-24 04-41-36): the search field and Collections button, Multiselect On/Off, the
// filter button and sort dropdown (New, Name, Price, Type), the grid of InventoryCellView tiles in the
// droppable background, and the sell area ("Drag here to sell"; with Multiselect on: Select All, Favorite and
// Sell, as btnMSSelectAll / btnMSFavorite / btnMSSell). Pressing a tile opens its pie menu at your hand; with
// Multiselect on it picks it. Sell asks a second press (the game asks in a dialog).
bool g_invMulti = false;
int g_invSort = 0;  // New, Name, Price, Type
std::vector<int> g_invPicked;  // rows of g_sim.inventory
ULONGLONG g_invSellAsked = 0;
template <class Button>
void drawInventory(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action, bool swipeY,
                   float dy, const PhoneDrag& drag, int* index) {
    constexpr float S = 2.5f, A = S / art::kScale;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(28, 72, 150, 255), kRed = IM_COL32(214, 40, 40, 255), kGreen = IM_COL32(60, 150, 40, 255);
    const ULONGLONG now = GetTickCount64();
    // The panel (SimInfo panels' grey), its parts in the game's greys.
    d->AddRectFilled(at(0, 0), at(230, 356), IM_COL32(212, 212, 212, 246), 6 * S);
    // Search field and Collections.
    d->AddRectFilled(at(8, 8), at(194, 26), IM_COL32(178, 180, 186, 255), 4 * S);
    {
        bool over = false;
        button(at(200, 5), at(224, 29), &over);
        pieceIn(d, art::kInvRoundButton, at(199, 4), at(225, 30));
        piece(d, art::kInvGem, at(212, 17), A * 0.95f);
    }
    // Multiselect: On / Off.
    {
        const char* label = "Multiselect:";
        ImVec2 ls = g_text->CalcTextSizeA(20, FLT_MAX, 0, label);
        d->AddText(g_text, 20, {at(184, 0).x - ls.x, at(0, 39).y - ls.y / 2}, kBlue, label);
        bool over = false;
        if (button(at(186, 32), at(224, 46), &over)) {
            g_invMulti = !g_invMulti;
            g_invPicked.clear();
            g_invSellAsked = 0;
        }
        d->AddRectFilled(at(186, 32), at(224, 46), IM_COL32(250, 250, 250, 255), 7 * S);
        d->AddRect(at(186, 32), at(224, 46), over ? kBlue : IM_COL32(190, 190, 195, 255), 7 * S, 0, 0.8f * S);
        const char* state = g_invMulti ? "On" : "Off";
        ImVec2 ss = g_title->CalcTextSizeA(20, FLT_MAX, 0, state);
        d->AddText(g_title, 20, {at(205, 0).x - ss.x / 2, at(0, 39).y - ss.y / 2}, g_invMulti ? kGreen : kRed, state);
    }
    // The grid's background, the filter button and the sort dropdown on it.
    const float gx0 = 4, gx1 = 226, gy0 = 52, gy1 = 298;
    d->AddRectFilled(at(gx0, gy0), at(gx1, gy1), IM_COL32(172, 174, 180, 255), 6 * S);
    {
        d->AddCircleFilled(at(18, 64), 9 * S, IM_COL32(250, 250, 250, 255), 24);
        d->AddCircle(at(18, 64), 9 * S, IM_COL32(150, 152, 160, 255), 24, 0.8f * S);
        // A funnel, as the filter button's.
        const ImVec2 f = at(18, 64);
        d->AddTriangleFilled({f.x - 4.5f * S, f.y - 3.5f * S}, {f.x + 4.5f * S, f.y - 3.5f * S}, {f.x, f.y + 1.5f * S}, kBlue);
        d->AddRectFilled({f.x - 1 * S, f.y}, {f.x + 1 * S, f.y + 4 * S}, kBlue);
        static const char* const kSorts[4] = {"New", "Name", "Price", "Type"};
        bool over = false;
        if (button(at(118, 56), at(222, 72), &over)) g_invSort = (g_invSort + 1) % 4;
        d->AddRectFilled(at(118, 56), at(222, 72), IM_COL32(252, 252, 252, 255), 3 * S);
        d->AddRect(at(118, 56), at(222, 72), over ? kBlue : IM_COL32(160, 162, 170, 255), 3 * S, 0, 0.8f * S);
        ImVec2 ts = g_text->CalcTextSizeA(21, FLT_MAX, 0, kSorts[g_invSort]);
        d->AddText(g_text, 21, {at(124, 0).x, at(0, 64).y - ts.y / 2}, kBlue, kSorts[g_invSort]);
        const ImVec2 t = at(213, 64);
        d->AddTriangleFilled({t.x - 4 * S, t.y - 2 * S}, {t.x + 4 * S, t.y - 2 * S}, {t.x, t.y + 3 * S}, IM_COL32(20, 70, 170, 255));
    }
    // The tiles, sorted as picked.
    const int n = static_cast<int>(g_sim.inventory.size());
    std::vector<int> rows(n);
    for (int i = 0; i < n; ++i) rows[i] = i;
    const auto& inv = g_sim.inventory;
    std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
        switch (g_invSort) {
        case 1: return found(inv[a].name) < found(inv[b].name);
        case 2: return inv[a].value * inv[a].count > inv[b].value * inv[b].count;
        case 3: return inv[a].picture < inv[b].picture;
        default: return inv[a].order > inv[b].order;  // newest first
        }
    });
    const float top = 76, bottom = gy1 - 4, cell = 52.5f, gap = 1.5f, x0 = gx0 + 4;
    const int perRow = 4;
    const float total = ((n + perRow - 1) / perRow) * (cell + gap), room = bottom - top;
    if (swipeY) g_mySimScroll = drag.sim - dy;
    g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
    d->PushClipRect(at(gx0, top), at(gx1, bottom), true);
    int hovered = -1;
    ImVec2 hoverAt{};
    for (int j = 0; j < n; ++j) {
        const int i = rows[j];
        const SimItem& it = inv[i];
        const float x = x0 + (j % perRow) * (cell + gap), y = top + (j / perRow) * (cell + gap) - g_mySimScroll;
        if (y + cell < top || y > bottom) continue;
        ImVec2 ca = at(x, y), cb = at(x + cell, y + cell);
        ImVec2 ha{ca.x, std::max(ca.y, at(0, top).y)}, hb{cb.x, std::min(cb.y, at(0, bottom).y)};
        bool over = false;
        if (hb.y - ha.y > 1 && button(ha, hb, &over)) {
            if (g_invMulti) {
                auto f = std::find(g_invPicked.begin(), g_invPicked.end(), i);
                if (f == g_invPicked.end()) g_invPicked.push_back(i);
                else g_invPicked.erase(f);
                g_invSellAsked = 0;
            } else {
                action = kInventoryPie;
                if (index) *index = i;
            }
        }
        const bool picked = std::find(g_invPicked.begin(), g_invPicked.end(), i) != g_invPicked.end();
        pieceIn(d, art::kInvCell, ca, cb);
        if (picked || over) {
            d->AddRect({ca.x + 1 * S, ca.y + 1 * S}, {cb.x - 1 * S, cb.y - 1 * S}, picked ? IM_COL32(110, 200, 60, 255) : IM_COL32(120, 190, 80, 200),
                       4 * S, 0, (picked ? 2.2f : 1.4f) * S);
        }
        if (it.picture) {
            shown.push_back(it.picture);
            icon(d, it.picture, at(x + cell / 2, y + cell / 2), (cell - 8) * S);
        }
        if (it.count > 1) {  // the stack's count, bottom right as the game's
            char cnt[12];
            std::snprintf(cnt, sizeof cnt, "%d", it.count);
            ImVec2 cs = g_title->CalcTextSizeA(20, FLT_MAX, 0, cnt);
            shadowText(d, g_title, 20, {at(x + cell - 4, 0).x - cs.x, at(0, y + cell - 4).y - cs.y}, kWhite, cnt);
        }
        if (it.favorite) piece(d, art::kInvFavorite, at(x + 8, y + 8), A * 0.8f);
        if (it.locked) piece(d, art::kInvLock, at(x + cell - 7, y + 7), A * 0.8f);
        if (over) hovered = i, hoverAt = at(x + cell / 2, y);
    }
    d->PopClipRect();
    if (total > room) {  // the scroll bar at the grid's right
        const float thumbH = std::max(18.0f, room * room / total);
        const float thumbY = top + (room - thumbH) * g_mySimScroll / std::max(1.0f, total - room);
        d->AddRectFilled(at(gx1 - 5, thumbY), at(gx1 - 2, thumbY + thumbH), IM_COL32(250, 250, 250, 255), 1.5f * S);
    }
    if (!n) {
        const char* none = g_sim.age ? "No items" : "...";
        ImVec2 ns = g_text->CalcTextSizeA(24, FLT_MAX, 0, none);
        d->AddText(g_text, 24, {at(115, 0).x - ns.x / 2, at(0, 170).y}, kBlue, none);
    }
    // The sell area.
    const float sy0 = 302, sy1 = 352;
    d->AddRectFilled(at(gx0, sy0), at(gx1, sy1), IM_COL32(192, 193, 197, 255), 6 * S);
    if (!g_invMulti) {
        piece(d, art::kInvRegister, at(26, (sy0 + sy1) / 2), A * 1.15f);
        const char* sell = "Drag here to sell";
        ImVec2 ss = g_title->CalcTextSizeA(24, FLT_MAX, 0, sell);
        d->AddText(g_title, 24, {at(124, 0).x - ss.x / 2, at(0, (sy0 + sy1) / 2).y - ss.y / 2}, kBlue, sell);
    } else {
        const float cy = (sy0 + sy1) / 2;
        bool over = false;
        if (button(at(10, cy - 10), at(80, cy + 10), &over)) {
            const bool all = static_cast<int>(g_invPicked.size()) == n;
            g_invPicked.clear();
            if (!all)
                for (int i = 0; i < n; ++i) g_invPicked.push_back(i);
        }
        d->AddRectFilled(at(10, cy - 10), at(80, cy + 10), IM_COL32(250, 250, 250, 255), 10 * S);
        d->AddRect(at(10, cy - 10), at(80, cy + 10), over ? kBlue : IM_COL32(170, 170, 176, 255), 10 * S, 0, 0.8f * S);
        const char* sel = static_cast<int>(g_invPicked.size()) == n && n ? "Deselect All" : "Select All";
        ImVec2 ts = g_text->CalcTextSizeA(19, FLT_MAX, 0, sel);
        d->AddText(g_text, 19, {at(45, 0).x - ts.x / 2, at(0, cy).y - ts.y / 2}, kBlue, sel);
        const bool any = !g_invPicked.empty();
        bool allFav = any;
        for (int i : g_invPicked) allFav = allFav && i < n && inv[i].favorite;
        bool favOver = false;
        if (button(at(132, cy - 22), at(176, cy + 22), &favOver) && any) {
            action = kInventorySell;
            if (index) *index = allFav ? 2 : 1;
        }
        pieceIn(d, favOver && any ? art::kInvMsFavoriteOver : art::kInvMsFavorite, at(132, cy - 22), at(176, cy + 22),
                any ? kWhite : IM_COL32(255, 255, 255, 120));
        bool sellOver = false;
        const bool asked = g_invSellAsked && now - g_invSellAsked < 3000;
        if (button(at(180, cy - 22), at(224, cy + 22), &sellOver) && any) {
            if (asked) {
                action = kInventorySell;
                if (index) *index = 0;
                g_invSellAsked = 0;
            } else {
                g_invSellAsked = now;
            }
        }
        pieceIn(d, asked || (sellOver && any) ? art::kInvMsSellOver : art::kInvMsSell, at(180, cy - 22), at(224, cy + 22),
                any ? kWhite : IM_COL32(255, 255, 255, 120));
        if (asked) {
            int value = 0;
            for (int i : g_invPicked)
                if (i < n && !inv[i].locked) value += inv[i].value * inv[i].count;
            char text[64];
            std::snprintf(text, sizeof text, "Sell for §%d? Press again", value);
            ImVec2 as = g_text->CalcTextSizeA(18, FLT_MAX, 0, text);
            d->AddText(g_text, 18, {at(224, 0).x - as.x, at(0, sy0 - 2).y - as.y}, kBlue, text);
        }
    }
    // The hovered tile's tooltip (the game's: its name and value), above it.
    if (hovered >= 0) {
        const SimItem& it = inv[hovered];
        const std::string name = found(it.name);
        char value[32];
        std::snprintf(value, sizeof value, "§%d", it.value);
        ImVec2 ns = g_title->CalcTextSizeA(22, FLT_MAX, 120 * S, name.c_str()), vs = g_text->CalcTextSizeA(19, FLT_MAX, 0, value);
        const float w = std::max(ns.x, vs.x) + 12 * S, h = ns.y + vs.y + 8 * S;
        float x = std::max(r.x + 2 * S, std::min(r.x + 228 * S - w, hoverAt.x - w / 2)), y = hoverAt.y - h - 2 * S;
        if (y < r.y + 2 * S) y = hoverAt.y + 56 * S;
        d->AddRectFilledMultiColor({x, y}, {x + w, y + h}, IM_COL32(255, 255, 255, 250), IM_COL32(255, 255, 255, 250),
                                   IM_COL32(238, 238, 238, 250), IM_COL32(238, 238, 238, 250));
        d->AddRect({x, y}, {x + w, y + h}, IM_COL32(200, 200, 200, 255), 3 * S, 0, 0.8f * S);
        d->AddText(g_title, 22, {x + 6 * S, y + 4 * S}, kBlue, name.c_str(), nullptr, 120 * S);
        d->AddText(g_text, 19, {x + 6 * S, y + 4 * S + ns.y}, kBlue, value);
    }
}

template <class Button>
void drawMySim(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action, bool swipeY,
               float dy, const PhoneDrag& drag, int* index) {
    constexpr float S = 2.5f, A = S / art::kScale;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(9, 73, 171, 255);
    static const art::Piece* const kIcons[kMySimApps] = {&art::kAppAspirations, &art::kAppCareer, &art::kAppWants,
                                                         &art::kAppSkills, &art::kAppNeeds, &art::kAppSimology,
                                                         &art::kAppInventory, &art::kAppSocialGroups, &art::kAppBusinesses,
                                                         &art::kAppHousehold, &art::kAppSoulsJourney};
    // The whole app in the colours of the Sim's mood (user 2026-10-23 23:55Z); a moodlet's page in its own.
    Item moodlets[kMaxItems];
    std::vector<Tip> moodletTips;
    int moodletCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        moodletCount = std::min(kMaxItems, g_counts[kMoodletItems]);
        std::copy(g_items[kMoodletItems], g_items[kMoodletItems] + moodletCount, moodlets);
        moodletTips = g_tips[kMoodletItems];
    }
    if (g_mySimMoodlet >= moodletCount) g_mySimMoodlet = -1;
    const uint32_t pageMood = g_mySimMoodlet >= 0 ? moodlets[g_mySimMoodlet].colour : static_cast<uint32_t>(g_sim.mood);
    if (g_mySim != 0 || g_mySimMoodlet >= 0) d->AddRectFilled(at(0, 0), at(230, 400), moodTint(pageMood, 170));
    auto header = [&](const char* head) {
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
        shadowText(d, g_title, 34, {at(115, 24).x - hs.x / 2, at(115, 24).y - hs.y / 2}, kWhite, head);
    };
    const ULONGLONG now = GetTickCount64();
    if (now - g_mySimAsked > 2000 && action == kNone) action = kMySimOpened, g_mySimAsked = now;  // fresh values while open
    if (g_mySim == 0 && g_mySimMoodlet >= 0) {
        // A moodlet's page: its tile and icon at the top, its name, what it says, why, how long.
        const Item& m = moodlets[g_mySimMoodlet];
        Tip tip;
        if (g_mySimMoodlet < static_cast<int>(moodletTips.size())) tip = moodletTips[g_mySimMoodlet];
        header("Moodlet");
        const ImVec2 c = at(115, 95);
        piece(d, art::kMoodTile, c, 90 * S / art::kMoodTile.w, moodTint(m.colour));
        if (m.icon) {
            shown.push_back(m.icon);
            icon(d, m.icon, c, 68 * S);
        }
        const float wrap = 200 * S;
        float y = 145;
        auto para = [&](ImFont* font, float size, const std::string& t, ImU32 col) {
            if (t.empty()) return;
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, t.c_str());
            const float x = 115 * S - std::min(wrap, ts.x) / 2;
            d->AddText(font, size, {r.x + x + 2, at(0, y).y + 2}, kShadow, t.c_str(), nullptr, wrap);
            d->AddText(font, size, {r.x + x, at(0, y).y}, col, t.c_str(), nullptr, wrap);
            y += ts.y / S + 8;
        };
        para(g_title, 32 * g_phoneText, found(tip.json[0]), kWhite);
        para(g_text, 23 * g_phoneText, found(tip.json[1]), kWhite);
        para(g_text, 21 * g_phoneText, found(tip.json[2]), IM_COL32(235, 238, 245, 230));
        if (tip.minutes > 0) {
            char left[48];
            if (tip.minutes >= 120) std::snprintf(left, sizeof left, "%d hours left", (tip.minutes + 30) / 60);
            else if (tip.minutes >= 60) std::snprintf(left, sizeof left, "1 hour left");
            else std::snprintf(left, sizeof left, "%d minutes left", tip.minutes);
            para(g_text, 21 * g_phoneText, left, IM_COL32(235, 238, 245, 230));
        }
        return;
    }
    if (g_mySim == 0) {
        // My Sim as a page of the game's phone (user 2026-10-24 03:37Z, 12:17Z): the wallpaper, the page's name
        // where the home screen shows its name (mcCategoryName, the app pointed at instead while pointing), our
        // own app icons at the home screen's size in its cells, 9 a page, turned with the game's page buttons
        // (PhonePageButton) or a sideways swipe, as the home screen.
        std::vector<int> apps;
        for (int k = 0; k < kMySimApps; ++k)
            if ((k != kSimCareer || careerName(g_sim.age)) && (k != kSimSouls || g_sim.souls.sent) &&  // Life & Death only
                k != kSimHousehold)  // user 2026-10-25 00:17Z: the Household app removed (Contacts' Household filter)
                apps.push_back(k);
        // Your order (the header's rearrange button): the saved apps first in theirs, the rest after.
        readOrder(L"mysim_order", g_mySimOrder, g_mySimOrderRead);
        if (!g_mySimOrder.empty()) {
            std::vector<int> ordered;
            for (const std::string& name : g_mySimOrder)
                for (int k : apps)
                    if (name == kMySimNames[k] && std::find(ordered.begin(), ordered.end(), k) == ordered.end()) ordered.push_back(k);
            for (int k : apps)
                if (std::find(ordered.begin(), ordered.end(), k) == ordered.end()) ordered.push_back(k);
            apps.swap(ordered);
        }
        const int perPage = 9, pages = static_cast<int>((apps.size() + perPage - 1) / perPage);
        g_mySimPage = std::max(0, std::min(pages - 1, g_mySimPage));
        float slide = g_phoneSwipeX ? g_phoneDx : 0;
        if ((g_mySimPage == 0 && slide > 0) || (g_mySimPage == pages - 1 && slide < 0)) slide *= 0.3f;
        if (g_phoneSwipeEnd && g_phoneSwipeX) {
            if (g_phoneDx < -55 && g_mySimPage + 1 < pages) ++g_mySimPage;
            if (g_phoneDx > 55 && g_mySimPage > 0) --g_mySimPage;
            slide = 0;
        }
        bool peek = false;
        int hovered = -1;
        if (slide != 0) d->PushClipRect(at(0, 95), at(230, 320), true);
        for (int pg = g_mySimPage - 1; pg <= g_mySimPage + 1; ++pg)
        for (int i = 0; i < perPage; ++i) {
            if (pg < 0 || pg >= pages || (pg != g_mySimPage && slide == 0)) break;
            const int n = pg * perPage + i;
            if (n >= static_cast<int>(apps.size())) break;
            const int k = apps[n];
            const float x = 13 + (i % 3) * 68 + 4 + slide + (pg - g_mySimPage) * 230, y = 101 + (i / 3) * 70 + 6;
            const ImVec2 c = at(x + 32, y + 32);
            bool over = false;
            if (g_reorder && !g_phoneSwipeX && button(at(x, y), at(x + 64, y + 64), &over)) {
                // Rearranging, as the home screen: press one, then the place to put it.
                const std::string key = kMySimNames[k];
                if (g_reorderPick.empty()) {
                    g_reorderPick = key;
                } else if (g_reorderPick != key) {
                    std::vector<std::string> keys;
                    for (int a : apps) keys.push_back(kMySimNames[a]);
                    auto from = std::find(keys.begin(), keys.end(), g_reorderPick);
                    if (from != keys.end()) {
                        const std::string moved = *from;
                        const bool forward = (from - keys.begin()) < n;
                        keys.erase(from);
                        keys.insert(std::find(keys.begin(), keys.end(), key) + (forward ? 1 : 0), moved);
                        for (const std::string& o : g_mySimOrder)
                            if (std::find(keys.begin(), keys.end(), o) == keys.end()) keys.push_back(o);
                        g_mySimOrder = keys;
                        saveOrder(L"mysim_order", g_mySimOrder);
                    }
                    g_reorderPick.clear();
                } else {
                    g_reorderPick.clear();
                }
            } else if (!g_reorder && !g_phoneSwipeX && button(at(x, y), at(x + 64, y + 64), &over))
                g_mySim = 1 + k, g_mySimScroll = 0, g_aspChange = false, g_aspPicked = -1, g_hhSim = -1;
            if (g_reorder) {
                const bool picked = g_reorderPick == kMySimNames[k];
                d->AddRect(at(x - 1, y - 1), at(x + 65, y + 65), picked ? IM_COL32(120, 230, 70, 255) : IM_COL32(255, 255, 255, 120),
                           12 * S, 0, (picked ? 2.5f : 1.2f) * S);
            }
            if (over) {
                hovered = k;
                piece(d, art::kPhoneAppOver, c, A);
                if (k == kSimAspirations && g_sim.aspiration.valid) peek = true;
            }
            if (k == kSimAspirations && g_sim.aspiration.valid) {
                // The live tile: a ring round it filling with the current milestone's goals.
                const float rad = 34 * S, p = g_sim.aspiration.done ? 1.0f : std::max(0.0f, std::min(1.0f, g_sim.aspiration.progress));
                d->AddCircle(c, rad, IM_COL32(0, 0, 0, 90), 48, 4.5f * S);
                if (p > 0) {
                    d->PathArcTo(c, rad, -3.14159265f / 2, -3.14159265f / 2 + 2 * 3.14159265f * p, 48);
                    d->PathStroke(IM_COL32(120, 230, 70, 255), 0, 3.2f * S);
                }
            }
            pieceIn(d, *kIcons[k], {c.x - 32 * S, c.y - 32 * S}, {c.x + 32 * S, c.y + 32 * S});
        }
        if (slide != 0) d->PopClipRect();
        if (pages > 1) {
            // The game's page buttons, above the Back button.
            const float start = (230 - pages * 28.0f) / 2;
            for (int i = 0; i < pages; ++i) {
                const float x = start + i * 28;
                bool over = false;
                if (button(at(x, 318), at(x + 28, 346), &over)) g_mySimPage = i;
                piece(d, i == g_mySimPage ? art::kPhonePageOn : over ? art::kPhonePageOver : art::kPhonePage, at(x + 14, 332), A);
            }
        }
        {
            // The banner (user 2026-10-25 01:20Z): your Sim's mood colour at the top, fading into your phone
            // background under the texts; your Sim's first name, its mood, then the household's funds.
            const ImU32 top = moodTint(static_cast<uint32_t>(g_sim.mood), 235), clear = moodTint(static_cast<uint32_t>(g_sim.mood), 0);
            d->AddRectFilled(at(0, 0), at(230, 62), top);
            d->AddRectFilledMultiColor(at(0, 62), at(230, 104), top, top, clear, clear);
            const size_t space = g_sim.name.find(' ');
            const std::string first = g_sim.name.empty() ? std::string("My Sim") : space == std::string::npos ? g_sim.name : g_sim.name.substr(0, space);
            ImVec2 size = g_small->CalcTextSizeA(34 * g_phoneText, FLT_MAX, 0, first.c_str());
            const float scale = g_phoneText * std::min(1.0f, 206 * S / std::max(1.0f, size.x));
            size.x /= g_phoneText, size.y /= g_phoneText;
            const ImVec2 t = at(115, 46);
            shadowText(d, g_small, 34 * scale, {t.x - size.x * scale / 2, t.y - size.y * scale / 2}, kWhite, first.c_str());
            const std::string& mood = found(g_sim.moodJson);
            if (!mood.empty()) {
                const float ms = 24 * g_phoneText;
                ImVec2 mz = g_text->CalcTextSizeA(ms, FLT_MAX, 0, mood.c_str());
                const ImVec2 m = at(115, 66);
                shadowText(d, g_text, ms, {m.x - mz.x / 2, m.y - mz.y / 2}, kWhite, mood.c_str());
            }
        }
        if (g_sim.funds >= 0) {
            // The household's funds (user 2026-10-24 16:10Z), as the HUD's funds: "§12,345".
            const std::string funds = moneyText(g_sim.funds).substr(1);
            const float fs = 26 * g_phoneText;
            ImVec2 size = g_title->CalcTextSizeA(fs, FLT_MAX, 0, funds.c_str());
            const ImVec2 t = at(115, 86);
            shadowText(d, g_title, fs, {t.x - size.x / 2, t.y - size.y / 2}, IM_COL32(150, 230, 90, 255), funds.c_str());
        }
        // The peek card (user 2026-10-23 23:22Z): pointed at for a moment, the Aspirations tile shows
        // the current milestone and its goals, as the game's aspiration button does under the pointer.
        if (!peek) g_aspHoverSince = 0;
        else if (!g_aspHoverSince) g_aspHoverSince = now;
        if (peek && now - g_aspHoverSince > 400) {
            const SimAspiration& a = g_sim.aspiration;
            const float wrap = 180 * S, ts = 20 * g_phoneText;
            std::string head = a.done ? found(a.nameJson) : a.current < static_cast<int>(a.milestones.size())
                                                                 ? found(a.milestones[a.current].first) : found(a.nameJson);
            float h = 26;
            for (const SimGoal& g : a.goals) h += g_text->CalcTextSizeA(ts, FLT_MAX, wrap, found(g.textJson).c_str()).y / S + 5;
            const float cy0 = std::max(140.0f, 354 - h - 4);
            const ImVec2 ca = at(8, cy0), cb = at(222, cy0 + h);
            d->AddRectFilled({ca.x + 4, ca.y + 6}, {cb.x + 4, cb.y + 6}, IM_COL32(0, 0, 0, 80), 8 * S);
            d->AddRectFilled(ca, cb, IM_COL32(250, 251, 253, 248), 8 * S);
            d->AddText(g_title, 24 * g_phoneText, at(16, cy0 + 5), kBlue, head.c_str());
            float gy = cy0 + 24;
            for (const SimGoal& g : a.goals) {
                const std::string& t = found(g.textJson);
                const ImVec2 dot = at(20, gy + 6);
                if (g.done) {
                    d->AddCircleFilled(dot, 5 * S, IM_COL32(70, 170, 50, 255));
                    d->AddLine({dot.x - 2.5f * S, dot.y}, {dot.x - 0.5f * S, dot.y + 2.2f * S}, kWhite, 1.4f * S);
                    d->AddLine({dot.x - 0.5f * S, dot.y + 2.2f * S}, {dot.x + 3 * S, dot.y - 2.2f * S}, kWhite, 1.4f * S);
                } else {
                    d->AddCircle(dot, 5 * S, IM_COL32(120, 130, 145, 255), 20, 1.2f * S);
                }
                ImVec2 tsz = g_text->CalcTextSizeA(ts, FLT_MAX, wrap, t.c_str());
                d->AddText(g_text, ts, at(30, gy), IM_COL32(40, 50, 70, 255), t.c_str(), nullptr, wrap);
                gy += tsz.y / S + 5;
            }
        }
        return;
    }
    const int k = g_mySim - 1;
    if (k == kSimInventory) {
        drawInventory(d, r, button, shown, action, swipeY, dy, drag, index);
        return;
    }
    if (k == kSimHousehold && g_hhSim >= static_cast<int>(g_sim.household.size())) g_hhSim = -1;
    header(k == kSimAspirations && g_aspChange ? "Change Aspiration" : k == kSimCareer ? careerName(g_sim.age)
           : k == kSimHousehold && g_hhSim >= 0 ? g_sim.household[g_hhSim].first.c_str() : kMySimNames[k]);
    static bool g_scrolls0 = false;  // the list scrolled last frame: room for its slider, else rows to the edge
    const float top = 48, bottom = 354, left = 6, right = g_scrolls0 ? 206.0f : 224.0f;
    float y = top - g_mySimScroll;
    d->PushClipRect(at(0, top), at(230, bottom), true);
    // Buttons in the scrolled list only where it shows: a row under the Back button can't take its press.
    auto inList = [&](ImVec2 a, ImVec2 b, bool* over) {
        a.y = std::max(a.y, at(0, top).y);
        b.y = std::min(b.y, at(0, bottom).y);
        if (b.y - a.y < 1) {
            if (over) *over = false;
            return false;
        }
        return button(a, b, over);
    };
    const float ts = 22 * g_phoneText;
    auto bar = [&](float x0, float x1, float by, float h, float fraction, ImU32 back, ImU32 fill) {
        d->AddRectFilled(at(x0, by), at(x1, by + h), back, h / 2 * S);
        const float f = std::max(0.06f, std::min(1.0f, fraction));
        d->AddRectFilled(at(x0, by), at(x0 + (x1 - x0) * f, by + h), fill, h / 2 * S);
        d->AddRectFilled(at(x0 + 1.5f, by + 1), at(std::max(x0 + 1.5f, x0 + (x1 - x0) * f - 1.5f), by + h * 0.45f),
                         IM_COL32(255, 255, 255, 70), h / 4 * S);
    };
    if (k == kSimAspirations && g_aspChange) {
        // Change Aspiration: the tracks by category (ui.aspirations.set_primary); press one, then again to change.
        if (g_aspPicked >= 0 && now - g_aspPickedAt > 4000) g_aspPicked = -1;
        int lastCategory = -1;
        const float rowH = 36 * std::max(1.0f, g_phoneText / 1.2f);
        for (int n = 0; n < static_cast<int>(g_sim.tracks.size()); ++n) {
            const SimTrack& t = g_sim.tracks[n];
            if (t.category != lastCategory) {
                lastCategory = t.category;
                if (t.category >= 0 && t.category < static_cast<int>(g_sim.trackCategories.size())) {
                    shadowText(d, g_title, 24 * g_phoneText, {at(left + 2, 0).x, at(0, y + 4).y}, kWhite,
                               found(g_sim.trackCategories[t.category]).c_str());
                    y += 22 * g_phoneText;
                }
            }
            if (y + rowH > top - 40 && y < bottom + 10) {
                const ImVec2 ra = at(left, y + 2), rb = at(right, y + rowH - 2);
                bool over = false;
                if (inList(ra, rb, &over) && !t.current) {
                    if (g_aspPicked == n) {
                        action = kAspirationPicked;
                        if (index) *index = n;
                        g_aspChange = false, g_aspPicked = -1, g_mySimScroll = 0;
                    } else {
                        g_aspPicked = n, g_aspPickedAt = now;
                    }
                }
                const bool picked = g_aspPicked == n;
                d->AddRectFilled(ra, rb, picked ? IM_COL32(205, 235, 190, 240) : IM_COL32(255, 255, 255, 225), 6 * S);
                if (over || t.current) d->AddRect(ra, rb, t.current ? IM_COL32(70, 170, 50, 255) : IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.4f * S);
                if (t.icon) {
                    shown.push_back(t.icon);
                    icon(d, t.icon, at(left + 17, y + rowH / 2), 28 * S);
                }
                const std::string label = picked ? std::string("Press again to change") : found(t.nameJson);
                ImVec2 ls = g_text->CalcTextSizeA(ts, FLT_MAX, 0, label.c_str());
                const float lk = std::min(1.0f, (right - left - 42) * S / std::max(1.0f, ls.x));
                d->AddText(g_text, ts * lk, {at(left + 36, 0).x, at(0, y + rowH / 2).y - ls.y * lk / 2},
                           picked ? IM_COL32(40, 120, 30, 255) : kBlue, label.c_str());
            }
            y += rowH;
        }
    } else if (k == kSimAspirations) {
        const SimAspiration& a = g_sim.aspiration;
        if (!a.valid) {
            const char* none = g_sim.age ? "No aspiration." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, none);
        } else {
            // The track: its icon, name and description on a card.
            const float wrap = (right - left - 52) * S;
            const std::string &name = found(a.nameJson), &text = found(a.textJson);
            ImVec2 ds = g_text->CalcTextSizeA(18 * g_phoneText, FLT_MAX, wrap, text.c_str());
            const float cardH = std::max(52.0f, 24 * g_phoneText + ds.y / S + 10);
            d->AddRectFilled(at(left, y + 2), at(right, y + cardH), IM_COL32(255, 255, 255, 225), 6 * S);
            if (a.icon) {
                shown.push_back(a.icon);
                icon(d, a.icon, at(left + 23, y + 2 + 23), 42 * S);
            }
            d->AddText(g_title, 24 * g_phoneText, at(left + 48, y + 6), kBlue, name.c_str());
            d->AddText(g_text, 18 * g_phoneText, at(left + 48, y + 8 + 10 * g_phoneText), IM_COL32(60, 70, 90, 255), text.c_str(),
                       nullptr, wrap);
            y += cardH + 6;
            // The milestones: a dot each (green done, ringed the current one), "Milestone n of N".
            char line[48];
            if (a.done) std::snprintf(line, sizeof line, "Aspiration complete!");
            else std::snprintf(line, sizeof line, "Milestone %d of %d", a.current + 1, std::max(1, a.count));
            shadowText(d, g_title, 22 * g_phoneText, {at(left + 2, 0).x, at(0, y).y}, kWhite, line);
            const int n = static_cast<int>(a.milestones.size());
            for (int i = 0; i < n; ++i) {
                const ImVec2 c = at(right - 8 - (n - 1 - i) * 14, y + 6);
                const bool done = a.milestones[i].second;
                d->AddCircleFilled(c, 4.5f * S, done ? IM_COL32(110, 210, 60, 255) : IM_COL32(255, 255, 255, 90));
                if (i == a.current && !a.done) d->AddCircle(c, 6.5f * S, kWhite, 20, 1.3f * S);
            }
            y += 18 * g_phoneText;
            // The current milestone: its name, then each goal with its tick and progress.
            if (!a.done && a.current < n) {
                const std::string& mile = found(a.milestones[a.current].first);
                d->AddRectFilled(at(left, y), at(right, y + 22 * g_phoneText), IM_COL32(9, 73, 171, 230), 6 * S);
                d->AddText(g_title, 22 * g_phoneText, at(left + 6, y + 3), kWhite, mile.c_str());
                y += 22 * g_phoneText + 3;
                const float gwrap = (right - left - 52) * S;
                for (const SimGoal& g : a.goals) {
                    const std::string& t = found(g.textJson);
                    ImVec2 tsz = g_text->CalcTextSizeA(ts, FLT_MAX, gwrap, t.c_str());
                    const float h = std::max(26.0f, tsz.y / S + 10);
                    d->AddRectFilled(at(left, y), at(right, y + h - 2), IM_COL32(255, 255, 255, 225), 6 * S);
                    const ImVec2 dot = at(left + 11, y + h / 2 - 1);
                    if (g.done) {
                        d->AddCircleFilled(dot, 6 * S, IM_COL32(70, 170, 50, 255));
                        d->AddLine({dot.x - 3 * S, dot.y}, {dot.x - 0.6f * S, dot.y + 2.6f * S}, kWhite, 1.6f * S);
                        d->AddLine({dot.x - 0.6f * S, dot.y + 2.6f * S}, {dot.x + 3.6f * S, dot.y - 2.6f * S}, kWhite, 1.6f * S);
                    } else {
                        d->AddCircle(dot, 6 * S, IM_COL32(120, 130, 145, 255), 20, 1.4f * S);
                    }
                    d->AddText(g_text, ts, {at(left + 22, 0).x, at(0, y + h / 2 - 1).y - tsz.y / 2}, g.done ? IM_COL32(90, 100, 115, 255) : kBlue,
                               t.c_str(), nullptr, gwrap);
                    if (g.showProgress && g.goal > 1 && !g.done) {
                        char pr[24];
                        std::snprintf(pr, sizeof pr, "%d/%d", std::min(g.value, g.goal), g.goal);
                        ImVec2 ps = g_text->CalcTextSizeA(20, FLT_MAX, 0, pr);
                        d->AddText(g_text, 20, {at(right - 4, 0).x - ps.x, at(0, y + h / 2 - 1).y - ps.y / 2}, IM_COL32(60, 70, 90, 255), pr);
                    }
                    y += h;
                }
            }
            // Change Aspiration (the game's button under the panel).
            y += 6;
            if (!g_sim.tracks.empty()) {
                const ImVec2 ba = at(left + 30, y), bb = at(right - 30, y + 26);
                bool over = false;
                if (inList(ba, bb, &over)) g_aspChange = true, g_aspPicked = -1, g_mySimScroll = 0;
                if (over) nine(d, art::kDialogButtonOver, {ba.x - 2, ba.y - 3}, {bb.x + 2, bb.y + 6}, 48, 36, 48, 54, 0.6f);
                else nine(d, art::kDialogButton, ba, bb, 48, 36, 48, 42, 0.6f);
                const char* label = "Change Aspiration";
                ImVec2 ls = g_text->CalcTextSizeA(24, FLT_MAX, 0, label);
                d->AddText(g_text, 24, {(ba.x + bb.x - ls.x) / 2, (ba.y + bb.y - ls.y) / 2 - 2}, IM_COL32(30, 70, 128, 255), label);
                y += 30;
            }
        }
    } else if (k == kSimCareer) {
        // The career panel per career (school and after-school activities too): its icon, name,
        // level title, level n of N, the performance bar, pay, company and the next shift.
        if (g_sim.jobs.empty()) {
            const char* none = g_sim.age == 4 || g_sim.age == 8 ? "Not enrolled." : g_sim.age ? "No job." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, none);
        }
        for (const SimJob& j : g_sim.jobs) {
            const float small = 19 * g_phoneText, lh = small / S + 3;
            const float h = 56 + 3 * lh + 14;
            d->AddRectFilled(at(left, y + 2), at(right, y + h), IM_COL32(255, 255, 255, 228), 6 * S);
            if (j.icon) {
                shown.push_back(j.icon);
                icon(d, j.icon, at(left + 24, y + 26), 40 * S);
            }
            d->AddText(g_title, 24 * g_phoneText, at(left + 50, y + 7), kBlue, found(j.nameJson).c_str());
            d->AddText(g_text, small, at(left + 50, y + 8 + 10 * g_phoneText), IM_COL32(50, 60, 80, 255), found(j.titleJson).c_str());
            char lv[32];
            std::snprintf(lv, sizeof lv, "Level %d of %d", j.level, std::max(j.level, j.levels));
            d->AddText(g_text, small, at(left + 50, y + 10 + 18 * g_phoneText), IM_COL32(50, 60, 80, 255), lv);
            float ly = y + 56;
            d->AddText(g_text, small, at(left + 6, ly), kBlue, "Performance");
            bar(left + 6 + 62, right - 6, ly + 3, 7, j.performance, IM_COL32(200, 206, 214, 255),
                j.performance >= 0.5f ? IM_COL32(88, 200, 56, 255) : j.performance >= 0.25f ? IM_COL32(240, 190, 40, 255)
                                                                   : IM_COL32(220, 70, 50, 255));
            ly += lh + 2;
            const std::string& company = found(j.companyJson);
            std::string line = j.pay;
            if (!company.empty()) line += (line.empty() ? "" : "  -  ") + company;
            d->AddText(g_text, small, at(left + 6, ly), IM_COL32(50, 60, 80, 255), line.c_str());
            ly += lh;
            d->AddText(g_text, small, at(left + 6, ly), IM_COL32(50, 60, 80, 255), j.shift.c_str());
            y += h + 4;
        }
    } else if (k == kSimWants) {
        // A notebook page in the Sim's own hand: "Want:" / "Fear:" and each one's name and
        // tooltip (hudinfo.py: the game's goal name, tooltip and reason).
        Item wants[kMaxItems];
        std::vector<Tip> wantTips;
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(g_itemMutex);
            count = std::min(kMaxItems, g_counts[kWantItems]);
            std::copy(g_items[kWantItems], g_items[kWantItems] + count, wants);
            wantTips = g_tips[kWantItems];
        }
        d->AddRectFilled(at(left, top), at(right + 16, bottom), IM_COL32(253, 250, 236, 250), 4 * S);
        for (float ly = top + 18 - std::fmod(g_mySimScroll, 14.0f); ly < bottom; ly += 14)
            d->AddLine(at(left, ly), at(right + 16, ly), IM_COL32(150, 185, 225, 160), 0.6f * S);
        d->AddLine(at(left + 20, top), at(left + 20, bottom), IM_COL32(225, 110, 110, 170), 0.8f * S);
        const ImU32 kInk = IM_COL32(25, 45, 120, 255);
        const float wrap = (right + 10 - left - 26) * S;
        y += 4;
        for (int i = 0; i < count; ++i) {
            Tip tip;
            if (i < static_cast<int>(wantTips.size())) tip = wantTips[i];
            const bool fear = (wants[i].flags & 1) != 0;
            d->AddText(g_title, 24 * g_phoneText, at(left + 24, y), fear ? IM_COL32(170, 30, 40, 255) : kInk, fear ? "Fear:" : "Want:");
            y += 13 * g_phoneText;
            for (int f = 0; f < 3; ++f) {
                const std::string& t = found(tip.json[f]);
                if (t.empty()) continue;
                ImVec2 ts2 = (f == 0 ? g_title : g_text)->CalcTextSizeA(ts, FLT_MAX, wrap, t.c_str());
                d->AddText(f == 0 ? g_title : g_text, ts, at(left + 24, y), kInk, t.c_str(), nullptr, wrap);
                y += ts2.y / S + 2;
            }
            y += 8;
        }
        if (!count) d->AddText(g_text, ts, at(left + 24, y), kInk, "Nothing on my mind right now.");
    } else if (k == kSimSkills) {
        if (g_sim.skills.empty()) {
            const char* none = g_sim.age ? "No skills yet." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 180).x - ns.x / 2, at(115, 180).y - ns.y / 2}, kWhite, none);
        }
        int lastCategory = -1000;
        const float rowH = 40 * std::max(1.0f, g_phoneText / 1.2f);
        for (const SimSkill& sk : g_sim.skills) {
            if (sk.category != lastCategory) {
                lastCategory = sk.category;
                std::string title;
                for (const auto& c : g_sim.categories)
                    if (c.first == sk.category) title = found(c.second);
                if (!title.empty() && g_sim.categories.size() > 1) {
                    shadowText(d, g_title, 24 * g_phoneText, {at(left + 2, 0).x, at(0, y + 3).y}, kWhite, title.c_str());
                    y += 20 * g_phoneText;
                }
            }
            if (y + rowH > top - 60 && y < bottom + 10) {
                const ImVec2 ra = at(left, y + 2), rb = at(right, y + rowH - 2);
                d->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 225), 6 * S);
                if (sk.icon) {
                    shown.push_back(sk.icon);
                    icon(d, sk.icon, at(left + 18, y + rowH / 2), 30 * S);
                }
                const std::string& name = found(sk.nameJson);
                const ImU32 ink = sk.mastered ? IM_COL32(170, 110, 0, 255) : kBlue;
                d->AddText(g_text, ts, at(left + 37, y + 5), ink, name.c_str());
                const float by = y + rowH - 13;
                bar(left + 37, right - 30, by, 6, sk.progress, IM_COL32(200, 206, 214, 255),
                    sk.mastered ? IM_COL32(235, 175, 30, 255) : IM_COL32(88, 200, 56, 255));
                // The level on its disc (the game's txtLevel), an arrow above while rising (mcHighlight).
                const ImVec2 c = at(right - 14, y + rowH / 2);
                d->AddCircleFilled(c, 11 * S, sk.mastered ? IM_COL32(235, 175, 30, 255) : kBlue);
                char lv[8];
                std::snprintf(lv, sizeof lv, "%d", sk.level);
                ImVec2 ls = g_title->CalcTextSizeA(28, FLT_MAX, 0, lv);
                d->AddText(g_title, 28, {c.x - ls.x / 2, c.y - ls.y / 2}, kWhite, lv);
                if (sk.rising) {
                    const ImVec2 t = at(right - 31, y + 10);
                    d->AddTriangleFilled({t.x, t.y - 4 * S}, {t.x + 4 * S, t.y + 2 * S}, {t.x - 4 * S, t.y + 2 * S},
                                         IM_COL32(60, 180, 40, 255));
                }
            }
            y += rowH;
        }
    } else if (k == kSimBusinesses) {
        // Owned Businesses (business_service): per business its kind, name, open or closed, stars, funds, today's
        // revenue and staff.
        static const char* const kKinds[6] = {"Business", "Retail store", "Restaurant", "Vet clinic", "Rental", "Small business"};
        const float small = 19 * g_phoneText;
        for (const SimBusiness& b : g_sim.businesses) {
            const float h = 30 + 4 * (small / S + 3) + 8;
            d->AddRectFilled(at(left, y), at(right, y + h), IM_COL32(255, 255, 255, 228), 6 * S);
            d->AddText(g_title, 24 * g_phoneText, at(left + 6, y + 4), kBlue, b.name.c_str());
            const char* state = b.open ? "Open" : "Closed";
            ImVec2 ss = g_text->CalcTextSizeA(small, FLT_MAX, 0, state);
            const ImVec2 pa = at(right - 8, y + 6);
            d->AddRectFilled({pa.x - ss.x - 6 * S, pa.y}, {pa.x, pa.y + ss.y + 2 * S},
                             b.open ? IM_COL32(70, 170, 50, 255) : IM_COL32(150, 158, 170, 255), 4 * S);
            d->AddText(g_text, small, {pa.x - ss.x - 3 * S, pa.y + 1 * S}, kWhite, state);
            float yy = y + 30;
            auto line = [&](const char* label, const std::string& value) {
                d->AddText(g_text, small, at(left + 8, yy), IM_COL32(50, 60, 80, 255), label);
                ImVec2 vs = g_text->CalcTextSizeA(small, FLT_MAX, 0, value.c_str());
                d->AddText(g_text, small, {at(right - 8, 0).x - vs.x, at(0, yy).y}, kBlue, value.c_str());
                yy += small / S + 3;
            };
            line("Kind", kKinds[b.type >= 0 && b.type <= 5 ? b.type : 0]);
            // Stars: five, filled to the rating.
            {
                d->AddText(g_text, small, at(left + 8, yy), IM_COL32(50, 60, 80, 255), "Rating");
                for (int i = 0; i < 5; ++i) {
                    const ImVec2 sc = at(right - 12 - (4 - i) * 11, yy + small / S / 2);
                    const float fill = std::max(0.0f, std::min(1.0f, b.stars - i));
                    d->AddCircleFilled(sc, 4.5f * S, IM_COL32(200, 206, 214, 255));
                    if (fill > 0) d->AddCircleFilled(sc, 4.5f * S * fill, IM_COL32(235, 175, 30, 255));
                }
                yy += small / S + 3;
            }
            char v[32];
            std::snprintf(v, sizeof v, "\xC2\xA7%d", b.funds);
            line("Funds", v);
            std::snprintf(v, sizeof v, "\xC2\xA7%d today, %d staff", b.revenue, b.employees);
            line("Revenue", v);
            y += h + 6;
        }
        if (g_sim.businesses.empty()) {
            const char* none = g_sim.age ? "No businesses." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, none);
        }
    } else if (k == kSimSouls) {
        // Soul's Journey (user 2026-10-24 12:55Z, mark 14-53-59): the aspiration panel's UnfinishedBusinessPanel as
        // the game draws it. Its RankedStatUnfinishedBusinessMeter: the background, the dashed ring (living or
        // ghost), its coloured dashes lit up to the rank, the nine pips round it (lit from their rank; pip 9 the
        // ghost lock, living only), the soul in the middle; then "<name>'s Bucket List" and the bucket list's goals.
        const SimSouls& sj = g_sim.souls;
        // The hover popups (user 2026-10-24 14:26Z): the game's tooltips on the pips, the help button and the goals
        // (TooltipExtension: title and description), drawn last as the game's tooltip card under (or over) the item.
        std::string tipTitle, tipText;
        float tipBelow = 0, tipAbove = 0;
        auto tipAt = [&](float below, float above, const std::string& title, const std::string& text) {
            if (title.empty() && text.empty()) return;
            tipTitle = title, tipText = text, tipBelow = below, tipAbove = above;
        };
        const float q = 0.75f;  // phone units a ring pixel (the ring's picture is 202 x 170)
        const float bgH = (right - left) * 280 / 354;
        pieceIn(d, sj.ghost ? art::kSjBackGhost : art::kSjBack, at(left, y), at(right, y + bgH));
        const float y0 = y + (bgH - 170 * q) / 2;
        auto ring = [&](float rx, float ry) { return at(115 + (rx - 102) * q, y0 + ry * q); };
        pieceIn(d, sj.ghost ? art::kSjTrackGhost : art::kSjTrack, ring(0, 0), ring(202, 170));
        static const art::Piece* const kFills[8] = {&art::kSjFill0, &art::kSjFill1, &art::kSjFill2, &art::kSjFill3,
                                                    &art::kSjFill4, &art::kSjFill5, &art::kSjFill6, &art::kSjFill7};
        static const float kFillAt[8][4] = {{28, 134, 25, 25}, {16, 95, 14, 20}, {20, 47, 20, 25}, {49, 14, 24, 23},
                                            {94, 8, 32, 15},   {144, 21, 23, 23}, {176, 78, 13, 27}, {161, 128, 20, 22}};
        for (int n = 0; n < 8; ++n)
            if (sj.rank > n || (sj.rank == n && sj.progress > 0))
                pieceIn(d, *kFills[n], ring(kFillAt[n][0], kFillAt[n][1]), ring(kFillAt[n][0] + kFillAt[n][2], kFillAt[n][1] + kFillAt[n][3]));
        const art::Piece& soul = sj.ghost ? art::kSjGhost : art::kSjLiving;
        pieceIn(d, soul, ring(102 - 46, 95 - 46), ring(102 + 46, 95 + 46));
        // The pips on the ring (their places measured from the game's panel), clockwise from the bottom left.
        static const art::Piece* const kOn[8] = {&art::kSjPip1On, &art::kSjPip2On, &art::kSjPip3On, &art::kSjPip4On,
                                                 &art::kSjPip5On, &art::kSjPip6On, &art::kSjPip7On, &art::kSjPip8On};
        static const art::Piece* const kOff[8] = {&art::kSjPip1Off, &art::kSjPip2Off, &art::kSjPip3Off, &art::kSjPip4Off,
                                                  &art::kSjPip5Off, &art::kSjPip6Off, &art::kSjPip7Off, &art::kSjPip8Off};
        static const float kPipAngle[9] = {155, 183, 213, 255, 298, 333, 19, 49, 358};  // pips 1-8, then 9
        for (int n = 0; n < 9; ++n) {
            if (n == 8 && sj.ghost) continue;
            const float rad = kPipAngle[n] * 3.14159265f / 180;
            const float px = 102 + 80 * std::cos(rad), py = 95 + 80 * std::sin(rad);
            const art::Piece& pip = n == 8 ? art::kSjPip9 : sj.rank > n ? *kOn[n] : *kOff[n];
            bool pipOver = false;
            inList(ring(px - 14, py - 14), ring(px + 14, py + 14), &pipOver);
            if (pipOver) tipAt(ring(px, py + 16).y, ring(px, py - 16).y, std::string(), found(sj.tips[n]));
            pieceIn(d, pip, ring(px - 18, py - 18), ring(px + 18, py + 18));
        }
        {
            // The panel's help button (its "?", the help component): the living or ghost help, then the packs' parts.
            const ImVec2 hc = at(right - 11, y + 11);
            bool helpOver = false;
            inList({hc.x - 9 * S, hc.y - 9 * S}, {hc.x + 9 * S, hc.y + 9 * S}, &helpOver);
            d->AddCircleFilled(hc, 7.5f * S, helpOver ? IM_COL32(92, 170, 240, 255) : IM_COL32(52, 132, 214, 255));
            d->AddCircle(hc, 7.5f * S, IM_COL32(255, 255, 255, 230), 0, 0.9f * S);
            ImVec2 qs = g_title->CalcTextSizeA(30, FLT_MAX, 0, "?");
            d->AddText(g_title, 30, {hc.x - qs.x / 2, hc.y - qs.y / 2}, kWhite, "?");
            if (helpOver) {
                std::string help = found(sj.tips[9]);
                for (int i = 10; i < 12; ++i) {
                    const std::string& more = found(sj.tips[i]);
                    if (!more.empty()) help += (help.empty() ? "" : "\n\n") + more;
                }
                tipAt(hc.y + 9 * S, hc.y - 9 * S, std::string(), help);
            }
        }
        y += bgH + 6;
        // "<name>'s Bucket List" (living) or "<name>'s Unfinished Business" (ghost), then the goals as cards.
        {
            const std::string head = sj.first + (sj.ghost ? "'s Unfinished Business" : "'s Bucket List");
            ImVec2 hs = g_title->CalcTextSizeA(24 * g_phoneText, FLT_MAX, 0, head.c_str());
            shadowText(d, g_title, 24 * g_phoneText, {at(115, 0).x - hs.x / 2, at(0, y).y}, kWhite, head.c_str());
            y += hs.y / S + 6;
        }
        const ImU32 kGoalBlue = IM_COL32(36, 108, 196, 255);
        for (const SimSouls::Goal& g : sj.goals) {
            const std::string text = g.text.empty() ? std::string("...") : found(g.text);
            char count[24] = "";
            if (g.show) std::snprintf(count, sizeof count, "%d/%d", g.value, g.goal);
            const float cw = g_text->CalcTextSizeA(20 * g_phoneText, FLT_MAX, 0, count).x / S;
            const float wrap = (right - left - 18 - cw) * S;
            const float th = g_text->CalcTextSizeA(20 * g_phoneText, FLT_MAX, wrap, text.c_str()).y / S;
            const float h = th + 10;
            bool goalOver = false;
            inList(at(left, y), at(right, y + h), &goalOver);
            if (goalOver) tipAt(at(0, y + h).y, at(0, y).y, text, found(g.tip));
            d->AddRectFilled(at(left, y), at(right, y + h), IM_COL32(255, 255, 255, g.done ? 200 : 240), 5 * S);
            d->AddRect(at(left, y), at(right, y + h), IM_COL32(205, 214, 228, 255), 5 * S, 0, 0.8f * S);
            d->AddText(g_text, 20 * g_phoneText, at(left + 6, y + 5), g.done ? IM_COL32(120, 128, 140, 255) : kGoalBlue,
                       text.c_str(), nullptr, wrap);
            if (g.done) {
                const ImVec2 c = at(right - 10, y + h / 2);
                d->AddLine({c.x - 3 * S, c.y}, {c.x - 0.6f * S, c.y + 2.6f * S}, IM_COL32(60, 150, 40, 255), 1.8f * S);
                d->AddLine({c.x - 0.6f * S, c.y + 2.6f * S}, {c.x + 3.6f * S, c.y - 2.6f * S}, IM_COL32(60, 150, 40, 255), 1.8f * S);
            } else if (count[0]) {
                d->AddText(g_text, 20 * g_phoneText, at(right - 5 - cw, y + 5), IM_COL32(50, 60, 80, 255), count);
            }
            y += h + 3;
        }
        y += 4;
        if (!tipTitle.empty() || !tipText.empty()) {
            // The game's tooltip card (as the contact page's): white to light grey, a thin border, the title
            // navy and bold, the description under it; under the item, or over it when there's no room.
            const float pad = 6 * S, wrap = (right - left - 12) * S, tsz = 20 * g_phoneText, dsz = 18 * g_phoneText;
            const ImVec2 th = tipTitle.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(tsz, FLT_MAX, wrap, tipTitle.c_str());
            const ImVec2 dh = tipText.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(dsz, FLT_MAX, wrap, tipText.c_str());
            const float h = pad * 2 + th.y + dh.y + (th.y > 0 && dh.y > 0 ? 3 * S : 0);
            const float lo = at(0, top).y, hi = at(0, bottom).y;
            float ty = tipBelow + 2 * S;
            if (ty + h > hi) ty = std::max(lo, tipAbove - 2 * S - h);
            const ImVec2 a = {at(left, 0).x, ty}, b = {at(right, 0).x, ty + h};
            d->AddRectFilled({a.x + 1.5f * S, a.y + 1.5f * S}, {b.x + 1.5f * S, b.y + 1.5f * S}, IM_COL32(0, 0, 0, 60), 4 * S);
            d->AddRectFilledMultiColor(a, b, IM_COL32(255, 255, 255, 252), IM_COL32(255, 255, 255, 252), IM_COL32(234, 238, 244, 252),
                                       IM_COL32(234, 238, 244, 252));
            d->AddRect(a, b, IM_COL32(200, 208, 220, 255), 4 * S, 0, 0.8f * S);
            float ly = a.y + pad;
            if (th.y > 0) {
                d->AddText(g_title, tsz, {a.x + pad, ly}, IM_COL32(28, 44, 82, 255), tipTitle.c_str(), nullptr, wrap);
                ly += th.y + 3 * S;
            }
            if (dh.y > 0) d->AddText(g_text, dsz, {a.x + pad, ly}, IM_COL32(36, 108, 196, 255), tipText.c_str(), nullptr, wrap);
        }
    } else if (k == kSimSimology) {
        // Simology (SimInfoSummaryPanel, mark 2026-10-24 04-46-51): the name field, then green section headers
        // (press to fold) over two-column grids of the game's trait cells (personality white, reward sunburst,
        // phase, tradition, fear), the icon on the cell's left square, the name in blue.
        static bool folded[3] = {false, false, false};
        {
            const ImVec2 a = at(left, y + 2), b = at(right, y + 28);
            d->AddRectFilledMultiColor(a, b, IM_COL32(246, 246, 246, 255), IM_COL32(246, 246, 246, 255), IM_COL32(214, 214, 214, 255),
                                       IM_COL32(214, 214, 214, 255));
            d->AddRect(a, b, IM_COL32(170, 170, 170, 255), 13 * S, 0, 0.8f * S);
            const ImVec2 ia = at(left + 22, y + 6), ib = at(right - 22, y + 24);
            d->AddRectFilledMultiColor(ia, ib, IM_COL32(40, 40, 40, 255), IM_COL32(40, 40, 40, 255), IM_COL32(78, 78, 78, 255),
                                       IM_COL32(78, 78, 78, 255));
            const std::string& name = g_sim.name;
            ImVec2 ns = g_title->CalcTextSizeA(22, FLT_MAX, 0, name.c_str());
            const float fit = std::min(1.0f, (ib.x - ia.x - 8) / std::max(1.0f, ns.x));
            d->AddText(g_title, 22 * fit, {(ia.x + ib.x - ns.x * fit) / 2, (ia.y + ib.y - ns.y * fit) / 2}, kWhite, name.c_str());
            y += 32;
        }
        const char* const titles[3] = {"TRAITS", "LIKES & DISLIKES", "TURN-ONS & TURN-OFFS"};
        const char* const groups[3] = {"T", "LD", "OF"};
        const art::Piece* const cells[5] = {&art::kSyCellPersonality, &art::kSyCellReward, &art::kSyCellPhase, &art::kSyCellTradition,
                                            &art::kSyCellFear};
        const float cw = (right - left - 2) / 2, ch = cw * 40 / 182;
        for (int sct = 0; sct < 3; ++sct) {
            int count = 0;
            for (const auto& t : g_sim.simology) count += std::strchr(groups[sct], t.group) != nullptr;
            if (!count && sct > 0) continue;
            // The header: the game's green bar, its title in white capitals, the fold arrow on the right.
            const ImVec2 ha = at(left, y), hb = at(right, y + 18);
            bool over = false;
            if (inList(ha, hb, &over)) folded[sct] = !folded[sct];
            d->AddRectFilledMultiColor(ha, hb, IM_COL32(120, 206, 70, 255), IM_COL32(120, 206, 70, 255), IM_COL32(62, 160, 34, 255),
                                       IM_COL32(62, 160, 34, 255));
            if (over) d->AddRect(ha, hb, IM_COL32(255, 255, 255, 160), 2 * S, 0, 1.0f * S);
            ImVec2 tsz = g_title->CalcTextSizeA(20, FLT_MAX, 0, titles[sct]);
            shadowText(d, g_title, 20, {(ha.x + hb.x - tsz.x) / 2, (ha.y + hb.y - tsz.y) / 2}, kWhite, titles[sct]);
            const ImVec2 ac = at(right - 10, y + 9);
            const float as = 12 * S / art::kSyArrow.w;
            if (folded[sct]) {
                d->AddTriangleFilled({ac.x - 3 * S, ac.y - 3 * S}, {ac.x - 3 * S, ac.y + 3 * S}, {ac.x + 3 * S, ac.y}, kWhite);
            } else {
                piece(d, art::kSyArrow, ac, as);
            }
            y += 20;
            if (folded[sct]) continue;
            for (int part = 0; part < 2; ++part) {
                const char g = groups[sct][part];
                if (!g) break;
                int n = 0;
                if (sct > 0) {
                    int here = 0;
                    for (const auto& t : g_sim.simology) here += t.group == g;
                    if (!here) continue;
                    const char* sub = g == 'L' ? "Likes" : g == 'D' ? "Dislikes" : g == 'O' ? "Turn-Ons" : "Turn-Offs";
                    d->AddText(g_title, 19, at(left + 4, y + 1), IM_COL32(60, 64, 72, 255), sub);
                    y += 10;
                }
                for (const auto& t : g_sim.simology) {
                    if (t.group != g) continue;
                    const float x = left + (n % 2) * (cw + 2), cy = y + (n / 2) * (ch + 2);
                    const ImVec2 ca = at(x, cy), cb = at(x + cw, cy + ch);
                    pieceIn(d, *cells[std::max(0, std::min(4, t.style))], ca, cb);
                    if (t.icon) {
                        shown.push_back(t.icon);
                        icon(d, t.icon, at(x + ch * 0.5f, cy + ch * 0.5f), ch * 0.92f * S);
                    }
                    const float tx = x + ch * 1.12f, wrapW = (x + cw - 2 - tx) * S;
                    float size = 19;
                    ImVec2 sz = g_title->CalcTextSizeA(size, FLT_MAX, wrapW, t.name.c_str());
                    if (sz.y > ch * S * 0.95f) size *= std::max(0.6f, ch * S * 0.95f / sz.y), sz = g_title->CalcTextSizeA(size, FLT_MAX, wrapW, t.name.c_str());
                    d->AddText(g_title, size, {at(tx, 0).x, (ca.y + cb.y - sz.y) / 2}, IM_COL32(36, 108, 196, 255), t.name.c_str(), nullptr, wrapW);
                    ++n;
                }
                y += ((n + 1) / 2) * (ch + 2) + 2;
            }
            y += 4;
        }
    } else if (k == kSimClubs) {
        // Social Groups (the game's club panel): per club its icon, name, leader, gathering now, description,
        // members' faces (the leader's ringed gold) and its rules, encouraged and discouraged.
        const float small = 19 * g_phoneText, wrap = (right - left - 12) * S;
        for (const SimClub& c : g_sim.clubs) {
            const std::string& desc = c.description;
            const float dh = desc.empty() ? 0 : g_text->CalcTextSizeA(small, FLT_MAX, wrap, desc.c_str()).y / S + 4;
            const float mh = c.members.empty() ? 0 : 40;
            float rh = 0;
            for (const auto& rl : c.rules) rh += g_text->CalcTextSizeA(small, FLT_MAX, wrap - 14 * S, rl.second.c_str()).y / S + 2;
            const float h = 44 + dh + mh + rh + 6;
            d->AddRectFilled(at(left, y), at(right, y + h), IM_COL32(255, 255, 255, 228), 6 * S);
            if (c.icon) {
                shown.push_back(c.icon);
                icon(d, c.icon, at(left + 21, y + 21), 36 * S);
            }
            d->AddText(g_title, 24 * g_phoneText, at(left + 44, y + 5), kBlue, c.name.c_str());
            std::string sub = c.leader.empty() ? std::string() : "Leader: " + c.leader;
            if (c.gathering) sub += sub.empty() ? "Gathering now" : "  -  Gathering now";
            d->AddText(g_text, small, at(left + 44, y + 7 + 12 * g_phoneText), IM_COL32(50, 60, 80, 255), sub.c_str());
            float yy = y + 44;
            if (!desc.empty()) {
                d->AddText(g_text, small, at(left + 6, yy), IM_COL32(50, 60, 80, 255), desc.c_str(), nullptr, wrap);
                yy += dh;
            }
            if (!c.members.empty()) {
                for (size_t m = 0; m < c.members.size(); ++m) {
                    const ImVec2 mc = at(left + 20 + m * 30, yy + 18);
                    if (mc.x > at(right - 10, 0).x) break;
                    if (c.members[m].leader) d->AddCircleFilled(mc, 15 * S, IM_COL32(235, 175, 30, 255));
                    if (c.members[m].face) {
                        shown.push_back(c.members[m].face);
                        icon(d, c.members[m].face, mc, 28 * S);
                    }
                }
                yy += mh;
            }
            for (const auto& rl : c.rules) {
                const ImVec2 dot = at(left + 12, yy + 6);
                d->AddCircleFilled(dot, 4.5f * S, rl.first ? IM_COL32(70, 170, 50, 255) : IM_COL32(220, 70, 50, 255));
                d->AddLine({dot.x - 2.2f * S, dot.y}, {dot.x + 2.2f * S, dot.y}, kWhite, 1.2f * S);
                if (rl.first) d->AddLine({dot.x, dot.y - 2.2f * S}, {dot.x, dot.y + 2.2f * S}, kWhite, 1.2f * S);
                d->AddText(g_text, small, at(left + 20, yy), kBlue, rl.second.c_str(), nullptr, wrap - 14 * S);
                yy += g_text->CalcTextSizeA(small, FLT_MAX, wrap - 14 * S, rl.second.c_str()).y / S + 2;
            }
            y += h + 6;
        }
        if (g_sim.clubs.empty()) {
            const char* none = g_sim.age ? "Not in any club." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, none);
        }
    } else if (k == kSimInventory) {
        // The Sim's inventory (the game's inventory panel): a tile a stack, its picture, count and name; press one
        // for its pie menu at your hand (phonesim.inventory_pie).
        const float cw = (right - left - 8) / 3, ch = 78;
        const int n = static_cast<int>(g_sim.inventory.size());
        for (int i = 0; i < n; ++i) {
            const SimItem& it = g_sim.inventory[i];
            const float x = left + (i % 3) * (cw + 4), cy = y + (i / 3) * (ch + 4);
            if (cy + ch < top - 10 || cy > bottom + 10) continue;
            const ImVec2 ca = at(x, cy), cb = at(x + cw, cy + ch);
            bool over = false;
            if (inList(ca, cb, &over)) {
                action = kInventoryPie;
                if (index) *index = i;
            }
            d->AddRectFilled(ca, cb, IM_COL32(255, 255, 255, 228), 6 * S);
            if (over) d->AddRect(ca, cb, kBlue, 6 * S, 0, 1.4f * S);
            if (it.picture) {
                shown.push_back(it.picture);
                icon(d, it.picture, at(x + cw / 2, cy + 28), 48 * S);
            }
            if (it.count > 1) {
                char cnt[12];
                std::snprintf(cnt, sizeof cnt, "%d", it.count);
                ImVec2 cs = g_title->CalcTextSizeA(22, FLT_MAX, 0, cnt);
                const ImVec2 bc = at(x + cw - 10, cy + 9);
                d->AddCircleFilled(bc, std::max(7 * S, cs.x / 2 + 2 * S), kBlue);
                d->AddText(g_title, 22, {bc.x - cs.x / 2, bc.y - cs.y / 2}, kWhite, cnt);
            }
            const float ns = 17 * g_phoneText, wrap = (cw - 4) * S;
            ImVec2 sz = g_text->CalcTextSizeA(ns, FLT_MAX, wrap, it.name.c_str());
            d->PushClipRect(at(x, cy), at(x + cw, cy + ch), true);
            d->AddText(g_text, ns, {at(x + cw / 2, 0).x - std::min(sz.x, wrap) / 2, at(0, cy + 55).y}, IM_COL32(40, 50, 70, 255),
                       it.name.c_str(), nullptr, wrap);
            d->PopClipRect();
        }
        y += ((n + 2) / 3) * (ch + 4);
        if (!n) {
            const char* none = g_sim.age ? "Empty." : "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, none);
        }
    } else if (k == kSimHousehold && g_hhSim >= 0) {
        // A household Sim's page (user 2026-10-24 00:39Z): the bust on top, name, age, and Play as:
        // the eyes close, the game switches to the Sim (phonesim.play_as), the eyes open.
        const SimMember& m = g_sim.household[g_hhSim];
        const uint64_t pic = m.bust ? m.bust : m.face;
        d->AddCircleFilled(at(115, y + 72), 66 * S, IM_COL32(255, 255, 255, 70), 64);
        if (pic) {
            shown.push_back(pic);
            icon(d, pic, at(115, y + 72), 130 * S);
        }
        y += 146;
        ImVec2 ns = g_title->CalcTextSizeA(30 * g_phoneText, FLT_MAX, 0, m.name.c_str());
        shadowText(d, g_title, 30 * g_phoneText, {at(115, 0).x - ns.x / 2, at(0, y).y}, kWhite, m.name.c_str());
        y += 14 * g_phoneText + 4;
        const char* state = m.active ? "Playing now" : m.here ? "Here" : "Not on this lot";
        char line[64];
        std::snprintf(line, sizeof line, "%s%s%s", ageName(m.age), *ageName(m.age) ? " - " : "", state);
        ImVec2 ls = g_text->CalcTextSizeA(22 * g_phoneText, FLT_MAX, 0, line);
        shadowText(d, g_text, 22 * g_phoneText, {at(115, 0).x - ls.x / 2, at(0, y).y}, kWhite, line);
        y += 12 * g_phoneText + 10;
        const bool busy = g_playAsAt && now - g_playAsAt < 2500;
        const bool can = m.here && !m.active && !busy;
        std::string label = m.active ? "Playing as " + m.first : "Play as " + m.first;
        const ImVec2 ba = at(left + 20, y), bb = at(right - 20, y + 30);
        bool over = false;
        if (button(ba, bb, &over) && can) {
            action = kPlayAs;
            if (index) *index = m.index;
            g_playAsAt = now;
        }
        const ImU32 fade = can ? kWhite : IM_COL32(255, 255, 255, 120);
        if (over && can) nine(d, art::kDialogButtonOver, {ba.x - 2, ba.y - 3}, {bb.x + 2, bb.y + 6}, 48, 36, 48, 54, 0.6f);
        else nine(d, art::kDialogButton, ba, bb, 48, 36, 48, 42, 0.6f, fade);
        ImVec2 bs = g_text->CalcTextSizeA(26, FLT_MAX, 0, label.c_str());
        d->AddText(g_text, 26, {(ba.x + bb.x - bs.x) / 2, (ba.y + bb.y - bs.y) / 2 - 2},
                   can ? IM_COL32(30, 70, 128, 255) : IM_COL32(30, 70, 128, 130), label.c_str());
        y += 36;
        if (!m.here) {
            const char* why = "Only Sims on this lot can be played.";
            ImVec2 ws = g_text->CalcTextSizeA(19 * g_phoneText, FLT_MAX, 0, why);
            shadowText(d, g_text, 19 * g_phoneText, {at(115, 0).x - ws.x / 2, at(0, y).y}, kWhite, why);
            y += 12 * g_phoneText;
        }
    } else if (k == kSimHousehold) {
        // The household tray as a 2-wide grid of portraits (user 2026-10-23 21:35Z): the Sim you play ringed
        // in green, Sims not on the lot dimmed. Press one for its page.
        const float cw = (right - left - 6) / 2, ch = 118;
        const int n = static_cast<int>(g_sim.household.size());
        for (int i = 0; i < n; ++i) {
            const SimMember& m = g_sim.household[i];
            const float x = left + (i % 2) * (cw + 6), cy = y + (i / 2) * (ch + 6);
            const ImVec2 ca = at(x, cy), cb = at(x + cw, cy + ch);
            bool over = false;
            if (button(ca, cb, &over)) g_hhSim = i, g_mySimScroll = 0;
            d->AddRectFilled(ca, cb, IM_COL32(255, 255, 255, m.here ? 225 : 150), 6 * S);
            if (m.active) d->AddRect(ca, cb, IM_COL32(70, 170, 50, 255), 6 * S, 0, 2 * S);
            else if (over) d->AddRect(ca, cb, IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.4f * S);
            if (m.face) {
                shown.push_back(m.face);
                icon(d, m.face, at(x + cw / 2, cy + 44), 80 * S, m.here ? kWhite : IM_COL32(255, 255, 255, 120));
            }
            const float ns = 20 * g_phoneText;
            ImVec2 sz = g_text->CalcTextSizeA(ns, FLT_MAX, 0, m.first.c_str());
            const float fit = std::min(1.0f, (cw - 6) * S / std::max(1.0f, sz.x));
            d->AddText(g_text, ns * fit, {at(x + cw / 2, 0).x - sz.x * fit / 2, at(0, cy + 88).y}, kBlue, m.first.c_str());
            const char* sub = m.active ? "Playing" : m.here ? ageName(m.age) : "Away";
            ImVec2 ss = g_text->CalcTextSizeA(17 * g_phoneText, FLT_MAX, 0, sub);
            d->AddText(g_text, 17 * g_phoneText, {at(x + cw / 2, 0).x - ss.x / 2, at(0, cy + 101).y}, IM_COL32(80, 90, 110, 255), sub);
        }
        y += ((n + 1) / 2) * (ch + 6);
        if (!n) {
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, "...");
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, "...");
        }
    } else if (k == kSimNeeds) {
        Item items[kMaxItems];
        std::vector<Tip> tips;
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(g_itemMutex);
            count = std::min(kMaxItems, g_counts[kNeedItems]);
            std::copy(g_items[kNeedItems], g_items[kNeedItems] + count, items);
            tips = g_tips[kNeedItems];
        }
        const float rowH = 40 * std::max(1.0f, g_phoneText / 1.2f);
        for (int i = 0; i < count; ++i, y += rowH) {
            const Item& it = items[i];
            const ImVec2 ra = at(left, y + 2), rb = at(right, y + rowH - 2);
            d->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 225), 6 * S);
            const std::string& name = i < static_cast<int>(tips.size()) ? found(tips[i].json[0]) : std::string();
            // Our own HD icons for the six base needs (user 2026-10-24 01:58Z); the game's for any other.
            static const std::pair<const char*, const art::Piece*> kOwn[6] = {
                {"Hunger", &art::kNeedHunger}, {"Hygiene", &art::kNeedHygiene}, {"Bladder", &art::kNeedBladder},
                {"Energy", &art::kNeedEnergy}, {"Fun", &art::kNeedFun}, {"Social", &art::kNeedSocial}};
            const art::Piece* own = nullptr;
            for (const auto& o : kOwn)
                if (name == o.first) own = o.second;
            if (own) {
                const ImVec2 c = at(left + 18, y + rowH / 2);
                pieceIn(d, *own, {c.x - 16 * S, c.y - 16 * S}, {c.x + 16 * S, c.y + 16 * S});
            } else if (it.icon) {
                shown.push_back(it.icon);
                icon(d, it.icon, at(left + 18, y + rowH / 2), 32 * S);
            }
            d->AddText(g_text, ts, at(left + 38, y + 4), kBlue, name.c_str());
            const ImU32 back = (it.flags >> 24) ? fromArgb(it.flags) : IM_COL32(60, 66, 76, 255);
            bar(left + 38, right - 6, y + rowH - 16, 9, 0.07f + 0.93f * it.value, back, fromArgb(it.colour | 0xff000000u));
        }
        if (!count) {
            const char* none = "...";
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, none);
            shadowText(d, g_text, 26, {at(115, 180).x - ns.x / 2, at(115, 180).y - ns.y / 2}, kWhite, none);
        }
    }
    d->PopClipRect();
    // A swipe up or down scrolls (held back at the ends).
    const float total = y + g_mySimScroll - top, room = bottom - top;
    if (swipeY) g_mySimScroll = drag.sim - dy;
    g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
    g_scrolls0 = total > room;
    if (total > room) {
        const float thumbH = std::max(24.0f, room * room / total);
        const float thumbY = top + (room - thumbH) * g_mySimScroll / std::max(1.0f, total - room);
        d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
        pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
        const float t = sliderFollow(r, top, bottom, thumbH);
        if (t >= 0) g_mySimScroll = t * (total - room);
    }
}

// Contacts (user 2026-10-24 01:08Z, the My Sim Relationships app moved to the home screen): the Sims your
// Sim knows, most liked first, each its face, friendship and romance (the relationship panel's bars and
// their bits); a contact's page is what the game's social hover shows of them: their bust, the traits,
// likes, dislikes and turn-ons your Sim has learned (unknown ones as "?"), known career, major, net worth,
// romantic interest, your Sim's sentiments toward them and the other relationship bits.
template <class Button>
void drawContacts(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action, bool swipeY,
                  float dy, const PhoneDrag& drag, int* index) {
    constexpr float S = 2.5f;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(9, 73, 171, 255), kInk = IM_COL32(50, 60, 80, 255);
    const ULONGLONG now = GetTickCount64();
    if (now - g_contactsAsked > 2000 && action == kNone) action = kContactsOpened, g_contactsAsked = now;
    d->AddRectFilled(at(0, 0), at(230, 400), IM_COL32(18, 60, 120, 235));
    const bool page = g_contacts >= 1 && g_contactSel >= 0 && g_contactSel < static_cast<int>(g_sim.contacts.size());
    const char* head = page ? g_sim.contacts[g_contactSel].name.c_str() : "Contacts";
    pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
    {
        ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
        const float fit = std::min(1.0f, 200 * S / std::max(1.0f, hs.x));
        shadowText(d, g_title, 34 * fit, {at(115, 24).x - hs.x * fit / 2, at(115, 24).y - hs.y * fit / 2}, kWhite, head);
    }
    static bool g_scrolls1 = false;  // the list scrolled last frame: room for its slider, else rows to the edge
    const float top = 48, bottom = 354, left = 6, right = g_scrolls1 ? 206.0f : 224.0f;
    float y = top - g_mySimScroll;
    d->PushClipRect(at(0, top), at(230, bottom), true);
    // Buttons in the scrolled list only where it shows: a row under the Back button can't take its press.
    auto inList = [&](ImVec2 a, ImVec2 b, bool* over) {
        a.y = std::max(a.y, at(0, top).y);
        b.y = std::min(b.y, at(0, bottom).y);
        if (b.y - a.y < 1) {
            if (over) *over = false;
            return false;
        }
        return button(a, b, over);
    };
    auto bar = [&](float x0, float x1, float by, float h, float value, ImU32 pos, ImU32 neg) {
        // The relationship panel's bar: centred at 0, filling right (liked) or left (disliked).
        d->AddRectFilled(at(x0, by), at(x1, by + h), IM_COL32(200, 206, 214, 255), h / 2 * S);
        const float mid = (x0 + x1) / 2, v = std::max(-1.0f, std::min(1.0f, value / 100));
        if (v >= 0) d->AddRectFilled(at(mid, by), at(mid + (x1 - mid) * v, by + h), pos, h / 2 * S);
        else d->AddRectFilled(at(mid + (mid - x0) * v, by), at(mid, by + h), neg, h / 2 * S);
        d->AddLine(at(mid, by - 1), at(mid, by + h + 1), IM_COL32(90, 100, 115, 255), 0.8f * S);
    };
    const ImU32 kFriend = IM_COL32(88, 200, 56, 255), kFoe = IM_COL32(220, 70, 50, 255), kLove = IM_COL32(236, 90, 160, 255);
    const float small = 19 * g_phoneText;
    if (!page) {
        g_contacts = 0;
        // The filter (user 2026-10-24 17:54Z): a dropdown over a 2-wide grid of portrait cards as the Household
        // app's (your relationship's bars under the name, Sims not here dimmed). Only filters someone is in show.
        static const char* const kFilterNames[11] = {"All", "Household", "Family", "Friends", "Romance", "Rivals",
                                                    "Work", "School", "On this lot", "Clubs", "Pets"};
        static const char kFilterTags[11] = {0, 'H', 'F', 'f', 'R', 'E', 'W', 'S', 'L', 'C', 'P'};
        int counts[11] = {};
        for (const SimContact& k : g_sim.contacts) {
            ++counts[0];
            for (int i = 1; i < 11; ++i) counts[i] += k.tags.find(kFilterTags[i]) != std::string::npos;
        }
        if (g_contactFilter < 0 || g_contactFilter >= 11 || (g_contactFilter > 0 && !counts[g_contactFilter] && !g_sim.contacts.empty()))
            g_contactFilter = 0;
        {
            char label[48];
            std::snprintf(label, sizeof label, "%s (%d)", kFilterNames[g_contactFilter], counts[g_contactFilter]);
            const ImVec2 fa = at(left, y + 2), fb = at(right, y + 28);
            bool over = false;
            if (inList(fa, fb, &over)) g_contactFilterOpen = !g_contactFilterOpen;
            d->AddRectFilled(fa, fb, IM_COL32(255, 255, 255, over || g_contactFilterOpen ? 250 : 230), 13 * S);
            d->AddText(g_title, 22 * g_phoneText, at(left + 10, y + 15 - 6 * g_phoneText), kBlue, label);
            const ImVec2 c = at(right - 14, y + 15);  // the dropdown's arrow
            const float u = S * (g_contactFilterOpen ? -1.0f : 1.0f);
            d->AddTriangleFilled({c.x - 5 * S, c.y - 2.5f * u}, {c.x + 5 * S, c.y - 2.5f * u}, {c.x, c.y + 3.5f * u}, kBlue);
            y += 34;
        }
        if (g_contactFilterOpen) {
            for (int i = 0; i < 11; ++i) {
                if (i && !counts[i]) continue;
                char label[48];
                std::snprintf(label, sizeof label, "%s (%d)", kFilterNames[i], counts[i]);
                const ImVec2 ra = at(left + 8, y), rb = at(right - 8, y + 30);
                bool over = false;
                if (inList(ra, rb, &over)) g_contactFilter = i, g_contactFilterOpen = false;
                d->AddRectFilled(ra, rb, i == g_contactFilter ? IM_COL32(205, 222, 246, 245) : over ? IM_COL32(232, 241, 255, 245)
                                                                                                : IM_COL32(255, 255, 255, 225), 6 * S);
                d->AddText(g_text, 22 * g_phoneText, at(left + 18, y + 15 - 6 * g_phoneText), kBlue, label);
                y += 33;
            }
        } else {
            const float cw = (right - left - 6) / 2, ch = 128;
            int shownN = 0;
            for (int n = 0; n < static_cast<int>(g_sim.contacts.size()); ++n) {
                const SimContact& k = g_sim.contacts[n];
                if (g_contactFilter > 0 && k.tags.find(kFilterTags[g_contactFilter]) == std::string::npos) continue;
                const int i = shownN++;
                const float x = left + (i % 2) * (cw + 6), cy = y + (i / 2) * (ch + 6);
                if (cy + ch < top - 10 || cy > bottom + 10) continue;
                const ImVec2 ca = at(x, cy), cb = at(x + cw, cy + ch);
                bool over = false;
                if (inList(ca, cb, &over)) {
                    g_contacts = 1, g_contactSel = n, g_mySimScroll = 0;
                    action = kContactPicked;
                    if (index) *index = n;
                    g_phoneDataUntil = now + 1500;
                }
                d->AddRectFilled(ca, cb, IM_COL32(255, 255, 255, k.here ? 228 : 150), 6 * S);
                if (over) d->AddRect(ca, cb, kBlue, 6 * S, 0, 1.4f * S);
                if (k.face) {
                    shown.push_back(k.face);
                    icon(d, k.face, at(x + cw / 2, cy + 42), 76 * S, k.here ? kWhite : IM_COL32(255, 255, 255, 120));
                }
                const size_t space = k.name.find(' ');
                const std::string first = space == std::string::npos ? k.name : k.name.substr(0, space);
                const float ns = 20 * g_phoneText;
                ImVec2 sz = g_text->CalcTextSizeA(ns, FLT_MAX, 0, first.c_str());
                const float fit = std::min(1.0f, (cw - 6) * S / std::max(1.0f, sz.x));
                d->AddText(g_text, ns * fit, {at(x + cw / 2, 0).x - sz.x * fit / 2, at(0, cy + 84).y}, kBlue, first.c_str());
                std::string sub = k.familyBitJson.empty() ? found(k.friendBitJson) : found(k.familyBitJson);
                ImVec2 ss = g_text->CalcTextSizeA(16 * g_phoneText, FLT_MAX, 0, sub.c_str());
                const float sfit = std::min(1.0f, (cw - 6) * S / std::max(1.0f, ss.x));
                d->AddText(g_text, 16 * g_phoneText * sfit, {at(x + cw / 2, 0).x - ss.x * sfit / 2, at(0, cy + 97).y}, kInk, sub.c_str());
                bar(x + 8, x + cw - 8, cy + ch - (k.hasRomance ? 18 : 11), 4, k.friendship, kFriend, kFoe);
                if (k.hasRomance) bar(x + 8, x + cw - 8, cy + ch - 10, 4, k.romance, kLove, kFoe);
            }
            y += ((shownN + 1) / 2) * (ch + 6);
        }
        if (g_sim.contacts.empty()) {
            ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, "...");
            shadowText(d, g_text, 26, {at(115, 150).x - ns.x / 2, at(115, 150).y - ns.y / 2}, kWhite, "...");
        }
    } else {
        const SimContact& k = g_sim.contacts[g_contactSel];
        const ContactPage& c = g_sim.contact;
        const bool ready = c.index == g_contactSel;
        const uint64_t pic = ready && c.bust ? c.bust : k.face;
        const bool info = g_contacts == 2;  // the Info page: the details, without the portrait and buttons
        if (!info) {
            // The portrait: pressed, their pie menu in front of the phone (user 2026-10-24 17:59Z), when they're here.
            bool picOver = false;
            if (inList(at(63, y + 4), at(167, y + 108), &picOver)) {
                action = kContactPie;  // phonesim.contact_pie (a Sim not on the lot has none: it says so in the log)
                if (index) *index = g_contactSel;
                LOG("Phone: %s's portrait pressed (%s)", k.name.c_str(), k.here ? "here" : "not on this lot");
            }
            d->AddCircleFilled(at(115, y + 56), 52 * S, IM_COL32(255, 255, 255, picOver ? 110 : 60), 64);
            if (pic) {
                shown.push_back(pic);
                icon(d, pic, at(115, y + 56), 104 * S);
            }
            y += 114;
            if (k.tags.find('H') != std::string::npos) {
                // Play as (household members), as the Household app's.
                const bool busy = g_playAsAt && now - g_playAsAt < 2500, can = k.here && !busy;
                const size_t space = k.name.find(' ');
                const std::string label = "Play as " + (space == std::string::npos ? k.name : k.name.substr(0, space));
                const ImVec2 ba = at(left + 20, y), bb = at(right - 20, y + 30);
                bool over = false;
                if (inList(ba, bb, &over) && can) {
                    action = kContactPlayAs;
                    if (index) *index = g_contactSel;
                    g_playAsAt = now;
                }
                d->AddRectFilled(ba, bb, over && can ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, can ? 228 : 130), 8 * S);
                ImVec2 bs = g_title->CalcTextSizeA(24 * g_phoneText, FLT_MAX, 0, label.c_str());
                d->AddText(g_title, 24 * g_phoneText, {(ba.x + bb.x - bs.x) / 2, (ba.y + bb.y - bs.y) / 2}, can ? kBlue : IM_COL32(9, 73, 171, 120),
                           label.c_str());
                y += 36;
            }
            if (ready && (c.calls > 0 || c.texts > 0)) {
                // Call and Text (user 2026-10-25 02:07Z): each opens our pie in front of the phone with the phone's
                // Social entries of that kind the game offers with this Sim (phonesim.contact_menu); none: no button.
                const int kinds = (c.calls > 0) + (c.texts > 0);
                const float bw = (right - left - 20 - 6 * (kinds - 1)) / kinds;
                float bx = left + 10;
                for (int kind = 0; kind < 2; ++kind) {
                    if ((kind ? c.texts : c.calls) <= 0) continue;
                    const ImVec2 ba = at(bx, y), bb = at(bx + bw, y + 30);
                    bool over = false;
                    if (inList(ba, bb, &over)) {
                        action = kContactCall;
                        if (index) *index = kind;
                    }
                    d->AddRectFilled(ba, bb, over ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, 228), 8 * S);
                    const char* label = kind ? "Text" : "Call";
                    ImVec2 ls = g_title->CalcTextSizeA(24 * g_phoneText, FLT_MAX, 0, label);
                    d->AddText(g_title, 24 * g_phoneText, {(ba.x + bb.x - ls.x) / 2, (ba.y + bb.y - ls.y) / 2}, kBlue, label);
                    bx += bw + 6;
                }
                y += 36;
            }
            // Info (user 2026-10-25 02:15Z): the rest of the page (bars, the game's relationship tooltip,
            // likes) on its own page, so this one stays short.
            {
                const ImVec2 ba = at(115 - 45, y), bb = at(115 + 45, y + 30);
                bool over = false;
                if (inList(ba, bb, &over)) g_contacts = 2, g_mySimScroll = 0;
                d->AddRectFilled(ba, bb, over ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, 228), 8 * S);
                ImVec2 ls = g_title->CalcTextSizeA(24 * g_phoneText, FLT_MAX, 0, "Info");
                d->AddText(g_title, 24 * g_phoneText, {(ba.x + bb.x - ls.x) / 2, (ba.y + bb.y - ls.y) / 2}, kBlue, "Info");
                y += 36;
            }
        } else {
            auto card = [&](float h) {
                d->AddRectFilled(at(left, y), at(right, y + h), IM_COL32(255, 255, 255, 228), 6 * S);
            };
            // The bars with their bits.
            {
                const float h = k.hasRomance ? 46 : 26;
                card(h);
                d->AddText(g_text, small, at(left + 6, y + 3), kInk, found(k.friendBitJson).c_str());
                bar(left + 6, right - 6, y + 16, 5, k.friendship, kFriend, kFoe);
                if (k.hasRomance) {
                    d->AddText(g_text, small, at(left + 6, y + 23), kInk, found(k.romanceBitJson).c_str());
                    bar(left + 6, right - 6, y + 36, 5, k.romance, kLove, kFoe);
                }
                y += h + 6;
            }
            auto section = [&](const char* title) {
                shadowText(d, g_title, 24 * g_phoneText, at(left + 2, y), kWhite, title);
                y += 13 * g_phoneText + 3;
            };
            if (!ready) {
                ImVec2 ns = g_text->CalcTextSizeA(26, FLT_MAX, 0, "...");
                shadowText(d, g_text, 26, {at(115, 0).x - ns.x / 2, at(0, y + 10).y}, kWhite, "...");
                y += 30;
            } else {
                // The game's relationship tooltip (RelationshipTooltip, user 2026-10-24 02:59Z: the hover popup on the
                // page), stacked as UpdateDataAndReposition stacks it, in its own pixels (272 wide, mark 02-59-48):
                // name; age; known careers (company, next hours, work days); Traits: three and "+ N more";
                // Compatibility:; Attraction:. Then likes and turn-ons the same way, and the bits.
                const float tk = (right - left) / 272.0f;  // phone units a tooltip pixel
                auto tp = [&](float size) { return size * tk * S; };
                const ImU32 kNavy = IM_COL32(28, 44, 82, 255), kHead = IM_COL32(66, 140, 214, 255), kDesc = IM_COL32(36, 108, 196, 255),
                            kHours = IM_COL32(52, 156, 36, 255), kOff = IM_COL32(170, 176, 184, 255);
                const float x0 = left + 13 * tk, xText = left + 63 * tk, wrapW = (right - left - 72 * tk) * S;
                const float cardTop = y;
                static float cardH = 300;
                d->AddRectFilledMultiColor(at(left, cardTop), at(right, cardTop + cardH), IM_COL32(255, 255, 255, 250),
                                           IM_COL32(255, 255, 255, 250), IM_COL32(234, 238, 244, 250), IM_COL32(234, 238, 244, 250));
                d->AddRect(at(left, cardTop), at(right, cardTop + cardH), IM_COL32(200, 208, 220, 255), 4 * S, 0, 0.8f * S);
                y += 12 * tk;
                auto text = [&](ImFont* f, float size, ImU32 col, float x, const std::string& t, float wrap = 0) {
                    const float h = f->CalcTextSizeA(tp(size), FLT_MAX, wrap, t.c_str()).y / S;
                    d->AddText(f, tp(size), at(x, y), col, t.c_str(), nullptr, wrap);
                    return h;
                };
                auto frame = [&](float fy, uint64_t ic) {  // the icon on its rounded light square
                    const ImVec2 fa = at(x0, fy), fb = at(x0 + 44 * tk, fy + 44 * tk);
                    d->AddRectFilledMultiColor(fa, fb, IM_COL32(246, 249, 252, 255), IM_COL32(246, 249, 252, 255),
                                               IM_COL32(214, 224, 236, 255), IM_COL32(214, 224, 236, 255));
                    d->AddRect(fa, fb, IM_COL32(196, 206, 220, 255), 5 * tk * S, 0, 0.7f * S);
                    if (ic) {
                        shown.push_back(ic);
                        icon(d, ic, at(x0 + 22 * tk, fy + 22 * tk), 40 * tk * S);
                    }
                };
                auto row = [&](uint64_t ic, const std::string& title, const std::string& desc, ImU32 descCol) {
                    const float ry = y;
                    frame(ry, ic);
                    y = ry + (desc.empty() ? 13 * tk : 5 * tk);
                    y += text(g_title, 15, kNavy, xText, title, wrapW) + 2 * tk;
                    if (!desc.empty()) y += text(g_text, 13, descCol, xText, desc, wrapW);
                    y = std::max(y, ry + 44 * tk) + 8 * tk;
                };
                auto header = [&](const char* t) {
                    y += 4 * tk;
                    y += text(g_text, 14, kHead, x0, t) + 4 * tk;
                };
                y += text(g_title, 17, kNavy, x0, k.name) + 6 * tk;
                if (c.age.icon || !c.age.nameJson.empty()) row(c.age.icon, found(c.age.nameJson), std::string(), kDesc);
                for (const auto& j : c.jobs) {
                    const float ry = y;
                    frame(ry, j.icon);
                    y += 4 * tk;
                    y += text(g_title, 15, kNavy, xText, found(j.name), wrapW) + 1 * tk;
                    if (!j.company.empty()) y += text(g_text, 14, kDesc, xText, "at " + found(j.company), wrapW) + 1 * tk;
                    if (!j.hours.empty()) y += text(g_title, 15, kHours, xText, j.hours) + 1 * tk;
                    if (j.days.size() == 7) {
                        static const char* const kDay[7] = {"S", "M", "T", "W", "T", "F", "S"};
                        float dx = xText;
                        for (int n = 0; n < 7; ++n) {
                            const bool on = j.days[n] == '1';
                            d->AddText(on ? g_title : g_text, tp(13), at(dx, y), on ? kNavy : kOff, kDay[n]);
                            dx += (g_title->CalcTextSizeA(tp(13), FLT_MAX, 0, kDay[n]).x / S) + 1.5f * tk;
                        }
                        y += 16 * tk;
                    }
                    y = std::max(y, ry + 44 * tk) + 8 * tk;
                }
                {
                    int count = 0;
                    for (const auto& t : c.traits) count += t.group == "Traits";
                    if (count) {
                        header("Traits:");
                        int drawn = 0;
                        for (const auto& t : c.traits) {
                            if (t.group != "Traits" || drawn >= 3) continue;
                            row(t.icon, found(t.nameJson), std::string(), kDesc);
                            ++drawn;
                        }
                        if (count > 3) {
                            char more[32];
                            std::snprintf(more, sizeof more, "+ %d more", count - 3);
                            y += text(g_text, 14, kDesc, x0, more) + 4 * tk;
                        }
                    }
                }
                if (c.compatibility.icon) {
                    header("Compatibility:");
                    row(c.compatibility.icon, found(c.compatibility.nameJson), found(c.compatibility.textJson), kDesc);
                }
                if (c.attraction.icon) {
                    header("Attraction:");
                    row(c.attraction.icon, found(c.attraction.nameJson), found(c.attraction.textJson), kDesc);
                }
                const char* more[] = {"Likes", "Dislikes", "Turn-ons", "Turn-offs", "Also"};
                for (const char* g : more) {
                    int count = 0;
                    for (const auto& t : c.traits) count += t.group == g;
                    if (!count) continue;
                    header((std::string(g) + ":").c_str());
                    for (const auto& t : c.traits)
                        if (t.group == g) row(t.icon, found(t.nameJson), std::string(), kDesc);
                }
                if (!c.bits.empty()) {
                    header("Relationships:");
                    for (const auto& b : c.bits) row(b.icon, found(b.nameJson), std::string(), kDesc);
                }
                y += 4 * tk;
                cardH = y - cardTop;
                y += 8;
                // The relationship panel's sentiments box (SimInfoRelationshipPanel): "Sentiments about <first name>"
                // and its sentiments, or the game's line for none.
                {
                    const float boxTop = y;
                    static float boxH = 60;
                    d->AddRectFilled(at(left, boxTop), at(right, boxTop + boxH), IM_COL32(226, 227, 229, 245), 4 * S);
                    const std::string first = k.name.substr(0, k.name.find(' '));
                    const std::string title = "Sentiments about " + first;
                    y += 8 * tk;
                    ImVec2 ts = g_title->CalcTextSizeA(tp(16), FLT_MAX, 0, title.c_str());
                    d->AddText(g_title, tp(16), {at(115, 0).x - ts.x / 2, at(0, y).y}, kDesc, title.c_str());
                    y += ts.y / S + 10 * tk;
                    if (c.sentiments.empty()) {
                        const char* none = "No Sentiments at the moment. Spend time together and see what happens!";
                        const float wrap = (right - left - 30 * tk) * S;
                        // Centred line by line, as the game's centred text field.
                        const char* p = none;
                        while (*p) {
                            const char* e = g_text->CalcWordWrapPositionA(tp(14) / g_text->FontSize, p, none + std::strlen(none), wrap);
                            if (e == p) e = p + 1;
                            ImVec2 ls = g_text->CalcTextSizeA(tp(14), FLT_MAX, 0, p, e);
                            d->AddText(g_text, tp(14), {at(115, 0).x - ls.x / 2, at(0, y).y}, IM_COL32(60, 64, 72, 255), p, e);
                            y += ls.y / S;
                            p = e;
                            while (*p == ' ') ++p;
                        }
                    } else {
                        for (const auto& b : c.sentiments) row(b.icon, found(b.nameJson), b.textJson.empty() ? std::string() : found(b.textJson), kDesc);
                    }
                    y += 12 * tk;
                    boxH = y - boxTop;
                    y += 8;
                }
            }
            if (ready && !c.actions.empty()) {
                // The Social app's entries for this Sim (user 2026-10-24 01:17Z), two to a row; its picker is answered
                // with this Sim (phonesim.auto_pick_option).
                section("Social");
                if (!c.note.empty()) {
                    // An action the game doesn't offer with them: no picker came up (phonesim.auto_pick_option).
                    const float ns = 20 * g_phoneText, wrap = (right - left - 4) * S;
                    ImVec2 nz = g_text->CalcTextSizeA(ns, FLT_MAX, wrap, c.note.c_str());
                    shadowText(d, g_text, ns, at(left + 2, y), IM_COL32(255, 210, 120, 255), c.note.c_str());
                    y += nz.y / S + 4;
                }
                static ULONGLONG pressedAt = 0;
                static int pressedNode = -1;
                const float bw = (right - left - 6) / 2, bh = 28 * std::max(1.0f, g_phoneText / 1.2f);
                for (size_t i = 0; i < c.actions.size(); ++i) {
                    const auto& a = c.actions[i];
                    const float bx = left + (i % 2) * (bw + 6), byy = y + (i / 2) * (bh + 4);
                    const ImVec2 ba = at(bx, byy), bb = at(bx + bw, byy + bh);
                    const bool off = !a.disabledJson.empty() && !found(a.disabledJson).empty();
                    const bool busy = pressedNode == a.node && now - pressedAt < 1500;
                    bool over = false;
                    if (inList(ba, bb, &over) && !off) {
                        action = kContactAction;
                        if (index) *index = a.node;
                        pressedNode = a.node, pressedAt = now;
                    }
                    d->AddRectFilled(ba, bb, busy ? IM_COL32(205, 235, 190, 240) : IM_COL32(255, 255, 255, off ? 150 : 232), 6 * S);
                    if (over && !off) d->AddRect(ba, bb, kBlue, 6 * S, 0, 1.4f * S);
                    const std::string& name = found(a.nameJson);
                    const float ts = 20 * g_phoneText, wrap = (bw - 6) * S;
                    ImVec2 sz = g_text->CalcTextSizeA(ts, FLT_MAX, wrap, name.c_str());
                    d->AddText(g_text, ts, {ba.x + 3 * S, (ba.y + bb.y - sz.y) / 2}, off ? IM_COL32(120, 128, 140, 255) : kBlue,
                               name.c_str(), nullptr, wrap);
                }
                y += ((c.actions.size() + 1) / 2) * (bh + 4) + 4;
            }
        }
    }
    d->PopClipRect();
    const float total = y + g_mySimScroll - top, room = bottom - top;
    if (swipeY) g_mySimScroll = drag.sim - dy;
    g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
    g_scrolls1 = total > room;
    if (total > room) {
        const float thumbH = std::max(24.0f, room * room / total);
        const float thumbY = top + (room - thumbH) * g_mySimScroll / std::max(1.0f, total - room);
        d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
        pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
        const float t = sliderFollow(r, top, bottom, thumbH);
        if (t >= 0) g_mySimScroll = t * (total - room);
    }
}

// Weather (user 2026-10-24 02:48Z: identical to the game's weather overlay): the game's weather and season
// tooltip (WeatherAndSeasonTooltip, TimeControls SWF art), filling the phone's width and scrolling: the season
// header with its day, the four season icons and the season bar, Current Weather (weather, temperature),
// Today's Forecast, Current Lunar Phase (and its effect), Eco Footprint. Laid out in the tooltip's own pixels
// (247 wide), measured from the game's (mark 2026-10-24 04-48-22); stacked as SetData stacks them.
template <class Button>
void drawWeather(ImDrawList* d, const Rect& r, Button& button, std::vector<uint64_t>& shown, Action& action, bool swipeY,
                 float dy, const PhoneDrag& drag) {
    constexpr float S = 2.5f;
    const float k = 230.0f / 247;  // phone units a tooltip pixel
    const float top = 0, bottom = 354;
    static ULONGLONG asked = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - asked > 2000 && action == kNone) action = kMySimOpened, asked = now;
    const SimWeather& w = g_sim.weather;
    const float y0 = top - g_mySimScroll;
    auto at = [&](float x, float y) { return ImVec2{r.x + x * k * S, r.y + (y0 + y * k) * S}; };
    auto px = [&](float size) { return size * k * S; };  // a tooltip font size in atlas pixels
    const ImU32 kSection = IM_COL32(48, 96, 168, 255), kName = IM_COL32(34, 62, 118, 255), kGrey = IM_COL32(120, 120, 120, 255);
    d->PushClipRect(ImVec2{r.x, r.y + top * S}, ImVec2{r.x + 230 * S, r.y + bottom * S}, true);
    float y = 0;
    auto centred = [&](ImFont* f, float size, ImU32 col, float cy, const std::string& t) {
        ImVec2 ts = f->CalcTextSizeA(px(size), FLT_MAX, 0, t.c_str());
        d->AddText(f, px(size), {at(123.5f, 0).x - ts.x / 2, at(0, cy).y - ts.y / 2}, col, t.c_str());
    };
    auto left = [&](ImFont* f, float size, ImU32 col, float x, float cy, const std::string& t) {
        ImVec2 ts = f->CalcTextSizeA(px(size), FLT_MAX, 0, t.c_str());
        d->AddText(f, px(size), {at(x, 0).x, at(0, cy).y - ts.y / 2}, col, t.c_str());
    };
    // The tooltip's card (TooltipBase: white to light grey, rounded).
    const float bodyTop = 0;
    float height = 520;
    {
        static float lastHeight = 520;
        height = lastHeight;
        const ImVec2 a = at(0, bodyTop), b = at(247, height);
        d->AddRectFilledMultiColor(a, b, IM_COL32(254, 253, 253, 255), IM_COL32(254, 253, 253, 255), IM_COL32(238, 238, 238, 255),
                                   IM_COL32(238, 238, 238, 255));
        d->AddRect(a, b, IM_COL32(205, 205, 205, 255), 4 * S, 0, 1.0f * S);
        if (!w.sent) {
            centred(g_text, 16, kName, 60, "...");
            d->PopClipRect();
            return;
        }
        static const char* const kSeasons[4] = {"Summer", "Fall", "Winter", "Spring"};
        static const ImU32 kSeasonInk[4] = {IM_COL32(196, 108, 16, 255), IM_COL32(170, 78, 30, 255), IM_COL32(40, 108, 186, 255),
                                            IM_COL32(170, 36, 124, 255)};
        if (w.season >= 0 && w.season < 4) {
            const art::Piece* heads[4] = {&art::kWxHeaderSummer, &art::kWxHeaderFall, &art::kWxHeaderWinter, &art::kWxHeaderSpring};
            pieceIn(d, *heads[w.season], at(0, 0), at(247, 74));
            centred(g_title, 17, kSeasonInk[w.season], 23, kSeasons[w.season]);
            if (w.length > 0) {
                char day[48];
                std::snprintf(day, sizeof day, "Day %d of %d", w.day, w.length);
                centred(g_text, 13, kSeasonInk[w.season], 41, day);
            }
            // Spring, summer, fall, winter (mSeasonDisplayOrder), their icons over the bar's quarters.
            const int order[4] = {3, 0, 1, 2};
            const art::Piece* icons[4] = {&art::kWxSeasonSummer, &art::kWxSeasonFall, &art::kWxSeasonWinter, &art::kWxSeasonSpring};
            const art::Piece* fills[4] = {&art::kWxBarSummer, &art::kWxBarFall, &art::kWxBarWinter, &art::kWxBarSpring};
            const float barX = 18, barW = 211, quarter = barW / 4;
            int current = 0;
            for (int i = 0; i < 4; ++i)
                if (order[i] == w.season) current = i;
            for (int i = 0; i < 4; ++i) {
                const float cx = barX + quarter * (i + 0.5f);
                pieceIn(d, *icons[order[i]], at(cx - 13, 51), at(cx + 13, 77));
            }
            pieceIn(d, art::kWxBarBack, at(barX, 86), at(barX + barW, 104));
            for (int i = 0; i < 4; ++i) {
                const float done = i < current ? 1.0f : i > current ? 0.0f
                                 : w.length > 0 ? std::min(1.0f, static_cast<float>(w.day) / w.length) : 0.0f;
                if (done > 0) pieceIn(d, *fills[order[i]], at(barX + quarter * i, 86), at(barX + quarter * (i + done), 104));
            }
            pieceIn(d, art::kWxBarFrame, at(barX - 5, 81), at(barX + barW + 5, 109));
            y = 112;
        } else {
            y = 10;
        }
        auto row = [&](uint64_t ic, const art::Piece* art, const std::string& name, float& yy) {
            if (ic) {
                shown.push_back(ic);
                icon(d, ic, at(33, yy + 24), px(48));
            } else if (art) {
                pieceIn(d, *art, at(9, yy), at(57, yy + 48));
            }
            left(g_text, 13, kName, 65, yy + 24, name);
            yy += 54;
        };
        if (w.enabled) {
            left(g_text, 12, kSection, 11, y + 8, "Current Weather:");
            y += 18;
            if (w.icon[0] || !w.nameJson[0].empty()) row(w.icon[0], nullptr, found(w.nameJson[0]), y);
            if (w.icon[1] || !w.nameJson[1].empty()) row(w.icon[1], nullptr, found(w.nameJson[1]), y);
            left(g_text, 12, kSection, 11, y + 4, "Today's Forecast:");
            y += 14;
            if (w.icon[2] || !w.nameJson[2].empty()) row(w.icon[2], nullptr, found(w.nameJson[2]), y);
            y += 6;
        }
        if (w.phase >= 0 && w.phase < 8) {
            static const char* const kPhases[8] = {"New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
                                                   "Full Moon", "Waning Gibbous", "Third Quarter", "Waning Crescent"};
            const art::Piece* moons[8] = {&art::kMoon0, &art::kMoon1, &art::kMoon2, &art::kMoon3,
                                          &art::kMoon4, &art::kMoon5, &art::kMoon6, &art::kMoon7};
            left(g_text, 12, kSection, 11, y + 8, "Current Lunar Phase:");
            y += 18;
            const std::string effect = w.effectJson.empty() ? std::string() : found(w.effectJson);
            pieceIn(d, *moons[w.phase], at(9, y), at(57, y + 48));
            if (effect.empty()) {
                left(g_text, 13, kName, 65, y + 24, kPhases[w.phase]);
                y += 54;
            } else {
                left(g_text, 13, kName, 65, y + 10, kPhases[w.phase]);
                const float wrap = px(247 - 65 - 10);
                ImVec2 es = g_text->CalcTextSizeA(px(10), FLT_MAX, wrap, effect.c_str());
                d->AddText(g_text, px(10), at(65, y + 20), kSection, effect.c_str(), nullptr, wrap);
                y += std::max(54.0f, 22 + es.y / (k * S) + 6);
            }
        }
        if (w.eco >= 0 && w.eco <= 2) {
            left(g_text, 12, kSection, 17, y + 8, "Eco Footprint");
            y += 22;
            // EcoProgressBarContainer: the frame, the state's fill from the middle (neutral) or the end, its handle.
            const float bx0 = 27, bx1 = 212, by0 = y + 4, by1 = y + 24, mid = (bx0 + bx1) / 2, p = std::max(0.0f, std::min(1.0f, w.ecoProgress));
            pieceIn(d, art::kEcoBack, at(bx0, by0), at(bx1, by1));
            float hx = mid;
            if (w.eco == 1) {
                pieceIn(d, art::kEcoNeutralGreen, at(bx0, by0), at(mid, by1));
                pieceIn(d, art::kEcoNeutralIndustrial, at(mid, by0), at(bx1, by1));
                hx = bx0 + (bx1 - bx0) * p;
            } else if (w.eco == 0) {
                pieceIn(d, art::kEcoGreen, at(bx0, by0), at(bx0 + (bx1 - bx0) * (1 - p), by1));
                hx = bx0 + (bx1 - bx0) * (1 - p);
            } else {
                pieceIn(d, art::kEcoIndustrial, at(bx0, by0), at(bx0 + (bx1 - bx0) * p, by1));
                hx = bx0 + (bx1 - bx0) * p;
            }
            nine(d, art::kEcoFrame, at(bx0 - 4, by0 - 4), at(bx1 + 4, by1 + 4), 45, 45, 45, 45, k * S / 3);
            if (w.ecoDelta > 0) {  // heading industrial: the yellow arrow at the right end
                const ImVec2 c = at(bx1 - 10, (by0 + by1) / 2);
                d->AddTriangleFilled({c.x - 4 * S, c.y - 5 * S}, {c.x - 4 * S, c.y + 5 * S}, {c.x + 5 * S, c.y}, IM_COL32(240, 196, 40, 255));
            } else if (w.ecoDelta < 0) {
                const ImVec2 c = at(bx0 + 10, (by0 + by1) / 2);
                d->AddTriangleFilled({c.x + 4 * S, c.y - 5 * S}, {c.x + 4 * S, c.y + 5 * S}, {c.x - 5 * S, c.y}, IM_COL32(96, 196, 72, 255));
            }
            const art::Piece& handle = w.eco == 0 ? art::kEcoHandleGreen : w.eco == 2 ? art::kEcoHandleIndustrial : art::kEcoHandleNeutral;
            pieceIn(d, handle, at(hx - 20, (by0 + by1) / 2 - 20), at(hx + 20, (by0 + by1) / 2 + 20));
            y = by1 + 6;
            centred(g_title, 13, kName, y + 8, found(w.ecoNameJson));
            y += 16;
            const std::string text = found(w.ecoTextJson);
            const float wrap = px(220);
            ImVec2 ts = g_text->CalcTextSizeA(px(10), FLT_MAX, wrap, text.c_str());
            d->AddText(g_text, px(10), {at(123.5f, 0).x - ts.x / 2, at(0, y).y}, kGrey, text.c_str(), nullptr, wrap);
            y += ts.y / (k * S) + 8;
        }
        lastHeight = y + 6;
    }
    d->PopClipRect();
    const float total = (height + 4) * k, room = bottom - top;
    if (swipeY) g_mySimScroll = drag.sim - dy;
    g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
}

// The game's phone screen (SimInfoTray: PhonePanel's PhoneCategoryScreen and
// PhoneInteractionScreen, their places from the SWF), 2.5 atlas pixels to the
// game's one. The background picture is the phone's glass (phone3d), not drawn here.
// A menu text (wide) for ImGui.
std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

Action drawPhone(ImDrawList* d, int* index) {
    const PhoneContent& p = g_phone;
    const Rect r = kPhone;
    constexpr float S = 2.5f, A = S / art::kScale;  // atlas pixels per game pixel; art scale
    auto at = [&](float x, float y) { return ImVec2{r.x + x * S, r.y + y * S}; };
    const ImU32 kBlue = IM_COL32(9, 73, 171, 255), kGrey = IM_COL32(120, 128, 140, 255);
    Action action = kNone;
    int id = 0;
    // Swipes: a press that moves 6 game pixels is a swipe (along its main
    // direction), followed while held; its release presses nothing.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    PhoneDrag& drag = g_drag;
    const bool blocked = g_cover.blocked || g_phoneAsleep.load();  // the cover is up: what is under it takes no presses
    if (blocked) drag = PhoneDrag{};
    if (!blocked && ImGui::IsMouseClicked(0) && mouse.x >= r.x && mouse.x <= r.x + r.w && mouse.y >= r.y && mouse.y <= r.y + r.h) {
        drag = PhoneDrag{};
        drag.on = true;
        drag.start = mouse;
        drag.startY = (mouse.y - r.y) / S;
        drag.page = g_phonePage, drag.scroll = g_phoneScroll, drag.notes = g_notesScroll, drag.cal = g_calScroll,
        drag.week = g_calWeek, drag.sim = g_mySimScroll;
    }
    const float dx = drag.on ? (mouse.x - drag.start.x) / S : 0, dy = drag.on ? (mouse.y - drag.start.y) / S : 0;
    if (drag.on && !drag.moved && std::max(std::fabs(dx), std::fabs(dy)) > 6) {
        drag.moved = true;
        drag.horizontal = std::fabs(dx) > std::fabs(dy);
    }
    const bool swiping = drag.on && drag.moved, swipeEnded = swiping && !io.MouseDown[0];
    const bool onSlider = drag.on && (drag.start.x - r.x) / S >= 203;  // a press on a list's slider moves its thumb (sliderFollow)
    const bool swipeX = swiping && drag.horizontal, swipeY = swiping && !drag.horizontal && !onSlider;
    g_phoneSwipeX = swipeX, g_phoneSwipeEnd = swipeEnded, g_phoneDx = dx;
    g_hoverKey = g_hoverKeySeen, g_hoverKeySeen = 0;
    auto button = [&](ImVec2 a, ImVec2 b, bool* hover, bool* held = nullptr) {
        // The footer (Back and the page dots, under y 356) is theirs alone: anything else reaching into it, a
        // row scrolled down or a card, stops at its top, so it never takes their presses (user 2026-10-24 13:32Z).
        const float footer = r.y + 356 * S;
        const bool footerButton = a.y >= footer - 0.5f * S && b.y <= r.y + 400 * S && b.x - a.x <= 40 * S;
        if (b.y > footer && !footerButton) b.y = footer;
        if (b.y - a.y < 1) {
            ++id;
            if (hover) *hover = false;
            if (held) *held = false;
            return false;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(3000 + id++);
        bool pressed = ImGui::InvisibleButton("phone", {std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)});
        if (swiping || blocked) pressed = false;
        if (pressed) ++g_phoneTaps;  // the phone's beep (phonesound.click), never for a swipe or a held slider
        if (!blocked && !swiping && ImGui::IsItemHovered()) {
            // Pointed at something new: the phone's tap sound (user 2026-10-24 12:36Z), not while scrolling.
            const uint32_t key = ((static_cast<uint32_t>(a.x - r.x) * 73856093u) ^ (static_cast<uint32_t>(a.y - r.y) * 19349663u) ^
                                  (static_cast<uint32_t>(b.x - a.x) * 83492791u)) | 1u;
            if (key != g_hoverKey && g_hoverKeySeen == 0) ++g_phoneHovers;
            g_hoverKeySeen = key;
        }
        if (hover) *hover = !blocked && ImGui::IsItemHovered();
        if (held) *held = !blocked && ImGui::IsItemActive();
        ImGui::PopID();
        return pressed;
    };
    std::vector<uint64_t> shown;
    // The home screen's header (PhoneCategoryScreen): the Sounds button on its pill, the wallpaper colour button,
    // rearrange (home only) and the notifications bell; My Sim's grid page has it too (user 2026-10-24 16:10Z).
    const float cs = 22 * 0.96f;  // btnBackgroundColorSelector (39, 10, 0.96)
    auto phoneHeader = [&](bool rearrange) {
        const bool colors = !p.colors.empty();
        const bool open = g_phoneColorsOpen;  // the selector takes the presses while it is up
        nine(d, art::kPhonePill, at(8, 8), at(63, 33), 37, 37, 37, 37, A);
        bool hover = false;
        if (button(at(9, 9), at(31, 31), &hover) && !open) g_phoneSoundsOpen = true, g_mySimScroll = 0;  // the Sounds screen
        const art::Piece& silence = p.silenced ? (hover ? art::kPhoneSilencedOver : art::kPhoneSilenced)
                                               : (hover ? art::kPhoneSilenceOver : art::kPhoneSilence);
        piece(d, silence, at(20, 20), A);
        // btnBackgroundColorSelector (39, 10, 0.96): a toggle showing the colour selector.
        bool colorHover = false;
        if (colors && button(at(39, 10), at(39 + cs, 10 + cs), &colorHover)) g_phoneColorsOpen = !g_phoneColorsOpen;
        if (colors)
            pieceIn(d, g_phoneColorsOpen ? art::kPhoneColorBtnDown : colorHover ? art::kPhoneColorBtnOver : art::kPhoneColorBtn,
                    at(39, 10), at(39 + cs, 10 + cs));
        {
            // The notifications: a bell in the header's top right (user 2026-10-23 23:55Z, where the
            // battery was), the number unread on its lower right (the game's red badge).
            // Rearrange (user 2026-10-24 02:06Z): left of the bell; press, move apps, press again when done.
            if (rearrange) {
                bool over = false;
                if (button(at(166, 9), at(188, 31), &over) && !open) {
                    g_reorder = !g_reorder;
                    g_reorderPick.clear();
                }
                const ImVec2 c = at(177, 20);
                const ImU32 ink = g_reorder ? IM_COL32(120, 230, 70, 255) : over ? IM_COL32(186, 240, 120, 255) : IM_COL32(240, 244, 248, 255);
                for (int gy = 0; gy < 2; ++gy)
                    for (int gx = 0; gx < 2; ++gx)
                        d->AddRectFilled({c.x + (gx ? 1.2f : -7.2f) * S, c.y + (gy ? 1.2f : -7.2f) * S},
                                         {c.x + (gx ? 7.2f : -1.2f) * S, c.y + (gy ? 7.2f : -1.2f) * S}, ink, 1.5f * S);
            }
            bool bellHover = false;
            if (button(at(194, 9), at(216, 31), &bellHover) && !open) {
                g_phoneNotesOpen = true;
                g_notesScroll = 0;
                action = kNotesOpened;
            }
            const ImU32 ink = bellHover ? IM_COL32(186, 240, 120, 255) : IM_COL32(240, 244, 248, 255);
            const ImVec2 c = at(205, 19);
            const float u = S;  // one game pixel
            // The bell: a dome over a flaring skirt, its rim, the clapper.
            d->PathArcTo({c.x, c.y - 2 * u}, 4.6f * u, 3.14159f, 2 * 3.14159f, 14);
            d->PathLineTo({c.x + 5.4f * u, c.y + 4.2f * u});
            d->PathLineTo({c.x + 7.2f * u, c.y + 5.6f * u});
            d->PathLineTo({c.x - 7.2f * u, c.y + 5.6f * u});
            d->PathLineTo({c.x - 5.4f * u, c.y + 4.2f * u});
            d->PathFillConvex(ink);
            d->AddRectFilled({c.x - 4.6f * u, c.y - 2.2f * u}, {c.x + 4.6f * u, c.y + 4.4f * u}, ink);
            d->AddCircleFilled({c.x, c.y - 7.2f * u}, 1.1f * u, ink);
            d->AddCircleFilled({c.x, c.y + 7.0f * u}, 1.5f * u, ink);
            if (g_notes.unread > 0) {
                char count[8];
                std::snprintf(count, sizeof count, "%d", std::min(99, g_notes.unread));
                const ImVec2 b = at(212, 27);
                ImVec2 ns = g_small->CalcTextSizeA(22, FLT_MAX, 0, count);
                const float rad = std::max(6.0f * u, ns.x / 2 + 3 * u);
                d->AddCircleFilled(b, rad + 1.2f * u, IM_COL32(255, 255, 255, 255));
                d->AddCircleFilled(b, rad, IM_COL32(214, 34, 34, 255));
                d->AddText(g_small, 22, {b.x - ns.x / 2, b.y - ns.y / 2}, kWhite, count);
            }
        }
    };
    // PhoneBackgroundColorSelector, under the header's colour button while it is up.
    auto phoneColorSelector = [&]() {
        if (g_phoneColorsOpen) {
            // PhoneBackgroundColorSelector: its panel (mcBg), the arrow to its
            // button (mcArrow) and the swatches (VariationGrid, 8 a row, margins
            // 1,1,2,2 and 3,3,5,2). The game puts it above the phone; on our
            // screen it opens under the button, its arrow pointing up at it.
            const int n = static_cast<int>(p.colors.size()), cols = 8, rows = (n + cols - 1) / cols;
            const float sw = 24, cell = sw + 3, gw = cols * cell + 5, gh = rows * cell + 8;
            const float px = (230 - gw) / 2, py = 44;
            nine(d, art::kPhonePill, at(px, py), at(px + gw, py + gh), 37, 37, 37, 37, A * 0.6f);
            {
                const art::Piece& a = art::kPhoneSelectorArrow;  // drawn pointing down: flipped
                const ImVec2 c = at(39 + cs / 2, py - 5);
                const float w = 22 * 0.727f * S, h = 17 * 0.706f * S;
                d->AddImage(reinterpret_cast<ImTextureID>(g_art), {c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2},
                            {static_cast<float>(a.x) / art::kAtlasW, static_cast<float>(a.y + a.h) / art::kAtlasH},
                            {static_cast<float>(a.x + a.w) / art::kAtlasW, static_cast<float>(a.y) / art::kAtlasH});
            }
            for (int i = 0; i < n; ++i) {
                const float x = px + 2 + (i % cols) * cell + 2, y = py + 3 + (i / cols) * cell + 1;
                bool over = false;
                if (button(at(x, y), at(x + sw, y + sw), &over)) {
                    action = kPhoneColor;
                    if (index) *index = i;
                    g_phoneColorsOpen = false;  // OnPhoneColorVariationSelected: Hide
                }
                const ImVec2 c = at(x + sw / 2, y + sw / 2);
                shown.push_back(p.colors[i]);
                icon(d, p.colors[i], c, sw * S);
                if (i == p.color || over)
                    d->AddRect(at(x - 1, y - 1), at(x + sw + 1, y + sw + 1),
                               i == p.color ? IM_COL32(120, 200, 40, 255) : IM_COL32(255, 255, 255, 200), 3 * S, 0,
                               (i == p.color ? 2.0f : 1.2f) * S);
            }
            // A press anywhere else hides it (PhoneColorSelector.OnFocusChange).
            const ImVec2 m = ImGui::GetIO().MousePos, a0 = at(px, py), b0 = at(px + gw, py + gh), ba = at(39, 10),
                         bb = at(39 + cs, 10 + cs);
            const bool inside = (m.x >= a0.x && m.x <= b0.x && m.y >= a0.y && m.y <= b0.y) ||
                                (m.x >= ba.x && m.x <= bb.x && m.y >= ba.y && m.y <= bb.y);
            if (!blocked && ImGui::IsMouseClicked(0) && !inside && m.x >= r.x && m.x <= r.x + r.w && m.y >= r.y &&
                m.y <= r.y + r.h)
                g_phoneColorsOpen = false;
        }
    };
    // An app's picture: its category icon, 64 game pixels (mcImage at 0.64).
    auto appIcon = [&](int node, ImVec2 c, ImU32 tint = kWhite) {
        // Our apps: our own icons in the game's phone app style (tools/make_phone_icons.py).
        const art::Piece* own = node == -1 ? &art::kAppSettings : node == -2 ? &art::kAppCalendar : node == -3 ? &art::kAppBuild
                              : node == -10 ? &art::kAppGameOptions : node == -11 ? &art::kAppPhoneOptions
                              : node == -12 ? &art::kAppVrOptions : node == -4 ? &art::kAppMySim : node == -5 ? &art::kAppRelationships : node == -7 ? &art::kAppSimsForever : node == -15 ? &art::kAppGodView
                              : node == -6 ? &art::kAppWeather : node == -16 ? &art::kAppCheats : nullptr;
        if (own) {
            const float h = 32 * S;
            pieceIn(d, *own, {c.x - h, c.y - h}, {c.x + h, c.y + h}, tint);
            return;
        }
        if (node < 0) {
            // Options (the game menu): the game menu button's dots on a dark tile;
            // Calendar (-2): the HUD's calendar button on one.
            const float h = 30 * S;
            nine(d, art::kPhonePill, {c.x - h, c.y - h}, {c.x + h, c.y + h}, 37, 37, 37, 37, A * 0.8f,
                 IM_COL32(255, 255, 255, (tint >> 24) & 0xff));
            if (node == -2) {
                const float b = 21 * S;
                pieceIn(d, art::kCalendarButton, {c.x - b, c.y - b}, {c.x + b, c.y + b}, tint);
            } else if (node == -13 || node == -14) {
                // Gallery (-13), Manage Worlds (-14): the GameTray buttons' icons (btnShare, btnNeighborhood).
                const art::Piece& p = node == -13 ? art::kGalleryIcon : art::kManageWorldsIcon;
                piece(d, p, c, 34 * S / std::max(p.w, p.h), tint);
            } else if (node == -3) {
                // Build Mode (-3): the wrist's build button, the game's build icon on its disc.
                piece(d, art::kQueueDisc[0], c, 50 * S / art::kQueueDisc[0].w, IM_COL32(205, 210, 215, (tint >> 24) & 0xff));
                piece(d, art::kBuildIcon, c, 31 * S / art::kBuildIcon.w, tint);
            } else {
                piece(d, art::kGameMenuDots, c, A * 1.6f, tint);
            }
            return;
        }
        shown.push_back(p.nodes[node].icon);
        icon(d, p.nodes[node].icon, c, 64 * S, tint);
    };
    auto badge = [&](ImVec2 c) {  // mcNotificationBadge: the red glow and its white dot
        piece(d, art::kPhoneBadgeGlow, c, A * 1.08f);
        d->AddCircleFilled(c, 2.5f * S, kWhite);
    };
    if (g_phoneHomeAsked.exchange(false))
        g_phoneApp = g_phoneRoot = -1, g_phonePage = 0, g_phoneScroll = 0, g_phoneColorsOpen = false, g_phoneNotesOpen = false,
        g_phoneCalOpen = false, g_phoneSettingsOpen = false, g_phoneOptsGroup.clear(), g_mySim = -1, g_mySimMoodlet = -1,
        g_phoneSoundsOpen = false, g_hhSim = -1, g_contacts = -1, g_gameMenuPhone = false, g_weatherOpen = false, g_cheatsOpen = false,
        g_reorder = false, g_reorderPick.clear();
    if (g_phoneApp >= 0) g_phoneNotesOpen = g_phoneCalOpen = g_phoneSettingsOpen = g_phoneSoundsOpen = false, g_phoneOptsGroup.clear(), g_mySim = -1, g_contacts = -1;
    if (g_phoneApp >= 0 || p.colors.empty()) g_phoneColorsOpen = false;

    if (!p.disabled.empty() || (p.nodes.empty() && !p.disabledJson.empty())) {
        // The phone can't be used now (PhoneTuning.DISABLE_PHONE_TESTS, a full queue): the game's reason.
        const std::string& why = p.disabled.empty() ? std::string("...") : p.disabled;
        const float wrap = 200 * S;
        ImVec2 size = g_text->CalcTextSizeA(30, FLT_MAX, wrap, why.c_str());
        ImVec2 a = at(115, 200);
        d->AddText(g_text, 30, {a.x - size.x / 2 + 2, a.y - size.y / 2 + 2}, kShadow, why.c_str(), nullptr, wrap);
        d->AddText(g_text, 30, {a.x - size.x / 2, a.y - size.y / 2}, kWhite, why.c_str(), nullptr, wrap);
    } else if (g_phoneNotesOpen) {
        // The notification list (the game's NotificationWall, newest first):
        // each one a light card with its picture, title, text and time, the
        // wall's delete button; Clear all at the top; back at the bottom.
        const std::vector<Note>& notes = g_notes.notes;
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const char* head = "Notifications";
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
            shadowText(d, g_title, 34, {at(14, 22).x, at(14, 22).y - hs.y / 2}, kWhite, head);
            if (!notes.empty()) {
                const char* clear = "Clear all";
                ImVec2 cs2 = g_text->CalcTextSizeA(26, FLT_MAX, 0, clear);
                const ImVec2 b1 = at(222, 34), a1{b1.x - cs2.x - 24, at(0, 10).y};
                bool over = false;
                if (button(a1, b1, &over)) action = kNotesClear;
                if (over) d->AddRectFilled(a1, b1, IM_COL32(255, 255, 255, 50), 10);
                d->AddText(g_text, 26, {a1.x + 12, (a1.y + b1.y - cs2.y) / 2}, kWhite, clear);
            }
        }
        static bool g_scrolls2 = false;  // the list scrolled last frame: room for its slider, else rows to the edge
        const float top = 48, bottom = 352, left = 6, right = g_scrolls2 ? 206.0f : 224.0f;
        const float pic = 30, pad = 4, wrap = (right - left - pic - 3 * pad - 14) * S;
        const ImU32 kTitle = kWhite, kBody = kWhite;
        const int total = static_cast<int>(notes.size());
        if (swipeY) g_notesScroll = drag.notes - static_cast<int>(std::lround(dy / 45));  // follows the swipe, a card a step
        g_notesScroll = std::max(0, std::min(std::max(0, total - 1), g_notesScroll));
        float y = top;
        int shownCount = 0;
        for (int i = g_notesScroll; i < total; ++i) {
            const Note& n = notes[i];
            const std::string &title = found(n.titleJson), &text = found(n.textJson);
            ImVec2 ts = title.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(26, FLT_MAX, wrap, title.c_str());
            ImVec2 xs = text.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(24, FLT_MAX, wrap, text.c_str());
            const int nb = static_cast<int>(n.buttons.size()), perRow = nb > 2 ? 2 : std::max(1, nb);
            const float bh = 22, bgap = 3, brows = nb ? static_cast<float>((nb + perRow - 1) / perRow) : 0;
            const float textH = std::max(pic + 2 * pad, (ts.y + xs.y + 30) / S + 2 * pad + 4);
            const float h = textH + brows * (bh + bgap);
            if (y + h > bottom && shownCount > 0) break;
            const ImVec2 a = at(left, y), b = at(right, y + h);
            noteBackground(d, n.style, a, b, 0.8f);
            if (!n.read) d->AddCircleFilled(at(left + 3.5f, y + 5), 1.6f * S, kWhite);  // unread
            if (n.picture) {
                shown.push_back(n.picture);
                icon(d, n.picture, at(left + pad + pic / 2, y + pad + pic / 2), pic * S);
            }
            float ty = (y + pad + 1) * S + r.y;
            const float tx = at(left + 2 * pad + pic, 0).x;
            if (ts.y > 0) {
                d->AddText(g_title, 26, {tx + 2, ty + 2}, kShadow, title.c_str(), nullptr, wrap);
                d->AddText(g_title, 26, {tx, ty}, kTitle, title.c_str(), nullptr, wrap);
                ty += ts.y + 2;
            }
            if (xs.y > 0) {
                d->AddText(g_text, 24, {tx + 2, ty + 2}, kShadow, text.c_str(), nullptr, wrap);
                d->AddText(g_text, 24, {tx, ty}, kBody, text.c_str(), nullptr, wrap);
                ty += xs.y + 2;
            }
            if (!n.time.empty()) d->AddText(g_text, 20, {tx, ty}, IM_COL32(255, 255, 255, 200), n.time.c_str());
            bool over = false;
            const ImVec2 da = at(right - 14, y + 2), db = at(right - 2, y + 14);
            if (button(da, db, &over)) {
                action = kNoteDelete;
                if (index) *index = static_cast<int>(n.id);
            }
            pieceIn(d, over ? art::kNoteDeleteOver : art::kNoteDelete, da, db);
            // Its answers (the game still waits for one): the dialog's buttons.
            const float bw = (right - left - 2 * pad - (perRow - 1) * bgap) / perRow;
            for (int k = 0; k < nb; ++k) {
                const float bx = left + pad + (k % perRow) * (bw + bgap), by = y + textH + (k / perRow) * (bh + bgap);
                const ImVec2 ba = at(bx, by), bb = at(bx + bw, by + bh);
                bool bo = false;
                if (button(ba, bb, &bo)) {
                    action = kNoteAnswer;
                    if (index) *index = static_cast<int>(n.id * 8 + k);
                }
                if (bo)
                    nine(d, art::kDialogButtonOver, {ba.x - 2, ba.y - 3}, {bb.x + 2, bb.y + 6}, 48, 36, 48, 54, 0.6f);
                else
                    nine(d, art::kDialogButton, ba, bb, 48, 36, 48, 42, 0.6f);
                std::string label = found(n.buttons[k]);
                if (label.empty()) label = "OK";
                const float bwrap = bb.x - ba.x - 24;
                ImVec2 ls = g_text->CalcTextSizeA(24, FLT_MAX, 0, label.c_str());
                const float lk = ls.x > bwrap ? std::max(0.6f, bwrap / ls.x) : 1;
                d->PushClipRect(ba, bb, true);
                d->AddText(g_text, 24 * lk, {(ba.x + bb.x - ls.x * lk) / 2, (ba.y + bb.y - ls.y * lk) / 2 - 2}, IM_COL32(30, 70, 128, 255),
                           label.c_str());
                d->PopClipRect();
            }
            y += h + 4;
            ++shownCount;
        }
        if (notes.empty()) {
            const char* none = "No notifications";
            ImVec2 ns = g_text->CalcTextSizeA(28, FLT_MAX, 0, none);
            const ImVec2 c = at(115, 180);
            shadowText(d, g_text, 28, {c.x - ns.x / 2, c.y - ns.y / 2}, kWhite, none);
        }
        g_scrolls2 = total > shownCount;
        if (total > shownCount) {
            // The slider: one notification a step; a press above or below the thumb scrolls.
            const float trackH = bottom - top, thumbH = std::max(24.0f, trackH * shownCount / total);
            const float thumbY = top + (trackH - thumbH) * g_notesScroll / std::max(1, total - shownCount);
            d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
            pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
            const float t = sliderFollow(r, top, bottom, thumbH);
            if (t >= 0) g_notesScroll = static_cast<int>(std::lround(t * (total - shownCount)));
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_phoneNotesOpen = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (!g_phoneOptsGroup.empty()) {
        // Phone Options: the debug menu's phone page (and its pages) on the phone, in the
        // phone's list style: the setting on the left, its value with arrows on the right.
        std::wstring title, parent;
        const std::vector<settings::PageRow> rows = settings::pageRows(g_phoneOptsGroup.c_str(), &title, &parent);
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const std::string head = utf8(title.empty() ? std::wstring(L"Phone Options") : title == L"Phone" ? L"Phone Options" : title);
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head.c_str());
            const float k = std::min(1.0f, 210 * S / std::max(1.0f, hs.x));
            shadowText(d, g_title, 34 * k, {at(115, 22).x - hs.x * k / 2, at(115, 22).y - hs.y * k / 2}, kWhite, head.c_str());
        }
        static bool g_scrolls3 = false;  // the list scrolled last frame: room for its slider, else rows to the edge
        const float top = 50, bottom = 352, left = 6, right = g_scrolls3 ? 206.0f : 224.0f, rowH = 50;
        const int shownRows = static_cast<int>((bottom - top) / rowH), total = static_cast<int>(rows.size());
        if (swipeY) g_phoneOptsScroll = drag.scroll - static_cast<int>(std::lround(dy / rowH));
        g_phoneOptsScroll = std::max(0, std::min(std::max(0, total - shownRows), g_phoneOptsScroll));
        const float ts = 24 * g_phoneText;
        for (int k = 0; k < shownRows; ++k) {
            const int i = g_phoneOptsScroll + k;
            if (i >= total) break;
            const settings::PageRow& row = rows[i];
            const float y = top + k * rowH;
            const ImVec2 ra = at(left, y + 2), rb = at(right, y + rowH - 2);
            d->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 225), 6 * S);
            const std::string label = utf8(row.label), value = utf8(row.value);
            // The value's part on the right: arrows around it (a value), a switch, or a chevron (a page).
            const float valueW = row.kind == 0 ? 74 : row.kind == 1 ? 40 : 14;
            const float wrap = (right - left - valueW - 12) * S;
            ImVec2 ls = g_text->CalcTextSizeA(ts, FLT_MAX, wrap, label.c_str());
            float lk = 1;
            if (ls.y > (rowH - 8) * S) lk = std::max(0.6f, (rowH - 8) * S / ls.y);
            d->AddText(g_text, ts * lk, {at(left + 5, 0).x, (ra.y + rb.y - ls.y * lk) / 2}, kBlue, label.c_str(), nullptr, wrap);
            const float vx = right - valueW - 3, cy = y + rowH / 2;
            bool over = false;
            if (row.kind == 0) {
                // < value >
                const ImVec2 la = at(vx, cy - 11), lb = at(vx + 20, cy + 11), ga = at(right - 23, cy - 11), gb = at(right - 3, cy + 11);
                if (button(la, lb, &over)) settings::pageChange(row.row, -1);
                d->AddRectFilled(la, lb, over ? IM_COL32(60, 140, 230, 255) : IM_COL32(9, 73, 171, 255), 5 * S);
                d->AddTriangleFilled({la.x + 13 * S, la.y + 5 * S}, {la.x + 13 * S, lb.y - 5 * S}, {la.x + 6 * S, (la.y + lb.y) / 2}, kWhite);
                over = false;
                if (button(ga, gb, &over)) settings::pageChange(row.row, 1);
                d->AddRectFilled(ga, gb, over ? IM_COL32(60, 140, 230, 255) : IM_COL32(9, 73, 171, 255), 5 * S);
                d->AddTriangleFilled({ga.x + 7 * S, ga.y + 5 * S}, {ga.x + 7 * S, gb.y - 5 * S}, {ga.x + 14 * S, (ga.y + gb.y) / 2}, kWhite);
                const float vw = (right - 26) - (vx + 22);
                ImVec2 vs = g_text->CalcTextSizeA(ts, FLT_MAX, 0, value.c_str());
                const float vk = vs.x > vw * S ? std::max(0.5f, vw * S / vs.x) : 1;
                d->AddText(g_text, ts * vk, {at(vx + 22 + vw / 2, 0).x - vs.x * vk / 2, at(0, cy).y - vs.y * vk / 2}, kBlue, value.c_str());
            } else if (row.kind == 1) {
                // A switch: the press flips it.
                const bool on = row.value == L"On";
                const ImVec2 sa = at(vx, cy - 9), sb = at(vx + 36, cy + 9);
                if (button(ra, rb, &over)) settings::pageChange(row.row, 0);
                d->AddRectFilled(sa, sb, on ? IM_COL32(70, 170, 60, 255) : IM_COL32(150, 158, 170, 255), 9 * S);
                const float kx = on ? sb.x - 9 * S : sa.x + 9 * S;
                d->AddCircleFilled({kx, (sa.y + sb.y) / 2}, 7 * S, kWhite);
                if (over) d->AddRect(ra, rb, IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.2f * S);
            } else {
                // A page (or a button): the press opens it.
                if (button(ra, rb, &over)) {
                    if (row.kind == 2 && !row.target.empty()) g_phoneOptsGroup = row.target, g_phoneOptsScroll = 0;
                    else settings::pageChange(row.row, 0);
                }
                if (row.kind == 2) {
                    const ImVec2 c = at(right - 9, cy);
                    d->AddTriangleFilled({c.x - 3 * S, c.y - 6 * S}, {c.x - 3 * S, c.y + 6 * S}, {c.x + 4 * S, c.y}, kBlue);
                }
                if (over) d->AddRect(ra, rb, IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.2f * S);
            }
        }
        g_scrolls3 = total > shownRows;
        if (total > shownRows) {
            const float trackH = bottom - top, thumbH = std::max(24.0f, trackH * shownRows / total);
            const float thumbY = top + (trackH - thumbH) * g_phoneOptsScroll / std::max(1, total - shownRows);
            d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
            pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
            const float t = sliderFollow(r, top, bottom, thumbH);
            if (t >= 0) g_phoneOptsScroll = static_cast<int>(std::lround(t * (total - shownRows)));
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) {
            // Back: the page above, or the Settings screen from the phone's own page.
            if (g_phoneOptsGroup == L"Phone" || parent.empty() || parent == L"Interface") g_phoneOptsGroup.clear();
            else g_phoneOptsGroup = parent;
            g_phoneOptsScroll = 0;
        }
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_phoneSoundsOpen) {
        // Sounds (user 2026-10-23 23:55Z): the phone's mode (sound, vibrate, mute), the game's
        // silence for calls, then the ringtones and the text and notification tones (the game's
        // stings and bumpers, phonesound.py); a press picks one and plays it.
        static const wchar_t* const kModes[3] = {L"Sound", L"Vibrate", L"Mute"};
        static const wchar_t* const kRings[] = {L"None", L"Off to Work", L"Promotion", L"Skill Up", L"Collection", L"Karaoke Win",
                                                L"First Kiss", L"Birthday", L"Reward", L"Eureka", L"Liftoff"};
        static const wchar_t* const kTexts[] = {L"None", L"Positive", L"Neutral", L"Uh-oh", L"Bills Paid", L"Club", L"Accepted",
                                                L"Moodlet", L"Advice"};
        const std::vector<settings::PageRow> rows = settings::pageRows(L"Phone sounds", nullptr, nullptr);
        auto rowFor = [&](const wchar_t* label) -> const settings::PageRow* {
            for (const settings::PageRow& row : rows)
                if (row.label == label) return &row;
            return nullptr;
        };
        auto indexOf = [](const settings::PageRow* row, const wchar_t* const* names, int n) {
            for (int i = 0; row && i < n; ++i)
                if (row->value == names[i]) return i;
            return 0;
        };
        auto setTo = [](const settings::PageRow* row, int from, int to) {
            if (!row) return;
            for (int k = 0; k < 20 && from != to; ++k) {
                if (!settings::pageChange(row->row, to > from ? 1 : -1)) break;
                from += to > from ? 1 : -1;
            }
        };
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const char* head = "Sounds";
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
            shadowText(d, g_title, 34, {at(115, 24).x - hs.x / 2, at(115, 24).y - hs.y / 2}, kWhite, head);
        }
        static bool g_scrolls4 = false;  // the list scrolled last frame: room for its slider, else rows to the edge
        const float top = 48, bottom = 354, left = 6, right = g_scrolls4 ? 206.0f : 224.0f;
        const float ts = 22 * g_phoneText;
        float y = top - g_mySimScroll;
        d->PushClipRect(at(0, top), at(230, bottom), true);
        // The mode: three segments.
        const settings::PageRow* modeRow = rowFor(L"Phone");
        const int mode = indexOf(modeRow, kModes, 3);
        for (int i = 0; i < 3; ++i) {
            const float x0 = left + i * (right - left) / 3, x1 = left + (i + 1) * (right - left) / 3 - 2;
            const ImVec2 a = at(x0, y + 4), b = at(x1, y + 30);
            bool over = false;
            if (button(a, b, &over) && i != mode) setTo(modeRow, mode, i), g_phoneModeSet = i;
            d->AddRectFilled(a, b, i == mode ? IM_COL32(9, 73, 171, 255) : over ? IM_COL32(230, 238, 250, 240) : IM_COL32(255, 255, 255, 225),
                             6 * S);
            const std::string label = utf8(kModes[i]);
            ImVec2 ls = g_text->CalcTextSizeA(ts, FLT_MAX, 0, label.c_str());
            d->AddText(g_text, ts, {(a.x + b.x - ls.x) / 2, (a.y + b.y - ls.y) / 2}, i == mode ? kWhite : kBlue, label.c_str());
        }
        y += 36;
        // The game's own silence (calls don't come through while it is on).
        {
            const ImVec2 a = at(left, y), b = at(right, y + 30);
            bool over = false;
            if (button(a, b, &over)) action = kPhoneSilence;
            d->AddRectFilled(a, b, IM_COL32(255, 255, 255, 225), 6 * S);
            if (over) d->AddRect(a, b, IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.2f * S);
            d->AddText(g_text, ts, {at(left + 6, 0).x, at(0, y + 15).y - ts / 2}, kBlue, "Silence calls (the game's)");
            const ImVec2 sa = at(right - 40, y + 6), sb = at(right - 4, y + 24);
            d->AddRectFilled(sa, sb, p.silenced ? IM_COL32(70, 170, 60, 255) : IM_COL32(150, 158, 170, 255), 9 * S);
            d->AddCircleFilled({p.silenced ? sb.x - 9 * S : sa.x + 9 * S, (sa.y + sb.y) / 2}, 7 * S, kWhite);
        }
        y += 36;
        auto list = [&](const char* title, const settings::PageRow* row, const wchar_t* const* names, int n, int kind) {
            shadowText(d, g_title, 24 * g_phoneText, {at(left + 2, 0).x, at(0, y + 2).y}, kWhite, title);
            y += 14 * g_phoneText + 6;
            const int chosen = indexOf(row, names, n);
            for (int i = 0; i < n; ++i) {
                const float h = 26;
                const ImVec2 a = at(left, y), b = at(right, y + h - 2);
                bool over = false;
                if (button(a, b, &over)) {
                    setTo(row, chosen, i);
                    if (i > 0) {
                        action = kTonePreview;
                        if (index) *index = kind * 100 + i;
                    }
                }
                d->AddRectFilled(a, b, i == chosen ? IM_COL32(205, 235, 190, 240) : IM_COL32(255, 255, 255, 225), 6 * S);
                if (over) d->AddRect(a, b, IM_COL32(9, 73, 171, 180), 6 * S, 0, 1.2f * S);
                const std::string label = utf8(names[i]);
                d->AddText(g_text, ts, {at(left + 22, 0).x, at(0, y + h / 2 - 1).y - ts / 2}, kBlue, label.c_str());
                if (i == chosen) {
                    const ImVec2 c = at(left + 11, y + h / 2 - 1);
                    d->AddLine({c.x - 3 * S, c.y}, {c.x - 0.6f * S, c.y + 2.6f * S}, IM_COL32(40, 130, 30, 255), 1.8f * S);
                    d->AddLine({c.x - 0.6f * S, c.y + 2.6f * S}, {c.x + 3.6f * S, c.y - 2.6f * S}, IM_COL32(40, 130, 30, 255), 1.8f * S);
                }
                y += h;
            }
            y += 6;
        };
        list("Ringtone", rowFor(L"Ringtone"), kRings, static_cast<int>(sizeof kRings / sizeof kRings[0]), 0);
        list("Text and notification tone", rowFor(L"Text and notification tone"), kTexts,
             static_cast<int>(sizeof kTexts / sizeof kTexts[0]), 1);
        d->PopClipRect();
        const float total = y + g_mySimScroll - top, room = bottom - top;
        if (swipeY) g_mySimScroll = drag.sim - dy;
        g_mySimScroll = std::max(0.0f, std::min(std::max(0.0f, total - room), g_mySimScroll));
        g_scrolls4 = total > room;
        if (total > room) {
            const float thumbH = std::max(24.0f, room * room / total);
            const float thumbY = top + (room - thumbH) * g_mySimScroll / std::max(1.0f, total - room);
            d->AddRectFilled(at(214, top), at(218, bottom), IM_COL32(255, 255, 255, 60), 2 * S);
            pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
            const float t = sliderFollow(r, top, bottom, thumbH);
            if (t >= 0) g_mySimScroll = t * (total - room);
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_phoneSoundsOpen = false, g_mySimScroll = 0;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_phoneSettingsOpen && !g_gameMenuPhone) {  // Game Options (below) opens over it, Back returns here
        // Settings: its apps (Game Options: the game's Escape menu, as the old Options app;
        // Phone Options: this phone's settings; VR Options: the VR debug menu), named under them.
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const char* head = "Settings";
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
            shadowText(d, g_title, 34, {at(115, 22).x - hs.x / 2, at(115, 22).y - hs.y / 2}, kWhite, head);
        }
        // Gallery: the GameTray's top right button (user 2026-10-24 01:00Z), sending what it sends. (Manage Worlds
        // dropped 01:22Z: it is in Game Options already; kManageWorlds / app -14 kept unused.)
        // God View (user 2026-10-24 02:03Z): out of first person to the game's own camera.
        static const int kApps[5] = {-10, -11, -12, -13, -15};
        static const char* kNames[5] = {"Game Options", "Phone Options", "VR Options", "Gallery", "God View"};
        for (int k = 0; k < 5; ++k) {
            const float x = 13 + (k % 3) * 68 + 4, y = 101 + 6 + (k / 3) * 92;
            const ImVec2 c = at(x + 32, y + 32);
            bool over = false;
            if (button(at(x, y), at(x + 64, y + 64), &over)) {
                if (kApps[k] == -10) g_gameMenuPhone = true;
                else if (kApps[k] == -11) g_phoneOptsGroup = L"Phone", g_phoneOptsScroll = 0;
                else if (kApps[k] == -13) action = kGallery;
                else if (kApps[k] == -15) action = kGodView;
                else if (kApps[k] == -14) action = kManageWorlds;
                else action = kVrOptions;
            }
            if (over) piece(d, art::kPhoneAppOver, c, A);
            appIcon(kApps[k], c);
            const float ns = 22 * g_phoneText;
            ImVec2 size = g_small->CalcTextSizeA(ns, FLT_MAX, 0, kNames[k]);
            const float fit = std::min(1.0f, 66 * S / std::max(1.0f, size.x));
            shadowText(d, g_small, ns * fit, {c.x - size.x * fit / 2, at(0, y + 68).y}, kWhite, kNames[k]);
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_phoneSettingsOpen = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_cheatsOpen) {
        // Cheats (user 2026-10-25 11:04Z): the debug menu's Testing cheats page (settings.cpp), each button sending
        // the same control: the two switches, then the one-press cheats (they turn testing cheats on first).
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const char* head = "Cheats";
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
            shadowText(d, g_title, 34, {at(115, 24).x - hs.x / 2, at(115, 24).y - hs.y / 2}, kWhite, head);
        }
        struct Cheat { const char* label; int on, off; };  // off < 0: a one-press cheat
        static const Cheat kCheats[6] = {{"Testing cheats", 6, 9}, {"Cheat pie menus", 11, 12}, {"Fill needs", 5, -1},
                                         {"Motherlode", 7, -1}, {"Move objects on", 8, -1}, {"CAS full edit mode", 10, -1}};
        const int state = settings::testingCheats();
        const ImU32 kBlue = IM_COL32(28, 72, 150, 255);
        for (int n = 0; n < 6; ++n) {
            const Cheat& c = kCheats[n];
            const bool toggle = c.off >= 0, on = toggle && (state & (n == 0 ? 1 : 2)) != 0;
            const float rowH = 46, top = 54 + n * (rowH + 4);
            const ImVec2 ba = at(8, top), bb = at(222, top + rowH);
            bool over = false, held = false;
            if (button(ba, bb, &over, &held)) {
                action = kCheat, *index = toggle && on ? c.off : c.on;
                LOG("Phone: cheat %s", c.label);
            }
            const ImU32 fill = held ? IM_COL32(205, 222, 246, 245) : over ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, 225);
            d->AddRectFilled(ba, bb, fill, 8 * S);
            if (over || held) d->AddRect(ba, bb, IM_COL32(66, 140, 214, 255), 8 * S, 0, 1.2f * S);
            const float size = 26 * g_phoneText;
            ImVec2 ls = g_title->CalcTextSizeA(size, FLT_MAX, 0, c.label);
            const float fit = std::min(1.0f, 140 * S / std::max(1.0f, ls.x));
            d->AddText(g_title, size * fit, {at(16, 0).x, (ba.y + bb.y - ls.y * fit) / 2}, kBlue, c.label);
            if (toggle) {
                // The switch: green and right when on.
                const ImVec2 sa = at(176, top + 13), sb = at(214, top + 33);
                d->AddRectFilled(sa, sb, on ? IM_COL32(88, 190, 70, 255) : IM_COL32(180, 186, 196, 255), 10 * S);
                d->AddCircleFilled(at(on ? 204 : 186, top + 23), 8 * S, IM_COL32(255, 255, 255, 255), 24);
            }
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_cheatsOpen = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_weatherOpen) {
        drawWeather(d, r, button, shown, action, swipeY, dy, drag);
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_weatherOpen = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_gameMenuPhone) {
        // Game Options (user 2026-10-24 01:26Z): the Escape menu's buttons (Sims4EscapeMenu, in its order), drawn
        // as the game draws them; each sends what its button sends (clientdlg), the window it opens shows in front
        // of you and a question to save shows on our popup card.
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 44));
        {
            const char* head = "Game Options";
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head);
            shadowText(d, g_title, 34, {at(115, 24).x - hs.x / 2, at(115, 24).y - hs.y / 2}, kWhite, head);
        }
        // Our own buttons (user 2026-10-24 15:33Z: not a copy of the game's menu), each a shortcut to the Escape
        // menu's button of the same name (its index in Sims4EscapeMenu's order): light cards two a row as the
        // phone's lists, the exits in red at the bottom.
        static const char* const kButtons[11] = {"Save", "Save As...", "Game Options", "Help", "Lessons", "Release Notes",
                                                 "Credits", "Achievements", "Manage Worlds", "Exit to Main Menu", "Exit Game"};
        static const int kOrder[12] = {0, 1, 2, 8, 7, 4, 3, 5, 6, -1, 9, 10};  // -1: the half Credits takes
        const ImU32 kBlue = IM_COL32(28, 72, 150, 255), kRed = IM_COL32(196, 40, 40, 255);
        for (int n = 0; n < 12; ++n) {
            const int k = kOrder[n];
            if (k < 0) continue;
            const float rowH = 46, top = 54 + (n / 2) * (rowH + 3) + (n >= 10 ? 4 : 0);
            const float x0 = n % 2 ? 117 : 8, x1 = n % 2 || k == 6 ? 222 : 113;  // Credits: the whole row
            const ImVec2 ba = at(x0, top), bb = at(x1, top + rowH);
            bool over = false, held = false;
            if (button(ba, bb, &over, &held)) action = kGameMenuButton, *index = k;
            const bool exit = k >= 9;
            const ImU32 fill = held ? IM_COL32(205, 222, 246, 245) : over ? IM_COL32(232, 241, 255, 245) : IM_COL32(255, 255, 255, 225);
            d->AddRectFilled(ba, bb, fill, 8 * S);
            if (over || held) d->AddRect(ba, bb, exit ? kRed : IM_COL32(66, 140, 214, 255), 8 * S, 0, 1.2f * S);
            const float size = 26 * g_phoneText, room = bb.x - ba.x - 10 * S;
            ImVec2 ls = g_title->CalcTextSizeA(size, FLT_MAX, 0, kButtons[k]);
            const float fit = std::min(1.0f, room / std::max(1.0f, ls.x));
            ls = {ls.x * fit, ls.y * fit};
            d->AddText(g_title, size * fit, {(ba.x + bb.x - ls.x) / 2, (ba.y + bb.y - ls.y) / 2}, exit ? kRed : kBlue, kButtons[k]);
        }
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_gameMenuPhone = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_contacts >= 0) {
        drawContacts(d, r, button, shown, action, swipeY, dy, drag, index);
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) {
            if (g_contacts >= 1) --g_contacts;  // Info -> the page -> the list
            else g_contacts = -1;
            g_mySimScroll = 0;
        }
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_mySim >= 0) {
        // The home screen's header on My Sim's grid (user 2026-10-24 16:10Z): its buttons go first, so the colour
        // selector takes the presses over the apps under it, and are drawn last, over them.
        const bool header = g_mySim == 0 && g_mySimMoodlet < 0;
        if (header) {
            d->ChannelsSplit(2);
            d->ChannelsSetCurrent(1);
            phoneHeader(true);  // with rearrange: My Sim's apps (user 2026-10-25 00:17Z)
            phoneColorSelector();
            d->ChannelsSetCurrent(0);
        }
        drawMySim(d, r, button, shown, action, swipeY, dy, drag, index);
        if (header) d->ChannelsMerge();
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) {
            if (g_aspChange) g_aspChange = false;
            else if (g_mySimMoodlet >= 0) g_mySimMoodlet = -1;
            else if (g_hhSim >= 0) g_hhSim = -1;
            else g_mySim = g_mySim > 0 ? 0 : -1;
            g_mySimScroll = 0, g_aspPicked = -1, g_reorder = false, g_reorderPick.clear();
        }
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_phoneCalOpen) {
        drawCalendar(d, r, button, shown, action);
        bool over = false;
        if (button(at(96, 361), at(129, 392), &over)) g_phoneCalOpen = false;
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    } else if (g_phoneApp < 0) {
        // PhoneCategoryScreen: the tool buttons on their pill, the battery,
        // the apps 3 x 3 a page (the Options app first), the page dots.
        const bool colors = !p.colors.empty();
        const bool open = g_phoneColorsOpen;  // the selector takes the presses while it is up
        phoneHeader(true);
        std::vector<int> apps;
        apps.push_back(-1);  // Options
        apps.push_back(-2);  // Calendar
        apps.push_back(-3);  // Build Mode (as the wrist's build button: F2)
    apps.push_back(-4);  // My Sim (phonesim.py)
        apps.push_back(-5);  // Contacts (phonesim.py)
        apps.push_back(-6);  // Weather (phonesim.py)
        apps.push_back(-16);  // Cheats (the debug menu's testing cheats)
        {
            // The Weather app's icon is the current weather: ask phonesim for it now and then while home shows.
            static ULONGLONG weatherAsked = 0;
            const ULONGLONG t = GetTickCount64();
            if (action == kNone && (weatherAsked == 0 || t - weatherAsked > (g_sim.weather.sent ? 60000 : 5000)))
                action = kMySimOpened, weatherAsked = t;
        }
        if (simsforever::available()) apps.push_back(-7);  // Sims Forever: only with Sims 1 set up
        apps.insert(apps.end(), p.home.begin(), p.home.end());
        // Your order: the saved apps first in theirs, the rest after in the game's.
        readAppOrder();
        if (!g_appOrder.empty()) {
            std::vector<int> ordered;
            for (const std::string& k : g_appOrder)
                for (int n : apps)
                    if (appKey(p, n) == k && std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
            for (int n : apps)
                if (std::find(ordered.begin(), ordered.end(), n) == ordered.end()) ordered.push_back(n);
            apps.swap(ordered);
        }
        const int perPage = 9, pages = static_cast<int>((apps.size() + perPage - 1) / perPage);
        g_phonePage = std::max(0, std::min(pages - 1, g_phonePage));
        int hovered = -100;
        // A sideways swipe slides the pages with it (held back past the ends);
        // let go past a quarter of a page and it turns.
        float slide = swipeX && !open ? dx : 0;
        if ((g_phonePage == 0 && slide > 0) || (g_phonePage == pages - 1 && slide < 0)) slide *= 0.3f;
        if (swipeEnded && swipeX && !open) {
            if (dx < -55 && g_phonePage + 1 < pages) ++g_phonePage;
            if (dx > 55 && g_phonePage > 0) --g_phonePage;
            slide = 0;
        }
        if (slide != 0) d->PushClipRect(at(0, 95), at(230, 320), true);
        for (int pg = g_phonePage - 1; pg <= g_phonePage + 1; ++pg)
        for (int k = 0; k < perPage; ++k) {
            if (pg < 0 || pg >= pages || (pg != g_phonePage && slide == 0)) break;
            const int i = pg * perPage + k;
            if (i >= static_cast<int>(apps.size())) break;
            const int node = apps[i];
            const float x = 13 + (k % 3) * 68 + 4 + slide + (pg - g_phonePage) * 230, y = 101 + (k / 3) * 70 + 6;
            const ImVec2 c = at(x + 32, y + 32);
            bool over = false;
            if (!open && g_reorder && button(at(x, y), at(x + 64, y + 64), &over)) {
                // Rearranging: press one, then the place to put it.
                const std::string key = appKey(p, node);
                if (g_reorderPick.empty()) {
                    g_reorderPick = key;
                } else if (g_reorderPick != key) {
                    std::vector<std::string> keys;
                    for (int n : apps) keys.push_back(appKey(p, n));
                    auto from = std::find(keys.begin(), keys.end(), g_reorderPick);
                    if (from != keys.end()) {
                        const std::string moved = *from;
                        const bool forward = (from - keys.begin()) < i;  // moved later: it goes after the one pressed
                        keys.erase(from);
                        keys.insert(std::find(keys.begin(), keys.end(), key) + (forward ? 1 : 0), moved);
                        // (keep the order of saved apps that aren't on the phone now, e.g. another Sim's)
                        for (const std::string& k : g_appOrder)
                            if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
                        g_appOrder = keys;
                        saveAppOrder();
                    }
                    g_reorderPick.clear();
                } else {
                    g_reorderPick.clear();
                }
            } else if (!open && !g_reorder && button(at(x, y), at(x + 64, y + 64), &over)) {
                if (node == -16) {
                    g_cheatsOpen = true;
                } else if (node == -7) {
                    action = kSimsForever;
                } else if (node == -6) {
                    g_weatherOpen = true, g_mySimScroll = 0;
                    action = kMySimFresh;
                    g_phoneDataUntil = GetTickCount64() + 1500;
                } else if (node == -5) {
                    g_contacts = 0, g_contactSel = -1, g_mySimScroll = 0, g_contactsAsked = GetTickCount64();
                    action = kContactsOpened;
                    g_phoneDataUntil = GetTickCount64() + 1500;
                } else if (node == -4) {
                    g_mySim = 0, g_mySimScroll = 0, g_mySimAsked = GetTickCount64();
                    action = kMySimFresh;
                    g_phoneDataUntil = GetTickCount64() + 1500;
                } else if (node == -3) {
                    action = kBuildPressed;
                } else if (node == -2) {
                    g_phoneCalOpen = true;
                    g_calWeek = g_calScroll = 0;
                    g_calSel = -1;
                    action = kCalendarOpened;
                    g_phoneDataUntil = GetTickCount64() + 1500;
                } else if (node < 0) {
                    g_phoneSettingsOpen = true;  // Settings: its own screen of apps
                } else {
                    g_phoneApp = g_phoneRoot = node;
                    g_phoneScroll = 0;
                }
            }
            if (over) {
                hovered = node;
                piece(d, art::kPhoneAppOver, c, A);
            }
            appIcon(node, c);
            if (node >= 0 && p.nodes[node].notification) badge(at(x + 55, y + 8));
            if (g_reorder) {
                const bool picked = appKey(p, node) == g_reorderPick;
                d->AddRect(at(x - 1, y - 1), at(x + 65, y + 65), picked ? IM_COL32(120, 230, 70, 255) : IM_COL32(255, 255, 255, 120),
                           12 * S, 0, (picked ? 2.5f : 1.2f) * S);
            }
        }
        if (slide != 0) d->PopClipRect();
        // mcCategoryName: the app pointed at.
        if (hovered != -100) {
            std::string name = hovered == -1 ? std::string("Settings") : hovered == -2 ? std::string("Calendar")
                             : hovered == -3 ? std::string("Build Mode") : hovered == -4 ? std::string("My Sim") : hovered == -5 ? std::string("Contacts") : hovered == -6 ? std::string("Weather") : hovered == -7 ? std::string("Sims Forever") : hovered == -16 ? std::string("Cheats")
                             : p.nodes[hovered].name;
            ImVec2 size = g_small->CalcTextSizeA(34 * g_phoneText, FLT_MAX, 0, name.c_str());
            float scale = g_phoneText * std::min(1.0f, 206 * S / std::max(1.0f, size.x));
            size.x /= g_phoneText, size.y /= g_phoneText;
            ImVec2 a = at(12 + 103, 46 + 10);
            shadowText(d, g_small, 34 * scale, {a.x - size.x * scale / 2, a.y - size.y * scale / 2}, kWhite, name.c_str());
        }
        if (pages > 1) {
            const float start = (230 - pages * 28.0f) / 2;
            for (int i = 0; i < pages; ++i) {
                const float x = start + i * 28;
                bool over = false;
                if (!open && button(at(x, 364), at(x + 28, 392), &over)) g_phonePage = i;
                piece(d, i == g_phonePage ? art::kPhonePageOn : over ? art::kPhonePageOver : art::kPhonePage,
                      at(x + 14, 378), A);
            }
        }
        phoneColorSelector();
    } else {
        // PhoneInteractionScreen without its app carousel (user 2026-10-24 02:06Z: no bar to change apps inside
        // one; Back goes up): the app's name as the header, the list, the back button. Only our drawing changes:
        // the entries are still the game's menu tree (vanilla, mods' apps and what mods add to them).
        const PhoneNode& app = p.nodes[g_phoneApp];
        pieceIn(d, art::kPhoneTint, at(0, 0), at(230, 48));
        {
            const std::string& head = app.name;
            ImVec2 hs = g_title->CalcTextSizeA(34, FLT_MAX, 0, head.c_str());
            const float k = std::min(1.0f, 200 * S / std::max(1.0f, hs.x));
            shadowText(d, g_title, 34 * k, {at(115, 30).x - hs.x * k / 2, at(115, 30).y - hs.y * k / 2}, kWhite, head.c_str());
        }
        bool over = false;
        // The list (PhoneMenuScrollList at y 84, margins 6, 27, 50, 8), rows of PhoneItemCellView.
        const float top = 52, bottom = 352, rowH = 58, left = 8;
        const int rows = static_cast<int>((bottom - top) / rowH), total = static_cast<int>(app.children.size());
        const float right = total > rows ? 203.0f : 224.0f;  // room for the slider only when there is one
        if (swipeY) g_phoneScroll = drag.scroll - static_cast<int>(std::lround(dy / rowH));  // follows the swipe, a row a step
        g_phoneScroll = std::max(0, std::min(std::max(0, total - rows), g_phoneScroll));
        for (int k = 0; k < rows; ++k) {
            const int i = g_phoneScroll + k;
            if (i >= total) break;
            const int node = app.children[i];
            const PhoneNode& e = p.nodes[node];
            const float y = top + k * rowH;
            bool rowOver = false, held = false;
            const bool enabled = e.disabled.empty();
            if (button(at(left, y), at(right, y + rowH), &rowOver, &held) && enabled) {
                if (e.category) {
                    g_phoneApp = node;
                    g_phoneScroll = 0;
                } else {
                    action = kPhonePick;
                    if (index) *index = node;
                    g_phoneApp = g_phoneRoot = -1;  // the game's panel hides once something is picked
                }
            }
            // Each entry on a light card (user 2026-10-24 00:22Z: the blue text was hard to read on the wallpaper).
            d->AddRectFilled(at(left, y + 3), at(right - 3, y + rowH - 3), IM_COL32(255, 255, 255, 225), 6 * S);
            if (enabled && (rowOver || held)) pieceIn(d, held ? art::kPhoneRowDown : art::kPhoneRowOver, at(left, y + 12), at(right - 3, y + 46));
            const std::string& text = e.name;
            const float wrap = 180 * S, size = 30 * g_phoneText;
            ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
            ImFont* font = e.notification ? g_title : g_text;  // the .notif style: bold
            const ImU32 colour = enabled ? kBlue : kGrey;
            const ImVec2 a = at(left + 8, y + 5 + 24.5f);
            d->AddText(font, size, {a.x, a.y - ts.y / 2}, colour, text.c_str(), nullptr, wrap);
            if (!enabled && (rowOver || held) && !e.disabled.empty()) {
                // The disabled reason (its tooltip), under the name.
                d->AddText(g_text, 24 * g_phoneText, {a.x, a.y + ts.y / 2}, kGrey, e.disabled.c_str(), nullptr, wrap);
            }
        }
        if (total > rows) {
            // The list's slider (always visible): its thumb along the track; a press above or below it pages.
            const float trackTop = top, trackH = bottom - top, thumbH = std::max(24.0f, trackH * rows / total);
            const float thumbY = trackTop + (trackH - thumbH) * g_phoneScroll / std::max(1, total - rows);
            d->AddRectFilled(at(214, trackTop), at(218, trackTop + trackH), IM_COL32(255, 255, 255, 60), 2 * S);
            pieceIn(d, art::kPhoneSlider, at(210, thumbY), at(222, thumbY + thumbH));
            const float t = sliderFollow(r, trackTop, trackTop + trackH, thumbH);
            if (t >= 0) g_phoneScroll = static_cast<int>(std::lround(t * (total - rows)));
        }
        if (button(at(96, 361), at(129, 392), &over)) {
            // Back: up a level, or home.
            g_phoneApp = g_phoneApp == g_phoneRoot ? -1 : p.nodes[g_phoneApp].parent;
            if (g_phoneApp < 0) g_phoneRoot = -1;
            g_phoneScroll = 0;
        }
        piece(d, over ? art::kPhoneBackOver : art::kPhoneBack, at(96 + 16.5f, 361 + 15.5f), A);
    }
    // The clock in the header's middle (user 2026-10-23 21:35Z): between the home screen's
    // buttons and the battery, small over the other screens' titles.
    if (!g_phoneColorsOpen)
        drawPhoneClock(d, r, g_phoneApp < 0 && !g_phoneNotesOpen && g_phoneOptsGroup.empty() && !g_phoneSettingsOpen &&
                                 g_mySim < 0 && !g_phoneCalOpen && !g_phoneSoundsOpen && p.disabled.empty());
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_phoneIcons.swap(shown);
    }
    if (drag.on && !io.MouseDown[0]) drag.on = false;
    return action;
}

// The phone's pictures on the screen just drawn are all in (or can't be read).
bool phonePicturesIn() {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    for (uint64_t id : g_phoneIcons)
        if (id && !g_loadedIcons.count(id) && !g_badIcons.count(id) && !g_pictureIds.count(id)) return false;
    return true;
}

// Which screen the phone is on (a change fades over from the last one).
int phoneScreenKey() {
    const int opts = static_cast<int>(std::hash<std::wstring>()(g_phoneOptsGroup) & 0xffff);
    return (((g_phoneApp + 2) * 8 + (g_phoneNotesOpen ? 1 : 0) + (g_phoneCalOpen ? 2 : 0) + (g_phoneSettingsOpen ? 4 : 0)) *
                65537 + opts) * 31 + g_phoneSerial + (g_mySim + 1) * 7919 + (g_aspChange ? 104729 : 0) + (g_mySimMoodlet + 1) * 1299709 + (g_hhSim + 1) * 7907 + (g_contacts + 1) * 6151 + (g_gameMenuPhone ? 49157 : 0) + (g_weatherOpen ? 98317 : 0) + (g_cheatsOpen ? 196613 + settings::testingCheats() * 7 : 0) + (g_contacts >= 1 ? g_contactSel * 104723 : 0) + (g_phoneSoundsOpen ? 15485863 : 0);
}

// The still and the cover's blend: premultiplied colours (the atlas's), drawn with ImGui's straight blend otherwise.
void premultipliedBlend(const ImDrawList*, const ImDrawCmd*) {
    if (g_drawContext && g_premultiplied) {
        const float factor[4] = {0, 0, 0, 0};
        g_drawContext->OMSetBlendState(g_premultiplied, factor, 0xffffffff);
    }
}

// The game's spinning plumbob (its loading animation's frames) centred on c, `height` atlas pixels tall.
void plumbob(ImDrawList* d, ImVec2 c, float height, float alpha) {
    static const art::Piece* kFrames[10] = {&art::kSpin0, &art::kSpin1, &art::kSpin2, &art::kSpin3, &art::kSpin4,
                                            &art::kSpin5, &art::kSpin6, &art::kSpin7, &art::kSpin8, &art::kSpin9};
    const art::Piece& a = *kFrames[(GetTickCount64() / 70) % 10];
    const float h = height, w = h * a.w / a.h;
    d->AddImage(reinterpret_cast<ImTextureID>(g_art), {c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2},
                {static_cast<float>(a.x) / art::kAtlasW, static_cast<float>(a.y) / art::kAtlasH},
                {static_cast<float>(a.x + a.w) / art::kAtlasW, static_cast<float>(a.y + a.h) / art::kAtlasH},
                IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)));
}

// The phone's screen into its own list, and the cover over it (PhoneCover). `wait`: something it
// needs from outside the HUD is still coming (vr: the wallpaper, Options' menu).
Action drawPhoneScreen(bool wait, float dt, int* index) {
    PhoneCover& c = g_cover;
    if (g_phoneTakenOut.exchange(false)) c = PhoneCover{};
    const ImGuiIO& io = ImGui::GetIO();
    for (ImDrawList* list : {g_phoneList, g_coverList}) {
        list->_ResetForNewFrame();
        list->Flags = ImGui::GetBackgroundDrawList()->Flags;  // anti-aliasing, vertex offsets: as ImGui's own lists
        list->PushTextureID(io.Fonts->TexID);
        list->PushClipRect({0, 0}, io.DisplaySize);
    }
    const int key = phoneScreenKey();
    Action action = kNone;
    const bool drawn = g_phoneOn && g_phone.resolved;
    if (drawn) {
        action = drawPhone(g_phoneList, index);
    } else {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_phoneIcons.clear();
    }
    const bool changed = phoneScreenKey() != key;  // pressed this frame: this frame still shows the old screen
    const ULONGLONG now = GetTickCount64();
    // Pictures are waited for a moment only (one that never comes doesn't hold the screen).
    const bool dataWait = g_phoneDataUntil && now < g_phoneDataUntil;
    bool ready = drawn && !wait && !dataWait && (phonePicturesIn() || (c.waiting && now - c.waiting > kPictureWait));
    // Once a screen showed ready it stays shown: a picture or value arriving later fills in, never re-covers it.
    if (drawn && !wait && !changed && key == c.readyKey) ready = true;
    if (ready && !changed) c.readyKey = key;
    if (ready || changed) {
        c.waiting = changed ? now : 0;
        c.gaveUp = false;
    } else if (!c.waiting) {
        c.waiting = now;
    } else if (now - c.waiting > kCoverMaxWait && drawn) {
        if (!c.gaveUp) LOG("Phone: its screen waited %llu ms, shown as it is", static_cast<unsigned long long>(now - c.waiting));
        c.gaveUp = true;
    }
    if (c.gaveUp) ready = drawn;
    const bool hold = !ready || changed;
    if (hold) c.fade = 1;
    else if (c.fade > 0) c.fade = std::max(0.0f, c.fade - dt / kCoverFade);
    if (c.fade <= 0 && !hold) c.loading = false;
    // Black (taken out, or no menu) or the last ready screen (a press): how it shows.
    const bool black = c.loading || !c.still || !drawn || !g_stillView;
    const float k = c.fade * c.fade * (3 - 2 * c.fade);  // smooth
    const Rect r = kPhone;
    const ImVec2 a{r.x, r.y}, b{r.x + r.w, r.y + r.h};
    if (k > 0) {
        if (black) {
            g_coverList->AddRectFilled(a, b, IM_COL32(0, 0, 0, static_cast<int>(255 * k)));
        } else {
            g_coverList->AddCallback(premultipliedBlend, nullptr);
            const int v = static_cast<int>(255 * k);
            g_coverList->AddImage(reinterpret_cast<ImTextureID>(g_stillView), a, b, {0, 0}, {1, 1}, IM_COL32(v, v, v, v));
            g_coverList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
    }
    const ULONGLONG waited = c.waiting ? now - c.waiting : 0;
    static float spin = 0;  // the plumbob's own fade (in once waiting a moment, out with the cover)
    spin = hold && waited > kSpinAfter ? std::min(1.0f, spin + dt / 0.15f) : std::max(0.0f, spin - dt / kCoverFade);
    if (black) spin = std::min(spin, k);
    if (spin > 0) {
        constexpr float S = 2.5f;  // atlas pixels per game pixel (drawPhone)
        if (black) plumbob(g_coverList, {r.x + 115 * S, r.y + 190 * S}, 64 * S, spin);  // the loading screen
        else plumbob(g_coverList, {r.x + 186 * S, r.y + 20 * S}, 22 * S, spin);  // a screen building: small, top right
    }
    c.blocked = hold || k > 0.5f;
    static ULONGLONG holdSince = 0;
    static int logged = 0;
    if (hold && !holdSince) holdSince = now;
    if (!hold && holdSince) {
        if (now - holdSince > 60 && logged++ < 40)
            LOG("Phone: its screen was ready after %llu ms (%s)", static_cast<unsigned long long>(now - holdSince),
                black ? "the loading screen" : "the last screen kept");
        holdSince = 0;
    }
    // (drawPhone already took no presses while the cover was up: a press that opens a screen counts.)
    return action;
}

// A notification as a card over a Sim's head (notify.py): the dialog panel,
// its picture on the left, the title, text and time, its answers as the
// dialog's buttons under them (kNoteAnswer, `index` = id * 8 + button); a
// press anywhere else dismisses it.
Action drawNote(uint64_t id, int* index) {
    const Note* n = nullptr;
    for (const Note& k : g_notes.notes)
        if (k.id == id) n = &k;
    // g_noteHeight is read by the card's placement on another thread: never 0 for a moment mid-draw
    // (user 2026-10-23: the card blinked out now and then).
    if (!n) {
        g_noteHeight = 0;
        return kNone;
    }
    const std::string &title = found(n->titleJson), &text = found(n->textJson);
    if (title.empty() && text.empty()) {
        g_noteHeight = 0;
        return kNone;
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kNote;
    const float pad = 22, pic = 96, x0 = r.x + 4, x1 = r.x + r.w - 4, wrap = x1 - x0 - 3 * pad - pic - 44;  // room for the X
    const ImU32 kTitle = kWhite, kBody = kWhite;
    ImVec2 ts = title.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(30, FLT_MAX, wrap, title.c_str());
    ImVec2 xs = text.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(26, FLT_MAX, wrap, text.c_str());
    const int nb = static_cast<int>(n->buttons.size()), perRow = nb > 2 ? 2 : std::max(1, nb);
    const float btnH = 52, btnGap = 6;
    const float btnRows = nb ? static_cast<float>((nb + perRow - 1) / perRow) : 0;
    const float body = std::max(pic + 2 * pad, ts.y + xs.y + 8 + 2 * pad);
    const float h = std::min(r.h - 8, body + (nb ? btnRows * (btnH + btnGap) + pad / 2 : 0));
    noteBackground(d, n->style, {x0, r.y + 4}, {x1, r.y + 4 + h}, 1.0f);
    if (n->picture) {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (std::find(g_noteIcons.begin(), g_noteIcons.end(), n->picture) == g_noteIcons.end()) {
            if (g_noteIcons.size() > 8) g_noteIcons.erase(g_noteIcons.begin());
            g_noteIcons.push_back(n->picture);
        }
    }
    icon(d, n->picture, {x0 + pad + pic / 2, r.y + 4 + pad + pic / 2}, pic);
    const ImVec4 clip{x0, r.y, x1, r.y + 4 + h};
    float y = r.y + 4 + pad;
    const float tx = x0 + 2 * pad + pic;
    if (ts.y > 0) {
        d->AddText(g_title, 30, {tx + 2, y + 2}, kShadow, title.c_str(), nullptr, wrap, &clip);
        d->AddText(g_title, 30, {tx, y}, kTitle, title.c_str(), nullptr, wrap, &clip);
        y += ts.y + 8;
    }
    if (xs.y > 0) {
        d->AddText(g_text, 26, {tx + 2, y + 2}, kShadow, text.c_str(), nullptr, wrap, &clip);
        d->AddText(g_text, 26, {tx, y}, kBody, text.c_str(), nullptr, wrap, &clip);
    }
    // The answers first (the card's own press only covers what is above them).
    Action action = kNone;
    const float bx0 = x0 + pad, bw = (x1 - x0 - 2 * pad - (perRow - 1) * btnGap) / perRow;
    const float by0 = r.y + 4 + std::min(body, h - btnRows * (btnH + btnGap) - pad / 2);
    for (int i = 0; i < nb; ++i) {
        const float bx = bx0 + (i % perRow) * (bw + btnGap), by = by0 + (i / perRow) * (btnH + btnGap);
        if (by + btnH > r.y + 4 + h) break;
        std::string label = found(n->buttons[i]);
        if (label.empty()) label = "OK";
        if (dialogButton(d, 400 + i, label, {bx, by}, {bx + bw, by + btnH})) {
            action = kNoteAnswer;
            if (index) *index = static_cast<int>(n->id * 8 + i);
        }
    }
    // Its close button, top right (user 2026-10-23); a press anywhere above the answers closes it as well.
    bool closed = false;
    {
        const float s = 52;
        const ImVec2 c{x1 - 10 - s / 2, r.y + 4 + 10 + s / 2};
        ImGui::SetCursorScreenPos({c.x - s / 2, c.y - s / 2});
        closed = ImGui::InvisibleButton("noteX", {s, s});
        const bool hover = ImGui::IsItemHovered();
        piece(d, art::kCancel, c, (hover ? s * 1.08f : s) / art::kCancel.w);
    }
    ImGui::SetCursorScreenPos({x0, r.y + 4});
    bool pressed = ImGui::InvisibleButton("note", {x1 - x0, nb ? by0 - (r.y + 4) : h}) || closed;
    g_noteHeight = h + 8;
    if (action != kNone) return action;
    return pressed ? kNoteDismiss : kNone;
}

// The VR keyboard (typing into the game's text fields): the focused field's text on a preview line with the X, and
// four rows of keys as the game's dialog buttons (QWERTY, a symbols page). A key is kKey with `index` = its
// character or a KeyCode; Shift (once, or twice for caps) and the page are ours. kKeyboard.
std::mutex g_keyboardMutex;
std::string g_keyboardText;  // (g_keyboardMutex)
int g_kbShift = 0;           // 0 off, 1 the next letter, 2 caps
bool g_kbSymbols = false;

bool keyButton(ImDrawList* d, int id, const char* label, ImVec2 a, ImVec2 b, bool lit) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(700 + id);
    const bool pressed = ImGui::InvisibleButton("key", {b.x - a.x, b.y - a.y});
    const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PopID();
    if (hover || lit)
        nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.8f, lit && !hover ? IM_COL32(200, 225, 255, 255) : kWhite);
    else
        nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.8f);
    const float size = g_text->FontSize * (strlen(label) > 1 ? 0.7f : 0.92f);
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, 0, label);
    d->AddText(g_text, size, {(a.x + b.x - ts.x) / 2, a.y + (b.y - a.y - 10 - ts.y) / 2 + (held ? 3 : 0)}, uikit::kButtonBlue, label);
    return pressed;
}

Action drawKeyboard(int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kKeyboard;
    const float pad = 14, top = 64, keyH = 80, gap = 8, x0 = r.x + 4, x1 = r.x + r.w - 4;
    Action action = kNone;  // (no panel behind it: transparent, user 2026-10-25)
    auto emit = [&](int code) {
        action = kKey;
        if (index) *index = code;
    };
    // Preview line: the tail of the text that fits, and a caret.
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_keyboardMutex);
        text = g_keyboardText;
    }
    const float closeS = 48, boxR = x1 - pad - closeS - 10, boxL = x0 + pad, boxT = r.y + 4 + 8, boxB = boxT + top - 14;
    d->AddRectFilled({boxL, boxT}, {boxR, boxB}, IM_COL32(255, 255, 255, 235), 10.0f);
    d->AddRect({boxL, boxT}, {boxR, boxB}, IM_COL32(30, 70, 128, 160), 10.0f, 0, 2.0f);
    {
        const float size = 34, room = boxR - boxL - 24;
        size_t from = 0;
        while (from < text.size() && g_text->CalcTextSizeA(size, FLT_MAX, 0, text.c_str() + from).x > room) {
            ++from;
            while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;  // not mid-character
        }
        const std::string shown = text.substr(from) + ((GetTickCount64() / 500) % 2 ? "|" : " ");
        const ImVec4 clip{boxL, boxT, boxR, boxB};
        d->AddText(g_text, size, {boxL + 12, boxT + (boxB - boxT - size) / 2 - 2}, uikit::kButtonBlue, shown.c_str(), nullptr, 0, &clip);
    }
    {
        const ImVec2 c{x1 - pad - closeS / 2, boxT + (boxB - boxT) / 2};
        ImGui::SetCursorScreenPos({c.x - closeS / 2, c.y - closeS / 2});
        const bool closed = ImGui::InvisibleButton("keyboardX", {closeS, closeS});
        const bool hover = ImGui::IsItemHovered();
        piece(d, art::kCancel, c, (hover ? closeS * 1.08f : closeS) / art::kCancel.w);
        if (closed) emit(kKeyClose);
    }
    // Rows of 10 cells.
    static const char* kLetters[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm,."};
    static const char* kSymbols[3] = {"1234567890", "-/:;()$&@", "?!'\"_#%*+="};
    const char** rows = g_kbSymbols ? kSymbols : kLetters;
    const float cell = (x1 - x0 - 2 * pad) / 10.0f;
    float y = r.y + 4 + top + 4;
    auto cellBox = [&](float col, float span, ImVec2& a, ImVec2& b) {
        a = {x0 + pad + col * cell, y};
        b = {x0 + pad + (col + span) * cell - gap, y + keyH};
    };
    int id = 0;
    auto character = [&](char ch, float col) {
        ImVec2 a, b;
        cellBox(col, 1, a, b);
        const bool upper = !g_kbSymbols && g_kbShift > 0 && ch >= 'a' && ch <= 'z';
        const char shown = upper ? static_cast<char>(ch - 32) : ch;
        const char label[2] = {shown, 0};
        if (keyButton(d, id++, label, a, b, false)) {
            emit(static_cast<unsigned char>(shown));
            if (g_kbShift == 1) g_kbShift = 0;
        }
    };
    // Row 1.
    for (int i = 0; rows[0][i]; ++i) character(rows[0][i], static_cast<float>(i));
    y += keyH + gap;
    // Row 2: nine characters and Backspace.
    for (int i = 0; rows[1][i]; ++i) character(rows[1][i], static_cast<float>(i));
    {
        ImVec2 a, b;
        cellBox(9, 1, a, b);
        if (keyButton(d, id++, "<-", a, b, false)) emit(kKeyBackspace);
    }
    y += keyH + gap;
    // Row 3: Shift and nine characters; the symbols page has ten characters.
    {
        const int first = g_kbSymbols ? 0 : 1;
        if (!g_kbSymbols) {
            ImVec2 a, b;
            cellBox(0, 1, a, b);
            if (keyButton(d, id++, g_kbShift == 2 ? "CAPS" : "Shift", a, b, g_kbShift > 0)) g_kbShift = (g_kbShift + 1) % 3;
        }
        for (int i = 0; rows[2][i]; ++i) character(rows[2][i], static_cast<float>(first + i));
    }
    y += keyH + gap;
    // Row 4: page, Space, arrows, Enter.
    {
        ImVec2 a, b;
        cellBox(0, 1, a, b);
        if (keyButton(d, id++, g_kbSymbols ? "ABC" : "123", a, b, false)) g_kbSymbols = !g_kbSymbols, g_kbShift = 0;
        cellBox(1, 5, a, b);
        if (keyButton(d, id++, "Space", a, b, false)) emit(' ');
        cellBox(6, 1, a, b);
        if (keyButton(d, id++, "<", a, b, false)) emit(kKeyLeft);
        cellBox(7, 1, a, b);
        if (keyButton(d, id++, ">", a, b, false)) emit(kKeyRight);
        cellBox(8, 2, a, b);
        if (keyButton(d, id++, "Enter", a, b, false)) emit(kKeyEnter);
    }
    return action;
}

// Balloons over Sims' heads, drawn as Balloon.as lays them out (ObjectBoundElements): the bubble background
// for its type with the icon in it, the small overlay and the category badge. Each in its own slot of kBalloons;
// the slot's top left is the bubble's top left (element -210,-230 around the Sim's anchor).
void drawBalloons() {
    std::vector<Balloon> list;
    {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_itemMutex);
        for (const Balloon& b : g_balloons)
            if (b.until >= now) list.push_back(b);
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    for (const Balloon& b : list) {
        const float x = kBalloons.x + (b.slot % 4) * kBalloonW, y = kBalloons.y + (b.slot / 4) * kBalloonH;
        const art::Piece& bg = b.type == 1 ? art::kBalloonSpeech
                               : b.type == 2 ? art::kBalloonShout
                               : b.type == 3 ? art::kBalloonSentiment
                                             : art::kBalloonThought;
        pieceIn(d, bg, {x, y}, {x + 245, y + 245});
        icon(d, b.picture, {x + 122, y + 108}, 120);
        if (b.overlay) icon(d, b.overlay, {x + 190, y + 172}, 56);
        if (b.category) {
            piece(d, art::kBalloonDisc, {x + 49, y + 166}, 78.0f / art::kBalloonDisc.w);
            icon(d, b.category, {x + 49, y + 166}, 52);
        }
    }
}

// The skill bar (ActionHeadline's SkillBar.as): the dark pill with the level's progress filling it from the
// bottom in green under the glass outline, and the skill's icon on a glass disc beside it. Element units x2.
void drawSkillBars() {
    std::vector<Bar> list;
    {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(g_itemMutex);
        for (const Bar& b : g_bars)
            if (b.until >= now) list.push_back(b);
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const ULONGLONG now = GetTickCount64();
    for (const Bar& b : list) {
        const float x = kBars.x + b.slot * kBarSlot, y = kBars.y;
        const float k = 2.0f;
        double points = b.current + (b.rate > 0 ? b.rate / 60.0 * (now - b.at) / 1000.0 : 0.0);
        double progress = b.level >= b.maxLevel ? 1.0 : (points - b.start) / (b.end - b.start);
        progress = std::max(0.0, std::min(1.0, progress));
        pieceIn(d, art::kBarTrack, {x, y + 2 * k}, {x + 32 * k, y + 110 * k});
        const float fillH = static_cast<float>(progress) * 98 * k, top = y + (5 + 98) * k + 2 * k - fillH;
        if (fillH > 0.5f && uikit::res().art) {  // rounded like the pill it fills
            const art::Piece& f = art::kBarFill;
            d->AddImageRounded(uikit::res().art, {x + 6 * k, top}, {x + 26 * k, y + (5 + 98) * k + 2 * k},
                               {static_cast<float>(f.x) / art::kAtlasW, static_cast<float>(f.y) / art::kAtlasH},
                               {static_cast<float>(f.x + f.w) / art::kAtlasW, static_cast<float>(f.y + f.h) / art::kAtlasH},
                               IM_COL32_WHITE, std::min(10.0f * k, fillH * 0.5f), ImDrawFlags_RoundCornersAll);
        }
        pieceIn(d, art::kBarGlass, {x + 6 * k, y + 7 * k}, {x + 26 * k, y + 105 * k});
        const ImVec2 c{x + 74 * k, y + 56 * k};
        piece(d, art::kBarDisc, c, (69.0f * k) / art::kBarDisc.w);
        icon(d, b.icon, c, 44 * k);
    }
}

Action drawBuild() {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kBuild;
    ImVec2 c{r.x + r.w / 2, r.y + r.h / 2};
    ImGui::SetCursorScreenPos({r.x, r.y});
    bool pressed = ImGui::InvisibleButton("build", {r.w, r.h});
    bool hover = ImGui::IsItemHovered();
    piece(d, art::kQueueDisc[0], c, (r.w - 8) / art::kQueueDisc[0].w, IM_COL32(205, 210, 215, 215));
    piece(d, hover ? art::kBuildIconOver : art::kBuildIcon, c, (r.w * 0.62f) / art::kBuildIcon.w);
    return pressed ? kBuildPressed : kNone;
}

}  // namespace

void addBalloon(uint64_t sim, uint64_t picture, uint64_t overlay, uint64_t category, int type, int priority, int durationMs) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_balloons.erase(std::remove_if(g_balloons.begin(), g_balloons.end(), [&](const Balloon& b) { return b.until < now; }),
                     g_balloons.end());
    Balloon n{sim, picture, overlay, category, type, priority, -1, now, now + 300 + std::max(durationMs, 500) + 200};
    for (Balloon& b : g_balloons) {
        if (b.sim != sim) continue;
        if (b.priority < priority) return;  // the shown one is more important: this one is dropped
        n.slot = b.slot;
        b = n;
        return;
    }
    bool used[kBalloonSlots] = {};
    for (const Balloon& b : g_balloons) used[b.slot] = true;
    for (int i = 0; i < kBalloonSlots && n.slot < 0; ++i)
        if (!used[i]) n.slot = i;
    if (n.slot < 0) return;  // no room: the oldest stays
    g_balloons.push_back(n);
    static int logged = 0;
    if (logged++ < 12)
        LOG("Balloon: Sim %llu type %d priority %d icon %llx overlay %llx for %d ms (slot %d)", static_cast<unsigned long long>(sim),
            type, priority, static_cast<unsigned long long>(picture), static_cast<unsigned long long>(overlay), durationMs, n.slot);
}

int activeBalloons(BalloonInfo* out, int max) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_balloons.erase(std::remove_if(g_balloons.begin(), g_balloons.end(), [&](const Balloon& b) { return b.until < now; }),
                     g_balloons.end());
    int n = 0;
    for (const Balloon& b : g_balloons)
        if (n < max) out[n++] = {b.sim, b.slot, b.type, (now - b.start) / 1000.0f};
    return n;
}

bool balloonActive(uint64_t sim) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    for (const Balloon& b : g_balloons)
        if (b.sim == sim && b.until >= now) return true;
    return false;
}

void setSkillBar(uint64_t sim, uint64_t icon, double start, double end, double current, double rate, int level, int maxLevel, bool hide) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_bars.erase(std::remove_if(g_bars.begin(), g_bars.end(), [&](const Bar& b) { return b.until < now || (hide && b.sim == sim); }),
                 g_bars.end());
    if (hide || !(end > start)) return;
    // SkillBar.as: a growing skill keeps its bar (fresh updates keep coming), a still one hides after
    // SKILL_BAR_HIDE_DELAY (2 s).
    Bar n{sim, icon, start, end, current, rate, level, maxLevel, -1, now, now + (rate > 0 ? 60000 : 2300)};
    for (Bar& b : g_bars) {
        if (b.sim != sim) continue;
        n.slot = b.slot;
        b = n;
        return;
    }
    bool used[kBarSlots] = {};
    for (const Bar& b : g_bars) used[b.slot] = true;
    for (int i = 0; i < kBarSlots && n.slot < 0; ++i)
        if (!used[i]) n.slot = i;
    if (n.slot < 0) return;
    g_bars.push_back(n);
    static int logged = 0;
    if (logged++ < 12)
        LOG("Skill bar: Sim %llu skill icon %llx level %d/%d points %.0f of %.0f..%.0f rate %.2f (slot %d)", static_cast<unsigned long long>(sim),
            static_cast<unsigned long long>(icon), level, maxLevel, current, start, end, rate, n.slot);
}

int activeBars(BarInfo* out, int max) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_bars.erase(std::remove_if(g_bars.begin(), g_bars.end(), [&](const Bar& b) { return b.until < now; }), g_bars.end());
    int n = 0;
    for (const Bar& b : g_bars)
        if (n < max) out[n++] = {b.sim, b.slot};
    return n;
}

bool barActive(uint64_t sim) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_itemMutex);
    for (const Bar& b : g_bars)
        if (b.sim == sim && b.until >= now) return true;
    return false;
}

void setItems(int kind, const Item* items, int count) {
    if (kind < 0 || kind >= kKinds) return;
    count = std::max(0, std::min(count, kMaxItems));
    std::lock_guard<std::mutex> lock(g_itemMutex);
    std::copy(items, items + count, g_items[kind]);
    g_counts[kind] = count;
}

int itemCount(int kind) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    return kind >= 0 && kind < kKinds ? g_counts[kind] : 0;
}

bool hasItems(int kind) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    return kind >= 0 && kind < kKinds && g_counts[kind] > 0;
}

void queueIcon(uint64_t instance, const uint32_t* rgba, int w, int h);  // below

void setIcon(uint64_t instance, const uint8_t* data, size_t size) {
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
    if (!data || !size || !phone3d::decodePicture(data, size, rgba, w, h) || w <= 0 || h <= 0) {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_badIcons.insert(instance);
        // What it was, to add its format (DDS: its FourCC at 84, flags at 80, bits at 88).
        char head[5] = {}, fourcc[5] = {};
        uint32_t ddsFlags = 0, ddsBits = 0;
        if (data && size >= 4) std::memcpy(head, data, 4);
        if (data && size >= 128) {
            std::memcpy(fourcc, data + 84, 4);
            std::memcpy(&ddsFlags, data + 80, 4);
            std::memcpy(&ddsBits, data + 88, 4);
        }
        for (char& c : head) c = c && (c < 32 || c > 126) ? '?' : c;
        for (char& c : fourcc) c = c && (c < 32 || c > 126) ? '?' : c;
        LOG("HUD: icon %016llx could not be read (%zu bytes, starts '%s', DDS FourCC '%s' flags %x bits %u)",
            static_cast<unsigned long long>(instance), data ? size : 0, head, fourcc, ddsFlags, ddsBits);
        return;
    }
    queueIcon(instance, rgba.data(), w, h);
}

void expectPicture(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_pictureIds.insert(id);
}

void setPicture(uint64_t id, const uint32_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    expectPicture(id);
    queueIcon(id, rgba, w, h);
}

bool hasPicture(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    if (g_loadedIcons.count(id)) return true;
    for (const PendingIcon& p : g_pendingIcons)
        if (p.id == id) return true;
    return false;
}

// A picture into a square slot, aspect kept, filtered down (box) or up (nearest).
void queueIcon(uint64_t instance, const uint32_t* rgba, int w, int h) {
    PendingIcon p{instance, std::vector<uint32_t>(kIconSize * kIconSize, 0)};
    float scale = std::min(static_cast<float>(kIconSize) / w, static_cast<float>(kIconSize) / h);
    int dw = std::max(1, static_cast<int>(w * scale)), dh = std::max(1, static_cast<int>(h * scale));
    int ox = (kIconSize - dw) / 2, oy = (kIconSize - dh) / 2;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
            int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
            float sum[4] = {}, weight = 0;
            for (int sy = y0; sy < y1 && sy < h; ++sy)
                for (int sx = x0; sx < x1 && sx < w; ++sx) {
                    uint32_t c = rgba[static_cast<size_t>(sy) * w + sx];
                    float a = static_cast<float>(c >> 24);
                    sum[0] += (c & 0xff) * a;
                    sum[1] += ((c >> 8) & 0xff) * a;
                    sum[2] += ((c >> 16) & 0xff) * a;
                    sum[3] += a;
                    weight += 1;
                }
            if (weight <= 0) continue;
            uint32_t out = 0;
            if (sum[3] > 0) {
                for (int k = 0; k < 3; ++k) out |= static_cast<uint32_t>(std::min(255.0f, sum[k] / sum[3] + 0.5f)) << (8 * k);
                out |= static_cast<uint32_t>(std::min(255.0f, sum[3] / weight + 0.5f)) << 24;
            }
            p.pixels[static_cast<size_t>(oy + y) * kIconSize + ox + x] = out;
        }
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_pendingIcons.push_back(std::move(p));
}

int missingIcons(uint64_t* out, int max) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    int n = 0;
    auto want = [&](uint64_t id) {
        if (n >= max || !id || g_loadedIcons.count(id) || g_badIcons.count(id) || g_pictureIds.count(id)) return;
        bool queued = false;
        for (const PendingIcon& p : g_pendingIcons) queued = queued || p.id == id;
        for (int j = 0; j < n; ++j) queued = queued || out[j] == id;
        if (!queued) out[n++] = id;
    };
    for (int k = 0; k < kKinds; ++k)
        for (int i = 0; i < g_counts[k]; ++i) want(g_items[k][i].icon);
    for (uint64_t id : g_pickerIcons) want(id);
    for (uint64_t id : g_pieIcons) want(id);
    for (uint64_t id : g_popupIcons) want(id);
    for (uint64_t id : g_phoneIcons) want(id);
    for (uint64_t id : g_noteIcons) want(id);
    for (uint64_t id : g_nbIcons) want(id);
    for (const Balloon& b : g_balloons) want(b.picture), want(b.overlay), want(b.category);
    for (const Bar& b : g_bars) want(b.icon);
    for (uint64_t id : g_phonePreload) want(id);  // what the phone may show next (read ahead)
    for (uint64_t id : g_notesPreload) want(id);
    return n;
}

void setPhoneContent(const char* data, size_t size) {
    PhoneContent c;
    std::string all(data ? data : "", data ? size : 0);
    {
        std::lock_guard<std::mutex> lock(g_phoneMutex);
        if (all == g_phoneRaw) return;  // the same menu again: the screen stays as it is
        g_phoneRaw = all;
    }
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (f[0] == "phone" && f.size() >= 3) {
            c.valid = true;
            c.silenced = f[1] == "1";
            c.disabledJson = f[2];
        } else if (f[0] == "colors" && f.size() >= 3) {
            c.color = std::atoi(f[1].c_str());
            size_t p = 0;
            while (p < f[2].size()) {
                size_t comma = f[2].find(',', p);
                c.colors.push_back(std::strtoull(f[2].substr(p, comma == std::string::npos ? std::string::npos : comma - p).c_str(), nullptr, 10));
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        } else if ((f[0] == "cat" && f.size() >= 6) || (f[0] == "item" && f.size() >= 7)) {
            const int node = std::atoi(f[1].c_str()), parent = std::atoi(f[2].c_str());
            if (node != static_cast<int>(c.nodes.size()) || parent >= node) break;  // in order, parents first
            PhoneNode n;
            n.parent = parent;
            if (f[0] == "cat") {
                n.category = true;
                n.icon = std::strtoull(f[3].c_str(), nullptr, 10);
                n.nameJson = f[4];
                n.priority = std::atoi(f[5].c_str());
            } else {
                n.nameJson = f[3];
                n.disabledJson = f[4];
                n.notification = f[5] == "1";
                n.priority = std::atoi(f[6].c_str());
            }
            c.nodes.push_back(n);
            if (parent >= 0) c.nodes[parent].children.push_back(node);
            else if (n.category) c.home.push_back(node);  // loose entries on the home screen: the game shows none
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_phoneMutex);
    g_phoneIn = c;
    g_phoneNew = true;
    static int logged = 0;
    if (logged++ < 30) LOG("Phone: the game's phone menu, %zu entries, %zu apps%s", c.nodes.size(), c.home.size(),
                           c.disabledJson.empty() ? "" : " (disabled now)");
}

void phoneHome() { g_phoneHomeAsked = true; }
void phoneTakenOut() {
    g_phoneHomeAsked = true;
    g_phoneTakenOut = true;
}

void setPhoneNotes(const char* data, size_t size) {
    NotesContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (f[0] == "notes" && f.size() >= 2) {
            c.unread = std::atoi(f[1].c_str());
        } else if (f[0] == "note" && f.size() >= 7) {
            Note n;
            n.id = std::strtoull(f[1].c_str(), nullptr, 10);
            n.read = f[2] == "1";
            n.picture = std::strtoull(f[3].c_str(), nullptr, 10);
            n.titleJson = f[4];
            n.textJson = f[5];
            n.time = f[6];
            if (f.size() >= 8) n.style = std::atoi(f[7].c_str());
            if (f.size() >= 9 && !f[8].empty()) {
                size_t b = 0;
                while (n.buttons.size() < 8) {
                    const size_t e = f[8].find('\x1d', b);
                    n.buttons.push_back(f[8].substr(b, e == std::string::npos ? std::string::npos : e - b));
                    if (e == std::string::npos) break;
                    b = e + 1;
                }
            }
            c.notes.push_back(n);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_notesMutex);
    g_notesIn = c;
    g_notesNew = true;
}

float noteHeight() { return g_noteHeight; }

std::atomic<bool> g_keyboardManual{false};
void toggleKeyboard() { g_keyboardManual = !g_keyboardManual.load(); }
void setKeyboardManual(bool on) { g_keyboardManual = on; }
bool keyboardManual() { return g_keyboardManual.load(); }

void setKeyboardText(const char* utf8) {
    std::lock_guard<std::mutex> lock(g_keyboardMutex);
    g_keyboardText = utf8 ? utf8 : "";
}

void setPhoneCalendar(const char* data, size_t size) {
    CalendarContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (f[0] == "cal" && f.size() >= 5) {
            c.valid = true;
            c.titleJson = f[1];
            c.today = std::atoi(f[2].c_str());
            c.count = std::max(0, std::min(7 * 53, std::atoi(f[3].c_str())));
            c.emptyJson = f[4];
            c.days.assign(static_cast<size_t>(c.count), CalDay{});
        } else if (f[0] == "day" && f.size() >= 9 && c.valid && c.count > 0) {
            const int i = std::max(0, std::min(c.count - 1, std::atoi(f[1].c_str())));  // (count >= 1 here: days were sent)
            CalDay& d = c.days[i];
            d.season = std::atoi(f[2].c_str());
            d.weather = std::strtoull(f[3].c_str(), nullptr, 10);
            d.moon = std::atoi(f[4].c_str());
            d.main = std::atoi(f[5].c_str());
            d.mainPicture = std::strtoull(f[6].c_str(), nullptr, 10);
            for (size_t k = 0; k < f[7].size();) {
                size_t e = f[7].find(',', k);
                if (e == std::string::npos) e = f[7].size();
                if (e > k) d.small.push_back(std::atoi(f[7].substr(k, e - k).c_str()));
                k = e + 1;
            }
            d.forecastJson = f[8];
        } else if (f[0] == "item" && f.size() >= 6) {
            CalItem it;
            it.day = std::atoi(f[1].c_str());
            it.kind = std::atoi(f[2].c_str());
            it.picture = std::strtoull(f[3].c_str(), nullptr, 10);
            it.nameJson = f[4];
            it.timeJson = f[5];
            c.items.push_back(it);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_calMutex);
    g_calIn = c;
    g_calNew = true;
}

void setPhoneAsleep(bool asleep) { g_phoneAsleep = asleep; }

void setPhoneSim(const char* data, size_t size) {
    SimContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (f[0] == "sim" && f.size() >= 2) {
            c.age = std::atoi(f[1].c_str());
            if (f.size() >= 3) c.name = f[2];
            if (f.size() >= 4 && !f[3].empty()) c.funds = std::atoll(f[3].c_str());
        } else if (f[0] == "cat" && f.size() >= 3) {
            c.categories.emplace_back(std::atoi(f[1].c_str()), f[2]);
        } else if (f[0] == "skill" && f.size() >= 9 && c.skills.size() < 100) {
            SimSkill k;
            k.category = std::atoi(f[1].c_str());
            k.icon = std::strtoull(f[2].c_str(), nullptr, 10);
            k.nameJson = f[3];
            k.level = std::atoi(f[4].c_str());
            k.maxLevel = std::atoi(f[5].c_str());
            k.progress = static_cast<float>(std::atof(f[6].c_str()));
            k.mastered = f[7] == "1";
            k.rising = f[8] == "1";
            c.skills.push_back(k);
        } else if (f[0] == "asp" && f.size() >= 8) {
            SimAspiration& a = c.aspiration;
            a.valid = true;
            a.icon = std::strtoull(f[1].c_str(), nullptr, 10);
            a.nameJson = f[2];
            a.textJson = f[3];
            a.current = std::atoi(f[4].c_str());
            a.count = std::atoi(f[5].c_str());
            a.progress = static_cast<float>(std::atof(f[6].c_str()));
            a.done = f[7] == "1";
        } else if (f[0] == "mile" && f.size() >= 4 && c.aspiration.milestones.size() < 20) {
            c.aspiration.milestones.emplace_back(f[2], f[3] == "1");
        } else if (f[0] == "goal" && f.size() >= 6 && c.aspiration.goals.size() < 20) {
            SimGoal g;
            g.textJson = f[1];
            g.done = f[2] == "1";
            g.value = std::atoi(f[3].c_str());
            g.goal = std::atoi(f[4].c_str());
            g.showProgress = f[5] == "1";
            c.aspiration.goals.push_back(g);
        } else if (f[0] == "job" && f.size() >= 10 && c.jobs.size() < 8) {
            SimJob j;
            j.icon = std::strtoull(f[1].c_str(), nullptr, 10);
            j.nameJson = f[2];
            j.titleJson = f[3];
            j.level = std::atoi(f[4].c_str());
            j.levels = std::atoi(f[5].c_str());
            j.performance = static_cast<float>(std::atof(f[6].c_str()));
            j.pay = f[7];
            j.companyJson = f[8];
            j.shift = f[9];
            c.jobs.push_back(j);
        } else if (f[0] == "wx2" && f.size() >= 18) {
            SimWeather& w = c.weather;
            w.sent = true;
            w.season = std::atoi(f[1].c_str());
            w.day = std::atoi(f[2].c_str());
            w.length = std::atoi(f[3].c_str());
            for (int k = 0; k < 3; ++k) {
                w.icon[k] = std::strtoull(f[4 + 2 * k].c_str(), nullptr, 10);
                w.nameJson[k] = f[5 + 2 * k];
            }
            w.phase = std::atoi(f[10].c_str());
            w.effectJson = f[11];
            w.eco = std::atoi(f[12].c_str());
            w.ecoProgress = static_cast<float>(std::atof(f[13].c_str()));
            w.ecoDelta = static_cast<float>(std::atof(f[14].c_str()));
            w.ecoNameJson = f[15];
            w.ecoTextJson = f[16];
            w.enabled = f[17] == "1";
        } else if (f[0] == "biz" && f.size() >= 8 && c.businesses.size() < 10) {
            SimBusiness b;
            b.type = std::atoi(f[1].c_str());
            b.name = f[2];
            b.open = f[3] == "1";
            b.funds = std::atoi(f[4].c_str());
            b.stars = static_cast<float>(std::atof(f[5].c_str()));
            b.revenue = std::atoi(f[6].c_str());
            b.employees = std::atoi(f[7].c_str());
            c.businesses.push_back(b);
        } else if (f[0] == "club" && f.size() >= 7 && c.clubs.size() < 10) {
            SimClub k;
            k.icon = std::strtoull(f[2].c_str(), nullptr, 10);
            k.name = f[3];
            k.description = f[4];
            k.leader = f[5];
            k.gathering = f[6] == "1";
            c.clubs.push_back(k);
        } else if (f[0] == "cmem" && f.size() >= 5 && !c.clubs.empty()) {
            c.clubs.back().members.push_back({std::strtoull(f[2].c_str(), nullptr, 10), f[3], f[4] == "1"});
        } else if (f[0] == "crule" && f.size() >= 4 && !c.clubs.empty()) {
            c.clubs.back().rules.emplace_back(f[2] == "1", f[3]);
        } else if (f[0] == "sj" && f.size() >= 6) {
            c.souls.sent = true;
            c.souls.ghost = f[1] == "1";
            c.souls.rank = std::atoi(f[2].c_str());
            c.souls.progress = static_cast<float>(std::atof(f[3].c_str()));
            c.souls.first = f[4];
            c.souls.complete = f[5] == "1";
        } else if (f[0] == "sjg" && f.size() >= 6 && c.souls.goals.size() < 20) {
            c.souls.goals.push_back({f[1], f[2] == "1", std::atoi(f[3].c_str()), std::atoi(f[4].c_str()), f[5] == "1",
                                     f.size() >= 7 ? f[6] : std::string()});
        } else if (f[0] == "sjt" && f.size() >= 3) {
            const int which = std::atoi(f[1].c_str());
            if (which >= 0 && which < 12) c.souls.tips[which] = f[2];
        } else if (f[0] == "sy" && f.size() >= 5 && c.simology.size() < 120) {
            c.simology.push_back({f[1].empty() ? 'T' : f[1][0], std::atoi(f[2].c_str()), std::strtoull(f[3].c_str(), nullptr, 10), f[4]});
        } else if (f[0] == "inv" && f.size() >= 6 && c.inventory.size() < 120) {
            SimItem it;
            it.picture = std::strtoull(f[2].c_str(), nullptr, 10);
            it.name = f[3];
            it.count = std::atoi(f[4].c_str());
            it.value = std::atoi(f[5].c_str());
            it.order = static_cast<int>(c.inventory.size());
            if (f.size() >= 8) it.favorite = f[6] == "1", it.locked = f[7] == "1";
            c.inventory.push_back(it);
        } else if (f[0] == "ct" && f.size() >= 9 && c.contacts.size() < 80) {
            SimContact k;
            k.name = f[2];
            k.face = std::strtoull(f[3].c_str(), nullptr, 10);
            k.friendship = static_cast<float>(std::atof(f[4].c_str()));
            k.hasRomance = !f[5].empty();
            k.romance = static_cast<float>(std::atof(f[5].c_str()));
            k.friendBitJson = f[6];
            k.romanceBitJson = f[7];
            k.here = f[8] == "1";
            if (f.size() >= 10) k.familyBitJson = f[9];
            if (f.size() >= 11) k.tags = f[10];
            c.contacts.push_back(k);
        } else if (f[0] == "cd" && f.size() >= 4) {
            c.contact.index = std::atoi(f[1].c_str());
            c.contact.bust = std::strtoull(f[2].c_str(), nullptr, 10);
            c.contact.unknown = std::atoi(f[3].c_str());
        } else if (f[0] == "cdt" && f.size() >= 4 && c.contact.traits.size() < 60) {
            c.contact.traits.push_back({f[1], f[3], std::strtoull(f[2].c_str(), nullptr, 10)});
        } else if (f[0] == "cdk" && f.size() >= 3 && c.contact.facts.size() < 20) {
            c.contact.facts.push_back({f[1], f[2]});
        } else if (f[0] == "cds" && f.size() >= 4 && c.contact.sentiments.size() < 20) {
            c.contact.sentiments.push_back({std::strtoull(f[1].c_str(), nullptr, 10), f[2], f[3]});
        } else if (f[0] == "cdo" && f.size() >= 3) {
            c.contact.calls = std::atoi(f[1].c_str());
            c.contact.texts = std::atoi(f[2].c_str());
        } else if (f[0] == "cdn" && f.size() >= 2) {
            c.contact.note = f[1];
        } else if (f[0] == "cda" && f.size() >= 4 && c.contact.actions.size() < 40) {
            c.contact.actions.push_back({std::atoi(f[1].c_str()), f[2], f[3]});
        } else if (f[0] == "cdg" && f.size() >= 3) {
            c.contact.age = ContactPage::Bit{std::strtoull(f[1].c_str(), nullptr, 10), f[2], std::string()};
        } else if (f[0] == "cdj" && f.size() >= 6 && c.contact.jobs.size() < 4) {
            c.contact.jobs.push_back({std::strtoull(f[1].c_str(), nullptr, 10), f[2], f[3], f[4], f[5]});
        } else if ((f[0] == "cdc" || f[0] == "cdx") && f.size() >= 4) {
            ContactPage::Bit& b = f[0] == "cdc" ? c.contact.compatibility : c.contact.attraction;
            b = ContactPage::Bit{std::strtoull(f[1].c_str(), nullptr, 10), f[2], f[3]};
        } else if (f[0] == "cdb" && f.size() >= 3 && c.contact.bits.size() < 30) {
            c.contact.bits.push_back({std::strtoull(f[1].c_str(), nullptr, 10), f[2], std::string()});
        } else if (f[0] == "hh" && f.size() >= 9 && c.household.size() < 16) {
            SimMember m;
            m.index = std::atoi(f[1].c_str());
            m.first = f[2];
            m.name = f[3];
            m.age = std::atoi(f[4].c_str());
            m.here = f[5] == "1";
            m.active = f[6] == "1";
            m.face = std::strtoull(f[7].c_str(), nullptr, 10);
            m.bust = std::strtoull(f[8].c_str(), nullptr, 10);
            c.household.push_back(m);
        } else if (f[0] == "mood" && f.size() >= 3) {
            c.mood = std::atoi(f[1].c_str());
            c.moodJson = f[2];
        } else if (f[0] == "acat" && f.size() >= 3 && c.trackCategories.size() < 40) {
            c.trackCategories.push_back(f[2]);
        } else if (f[0] == "atrack" && f.size() >= 6 && c.tracks.size() < 300) {
            SimTrack t;
            t.category = std::atoi(f[2].c_str());
            t.icon = std::strtoull(f[3].c_str(), nullptr, 10);
            t.nameJson = f[4];
            t.current = f[5] == "1";
            c.tracks.push_back(t);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_simMutex);
    g_simIn = c;
    g_simNew = true;
}

void setPickerContent(const char* data, size_t size) {
    PickerContent c;
    std::string all(data ? data : "", data ? size : 0);
    auto numbers = [](const std::string& s) {
        std::vector<long long> v;
        for (size_t k = 0; k < s.size();) {
            size_t e = s.find(',', k);
            if (e == std::string::npos) e = s.size();
            if (e > k) v.push_back(std::atoll(s.substr(k, e - k).c_str()));
            k = e + 1;
        }
        return v;
    };
    size_t at = 0;
    bool first = true;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        const std::vector<std::string> f = fieldsOf(all.substr(at, end - at));
        at = end + 1;
        if (first) {
            first = false;
            if (f.size() >= 4 && f[0] == "picker") {
                c.dialog = std::atoll(f[1].c_str());
                c.flags = std::atoi(f[2].c_str());
                c.sort = f[3];
            }
        } else if (f[0] == "title" && f.size() > 1) {
            c.titleJson = f[1];
        } else if (f[0] == "filter" && f.size() > 3) {
            PickerFilter filter;
            filter.tag = static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
            filter.icon = std::strtoull(f[2].c_str(), nullptr, 10);
            filter.json = f[3];
            c.filters.push_back(filter);
        } else if (f[0] == "row" && f.size() > 13) {
            PickerRow row;
            row.top = f[1] == "1";
            row.nameJson = f[2];
            row.icon = std::strtoull(f[3].c_str(), nullptr, 10);
            row.enabled = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
            for (int k = 0; k < 4; ++k) row.price[k] = static_cast<unsigned>(std::strtoul(f[5 + k].c_str(), nullptr, 10));
            row.discounted = static_cast<unsigned>(std::strtoul(f[9].c_str(), nullptr, 10));
            row.skill = std::atoi(f[10].c_str());
            row.servingJson = f[11];
            for (long long k : numbers(f[12])) row.servings.push_back(static_cast<int>(k));
            for (long long k : numbers(f[13])) row.tags.push_back(static_cast<unsigned>(k));
            c.rows.push_back(row);
        }
        if (end >= all.size()) break;
    }
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    g_pickerIn = c;
    g_pickerNew = true;
    g_pickerHeight = 0;
    g_pickerState = c.rows.empty() ? 0 : 1;
}

bool pickerPending() {
    int state = g_pickerState.load();
    return state == 1 || state == 2;
}

float pickerHeight() { return g_pickerState.load() == 2 ? g_pickerHeight.load() : 0.0f; }

void setPopupContent(const char* data, size_t size) {
    PopupContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    bool first = true;
    while (at <= all.size() && !all.empty()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        std::string record = all.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> f;
        for (size_t k = 0;;) {
            size_t e = record.find('\x1f', k);
            f.push_back(record.substr(k, e == std::string::npos ? std::string::npos : e - k));
            if (e == std::string::npos) break;
            k = e + 1;
        }
        if (first) {
            c.layout = f[0] == "1" ? 1 : f[0] == "2" ? 2 : f[0] == "3" ? 3 : 0;
            for (int k = 0; k < 2; ++k)
                if (f.size() > static_cast<size_t>(2 + k)) c.pictures[k] = std::strtoull(f[2 + k].c_str(), nullptr, 10);
            first = false;
        } else if (f[0] == "title" && f.size() > 1) {
            c.titleJson = f[1];
        } else if (f[0] == "text" && f.size() > 1) {
            c.textJson = f[1];
        } else if (f[0] == "list" && f.size() > 4) {
            c.multi = f[1] == "1";
            c.minSel = std::atoi(f[2].c_str());
            c.maxSel = std::atoi(f[3].c_str());
            c.canClose = f[4] == "1";
        } else if (f[0] == "subtitle" && f.size() > 1) {
            c.subtitleJson = f[1];
        } else if (f[0] == "row" && f.size() > 5) {
            PopupContent::Row r;
            r.icon = std::strtoull(f[1].c_str(), nullptr, 10);
            r.nameJson = f[2];
            r.descJson = f[3];
            r.enabled = f[4] == "1";
            r.selected = f[5] == "1";
            c.rows.push_back(r);
        } else if (f[0] == "button" && f.size() > 2) {
            c.buttonJson.push_back(f[1]);
            c.choices.push_back(std::atoll(f[2].c_str()));
            c.tipJson.push_back(f.size() > 3 ? f[3] : std::string());
        }
        if (end >= all.size()) break;
    }
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_popupIcons.assign(c.pictures, c.pictures + 2);
    }
    std::lock_guard<std::mutex> lock(g_popupMutex);
    g_popupIn = c;
    g_popupNew = true;
    g_popupHeight = 0;
    g_popupState = !c.textJson.empty() || !c.titleJson.empty() || !c.buttonJson.empty() || !c.rows.empty() ? 1 : 0;
}

std::vector<int> listPicked() { return g_listPicked; }
int phoneTaps() { return g_phoneTaps.exchange(0); }
int phoneHovers() { return g_phoneHovers.exchange(0); }
int phoneModeSet() { return g_phoneModeSet.exchange(-1); }
std::vector<int> inventoryPicked() { return g_invPicked; }

void setTexts(int kind, const char* data, size_t size) {
    if (kind < 0 || kind >= kKinds) return;
    std::vector<Tip> tips;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        std::string record = all.substr(at, end - at);
        Tip tip;
        size_t k = 0;
        for (int field = 0; field < 4; ++field) {
            size_t e = record.find('\x1f', k);
            std::string value = record.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (field < 3)
                tip.json[field] = value;
            else
                tip.minutes = std::atoi(value.c_str());
            if (e == std::string::npos) break;
            k = e + 1;
        }
        tips.push_back(tip);
        if (end >= all.size()) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_tips[kind] = tips;
}

int tipKind() { return g_tipKind.load(); }

void moneyChanged(long long amount) {
    if (!amount) return;
    std::lock_guard<std::mutex> lock(g_moneyMutex);
    g_money.push_back({amount, GetTickCount64()});
    if (g_money.size() > 6) g_money.erase(g_money.begin());
}

bool moneyShowing() {
    std::lock_guard<std::mutex> lock(g_moneyMutex);
    const ULONGLONG now = GetTickCount64();
    for (const MoneyChange& m : g_money)
        if (now - m.at <= kMoneySeconds * 1000) return true;
    return false;
}
float tipHeight() { return g_tipHeight.load(); }

bool popupPending() {
    int state = g_popupState.load();
    return state == 1 || state == 2;
}

float popupHeight() { return g_popupState.load() == 2 ? g_popupHeight.load() : 0.0f; }

void setClock(int hour, int minute, int day, int speed) {
    g_hour = hour;
    g_minute = minute;
    g_day = day;
    g_speed = speed;
}

#include "hud_pie.inc"
#include "hud_buildtray.inc"

namespace {
#include "notebook_ui.inc"
}  // namespace

// Python (notebook.py): the game's notebook for the book in your hand; empty: gone.
void setNotebookContent(const char* data, size_t size) {
    NbContent c;
    const std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    NbCat* cat = nullptr;
    NbSub* sub = nullptr;
    NbEntry* entry = nullptr;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        const std::vector<std::string> f = fieldsOf(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? all.size() + 1 : end + 1;
        if (f[0] == "nb" && f.size() >= 4) {
            c.valid = true;
            c.name = f[1];
            c.portrait = std::strtoull(f[2].c_str(), nullptr, 10);
            c.notes = f[3];
        } else if (f[0] == "cat" && f.size() >= 5) {
            c.cats.emplace_back();
            cat = &c.cats.back();
            sub = nullptr;
            entry = nullptr;
            cat->nameJson = f[1];
            cat->descJson = f[2];
            cat->icon = std::strtoull(f[3].c_str(), nullptr, 10);
            cat->large = std::strtoull(f[4].c_str(), nullptr, 10);
        } else if (f[0] == "sub" && f.size() >= 3 && cat) {
            cat->subs.emplace_back();
            sub = &cat->subs.back();
            entry = nullptr;
            sub->nameJson = f[1];
            sub->icon = std::strtoull(f[2].c_str(), nullptr, 10);
        } else if (f[0] == "ent" && f.size() >= 5 && sub) {
            sub->entries.emplace_back();
            entry = &sub->entries.back();
            entry->textJson = f[1];
            entry->descJson = f[2];
            entry->icon = std::strtoull(f[3].c_str(), nullptr, 10);
            entry->isNew = f[4] == "1";
        } else if (f[0] == "itm" && f.size() >= 5 && entry) {
            NbItem item;
            item.textJson = f[1];
            item.icon = std::strtoull(f[2].c_str(), nullptr, 10);
            item.count = std::atoi(f[3].c_str());
            item.total = std::atoi(f[4].c_str());
            entry->items.push_back(item);
        }
        if (end == std::string::npos) break;
    }
    if (c.valid)
        LOG("HUD: notebook of %s: %zu categories", c.name.c_str(), c.cats.size());
    std::lock_guard<std::mutex> lock(g_nbMutex);
    g_nbIn = c;
    g_nbNew = true;
}

int notebookSpreads() { return g_nbSpreads.load(); }
int notebookTakeSpread() { return g_nbWant.exchange(-1); }

Action render(ID3D11Device* device, ID3D11DeviceContext* context, const Frame& f, int* index) {
    if (g_failed) return kNone;
    if (!g_imgui && !init(device, context)) {
        g_failed = true;
        return kNone;
    }
    if (device != g_device) return kNone;  // a new game device: not ours to draw on
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now, rate;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    float dt = static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(rate.QuadPart);
    g_last = now;
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 72;
    io.AddMousePosEvent(f.px >= 0 ? f.px : -FLT_MAX, f.px >= 0 ? f.py : -FLT_MAX);
    io.AddMouseButtonEvent(0, f.down && f.px >= 0);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    // One invisible window over the whole atlas holds the buttons.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ++g_frameNo;
    uploadIcons(context);
    Item items[kKinds][kMaxItems];
    int counts[kKinds];
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        std::copy(&g_items[0][0], &g_items[0][0] + kKinds * kMaxItems, &items[0][0]);
        std::copy(g_counts, g_counts + kKinds, counts);
    }
    Action action = kNone;
    auto take = [&](Action a) {
        if (a != kNone) action = a;
    };
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 green = IM_COL32(40, 220, 90, 90), edge = IM_COL32(40, 255, 110, 200);
        auto box = [&](const Rect& r) {
            dl->AddRectFilled({r.x, r.y}, {r.x + r.w, r.y + r.h}, green, 12.0f);
            dl->AddRect({r.x + 2, r.y + 2}, {r.x + r.w - 2, r.y + r.h - 2}, edge, 12.0f, 0, 4.0f);
        };
        if (f.preview.w > 0) box(f.preview);
        if (f.previewCard) box(kPreview);
    }
    if (f.watch) take(drawWatch(f));
    if (f.pause) drawPause();
    if (f.build) take(drawBuild());
    if (f.buildTray) take(drawBuildTray(f, index));
    g_hoverKind = g_hoverIndex = -1;
    if (f.needs) drawNeeds(items[kNeedItems], counts[kNeedItems]);
    if (f.moodlets) drawMoodlets(items[kMoodletItems], counts[kMoodletItems]);
    if (f.queue) take(drawQueue(items[kQueueItems], items[kQueueBadges], counts[kQueueItems], index));
    if (f.wants) take(drawWants(items[kWantItems], counts[kWantItems], index));
    if (f.money) drawMoney();
    if (f.balloons) drawBalloons();
    if (f.bars) drawSkillBars();
    {
        Tip tip;
        bool have = false;
        if (g_hoverKind >= 0) {
            std::lock_guard<std::mutex> lock(g_itemMutex);
            const std::vector<Tip>& tips = g_tips[g_hoverKind];
            if (g_hoverIndex >= 0 && g_hoverIndex < static_cast<int>(tips.size())) tip = tips[g_hoverIndex], have = true;
        }
        bool drawn = have && drawTip(tip);
        g_tipKind = drawn ? g_hoverKind : -1;
        if (!drawn) g_tipHeight = 0;
    }
    resolvePopup();
    if (f.popup && g_popupOn && g_popup.resolved && g_popup.valid) take(drawPopup(index));
    resolvePicker();
    if (f.picker && g_pickerOn && g_picker.resolved && g_picker.valid) take(drawPicker(index));
    resolvePhone();
    g_drawContext = context;
    const bool phoneShown = f.phone && g_phoneList && g_coverList;
    g_phoneText = f.phoneText;
    if (phoneShown) take(drawPhoneScreen(f.phoneWait, io.DeltaTime, index));
    resolvePie();
    if (f.pie && g_pieMenu.valid) take(drawPie(index));
    if (f.note) take(drawNote(f.note, index));
    else g_noteHeight = 0;
    if (f.keyboard) take(drawKeyboard(index));
    if (f.book) drawNotebook(f);
    ImGui::End();
    ImGui::Render();
    if (phoneShown) {
        // The phone's screen (unless the cover hides it whole), then its cover: after the rest, in its own place.
        const bool whole = g_cover.fade >= 1;
        if (!whole) ImGui::GetDrawData()->AddDrawList(g_phoneList);
        if (!g_coverList->CmdBuffer.empty()) ImGui::GetDrawData()->AddDrawList(g_coverList);
    }
    // Into the atlas, cleared to nothing; the game's targets put back after.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(g_target, clear);
    context->OMSetRenderTargets(1, &g_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
    if (phoneShown && g_stillTexture && g_cover.fade <= 0 && !g_cover.blocked) {
        // A ready screen, nothing over it: kept as the still a press fades over from.
        const D3D11_BOX box{static_cast<UINT>(kPhone.x), static_cast<UINT>(kPhone.y), 0,
                            static_cast<UINT>(kPhone.x + kPhone.w), static_cast<UINT>(kPhone.y + kPhone.h), 1};
        context->CopySubresourceRegion(g_stillTexture, 0, 0, 0, 0, g_texture, 0, &box);
        g_cover.still = true;
    }
    if (g_atlasView) context->GenerateMips(g_atlasView);
    for (ID3D11RenderTargetView* t : targets)
        if (t) t->Release();
    if (depth) depth->Release();
    static int logged = 0;
    if (logged++ < 3) LOG("HUD: drawn (watch %d, pause %d)", f.watch ? 1 : 0, f.pause ? 1 : 0);
    return action;
}

ID3D11Texture2D* texture() { return g_imgui ? g_texture : nullptr; }

void release() {
    if (g_imgui) {
        ImGui::SetCurrentContext(g_imgui);
        for (ImDrawList** list : {&g_phoneList, &g_coverList}) {
            if (*list) IM_DELETE(*list);
            *list = nullptr;
        }
        if (g_device) ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_imgui);
        g_imgui = nullptr;
    }
    if (g_art) g_art->Release();
    g_art = nullptr;
    if (g_stillView) g_stillView->Release();
    if (g_stillTexture) g_stillTexture->Release();
    if (g_premultiplied) g_premultiplied->Release();
    g_stillView = nullptr;
    g_stillTexture = nullptr;
    g_premultiplied = nullptr;
    g_drawContext = nullptr;
    g_cover = PhoneCover{};
    if (g_icons) g_icons->Release();
    if (g_iconTexture) g_iconTexture->Release();
    g_icons = nullptr;
    g_iconTexture = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_loadedIcons.clear();
    }
    for (IconSlot& slot : g_slots) slot = IconSlot{};
    if (g_atlasView) g_atlasView->Release();
    g_atlasView = nullptr;
    if (g_target) g_target->Release();
    if (g_texture) g_texture->Release();
    if (g_device) g_device->Release();
    g_target = nullptr;
    g_texture = nullptr;
    g_device = nullptr;
}

}  // namespace s4vr::hud
