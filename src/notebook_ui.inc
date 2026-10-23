// The game's notebook in your Sim's book (included by hud.cpp; thread "Notebook in VR", user 2026-10-23): the
// pages of the Notebook widget (NotebookMain: the owner page with the notes and a grid of category cells, then
// each category's subcategories and entries) drawn into the atlas with the game's own page art (Notebook SWF:
// the page with its leather rim and rings, the category cell, the portrait ring, the page curl), and the
// leather cover drawn by us. Read only. notebook.py sends the data (setNotebookContent); vr.cpp shows the
// regions on a book in your hand (phone3d::setBook) and turns the spread (Frame::bookSpread).

struct NbItem {
    std::string textJson;
    uint64_t icon = 0;
    int count = 0, total = 0;
};
struct NbEntry {
    std::string textJson, descJson;
    uint64_t icon = 0;
    bool isNew = false;
    std::vector<NbItem> items;
};
struct NbSub {
    std::string nameJson;
    uint64_t icon = 0;
    std::vector<NbEntry> entries;
};
struct NbCat {
    std::string nameJson, descJson;
    uint64_t icon = 0, large = 0;
    std::vector<NbSub> subs;
};
struct NbContent {
    bool valid = false;
    std::string name, notes;
    uint64_t portrait = 0;
    std::vector<NbCat> cats;
};
NbContent g_nbIn, g_nb;             // g_nbMutex / present thread
std::mutex g_nbMutex;
bool g_nbNew = false;               // g_nbMutex
std::atomic<int> g_nbSpreads{1};

// One thing on a page, placed top-down. kind: 0 the category's title block, 1 a subcategory's heading,
// 2 an entry, 3 an entry's list item.
struct NbLine {
    int kind = 0;
    const NbCat* cat = nullptr;
    const NbSub* sub = nullptr;
    const NbEntry* entry = nullptr;
    const NbItem* item = nullptr;
    float h = 0;
};
struct NbPage {
    int kind = 2;  // 0 the owner page, 1 the categories, 2 a category's page
    std::vector<NbLine> lines;
    std::string empty;  // a category without entries
};
std::vector<NbPage> g_nbPages;      // even index = a left page; present thread
bool g_nbBuilt = false;
bool g_nbUnresolved = false;        // a text was not found yet: build again

// Page geometry in atlas pixels (a page is kBookLeft: 512 x 688; the right one is the art mirrored, so its
// spine side is on the left).
constexpr float kNbTop = 62, kNbBottom = 636, kNbSpine = 40, kNbOuter = 58, kNbW = 512;
constexpr float kNbBody = 25, kNbSmall = 21, kNbHead = 32;
const ImU32 kNbBlue = IM_COL32(20, 70, 150, 255), kNbInk = IM_COL32(40, 52, 86, 255), kNbGrey = IM_COL32(104, 92, 80, 255),
            kNbGreen = IM_COL32(36, 142, 58, 255), kNbNew = IM_COL32(60, 190, 60, 255);

float nbLeft(bool right) { return right ? kNbSpine : kNbOuter; }
float nbRight(bool right) { return kNbW - (right ? kNbOuter : kNbSpine); }

const std::string& nbFound(const std::string& json) {
    const std::string& s = found(json);
    if (!json.empty() && s.empty()) g_nbUnresolved = true;
    return s;
}

float nbTextHeight(ImFont* font, float size, float wrap, const std::string& text) {
    if (text.empty()) return 0;
    return font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str()).y;
}

// A category's lines (after its title block on the first page).
void nbCategoryLines(const NbCat& cat, std::vector<NbLine>& lines) {
    NbLine head;
    head.kind = 0;
    head.cat = &cat;
    const float wrap = nbRight(false) - nbLeft(false);
    head.h = 150 + nbTextHeight(g_text, kNbSmall, wrap, nbFound(cat.descJson)) + 14;
    lines.push_back(head);
    for (const NbSub& sub : cat.subs) {
        NbLine s;
        s.kind = 1;
        s.cat = &cat;
        s.sub = &sub;
        s.h = sub.nameJson.empty() ? 0 : 44;
        if (s.h > 0) lines.push_back(s);
        for (const NbEntry& e : sub.entries) {
            NbLine l;
            l.kind = 2;
            l.cat = &cat;
            l.sub = &sub;
            l.entry = &e;
            const float tw = wrap - 56;
            l.h = std::max(48.0f, 8 + nbTextHeight(g_title, kNbBody, tw, nbFound(e.textJson)) +
                                      nbTextHeight(g_text, kNbSmall, tw, nbFound(e.descJson)) + 6);
            lines.push_back(l);
            for (const NbItem& it : e.items) {
                NbLine i;
                i.kind = 3;
                i.cat = &cat;
                i.sub = &sub;
                i.entry = &e;
                i.item = &it;
                i.h = std::max(30.0f, nbTextHeight(g_text, kNbSmall, tw - 40, nbFound(it.textJson)) + 8);
                lines.push_back(i);
            }
        }
    }
}

void nbBuild() {
    g_nbPages.clear();
    g_nbUnresolved = false;
    NbPage home, grid;
    home.kind = 0;
    grid.kind = 1;
    g_nbPages.push_back(home);
    g_nbPages.push_back(grid);
    const float room = kNbBottom - kNbTop;
    for (const NbCat& cat : g_nb.cats) {
        if (g_nbPages.size() % 2 == 1) g_nbPages.push_back(NbPage{});  // a category starts on a left page
        std::vector<NbLine> lines;
        nbCategoryLines(cat, lines);
        NbPage page;
        page.kind = 2;
        float used = 0;
        bool any = false;
        for (const NbLine& l : lines) {
            if (used + l.h > room && !page.lines.empty()) {
                g_nbPages.push_back(page);
                page = NbPage{};
                page.kind = 2;
                used = 0;
            }
            page.lines.push_back(l);
            used += l.h;
            any = any || l.kind >= 2;
        }
        if (!any) page.empty = "Nothing here yet.";
        g_nbPages.push_back(page);
    }
    if (g_nbPages.size() % 2 == 1) g_nbPages.push_back(NbPage{});
    g_nbSpreads = static_cast<int>(g_nbPages.size() / 2);
    g_nbBuilt = true;
    // The icons to read (all categories; the entries of the pages near the spread shown are added by the draw).
    std::vector<uint64_t> icons;
    for (const NbCat& c : g_nb.cats) icons.push_back(c.icon), icons.push_back(c.large);
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_nbIcons.swap(icons);
}

// The page art as drawn: the left page as it is, the right page mirrored.
void nbPageArt(ImDrawList* d, const Rect& r, bool right) {
    const art::Piece& p = art::kNbPage;
    const float u0 = static_cast<float>(p.x) / art::kAtlasW, u1 = static_cast<float>(p.x + p.w) / art::kAtlasW;
    const float v0 = static_cast<float>(p.y) / art::kAtlasH, v1 = static_cast<float>(p.y + p.h) / art::kAtlasH;
    d->AddImage(reinterpret_cast<ImTextureID>(g_art), {r.x, r.y}, {r.x + r.w, r.y + r.h}, {right ? u1 : u0, v0},
                {right ? u0 : u1, v1});
}

void nbCentred(ImDrawList* d, ImFont* font, float size, ImVec2 mid, ImU32 colour, const std::string& text, float wrap) {
    if (text.empty()) return;
    ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
    d->AddText(font, size, {mid.x - s.x / 2, mid.y}, colour, text.c_str(), nullptr, wrap);
}

void nbHome(ImDrawList* d, const Rect& r) {
    auto at = [&](float x, float y) { return ImVec2{r.x + x, r.y + y}; };
    const float mid = (nbLeft(false) + nbRight(false)) / 2;
    nbCentred(d, g_title, 40, at(mid, 80), kNbBlue, "PROPERTY OF:", 0);
    piece(d, art::kNbPortrait, at(mid, 250), 0.78f);
    if (g_nb.portrait) icon(d, g_nb.portrait, at(mid, 250), 116);
    nbCentred(d, g_title, 30, at(mid, 346), kNbBlue, g_nb.name, nbRight(false) - nbLeft(false));
    const ImVec2 a = at(nbLeft(false), 410), b = at(nbRight(false), 610);
    d->AddRectFilled(a, b, IM_COL32(255, 250, 235, 60), 6);
    d->PushClipRect(a, b, true);
    if (!g_nb.notes.empty())
        d->AddText(g_text, kNbSmall + 2, {a.x + 8, a.y + 8}, kNbGreen, g_nb.notes.c_str(), nullptr, b.x - a.x - 16);
    d->PopClipRect();
}

void nbCategories(ImDrawList* d, const Rect& r) {
    auto at = [&](float x, float y) { return ImVec2{r.x + x, r.y + y}; };
    const float x0 = nbLeft(true), cw = 206, ch = 96, gap = 10;
    const int n = static_cast<int>(g_nb.cats.size());
    for (int i = 0; i < n && i < 12; ++i) {
        const NbCat& c = g_nb.cats[i];
        const float x = x0 + (i % 2) * (cw + gap), y = 72 + (i / 2) * (ch + gap);
        pieceIn(d, art::kNbCell, at(x, y), at(x + cw, y + ch));
        if (c.icon) icon(d, c.icon, at(x + 40, y + ch / 2), 64);
        const std::string& name = nbFound(c.nameJson);
        const float wrap = cw - 88;
        const float th = nbTextHeight(g_title, kNbBody, wrap, name);
        d->AddText(g_title, kNbBody, {at(x + 80, 0).x, at(0, y + (ch - th) / 2).y}, kNbBlue, name.c_str(), nullptr, wrap);
    }
}

// A category's page: its title block (large icon, name, description) first, then what follows.
void nbBody(ImDrawList* d, const Rect& r, const NbPage& page, bool right) {
    const float l = r.x + nbLeft(right), w = nbRight(right) - nbLeft(right);
    float y = r.y + kNbTop;
    for (const NbLine& line : page.lines) {
        const float top = y;
        if (line.kind == 0) {
            const NbCat& c = *line.cat;
            if (c.large || c.icon) icon(d, c.large ? c.large : c.icon, {l + 52, top + 56}, 100);
            const std::string& name = nbFound(c.nameJson);
            d->AddText(g_title, 38, {l + 118, top + 28}, kNbBlue, name.c_str(), nullptr, w - 118);
            const std::string& desc = nbFound(c.descJson);
            if (!desc.empty()) d->AddText(g_text, kNbSmall, {l, top + 126}, kNbGrey, desc.c_str(), nullptr, w);
        } else if (line.kind == 1) {
            const NbSub& s = *line.sub;
            if (s.icon) icon(d, s.icon, {l + 16, top + 22}, 30);
            d->AddText(g_title, 28, {l + 40, top + 6}, kNbBlue, nbFound(s.nameJson).c_str(), nullptr, w - 40);
            d->AddLine({l, top + 40}, {l + w, top + 40}, IM_COL32(120, 80, 40, 90), 2);
        } else if (line.kind == 2) {
            const NbEntry& e = *line.entry;
            d->AddRectFilled({l, top + 2}, {l + w, top + line.h - 2}, IM_COL32(255, 252, 240, 70), 6);
            if (e.icon) icon(d, e.icon, {l + 26, top + line.h / 2}, 40);
            const std::string& text = nbFound(e.textJson);
            const float tw = w - 56;
            d->AddText(g_title, kNbBody, {l + 52, top + 6}, kNbInk, text.c_str(), nullptr, tw);
            const std::string& desc = nbFound(e.descJson);
            if (!desc.empty())
                d->AddText(g_text, kNbSmall, {l + 52, top + 8 + nbTextHeight(g_title, kNbBody, tw, text)}, kNbGrey, desc.c_str(),
                           nullptr, tw);
            if (e.isNew) d->AddCircleFilled({l + w - 10, top + 12}, 6, kNbNew);
        } else {
            const NbItem& it = *line.item;
            if (it.icon) icon(d, it.icon, {l + 40, top + line.h / 2}, 24);
            const std::string& text = nbFound(it.textJson);
            d->AddText(g_text, kNbSmall, {l + 60, top + 4}, kNbInk, text.c_str(), nullptr, w - 120);
            if (it.total > 0) {
                char count[24];
                std::snprintf(count, sizeof count, "%d/%d", it.count, it.total);
                ImVec2 s = g_text->CalcTextSizeA(kNbSmall, FLT_MAX, 0, count);
                d->AddText(g_text, kNbSmall, {l + w - s.x - 6, top + 4}, it.count >= it.total ? kNbGreen : kNbGrey, count);
            }
        }
        y += line.h;
    }
    if (!page.empty.empty())
        nbCentred(d, g_text, kNbBody, {l + w / 2, r.y + kNbTop + 200}, kNbGrey, page.empty, w);
}

// The leather cover seen from outside, its spine along the left edge.
void nbCover(ImDrawList* d, const Rect& r) {
    const ImVec2 a{r.x, r.y}, b{r.x + r.w, r.y + r.h};
    const ImU32 top = IM_COL32(118, 62, 34, 255), bottom = IM_COL32(86, 42, 22, 255);
    d->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
    uint32_t seed = 0x2545F491u;
    auto rnd = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (seed & 0xffff) / 65535.0f;
    };
    for (int i = 0; i < 1400; ++i) {  // the grain
        const float x = a.x + rnd() * r.w, y = a.y + rnd() * r.h, s = 2 + rnd() * 5;
        const bool light = rnd() > 0.5f;
        d->AddRectFilled({x, y}, {x + s, y + s * (0.5f + rnd())}, light ? IM_COL32(190, 120, 80, 22) : IM_COL32(40, 15, 6, 30));
    }
    // The spine's roll and the page edge's shadow.
    d->AddRectFilledMultiColor(a, {a.x + 56, b.y}, IM_COL32(30, 12, 4, 200), IM_COL32(30, 12, 4, 0), IM_COL32(30, 12, 4, 0),
                               IM_COL32(30, 12, 4, 200));
    d->AddRectFilledMultiColor({b.x - 24, a.y}, b, IM_COL32(30, 12, 4, 0), IM_COL32(30, 12, 4, 110), IM_COL32(30, 12, 4, 110),
                               IM_COL32(30, 12, 4, 0));
    // The embossed frame and the stitching.
    const float in = 34;
    d->AddRect({a.x + in + 22, a.y + in}, {b.x - in, b.y - in}, IM_COL32(40, 16, 6, 255), 6, 0, 5);
    d->AddRect({a.x + in + 24, a.y + in + 2}, {b.x - in - 2, b.y - in - 2}, IM_COL32(176, 112, 70, 150), 6, 0, 2);
    for (float x = a.x + 70; x < b.x - 20; x += 16) {
        d->AddLine({x, a.y + 18}, {x + 8, a.y + 18}, IM_COL32(220, 190, 140, 190), 2.5f);
        d->AddLine({x, b.y - 18}, {x + 8, b.y - 18}, IM_COL32(220, 190, 140, 190), 2.5f);
    }
    for (float y = a.y + 20; y < b.y - 20; y += 16)
        d->AddLine({b.x - 18, y}, {b.x - 18, y + 8}, IM_COL32(220, 190, 140, 190), 2.5f);
    // A gilded title.
    const ImU32 gold = IM_COL32(222, 184, 96, 255), shade = IM_COL32(40, 16, 6, 200);
    const ImVec2 mid{a.x + 34 + r.w / 2, a.y + 250};
    for (int k = 0; k < 2; ++k) {
        const float o = k == 0 ? 2.0f : 0.0f;
        const ImU32 c = k == 0 ? shade : gold;
        ImVec2 s = g_title->CalcTextSizeA(54, FLT_MAX, 0, "NOTEBOOK");
        d->AddText(g_title, 54, {mid.x - s.x / 2 + o, mid.y + o}, c, "NOTEBOOK");
    }
    d->AddLine({mid.x - 120, mid.y + 82}, {mid.x + 120, mid.y + 82}, gold, 3);
    d->AddCircleFilled({mid.x, mid.y + 82}, 7, gold);
    if (!g_nb.name.empty()) {
        ImVec2 s = g_text->CalcTextSizeA(30, FLT_MAX, 0, g_nb.name.c_str());
        d->AddText(g_text, 30, {mid.x - s.x / 2, mid.y + 108}, gold, g_nb.name.c_str());
    }
}

void drawNotebook(const Frame& f) {
    {
        std::lock_guard<std::mutex> lock(g_nbMutex);
        if (g_nbNew) {
            g_nb = g_nbIn;
            g_nbNew = false;
            g_nbBuilt = false;
        }
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    nbCover(d, kBookCover);
    if (!g_nb.valid) {
        // Nothing yet (the script is reading the game's notebook): empty pages.
        nbPageArt(d, kBookLeft, false);
        nbPageArt(d, kBookRight, true);
        nbCentred(d, g_text, 30, {kBookRight.x + 256, kBookRight.y + 260}, kNbGrey, strings::ready() ? "Opening..." : "", 380);
        return;
    }
    if (!g_nbBuilt || g_nbUnresolved) nbBuild();
    const int spread = std::max(0, std::min(static_cast<int>(g_nbPages.size() / 2) - 1, f.bookSpread));
    const NbPage& left = g_nbPages[spread * 2];
    const NbPage& right = g_nbPages[spread * 2 + 1];
    nbPageArt(d, kBookLeft, false);
    nbPageArt(d, kBookRight, true);
    const Rect lr = kBookLeft, rr = kBookRight;
    for (int side = 0; side < 2; ++side) {
        const NbPage& page = side ? right : left;
        const Rect& r = side ? rr : lr;
        d->PushClipRect({r.x, r.y + 20}, {r.x + r.w, r.y + r.h - 20}, true);
        if (page.kind == 0) nbHome(d, r);
        else if (page.kind == 1) nbCategories(d, r);
        else nbBody(d, r, page, side == 1);
        d->PopClipRect();
        // The page's number, small, at the foot.
        if (spread > 0 || side == 1) {
            char number[16];
            std::snprintf(number, sizeof number, "%d", spread * 2 + side + 1);
            ImVec2 s = g_text->CalcTextSizeA(20, FLT_MAX, 0, number);
            d->AddText(g_text, 20, {r.x + r.w / 2 - s.x / 2, r.y + r.h - 50}, kNbGrey, number);
        }
    }
    // The page's curled corner while there are more pages.
    if (spread + 1 < static_cast<int>(g_nbPages.size() / 2))
        pieceIn(d, art::kNbCurl, {rr.x + rr.w - 112, rr.y + rr.h - 130}, {rr.x + rr.w - 22, rr.y + rr.h - 40});
    // The icons of the spreads near this one are read ahead.
    std::vector<uint64_t> icons;
    for (const NbCat& c : g_nb.cats) icons.push_back(c.icon), icons.push_back(c.large);
    for (int p = std::max(0, spread * 2 - 2); p < static_cast<int>(g_nbPages.size()) && p < spread * 2 + 6; ++p)
        for (const NbLine& l : g_nbPages[p].lines) {
            if (l.kind == 1 && l.sub) icons.push_back(l.sub->icon);
            if (l.kind == 2 && l.entry) icons.push_back(l.entry->icon);
            if (l.kind == 3 && l.item) icons.push_back(l.item->icon);
        }
    std::sort(icons.begin(), icons.end());
    icons.erase(std::unique(icons.begin(), icons.end()), icons.end());
    std::lock_guard<std::mutex> lock(g_itemMutex);
    if (icons != g_nbIcons) g_nbIcons.swap(icons);
}
