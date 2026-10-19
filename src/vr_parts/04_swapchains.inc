// ---------------------------------------------------------------------------
// Swapchains and copies

static DXGI_FORMAT srgbVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

static DXGI_FORMAT unormVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// The game writes gamma-encoded colour into a UNORM backbuffer, so the sRGB
// variant tells the runtime how to read those same bytes. Copies between the
// two are legal because they share a typeless family.
static DXGI_FORMAT chooseFormat(DXGI_FORMAT backbuffer) {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());
    for (DXGI_FORMAT wanted : {srgbVariant(backbuffer), unormVariant(backbuffer), backbuffer}) {
        for (int64_t f : formats)
            if (f == wanted) return wanted;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// Returns false if no usable swapchain; sets `recreated` when images were replaced.
static bool ensureSwapchain(Swapchain& s, const D3D11_TEXTURE2D_DESC& source, bool& recreated) {
    recreated = false;
    if (s.handle && s.width == source.Width && s.height == source.Height && s.sourceFormat == source.Format)
        return true;
    destroySwapchain(s);
    recreated = true;

    DXGI_FORMAT format = chooseFormat(source.Format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        setError("runtime has no swapchain format compatible with backbuffer format " + std::to_string(source.Format));
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = source.Width;
    info.height = source.Height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(xrCreateSwapchain(g_session, &info, &s.handle), "xrCreateSwapchain")) return false;

    uint32_t count = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &count, nullptr);
    s.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data()));
    s.width = source.Width;
    s.height = source.Height;
    s.sourceFormat = source.Format;
    s.format = format;
    LOG("Swapchain %ux%u format %d (%u images)", s.width, s.height, static_cast<int>(format), count);
    return true;
}


// ---------------------------------------------------------------------------
// Game window, for turning the laser into mouse input.
static HWND g_gameWindow = nullptr;

