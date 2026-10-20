#include "screenshot.h"

#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "log.h"

namespace s4vr::screenshot {

namespace {

constexpr ULONGLONG kGiveUpMs = 2000;  // a shot not taken by then is dropped
constexpr float kQuality = 0.92f;      // JPEG

struct Image {
    int width = 0, height = 0;
    bool rgba = false;            // R8G8B8A8 rather than B8G8R8A8
    std::vector<uint8_t> pixels;  // 4 bytes a pixel, rows packed
};

struct Job {
    Image images[kSlots];
    bool have[kSlots] = {};
    SYSTEMTIME when{};
};

std::wstring g_dir;
std::atomic<bool> g_requested{false};
std::atomic<int> g_writing{0};

// Render thread only.
bool g_active = false;
ULONGLONG g_started = 0;
SYSTEMTIME g_when{};
ID3D11Texture2D* g_staging[kSlots] = {};
D3D11_TEXTURE2D_DESC g_stagingDesc[kSlots] = {};
bool g_copied[kSlots] = {};
bool g_read[kSlots] = {};
std::unique_ptr<Job> g_job;

bool byteOrder(DXGI_FORMAT format, bool& rgba) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            rgba = false;
            return true;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            rgba = true;
            return true;
        default:
            return false;
    }
}

// Half size into a 24-bit BGR picture at column ox: each pixel is the
// average of 2x2. The menu is laid over grey by its alpha.
void halve(const Image& in, bool overGrey, std::vector<uint8_t>& out, int outWidth, int ox) {
    const int w = in.width / 2, h = in.height / 2;
    const int r = in.rgba ? 0 : 2, b = in.rgba ? 2 : 0;
    const size_t inStride = static_cast<size_t>(in.width) * 4, outStride = static_cast<size_t>(outWidth) * 3;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row0 = in.pixels.data() + static_cast<size_t>(2 * y) * inStride;
        const uint8_t* row1 = row0 + inStride;
        uint8_t* o = out.data() + static_cast<size_t>(y) * outStride + static_cast<size_t>(ox) * 3;
        for (int x = 0; x < w; ++x, o += 3) {
            const uint8_t* p[4] = {row0 + 8 * x, row0 + 8 * x + 4, row1 + 8 * x, row1 + 8 * x + 4};
            int sum[3] = {0, 0, 0};  // b g r
            for (const uint8_t* q : p) {
                int c[3] = {q[b], q[1], q[r]};
                if (overGrey) {  // the menu's colours come premultiplied by its alpha
                    int a = q[3];
                    for (int& v : c) v = std::min(255, v + 128 * (255 - a) / 255);
                }
                sum[0] += c[0];
                sum[1] += c[1];
                sum[2] += c[2];
            }
            o[0] = static_cast<uint8_t>(sum[0] / 4);
            o[1] = static_cast<uint8_t>(sum[1] / 4);
            o[2] = static_cast<uint8_t>(sum[2] / 4);
        }
    }
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool writeJpeg(IWICImagingFactory* factory, const std::wstring& path, std::vector<uint8_t>& bgr, int w, int h,
               HRESULT& error) {
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr)) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        std::memset(&value, 0, sizeof(value));
        value.vt = VT_R4;
        value.fltVal = kQuality;
        options->Write(1, &option, &value);  // a codec without it keeps its default
        hr = frame->Initialize(options);
    }
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = E_NOTIMPL;
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 3), static_cast<UINT>(bgr.size()),
                                bgr.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    release(options);
    release(frame);
    release(encoder);
    release(stream);
    error = hr;
    return SUCCEEDED(hr);
}

void write(const Job& job) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                  reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        LOG("Screenshot: Windows' image encoder isn't available (0x%08lx)", static_cast<unsigned long>(hr));
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    CreateDirectoryW(g_dir.c_str(), nullptr);
    wchar_t base[64];
    const SYSTEMTIME& t = job.when;
    std::swprintf(base, 64, L"sims4vr_%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    std::wstring name = base;
    for (int n = 2; n < 10 && GetFileAttributesW((g_dir + L"\\" + name + L".jpg").c_str()) != INVALID_FILE_ATTRIBUTES;
         ++n)
        name = std::wstring(base) + L"_" + std::to_wstring(n);

    // Both eyes side by side, or the flat screen.
    const bool eyes = job.have[kLeftEye] && job.have[kRightEye];
    int width = 0, height = 0;
    std::vector<const Image*> parts;
    if (eyes) {
        parts.push_back(&job.images[kLeftEye]);
        parts.push_back(&job.images[kRightEye]);
    } else if (job.have[kFlatScreen]) {
        parts.push_back(&job.images[kFlatScreen]);
    }
    for (const Image* p : parts) {
        width += p->width / 2;
        if (p->height / 2 > height) height = p->height / 2;
    }
    HRESULT error = S_OK;
    bool saved = false;
    if (width > 0 && height > 0) {
        std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3, 0);
        int ox = 0;
        for (const Image* p : parts) {
            halve(*p, false, bgr, width, ox);
            ox += p->width / 2;
        }
        saved = writeJpeg(factory, g_dir + L"\\" + name + L".jpg", bgr, width, height, error);
    }
    bool menu = false;
    if (saved && job.have[kMenu]) {
        const Image& m = job.images[kMenu];
        std::vector<uint8_t> bgr(static_cast<size_t>(m.width / 2) * (m.height / 2) * 3, 0);
        halve(m, true, bgr, m.width / 2, 0);
        HRESULT menuError = S_OK;
        menu = writeJpeg(factory, g_dir + L"\\" + name + L"_menu.jpg", bgr, m.width / 2, m.height / 2, menuError);
    }
    if (saved)
        LOG("Screenshot: saved %ls\\%ls.jpg (%dx%d, %s)%s", g_dir.c_str(), name.c_str(), width, height,
            eyes ? "left and right eye" : "the screen", menu ? ", the menu layer in _menu.jpg" : "");
    else
        LOG("Screenshot: couldn't write %ls\\%ls.jpg (0x%08lx)", g_dir.c_str(), name.c_str(),
            static_cast<unsigned long>(error));
    factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}

void finish() {
    g_active = false;
    for (int s = 0; s < kSlots; ++s) g_copied[s] = g_read[s] = false;
}

}  // namespace

// Screenshots Claude is done with (the problem they showed is fixed) are listed
// in screenshots\resolved.txt, one file name a line; they are deleted here, at
// the game's start. Only our own pictures in that folder, nothing else.
static void removeResolved() {
    const std::wstring list = g_dir + L"\\resolved.txt";
    FILE* file = _wfopen(list.c_str(), L"r");
    if (!file) return;
    char line[256];
    int removed = 0, missing = 0, refused = 0;
    while (std::fgets(line, sizeof(line), file)) {
        std::string name(line);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (name.empty()) continue;
        bool ours = name.rfind("sims4vr_", 0) == 0 && name.size() > 12 &&
                    name.compare(name.size() - 4, 4, ".jpg") == 0 && name.find_first_of("\\/:") == std::string::npos &&
                    name.find("..") == std::string::npos;
        if (!ours) {
            ++refused;
            continue;
        }
        std::wstring path = g_dir + L"\\" + std::wstring(name.begin(), name.end());
        if (DeleteFileW(path.c_str()))
            ++removed;
        else
            ++missing;
    }
    std::fclose(file);
    DeleteFileW(list.c_str());
    LOG("Screenshot: removed %d resolved screenshot(s)%s%s", removed, missing ? " (some were already gone)" : "",
        refused ? " (skipped names that aren't our screenshots)" : "");
}

void init(const std::wstring& moduleDir) {
    g_dir = moduleDir + L"\\screenshots";
    removeResolved();
}

void request() { g_requested = true; }

bool taking() { return g_active; }

void offer(ID3D11DeviceContext* context, ID3D11Texture2D* image, int slot) {
    if (!g_active || slot < 0 || slot >= kSlots || g_copied[slot] || !image || !context) return;
    D3D11_TEXTURE2D_DESC desc;
    image->GetDesc(&desc);
    bool rgba = false;
    if (!byteOrder(desc.Format, rgba) || desc.SampleDesc.Count != 1) {
        static int logged = 0;
        if (logged++ < 5) LOG("Screenshot: can't read image format %d (%u samples)", desc.Format, desc.SampleDesc.Count);
        return;
    }
    D3D11_TEXTURE2D_DESC& have = g_stagingDesc[slot];
    if (!g_staging[slot] || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
        release(g_staging[slot]);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        HRESULT hr = device->CreateTexture2D(&staging, nullptr, &g_staging[slot]);
        device->Release();
        if (FAILED(hr)) {
            LOG("Screenshot: no copy texture %ux%u (0x%08lx)", desc.Width, desc.Height, static_cast<unsigned long>(hr));
            g_staging[slot] = nullptr;
            return;
        }
        have = staging;
    }
    context->CopySubresourceRegion(g_staging[slot], 0, 0, 0, 0, image, 0, nullptr);
    g_copied[slot] = true;
    if (!g_job) g_job = std::make_unique<Job>();
    g_job->images[slot].rgba = rgba;
}

void pump(ID3D11DeviceContext* context) {
    const ULONGLONG now = GetTickCount64();
    if (!g_active) {
        if (!g_requested.exchange(false)) return;
        if (g_writing.load() >= 2) {
            LOG("Screenshot: still writing the last ones; skipped");
            return;
        }
        finish();
        g_active = true;
        g_started = now;
        GetLocalTime(&g_when);
        g_job = std::make_unique<Job>();
        return;
    }
    if (now - g_started > kGiveUpMs) {
        LOG("Screenshot: no picture to take (copied: left %d, right %d, screen %d)", g_copied[kLeftEye],
            g_copied[kRightEye], g_copied[kFlatScreen]);
        finish();
        return;
    }
    const bool eyes = g_copied[kLeftEye] && g_copied[kRightEye];
    if (!eyes && !g_copied[kFlatScreen]) return;
    // Read the copies back once the GPU has made them (never waiting for it).
    bool done = true;
    for (int s = 0; s < kSlots; ++s) {
        if (!g_copied[s] || g_read[s] || (eyes && s == kFlatScreen)) continue;
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(g_staging[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            done = false;
            continue;
        }
        if (FAILED(hr)) {
            LOG("Screenshot: couldn't read image %d back (0x%08lx)", s, static_cast<unsigned long>(hr));
            g_copied[s] = false;
            continue;
        }
        Image& image = g_job->images[s];
        image.width = static_cast<int>(g_stagingDesc[s].Width);
        image.height = static_cast<int>(g_stagingDesc[s].Height);
        const size_t row = static_cast<size_t>(image.width) * 4;
        image.pixels.resize(row * image.height);
        for (int y = 0; y < image.height; ++y)
            std::memcpy(image.pixels.data() + row * y, static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * y,
                        row);
        context->Unmap(g_staging[s], 0);
        g_read[s] = true;
    }
    if (!done) return;
    std::shared_ptr<Job> job(g_job.release());
    job->when = g_when;
    for (int s = 0; s < kSlots; ++s) job->have[s] = g_read[s] && !(eyes && s == kFlatScreen);
    if (!(job->have[kLeftEye] && job->have[kRightEye]) && !job->have[kFlatScreen]) {
        finish();
        return;
    }
    finish();
    ++g_writing;
    std::thread([job] {
        write(*job);
        --g_writing;
    }).detach();
}

}  // namespace s4vr::screenshot
