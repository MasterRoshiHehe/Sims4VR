#include "hud.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hud_art.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "log.h"
#include "phone3d.h"
#include "strings.h"

// The game's own UI art (tools/pack_hud_art.py: colour re-cut of re/liveui/art),
// in the DLL: u32 width, height, then run-length coded straight-alpha BGRA.
__asm__(".section .rdata,\"dr\"\n"
        ".p2align 4\n"
        "s4vr_hud_art:\n"
        ".incbin \"" S4VR_HUD_ART "\"\n"
        "s4vr_hud_art_end:\n"
        ".text\n");
extern "C" const unsigned char s4vr_hud_art[], s4vr_hud_art_end[];

// imgui_impl_dx11.cpp calls this for D3DCompile (CMakeLists renames it):
// d3dcompiler_47.dll is loaded at runtime, as bodylayer.cpp does, not linked.
extern "C" HRESULT WINAPI s4vr_D3DCompile(LPCVOID data, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                               ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                               ID3DBlob** code, ID3DBlob** errors) {
    static pD3DCompile fn = [] {
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        return dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }();
    if (!fn) return E_FAIL;
    return fn(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace s4vr::hud {
namespace {

std::atomic<int> g_hour{-1}, g_minute{0}, g_day{0}, g_speed{1};
ImGuiContext* g_imgui = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11Texture2D* g_texture = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
ImFont* g_big = nullptr;    // the time
ImFont* g_small = nullptr;  // the day, labels
ImFont* g_text = nullptr;   // popup text (regular)
ImFont* g_title = nullptr;  // popup titles
ID3D11ShaderResourceView* g_art = nullptr;  // the game's art

// The popup on show (objmenu.py), its texts once the strings are loaded.
struct PopupContent {
    int layout = 0;                     // 1: two Sims talking
    uint64_t pictures[2] = {};          // the other Sim's, yours (icon or thumbnail ids)
    std::string titleJson, textJson;
    std::vector<std::string> buttonJson;
    std::vector<long long> choices;
    bool resolved = false, valid = false;
    std::string title, text;
    std::vector<std::string> buttons;
};
std::mutex g_popupMutex;
PopupContent g_popupIn;      // set by Python (g_popupMutex)
bool g_popupNew = false;     // g_popupIn changed (g_popupMutex)
PopupContent g_popup;        // present thread
bool g_popupOn = false;      // present thread: there is one
std::atomic<float> g_popupHeight{0};
std::atomic<int> g_popupState{0};  // 0 none, 1 waiting for its texts, 2 drawn from them, 3 texts missing

// The picker on its object (objmenu.py; user 2026-10-16 09:57Z: the cut of
// the game's flat picker on the fridge looked badly cropped), its texts once
// the strings are loaded.
struct PickerRow {
    bool top = true;  // in the list (else only one of a top row's servings)
    std::string nameJson, servingJson, name, serving;
    uint64_t icon = 0;
    unsigned enabled = 0;     // 1 plain, 2 with ingredients, 4 prepped, 8 both
    unsigned price[4] = {};   // plain, with ingredients, prepped, both
    unsigned discounted = 0;  // 0: none
    int skill = 0;
    std::vector<int> servings;  // a top row's linked rows (may hold itself)
    std::vector<unsigned> tags;
};
struct PickerFilter {
    unsigned tag = 0;
    uint64_t icon = 0;
    std::string json, text;
};
struct PickerContent {
    long long dialog = 0;
    int flags = 0;     // hud.h setPickerContent
    std::string sort;  // the column it is sorted by ("" the message's order)
    std::string titleJson, title;
    std::vector<PickerFilter> filters;
    std::vector<PickerRow> rows;
    std::vector<int> order;  // the top rows, sorted
    size_t mostServings = 0;
    bool resolved = false, valid = false;
};
std::mutex g_pickerMutex;
PickerContent g_pickerIn;  // set by Python (g_pickerMutex)
bool g_pickerNew = false;  // g_pickerIn changed (g_pickerMutex)
PickerContent g_picker;    // present thread
bool g_pickerOn = false;   // present thread: there is one
std::atomic<float> g_pickerHeight{0};
std::atomic<int> g_pickerState{0};  // as g_popupState
// What you did on it (present thread): the page, the filter tab (-1 all), the
// top row whose servings are open (-1 none), the two ingredient boxes.
int g_pickerPage = 0, g_pickerTab = -1, g_pickerOpen = -1;
bool g_pickerFresh = false, g_pickerPrepped = false;
std::vector<uint64_t> g_pickerIcons;  // the icons it shows (g_itemMutex)
std::vector<uint64_t> g_popupIcons;   // the popup's pictures (g_itemMutex)

// The tooltips of the needs and moodlets (hudinfo.py), and their texts once found.
struct Tip {
    std::string json[3];  // title, text, reason
    int minutes = -1;
};
std::vector<Tip> g_tips[kKinds];  // g_itemMutex
std::map<std::string, std::string> g_found;  // present thread: LocalizedString JSON -> text
int g_hoverKind = -1, g_hoverIndex = -1;    // present thread: the item pointed at
std::atomic<int> g_tipKind{-1};
std::atomic<float> g_tipHeight{0};

// The game's values for the needs, moodlets, queue and wants (hudinfo.py).
std::mutex g_itemMutex;
Item g_items[kKinds][kMaxItems];
int g_counts[kKinds] = {};

// The game's icons (buffs, needs, wants, interactions), 128 px slots in one
// texture, read on demand (missingIcons) and kept while used.
constexpr int kIconSize = 128, kIconsAcross = 8, kIconSlots = kIconsAcross * kIconsAcross;
struct PendingIcon {
    uint64_t id;
    std::vector<uint32_t> pixels;  // kIconSize^2, RGBA straight
};
std::vector<PendingIcon> g_pendingIcons;  // decoded, to upload (g_itemMutex)
std::set<uint64_t> g_loadedIcons, g_badIcons;  // in a slot / could not be read (g_itemMutex)
std::set<uint64_t> g_pictureIds;  // thumbnails (thumbs.cpp), not asked of Python (g_itemMutex)
struct IconSlot {
    uint64_t id = 0;
    unsigned used = 0;  // frame last drawn
};
IconSlot g_slots[kIconSlots];  // present thread
unsigned g_frameNo = 0;
ID3D11Texture2D* g_iconTexture = nullptr;
ID3D11ShaderResourceView* g_icons = nullptr;
LARGE_INTEGER g_last{};
bool g_failed = false;

// The embedded art into a texture.
bool loadArt(ID3D11Device* device) {
    const unsigned char* p = s4vr_hud_art;
    const size_t size = static_cast<size_t>(s4vr_hud_art_end - s4vr_hud_art);
    if (size < 8) return false;
    uint32_t w, h;
    std::memcpy(&w, p, 4);
    std::memcpy(&h, p + 4, 4);
    if (w != art::kAtlasW || h != art::kAtlasH) return false;
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h, 0);
    size_t at = 8, i = 0;
    while (at + 4 <= size && i < pixels.size()) {
        uint32_t n;
        std::memcpy(&n, p + at, 4);
        at += 4;
        size_t count = n & 0x7fffffffu;
        if (count > pixels.size() - i) return false;
        if (!(n & 0x80000000u)) {
            if (at + count * 4 > size) return false;
            std::memcpy(&pixels[i], p + at, count * 4);
            at += count * 4;
        }
        i += count;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), w * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture))) return false;
    HRESULT hr = device->CreateShaderResourceView(texture, nullptr, &g_art);
    texture->Release();
    return SUCCEEDED(hr);
}

bool init(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_texture)) ||
        FAILED(device->CreateRenderTargetView(g_texture, nullptr, &g_target))) {
        LOG("HUD: could not make its texture");
        release();
        return false;
    }
    IMGUI_CHECKVERSION();
    g_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    // Windows' own UI font, large: the atlas is seen up close in the headset.
    ImFontConfig config;
    config.OversampleH = 2;
    g_big = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 46.0f, &config);
    g_small = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 34.0f, &config);
    // Popup texts: Latin with accents, the typographic quotes, dashes, ellipsis
    // and bullet the game's strings use, and the Simoleon sign.
    static const ImWchar kTextRanges[] = {0x0020, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x20AC, 0x20AC,
                                          0x2122, 0x2122, 0};
    g_text = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 36.0f, &config, kTextRanges);
    g_title = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 44.0f, &config, kTextRanges);
    if (!g_big || !g_small) {
        ImFontConfig fallback;
        fallback.SizePixels = 40;
        g_small = g_big = io.Fonts->AddFontDefault(&fallback);
        LOG("HUD: Segoe UI not found, the built-in font");
    }
    if (!g_text) g_text = g_small;
    if (!g_title) g_title = g_big;
    if (!ImGui_ImplDX11_Init(device, context)) {
        LOG("HUD: Dear ImGui's DirectX 11 renderer did not start");
        release();
        return false;
    }
    if (!loadArt(device)) LOG("HUD: the game's art did not load; shapes only");
    {
        D3D11_TEXTURE2D_DESC icons{};
        icons.Width = icons.Height = kIconSize * kIconsAcross;
        icons.MipLevels = 1;
        icons.ArraySize = 1;
        icons.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        icons.SampleDesc.Count = 1;
        icons.Usage = D3D11_USAGE_DEFAULT;
        icons.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&icons, nullptr, &g_iconTexture)) ||
            FAILED(device->CreateShaderResourceView(g_iconTexture, nullptr, &g_icons)))
            LOG("HUD: no texture for the game's icons");
    }
    g_device = device;
    g_device->AddRef();
    QueryPerformanceCounter(&g_last);
    LOG("HUD: Dear ImGui %s ready (%dx%d atlas)", IMGUI_VERSION, kWidth, kHeight);
    return true;
}

const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kShadow = IM_COL32(0, 0, 0, 110);
const ImU32 kTextOn = IM_COL32(120, 214, 82, 255);  // the game's selected-speed green, for text

// A piece of the game's art, its own size times `scale`, centred on c.
void piece(ImDrawList* d, const art::Piece& p, ImVec2 c, float scale = 1, ImU32 tint = kWhite) {
    if (!g_art) return;
    float w = p.w * scale, h = p.h * scale;
    d->AddImage(reinterpret_cast<ImTextureID>(g_art), {c.x - w / 2, c.y - h / 2}, {c.x + w / 2, c.y + h / 2},
                {static_cast<float>(p.x) / art::kAtlasW, static_cast<float>(p.y) / art::kAtlasH},
                {static_cast<float>(p.x + p.w) / art::kAtlasW, static_cast<float>(p.y + p.h) / art::kAtlasH}, tint);
}
// Stretched into a box.
void pieceIn(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, ImU32 tint = kWhite) {
    if (!g_art) return;
    d->AddImage(reinterpret_cast<ImTextureID>(g_art), a, b,
                {static_cast<float>(p.x) / art::kAtlasW, static_cast<float>(p.y) / art::kAtlasH},
                {static_cast<float>(p.x + p.w) / art::kAtlasW, static_cast<float>(p.y + p.h) / art::kAtlasH}, tint);
}
void shadowText(ImDrawList* d, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
    d->AddText(font, size, {at.x + 2, at.y + 2}, kShadow, text);
    d->AddText(font, size, at, colour, text);
}

// The game's TimeControls in its own art (TimeControlTray, the speed buttons'
// up / over / selected_up pictures; selected = running speed, pause red when
// paused) with the time below in the weather pill, as on the game's HUD. The
// pill's darker end is our full-UI-on-the-wrist button.
Action drawWatch(const Frame& f) {
    Action action = kNone;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kWatch;
    const float cx = r.x + r.w / 2;
    // The tray: the game's TimeControlTray, as drawn (light glass dome).
    piece(d, art::kTray, {cx, r.y + 20 + art::kTray.h / 2.0f});
    // The speed buttons: the running one in its selected picture, the one
    // under your finger with the game's over picture and green disc.
    int speed = g_speed.load();
    int lit = speed == 0 ? 0 : speed == 5 ? 3 : speed >= 1 && speed <= 3 ? speed : 1;
    static const art::Piece* const kUp[4] = {&art::kPauseUp, &art::kOneUp, &art::kTwoUp, &art::kThreeUp};
    static const art::Piece* const kOver[4] = {&art::kPauseOver, &art::kOneOver, &art::kTwoOver, &art::kThreeOver};
    static const art::Piece* const kOn[4] = {&art::kPauseOn, &art::kOneOn, &art::kTwoOn, &art::kThreeOn};
    const float by = r.y + 83, gap = 96, br = 46;
    for (int i = 0; i < 4; ++i) {
        ImVec2 c{cx + gap * (i - 1.5f), by};
        ImGui::SetCursorScreenPos({c.x - br, c.y - br});
        ImGui::PushID(i);
        bool pressed = ImGui::InvisibleButton("speed", {br * 2, br * 2});
        bool hover = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hover && i != lit) piece(d, art::kHoverDisc, c, 0.85f);
        piece(d, i == lit ? *kOn[i] : hover ? *kOver[i] : *kUp[i], c);
        if (pressed) action = i == 0 ? kPausePressed : static_cast<Action>(kSpeed1 + i - 1);
    }
    // The pill: day and time on its light part, "UI" on its dark end.
    const ImVec2 pa{cx - 235, r.y + 160}, pb{cx + 235, r.y + 226};
    pieceIn(d, art::kTimePill, pa, pb);
    int hour = g_hour.load(), minute = g_minute.load();
    char text[32] = "--:--";
    if (hour >= 0) {
        static const char* const kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int h12 = hour % 12 == 0 ? 12 : hour % 12;
        std::snprintf(text, sizeof text, "%s %d:%02d %s", kDays[(g_day.load() % 7 + 7) % 7], h12, minute,
                      hour < 12 ? "AM" : "PM");
    }
    const float split = pa.x + (pb.x - pa.x) * 0.635f;  // where the pill's dark end starts
    ImVec2 ts = g_big->CalcTextSizeA(g_big->FontSize, 1e9f, 0, text);
    shadowText(d, g_big, g_big->FontSize, {pa.x + (split - pa.x - ts.x) / 2, (pa.y + pb.y - ts.y) / 2}, kWhite, text);
    if (f.uiButton) {
        ImVec2 a{split, pa.y}, b{pb.x, pb.y};
        ImGui::SetCursorScreenPos(a);
        bool pressed = ImGui::InvisibleButton("fullui", {b.x - a.x, b.y - a.y});
        bool hover = ImGui::IsItemHovered();
        if (hover) d->AddRectFilled({a.x + 6, a.y + 6}, {b.x - 6, b.y - 6}, IM_COL32(255, 255, 255, 40), 26);
        const char* label = "UI";
        ImVec2 ls = g_small->CalcTextSizeA(g_small->FontSize, 1e9f, 0, label);
        shadowText(d, g_small, g_small->FontSize, {(a.x + b.x - ls.x) / 2, (a.y + b.y - ls.y) / 2},
                   f.fullUi ? kTextOn : kWhite, label);
        if (pressed) action = kFullUi;
    }
    return action;
}

// While paused: the game's selected pause picture (red), large.
void drawPause() {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kPauseArea;
    piece(d, art::kPauseOn, {r.x + r.w / 2, r.y + r.h / 2}, (r.h - 16) / art::kPauseOn.h);
}

// ARGB (Python's colours) to ImGui's.
ImU32 fromArgb(uint32_t c) {
    return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, (c >> 24) & 0xff);
}
ImU32 moodTint(uint32_t mood, int alpha = 255) {
    ImU32 c = art::kMoodTint[mood < sizeof(art::kMoodTint) / sizeof(art::kMoodTint[0]) ? mood : 0];
    return (c & 0x00ffffffu) | (static_cast<ImU32>(alpha) << 24);
}

// The game's icon `id` in a box centred on c (nothing until it is read).
void icon(ImDrawList* d, uint64_t id, ImVec2 c, float size, ImU32 tint = kWhite) {
    if (!id || !g_icons) return;
    for (int i = 0; i < kIconSlots; ++i) {
        IconSlot& slot = g_slots[i];
        if (slot.id != id) continue;
        slot.used = g_frameNo;
        float u = static_cast<float>(i % kIconsAcross) / kIconsAcross, v = static_cast<float>(i / kIconsAcross) / kIconsAcross;
        float step = 1.0f / kIconsAcross;
        d->AddImage(reinterpret_cast<ImTextureID>(g_icons), {c.x - size / 2, c.y - size / 2},
                    {c.x + size / 2, c.y + size / 2}, {u, v}, {u + step, v + step}, tint);
        return;
    }
}

// Decoded icons into free (or the longest unused) slots.
void uploadIcons(ID3D11DeviceContext* context) {
    std::vector<PendingIcon> pending;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        pending.swap(g_pendingIcons);
    }
    if (!g_iconTexture) return;
    for (PendingIcon& p : pending) {
        int best = -1;
        for (int i = 0; i < kIconSlots; ++i) {
            if (g_slots[i].id == p.id) {
                best = i;
                break;
            }
            if (g_slots[i].used == g_frameNo && g_slots[i].id) continue;  // drawn this frame
            if (best < 0 || (g_slots[best].id && (!g_slots[i].id || g_slots[i].used < g_slots[best].used))) best = i;
        }
        if (best < 0) continue;
        uint64_t old = g_slots[best].id;
        D3D11_BOX box{static_cast<UINT>(best % kIconsAcross * kIconSize), static_cast<UINT>(best / kIconsAcross * kIconSize),
                      0, static_cast<UINT>(best % kIconsAcross * kIconSize + kIconSize),
                      static_cast<UINT>(best / kIconsAcross * kIconSize + kIconSize), 1};
        context->UpdateSubresource(g_iconTexture, 0, &box, p.pixels.data(), kIconSize * 4, 0);
        g_slots[best] = {p.id, g_frameNo};
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (old && old != p.id) g_loadedIcons.erase(old);
        g_loadedIcons.insert(p.id);
    }
}

// The needs: the game's motive panel grid (2 x 3, its order), each the need's
// icon and its bar in the tuned fill and background colours of its state
// (CommodityState.fill_color / background_color), a sliver even when empty
// as the game pads it.
void drawNeeds(const Item* items, int count) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kNeeds;
    const float cw = r.w / 2, ch = r.h / 3;
    for (int i = 0; i < count && i < 6; ++i) {
        const Item& it = items[i];
        float x = r.x + cw * (i % 2), y = r.y + ch * (i / 2), cy = y + ch / 2;
        ImGui::SetCursorScreenPos({x, y});
        ImGui::PushID(400 + i);
        ImGui::InvisibleButton("need", {cw, ch});
        if (ImGui::IsItemHovered()) g_hoverKind = kNeedItems, g_hoverIndex = i;
        ImGui::PopID();
        icon(d, it.icon, {x + 44, cy}, 66);
        ImVec2 a{x + 88, cy - 15}, b{x + cw - 12, cy + 15};
        ImU32 back = (it.flags >> 24) ? fromArgb(it.flags) : IM_COL32(40, 44, 52, 200);
        d->AddRectFilled({a.x - 3, a.y - 3}, {b.x + 3, b.y + 3}, IM_COL32(20, 22, 28, 150), 18);
        d->AddRectFilled(a, b, back, 15);
        float fill = 0.07f + 0.93f * std::max(0.0f, std::min(1.0f, it.value));
        d->AddRectFilled(a, {a.x + (b.x - a.x) * fill, b.y}, fromArgb(it.colour | 0xff000000u), 15);
        d->AddRectFilled({a.x + 4, a.y + 3}, {a.x + (b.x - a.x) * fill - 4, a.y + 10}, IM_COL32(255, 255, 255, 60), 6);
    }
}

// The moodlets: the game's BuffBGClip tile in the buff's mood colour, the
// buff's icon on it (SimHub), 4 across, 2 down.
void drawMoodlets(const Item* items, int count) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kMoodlets;
    const float cell = r.w / 4;
    for (int i = 0; i < count && i < 8; ++i) {
        ImVec2 c{r.x + cell * (i % 4) + cell / 2, r.y + cell * (i / 4) + cell / 2};
        ImGui::SetCursorScreenPos({c.x - cell / 2, c.y - cell / 2});
        ImGui::PushID(500 + i);
        ImGui::InvisibleButton("moodlet", {cell, cell});
        bool hover = ImGui::IsItemHovered();
        if (hover) g_hoverKind = kMoodletItems, g_hoverIndex = i;
        ImGui::PopID();
        piece(d, art::kMoodTile, c, (hover ? 120.0f : 112.0f) / art::kMoodTile.w, moodTint(items[i].colour));
        icon(d, items[i].icon, c, 84);
    }
}

// The action queue as the game's InteractionQueue draws it (its SWF:
// NormalInteractionButton, MixerInteractionButton; InteractionButton.as),
// one game pixel kQueuePx atlas pixels, the buttons side by side (the game
// stacks them), running first:
// - the disc: the Sim's mood frame of InteractionButtonEmotionBg when the
//   interaction lists that mood, else its Default (Fine) frame, at 0.917
//   (1.0 under the pointer), 1 px low; queued ones at 0.2 alpha, running or
//   starting ones full;
// - the picture: 44 px (48 under the pointer), queued at 0.8 alpha (0.9);
// - its progress, when the game sends one: the dark back, the green spokes
//   (100 round, clockwise from the top) and the rim, under the disc;
// - the X (mcCancel) under the pointer, if it can be cancelled, and on the
//   buttons the one pointed at would cancel (interactions_to_be_canceled);
// - a side action (a mixer): its small button 20 px right and 10 px down,
//   disc at 0.889 (32 px), picture 24 px (28), its own X.
constexpr float kQueuePx = 2.1f;
Action drawQueue(const Item* items, const Item* badges, int count, int* index) {
    Action action = kNone;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kQueue;
    count = std::min(count, kMaxItems);
    // Each button 58 game px apart (BUTTON_HEIGHT), more after one with a side
    // action (its badge reaches 36 px out); all made smaller if they don't fit.
    float centres[kMaxItems] = {}, x = 0;
    for (int i = 0; i < count; ++i) {
        centres[i] = x + 31;
        x += 58 + ((badges[i].flags & 1) ? 10 : 0);
    }
    const float px = std::min(kQueuePx, (r.w - 16) / std::max(1.0f, x + 4));
    const float cy = r.y + 72;
    auto at = [&](int i) { return ImVec2{r.x + 8 + centres[i] * px, cy}; };
    auto badgeAt = [&](int i) { return ImVec2{at(i).x + 20 * px, cy + 10 * px}; };
    auto mood = [](uint32_t m) { return m < sizeof(art::kQueueDisc) / sizeof(art::kQueueDisc[0]) ? m : 0u; };
    // The side actions' buttons first: over their main button, they get the pointer.
    bool badgeHover[kMaxItems] = {}, hover[kMaxItems] = {};
    for (int i = 0; i < count; ++i) {
        if (!(badges[i].flags & 1)) continue;
        const ImVec2 c = badgeAt(i);
        ImGui::SetCursorScreenPos({c.x - 16 * px, c.y - 16 * px});
        ImGui::PushID(140 + i);
        const bool pressed = ImGui::InvisibleButton("mixer", {32 * px, 32 * px});
        badgeHover[i] = ImGui::IsItemHovered();
        ImGui::PopID();
        if (badgeHover[i]) g_hoverKind = kQueueItems, g_hoverIndex = 2 * i + 1;
        if (pressed && (badges[i].flags & 2)) {
            action = kCancel;
            if (index) *index = kMaxItems + i;
        }
    }
    unsigned preview = 0;  // the buttons the one pointed at would cancel
    for (int i = 0; i < count; ++i) {
        const ImVec2 c = at(i);
        ImGui::SetCursorScreenPos({c.x - 30 * px, c.y - 30 * px});
        ImGui::PushID(100 + i);
        const bool pressed = ImGui::InvisibleButton("queue", {60 * px, 60 * px});
        hover[i] = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hover[i]) {
            g_hoverKind = kQueueItems, g_hoverIndex = 2 * i;
            preview = (items[i].flags >> 8) & 0xff;
        }
        if (pressed && (items[i].flags & 2)) {
            action = kCancel;
            if (index) *index = i;
        }
    }
    for (int i = 0; i < count; ++i) {
        const Item& it = items[i];
        const ImVec2 c = at(i);
        const bool full = it.flags & 1;  // running or starting
        const bool over = hover[i] || ((preview >> i) & 1);
        const float done = std::max(0.0f, std::min(1.0f, it.value));
        if (full && done > 0) {
            piece(d, art::kProgressShadow, c, 62 * px / art::kProgressShadow.w);
            const art::Piece& s = art::kProgressSpoke;
            const ImVec2 uv0{static_cast<float>(s.x) / art::kAtlasW, static_cast<float>(s.y) / art::kAtlasH};
            const ImVec2 uv1{static_cast<float>(s.x + s.w) / art::kAtlasW, static_cast<float>(s.y + s.h) / art::kAtlasH};
            const float len = 30 * px, half = 1 * px;
            const int spokes = static_cast<int>(done * 100 + 0.5f);
            for (int k = 0; k < spokes && g_art; ++k) {
                const float a = k * 6.2831853f / 100;
                const ImVec2 dir{std::sin(a), -std::cos(a)}, side{std::cos(a), std::sin(a)};
                d->AddImageQuad(reinterpret_cast<ImTextureID>(g_art), {c.x - side.x * half, c.y - side.y * half},
                                {c.x + dir.x * len - side.x * half, c.y + dir.y * len - side.y * half},
                                {c.x + dir.x * len + side.x * half, c.y + dir.y * len + side.y * half},
                                {c.x + side.x * half, c.y + side.y * half}, uv0, {uv1.x, uv0.y}, uv1, {uv0.x, uv1.y});
            }
            piece(d, art::kProgressRim, c, 58 * px / art::kProgressRim.w);
        }
        const art::Piece& disc = art::kQueueDisc[mood(it.colour)];
        piece(d, disc, {c.x, c.y + 1 * px}, (over ? 1.0f : 0.917f) * 60 * px / disc.w,
              IM_COL32(255, 255, 255, full ? 255 : 51));
        const int alpha = full ? 255 : over ? 229 : 204;
        icon(d, it.icon, c, (over ? 48 : 44) * px, IM_COL32(255, 255, 255, alpha));
        if (over && (it.flags & 2)) piece(d, art::kCancel, c, 36 * px / art::kCancel.w);
        const Item& b = badges[i];
        if (b.flags & 1) {
            const ImVec2 m = badgeAt(i);
            const bool bfull = b.flags & 4, bover = badgeHover[i];
            const art::Piece& mdisc = art::kMixerDisc[mood(b.colour)];
            piece(d, mdisc, m, (bover ? 1.0f : 0.889f) * 36 * px / mdisc.w, IM_COL32(255, 255, 255, bfull ? 255 : 51));
            icon(d, b.icon, m, (bover ? 28 : 24) * px, IM_COL32(255, 255, 255, bfull ? 255 : bover ? 229 : 204));
            if (bover && (b.flags & 2)) piece(d, art::kMixerCancel, {m.x - 1 * px, m.y}, 22 * px / art::kMixerCancel.w);
        }
    }
    return action;
}

// The money gained or lost, as the game's flyaway over its funds: "+§100"
// in green, "-§100" in red, rising and fading over kMoneySeconds; the newest
// lowest, up to three at once.
constexpr float kMoneySeconds = 2.6f;
struct MoneyChange {
    long long amount;
    ULONGLONG at;
};
std::mutex g_moneyMutex;
std::vector<MoneyChange> g_money;  // g_moneyMutex

std::string moneyText(long long amount) {
    unsigned long long v = amount < 0 ? 0ull - static_cast<unsigned long long>(amount) : amount;
    std::string digits = std::to_string(v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return std::string(amount < 0 ? "-" : "+") + "\xC2\xA7" + out;
}

void drawMoney() {
    std::vector<MoneyChange> shown;
    {
        std::lock_guard<std::mutex> lock(g_moneyMutex);
        const ULONGLONG now = GetTickCount64();
        g_money.erase(std::remove_if(g_money.begin(), g_money.end(),
                                     [&](const MoneyChange& m) { return now - m.at > kMoneySeconds * 1000; }),
                      g_money.end());
        shown = g_money;
    }
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kMoney;
    const ULONGLONG now = GetTickCount64();
    const int n = static_cast<int>(shown.size());
    for (int i = std::max(0, n - 3); i < n; ++i) {
        const float t = (now - shown[i].at) / 1000.0f / kMoneySeconds;
        const float alpha = t < 0.75f ? 1.0f : std::max(0.0f, (1 - t) / 0.25f);
        const std::string text = moneyText(shown[i].amount);
        const float size = g_title->FontSize * 1.25f;
        const ImVec2 ts = g_title->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        const float y = r.y + r.h - ts.y - 4 - t * (r.h - ts.y - 8);
        const float x = r.x + (r.w - ts.x) / 2;
        const bool gain = shown[i].amount >= 0;
        const ImU32 fill = gain ? IM_COL32(118, 200, 64, static_cast<int>(255 * alpha))
                                : IM_COL32(222, 62, 52, static_cast<int>(255 * alpha));
        const ImU32 edge = IM_COL32(20, 30, 20, static_cast<int>(200 * alpha));
        for (int dx = -2; dx <= 2; dx += 2)
            for (int dy = -2; dy <= 2; dy += 2)
                if (dx || dy) d->AddText(g_title, size, {x + dx, y + dy}, edge, text.c_str());
        d->AddText(g_title, size, {x, y}, fill, text.c_str());
    }
}

// The wants: the game's whim cloud with the want's icon, up to three.
Action drawWants(const Item* items, int count, int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kWants;
    // The game's wants panel: each want in a cloud, a fear (flags bit 0) in
    // the spiky bubble. Pointed at, a want shows the game's X; pressed, it is
    // dismissed (whims.refresh), as in the game.
    const float cell = r.w / 4;
    Action action = kNone;
    for (int i = 0; i < count && i < 4; ++i) {
        ImVec2 c{r.x + cell * i + cell / 2, r.y + r.h / 2};
        const bool fear = items[i].flags & 1;
        ImGui::SetCursorScreenPos({c.x - cell / 2, r.y});
        ImGui::PushID(200 + i);
        bool pressed = ImGui::InvisibleButton("want", {cell, r.h});
        bool hover = ImGui::IsItemHovered();
        if (hover) g_hoverKind = kWantItems, g_hoverIndex = i;
        ImGui::PopID();
        const art::Piece& back = fear ? art::kFearBubble : art::kWantCloud;
        float scale = (cell - 12) / back.w * (hover ? 1.06f : 1.0f);
        piece(d, back, c, scale);
        icon(d, items[i].icon, {c.x, c.y + (fear ? 0 : 5)}, fear ? 92 : 84);
        if (hover && !fear) piece(d, art::kCancel, {c.x + cell * 0.3f, c.y - r.h * 0.3f}, 56.0f / art::kCancel.w);
        if (pressed && !fear) {
            action = kDismissWant;
            if (index) *index = i;
        }
    }
    return action;
}

// A piece of art stretched over a box by nine slices: its corners (insets
// l, t, r, b in the piece's own pixels, drawn k times that) kept, its edges
// and middle stretched.
void nine(ImDrawList* d, const art::Piece& p, ImVec2 a, ImVec2 b, float l, float t, float r, float bt, float k = 1,
          ImU32 tint = kWhite) {
    if (!g_art) return;
    const float xs[4] = {a.x, a.x + l * k, b.x - r * k, b.x}, ys[4] = {a.y, a.y + t * k, b.y - bt * k, b.y};
    const float us[4] = {static_cast<float>(p.x), p.x + l, p.x + p.w - r, static_cast<float>(p.x + p.w)};
    const float vs[4] = {static_cast<float>(p.y), p.y + t, p.y + p.h - bt, static_cast<float>(p.y + p.h)};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            d->AddImage(reinterpret_cast<ImTextureID>(g_art), {xs[i], ys[j]}, {xs[i + 1], ys[j + 1]},
                        {us[i] / art::kAtlasW, vs[j] / art::kAtlasH}, {us[i + 1] / art::kAtlasW, vs[j + 1] / art::kAtlasH},
                        tint);
        }
}

// The texts of the popup from the game's string tables, once they are loaded.
void resolvePopup() {
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (g_popupNew) {
            g_popup = g_popupIn;
            g_popupOn = !g_popupIn.textJson.empty() || !g_popupIn.titleJson.empty() || !g_popupIn.buttonJson.empty();
            g_popupNew = false;
            g_popupHeight = 0;
        }
    }
    if (!g_popupOn || g_popup.resolved || !strings::ready()) return;
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (g_popupNew) return;  // a newer one came meanwhile
    }
    PopupContent& p = g_popup;
    p.resolved = true;
    if (!p.titleJson.empty()) p.title = strings::resolve(p.titleJson);
    if (!p.textJson.empty()) p.text = strings::resolve(p.textJson);
    int named = 0;
    for (size_t i = 0; i < p.buttonJson.size(); ++i) {
        std::string label = p.buttonJson[i].size() > 2 ? strings::resolve(p.buttonJson[i]) : std::string();
        if (!label.empty()) {
            ++named;
        } else {
            // A button without a text of its own: the game's OK / Cancel.
            long long id = i < p.choices.size() ? p.choices[i] : 0;
            label = id == 10002 ? "Cancel" : id == -1 ? "Close" : "OK";
        }
        p.buttons.push_back(label);
    }
    // Its words must be there: an unknown text (a string we don't have) keeps
    // the cut of the game's own dialog.
    bool textOk = p.textJson.empty() || !p.text.empty();
    p.valid = textOk && (!p.text.empty() || !p.title.empty()) &&
              (p.buttonJson.empty() || named > 0 || p.buttonJson.size() <= 2);
    {
        std::lock_guard<std::mutex> lock(g_popupMutex);
        if (!g_popupNew) g_popupState = p.valid ? 2 : 3;
    }
    static int logged = 0;
    if (logged++ < 40)
        LOG("HUD: popup texts %s (layout %d, title %zu chars, text %zu chars, %zu buttons, %d with own text)",
            p.valid ? "found" : "missing: the game's dialog is shown cut out", p.layout, p.title.size(), p.text.size(),
            p.buttons.size(), named);
}

// A Sim's picture in a portrait ring, centred on c: the bust the game's
// SimSpeechHeader / SimSpeechFooter load (thumbs.cpp), or the dialog's icon.
void portrait(ImDrawList* d, uint64_t picture, const art::Piece& ring, ImVec2 c, float size) {
    // The ring's inner circle is 102 of its 138 pixels.
    piece(d, ring, c, size * (138.0f / 102.0f) / ring.w);
    icon(d, picture, c, size);
}

// One of the dialog's buttons (DialogButton: the white pill, its over
// picture under the pointer) with its text in the game's button blue.
bool dialogButton(ImDrawList* d, int i, const std::string& label, ImVec2 a, ImVec2 b) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(300 + i);
    bool pressed = ImGui::InvisibleButton("answer", {b.x - a.x, b.y - a.y});
    bool hover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hover)
        nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.8f);
    else
        nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.8f);
    const float size = g_text->FontSize * 0.92f, wrap = b.x - a.x - 60;
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, label.c_str());
    ImVec2 at{(a.x + b.x - ts.x) / 2, a.y + (b.y - a.y - 10 - ts.y) / 2};
    d->AddText(g_text, size, at, IM_COL32(30, 70, 128, 255), label.c_str(), nullptr, wrap);
    return pressed;
}

// The popup as the game draws it. Two Sims talking (SimInteractionDialog):
// the other Sim's portrait with what they say in the blue speech bubble, your
// answers as buttons in the light bubble beside your portrait. Otherwise the
// plain dialog: its panel, title, text and buttons. Returns kAnswer with the
// button's index; its height goes to popupHeight().
Action drawPopup(int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const PopupContent& p = g_popup;
    const Rect r = kPopup;
    Action action = kNone;
    float bottom = r.y;
    const int n = static_cast<int>(p.buttons.size());
    auto answer = [&](int i, ImVec2 a, ImVec2 b) {
        if (dialogButton(d, i, p.buttons[i], a, b)) {
            action = kAnswer;
            if (index) *index = i;
        }
    };
    if (p.layout == 1) {
        const float P = 176, gap = 26;
        const float bx0 = r.x + 8 + P + gap, bx1 = r.x + r.w - 8 - P - gap;
        // What they say: the speech bubble (its tail on the left, at the top).
        const std::string& said = p.text.empty() ? p.title : p.text;
        // (Its corners at 0.6: the tail and roundings as the game's are to its text.)
        const float size = g_text->FontSize, wrap = bx1 - bx0 - 40 - 44;
        ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, wrap, said.c_str());
        const float y0 = r.y + 8, h = std::max(P * 0.75f, ts.y + 64);
        nine(d, art::kSpeechBubble, {bx0, y0}, {bx1, y0 + h}, 66, 108, 66, 66, 0.6f);
        d->AddText(g_text, size, {bx0 + 40 + 16, y0 + 5 + (h - 9 - ts.y) / 2}, kWhite, said.c_str(), nullptr, wrap);
        portrait(d, p.pictures[0], art::kPortraitRing, {r.x + 8 + P / 2, y0 + P / 2}, P);
        // Your answers: the light bubble (its tail on the right, low), twice
        // over: the game draws it over its dimming, here it is over the world.
        const float bh = 76, bgap = 12;
        const float y1 = y0 + h + 18;
        const float fh = std::max(130.0f, 30 + n * bh + std::max(0, n - 1) * bgap + 34);
        for (int k = 0; k < 2; ++k) nine(d, art::kAnswerBubble, {bx0, y1}, {bx1 + 40, y1 + fh}, 60, 60, 66, 138, 0.6f);
        for (int i = 0; i < n; ++i) {
            float y = y1 + 30 + i * (bh + bgap);
            answer(i, {bx0 + 24, y}, {bx1 + 40 - 40 - 18, y + bh});
        }
        portrait(d, p.pictures[1], art::kYourRing, {r.x + r.w - 8 - P / 2, y1 + P / 2 + 6}, P);
        bottom = std::max(y1 + fh, y1 + P + 20);
    } else {
        // The plain dialog: the panel, the title, the text, the buttons (side
        // by side for two short ones, else one above the other).
        const float x0 = r.x + 40, x1 = r.x + r.w - 40, pad = 56, wrap = x1 - x0 - 2 * pad;
        float y = r.y + 8 + 44;
        const ImU32 kTitle = IM_COL32(29, 58, 102, 255), kBody = IM_COL32(44, 52, 66, 255);
        ImVec2 tt = p.title.empty() ? ImVec2{0, 0} : g_title->CalcTextSizeA(g_title->FontSize, FLT_MAX, wrap, p.title.c_str());
        ImVec2 xs = p.text.empty() ? ImVec2{0, 0} : g_text->CalcTextSizeA(g_text->FontSize, FLT_MAX, wrap, p.text.c_str());
        bool row = n == 2 && p.buttons[0].size() < 18 && p.buttons[1].size() < 18;
        const float bh = 76, bgap = 16;
        float buttonsH = n == 0 ? 0 : row ? bh : n * bh + (n - 1) * bgap;
        float h = 44 + tt.y + (tt.y > 0 && xs.y > 0 ? 20 : 0) + xs.y + (n ? 40 : 0) + buttonsH + 50;
        nine(d, art::kDialogPanel, {x0, r.y + 8}, {x1, r.y + 8 + h}, 36, 36, 36, 36, 1, IM_COL32(244, 247, 252, 255));
        if (tt.y > 0) {
            d->AddText(g_title, g_title->FontSize, {(x0 + x1 - tt.x) / 2, y}, kTitle, p.title.c_str(), nullptr, wrap);
            y += tt.y + (xs.y > 0 ? 20 : 0);
        }
        if (xs.y > 0) {
            d->AddText(g_text, g_text->FontSize, {x0 + pad, y}, kBody, p.text.c_str(), nullptr, wrap);
            y += xs.y;
        }
        y += n ? 40 : 0;
        if (row) {
            float w = (wrap - 30) / 2;
            answer(0, {x0 + pad, y}, {x0 + pad + w, y + bh});
            answer(1, {x1 - pad - w, y}, {x1 - pad, y + bh});
        } else {
            for (int i = 0; i < n; ++i, y += bh + bgap) answer(i, {x0 + pad + 40, y}, {x1 - pad - 40, y + bh});
        }
        bottom = r.y + 8 + h;
    }
    g_popupHeight = std::min(r.h, bottom - r.y + 12);
    return action;
}

// A LocalizedString's text (strings.cpp), kept once found.
const std::string& found(const std::string& json) {
    static const std::string kNothing;
    if (json.empty()) return kNothing;
    auto it = g_found.find(json);
    if (it != g_found.end()) return it->second;
    if (!strings::ready()) return kNothing;
    if (g_found.size() > 2000) g_found.clear();
    return g_found[json] = strings::resolve(json);
}

// The tooltip of the need or moodlet pointed at, as the game's: its name,
// what it means, why you have it (grey) and the time it has left, on the
// dialog panel. False with no texts for it.
bool drawTip(const Tip& tip) {
    const std::string &title = found(tip.json[0]), &text = found(tip.json[1]), &reason = found(tip.json[2]);
    std::string left;
    if (tip.minutes > 0) {
        char buffer[48];
        if (tip.minutes >= 120)
            std::snprintf(buffer, sizeof buffer, "%d hours left", (tip.minutes + 30) / 60);
        else if (tip.minutes >= 60)
            std::snprintf(buffer, sizeof buffer, "1 hour left");
        else
            std::snprintf(buffer, sizeof buffer, "%d minutes left", std::max(1, tip.minutes));
        left = buffer;
    }
    if (title.empty() && text.empty()) return false;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kTip;
    const float pad = 34, wrap = r.w - 16 - 2 * pad;
    struct Line {
        ImFont* font;
        float size;
        ImU32 colour;
        const std::string* text;
    };
    const Line lines[4] = {{g_small, g_small->FontSize, IM_COL32(29, 58, 102, 255), &title},
                           {g_text, g_text->FontSize * 0.9f, IM_COL32(44, 52, 66, 255), &text},
                           {g_text, g_text->FontSize * 0.8f, IM_COL32(110, 118, 130, 255), &reason},
                           {g_text, g_text->FontSize * 0.8f, IM_COL32(44, 52, 66, 255), &left}};
    float w = 0, h = 0;
    ImVec2 sizes[4];
    for (int i = 0; i < 4; ++i) {
        sizes[i] = lines[i].text->empty() ? ImVec2{0, 0}
                                          : lines[i].font->CalcTextSizeA(lines[i].size, FLT_MAX, wrap, lines[i].text->c_str());
        w = std::max(w, sizes[i].x);
        if (sizes[i].y > 0) h += sizes[i].y + (h > 0 ? 10 : 0);
    }
    w += 2 * pad;
    h += 2 * pad - 6;
    const float x0 = r.x + (r.w - w) / 2, y0 = r.y + 8;
    nine(d, art::kDialogPanel, {x0, y0}, {x0 + w, y0 + h}, 36, 36, 36, 36, 1, IM_COL32(244, 247, 252, 250));
    float y = y0 + pad - 4;
    for (int i = 0; i < 4; ++i) {
        if (sizes[i].y <= 0) continue;
        d->AddText(lines[i].font, lines[i].size, {x0 + pad, y}, lines[i].colour, lines[i].text->c_str(), nullptr, wrap);
        y += sizes[i].y + 10;
    }
    g_tipHeight = std::min(r.h, h + 16);
    return true;
}

// A record's fields (0x1f between them).
std::vector<std::string> fieldsOf(const std::string& record) {
    std::vector<std::string> f;
    for (size_t k = 0;;) {
        size_t e = record.find('\x1f', k);
        f.push_back(record.substr(k, e == std::string::npos ? std::string::npos : e - k));
        if (e == std::string::npos) break;
        k = e + 1;
    }
    return f;
}

// The picker's row state with the boxes as they are (the game's
// RecipePickerRowData.is_enable / GetDiscountedPrice).
int pickerVariant() { return g_pickerFresh && g_pickerPrepped ? 3 : g_pickerFresh ? 1 : g_pickerPrepped ? 2 : 0; }
bool rowEnabled(const PickerRow& r) {
    static const unsigned kBit[4] = {1, 2, 4, 8};
    return (r.enabled & kBit[pickerVariant()]) != 0;
}
unsigned rowPrice(const PickerRow& r) {
    const int v = pickerVariant();
    if (v) return r.price[v];
    return r.discounted ? r.discounted : r.price[0];
}

// The picker's texts from the game's string tables, once they are loaded,
// and its top rows in the order of its sort column.
void resolvePicker() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (g_pickerNew) {
            changed = true;
            g_picker = g_pickerIn;
            g_pickerOn = !g_pickerIn.rows.empty();
            g_pickerNew = false;
            g_pickerHeight = 0;
            g_pickerPage = 0;
            g_pickerTab = g_pickerOpen = -1;
            // GridPicker: Use Ingredients is ticked whenever the picker has an
            // ingredients column, Use Prepped Ingredients whenever it shows.
            g_pickerFresh = (g_picker.flags & 4) != 0;
            g_pickerPrepped = (g_picker.flags & 2) != 0;
        }
    }
    if (changed) {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_pickerIcons.clear();  // a new picker (or none): its icons are asked for as it draws them
    }
    if (!g_pickerOn || g_picker.resolved || !strings::ready()) return;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (g_pickerNew) return;  // a newer one came meanwhile
    }
    PickerContent& p = g_picker;
    p.resolved = true;
    if (!p.titleJson.empty()) p.title = strings::resolve(p.titleJson);
    for (PickerFilter& f : p.filters)
        if (!f.json.empty()) f.text = strings::resolve(f.json);
    int tops = 0, named = 0;
    const int n = static_cast<int>(p.rows.size());
    for (int i = 0; i < n; ++i) {
        PickerRow& r = p.rows[i];
        if (!r.nameJson.empty()) r.name = strings::resolve(r.nameJson);
        if (!r.servingJson.empty()) r.serving = strings::resolve(r.servingJson);
        r.servings.erase(std::remove_if(r.servings.begin(), r.servings.end(), [n](int k) { return k < 0 || k >= n; }),
                         r.servings.end());
        if (!r.top) continue;
        ++tops;
        if (!r.name.empty()) ++named;
        p.order.push_back(i);
        p.mostServings = std::max(p.mostServings, r.servings.size());
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (p.sort == "name")
        std::stable_sort(p.order.begin(), p.order.end(),
                         [&](int a, int b) { return lower(p.rows[a].name) < lower(p.rows[b].name); });
    else if (p.sort == "price")
        std::stable_sort(p.order.begin(), p.order.end(), [&](int a, int b) { return p.rows[a].price[0] < p.rows[b].price[0]; });
    else if (p.sort == "skill_level")
        std::stable_sort(p.order.begin(), p.order.end(), [&](int a, int b) { return p.rows[a].skill < p.rows[b].skill; });
    // Every row's name must be there: a missing one keeps the cut of the game's picker.
    p.valid = tops > 0 && named == tops;
    {
        std::lock_guard<std::mutex> lock(g_pickerMutex);
        if (!g_pickerNew) g_pickerState = p.valid ? 2 : 3;
    }
    static int logged = 0;
    if (logged++ < 40)
        LOG("HUD: picker texts %s (%d rows, %d in the list, %d named, %zu filters, sorted by %s, boxes %d)",
            p.valid ? "found" : "missing: the game's picker is shown cut out", n, tops, named, p.filters.size(),
            p.sort.empty() ? "the message's order" : p.sort.c_str(), p.flags);
}

// The price as the game shows it ("§45"); a changed one (ingredients, a
// discount) the old one struck through, the new one green (cheaper) or red.
void pickerPrice(ImDrawList* d, const PickerRow& r, ImVec2 rightTop, float size) {
    const unsigned now = rowPrice(r), was = r.price[0];
    if (!now && !was) return;
    char text[32], old[32];
    std::snprintf(text, sizeof text, "\xc2\xa7%u", now);
    ImVec2 ts = g_text->CalcTextSizeA(size, FLT_MAX, 0, text);
    const ImU32 colour = now == was ? IM_COL32(44, 52, 66, 255) : now < was ? IM_COL32(40, 140, 60, 255) : IM_COL32(190, 60, 50, 255);
    d->AddText(g_text, size, {rightTop.x - ts.x, rightTop.y}, colour, text);
    if (now == was) return;
    std::snprintf(old, sizeof old, "\xc2\xa7%u", was);
    ImVec2 os = g_text->CalcTextSizeA(size * 0.85f, FLT_MAX, 0, old);
    ImVec2 at{rightTop.x - ts.x - 12 - os.x, rightTop.y + size * 0.1f};
    const ImU32 grey = IM_COL32(130, 136, 146, 255);
    d->AddText(g_text, size * 0.85f, at, grey, old);
    d->AddLine({at.x - 2, at.y + os.y * 0.55f}, {at.x + os.x + 2, at.y + os.y * 0.55f}, grey, 2.5f);
}

// The game's recipe picker (GridPicker) as a card in the game's dialog art:
// the title and the close X, the filter tabs, the top rows two by two (icon,
// name, price; greyed when they can't be made now), the page arrows and the
// Use Ingredients boxes. A row with servings (the game's linked rows) opens
// them; a row or serving picks it. Returns kPick (index: row * 4 + the boxes)
// or kPickerClose; its height goes to pickerHeight().
Action drawPicker(int* index) {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const PickerContent& p = g_picker;
    const Rect r = kPicker;
    const ImU32 kTitle = IM_COL32(29, 58, 102, 255), kBody = IM_COL32(44, 52, 66, 255), kGrey = IM_COL32(130, 136, 146, 255);
    const float x0 = r.x + 8, x1 = r.x + r.w - 8, top = r.y + 8, pad = 34;
    const float in0 = x0 + pad, in1 = x1 - pad;
    Action action = kNone;
    int pressedRow = -1;
    std::vector<uint64_t> shown;
    int id = 0;
    auto button = [&](ImVec2 a, ImVec2 b, bool* hover) {
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(2000 + id++);
        bool pressed = ImGui::InvisibleButton("pick", {std::max(1.0f, b.x - a.x), std::max(1.0f, b.y - a.y)});
        if (hover) *hover = ImGui::IsItemHovered();
        ImGui::PopID();
        return pressed;
    };
    auto boxes = [&]() {
        return ((p.flags & 1) && g_pickerFresh ? 1 : 0) + ((p.flags & 2) && g_pickerPrepped ? 2 : 0);
    };
    auto anyEnabled = [&](const PickerRow& row) {
        if (rowEnabled(row)) return true;
        for (int k : row.servings)
            if (rowEnabled(p.rows[k])) return true;
        return false;
    };

    // Its height first (the panel goes under everything): the same for every
    // page, tab and opened row, so the card keeps its size; as many lines of
    // rows a page as fit in kPicker (up to 6).
    const float titleSize = g_title->FontSize, textSize = g_text->FontSize * 0.8f;
    ImVec2 tt = p.title.empty() ? ImVec2{0, titleSize} : g_title->CalcTextSizeA(titleSize, FLT_MAX, in1 - in0 - 140, p.title.c_str());
    const float tabW = 96, tabH = 80, tabGap = 10;
    const int tabsPerLine = std::max(1, static_cast<int>((in1 - in0 + tabGap) / (tabW + tabGap)));
    const int tabCount = p.filters.empty() ? 0 : static_cast<int>(p.filters.size()) + 1;
    const int tabLines = (tabCount + tabsPerLine - 1) / tabsPerLine;
    const bool freshBox = (p.flags & 1) != 0, preppedBox = (p.flags & 2) != 0;
    const float boxLine = 56, arrowsH = 76;
    const float boxesH = ((freshBox ? 1 : 0) + (preppedBox ? 1 : 0)) * boxLine;
    const int kCols = 2;
    const float cellGap = 10, cellH = 112, cellW = (in1 - in0 - cellGap * (kCols - 1)) / kCols;
    const float fixedH = 30 + tt.y + 18 + (tabLines ? tabLines * (tabH + tabGap) + 8 : 0) + 22 +
                         std::max(arrowsH, boxesH) + 30 + 16;
    const int rowsPerPage = std::max(2, std::min(6, static_cast<int>((r.h - fixedH + cellGap) / (cellH + cellGap))));
    const int perPage = kCols * rowsPerPage;
    const int totalTop = static_cast<int>(p.order.size());
    const int gridLines = std::max(1, std::min(rowsPerPage, (totalTop + kCols - 1) / kCols));
    const float gridH = gridLines * (cellH + cellGap) - cellGap;
    const float servH = 76, servGap = 12;
    const float openH = p.mostServings ? cellH + 16 + p.mostServings * (servH + servGap) + servH : 0;
    const float contentH = std::max(gridH, openH);
    const bool paged = totalTop > perPage;  // (all rows: the card keeps its size whatever the tab)
    const float footerH = std::max(paged ? arrowsH : 0.0f, boxesH);

    // The rows of the chosen tab, and the page of them.
    std::vector<int> list;
    for (int i : p.order) {
        const PickerRow& row = p.rows[i];
        if (g_pickerTab >= 0 && g_pickerTab < static_cast<int>(p.filters.size()) &&
            std::find(row.tags.begin(), row.tags.end(), p.filters[g_pickerTab].tag) == row.tags.end())
            continue;
        list.push_back(i);
    }
    const int pages = std::max(1, (static_cast<int>(list.size()) + perPage - 1) / perPage);
    g_pickerPage = std::max(0, std::min(g_pickerPage, pages - 1));
    if (g_pickerOpen >= static_cast<int>(p.rows.size())) g_pickerOpen = -1;

    float y = top + 30;
    const float titleY = y;
    y += tt.y + 18;
    const float tabsY = y;
    if (tabLines) y += tabLines * (tabH + tabGap) + 8;
    const float gridY = y;
    y += contentH;
    const float footerY = y + (footerH > 0 ? 22 : 0);
    const float bottom = footerY + footerH + 30;
    nine(d, art::kDialogPanel, {x0, top}, {x1, bottom}, 36, 36, 36, 36, 1, IM_COL32(244, 247, 252, 255));

    // The title, and the close X in the corner (the game's Cancel).
    if (!p.title.empty())
        d->AddText(g_title, titleSize, {(in0 + in1 - tt.x) / 2, titleY}, kTitle, p.title.c_str(), nullptr, in1 - in0 - 140);
    {
        const float s = 76;
        ImVec2 c{in1 - s / 2 + 8, titleY + titleSize / 2};
        bool hover = false;
        if (button({c.x - s / 2, c.y - s / 2}, {c.x + s / 2, c.y + s / 2}, &hover)) action = kPickerClose;
        piece(d, art::kCancel, c, (hover ? s * 1.08f : s) / art::kCancel.w);
    }

    // The filter tabs: All, then the picker's categories (their icons).
    for (int t = 0; t < tabCount; ++t) {
        const int tab = t - 1;
        const float tx = in0 + (t % tabsPerLine) * (tabW + tabGap), ty = tabsY + (t / tabsPerLine) * (tabH + tabGap);
        bool hover = false;
        if (button({tx, ty}, {tx + tabW, ty + tabH}, &hover)) {
            g_pickerTab = tab;
            g_pickerPage = 0;
            g_pickerOpen = -1;
        }
        const bool on = g_pickerTab == tab;
        nine(d, art::kDialogPanel, {tx, ty}, {tx + tabW, ty + tabH}, 36, 36, 36, 36, 0.4f,
             on ? IM_COL32(150, 196, 240, 255) : hover ? IM_COL32(218, 232, 250, 255) : IM_COL32(228, 233, 241, 255));
        const PickerFilter* f = tab >= 0 ? &p.filters[tab] : nullptr;
        if (f && f->icon) {
            icon(d, f->icon, {tx + tabW / 2, ty + tabH / 2}, 60);
            shown.push_back(f->icon);
        } else {
            const char* label = f ? f->text.c_str() : "All";
            const float size = g_small->FontSize * 0.7f;
            ImVec2 ls = g_small->CalcTextSizeA(size, FLT_MAX, tabW - 8, label);
            d->AddText(g_small, size, {tx + (tabW - ls.x) / 2, ty + (tabH - ls.y) / 2}, kTitle, label, nullptr, tabW - 8);
        }
    }

    // A row's cell: its icon, name and price.
    auto cell = [&](const PickerRow& row, ImVec2 a, ImVec2 b, bool enabled, bool hover, bool more) {
        nine(d, art::kDialogPanel, a, b, 36, 36, 36, 36, 0.5f,
             !enabled ? IM_COL32(234, 236, 240, 255) : hover ? IM_COL32(214, 230, 250, 255) : kWhite);
        const float is = b.y - a.y - 24;
        if (row.icon) {
            icon(d, row.icon, {a.x + 14 + is / 2, (a.y + b.y) / 2}, is, enabled ? kWhite : IM_COL32(255, 255, 255, 110));
            shown.push_back(row.icon);
        }
        const float tx = a.x + 14 + is + 14, wrap = b.x - tx - (more ? 34 : 14);
        const ImVec4 clip{tx, a.y + 6, b.x - 8, b.y - 6};
        d->AddText(g_text, textSize, {tx, a.y + 12}, enabled ? kBody : kGrey, row.name.c_str(), nullptr, wrap, &clip);
        if (p.flags & 8) pickerPrice(d, row, {b.x - (more ? 34 : 16), b.y - 14 - textSize}, textSize);
        if (more) {
            // It opens its servings: the game's expand arrow.
            const float mx = b.x - 22, my = (a.y + b.y) / 2;
            d->AddTriangleFilled({mx - 7, my - 12}, {mx - 7, my + 12}, {mx + 7, my}, enabled ? kTitle : kGrey);
        }
    };

    if (g_pickerOpen < 0) {
        // The page of rows, two by two.
        const int first = g_pickerPage * perPage;
        for (int k = first; k < std::min(first + perPage, static_cast<int>(list.size())); ++k) {
            const int i = list[k], slot = k - first;
            const PickerRow& row = p.rows[i];
            const float cx = in0 + (slot % kCols) * (cellW + cellGap), cy = gridY + (slot / kCols) * (cellH + cellGap);
            const bool enabled = anyEnabled(row), more = !row.servings.empty();
            bool hover = false;
            if (button({cx, cy}, {cx + cellW, cy + cellH}, &hover) && enabled) {
                if (more) {
                    g_pickerOpen = i;
                } else {
                    pressedRow = i;
                }
            }
            cell(row, {cx, cy}, {cx + cellW, cy + cellH}, enabled, hover && enabled, more);
        }
        if (list.empty()) {
            const char* none = "Nothing here";
            ImVec2 ns = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, none);
            d->AddText(g_text, textSize, {(in0 + in1 - ns.x) / 2, gridY + 20}, kGrey, none);
        }
    } else {
        // The opened row: its servings, each picked as the game's linked rows are, and Back.
        const PickerRow& row = p.rows[g_pickerOpen];
        cell(row, {in0, gridY}, {in1, gridY + cellH}, anyEnabled(row), false, false);
        float sy = gridY + cellH + 16;
        for (int k : row.servings) {
            const PickerRow& s = p.rows[k];
            const bool enabled = rowEnabled(s);
            const ImVec2 a{in0 + 40, sy}, b{in1 - 40, sy + servH};
            bool hover = false;
            if (button(a, b, &hover) && enabled) pressedRow = k;
            if (hover && enabled)
                nine(d, art::kDialogButtonOver, {a.x - 2, a.y - 3}, {b.x + 2, b.y + 6}, 48, 36, 48, 54, 0.8f);
            else
                nine(d, art::kDialogButton, a, b, 48, 36, 48, 42, 0.8f, enabled ? kWhite : IM_COL32(255, 255, 255, 120));
            const std::string& label = s.serving.empty() ? s.name : s.serving;
            const ImVec4 clip{a.x + 30, a.y, b.x - 150, b.y};
            ImVec2 ls = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, label.c_str());
            d->AddText(g_text, textSize, {a.x + 36, a.y + (b.y - a.y - 8 - ls.y) / 2},
                       enabled ? IM_COL32(30, 70, 128, 255) : kGrey, label.c_str(), nullptr, 0, &clip);
            if (p.flags & 8) pickerPrice(d, s, {b.x - 36, a.y + (b.y - a.y - 8 - textSize) / 2}, textSize);
            sy += servH + servGap;
        }
        if (dialogButton(d, 60, "Back", {in0 + 40, sy}, {in0 + 300, sy + servH})) g_pickerOpen = -1;
    }

    // The footer: the ingredient boxes (left) and the page arrows (right).
    float by = footerY;
    auto box = [&](const char* label, bool& on) {
        const float s = 40;
        ImVec2 a{in0, by + (boxLine - s) / 2}, b{in0 + s, by + (boxLine + s) / 2};
        ImVec2 ls = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, label);
        bool hover = false;
        if (button({a.x - 6, by}, {b.x + 16 + ls.x + 6, by + boxLine}, &hover)) on = !on;
        d->AddRectFilled(a, b, hover ? IM_COL32(218, 232, 250, 255) : kWhite, 6);
        d->AddRect(a, b, kTitle, 6, 0, 3);
        if (on) {
            const ImVec2 tick[3] = {{a.x + 8, a.y + s * 0.52f}, {a.x + s * 0.42f, b.y - 9}, {b.x - 7, a.y + 9}};
            d->AddPolyline(tick, 3, kTitle, 0, 5);
        }
        d->AddText(g_text, textSize, {b.x + 16, by + (boxLine - ls.y) / 2}, kBody, label);
        by += boxLine;
    };
    if (freshBox) box("Use Ingredients", g_pickerFresh);
    if (preppedBox) box("Use Prepped Ingredients", g_pickerPrepped);
    if (pages > 1 && g_pickerOpen < 0) {
        const float bw = 104, bh = 76;
        char where[32];
        std::snprintf(where, sizeof where, "%d / %d", g_pickerPage + 1, pages);
        ImVec2 ws = g_text->CalcTextSizeA(textSize, FLT_MAX, 0, where);
        const float right = in1, nx = right - bw, px = nx - 24 - ws.x - 24 - bw;
        if (dialogButton(d, 61, "<", {px, footerY}, {px + bw, footerY + bh})) g_pickerPage = (g_pickerPage + pages - 1) % pages;
        d->AddText(g_text, textSize, {px + bw + 24, footerY + (bh - 10 - ws.y) / 2}, kBody, where);
        if (dialogButton(d, 62, ">", {nx, footerY}, {nx + bw, footerY + bh})) g_pickerPage = (g_pickerPage + 1) % pages;
    }

    if (pressedRow >= 0) {
        action = kPick;
        if (index) *index = pressedRow * 4 + boxes();
    }
    std::sort(shown.begin(), shown.end());
    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        if (shown != g_pickerIcons) g_pickerIcons.swap(shown);
    }
    g_pickerHeight = std::min(r.h, bottom - r.y + 8);
    return action;
}

// The round build/buy button: the game's GameTray icon on a light disc.
Action drawBuild() {
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const Rect r = kBuild;
    ImVec2 c{r.x + r.w / 2, r.y + r.h / 2};
    ImGui::SetCursorScreenPos({r.x, r.y});
    bool pressed = ImGui::InvisibleButton("build", {r.w, r.h});
    bool hover = ImGui::IsItemHovered();
    piece(d, art::kQueueDisc[0], c, (r.w - 8) / art::kQueueDisc[0].w, IM_COL32(205, 210, 215, 215));
    piece(d, hover ? art::kBuildIconOver : art::kBuildIcon, c, (r.w * 0.62f) / art::kBuildIcon.w);
    return pressed ? kBuildPressed : kNone;
}

}  // namespace

void setItems(int kind, const Item* items, int count) {
    if (kind < 0 || kind >= kKinds) return;
    count = std::max(0, std::min(count, kMaxItems));
    std::lock_guard<std::mutex> lock(g_itemMutex);
    std::copy(items, items + count, g_items[kind]);
    g_counts[kind] = count;
}

int itemCount(int kind) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    return kind >= 0 && kind < kKinds ? g_counts[kind] : 0;
}

bool hasItems(int kind) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    return kind >= 0 && kind < kKinds && g_counts[kind] > 0;
}

void queueIcon(uint64_t instance, const uint32_t* rgba, int w, int h);  // below

void setIcon(uint64_t instance, const uint8_t* data, size_t size) {
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
    if (!data || !size || !phone3d::decodePicture(data, size, rgba, w, h) || w <= 0 || h <= 0) {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_badIcons.insert(instance);
        // What it was, to add its format (DDS: its FourCC at 84, flags at 80, bits at 88).
        char head[5] = {}, fourcc[5] = {};
        uint32_t ddsFlags = 0, ddsBits = 0;
        if (data && size >= 4) std::memcpy(head, data, 4);
        if (data && size >= 128) {
            std::memcpy(fourcc, data + 84, 4);
            std::memcpy(&ddsFlags, data + 80, 4);
            std::memcpy(&ddsBits, data + 88, 4);
        }
        for (char& c : head) c = c && (c < 32 || c > 126) ? '?' : c;
        for (char& c : fourcc) c = c && (c < 32 || c > 126) ? '?' : c;
        LOG("HUD: icon %016llx could not be read (%zu bytes, starts '%s', DDS FourCC '%s' flags %x bits %u)",
            static_cast<unsigned long long>(instance), data ? size : 0, head, fourcc, ddsFlags, ddsBits);
        return;
    }
    queueIcon(instance, rgba.data(), w, h);
}

void expectPicture(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_pictureIds.insert(id);
}

void setPicture(uint64_t id, const uint32_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    expectPicture(id);
    queueIcon(id, rgba, w, h);
}

bool hasPicture(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    if (g_loadedIcons.count(id)) return true;
    for (const PendingIcon& p : g_pendingIcons)
        if (p.id == id) return true;
    return false;
}

// A picture into a square slot, aspect kept, filtered down (box) or up (nearest).
void queueIcon(uint64_t instance, const uint32_t* rgba, int w, int h) {
    PendingIcon p{instance, std::vector<uint32_t>(kIconSize * kIconSize, 0)};
    float scale = std::min(static_cast<float>(kIconSize) / w, static_cast<float>(kIconSize) / h);
    int dw = std::max(1, static_cast<int>(w * scale)), dh = std::max(1, static_cast<int>(h * scale));
    int ox = (kIconSize - dw) / 2, oy = (kIconSize - dh) / 2;
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
            int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
            float sum[4] = {}, weight = 0;
            for (int sy = y0; sy < y1 && sy < h; ++sy)
                for (int sx = x0; sx < x1 && sx < w; ++sx) {
                    uint32_t c = rgba[static_cast<size_t>(sy) * w + sx];
                    float a = static_cast<float>(c >> 24);
                    sum[0] += (c & 0xff) * a;
                    sum[1] += ((c >> 8) & 0xff) * a;
                    sum[2] += ((c >> 16) & 0xff) * a;
                    sum[3] += a;
                    weight += 1;
                }
            if (weight <= 0) continue;
            uint32_t out = 0;
            if (sum[3] > 0) {
                for (int k = 0; k < 3; ++k) out |= static_cast<uint32_t>(std::min(255.0f, sum[k] / sum[3] + 0.5f)) << (8 * k);
                out |= static_cast<uint32_t>(std::min(255.0f, sum[3] / weight + 0.5f)) << 24;
            }
            p.pixels[static_cast<size_t>(oy + y) * kIconSize + ox + x] = out;
        }
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_pendingIcons.push_back(std::move(p));
}

int missingIcons(uint64_t* out, int max) {
    std::lock_guard<std::mutex> lock(g_itemMutex);
    int n = 0;
    auto want = [&](uint64_t id) {
        if (n >= max || !id || g_loadedIcons.count(id) || g_badIcons.count(id) || g_pictureIds.count(id)) return;
        bool queued = false;
        for (const PendingIcon& p : g_pendingIcons) queued = queued || p.id == id;
        for (int j = 0; j < n; ++j) queued = queued || out[j] == id;
        if (!queued) out[n++] = id;
    };
    for (int k = 0; k < kKinds; ++k)
        for (int i = 0; i < g_counts[k]; ++i) want(g_items[k][i].icon);
    for (uint64_t id : g_pickerIcons) want(id);
    for (uint64_t id : g_popupIcons) want(id);
    return n;
}

void setPickerContent(const char* data, size_t size) {
    PickerContent c;
    std::string all(data ? data : "", data ? size : 0);
    auto numbers = [](const std::string& s) {
        std::vector<long long> v;
        for (size_t k = 0; k < s.size();) {
            size_t e = s.find(',', k);
            if (e == std::string::npos) e = s.size();
            if (e > k) v.push_back(std::atoll(s.substr(k, e - k).c_str()));
            k = e + 1;
        }
        return v;
    };
    size_t at = 0;
    bool first = true;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        const std::vector<std::string> f = fieldsOf(all.substr(at, end - at));
        at = end + 1;
        if (first) {
            first = false;
            if (f.size() >= 4 && f[0] == "picker") {
                c.dialog = std::atoll(f[1].c_str());
                c.flags = std::atoi(f[2].c_str());
                c.sort = f[3];
            }
        } else if (f[0] == "title" && f.size() > 1) {
            c.titleJson = f[1];
        } else if (f[0] == "filter" && f.size() > 3) {
            PickerFilter filter;
            filter.tag = static_cast<unsigned>(std::strtoul(f[1].c_str(), nullptr, 10));
            filter.icon = std::strtoull(f[2].c_str(), nullptr, 10);
            filter.json = f[3];
            c.filters.push_back(filter);
        } else if (f[0] == "row" && f.size() > 13) {
            PickerRow row;
            row.top = f[1] == "1";
            row.nameJson = f[2];
            row.icon = std::strtoull(f[3].c_str(), nullptr, 10);
            row.enabled = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
            for (int k = 0; k < 4; ++k) row.price[k] = static_cast<unsigned>(std::strtoul(f[5 + k].c_str(), nullptr, 10));
            row.discounted = static_cast<unsigned>(std::strtoul(f[9].c_str(), nullptr, 10));
            row.skill = std::atoi(f[10].c_str());
            row.servingJson = f[11];
            for (long long k : numbers(f[12])) row.servings.push_back(static_cast<int>(k));
            for (long long k : numbers(f[13])) row.tags.push_back(static_cast<unsigned>(k));
            c.rows.push_back(row);
        }
        if (end >= all.size()) break;
    }
    std::lock_guard<std::mutex> lock(g_pickerMutex);
    g_pickerIn = c;
    g_pickerNew = true;
    g_pickerHeight = 0;
    g_pickerState = c.rows.empty() ? 0 : 1;
}

bool pickerPending() {
    int state = g_pickerState.load();
    return state == 1 || state == 2;
}

float pickerHeight() { return g_pickerState.load() == 2 ? g_pickerHeight.load() : 0.0f; }

void setPopupContent(const char* data, size_t size) {
    PopupContent c;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    bool first = true;
    while (at <= all.size() && !all.empty()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        std::string record = all.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> f;
        for (size_t k = 0;;) {
            size_t e = record.find('\x1f', k);
            f.push_back(record.substr(k, e == std::string::npos ? std::string::npos : e - k));
            if (e == std::string::npos) break;
            k = e + 1;
        }
        if (first) {
            c.layout = f[0] == "1" ? 1 : 0;
            for (int k = 0; k < 2; ++k)
                if (f.size() > static_cast<size_t>(2 + k)) c.pictures[k] = std::strtoull(f[2 + k].c_str(), nullptr, 10);
            first = false;
        } else if (f[0] == "title" && f.size() > 1) {
            c.titleJson = f[1];
        } else if (f[0] == "text" && f.size() > 1) {
            c.textJson = f[1];
        } else if (f[0] == "button" && f.size() > 2) {
            c.buttonJson.push_back(f[1]);
            c.choices.push_back(std::atoll(f[2].c_str()));
        }
        if (end >= all.size()) break;
    }
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_popupIcons.assign(c.pictures, c.pictures + 2);
    }
    std::lock_guard<std::mutex> lock(g_popupMutex);
    g_popupIn = c;
    g_popupNew = true;
    g_popupHeight = 0;
    g_popupState = !c.textJson.empty() || !c.titleJson.empty() || !c.buttonJson.empty() ? 1 : 0;
}

void setTexts(int kind, const char* data, size_t size) {
    if (kind < 0 || kind >= kKinds) return;
    std::vector<Tip> tips;
    std::string all(data ? data : "", data ? size : 0);
    size_t at = 0;
    while (!all.empty() && at <= all.size()) {
        size_t end = all.find('\x1e', at);
        if (end == std::string::npos) end = all.size();
        std::string record = all.substr(at, end - at);
        Tip tip;
        size_t k = 0;
        for (int field = 0; field < 4; ++field) {
            size_t e = record.find('\x1f', k);
            std::string value = record.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (field < 3)
                tip.json[field] = value;
            else
                tip.minutes = std::atoi(value.c_str());
            if (e == std::string::npos) break;
            k = e + 1;
        }
        tips.push_back(tip);
        if (end >= all.size()) break;
        at = end + 1;
    }
    std::lock_guard<std::mutex> lock(g_itemMutex);
    g_tips[kind] = tips;
}

int tipKind() { return g_tipKind.load(); }

void moneyChanged(long long amount) {
    if (!amount) return;
    std::lock_guard<std::mutex> lock(g_moneyMutex);
    g_money.push_back({amount, GetTickCount64()});
    if (g_money.size() > 6) g_money.erase(g_money.begin());
}

bool moneyShowing() {
    std::lock_guard<std::mutex> lock(g_moneyMutex);
    const ULONGLONG now = GetTickCount64();
    for (const MoneyChange& m : g_money)
        if (now - m.at <= kMoneySeconds * 1000) return true;
    return false;
}
float tipHeight() { return g_tipHeight.load(); }

bool popupPending() {
    int state = g_popupState.load();
    return state == 1 || state == 2;
}

float popupHeight() { return g_popupState.load() == 2 ? g_popupHeight.load() : 0.0f; }

void setClock(int hour, int minute, int day, int speed) {
    g_hour = hour;
    g_minute = minute;
    g_day = day;
    g_speed = speed;
}

Action render(ID3D11Device* device, ID3D11DeviceContext* context, const Frame& f, int* index) {
    if (g_failed) return kNone;
    if (!g_imgui && !init(device, context)) {
        g_failed = true;
        return kNone;
    }
    if (device != g_device) return kNone;  // a new game device: not ours to draw on
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now, rate;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    float dt = static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(rate.QuadPart);
    g_last = now;
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 72;
    io.AddMousePosEvent(f.px >= 0 ? f.px : -FLT_MAX, f.px >= 0 ? f.py : -FLT_MAX);
    io.AddMouseButtonEvent(0, f.down && f.px >= 0);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    // One invisible window over the whole atlas holds the buttons.
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ++g_frameNo;
    uploadIcons(context);
    Item items[kKinds][kMaxItems];
    int counts[kKinds];
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        std::copy(&g_items[0][0], &g_items[0][0] + kKinds * kMaxItems, &items[0][0]);
        std::copy(g_counts, g_counts + kKinds, counts);
    }
    Action action = kNone;
    auto take = [&](Action a) {
        if (a != kNone) action = a;
    };
    if (f.watch) take(drawWatch(f));
    if (f.pause) drawPause();
    if (f.build) take(drawBuild());
    g_hoverKind = g_hoverIndex = -1;
    if (f.needs) drawNeeds(items[kNeedItems], counts[kNeedItems]);
    if (f.moodlets) drawMoodlets(items[kMoodletItems], counts[kMoodletItems]);
    if (f.queue) take(drawQueue(items[kQueueItems], items[kQueueBadges], counts[kQueueItems], index));
    if (f.wants) take(drawWants(items[kWantItems], counts[kWantItems], index));
    if (f.money) drawMoney();
    {
        Tip tip;
        bool have = false;
        if (g_hoverKind >= 0) {
            std::lock_guard<std::mutex> lock(g_itemMutex);
            const std::vector<Tip>& tips = g_tips[g_hoverKind];
            if (g_hoverIndex >= 0 && g_hoverIndex < static_cast<int>(tips.size())) tip = tips[g_hoverIndex], have = true;
        }
        bool drawn = have && drawTip(tip);
        g_tipKind = drawn ? g_hoverKind : -1;
        if (!drawn) g_tipHeight = 0;
    }
    resolvePopup();
    if (f.popup && g_popupOn && g_popup.resolved && g_popup.valid) take(drawPopup(index));
    resolvePicker();
    if (f.picker && g_pickerOn && g_picker.resolved && g_picker.valid) take(drawPicker(index));
    ImGui::End();
    ImGui::Render();
    // Into the atlas, cleared to nothing; the game's targets put back after.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(g_target, clear);
    context->OMSetRenderTargets(1, &g_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
    for (ID3D11RenderTargetView* t : targets)
        if (t) t->Release();
    if (depth) depth->Release();
    static int logged = 0;
    if (logged++ < 3) LOG("HUD: drawn (watch %d, pause %d)", f.watch ? 1 : 0, f.pause ? 1 : 0);
    return action;
}

ID3D11Texture2D* texture() { return g_imgui ? g_texture : nullptr; }

void release() {
    if (g_imgui) {
        ImGui::SetCurrentContext(g_imgui);
        if (g_device) ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_imgui);
        g_imgui = nullptr;
    }
    if (g_art) g_art->Release();
    g_art = nullptr;
    if (g_icons) g_icons->Release();
    if (g_iconTexture) g_iconTexture->Release();
    g_icons = nullptr;
    g_iconTexture = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_itemMutex);
        g_loadedIcons.clear();
    }
    for (IconSlot& slot : g_slots) slot = IconSlot{};
    if (g_target) g_target->Release();
    if (g_texture) g_texture->Release();
    if (g_device) g_device->Release();
    g_target = nullptr;
    g_texture = nullptr;
    g_device = nullptr;
}

}  // namespace s4vr::hud
