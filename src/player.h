#pragma once
#include <cstdint>

// First person with free movement. The player is our own object, not the Sim:
// a spot on the floor that the left stick walks (sliding along the walls
// Python finds), the right stick turns, and your real head moves around in.
// The camera hangs off the player, and every frame the Sim's body is put
// under your headset, facing where you walk (or look, when standing still).
// The simulation's Sim is only moved to the player now and then, by Python
// with a plain teleport: no routes, no interactions, nothing to fight.
//
// While the game itself moves the Sim (a route to an object, sitting down),
// Python says free movement isn't allowed and the camera follows the Sim's
// head as before. Pushing the stick then asks Python to end what it's doing.
//
// Sim actor layout (Sims3::Animation::Actor, from the object lookup), live
// tested 2026-10-11: +0x2e0 float3 position, +0x2f0 quat xyzw, +0x300 scale.
//
// While you move freely the body is pinned to you: the actor's SetTransform
// (vtable +0x28, which the game's root motion and route following call too)
// is hooked and gets the player's place for that actor, so nothing else can
// pull the body away for a frame (it twitched, and on stairs the walk route
// held it at the floor it started from).
namespace s4vr::player {

struct Input {
    uint8_t* actor;        // the body Sim's actor
    float headJoint[3];    // its head joint in the world, read this frame
    float eyes[3];         // midpoint between its eyes in the world (NAN: no eye joints)
    float eyeUp;           // how far its eyes sit above the head joint, head level (NAN: not measured)
    float eyeInset;        // metres your view sits behind its eyes
    float hmd[3];          // headset centre in app space (metres)
    float hmdForward[2];   // headset forward in app space, x and z
    float scale;           // game units per metre
    // Paired eyes: work out the next pair's place on the right eye before it,
    // but only pin the body there; the game's own SetTransform (root motion,
    // while the walk plays) puts it there as it poses the Sim, or settle()
    // does on the left eye.
    bool deferPlace = false;
};
struct Output {
    float base[3];  // world position of the app space origin (eye level at recenter)
    float viewYaw;  // heading of app space in the world (camera yaw convention)
};

// Game thread, once per frame in body mode. viewYaw is the view heading so far
// (free movement starts from it); turn is the right stick's turn this frame.
// True while free movement is on: put the camera at out.
bool update(const Input& in, float viewYaw, float turn, Output& out);
// Body mode off, or another Sim.
void reset();
// Any thread, first person ending: the game moves the Sim's body again at
// once (reset() follows on the game thread).
void release();
void configure(float walkSpeed, float runSpeed, float eyeForward, float eyeHeight, bool simCollision);

// First person: the game draws your body at your own height instead of snapping
// it to the floor of the level your Sim is on ([body] keep_height).
void setKeepHeight(bool on);

// First person: your head can't go into other Sims ([physical] head): leaning
// into one pushes you (the player, not any Sim) back out sideways, keeping
// your view radiusMetres clear of their body. Needs simCollision (configure).
void setHeadRepel(bool on, float radiusMetres);
// Game thread: whether that is on, and the radius (camera.cpp pushes only the
// view the same way while the game moves your Sim: scenes, seats).
bool headRepelOn(float& radiusMetres);

// First person: walking into walls and furniture slides along them smoothly
// ([body] smooth_slide); off = the old snap to the nearest clear direction.
void setSmoothSlide(bool on);

// First person: seconds walking takes to come to rest after you let go of the
// stick ([body] stop_time), easing in and out; 0 = the old quick stop.
void setStopTime(float seconds);

// Any thread: running (a tap of the left stick switches it). Free movement
// goes at the run speed and Python gives the walk the Sim's run walkstyle.
// Off again when first person ends.
void setRunning(bool on);
// Present thread, every headset frame in first person ([stereo]
// walk_display_clock): the time the headset shows that frame for (its
// refreshes since the frame before). The next update moves you by the time
// gathered since the one before, instead of the PC clock.
void addShownTime(float seconds);
bool running();

// Python, ten times a second: may the player move freely (the Sim stands and
// the game isn't moving it); which of 24 directions are blocked close by
// (bit i = heading i*15 degrees from +Z toward +X); and the floor near (x, z):
// floor = {x, z, height there, rise per unit along X, rise per unit along Z,
// lowest, highest} (a stair ramp is a plane held between its two floors).
// flight = the stairs you are on or at: {bottom step centre x, z, up
// direction x, z, run length, half width}; run 0 for none. Its sides hold you.
void setState(bool freeAllowed, uint32_t blockedMask, const float floor[7], const float flight[6]);
// For Python: x y z of the Sim's body, its facing (radians, 0 = +Z), moving
// (0/1), free (0/1), and walking velocity x z (units per second). False until
// the first frame in body mode.
bool pose(float out[8]);
// For Python while you slow down after letting go of the stick: where you come
// to rest (x, z) and how far that still is; false when not slowing.
bool rest(float out[3]);
// The walk's stop clip distance (walkstop.cpp reads it from the client's walk
// controller; 0 = not known or walk clips off): slowing down covers it.
void setClipStop(float metres);
// Where the body is pinned (position, rotation xyzw), while it is or for a
// few seconds after the pin let go.
bool pinPlace(float out[7]);
// Any thread: where this actor is pinned right now (position, rotation xyzw);
// false if it isn't pinned.
bool pinnedAt(const void* actor, float out[7]);
// Game thread: a body pinned somewhere it isn't yet (deferPlace) is put there
// now. True if it had to be moved.
bool settle(uint8_t* actor);

}  // namespace s4vr::player
