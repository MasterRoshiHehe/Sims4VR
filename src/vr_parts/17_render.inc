// ---------------------------------------------------------------------------
// Per-frame rendering

static XrSwapchainSubImage subImage(const Swapchain& s) {
    XrSwapchainSubImage sub{};
    sub.swapchain = s.handle;
    sub.imageRect.extent = {static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)};
    return sub;
}

static bool renderUiPanel(XrCompositionLayerQuad& layer);

static bool renderQuad(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, XrCompositionLayerQuad& layer) {
    bool recreated;
    if (!ensureSwapchain(g_quadSwapchain, desc, recreated)) return false;
    g_captureSlot = screenshot::kFlatScreen;
    bool copied = copyIntoSwapchain(g_quadSwapchain, backbuffer, desc);
    g_captureSlot = -1;
    if (!copied) return false;

    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(g_quadSwapchain);
    layer.pose.orientation.w = 1.0f;
    layer.pose.position = {0.0f, 0.0f, -g_config.quadDistance};
    layer.size = {g_config.quadWidth, g_config.quadWidth * desc.Height / desc.Width};
    return true;
}


// The flat screen (main menu, loading) floats in a black room. Sent as quad
// layers alone, it swam with your head in Virtual Desktop: turn your head
// and it slid along a little, then caught up. Frames that carry one
// projection layer as well kept their quads still (the Crash Team Racing VR
// mod's "one-picture fix" notes, and our own first person view), so the black
// room is now that projection layer: plain black, at this frame's views.
static bool voidLayer(ID3D11Device* device, XrTime time, XrCompositionLayerProjection& layer,
                      XrCompositionLayerProjectionView (&views)[2]) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = g_appSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!XR_SUCCEEDED(xrLocateViews(g_session, &locate, &viewState, 2, &count, located)) || count != 2 ||
        !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;
    if (!ensureLayerImage(g_voidImage, device, 256, 256, [](float, float, float* c) {
            c[0] = c[1] = c[2] = 0;
            c[3] = 1;
        }))
        return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    bool ok = submitLayerImage(g_voidImage, context);
    context->Release();
    if (!ok) return false;
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage = subImage(g_voidImage.swapchain);
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

// Every layer is checked before it goes to the runtime: a NaN or absurd pose
// or size can take the headset's compositor down with it.
static bool saneFloat(float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

static bool sanePose(XrPosef& pose) {
    XrQuaternionf& q = pose.orientation;
    float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(n) || n < 0.5f || n > 1.5f) return false;
    n = 1 / std::sqrt(n);
    q = {q.x * n, q.y * n, q.z * n, q.w * n};
    const XrVector3f& p = pose.position;
    return saneFloat(p.x, 1000) && saneFloat(p.y, 1000) && saneFloat(p.z, 1000);
}

__attribute__((noinline)) static bool saneLayer(const XrCompositionLayerBaseHeader* header) {
    if (header->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        auto* quad = const_cast<XrCompositionLayerQuad*>(reinterpret_cast<const XrCompositionLayerQuad*>(header));
        return sanePose(quad->pose) && saneFloat(quad->size.width, 200) && saneFloat(quad->size.height, 200) &&
               quad->size.width > 0 && quad->size.height > 0;
    }
    if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        for (uint32_t i = 0; i < projection->viewCount; ++i) {
            auto& view = const_cast<XrCompositionLayerProjectionView&>(projection->views[i]);
            const XrFovf& f = view.fov;
            if (!sanePose(view.pose) || !saneFloat(f.angleLeft, 1.55f) || !saneFloat(f.angleRight, 1.55f) ||
                !saneFloat(f.angleUp, 1.55f) || !saneFloat(f.angleDown, 1.55f) || f.angleLeft >= f.angleRight ||
                f.angleDown >= f.angleUp)
                return false;
        }
    }
    return true;
}

static void steadyMirror(ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc, int eye) {
    ID3D11Device* device = nullptr;
    backbuffer->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (g_mirror) {
        D3D11_TEXTURE2D_DESC have;
        g_mirror->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format ||
            have.SampleDesc.Count != desc.SampleDesc.Count) {
            g_mirror->Release();
            g_mirror = nullptr;
            g_mirrorValid = false;
        }
    }
    if (!g_mirror) {
        D3D11_TEXTURE2D_DESC m = desc;
        m.Usage = D3D11_USAGE_DEFAULT;
        m.BindFlags = desc.SampleDesc.Count > 1 ? D3D11_BIND_RENDER_TARGET : 0;
        m.CPUAccessFlags = 0;
        m.MiscFlags = 0;
        HRESULT hr = device->CreateTexture2D(&m, nullptr, &g_mirror);
        if (FAILED(hr)) g_mirror = nullptr;
        LOG("Monitor mirror %ux%u format %d samples %u bind 0x%x misc 0x%x: %s (0x%08lx)", desc.Width, desc.Height,
            static_cast<int>(desc.Format), desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags,
            g_mirror ? "created" : "FAILED", static_cast<unsigned long>(hr));
    }
    static uint64_t counts[2] = {};
    if (eye == 0 || eye == 1) {
        if (++counts[eye] % 3000 == 0)
            LOG("Monitor mirror: %llu left, %llu right frames", static_cast<unsigned long long>(counts[0]),
                static_cast<unsigned long long>(counts[1]));
    }
    if (g_mirror) {
        if (eye == 0) {
            context->CopyResource(g_mirror, backbuffer);
            g_mirrorValid = true;
        } else if (g_mirrorValid) {
            context->CopyResource(backbuffer, g_mirror);
        }
    }
    context->Release();
    device->Release();
}

// Game render size for the headset: the runtime's pixel density (recommended
// size over the eye's FOV) across the frustum the game renders (the eye's own,
// or with exact_fov off a centred one covering its wider side), times
// render_scale. Computed once per session from the first located views, and
// again when the settings menu closes after a change to these.
static void computeRenderSize(const XrView (&views)[2]) {
    if (g_renderSize[0] || g_config.renderScale <= 0 || !g_recommended[0][0]) return;
    const bool exact = g_config.fovOverride && g_config.exactFov;
    float densityX = 0, densityY = 0, width = 0, height = 0;
    for (int eye = 0; eye < 2; ++eye) {
        const XrFovf& f = views[eye].fov;
        float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
        if (r - l < 0.1f || u - d < 0.1f) return;
        densityX = std::fmax(densityX, g_recommended[eye][0] / (r - l));
        densityY = std::fmax(densityY, g_recommended[eye][1] / (u - d));
        width = std::fmax(width, exact ? r - l : 2 * std::fmax(-l, r));
        height = std::fmax(height, exact ? u - d : 2 * std::fmax(-d, u));
        if (!g_fovLogged)
            LOG("Headset %s eye FOV: left %.1f, right %.1f, up %.1f, down %.1f degrees", eye ? "right" : "left",
                f.angleLeft * 57.29578f, f.angleRight * 57.29578f, f.angleUp * 57.29578f, f.angleDown * 57.29578f);
    }
    g_fovLogged = true;
    auto size = [](float pixels) {
        int n = static_cast<int>(pixels * g_config.renderScale / 8 + 0.5f) * 8;
        return n < 64 ? 64 : n > 4096 ? 4096 : n;
    };
    g_renderSize[0] = size(densityX * width);
    g_renderSize[1] = size(densityY * height);
    LOG("Headset render size %dx%d (render_scale %.2f, %s eye view)", g_renderSize[0], g_renderSize[1],
        g_config.renderScale, exact ? "lopsided" : "centred");
}

// Ask the game for the headset size while stereo runs; its own size otherwise.
// Switching resets the game's device, so brief gaps don't switch back.
static void updateRenderSize(bool stereo) {
    static ULONGLONG lastStereo = 0;
    ULONGLONG now = GetTickCount64();
    if (stereo && g_renderSize[0]) {
        lastStereo = now;
        renderscale::setFitWidth(g_config.headsetWidth);
        renderscale::request(g_renderSize[0], g_renderSize[1]);
    } else if (now - lastStereo > 1500) {
        renderscale::request(0, 0);
    }
}

