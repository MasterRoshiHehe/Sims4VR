#include "foleyaudio.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "settings.h"

namespace s4vr::foleyaudio {
namespace {

const wchar_t* const kBodyFolders[kObjects] = {L"touch", L"pat", L"slap", L"punch", L"grab"};
const wchar_t* const kMaterialFolders[kMaterials] = {L"hard", L"metal", L"soft", L"glass", L"wall"};
const wchar_t* const kHitFolders[kObjHits] = {L"tap", L"slap", L"punch"};

// A kind's folder under sounds\.
std::wstring folderOf(int k) {
    if (k < kObjects) return std::wstring(L"touch\\") + kBodyFolders[k];
    k -= kObjects;
    return std::wstring(L"objects\\") + kMaterialFolders[k / kObjHits] + L"\\" + kHitFolders[k % kObjHits];
}
constexpr int kRate = 48000;
constexpr int kVoices = 8;

std::wstring g_dir;
std::vector<std::vector<int16_t>> g_sounds[kKinds];  // written once on the audio thread before g_loaded
std::atomic<int> g_count[kKinds];
std::atomic<bool> g_loaded{false};

// [foley] settings, read again every 2 s on the audio thread.
std::atomic<bool> g_on{true}, g_own{true};
std::atomic<float> g_patSpeed{0.5f}, g_slapSpeed{2.2f}, g_punchSpeed{1.8f}, g_volume{1.0f}, g_knockback{1.0f};
std::atomic<bool> g_walkHits{true};
std::atomic<float> g_kindVolume[kKinds];
std::atomic<float> g_objTapSpeed{0.8f};
std::atomic<bool> g_objects{true};
// A kind's volume key: each body kind its own, objects one per hit (all materials).
const char* volumeKey(int k) {
    static const char* const body[kObjects] = {"volume_touch", "volume_pat", "volume_slap", "volume_punch", "volume_grab"};
    static const char* const hits[kObjHits] = {"volume_object_tap", "volume_object_slap", "volume_object_punch"};
    return k < kObjects ? body[k] : hits[(k - kObjects) % kObjHits];
}

struct Request {
    int kind;
    float at[3], strength;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Request g_queue[16];
int g_head = 0, g_queued = 0;
HANDLE g_wake = nullptr;
std::atomic<bool> g_started{false};

// A mono 16-bit WAV at kRate (the build makes them so); anything else is skipped.
bool loadWav(const std::wstring& path, std::vector<int16_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> data(GetFileSize(f, nullptr));
    DWORD got = 0;
    bool ok = !data.empty() && ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size();
    CloseHandle(f);
    if (!ok || data.size() < 12 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4)) return false;
    bool format = false;
    for (size_t at = 12; at + 8 <= data.size();) {
        uint32_t size;
        memcpy(&size, data.data() + at + 4, 4);
        const uint8_t* body = data.data() + at + 8;
        if (at + 8 + size > data.size()) size = static_cast<uint32_t>(data.size() - at - 8);
        if (!memcmp(data.data() + at, "fmt ", 4) && size >= 16) {
            uint16_t tag, channels, bits;
            uint32_t rate;
            memcpy(&tag, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
            format = tag == WAVE_FORMAT_PCM && channels == 1 && rate == kRate && bits == 16;
        } else if (!memcmp(data.data() + at, "data", 4) && format) {
            out.resize(size / 2);
            memcpy(out.data(), body, out.size() * 2);
            return !out.empty();
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

void loadAll() {
    int total = 0;
    for (int k = 0; k < kKinds; ++k) {
        std::wstring folder = g_dir + L"\\sounds\\" + folderOf(k) + L"\\";
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"*.wav").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            std::vector<int16_t> s;
            if (loadWav(folder + fd.cFileName, s))
                g_sounds[k].push_back(std::move(s));
            else
                LOG("Touch sounds: %ls%ls is not a 48 kHz mono 16-bit WAV, left out", folder.c_str(), fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        g_count[k] = static_cast<int>(g_sounds[k].size());
        total += g_count[k];
    }
    g_loaded = true;
    int objects = 0;
    for (int k = kObjects; k < kKinds; ++k) objects += g_count[k];
    LOG("Touch sounds: your own: %d touch, %d pat, %d slap, %d punch, %d grab, %d for objects (%d in all, sounds\\touch, "
        "sounds\\objects)",
        g_count[kTouch].load(), g_count[kPat].load(), g_count[kSlap].load(), g_count[kPunch].load(),
        g_count[kGrab].load(), objects, total);
}

float settingNumber(const std::wstring& ini, const char* key, float def) {
    std::string v;
    if (!settings::liveValue("foley", key, v)) v = readSetting(ini, "foley", key, "");
    char* end = nullptr;
    float f = v.empty() ? def : std::strtof(v.c_str(), &end);
    return end == v.c_str() || !std::isfinite(f) ? def : f;
}

void readSettings() {
    std::wstring ini = g_dir + L"\\sims4vr.ini";
    g_on = settingNumber(ini, "on", 1) != 0;
    g_own = settingNumber(ini, "own_sounds", 1) != 0;
    g_patSpeed = settingNumber(ini, "pat_speed", 0.5f);
    g_slapSpeed = settingNumber(ini, "slap_speed", 2.2f);
    g_punchSpeed = settingNumber(ini, "punch_speed", 1.8f);
    g_walkHits = settingNumber(ini, "walk_hits", 1) != 0;
    g_objects = settingNumber(ini, "objects", 1) != 0;
    g_objTapSpeed = settingNumber(ini, "object_tap_speed", 0.8f);
    float k = settingNumber(ini, "knockback", 1.0f);
    g_knockback = k < 0 ? 0 : k > 2 ? 2 : k;
    float v = settingNumber(ini, "own_volume", 1.0f);
    g_volume = v < 0 ? 0 : v > 2 ? 2 : v;
    for (int k = 0; k < kKinds; ++k) {
        v = settingNumber(ini, volumeKey(k), 1.0f);
        g_kindVolume[k] = v < 0 ? 0 : v > 2 ? 2 : v;
    }
}

// One sound made binaural for where it is (`at`, metres in the headset's frame: +x right,
// +y up, +z behind): the far ear hears it later (head width, Woodworth), quieter and
// duller (head shadow, a one-pole low-pass), more so the closer it is; behind you both
// ears a little duller; further than half a metre quieter. Interleaved stereo floats.
void binaural(const std::vector<int16_t>& in, const float at[3], float gain, std::vector<float>& out) {
    float x = at[0], y = at[1], z = at[2];
    float d = std::sqrt(x * x + y * y + z * z);
    if (!(d > 0.05f)) {
        x = 0, y = 0, z = -0.3f;  // unknown: in front
        d = 0.3f;
    }
    float side = x / d;  // -1 left .. 1 right
    float lateral = std::asin(side < -1 ? -1 : side > 1 ? 1 : side);
    float itd = 0.0875f / 343.0f * (std::fabs(lateral) + std::fabs(std::sin(lateral)));  // seconds
    int delay = static_cast<int>(itd * kRate + 0.5f);
    float closeness = d < 1.0f ? (1.0f - d) : 0.0f;  // close sounds: bigger level difference between the ears
    float shadow = std::fabs(side);
    float farGain = 1.0f - shadow * (0.35f + 0.35f * closeness);
    float nearGain = 1.0f + shadow * 0.15f;
    float distance = 0.5f / (d < 0.25f ? 0.25f : d);
    distance = distance > 1.6f ? 1.6f : distance < 0.15f ? 0.15f : distance;
    // Low-pass coefficients (one pole): the far ear down to ~1.8 kHz at the side, behind both to ~7 kHz.
    float behind = z > 0 ? z / d : 0;
    auto alpha = [](float hz) { return 1.0f - std::exp(-6.2831853f * hz / kRate); };
    float farHz = 20000.0f - shadow * (18200.0f - 800.0f * closeness);
    float backHz = 20000.0f - behind * 13000.0f;
    float aFar = alpha(std::min(farHz, backHz)), aNear = alpha(backHz);
    bool rightNear = side >= 0;
    out.assign((in.size() + delay) * 2, 0.0f);
    float lpNear = 0, lpFar = 0;
    const float scale = gain * distance / 32768.0f;
    for (size_t i = 0; i < in.size() + delay; ++i) {
        float nearIn = i < in.size() ? in[i] * scale : 0;
        float farIn = i >= static_cast<size_t>(delay) && i - delay < in.size() ? in[i - delay] * scale : 0;
        lpNear += aNear * (nearIn - lpNear);
        lpFar += aFar * (farIn - lpFar);
        out[i * 2 + (rightNear ? 1 : 0)] = lpNear * nearGain;
        out[i * 2 + (rightNear ? 0 : 1)] = lpFar * farGain;
    }
}

using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

struct Player {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice[kVoices] = {};
    std::vector<float> pcm[kVoices];  // what each voice plays (kept while it plays)
    ULONGLONG startedAt[kVoices] = {};
    UINT32 outChannels = 2;
    bool failed = false;
    uint32_t pick = 0x9e3779b9;  // which file of a kind (not the same twice in a row)
    int last[kKinds];
    Player() { std::fill(std::begin(last), std::end(last), -1); }

    bool open() {
        if (xa || failed) return xa != nullptr;
        failed = true;
        HMODULE lib = LoadLibraryW(L"xaudio2_9.dll");
        if (!lib) lib = LoadLibraryW(L"xaudio2_8.dll");
        auto create = lib ? reinterpret_cast<CreateFn>(GetProcAddress(lib, "XAudio2Create")) : nullptr;
        if (!create || FAILED(create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !xa) {
            LOG("Touch sounds: XAudio2 isn't there (%s): your own sounds can't play", lib ? "no XAudio2Create" : "no xaudio2_9.dll");
            xa = nullptr;
            return false;
        }
        if (FAILED(xa->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
                                            nullptr, AudioCategory_GameEffects))) {
            LOG("Touch sounds: no speakers for XAudio2");
            xa->Release();
            xa = nullptr;
            return false;
        }
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        outChannels = details.InputChannels ? details.InputChannels : 2;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        for (auto& v : voice)
            if (FAILED(xa->CreateSourceVoice(&v, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr))) v = nullptr;
        failed = false;
        LOG("Touch sounds: XAudio2 open (%u speaker channels, %d sounds at once)", outChannels, kVoices);
        return true;
    }

    void play(const Request& r) {
        int n = g_count[r.kind];
        if (n <= 0 || !open()) return;
        pick = pick * 1664525u + 1013904223u;
        int which = static_cast<int>((pick >> 8) % static_cast<uint32_t>(n));
        if (n > 1 && which == last[r.kind]) which = (which + 1) % n;
        last[r.kind] = which;
        const std::vector<int16_t>& s = g_sounds[r.kind][which];
        // A free voice, else the one playing longest.
        int use = -1;
        ULONGLONG oldest = ~0ull;
        for (int i = 0; i < kVoices; ++i) {
            if (!voice[i]) continue;
            XAUDIO2_VOICE_STATE st{};
            voice[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (!st.BuffersQueued) {
                use = i;
                break;
            }
            if (startedAt[i] < oldest) {
                oldest = startedAt[i];
                use = i;
            }
        }
        if (use < 0) return;
        IXAudio2SourceVoice* v = voice[use];
        v->Stop(0);
        v->FlushSourceBuffers();
        float gain = g_volume * g_kindVolume[r.kind] * (0.55f + 0.45f * (r.strength < 0 ? 0 : r.strength > 1 ? 1 : r.strength));
        binaural(s, r.at, gain, pcm[use]);
        // Left to the front left speaker, right to the front right (headphones: the two ears).
        float matrix[2 * 8] = {};
        UINT32 outs = outChannels > 8 ? 8 : outChannels;
        if (outs >= 2) {
            matrix[0 * 2 + 0] = 1;  // matrix[out * sourceChannels + in]
            matrix[1 * 2 + 1] = 1;
        } else {
            matrix[0] = matrix[1] = 0.5f;
        }
        v->SetOutputMatrix(master, 2, outs, matrix);
        XAUDIO2_BUFFER b{};
        b.Flags = XAUDIO2_END_OF_STREAM;
        b.AudioBytes = static_cast<UINT32>(pcm[use].size() * sizeof(float));
        b.pAudioData = reinterpret_cast<const BYTE*>(pcm[use].data());
        if (FAILED(v->SubmitSourceBuffer(&b))) return;
        v->Start(0);
        startedAt[use] = GetTickCount64();
    }
};

DWORD WINAPI audioThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    readSettings();
    loadAll();
    Player player;
    ULONGLONG readAt = GetTickCount64();
    for (;;) {
        WaitForSingleObject(g_wake, 2000);
        ULONGLONG now = GetTickCount64();
        if (now - readAt >= 2000) {
            readAt = now;
            readSettings();
        }
        for (;;) {
            Request r;
            AcquireSRWLockExclusive(&g_lock);
            bool any = g_queued > 0;
            if (any) {
                r = g_queue[g_head];
                g_head = (g_head + 1) % 16;
                --g_queued;
            }
            ReleaseSRWLockExclusive(&g_lock);
            if (!any) break;
            player.play(r);
        }
    }
}

}  // namespace

void init(const std::wstring& modDir) {
    if (g_started.exchange(true)) return;
    g_dir = modDir;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = g_wake ? CreateThread(nullptr, 0, audioThread, nullptr, 0, nullptr) : nullptr;
    if (t)
        CloseHandle(t);
    else
        LOG("Touch sounds: no audio thread: your own sounds can't play");
}

int hitKind(bool fist, float into) {
    if (fist) return into >= g_punchSpeed ? kPunch : -1;
    return into >= g_slapSpeed ? kSlap : -1;
}

float belowHits(float into) {
    float top = (g_slapSpeed < g_punchSpeed ? g_slapSpeed : g_punchSpeed) * 0.95f;
    return into < top ? into : top;
}

bool walkHits() { return g_walkHits; }
float knockback() { return g_knockback; }

int objectHit(bool fist, float into) {
    if (!g_on || !g_objects) return -1;
    if (fist && into >= g_punchSpeed) return kObjPunch;
    if (!fist && into >= g_slapSpeed) return kObjSlap;
    return into >= g_objTapSpeed ? kObjTap : -1;
}

int objectKind(int material, int hit) {
    if (!g_loaded || !g_own || hit < 0 || hit >= kObjHits || material < 0 || material >= kMaterials) return -1;
    int kind = kObjects + material * kObjHits + hit;
    if (g_count[kind] > 0) return kind;
    kind = kObjects + hit;  // hard's
    return g_count[kind] > 0 ? kind : -1;
}

int kindFor(bool grab, bool fist, bool theirHand, float into) {
    if (!g_loaded || !g_on || !g_own) return -1;
    int kind = -1;
    if (grab)
        kind = kGrab;
    else if (theirHand && into >= g_patSpeed)
        return -1;  // a high five: foley.py's
    else if (hitKind(fist, into) >= 0)
        kind = hitKind(fist, into);
    else if (into >= g_patSpeed)
        kind = kPat;
    else
        kind = kTouch;
    return g_count[kind] > 0 ? kind : -1;
}

bool play(int kind, const float at[3], float strength) {
    if (kind < 0 || kind >= kKinds || !g_wake || g_count[kind] <= 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    bool room = g_queued < 16;
    if (room) {
        g_queue[(g_head + g_queued) % 16] = {kind, {at[0], at[1], at[2]}, strength};
        ++g_queued;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (room) SetEvent(g_wake);
    return room;
}

}  // namespace s4vr::foleyaudio
