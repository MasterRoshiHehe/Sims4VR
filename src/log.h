#pragma once
#include <string>

namespace s4vr::log {
void init(const std::wstring& path);
void write(const char* fmt, ...);
// Same, with an explicit subsystem tag (otherwise the tag is made from the text before the first ": ").
void writeTagged(const char* tag, const char* fmt, ...);
// [log] tags = all | comma list of tags to keep; mute = comma list to drop. Lines: "[hh:mm:ss.mmm] [tag] text".
void setFilter(const std::string& tags, const std::string& mute);
}

#define LOG(...) ::s4vr::log::write(__VA_ARGS__)
#define LOGT(tag, ...) ::s4vr::log::writeTagged(tag, __VA_ARGS__)
