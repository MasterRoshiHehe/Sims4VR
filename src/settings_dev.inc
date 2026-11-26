// Developer-only rows (included by settings.cpp, inside its namespace).
// The "Developer options" switch (Options page, [debug] dev_options; default on in dev builds, off in a
// tools/release.sh --public build) shows or hides them. Hidden rows stay where they are, keep their saved
// values and keep working; only the menu line goes. A page whose rows are all hidden (and the link to it)
// goes too. RULE: a new testing / diagnostic / experimental row sets `.dev = true` in makeRows or is listed here.
// A genuine player's rows (display, performance, graphics, sound, controls, comfort, phone/HUD choices) are never dev.
#ifdef S4VR_PUBLIC
constexpr bool kDevDefault = false;
#else
constexpr bool kDevDefault = true;
#endif

// Pages (groups) that are developer-only as a whole, with their sub pages.
const wchar_t* const kDevPages[] = {L"Testing", L"View eyes", L"Logging"};

// Rows by ini "section.key".
const char* const kDevKeys[] = {
    "influence.ignore_willingness",  // test: Influence works on Sims who refuse
    "foliage.lift_cm",       // test: every leaf rises
    "foliage.patch_shaders", // hook shader creation (next start)
    "body.god_hands",        // selected Sim's hands in god mode (testing)
    "photo.game_camera",     // switch off the photo session (testing)
    "lightswitch.log",       // log the switches found and flips
    "knock.log",             // log the doors and windows found and knocks
    "passive.log",           // log what the game's objects do to you
    "stereo.probe_double",   // draws the scene twice for the log
    "perf.hitch_catcher",    // stall logger
    "ragdoll.test_on_hit",   // test hack
    "ui.own_pie",            // old way vs own drawing
    "bubbles.cut_others",    // old cut method
    "bubbles.own_balloons",  // old cut method vs own drawing
    "wrist.lot_intro_crop_w", "wrist.lot_intro_crop_h", "wrist.lot_intro_crop_y",  // cut-out tuning
    "outfit.all_variants", "outfit.apply_route", "outfit.log",  // outfit planning tests (the probe row sets .dev)
};

// Rows without an ini key (buttons), by label.
const wchar_t* const kDevLabels[] = {
    L"Run turn test (game paused)", L"Run turn test (game running)",
    L"Timing log on", L"Timing log off", L"Profile 30 s", L"Profile 60 s",
};

bool devPage(int page) {
    for (int p = page, guard = 0; p > 0 && p < pageCount() && guard < 8; p = parentOf(p), ++guard)
        for (const wchar_t* g : kDevPages)
            if (!wcscmp(g_pages[p].group, g)) return true;
    return false;
}

void applyDev(std::vector<Row>& rows) {
    for (Row& r : rows) {
        if (r.kind == Kind::Back) continue;
        if (r.dev || devPage(r.page) || (r.kind == Kind::Link && devPage(r.linkPage))) r.dev = true;
        if (r.key && r.section) {
            const std::string full = std::string(r.section) + "." + r.key;
            for (const char* k : kDevKeys)
                if (full == k) r.dev = true;
        }
        for (const wchar_t* l : kDevLabels)
            if (!wcscmp(r.label, l)) r.dev = true;
    }
}
