// Planning outfits in the room (outfit.py, hud_outfit.inc; docs/OUTFIT-LIVE-PROGRESS.md): where its panel shows,
// as Python sends it when the panel opens (setOutfitPlace, world units): on a mirror's glass (as large as the
// glass allows, its shape kept; on a tall mirror at your eyes' height when it opened), lying in a drawer of a
// dresser or wardrobe (sliding out of the front toward you as it opens), or in front of you. It stays where it
// is while it shows (never follows you; user 2026-10-23), turned into app space every frame from the camera so it
// stays on its object while you walk. Poked with a finger or pointed at with the laser (12_hud.inc).

struct OutfitPlace {
    int kind = 0;                  // 0 none, 1 a mirror, 2 a drawer, 3 in front of you
    float c[3]{}, n[3]{}, u[3]{};  // the front's centre (a drawer: the middle of its floor at the front), out of it, up
    float w = 0, h = 0, d = 0;     // the front's width and height (a drawer: its height), the drawer's depth
};
static std::mutex g_outfitPlaceMutex;
static OutfitPlace g_outfitPlace;         // g_outfitPlaceMutex
static unsigned g_outfitPlaceSerial = 0;  // g_outfitPlaceMutex: each place sent (another drawer of the same dresser)
static unsigned g_outfitShownSerial = ~0u;  // present thread: the place shown
static float g_outfitSlide = 1;           // a drawer: 0 in, 1 all the way out
static float g_outfitEyeY = 0;            // world: your eyes' height when it opened (a tall mirror's panel)
static ULONGLONG g_outfitTick = 0;
static constexpr float kOutfitSlideSeconds = 0.3f;
static constexpr float kOutfitGlassOut = 0.006f;  // world metres in front of a mirror's glass

void setOutfitPlace(int kind, const float v[12]) {
    OutfitPlace p;
    p.kind = kind >= 1 && kind <= 3 ? kind : 0;
    for (int k = 0; k < 3; ++k) p.c[k] = v[k], p.n[k] = v[3 + k], p.u[k] = v[6 + k];
    p.w = v[9], p.h = v[10], p.d = v[11];
    {
        std::lock_guard<std::mutex> lock(g_outfitPlaceMutex);
        g_outfitPlace = p;
        ++g_outfitPlaceSerial;
    }
    static int logged = 0;
    if (logged++ < 60) {
        static const char* const kKinds[4] = {"away", "on a mirror", "in a drawer", "in front of you"};
        if (p.kind)
            LOG("Outfit panel: %s at %.2f %.2f %.2f facing %.2f %.2f %.2f, %.2f x %.2f m (depth %.2f)", kKinds[p.kind], p.c[0],
                p.c[1], p.c[2], p.n[0], p.n[1], p.n[2], p.w, p.h, p.d);
        else
            LOG("Outfit panel: away");
    }
}

static void hideOutfitHud() {
    hideHud(g_outfitSheet);
    g_outfitWanted = false;
}

// Every frame (updateHud): the panel's sheet on its place, as tall as the HUD drew it.
static void updateOutfitHud(const camera::View* view, bool firstPerson, bool headOk, const XrPosef& head) {
    OutfitPlace p;
    unsigned serial;
    {
        std::lock_guard<std::mutex> lock(g_outfitPlaceMutex);
        p = g_outfitPlace;
        serial = g_outfitPlaceSerial;
    }
    const ULONGLONG now = GetTickCount64();
    const float dt = g_outfitTick ? std::min(0.1f, static_cast<float>(now - g_outfitTick) / 1000.0f) : 0.0f;
    g_outfitTick = now;
    if (!p.kind || !firstPerson || !view || !hud::outfitPending()) {
        hideOutfitHud();
        return;
    }
    const float scale = view->scale > 0 ? view->scale : 1.0f;
    const XrQuaternionf heading{view->heading.x, view->heading.y, view->heading.z, view->heading.w};
    if (serial != g_outfitShownSerial) {
        g_outfitShownSerial = serial;
        g_outfitSlide = p.kind == 2 ? 0.0f : 1.0f;
        g_outfitEyeY = p.c[1];
        if (headOk) {
            const XrVector3f eye = rotate3(heading, {head.position.x * scale, head.position.y * scale, head.position.z * scale});
            g_outfitEyeY = view->anchor.y + eye.y;
        }
    }
    g_outfitSlide = std::min(1.0f, g_outfitSlide + dt / kOutfitSlideSeconds);
    // Its axes and size in the world: x right, y up on the panel, z out of it toward you.
    XrVector3f n = normalize3({p.n[0], p.n[1], p.n[2]}), up = normalize3({p.u[0], p.u[1], p.u[2]});
    XrVector3f x, y, z, c{p.c[0], p.c[1], p.c[2]};
    float w = std::max(0.1f, p.w), aspect = 1.5f;
    if (p.kind == 2) {
        // Lying in the drawer: its top toward the back of the dresser.
        z = up;
        y = {-n.x, -n.y, -n.z};
        x = normalize3(cross3(y, z));
        y = cross3(z, x);
        aspect = std::max(0.375f, std::min(1.5f, p.d / w));
        const float depth = w * aspect, out = g_outfitSlide * depth - depth / 2;
        c = {c.x + n.x * out + up.x * 0.002f, c.y + n.y * out + up.y * 0.002f, c.z + n.z * out + up.z * 0.002f};
    } else {
        z = n;
        x = cross3(up, z);
        if (!(dot3(x, x) > 1e-4f)) {
            hideOutfitHud();
            return;
        }
        x = normalize3(x);
        y = cross3(z, x);
        const float h = std::max(0.1f, p.h);
        aspect = std::max(0.6f, std::min(1.5f, h / w));
        if (p.kind == 1) {
            // On the glass, as large as it allows (never larger than its object).
            w *= 0.92f;
            if (w * aspect > h * 0.92f) w = h * 0.92f / aspect;
            const float ph = w * aspect, room = (h - ph) / 2 - h * 0.04f;
            if (room > 0.05f) {  // a tall mirror: at your eyes' height, a little under them
                const float lift = std::max(-room, std::min(room, g_outfitEyeY - 0.18f - c.y));
                c = {c.x + y.x * lift, c.y + y.y * lift, c.z + y.z * lift};
            }
            c = {c.x + z.x * kOutfitGlassOut, c.y + z.y * kOutfitGlassOut, c.z + z.z * kOutfitGlassOut};
        }
    }
    g_outfitAspect = aspect;
    g_outfitWanted = true;
    const float drawn = hud::outfitHeight();
    if (!(drawn > 0)) {  // not drawn yet: the sheet shows from the next frame
        hideHud(g_outfitSheet);
        return;
    }
    const float centre[3] = {c.x, c.y, c.z}, ax[3] = {x.x, x.y, x.z}, ay[3] = {y.x, y.y, y.z};
    const XrVector3f at = worldToApp(*view, centre, false);
    const XrVector3f appX = normalize3(worldToApp(*view, ax, true)), appY = normalize3(worldToApp(*view, ay, true));
    if (!g_outfitSheet.m.active) {
        static int logged = 0;
        if (logged++ < 40)
            LOG("Outfit panel: shown %.2f x %.2f m (%s)", w, w * drawn / hud::kOutfitArea.w,
                p.kind == 1 ? "mirror" : p.kind == 2 ? "drawer" : "in front");
    }
    g_outfitSheet.onTop = false;
    placeHud(g_outfitSheet, at, appX, appY, w / scale, {hud::kOutfitArea.x, hud::kOutfitArea.y, hud::kOutfitArea.w, drawn});
}
