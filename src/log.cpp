#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace s4vr::log {

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void init(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    // The last two sessions' logs stay as _prev and _prev2 (a crash or a
    // headset drop is often only reported after the game was started again).
    std::wstring stem = path.size() > 4 ? path.substr(0, path.size() - 4) : path;
    MoveFileExW((stem + L"_prev.log").c_str(), (stem + L"_prev2.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(path.c_str(), (stem + L"_prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfopen(path.c_str(), L"w");
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

static void emit(const char* tagOrNull, const char* fmt, va_list args) {
    char text[2048];
    vsnprintf(text, sizeof(text), fmt, args);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::string tag = tagOrNull ? tagOrNull : tagOf(text);
    if (g_mute.count(tag) || (!g_all && !g_keep.count(tag) && tag != "session" && tag != "warning")) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag.c_str(), text);
    fflush(g_file);
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
