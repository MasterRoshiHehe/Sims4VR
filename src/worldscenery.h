#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The neighbourhood's own scenery (trees, rocks, lamps, fences outside the
// lots): placed by the area's .world file (resource FCB1A1E4, re/client
// WORLD-AND-ROADS.md) and only drawn by the game, never a live object, so
// collide.py can't find it among the game's objects. Read here from the game
// folder (read only) for collision.
namespace s4vr::worldscenery {

struct Placed {
    uint64_t model;  // MODL instance (the record's footprint key; the same instance in 98.5% of them)
    float x, y, z, qx, qy, qz, qw, scale;
};

// The placed objects of the area holding the lot with this lot description id
// (Python: services.get_world_and_lot_description_id_from_zone_id). 0 = found;
// 1 = still looking (a background thread reads the .world files' indexes the
// first time); 2 = no area has that lot; 3 = read failed. area gets the .world
// file's name.
int forLot(uint64_t lotDescription, std::vector<Placed>& out, std::string& area);

}  // namespace s4vr::worldscenery
