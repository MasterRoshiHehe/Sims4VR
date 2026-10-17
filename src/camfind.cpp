#include "camfind.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "camera.h"
#include "log.h"
#include "rtti.h"

namespace s4vr::camfind {

namespace {

// Every class that could own the camera transform.
const char* const kClasses[] = {
    ".?AVCamera@Cameras@Sims3@@",
    ".?AVCameraControllerManager@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerMain@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerFPS@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerFPSImpl@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerCAS@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerPhotography@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerVideo@ClientApp@Sims4@EA@@",
    ".?AVCameraControllerService@Services@ClientApp@Sims4@EA@@",
};

constexpr size_t kObjectBytes = 0x1000;   // bytes scanned in each object
constexpr size_t kPointerBytes = 0x400;   // bytes searched for child pointers
constexpr size_t kChildBytes = 0x800;     // bytes scanned in each child object
constexpr float kTolerance = 0.02f;

// Copies memory without faulting on pages that vanish mid-read.
bool safeRead(uintptr_t address, void* out, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &read) && read == size;
}

bool near3(const float* a, const float* b) {
    return std::fabs(a[0] - b[0]) < kTolerance && std::fabs(a[1] - b[1]) < kTolerance &&
           std::fabs(a[2] - b[2]) < kTolerance;
}

bool isUnitRow(const float* r) {
    float l = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
    return std::isfinite(l) && std::fabs(l - 1.0f) < 0.01f;
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// One pass over private read-write memory, collecting objects whose first
// pointer is one of the vtables.
std::map<uintptr_t, std::vector<uintptr_t>> findInstances(const std::vector<uintptr_t>& vtables, size_t maxEach) {
    std::map<uintptr_t, std::vector<uintptr_t>> found;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<uintptr_t> chunk(1 << 17);  // 1 MB
    MEMORY_BASIC_INFORMATION mbi;
    while (address < end && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        auto regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t p = regionBase; p < regionBase + mbi.RegionSize; p += chunk.size() * 8) {
                size_t bytes = regionBase + mbi.RegionSize - p;
                if (bytes > chunk.size() * 8) bytes = chunk.size() * 8;
                if (!safeRead(p, chunk.data(), bytes)) continue;
                for (size_t i = 0; i < bytes / 8; ++i) {
                    for (uintptr_t vt : vtables) {
                        if (chunk[i] == vt && found[vt].size() < maxEach) found[vt].push_back(p + i * 8);
                    }
                }
            }
        }
        address = regionBase + mbi.RegionSize;
    }
    return found;
}

struct Scanner {
    const float* position;
    const float* target;
    std::vector<std::string> hits;

    void scan(const char* owner, uintptr_t object, size_t size) {
        std::vector<uint8_t> bytes(size);
        if (!safeRead(object, bytes.data(), size)) {
            // Shrink to what is readable in the first page.
            size = 0x1000 - (object & 0xFFF);
            if (size < 16 || !safeRead(object, bytes.data(), size)) return;
            bytes.resize(size);
        }
        char line[256];
        for (size_t off = 0; off + 12 <= size; off += 4) {
            auto* f = reinterpret_cast<const float*>(bytes.data() + off);
            if (near3(f, position)) {
                std::snprintf(line, sizeof(line), "%s %p +0x%zx position (%.3f %.3f %.3f)", owner,
                              reinterpret_cast<void*>(object), off, f[0], f[1], f[2]);
                hits.push_back(line);
            }
            if (near3(f, target)) {
                std::snprintf(line, sizeof(line), "%s %p +0x%zx target (%.3f %.3f %.3f)", owner,
                              reinterpret_cast<void*>(object), off, f[0], f[1], f[2]);
                hits.push_back(line);
            }
            // 4x4 matrix with an orthonormal 3x3 and (0,0,0,1) last column.
            if (off + 64 <= size && isUnitRow(f) && isUnitRow(f + 4) && isUnitRow(f + 8) &&
                std::fabs(dot3(f, f + 4)) < 0.01f && std::fabs(dot3(f, f + 8)) < 0.01f &&
                std::fabs(dot3(f + 4, f + 8)) < 0.01f && std::fabs(f[3]) < 1e-4f && std::fabs(f[7]) < 1e-4f &&
                std::fabs(f[11]) < 1e-4f && std::fabs(f[15] - 1.0f) < 1e-4f) {
                const float* t = f + 12;
                // View matrix (row vectors): eye = -t * R^T.
                float eye[3] = {-(t[0] * f[0] + t[1] * f[1] + t[2] * f[2]), -(t[0] * f[4] + t[1] * f[5] + t[2] * f[6]),
                                -(t[0] * f[8] + t[1] * f[9] + t[2] * f[10])};
                const char* kind = near3(t, position) ? "world matrix at camera" :
                                   near3(eye, position) ? "VIEW MATRIX of camera" : nullptr;
                if (kind) {
                    std::snprintf(line, sizeof(line), "%s %p +0x%zx %s", owner, reinterpret_cast<void*>(object), off, kind);
                    hits.push_back(line);
                }
            }
        }
    }
};


// ---------------------------------------------------------------------------
// Camera copy probe. The game copies whole camera objects (vtable, then a
// 4x4 transform at +0x30 whose last row, +0x60, is the camera position) with
// one small function. The old convexvr mod hooked the same function. Here we
// record which objects get copied where, and how often, for a few seconds.

struct CopyRecord {
    const void* src;
    void* dst;
    void* caller;
    unsigned count;
};
std::mutex g_probeMutex;
CopyRecord g_records[64];
size_t g_recordCount = 0;
ULONGLONG g_probeUntil = 0;
std::atomic<bool> g_probeReported{true};

void reportProbe();

void observeCopy(void* dst, const void* src, void* caller) {
    if (!g_probeReported) {
        std::lock_guard<std::mutex> lock(g_probeMutex);
        if (GetTickCount64() < g_probeUntil) {
            size_t i = 0;
            for (; i < g_recordCount; ++i) {
                if (g_records[i].src == src && g_records[i].dst == dst && g_records[i].caller == caller) break;
            }
            if (i < g_recordCount) {
                ++g_records[i].count;
            } else if (g_recordCount < 64) {
                g_records[g_recordCount++] = {src, dst, caller, 1};
            }
        } else {
            reportProbe();
        }
    }
}

void logFloats(const char* label, uintptr_t object, size_t from, size_t to) {
    std::vector<float> f((to - from) / 4);
    if (!safeRead(object + from, f.data(), f.size() * 4)) {
        LOG("  %s %p unreadable", label, reinterpret_cast<void*>(object));
        return;
    }
    for (size_t i = 0; i < f.size(); i += 4) {
        LOG("  %s %p +0x%03zx: %10.4f %10.4f %10.4f %10.4f", label, reinterpret_cast<void*>(object), from + i * 4, f[i],
            f[i + 1], f[i + 2], f[i + 3]);
    }
}

// Called with g_probeMutex held, once the probe time is up.
void reportProbe() {
    g_probeReported = true;
    camera::setCopyObserver(nullptr);
    HMODULE game = GetModuleHandleW(nullptr);
    auto base = reinterpret_cast<uintptr_t>(game);
    LOG("Camera copy probe: %zu distinct copies in 3 s", g_recordCount);
    for (size_t i = 0; i < g_recordCount; ++i) {
        const CopyRecord& r = g_records[i];
        LOG("  copy %p -> %p from game+0x%llx: %u times", r.src, r.dst,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r.caller) - base), r.count);
    }
    for (size_t i = 0; i < g_recordCount && i < 6; ++i) {
        logFloats("src", reinterpret_cast<uintptr_t>(g_records[i].src), 0x10, 0x80);
    }
}

void startCopyProbe() {
    if (!camera::installCopyHook()) return;
    std::lock_guard<std::mutex> lock(g_probeMutex);
    g_recordCount = 0;
    g_probeUntil = GetTickCount64() + 3000;
    g_probeReported = false;
    camera::setCopyObserver(&observeCopy);
}

}  // namespace

std::string run(const float position[3], const float target[3]) {
    HMODULE game = GetModuleHandleW(nullptr);
    LOG("Camera search: position %.3f %.3f %.3f, target %.3f %.3f %.3f", position[0], position[1], position[2],
        target[0], target[1], target[2]);

    std::vector<uintptr_t> vtables;
    std::map<uintptr_t, const char*> names;
    for (const char* cls : kClasses) {
        uintptr_t vt = rtti::findVTable(game, cls);
        LOG("  vtable %s = %p (rva 0x%llx)", cls, reinterpret_cast<void*>(vt),
            vt ? static_cast<unsigned long long>(vt - reinterpret_cast<uintptr_t>(game)) : 0ULL);
        if (vt) {
            vtables.push_back(vt);
            names[vt] = cls;
        }
    }
    auto instances = findInstances(vtables, 8);
    startCopyProbe();

    Scanner scanner{position, target, {}};
    int objects = 0;
    for (auto& [vt, list] : instances) {
        for (uintptr_t object : list) {
            ++objects;
            LOG("  instance %s at %p", names[vt], reinterpret_cast<void*>(object));
            if (std::strstr(names[vt], "CameraControllerManager")) {
                uintptr_t cams[2] = {};
                if (safeRead(object + 0x10, cams, sizeof(cams))) {
                    LOG("  manager camera +0x10 = %p, saved camera +0x18 = %p", reinterpret_cast<void*>(cams[0]),
                        reinterpret_cast<void*>(cams[1]));
                    logFloats("manager camera", cams[0], 0x10, 0x80);
                }
            }
            scanner.scan(names[vt], object, kObjectBytes);
            // One level down: fields pointing at other heap objects.
            uintptr_t pointers[kPointerBytes / 8];
            if (!safeRead(object, pointers, sizeof(pointers))) continue;
            for (size_t i = 1; i < kPointerBytes / 8; ++i) {
                uintptr_t child = pointers[i];
                if (child < 0x10000 || (child & 7) || !rtti::readable(child, 16)) continue;
                char owner[160];
                std::snprintf(owner, sizeof(owner), "%s+0x%zx ->", names[vt], i * 8);
                scanner.scan(owner, child, kChildBytes);
            }
        }
    }
    for (const std::string& hit : scanner.hits) LOG("  HIT %s", hit.c_str());
    char summary[200];
    std::snprintf(summary, sizeof(summary), "Camera search: %zu vtables, %d objects, %zu hits. Keep the camera moving for 3 s (details in sims4vr.log)",
                  vtables.size(), objects, scanner.hits.size());
    LOG("%s", summary);
    return summary;
}

}  // namespace s4vr::camfind
