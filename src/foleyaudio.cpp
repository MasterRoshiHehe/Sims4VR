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
    if (k == kLightSwitch) return L"objects";  // sounds\objects\lightswitch*.wav
    if (k == kKnockDoor) return L"knock\\door";
    if (k == kKnockWindow) return L"knock\\window";
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
// The phone's own ringtones (user 2026-10-28): sounds\\ringtones\\*.wav, in name order, kind kRingBase + n.
// Where the phone is, in the headset's frame (metres: +x right, +y up, +z behind), set every frame by the VR code: the
// ringtones and notification sounds are streamed and re-placed there every 10 ms, so they stay at the phone as your
// head and hand move (user 2026-10-29: ALWAYS 3D).
std::atomic<float> g_phoneAt[3] = {{0.18f}, {-0.55f}, {0.0f}};
std::atomic<ULONGLONG> g_phoneAtTick{0};
constexpr int kRingBase = 1000, kStopRings = -2, kMaxRings = 8;
std::vector<std::vector<int16_t>> g_rings;  // written once on the audio thread before g_loaded
std::atomic<int> g_ringCount{0};
// Notification sounds the same way: sounds\\notifications\\*.wav, kind kNoteBase + n (one-shots, [foley] volume_ringtone too).
constexpr int kNoteBase = 2000;
std::vector<std::vector<int16_t>> g_notes;
std::atomic<int> g_noteCount{0};

// The elevator ride (user 2026-10-29): sounds\\elevator\\*.wav (the first), streamed like the phone but placed at the car's
// ceiling, where Python / the VR code say (setElevatorAt, headset frame), kind kElevBase.
constexpr int kElevBase = 3000, kStopElev = -3;
std::vector<std::vector<int16_t>> g_elev;
std::atomic<int> g_elevCount{0};
std::atomic<float> g_elevAt[3] = {{0.0f}, {0.9f}, {0.0f}};
std::atomic<ULONGLONG> g_elevAtTick{0};
std::atomic<float> g_elevVolume{1.0f};  // [foley] volume_elevator
std::atomic<bool> g_elevPlaying{false}, g_elevAnchorPending{false};

// [foley] settings, read again every 2 s on the audio thread.
std::atomic<bool> g_on{true}, g_own{true};
std::atomic<float> g_patSpeed{0.5f}, g_slapSpeed{2.2f}, g_punchSpeed{1.8f}, g_volume{1.0f}, g_knockback{1.0f}, g_knockSpeed{0.2f};
std::atomic<bool> g_walkHits{true};
// Shoves (open palm pushed forward into the torso/shoulders, armik.cpp foleyTrack).
std::atomic<bool> g_shoveOn{true};
std::atomic<float> g_shoveSpeed{1.3f}, g_shoveRatio{0.7f}, g_shoveForce{1.0f}, g_shoveFall{2.8f}, g_shoveLimp{3.0f};
std::atomic<float> g_kindVolume[kKinds];
std::atomic<float> g_phoneHighpass{600.0f};  // [foley] phone_highpass: Hz, 0 = off (small phone speaker)
std::atomic<float> g_ringVolume{1.0f};  // [foley] volume_ringtone
std::atomic<float> g_objTapSpeed{0.8f};
std::atomic<bool> g_objects{true};
// A kind's volume key: each body kind its own, objects one per hit (all materials).
const char* volumeKey(int k) {
    static const char* const body[kObjects] = {"volume_touch", "volume_pat", "volume_slap", "volume_punch", "volume_grab"};
    static const char* const hits[kObjHits] = {"volume_object_tap", "volume_object_slap", "volume_object_punch"};
    if (k == kLightSwitch) return "volume_lightswitch";
    if (k == kKnockDoor || k == kKnockWindow) return "volume_knock";
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

// A WAV (PCM 8/16/24/32-bit or float, any channels and rate) made mono 16-bit at kRate.
bool loadWav(const std::wstring& path, std::vector<int16_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> data(GetFileSize(f, nullptr));
    DWORD got = 0;
    bool ok = !data.empty() && ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size();
    CloseHandle(f);
    if (!ok || data.size() < 12 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4)) return false;
    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    for (size_t at = 12; at + 8 <= data.size();) {
        uint32_t size;
        memcpy(&size, data.data() + at + 4, 4);
        const uint8_t* body = data.data() + at + 8;
        if (at + 8 + size > data.size()) size = static_cast<uint32_t>(data.size() - at - 8);
        if (!memcmp(data.data() + at, "fmt ", 4) && size >= 16) {
            memcpy(&tag, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
            if (tag == 0xFFFE && size >= 26) memcpy(&tag, body + 24, 2);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!memcmp(data.data() + at, "data", 4) && channels && rate) {
            bool isFloat = tag == 3 && bits == 32;
            if (!(tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !isFloat) return false;
            size_t bytes = bits / 8, frames = size / (bytes * channels);
            if (!frames) return false;
            std::vector<float> mono(frames);
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0;
                for (int c = 0; c < channels; ++c) {
                    const uint8_t* q = body + (i * channels + c) * bytes;
                    float v;
                    if (isFloat) {
                        memcpy(&v, q, 4);
                    } else if (bits == 8) {
                        v = (q[0] - 128) / 128.0f;
                    } else if (bits == 16) {
                        int16_t t;
                        memcpy(&t, q, 2);
                        v = t / 32768.0f;
                    } else if (bits == 24) {
                        int32_t t = (q[0] << 8) | (q[1] << 16) | (q[2] << 24);
                        v = t / 2147483648.0f;
                    } else {
                        int32_t t;
                        memcpy(&t, q, 4);
                        v = t / 2147483648.0f;
                    }
                    sum += v;
                }
                mono[i] = sum / channels;
            }
            size_t n = rate == kRate ? frames : static_cast<size_t>(static_cast<double>(frames) * kRate / rate);
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                double pos = rate == kRate ? static_cast<double>(i) : static_cast<double>(i) * rate / kRate;
                size_t i0 = static_cast<size_t>(pos);
                size_t i1 = i0 + 1 < frames ? i0 + 1 : i0;
                float frac = static_cast<float>(pos - static_cast<double>(i0));
                float v = mono[i0] + (mono[i1] - mono[i0]) * frac;
                v = v > 1 ? 1 : v < -1 ? -1 : v;
                out[i] = static_cast<int16_t>(v * 32767.0f);
            }
            return !out.empty();
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

void loadAll() {
    int total = 0;
    for (int k = 0; k < kKinds; ++k) {
        // The light switch click: sounds\objects\lightswitch*.wav or any wav in sounds\objects\lightswitch\.
        int places = k == kLightSwitch ? 2 : 1;
        for (int place = 0; place < places; ++place) {
            std::wstring folder = g_dir + L"\\sounds\\" + folderOf(k) + L"\\";
            const wchar_t* pattern = L"*.wav";
            if (k == kLightSwitch) {
                if (place == 0) pattern = L"lightswitch*.wav";
                else folder += L"lightswitch\\";
            }
            WIN32_FIND_DATAW fd;
            HANDLE find = FindFirstFileW((folder + pattern).c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE) continue;
            do {
                std::vector<int16_t> s;
                if (loadWav(folder + fd.cFileName, s))
                    g_sounds[k].push_back(std::move(s));
                else
                    LOG("Touch sounds: %ls%ls is not a readable WAV, left out", folder.c_str(), fd.cFileName);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        g_count[k] = static_cast<int>(g_sounds[k].size());
        total += g_count[k];
    }
    {
        const std::wstring folder = g_dir + L"\\sounds\\ringtones\\";
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"*.wav").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName);
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const std::wstring& n : names) {
            std::vector<int16_t> r;
            if (g_rings.size() < static_cast<size_t>(kMaxRings) && loadWav(folder + n, r))
                g_rings.push_back(std::move(r));
            else
                LOG("Ringtones: %ls%ls is not a readable WAV (or more than %d), left out", folder.c_str(), n.c_str(), kMaxRings);
        }
        g_ringCount = static_cast<int>(g_rings.size());
        LOG("Ringtones: %d in sounds\\ringtones", g_ringCount.load());
    }
    {
        const std::wstring folder = g_dir + L"\\sounds\\elevator\\";
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"*.wav").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName);
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const std::wstring& n : names) {
            std::vector<int16_t> r;
            if (g_elev.size() < 4 && loadWav(folder + n, r))
                g_elev.push_back(std::move(r));
            else
                LOG("Elevator: %ls%ls is not a readable WAV (or more than 4), left out", folder.c_str(), n.c_str());
        }
        g_elevCount = static_cast<int>(g_elev.size());
        LOG("Elevator: %d sound(s) in sounds\\elevator (WAV only: convert Elevator.mp3 to Elevator.wav)", g_elevCount.load());
    }
    {
        const std::wstring folder = g_dir + L"\\sounds\\notifications\\";
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((folder + L"*.wav").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do names.push_back(fd.cFileName);
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const std::wstring& n : names) {
            std::vector<int16_t> r;
            if (g_notes.size() < static_cast<size_t>(kMaxRings) && loadWav(folder + n, r)) {
                // A notification sound is cut to 4 s, the last half second fading out.
                const size_t cut = static_cast<size_t>(4) * kRate, fade = kRate / 2;
                const bool cutShort = r.size() > cut;  // short ones (a click) play whole, no fade
                if (cutShort) r.resize(cut);
                for (size_t i = 0; cutShort && i < fade; ++i) {
                    const size_t at = r.size() - 1 - i;
                    r[at] = static_cast<int16_t>(r[at] * (static_cast<float>(i) / fade));
                }
                g_notes.push_back(std::move(r));
            }
            else
                LOG("Notification sounds: %ls%ls is not a readable WAV (or more than %d), left out", folder.c_str(), n.c_str(), kMaxRings);
        }
        g_noteCount = static_cast<int>(g_notes.size());
        LOG("Notification sounds: %d in sounds\\notifications", g_noteCount.load());
    }
    g_loaded = true;
    LOG("Knock sounds: %d door (sounds\\knock\\door), %d window (sounds\\knock\\window)", g_count[kKnockDoor].load(),
        g_count[kKnockWindow].load());
    int objects = 0;
    for (int k = kObjects; k < kLightSwitch; ++k) objects += g_count[k];
    LOG("Touch sounds: your own: %d touch, %d pat, %d slap, %d punch, %d grab, %d for objects (%d in all, sounds\\touch, "
        "sounds\\objects), %d light switch",
        g_count[kTouch].load(), g_count[kPat].load(), g_count[kSlap].load(), g_count[kPunch].load(),
        g_count[kGrab].load(), objects, total, g_count[kLightSwitch].load());
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
    g_shoveOn = settingNumber(ini, "shove", 1) != 0;
    g_shoveSpeed = settingNumber(ini, "shove_speed", 1.3f);
    g_shoveRatio = settingNumber(ini, "shove_ratio", 0.7f);
    g_shoveForce = settingNumber(ini, "shove_force", 1.0f);
    g_shoveFall = settingNumber(ini, "shove_fall_speed", 2.8f);
    g_shoveLimp = settingNumber(ini, "shove_limp", 3.0f);
    g_objects = settingNumber(ini, "objects", 1) != 0;
    g_objTapSpeed = settingNumber(ini, "object_tap_speed", 0.8f);
    g_knockSpeed = settingNumber(ini, "knock", 1) != 0 ? settingNumber(ini, "knock_speed", 0.2f) : 0.0f;
    float k = settingNumber(ini, "knockback", 1.0f);
    g_knockback = k < 0 ? 0 : k > 2 ? 2 : k;
    float v = settingNumber(ini, "own_volume", 1.0f);
    g_volume = v < 0 ? 0 : v > 2 ? 2 : v;
    v = settingNumber(ini, "volume_ringtone", 1.0f);
    g_ringVolume = v < 0 ? 0 : v > 2 ? 2 : v;
    v = settingNumber(ini, "phone_highpass", 600.0f);
    g_phoneHighpass = v < 0 ? 0 : v > 4000 ? 4000 : v;
    v = settingNumber(ini, "volume_elevator", 1.0f);
    g_elevVolume = v < 0 ? 0 : v > 2 ? 2 : v;
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
    // A streamed sound (a ringtone, a notification sound): rendered in 10 ms chunks for where the phone is now.
    static constexpr int kChunk = kRate / 100, kChunks = 4;
    struct Ear {
        float delay = 0, gain = 1, alpha = 1;  // samples, level, low-pass coefficient
    };
    struct Stream {
        IXAudio2SourceVoice* v = nullptr;
        const std::vector<int16_t>* src = nullptr;
        double pos = 0;  // the source sample the next chunk starts at
        bool active = false, started = false;
        Ear left, right;
        float lpL = 0, lpR = 0;
        float hpL[2] = {0, 0}, hpR[2] = {0, 0};  // the phone speaker's high-pass (two one-pole stages per ear)
        float fallback[3] = {0, 0, -0.3f};
        float buf[kChunks][kChunk * 2];
        int next = 0;
    } streams[3];  // 0: ringtone, 1: notification sound, 2: the elevator ride
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

    // The ears for a source at `at` (the same model as binaural(): the far ear later, quieter, duller).
    static void ears(const float at[3], float gain, Ear& l, Ear& r) {
        float x = at[0], y = at[1], z = at[2];
        float d = std::sqrt(x * x + y * y + z * z);
        if (!(d > 0.05f)) x = 0, y = 0, z = -0.3f, d = 0.3f;
        const float side = std::max(-1.0f, std::min(1.0f, x / d));
        const float lateral = std::asin(side);
        const float itd = 0.0875f / 343.0f * (std::fabs(lateral) + std::fabs(std::sin(lateral)));
        const float delay = itd * kRate;
        const float closeness = d < 1.0f ? (1.0f - d) : 0.0f;
        const float shadow = std::fabs(side);
        const float farGain = 1.0f - shadow * (0.35f + 0.35f * closeness), nearGain = 1.0f + shadow * 0.15f;
        float distance = 0.5f / (d < 0.25f ? 0.25f : d);
        distance = distance > 1.6f ? 1.6f : distance < 0.15f ? 0.15f : distance;
        const float behind = z > 0 ? z / d : 0;
        auto alpha = [](float hz) { return 1.0f - std::exp(-6.2831853f * hz / kRate); };
        const float farHz = 20000.0f - shadow * (18200.0f - 800.0f * closeness), backHz = 20000.0f - behind * 13000.0f;
        const float aFar = alpha(std::min(farHz, backHz)), aNear = alpha(backHz);
        const bool rightNear = side >= 0;
        Ear& nearEar = rightNear ? r : l;  // (near and far are macros in windows.h)
        Ear& farEar = rightNear ? l : r;
        nearEar = {0, nearGain * gain * distance, aNear};
        farEar = {delay, farGain * gain * distance, aFar};
    }

    static float sampleAt(const std::vector<int16_t>& src, double i) {
        if (i < 0) return 0;
        const size_t i0 = static_cast<size_t>(i);
        if (i0 >= src.size()) return 0;
        const size_t i1 = i0 + 1 < src.size() ? i0 + 1 : i0;
        const float frac = static_cast<float>(i - static_cast<double>(i0));
        return (src[i0] + (src[i1] - src[i0]) * frac) / 32768.0f;
    }

    void startStream(int slot, const std::vector<int16_t>* src, const float at[3], float gain, double startPos = 0) {
        Stream& st = streams[slot];
        st.hpL[0] = st.hpL[1] = st.hpR[0] = st.hpR[1] = 0;
        if (!st.v) {
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            wf.nChannels = 2;
            wf.nSamplesPerSec = kRate;
            wf.wBitsPerSample = 32;
            wf.nBlockAlign = 8;
            wf.nAvgBytesPerSec = kRate * 8;
            if (FAILED(xa->CreateSourceVoice(&st.v, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr))) {
                st.v = nullptr;
                return;
            }
            float matrix[2 * 8] = {};
            UINT32 outs = outChannels > 8 ? 8 : outChannels;
            if (outs >= 2) matrix[0 * 2 + 0] = matrix[1 * 2 + 1] = 1;
            else matrix[0] = matrix[1] = 0.5f;
            st.v->SetOutputMatrix(master, 2, outs, matrix);
        }
        st.v->Stop(0);
        st.v->FlushSourceBuffers();
        st.src = src;
        st.pos = startPos;
        st.active = true;
        st.started = false;
        st.lpL = st.lpR = 0;
        st.next = 0;
        std::memcpy(st.fallback, at, sizeof st.fallback);
        streamGain[slot] = gain;
        pump();
    }
    float streamGain[3] = {1, 1, 1};

    void stopRings() {
        Stream& st = streams[0];
        if (st.v) {
            st.v->Stop(0);
            st.v->FlushSourceBuffers();
        }
        st.active = false;
    }

    void stopElevator() {
        Stream& st = streams[2];
        if (st.v) {
            st.v->Stop(0);
            st.v->FlushSourceBuffers();
        }
        st.active = false;
        g_elevPlaying = false;
    }

    bool streaming() const { return streams[0].active || streams[1].active || streams[2].active; }

    // Fills each active stream's queue (about 30 ms ahead) with chunks placed at the phone as it is now.
    void pump() {
        const ULONGLONG now = GetTickCount64();
        for (int k = 0; k < 3; ++k) {
            Stream& st = streams[k];
            if (!st.active || !st.v || !st.src) continue;
            XAUDIO2_VOICE_STATE state{};
            st.v->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.pos >= static_cast<double>(st.src->size()) + 64 && state.BuffersQueued == 0) {
                st.active = false;
                if (k == 2) g_elevPlaying = false;
                continue;
            }
            float at[3] = {st.fallback[0], st.fallback[1], st.fallback[2]};
            if (k == 2) {
                if (now - g_elevAtTick.load() < 300)
                    for (int c = 0; c < 3; ++c) at[c] = g_elevAt[c].load();
            } else if (now - g_phoneAtTick.load() < 300)
                for (int c = 0; c < 3; ++c) at[c] = g_phoneAt[c].load();
            Ear tl, tr;
            ears(at, streamGain[k], tl, tr);
            if (!st.started) st.left = tl, st.right = tr;  // the first chunk starts where it is
            const float hpHz = g_phoneHighpass.load();
            const float hpA = hpHz > 0 ? 1.0f - std::exp(-6.2831853f * hpHz / kRate) : 0.0f;
            UINT32 queued = state.BuffersQueued;
            while (queued < static_cast<UINT32>(kChunks - 1) && st.pos < static_cast<double>(st.src->size()) + 64) {
                float* out = st.buf[st.next];
                for (int i = 0; i < kChunk; ++i) {
                    const float t = static_cast<float>(i + 1) / kChunk;
                    const Ear l{st.left.delay + (tl.delay - st.left.delay) * t, st.left.gain + (tl.gain - st.left.gain) * t,
                                st.left.alpha + (tl.alpha - st.left.alpha) * t};
                    const Ear r{st.right.delay + (tr.delay - st.right.delay) * t, st.right.gain + (tr.gain - st.right.gain) * t,
                                st.right.alpha + (tr.alpha - st.right.alpha) * t};
                    const double at0 = st.pos + i;
                    st.lpL += l.alpha * (sampleAt(*st.src, at0 - l.delay) * l.gain - st.lpL);
                    st.lpR += r.alpha * (sampleAt(*st.src, at0 - r.delay) * r.gain - st.lpR);
                    float yl = st.lpL, yr = st.lpR;
                    if (hpA > 0 && k < 2) {  // ringtones and notification sounds only; the elevator is not the phone
                        for (int s = 0; s < 2; ++s) {
                            st.hpL[s] += hpA * (yl - st.hpL[s]);
                            yl -= st.hpL[s];
                            st.hpR[s] += hpA * (yr - st.hpR[s]);
                            yr -= st.hpR[s];
                        }
                    }
                    out[i * 2] = yl;
                    out[i * 2 + 1] = yr;
                }
                st.left = tl, st.right = tr;
                st.pos += kChunk;
                XAUDIO2_BUFFER b{};
                b.AudioBytes = kChunk * 2 * sizeof(float);
                b.pAudioData = reinterpret_cast<const BYTE*>(out);
                if (FAILED(st.v->SubmitSourceBuffer(&b))) break;
                st.next = (st.next + 1) % kChunks;
                ++queued;
                if (!st.started) st.v->Start(0), st.started = true;
            }
        }
    }

    void play(const Request& r) {
        if (r.kind == kStopRings) {
            stopRings();
            return;
        }
        if (r.kind == kStopElev) {
            stopElevator();
            return;
        }
        if (r.kind >= kElevBase) {
            if (g_elevCount.load() <= 0 || !open()) return;
            startStream(2, &g_elev[0], r.at, g_volume * g_elevVolume.load(), static_cast<double>(r.strength) * kRate);
            g_elevPlaying = true;
            return;
        }
        const bool note = r.kind >= kNoteBase, ring = r.kind >= kRingBase && !note;
        int n = note ? g_noteCount.load() : ring ? g_ringCount.load() : g_count[r.kind].load();
        if (n <= 0 || !open()) return;
        if (note ? r.kind - kNoteBase >= n : ring && r.kind - kRingBase >= n) return;
        int which = 0;
        if (note || ring) {
            // Streamed, so it follows the phone (pump); a ringtone replaces the one before it.
            const int slot = note ? 1 : 0;
            which = note ? r.kind - kNoteBase : r.kind - kRingBase;
            startStream(slot, note ? &g_notes[which] : &g_rings[which], r.at, g_volume * g_ringVolume.load());
            return;
        } else {
            pick = pick * 1664525u + 1013904223u;
            which = static_cast<int>((pick >> 8) % static_cast<uint32_t>(n));
            if (n > 1 && which == last[r.kind]) which = (which + 1) % n;
            last[r.kind] = which;
        }
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
        WaitForSingleObject(g_wake, player.streaming() ? 10 : 2000);
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
        player.pump();
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
bool isShove(bool fist, float into, float along) {
    return g_shoveOn && !fist && into >= g_shoveSpeed && along <= into * g_shoveRatio;
}
float shoveForce() { return g_shoveForce; }
float shoveFallSpeed() { return g_shoveFall; }
float shoveLimp() { return g_shoveLimp; }
float knockSpeed() { return g_knockSpeed; }
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

bool playRing(int n, const float at[3], bool notification) {
    if (n < 0 || n >= (notification ? g_noteCount.load() : g_ringCount.load()) || !g_wake) return false;
    AcquireSRWLockExclusive(&g_lock);
    bool room = g_queued < 16;
    if (room) {
        g_queue[(g_head + g_queued) % 16] = {(notification ? kNoteBase : kRingBase) + n, {at[0], at[1], at[2]}, 1.0f};
        ++g_queued;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (room) SetEvent(g_wake);
    return room;
}

void stopRing() {
    if (!g_wake) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_queued < 16) {
        g_queue[(g_head + g_queued) % 16] = {kStopRings, {0, 0, 0}, 0};
        ++g_queued;
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
}

int ringCount() { return g_ringCount; }

bool playElevator(const float at[3], float fromSeconds) {
    if (g_elevCount.load() <= 0 || !g_wake) return false;
    g_elevAt[0] = at[0], g_elevAt[1] = at[1], g_elevAt[2] = at[2];
    g_elevAnchorPending = true;  // the VR code fixes it above your head now
    AcquireSRWLockExclusive(&g_lock);
    bool room = g_queued < 16;
    if (room) {
        g_queue[(g_head + g_queued) % 16] = {kElevBase, {at[0], at[1], at[2]}, fromSeconds};
        ++g_queued;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (room) SetEvent(g_wake);
    return room;
}

void stopElevator() {
    if (!g_wake) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_queued < 16) {
        g_queue[(g_head + g_queued) % 16] = {kStopElev, {0, 0, 0}, 0};
        ++g_queued;
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
    g_elevPlaying = false;
}

bool elevatorPlaying() { return g_elevPlaying.load(); }
bool takeElevatorAnchor() { return g_elevAnchorPending.exchange(false); }

void setElevatorAt(const float at[3]) {
    for (int c = 0; c < 3; ++c) g_elevAt[c] = at[c];
    g_elevAtTick = GetTickCount64();
}

void setPhoneAt(const float at[3]) {
    for (int c = 0; c < 3; ++c) g_phoneAt[c] = at[c];
    g_phoneAtTick = GetTickCount64();
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
