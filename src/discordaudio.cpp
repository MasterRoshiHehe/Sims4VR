#include "discordaudio.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

// The level meter (endpointvolume.h isn't in this toolchain's headers): only GetPeakValue is used.
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

namespace s4vr::discordaudio {
namespace {

const GUID kIidMeter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

std::atomic<int> g_mode{0};
std::atomic<bool> g_shown{false};
std::atomic<float> g_left{1.0f}, g_right{1.0f};
std::atomic<bool> g_thread{false};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_done{false};

struct Session {
    ISimpleAudioVolume* simple;
    IChannelAudioVolume* channels;
    UINT count;
    IAudioMeterInformation* meter;  // for the log: which session carries the sound
    std::string name;               // "pid 1234 Discord.exe, display name, instance id (state)"
};


void release(std::vector<Session>& sessions) {
    for (auto& s : sessions) {
        if (s.simple) s.simple->Release();
        if (s.channels) s.channels->Release();
        if (s.meter) s.meter->Release();
    }
    sessions.clear();
}

// A program of Discord's (Discord.exe, DiscordPTB.exe, DiscordCanary.exe).
bool isDiscord(DWORD pid, std::string* image = nullptr) {
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    bool yes = false;
    if (QueryFullProcessImageNameW(process, 0, path, &n)) {
        std::wstring name(path, n);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        if (image) image->assign(name.begin(), name.end());
        yes = name.size() >= 11 && name.compare(0, 7, L"discord") == 0 && name.compare(name.size() - 4, 4, L".exe") == 0;
    }
    CloseHandle(process);
    return yes;
}

// Every audio session of Discord's programs on every playing device.
void find(std::vector<Session>& sessions) {
    release(sessions);
    IMMDeviceEnumerator* devices = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&devices))))
        return;
    IMMDeviceCollection* all = nullptr;
    UINT count = 0;
    if (SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) all->GetCount(&count);
    for (UINT d = 0; d < count; ++d) {
        IMMDevice* device = nullptr;
        if (FAILED(all->Item(d, &device))) continue;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* list = nullptr;
        int n = 0;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&manager))) &&
            SUCCEEDED(manager->GetSessionEnumerator(&list)))
            list->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(list->GetSession(i, &control))) continue;
            IAudioSessionControl2* control2 = nullptr;
            DWORD owner = 0;
            std::string info;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2)))) {
                control2->GetProcessId(&owner);
                LPWSTR id = nullptr;
                if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&id)) && id) {
                    std::wstring w(id);
                    CoTaskMemFree(id);
                    // after the last '|' : the instance part; keep it short
                    size_t bar = w.find(L'|');
                    info += " id=" + std::string(w.begin() + (bar == std::wstring::npos ? 0 : bar + 1), w.end());
                }
                control2->Release();
            }
            std::string image;
            if (isDiscord(owner, &image)) {
                Session s{};
                AudioSessionState st = AudioSessionStateInactive;
                control->GetState(&st);
                LPWSTR dn = nullptr;
                std::string display;
                if (SUCCEEDED(control->GetDisplayName(&dn)) && dn) {
                    std::wstring w(dn);
                    CoTaskMemFree(dn);
                    display.assign(w.begin(), w.end());
                }
                char head[160];
                snprintf(head, sizeof(head), "pid %lu %s device %u %s", static_cast<unsigned long>(owner), image.c_str(), d,
                         st == AudioSessionStateActive ? "active" : st == AudioSessionStateInactive ? "inactive" : "expired");
                s.name = std::string(head) + (display.empty() ? "" : " name=" + display) + info;
                control->QueryInterface(kIidMeter, reinterpret_cast<void**>(&s.meter));
                control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&s.simple));
                if (SUCCEEDED(control->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void**>(&s.channels))) &&
                    FAILED(s.channels->GetChannelCount(&s.count))) {
                    s.channels->Release();
                    s.channels = nullptr;
                }
                if (s.simple || s.channels)
                    sessions.push_back(s);
            }
            control->Release();
        }
        if (list) list->Release();
        if (manager) manager->Release();
        device->Release();
    }
    if (all) all->Release();
    devices->Release();
}

// Written while we hold Discord muted, deleted when given back: a game that crashed meanwhile leaves Discord muted, and
// the next start sees the mark and gives it back (a mute you made yourself has no mark and stays).
std::wstring markPath() {
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp, n) + L"sims4vr_discord_muted";
}
void setMark(bool on) {
    std::wstring path = markPath();
    if (on) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    } else {
        DeleteFileW(path.c_str());
    }
}
bool marked() { return GetFileAttributesW(markPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

// What we last asked of the sessions, so Discord is only talked to when it changes.
struct Applied {
    bool muted = false;
    float left = 1, right = 1;
    bool ours = false;  // we changed something
};

void apply(std::vector<Session>& sessions, bool mute, bool setMute, float left, float right, Applied& a) {
    for (auto& s : sessions) {
        if (s.simple && setMute) s.simple->SetMute(mute ? TRUE : FALSE, nullptr);
        if (s.channels) {
            for (UINT c = 0; c < s.count; ++c) {
                float g = s.count == 1 ? 0.5f * (left + right) : (c % 2 ? right : left);
                s.channels->SetChannelVolume(c, std::fmax(0.0f, std::fmin(1.0f, g)), nullptr);
            }
        }
    }
    a.muted = mute;
    a.left = left;
    a.right = right;
}

DWORD WINAPI soundThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<Session> sessions;
    ULONGLONG lookedAt = 0;
    float left = 1, right = 1;
    bool muted = false, touched = false, wasCrashed = marked();
    int found = -1;
    float appliedL = 1, appliedR = 1;
    bool fresh = false;
    while (!g_quit) {
        int mode = g_mode.load();
        bool wantMute = false;
        float tl = 1, tr = 1;
        if (mode) {
            bool shown = g_shown.load();
            wantMute = !shown && (mode & 1);
            if (shown && (mode & 2)) {
                tl = g_left.load();
                tr = g_right.load();
            }
        }
        bool active = mode != 0 || touched;
        if (!active) {
            Sleep(250);
            continue;
        }
        // Discord opens its sound when something plays: look again every second (sessions come and go).
        if (GetTickCount64() - lookedAt > 1000) {
            lookedAt = GetTickCount64();
            find(sessions);
            if (static_cast<int>(sessions.size()) != found) {
                found = static_cast<int>(sessions.size());
                LOG("Discord sound: %d session(s) found", found);
                for (auto& s : sessions) LOG("Discord sound:   %s", s.name.c_str());
            }
            if (wasCrashed && !sessions.empty()) {
                // A mute from a game that didn't end properly.
                wasCrashed = false;
                for (auto& s : sessions)
                    if (s.simple) s.simple->SetMute(FALSE, nullptr);
                setMark(false);
                LOG("Discord sound: gave Discord's sound back (it was left muted)");
            }
            fresh = true;  // new sessions, not yet told
            touched = touched || !sessions.empty();
        }
        // Off: everything back to normal, then idle.
        if (!mode) {
            wantMute = false;
            tl = tr = 1;
        }
        float l = tl, r = tr;
        left += (l - left) * 0.35f;  // a glide, no clicks
        right += (r - right) * 0.35f;
        if (std::fabs(l - left) < 0.004f) left = l;
        if (std::fabs(r - right) < 0.004f) right = r;
        if (!sessions.empty()) {
            if (fresh || wantMute != muted || std::fabs(left - appliedL) > 0.001f || std::fabs(right - appliedR) > 0.001f) {
                Applied a;
                apply(sessions, wantMute, wantMute || muted, left, right, a);
                if (wantMute != muted) {
                    LOG("Discord sound: %s", wantMute ? "muted (not on a screen)" : "on");
                    setMark(wantMute);
                }
                muted = wantMute;
                appliedL = left;
                appliedR = right;
                fresh = false;
            }
            if (!mode && !wantMute && left == 1 && right == 1) {
                touched = false;  // given back
                LOG("Discord sound: left alone");
            }
        }
        Sleep(30);
    }
    // Process exit: give the sound back.
    find(sessions);
    for (auto& s : sessions) {
        if (s.simple) s.simple->SetMute(FALSE, nullptr);
        if (s.channels)
            for (UINT c = 0; c < s.count; ++c) s.channels->SetChannelVolume(c, 1.0f, nullptr);
    }
    release(sessions);
    setMark(false);
    g_done = true;
    return 0;
}

}  // namespace

void setMode(int mode) {
    g_mode = mode;
    if (mode && !g_thread.exchange(true)) CreateThread(nullptr, 0, soundThread, nullptr, 0, nullptr);
}

void setEars(bool shown, float left, float right) {
    g_shown = shown;
    g_left = left;
    g_right = right;
}

void shutdown() {
    if (!g_thread) return;
    g_mode = 0;
    g_quit = true;
    for (int i = 0; i < 60 && !g_done; ++i) Sleep(10);
}

}  // namespace s4vr::discordaudio
