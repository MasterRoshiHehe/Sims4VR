#pragma once
#include <cstdint>
#include <vector>

// Object models straight from the game's own packages, for their size.
//
// The game's Python can't load client resources (models), so objmenu.py asks
// here: a background thread reads the index of every Client*Build*.package
// of the game and its packs once (DBPF 2.1; delta builds win over full
// builds) and keeps where each model (MODL, type 0x01661233) is. A model's
// bytes come back still compressed, as stored; Python unpacks them.
namespace s4vr::modelbox {

// Starts the index scan (once; any thread).
void start();
// The stored bytes of the model with this instance id and how they are
// compressed (0 none, 0x5A42 zlib, 0xFFFF RefPack). 0 = found; 1 = the scan
// isn't done yet; 2 = no such model in the game's packages; 3 = read failed.
int read(uint64_t instance, uint16_t& compression, uint32_t& size, std::vector<uint8_t>& bytes);

// Any resource by type and instance (looked up on one background thread
// the first time it is asked for, every answer kept): the game's Client
// packages, then the packages in the Mods folder (a mod's override wins).
// Same answers as read(); 1 = still looking. Type 0 = any picture (PNG, DST/DDS, RLE2).
int readResource(uint32_t type, uint64_t instance, uint16_t& compression, uint32_t& size, std::vector<uint8_t>& bytes);

}  // namespace s4vr::modelbox
