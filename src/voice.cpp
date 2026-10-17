#include "voice.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <thread>

#include "armik.h"
#include "log.h"

namespace s4vr::voice {

namespace {

// COM ids, spelled out so nothing extra has to be linked.
const CLSID kEnumeratorClass = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const IID kEnumeratorIid = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const IID kAudioClientIid = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const IID kCaptureClientIid = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
const GUID kFloatFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kPcmFormat = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

Settings g_settings;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_sim{0};
std::atomic<bool> g_firstPerson{false};  // the mouth moves in first person only; god view stays the game's
std::atomic<bool> g_quit{false};
std::atomic<bool> g_flowOpen{false};
std::atomic<uint64_t> g_flowFrames{0}, g_flowLive{0};
HANDLE g_wake = nullptr;
std::thread g_thread;

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring nameOf(IMMDevice* device) {
    std::wstring name;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            name = value.pwszVal;
        PropVariantClear(&value);
        store->Release();
    }
    return name;
}

std::wstring idOf(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(device->GetId(&raw)) && raw) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
}

// The active mic whose name contains `part` (any case); every mic is logged once.
IMMDevice* findByName(IMMDeviceEnumerator* devices, const std::wstring& part, bool list) {
    if (part.empty() && !list) return nullptr;
    IMMDeviceCollection* all = nullptr;
    if (FAILED(devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all))) return nullptr;
    IMMDevice* found = nullptr;
    UINT count = 0;
    all->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(all->Item(i, &device))) continue;
        std::wstring name = nameOf(device);
        if (list) LOG("Voice: mic %u: \"%s\"", i + 1, utf8(name).c_str());
        if (!found && !part.empty() && lower(name).find(part) != std::wstring::npos) {
            found = device;
            if (!list) break;
        } else {
            device->Release();
        }
    }
    all->Release();
    return found;
}

enum class Sample { Float32, Int16, Int32, Unknown };

struct Capture {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    std::wstring id;
    bool byDefault = true;  // Windows' default input, not one chosen by name
    Sample sample = Sample::Unknown;
    UINT channels = 1, rate = 48000;

    bool open() const { return capture != nullptr; }
    void close() {
        if (client) client->Stop();
        release(capture);
        release(client);
        id.clear();
    }
};

const char* sampleName(Sample s) {
    switch (s) {
        case Sample::Float32: return "float";
        case Sample::Int16: return "16-bit";
        case Sample::Int32: return "32-bit";
        default: return "unknown";
    }
}

bool openMic(IMMDeviceEnumerator* devices, Capture& cap) {
    static bool listed = false;
    static bool missingLogged = false;
    std::wstring wanted;
    if (!g_settings.device.empty()) {
        int n = MultiByteToWideChar(CP_ACP, 0, g_settings.device.c_str(), -1, nullptr, 0);
        wanted.resize(n > 0 ? n - 1 : 0);
        if (n > 1) MultiByteToWideChar(CP_ACP, 0, g_settings.device.c_str(), -1, &wanted[0], n);
        wanted = lower(wanted);
    }
    IMMDevice* device = findByName(devices, wanted, !listed);
    listed = true;
    cap.byDefault = device == nullptr;
    if (!device && !wanted.empty() && !missingLogged) {
        missingLogged = true;
        LOG("Voice: no mic named like \"%s\" (mic_device); using Windows' default input", g_settings.device.c_str());
    }
    if (!device && FAILED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) {
        LOG("Voice: Windows has no default input (no mic plugged in or enabled)");
        return false;
    }
    cap.id = idOf(device);
    std::string name = utf8(nameOf(device));
    HRESULT hr = device->Activate(kAudioClientIid, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap.client));
    device->Release();
    if (FAILED(hr)) {
        LOG("Voice: can't open \"%s\" (0x%08lx)%s", name.c_str(), static_cast<unsigned long>(hr),
            hr == E_ACCESSDENIED ? ": Windows privacy settings block desktop apps from the microphone" : "");
        cap.close();
        return false;
    }
    WAVEFORMATEX* format = nullptr;
    if (FAILED(cap.client->GetMixFormat(&format)) || !format) {
        cap.close();
        return false;
    }
    cap.channels = format->nChannels ? format->nChannels : 1;
    cap.rate = format->nSamplesPerSec ? format->nSamplesPerSec : 48000;
    bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const GUID& sub = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat;
        isFloat = IsEqualGUID(sub, kFloatFormat);
        isPcm = IsEqualGUID(sub, kPcmFormat);
    }
    cap.sample = isFloat && format->wBitsPerSample == 32 ? Sample::Float32
                 : isPcm && format->wBitsPerSample == 16 ? Sample::Int16
                 : isPcm && format->wBitsPerSample == 32 ? Sample::Int32
                                                         : Sample::Unknown;
    hr = cap.sample == Sample::Unknown
             ? E_FAIL
             : cap.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /* 100 ms */, 0, format, nullptr);
    UINT bits = format->wBitsPerSample;
    CoTaskMemFree(format);
    if (FAILED(hr)) {
        LOG("Voice: \"%s\" won't start (0x%08lx, %u-bit %s)", name.c_str(), static_cast<unsigned long>(hr), bits,
            sampleName(cap.sample));
        cap.close();
        return false;
    }
    if (FAILED(cap.client->GetService(kCaptureClientIid, reinterpret_cast<void**>(&cap.capture))) ||
        FAILED(cap.client->Start())) {
        cap.close();
        return false;
    }
    LOG("Voice: listening to \"%s\"%s (%u Hz, %u channels, %s)", name.c_str(),
        cap.byDefault ? " (Windows' default input)" : "", cap.rate, cap.channels, sampleName(cap.sample));
    return true;
}

// Loudness to mouth: 10 ms blocks, in dB, mapped between the gate and full,
// opening quickly and closing a little slower, like a jaw does.
struct Mouth {
    double sum = 0;
    UINT frames = 0;
    float open = 0;
    // Stats for the log, so the gate and full levels can be tuned.
    float statPeak = -200, statSpeech = 0;
    UINT statBlocks = 0, statOpen = 0;
    float statQuiet = 0;
    UINT statQuietBlocks = 0;
    ULONGLONG statSince = 0;

    void block(UINT rate) {
        float seconds = frames / static_cast<float>(rate);
        float db = 10.0f * std::log10(static_cast<float>(sum / frames) + 1e-12f);
        sum = 0;
        frames = 0;
        float range = g_settings.fullDb - g_settings.gateDb;
        float target = range > 1 ? (db - g_settings.gateDb) / range : 0;
        target = target < 0 ? 0 : target > 1 ? 1 : target;
        float tau = target > open ? 0.025f : 0.08f;
        open += (target - open) * (1 - std::exp(-seconds / tau));
        armik::setMouth(open * g_settings.openDegrees * 3.14159265f / 180.0f);

        ++statBlocks;
        if (db > -119.5f) ++g_flowLive;  // digital silence comes out at -120
        if (db > statPeak) statPeak = db;
        if (target > 0) {
            statSpeech += db;
            ++statOpen;
        } else {
            statQuiet += db;
            ++statQuietBlocks;
        }
        ULONGLONG now = GetTickCount64();
        if (!statSince) statSince = now;
        if (now - statSince >= 30000) {
            static int logged = 0;
            if (logged++ < 60)
                LOG("Voice: last 30 s: peak %.0f dB, talking %.0f dB (%u%% of the time), quiet %.0f dB (gate %.0f, "
                    "full %.0f)",
                    statPeak, statOpen ? statSpeech / statOpen : -200.0f, statBlocks ? statOpen * 100 / statBlocks : 0,
                    statQuietBlocks ? statQuiet / statQuietBlocks : -200.0f, g_settings.gateDb, g_settings.fullDb);
            statPeak = -200;
            statSpeech = statQuiet = 0;
            statBlocks = statOpen = statQuietBlocks = 0;
            statSince = now;
        }
    }

    void add(const Capture& cap, const BYTE* data, UINT32 count, bool silent) {
        UINT blockFrames = cap.rate / 100;
        for (UINT32 f = 0; f < count; ++f) {
            float frame = 0;
            for (UINT c = 0; c < cap.channels && !silent; ++c) {
                size_t i = static_cast<size_t>(f) * cap.channels + c;
                float s = cap.sample == Sample::Float32 ? reinterpret_cast<const float*>(data)[i]
                          : cap.sample == Sample::Int16 ? reinterpret_cast<const int16_t*>(data)[i] / 32768.0f
                                                         : reinterpret_cast<const int32_t*>(data)[i] / 2147483648.0f;
                frame += s * s;
            }
            sum += frame / cap.channels;
            if (++frames >= blockFrames) block(cap.rate);
        }
    }

    void shut() {
        sum = 0;
        frames = 0;
        open = 0;
        armik::setMouth(0);
    }
};

void run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        LOG("Voice: COM failed; off");
        return;
    }
    IMMDeviceEnumerator* devices = nullptr;
    if (FAILED(CoCreateInstance(kEnumeratorClass, nullptr, CLSCTX_ALL, kEnumeratorIid,
                                reinterpret_cast<void**>(&devices)))) {
        LOG("Voice: Windows audio devices unavailable; off");
        CoUninitialize();
        return;
    }
    Capture cap;
    Mouth mouth;
    ULONGLONG retryAt = 0, defaultCheck = 0;
    while (!g_quit.load()) {
        bool on = g_on.load(), sim = g_sim.load() != 0, firstPerson = g_firstPerson.load();
        if (!on || !sim || !firstPerson) {
            g_flowOpen = false;
            if (cap.open()) {
                cap.close();
                LOG("Voice: mic released (%s)", !on ? "switched off" : !sim ? "no Sim on a lot" : "god view");
            }
            mouth.shut();
            WaitForSingleObject(g_wake, 500);
            continue;
        }
        ULONGLONG now = GetTickCount64();
        if (!cap.open()) {
            if (now < retryAt) {
                WaitForSingleObject(g_wake, 250);
                continue;
            }
            if (!openMic(devices, cap)) {
                retryAt = now + 5000;
                continue;
            }
            defaultCheck = now + 3000;
        }
        // Following Windows' default: a new default mic takes over.
        if (cap.byDefault && now >= defaultCheck) {
            defaultCheck = now + 3000;
            IMMDevice* current = nullptr;
            if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, &current))) {
                bool changed = idOf(current) != cap.id;
                current->Release();
                if (changed) {
                    LOG("Voice: Windows' default input changed");
                    cap.close();
                    mouth.shut();
                    continue;
                }
            }
        }
        g_flowOpen = true;
        UINT32 packet = 0;
        HRESULT hr = cap.capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = cap.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            mouth.add(cap, data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            g_flowFrames += frames;
            cap.capture->ReleaseBuffer(frames);
            hr = cap.capture->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            LOG("Voice: the mic stopped (0x%08lx, unplugged?); trying again", static_cast<unsigned long>(hr));
            g_flowOpen = false;
            cap.close();
            mouth.shut();
            retryAt = now + 1000;
            continue;
        }
        WaitForSingleObject(g_wake, 10);
    }
    cap.close();
    mouth.shut();
    devices->Release();
    CoUninitialize();
}

}  // namespace

Flow flow() {
    Flow f;
    f.open = g_flowOpen.load();
    f.frames = g_flowFrames.load();
    f.live = g_flowLive.load();
    return f;
}

void install(const Settings& settings) {
    g_settings = settings;
    if (!settings.on) {
        LOG("Voice: off ([voice] mic = false); the mic isn't used");
        return;
    }
    if (!(g_settings.fullDb > g_settings.gateDb + 1)) g_settings.fullDb = g_settings.gateDb + 30;
    if (!(g_settings.openDegrees >= 0 && g_settings.openDegrees <= 40)) g_settings.openDegrees = 14;
    g_on = settings.on;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = std::thread(run);
    LOG("Voice: mouth follows your mic (gate %.0f dB, full %.0f dB, opens %.0f degrees)", g_settings.gateDb,
        g_settings.fullDb, g_settings.openDegrees);
}

void setSim(uint64_t objectId) {
    if (g_sim.exchange(objectId) == objectId) return;
    if (!objectId) armik::setVoiceActor(nullptr);
    if (g_wake) SetEvent(g_wake);
    static int logged = 0;
    if (logged++ < 20) LOG("Voice: mouth of Sim %llu", static_cast<unsigned long long>(objectId));
}

uint64_t sim() { return g_sim.load(); }

void setFirstPerson(bool on) {
    if (g_firstPerson.exchange(on) == on) return;
    if (!on) armik::setVoiceActor(nullptr);
    if (g_wake) SetEvent(g_wake);
}

void shutdown() {
    if (!g_thread.joinable()) return;
    g_quit = true;
    if (g_wake) SetEvent(g_wake);
    g_thread.join();
}

}  // namespace s4vr::voice
