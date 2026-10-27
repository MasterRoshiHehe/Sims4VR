// The phone lens' picture (a photo session, camera.h photoSession): the frames the game draws from the phone's back,
// copied into one texture the phone's screen shows (phone3d). Present thread.
static constexpr float kLensFov = 65.0f;  // degrees, the phone lens' vertical angle
static ID3D11Texture2D* g_viewfinder = nullptr;
static D3D11_TEXTURE2D_DESC g_viewfinderDesc{};
static ULONGLONG g_viewfinderAt = 0;

static DXGI_FORMAT plainFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static void storeViewfinder(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc) {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    if (!device) return;
    device->GetImmediateContext(&context);
    if (!g_viewfinder || g_viewfinderDesc.Width != desc.Width || g_viewfinderDesc.Height != desc.Height ||
        g_viewfinderDesc.Format != desc.Format) {
        if (g_viewfinder) g_viewfinder->Release();
        g_viewfinder = nullptr;
        D3D11_TEXTURE2D_DESC d = desc;
        d.SampleDesc = {1, 0};
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = 0;
        d.CPUAccessFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.MipLevels = 1;
        d.ArraySize = 1;
        if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_viewfinder))) {
            g_viewfinderDesc = d;
            LOG("Photo: viewfinder picture %ux%u format %d", d.Width, d.Height, static_cast<int>(d.Format));
        } else {
            g_viewfinder = nullptr;
            static int failed = 0;
            if (failed++ < 3) LOG("Photo: can't make the viewfinder picture (format %d)", static_cast<int>(desc.Format));
        }
    }
    if (g_viewfinder) {
        if (desc.SampleDesc.Count > 1)
            context->ResolveSubresource(g_viewfinder, 0, backbuffer, 0, plainFormat(desc.Format));
        else
            context->CopySubresourceRegion(g_viewfinder, 0, 0, 0, 0, backbuffer, 0, nullptr);
        g_viewfinderAt = GetTickCount64();
        static int logs = 0;
        if (logs++ < 5) LOG("Photo: a viewfinder picture from the phone lens");
    }
    context->Release();
    device->Release();
}

// The viewfinder has a recent picture (it comes every third frame; a second is long past a session's end).
static bool viewfinderReady() { return g_viewfinder && GetTickCount64() - g_viewfinderAt < 1000; }
