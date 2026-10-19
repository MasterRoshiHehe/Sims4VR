// ---------------------------------------------------------------------------
// UI panel

static XrSwapchainSubImage subImage(const Swapchain& s);

static float uiPanelHeight(const D3D11_TEXTURE2D_DESC& desc) { return g_config.uiWidth * desc.Height / desc.Width; }

// A window the phone's Options app (the game's menu) opened shows in front of
// you on its own (user 2026-10-19 01:08Z: the game's exact window, not the whole
// UI and not on the phone): the panel shows only its box in the UI (fitMenu),
// larger than on the whole panel. Off: the whole UI (a window filling the screen).
static bool g_frontCropOn = false;
static int32_t g_frontCrop[4] = {};  // x, y, width, height in the UI
constexpr float kFrontCropScale = 1.8f;

// The panel's size in metres (the window alone while g_frontCropOn).
static void uiPanelSize(const D3D11_TEXTURE2D_DESC& desc, float& w, float& h) {
    w = g_config.uiWidth;
    h = uiPanelHeight(desc);
    if (g_frontCropOn && g_frontCrop[2] > 0 && g_frontCrop[3] > 0 && desc.Width > 0) {
        w = std::min(g_config.uiWidth, g_config.uiWidth * g_frontCrop[2] / desc.Width * kFrontCropScale);
        h = w * g_frontCrop[3] / g_frontCrop[2];
    }
}

// Laser against the panel: true with the hit point and 0..1 coordinates (of the whole UI).
static bool hitPanel(const XrVector3f& o, const XrVector3f& d, const D3D11_TEXTURE2D_DESC& desc, XrVector3f& hit,
                     float& u, float& v) {
    if (d.z > -0.01f) return false;
    float t = (-g_config.uiDistance - o.z) / d.z;
    if (t <= 0) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, -g_config.uiDistance};
    float w, h;
    uiPanelSize(desc, w, h);
    u = (hit.x + w / 2) / w;
    v = (g_config.uiHeight + h / 2 - hit.y) / h;
    const bool in = u >= 0 && u <= 1 && v >= 0 && v <= 1;
    if (g_frontCropOn && desc.Width > 0 && desc.Height > 0) {
        u = (g_frontCrop[0] + u * g_frontCrop[2]) / static_cast<float>(desc.Width);
        v = (g_frontCrop[1] + v * g_frontCrop[3]) / static_cast<float>(desc.Height);
    }
    return in;
}

// Reads back the UI alpha under the laser, a couple of frames late, so the
// laser can pass through transparent parts of the panel to the world.
// The UI's alpha under the laser (two frames late, so the GPU never waits),
// and the most opaque pixel within `nearPx` pixels of it.
// A UI texture whose alpha the CPU can read: 8 bits a channel, alpha in byte 3, no MSAA.
static bool alphaReadable(const D3D11_TEXTURE2D_DESC& desc) {
    bool eightBit = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                    desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    return eightBit && desc.SampleDesc.Count == 1;
}

static void probeUiAlpha(ID3D11Texture2D* ui, float u, float v, int nearPx = 0) {
    if (u < 0 || v < 0) {  // between the tablet's pieces: nothing
        g_uiAlphaUnderLaser = g_uiAlphaNearLaser = 0;
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    ui->GetDesc(&desc);
    if (!alphaReadable(desc)) {
        g_uiAlphaUnderLaser = g_uiAlphaNearLaser = 1;  // can't tell; treat the whole panel as UI
        return;
    }
    ID3D11Device* device = nullptr;
    ui->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    int slot = g_uiProbeFrame++ % 3;
    if (!g_uiProbe[slot]) {
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = staging.Height = 2 * kProbeMax + 1;
        staging.MipLevels = staging.ArraySize = 1;
        staging.Format = desc.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        device->CreateTexture2D(&staging, nullptr, &g_uiProbe[slot]);
    }
    // Read the oldest copy (two frames back), then reuse its slot.
    if (g_uiProbe[slot] && g_uiProbePending[slot]) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(g_uiProbe[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            const UINT* box = g_uiProbeBox[slot];
            const uint8_t* pixels = static_cast<const uint8_t*>(mapped.pData);
            uint8_t most = 0;
            for (UINT y = 0; y < box[1]; ++y)
                for (UINT x = 0; x < box[0]; ++x) most = std::max(most, pixels[y * mapped.RowPitch + x * 4 + 3]);
            g_uiAlphaUnderLaser = pixels[box[3] * mapped.RowPitch + box[2] * 4 + 3] / 255.0f;
            g_uiAlphaNearLaser = most / 255.0f;
            context->Unmap(g_uiProbe[slot], 0);
        }
        g_uiProbePending[slot] = false;
    }
    if (g_uiProbe[slot]) {
        UINT x = static_cast<UINT>(u * (desc.Width - 1)), y = static_cast<UINT>(v * (desc.Height - 1));
        UINT r = static_cast<UINT>(std::max(0, std::min(kProbeMax, nearPx)));
        UINT x0 = x > r ? x - r : 0, y0 = y > r ? y - r : 0;
        D3D11_BOX box{x0, y0, 0, std::min(desc.Width, x + r + 1), std::min(desc.Height, y + r + 1), 1};
        context->CopySubresourceRegion(g_uiProbe[slot], 0, 0, 0, 0, ui, 0, &box);
        UINT* kept = g_uiProbeBox[slot];
        kept[0] = box.right - box.left;
        kept[1] = box.bottom - box.top;
        kept[2] = x - x0;
        kept[3] = y - y0;
        g_uiProbePending[slot] = true;
    }
    context->Release();
    device->Release();
}

static bool renderUiPanel(XrCompositionLayerQuad& layer) {
    if (!g_uiFrame) return false;
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    bool recreated;
    if (!ensureSwapchain(g_uiSwapchain, desc, recreated)) return false;
    g_captureSlot = screenshot::kMenu;
    bool copied = copyIntoSwapchain(g_uiSwapchain, g_uiFrame, desc);
    g_captureSlot = -1;
    if (!copied) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied, as Scaleform blends
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_uiSwapchain);
    if (g_frontCropOn && g_frontCrop[2] > 0 && g_frontCrop[3] > 0)
        layer.subImage.imageRect = {{g_frontCrop[0], g_frontCrop[1]}, {g_frontCrop[2], g_frontCrop[3]}};
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, g_config.uiHeight, -g_config.uiDistance};
    uiPanelSize(desc, layer.size.width, layer.size.height);
    return true;
}

