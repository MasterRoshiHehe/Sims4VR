#include "log.h"

#include <windows.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>
#include <mutex>
#include <set>
#include <string>

namespace s4vr::log {

static FILE* g_file = nullptr;
static std::mutex g_mutex;
static std::wstring g_dir;  // where the log is (traces go beside it)

// Flight recorder.
struct RingLine {
    ULONGLONG tick;
    std::string text;
};
static std::deque<RingLine> g_ring;
static size_t g_ringBytes = 0;
constexpr size_t kRingMax = 8u << 20;
static bool g_recorder = true, g_why = true, g_noisy = false;
static int g_ringSeconds = 20, g_keepDumps = 20;
static std::deque<std::string> g_whyLines;
static ULONGLONG g_lastDump = 0;
// Subsystems that wrote ~75% of a 21 min log (cas-834) while nobody debugged them: muted unless "Noisy tags" is on.
static const char* const kNoisy[] = {"frame_map", "frame_map_stack", "photo_probe", "sound_probe", "sound_words",
                                     "voice_fall_off", "shadow_lists_kind", "lopsided_eye_check"};

void init(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    // The last two sessions' logs stay as _prev and _prev2 (a crash or a
    // headset drop is often only reported after the game was started again).
    std::wstring stem = path.size() > 4 ? path.substr(0, path.size() - 4) : path;
    const BOOL movedPrev = MoveFileExW((stem + L"_prev.log").c_str(), (stem + L"_prev2.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    const DWORD prevError = movedPrev ? 0 : GetLastError();
    const BOOL moved = MoveFileExW(path.c_str(), (stem + L"_prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    const DWORD moveError = moved ? 0 : GetLastError();
    g_dir = path.substr(0, path.find_last_of(L"\\/") == std::wstring::npos ? 0 : path.find_last_of(L"\\/"));
    g_file = _wfopen(path.c_str(), L"w");
    if (!g_file) {
        // The log can't be opened (the old one is locked or the move was refused): a log of its own name per start,
        // and the reasons beside the crash notes.
        const int openError = errno;
        wchar_t tail[48];
        swprintf(tail, 48, L"_s%lu.log", static_cast<unsigned long>(GetTickCount()));
        g_file = _wfopen((stem + tail).c_str(), L"w");
        FILE* note = _wfopen((stem + L"_logerr.txt").c_str(), L"w");
        if (note) {
            fprintf(note, "log %ls: move of _prev.log error %lu, move of the log error %lu, open errno %d; %s\n",
                    path.c_str(), static_cast<unsigned long>(prevError), static_cast<unsigned long>(moveError), openError,
                    g_file ? "wrote a log with a number in its name instead" : "no log at all");
            fclose(note);
        }
    }
}

static std::set<std::string> g_keep, g_mute;
static bool g_all = true;

static std::string splitTag(const std::string& s, std::set<std::string>& out) {
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
        } else if (s[i] != ' ') {
            cur += static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
        }
    }
    return "";
}

void setFilter(const std::string& tags, const std::string& mute) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_keep.clear();
    g_mute.clear();
    splitTag(tags, g_keep);
    splitTag(mute, g_mute);
    g_all = g_keep.empty() || g_keep.count("all");
}

// "Arm IK: ..." -> "arm_ik"; no short "Word words:" prefix -> "misc".
static std::string tagOf(const char* text) {
    const char* colon = strchr(text, ':');
    if (!colon || colon == text || colon - text > 24 || colon[1] != ' ') return "misc";
    std::string tag;
    for (const char* p = text; p < colon; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (isalnum(ch)) tag += static_cast<char>(tolower(ch));
        else if (ch == ' ' || ch == '_' || ch == '-') tag += '_';
        else return "misc";
    }
    return tag;
}

static bool noisyTag(const std::string& tag) {
    for (const char* n : kNoisy)
        if (tag == n) return true;
    return false;
}

static void ringAdd(ULONGLONG tick, const std::string& line) {
    g_ring.push_back({tick, line});
    g_ringBytes += line.size() + 32;
    while (g_ringBytes > kRingMax && !g_ring.empty()) {
        g_ringBytes -= g_ring.front().text.size() + 32;
        g_ring.pop_front();
    }
    const ULONGLONG keep = static_cast<ULONGLONG>(g_ringSeconds) * 1000 + 5000;
    while (!g_ring.empty() && tick - g_ring.front().tick > keep) {
        g_ringBytes -= g_ring.front().text.size() + 32;
        g_ring.pop_front();
    }
}

static void emit(const char* tagOrNull, const char* fmt, va_list args) {
    char text[2048];
    vsnprintf(text, sizeof(text), fmt, args);
    bool doDump = false;
    char reason[96] = "";
    bool ringOnly = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) return;
        std::string tag = tagOrNull ? tagOrNull : tagOf(text);
        // Python: "trace: ..." -> ring only; "dump: reason" -> dump the ring; "why: ..." -> a decision line.
        if (!strncmp(text, "Python: trace: ", 15)) ringOnly = true;
        else if (!strncmp(text, "Python: dump: ", 14)) {
            doDump = true;
            snprintf(reason, sizeof(reason), "%s", text + 14);
            ringOnly = true;
        } else if (!strncmp(text, "Python: why: ", 13)) {
            if (!g_why) return;
            g_whyLines.push_back(text + 13);
            if (g_whyLines.size() > 8) g_whyLines.pop_front();
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[2200];
        snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 tag.c_str(), text);
        if (g_recorder) ringAdd(GetTickCount64(), line);
        if (!ringOnly) {
            const bool muted = g_mute.count(tag) || (!g_noisy && noisyTag(tag));
            if (!(muted || (!g_all && !g_keep.count(tag) && tag != "session" && tag != "warning"))) {
                fputs(line, g_file);
                fflush(g_file);
            }
        }
    }
    if (doDump) dumpTrace(reason);
}

void write(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(nullptr, fmt, args);
    va_end(args);
}

void writeTagged(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(tag, fmt, args);
    va_end(args);
}

}  // namespace s4vr::log

namespace s4vr::log {

static float num(const std::string& s, float def) { return s.empty() ? def : static_cast<float>(atof(s.c_str())); }
static bool on(const std::string& s, bool def) {
    if (s.empty()) return def;
    return !(s == "0" || s == "false" || s == "off" || s == "no");
}

void configure(const std::string& recorder, const std::string& ringSeconds, const std::string& why,
               const std::string& noisy, const std::string& keepDumps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recorder = on(recorder, true);
    g_ringSeconds = static_cast<int>(num(ringSeconds, 20));
    g_why = on(why, true);
    g_noisy = on(noisy, false);
    g_keepDumps = static_cast<int>(num(keepDumps, 20));
    if (g_ringSeconds < 5) g_ringSeconds = 5;
    if (g_ringSeconds > 120) g_ringSeconds = 120;
    if (g_keepDumps < 1) g_keepDumps = 1;
}

void setOption(const char* key, float v) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!strcmp(key, "recorder")) {
        g_recorder = v > 0.5f;
        if (!g_recorder) {
            g_ring.clear();
            g_ringBytes = 0;
        }
    } else if (!strcmp(key, "why")) g_why = v > 0.5f;
    else if (!strcmp(key, "noisy")) g_noisy = v > 0.5f;
    else if (!strcmp(key, "ring_seconds")) g_ringSeconds = v < 5 ? 5 : v > 120 ? 120 : static_cast<int>(v);
    else if (!strcmp(key, "keep_dumps")) g_keepDumps = v < 1 ? 1 : static_cast<int>(v);
}

std::string recentWhy(int n) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    int from = static_cast<int>(g_whyLines.size()) - n;
    for (int i = from < 0 ? 0 : from; i < static_cast<int>(g_whyLines.size()); ++i) {
        if (!out.empty()) out += " ; ";
        out += g_whyLines[i];
    }
    return out;
}

std::string dumpTrace(const char* reason, bool force) {
    std::vector<std::string> lines;
    int keepDumps;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_recorder || g_ring.empty() || g_dir.empty()) return "";
        const ULONGLONG now = GetTickCount64();
        if (!force && g_lastDump && now - g_lastDump < 3000) return "";
        g_lastDump = now;
        const ULONGLONG from = now - static_cast<ULONGLONG>(g_ringSeconds) * 1000;
        for (const RingLine& l : g_ring)
            if (l.tick >= from) lines.push_back(l.text);
        keepDumps = g_keepDumps;
        dir = g_dir;
    }
    // Next number, and the oldest files beyond keep_dumps removed.
    std::vector<std::wstring> names;
    int top = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\sims4vr_trace_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            names.push_back(fd.cFileName);
            int n = _wtoi(fd.cFileName + 14);
            if (n > top) top = n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t name[48];
    swprintf(name, 48, L"sims4vr_trace_%04d.log", top + 1);
    FILE* f = _wfopen((dir + L"\\" + name).c_str(), L"w");
    if (!f) return "";
    fprintf(f, "# trace dump: %s (last %d s, %d lines)\n", reason, g_ringSeconds, static_cast<int>(lines.size()));
    for (const std::string& l : lines) fputs(l.c_str(), f);
    fclose(f);
    names.push_back(name);
    std::sort(names.begin(), names.end());
    while (static_cast<int>(names.size()) > keepDumps) {
        DeleteFileW((dir + L"\\" + names.front()).c_str());
        names.erase(names.begin());
    }
    char narrow[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    write("Trace: dumped %d lines to %s (%s)", static_cast<int>(lines.size()), narrow, reason);
    return narrow;
}

}  // namespace s4vr::log
