#pragma once
#include <d3d11.h>

// First person: the near limit (how close things can be before the view cuts
// them off) follows what is actually close, to fight flicker (DX11 game).
//
// Depth precision shrinks with the near limit: at 8 cm, two surfaces a few
// millimetres apart fight over which one shows, and far buildings shimmer, 18
// times worse than at the game's own 1.5 m. So the limit only stays that low
// while something is that close. Each frame the GPU finds the nearest point
// of the finished 3D view (sense, read back a frame or two later by collect),
// your own body left out of it, and the near limit sits well inside it; your
// hands and legs (joints, measured fresh each frame by camera.cpp) and
// anything seen close in the last moments keep it down too. It drops at once and rises slowly, never past maxNear.
// Without a fresh measurement it stays at minNear ([body] near_clip).
namespace s4vr::nearplane {

// A piece of your own body: the segment a..b (world), this thick (a == b: a
// sphere).
struct Capsule {
    float a[3], b[3], radius;
};
constexpr int kMaxCapsules = 24;

// Game thread (camera applyEye), first person, just before choose: your own
// body, which the measurement leaves out (choose's body keeps what of it must
// stay whole), and the view it is seen with (eye, axes, half-FOV tangents).
// The frustum is centred on (centreX, centreY) at distance 1 (a lopsided eye's).
void ownBody(const float eye[3], const float right[3], const float up[3], const float ahead[3], float tanX,
             float tanY, float centreX, float centreY, const Capsule* capsules, int count);

// Game thread (camera applyEye), first person only: the near limit for this
// frame. eye: the camera's world position; forward: its view direction;
// body: your own body's nearest view depth in the view (+inf none, <= 0 not
// known); head: the view depth where your own head ends (0 not known): the
// limit stays at least there, so the inside of your face (eyes, teeth) never
// shows, unless a hand is nearer (mirrors have their own camera and still
// show your face); farZ: the game's far limit.
float choose(const float eye[3], const float forward[3], float body, float head, float farZ, float minNear,
             float maxNear);

// The UI's render thread (uihook), the 3D view finished: measure its nearest
// point, your own body left out. Only while choose ran lately.
void sense(ID3D11DeviceContext* context, ID3D11DepthStencilView* depth, bool reversed);

// Present thread, the immediate context: read finished measurements back.
void collect(ID3D11DeviceContext* immediate);

// The UI's render thread, right after sense: this frame's copy of the scene
// depth (shader-readable; AddRef'd, the caller releases it), and its sample
// count. Null when sense made none this frame.
ID3D11ShaderResourceView* sceneDepth(UINT& samples);

// Present thread, the immediate context: the scene depth copy of the frame
// being presented (the game finishes a frame's drawing before presenting it),
// as sceneDepth, with its size and whether it clears to 0 (reversed). Null
// when none was made lately (the near limit isn't measuring).
ID3D11ShaderResourceView* latestDepth(UINT& samples, UINT& width, UINT& height, bool& reversed);

// The UI's render thread, after our own 3D pieces (phone, menus, pie) were
// drawn into the view with their depth (phone3d::setWriteDepth): a second copy
// of the scene depth, for the depth eye (deptheye.h). Only after sense copied.
void senseLate(ID3D11DeviceContext* context, ID3D11DepthStencilView* depth);
// Present thread: that copy, as latestDepth.
ID3D11ShaderResourceView* lateDepth(UINT& samples, UINT& width, UINT& height, bool& reversed);
// Present thread, the immediate context: copies `scene` (the 3D view's depth-stencil, which holds
// the presented frame's finished depth at Present) and returns that copy, as latestDepth. Always
// this frame's, where the copies above may be a frame behind (taken when the UI is recorded).
// Null until sense has copied once, or if the depth is another size.
ID3D11ShaderResourceView* presentDepth(ID3D11DeviceContext* immediate, ID3D11DepthStencilView* scene, UINT& samples,
                                       UINT& width, UINT& height);
// How long ago (ms, CPU time) the late or the plain copy was made (-1: never).
double depthAgeMs(bool late);

}  // namespace s4vr::nearplane
