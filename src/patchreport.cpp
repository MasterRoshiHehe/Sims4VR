#include "patchreport.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MinHook.h"
#include "addrs.h"
#include "gameaddr.h"
#include "log.h"
#include "rtti.h"

extern "C" {
MH_STATUS __real_MH_CreateHook(LPVOID target, LPVOID detour, LPVOID* original);
MH_STATUS __real_MH_EnableHook(LPVOID target);
}
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace s4vr::patchreport {

namespace {

struct Hook {
    const void* target;
    std::string label;
    uint32_t caller;  // rva in sims4vr.dll (sims4vr.map names it)
    MH_STATUS created;
    MH_STATUS enabled = MH_UNKNOWN;
    bool enableSeen = false;
};

// Features and the table rows each needs (from the addrs:: calls in src/). A feature is on when all are usable.
struct Feature {
    const char* name;
    const char* rows;  // space separated
};
const Feature kFeatures[] = {
    {"Main menu: VR starts when the loading screen has gone", "ScreenHidden"},
    {"Hands / arm IK (skeleton joints)", "JointCount JointIndex ModelTransform Parent PoseToJoints"},
    {"3D speech bubbles", "Anchor GetInfo RenderSystem ScreenSize SetInfo UiObject Updater"},
    {"Camera: lopsided eye (projection)", "Projection"},
    {"Camera: sound follows head / placement", "CameraUpdate DistanceUpdate Pan SoundFlags SoundInit"},
    {"Game UI access (phone, dialogs, menus layer)", "GfxGetMember GfxInvoke GfxRelease MovieInvoke UiManager UiManagerUpdate"},
    {"Doors", "Component ComponentMaskGlobal Enter Exit FindObject PortalOf"},
    {"Face hide", "JointIndex MeshDraw Palette SceneDraw SceneSkin SkinUpdate SkinVerts"},
    {"Game fixes (near fade, walls, rooms)",
     "ClearOverride CullRooms FadeValuesGlobal LotById LotManagerGlobal NearFade SetOverride TopLevel WallsUpOp ZoneGlobal"},
    {"God hands", "Component ComponentMaskGlobal JointIndex"},
    {"No dimming", "Ramp"},
    {"First person body height", "FloorSnap"},
    {"Render scale", "DisplayManagerGlobal"},
    {"Shadow reuse", "ShadowLists"},
    {"Shadows", "ConfigInt Fit"},
    {"Single view", "SceneJob SceneRender"},
    {"Thumbnails", "Fetch"},
    {"Touch / pick", "RenderSystemGlobal RequestPick SceneGlobal"},
    {"UI drawing hook", "ApplyBlend GfxByIndex Render Threading"},
    {"Walk stop", "ClearRoute Component FindObject FireComplete MasterVTable PathVTable ProxyVTable ReleaseBlocker "
                  "RoutingMaskGlobal WalkLogicVTable"},
    {"Outdoor ambience", "AudioManagerGlobal Curve Value WallDistance"},
};

std::mutex g_mutex;
std::wstring g_dir;
std::vector<std::string> g_exe;
std::vector<std::string> g_lines[3];  // rtti, probe, note
std::vector<Hook> g_hooks;
std::vector<std::pair<const void*, std::string>> g_names;
bool g_tableDone = false;
bool g_armed = false;
std::wstring g_marker;

const char* const kGroups[3] = {"rtti", "probe", "note"};

std::string rvaText(const void* p) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) || !m)
        return "unknown module";
    char buf[300];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(buf, sizeof(buf), "game+0x%llx", static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(m)));
    } else {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = std::wcsrchr(path, L'\\');
        std::snprintf(buf, sizeof(buf), "%ls+0x%llx", name ? name + 1 : path,
                      static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(m)));
    }
    return buf;
}

std::string labelFor(const void* target) {
    for (auto& n : g_names)
        if (n.first == target) return n.second;
    if (const char* t = addrs::nameAt(target)) return t;
    return "";
}

void writeFile() {
    if (g_dir.empty()) return;
    std::string out;
    char buf[600];
    out += "Sims4VR patch report (rewritten as the game runs; the same lines are in sims4vr.log)\n";
    std::snprintf(buf, sizeof(buf), "sims4vr v%s commit %s, table for game %s (stamp %08x)\n\n", S4VR_BUILD_VERSION,
                  S4VR_BUILD_COMMIT, addrs::gameVersion(), addrs::tableStamp());
    out += buf;
    out += "== Game executable ==\n";
    for (auto& l : g_exe) out += l + "\n";
    if (g_tableDone) {
        out += "\n== Address table ==\n";
        out += "name | kind | table rva | now at | used | how\n";
        for (size_t i = 0; i < addrs::rowCount(); ++i) {
            addrs::RowInfo r = addrs::row(i);
            char now[24] = "-";
            if (r.foundRva) std::snprintf(now, sizeof(now), "0x%x", r.foundRva);
            std::snprintf(buf, sizeof(buf), "%s | %s | 0x%x | %s | %s | %s%s%s%s", r.name, r.global ? "global" : "function", r.tableRva, now,
                          r.used ? "yes" : "no", r.state, r.why[0] ? " (" : "", r.why, r.why[0] ? ")" : "");
            out += buf;
            if (r.anchor[0]) out += std::string(" | anchor ") + r.anchor;
            out += "\n";
        }
        out += "\n== Features ==\n";
        for (const Feature& f : kFeatures) {
            std::string missing;
            std::string rows = f.rows;
            for (size_t s = 0; s < rows.size();) {
                size_t e = rows.find(' ', s);
                if (e == std::string::npos) e = rows.size();
                std::string name = rows.substr(s, e - s);
                s = e + 1;
                bool used = false;
                for (size_t i = 0; i < addrs::rowCount(); ++i)
                    if (name == addrs::row(i).name) used = addrs::row(i).used;
                if (!used) missing += (missing.empty() ? "" : ", ") + name;
            }
            out += std::string(missing.empty() ? "ON   " : "OFF  ") + f.name + (missing.empty() ? "" : " - missing: " + missing) + "\n";
        }
        const bool exact = gameaddr::recognised();
        out += std::string(exact ? "ON   " : "OFF  ") + "Fixed-address features (mirror capture, sim cache, texture probe, mirror camera counter)" +
               (exact ? "" : " - need the exact build") + "\n";
        out += "Pattern/RTTI features (camera copy hook, audio listener, camera finder, virtual gamepad): see 'rtti' and 'hooks'.\n";
    }
    for (int g = 0; g < 3; ++g) {
        if (g_lines[g].empty()) continue;
        out += std::string("\n== ") + (g == 0 ? "RTTI and vtable lookups" : g == 1 ? "Probes (log-only)" : "Notes") + " ==\n";
        for (auto& l : g_lines[g]) out += l + "\n";
    }
    if (!g_hooks.empty()) {
        out += "\n== MinHook ==\n";
        for (const Hook& h : g_hooks) {
            std::string label = labelFor(h.target);
            std::snprintf(buf, sizeof(buf), "%s%s%s: create %s, enable %s (installed from sims4vr.dll+0x%x)", rvaText(h.target).c_str(),
                          label.empty() ? "" : " ", label.c_str(), MH_StatusToString(h.created),
                          h.enableSeen ? MH_StatusToString(h.enabled) : "not called", h.caller);
            out += buf;
            out += "\n";
        }
    }
    FILE* f = _wfopen((g_dir + L"\\sims4vr_patch_report.txt").c_str(), L"wb");
    if (!f) return;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

void logLine(const std::string& s) { LOGT("patch", "%s", s.c_str()); }

}  // namespace

void init(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dir = dir;
    g_marker = dir + L"\\sims4vr_patch_inflight.txt";
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
    char buf[600];
    std::snprintf(buf, sizeof(buf), "exe %ls", path);
    g_exe.push_back(buf);
    std::snprintf(buf, sizeof(buf), "stamp %08x (table %08x: %s), file %llu bytes, image %u bytes, entry rva 0x%x, %u sections",
                  static_cast<unsigned>(nt->FileHeader.TimeDateStamp), addrs::tableStamp(),
                  nt->FileHeader.TimeDateStamp == addrs::tableStamp() ? "same build" : "DIFFERENT build",
                  (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow,
                  static_cast<unsigned>(nt->OptionalHeader.SizeOfImage), static_cast<unsigned>(nt->OptionalHeader.AddressOfEntryPoint),
                  static_cast<unsigned>(nt->FileHeader.NumberOfSections));
    g_exe.push_back(buf);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint8_t* at = base + sec[i].VirtualAddress;
        const size_t size = sec[i].Misc.VirtualSize;
        // How much of the section is mapped and readable right now (a protected exe may leave gaps).
        size_t committed = 0, regions = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (const uint8_t* p = at; p < at + size && VirtualQuery(p, &mbi, sizeof(mbi));) {
            const uint8_t* end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (end > at + size) end = at + size;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) committed += static_cast<size_t>(end - p);
            ++regions;
            p = end;
        }
        std::snprintf(buf, sizeof(buf), "section %.8s rva 0x%x size 0x%zx flags %08x, readable %zu of %zu bytes in %zu region(s)",
                      reinterpret_cast<const char*>(sec[i].Name), static_cast<unsigned>(sec[i].VirtualAddress), size,
                      static_cast<unsigned>(sec[i].Characteristics), committed, size, regions);
        g_exe.push_back(buf);
    }
    for (auto& l : g_exe) logLine(l);
    writeFile();
}

bool armUnknownBuild() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (GetFileAttributesW(g_marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Only a run the crash recorder saw die counts; one you closed (window, alt-F4, kill) leaves a plain marker.
        char text[16] = {};
        if (FILE* in = _wfopen(g_marker.c_str(), L"r")) {
            std::fread(text, 1, 5, in);
            std::fclose(in);
        }
        const bool crashed = std::strncmp(text, "CRASH", 5) == 0;
        DeleteFileW(g_marker.c_str());
        if (!crashed) {
            logLine("unrecognised build: the last run was closed without a crash; normal mode");
            goto arm;
        }
        logLine("unrecognised build: the last run on one did not get 120 s in or end cleanly (crash?): safe mode this run "
                "(only what needs no table); the next run tries again");
        g_lines[2].push_back("SAFE MODE: the last run on an unrecognised build did not get 120 s in or end cleanly");
        writeFile();
        return false;
    }
arm:
    FILE* f = _wfopen(g_marker.c_str(), L"w");
    if (f) {
        std::fputs("an unrecognised-build run is in progress; deleted after 120 s or at a clean shutdown\n", f);
        std::fclose(f);
    }
    g_armed = true;
    std::thread([] {
        Sleep(120000);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_armed) {
            DeleteFileW(g_marker.c_str());
            g_armed = false;
        }
    }).detach();
    return true;
}

void tableDone() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tableDone = true;
    for (size_t i = 0; i < addrs::rowCount(); ++i) {
        addrs::RowInfo r = addrs::row(i);
        char buf[400];
        if (r.foundRva)
            std::snprintf(buf, sizeof(buf), "%s %s: table 0x%x, now 0x%x (%s)%s", r.global ? "global" : "function", r.name, r.tableRva,
                          r.foundRva, r.state, r.used ? "" : ", not used");
        else
            std::snprintf(buf, sizeof(buf), "%s %s: table 0x%x, %s%s%s%s", r.global ? "global" : "function", r.name, r.tableRva, r.state,
                          r.why[0] ? " (" : "", r.why, r.why[0] ? ")" : "");
        logLine(buf);
    }
    writeFile();
}

void note(const char* group, const char* fmt, ...) {
    char text[900];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    int g = std::strcmp(group, "rtti") == 0 ? 0 : std::strcmp(group, "probe") == 0 ? 1 : 2;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_lines[g].size() < 400) g_lines[g].push_back(text);
    LOGT("patch", "%s: %s", kGroups[g], text);
    writeFile();
}

void nameHook(const void* target, const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_names.emplace_back(target, name);
}

void vtableProbe(const char* name, uintptr_t vtable, int slots) {
    if (!vtable || !rtti::readable(vtable, 8)) {
        note("probe", "vtable %s: not available", name);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string text;
    char buf[40];
    for (int i = 0; i < slots; ++i) {
        if (!rtti::readable(vtable + i * 8, 8)) break;
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtable + i * 8);
        std::snprintf(buf, sizeof(buf), "%s%d:%llx", i ? " " : "", i, static_cast<unsigned long long>(fn - base));
        text += buf;
    }
    char cls[200] = "?";
    uintptr_t fake = vtable;
    rtti::className(reinterpret_cast<uintptr_t>(&fake), cls, sizeof(cls));
    note("probe", "vtable %s at game+0x%llx (rtti class %s), slots as rva: %s", name,
         static_cast<unsigned long long>(vtable - base), cls, text.c_str());
}

void hookCreated(void* target, uint32_t caller, int status) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hooks.push_back({target, "", caller, static_cast<MH_STATUS>(status)});
    }
    if (status != MH_OK) note("note", "MinHook: create %s at %s failed", MH_StatusToString(static_cast<MH_STATUS>(status)), rvaText(target).c_str());
}

void hookEnabled(void* target, int status) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& h : g_hooks)
            if (h.target == target) {
                h.enabled = static_cast<MH_STATUS>(status);
                h.enableSeen = true;
            }
    }
    if (status != MH_OK) note("note", "MinHook: enable %s at %s failed", MH_StatusToString(static_cast<MH_STATUS>(status)), rvaText(target).c_str());
}

void markCrash() {
    if (!g_armed || g_marker.empty()) return;
    HANDLE h = CreateFileW(g_marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, "CRASH\n", 6, &n, nullptr);
    CloseHandle(h);
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_armed) {
        DeleteFileW(g_marker.c_str());
        g_armed = false;
    }
}

}  // namespace s4vr::patchreport

extern "C" MH_STATUS __wrap_MH_CreateHook(LPVOID target, LPVOID detour, LPVOID* original) {
    MH_STATUS status = __real_MH_CreateHook(target, detour, original);
    s4vr::patchreport::hookCreated(target,
                                   static_cast<uint32_t>(static_cast<uint8_t*>(__builtin_return_address(0)) - reinterpret_cast<uint8_t*>(&__ImageBase)),
                                   status);
    return status;
}

extern "C" MH_STATUS __wrap_MH_EnableHook(LPVOID target) {
    MH_STATUS status = __real_MH_EnableHook(target);
    if (target != MH_ALL_HOOKS) s4vr::patchreport::hookEnabled(target, status);
    return status;
}
