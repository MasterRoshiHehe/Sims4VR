// Sims Forever: Sims 1's sound placed at the in-game computer (user
// 2026-10-14 18:30Z: "it's sound should come from within the sims 4s spacial
// sound engine"). Sims 1 plays through Windows like any program; its audio
// session (found by its process id on every playing device) gets a left and a
// right volume from where the computer's screen is around your head: quieter
// with distance, panned to the side it's on, a little duller behind you. The
// Present thread sets the target (setEars); a thread of its own talks to
// Windows (COM) and glides there, so nothing waits on the audio service.
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "log.h"
#include "simsforever.h"

namespace s4vr::simsforever {
namespace {

std::atomic<float> g_left{1.0f}, g_right{1.0f};
std::atomic<uint32_t> g_pid{0};
std::atomic<bool> g_thread{false};

struct Session {
    IChannelAudioVolume* volume;
    UINT channels;
};

void release(std::vector<Session>& sessions) {
    for (auto& s : sessions) s.volume->Release();
    sessions.clear();
}

// Every audio session of process pid on every playing device.
void find(DWORD pid, std::vector<Session>& sessions) {
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
            IAudioSessionControl2* control2 = nullptr;
            if (FAILED(list->GetSession(i, &control))) continue;
            DWORD owner = 0;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2)))) {
                control2->GetProcessId(&owner);
                control2->Release();
            }
            IChannelAudioVolume* volume = nullptr;
            UINT channels = 0;
            if (owner == pid &&
                SUCCEEDED(control->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void**>(&volume)))) {
                if (SUCCEEDED(volume->GetChannelCount(&channels)) && channels > 0)
                    sessions.push_back({volume, channels});
                else
                    volume->Release();
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

DWORD WINAPI soundThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<Session> sessions;
    DWORD pid = 0;
    ULONGLONG lookedAt = 0;
    float left = 1, right = 1;
    int found = -1;
    for (;;) {
        DWORD want = g_pid.load();
        if (want != pid) {
            release(sessions);
            pid = want;
            lookedAt = 0;
            found = -1;
            left = right = 1;
        }
        if (!pid) {
            Sleep(250);
            continue;
        }
        // Sims 1 opens its sound some time after it starts: look again every second until found.
        if (sessions.empty() && GetTickCount64() - lookedAt > 1000) {
            lookedAt = GetTickCount64();
            find(pid, sessions);
            if (static_cast<int>(sessions.size()) != found) {
                found = static_cast<int>(sessions.size());
                if (found) LOG("Sims Forever: Sims 1's sound found (%d session(s)), placed at the computer", found);
            }
        }
        float l = g_left.load(), r = g_right.load();
        left += (l - left) * 0.35f;  // a glide, no clicks
        right += (r - right) * 0.35f;
        for (size_t i = 0; i < sessions.size();) {
            auto& s = sessions[i];
            bool ok = true;
            for (UINT c = 0; c < s.channels && ok; ++c) {
                // Stereo: 0 left, 1 right; more channels: even left, odd right.
                float g = s.channels == 1 ? 0.5f * (left + right) : (c % 2 ? right : left);
                ok = SUCCEEDED(s.volume->SetChannelVolume(c, std::fmax(0.0f, std::fmin(1.0f, g)), nullptr));
            }
            if (ok) {
                ++i;
            } else {
                s.volume->Release();  // the session ended: looked for again
                sessions.erase(sessions.begin() + i);
            }
        }
        Sleep(30);
    }
}

}  // namespace

void placeSound(uint32_t pid, float left, float right) {
    g_left = left;
    g_right = right;
    g_pid = pid;
    if (pid && !g_thread.exchange(true)) CreateThread(nullptr, 0, soundThread, nullptr, 0, nullptr);
}

}  // namespace s4vr::simsforever
