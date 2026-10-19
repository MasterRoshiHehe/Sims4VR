#include "addrs.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gameaddr.h"
#include "log.h"

namespace s4vr::addrs {

namespace {

struct Row {
    const char* name;
    char kind;  // 'f' function, 'g' global or vtable
    uint32_t rva;
    const char* sig;     // hex bytes, ?? = any (relative operands)
    const char* anchor;  // globals: the table function that reads it through a rip-relative operand
    uint32_t dispOffset;  // ... offset of that disp32 inside the function
    uint32_t endOffset;   // ... offset of the instruction's end (rip) inside the function
};

#include "addrs_table.inc"

constexpr size_t kCount = sizeof(kRows) / sizeof(kRows[0]);

enum class State : uint8_t { Pending, Ok, Moved, Unresolved, Ambiguous };

struct Slot {
    uint8_t* addr = nullptr;
    State state = State::Pending;
};

Slot g_slots[kCount];
std::once_flag g_once;
bool g_recognised = false;
uint8_t* g_text = nullptr;
size_t g_textSize = 0;

uint8_t* base() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = compare
};

Pattern parse(const char* sig) {
    Pattern p;
    for (const char* s = sig; *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
        } else {
            p.bytes.push_back(static_cast<uint8_t>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            p.mask.push_back(1);
        }
        s += 2;
    }
    return p;
}

bool matchAt(const uint8_t* at, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); ++i)
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    return true;
}

// Matches of a signature in .text, up to two (unique or not).
int scan(const Pattern& p, uint8_t** first) {
    if (p.bytes.empty() || !g_text) return 0;
    size_t lead = 0;
    while (lead < p.bytes.size() && !p.mask[lead]) ++lead;
    if (lead == p.bytes.size()) return 0;
    int hits = 0;
    const size_t n = p.bytes.size();
    for (size_t i = 0; i + n <= g_textSize; ++i) {
        if (g_text[i + lead] != p.bytes[lead] || !matchAt(g_text + i, p)) continue;
        if (!hits++ && first) *first = g_text + i;
        if (hits == 2) break;
    }
    return hits;
}

void findText() {
    uint8_t* b = base();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 6) == 0) {
            g_text = b + sec[i].VirtualAddress;
            g_textSize = sec[i].Misc.VirtualSize;
        }
    }
}

const Row* find(const char* name, size_t* index) {
    for (size_t i = 0; i < kCount; ++i)
        if (std::strcmp(kRows[i].name, name) == 0) {
            *index = i;
            return &kRows[i];
        }
    return nullptr;
}

// A global through its anchor function's rip-relative operand.
uint8_t* viaAnchor(const Row& r) {
    size_t ai;
    if (!r.anchor[0] || !find(r.anchor, &ai) || !g_slots[ai].addr) return nullptr;
    const uint8_t* f = g_slots[ai].addr;
    int32_t disp;
    std::memcpy(&disp, f + r.dispOffset, sizeof(disp));
    return const_cast<uint8_t*>(f) + r.endOffset + disp;
}

void resolveAll() {
    g_recognised = gameaddr::recognised();
    const bool report = !g_recognised && !gameaddr::dx9();  // other build: only say where things went
    if (g_recognised || report) findText();
    uint8_t* b = base();
    int ok = 0, moved = 0, bad = 0, anchored = 0, byStamp = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const Row& r = kRows[i];
        Slot& s = g_slots[i];
        if (r.kind != 'f') continue;
        if (!g_recognised && !report) continue;
        Pattern p = parse(r.sig);
        if (g_recognised && matchAt(b + r.rva, p)) {
            s.addr = b + r.rva;
            s.state = State::Ok;
            ++ok;
            continue;
        }
        uint8_t* found = nullptr;
        int hits = scan(p, &found);
        if (hits == 1) {
            LOG("Game address %s: 0x%x -> 0x%llx (by signature)", r.name, r.rva,
                static_cast<unsigned long long>(found - b));
            s.state = State::Moved;
            ++moved;
            if (g_recognised) s.addr = found;  // the bytes verify, only the rva moved
        } else {
            LOG("Game address %s: %s (table rva 0x%x)", r.name, hits ? "signature not unique" : "not found", r.rva);
            s.state = hits ? State::Ambiguous : State::Unresolved;
            ++bad;
        }
    }
    for (size_t i = 0; i < kCount; ++i) {
        const Row& r = kRows[i];
        Slot& s = g_slots[i];
        if (r.kind != 'g') continue;
        if (!g_recognised) continue;  // another build: globals could have moved or changed, none is used
        uint8_t* via = viaAnchor(r);
        if (via && via != b + r.rva) {
            LOG("Game address %s: table 0x%x, anchor %s gives 0x%llx; using the anchor", r.name, r.rva, r.anchor,
                static_cast<unsigned long long>(via - b));
            s.addr = via;
            s.state = State::Moved;
            ++moved;
        } else {
            s.addr = b + r.rva;
            s.state = State::Ok;
            via ? ++anchored : ++byStamp;
        }
    }
    if (g_recognised)
        LOG("Game addresses (%s): %u entries; functions: %d at their table rva, %d found by signature, %d unresolved; "
            "globals: %d confirmed through an anchor, %d by exe stamp only",
            kGameVersion, static_cast<unsigned>(kCount), ok, moved, bad, anchored, byStamp);
    else if (report)
        LOG("Game addresses: exe stamp is not the table's (%08x): %d functions found by signature, %d not (lines above). "
            "Nothing is used until the table is remapped (re/exe/README.md, After a game patch).",
            kTableStamp, moved, bad);
}

void init() { std::call_once(g_once, resolveAll); }

uint8_t* get(const char* name, char kind) {
    init();
    size_t i;
    const Row* r = find(name, &i);
    if (!r || r->kind != kind) {
        LOG("Game address %s: not in the table (code bug)", name);
        return nullptr;
    }
    return g_slots[i].addr;
}

}  // namespace

uint8_t* fn(const char* name) { return get(name, 'f'); }
uint8_t* global(const char* name) { return get(name, 'g'); }
uint32_t tableStamp() { return kTableStamp; }
const char* gameVersion() { return kGameVersion; }

}  // namespace s4vr::addrs
