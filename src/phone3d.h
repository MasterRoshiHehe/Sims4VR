#pragma once
#include <vector>
#include <d3d11.h>
#include <cstddef>
#include <cstdint>

// Your Sim's phone as a real 3D object in the game's view (DX11 game only).
//
// The phone is a rounded slab drawn straight into the game's main colour
// target just before the UI, with the game's own scene depth, so whatever is
// nearer (your Sim's fingers wrapped around it) stays in front of it. Its
// screen shows the game menu from the UI layer (uihook), fitted inside the
// glass. The camera is the render camera applyEye wrote (camera::lastView),
// the projection rebuilt from its frustum and clip distances.
namespace s4vr::phone3d {

constexpr int kMaxTiles = 10;

struct Phone {
    bool show = false;
    // OpenXR app space (metres): the phone's centre and axes (x right, y up,
    // z out of the screen).
    float centre[3]{}, x[3]{}, y[3]{}, z[3]{};
    float width = 0, height = 0, depth = 0, radius = 0;  // metres
    // In the Sim's right hand: the centre from the grip pose (tilted by
    // hand_pitch, metres) and the tilt of its top away from you (radians).
    // The drawn phone follows the hand joint as the game posed it, so it stays
    // in the Sim's fingers; centre and axes above are the fallback.
    float grip[3]{};
    int side = 1;       // the hand: 1 right, 0 left (its phone the mirror image of the right's)
    float tilt = 0;
    float yaw = 0;      // then its turn about its own up axis (radians)
    float pitch = 0;    // then its top tipping away from you, about its own left-right axis (radians)
    float screen[4]{};  // the glass in the phone's frame: centre x, y, width, height (metres)
    float menu[4]{};    // the menu on it, same terms
    float crop[4]{};    // the menu's part of the UI texture: u0, v0, u1, v1
    // The tablet: pieces of the UI laid out on the menu rect instead of crop.
    // Each tile's place on it from its top left (x0, y0, x1, y1, 0..1) and
    // its part of the UI texture (u0, v0, u1, v1).
    int tiles = 0;
    float tileDst[kMaxTiles][4]{}, tileSrc[kMaxTiles][4]{};
    bool wallpaper = false;  // the tablet: the phone background picture (setWallpaper) fills the glass behind the UI
};

// Any thread: the played Sim's phone background picture (an image resource's
// bytes: PNG, DDS or EA's shuffled DST), for the tablet. False if it can't be read.
bool setWallpaper(const uint8_t* data, size_t size);
// Any thread: an image resource's bytes (PNG and other WIC formats, DDS or
// EA's shuffled DST) to RGBA pixels (R in the low byte, straight alpha).
bool decodePicture(const uint8_t* data, size_t size, std::vector<uint32_t>& rgba, int& w, int& h,
                   const char** kind = nullptr);

// Present thread: what to draw from now on, and the UI texture to take the
// menu from (single sample, shader-readable; null for a black screen).
void setPhone(const Phone& phone, ID3D11Texture2D* ui);

// The UI's render thread (uihook), just before the UI is drawn: draws the
// phone into `target` (the game's main colour view) with `depth` (its scene
// depth-stencil view), depth test only. `reversed`: the scene clears depth
// to 0 (nearer is larger). Restores every state it touches.
void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth, bool reversed);

// Any thread: drawn within the last 200 ms (else vr.cpp shows the flat phone).
bool drawnRecently();

// Depth eye (deptheye.h): every piece drawn writes its depth into the scene's
// (after the near limit's copy), and the depth is copied again after them
// (nearplane::senseLate). Any thread.
void setWriteDepth(bool on);
bool writesDepth();

// A flat menu panel fixed in the world (menus on objects: a picker on a fridge
// door or a TV screen), drawn the same way so your hands and anything nearer
// stay in front of it. World units: its centre, its right and up axes (unit
// length) and its size. Shows `crop` (u0, v0, u1, v1) of the UI texture.
struct Panel {
    bool show = false;
    float centre[3]{}, x[3]{}, y[3]{};
    float width = 0, height = 0;
    float crop[4]{};
    // Where the menu sits on it, as fractions of its size: centre x, y (0 =
    // the middle), width, height. Dark glass around it, like a screen.
    float menu[4]{0, 0, 1, 1};
    // Clear: no glass, only the menu where the UI draws it (the pie menu),
    // blended over the world at `opacity`.
    bool clear = false;
    float opacity = 1;
    // A clear panel drawn in front of the world, like the pie (only your arms
    // and hands stay in front of it): a popup in front of you never ends up
    // inside a wall. Only for the panel (setPanel).
    bool onTop = false;
    // A clear panel cut to the ellipse inside its menu rect (a round button).
    bool round = false;
    // A clear panel keeping only the UI's solid parts (alpha >= key): the
    // action queue's bubbles without the see-through backdrop behind them.
    float key = 0;
    // A piece (setPieces) placed again on the render thread just before it is
    // drawn (setPiecePlacer), so it moves with the body of that very frame.
    int tag = -1;
    // Placed in OpenXR app space (`appAt` metres, axes appX / appY): put into
    // the world on the render thread with the camera of the very frame drawn,
    // so it doesn't sit differently in each eye while you walk (user 20:13Z,
    // the pie). centre / x / y / width / height are then worked out there.
    bool app = false;
    float appAt[3]{}, appX[3]{}, appY[3]{}, appW = 0, appH = 0;
};
// Present thread: what to draw from now on (show = false: nothing).
void setPanel(const Panel& panel, ID3D11Texture2D* ui);
// Any thread: the panel was drawn within the last 200 ms (else vr.cpp shows it as a flat layer).
bool panelDrawnRecently();
// The pie menu (first person): a second panel, normally clear, drawn after the
// one above. Same terms.
void setPie(const Panel& pie, ID3D11Texture2D* ui);
bool pieDrawnRecently();
// Speech bubbles and the like (bubbles.h): clear panels taking `crop` from
// `atlas`, depth-tested, drawn after the panel above and before the pie.
// Present thread; count 0 (or no atlas): none.
constexpr int kMaxBubbles = 24;
void setBubbles(const Panel* bubbles, int count, ID3D11Texture2D* atlas);
// Pieces of the game's UI on your arms and around you (first person, vr.cpp
// "Wrist pieces"): clear panels taking `crop` from the UI texture, drawn after
// the bubbles; depth-tested, or over the world when onTop. Present thread.
constexpr int kMaxPieces = 12;
void setPieces(const Panel* pieces, int count, ID3D11Texture2D* ui);
// Render thread, as the pieces are drawn: places the tagged ones again from
// the joints of the frame being drawn (vr.cpp: the HUD on your arms; up to
// v0.13.152 they were placed at Present, a frame behind the body, and showed
// doubled in the two eyes while walking).
void setPiecePlacer(void (*placer)(Panel* pieces, int count));
// Sims Forever (simsforever.h): Sims 1's picture on a computer's screen, drawn
// first of the panels. Same terms; `crop` is of Sims 1's frame, which this
// takes from simsforever::frameView on the render thread.
void setSims1Screen(const Panel& screen);

// Game thread (camera.cpp), first person: your arms and hands as segments
// a..b (world) this thick. The pie menu is drawn in front of everything else
// in the world (it never ends up inside a computer or a wall), but these stay
// in front of it.
struct HandCapsule {
    float a[3], b[3], radius;
};
void setHands(const HandCapsule* capsules, int count);
bool sims1ScreenDrawnRecently();

}  // namespace s4vr::phone3d
