#pragma once
#include <string>

namespace s4vr::log {
void init(const std::wstring& path);
void write(const char* fmt, ...);
// Same, with an explicit subsystem tag (otherwise the tag is made from the text before the first ": ").
void writeTagged(const char* tag, const char* fmt, ...);
// [log] tags = all | comma list of tags to keep; mute = comma list to drop. Lines: "[hh:mm:ss.mmm] [tag] text".
void setFilter(const std::string& tags, const std::string& mute);

// Flight recorder (docs/LOGGING-PROPOSAL.md): every line, even a muted one, also goes to an in-memory ring (8 MB,
// the last ring_seconds). dumpTrace writes it to sims4vr_trace_<n>.log (a mark, a level change, the menu button).
// Python lines "trace: ..." go to the ring only; "dump: reason" dumps it; "why: ..." are decision lines.
// [logging] recorder (1), ring_seconds (20), why (1), noisy (0), keep_dumps (20): read at start, then set from the menu.
void configure(const std::string& recorder, const std::string& ringSeconds, const std::string& why,
               const std::string& noisy, const std::string& keepDumps);
void setOption(const char* key, float value);
// Writes the ring's last ring_seconds; returns the file's name ("" if off or empty). Rate limited (1 per 3 s) unless force.
std::string dumpTrace(const char* reason, bool force = false);
// The last n "why:" lines, joined with " ; " (for a mark).
std::string recentWhy(int n);
}  // namespace s4vr::log

#define LOG(...) ::s4vr::log::write(__VA_ARGS__)
#define LOGT(tag, ...) ::s4vr::log::writeTagged(tag, __VA_ARGS__)
