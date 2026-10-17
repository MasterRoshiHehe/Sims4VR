#pragma once

#include <cstdint>

// Ends the client's walk for the player. The walk cycle comes from a client
// route we send (scriptmod stride.py), and the client only leaves a route when
// its locomotion controller picks a stop clip, which needs the Sim to have
// walked the path itself. Ours is pinned under the player, so the stop never
// came and the legs walked in place. This stops the controller the way the
// engine's own stuck-route watchdog does (AnimatedSimBody::Update, rva
// 0x1919f62; 0x1956d12 in 1.112): controller vtable slot 57 (+0x1c8). That
// ends the path, sends route_complete and frees the Sim's op channel. Static
// analysis of 1.112.519.1020, rechecked on 1.121.361.1020 (DX11); refuses to
// run on other builds.
namespace s4vr::walkstop {

// Any thread: end route `routeId` on object `objectId` if it is still the
// client's active route; routeId 0: whatever route the client has on it.
void request(uint64_t objectId, uint64_t routeId);

// Any thread: the route the client has active on `objectId` (0: none), as of
// the last frame; watches that object from now on (v0.13.165: halt.py ends a
// game route left playing on the client in free movement).
uint64_t clientRoute(uint64_t objectId);

// [body] walk_clips: the game's own start and stop walk clips (stride.py sends
// routes they can play on; the player slows down over the stop clip).
void setClips(bool on);
// Any thread: the client walk controller of `objectId` as of the last frame
// (watches it from now on): state (2 start, 4 stop, -1 none), path ahead,
// stop clip and start clip distances (0 unknown). False if not read yet.
bool info(uint64_t objectId, float out[4]);

// Main thread (the camera copy hook).
void pump();

}  // namespace s4vr::walkstop
