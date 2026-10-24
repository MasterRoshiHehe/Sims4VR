// ---------------------------------------------------------------------------
// The pie menu drawn by us (first person; user 2026-10-22: the cut of the game's
// pie took in boxes from the screen's corners). From the game's own
// PieMenuCreate (pie.py), built into the tree exactly as the PieMenu widget
// does (InteractionMenuData.GenerateTree, re/liveui/as/gamedata/Gameplay/
// InteractionMenu), laid out on the widget's nub positions (PieMenuBase) with
// its buttons (PieMenuButton / BackButton: PickerButtonBackground 9-slice,
// mood cap, category icon, icon_infos, label) in stage pixels, scaled into
// kPie. Included by hud.cpp (its fonts, icons and locks).

struct PieItem {
    uint64_t category = 0;
    int priority = 0, mood = 0, intensity = 0;
    float score = 0;
    std::string labelJson, disabledJson, label, disabled;
    std::vector<uint64_t> icons;
};
struct PieCategory {
    uint64_t parent = 0, icon = 0;
    bool collapsible = true, always = false;
    int priority = 1;
    std::string nameJson, overrideJson, name;
};
struct PieContent {
    bool front = false;
    std::vector<PieItem> items;
    std::map<uint64_t, PieCategory> categories;
    bool resolved = false, valid = false;
};
// A node of the tree: an item (item >= 0) or a category (its key; More pages are categories too).
struct PieNode {
    int item = -1;
    uint64_t key = 0;
    bool more = false;
    int priority = 0;
    int loop = -1, root = -1;     // InteractionCategory.LoopCategory / RootCategory (node indices)
    std::shared_ptr<std::vector<int>> kids = std::make_shared<std::vector<int>>();  // shared: a loop page shows its category's
};
std::mutex g_pieMutex;
PieContent g_pieIn, g_pieMenu;
bool g_pieNew = false;              // g_pieMutex
std::atomic<int> g_pieState{0};     // 0 none, 1 texts pending, 2 ready, 3 not drawable (the game's cut stays)
std::atomic<float> g_pieScale{1.0f};
unsigned g_pieSerial = 0;  // g_pieMutex
std::vector<PieNode> g_pieNodes;    // present thread
int g_pieCurrent = -1;
std::vector<int> g_pieStack;
bool g_pieFrontPage = false;        // PieMenuButtonData.HasSocialFrontPage
std::string g_pieMoreText, g_pieMarker;

int g_pieHover = -1;
bool g_piePressed = false;          // the press began on the pie (the one that opened it doesn't count)
ULONGLONG g_piePageAt = 0;

const std::string& pieText(const PieNode& n) {
    static const std::string kNothing;
    if (n.item >= 0) return g_pieMenu.items[n.item].label;
    if (n.more) return g_pieMoreText;
    auto it = g_pieMenu.categories.find(n.key);
    return it == g_pieMenu.categories.end() ? kNothing : it->second.name;
}

// InteractionMenuData.OptionsSortingFunction: More first, then higher priority, then by name.
bool pieBefore(int a, int b) {
    const PieNode &x = g_pieNodes[a], &y = g_pieNodes[b];
    if (x.more != y.more) return x.more;
    if (x.priority != y.priority) return x.priority > y.priority;
    std::string p = pieText(x), q = pieText(y);
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p < q;
}

int pieNewNode(PieNode n) {
    g_pieNodes.push_back(n);
    return static_cast<int>(g_pieNodes.size()) - 1;
}

int pieMore(const std::vector<int>& kids) {
    PieNode n;
    n.more = true;
    *n.kids = kids;
    return pieNewNode(n);
}

// CheckOverflow: over 9 on a level, from the 9th on into a "More" page in front (chained); the last page
// gets a More that loops back to the category.
void pieOverflow(int cat, int top, uint64_t rootKey) {
    const size_t kMax = 9, kKeep = kMax - 1;
    int cur = cat;
    size_t n = g_pieNodes[cat].kids->size();
    while (n > kMax) {
        std::vector<int>& items = *g_pieNodes[cur].kids;
        std::vector<int> spliced(items.begin() + kKeep, items.end());
        items.resize(kKeep);
        const int m = pieMore(spliced);
        if (g_pieNodes[top].key != rootKey) g_pieNodes[m].root = top;
        g_pieNodes[cur].kids->insert(g_pieNodes[cur].kids->begin(), m);
        cur = m;
        n -= kKeep;
    }
    if (!g_pieNodes[cur].more || cur == cat) return;
    if (n == kMax) {
        std::vector<int>& items = *g_pieNodes[cur].kids;
        std::vector<int> spliced(items.begin() + kKeep, items.begin() + kKeep + 1);
        items.erase(items.begin() + kKeep);
        const int m = pieMore(spliced);
        if (g_pieNodes[top].key != rootKey) g_pieNodes[m].root = top;
        g_pieNodes[cur].kids->insert(g_pieNodes[cur].kids->begin(), m);
        PieNode loop;
        loop.more = true;
        loop.kids = g_pieNodes[top].kids;
        loop.loop = top;
        const int l = pieNewNode(loop);
        g_pieNodes[m].kids->insert(g_pieNodes[m].kids->begin(), l);
    } else {
        PieNode loop;
        loop.more = true;
        loop.kids = g_pieNodes[top].kids;
        loop.loop = top;
        const int l = pieNewNode(loop);
        g_pieNodes[cur].kids->insert(g_pieNodes[cur].kids->begin(), l);
    }
}

// PostProcessCategory: sort, overflow, collapse single-child collapsible categories, recurse.
void piePost(int cat, int top, uint64_t rootKey) {
    if (!g_pieNodes[cat].more) {
        std::vector<int>& k = *g_pieNodes[cat].kids;
        std::sort(k.begin(), k.end(), pieBefore);
    }
    pieOverflow(cat, top, rootKey);
    const size_t n = g_pieNodes[cat].kids->size();
    for (size_t i = 0; i < n; ++i) {
        int c = (*g_pieNodes[cat].kids)[i];
        if (g_pieNodes[c].item >= 0) continue;
        while (c >= 0 && g_pieNodes[c].item < 0 && !g_pieNodes[c].more && g_pieNodes[c].kids->size() == 1) {
            auto it = g_pieMenu.categories.find(g_pieNodes[c].key);
            if (it == g_pieMenu.categories.end() || !it->second.collapsible) break;
            const int only = (*g_pieNodes[c].kids)[0];
            g_pieNodes[only].priority = g_pieNodes[c].priority;  // the child inherits display_priority
            (*g_pieNodes[cat].kids)[i] = only;
            c = only;
        }
        if (g_pieNodes[c].item >= 0) continue;
        if (g_pieNodes[c].more) {
            if (g_pieNodes[c].loop < 0) piePost(c, top, rootKey);
        } else {
            piePost(c, c, rootKey);
        }
    }
}

// FindDisabledText / DetermineDisabledText: a category is disabled when all it holds is, for the same reason.
std::string pieDisabled(int node) {
    const PieNode& n = g_pieNodes[node];
    if (n.item >= 0) return g_pieMenu.items[n.item].disabled;
    if (n.loop >= 0) return "\x01";  // LOOP_BACK_STR
    if (n.kids->empty()) return "";
    if (!n.more) {
        auto it = g_pieMenu.categories.find(n.key);
        if (it != g_pieMenu.categories.end() && it->second.always) return "";
    }
    const std::string first = pieDisabled((*n.kids)[0]);
    for (size_t i = 1; i < n.kids->size(); ++i) {
        const std::string t = pieDisabled((*n.kids)[i]);
        if (t != "\x01" && t != first) return "";
    }
    return first == "\x01" ? "" : first;
}

// GenerateTree (+ the social front page: random picks weighted by score, as the widget does).
void pieBuild() {
    g_pieNodes.clear();
    g_pieStack.clear();
    std::map<uint64_t, int> map;
    PieNode rootNode;
    const int root = pieNewNode(rootNode);
    map[0] = root;
    auto fetch = [&](uint64_t key) {
        const uint64_t wanted = key;
        int prev = -1, cur = -1;
        for (int guard = 0; guard < 64; ++guard) {
            auto it = map.find(key);
            if (it != map.end()) {
                cur = it->second;
                break;
            }
            PieNode n;
            n.key = key;
            auto c = g_pieMenu.categories.find(key);
            n.priority = c != g_pieMenu.categories.end() ? c->second.priority : 1;
            cur = pieNewNode(n);
            map[key] = cur;
            if (prev >= 0) g_pieNodes[cur].kids->push_back(prev);
            prev = cur;
            key = c != g_pieMenu.categories.end() ? c->second.parent : 0;
            if (!g_pieMenu.categories.count(key)) key = 0;
        }
        if (prev >= 0 && cur != prev) g_pieNodes[cur].kids->push_back(prev);
        auto w = map.find(wanted);
        return w != map.end() ? w->second : root;
    };
    std::vector<int> itemNodes;
    for (size_t i = 0; i < g_pieMenu.items.size(); ++i) {
        PieNode n;
        n.item = static_cast<int>(i);
        n.priority = g_pieMenu.items[i].priority;
        const int node = pieNewNode(n);
        itemNodes.push_back(node);
        const int cat = fetch(g_pieMenu.items[i].category);
        g_pieNodes[cat].kids->push_back(node);
    }
    g_pieFrontPage = g_pieMenu.front && !g_pieNodes[root].kids->empty();
    if (g_pieFrontPage) {
        // ProcessSocialFrontPage(root, items, 8)
        std::vector<int> page;
        std::vector<int>& rootKids = *g_pieNodes[root].kids;
        for (size_t i = 0; i < rootKids.size();) {
            if (g_pieNodes[rootKids[i]].item >= 0) {
                page.push_back(rootKids[i]);
                rootKids.erase(rootKids.begin() + i);
            } else {
                ++i;
            }
        }
        std::vector<int> sorted = itemNodes;
        std::stable_sort(sorted.begin(), sorted.end(), [](int a, int b) {
            return g_pieMenu.items[g_pieNodes[a].item].score < g_pieMenu.items[g_pieNodes[b].item].score;
        });
        std::vector<int> pool;
        double total = 0;
        for (int n : sorted) {
            const PieItem& it = g_pieMenu.items[g_pieNodes[n].item];
            if (it.category && it.score > 0) pool.push_back(n), total += it.score;
        }
        static std::mt19937 rng(static_cast<unsigned>(GetTickCount64()));
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        while (!pool.empty() && page.size() < 8) {
            const double r = unit(rng);
            size_t pick = pool.size() - 1;
            if (total == 0) {
                pick = std::min(pool.size() - 1, static_cast<size_t>(r * pool.size()));
            } else {
                double acc = 0;
                for (size_t k = 0; k < pool.size(); ++k) {
                    acc += g_pieMenu.items[g_pieNodes[pool[k]].item].score / total;
                    if (r < acc) {
                        pick = k;
                        break;
                    }
                }
            }
            total -= g_pieMenu.items[g_pieNodes[pool[pick]].item].score;
            page.push_back(pool[pick]);
            pool.erase(pool.begin() + pick);
        }
        if (!rootKids.empty()) page.push_back(pieMore(rootKids));
        *g_pieNodes[root].kids = page;
    }
    piePost(root, root, 0);
    g_pieCurrent = root;
    g_piePageAt = GetTickCount64();
    g_piePressed = false;
}

void resolvePie() {
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_pieMutex);
        if (g_pieNew) {
            g_pieMenu = g_pieIn;
            g_pieNew = false;
            fresh = true;
        }
    }
    if (fresh) {
        g_pieNodes.clear();
        g_pieCurrent = -1;
        g_pieHover = -1;
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_pieIcons.clear();
    }
    PieContent& p = g_pieMenu;
    if (p.items.empty() || p.resolved || !strings::ready()) return;
    p.resolved = true;
    if (g_pieMoreText.empty()) {
        g_pieMoreText = strings::resolve("{\"hash\": 3007953324}");  // KeysGP.PIE_MENU_MORE_CATEGORY_LABEL
        g_pieMarker = strings::resolve("{\"hash\": 1540200036}");    // KeysGP.PIE_MENU_CATEGORY_MARKER
    }
    int named = 0;
    for (PieItem& it : p.items) {
        it.label = strings::resolve(it.labelJson);
        if (!it.disabledJson.empty()) it.disabled = strings::resolve(it.disabledJson);
        if (!it.label.empty()) ++named;
    }
    for (auto& kv : p.categories) {
        PieCategory& c = kv.second;
        if (!c.overrideJson.empty()) c.name = strings::resolve(c.overrideJson);
        if (c.name.empty()) c.name = strings::resolve(c.nameJson);
    }
    p.valid = named == static_cast<int>(p.items.size());
    if (p.valid) pieBuild();
    {
        std::lock_guard<std::mutex> lock(g_pieMutex);
        if (!g_pieNew) g_pieState = p.valid ? 2 : 3;
    }
    static int logged = 0;
    if (logged++ < 40)
        LOG("HUD: pie menu %s (%zu options, %zu categories%s)", p.valid ? "drawn by us" : "texts missing: the game's pie is cut out",
            p.items.size(), p.categories.size(), g_pieFrontPage ? ", social front page" : "");
}

// The category's button text: KeysGP.PIE_MENU_CATEGORY(name, marker).
std::string pieCategoryLabel(const std::string& name) {
    auto quote = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (static_cast<unsigned char>(c) < 0x20) continue;
            o += c;
        }
        return o + "\"";
    };
    std::string t = strings::resolve("{\"hash\": 2718603618, \"tokens\": [{\"type\": \"RAW_TEXT\", \"raw_text\": " + quote(name) +
                                     "}, {\"type\": \"RAW_TEXT\", \"raw_text\": " + quote(g_pieMarker) + "}]}");
    return t.empty() ? name + g_pieMarker : t;
}

// PieMenuBase's settled nub positions (stage px from the centre), nub 0 at the top then clockwise.
const float kPieNubs[9][9][2] = {
    {{0, -45}},
    {{0, -65}, {0, 65}},
    {{0, -80}, {69, 46}, {-69, 46}},
    {{0, -90}, {100, 0}, {0, 90}, {-100, 0}},
    {{0, -100}, {91, -33}, {69, 76}, {-65, 76}, {-91, -33}},
    {{0, -120}, {131, -50}, {131, 50}, {0, 125}, {-131, 50}, {-131, -50}},
    {{0, -130}, {112, -66.5f}, {128, 19}, {74, 104}, {-74, 104}, {-128, 19}, {-112, -66.5f}},
    {{0, -135}, {105, -84}, {135, 0}, {95, 94}, {0, 135}, {-95, 94}, {-135, 0}, {-105, -84}},
    {{0, -140}, {103, -95}, {138, -23}, {128.5f, 48}, {75.7f, 118.7f}, {-71.6f, 115.6f}, {-130, 52}, {-137.5f, -23.6f}, {-104, -92}},
};
constexpr float kPieHitBuffer[8] = {45, 45, 45, 35, 35, 35, 35, 25};  // OPTION_HIT_AREA_BUFFER_WIDTH
constexpr float kPieBgH = 47, kPieShadow = 13, kPiePad = 25, kPieIcon = 24, kPieIconPad = 4, kPieCatIcon = 46,
                kPieCapW = 18, kPieCapPad = 5, kPieMaxLabel = 320, kPieTextPx = 15, kPieCloseRadius = 50;

struct PieButton {
    int node = -1;
    ImVec2 a, b;          // its pill (stage px from the centre, shadow excluded)
    ImVec2 hitA, hitB;
    std::string label, disabled;
    bool enabled = true, back = false;
    uint64_t catIcon = 0;
    int mood = 0;
    std::vector<uint64_t> icons;
    float labelX = 0, iconsX = 0, labelW = 0;
};

void pieLayout(std::vector<PieButton>& out, float& halfW, float& halfH) {
    out.clear();
    halfW = halfH = 60;
    if (g_pieCurrent < 0) return;
    PieNode& cur = g_pieNodes[g_pieCurrent];
    std::vector<int> kids = *cur.kids;
    std::sort(kids.begin(), kids.end(), pieBefore);  // UpdateOptionButtons sorts again
    const int count = std::min<int>(9, static_cast<int>(kids.size()));
    if (!count) return;
    ImFont* font = g_text;
    const float fs = kPieTextPx;
    const bool goHere = count == 1 && g_pieStack.empty();
    for (int i = 0; i < count; ++i) {
        PieButton btn;
        btn.node = kids[i];
        const PieNode& n = g_pieNodes[kids[i]];
        if (n.item >= 0) {
            const PieItem& it = g_pieMenu.items[n.item];
            btn.label = it.label;
            btn.disabled = it.disabled;
            btn.enabled = it.disabled.empty();
            btn.mood = it.mood;
            btn.icons = it.icons;
        } else {
            btn.label = pieCategoryLabel(pieText(n));
            btn.disabled = n.more ? "" : pieDisabled(kids[i]);
            btn.enabled = n.more || btn.disabled.empty();
            if (!n.more) {
                auto c = g_pieMenu.categories.find(n.key);
                if (c != g_pieMenu.categories.end()) btn.catIcon = c->second.icon;
            }
        }
        btn.labelW = std::min(kPieMaxLabel, font->CalcTextSizeA(fs, FLT_MAX, 0, btn.label.c_str()).x);
        float w = btn.labelW + kPiePad * 2;
        float x = 0;
        if (btn.mood) w += kPieCapW + kPieCapPad, x += kPieCapW + kPieCapPad;
        if (btn.catIcon) w += kPieCatIcon * 0.5f, x += kPieCatIcon * 0.5f;
        const float iconsW = btn.icons.size() * (kPieIcon + kPieIconPad);
        w += iconsW;
        x += kPiePad;
        btn.iconsX = x;
        btn.labelX = x + iconsW;
        const float* nub = goHere ? kPieNubs[0][0] : kPieNubs[count - 1][i];
        float nx = nub[0], ny = goHere ? -8 : nub[1];
        float left, cy;
        if (nx < 0) left = nx - w, cy = ny;
        else if (nx > 0) left = nx, cy = ny;
        else if (ny < 0) left = -w / 2, cy = ny - kPieBgH / 2;
        else left = -w / 2, cy = ny + kPieBgH / 2;
        btn.a = {left, cy - 15};
        btn.b = {left + w, cy + 15};
        const float buffer = kPieHitBuffer[std::min(count, 7)] / 2;
        btn.hitA = {left - buffer, cy - kPieBgH / 2};
        btn.hitB = {left + w + buffer, cy + kPieBgH / 2};
        // The x of each piece is from the pill's left edge.
        btn.iconsX += left, btn.labelX += left;
        halfW = std::max({halfW, std::fabs(left - kPieShadow), std::fabs(left + w + kPieShadow)});
        halfH = std::max({halfH, std::fabs(cy) + kPieBgH / 2});
        out.push_back(btn);
    }
    // BackButton: the category shown (its root for a More page), centred, visible as PieMenuBase.Draw says.
    const bool backShown = !g_pieStack.empty() && (!cur.more || (cur.more && cur.root >= 0) ||
                                                   (g_pieStack.size() == 1 && cur.more && g_pieFrontPage));
    if (backShown) {
        PieButton back;
        back.back = true;
        const PieNode& named = cur.root >= 0 ? g_pieNodes[cur.root] : cur;
        back.label = pieCategoryLabel(pieText(named));
        back.labelW = std::min(kPieMaxLabel, font->CalcTextSizeA(fs, FLT_MAX, 0, back.label.c_str()).x);
        const float w = back.labelW + kPieCapW + kPiePad;
        back.a = {-w / 2, -15};
        back.b = {w / 2, 15};
        back.hitA = {-w / 2, -kPieBgH / 2};
        back.hitB = {w / 2, kPieBgH / 2};
        back.labelX = -w / 2 + kPieCapW + kPiePad * 0.5f;
        halfW = std::max(halfW, w / 2 + kPieShadow);
        out.push_back(back);
    }
}

// Opens a category / More page or goes back as PieMenuBase does (OnPieMenuButtonClick, GoBack).
bool pieOpen(int node) {
    PieNode& n = g_pieNodes[node];
    if (n.loop >= 0) {
        // Back to the looped category's page, the pages after it dropped.
        while (!g_pieStack.empty()) {
            const int popped = g_pieStack.back();
            g_pieStack.pop_back();
            if (popped == n.loop) break;
        }
        g_pieCurrent = n.loop;
    } else {
        g_pieStack.push_back(g_pieCurrent);
        g_pieCurrent = node;
    }
    g_piePageAt = GetTickCount64();
    return true;
}
bool pieBack() {
    const PieNode& cur = g_pieNodes[g_pieCurrent];
    if (cur.root >= 0)
        while (!g_pieStack.empty()) {
            const int popped = g_pieStack.back();
            g_pieStack.pop_back();
            if (popped == cur.root) break;
        }
    if (g_pieStack.empty()) return false;
    g_pieCurrent = g_pieStack.back();
    g_pieStack.pop_back();
    g_piePageAt = GetTickCount64();
    return true;
}

Action drawPie(int* index) {
    if (g_pieCurrent < 0) return kNone;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    std::vector<PieButton> buttons;
    float halfW, halfH;
    pieLayout(buttons, halfW, halfH);
    if (buttons.empty()) return kNone;
    // Stage pixels to atlas pixels: as large as kPie holds (sharper text for short labels).
    const float S = std::min({1.6f, (kPie.w / 2 - 8) / halfW, (kPie.h / 2 - 8) / halfH});
    g_pieScale = S;
    const ImVec2 c{kPie.x + kPie.w / 2, kPie.y + kPie.h / 2};
    auto at = [&](ImVec2 p) { return ImVec2{c.x + p.x * S, c.y + p.y * S}; };
    ImGui::PushClipRect({kPie.x, kPie.y}, {kPie.x + kPie.w, kPie.y + kPie.h}, false);
    Action action = kNone;
    // Fly-in: the buttons grow out of the centre over 150 ms (PieMenuBase optionN .. _appear_end).
    const float grow = std::min(1.0f, (GetTickCount64() - g_piePageAt) / 150.0f);
    std::vector<uint64_t> shown;
    const ImU32 kText = IM_COL32(20, 44, 110, 255), kDim = IM_COL32(120, 128, 140, 255);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered = -1;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const PieButton& btn = buttons[i];
        const ImVec2 ha = at(btn.hitA), hb = at(btn.hitB);
        if (mouse.x >= ha.x && mouse.x <= hb.x && mouse.y >= ha.y && mouse.y <= hb.y) hovered = static_cast<int>(i);
    }
    const bool down = ImGui::IsMouseDown(0), released = ImGui::IsMouseReleased(0);
    const bool inside = mouse.x >= kPie.x && mouse.x <= kPie.x + kPie.w && mouse.y >= kPie.y && mouse.y <= kPie.y + kPie.h;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const PieButton& btn = buttons[i];
        const bool over = static_cast<int>(i) == hovered && btn.enabled;
        // Roll-over: 1.1 times (OPTION_ROLL_OVER_SCALE), around the button's own middle.
        const float k = (over ? 1.1f : 1.0f) * (btn.back ? 1.0f : 0.6f + 0.4f * grow);
        const ImVec2 mid{(btn.a.x + btn.b.x) / 2 * (btn.back ? 1 : grow), (btn.a.y + btn.b.y) / 2 * (btn.back ? 1 : grow)};
        auto place = [&](ImVec2 p) {
            const float mx = (btn.a.x + btn.b.x) / 2, my = (btn.a.y + btn.b.y) / 2;
            return at({mid.x + (p.x - mx) * k, mid.y + (p.y - my) * k});
        };
        const art::Piece& bg = over && down ? art::kPieButtonDown : over ? art::kPieButtonOver : art::kPieButton;
        const float u = S * k;
        // The 9-slice background (the art carries the drop shadow, 13 px each side).
        const ImVec2 bgA = place({btn.a.x - kPieShadow, (btn.a.y + btn.b.y) / 2 - kPieBgH / 2});
        const ImVec2 bgB = place({btn.b.x + kPieShadow, (btn.a.y + btn.b.y) / 2 + kPieBgH / 2});
        uikit::nine(d, bg, bgA, bgB, 34 * art::kScale, 21 * art::kScale, 32 * art::kScale, 23 * art::kScale,
                    u / art::kScale, btn.enabled ? uikit::kWhite : IM_COL32(255, 255, 255, 170));
        const float cy = (btn.a.y + btn.b.y) / 2;
        if (btn.back) {
            uikit::pieceIn(d, art::kPieBackArrow, place({btn.a.x, cy - 15}), place({btn.a.x + kPieCapW, cy + 15}));
        }
        if (btn.mood > 0 && btn.mood < static_cast<int>(sizeof(art::kPieMoodCap) / sizeof(art::kPieMoodCap[0])))
            uikit::pieceIn(d, art::kPieMoodCap[btn.mood], place({btn.a.x, cy - 15}), place({btn.a.x + kPieCapW, cy + 15}));
        if (btn.catIcon) {
            icon(d, btn.catIcon, place({btn.a.x + (btn.mood ? kPieCapW + kPieCapPad : 0) + 2, cy}), kPieCatIcon * 0.8f * u);
            shown.push_back(btn.catIcon);
        }
        for (size_t k2 = 0; k2 < btn.icons.size(); ++k2) {
            icon(d, btn.icons[k2], place({btn.iconsX + k2 * (kPieIcon + kPieIconPad) + kPieIcon / 2, cy}), kPieIcon * u);
            shown.push_back(btn.icons[k2]);
        }
        const float fs = kPieTextPx * u;
        const ImVec2 ts = g_text->CalcTextSizeA(fs, FLT_MAX, 0, btn.label.c_str());
        const ImVec2 tp = place({btn.labelX, cy});
        const float maxW = kPieMaxLabel * u;
        ImGui::PushClipRect({tp.x, tp.y - fs}, {tp.x + maxW, tp.y + fs}, true);
        d->AddText(g_text, fs, {tp.x, tp.y - ts.y / 2}, btn.enabled ? kText : kDim, btn.label.c_str());
        ImGui::PopClipRect();
    }
    if (hovered >= 0 && !buttons[hovered].enabled && !buttons[hovered].disabled.empty()) {
        // A greyed option's reason (its tooltip), under the pie.
        const std::string& why = buttons[hovered].disabled;
        const float fs = kPieTextPx * S * 0.9f, wrap = 420 * S;
        const ImVec2 ts = g_text->CalcTextSizeA(fs, FLT_MAX, wrap, why.c_str());
        const ImVec2 p{c.x - ts.x / 2, std::min(kPie.y + kPie.h - ts.y - 16, c.y + (halfH + 6) * S)};
        uikit::dialogPanel(d, {p.x - 18, p.y - 10}, {p.x + ts.x + 18, p.y + ts.y + 10});
        d->AddText(g_text, fs, p, uikit::kBodyDark, why.c_str(), nullptr, wrap);
    }
    if (ImGui::IsMouseClicked(0)) g_piePressed = inside;
    const bool click = released && g_piePressed;
    if (released) g_piePressed = false;
    if (click && inside) {
        if (hovered >= 0) {
            const PieButton& btn = buttons[hovered];
            if (btn.back) {
                if (!pieBack()) action = kPieClose;
            } else if (btn.enabled) {
                const PieNode& n = g_pieNodes[btn.node];
                if (n.item >= 0) {
                    action = kPiePick;
                    if (index) *index = n.item;
                } else {
                    pieOpen(btn.node);
                }
            }
        } else {
            // Off every button: the widget closes on a click near its centre or off the menu.
            action = kPieClose;
        }
    }
    g_pieHover = hovered;
    ImGui::PopClipRect();
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (shown != g_pieIcons) g_pieIcons.swap(shown);
    }
    return action;
}

void setPieContent(const char* data, size_t size) {
    PieContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        const std::vector<std::string> f = fieldsOf(all.substr(at, end - at));
        at = end + 1;
        if (f.size() >= 2 && f[0] == "pie") {
            c.front = f[1] == "1";
        } else if (f.size() >= 9 && f[0] == "cat") {
            PieCategory cat;
            const uint64_t key = std::strtoull(f[1].c_str(), nullptr, 10);
            cat.parent = std::strtoull(f[2].c_str(), nullptr, 10);
            cat.collapsible = f[3] == "1";
            cat.priority = std::atoi(f[4].c_str());
            cat.always = f[5] == "1";
            cat.icon = std::strtoull(f[6].c_str(), nullptr, 10);
            cat.nameJson = f[7];
            cat.overrideJson = f[8];
            c.categories[key] = cat;
        } else if (f.size() >= 10 && f[0] == "item") {
            PieItem it;
            it.category = std::strtoull(f[2].c_str(), nullptr, 10);
            it.priority = std::atoi(f[3].c_str());
            it.score = static_cast<float>(std::atof(f[4].c_str()));
            it.mood = std::atoi(f[5].c_str());
            it.intensity = std::atoi(f[6].c_str());
            it.labelJson = f[7];
            it.disabledJson = f[8];
            for (size_t k = 0; k < f[9].size();) {
                size_t e = f[9].find(',', k);
                if (e == std::string::npos) e = f[9].size();
                const uint64_t id = std::strtoull(f[9].substr(k, e - k).c_str(), nullptr, 10);
                if (id) it.icons.push_back(id);
                k = e + 1;
            }
            c.items.push_back(it);
        }
        if (end >= all.size()) break;
    }
    std::lock_guard<std::mutex> lock(g_pieMutex);
    g_pieIn = c;
    g_pieNew = true;
    g_pieState = c.items.empty() ? 0 : 1;
    ++g_pieSerial;
}

int pieState() { return g_pieState.load(); }
float pieScale() { return g_pieScale.load(); }
unsigned pieSerial() {
    std::lock_guard<std::mutex> lock(g_pieMutex);
    return g_pieSerial;
}
void clearPie(unsigned serial) {
    std::lock_guard<std::mutex> lock(g_pieMutex);
    if (serial != g_pieSerial) {
        static int logged = 0;
        if (logged++ < 10) LOG("HUD: pie menu closed after the next one's options came; those kept");
        return;
    }
    g_pieIn = PieContent{};
    g_pieNew = true;
    g_pieState = 0;
}
