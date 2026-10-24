#pragma once
#include <vector>

#include <d3d11.h>
#include <cstddef>
#include <cstdint>

// Our own HUD (first person; user 2026-10-15 05:34Z: crops of the game's UI
// looked wrong, draw it with a third-party UI library instead; 06:33Z/14:07Z:
// built for VR but looking like vanilla). Dear ImGui lays each element out in
// one texture (the atlas) from the game's values (Python sends them), drawn
// with the game's own UI art (art/hud_art.bin, tools/pack_hud_art.py) and the
// game's own icons (read from its packages); vr.cpp shows the parts of it on
// your arms and around you (phone3d pieces, or a headset layer over the closed
// eyelids). Pointer input comes back in atlas pixels; buttons return actions.
namespace s4vr::hud {

constexpr int kWidth = 1024, kHeight = 7504;  // the atlas
struct Rect {
    float x, y, w, h;
};
constexpr Rect kWatch{0, 0, 640, 256};        // clock, day, speed buttons, full-UI button
constexpr Rect kPauseArea{640, 0, 128, 128};  // the pause symbol
constexpr Rect kBuild{768, 0, 128, 128};      // the round build/buy button
constexpr Rect kPreview{896, 0, 128, 128};    // see-through green: where a HUD piece goes while its menu page is open
constexpr Rect kNeeds{0, 256, 512, 256};      // six need bars with their icons, 2 x 3
constexpr Rect kMoodlets{512, 256, 512, 256};  // up to 8 moodlets, 4 x 2
constexpr Rect kQueue{0, 512, 1024, 160};     // the action queue: running first, then queued
constexpr Rect kWants{0, 672, 768, 192};      // up to 3 wants and a fear
constexpr Rect kMoney{0, 864, 1024, 160};     // the money gained / lost, rising (the game's flyaway)
constexpr Rect kPopup{0, 1024, 1024, 1024};   // a game popup as a card (as tall as its content)
constexpr Rect kTip{0, 2048, 1024, 512};      // the tooltip of the need or moodlet pointed at
constexpr Rect kPicker{0, 2560, 1024, 1024};  // a game picker on its object (the fridge's recipes) as a card
// The game's phone menu on our phone's screen: the game's PhonePanel screen
// (230 x 400 Flash pixels) at 2.5 atlas pixels each.
constexpr Rect kPhone{0, 3584, 575, 1000};
constexpr Rect kBalloons{0, 4608, 1024, 528};  // balloon slots over Sims' heads, 4 x 2 of kBalloonW x kBalloonH
constexpr int kBalloonW = 256, kBalloonH = 264, kBalloonSlots = 8;
constexpr Rect kBars{0, 5136, 1024, 224};  // skill bars over Sims' heads, 4 slots of kBarSlot x kBarSlot
constexpr int kBarSlot = 224, kBarSlots = 4;
constexpr Rect kNote{576, 3584, 448, 360};  // a notification card over a Sim's head (notify.py)
// The pie menu drawn by us (first person), centred in it; pieScale() atlas pixels per game stage pixel.
constexpr Rect kPie{0, 5360, 1024, 640};
// The notebook in your hand (notebook pages, hud.cpp): the left and right page of the open spread, and the leather
// cover. Each is the game's page 422 x 568 at 512 x 689 atlas pixels.
constexpr Rect kBookLeft{0, 6000, 512, 688};
constexpr Rect kBookRight{512, 6000, 512, 688};
constexpr Rect kBookCover{0, 6688, 512, 688};
constexpr Rect kBookLeatherAll{0, 6688, 512, 688};     // the cover, as the front board's outside
constexpr Rect kBookLeatherPlain{150, 7108, 100, 100};  // plain leather of it: the boards' edges and back
constexpr Rect kBookEdge{0, 7376, 128, 128};           // the pages' edges
constexpr Rect kBookPaper{256, 7376, 128, 128};        // the boards' endpapers

// What the buttons ask for. kCancel: the queue item `index` (render's out);
// kDismissWant: the want `index` (the game's X on a want: a new one comes).
// kAnswer: the popup's button `index`. kPick: the picker's row, `index` =
// row * 4 + 1 (use ingredients) + 2 (use prepped ingredients); kPickerClose:
// its close button (the game's Cancel). kPhonePick: the phone menu's entry
// `index` (phoneapp.py's node); kPhoneSilence: the phone's silence button;
// kPhoneOptions: the Options app (the game menu); kPhoneColor: wallpaper colour `index`.
enum Action { kNone = 0, kPausePressed = 1, kSpeed1, kSpeed2, kSpeed3, kFullUi, kBuildPressed, kCancel, kDismissWant, kAnswer,
              kPick, kPickerClose, kPhonePick, kPhoneSilence, kPhoneOptions, kPhoneColor, kNotesOpened, kNotesClear, kNoteDelete, kNoteDismiss, kCalendarOpened, kMySimOpened, kMySimFresh, kAspirationPicked, kTonePreview, kPlayAs, kGallery, kManageWorlds, kContactsOpened, kContactPicked, kContactAction, kNoteAnswer,
              kPiePick, kPieClose, kListPick, kListDone, kListClose, kVrOptions };
// kVrOptions: the phone's Settings > VR Options (opens the VR debug menu).
// kListPick: the list picker's row `index` (one pick); kListDone: Done with listPicked(); kListClose: its X.
// The rows picked on the list picker when kListDone came (render's thread).
std::vector<int> listPicked();

// Python (controls.py), whenever it changes: the game's clock. speed is
// ClockSpeedMode (0 paused, 1..3, 4 loading, 5 super speed); day 0 = Sunday.
void setClock(int hour, int minute, int day, int speed);

// Python (hudinfo.py): the played Sim's needs, moodlets, queue and wants, as
// the game's own widgets get them. icon: image resource instance (0 none).
//   needs:    colour = the state's fill colour (ARGB), flags = its background
//             colour (ARGB), value = 0..1 full.
//   moodlets: colour = mood (hudinfo.py MOODS index).
//   queue:    the buttons of the game's InteractionQueue, running first:
//             icon = the picture (an icon resource, or a thumbnail picture,
//             setPicture), colour = the disc's mood (MOODS index; 0 the
//             default frame), value = 0..1 done (0: no progress ring),
//             flags bit 0 running (or starting: full colour), bit 1 can be
//             cancelled, bits 8..15 the other buttons it would cancel (they
//             show their X while it is pointed at).
//   queue badges: per queue button (same index), its side action's small
//             button (MixerInteractionButton); flags bit 0 there is one,
//             bit 1 can be cancelled, bit 2 running; icon and colour as above.
//   wants:    flags bit 0: a fear (a confrontation want, the game's spiky
//             bubble; it can't be dismissed).
enum Kind { kNeedItems = 0, kMoodletItems, kQueueItems, kWantItems, kQueueBadges, kPopupButtons, kKinds };  // kPopupButtons: tooltips only (the popup's answers)
struct Item {
    uint64_t icon = 0;
    uint32_t colour = 0;
    float value = 0;
    uint32_t flags = 0;
};
constexpr int kMaxItems = 8;
void setItems(int kind, const Item* items, int count);
bool hasItems(int kind);
int itemCount(int kind);
// Python (hudinfo.py): the tooltips of the needs or moodlets, as the game's
// own: per item "title\x1ftext\x1freason\x1fminutes left" (LocalizedString
// JSON each, "" none; minutes -1 none), items split by 0x1e, in item order.
// The queue's: two per button, its own and its side action's (text only).
void setTexts(int kind, const char* data, size_t size);
// Any thread: the household's money went up or down by `amount` (the game's
// SetMoney vfx_amount; hudinfo.py): shown rising and fading for a moment.
void moneyChanged(long long amount);
// A money change is still showing.
bool moneyShowing();
// The tooltip drawn this frame (render): for kNeedItems or kMoodletItems (-1
// none), its height in the atlas from kTip.y.
int tipKind();
float tipHeight();
// Python: an icon's image bytes (PNG, DDS/DST), decoded here (any thread).
void setIcon(uint64_t instance, const uint8_t* data, size_t size);
// The icons the shown items still need (Python reads and sends them).
int missingIcons(uint64_t* out, int max);
// Pictures from the game's thumbnail system (thumbs.cpp), drawn like icons:
// `id` is one of ours, not an icon resource (Python isn't asked for it).
void expectPicture(uint64_t id);
// Any thread: its pixels (RGBA straight, w x h, rows in order).
void setPicture(uint64_t id, const uint32_t* rgba, int w, int h);
// It is in the HUD's icon slots, or on its way there.
bool hasPicture(uint64_t id);

// Python (pie.py), when the game builds a pie menu in first person (empty: none): the game's PieMenuCreate,
// records split by 0x1e, fields by 0x1f: "pie" front (1: the social front page), server reference; per
// category the items name (up to the root) "cat" key, parent key (0 the root), collapsible, display
// priority, always show disabled, icon, name (LocalizedString JSON with the menu's category tokens), the
// name for the active Sim's mood ("" none); per item in the message's order "item" index, category key (0
// the root), pie_menu_priority, score, mood (MOODS index, 0 none), mood intensity, label JSON, disabled
// text JSON ("" enabled), its icon_infos' pictures (comma list). kPiePick: the item `index`; kPieClose: closed
// on the pie (its back button on the first page, or a press off its buttons).
void setPieContent(const char* data, size_t size);
// 0 none, 1 its texts are being looked up, 2 drawn by us, 3 not drawable (the game's pie cut out instead).
int pieState();
float pieScale();
// Which content the pie shows (a new one each setPieContent); clearPie(serial) clears only that one, so a
// closing pie never wipes the next pie's options sent meanwhile (user 2026-10-23: the old cut showed).
unsigned pieSerial();
void clearPie(unsigned serial);

// Python (objmenu.py), when a popup shows (empty: gone): its content from the
// game's own message, records split by 0x1e, fields by 0x1f: first
// "<layout>\x1f<dialog id>\x1f<left picture>\x1f<right picture>" (layout 1:
// two Sims talking, SimInteractionDialog; 0: a plain dialog; the pictures,
// icon or thumbnail ids, 0 none: the other Sim's and yours), then "title\x1f<json>", "text\x1f<json>" and per button
// "button\x1f<json>\x1f<choice id>"; each json a LocalizedString for strings.cpp.
void setPopupContent(const char* data, size_t size);
// There is popup content to draw (its texts may still be waiting for the strings).
bool popupPending();
// The popup card is drawn from the game's texts: its height in the atlas
// (from kPopup.y), 0 while not (strings not loaded, or unknown texts: the
// caller shows a cut of the game's own dialog instead).
float popupHeight();

// Python (objmenu.py), when a picker shows on its object (empty: gone): the
// game's recipe picker from its own message, records split by 0x1e, fields by
// 0x1f: first "picker\x1f<dialog id>\x1f<flags>\x1f<sort column>" (flags: bit
// 0 the Use Ingredients box shows, 1 the Use Prepped Ingredients box shows, 2
// the picker has an ingredients column: the ingredients are used unless the
// box is cleared, 3 it has a price column: prices show), then "title\x1f<json>", per filter tab
// "filter\x1f<tag>\x1f<icon>\x1f<json>" and per row (in the message's order)
// "row\x1f<top>\x1f<name json>\x1f<icon>\x1f<enabled bits>\x1f<price>\x1f<price
// with ingredients>\x1f<prepped>\x1f<both>\x1f<discounted (0 none)>\x1f<skill
// level>\x1f<serving name json>\x1f<its servings: row indices>\x1f<tags>";
// enabled bits: 1 plain, 2 with ingredients, 4 prepped, 8 both. A top row
// with servings opens them, as the game's GridPicker's linked rows.
void setPickerContent(const char* data, size_t size);
// There is picker content (its texts may still be waiting for the strings).
bool pickerPending();
// The picker card is drawn from the game's texts: its height in the atlas
// (from kPicker.y); 0 while not (strings not loaded, or texts missing: the
// caller shows the cut of the game's own picker instead).
float pickerHeight();

// Python (phoneapp.py), when the phone comes out (empty: put away): the game's
// phone menu, records split by 0x1e, fields by 0x1f: first
// "phone\x1f<silenced 0/1>\x1f<disabled tooltip json or empty>", then the
// tree in order: "cat\x1f<node>\x1f<parent node, -1 home>\x1f<icon>\x1f<name
// json>\x1f<priority>" and "item\x1f<node>\x1f<parent>\x1f<name json>\x1f<disabled
// text json>\x1f<notification 0/1>\x1f<priority>".
void setPhoneContent(const char* data, size_t size);
// Python (notebook.py): the game's notebook for the book in your hand, records split by 0x1e (see notebook.py); empty: gone.
void setNotebookContent(const char* data, size_t size);
// How many spreads the notebook has as drawn last (render thread's layout; 0 before the first draw).
int notebookSpreads();
// A spread the pointer asked for (a category cell, the page corners) since the last call; -1 none.
int notebookTakeSpread();
// Any thread (notify.py): the game's notifications for the phone's bell and
// list: "notes" <unread>, then "note" <id> <read> <picture> <title json> <text json> <time>.
void setPhoneNotes(const char* data, size_t size);
// The calendar app's days and entries (phonecal.py records).
void setPhoneCalendar(const char* data, size_t size);
// The My Sim app's values (phonesim.py records).
void setPhoneSim(const char* data, size_t size);
// Present thread: the height of the notification card drawn in kNote (Frame.note), 0 while not drawn.
float noteHeight();
// Present thread: the phone shows its home screen again (Options closed: fades over from the last screen).
void phoneHome();
// Present thread: the phone was taken out: its home screen, black with the game's spinning plumbob until
// it is ready, then fading in.
void phoneTakenOut();

// Any thread (hudinfo / scripted.py via native.hud_balloon): the game is sending an AddBalloon for `sim`
// (BalloonRequest): the pictures (icon resource instance or thumbnail id; 0: none), the balloon type
// (0 thought, 1 speech, 2 distress/shout, 3 sentiment, 4 infant sentiment), its priority (lower = more
// important) and how long it stays (ms). Same rules as SimBalloon.HandleAddBalloon: dropped when this Sim's
// shown balloon is more important; else it takes its slot. Expires by itself.
void addBalloon(uint64_t sim, uint64_t picture, uint64_t overlay, uint64_t category, int type, int priority, int durationMs);
struct BalloonInfo {
    uint64_t sim;
    int slot;   // 0..kBalloonSlots-1: its place in kBalloons
    int type;
    float age;  // seconds since it was added
};
// The balloons still showing (expired ones are dropped here).
int activeBalloons(BalloonInfo* out, int max);
bool balloonActive(uint64_t sim);

// Any thread (scripted.py via native.hud_skillbar): the game sends a SkillProgressUpdate for `sim` (its
// ActionHeadline's SkillBar): the skill's icon (resource instance), the points range of its current level,
// the points now and how fast they grow (points / game minute); hide = the bar is suppressed (NPC, hidden skill).
// Like SkillBar.as: with a rate the bar stays and fills on by itself; at rate 0 it hides after 2 s.
void setSkillBar(uint64_t sim, uint64_t icon, double start, double end, double current, double rate, int level, int maxLevel, bool hide);
struct BarInfo {
    uint64_t sim;
    int slot;
};
int activeBars(BarInfo* out, int max);
bool barActive(uint64_t sim);

struct Frame {
    // Which elements are shown (others are not drawn).
    bool watch = false, pause = false, build = false, needs = false, moodlets = false, queue = false, wants = false;
    bool fullUi = false;     // the full UI on your wrist is on (the button lit)
    bool uiButton = false;   // the watch has the UI button (game menus "Both")
    bool popup = false;      // the popup card
    bool picker = false;     // the picker card (on its object)
    bool pie = false;        // the pie menu drawn by us (kPie)
    bool book = false;       // the notebook's pages (kBookLeft / kBookRight) and cover (kBookCover)
    float bookCrop[4] = {0.07f, 0.04f, 0.02f, 0.05f};  // the page art's outer, spine, top and bottom edges cut off (fractions)
    int bookSpread = 0;      // the spread to draw (0 = the owner and the categories)
    bool money = false;      // the money gained / lost
    bool phone = false;      // the game's phone menu (kPhone)
    float phoneText = 1;     // the phone's text size ([phone] text_size)
    bool phoneWait = false;  // something the phone's screen needs is still coming (the wallpaper, Options' menu): it waits covered
    bool balloons = false;
    bool bars = false;       // the skill bars over Sims' heads   // the balloons over Sims' heads (addBalloon)
    uint64_t note = 0;       // the notification drawn as a card in kNote (notify.py), 0: none
    // The settings page open places this piece (user 2026-10-23): its area gets a see-through green box behind it,
    // and kPreview is filled (cards with no area of their own). w 0: none.
    Rect preview{0, 0, 0, 0};
    bool previewCard = false;
    float px = -1, py = -1;  // the pointer in atlas pixels; < 0: none
    bool down = false;       // pressing
};
// Present thread: draws the atlas. The pressed button's action, once, on
// release; `index` the queue item for kCancel (kMaxItems + n: the side action
// of button n).
Action render(ID3D11Device* device, ID3D11DeviceContext* context, const Frame& frame, int* index = nullptr);
// The atlas (null until drawn once). Premultiplied alpha, B8G8R8A8_UNORM.
ID3D11Texture2D* texture();
void release();

}  // namespace s4vr::hud
