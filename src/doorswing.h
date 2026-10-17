#pragma once

#include <cstdint>

// Swings the game's doors open and shut for the first-person player, the way
// the game does when a Sim's route passes through one. Each door counts the
// Sims passing through it (AnimatedPortalActorComponent +0x38): its open
// animation plays when the count leaves 0 and its close animation when it
// gets back to 0. A route's PortalEnter event adds one (rva 0x9ac080) and its
// PortalExit event takes one away (rva 0x9ac0a0); we call the same two. Static
// analysis of 1.112.519.1020, rechecked on 1.121.361.1020 (DX11); refuses to
// run on other builds.
namespace s4vr::doorswing {

// Any thread: count the player into (open) or out of (close) the door object.
void request(uint64_t objectId, bool open);

// Main thread (the camera copy hook): carry out what was requested.
void pump();

}  // namespace s4vr::doorswing
