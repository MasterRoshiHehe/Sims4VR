#pragma once
#include <d3d11.h>

// First person: your own face, eyes, teeth and eyelashes are folded away in
// the view from your eyes only. Mirrors, water and shadows still show them
// (user 2026-10-13, again 2026-10-16: no teeth and eyes from the inside, keep
// the face in mirrors, keep the hair).
//
// How the game draws a Sim: each mesh of it (head, body, hair, teeth...) is
// skinned on the CPU every frame (game+0x1220550: the mesh's bone matrices
// from the pose, then game+0x1296ee0 writes every vertex's position and
// normal into the mesh's vertex buffer, 12 bytes a vertex). Every view of the
// frame (shadows, mirrors, water, your eyes) then draws from that one buffer.
//
// So after the game skins a mesh of the Sim you are in that has face bones
// (any joint below b__Head__), the same vertices are skinned again into our
// own copy, and there the vertices that mostly follow the head and its face
// joints are all moved to one point at the neck: their triangles have no area
// and draw nothing; the ones joining the neck close it. When the game binds
// that vertex buffer for a pass that draws into the 3D scene's depth (your
// eyes' view: depth, opaque and see-through passes), it gets our copy;
// mirrors, water and shadow maps draw into their own targets and keep the
// game's. Meshes without face bones (hair, hats, the body) are left alone.
namespace s4vr::facehide {

// Hooks the game's per-mesh skinning (DX11 game 1.128.90.1030; once).
void install(bool on);
// [body] hide_face, the B menu.
void setEnabled(bool on);

// Game thread (camera), every frame in first person: the Sim you are in
// (its client Actor). Stops by itself when this isn't called for a moment.
void setBody(void* actor);

// uihook: hook IASetVertexBuffers of these contexts' implementations (once a device exists).
void hookContexts(ID3D11Device* device);
// uihook, every OMSetRenderTargets: whether this context now draws into the
// 3D scene's depth.
void targets(ID3D11DeviceContext* context, bool sceneDepth);

// Your face was folded away in your view lately (camera: the near limit
// needn't cut past your own head).
bool hiding();

}  // namespace s4vr::facehide
