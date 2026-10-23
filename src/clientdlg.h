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
// Any thread: a server popup (picker: a picker) answered on our card closes on the game's UI too (UI thread).
bool closeGameDialog(bool picker);

}  // namespace s4vr::clientdlg
