#pragma once
#include <cstdint>

#include "camera.h"

// Touch to interact (first person): point with your index finger (grip
// pressed, trigger not touched) and touch something to click it, which opens
// its pie menu at your hand.
//
// The game finds what is under the mouse with an asynchronous GPU pick
// (RenderPickManager): a request is rendered with the render camera of that
// frame and read back 1 to 3 frames later into the pick dispatcher's hover
// result (object id, pick type, world hit point; RE notes
// /tmp/claude-0/re/pick_notes.md, game 1.112.519.1020 DX11; rechecked on
// 1.121.361.1020). While you point, every frame asks for a pick at the pixel
// where the fingertip shows in that frame's eye camera, so each answer's hit
// point can be compared with the fingertip along the same ray. A mouse click
// then uses the cached answer, so it lands exactly on what the finger touched.
namespace s4vr::touch {

// Finds the pick globals and the request function; false on other builds.
bool install();

// Present thread, every frame: the hand whose index finger points (0 left,
// 1 right, -1 none).
void setPointing(int hand);
int pointingHand();

// Camera copy hook (main thread), right after the eye camera was written into
// `camera`: asks for a pick through the fingertip (world space) with that
// camera and checks the answers to earlier asks. `self` is the player Sim's
// object id: the finger itself is never a touch.
void update(const uint8_t* camera, uint64_t self, bool haveTip, const camera::Vec3& tip);

struct Contact {
    bool valid = false;     // an answer for the pointing finger
    bool touching = false;  // it is on (or in) something you can click
    uint64_t objectId = 0;
    int pickType = 0;       // PickType: 2 object, 3 Sim, 9 misc, ...
    float gap = 0;          // metres from the fingertip to the surface along your view; < 0 inside it
    float u = 0, v = 0;     // where the pick was, 0..1 of the window
    uint64_t tick = 0;      // GetTickCount64 of the answer
};
// Present thread: the latest answer.
Contact latest();

// The laser into the world (first person and the game camera, user
// 2026-10-15 17:13Z: it looked different in each eye). It used to end 30 m
// out (or on the floor plane), so its end marker floated far behind what it
// pointed at, on a different spot in each eye, and the mouse went where that
// far point showed in the latest eye, a few cm off in the other eye's frames.
// Now, while no finger points, every frame asks for a pick where a point on
// the laser shows in that frame's eye camera, and that point moves to the
// depth of what the answer hit (projected onto the laser), so within a few
// frames it sits on the surface the laser crosses: the same spot in both eyes,
// and the spot the game picks. World units; Present thread.
void setLaser(const camera::Vec3& origin, const camera::Vec3& direction, float maxLength, float startLength);
void laserOff();
struct LaserEnd {
    bool valid = false;
    float length = 0;  // world units along the laser
    int pickType = 0;  // what it ends on (0: nothing, as far as it goes)
    uint64_t objectId = 0;
    uint64_t tick = 0;  // GetTickCount64 of the answer
};
LaserEnd laserEnd();

}  // namespace s4vr::touch
