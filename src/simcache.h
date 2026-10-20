#pragma once
// Sim detail caches (perf experiment). The game reads DeformerMapsCacheBudget (bytes, default 40 MB),
// SimCacheSizeLimit (MB, 0 = unset) and RenderSimLODDistances (4 floats) from its options. This logs the game's own
// values and, when asked, raises the two caches (so walking past Sims rebuilds their body shape less often) and
// stretches the LOD distances (so Sims change detail level less often). Takes effect when the game reads its options.
#include <cstdint>

namespace s4vr::simcache {

// boost: caches x4 (SimCacheSizeLimit unset -> cacheMb). lodScale: 1 = the game's own LOD distances.
void install(bool boost, int cacheMb, float lodScale);
// Called from the ConfigInt hook (shadows.cpp) for every int option; returns the value to give.
int32_t intValue(const char* name, int32_t value);

}  // namespace s4vr::simcache
