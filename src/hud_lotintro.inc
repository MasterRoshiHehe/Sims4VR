// The lot's name and traits when you arrive (user 2026-10-26): the game's own arrival banner (ScreenSlam travel
// billboard: TransitionScreenMolecule.Hide -> ShowScreenSlam; lot name, household, trait icons, challenges, 4 s) is
// Flash UI. Drawn by us it looked wrong (23:48Z), so it is a crop of the vanilla UI's rendered banner instead (the
// only element allowed to be: it shows only while the eyes are shut). Python (lotintro.py) starts it when the eyes
// stay shut for it; vr_parts/12_hud.inc shows the middle of the UI in front of you for that long. Included by hud.cpp.

namespace {
std::mutex g_lotMutex;
ULONGLONG g_lotStart = 0;  // g_lotMutex
float g_lotSeconds = 0;    // g_lotMutex
constexpr float kLotTween = 0.2f;
}  // namespace

void setLotIntro(const char* data, size_t size) {
    const std::string all(data ? data : "", data ? size : 0);
    std::lock_guard<std::mutex> lock(g_lotMutex);
    g_lotSeconds = all.empty() ? 0.0f : std::max(0.5f, static_cast<float>(std::atof(all.c_str())));
    g_lotStart = GetTickCount64();
}

bool lotIntroShowing() {
    std::lock_guard<std::mutex> lock(g_lotMutex);
    return g_lotSeconds > 0 && (GetTickCount64() - g_lotStart) / 1000.0f < g_lotSeconds + kLotTween;
}
