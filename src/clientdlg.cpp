#include "clientdlg.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "hud.h"
#include "log.h"
#include "vr.h"

namespace s4vr::clientdlg {

namespace {

// Scaleform 4 GFx::Value as the game lays it out (0x20 bytes): its object interface (the
// AS3MovieRoot), the type (low bits & 0x8f; 0x40 = managed, released with GfxRelease), the
// data and a second word (a closure's bound object).
struct Value {
    void* oi = nullptr;
    uint32_t type = 0;
    uint32_t pad = 0;
    void* data = nullptr;
    void* data2 = nullptr;
};
static_assert(sizeof(Value) == 0x20, "GFx::Value");
enum : uint32_t { kUndefined = 0, kNull = 1, kBool = 2, kInt = 3, kUInt = 4, kNumber = 5, kString = 6, kObject = 8,
                  kArray = 9, kDisplay = 10, kClosure = 11 };

using UpdateFn = void (*)(void* manager, float dt);
using GetMemberFn = bool (*)(void* root, void* data, const char* name, Value* out, bool isDisplay);
using InvokeFn = bool (*)(void* root, void* data, void* data2, Value* result, const Value* args, size_t n);
using ReleaseFn = void (*)(void* oi, Value* v, void* data);
using MovieInvokeFn = bool (*)(void* movie, const char* path, Value* result, const Value* args, size_t n);

UpdateFn g_update = nullptr;
GetMemberFn g_getMember = nullptr;
InvokeFn g_invoke = nullptr;
ReleaseFn g_release = nullptr;
MovieInvokeFn g_movieInvoke = nullptr;
uint8_t* g_manager = nullptr;

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_showing{false};
std::atomic<int> g_press{-1};
std::atomic<bool> g_hidePie{false};
std::atomic<int> g_gameTray{-1};    // sendGameTray
std::atomic<int> g_closeGameUi{0};  // 1: a server popup answered on our card, 2: a picker
std::string g_shownId;   // UI thread only
std::string g_skippedId;

uint32_t kind(const Value& v) { return v.type & 0x8f; }

void release(Value& v) {
    if ((v.type >> 6) & 1) g_release(v.oi, &v, v.data);
    v = Value{};
}

// A holder that frees its value.
struct Held {
    Value v;
    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    ~Held() { release(v); }
    bool ok() const { return v.oi && kind(v) != kUndefined && kind(v) != kNull; }
};

bool member(const Value& of, const char* name, Held& out) {
    release(out.v);
    if (!of.oi) return false;
    bool got = g_getMember(of.oi, of.data, name, &out.v, kind(of) == kDisplay);
    return got && out.ok();
}

bool call(const Value& closure, const Value* args, size_t n, Held* result) {
    if (!closure.oi || kind(closure) != kClosure) return false;
    Value scratch;
    Value* r = result ? &result->v : &scratch;
    if (result) release(result->v);
    bool done = g_invoke(closure.oi, closure.data, closure.data2, r, args, n);
    if (!result) release(scratch);
    return done;
}

bool callMember(const Value& of, const char* name, const Value* args, size_t n, Held* result) {
    Held fn;
    if (!member(of, name, fn)) return false;
    return call(fn.v, args, n, result);
}

std::string text(const Value& v) {
    if (kind(v) != kString || !v.data) return {};
    const char* s = (v.type & 0x40) ? *static_cast<const char* const*>(v.data) : static_cast<const char*>(v.data);
    return s ? s : "";
}

std::string memberText(const Value& of, const char* name) {
    Held h;
    return member(of, name, h) ? text(h.v) : std::string();
}

double number(const Value& v) {
    switch (kind(v)) {
        case kNumber: { double d; std::memcpy(&d, &v.data, sizeof d); return d; }
        case kInt: return static_cast<double>(static_cast<int32_t>(reinterpret_cast<intptr_t>(v.data)));
        case kUInt: return static_cast<double>(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v.data)));
        default: return 0;
    }
}

// JSON for strings.cpp: {"raw": "..."} (plain text as is).
std::string raw(const std::string& s) {
    std::string out = "{\"raw\": \"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    return out + "\"}";
}

void* movie() {
    if (!g_manager) return nullptr;
    auto* manager = *reinterpret_cast<uint8_t**>(g_manager);
    if (!manager) return nullptr;
    return *reinterpret_cast<void**>(manager + 0xe0);  // UIContainer.gfx
}

// An AS3 class by its full name (its static functions are members of it).
bool definition(const char* className, Held& out) {
    void* m = movie();
    if (!m || !*reinterpret_cast<void**>(static_cast<uint8_t*>(m) + 0x18)) return false;
    Value name;
    name.type = kString;
    name.data = const_cast<char*>(className);
    release(out.v);
    return g_movieInvoke(m, "flash.utils.getDefinitionByName", &out.v, &name, 1) && out.ok();
}

bool dialogManager(Held& out) { return definition("olympus.dialog.DialogManager", out); }

// The game's pie menu widget closes as after a pick of its own (PieMenuMain.OnOptionSelected):
// CommunicationManager.SendMessage("HidePieMenu", {isOptionSelected: true}) is what it sends; null does the same close.
void hideGamePie() {
    Held comms;
    if (!definition("olympus.io.CommunicationManager", comms)) {
        LOG("Pie: the game's pie menu not closed (CommunicationManager not found)");
        return;
    }
    Value args[2];
    args[0].type = kString;
    args[0].data = const_cast<char*>("HidePieMenu");
    args[1].type = kNull;
    bool sent = callMember(comms.v, "SendMessage", args, 2, nullptr);
    static int logged = 0;
    if (logged++ < 20) LOG("Pie: the game's pie menu %s", sent ? "closed (HidePieMenu)" : "not closed (SendMessage failed)");
}

// A server popup or picker answered on our card closes on the game's UI too, as its debug "close all"
// (UIContainerMain.DebugDialogCloseAll): DialogManager.CancelCurrentRequest(false) while the current request is
// the server's, and SendUIMessage("HidePickers") for a picker. The server's own UiDialogCloseRequest didn't close
// them (user 2026-10-23 19:22Z).
// The phone's Game Options buttons (Sims4EscapeMenu's, user 2026-10-24 01:26Z) and the GameTray's Gallery, sending
// what those buttons send (payload objects made by AS3's JSON.parse; Help / Release Notes ask the game for the URL).
bool uiMessage(const Held& comms, const char* message, const std::string& json) {
    Held parser, payload;
    bool parsed = false;
    if (!json.empty() && definition("JSON", parser)) {
        Value arg;
        arg.type = kString;
        arg.data = const_cast<char*>(json.c_str());
        parsed = callMember(parser.v, "parse", &arg, 1, &payload) && payload.ok();
    }
    Value args[2];
    args[0].type = kString;
    args[0].data = const_cast<char*>(message);
    if (parsed) args[1] = payload.v;
    else args[1].type = kNull;
    bool sent = callMember(comms.v, "SendUIMessage", args, 2, nullptr);
    LOG("Phone: %s %s (payload %s)", message, sent ? "sent" : "not sent (SendUIMessage failed)",
        parsed ? json.c_str() : json.empty() ? "none" : "null, JSON.parse failed");
    return sent;
}

void gameTray(int which) {
    Held comms;
    if (!definition("olympus.io.CommunicationManager", comms)) {
        LOG("Phone: game menu button %d not sent (CommunicationManager not found)", which);
        return;
    }
    switch (which) {
        case kEscSave:
            uiMessage(comms, "SaveCurrentGame", "");
            uiMessage(comms, "HideEscapeMenu", "");
            break;
        case kEscSaveAs: uiMessage(comms, "ShowSaveLoadMenu", "{\"isSaving\":true}"); break;
        case kEscOptions: uiMessage(comms, "ShowOptionsMenu", ""); break;
        case kEscHelp:
        case kEscReleaseNotes: {
            Value name;
            name.type = kString;
            name.data = const_cast<char*>(which == kEscHelp ? "GetHelpURL" : "GetPatchNotesURL");
            Held url;
            std::string u;
            if (callMember(comms.v, "CallGameService", &name, 1, &url)) u = text(url.v);
            std::string json = "{\"url\":\"";
            for (char ch : u) {
                if (ch == '"' || ch == '\\') json += '\\';
                json += ch;
            }
            json += "\"}";
            if (u.empty()) LOG("Phone: no URL from the game for %s", static_cast<const char*>(name.data));
            else uiMessage(comms, "OpenExternalURL", json);
            break;
        }
        case kEscLessons: uiMessage(comms, "ShowTutorial", "{\"source\":\"esc_menu\"}"); break;
        case kEscCredits: uiMessage(comms, "ShowGameCredits", ""); break;
        case kEscAchievements: uiMessage(comms, "ShowAchievements", ""); break;
        case kEscManageWorlds: uiMessage(comms, "ShowExitToManageWorldsDialog", "{\"context\":1}"); break;
        case kEscMainMenu: uiMessage(comms, "ShowExitToMainMenuDialog", ""); break;
        case kEscQuit: uiMessage(comms, "ShowCloseGameDialog", ""); break;
        case kGallery: uiMessage(comms, "ShowExchangeShare", "{\"mode\":\"share\"}"); break;
        default: break;
    }
}

void closeGameUi(int what) {
    Held comms;
    if (what == 2) {
        bool sent = false;
        if (definition("olympus.io.CommunicationManager", comms)) {
            Value args[2];
            args[0].type = kString;
            args[0].data = const_cast<char*>("HidePickers");
            args[1].type = kNull;
            sent = callMember(comms.v, "SendUIMessage", args, 2, nullptr);
        }
        LOG("Popup: the game's picker %s", sent ? "closed on its UI (HidePickers)" : "not closed (SendUIMessage failed)");
        return;
    }
    Held manager, request, data, server;
    if (!dialogManager(manager) || !callMember(manager.v, "GetCurrentRequest", nullptr, 0, &request) ||
        !request.ok() || !member(request.v, "data", data)) {
        LOG("Popup: none left open on the game's UI");
        return;
    }
    if (!member(data.v, "dialogMessage", server)) {
        LOG("Popup: the game's UI shows its own dialog now, left open");
        return;
    }
    Value no;
    no.type = kBool;
    no.data = nullptr;
    bool closed = callMember(manager.v, "CancelCurrentRequest", &no, 1, nullptr);
    LOG("Popup: still open on the game's UI, %s", closed ? "closed (CancelCurrentRequest)" : "not closed (call failed)");
}

void hide() {
    if (!g_showing.exchange(false)) return;
    hud::setPopupContent(nullptr, 0);
    const float zero[11] = {};
    vr::setObjectMenu(0, zero);
}

void answer(const Value& manager, const Value& data, int index) {
    Held actions, action;
    if (!member(data, "actions", actions)) return;
    if (!member(actions.v, std::to_string(index).c_str(), action)) {
        LOG("Client dialog: button %d not found", index);
        return;
    }
    // As the footers do (DialogButtonFooter.OnButtonSelect): the dialog closes, then its action runs.
    Value args[2];
    args[0].type = kString;
    args[0].data = const_cast<char*>(g_shownId.c_str());
    args[1].type = kBool;
    args[1].data = nullptr;  // false: no cancellation action
    bool closed = callMember(manager, "CancelRequest", args, 2, nullptr);
    bool ran = callMember(action.v, "Execute", &data, 1, nullptr);
    LOG("Client dialog '%s': button %d (closed %d, action %d)", g_shownId.c_str(), index, closed, ran);
}

void poll() {
    if (g_hidePie.exchange(false)) hideGamePie();
    if (int which = g_gameTray.exchange(-1); which >= 0) gameTray(which);
    if (int what = g_closeGameUi.exchange(0)) closeGameUi(what);
    int pressed = g_press.exchange(-1);
    static int tick = 0;
    if (pressed < 0 && (++tick % 4)) return;
    if (!g_wanted.load(std::memory_order_relaxed)) {
        hide();
        return;
    }
    Held manager, request, data;
    if (!dialogManager(manager) || !callMember(manager.v, "GetCurrentRequest", nullptr, 0, &request) ||
        !request.ok() || !member(request.v, "data", data)) {
        hide();
        return;
    }
    std::string id = memberText(data.v, "dialog_id");
    if (g_showing && id == g_shownId) {
        if (pressed >= 0) answer(manager.v, data.v, pressed);
        return;
    }
    if (id == g_skippedId) return;
    // Not ours: the server's dialogs (objmenu.py draws those), text inputs (still the game's).
    Held server, inputs, actions, length;
    const char* why = nullptr;
    if (member(data.v, "dialogMessage", server))
        why = "the server's dialog";
    else if (member(data.v, "inputs", inputs) && member(inputs.v, "length", length) && number(length.v) > 0)
        why = "it has text inputs";
    std::string title = memberText(data.v, "title"), prompt = memberText(data.v, "promptText");
    std::vector<std::string> labels;
    int count = -1;  // the actions' length as read; -1 unknown
    if (!why && member(data.v, "actions", actions)) {
        if (member(actions.v, "length", length)) count = static_cast<int>(number(length.v));
        // Each element by its index as a name (Vector); probed when the length can't be read.
        for (int i = 0; i < (count >= 0 ? count : 6) && i < 6; ++i) {
            Held action;
            if (!member(actions.v, std::to_string(i).c_str(), action)) {
                if (count < 0) break;
                labels.emplace_back();
            } else {
                labels.push_back(memberText(action.v, "label"));
            }
            if (labels.back().empty()) why = "a button without its text";
        }
        static int logged = 0;
        if (logged++ < 20)
            LOG("Client dialog '%s': actions type %u, length %d, %zu buttons read", id.c_str(), kind(actions.v), count,
                labels.size());
    }
    if (!why && title.empty() && prompt.empty()) why = "no texts";
    if (!why && labels.empty() && count != 0) why = "no buttons";  // none at all (Saving...): shown as it is
    if (why) {
        // The server's dialog has put its own content up already (objmenu.py): only ours is let go.
        if (server.ok())
            g_showing = false;
        else
            hide();
        g_skippedId = id;
        LOG("Client dialog '%s' left to the game: %s", id.c_str(), why);
        return;
    }
    hide();
    std::string records = "0\x1f" + id + "\x1f" "0\x1f" "0";
    if (!title.empty()) records += "\x1etitle\x1f" + raw(title);
    if (!prompt.empty()) records += "\x1etext\x1f" + raw(prompt);
    for (size_t i = 0; i < labels.size(); ++i) records += "\x1e" "button\x1f" + raw(labels[i]) + "\x1f" + std::to_string(i);
    g_shownId = id;
    g_skippedId.clear();
    hud::setPopupContent(records.data(), records.size());
    const float zero[11] = {};
    vr::setObjectMenu(4, zero);
    g_showing = true;
    LOG("Client dialog '%s' drawn: title %zu chars, text %zu chars, %zu buttons", id.c_str(), title.size(),
        prompt.size(), labels.size());
}

void hookedUpdate(void* manager, float dt) {
    g_update(manager, dt);
    poll();
}

}  // namespace

bool install() {
    if (gameaddr::dx9()) return false;
    void* update = addrs::fn("UiManagerUpdate");
    g_getMember = reinterpret_cast<GetMemberFn>(addrs::fn("GfxGetMember"));
    g_invoke = reinterpret_cast<InvokeFn>(addrs::fn("GfxInvoke"));
    g_release = reinterpret_cast<ReleaseFn>(addrs::fn("GfxRelease"));
    g_movieInvoke = reinterpret_cast<MovieInvokeFn>(addrs::fn("MovieInvoke"));
    g_manager = addrs::global("UiManager");
    if (!update || !g_getMember || !g_invoke || !g_release || !g_movieInvoke || !g_manager) {
        LOG("Client dialogs: game build not recognised; the game's own dialogs stay as they are");
        return false;
    }
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    bool ok = MH_CreateHook(update, reinterpret_cast<void*>(&hookedUpdate), reinterpret_cast<void**>(&g_update)) == MH_OK &&
              MH_EnableHook(update) == MH_OK;
    LOG("Client dialogs: game hook %s", ok ? "installed" : "FAILED");
    return ok;
}

void setWanted(bool wanted) {
    if (g_wanted.exchange(wanted) != wanted) LOG("Client dialogs: %s", wanted ? "drawn as our card" : "left to the game");
}

bool showing() { return g_showing.load(); }

void press(int index) { g_press = index; }

bool closeGameDialog(bool picker) {
    if (!g_update) return false;
    g_closeGameUi = picker ? 2 : 1;
    return true;
}

bool sendGameTray(int which) {
    if (!g_update) return false;
    g_gameTray = which;
    return true;
}

bool closeGamePie() {
    if (!g_update) return false;
    g_hidePie = true;
    return true;
}

}  // namespace s4vr::clientdlg
