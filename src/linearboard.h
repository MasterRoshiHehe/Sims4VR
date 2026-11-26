#pragma once
#include <d3d11.h>

// The Linear board on an in-game screen (thread "Bring the Linear board into the game", GLA-46, user
// 2026-11-26): "Open Linear board" on a TV or computer (linearboard.py) draws your Linear team's board on
// that screen, our own drawing (columns = workflow states, cards = issues), not a capture of a browser.
// The laser or a finger moves cards: press on a card and drag it to another column (or tap it, then pick
// a column on its details card). A move is sent to Linear at once; what changes in Linear shows within
// [linear] poll seconds while the board is in view.
//
// Linear: GraphQL https://api.linear.app/graphql over WinHTTP, your personal API key in the Authorization
// header. The key is read from %APPDATA%\sims4vr\linear_key.txt the first time, then kept only encrypted
// for your Windows account (DPAPI, linear_key.bin in the same folder) and the text file is wiped. It is
// never written to a log, an ini or the mod's folders.
// Offline: the last board stays up, moves wait and are sent when Linear answers again (a card that was
// changed in Linear meanwhile is left as Linear has it, and the board says so).
//
// Drawn with GDI+ on the board's own thread into a picture that the TV screen path shows like Discord's
// window (claudescreen slot kLinear forwards here; vr_parts/13b_tv_screen.inc, phone3d setTvScreen 1).
namespace s4vr::linearboard {

// Python (through claudescreen::set slot 2): on, the screen (world units: centre, outward normal, up,
// width, height), the team's key ("GLA"), options "done_days=14;canceled=0", seconds between refreshes,
// the picture's width in pixels.
void set(int on, const float v[11], const char* team, const char* options, int pollSeconds, int width);
bool on();
bool screen(float v[11]);
bool frameSize(int& w, int& h);
// The UI render thread: the latest picture as a shader view (not owned).
ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context);
// Present thread: the laser or a finger at u v (0..1 across the picture); buttons bit 0 pressed.
void pointer(float u, float v, int buttons);

// The scrumboard (options mode=cork, user 2026-11-26 "an actual scrumboard"): the board on wall art (a painting,
// a bulletin board) as a cork board with paper notes pinned on it. A hand closing on a note takes it off the
// board: it is drawn in a strip below the board's part of the picture and shown in your hand; let go at the
// board it is pinned there (moved in Linear), let go away from it and it goes back.
bool cork();
// The board's part of the picture (x0 y0 x1 y1, for the panel's crop).
void boardCrop(float crop[4]);
// The card in your hand: its part of the picture (x0 y0 x1 y1), and its size as fractions of the board's width and height.
bool heldCard(float crop[4], float size[2]);
// Present thread, a hand: u v on the board (0..1). grab: true when a note is there (at: where on it the hand
// took it, 0..1); hold: the hand moved (atBoard: close enough to pin it, the slot is shown); drop: let go.
bool grab(float u, float v, float at[2]);
void hold(float u, float v, bool atBoard);
void drop(float u, float v, bool atBoard);

}  // namespace s4vr::linearboard
