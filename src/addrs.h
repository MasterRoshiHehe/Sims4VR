#pragma once
#include <cstdint>

// The game's own functions, globals and vtables the mod uses, by name. One table
// (addrs_table.inc, generated from re/exe/tools/addrs_src.tsv by gen_addrs.py)
// holds each one's rva, a masked byte signature and, for globals, an anchor; after
// a game patch that table is the only thing to remap.
namespace s4vr::addrs {

// A function by table name, checked against its signature; null (and one log line,
// once) if the running game build is not the table's or the bytes do not match.
uint8_t* fn(const char* name);

// A global variable or vtable by table name; null if the game build is not the table's.
uint8_t* global(const char* name);

// The exe stamp / game version the table was generated for.
uint32_t tableStamp();
const char* gameVersion();

}  // namespace s4vr::addrs
