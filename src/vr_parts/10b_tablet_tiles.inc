// The build tablet's screen: the whole vanilla build/buy UI scaled by the mod to the tablet's screen (user
// 2026-10-24: the game's own UI scale and its viewport could not be changed from here; 13:41Z: the full UI, cut and
// placed with the old crop-outs' technique, not the old palette layout).
// Included by vr.cpp after 10_phone_screen.inc.

// The old palette's scan (v0.13.96): "something solid in the middle" shows the whole UI as one picture.
struct TabletScan {
    bool middle = false;     // something solid in the middle of the UI
    bool tip = false;        // the item tooltip, in UI pixels:
    int tipBox[4] = {};      // x0, y0, x1, y1
    bool popup = false;      // a list popped up over the catalog's left panel (categories), in UI pixels:
    int popBox[4] = {};      // x0, y0, x1, y1
    int leftTop = 0;         // the catalog's left panel's top (its search box), 0 = not found
    int gridTop = 0;         // the catalog items' top, 0 = not found
    int gaps[64] = {};       // the gaps between the catalog's items (their middles), left to right
    int gapCount = 0;
};
static TabletScan g_tabletScan;
static ID3D11Texture2D* g_tabletScanCopy = nullptr;
static bool g_tabletScanPending = false;
static int g_tabletScanY0 = 0, g_tabletScanWait = 0, g_tabletMiddleRuns = 0;
static int g_tabletLeftSeen = 0, g_tabletGridSeen = 0;  // a new top, seen once so far
static ULONGLONG g_tabletScanAt = 0;

// The tablet's band of the UI: from 15% down to the bottom.
static void tabletScanBand(int W, int H, int& y0, int& y1) {
    y0 = H * 15 / 100;
    y1 = H;
    (void)W;
}

static void readTabletScan(const uint8_t* px, UINT pitch, int W, int H) {
    float s = H / 2808.0f;
    int y0, y1;
    tabletScanBand(W, H, y0, y1);
    auto alpha = [&](int x, int y) { return px[static_cast<size_t>(y - y0) * pitch + static_cast<size_t>(x) * 4 + 3]; };
    // The middle: rows above the tooltip's band, clear of the catalog's left panel, every 16 px.
    int midEnd = H - static_cast<int>(600 * s), solid = 0, total = 0;
    for (int y = y0; y < midEnd; y += 16)
        for (int x = static_cast<int>(600 * s); x < W - static_cast<int>(600 * s); x += 16, ++total)
            solid += alpha(x, y) > 200;
    bool middle = total > 0 && solid * 25 > total;  // over 4% of it solid
    g_tabletMiddleRuns = middle == g_tabletScan.middle ? 0 : g_tabletMiddleRuns + 1;
    if (g_tabletMiddleRuns >= 2) {  // twice in a row before it changes
        g_tabletScan.middle = middle;
        g_tabletMiddleRuns = 0;
        static int logged = 0;
        if (logged++ < 20)
            LOG("Tablet: %s", middle ? "something solid in the middle of the UI: the whole UI on the tablet"
                                     : "the middle is clear again: the palette layout");
    }
    // A panel's top: up from the bottom at column x while the UI is solid (gaps up to 12 px).
    auto topAt = [&](int x) {
        int top = H, gap = 0;
        for (int y = H - 1; y >= y0; --y) {
            if (alpha(x, y) > 120) {
                top = y;
                gap = 0;
            } else if (++gap > static_cast<int>(12 * s) + 1) {
                break;
            }
        }
        return top;
    };
    // A top changes only when found the same twice in a row; not found (something
    // standing on it, like the tooltip) keeps the last one.
    auto settle = [](int found, int& kept, int& seen) {
        if (!found || found == kept) {
            seen = 0;
            return;
        }
        if (seen && std::abs(found - seen) <= 2) {
            kept = found;
            seen = 0;
        } else {
            seen = found;
        }
    };
    int leftTop = topAt(static_cast<int>(40 * s));    // through the search box
    int gridTop = topAt(static_cast<int>(700 * s));   // through the first item's column
    int oldLeft = g_tabletScan.leftTop, oldGrid = g_tabletScan.gridTop;
    settle(H - leftTop >= 150 * s && H - leftTop <= H / 2 ? leftTop : 0, g_tabletScan.leftTop, g_tabletLeftSeen);
    settle(H - gridTop >= 120 * s && H - gridTop <= 400 * s ? gridTop : 0, g_tabletScan.gridTop, g_tabletGridSeen);
    if (g_tabletScan.leftTop != oldLeft || g_tabletScan.gridTop != oldGrid) {
        static int logged = 0;
        if (logged++ < 30)
            LOG("Tablet: catalog's left panel from %d px up, items from %d px up (of %d; 0 = not found)",
                g_tabletScan.leftTop ? H - g_tabletScan.leftTop : 0, g_tabletScan.gridTop ? H - g_tabletScan.gridTop : 0, H);
    }
    // The gaps between the items, along their first row (runs of 3+ clear pixels).
    g_tabletScan.gapCount = 0;
    if (g_tabletScan.gridTop) {
        int gy = std::min(H - 1, g_tabletScan.gridTop + static_cast<int>(30 * s)), run = 0;
        for (int x = static_cast<int>(512 * s); x < W; ++x) {
            if (alpha(x, gy) < 60) {
                ++run;
            } else {
                if (run >= 3 && g_tabletScan.gapCount < 64) g_tabletScan.gaps[g_tabletScan.gapCount++] = x - (run + 1) / 2;
                run = 0;
            }
        }
    }
    // The tooltip: the solid box in the band above the catalog, right of its left panel.
    int ta = H - static_cast<int>(600 * s), tb = H - static_cast<int>(300 * s), xa = static_cast<int>(512 * s);
    int bx0 = W, bx1 = -1, by0 = H, by1 = -1;
    for (int y = std::max(ta, y0); y < std::min(tb, y1); y += 4)
        for (int x = xa; x < W; x += 4)
            if (alpha(x, y) > 200) {
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
    bool tip = bx1 - bx0 > 100 * s && by1 - by0 > 60 * s;
    if (tip) {
        // Its bottom reaches down past the band: follow its middle down, to the items' top.
        int cx = (bx0 + bx1) / 2, stop = H - static_cast<int>(238 * s);
        while (by1 + 2 < stop && alpha(cx, by1 + 2) > 200) by1 += 2;
        g_tabletScan.tipBox[0] = bx0;
        g_tabletScan.tipBox[1] = by0;
        g_tabletScan.tipBox[2] = bx1 + 1;
        g_tabletScan.tipBox[3] = by1 + 1;
    }
    g_tabletScan.tip = tip;
    // A popped-up list (the categories' "Show All"): solid above the left panel's search box,
    // within the panel's width; it reaches down to the panel along its right side.
    int panelTop = g_tabletScan.leftTop ? g_tabletScan.leftTop : H - static_cast<int>(298 * s);
    int px0 = W, px1 = -1, py0 = H, py1 = -1;
    for (int y = y0; y < panelTop - static_cast<int>(4 * s); y += 4)
        for (int x = static_cast<int>(8 * s); x < static_cast<int>(504 * s); x += 4)
            if (alpha(x, y) > 200) {
                px0 = std::min(px0, x);
                px1 = std::max(px1, x);
                py0 = std::min(py0, y);
                py1 = std::max(py1, y);
            }
    bool popup = px1 - px0 > 150 * s && py1 - py0 > 60 * s;
    if (popup) {
        int cx = px1 - static_cast<int>(10 * s), stop = std::min(H - 1, panelTop + static_cast<int>(60 * s)), gap = 0;
        for (int y = py1 + 1; y < stop; ++y) {
            if (alpha(cx, y) > 120) {
                py1 = y;
                gap = 0;
            } else if (++gap > 3) {
                break;
            }
        }
        g_tabletScan.popBox[0] = px0;
        g_tabletScan.popBox[1] = py0;
        g_tabletScan.popBox[2] = px1 + 1;
        g_tabletScan.popBox[3] = py1 + 1;
    }
    if (popup != g_tabletScan.popup) {
        static int logged = 0;
        if (logged++ < 20) {
            if (popup)
                LOG("Tablet: a list popped up over the catalog's left panel (%d,%d to %d,%d): beside the panel",
                    px0, py0, px1 + 1, py1 + 1);
            else
                LOG("Tablet: the popped-up list is gone");
        }
    }
    g_tabletScan.popup = popup;
}

// While the tablet is out: a copy of the UI's band about 3 times a second, read a frame or two later.
static void updateTabletScan(ULONGLONG now) {
    if (!g_uiFrame) return;
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    if (!alphaReadable(desc)) return;
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    int W = static_cast<int>(desc.Width), H = static_cast<int>(desc.Height), y0, y1;
    tabletScanBand(W, H, y0, y1);
    if (g_tabletScanPending && g_tabletScanCopy) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(g_tabletScanCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++g_tabletScanWait < 30) {
            // next frame
        } else {
            if (SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC have;
                g_tabletScanCopy->GetDesc(&have);
                if (static_cast<int>(have.Width) == W && g_tabletScanY0 == y0 &&
                    static_cast<int>(have.Height) == y1 - y0)
                    readTabletScan(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, W, H);
                context->Unmap(g_tabletScanCopy, 0);
            }
            g_tabletScanPending = false;
        }
    } else if (now >= g_tabletScanAt) {
        g_tabletScanAt = now + 300;
        if (g_tabletScanCopy) {
            D3D11_TEXTURE2D_DESC have;
            g_tabletScanCopy->GetDesc(&have);
            if (static_cast<int>(have.Width) != W || static_cast<int>(have.Height) != y1 - y0 ||
                have.Format != desc.Format) {
                g_tabletScanCopy->Release();
                g_tabletScanCopy = nullptr;
            }
        }
        if (!g_tabletScanCopy) {
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = static_cast<UINT>(W);
            staging.Height = static_cast<UINT>(y1 - y0);
            staging.MipLevels = staging.ArraySize = 1;
            staging.Format = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            device->CreateTexture2D(&staging, nullptr, &g_tabletScanCopy);
        }
        if (g_tabletScanCopy) {
            D3D11_BOX box{0, static_cast<UINT>(y0), 0, static_cast<UINT>(W), static_cast<UINT>(y1), 1};
            context->CopySubresourceRegion(g_tabletScanCopy, 0, 0, 0, 0, g_uiFrame, 0, &box);
            g_tabletScanY0 = y0;
            g_tabletScanPending = true;
            g_tabletScanWait = 0;
        }
    }
    context->Release();
    device->Release();
}

// Once a session, the first time the tablet comes out: the whole UI as the tablet gets it, saved next to
// sims4vr.log (tablet_ui.bmp, 32 bit with alpha), to check the cut-outs against.
static std::wstring g_tabletDumpPath;
static bool g_tabletDumped = false;
static ID3D11Texture2D* g_tabletDumpCopy = nullptr;
static int g_tabletDumpWait = 0;

static void dumpTabletUi() {
    if (g_tabletDumped || !g_uiFrame) return;
    if (g_tabletDumpPath.empty()) {
        HMODULE self = nullptr;
        wchar_t file[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&dumpTabletUi), &self) &&
            GetModuleFileNameW(self, file, MAX_PATH)) {
            std::wstring dir(file);
            dir = dir.substr(0, dir.find_last_of(L"\\/"));
            g_tabletDumpPath = dir + L"\\tablet_ui.bmp";
        } else {
            g_tabletDumped = true;
            return;
        }
    }
    D3D11_TEXTURE2D_DESC desc;
    g_uiFrame->GetDesc(&desc);
    if (!alphaReadable(desc) || desc.SampleDesc.Count != 1) {
        g_tabletDumped = true;
        LOG("Tablet: UI picture not saved (format %d, %u samples)", static_cast<int>(desc.Format), desc.SampleDesc.Count);
        return;
    }
    ID3D11Device* device = nullptr;
    g_uiFrame->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!g_tabletDumpCopy) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.MipLevels = staging.ArraySize = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.MiscFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &g_tabletDumpCopy)))
            context->CopyResource(g_tabletDumpCopy, g_uiFrame);
        g_tabletDumpWait = 0;
    } else {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context->Map(g_tabletDumpCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING || ++g_tabletDumpWait >= 30) {
            g_tabletDumped = true;
            if (SUCCEEDED(hr)) {
                const int W = static_cast<int>(desc.Width), H = static_cast<int>(desc.Height);
                std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
                for (int y = 0; y < H; ++y)  // bottom-up rows
                    std::memcpy(&px[static_cast<size_t>(H - 1 - y) * W * 4],
                                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                static_cast<size_t>(W) * 4);
                context->Unmap(g_tabletDumpCopy, 0);
                const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                std::wstring path = g_tabletDumpPath;
                std::thread([px = std::move(px), W, H, rgba, path]() mutable {
                    if (rgba)
                        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
                    uint8_t head[54] = {'B', 'M'};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
                    put32(2, static_cast<uint32_t>(54 + px.size()));
                    put32(10, 54);
                    put32(14, 40);
                    put32(18, static_cast<uint32_t>(W));
                    put32(22, static_cast<uint32_t>(H));
                    head[26] = 1;
                    head[28] = 32;
                    put32(34, static_cast<uint32_t>(px.size()));
                    FILE* f = _wfopen(path.c_str(), L"wb");
                    if (!f) return;
                    std::fwrite(head, 1, sizeof head, f);
                    std::fwrite(px.data(), 1, px.size(), f);
                    std::fclose(f);
                }).detach();
                LOG("Tablet: the UI picture (%dx%d) saved as tablet_ui.bmp next to the log", W, H);
            }
            g_tabletDumpCopy->Release();
            g_tabletDumpCopy = nullptr;
        }
    }
    context->Release();
    device->Release();
}

// The tablet's screen shape (width / height): [tablet] height's, else the 1.4 it keeps (phoneSize).
static float tabletScreenAspect() {
    const float b = g_config.tabletBezel, w = g_config.tabletWidth - 2 * b, h = g_config.tabletHeight - 2 * b;
    return g_config.tabletHeight >= 0.08f && h > 0.01f ? w / h : 1.4f;
}

// The whole UI on the tablet's screen, scaled by the mod (user 2026-10-24 13:41Z: the full UI, not the old palette):
// cut into parts, each keeps its anchor (left / middle / right, top / middle / bottom) on the tablet's
// screen, so the full UI is laid out as the game lays it out on a screen of the tablet's shape, at [tablet]
// menu_size ("Build menu size"): 1 = the UI's height fills the screen's height, 2 = twice as big. Parts that would
// run into each other are cut between them (the middle third keeps 30 % of the screen). 0 = the whole UI as one picture (something solid in its
// middle: a dialog, the game menu). Place on the screen 0..1 from its top left, part of the UI in pixels.
static int tabletLayout(int W, int H, float dst[][4], float src[][4], float& aspect) {
    aspect = tabletScreenAspect();
    if (g_tabletScan.middle) return 0;
    const float Wf = static_cast<float>(W), Hf = static_cast<float>(H);
    const float zoom = std::max(0.3f, g_config.tabletUiScale);
    const float SH = Hf / zoom, SW = SH * aspect;  // the screen in UI pixels
    // One axis: the UI's [0, size) in three parts, each placed on the screen's [0, room): the first from 0, the last
    // up to room, the middle one centred; a part is cut where the screen has no room left for it.
    struct Span { float from0, from1, at0; };
    auto spans = [](float size, float room, Span out[3]) {
        const float third = size / 3;
        if (room >= size) {  // more room than UI: the parts move apart
            out[0] = {0, third, 0};
            out[1] = {third, 2 * third, (room - third) / 2};
            out[2] = {2 * third, size, room - third};
        } else {  // less: each keeps its anchor; the middle gets 30 % of the room (the tool bar), the edges the rest
            const float m = std::min(third, room * 0.3f), e = (room - m) / 2;
            out[0] = {0, e, 0};
            out[1] = {(size - m) / 2, (size + m) / 2, e};
            out[2] = {size - e, size, e + m};
        }
    };
    int n = 0;
    auto put = [&](float x0, float y0, float x1, float y1, float atX, float atY, float k) {
        dst[n][0] = atX / SW;
        dst[n][1] = atY / SH;
        dst[n][2] = (atX + (x1 - x0) * k) / SW;
        dst[n][3] = (atY + (y1 - y0) * k) / SH;
        src[n][0] = x0;
        src[n][1] = y0;
        src[n][2] = x1;
        src[n][3] = y1;
        ++n;
    };
    Span xs[3];
    spans(Wf, SW, xs);
    if (SH >= Hf) {  // the whole UI's height fits: a 3 x 3 grid, the parts moved apart
        Span ys[3];
        spans(Hf, SH, ys);
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                put(xs[i].from0, ys[j].from0, xs[i].from1, ys[j].from1, xs[i].at0, ys[j].at0, 1);
    } else {
        // Taller than the screen (user 2026-10-24 14:24Z: the catalog, its tooltips and lists were cut): the empty
        // middle goes. The top 40 % of the screen: the UI's top in three parts (lot name, tool bar, mode buttons and
        // notifications), each at its anchor. The bottom 60 %: the UI's bottom across its whole width in one piece
        // (the catalog's panel, items, tooltip and popped-up lists stay whole), made smaller when it is wider than
        // the screen.
        const float topH = std::min(Hf / 2, SH * 0.4f);
        for (int i = 0; i < 3; ++i) put(xs[i].from0, 0, xs[i].from1, topH, xs[i].at0, 0, 1);
        const float k = std::min(1.0f, SW / Wf), bottomH = std::min(Hf - topH, (SH - topH) / k);
        put(0, Hf - bottomH, Wf, Hf, 0, SH - bottomH * k, k);
    }
    static float loggedZoom = 0, loggedAspect = 0;
    static int logs = 0;
    if ((zoom != loggedZoom || aspect != loggedAspect) && logs++ < 60) {
        loggedZoom = zoom;
        loggedAspect = aspect;
        LOG("Tablet: build menu at %.0f %%: the whole UI (%dx%d) laid out on a %.0fx%.0f px screen (%.2f wide)",
            zoom * 100, W, H, SW, SH, aspect);
    }
    return n;
}

// The tablet put away or brought out: the pieces are found again.
static void resetTabletScan() {
    g_tabletScan = TabletScan{};
    g_tabletLeftSeen = g_tabletGridSeen = 0;
    g_tabletMiddleRuns = 0;
}
