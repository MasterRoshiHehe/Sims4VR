#pragma once

// Both real eyes: CPU time per eye of the scene lists' other stages (job threads), to find the next
// part of the right eye's pass worth leaving out (v0.13.321). Logs "Pass time" every 10 s; changes
// nothing in the game.
namespace s4vr::passtime {

void install();

}  // namespace s4vr::passtime
