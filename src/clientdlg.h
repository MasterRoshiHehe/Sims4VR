#pragma once

// The game's own dialogs that its Flash UI makes by itself (save before exit,
// missing custom content on load, ...) as our popup card in first person.
//
// They never reach the game's scripts: UIContainerMain.as and the client send
// "ShowDialog" inside Flash, DialogMain.Show turns it into a LocalRequestData and
// DialogManager shows it (re/allui/as/olympus/dialog/DialogManager.as). After the
// UI manager's update each frame (UI thread, where the game makes its own AS3 calls)
// this module asks DialogManager.GetCurrentRequest() for the dialog on show, reads
// its title, promptText and actions[i].label, and sends them as popup content
// (hud::setPopupContent, as objmenu.py does for the server's dialogs). A button
// answers as the game's footers do: the dialog closes (DialogManager.CancelRequest
// (id, false)), then actions[i].Execute(data). Server dialogs (ServerRequestData),
// dialogs with text inputs and those whose texts can't be read stay as they are.
namespace s4vr::clientdlg {

// Hooks the game. Call once at start; false in the DX9 game or another game version.
bool install();

// Any thread, once a frame: whether the card is wanted (first person, [ui] popups_3d).
void setWanted(bool wanted);
// Whether the popup content on show is a client dialog of ours.
bool showing();
// Any thread: answer the client dialog with its n-th button (run on the UI thread).
void press(int index);
// Any thread: closes the game's own pie menu widget (on the UI thread), as its own pick does.
// False when the game isn't hooked.
bool closeGamePie();
// The phone's Game Options buttons (the Escape menu's, in its order) and the GameTray's Gallery, sent on the game
// thread at the next poll as those buttons send them.
enum : int { kEscSave, kEscSaveAs, kEscOptions, kEscHelp, kEscLessons, kEscReleaseNotes, kEscCredits, kEscAchievements,
             kEscManageWorlds, kEscMainMenu, kEscQuit, kEscButtons, kGallery = 20 };
bool sendGameTray(int which);
// Any thread: a build mode service of the game's client, CallGameService(name[, arg]) as the game's controller build
// mode sends it, or (uiMessage) SendUIMessage(name[, arg]); json the argument as JSON ("true", "-0.5",
// "{\"value\":1}"; empty: none). Sent on the UI thread at the next poll. False when the game isn't hooked.
bool gameService(const char* name, const char* json = "", bool uiMessage = false);
// Any thread: the build tablet is out (the game's UI scale at its largest, not saved) or away (the player's own back).
void setTabletUiScale(bool on);
// Any thread: a server popup (picker: a picker) answered on our card closes on the game's UI too (UI thread).
bool closeGameDialog(bool picker);

}  // namespace s4vr::clientdlg
