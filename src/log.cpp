#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

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

void write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

}  // namespace s4vr::log
