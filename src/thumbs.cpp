#include "thumbs.h"

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gameaddr.h"
#include "hud.h"
#include "log.h"

namespace s4vr::thumbs {

namespace {

// The image loader's thumbnail fetch (game 1.128.90.1030): its "img://"
// handler (0xe115f0) sends "thumbs/" URLs to 0xe13600, which calls this with
// (unused, the whole URL as a Scaleform::String, the URL after
// "img://thumbs/", a callback, its context). It parses the URL (0xdec540),
// asks the ThumbnailSystem (0x18b2210) and returns the ThumbnailImage with a
// reference for the caller, or null.
constexpr uint32_t kFetchRva = 0xe0f250;
constexpr uint8_t kFetchBytes[16] = {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x56, 0x41, 0x57,
                                     0x48, 0x8d, 0x68, 0xa9, 0x48, 0x81, 0xec, 0xc0};
// The callback, once the picture is there (status 0) or not (1): (image,
// request, status); the request holds the context at +0x20 (the loader's own
// callback 0xe15860 reads it there). It may come at once, inside the fetch.
using Callback = void (*)(uint8_t* image, uint8_t* request, int status);
using FetchFn = uint8_t* (*)(void* unused, const uint64_t* url, const char* path, Callback callback, void* context);
// EA::Sims4::Thumbnails::ThumbnailImage (vtable 0x229e158): slot 1 releases a
// reference. Its pixels (0x18b9710 copies them into the UI's texture): +0x18
// the data, +0x20 width, +0x24 height (u32), +0x28 bytes a row (u64), +0x30
// the format: 0x3d/0x3e R8G8B8A8 (Scaleform Image_R8G8B8A8, 0x18a23b0),
// 0x3f/0x40 the same with no alpha (the game sets it opaque).
constexpr size_t kPixels = 0x18, kWidth = 0x20, kHeight = 0x24, kPitch = 0x28, kFormat = 0x30, kContext = 0x20;
constexpr ULONGLONG kRetryMs = 20000;

enum State { kAsked, kDone, kFailed };
struct Entry {
    State state = kAsked;
    uint8_t* image = nullptr;  // our reference while asked
    ULONGLONG at = 0;
};
std::mutex g_mutex;
std::unordered_map<uint64_t, Entry> g_entries;  // g_mutex
FetchFn g_fetch = nullptr;
bool g_looked = false;
int g_logs = 0;

void release(uint8_t* image) {
    using ReleaseFn = void (*)(uint8_t*);
    if (image) reinterpret_cast<ReleaseFn*>(*reinterpret_cast<void**>(image))[1](image);
}

// The game's callback (whatever thread it calls on): the pixels to the HUD.
void onPicture(uint8_t* image, uint8_t* request, int status) {
    const uint64_t id = reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(request + kContext));
    bool ok = false;
    uint32_t w = 0, h = 0;
    uint8_t format = 0;
    if (status == 0 && image) {
        const uint8_t* data = *reinterpret_cast<uint8_t**>(image + kPixels);
        std::memcpy(&w, image + kWidth, 4);
        std::memcpy(&h, image + kHeight, 4);
        uint64_t pitch = 0;
        std::memcpy(&pitch, image + kPitch, 8);
        format = image[kFormat];
        if (data && w && h && w <= 4096 && h <= 4096 && pitch >= w * 4ull && format >= 0x3d && format <= 0x40) {
            std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y) std::memcpy(&rgba[static_cast<size_t>(y) * w], data + y * pitch, w * 4);
            if (format >= 0x3f)
                for (uint32_t& c : rgba) c |= 0xff000000u;
            hud::setPicture(id, rgba.data(), static_cast<int>(w), static_cast<int>(h));
            ok = true;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_entries.find(id);
    if (it != g_entries.end()) it->second.state = ok ? kDone : kFailed;
    if (g_logs < 60) {
        ++g_logs;
        LOG("Thumbnails: picture %016llx %s (status %d, %ux%u, format %02x, thread %lu)",
            static_cast<unsigned long long>(id), ok ? "came" : "failed", status, w, h, format, GetCurrentThreadId());
    }
}

// Scaleform::String: a pointer (low 2 bits flags) to its DataDesc: size, a
// reference count, then the text at +0xc.
struct alignas(8) UrlText {
    uint64_t size;
    int32_t refs;
    char text[500];
};
static_assert(offsetof(UrlText, text) == 0xc, "Scaleform::String DataDesc");

}  // namespace

bool request(uint64_t id, const char* path) {
    if (!id || !path || !*path || std::strlen(path) > 400) return false;
    std::vector<uint8_t*> done;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_looked) {
            g_looked = true;
            g_fetch = reinterpret_cast<FetchFn>(gameaddr::function(kFetchRva, kFetchBytes));
            LOG("Thumbnails: %s", g_fetch ? "the game's thumbnail fetch found" : "game build not recognised; no pictures");
        }
        if (!g_fetch) return false;
        // Our references to pictures that came (or failed) go back here, on
        // this thread, never inside the game's callback.
        for (auto& [key, e] : g_entries)
            if (e.state != kAsked && e.image) done.push_back(e.image), e.image = nullptr;
        auto it = g_entries.find(id);
        if (it != g_entries.end()) {
            const bool retry = it->second.state == kFailed && GetTickCount64() - it->second.at > kRetryMs;
            if (!retry) {
                // (A picture the HUD dropped from its slots is asked for again.)
                if (it->second.state != kDone || hud::hasPicture(id)) {
                    for (uint8_t* image : done) release(image);
                    return true;
                }
            }
        }
        Entry& e = g_entries[id];
        if (e.image) done.push_back(e.image);
        e = Entry{kAsked, nullptr, GetTickCount64()};
    }
    for (uint8_t* image : done) release(image);
    hud::expectPicture(id);  // not an icon resource: the HUD doesn't ask Python for it
    static UrlText url;  // Python's thread only
    const int n = std::snprintf(url.text, sizeof(url.text), "img://thumbs/%s", path);
    url.size = static_cast<uint64_t>(n);
    url.refs = 1 << 20;  // never freed by the game
    const uint64_t text = reinterpret_cast<uint64_t>(&url);
    uint8_t* image = g_fetch(nullptr, &text, url.text + 13, &onPicture, reinterpret_cast<void*>(id));
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_entries[id];
    if (!image && e.state == kAsked) e.state = kFailed;
    if (image) {
        if (e.state == kAsked) {
            e.image = image;
        } else {
            release(image);  // it came at once
        }
    }
    if (g_logs < 60) {
        ++g_logs;
        LOG("Thumbnails: asked for %s as %016llx (%s, thread %lu)", url.text, static_cast<unsigned long long>(id),
            !image ? "refused" : e.state == kAsked ? "on its way" : "came at once", GetCurrentThreadId());
    }
    return true;
}

}  // namespace s4vr::thumbs
