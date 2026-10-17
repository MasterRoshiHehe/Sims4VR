#include "strings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "log.h"

// The parts that don't touch Windows (inflate, RefPack, STBL, JSON and the
// token formatting) build on Linux too, so they can be tested there.
namespace s4vr::strings {
namespace {

constexpr uint32_t kStbl = 0x220557DA;

// ---------------------------------------------------------------------------
// zlib inflate (RFC 1950/1951): stored, fixed and dynamic Huffman blocks.
// Written for this file, in the style of zlib's puff.c. The Adler checksum
// isn't checked.

struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t buf = 0;
    int count = 0;
    bool bad = false;

    int bits(int n) {
        while (count < n) {
            if (p >= end) {
                bad = true;
                return 0;
            }
            buf |= static_cast<uint32_t>(*p++) << count;
            count += 8;
        }
        int v = static_cast<int>(buf & ((1u << n) - 1));
        buf >>= n;
        count -= n;
        return v;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

// Canonical Huffman table from code lengths. False on a broken set.
bool buildHuffman(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;  // no codes (allowed for an unused distance table)
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left = (left << 1) - h.count[len];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
}

int decodeSymbol(BitReader& in, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= in.bits(1);
        if (in.bad) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateCodes(BitReader& in, std::vector<uint8_t>& out, const Huffman& lengths, const Huffman& dists) {
    for (;;) {
        int sym = decodeSymbol(in, lengths);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLengthBase[sym] + in.bits(kLengthExtra[sym]);
            int d = decodeSymbol(in, dists);
            if (d < 0 || d >= 30) return false;
            size_t dist = kDistBase[d] + in.bits(kDistExtra[d]);
            if (in.bad || dist > out.size()) return false;
            size_t from = out.size() - dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

// zlib data (2-byte header, deflate blocks) to out. False when broken.
bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t expected) {
    if (size < 2 || (data[0] & 0x0F) != 8 || (data[1] & 0x20)) return false;
    out.clear();
    out.reserve(expected);
    BitReader in{data + 2, data + size};
    int last;
    do {
        last = in.bits(1);
        int type = in.bits(2);
        if (in.bad) return false;
        if (type == 0) {
            in.buf = 0;
            in.count = 0;
            if (in.end - in.p < 4) return false;
            unsigned len = in.p[0] | (in.p[1] << 8);
            unsigned nlen = in.p[2] | (in.p[3] << 8);
            in.p += 4;
            if (len != (~nlen & 0xFFFF) || static_cast<size_t>(in.end - in.p) < len) return false;
            out.insert(out.end(), in.p, in.p + len);
            in.p += len;
        } else if (type == 1) {
            static Huffman fixedLengths, fixedDists;
            static bool built = [] {
                uint8_t l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                buildHuffman(fixedLengths, l, 288);
                for (i = 0; i < 30; ++i) l[i] = 5;
                buildHuffman(fixedDists, l, 30);
                return true;
            }();
            (void)built;
            if (!inflateCodes(in, out, fixedLengths, fixedDists)) return false;
        } else if (type == 2) {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = in.bits(5) + 257, ndist = in.bits(5) + 1, ncode = in.bits(4) + 4;
            if (nlen > 286 || ndist > 30) return false;
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(in.bits(3));
            Huffman codes, lens, dists;
            if (in.bad || !buildHuffman(codes, lengths, 19)) return false;
            int at = 0;
            while (at < nlen + ndist) {
                int sym = decodeSymbol(in, codes);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[at++] = static_cast<uint8_t>(sym);
                    continue;
                }
                uint8_t value = 0;
                int repeat;
                if (sym == 16) {
                    if (at == 0) return false;
                    value = lengths[at - 1];
                    repeat = 3 + in.bits(2);
                } else if (sym == 17) {
                    repeat = 3 + in.bits(3);
                } else {
                    repeat = 11 + in.bits(7);
                }
                if (in.bad || at + repeat > nlen + ndist) return false;
                while (repeat--) lengths[at++] = value;
            }
            if (!buildHuffman(lens, lengths, nlen) || !buildHuffman(dists, lengths + nlen, ndist)) return false;
            if (!inflateCodes(in, out, lens, dists)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

// ---------------------------------------------------------------------------
// EA's RefPack (QFS), as _refpack in scriptmod/sims4vr/objmenu.py.
bool refpack(const uint8_t* data, size_t n, std::vector<uint8_t>& out, size_t expected) {
    out.clear();
    out.reserve(expected);
    if (n < 2) return false;
    uint8_t flags = data[0];
    size_t at = 2 + ((flags & 0x80) ? 4 : 3);
    if (flags & 0x01) at += (flags & 0x80) ? 4 : 3;
    while (at < n) {
        uint8_t b0 = data[at];
        size_t plain, copy = 0, offset = 0;
        if (b0 < 0x80) {
            if (at + 2 > n) return false;
            uint8_t b1 = data[at + 1];
            plain = b0 & 3;
            copy = ((b0 >> 2) & 7) + 3;
            offset = ((b0 & 0x60) << 3) + b1 + 1;
            at += 2;
        } else if (b0 < 0xC0) {
            if (at + 3 > n) return false;
            uint8_t b1 = data[at + 1], b2 = data[at + 2];
            plain = b1 >> 6;
            copy = (b0 & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
            at += 3;
        } else if (b0 < 0xE0) {
            if (at + 4 > n) return false;
            uint8_t b1 = data[at + 1], b2 = data[at + 2], b3 = data[at + 3];
            plain = b0 & 3;
            copy = ((b0 & 0x0C) << 6) + b3 + 5;
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
            at += 4;
        } else if (b0 < 0xFC) {
            plain = ((b0 & 0x1F) << 2) + 4;
            at += 1;
        } else {
            plain = b0 & 3;
            at += 1;
        }
        if (at + plain > n) return false;
        out.insert(out.end(), data + at, data + at + plain);
        at += plain;
        if (copy) {
            if (offset > out.size()) return false;
            size_t from = out.size() - offset;
            for (size_t i = 0; i < copy; ++i) out.push_back(out[from + i]);
        }
        if (b0 >= 0xFC) break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The string table: every string's bytes in one block, found by key hash.

struct Table {
    std::string text;                                            // all strings back to back
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> at;  // hash -> (offset, length)

    void put(uint32_t hash, const char* s, uint32_t len) {
        at[hash] = {static_cast<uint32_t>(text.size()), len};
        text.append(s, len);
    }
    bool get(uint32_t hash, std::string& out) const {
        auto it = at.find(hash);
        if (it == at.end()) return false;
        out.assign(text, it->second.first, it->second.second);
        return true;
    }
};

// One unpacked STBL into the table (later ones win). The number of strings.
size_t addStbl(const uint8_t* d, size_t n, Table& table) {
    if (n < 21 || std::memcmp(d, "STBL", 4) != 0) return 0;
    uint64_t count;
    std::memcpy(&count, d + 7, 8);
    size_t at = 21, added = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (at + 7 > n) break;
        uint32_t hash;
        uint16_t len;
        std::memcpy(&hash, d + at, 4);
        std::memcpy(&len, d + at + 5, 2);
        at += 7;
        if (at + len > n) break;
        table.put(hash, reinterpret_cast<const char*>(d + at), len);
        at += len;
        ++added;
    }
    return added;
}

// One package resource, unpacked. False when broken or of unknown compression.
bool unpack(uint16_t compression, std::vector<uint8_t>& stored, uint32_t size, std::vector<uint8_t>& out) {
    if (compression == 0) {
        out.swap(stored);
        return true;
    }
    if (compression == 0x5A42) return inflate(stored.data(), stored.size(), out, size);
    if (compression == 0xFFFF) return refpack(stored.data(), stored.size(), out, size);
    return false;
}

// ---------------------------------------------------------------------------
// Package files (DBPF 2.1), read the same way as modelbox.cpp.

#ifdef _WIN32
using Path = std::wstring;
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit File(const Path& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    ~File() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    bool readAt(uint64_t at, void* out, uint32_t size) {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        return ReadFile(h, out, size, &got, &o) && got == size;
    }
};
#else
using Path = std::string;
struct File {
    FILE* f = nullptr;
    explicit File(const Path& path) { f = std::fopen(path.c_str(), "rb"); }
    ~File() {
        if (f) std::fclose(f);
    }
    bool ok() const { return f != nullptr; }
    bool readAt(uint64_t at, void* out, uint32_t size) {
        return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
    }
};
#endif

struct Entry {
    uint64_t position;
    uint32_t stored;  // bytes in the package
    uint32_t size;    // bytes unpacked
    uint16_t compression;
};

// Every entry in one package's index: fn(type, instance, entry).
template <class Fn>
void forEachEntry(File& f, Fn&& fn) {
    uint8_t header[96];
    if (!f.readAt(0, header, sizeof(header)) || std::memcmp(header, "DBPF", 4) != 0) return;
    uint32_t count, size, low;
    uint64_t position;
    std::memcpy(&count, header + 36, 4);
    std::memcpy(&low, header + 40, 4);
    std::memcpy(&size, header + 44, 4);
    std::memcpy(&position, header + 64, 8);
    if (!position) position = low;
    if (!count || size < 4 || size >= (512u << 20)) return;
    std::vector<uint8_t> index(size);
    if (!f.readAt(position, index.data(), size)) return;
    const uint8_t* p = index.data();
    const uint8_t* end = p + size;
    auto u32 = [&](uint32_t& v) {
        if (p + 4 > end) return false;
        std::memcpy(&v, p, 4);
        p += 4;
        return true;
    };
    uint32_t flags = 0, constant[4] = {};
    u32(flags);
    for (int i = 0; i < 4; ++i)
        if (flags & (1u << i)) u32(constant[i]);
    for (uint32_t e = 0; e < count; ++e) {
        uint32_t field[4], at, stored, unpacked;
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            if (flags & (1u << i))
                field[i] = constant[i];
            else
                ok = u32(field[i]);
        }
        ok = ok && u32(at) && u32(stored) && u32(unpacked);
        uint16_t compression = 0;
        if (ok && (stored & 0x80000000u)) {
            if (p + 4 > end) break;
            std::memcpy(&compression, p, 2);
            p += 4;
        }
        if (!ok) break;
        fn(field[0], (static_cast<uint64_t>(field[2]) << 32) | field[3],
           Entry{at, stored & 0x7FFFFFFFu, unpacked, compression});
    }
}

// The string tables in one package into the table. englishOnly: only those
// whose instance's top byte is 0 (a mod's other languages are skipped).
// The number of strings read.
size_t loadPackage(const Path& path, bool englishOnly, Table& table) {
    File f(path);
    if (!f.ok()) return 0;
    std::vector<Entry> stbls;
    forEachEntry(f, [&](uint32_t type, uint64_t instance, const Entry& e) {
        if (type != kStbl || e.compression == 0xFFE0) return;  // 0xFFE0: deleted
        if (englishOnly && (instance >> 56) != 0) return;
        if (e.stored == 0 || e.stored > (256u << 20) || e.size > (256u << 20)) return;
        stbls.push_back(e);
    });
    size_t added = 0;
    std::vector<uint8_t> stored, data;
    for (const Entry& e : stbls) {
        stored.resize(e.stored);
        if (!f.readAt(e.position, stored.data(), e.stored)) continue;
        if (!unpack(e.compression, stored, e.size, data)) continue;
        added += addStbl(data.data(), data.size(), table);
    }
    return added;
}

// ---------------------------------------------------------------------------
// A small JSON reader: objects, arrays, strings, numbers, true/false/null.

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;                           // Array
    std::vector<std::pair<std::string, Json>> fields;  // Object

    const Json* get(const char* key) const {
        if (kind != Object) return nullptr;
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string str(const char* key) const {
        const Json* v = get(key);
        return v && v->kind == String ? v->s : std::string();
    }
    double num(const char* key, double fallback = 0) const {
        const Json* v = get(key);
        return v && v->kind == Number ? v->n : fallback;
    }
    bool flag(const char* key) const {
        const Json* v = get(key);
        return v && ((v->kind == Bool && v->b) || (v->kind == Number && v->n != 0));
    }
};

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

struct JsonReader {
    const char* p;
    const char* end;
    int depth = 0;

    void space() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool word(const char* w) {
        size_t n = std::strlen(w);
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, w, n) != 0) return false;
        p += n;
        return true;
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= c - '0';
            else if (c >= 'a' && c <= 'f')
                v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v |= c - 'A' + 10;
            else
                return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end) return false;
            char c = *p++;
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t u;
                    if (!hex4(u)) return false;
                    if (u >= 0xD800 && u < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t low;
                        if (!hex4(low)) return false;
                        if (low >= 0xDC00 && low < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, u);
                    break;
                }
                default: out += c; break;  // \" \\ \/
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    // Numbers by hand: strtod would follow the C locale's decimal mark.
    bool number(double& out) {
        const char* start = p;
        bool negative = false;
        if (p < end && *p == '-') negative = true, ++p;
        double v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (p < end && *p == '.') {
            ++p;
            double scale = 0.1;
            while (p < end && *p >= '0' && *p <= '9') v += (*p++ - '0') * scale, scale *= 0.1;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            bool down = false;
            if (p < end && (*p == '+' || *p == '-')) down = *p++ == '-';
            int e = 0;
            while (p < end && *p >= '0' && *p <= '9') e = std::min(e * 10 + (*p++ - '0'), 400);
            while (e--) v = down ? v / 10 : v * 10;
        }
        out = negative ? -v : v;
        return p > start + (negative ? 1 : 0);
    }
    bool value(Json& out) {
        space();
        if (p >= end || ++depth > 64) return false;
        bool ok = true;
        if (*p == '{') {
            out.kind = Json::Object;
            ++p;
            space();
            if (p < end && *p == '}') {
                ++p;
            } else {
                for (;;) {
                    space();
                    std::pair<std::string, Json> field;
                    if (!string(field.first)) return false;
                    space();
                    if (p >= end || *p++ != ':') return false;
                    if (!value(field.second)) return false;
                    out.fields.push_back(std::move(field));
                    space();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        ++p;
                        break;
                    }
                    return false;
                }
            }
        } else if (*p == '[') {
            out.kind = Json::Array;
            ++p;
            space();
            if (p < end && *p == ']') {
                ++p;
            } else {
                for (;;) {
                    out.items.emplace_back();
                    if (!value(out.items.back())) return false;
                    space();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        ++p;
                        break;
                    }
                    return false;
                }
            }
        } else if (*p == '"') {
            out.kind = Json::String;
            ok = string(out.s);
        } else if (word("true")) {
            out.kind = Json::Bool;
            out.b = true;
        } else if (word("false")) {
            out.kind = Json::Bool;
        } else if (word("null")) {
            out.kind = Json::Null;
        } else {
            out.kind = Json::Number;
            ok = number(out.n);
        }
        --depth;
        return ok;
    }
};

bool parseJson(const std::string& text, Json& out) {
    JsonReader r{text.data(), text.data() + text.size()};
    return r.value(out);
}

// ---------------------------------------------------------------------------
// Filling in a string's tokens.

std::atomic<const Table*> g_table{nullptr};

std::string format(const Json& loc, int depth);

// The string with this key, its markup and tokens left as they are.
std::string lookup(uint32_t hash) {
    std::string s;
    const Table* t = g_table.load(std::memory_order_acquire);
    if (t && hash) t->get(hash, s);
    return s;
}

uint32_t keyOf(const Json& j, const char* key) { return static_cast<uint32_t>(static_cast<uint64_t>(j.num(key))); }

std::string withCommas(const std::string& digits) {
    std::string out;
    int n = static_cast<int>(digits.size());
    for (int i = 0; i < n; ++i) {
        out += digits[i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1) out += ',';
    }
    return out;
}

// A whole number without decimals, else up to 2 decimals.
std::string numberText(double v, bool commas) {
    bool negative = v < 0;
    if (negative) v = -v;
    uint64_t cents = static_cast<uint64_t>(v * 100 + 0.5);
    std::string out = std::to_string(cents / 100);
    if (commas) out = withCommas(out);
    unsigned frac = static_cast<unsigned>(cents % 100);
    if (frac) {
        out += '.';
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10) out += static_cast<char>('0' + frac % 10);
    }
    return negative ? "-" + out : out;
}

std::string keyText(uint32_t hash);

// First and last name; a special Sim's name by its string key.
std::string simFullName(const Json& t) {
    std::string first = t.str("first_name"), last = t.str("last_name");
    if (first.empty() && last.empty()) return keyText(keyOf(t, "full_name_key"));
    if (first.empty()) return last;
    if (last.empty()) return first;
    return first + " " + last;
}

// A string key's text, its tokens left empty (catalog names and the like).
std::string keyText(uint32_t hash) {
    if (!hash) return "";
    Json loc;
    loc.kind = Json::Object;
    Json h;
    h.kind = Json::Number;
    h.n = hash;
    loc.fields.emplace_back("hash", h);
    return format(loc, 1);
}

std::string objectName(const Json& t) {
    std::string s = t.str("custom_name");
    return s.empty() ? keyText(keyOf(t, "catalog_name_key")) : s;
}

std::string objectDescription(const Json& t) {
    std::string s = t.str("custom_description");
    return s.empty() ? keyText(keyOf(t, "catalog_description_key")) : s;
}

const Json* simList(const Json& t) {
    const Json* list = t.get("sim_list");
    return list && list->kind == Json::Array ? list : nullptr;
}

// "A", "A and B", "A, B and C".
std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

// What a token shows as {n.String}: the text it stands for, whatever its type.
std::string tokenText(const Json& t, int depth) {
    std::string type = t.str("type");
    if (type == "SIM" || t.get("first_name") || t.get("last_name") || t.get("full_name_key")) return simFullName(t);
    if (type == "STRING" || t.get("text_string")) {
        const Json* inner = t.get("text_string");
        return inner ? format(*inner, depth + 1) : "";
    }
    if (type == "RAW_TEXT" || t.get("raw_text")) return t.str("raw_text");
    if (type == "NUMBER" || t.get("number")) return numberText(t.num("number"), false);
    if (type == "OBJECT" || t.get("custom_name") || t.get("catalog_name_key")) return objectName(t);
    if (const Json* list = simList(t)) {
        std::vector<std::string> names;
        for (const Json& s : list->items) names.push_back(tokenText(s, depth));
        return joinNames(names);
    }
    return "";
}

// ASCII only; other letters are left as they are.
void upperFirst(std::string& s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
}

bool endsWith(const std::string& s, const char* tail) {
    size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// The text for {n.form}; false for forms not known (shown as nothing).
bool formText(const Json* t, const std::string& form, int depth, std::string& out) {
    static const Json none;
    const Json& tok = t ? *t : none;
    bool female = tok.flag("is_female");
    if (form == "SimFirstName" || form == "SimFirstNameWithPrefix") {
        out = tok.get("first_name") ? tok.str("first_name") : tokenText(tok, depth);
    } else if (form == "SimLastName") {
        out = tok.get("last_name") ? tok.str("last_name") : tokenText(tok, depth);
    } else if (form == "SimName" || form == "SimNameWithPrefix") {
        out = tokenText(tok, depth);
    } else if (form == "SimPronounSubjective") {
        out = female ? "she" : "he";
    } else if (form == "SimPronounObjective") {
        out = female ? "her" : "him";
    } else if (form == "SimPronounPossessiveDependent") {
        out = female ? "her" : "his";
    } else if (form == "SimPronounPossessiveIndependent") {
        out = female ? "hers" : "his";
    } else if (form == "SimPronounReflexive") {
        out = female ? "herself" : "himself";
    } else if (form == "SimPronouns") {
        out = female ? "she/her" : "he/him";
    } else if (form == "SimFirstNameList" || form == "SimNameList") {
        std::vector<std::string> names;
        if (const Json* list = simList(tok))
            for (const Json& s : list->items)
                names.push_back(form == "SimNameList" ? simFullName(s) : s.get("first_name") ? s.str("first_name")
                                                                                             : tokenText(s, depth));
        out = joinNames(names);
    } else if (form == "String") {
        out = tokenText(tok, depth);
    } else if (form == "Number") {
        out = tok.get("number") ? numberText(tok.num("number"), false) : tokenText(tok, depth);
    } else if (form == "Money" || form == "Currency") {
        out = "\xC2\xA7" + numberText(tok.num("number"), true);  // §
    } else if (endsWith(form, "Points") || form == "Influence" || form == "RecyclingBits" ||
               form == "RecyclingPieces" || form == "GalacticCredits") {
        out = numberText(tok.num("number"), true);
    } else if (form == "ObjectName" || form == "ObjectCatalogName") {
        out = tok.get("custom_name") || tok.get("catalog_name_key") ? objectName(tok) : tokenText(tok, depth);
    } else if (form == "ObjectDescription" || form == "ObjectCatalogDescription") {
        out = objectDescription(tok);
    } else {
        return false;  // times, dates, timespans: not sent by Python
    }
    return true;
}

void applyModifier(std::string& s, const std::string& mod) {
    if (mod == "xxUpper") {
        upperFirst(s);
    } else if (mod == "enAn") {
        if (s.empty()) return;
        char c = static_cast<char>(s[0] | 0x20);
        bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
        s = (vowel ? "an " : "a ") + s;
    } else if (mod == "enHouseholdNamePlural") {
        if (s.empty()) return;
        if (endsWith(s, "s") || endsWith(s, "x") || endsWith(s, "z") || endsWith(s, "ch") || endsWith(s, "sh"))
            s += "es";
        else
            s += "s";
    }
}

// Is token n "one": its number is 1, or its list has one item.
bool isSingular(const Json* t) {
    if (!t) return true;
    if (t->get("number")) return t->num("number") == 1;
    if (const Json* list = simList(*t)) return list->items.size() == 1;
    return true;
}

// The {...} at text[open] split up: prefix letters, token index, the rest after
// the dot. False when it isn't a token at all (kept as written).
struct Brace {
    std::string prefix;
    int index = 0;
    std::string rest;
    size_t close = 0;
};
bool parseBrace(const std::string& text, size_t open, Brace& b) {
    // The matching }: the text of {S0.{S1.ObjectName}} holds a token too.
    size_t close = std::string::npos;
    int nest = 0;
    for (size_t k = open + 1; k < text.size(); ++k) {
        if (text[k] == '{') {
            ++nest;
        } else if (text[k] == '}') {
            if (nest == 0) {
                close = k;
                break;
            }
            --nest;
        }
    }
    if (close == std::string::npos) return false;
    size_t i = open + 1;
    while (i < close && text[i] >= 'A' && text[i] <= 'Z') b.prefix += text[i++];
    bool digits = false;
    int index = 0;
    while (i < close && text[i] >= '0' && text[i] <= '9') index = std::min(index * 10 + (text[i++] - '0'), 1000), digits = true;
    if (i >= close || text[i] != '.') {
        // {0}: the token itself.
        if (b.prefix.empty() && digits && i == close) {
            b.index = index;
            b.rest = "String";
            b.close = close;
            return true;
        }
        return false;
    }
    if (!digits && b.prefix.empty()) return false;
    b.index = index;
    b.rest = text.substr(i + 1, close - i - 1);
    b.close = close;
    return true;
}

// Markup out: tags dropped, <br> and </li> to new lines, <li> to a bullet.
// The two characters \n are a new line too.
std::string cleanMarkup(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'n') {
            out += '\n';
            ++i;
            continue;
        }
        if (c == '<' && i + 1 < in.size()) {
            char next = in[i + 1];
            bool tagLike = next == '/' || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z');
            size_t close = tagLike ? in.find('>', i + 1) : std::string::npos;
            size_t nextOpen = in.find('<', i + 1);
            if (close != std::string::npos && (nextOpen == std::string::npos || nextOpen > close)) {
                std::string name;
                for (size_t k = i + 1; k < close && in[k] != ' ' && in[k] != '>'; ++k)
                    name += static_cast<char>(in[k] >= 'A' && in[k] <= 'Z' ? in[k] + 32 : in[k]);
                if (name == "br" || name == "br/" || name == "/li")
                    out += '\n';
                else if ((name == "ul" || name == "ol" || name == "p" || name == "/p") && !out.empty() &&
                         out.back() != '\n')
                    out += '\n';  // a list or paragraph starts on its own line
                else if (name == "li")
                    out += "\xE2\x80\xA2 ";  // •
                i = close;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// The string's tokens filled in (markup left in).
std::string fill(const std::string& text, const Json* tokens, int depth) {
    auto token = [&](int n) -> const Json* {
        if (!tokens || tokens->kind != Json::Array || n < 0 || n >= static_cast<int>(tokens->items.size()))
            return nullptr;
        return &tokens->items[n];
    };
    std::string out;
    out.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        Brace b;
        if (text[i] != '{' || !parseBrace(text, i, b)) {
            out += text[i];
            continue;
        }
        i = b.close;
        const Json* t = token(b.index);
        const std::string& p = b.prefix;
        std::string form = b.rest, mod, s;
        size_t bar = form.find('|');
        if (bar != std::string::npos) {
            mod = form.substr(bar + 1);
            form.resize(bar);
        }
        if (p.empty()) {
            if (formText(t, form, depth, s)) {
                applyModifier(s, mod);
                out += s;
            }
            continue;
        }
        // {S1.ObjectName}: the singular (or P plural) form of a token's text.
        // We have one name for both.
        if ((p == "S" || p == "P") && !form.empty() && form[0] >= 'A' && form[0] <= 'Z' &&
            form.find(' ') == std::string::npos && formText(t, form, depth, s)) {
            applyModifier(s, mod);
            out += s;
            continue;
        }
        bool show;
        if (p == "M")
            show = !(t && t->flag("is_female"));
        else if (p == "F")
            show = t && t->flag("is_female");
        else if (p == "S")
            show = isSingular(t);
        else if (p == "P")
            show = !isSingular(t);
        else if (p == "N" || p == "U")
            show = false;  // they/them forms: Python only sends is_female
        else {
            // Other conditions (T teen, DAE adult, ...) we can't tell: of a run
            // like {T0.Mess Around}{DAE0.WooHoo} show only the last one.
            Brace next;
            show = !(b.close + 1 < text.size() && text[b.close + 1] == '{' && parseBrace(text, b.close + 1, next) &&
                     !next.prefix.empty());
        }
        if (show) out += depth < 8 ? fill(b.rest, tokens, depth + 1) : b.rest;
    }
    return out;
}

// A LocalizedString object to text.
std::string format(const Json& loc, int depth) {
    if (depth > 8 || loc.kind != Json::Object) return "";
    if (const Json* raw = loc.get("raw")) return raw->kind == Json::String ? raw->s : "";
    std::string text = lookup(keyOf(loc, "hash"));
    if (text.empty()) return "";
    return cleanMarkup(fill(text, loc.get("tokens"), depth));
}

// ---------------------------------------------------------------------------
// Loading: the game's packages, then the mods' (later ones win).

void load(const std::vector<Path>& game, const std::vector<Path>& mods) {
    auto start = std::chrono::steady_clock::now();
    auto* table = new Table();
    table->at.reserve(1 << 19);
    table->text.reserve(48u << 20);
    size_t gameStrings = 0, modStrings = 0, modsWithStrings = 0;
    for (const Path& p : game) gameStrings += loadPackage(p, false, *table);
    for (const Path& p : mods) {
        size_t n = loadPackage(p, true, *table);
        modStrings += n;
        if (n) ++modsWithStrings;
    }
    table->text.shrink_to_fit();
    g_table.store(table, std::memory_order_release);  // never freed: kept for the session
    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    LOG("strings: %zu strings (%zu from %zu game packages, %zu from %zu of %zu mod packages), %zu MB, %lld ms",
        table->at.size(), gameStrings, game.size(), modStrings, modsWithStrings, mods.size(),
        table->text.size() >> 20, ms);
}

#ifdef _WIN32
std::atomic<bool> g_started{false};

// The game folder: the exe is in <root>\Game\Bin (or Game-cracked etc.), so two up.
std::wstring gameRoot() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    for (int i = 0; i < 3; ++i) {
        size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return L"";
        p.resize(cut);
    }
    return p;
}

// Strings_ENG_US.package files under dir, a few folders deep (Data\Client,
// EPxx, GPxx, Delta\EPxx ...).
void findStringPackages(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && _wcsicmp(name.c_str(), L"Game") != 0 && _wcsicmp(name.c_str(), L"__Installer") != 0 &&
                _wcsicmp(name.c_str(), L"Soundtrack") != 0 && _wcsicmp(name.c_str(), L"Support") != 0)
                findStringPackages(path, depth - 1, out);
            continue;
        }
        if (_wcsicmp(name.c_str(), L"Strings_ENG_US.package") == 0) out.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Every .package under dir, a few folders deep (the Mods folder).
void findModPackages(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) findModPackages(path, depth - 1, out);
            continue;
        }
        size_t n = name.size();
        if (n >= 8 && _wcsicmp(name.c_str() + n - 8, L".package") == 0) out.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The Mods folder: this DLL sits in Mods\sims4vr.
std::wstring modsFolder() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&modsFolder), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    for (int i = 0; i < 2; ++i) {
        size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return L"";
        p.resize(cut);
    }
    return p;
}

bool isDelta(const std::wstring& path) {
    std::wstring lower = path;
    for (auto& c : lower) c = towlower(c);
    return lower.find(L"\\delta\\") != std::wstring::npos;
}

void loadAll() {
    std::vector<std::wstring> game, mods;
    std::wstring root = gameRoot();
    if (!root.empty()) findStringPackages(root, 3, game);
    // Full builds first, then the delta builds that patch them.
    std::stable_partition(game.begin(), game.end(), [](const std::wstring& p) { return !isDelta(p); });
    std::wstring modsDir = modsFolder();
    if (!modsDir.empty()) findModPackages(modsDir, 5, mods);
    load(game, mods);
}
#endif

}  // namespace

void start() {
#ifdef _WIN32
    if (g_started.exchange(true)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        loadAll();
    }).detach();
#endif
}

bool ready() { return g_table.load(std::memory_order_acquire) != nullptr; }

std::string resolve(const std::string& json) {
    Json j;
    if (!parseJson(json, j)) return "";
    return format(j, 0);
}

}  // namespace s4vr::strings
