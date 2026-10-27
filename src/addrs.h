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

// Resolve the whole table now (otherwise at the first lookup). Call after gameaddr::setTryUnknown.
void prepare();

// Where a row is according to its signature or anchor, whether or not the mod may use it (the RTTI
// fallbacks and the patch report); null if it was not found.
uint8_t* resolved(const char* name);

// One row for the patch report.
struct RowInfo {
    const char* name;
    bool global;
    uint32_t tableRva;
    uint32_t foundRva;  // 0: not found
    bool used;
    const char* anchor;
    const char* state;
    const char* why;
};
size_t rowCount();
RowInfo row(size_t index);

// The table name of a function the mod resolved at this address (hook logging), or null.
const char* nameAt(const void* address);

// The exe stamp / game version the table was generated for.
uint32_t tableStamp();
const char* gameVersion();

}  // namespace s4vr::addrs
