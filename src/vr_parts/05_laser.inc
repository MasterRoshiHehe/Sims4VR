// ---------------------------------------------------------------------------
// Laser. Drawn into each eye image as dots along the ray, projected with the
// pose that image was rendered from, so it sits at the right depth.

static bool g_overlayEye = false;  // the next copy is an eye image
static XrPosef g_overlayPose{};
static XrFovf g_overlayFov{};

// App-space point to pixel in an image rendered with this pose and FOV.
static bool projectNormalized(const XrVector3f& p, const XrPosef& pose, const XrFovf& fov, float& nx, float& ny) {
    // v = conj(q) * (p - position)
    const XrQuaternionf& q = pose.orientation;
    camera::Vec3 d{p.x - pose.position.x, p.y - pose.position.y, p.z - pose.position.z};
    camera::Vec3 u{-q.x, -q.y, -q.z};
    camera::Vec3 t{2 * (u.y * d.z - u.z * d.y), 2 * (u.z * d.x - u.x * d.z), 2 * (u.x * d.y - u.y * d.x)};
    camera::Vec3 v{d.x + q.w * t.x + (u.y * t.z - u.z * t.y), d.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                   d.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    if (v.z > -0.01f) return false;
    float x = v.x / -v.z, y = v.y / -v.z;
    float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight), up = std::tan(fov.angleUp), dn = std::tan(fov.angleDown);
    nx = (x - l) / (r - l);
    ny = (up - y) / (up - dn);
    return nx >= 0 && ny >= 0 && nx < 1 && ny < 1;
}

// The frame the game picks in: a centred frustum as wide as the drawn one
// (touch.cpp: its pick camera gets the pixel's centred direction). The same
// as the picture with a centred eye view; with the lopsided one the mouse
// went where the laser shows in the picture and picked about 18 degrees off.
static XrFovf pickFov(const XrFovf& f) {
    const float halfX = (std::tan(f.angleRight) - std::tan(f.angleLeft)) / 2,
                halfY = (std::tan(f.angleUp) - std::tan(f.angleDown)) / 2;
    return {-std::atan(halfX), std::atan(halfX), std::atan(halfY), -std::atan(halfY)};
}

static int g_captureSlot = -1;  // what copyIntoSwapchain is drawing, for a screenshot (screenshot::Slot)

// The body swapchain beside the eye swapchain `eye` (g_splitSlot), whose new
// picture `image` was just drawn: your body split off it into its own image
// (bodylayer.h), or that image made transparent. An image already
// transparent stays shown as it is.
static void splitBody(const Swapchain& eye, ID3D11Texture2D* image) {
    const int slot = g_splitSlot;
    Swapchain& chain = g_bodyChains[slot];
    const bool want = g_splitWanted && g_splitFrom && g_splitFrom->body.count > 0;
    if (!want && g_bodyState[slot] == kBodyEmpty) return;
    D3D11_TEXTURE2D_DESC like{};
    like.Width = eye.width;
    like.Height = eye.height;
    like.Format = eye.sourceFormat;
    bool recreated = false;
    uint32_t index = 0;
    if (!ensureSwapchain(chain, like, recreated) || chain.format != eye.format || !acquireImage(chain, index)) {
        g_bodyState[slot] = kBodyUnknown;
        return;
    }
    ID3D11Texture2D* body = chain.images[index].texture;
    const bool drawn =
        want && bodylayer::split(image, body, eye.format, eye.width, eye.height, g_splitFrom->fov, g_splitFrom->body);
    const bool empty = !drawn && bodylayer::clear(body, chain.format);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released = check(xrReleaseSwapchainImage(chain.handle, &release), "xrReleaseSwapchainImage");
    g_bodyState[slot] = !released ? kBodyUnknown : drawn ? kBodyDrawn : empty ? kBodyEmpty : kBodyUnknown;
}

static bool copyIntoSwapchain(Swapchain& s, ID3D11Texture2D* backbuffer, const D3D11_TEXTURE2D_DESC& desc) {
    uint32_t index = 0;
    if (!acquireImage(s, index)) return false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    backbuffer->GetDevice(&device);
    device->GetImmediateContext(&context);
    ID3D11Texture2D* target = s.images[index].texture;
    eyelids::State lids = eyelids::advance();
    if (s.width != desc.Width || s.height != desc.Height) {
        // made larger by fsr::eyeDesc: upscaled (a frame left as it was if that fails; the next is copied)
        fsr::draw(backbuffer, desc, target, s.format, s.width, s.height, g_config.sharpen, lids.closure, lids.dark);
    } else if (g_overlayEye &&
               sharpen::draw(backbuffer, desc, target, s.format, g_config.sharpen, lids.closure, lids.dark)) {
        // sharpened (and eyelids drawn) into the eye image
    } else if (desc.SampleDesc.Count > 1)
        context->ResolveSubresource(target, 0, backbuffer, 0, s.format);
    else
        context->CopySubresourceRegion(target, 0, 0, 0, 0, backbuffer, 0, nullptr);
    if (g_keepEye >= 0) eyealign::keep(g_keepEye, target, s.format, s.width, s.height);
    if (g_captureSlot >= 0) screenshot::offer(context, target, g_captureSlot);
    if (g_splitSlot >= 0) splitBody(s, target);
    context->Release();
    device->Release();

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
}

// ---------------------------------------------------------------------------
// Laser beam: a thin quad along the ray that turns to face the head, plus a
// round marker where it lands. Both are compositor layers, so they are sharp
// and steady whatever the game renders.

static XrSwapchainSubImage subImage(const Swapchain& s);

struct LayerImage {
    Swapchain swapchain;
    ID3D11Texture2D* source = nullptr;  // our picture, copied into the swapchain when it changes
    // The picture is in the swapchain's last released image. A layer shows the
    // last released image, so an unchanged picture needs no acquire, copy or
    // release (each a call into the runtime, and a wait) every frame.
    bool uploaded = false;
};
static LayerImage g_beamImage, g_tipImage, g_phoneBodyImage, g_settingsImage, g_voidImage;

static void releaseLayerImage(LayerImage& image) {
    destroySwapchain(image.swapchain);
    if (image.source) image.source->Release();
    image.source = nullptr;
    image.uploaded = false;
}

static void releaseBeam() {
    releaseLayerImage(g_beamImage);
    releaseLayerImage(g_tipImage);
    releaseLayerImage(g_phoneBodyImage);
    releaseLayerImage(g_settingsImage);
    releaseLayerImage(g_voidImage);
}

// rgba(x, y) returns premultiplied {r, g, b, a} in 0..1.
template <typename Fn>
static bool ensureLayerImage(LayerImage& image, ID3D11Device* device, uint32_t width, uint32_t height, Fn rgba) {
    D3D11_TEXTURE2D_DESC want{};
    want.Width = width;
    want.Height = height;
    want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    want.SampleDesc.Count = 1;
    bool recreated;
    if (!ensureSwapchain(image.swapchain, want, recreated)) return false;
    if (image.source && !recreated) return true;
    if (image.source) image.source->Release();
    image.source = nullptr;
    image.uploaded = false;
    const DXGI_FORMAT f = image.swapchain.format;
    const bool rgbaOrder = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                           f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool srgb = f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    auto encode = [srgb](float v) {
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        if (srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
        return static_cast<uint32_t>(v * 255 + 0.5f);
    };
    std::vector<uint32_t> pixels(width * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            float c[4];
            rgba((x + 0.5f) / width, (y + 0.5f) / height, c);
            uint32_t r = encode(c[0]), g = encode(c[1]), b = encode(c[2]);
            uint32_t a = static_cast<uint32_t>((c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3]) * 255 + 0.5f);
            pixels[y * width + x] = rgbaOrder ? (a << 24 | b << 16 | g << 8 | r) : (a << 24 | r << 16 | g << 8 | b);
        }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = f;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4, 0};
    if (FAILED(device->CreateTexture2D(&desc, &init, &image.source))) {
        image.source = nullptr;
        return false;
    }
    return true;
}

static bool submitLayerImage(LayerImage& image, ID3D11DeviceContext* context) {
    if (image.uploaded) return true;
    Swapchain& s = image.swapchain;
    uint32_t index = 0;
    if (!acquireImage(s, index)) return false;
    context->CopyResource(s.images[index].texture, image.source);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    image.uploaded = check(xrReleaseSwapchainImage(s.handle, &release), "xrReleaseSwapchainImage");
    return image.uploaded;
}

static XrVector3f sub3(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot3(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static XrVector3f cross3(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static XrVector3f normalize3(const XrVector3f& v) {
    float l = std::sqrt(dot3(v, v));
    return l > 1e-6f ? XrVector3f{v.x / l, v.y / l, v.z / l} : XrVector3f{0, 0, 1};
}

// Rotation whose x, y and z axes are the given orthonormal vectors.
static XrQuaternionf fromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z) {
    float trace = x.x + y.y + z.z;
    XrQuaternionf q;
    if (trace > 0) {
        float s = std::sqrt(trace + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    } else if (x.x > y.y && x.x > z.z) {
        float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return q;
}

static bool renderBeam(ID3D11Device* device, XrTime time, XrCompositionLayerQuad& beam, XrCompositionLayerQuad& tip) {
    if (!g_laserValid) return false;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, time, &head)) ||
        !(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return false;
    // Beam: bright core with a soft blue glow, thinning and fading with distance.
    bool ok = ensureLayerImage(g_beamImage, device, 256, 32, [](float u, float v, float* c) {
        float across = std::fabs(v - 0.5f) * 2;  // 0 centre, 1 edge
        float width = 1.0f - 0.55f * u;          // taper
        float r = across / width;
        float core = r < 0.25f ? 1.0f : r < 0.45f ? 1.0f - (r - 0.25f) / 0.2f : 0.0f;
        float glow = r < 1 ? (1 - r) * (1 - r) : 0;
        float fade = (u < 0.04f ? u / 0.04f : 1.0f) * (1.0f - 0.6f * u);
        float a = (core * 0.9f + glow * 0.45f) * fade;
        a = a > 1 ? 1 : a;
        float white = core * fade;
        c[0] = (0.35f * a + 0.65f * white);
        c[1] = (0.7f * a + 0.3f * white);
        c[2] = a;
        for (int i = 0; i < 3; ++i) c[i] = c[i] > a ? a : c[i];  // keep premultiplied
        c[3] = a;
    });
    ok = ok && ensureLayerImage(g_tipImage, device, 64, 64, [](float u, float v, float* c) {
        float r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2;  // 0 centre, 1 edge
        float ring = r > 0.7f && r < 0.95f ? 1.0f : 0.0f;
        float dot = r < 0.35f ? 1.0f : r < 0.45f ? (0.45f - r) / 0.1f : 0.0f;
        float a = std::max(ring * 0.9f, dot);
        float glow = r < 1 ? (1 - r) * 0.25f : 0;
        a = std::max(a, glow);
        c[0] = a * (ring + dot > 0 ? 1.0f : 0.35f);
        c[1] = a * (ring + dot > 0 ? 1.0f : 0.7f);
        c[2] = a;
        c[3] = a;
    });
    if (!ok || !g_beamImage.source || !g_tipImage.source) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    ok = submitLayerImage(g_beamImage, context) && submitLayerImage(g_tipImage, context);
    context->Release();
    if (!ok) return false;

    const XrVector3f eye = head.pose.position;
    XrVector3f along = sub3(g_laserTo, g_laserFrom);
    float length = std::sqrt(dot3(along, along));
    if (!(length >= 0.01f)) return false;
    along = normalize3(along);
    XrVector3f to = g_laserTo;
    if (length > 50) {  // pointing at the horizon: draw a long beam, not a kilometre-wide one
        length = 50;
        to = {g_laserFrom.x + along.x * length, g_laserFrom.y + along.y * length, g_laserFrom.z + along.z * length};
    }
    XrVector3f mid{(g_laserFrom.x + to.x) / 2, (g_laserFrom.y + to.y) / 2, (g_laserFrom.z + to.z) / 2};
    XrVector3f toEye = sub3(eye, mid);
    XrVector3f facing = normalize3(sub3(toEye, XrVector3f{along.x * dot3(toEye, along), along.y * dot3(toEye, along),
                                                          along.z * dot3(toEye, along)}));
    XrVector3f up = cross3(facing, along);

    beam = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    beam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    beam.space = g_appSpace;
    beam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    beam.subImage = subImage(g_beamImage.swapchain);
    beam.pose = {fromAxes(along, up, facing), mid};
    beam.size = {length, 0.012f};

    // The marker keeps the same apparent size at any distance.
    XrVector3f tipToEye = sub3(eye, to);
    float distance = std::sqrt(dot3(tipToEye, tipToEye));
    XrVector3f z = normalize3(tipToEye);
    XrVector3f x = normalize3(cross3(XrVector3f{0, 1, 0}, z));
    if (std::fabs(z.y) > 0.99f) x = {1, 0, 0};
    XrVector3f y = cross3(z, x);
    tip = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    tip.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    tip.space = g_appSpace;
    tip.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    tip.subImage = subImage(g_tipImage.swapchain);
    tip.pose = {fromAxes(x, y, z), to};
    float tipSize = 0.025f * distance;
    tip.size = {tipSize, tipSize};
    return true;
}

