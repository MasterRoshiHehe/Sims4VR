#pragma once
#include <string>

namespace s4vr::log {
void init(const std::wstring& path);
void write(const char* fmt, ...);
}

#define LOG(...) ::s4vr::log::write(__VA_ARGS__)
