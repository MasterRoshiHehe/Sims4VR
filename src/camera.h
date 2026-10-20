#pragma once
#include <cmath>
#include <cstdint>
#include "config.h"

// Drives the game's render camera from the headset.
//
// The game keeps its live camera in a Sims3::Cameras::Camera object owned by
// the CameraControllerManager (+0x10). Once per frame the game copies that
// object into the renderer's camera with one small copy function. We hook the
// copy and edit only the renderer's copy, so the game's own camera logic never
// sees the head pose.
//
// Camera object layout (found in game on 2026-10-11, DX9 and DX11 builds):
//   +0x14 float  frustum width at distance 1  (2 tan(horizontal FOV / 2))
//   +0x18 float  frustum height at distance 1 (+0x14 / aspect)
//   +0x1c float  horizontal FOV, radians
//   +0x20 float  aspect ratio
//   (the game's Camera::SetAspect fills +0x18..+0x20 from +0x14)
//   +0x24 float  near clip distance (about 1.0-1.5 in live mode)
//   +0x28 float  far clip distance (about 1000)
//   +0x30 float4 right axis   (world space, w = 0)
//   +0x40 float4 up axis
//   +0x50 float4 back axis    (the camera looks along -back, as in OpenXR)
//   +0x60 float4 position     (w = 1)
namespace s4vr::camera {

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
// Same meaning as XrFovf: half-angles in radians, left and down negative.
struct Fov {
    float left, right, up, down;
};

// Starts a background search for the game camera.
void init(const Config& config);
// The settings menu: body mode's view smoothing, turn smoothing, eye inset and near limit
// (lowest, follows what's close, highest), live.
void configureView(float smoothing, float turnSmoothing, float eyeInset, float nearClip, bool nearAdaptive,
                   float nearMax);
// Settings menu: [body] seat_follow, live.
void setSeatFollow(bool on);
// Settings menu: [audio] pan_any_distance, live: in first person every sound 1 m or more
// away is placed where it is (the game spreads some toward the middle of your head).
void setPanAnyDistance(bool on);
// True once the live camera has been found.
bool available();

// Use this eye pose for the frames the game renders from now on. Pose is in
// OpenXR space (metres, Y up, -Z forward), relative to the recentered origin.
void apply(int eye, const Vec3& eyePosition, const Quat& eyeOrientation, const Fov& fov);
// The headset itself (between the eyes), same space; body mode places the Sim under it.
void setHead(const Vec3& centre, const Quat& orientation);

// What the game actually rendered, oldest first: one entry per camera copy the
// hook changed. Present pops one per frame, so game-thread latency doesn't
// mix up the eyes. fov is the symmetric FOV the game rendered when the FOV
// override is on.
// First person, moving freely: your own body as the game drew it in one eye's
// view, as capsules (view space: x right, y up, z ahead, world units), with
// the near and far limits that view was drawn with, so its depth tells which
// pixels are your body (vr.cpp, bodylayer.h).
constexpr int kBodyParts = 48;
struct BodyParts {
    int count = 0;  // 0: no body to split off
    float nearZ = 0, farZ = 0;
    float a[kBodyParts][4];  // segment start, radius
    float b[kBodyParts][4];  // segment end
};
struct Rendered {
    int eye;
    Vec3 position;
    Quat orientation;
    Fov fov;
    float yaw = NAN;  // radians: app space's heading in the world when this eye was rendered
    float lead = 0;   // radians of yaw drawn ahead of the stick turn (setTurnLead)
    // First person, paired eyes: a left eye the next right eye is drawn from
    // the same place as (Present holds it back until that right eye arrives).
    bool pairFirst = false;
    float nearZ = 0, farZ = 0;  // the planes it was drawn with (0: not known)
    // The near / far planes of the projection the game drew the frame before
    // with (its z terms at the render camera's +0x70; 0: not known). A check
    // on nearZ / farZ for the depth eye's numbers log.
    float drawnNear = 0, drawnFar = 0;
    BodyParts body;
};
bool takeRendered(Rendered& out);
// App space's heading in the world now: the newest rendered frame's, plus the
// stick turn not rendered yet (NAN if unknown). Present thread.
float viewYaw();
// Turn lead: how far ahead of the stick turn the next pair's heading is drawn
// (radians of yaw, free movement only), so it shows where the turn is by then.
void setTurnLead(float radians);
// Paired eyes (first person): each right eye drawn from where its left eye
// was drawn (Config::pairedEyes); Present switches it off under SteamVR's eye align.
void setPairedEyes(bool on);
// Each eye drawn with its own lopsided field of view (Config::exactFov), live.
void setExactFov(bool on);
// Paired hands (Config::pairedHands): the right eye of a free-moving pair
// already moves the player on for the next pair, so the game poses the body
// and arms from where that pair is drawn from.
void setPairedHands(bool on);
// Body layer (Config::bodyLayer): Rendered::body is filled in while on.
void setBodyLayer(bool on);
// Body mode: anchor the view at this game object's head joint (an active
// Sim's object id) instead of the game camera position. 0 turns it off.
void setBody(uint64_t objectId);
// The Sim's world position and orientation, as Python reports them.
void setBodyRoot(const Vec3& position, const Quat& orientation);
// Objects that may be in the Sim's hands (props, carried objects), from
// Python; checked each frame against where your palms are.
void setHeldCandidates(const uint64_t* ids, int count);
// Stop changing the render camera (quad mode).
void release();

// Mapping between OpenXR app space and the game world, as of the latest frame
// the hook changed. False until stereo has rendered a frame.
bool xrToWorld(const Vec3& xr, Vec3& world);
// The same for a direction (no translation or scale).
bool xrDirectionToWorld(const Vec3& xr, Vec3& world);
// Body mode is on and the view is at the Sim's eyes.
bool bodyActive();
// The first-person Sim's object id (0 in the game camera).
uint64_t bodyId();
// Any thread: the headset centre in the world and its axes (world units), as of
// the latest pose. False until stereo has rendered a frame.
bool headInWorld(Vec3& position, Vec3& right, Vec3& up, Vec3& back);
// Body mode: keep the view heading still (while the stick steers the Sim).
void holdBodyTurn(bool hold);
// Python: an animation listed in [body] body_view_anims plays, so "straight
// ahead" lies the way the Sim's upper body does (headset still looks around).
void setBodyView(bool on);
// Body mode: turn the view (and so where the stick walks) by this many radians.
void turnBody(float radians);
// Body mode, only while the view lies with the body (setBodyView): tilt the
// view up (+) or down by this many radians; dropped otherwise.
void pitchBody(float radians);
// Height of the floor the player is looking at, in app space.
bool floorInXr(float& y);
// The game's floor height in world units (the point its camera looks at).
void setFloor(float worldY);
// Projects a world point through the game's own (unmodified) camera, the one
// it uses for mouse picking. x right, y up, -1..1 across the window.
bool projectLive(const Vec3& world, float& x, float& y);

// Any thread: the render camera as applyEye last wrote it (world units), and
// how OpenXR app space sits in the world then (world = anchor + heading *
// (xr * scale)). For drawing our own things into the game's 3D view.
struct View {
    Vec3 right, up, back, position;  // the camera looks along -back
    float nearZ, farZ;               // clip distances (world units)
    float tanX, tanY;                // half-width and half-height of the frustum at distance 1
    float centreX = 0, centreY = 0;  // its centre at distance 1 (a lopsided eye's frustum is off centre)
    Vec3 anchor;
    Quat heading;
    float scale;
};
bool lastView(View& out);

// Any thread: the first-person Sim's hand joint (0 left, 1 right) in the
// world as the game thread last read it (under half a second ago), and that
// Sim's id. Python uses this instead of reading client objects itself.
bool handJoint(int hand, uint64_t& simId, float position[3], float orientation[4]);

// Any thread, first person: your Sim's forearm (0 left, 1 right): its elbow
// and wrist joints in the world, as drawn (under 0.3 s old).
bool forearm(int side, float elbow[3], float wrist[3]);
// Any thread, first person: your Sim's torso joints in the world, as drawn
// (under 0.3 s old): pelvis, spine 0, 1 and 2, left and right upper arm.
bool torso(float joints[6][3]);

// Dev probe: the client object for a game object id (null if none), and a
// joint's world transform (float[12]: t xyz _, q xyzw, scale) by joint name.
void* objectPointer(uint64_t id);
bool jointTransform(uint64_t id, const char* joint, float out[12]);

// Game units per metre of head and hand movement; changeable while playing.
void setWorldScale(float scale);
float worldScale();

// The camera finder watches copies through this; null stops watching.
using CopyObserver = void (*)(void* dst, const void* src, void* caller);
// Whether a game camera object is the one the headset's eyes render from
// (pointer or eye position), not a mirror's, water's or shadow's.
bool yourEyeCamera(const void* camera);
bool installCopyHook();
void setCopyObserver(CopyObserver observer);

}  // namespace s4vr::camera
