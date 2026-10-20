// C entry points called from the Python script mod through ctypes.
// Nothing heavy happens in DllMain; the script mod calls s4vr_init after loading.

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "armik.h"
#include "phone3d.h"
#include "touch.h"
#include "camera.h"
#include "camfind.h"
#include "config.h"
#include "crashlog.h"
#include "doorswing.h"
#include "walkstop.h"
#include "framemap.h"
#include "hitch.h"
#include "singleview.h"
#include "gamefix.h"
#include "ambience.h"
#include "mirrorcap.h"
#include "eyelids.h"
#include "facehide.h"
#include "voice.h"
#include "d3d9_hook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "d3d11_hook.h"
#include "gamepad.h"
#include "grab.h"
#include "othermods.h"
#include "hud.h"
#include "thumbs.h"
#include "strings.h"
#include "bubbles.h"
#include "uihook.h"
#include "log.h"
#include "modelbox.h"
#include "nodim.h"
#include "pointer.h"
#include "vr.h"
#include "rtti.h"
#include "player.h"
#include "renderscale.h"
#include "screenshot.h"
#include "settings.h"
#include "shadows.h"
#include "simsforever.h"
#include "claudescreen.h"

#include <MinHook.h>

#define S4VR_API extern "C" __declspec(dllexport)

static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_started{false};
static s4vr::Config g_config;
static std::wstring g_moduleDir;

static std::wstring moduleDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDirectory), &self);
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, length);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

#ifndef S4VR_BUILD_VERSION
#define S4VR_BUILD_VERSION "dev"
#endif
#ifndef S4VR_BUILD_COMMIT
#define S4VR_BUILD_COMMIT "none"
#endif

S4VR_API int s4vr_version() { return 1; }

// Reads sims4vr.ini and the OpenXR loader; starts VR now (autostart = 2) or at the main menu (1).
// Returns 0 on success, negative on failure (see sims4vr.log).
S4VR_API int s4vr_start();

// autostart = 1: VR starts once the first loading screen has gone, so it stays
// on the desktop view. The loading screen (TransitionScreen widget) sends the game
// "TransitionScreenHidden" when its fade-out ends (TransitionScreenMoleculeMain.as,
// OnTransitionAnimationOutComplete); we hook the main menu's native listener for it
// (registered with the MainMenu_* services). MainMenu_GetIsNewGame was too early:
// the menu builds behind the loading screen.
using UiHandlerFn = uintptr_t (*)(void*, void*, void*, void*);
static UiHandlerFn g_originalScreenHidden = nullptr;

static uintptr_t hookedScreenHidden(void* listener, void* args, void* r8, void* r9) {
    uintptr_t result = g_originalScreenHidden(listener, args, r8, r9);
    if (!g_started) {
        LOG("Main menu: the loading screen has gone, starting VR");
        s4vr_start();
    }
    return result;
}

// False if the listener isn't there (another game build, DX9): VR then starts at once.
static bool startAtMainMenu() {
    uint8_t* target = s4vr::gameaddr::dx9() ? nullptr : s4vr::addrs::fn("ScreenHidden");
    if (!target) {
        LOG("Main menu: loading screen listener not recognised; VR starts now");
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        status = MH_CreateHook(target, reinterpret_cast<void*>(&hookedScreenHidden),
                               reinterpret_cast<void**>(&g_originalScreenHidden));
    if (status == MH_OK) status = MH_EnableHook(target);
    LOG("Main menu: VR starts when the loading screen has gone (hook %s)", MH_StatusToString(status));
    return status == MH_OK;
}

S4VR_API int s4vr_init() {
    if (g_initialized.exchange(true)) return 0;
    g_moduleDir = moduleDirectory();
    s4vr::log::init(g_moduleDir + L"\\sims4vr.log");
    LOGT("session", "sims4vr v%s (commit %s) loading from %ls", S4VR_BUILD_VERSION, S4VR_BUILD_COMMIT, g_moduleDir.c_str());
    {
        // PE link time stamp of the running exe (what the address table is keyed on).
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew);
        LOGT("session", "game exe stamp 0x%08x", static_cast<unsigned>(nt->FileHeader.TimeDateStamp));
    }
    s4vr::crashlog::install(g_moduleDir);
    s4vr::screenshot::init(g_moduleDir);
    g_config = s4vr::loadConfig(g_moduleDir + L"\\sims4vr.ini");
    {
        std::wstring ini = g_moduleDir + L"\\sims4vr.ini";
        s4vr::log::setFilter(s4vr::readSetting(ini, "log", "tags", "all"), s4vr::readSetting(ini, "log", "mute", ""));
        LOGT("session", "ini: log tags=%s mute=%s | mode=%d autostart=%d arm_ik=%d play_in_body=%d",
             s4vr::readSetting(ini, "log", "tags", "all").c_str(), s4vr::readSetting(ini, "log", "mute", "").c_str(),
             static_cast<int>(g_config.mode), g_config.autostart, g_config.armIk ? 1 : 0, g_config.playInBody ? 1 : 0);
    }
    s4vr::framemap::init(g_config.frameMap);
    s4vr::gamefix::install(g_config.nearFade);
    s4vr::singleview::install();
    s4vr::singleview::setProbe(g_config.probeDouble);
    s4vr::nodim::installSource(g_config.noDimming);
    s4vr::ambience::install(g_config.outdoorFade);
    s4vr::mirrorcap::install(g_config.mirrorInterior, g_config.mirrorExterior, g_config.reflectionsOff, !g_config.ambientOcclusion);
    s4vr::shadows::install(g_config.steadyShadows, g_config.shadowStep, g_config.shadowSharp, static_cast<int>(g_config.shadowMap), !g_config.ambientOcclusion);
    s4vr::hitch::install(g_config.hitchCatcher);
    LOGT("perf", "GPU pack: exact_fov %d, mirrors %.0f m in rooms / %.0f m outdoors (0 = game's), reflections %s, SSAO %s, shadow map %.0f px, render_scale %.2f, msaa %d, hitch catcher %s",
         g_config.exactFov ? 1 : 0, g_config.mirrorInterior, g_config.mirrorExterior, g_config.reflectionsOff ? "off" : "on",
         !g_config.ambientOcclusion ? "off" : "on", g_config.shadowMap, g_config.renderScale, g_config.renderMsaa, g_config.hitchCatcher ? "on" : "off");
    s4vr::armik::install(g_config.armIk, g_config.handPitch, g_config.headTracking);
    s4vr::armik::setSeatTorso(g_config.bodySeatTorso);
    s4vr::facehide::install(g_config.bodyHideFace);
    s4vr::armik::configureTouch(g_config.touchBodies, g_config.holdHands, g_config.lookAtYou, g_config.grabBodies);
    s4vr::armik::configureGrab(g_config.grabCurl, g_config.grabDrag);
    s4vr::armik::loadGameGrip(g_moduleDir + L"\\sims4vr_grips.ini");
    if (g_config.armIk) {
        s4vr::voice::Settings voice;
        voice.on = g_config.voiceMic;
        voice.device = g_config.voiceDevice;
        voice.gateDb = g_config.voiceGateDb;
        voice.fullDb = g_config.voiceFullDb;
        voice.openDegrees = g_config.voiceMouthOpen;
        s4vr::voice::install(voice);
    }
    if (g_config.touch) s4vr::touch::install();
    s4vr::camera::init(g_config);
    if (g_config.gamepad) s4vr::gamepad::install();
    // The game's menus on their own layer (a panel, your wrist, the pie at your hand).
    if (g_config.uiPanel && s4vr::uihook::install()) s4vr::bubbles::install();
    if (g_config.renderScale > 0) s4vr::renderscale::install(g_config.renderMsaa);
    if (!s4vr::vr::init(g_moduleDir, g_config)) return -1;
    if (g_config.autostart == 1 && startAtMainMenu()) return 0;
    return g_config.autostart ? s4vr_start() : 0;
}

// Hooks Present; from then on every game frame is mirrored to the headset.
S4VR_API int s4vr_start() {
    if (!g_initialized) return s4vr_init();
    if (g_started.exchange(true)) return 0;
    // The DX9 game's frames reach the headset through a texture shared with D3D11.
    bool hooked = s4vr::gameaddr::dx9() ? s4vr::d3d9_hook::install() : s4vr::d3d11_hook::install(&s4vr::vr::onPresent);
    if (!hooked) {
        g_started = false;
        return -2;
    }
    LOG("VR started");
    if (g_config.objectMenus) s4vr::modelbox::start();  // object sizes for menus on objects
    if (g_config.popups3d) s4vr::strings::start();      // the game's texts, for popup cards
    return 0;
}

S4VR_API void s4vr_recenter() { s4vr::vr::requestRecenter(); }

// 0 = quad (virtual screen), 1 = stereo (6DoF, needs the camera hook)
S4VR_API void s4vr_set_mode(int mode) {
    s4vr::vr::setMode(mode == 1 ? s4vr::Mode::Stereo : s4vr::Mode::Quad);
    LOG("Mode set to %s", mode == 1 ? "stereo" : "quad");
}

// Body mode: view from this game object's head (0 = off, back to the camera).
S4VR_API void s4vr_set_body(unsigned long long objectId) {
    static unsigned long long current = 0;
    static int godViewLevel = s4vr::gamefix::kNoLevel;
    // In first person the whole house shows: every floor, whichever one the
    // Sim is on. God view gets back the floor it showed.
    if (objectId && !current) godViewLevel = s4vr::gamefix::shownLevel();
    if (objectId && objectId != current) s4vr::pointer::showAllFloors();
    if (!objectId && current) {
        s4vr::gamefix::setFirstPerson(false);
        s4vr::voice::setFirstPerson(false);
        s4vr::ambience::setFirstPerson(false);
        int now = s4vr::gamefix::shownLevel();
        if (godViewLevel != s4vr::gamefix::kNoLevel && now != s4vr::gamefix::kNoLevel && now > godViewLevel) {
            LOG("God view: back to level %d (first person showed %d)", godViewLevel, now);
            s4vr::pointer::floorsDown(now - godViewLevel);
        }
        godViewLevel = s4vr::gamefix::kNoLevel;
    }
    current = objectId;
    s4vr::camera::setBody(objectId);
}

S4VR_API void s4vr_set_body_root(const float* position, const float* orientation) {
    s4vr::camera::setBodyRoot({position[0], position[1], position[2]},
                              {orientation[0], orientation[1], orientation[2], orientation[3]});
}

// The script mod reports entering (1) and leaving (0) gameplay on a lot.
// Returns 1 if that switched the mode (auto_stereo = 1).
S4VR_API int s4vr_on_gameplay(int playing) {
    if (playing && g_config.autostart && !g_started) {  // the main menu never told us
        LOG("Entered gameplay before VR started: starting it now");
        s4vr_start();
    }
    if (!g_config.autoStereo) return 0;
    s4vr::vr::setMode(playing ? s4vr::Mode::Stereo : s4vr::Mode::Quad);
    LOG("%s gameplay: mode %s", playing ? "Entered" : "Left", playing ? (g_config.playInBody ? "body" : "stereo") : "quad");
    return playing && g_config.playInBody ? 2 : 1;
}

S4VR_API void s4vr_set_scale(float scale) { s4vr::camera::setWorldScale(scale); }

S4VR_API void s4vr_set_player_state(int freeAllowed, unsigned int blockedMask, const float* floor,
                                     const float* flight) {
    s4vr::crashlog::note(s4vr::crashlog::kPython);
    s4vr::player::setState(freeAllowed != 0, blockedMask, floor, flight);
}
S4VR_API int s4vr_walk_pose(float* out) { return s4vr::player::pose(out) ? 1 : 0; }
// 1 while running (a tap of the left stick in first person), 0 walking.
S4VR_API int s4vr_running() { return s4vr::player::running() ? 1 : 0; }

// Count the player into (open = 1) or out of (open = 0) a door, so it swings.
S4VR_API void s4vr_door(unsigned long long objectId, int open) { s4vr::doorswing::request(objectId, open != 0); }

S4VR_API void s4vr_set_held(const unsigned long long* ids, int count) {
    s4vr::camera::setHeldCandidates(reinterpret_cast<const uint64_t*>(ids), count);
}
S4VR_API void s4vr_set_sleeping(int sleeping) { s4vr::eyelids::setSleeping(sleeping != 0); }
S4VR_API void s4vr_set_body_view(int on) { s4vr::camera::setBodyView(on != 0); }
// A game menu or dialog is open (1) or not (0): the phone closes it when put away.
S4VR_API void s4vr_set_game_menu(int open) { s4vr::vr::setGameMenu(open != 0); }
S4VR_API void s4vr_set_popup(int open) { s4vr::vr::setPopup(open != 0); }
S4VR_API void s4vr_set_call(int on) { s4vr::vr::setCall(on != 0); }
S4VR_API void s4vr_set_paused(int paused) { s4vr::vr::setPaused(paused != 0); }
S4VR_API void s4vr_set_clock(int hour, int minute, int day, int speed) { s4vr::vr::setClock(hour, minute, day, speed); }

// The HUD's needs / moodlets / queue / wants (hudinfo.py): `count` packed
// items of 20 bytes (u64 icon, u32 colour, f32 value, u32 flags).
S4VR_API void s4vr_hud_items(int kind, const char* data, int size) {
    s4vr::hud::Item items[s4vr::hud::kMaxItems];
    int count = 0;
    for (; count < s4vr::hud::kMaxItems && (count + 1) * 20 <= size; ++count) {
        const char* p = data + count * 20;
        std::memcpy(&items[count].icon, p, 8);
        std::memcpy(&items[count].colour, p + 8, 4);
        std::memcpy(&items[count].value, p + 12, 4);
        std::memcpy(&items[count].flags, p + 16, 4);
    }
    s4vr::hud::setItems(kind, items, count);
}
S4VR_API void s4vr_hud_icon(unsigned long long instance, const char* data, int size) {
    s4vr::hud::setIcon(instance, reinterpret_cast<const uint8_t*>(data), size > 0 ? static_cast<size_t>(size) : 0);
}
// The tooltips of the needs or moodlets (hud::setTexts).
S4VR_API void s4vr_hud_texts(int kind, const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setTexts(kind, data, size > 0 ? static_cast<size_t>(size) : 0);
}
// The household's money changed (hudinfo.py, the game's SetMoney vfx_amount).
S4VR_API void s4vr_money_changed(long long amount) { s4vr::hud::moneyChanged(amount); }
// The popup on show, from the game's message (hud::setPopupContent); empty: gone.
S4VR_API void s4vr_popup_content(const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setPopupContent(data, size > 0 ? static_cast<size_t>(size) : 0);
}
// The picker on its object, from the game's message (hud::setPickerContent); empty: gone.
S4VR_API void s4vr_picker_content(const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setPickerContent(data, size > 0 ? static_cast<size_t>(size) : 0);
}
// The game's phone menu for the phone in your hand (hud::setPhoneContent); empty: gone.
S4VR_API void s4vr_phone_content(const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setPhoneContent(data, size > 0 ? static_cast<size_t>(size) : 0);
}
S4VR_API void s4vr_phone_notes(const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setPhoneNotes(data, size > 0 ? static_cast<size_t>(size) : 0);
}
S4VR_API void s4vr_phone_calendar(const char* data, int size) {
    s4vr::strings::start();
    s4vr::hud::setPhoneCalendar(data, size > 0 ? static_cast<size_t>(size) : 0);
}
S4VR_API void s4vr_note_over(unsigned long long id, float x, float y, float z, int front) {
    const float at[3] = {x, y, z};
    s4vr::vr::setNoteOver(id, at, front != 0);
}
// A picture from the game's thumbnail system ("img://thumbs/<path>", as the
// game's UI builds them) as HUD picture `id` (thumbs.h); 0: not asked.
S4VR_API int s4vr_thumb(unsigned long long id, const char* path) { return s4vr::thumbs::request(id, path) ? 1 : 0; }
// The icons still wanted: up to `max` instances into `out`; how many.
S4VR_API int s4vr_hud_missing_icons(unsigned long long* out, int max) {
    uint64_t ids[16];
    int n = s4vr::hud::missingIcons(ids, max < 16 ? max : 16);
    for (int i = 0; i < n; ++i) out[i] = ids[i];
    return n;
}
// The lot is in build/buy mode (buildbuy.py): the phone gesture brings out the tablet in first person.
S4VR_API void s4vr_set_build_buy(int on) { s4vr::vr::setBuildBuy(on != 0); }
// The open picker shows on an object's front face (kind 1 front, 2 screen guessed in the front, 3 the exact screen; 0 off; -1 walked away: Escape cancels the picker):
// centre xyz, normal xyz, up xyz, width, height (world units).
S4VR_API void s4vr_set_object_menu(int kind, const float* v) { s4vr::vr::setObjectMenu(kind, v); }
// Sims Forever: Sims 1 on a computer's screen (simsforever.h); on = 0 takes it off.
S4VR_API void s4vr_sims_forever(int on, const float* v, const char* exe, const char* args) {
    s4vr::simsforever::setScreen(on, v, exe, args);
}
S4VR_API void s4vr_claude_screen(int on, const float* v, const char* exe) { s4vr::claudescreen::set(on, v, exe); }
S4VR_API void s4vr_sims_forever_surface(int nx, int ny, const float* depths, int count) {
    s4vr::simsforever::setSurface(nx, ny, depths, count);
}
// An object model's stored bytes from the game's packages (modelbox::read's status; 0 = found).
// *data stays valid until the next call (game thread only).
S4VR_API int s4vr_model_bytes(unsigned long long instance, int* compression, unsigned* size, const char** data,
                              int* length) {
    static std::vector<uint8_t> bytes;
    s4vr::modelbox::start();
    uint16_t c = 0;
    uint32_t n = 0;
    int status = s4vr::modelbox::read(instance, c, n, bytes);
    *compression = c;
    *size = n;
    *data = reinterpret_cast<const char*>(bytes.data());
    *length = status == 0 ? static_cast<int>(bytes.size()) : 0;
    return status;
}
// Any game resource by type and instance, as stored (modelbox::readResource; status as above).
S4VR_API int s4vr_resource_bytes(unsigned type, unsigned long long instance, int* compression, unsigned* size,
                                 const char** data, int* length) {
    static std::vector<uint8_t> bytes;
    uint16_t c = 0;
    uint32_t n = 0;
    int status = s4vr::modelbox::readResource(type, instance, c, n, bytes);
    *compression = c;
    *size = n;
    *data = reinterpret_cast<const char*>(bytes.data());
    *length = status == 0 ? static_cast<int>(bytes.size()) : 0;
    return status;
}
// The played Sim's phone background (an image resource, unpacked; buildbuy.py) for the tablet.
S4VR_API void s4vr_set_tablet_wallpaper(unsigned type, const char* data, int length) {
    LOG("Tablet: phone background picture arrived (type %08x)", type);
    if (data && length > 0) s4vr::phone3d::setWallpaper(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(length));
}
S4VR_API int s4vr_phone_mount(float* out) { return s4vr::vr::phoneMount(out) ? 1 : 0; }
S4VR_API void s4vr_set_phone_prop(float w, float h, float d) { s4vr::vr::setPhoneProp(w, h, d); }
// The active Sim on the lot, whose mouth your mic moves (0: none).
S4VR_API void s4vr_set_voice_sim(unsigned long long objectId) { s4vr::voice::setSim(objectId); }
// The game built a pie menu with this many options for a world click; 0: you picked.
S4VR_API void s4vr_pie_menu(int items) {
    if (items > 0)
        s4vr::vr::pieOpened(items);
    else
        s4vr::vr::pieClosed();
}
// The hand of the last click into the world in first person (0 left, 1 right), -1 none lately.
S4VR_API int s4vr_click_hand() { return s4vr::vr::clickHand(); }
// 1 while the pie menu shows at your hand.
S4VR_API int s4vr_pie_shown() { return s4vr::vr::pieShown() ? 1 : 0; }
// Picking things up (grab.cpp): the objects within reach, ids and centre + reach (4 floats each).
S4VR_API void s4vr_set_grabbables(const unsigned long long* ids, const float* xyzr, int count) {
    s4vr::grab::setGrabbables(reinterpret_cast<const uint64_t*>(ids), xyzr, count);
}
// The oldest hand event: kind 1 grabbed, 2 let go. Returns 0 if none.
S4VR_API int s4vr_hand_event(int* kind, int* hand, unsigned long long* id) {
    s4vr::grab::Event e;
    if (!s4vr::grab::takeEvent(e)) return 0;
    *kind = e.kind;
    *hand = e.hand;
    *id = e.id;
    return 1;
}
S4VR_API void s4vr_grab_set(int hand, unsigned long long id) { s4vr::grab::setHeld(hand, id); }
// The oldest touch event on another Sim's body (armik.cpp): kind 1 took hold,
// 2 let go (value seconds), 3 pulled hard (value metres); actor = the Sim's
// client object (compare s4vr_object_ptr). Returns 0 if none.
// Sims whose body may only be held at the hand, arms, shoulders, back and hair (armik.cpp).
S4VR_API void s4vr_set_touch_restricted(const unsigned long long* ids, int count) {
    void* actors[16] = {};
    int n = 0;
    for (int i = 0; i < count && n < 16; ++i)
        if (void* a = s4vr::camera::objectPointer(ids[i])) actors[n++] = a;
    s4vr::armik::setTouchRestricted(actors, n);
}
S4VR_API int s4vr_touch_event(int* kind, int* hand, int* region, unsigned long long* actor, float* value) {
    s4vr::armik::TouchEvent e;
    if (!s4vr::armik::takeTouchEvent(e)) return 0;
    *kind = e.kind;
    *hand = e.hand;
    *region = e.region;
    *actor = reinterpret_cast<unsigned long long>(e.actor);
    *value = e.value;
    return 1;
}
// Head and hands in the world: out[0..8] head position, forward, up; then per
// hand (10 floats): valid, palm xyz, orientation xyzw, grip, trigger. held[2]:
// what each hand holds. Returns 0 outside first person.
S4VR_API int s4vr_hands(float* out, unsigned long long* held) {
    s4vr::grab::Snapshot s;
    if (!s4vr::grab::snapshot(s)) return 0;
    std::memcpy(out, s.hmd, 3 * sizeof(float));
    std::memcpy(out + 3, s.forward, 3 * sizeof(float));
    std::memcpy(out + 6, s.up, 3 * sizeof(float));
    if (!s.head) out[0] = out[1] = out[2] = NAN;
    for (int i = 0; i < 2; ++i) {
        float* h = out + 9 + i * 10;
        h[0] = s.hand[i].valid ? 1.0f : 0.0f;
        std::memcpy(h + 1, s.hand[i].palm, 3 * sizeof(float));
        std::memcpy(h + 4, s.hand[i].q, 4 * sizeof(float));
        h[8] = s.hand[i].grip;
        h[9] = s.hand[i].trigger;
        held[i] = s.hand[i].held;
    }
    return 1;
}
// The debug menu's testing cheats switch, as the script mod sees it (cheats on and
// every pie menu the Shift+click cheat menu).
S4VR_API void s4vr_set_testing_cheats(int state) { s4vr::settings::setTestingCheats(state); }
// Other script mods the script mod found loaded (othermods.h: 1 << kPerformance), for the debug menu's pages.
S4VR_API void s4vr_set_script_mods(int bits) { s4vr::othermods::setScriptMods(bits); }
// Controller shortcuts: 1 switch first person / game camera, 2 next Sim; 0 if none.
S4VR_API int s4vr_control_event() {
    int what = 0;
    return s4vr::grab::takeControl(what) ? what : 0;
}
S4VR_API void s4vr_haptic(int hand, float amplitude, float seconds) { s4vr::grab::haptic(hand, amplitude, seconds); }
// Any sims4vr.ini value as text (the script mod's own switches live there too).
S4VR_API int s4vr_setting(const char* section, const char* key, char* buffer, int size) {
    if (!buffer || size <= 0) return 0;
    std::wstring ini = (g_moduleDir.empty() ? moduleDirectory() : g_moduleDir) + L"\\sims4vr.ini";
    std::string value;  // the settings menu's value first, then sims4vr_user.ini's, then sims4vr.ini's
    if (!s4vr::settings::liveValue(section, key, value)) value = s4vr::readSetting(ini, section, key, "");
    int n = static_cast<int>(std::min<size_t>(value.size(), static_cast<size_t>(size - 1)));
    std::memcpy(buffer, value.data(), n);
    buffer[n] = '\0';
    return n;
}

// End the player's client walk route (stride.py) if it is still running.
S4VR_API void s4vr_walk_stop(unsigned long long objectId, unsigned long long routeId) {
    s4vr::walkstop::request(objectId, routeId);
}
// The player's client walk controller (state, path ahead, stop and start clip metres); 0 = not read yet.
S4VR_API int s4vr_walk_info(unsigned long long objectId, float* out) {
    return s4vr::walkstop::info(objectId, out) ? 1 : 0;
}
// A line from Python in sims4vr.log (with its time).
S4VR_API void s4vr_log_line(const char* text) {
    if (text) LOG("Python: %s", text);
}
// Where slowing down ends (x, z, metres to go); 0 = not slowing.
S4VR_API int s4vr_walk_rest(float* out) { return s4vr::player::rest(out) ? 1 : 0; }
// The route the client has active on that object (0: none), as of the last frame.
S4VR_API unsigned long long s4vr_client_route(unsigned long long objectId) {
    return s4vr::walkstop::clientRoute(objectId);
}
S4VR_API int s4vr_walk_input(float* out) { return s4vr::vr::walkInput(out[0], out[1], out[2]) ? 1 : 0; }

// Height of the floor the game camera looks at, for the laser pointer.
S4VR_API void s4vr_set_floor(float worldY) { s4vr::camera::setFloor(worldY); }

// Writes a NUL-terminated status line into buffer; returns its full length.
S4VR_API int s4vr_status(char* buffer, int size) {
    std::string s = std::string(g_started ? "started " : "not started ") + s4vr::vr::status();
    if (buffer && size > 0) {
        size_t n = s.size() < static_cast<size_t>(size - 1) ? s.size() : static_cast<size_t>(size - 1);
        std::memcpy(buffer, s.data(), n);
        buffer[n] = '\0';
    }
    return static_cast<int>(s.size());
}

// Searches game memory for the camera at the position/target the game reported
// to Python; writes a summary into buffer. Details go to sims4vr.log.
S4VR_API int s4vr_find_camera(const float* position, const float* target, char* buffer, int size) {
    if (!g_initialized) s4vr_init();
    std::string s = s4vr::camfind::run(position, target);
    if (buffer && size > 0) {
        size_t n = s.size() < static_cast<size_t>(size - 1) ? s.size() : static_cast<size_t>(size - 1);
        std::memcpy(buffer, s.data(), n);
        buffer[n] = '\0';
    }
    return static_cast<int>(s.size());
}

// Must run before process exit; the script mod registers it with atexit.
S4VR_API void s4vr_shutdown() {
    if (!g_initialized) return;
    s4vr::vr::shutdown();
    s4vr::voice::shutdown();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

// Developer probe helpers (Python: native.mem_read etc.). Reads never fault.
S4VR_API int s4vr_dev_probe() { return g_config.devProbe ? 1 : 0; }

S4VR_API unsigned long long s4vr_game_base() {
    return reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
}

S4VR_API int s4vr_mem_read(unsigned long long address, void* out, int size) {
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &read)) return 0;
    return static_cast<int>(read);
}

S4VR_API int s4vr_mem_write(unsigned long long address, const void* data, int size) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &written)) return 0;
    return static_cast<int>(written);
}

S4VR_API int s4vr_rtti_name(unsigned long long object, char* buffer, int size) {
    return s4vr::rtti::className(static_cast<uintptr_t>(object), buffer, size) ? 1 : 0;
}

// The game's own grip on the TV remote: learn it while its remote animation
// runs on your Sim (1), stop (0).
S4VR_API void s4vr_grip_learn(int on) { s4vr::armik::learnGameGrip(on != 0); }
// That hand (0 left) holds our remote: its fingers take the learned grip.
S4VR_API void s4vr_grip_hold(int hand, int on) { s4vr::armik::holdGameGrip(hand, on != 0); }
// Where the remote goes on that hand's joint: out = position xyz, orientation
// xyzw in the joint's frame; 0 until learned. Saves a newly learned grip.
S4VR_API int s4vr_grip(int hand, float* out) {
    s4vr::armik::saveGameGrip();
    s4vr::camera::Vec3 p;
    s4vr::camera::Quat q;
    if (!s4vr::armik::gameGrip(hand, p, q)) return 0;
    float v[7] = {p.x, p.y, p.z, q.x, q.y, q.z, q.w};
    std::memcpy(out, v, sizeof(v));
    return 1;
}

// First person: how much of the outdoors reaches you inside through doors and
// arches (0..1, negative: not known) and through plain walls (ambience.h).
S4VR_API void s4vr_outdoors(float through, float wallLeak) { s4vr::ambience::setOpenings(through, wallLeak); }

// The first-person Sim's hand joint: out = position xyz, orientation xyzw.
S4VR_API int s4vr_hand_joint(int hand, unsigned long long* sim, float* out) {
    uint64_t id = 0;
    if (!s4vr::camera::handJoint(hand, id, out, out + 3)) return 0;
    *sim = id;
    return 1;
}

S4VR_API unsigned long long s4vr_object_ptr(unsigned long long id) {
    return reinterpret_cast<unsigned long long>(s4vr::camera::objectPointer(id));
}

S4VR_API int s4vr_joint(unsigned long long id, const char* joint, float* out) {
    return s4vr::camera::jointTransform(id, joint, out) ? 1 : 0;
}
