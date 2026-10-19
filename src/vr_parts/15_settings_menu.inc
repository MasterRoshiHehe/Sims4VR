// ---------------------------------------------------------------------------
// The VR settings menu (B; settings.cpp has the rows): a panel set down in
// front of you when it opens. The right stick and A drive it while it's open,
// and the laser points at it.
struct SettingsPanel {
    bool placed = false;
    XrPosef pose{};
    XrVector3f x{}, y{}, z{};  // z points at you
    float width = 0.62f, height = 0;
};
static SettingsPanel g_settingsPanel;
static uint64_t g_settingsUploaded = 0;  // the picture's version in g_settingsImage

// Moving the menu (user 2026-10-14 22:34Z, v0.13.117): hold its title bar with
// the laser (its trigger, or in the game camera also the grip) or close your hand
// on it, and move it; it stays upright and turns to face you. Let go and it opens
// there, relative to your head, from then on (settings.cpp saves it).
struct SettingsDrag {
    int hand = -1;              // the hand holding the menu; -1 none
    bool laser = false;         // held by that hand's laser (else by the hand itself)
    bool grip = false;          // the laser's grip holds it (else its trigger)
    float distance = 0;         // laser: the held point's distance along it
    XrVector3f fromHand{};      // hand: the held point from the hand
    float offX = 0, offY = 0;   // the held point from the panel's centre, along its x and y
};
static SettingsDrag g_settingsDrag;

static bool locateHead(XrTime time, XrPosef& pose) {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags both = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (!g_viewSpace || !XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) ||
        (head.locationFlags & both) != both)
        return false;
    pose = head.pose;
    return true;
}

// Level forward and right of the head.
static void headAxes(const XrPosef& head, XrVector3f& f, XrVector3f& r) {
    f = forwardOf(head.orientation);
    f.y = 0;
    f = dot3(f, f) > 1e-4f ? normalize3(f) : XrVector3f{0, 0, -1};
    r = cross3(f, XrVector3f{0, 1, 0});
}

static void startSettingsDrag(int hand, bool laser, const XrVector3f& held) {
    SettingsDrag& d = g_settingsDrag;
    const SettingsPanel& p = g_settingsPanel;
    d.hand = hand;
    d.laser = laser;
    XrVector3f r = sub3(held, p.pose.position);
    d.offX = dot3(r, p.x);
    d.offY = dot3(r, p.y);
    buzz(hand, 0.4f, 0.03f);
    LOG("Settings: moving the menu (%s, %s hand)", laser ? (d.grip ? "laser, grip" : "laser, trigger") : "hand",
        hand ? "right" : "left");
}

// The held point to `at`; upright, facing the head.
static void moveSettings(const XrVector3f& at, const XrVector3f& head) {
    SettingsPanel& p = g_settingsPanel;
    const SettingsDrag& d = g_settingsDrag;
    XrVector3f z{head.x - at.x, 0, head.z - at.z};
    if (dot3(z, z) > 1e-4f) p.z = normalize3(z);
    p.x = normalize3(cross3(XrVector3f{0, 1, 0}, p.z));
    p.y = cross3(p.z, p.x);
    const XrVector3f c{at.x - p.x.x * d.offX - p.y.x * d.offY, at.y - p.x.y * d.offX - p.y.y * d.offY,
                       at.z - p.x.z * d.offX - p.y.z * d.offY};
    p.pose = {fromAxes(p.x, p.y, p.z), c};
}

static void endSettingsDrag(XrTime time) {
    SettingsDrag& d = g_settingsDrag;
    if (d.hand < 0) return;
    buzz(d.hand, 0.25f, 0.02f);
    d.hand = -1;
    XrPosef head;
    if (!locateHead(time, head)) return;
    XrVector3f f, r;
    headAxes(head, f, r);
    const SettingsPanel& p = g_settingsPanel;
    const XrVector3f delta = sub3(p.pose.position, head.position);
    const XrVector3f facing{-f.x, 0, -f.z};
    const float turn = std::atan2(facing.z * p.z.x - facing.x * p.z.z, dot3(facing, p.z)) * 57.29578f;
    const float place[4] = {dot3(delta, r), delta.y, dot3(delta, f), turn};
    settings::setPlacement(place);
}

static void openSettings(XrTime time) {
    XrPosef head;
    if (!locateHead(time, head)) return;
    XrVector3f f, right;
    headAxes(head, f, right);
    // Where you last put it (relative to your head), or 0.75 m ahead, a little low.
    float place[4] = {0, -0.08f, 0.75f, 0};
    settings::placement(place);
    SettingsPanel& p = g_settingsPanel;
    const float a = place[3] / 57.29578f, ca = std::cos(a), sa = std::sin(a);
    p.z = {-f.x * ca - f.z * sa, 0, f.x * sa - f.z * ca};  // facing you, turned by place[3] about up
    p.x = normalize3(cross3(XrVector3f{0, 1, 0}, p.z));
    p.y = cross3(p.z, p.x);
    const XrVector3f& h = head.position;
    p.pose = {fromAxes(p.x, p.y, p.z),
              {h.x + right.x * place[0] + f.x * place[2], h.y + place[1], h.z + right.z * place[0] + f.z * place[2]}};
    g_settingsDrag.hand = -1;
    int w = 0, hgt = 0;
    uint64_t version = 0;
    settings::image(w, hgt, version);
    p.height = w > 0 ? p.width * hgt / w : p.width * 0.8f;
    p.placed = true;
    settings::open();
    buzz(1, 0.3f, 0.03f);
}

static void closeSettings() {
    g_settingsDrag.hand = -1;  // a move in progress isn't kept
    g_settingsPanel.placed = false;
    // A new render_scale: the size is worked out again from the next views.
    if (settings::close()) g_renderSize[0] = g_renderSize[1] = 0;
    buzz(1, 0.3f, 0.03f);
}

static bool hitSettings(const XrVector3f& o, const XrVector3f& d, XrVector3f& hit, float& u, float& v) {
    const SettingsPanel& p = g_settingsPanel;
    if (!p.placed || !settings::isOpen()) return false;
    float facing = dot3(d, p.z);
    if (facing > -0.01f) return false;
    float t = -dot3(sub3(o, p.pose.position), p.z) / facing;
    if (t <= 0) return false;
    hit = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    XrVector3f r = sub3(hit, p.pose.position);
    float lx = dot3(r, p.x), ly = dot3(r, p.y);
    if (std::fabs(lx) > p.width / 2 || std::fabs(ly) > p.height / 2) return false;
    u = lx / p.width + 0.5f;
    v = 0.5f - ly / p.height;
    return true;
}

static bool settingsLayer(ID3D11Device* device, XrCompositionLayerQuad& layer) {
    if (!g_settingsPanel.placed || !settings::isOpen()) return false;
    int w = 0, h = 0;
    uint64_t version = 0;
    const uint32_t* pixels = settings::image(w, h, version);
    if (!pixels || w <= 0 || h <= 0) return false;
    D3D11_TEXTURE2D_DESC want{};
    want.Width = static_cast<UINT>(w);
    want.Height = static_cast<UINT>(h);
    want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    want.SampleDesc.Count = 1;
    bool recreated;
    LayerImage& image = g_settingsImage;
    if (!ensureSwapchain(image.swapchain, want, recreated)) return false;
    const DXGI_FORMAT f = image.swapchain.format;
    if (recreated || !image.source) {
        if (image.source) image.source->Release();
        image.source = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = want.Width;
        desc.Height = want.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = f;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &image.source))) {
            image.source = nullptr;
            return false;
        }
        g_settingsUploaded = 0;
        image.uploaded = false;
    }
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (version != g_settingsUploaded) {
        // The picture is sRGB, premultiplied BGRA: reorder for an RGBA swapchain,
        // and linearise for one the runtime reads as linear.
        const bool rgbaOrder = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                               f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const bool srgb = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        static std::vector<uint32_t> converted;
        const uint32_t* data = pixels;
        if (rgbaOrder || !srgb) {
            static uint8_t linear[256];
            static bool table = false;
            if (!table) {
                for (int i = 0; i < 256; ++i) {
                    float c = i / 255.0f;
                    c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                    linear[i] = static_cast<uint8_t>(c * 255 + 0.5f);
                }
                table = true;
            }
            converted.resize(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < converted.size(); ++i) {
                uint32_t p = pixels[i], a = p >> 24, r = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
                if (!srgb) {
                    r = linear[r];
                    g = linear[g];
                    b = linear[b];
                }
                converted[i] = rgbaOrder ? (a << 24 | b << 16 | g << 8 | r) : (a << 24 | r << 16 | g << 8 | b);
            }
            data = converted.data();
        }
        context->UpdateSubresource(image.source, 0, nullptr, data, static_cast<UINT>(w) * 4, 0);
        g_settingsUploaded = version;
        image.uploaded = false;
    }
    bool ok = submitLayerImage(image, context);
    context->Release();
    if (!ok) return false;
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied
    layer.space = g_appSpace;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = subImage(image.swapchain);
    layer.pose = g_settingsPanel.pose;
    layer.size = {g_settingsPanel.width, g_settingsPanel.height};
    return true;
}

