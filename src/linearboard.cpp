#include "linearboard.h"

#include <windows.h>
#include <objidl.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace s4vr::linearboard {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// JSON: just enough to read Linear's answers and to quote strings into requests.
struct Json {
    enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json& operator[](const char* key) const;
    const Json& at(size_t i) const;
    size_t size() const { return type == kArr ? a.size() : 0; }
    bool isNull() const { return type == kNull; }
    std::string str() const { return type == kStr ? s : std::string(); }
    double num() const { return type == kNum ? n : 0; }
};
const Json kNullJson;
const Json& Json::operator[](const char* key) const {
    if (type != kObj) return kNullJson;
    for (const auto& kv : o)
        if (kv.first == key) return kv.second;
    return kNullJson;
}
const Json& Json::at(size_t i) const { return type == kArr && i < a.size() ? a[i] : kNullJson; }

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= end) return ok = false, 0;
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return ok = false, 0;
        }
        return v;
    }
    std::string string() {
        std::string out;
        ++p;  // "
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end) break;
            c = *p++;
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += c;
            }
        }
        if (p < end) ++p;  // "
        else ok = false;
        return out;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (p >= end || depth > 64) return ok = false, j;
        char c = *p;
        if (c == '{') {
            j.type = Json::kObj;
            ++p;
            ws();
            if (p < end && *p == '}') return ++p, j;
            while (ok && p < end) {
                ws();
                if (p >= end || *p != '"') return ok = false, j;
                std::string key = string();
                ws();
                if (p >= end || *p != ':') return ok = false, j;
                ++p;
                j.o.emplace_back(std::move(key), value(depth + 1));
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') return ++p, j;
                return ok = false, j;
            }
        } else if (c == '[') {
            j.type = Json::kArr;
            ++p;
            ws();
            if (p < end && *p == ']') return ++p, j;
            while (ok && p < end) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') return ++p, j;
                return ok = false, j;
            }
        } else if (c == '"') {
            j.type = Json::kStr;
            j.s = string();
        } else if (c == 't' && end - p >= 4 && !std::strncmp(p, "true", 4)) {
            j.type = Json::kBool, j.b = true, p += 4;
        } else if (c == 'f' && end - p >= 5 && !std::strncmp(p, "false", 5)) {
            j.type = Json::kBool, p += 5;
        } else if (c == 'n' && end - p >= 4 && !std::strncmp(p, "null", 4)) {
            p += 4;
        } else {
            char* stop = nullptr;
            std::string num;
            while (p < end && (std::strchr("+-0123456789.eE", *p))) num += *p++;
            j.type = Json::kNum;
            j.n = std::strtod(num.c_str(), &stop);
            if (num.empty()) ok = false;
        }
        return j;
    }
};
bool parseJson(const std::string& text, Json& out) {
    Parser ps{text.data(), text.data() + text.size()};
    out = ps.value();
    return ps.ok;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

uint32_t hexColour(const std::string& s, uint32_t fallback) {
    if (s.size() < 7 || s[0] != '#') return fallback;
    return static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16)) & 0xFFFFFFu;
}

// ---------------------------------------------------------------------------------------------------------------------
// The key: %APPDATA%\sims4vr\linear_key.txt once (you put it there), then only DPAPI-encrypted (linear_key.bin).
std::wstring keyDir() {
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) return L"";
    std::wstring dir = std::wstring(buf) + L"\\sims4vr";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool readAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool writeAll(const std::wstring& path, const void* data, size_t n) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data, static_cast<DWORD>(n), &put, nullptr) && put == n;
    CloseHandle(f);
    return ok;
}

std::string trimKey(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    // The first line only, without spaces or quotes around it.
    size_t nl = s.find_first_of("\r\n");
    if (nl != std::string::npos) s.resize(nl);
    const char* junk = " \t\"'";
    size_t a = s.find_first_not_of(junk), b = s.find_last_not_of(junk);
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Empty when there is none; `why` says what was found (never the key itself).
std::string loadKey(std::string& why) {
    const std::wstring dir = keyDir();
    if (dir.empty()) return why = "no APPDATA folder", std::string();
    const std::wstring txt = dir + L"\\linear_key.txt", bin = dir + L"\\linear_key.bin";
    std::string text;
    if (readAll(txt, text)) {
        std::string key = trimKey(text);
        if (!key.empty()) {
            DATA_BLOB in{static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(&key[0])}, out{};
            if (CryptProtectData(&in, L"sims4vr Linear key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                bool saved = writeAll(bin, out.pbData, out.cbData);
                LocalFree(out.pbData);
                if (saved) {
                    // The plain text is wiped: overwritten, then deleted.
                    std::string blank(text.size(), ' ');
                    writeAll(txt, blank.data(), blank.size());
                    DeleteFileW(txt.c_str());
                    why = "read from linear_key.txt, now kept encrypted for your Windows account (linear_key.bin); the "
                          "text file is wiped";
                } else {
                    why = "read from linear_key.txt (could not save the encrypted copy; the text file stays)";
                }
            } else {
                why = "read from linear_key.txt (Windows could not encrypt it; the text file stays)";
            }
            SecureZeroMemory(&text[0], text.size());
            return key;
        }
    }
    std::string sealed;
    if (readAll(bin, sealed) && !sealed.empty()) {
        DATA_BLOB in{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(&sealed[0])}, out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            std::string key(reinterpret_cast<const char*>(out.pbData), out.cbData);
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
            why = "from the encrypted store (linear_key.bin)";
            return trimKey(key);
        }
        why = "linear_key.bin could not be decrypted (made by another Windows account?)";
        return std::string();
    }
    why = "none yet (%APPDATA%\\sims4vr\\linear_key.txt)";
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------
// HTTPS to api.linear.app (WinHTTP: Windows' own, no extra files).
struct Http {
    HINTERNET session = nullptr, connect = nullptr;
    bool open() {
        if (connect) return true;
        session = WinHttpOpen(L"sims4vr-linear/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)  // before Windows 8.1
            session = WinHttpOpen(L"sims4vr-linear/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 5000, 8000, 10000, 15000);
        connect = WinHttpConnect(session, L"api.linear.app", INTERNET_DEFAULT_HTTPS_PORT, 0);
        return connect != nullptr;
    }
    void close() {
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
        connect = session = nullptr;
    }
    // HTTP status (0: no answer, `error` the Windows error), the body in `out`.
    int post(const std::string& body, const std::string& key, std::string& out, DWORD& error) {
        out.clear();
        error = 0;
        if (!open()) return error = GetLastError(), 0;
        HINTERNET req = WinHttpOpenRequest(connect, L"POST", L"/graphql", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) return error = GetLastError(), 0;
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: " + widen(key) + L"\r\n";
        int status = 0;
        if (WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                size_t at = out.size();
                out.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, &out[at], avail, &got)) break;
                out.resize(at + got);
                if (out.size() > (8u << 20)) break;
            }
        } else {
            error = GetLastError();
        }
        SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
        WinHttpCloseHandle(req);
        if (!status) close();  // a fresh connection next time
        return status;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// The board.
struct Label {
    std::wstring name;
    uint32_t colour = 0x8A8F98;
};
struct Issue {
    std::string id, stateId, updatedAt, completedAt;
    std::wstring identifier, title, description;
    int priority = 0;  // 0 none, 1 urgent, 2 high, 3 medium, 4 low
    double sortOrder = 0;
    std::vector<Label> labels;
    bool syncing = false;
};
struct State {
    std::string id, type;
    std::wstring name;
    uint32_t colour = 0x8A8F98;
    double position = 0;
};
struct Move {
    std::string issueId, stateId, baseUpdatedAt;
    std::wstring identifier;
    double sortOrder = 0;
    bool hasSort = false;
    ULONGLONG queuedAt = 0;
};
// A move done here wins over a refresh that started before Linear had it.
struct LocalMove {
    std::string stateId, completedAt;
    double sortOrder = 0;
    bool hasSort = false;
    ULONGLONG setAt = 0;
};

enum class Status { kNoKey, kLoading, kOk, kOffline, kBadKey, kNoTeam, kWait, kError };

struct Event {
    float x, y;
    int buttons;   // pointer: bit 0 pressed; a hand's hold / drop: 1 = at the board
    int kind = 0;  // 0 the laser or a finger, 1 a hand took a card, 2 it moves, 3 it lets go
};

// Pointer state and what it is on (board thread).
struct Ui {
    bool hover = false;
    float x = 0, y = 0;
    int buttons = 0;
    int hoverHit = -1;  // index of the hit under the pointer (redrawn when it changes)
    // A press: on what, where.
    bool pressed = false;
    float pressX = 0, pressY = 0;
    std::string pressIssue;
    int pressButton = -1;  // a hit target's id (see Hit)
    bool dragging = false;
    bool inHand = false;  // the dragged card is in your hand (off the board), not under a pointer
    bool atBoard = false;    // that hand is at the board: where it would go is shown
    float grabDX = 0, grabDY = 0;  // the card's corner from the pointer
    std::string detail;  // the issue whose details card is open
    std::map<std::string, int> scroll;  // per state: cards scrolled past
    std::wstring toast;
    ULONGLONG toastUntil = 0;
};

struct Board {
    std::mutex mutex;
    std::condition_variable wake;  // the network thread
    std::condition_variable uiWake;
    std::atomic<bool> on{false};
    float v[11] = {};
    std::string teamKey = "GLA";
    int doneDays = 14;
    bool showCanceled = false;
    int pollSeconds = 20;
    int width = 1920;
    bool cork = false;  // options mode=cork: the scrumboard on wall art (cards taken off by hand)
    bool threads = false;

    // Linear's data.
    std::string teamId, teamFor;
    std::wstring teamName;
    std::vector<State> states;
    std::vector<Issue> issues;
    bool loaded = false;
    ULONGLONG syncedAt = 0;
    Status status = Status::kLoading;
    std::wstring statusText;
    std::deque<Move> moves;
    std::map<std::string, LocalMove> local;
    bool refreshWanted = true;
    int version = 1;  // bumped on every change the picture shows

    std::mutex eventMutex;  // the present thread's pointer events (never waits on a drawing)
    std::deque<Event> events;
    std::atomic<ULONGLONG> lastPointer{0};
    std::atomic<int> picW{0}, picH{0};
    // What the latest picture shows, for the present thread (under eventMutex): the board's part of the picture
    // (the rest is the strip the card in your hand is drawn in), the card in your hand, the cards to take.
    struct Shown {
        float boardCrop[4] = {0, 0, 1, 1}, heldCrop[4] = {};
        float boardW = 1920, boardH = 1080;  // pixels
        float cardW = 0, cardH = 0;          // pixels
        bool held = false, detail = false;
        std::vector<std::pair<Gdiplus::RectF, std::string>> cards;
    } shown;
    Ui ui;

    // The latest picture (BGRA), board thread -> render thread.
    std::mutex frameMutex;
    std::vector<uint32_t> frame;
    int frameW = 0, frameH = 0, frameSeq = 0;  // frameH: the board and the held card's strip below it
    int boardH = 0;                            // the board's rows of it
    std::atomic<ULONGLONG> viewedAt{0};

    // Render thread only.
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Device* device = nullptr;
    int texW = 0, texH = 0, uploadedSeq = -1;
};
Board g;

ULONGLONG now() { return GetTickCount64(); }

void toast(const std::wstring& text, ULONGLONG ms = 5000) {
    g.ui.toast = text;
    g.ui.toastUntil = now() + ms;
    ++g.version;
}

// Canceled and duplicate: closed without being done (hidden unless [linear] canceled).
bool closedType(const std::string& type) { return type == "canceled" || type == "duplicate"; }
bool doneType(const std::string& type) { return type == "completed" || closedType(type); }

int typeRank(const std::string& type) {
    static const char* order[] = {"triage", "backlog", "unstarted", "started", "completed", "canceled", "duplicate"};
    for (int i = 0; i < 7; ++i)
        if (type == order[i]) return i;
    return 6;
}

// The columns shown, in Linear's order (caller holds the mutex).
std::vector<const State*> columns() {
    std::vector<const State*> out;
    for (const State& s : g.states)
        if (!closedType(s.type) || g.showCanceled) out.push_back(&s);
    std::stable_sort(out.begin(), out.end(), [](const State* a, const State* b) {
        int ra = typeRank(a->type), rb = typeRank(b->type);
        return ra != rb ? ra < rb : a->position < b->position;
    });
    return out;
}

const State* stateById(const std::string& id) {
    for (const State& s : g.states)
        if (s.id == id) return &s;
    return nullptr;
}

Issue* issueById(const std::string& id) {
    for (Issue& i : g.issues)
        if (i.id == id) return &i;
    return nullptr;
}

// A column's cards in the board's order: done ones newest first, the rest by Linear's manual order.
std::vector<Issue*> cardsOf(const State& s) {
    std::vector<Issue*> out;
    for (Issue& i : g.issues)
        if (i.stateId == s.id) out.push_back(&i);
    const bool done = doneType(s.type);
    std::stable_sort(out.begin(), out.end(), [done](const Issue* a, const Issue* b) {
        if (done) return a->completedAt > b->completedAt;
        return a->sortOrder < b->sortOrder;
    });
    return out;
}

std::string isoDaysAgo(int days) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t{};
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    t.QuadPart -= static_cast<ULONGLONG>(days) * 24ull * 3600ull * 10000000ull;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.000Z", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------------
// The network thread: the key, the team, refreshes and moves.
struct Net {
    Http http;
    std::string key, refused;
    ULONGLONG keyTriedAt = 0, nextPoll = 0, backoffUntil = 0, offlineSince = 0;
    int requests = 0;
    ULONGLONG requestsSince = 0;

    // A GraphQL request: true with `data` on success; else `error` and the HTTP status (0 no answer).
    bool gql(const std::string& query, const std::string& variables, Json& data, std::string& error, int& status) {
        std::string body = "{\"query\":" + quote(query) + ",\"variables\":" + variables + "}", out;
        DWORD werr = 0;
        status = http.post(body, key, out, werr);
        ++requests;
        if (!requestsSince) requestsSince = now();
        if (now() - requestsSince > 3600000) {
            LOG("Linear board: %d requests in the last hour", requests);
            requests = 0;
            requestsSince = now();
        }
        if (!status) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "no answer from Linear (Windows error %lu)", werr);
            error = buf;
            return false;
        }
        Json root;
        if (!parseJson(out, root)) {
            error = "Linear's answer was not readable (HTTP " + std::to_string(status) + ")";
            return false;
        }
        const Json& errors = root["errors"];
        if (errors.size()) {
            const Json& e = errors.at(0);
            error = e["message"].str();
            std::string code = e["extensions"]["code"].str();
            if (!code.empty()) error += " [" + code + "]";
            if (code == "RATELIMITED") status = 429;
            if (code == "AUTHENTICATION_ERROR" || error.find("uthenticat") != std::string::npos) status = 401;
            return false;
        }
        if (status != 200) {
            error = "HTTP " + std::to_string(status);
            return false;
        }
        data = root["data"];
        return true;
    }

    void setStatus(Status s, const std::wstring& text) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.status != s || g.statusText != text) {
            g.status = s;
            g.statusText = text;
            ++g.version;
        }
    }

    // A failed request: offline, a bad key, or wait.
    void failed(int status, const std::string& error) {
        if (status == 401 || status == 403) {
            LOG("Linear board: Linear refused the key (%s)", error.c_str());
            setStatus(Status::kBadKey, L"Linear refused the key. Make a new personal API key (Linear: Settings > "
                                       L"Security & access) and put it in %APPDATA%\\sims4vr\\linear_key.txt");
            refused = key;  // the same key again is not sent (a new one is)
            SecureZeroMemory(&key[0], key.size());
            key.clear();
            keyTriedAt = now() + 25000;  // look for a new one in 30 s
            return;
        }
        if (status == 429) {
            LOG("Linear board: Linear asks to wait (%s); next try in 60 s", error.c_str());
            backoffUntil = now() + 60000;
            setStatus(Status::kWait, L"Linear asks us to slow down; trying again in a minute");
            return;
        }
        if (status != 0 && status < 500) {
            // Linear answered but said no (a request it can't read): shown, tried again in a minute.
            LOG("Linear board: Linear answered with an error (%s)", error.c_str());
            backoffUntil = now() + 60000;
            setStatus(Status::kError, L"Linear: " + widen(error));
            return;
        }
        if (!offlineSince) {
            offlineSince = now();
            LOG("Linear board: offline (%s)", error.c_str());
        }
        backoffUntil = now() + 10000;
        setStatus(Status::kOffline, widen(error));
    }

    bool fetchTeam(const std::string& want) {
        Json data;
        std::string error;
        int status = 0;
        const char* q =
            "query Team($key: String!) { teams(filter: {key: {eq: $key}}) { nodes { id key name "
            "states { nodes { id name type color position } } } } }";
        if (!gql(q, "{\"key\":" + quote(want) + "}", data, error, status)) {
            failed(status, error);
            return false;
        }
        const Json& team = data["teams"]["nodes"].at(0);
        if (team.isNull()) {
            LOG("Linear board: no team with the key %s", want.c_str());
            setStatus(Status::kNoTeam, L"No Linear team with the key " + widen(want) + L" ([linear] team)");
            backoffUntil = now() + 30000;
            return false;
        }
        std::vector<State> states;
        const Json& nodes = team["states"]["nodes"];
        for (size_t i = 0; i < nodes.size(); ++i) {
            const Json& n = nodes.at(i);
            State s;
            s.id = n["id"].str();
            s.type = n["type"].str();
            s.name = widen(n["name"].str());
            s.colour = hexColour(n["color"].str(), 0x8A8F98);
            s.position = n["position"].num();
            states.push_back(std::move(s));
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        g.teamId = team["id"].str();
        g.teamFor = want;
        g.teamName = widen(team["name"].str());
        g.states = std::move(states);
        ++g.version;
        LOG("Linear board: team %s (%zu columns)", want.c_str(), g.states.size());
        return true;
    }

    bool fetchIssues() {
        std::string teamId;
        int doneDays;
        bool canceled;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            teamId = g.teamId;
            doneDays = g.doneDays;
            canceled = g.showCanceled;
        }
        const ULONGLONG startedAt = now();
        std::vector<Issue> issues;
        std::string after;
        for (int page = 0; page < 10; ++page) {
            std::string q =
                "query Board($after: String) { issues(first: 100, after: $after, filter: {team: {id: {eq: " +
                quote(teamId) + "}}, ";
            if (!canceled) q += "state: {type: {nin: [\"canceled\", \"duplicate\"]}}, ";
            q += "or: [{completedAt: {null: true}}, {completedAt: {gt: " + quote(isoDaysAgo(std::max(0, doneDays))) +
                 "}}]}) { nodes { id identifier title "
                 "description priority sortOrder updatedAt completedAt state { id } labels(first: 4) { nodes { name "
                 "color } } } pageInfo { hasNextPage endCursor } } }";
            std::string vars = "{\"after\":" + (after.empty() ? std::string("null") : quote(after)) + "}";
            Json data;
            std::string error;
            int status = 0;
            if (!gql(q, vars, data, error, status)) {
                failed(status, error);
                return false;
            }
            const Json& conn = data["issues"];
            const Json& nodes = conn["nodes"];
            for (size_t i = 0; i < nodes.size(); ++i) {
                const Json& n = nodes.at(i);
                Issue it;
                it.id = n["id"].str();
                it.identifier = widen(n["identifier"].str());
                it.title = widen(n["title"].str());
                std::string d = n["description"].str();
                if (d.size() > 1500) d.resize(1500);
                it.description = widen(d);
                it.priority = static_cast<int>(n["priority"].num());
                it.sortOrder = n["sortOrder"].num();
                it.updatedAt = n["updatedAt"].str();
                it.completedAt = n["completedAt"].str();
                it.stateId = n["state"]["id"].str();
                const Json& labels = n["labels"]["nodes"];
                for (size_t k = 0; k < labels.size(); ++k)
                    it.labels.push_back({widen(labels.at(k)["name"].str()), hexColour(labels.at(k)["color"].str(), 0x8A8F98)});
                issues.push_back(std::move(it));
            }
            if (!conn["pageInfo"]["hasNextPage"].b) break;
            after = conn["pageInfo"]["endCursor"].str();
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        // Moves of yours Linear may not have had when this refresh started, and ones still waiting, stay as you left them.
        for (Issue& it : issues) {
            auto lm = g.local.find(it.id);
            if (lm != g.local.end() && lm->second.setAt >= startedAt - 2000) {
                it.stateId = lm->second.stateId;
                it.completedAt = lm->second.completedAt;
                if (lm->second.hasSort) it.sortOrder = lm->second.sortOrder;
            }
            for (const Move& m : g.moves)
                if (m.issueId == it.id) it.syncing = true;
        }
        for (auto it = g.local.begin(); it != g.local.end();)
            it = now() - it->second.setAt > 120000 ? g.local.erase(it) : std::next(it);
        const bool first = !g.loaded;
        g.issues = std::move(issues);
        g.loaded = true;
        g.syncedAt = now();
        g.status = Status::kOk;
        g.statusText.clear();
        ++g.version;
        if (first) LOG("Linear board: %zu cards loaded", g.issues.size());
        if (offlineSince) {
            LOG("Linear board: online again after %llu s", static_cast<unsigned long long>((now() - offlineSince) / 1000));
            offlineSince = 0;
        }
        return true;
    }

    // Sends the oldest waiting move; false when it has to wait (offline).
    bool sendMove() {
        Move m;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (g.moves.empty()) return true;
            m = g.moves.front();
        }
        // A move that waited (offline): a card changed in Linear meanwhile stays as Linear has it.
        if (now() - m.queuedAt > 15000) {
            Json data;
            std::string error;
            int status = 0;
            if (!gql("query One($id: String!) { issue(id: $id) { id updatedAt } }", "{\"id\":" + quote(m.issueId) + "}",
                     data, error, status)) {
                failed(status, error);
                return false;
            }
            if (data["issue"]["updatedAt"].str() != m.baseUpdatedAt) {
                std::lock_guard<std::mutex> lock(g.mutex);
                g.moves.pop_front();
                g.local.erase(m.issueId);
                toast(m.identifier + L" was changed in Linear while offline; left as Linear has it");
                g.refreshWanted = true;
                LOG("Linear board: a waiting move was dropped (the card changed in Linear meanwhile)");
                return true;
            }
        }
        std::string q = m.hasSort ? "mutation Move($id: String!, $state: String!, $sort: Float) { issueUpdate(id: $id, "
                                    "input: {stateId: $state, sortOrder: $sort}) { success issue { id updatedAt } } }"
                                  : "mutation Move($id: String!, $state: String!) { issueUpdate(id: $id, input: "
                                    "{stateId: $state}) { success issue { id updatedAt } } }";
        std::string vars = "{\"id\":" + quote(m.issueId) + ",\"state\":" + quote(m.stateId);
        if (m.hasSort) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ",\"sort\":%.6f", m.sortOrder);
            vars += buf;
        }
        vars += "}";
        Json data;
        std::string error;
        int status = 0;
        if (!gql(q, vars, data, error, status)) {
            if (status == 0 || status == 429 || status >= 500) {
                failed(status, error);
                return false;
            }
            // Linear said no (not a connection problem): the card goes back where Linear has it.
            LOG("Linear board: Linear refused a move (%s)", error.c_str());
            std::lock_guard<std::mutex> lock(g.mutex);
            g.moves.pop_front();
            g.local.erase(m.issueId);
            toast(L"Linear refused moving " + m.identifier + L": " + widen(error));
            g.refreshWanted = true;
            return true;
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        g.moves.pop_front();
        if (Issue* it = issueById(m.issueId)) {
            it->updatedAt = data["issueUpdate"]["issue"]["updatedAt"].str();
            bool more = false;
            for (const Move& o : g.moves) more = more || o.issueId == m.issueId;
            it->syncing = more;
        }
        auto lm = g.local.find(m.issueId);
        if (lm != g.local.end()) lm->second.setAt = now();
        ++g.version;
        LOG("Linear board: moved a card (%zu more waiting)", g.moves.size());
        return true;
    }

    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(g.mutex);
                g.wake.wait_for(lock, std::chrono::milliseconds(500));
            }
            {
                // Off the screen: moves still waiting are sent all the same; nothing else is asked.
                std::lock_guard<std::mutex> lock(g.mutex);
                if (!g.on && g.moves.empty()) continue;
            }
            const ULONGLONG t = now();
            if (key.empty()) {
                if (t < keyTriedAt + 5000) continue;
                keyTriedAt = t;
                std::string why;
                key = loadKey(why);
                static std::string lastWhy;
                if (why != lastWhy) {
                    lastWhy = why;
                    LOG("Linear board: key %s", why.c_str());
                }
                if (!key.empty() && key == refused) {
                    key.clear();
                    continue;  // still the refused one: the board keeps saying so
                }
                if (key.empty()) {
                    setStatus(Status::kNoKey, L"No Linear key yet");
                    continue;
                }
                setStatus(Status::kLoading, L"Loading the board");
            }
            if (t < backoffUntil) continue;
            std::string want;
            bool refresh, teamOk, haveMoves;
            ULONGLONG viewed = g.viewedAt.load();
            int poll;
            {
                std::lock_guard<std::mutex> lock(g.mutex);
                want = g.teamKey;
                teamOk = !g.teamId.empty() && g.teamFor == want;
                refresh = g.refreshWanted;
                haveMoves = !g.moves.empty();
                poll = g.pollSeconds;
            }
            if (!teamOk) {
                if (!fetchTeam(want)) continue;
                refresh = true;
            }
            if (haveMoves) {
                if (!sendMove()) continue;
                std::lock_guard<std::mutex> lock(g.mutex);
                if (!g.moves.empty()) g.wake.notify_one();
            }
            // Refreshed only while the board is looked at (none while the screen is out of view).
            const bool lookedAt = g.on && t - viewed < 3000;
            if ((refresh && g.on) || (lookedAt && t >= nextPoll)) {
                {
                    std::lock_guard<std::mutex> lock(g.mutex);
                    g.refreshWanted = false;
                }
                if (fetchIssues()) nextPoll = now() + static_cast<ULONGLONG>(std::max(5, poll)) * 1000;
            }
        }
    }
};
Net g_net;

// ---------------------------------------------------------------------------------------------------------------------
// Drawing (GDI+, the board thread).
using namespace Gdiplus;

constexpr uint32_t kRed = 0xEB5757, kOrange = 0xF2994A, kPaper = 0xF6F1E7;

// Linear's dark look (a TV or computer screen), or a cork board with paper notes pinned on it (wall art,
// [linear] cork: the scrumboard you take cards off with your hand).
struct Theme {
    bool cork;
    uint32_t back, column, card, cardHover, line, text, dim, accent;
};
const Theme kDarkTheme{false, 0x0F1011, 0x17181B, 0x1F2025, 0x272930, 0x2C2E35, 0xE6E6E9, 0x8A8F98, 0x5E6AD2};
const Theme kCorkTheme{true, 0xB48A5E, 0x9C754C, 0xFFF1A8, 0xFFF7CF, 0x8C6A45, 0x24211E, 0x5C564D, 0x2F6FD6};
const Theme* T = &kDarkTheme;

Color col(uint32_t rgb, BYTE a = 255) { return Color(a, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

void roundRect(GraphicsPath& p, RectF r, float radius) {
    float d = std::min(radius * 2, std::min(r.Width, r.Height));
    p.Reset();
    if (d <= 0.5f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void fillRound(Graphics& gr, RectF r, float radius, uint32_t rgb, BYTE a = 255) {
    GraphicsPath p;
    roundRect(p, r, radius);
    SolidBrush b(col(rgb, a));
    gr.FillPath(&b, &p);
}

void strokeRound(Graphics& gr, RectF r, float radius, uint32_t rgb, float width, BYTE a = 255) {
    GraphicsPath p;
    roundRect(p, r, radius);
    Pen pen(col(rgb, a), width);
    gr.DrawPath(&pen, &p);
}

// What the pointer can hit, laid out with the picture.
enum HitKind { kHitNone, kHitCard, kHitColumn, kHitUp, kHitDown, kHitRefresh, kHitClose, kHitMoveTo, kHitBackdrop, kHitPanel };
struct Hit {
    HitKind kind = kHitNone;
    RectF r;
    std::string id;  // issue or state
};

struct Fonts {
    float scale = 0;
    FontFamily* family = nullptr;
    Font *title = nullptr, *card = nullptr, *small = nullptr, *head = nullptr, *big = nullptr, *body = nullptr;
    void make(float s) {
        if (s == scale && card) return;
        delete title, delete card, delete small, delete head, delete big, delete body;
        if (!family) {
            family = new FontFamily(L"Segoe UI");
            if (!family->IsAvailable()) {
                delete family;
                family = FontFamily::GenericSansSerif()->Clone();
            }
        }
        scale = s;
        title = new Font(family, 26 * s, FontStyleBold, UnitPixel);
        head = new Font(family, 19 * s, FontStyleBold, UnitPixel);
        card = new Font(family, 18 * s, FontStyleRegular, UnitPixel);
        small = new Font(family, 14 * s, FontStyleRegular, UnitPixel);
        big = new Font(family, 30 * s, FontStyleBold, UnitPixel);
        body = new Font(family, 17 * s, FontStyleRegular, UnitPixel);
    }
};

struct Layout {
    float s = 1, W = 0, H = 0;  // H: the board's part of the picture
    float headH = 0, cardH = 0, cardW = 0, gap = 0, colHeadH = 0;
    std::vector<Hit> hits;
};

void text(Graphics& gr, const std::wstring& t, Font* f, RectF r, uint32_t rgb, bool oneLine = true,
          StringAlignment align = StringAlignmentNear, BYTE a = 255) {
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(oneLine ? StringAlignmentCenter : StringAlignmentNear);
    fmt.SetTrimming(StringTrimmingEllipsisWord);
    fmt.SetFormatFlags(oneLine ? StringFormatFlagsNoWrap : StringFormatFlagsLineLimit);
    SolidBrush b(col(rgb, a));
    gr.DrawString(t.c_str(), static_cast<INT>(t.size()), f, r, &fmt, &b);
}

float textWidth(Graphics& gr, const std::wstring& t, Font* f) {
    RectF out;
    StringFormat fmt(StringFormat::GenericTypographic());
    gr.MeasureString(t.c_str(), static_cast<INT>(t.size()), f, PointF(0, 0), &fmt, &out);
    return out.Width;
}

// Linear's priority marks: bars (high 3, medium 2, low 1), an orange box with ! (urgent), dashes (none).
void priorityMark(Graphics& gr, int p, float x, float y, float s) {
    if (p == 1) {
        fillRound(gr, RectF(x, y, 16 * s, 16 * s), 3 * s, kOrange);
        Font f(L"Segoe UI", 12 * s, FontStyleBold, UnitPixel);
        text(gr, L"!", &f, RectF(x, y, 16 * s, 16 * s), 0xFFFFFF, true, StringAlignmentCenter);
        return;
    }
    if (p == 0) {
        SolidBrush b(col(T->dim));
        for (int i = 0; i < 3; ++i) gr.FillRectangle(&b, x + i * 5.5f * s, y + 7 * s, 3.5f * s, 2 * s);
        return;
    }
    int lit = p == 2 ? 3 : p == 3 ? 2 : 1;
    for (int i = 0; i < 3; ++i) {
        SolidBrush b(col(i < lit ? T->text : T->line));
        float h = (5 + i * 4) * s;
        gr.FillRectangle(&b, x + i * 5.5f * s, y + 16 * s - h, 3.5f * s, h);
    }
}

// A paper note's colour on the cork board: its first label's colour, pale; plain notes are sticky-note yellow.
uint32_t paperOf(const Issue& it, bool hover) {
    uint32_t c = it.labels.empty() ? T->card : it.labels[0].colour;
    const float keep = it.labels.empty() ? 1.0f : 0.28f;  // of the label's colour; the rest white
    auto mix = [&](int shift) {
        float v = ((c >> shift) & 0xFF) * keep + 255 * (1 - keep);
        if (hover) v = v + (255 - v) * 0.35f;
        return static_cast<uint32_t>(std::min(255.0f, v)) << shift;
    };
    return mix(16) | mix(8) | mix(0);
}

void drawCard(Graphics& gr, Fonts& f, const Layout& L, const Issue& it, RectF r, bool hover, bool selected, BYTE alpha = 255,
              bool flat = false) {
    const float s = L.s;
    GraphicsState saved = gr.Save();
    if (T->cork) {
        // Pinned paper: a small tilt of its own (the same every time), a shadow on the cork, a pin at the top.
        if (!flat) {
            uint32_t h = 2166136261u;
            for (char ch : it.id) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
            const float tilt = (static_cast<int>(h % 25) - 12) * 0.12f;  // -1.4..1.4 degrees
            const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
            gr.TranslateTransform(cx, cy);
            gr.RotateTransform(tilt);
            gr.TranslateTransform(-cx, -cy);
            fillRound(gr, RectF(r.X + 4 * s, r.Y + 6 * s, r.Width, r.Height), 3 * s, 0x000000, static_cast<BYTE>(alpha * 0.3f));
        }
        fillRound(gr, r, 3 * s, paperOf(it, hover), alpha);
        if (selected) strokeRound(gr, r, 3 * s, T->accent, 3 * s, alpha);
        SolidBrush pin(col(0xD23B3B, alpha)), shine(col(0xFFFFFF, static_cast<BYTE>(alpha * 0.7f)));
        const float px = r.X + r.Width / 2, py = r.Y + 9 * s;
        gr.FillEllipse(&pin, px - 7 * s, py - 7 * s, 14 * s, 14 * s);
        gr.FillEllipse(&shine, px - 4 * s, py - 5 * s, 5 * s, 4 * s);
    } else {
        fillRound(gr, r, 8 * s, hover ? T->cardHover : T->card, alpha);
        strokeRound(gr, r, 8 * s, selected ? T->accent : T->line, selected ? 2.5f * s : 1 * s, alpha);
    }
    priorityMark(gr, it.priority, r.X + 12 * s, r.Y + 11 * s, s);
    text(gr, it.identifier, f.small, RectF(r.X + 34 * s, r.Y + 6 * s, r.Width - 60 * s, 24 * s), T->dim, true,
         StringAlignmentNear, alpha);
    if (it.syncing) {
        SolidBrush b(col(T->accent, alpha));
        gr.FillEllipse(&b, r.X + r.Width - 22 * s, r.Y + 12 * s, 10 * s, 10 * s);
    }
    text(gr, it.title, f.card, RectF(r.X + 12 * s, r.Y + 32 * s, r.Width - 24 * s, 48 * s), T->text, false,
         StringAlignmentNear, alpha);
    float x = r.X + 12 * s, y = r.Y + r.Height - 26 * s;
    for (const Label& l : it.labels) {
        float w = std::min(textWidth(gr, l.name, f.small) + 26 * s, r.X + r.Width - 12 * s - x);
        if (w < 40 * s) break;
        RectF chip(x, y, w, 20 * s);
        strokeRound(gr, chip, 10 * s, T->line, 1 * s, alpha);
        SolidBrush dot(col(l.colour, alpha));
        gr.FillEllipse(&dot, x + 7 * s, y + 6 * s, 8 * s, 8 * s);
        text(gr, l.name, f.small, RectF(x + 18 * s, y, w - 22 * s, 20 * s), T->dim, true, StringAlignmentNear, alpha);
        x += w + 6 * s;
    }
    gr.Restore(saved);
}

// A paper label behind text on the cork board (the header, a column's name).
void paperLabel(Graphics& gr, RectF r, float s) {
    if (!T->cork) return;
    fillRound(gr, RectF(r.X + 3 * s, r.Y + 4 * s, r.Width, r.Height), 3 * s, 0x000000, 60);
    fillRound(gr, r, 3 * s, kPaper);
    SolidBrush tape(col(0xE8DFC4, 200));
    gr.FillRectangle(&tape, r.X + r.Width / 2 - 22 * s, r.Y - 7 * s, 44 * s, 14 * s);
}

// The cork itself: a speckled brown made once per size, behind a wooden frame.
Bitmap* corkPicture(int W, int H) {
    static Bitmap* made = nullptr;
    static int mw = 0, mh = 0;
    if (made && mw == W && mh == H) return made;
    delete made;
    made = new Bitmap(W, H, PixelFormat32bppARGB);
    mw = W, mh = H;
    Graphics gr(made);
    gr.Clear(col(T->back));
    uint32_t seed = 12345;
    auto rnd = [&]() { return (seed = seed * 1664525u + 1013904223u) >> 8; };
    const int dots = W * H / 90;
    for (int i = 0; i < dots; ++i) {
        float x = static_cast<float>(rnd() % W), y = static_cast<float>(rnd() % H), d = 1.0f + (rnd() % 30) / 10.0f;
        int tone = static_cast<int>(rnd() % 3);
        uint32_t c = tone == 0 ? 0x8A6440 : tone == 1 ? 0xC9A271 : 0x9E7650;
        SolidBrush b(col(c, static_cast<BYTE>(90 + rnd() % 120)));
        gr.FillEllipse(&b, x, y, d, d * 0.8f);
    }
    return made;
}

std::wstring agoText(ULONGLONG ms) {
    ULONGLONG sec = ms / 1000;
    wchar_t buf[64];
    if (sec < 10) return L"just now";
    if (sec < 60) std::swprintf(buf, 64, L"%llu s ago", static_cast<unsigned long long>(sec / 5 * 5));
    else std::swprintf(buf, 64, L"%llu min ago", static_cast<unsigned long long>(sec / 60));
    return buf;
}

// Draws the whole board into `bmp` and lays out what the pointer hits (caller holds g.mutex).
void drawBoard(Graphics& gr, Fonts& f, Layout& L) {
    const float s = L.s, W = L.W, H = L.H;
    L.hits.clear();
    gr.Clear(col(T->back));
    if (T->cork) {
        gr.DrawImage(corkPicture(static_cast<int>(W), static_cast<int>(H)), 0.0f, 0.0f, W, H);
        Pen frame(col(0x6B4A2B), 14 * s);
        gr.DrawRectangle(&frame, 7 * s, 7 * s, W - 14 * s, H - 14 * s);
    }
    Ui& ui = g.ui;
    // Header: the logo, the team, how fresh the board is, refresh.
    L.headH = 64 * s;
    {
        std::wstring name = g.teamName.empty() ? widen(g.teamKey) : g.teamName + L"  \x00B7  " + widen(g.teamKey);
        paperLabel(gr, RectF(12 * s, 12 * s, std::min(W * 0.5f, textWidth(gr, name, f.title) + 74 * s), L.headH - 18 * s), s);
        // Linear's mark: an indigo disc crossed by three light stripes.
        GraphicsPath disc;
        disc.AddEllipse(20 * s, 16 * s, 32 * s, 32 * s);
        SolidBrush logo(col(0x5E6AD2));
        gr.FillPath(&logo, &disc);
        gr.SetClip(&disc);
        Pen stripe(col(0xF4F5F8), 3 * s);
        for (int i = -1; i <= 1; ++i) {
            const float o = i * 9 * s, cx = 36 * s + o * 0.7071f, cy = 32 * s + o * 0.7071f, h = 20 * s;
            gr.DrawLine(&stripe, cx - h * 0.7071f, cy + h * 0.7071f, cx + h * 0.7071f, cy - h * 0.7071f);
        }
        gr.ResetClip();
        text(gr, name, f.title, RectF(66 * s, 0, W * 0.5f, L.headH), T->text);
        std::wstring st;
        uint32_t stc = T->dim;
        switch (g.status) {
            case Status::kOk: st = L"Synced " + agoText(now() - g.syncedAt); break;
            case Status::kLoading: st = L"Loading\x2026"; break;
            case Status::kNoKey: st = L"No Linear key"; stc = kOrange; break;
            case Status::kBadKey: st = L"Linear refused the key"; stc = kRed; break;
            case Status::kNoTeam: st = g.statusText; stc = kRed; break;
            case Status::kWait: st = L"Linear asks us to wait"; stc = kOrange; break;
            case Status::kOffline:
                st = L"Offline" + (g.loaded ? L"  \x00B7  last synced " + agoText(now() - g.syncedAt) : std::wstring());
                stc = kOrange;
                break;
            case Status::kError: st = g.statusText; stc = kRed; break;
        }
        if (!g.moves.empty()) {
            wchar_t buf[64];
            std::swprintf(buf, 64, L"  \x00B7  %zu move%ls waiting", g.moves.size(), g.moves.size() == 1 ? L"" : L"s");
            st += buf;
        }
        RectF refresh(W - 64 * s, 12 * s, 44 * s, 40 * s);
        if (T->cork) {
            float sw = std::min(W * 0.5f, textWidth(gr, st, f.small) + 30 * s);
            paperLabel(gr, RectF(W - 80 * s - sw, 16 * s, sw, L.headH - 26 * s), s);
        }
        text(gr, st, f.small, RectF(W * 0.45f, 0, W * 0.55f - 95 * s, L.headH), stc, true, StringAlignmentFar);
        bool hov = ui.hover && refresh.Contains(ui.x, ui.y);
        fillRound(gr, refresh, 8 * s, T->cork ? (hov ? T->cardHover : T->card) : hov ? T->cardHover : T->column);
        Pen arrow(col(T->text), 2.5f * s);
        gr.DrawArc(&arrow, refresh.X + 12 * s, refresh.Y + 10 * s, 20 * s, 20 * s, 40, 290);
        PointF tip[3] = {PointF(refresh.X + 30 * s, refresh.Y + 8 * s), PointF(refresh.X + 33 * s, refresh.Y + 19 * s),
                         PointF(refresh.X + 23 * s, refresh.Y + 17 * s)};
        SolidBrush tipB(col(T->text));
        gr.FillPolygon(&tipB, tip, 3);
        L.hits.push_back({kHitRefresh, refresh, ""});
    }
    // No key: how to give it one, instead of the board.
    if ((g.status == Status::kNoKey || g.status == Status::kBadKey) && !g.loaded) {
        RectF box(W * 0.15f, H * 0.2f, W * 0.7f, H * 0.6f);
        fillRound(gr, box, 14 * s, T->cork ? kPaper : T->column);
        text(gr, g.status == Status::kBadKey ? L"Linear refused the key" : L"Connect your Linear board", f.big,
             RectF(box.X + 40 * s, box.Y + 30 * s, box.Width - 80 * s, 50 * s), T->text);
        std::wstring how =
            L"1.  In Linear: Settings > Security & access > Personal API keys > New key.\n\n"
            L"2.  On this PC, make the file  %APPDATA%\\sims4vr\\linear_key.txt  and paste the key into it "
            L"(Windows key + R, type %APPDATA%, Enter).\n\n"
            L"3.  The board picks it up within a few seconds. The key is then kept encrypted for your Windows account "
            L"and the text file is wiped. It never goes into the mod's files or logs.";
        text(gr, how, f.body, RectF(box.X + 40 * s, box.Y + 100 * s, box.Width - 80 * s, box.Height - 130 * s), T->dim,
             false);
        return;
    }
    if (!g.loaded) {
        text(gr, g.status == Status::kOffline ? L"Linear can't be reached: " + g.statusText
                 : g.status == Status::kError || g.status == Status::kNoTeam || g.status == Status::kWait ? g.statusText
                                                                                                         : std::wstring(L"Loading\x2026"),
             f.title, RectF(0, 0, W, H), T->dim, true, StringAlignmentCenter);
        return;
    }
    // Columns.
    std::vector<const State*> cols = columns();
    const int n = std::max<int>(1, static_cast<int>(cols.size()));
    const float pad = 16 * s, colGap = 12 * s, top = L.headH + 6 * s, bottom = H - pad;
    const float colW = (W - pad * 2 - colGap * (n - 1)) / n;
    L.colHeadH = 46 * s;
    L.cardH = 112 * s;
    L.cardW = colW - 16 * s;
    L.gap = T->cork ? 12 * s : 8 * s;
    const Issue* dragged = ui.dragging ? issueById(ui.pressIssue) : nullptr;
    // Where a dragged card would land: the column under the pointer and the slot in it.
    const State* dropCol = nullptr;
    int dropAt = -1;
    for (int c = 0; c < static_cast<int>(cols.size()); ++c) {
        const State& st = *cols[c];
        RectF cr(pad + c * (colW + colGap), top, colW, bottom - top);
        const bool target = dragged && (!ui.inHand || ui.atBoard) && ui.x >= cr.X - colGap / 2 && ui.x < cr.X + cr.Width + colGap / 2;
        if (T->cork) fillRound(gr, cr, 6 * s, 0x000000, target ? 45 : 22);
        else fillRound(gr, cr, 10 * s, T->column);
        if (target) strokeRound(gr, cr, T->cork ? 6 * s : 10 * s, T->accent, 2 * s);
        // Its header: the state's colour, name and count, scroll arrows when its cards don't fit.
        std::vector<Issue*> cards = cardsOf(st);
        SolidBrush dot(col(st.colour));
        gr.FillEllipse(&dot, cr.X + 14 * s, cr.Y + 16 * s, 14 * s, 14 * s);
        wchar_t count[16];
        std::swprintf(count, 16, L"%zu", cards.size());
        float nameW = std::min(textWidth(gr, st.name, f.head) + 4 * s, colW - 120 * s);
        if (T->cork) {
            paperLabel(gr, RectF(cr.X + 6 * s, cr.Y + 6 * s, nameW + 76 * s, L.colHeadH - 10 * s), s);
            gr.FillEllipse(&dot, cr.X + 14 * s, cr.Y + 16 * s, 14 * s, 14 * s);
        }
        text(gr, st.name, f.head, RectF(cr.X + 36 * s, cr.Y, nameW, L.colHeadH), T->text);
        text(gr, count, f.small, RectF(cr.X + 42 * s + nameW, cr.Y, 40 * s, L.colHeadH), T->dim);
        const float listTop = cr.Y + L.colHeadH, listH = cr.Height - L.colHeadH - 8 * s;
        const int fit = std::max(1, static_cast<int>((listH + L.gap) / (L.cardH + L.gap)));
        int& scroll = ui.scroll[st.id];
        scroll = std::max(0, std::min(scroll, std::max(0, static_cast<int>(cards.size()) - fit)));
        L.hits.push_back({kHitColumn, cr, st.id});
        if (static_cast<int>(cards.size()) > fit) {
            RectF up(cr.X + cr.Width - 76 * s, cr.Y + 8 * s, 32 * s, 30 * s), down(cr.X + cr.Width - 40 * s, cr.Y + 8 * s, 32 * s, 30 * s);
            for (int k = 0; k < 2; ++k) {
                RectF r = k ? down : up;
                bool can = k ? scroll + fit < static_cast<int>(cards.size()) : scroll > 0;
                bool hov = ui.hover && r.Contains(ui.x, ui.y);
                fillRound(gr, r, 6 * s, hov && can ? T->cardHover : T->card);
                float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, d = k ? 1.0f : -1.0f;
                PointF tri[3] = {PointF(cx - 7 * s, cy - 4 * s * d), PointF(cx + 7 * s, cy - 4 * s * d), PointF(cx, cy + 5 * s * d)};
                SolidBrush b(col(can ? T->text : T->line));
                gr.FillPolygon(&b, tri, 3);
                if (can) L.hits.push_back({k ? kHitDown : kHitUp, r, st.id});
            }
        }
        // The slot a dragged card would take here.
        int slots = 0;
        if (target) {
            dropCol = &st;
            const bool doneCol = doneType(st.type);
            int idx = 0;
            for (Issue* it : cards) {
                if (it == dragged) continue;
                ++idx;
            }
            float rel = ui.y + ui.grabDY + L.cardH / 2 - listTop;
            dropAt = doneCol ? 0 : std::max(0, std::min(idx, static_cast<int>(rel / (L.cardH + L.gap)) + scroll));
            slots = idx;
        }
        (void)slots;
        int row = 0, shown = 0;
        for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
            Issue* it = cards[i];
            if (i < scroll) continue;
            if (shown >= fit) break;
            ++shown;
            RectF r(cr.X + 8 * s, listTop + row * (L.cardH + L.gap), colW - 16 * s, L.cardH);
            ++row;
            if (it == dragged) {
                strokeRound(gr, r, 8 * s, T->line, 1.5f * s);  // where it was
                continue;
            }
            bool hov = ui.hover && !ui.dragging && r.Contains(ui.x, ui.y) && ui.detail.empty();
            drawCard(gr, f, L, *it, r, hov, it->id == ui.detail);
            L.hits.push_back({kHitCard, r, it->id});
        }
        if (target && dropAt >= 0 && !doneType(st.type)) {  // (target: a pointer, or a hand at the board)
            float y = listTop + (dropAt - scroll) * (L.cardH + L.gap) - L.gap / 2;
            y = std::max(listTop - 2 * s, std::min(y, listTop + listH));
            SolidBrush line(col(T->accent));
            gr.FillRectangle(&line, cr.X + 8 * s, y - 1.5f * s, colW - 16 * s, 3 * s);
        }
    }
    (void)dropCol;
    // The dragged card under the pointer (one in your hand is drawn in the strip below the board instead).
    if (dragged && !ui.inHand) {
        RectF r(ui.x + ui.grabDX, ui.y + ui.grabDY, colW - 16 * s, L.cardH);
        fillRound(gr, RectF(r.X + 6 * s, r.Y + 8 * s, r.Width, r.Height), 10 * s, 0x000000, 110);
        drawCard(gr, f, L, *dragged, r, true, true);
    }
    // The details card of a tapped card: everything about it and a button per column to move it there.
    if (!ui.detail.empty()) {
        Issue* it = issueById(ui.detail);
        if (!it) {
            ui.detail.clear();
        } else {
            SolidBrush shade(col(0x000000, 150));
            gr.FillRectangle(&shade, 0.0f, L.headH, W, H - L.headH);
            L.hits.push_back({kHitBackdrop, RectF(0, L.headH, W, H - L.headH), ""});
            RectF box(W * 0.18f, L.headH + 30 * s, W * 0.64f, H - L.headH - 60 * s);
            L.hits.push_back({kHitPanel, box, ""});  // a tap on the card itself keeps it open
            fillRound(gr, box, 14 * s, T->cork ? kPaper : T->column);
            strokeRound(gr, box, 14 * s, T->line, 1.5f * s);
            const State* st = stateById(it->stateId);
            priorityMark(gr, it->priority, box.X + 30 * s, box.Y + 32 * s, s);
            std::wstring head = it->identifier + (st ? L"  \x00B7  " + st->name : std::wstring());
            text(gr, head, f.body, RectF(box.X + 56 * s, box.Y + 22 * s, box.Width - 140 * s, 36 * s), T->dim);
            RectF close(box.X + box.Width - 60 * s, box.Y + 16 * s, 44 * s, 44 * s);
            bool hc = ui.hover && close.Contains(ui.x, ui.y);
            fillRound(gr, close, 8 * s, hc ? T->cardHover : T->card);
            Pen x(col(T->text), 3 * s);
            gr.DrawLine(&x, close.X + 14 * s, close.Y + 14 * s, close.X + 30 * s, close.Y + 30 * s);
            gr.DrawLine(&x, close.X + 30 * s, close.Y + 14 * s, close.X + 14 * s, close.Y + 30 * s);
            L.hits.push_back({kHitClose, close, ""});
            text(gr, it->title, f.big, RectF(box.X + 30 * s, box.Y + 66 * s, box.Width - 60 * s, 84 * s), T->text, false);
            float lx = box.X + 30 * s;
            for (const Label& l : it->labels) {
                float w = textWidth(gr, l.name, f.small) + 30 * s;
                RectF chip(lx, box.Y + 156 * s, w, 24 * s);
                strokeRound(gr, chip, 12 * s, T->line, 1 * s);
                SolidBrush dot(col(l.colour));
                gr.FillEllipse(&dot, lx + 8 * s, box.Y + 164 * s, 9 * s, 9 * s);
                text(gr, l.name, f.small, RectF(lx + 21 * s, box.Y + 156 * s, w - 24 * s, 24 * s), T->dim);
                lx += w + 8 * s;
            }
            // Move-to buttons along the bottom.
            const float bh = 46 * s, by = box.Y + box.Height - bh - 26 * s;
            const float descTop = box.Y + 196 * s;
            std::wstring desc = it->description.empty() ? std::wstring(L"No description.") : it->description;
            text(gr, desc, f.body, RectF(box.X + 30 * s, descTop, box.Width - 60 * s, by - 50 * s - descTop), T->dim, false);
            text(gr, L"Move to", f.small, RectF(box.X + 30 * s, by - 34 * s, 200 * s, 26 * s), T->dim);
            float bx = box.X + 30 * s;
            const float bw = std::min(200 * s, (box.Width - 60 * s - (cols.size() - 1) * 10 * s) / std::max<size_t>(1, cols.size()));
            for (const State* c : cols) {
                RectF b(bx, by, bw, bh);
                const bool here = c->id == it->stateId;
                bool hov = ui.hover && !here && b.Contains(ui.x, ui.y);
                fillRound(gr, b, 8 * s, here ? (T->cork ? 0xE4DCCB : T->column) : hov ? T->cardHover : T->card);
                strokeRound(gr, b, 8 * s, here ? T->accent : T->line, (here ? 2 : 1) * s);
                SolidBrush dot(col(c->colour));
                gr.FillEllipse(&dot, b.X + 12 * s, b.Y + bh / 2 - 6 * s, 12 * s, 12 * s);
                text(gr, c->name, f.small, RectF(b.X + 30 * s, b.Y, b.Width - 36 * s, bh), here ? T->dim : T->text);
                if (!here) L.hits.push_back({kHitMoveTo, b, c->id});
                bx += bw + 10 * s;
            }
        }
    }
    // A short note along the bottom (a refused move, a conflict).
    if (!ui.toast.empty() && now() < ui.toastUntil) {
        float w = std::min(W - 80 * s, textWidth(gr, ui.toast, f.body) + 48 * s);
        RectF r((W - w) / 2, H - 76 * s, w, 50 * s);
        fillRound(gr, r, 10 * s, 0x2B2D3A);
        strokeRound(gr, r, 10 * s, T->accent, 1.5f * s);
        text(gr, ui.toast, f.body, r, T->text, true, StringAlignmentCenter);
    }
}

const Hit* hitAt(const Layout& L, float x, float y, HitKind skip = kHitNone) {
    // Last laid out is on top.
    for (auto it = L.hits.rbegin(); it != L.hits.rend(); ++it)
        if (it->kind != skip && it->r.Contains(x, y)) return &*it;
    return nullptr;
}

// Moves a card here (optimistic) and queues it for Linear (caller holds g.mutex).
void moveCard(Issue& it, const State& to, int index) {
    const State* from = stateById(it.stateId);
    const bool doneCol = doneType(to.type);
    std::vector<Issue*> cards = cardsOf(to);
    cards.erase(std::remove(cards.begin(), cards.end(), &it), cards.end());
    Move m;
    m.issueId = it.id;
    m.stateId = to.id;
    m.baseUpdatedAt = it.updatedAt;
    m.identifier = it.identifier;
    m.queuedAt = now();
    // Linear's manual order: lower sortOrder is higher up. Between the two cards it lands between.
    if (!doneCol && index >= 0 && !cards.empty()) {
        const int n = static_cast<int>(cards.size());
        index = std::max(0, std::min(index, n));
        if (index == 0) m.sortOrder = cards[0]->sortOrder - 1;
        else if (index == n) m.sortOrder = cards[n - 1]->sortOrder + 1;
        else m.sortOrder = (cards[index - 1]->sortOrder + cards[index]->sortOrder) / 2;
        m.hasSort = true;
    }
    if (from == &to && !m.hasSort) return;  // nothing changes
    if (from == &to && m.hasSort && std::fabs(m.sortOrder - it.sortOrder) < 1e-9) return;
    it.stateId = to.id;
    if (m.hasSort) it.sortOrder = m.sortOrder;
    if (doneCol && (!from || !doneType(from->type))) it.completedAt = "9999";  // newest in its column until refreshed
    if (!doneCol) it.completedAt.clear();
    it.syncing = true;
    g.local[it.id] = {it.stateId, it.completedAt, it.sortOrder, m.hasSort, now()};
    g.moves.push_back(m);
    ++g.version;
    g.wake.notify_one();
    LOG("Linear board: card moved to a %s column (%s)", to.type.c_str(), from == &to ? "new place" : "new column");
}

// One pointer event (caller holds g.mutex).
// A dragged card let go at x y: into the column there (or the nearest), at the slot there (caller holds g.mutex).
void dropCard(const Layout& L, float x, float y) {
    Ui& ui = g.ui;
    Issue* it = issueById(ui.pressIssue);
    const Hit* col = hitAt(L, x, y, kHitCard);
    if (col && col->kind != kHitColumn) col = nullptr;
    // Nearest column by x (a drop in a gap counts).
    if (!col) {
        float best = 1e9f;
        for (const Hit& h : L.hits) {
            if (h.kind != kHitColumn) continue;
            float d = std::fabs(h.r.X + h.r.Width / 2 - x);
            if (d < best && d < h.r.Width) best = d, col = &h;
        }
    }
    const State* to = col ? stateById(col->id) : nullptr;
    if (it && to) {
        const float listTop = col->r.Y + L.colHeadH;
        int index = static_cast<int>((y + ui.grabDY + L.cardH / 2 - listTop) / (L.cardH + L.gap)) + ui.scroll[to->id];
        moveCard(*it, *to, index);
    }
}

// A hand taking a card off the board, moving it, letting it go (caller holds g.mutex).
void handleHand(const Layout& L, const Event& e) {
    Ui& ui = g.ui;
    if (e.kind == 1) {
        const Hit* h = hitAt(L, e.x, e.y);
        if (!h || h->kind != kHitCard || !ui.detail.empty() || ui.inHand) return;
        ui.pressed = ui.dragging = ui.inHand = ui.atBoard = true;
        ui.pressIssue = h->id;
        ui.grabDX = h->r.X - e.x;
        ui.grabDY = h->r.Y - e.y;
        ui.x = e.x;
        ui.y = e.y;
        ++g.version;
        LOG("Linear board: a card taken off the board by hand");
        return;
    }
    if (!ui.inHand) return;
    const bool atBoard = e.buttons != 0;
    if (e.kind == 2) {
        if (atBoard != ui.atBoard || (atBoard && std::fabs(ui.x - e.x) + std::fabs(ui.y - e.y) > 0.5f)) ++g.version;
        ui.x = e.x;
        ui.y = e.y;
        ui.atBoard = atBoard;
        return;
    }
    // Let go: pinned where the hand is when it is at the board, else it goes back where it was.
    if (atBoard) dropCard(L, e.x, e.y);
    else toast(L"Put back (let go away from the board)", 2000);
    ui.pressed = ui.dragging = ui.inHand = ui.atBoard = false;
    ui.pressIssue.clear();
    ++g.version;
}

void handle(const Layout& L, const Event& e) {
    if (e.kind != 0) return handleHand(L, e);
    Ui& ui = g.ui;
    const bool down = e.buttons & 1, was = ui.buttons & 1;
    ui.buttons = e.buttons;
    const float oldX = ui.x, oldY = ui.y;
    if (ui.inHand) return;  // a card in your hand: the laser and fingers wait
    ui.x = e.x;
    ui.y = e.y;
    ui.hover = true;
    if (ui.dragging && std::fabs(oldX - e.x) + std::fabs(oldY - e.y) > 0.5f) ++g.version;
    {
        const Hit* h = hitAt(L, e.x, e.y);
        int idx = h ? static_cast<int>(h - L.hits.data()) : -1;
        if (idx != ui.hoverHit) {
            ui.hoverHit = idx;
            ++g.version;
        }
    }
    if (down && !was) {
        ui.pressed = true;
        ui.pressX = e.x;
        ui.pressY = e.y;
        ui.pressIssue.clear();
        ui.pressButton = -1;
        const Hit* h = hitAt(L, e.x, e.y);
        if (h && h->kind == kHitCard && ui.detail.empty()) {
            ui.pressIssue = h->id;
            ui.grabDX = h->r.X - e.x;
            ui.grabDY = h->r.Y - e.y;
        }
    } else if (down && ui.pressed && !ui.dragging && !ui.pressIssue.empty()) {
        if (std::fabs(e.x - ui.pressX) + std::fabs(e.y - ui.pressY) > 14 * L.s) {
            ui.dragging = true;
            ++g.version;
        }
    } else if (!down && was && ui.pressed) {
        ui.pressed = false;
        if (ui.dragging) {
            ui.dragging = false;
            dropCard(L, e.x, e.y);
            ui.pressIssue.clear();
            ++g.version;
            return;
        }
        // A tap: what it was pressed on and let go on.
        const Hit* h = hitAt(L, e.x, e.y);
        if (!h) return;
        switch (h->kind) {
            case kHitCard:
                if (ui.detail.empty()) ui.detail = h->id;
                break;
            case kHitClose:
            case kHitBackdrop: ui.detail.clear(); break;
            case kHitUp: --ui.scroll[h->id]; break;
            case kHitDown: ++ui.scroll[h->id]; break;
            case kHitRefresh:
                g.refreshWanted = true;
                g.wake.notify_one();
                toast(L"Refreshing\x2026", 1500);
                break;
            case kHitMoveTo: {
                Issue* it = issueById(ui.detail);
                const State* to = stateById(h->id);
                if (it && to) moveCard(*it, *to, 0);
                ui.detail.clear();
                break;
            }
            default: break;
        }
        ++g.version;
    }
}

// The board thread: pointer events in, a new picture whenever something shown changed.
void uiThread() {
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        LOG("Linear board: GDI+ did not start; no board");
        return;
    }
    Fonts fonts;
    Layout layout;
    int drawnVersion = -1;
    std::wstring drawnStatus;
    ULONGLONG drawnAt = 0;
    for (;;) {
        std::unique_lock<std::mutex> lock(g.mutex);
        g.uiWake.wait_for(lock, std::chrono::milliseconds(g.ui.dragging ? 16 : 100));
        if (!g.on) {
            g.ui = Ui{};
            std::lock_guard<std::mutex> el(g.eventMutex);
            g.events.clear();
            continue;
        }
        // Pointer events since last time.
        std::deque<Event> events;
        {
            std::lock_guard<std::mutex> el(g.eventMutex);
            events.swap(g.events);
        }
        for (const Event& e : events) handle(layout, e);
        // Nothing pointed at it for a moment: no hover, and a held press lets go where it was.
        if (g.ui.hover && now() - g.lastPointer.load() > 300) {
            if ((g.ui.buttons & 1) && !g.ui.inHand) handle(layout, {g.ui.x, g.ui.y, 0});
            g.ui.hover = false;
            ++g.version;
        }
        if (!g.ui.toast.empty() && now() >= g.ui.toastUntil) {
            g.ui.toast.clear();
            ++g.version;
        }
        // The "synced N ago" text changes on its own.
        std::wstring stamp = g.status == Status::kOk || g.status == Status::kOffline ? agoText(now() - g.syncedAt) : L"";
        if (g.version == drawnVersion && stamp == drawnStatus) continue;
        if (g.ui.dragging && now() - drawnAt < 33) continue;  // a dragged card: 30 pictures a second at most
        const ULONGLONG unseen = now() - g.viewedAt.load();
        if (unseen > 2000 && g.frameW > 0 && drawnVersion >= 0) continue;  // nobody looks: drawn when looked at again
        drawnVersion = g.version;
        drawnStatus = stamp;
        drawnAt = now();
        // The picture: the screen's (or the wall art's) shape, [linear] width pixels across. On the cork board a
        // strip below the board holds the card in your hand (13b_tv_screen.inc shows it in your hand).
        T = g.cork ? &kCorkTheme : &kDarkTheme;
        float aspect = g.v[10] > 0 ? g.v[9] / g.v[10] : 16.0f / 9.0f;
        if (g.cork) {
            aspect = std::max(0.8f, std::min(2.6f, aspect));  // wall art: its own shape, upright too
        } else {
            if (aspect < 1) aspect = 1 / aspect;  // a portrait screen gets it turned (13b_tv_screen.inc)
            aspect = std::max(1.25f, std::min(2.4f, aspect));
        }
        const int W = std::max(960, std::min(3840, g.width)), H = std::max(400, static_cast<int>(W / aspect + 0.5f));
        layout.W = static_cast<float>(W);
        layout.H = static_cast<float>(H);
        layout.s = std::min(layout.H, layout.W / aspect) / 1080.0f;
        if (layout.W / layout.s < 1500) layout.s = layout.W / 1500.0f;  // narrow: smaller text
        fonts.make(layout.s);
        const int strip = g.cork ? static_cast<int>(112 * layout.s + 24) : 0;
        const int Htot = H + strip;
        Bitmap bmp(W, Htot, PixelFormat32bppARGB);
        Issue* held = g.ui.inHand ? issueById(g.ui.pressIssue) : nullptr;
        {
            Graphics gr(&bmp);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            gr.SetPixelOffsetMode(PixelOffsetModeHalf);
            drawBoard(gr, fonts, layout);
            if (strip) {
                gr.SetClip(RectF(0, static_cast<REAL>(H), static_cast<REAL>(W), static_cast<REAL>(strip)));
                gr.Clear(col(0x000000, 0));
                if (held) drawCard(gr, fonts, layout, *held, RectF(12, H + 12.0f, layout.cardW, layout.cardH), false, false, 255, true);
                gr.ResetClip();
            }
        }
        {
            std::lock_guard<std::mutex> el(g.eventMutex);
            Board::Shown& sh = g.shown;
            sh.boardW = layout.W;
            sh.boardH = layout.H;
            sh.cardW = layout.cardW;
            sh.cardH = layout.cardH;
            const float ht = static_cast<float>(Htot);
            sh.boardCrop[0] = 0, sh.boardCrop[1] = 0, sh.boardCrop[2] = 1, sh.boardCrop[3] = layout.H / ht;
            sh.heldCrop[0] = 12 / layout.W, sh.heldCrop[1] = (layout.H + 12) / ht;
            sh.heldCrop[2] = (12 + layout.cardW) / layout.W, sh.heldCrop[3] = (layout.H + 12 + layout.cardH) / ht;  // x0 y0 x1 y1
            sh.held = held != nullptr;
            sh.detail = !g.ui.detail.empty();
            sh.cards.clear();
            for (const Hit& h : layout.hits)
                if (h.kind == kHitCard) sh.cards.emplace_back(h.r, h.id);
        }
        lock.unlock();
        BitmapData data{};
        Rect all(0, 0, W, Htot);
        if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) == Ok) {
            std::vector<uint32_t> px(static_cast<size_t>(W) * Htot);
            for (int y = 0; y < Htot; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data.Scan0) + y * data.Stride);
                uint32_t* out = px.data() + static_cast<size_t>(y) * W;
                for (int x = 0; x < W; ++x) out[x] = y < H ? row[x] | 0xFF000000u : row[x];  // the strip keeps its see-through
            }
            bmp.UnlockBits(&data);
            std::lock_guard<std::mutex> fl(g.frameMutex);
            g.frame.swap(px);
            g.frameW = W;
            g.frameH = Htot;
            g.boardH = H;
            g.picW = W;
            g.picH = H;
            ++g.frameSeq;
        }
    }
}

}  // namespace

void set(int on, const float v[11], const char* team, const char* options, int pollSeconds, int width) {
    bool wasOn = g.on;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (on && v) std::memcpy(g.v, v, sizeof(g.v));
        if (on) {
            if (team && *team) g.teamKey = team;
            g.pollSeconds = std::max(5, std::min(600, pollSeconds));
            g.width = width > 0 ? width : 1920;
            // options: "done_days=14;canceled=0;mode=screen|cork"
            std::string o = options ? options : "";
            for (size_t a = 0; a < o.size();) {
                size_t b = o.find(';', a);
                if (b == std::string::npos) b = o.size();
                std::string kv = o.substr(a, b - a);
                size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    std::string k = kv.substr(0, eq), val = kv.substr(eq + 1);
                    if (k == "done_days") g.doneDays = std::max(0, std::min(365, std::atoi(val.c_str())));
                    if (k == "canceled") g.showCanceled = std::atoi(val.c_str()) != 0;
                    if (k == "mode") g.cork = val == "cork";
                }
                a = b + 1;
            }
            ++g.version;
        }
        if (on && !g.threads) {
            g.threads = true;
            std::thread([] { g_net.run(); }).detach();
            std::thread(uiThread).detach();
        }
        if (on && !wasOn) {
            g.refreshWanted = true;
            g.viewedAt = now();
            std::lock_guard<std::mutex> fl(g.frameMutex);
            g.frameW = g.frameH = 0;
        }
    }
    g.on = on != 0;
    if (on != static_cast<int>(wasOn)) LOG("Linear board: %s", on ? "on a screen" : "off the screen");
    g.wake.notify_one();
    g.uiWake.notify_one();
}

bool on() { return g.on; }

bool screen(float v[11]) {
    if (!g.on) return false;
    std::lock_guard<std::mutex> lock(g.mutex);
    std::memcpy(v, g.v, sizeof(g.v));
    return true;
}

bool frameSize(int& w, int& h) {
    std::lock_guard<std::mutex> lock(g.frameMutex);
    w = g.frameW;
    h = g.boardH;
    return w > 0 && h > 0;
}

bool cork() { return g.on && g.cork; }

void boardCrop(float crop[4]) {
    std::lock_guard<std::mutex> lock(g.eventMutex);
    std::memcpy(crop, g.shown.boardCrop, sizeof(float) * 4);
}

bool heldCard(float crop[4], float size[2]) {
    std::lock_guard<std::mutex> lock(g.eventMutex);
    if (!g.shown.held) return false;
    std::memcpy(crop, g.shown.heldCrop, sizeof(float) * 4);
    size[0] = g.shown.cardW / g.shown.boardW;
    size[1] = g.shown.cardH / g.shown.boardH;
    return true;
}

bool grab(float u, float v, float at[2]) {
    if (!g.on || !g.cork) return false;
    std::lock_guard<std::mutex> lock(g.eventMutex);
    const Board::Shown& sh = g.shown;
    if (sh.held || sh.detail) return false;
    const float x = u * sh.boardW, y = v * sh.boardH;
    for (const auto& c : sh.cards) {
        // A little margin: a hand is not a mouse.
        const float m = 0.02f * sh.boardW;
        if (x < c.first.X - m || x > c.first.X + c.first.Width + m || y < c.first.Y - m || y > c.first.Y + c.first.Height + m)
            continue;
        const float cx = std::max(c.first.X, std::min(x, c.first.X + c.first.Width)),
                    cy = std::max(c.first.Y, std::min(y, c.first.Y + c.first.Height));
        at[0] = (cx - c.first.X) / c.first.Width;
        at[1] = (cy - c.first.Y) / c.first.Height;
        g.events.push_back({cx, cy, 1, 1});
        g.uiWake.notify_one();
        return true;
    }
    return false;
}

void hold(float u, float v, bool atBoard) {
    std::lock_guard<std::mutex> lock(g.eventMutex);
    Event e{u * g.shown.boardW, v * g.shown.boardH, atBoard ? 1 : 0, 2};
    if (!g.events.empty() && g.events.back().kind == 2) g.events.back() = e;
    else g.events.push_back(e);
    g.uiWake.notify_one();
}

void drop(float u, float v, bool atBoard) {
    std::lock_guard<std::mutex> lock(g.eventMutex);
    g.events.push_back({u * g.shown.boardW, v * g.shown.boardH, atBoard ? 1 : 0, 3});
    g.uiWake.notify_one();
}

ID3D11ShaderResourceView* frameView(ID3D11DeviceContext* context) {
    if (!context) return nullptr;
    if (now() - g.viewedAt.load() > 2000) g.uiWake.notify_one();  // looked at again: drawn now
    g.viewedAt = now();
    std::lock_guard<std::mutex> lock(g.frameMutex);
    int w = g.frameW, h = g.frameH;
    if (w <= 0 || h <= 0) return nullptr;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return nullptr;
    device->Release();  // the context holds it
    if (device != g.device || w != g.texW || h != g.texH) {
        if (g.view) g.view->Release();
        if (g.texture) g.texture->Release();
        g.view = nullptr;
        g.texture = nullptr;
        g.device = device;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 0;  // a full chain: text seen from across the room stays readable (as claudescreen.cpp)
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &g.texture)) ||
            FAILED(device->CreateShaderResourceView(g.texture, nullptr, &g.view))) {
            if (g.texture) g.texture->Release();
            g.texture = nullptr;
            static int failed = 0;
            if (failed++ < 3) LOG("Linear board: no %dx%d texture", w, h);
            return nullptr;
        }
        g.texW = w;
        g.texH = h;
        g.uploadedSeq = -1;
    }
    if (g.frameSeq != g.uploadedSeq) {
        context->UpdateSubresource(g.texture, 0, nullptr, g.frame.data(), w * 4, 0);
        context->GenerateMips(g.view);
        g.uploadedSeq = g.frameSeq;
    }
    return g.view;
}

void pointer(float u, float v, int buttons) {
    if (!g.on) return;
    const int pw = g.picW.load(), ph = g.picH.load();
    const float W = static_cast<float>(pw > 0 ? pw : 1920), H = static_cast<float>(ph > 0 ? ph : 1080);
    Event e{std::max(0.0f, std::min(1.0f, u)) * W, std::max(0.0f, std::min(1.0f, v)) * H, buttons};
    g.lastPointer = now();
    std::lock_guard<std::mutex> lock(g.eventMutex);
    // Moves fold into the last one; presses and releases are each kept.
    if (!g.events.empty() && g.events.back().kind == 0 && g.events.back().buttons == buttons) g.events.back() = e;
    else g.events.push_back(e);
    if (g.events.size() > 128) g.events.pop_front();
    g.uiWake.notify_one();
}

}  // namespace s4vr::linearboard
