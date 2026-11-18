#include "system_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "imgui.h"
#include "psvrecore/log.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::frontend {

namespace {

using runtime::DialogAnswer;
using runtime::DialogView;
namespace dialogs = runtime::dialogs;

// SCE_CTRL_* bits (Cross is enter and Circle cancel, as the window swaps them).
constexpr u32 kSelect = 1u << 0, kStart = 1u << 3, kUp = 1u << 4, kDown = 1u << 6, kLeft = 1u << 7, kRight = 1u << 5,
              kL = 1u << 8, kR = 1u << 9, kTriangle = 1u << 12, kCircle = 1u << 13, kSquare = 1u << 15;
constexpr f32 kW = 960, kH = 544;
constexpr i32 kOk = 0, kCanceled = 1;

// The colours.
constexpr ImU32 kDim = IM_COL32(0, 0, 0, 150), kPanel = IM_COL32(242, 242, 242, 250), kInk = IM_COL32(30, 30, 30, 255),
                kMuted = IM_COL32(105, 105, 105, 255), kAccent = IM_COL32(46, 124, 222, 255),
                kAccentDark = IM_COL32(30, 98, 186, 255), kButton = IM_COL32(255, 255, 255, 255),
                kEdge = IM_COL32(196, 196, 196, 255), kDark = IM_COL32(28, 30, 34, 244), kDarkRow = IM_COL32(44, 47, 53, 255),
                kKey = IM_COL32(70, 74, 80, 255), kKeyText = IM_COL32(240, 240, 240, 255), kTrack = IM_COL32(208, 208, 208, 255),
                kWhite = IM_COL32(255, 255, 255, 255), kShadow = IM_COL32(0, 0, 0, 60);

ImFont* regular() { return ImGui::GetIO().Fonts->Fonts[0]; }
ImFont* bold() { return ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : regular(); }

// Text in lines no wider than `wrap` (and its own new lines).
nx::vector<std::pair<const char*, const char*>> lines(ImFont* font, f32 size, const char* text, f32 wrap) {
    nx::vector<std::pair<const char*, const char*>> out;
    const char* end = text + std::strlen(text);
    const char* s = text;
    while (s < end) {
        const char* nl = std::find(s, end, '\n');
        const char* e = font->CalcWordWrapPosition(size, s, nl, wrap);
        if (e == s) e = nl > s ? s + 1 : nl;
        out.emplace_back(s, e);
        s = e;
        while (s < end && *s == ' ') ++s;
        if (s < end && *s == '\n') ++s;
    }
    return out;
}

// Text centred (or from `x`) in lines; the height it took.
f32 text_block(ImDrawList* dl, ImFont* font, f32 size, const char* text, f32 x, f32 y, f32 w, ImU32 col, bool centre) {
    f32 at = y;
    for (const auto& [b, e] : lines(font, size, text, w)) {
        const f32 lw = font->CalcTextSizeA(size, FLT_MAX, 0, b, e).x;
        dl->AddText(font, size, ImVec2(centre ? x + (w - lw) * 0.5f : x, at), col, b, e);
        at += size * 1.3f;
    }
    return at - y;
}
f32 text_height(ImFont* font, f32 size, const char* text, f32 w) {
    return static_cast<f32>(lines(font, size, text, w).size()) * size * 1.3f;
}

// A button: a pill, white with a grey edge, blue when focused (or pressed); true when pressed.
bool pill(const char* id, const char* label, ImVec2 pos, ImVec2 size, bool dark = false) {
    ImGui::SetCursorScreenPos(pos);
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool focused = ImGui::IsItemFocused(), hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const f32 r = std::min(size.y * 0.5f, 14.0f);
    const ImU32 fill = focused ? kAccent : dark ? (hot ? IM_COL32(92, 97, 104, 255) : kKey) : hot ? IM_COL32(232, 240, 252, 255) : kButton;
    dl->AddRectFilled(pos, max, fill, r);
    if (!focused && !dark) dl->AddRect(pos, max, kEdge, r, 1.0f);
    if (focused) dl->AddRect(ImVec2(pos.x - 2, pos.y - 2), ImVec2(max.x + 2, max.y + 2), kAccentDark, r + 2, 2.0f);
    const f32 fs = 20.0f;
    const ImVec2 ts = regular()->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(regular(), fs, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                focused || dark ? kWhite : kInk, label);
    return pressed;
}

// The controller's symbols drawn as plain shapes (no pictures): cross, circle, square, triangle.
void symbol(ImDrawList* dl, char which, ImVec2 c, f32 r, ImU32 col) {
    switch (which) {
        case 'x':
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 2.0f);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 2.0f);
            break;
        case 'o': dl->AddCircle(c, r, col, 20, 2.0f); break;
        case 's': dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 0, 2.0f); break;
        case 't':
            dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f), ImVec2(c.x - r, c.y + r * 0.8f), col, 2.0f);
            break;
        default: break;
    }
}

// Hints along the bottom: "(symbol) what" pairs, right-aligned at `right`.
void hints(ImDrawList* dl, std::initializer_list<std::pair<char, const char*>> items, f32 right, f32 y, ImU32 col,
           bool circle_enter) {
    f32 x = right;
    nx::vector<std::pair<char, const char*>> v(items.begin(), items.end());
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        char s = it->first;
        if (circle_enter && (s == 'x' || s == 'o')) s = s == 'x' ? 'o' : 'x';  // (enter is Circle there)
        const f32 tw = regular()->CalcTextSizeA(17.0f, FLT_MAX, 0, it->second).x;
        x -= tw;
        dl->AddText(regular(), 17.0f, ImVec2(x, y), col, it->second);
        x -= 20;
        symbol(dl, s, ImVec2(x + 7, y + 10), 6.0f, col);
        x -= 22;
    }
}

void spinner(ImDrawList* dl, ImVec2 c, f32 r, ImU32 col) {
    const f32 t = static_cast<f32>(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(c, r, t, t + 4.4f, 24);
    dl->PathStroke(col, 3.0f);
}

// A save data slot as a card: a tile, its title, subtitle and date (or the empty slot's title).
void slot_card(ImDrawList* dl, const runtime::DialogSlot& s, ImVec2 pos, f32 w, bool focused, bool dark) {
    const f32 h = 76;
    const ImU32 ink = dark ? kWhite : kInk, muted = dark ? IM_COL32(170, 174, 180, 255) : kMuted;
    if (focused) dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), dark ? kAccent : IM_COL32(220, 233, 250, 255), 8);
    // (the tile: the save's own icon is not read; a plain tile with the slot's number)
    const ImVec2 t0(pos.x + 10, pos.y + 8), t1(pos.x + 10 + 108, pos.y + h - 8);
    dl->AddRectFilled(t0, t1, s.exists ? IM_COL32(70, 104, 150, 255) : IM_COL32(150, 150, 150, 255), 6);
    const nx::string number = s.exists ? nx::format("{}", s.id) : nx::string("+");
    const ImVec2 ns = bold()->CalcTextSizeA(26.0f, FLT_MAX, 0, number.c_str());
    dl->AddText(bold(), 26.0f, ImVec2((t0.x + t1.x - ns.x) * 0.5f, (t0.y + t1.y - ns.y) * 0.5f), kWhite, number.c_str());
    const f32 x = t1.x + 14, tw = w - (x - pos.x) - 10;
    const ImVec4 clip(x, pos.y, x + tw, pos.y + h);
    dl->AddText(bold(), 20.0f, ImVec2(x, pos.y + 8), ink, s.title.c_str(), nullptr, 0, &clip);
    if (s.exists) {
        dl->AddText(regular(), 17.0f, ImVec2(x, pos.y + 32), muted, s.subtitle.c_str(), nullptr, 0, &clip);
        const nx::string date = s.broken ? nx::string("(damaged)") : s.date;
        dl->AddText(regular(), 16.0f, ImVec2(x, pos.y + 53), muted, date.c_str(), nullptr, 0, &clip);
    }
}

// A full-screen window of the screens (no background of its own), focused when asked.
bool begin_screen(const char* name, bool focus) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(kW, kH));
    if (focus) ImGui::SetNextWindowFocus();
    return ImGui::Begin(name, nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
}

// UTF-8 and code points.
nx::vector<u32> decode(const nx::string& s) {
    nx::vector<u32> out;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = c, n = 1;
        if (c >= 0xF0) cp = c & 0x07u, n = 4;
        else if (c >= 0xE0) cp = c & 0x0Fu, n = 3;
        else if (c >= 0xC0) cp = c & 0x1Fu, n = 2;
        for (u32 k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<u8>(s[i + k]) & 0x3Fu);
        out.push_back(cp);
        i += n;
    }
    return out;
}
void encode(u32 cp, nx::string& out) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) out.push_back(static_cast<char>(0xC0 | cp >> 6)), out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    else if (cp < 0x10000)
        out.push_back(static_cast<char>(0xE0 | cp >> 12)), out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))),
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    else
        out.push_back(static_cast<char>(0xF0 | cp >> 18)), out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))),
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))), out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}
nx::string encode(const nx::vector<u32>& cps, usize from = 0, usize to = ~usize{0}) {
    nx::string out;
    for (usize i = from; i < std::min(to, cps.size()); ++i) encode(cps[i], out);
    return out;
}
u32 utf16_length(const nx::vector<u32>& cps) {
    u32 n = 0;
    for (u32 c : cps) n += c >= 0x10000 ? 2 : 1;
    return n;
}

// "render_scale" as "Render scale".
nx::string label_of(nx::string_view key) {
    nx::string out(key);
    for (char& ch : out)
        if (ch == '_') ch = ' ';
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}
nx::string section_of(nx::string_view section) { return label_of(section); }

}  // namespace

SystemScreens::SystemScreens(Options options) : options_(std::move(options)) {}

bool SystemScreens::visible() const {
    return home_.load() || notice_active_.load() || dialogs::showing() != DialogView::Kind::None || dialogs::has_notice();
}

bool SystemScreens::takes_controls() const { return home_.load() || dialogs::showing() != DialogView::Kind::None; }

bool SystemScreens::takes_text() const {
    if (!options_.desktop || home_.load()) return false;
    const DialogView::Kind k = dialogs::showing();
    return k == DialogView::Kind::Ime || k == DialogView::Kind::InGameIme;
}

void SystemScreens::answer(const DialogView& v, i32 result, i32 button, u32 slot) {
    DialogAnswer a;
    a.result = result, a.button = button, a.slot = slot;
    if (v.kind == DialogView::Kind::Ime) a.text = encode(text_);
    dialogs::answer(v.serial, a);
}

void SystemScreens::build(const Controls& c) {
    if (!styled_) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(0, 0);
        style.WindowBorderSize = 0;
        style.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // (the screens draw their own focus)
        style.FontSizeBase = 20.0f;
        styled_ = true;
    }
    const bool home = home_.load();
    if (home && !home_was_) page_ = 0;
    if (!home && home_was_) focus_dialog_ = true;
    DialogView v;
    if (dialogs::current(&v)) {
        enter_circle_.store(v.enter_circle);
        draw_dialog(v, home ? Controls{} : c, !home);
    }
    draw_notice();
    if (home) draw_home(c);
    home_was_ = home;
}

// ---- dialogs

void SystemScreens::draw_dialog(const DialogView& v, const Controls& c, bool focus) {
    const bool fresh = v.serial != serial_;
    fresh_ = fresh || focus_dialog_;
    if (fresh || (v.kind == DialogView::Kind::InGameIme && v.revision != revision_)) {
        // (new, or the in-game keyboard's text set by the game itself)
        if (fresh) since_ = ImGui::GetTime(), shift_ = symbols_ = false;
        serial_ = v.serial;
        revision_ = v.revision;
        text_ = decode(v.ime_text);
        caret_ = text_.size();
        if (v.kind == DialogView::Kind::InGameIme) {
            // (its caret in UTF-16 units)
            u32 units = 0;
            for (usize i = 0; i < text_.size(); ++i) {
                if (units >= v.caret) {
                    caret_ = i;
                    break;
                }
                units += text_[i] >= 0x10000 ? 2 : 1;
            }
        }
    }
    // (the window is new for each dialog: it takes the focus, its default button with it)
    const nx::string name = nx::format("##dialog{}", v.serial);
    begin_screen(name.c_str(), (fresh || focus_dialog_) && focus);
    if (focus) focus_dialog_ = false;
    // (the picture dimmed, but under the in-game keyboard, whose game draws its own text field)
    if (v.kind != DialogView::Kind::InGameIme) ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(kW, kH), kDim);
    switch (v.kind) {
        case DialogView::Kind::SaveData:
            if (v.mode == 2) draw_list(v, c);
            else draw_message(v, c);
            break;
        case DialogView::Kind::Ime:
        case DialogView::Kind::InGameIme: draw_ime(v, c); break;
        case DialogView::Kind::TrophySetup:
            draw_message(v, c);
            if (ImGui::GetTime() - since_ > 0.8) answer(v, kOk, 0);  // (trophies are local: ready at once)
            break;
        default: draw_message(v, c); break;
    }
    ImGui::End();
}

void SystemScreens::draw_message(const DialogView& v, const Controls& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 w = 640, pad = 30, inner = w - 2 * pad;
    const bool waiting = v.buttons == DialogView::Buttons::None;
    const bool card = v.kind == DialogView::Kind::SaveData && !v.slots.empty();
    const f32 fs = 21.0f;
    f32 h = pad + text_height(regular(), fs, v.text.c_str(), inner) + pad;
    if (card) h += 76 + 18;
    if (v.progress) h += 46;
    else if (waiting) h += 40;
    if (!waiting) h += 46 + 8;
    const ImVec2 p0((kW - w) * 0.5f, std::max(20.0f, (kH - h) * 0.5f)), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 5), ImVec2(p1.x + 3, p1.y + 5), kShadow, 14);
    dl->AddRectFilled(p0, p1, kPanel, 14);
    f32 y = p0.y + pad;
    if (card) {
        slot_card(dl, v.slots.front(), ImVec2(p0.x + pad, y), inner, false, false);
        y += 76 + 18;
    }
    y += text_block(dl, regular(), fs, v.text.c_str(), p0.x + pad, y, inner, kInk, true);
    if (v.progress) {
        y += 12;
        const f32 bw = inner - 60;
        const ImVec2 b0(p0.x + pad, y + 6), b1(b0.x + bw, y + 18);
        dl->AddRectFilled(b0, b1, kTrack, 6);
        if (v.percent) dl->AddRectFilled(b0, ImVec2(b0.x + bw * static_cast<f32>(v.percent) / 100.0f, b1.y), kAccent, 6);
        const nx::string pct = nx::format("{}%", v.percent);
        dl->AddText(regular(), 18.0f, ImVec2(b1.x + 12, y + 1), kMuted, pct.c_str());
        if (!v.progress_text.empty()) dl->AddText(regular(), 16.0f, ImVec2(b0.x, y + 22), kMuted, v.progress_text.c_str());
        y += 34;
    } else if (waiting) {
        spinner(dl, ImVec2(p0.x + w * 0.5f, y + 18), 11.0f, kAccent);
        y += 40;
    }
    if (waiting) {
        // (nothing to press: the game closes it; a dummy focus keeps ImGui's focus here)
        return;
    }
    // The buttons, side by side and centred: their labels, and what each answers (result, button).
    struct B {
        nx::string label;
        i32 result, button;
    };
    nx::vector<B> bs;
    switch (v.buttons) {
        case DialogView::Buttons::YesNo: bs = {{"Yes", kOk, 1}, {"No", kOk, 2}}; break;
        case DialogView::Buttons::OkCancel: bs = {{"OK", kOk, 1}, {"Cancel", kCanceled, 0}}; break;
        case DialogView::Buttons::Cancel: bs = {{"Cancel", kCanceled, 0}}; break;
        case DialogView::Buttons::Three:
            for (i32 i = 0; i < 3; ++i)
                if (!v.labels[i].empty()) bs.push_back({v.labels[i], kOk, i + 1});
            if (bs.empty()) bs = {{"OK", kOk, 1}};
            break;
        default: bs = {{"OK", kOk, 1}}; break;
    }
    const f32 bw = bs.size() > 2 ? 176 : 200, gap = 18;
    const f32 total = bw * static_cast<f32>(bs.size()) + gap * static_cast<f32>(bs.size() - 1);
    f32 x = p0.x + (w - total) * 0.5f;
    y += 8;
    for (usize i = 0; i < bs.size(); ++i) {
        const nx::string id = nx::format("##b{}", i);
        if (pill(id.c_str(), bs[i].label.c_str(), ImVec2(x, y), ImVec2(bw, 44))) answer(v, bs[i].result, bs[i].button);
        if (i == 0 && fresh_) ImGui::SetItemDefaultFocus();
        x += bw + gap;
    }
    // Cancel: No for a question, the cancel button where there is one, else OK.
    if (c.pressed & kCircle) {
        switch (v.buttons) {
            case DialogView::Buttons::YesNo: answer(v, kOk, 2); break;
            case DialogView::Buttons::OkCancel:
            case DialogView::Buttons::Cancel:
            case DialogView::Buttons::Three: answer(v, kCanceled, 0); break;
            default: answer(v, kOk, 1); break;
        }
    }
}

void SystemScreens::draw_list(const DialogView& v, const Controls& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0(70, 26), p1(890, 518);
    dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 5), ImVec2(p1.x + 3, p1.y + 5), kShadow, 14);
    dl->AddRectFilled(p0, p1, kPanel, 14);
    const char* what = v.type == 2 ? "Load" : v.type == 3 ? "Delete" : "Save";
    const nx::string title = v.title.empty() ? nx::string("Saved Data") : v.title;
    dl->AddText(bold(), 23.0f, ImVec2(p0.x + 26, p0.y + 18), kInk, title.c_str());
    dl->AddLine(ImVec2(p0.x + 16, p0.y + 56), ImVec2(p1.x - 16, p0.y + 56), kEdge, 1.0f);
    hints(dl, {{'x', what}, {'o', "Back"}}, p1.x - 24, p1.y - 34, kMuted, v.enter_circle);
    // (the slots in a scrolling list: the focus scrolls it)
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 16, p0.y + 64));
    ImGui::BeginChild("##slots", ImVec2(p1.x - p0.x - 32, p1.y - p0.y - 64 - 46), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* ld = ImGui::GetWindowDrawList();
    const f32 w = ImGui::GetContentRegionAvail().x;
    for (usize i = 0; i < v.slots.size(); ++i) {
        const nx::string id = nx::format("##slot{}", i);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton(id.c_str(), ImVec2(w, 76), ImGuiButtonFlags_EnableNav)) answer(v, kOk, 1, v.slots[i].id);
        if (i == v.focus && fresh_) ImGui::SetItemDefaultFocus();
        slot_card(ld, v.slots[i], at, w, ImGui::IsItemFocused() || ImGui::IsItemHovered(), false);
        ImGui::Dummy(ImVec2(w, 4));
    }
    if (v.slots.empty()) {
        ImGui::Dummy(ImVec2(w, 30));
        ld->AddText(regular(), 20.0f, ImVec2(p0.x + 30, p0.y + 90), kMuted, "There is no saved data.");
    }
    ImGui::EndChild();
    if (c.pressed & kCircle) answer(v, kCanceled, 0);
}

// The keyboard's pages: four rows of ten keys.
constexpr const char* kLetters[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
constexpr const char* kSymbols[4] = {"1234567890", "!@#$%^&*()", "-_=+[]{};:", "\"'/\\<>|~`,"};
constexpr const char* kWeb[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm@./"};

void SystemScreens::type(u32 cp, const DialogView& v) {
    // (what the type allows: basic Latin only printable ASCII, numbers their digits (and . - + , for extended))
    if (v.ime_type == 1 && (cp < 0x20 || cp > 0x7E)) return;
    if (v.ime_type == 2 && !(cp >= '0' && cp <= '9')) return;
    if (v.ime_type == 3 && !((cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+' || cp == ',')) return;
    if (cp == '\n' && !(v.ime_option & 1)) return;
    if (utf16_length(text_) + (cp >= 0x10000 ? 2 : 1) > v.max_length) return;
    text_.insert(text_.begin() + static_cast<isize>(caret_), cp);
    ++caret_;
}

void SystemScreens::draw_ime(const DialogView& v, const Controls& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool multiline = (v.ime_option & 1) != 0;
    const bool numbers = v.ime_type == 2 || v.ime_type == 3;
    const bool in_game = v.kind == DialogView::Kind::InGameIme;
    const nx::vector<u32> before = text_;
    const usize caret_before = caret_;
    // The title and the text box (the in-game keyboard's game draws its own).
    if (!in_game) {
        dl->AddText(bold(), 21.0f, ImVec2(40, 18), kWhite, v.title.c_str());
        const ImVec2 b0(36, 52), b1(924, multiline ? 262.0f : 110.0f);
        dl->AddRectFilled(b0, b1, kButton, 10);
        dl->AddRect(b0, b1, kAccent, 10, 2.0f);
        const f32 fs = 24.0f, x0 = b0.x + 16, wmax = b1.x - b0.x - 32;
        f32 x = x0, y = b0.y + (multiline ? 12.0f : (b1.y - b0.y - fs) * 0.5f);
        f32 cx = x, cy = y;
        dl->PushClipRect(b0, b1, true);
        for (usize i = 0; i <= text_.size(); ++i) {
            if (i == caret_) cx = x, cy = y;
            if (i == text_.size()) break;
            const u32 cp = v.password ? 0x2022 : text_[i];
            nx::string one;
            encode(cp, one);
            const f32 cw = regular()->CalcTextSizeA(fs, FLT_MAX, 0, one.c_str()).x;
            if (text_[i] == '\n' || (multiline && x + cw > x0 + wmax)) {
                x = x0, y += fs * 1.25f;
                if (text_[i] == '\n') continue;
            }
            dl->AddText(regular(), fs, ImVec2(x, y), kInk, one.c_str());
            x += cw;
        }
        if (std::fmod(ImGui::GetTime() - since_, 1.0) < 0.6) dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + 2, cy + fs), kAccent);
        dl->PopClipRect();
        const nx::string count = nx::format("{}/{}", utf16_length(text_), v.max_length);
        const f32 tw = regular()->CalcTextSizeA(15.0f, FLT_MAX, 0, count.c_str()).x;
        dl->AddText(regular(), 15.0f, ImVec2(b1.x - tw, b1.y + 4), IM_COL32(220, 220, 220, 255), count.c_str());
    }
    // The keyboard, in the lower half (the in-game one where SceIme says it is: from 272).
    const f32 top = in_game ? 272 : 284;
    dl->AddRectFilled(ImVec2(0, top), ImVec2(kW, kH), kDark);
    bool enter = false, close = false;
    auto key = [&](const char* id, const char* label, f32 x, f32 y, f32 w, f32 h) {
        return pill(id, label, ImVec2(x, y), ImVec2(w, h), true);
    };
    const f32 kw = 74, kh = 44, gap = 6, left = 16, row0 = top + 12;
    if (numbers) {
        // (a keypad: digits, and for extended numbers . - +)
        static constexpr const char* kPad[4] = {"123", "456", "789", ".0-"};
        const f32 pw = 110, px = (kW - 3 * pw - 2 * gap) * 0.5f - 120;
        for (i32 r = 0; r < 4; ++r) {
            for (i32 k = 0; k < 3; ++k) {
                const char ch = kPad[r][k];
                if (v.ime_type == 2 && (ch == '.' || ch == '-')) continue;
                const char label[2] = {ch, 0};
                const nx::string id = nx::format("##k{}{}", r, k);
                if (key(id.c_str(), label, px + static_cast<f32>(k) * (pw + gap), row0 + static_cast<f32>(r) * (kh + gap), pw, kh))
                    type(static_cast<u32>(ch), v);
                if (r == 0 && k == 0 && fresh_) ImGui::SetItemDefaultFocus();
            }
        }
    } else {
        const char* const* page = symbols_ ? kSymbols : (v.ime_type == 4 || v.ime_type == 5) ? kWeb : kLetters;
        for (i32 r = 0; r < 4; ++r) {
            for (i32 k = 0; k < 10; ++k) {
                char ch = page[r][k];
                if (!symbols_ && shift_ && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                const char label[2] = {ch, 0};
                const nx::string id = nx::format("##k{}{}", r, k);
                if (key(id.c_str(), label, left + static_cast<f32>(k) * (kw + gap), row0 + static_cast<f32>(r) * (kh + gap), kw, kh)) {
                    type(static_cast<u32>(static_cast<u8>(ch)), v);
                    if (shift_ && !symbols_) shift_ = false;
                }
                if (r == 1 && k == 0 && fresh_) ImGui::SetItemDefaultFocus();
            }
        }
        // (the bottom row: shift, the symbols page, space)
        const f32 y4 = row0 + 4 * (kh + gap);
        if (key("##shift", shift_ ? "SHIFT" : "Shift", left, y4, 2 * kw + gap, kh)) shift_ = !shift_;
        if (key("##page", symbols_ ? "ABC" : "#+=", left + 2 * (kw + gap), y4, 2 * kw + gap, kh)) symbols_ = !symbols_;
        if (key("##space", "Space", left + 4 * (kw + gap), y4, 4 * kw + 3 * gap, kh)) type(' ', v);
    }
    // (the caret's keys, delete, enter and close: on the right)
    const f32 rx = 822, rw = 122;
    const f32 ly = row0 + 4 * (kh + gap);
    if (key("##left", "<", numbers ? rx - 2 * (kw + gap) : left + 8 * (kw + gap), ly, kw, kh) && caret_ > 0) --caret_;
    if (key("##right", ">", numbers ? rx - (kw + gap) : left + 9 * (kw + gap), ly, kw, kh) && caret_ < text_.size()) ++caret_;
    if (key("##delete", "Delete", rx, row0, rw, kh) && caret_ > 0) text_.erase(text_.begin() + static_cast<isize>(--caret_));
    if (multiline && key("##newline", "New line", rx, row0 + (kh + gap), rw, kh)) type('\n', v);
    static constexpr const char* kEnter[] = {"Enter", "Send", "Search", "Go"};
    if (key("##enter", kEnter[std::min<u32>(v.enter_label, 3)], rx, row0 + (multiline ? 2 : 1) * (kh + gap), rw,
            multiline ? kh : 2 * kh + gap))
        enter = true;
    if (key("##close", v.with_cancel ? "Cancel" : "Close", rx, row0 + 3 * (kh + gap), rw, kh)) close = true;
    // The controller's shortcuts, and a desktop's keyboard.
    if (c.pressed & kSquare && caret_ > 0) text_.erase(text_.begin() + static_cast<isize>(--caret_));
    if (c.pressed & kTriangle && !numbers) type(' ', v);
    if (c.pressed & kL && caret_ > 0) --caret_;
    if (c.pressed & kR && caret_ < text_.size()) ++caret_;
    if (c.pressed & kSelect && !numbers) shift_ = !shift_;
    if (c.pressed & kStart) enter = true;
    if (c.pressed & kCircle) close = true;
    const ImGuiIO& io = ImGui::GetIO();
    for (ImWchar ch : io.InputQueueCharacters)
        if (ch >= 0x20 && ch != 0x7F) type(static_cast<u32>(ch), v);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && caret_ > 0) text_.erase(text_.begin() + static_cast<isize>(--caret_));
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && caret_ < text_.size()) text_.erase(text_.begin() + static_cast<isize>(caret_));
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && caret_ > 0) --caret_;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && caret_ < text_.size()) ++caret_;
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) caret_ = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End)) caret_ = text_.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (multiline && !io.KeyCtrl) type('\n', v);
        else enter = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (in_game && (text_ != before || caret_ != caret_before)) {
        u32 units = 0;
        for (usize i = 0; i < caret_ && i < text_.size(); ++i) units += text_[i] >= 0x10000 ? 2 : 1;
        dialogs::edit(v.serial, encode(text_), units);
    }
    if (enter) answer(v, kOk, 2);
    else if (close) answer(v, v.with_cancel ? kCanceled : kOk, v.with_cancel ? 0 : 1);
}

// ---- notices

void SystemScreens::draw_notice() {
    const f64 now = ImGui::GetTime();
    if (!notice_ && dialogs::take_notice(&shown_)) notice_ = true, notice_at_ = now;
    if (!notice_) {
        notice_active_.store(false);
        return;
    }
    notice_active_.store(true);
    const f64 t = now - notice_at_, in = 0.25, hold = 4.0, out = 0.3;
    if (t > in + hold + out) {
        notice_ = false;
        notice_active_.store(dialogs::has_notice());
        return;
    }
    const f32 a = static_cast<f32>(t < in ? t / in : t > in + hold ? 1.0 - (t - in - hold) / out : 1.0);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const f32 w = 330, h = 66, x = kW - w - 16, y = 14 - (1.0f - a) * 20.0f;
    const auto alpha = [&](ImU32 col) {
        return (col & 0x00FFFFFFu) | (static_cast<u32>(static_cast<f32>(col >> 24) * a) << 24);
    };
    dl->AddRectFilled(ImVec2(x + 2, y + 4), ImVec2(x + w + 2, y + h + 4), alpha(kShadow), 12);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), alpha(kDark), 12);
    // (a cup, drawn: a bowl, its stem and base)
    const f32 cx = x + 34, cy = y + h * 0.5f;
    const ImU32 gold = alpha(IM_COL32(214, 178, 82, 255));
    dl->AddRectFilled(ImVec2(cx - 12, cy - 16), ImVec2(cx + 12, cy - 2), gold, 8, ImDrawFlags_RoundBottom);
    dl->AddRectFilled(ImVec2(cx - 2, cy - 2), ImVec2(cx + 2, cy + 8), gold);
    dl->AddRectFilled(ImVec2(cx - 9, cy + 8), ImVec2(cx + 9, cy + 13), gold, 2);
    dl->AddText(bold(), 19.0f, ImVec2(x + 64, y + 10), alpha(kWhite), shown_.title.c_str());
    dl->AddText(regular(), 17.0f, ImVec2(x + 64, y + 36), alpha(IM_COL32(200, 204, 210, 255)), shown_.text.c_str());
}

// ---- the Home menu

void SystemScreens::draw_home(const Controls& c) {
    const bool fresh = !home_was_;
    if (fresh) focus_page_ = 0;
    begin_screen("##home", fresh || focus_page_ >= 0);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(kW, kH), IM_COL32(0, 0, 0, 170));
    const f32 mw = 300;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(mw, kH), kDark);
    text_block(dl, bold(), 22.0f, options_.title.c_str(), 24, 26, mw - 48, kWhite, false);
    dl->AddText(regular(), 16.0f, ImVec2(24, kH - 34), IM_COL32(150, 154, 160, 255), "Paused");
    struct Item {
        const char* label;
        i32 action;
    };
    const Item items[] = {{"Resume", 0}, {"Settings", 1}, {"Quit game", 2}};
    f32 y = 120;
    i32 next_focus = -1;  // (the page whose first item takes the focus next frame)
    for (const Item& item : items) {
        if (item.action == 1 && options_.settings_file.empty()) continue;
        const nx::string id = nx::format("##home{}", item.action);
        ImGui::SetCursorScreenPos(ImVec2(12, y));
        // (while a page is open on the right the menu keeps its place but not the focus)
        if (focus_page_ == 0 && item.action == menu_focus_) ImGui::SetKeyboardFocusHere();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, page_ != 0);
        const bool pressed = ImGui::InvisibleButton(id.c_str(), ImVec2(mw - 24, 50), ImGuiButtonFlags_EnableNav);
        ImGui::PopItemFlag();
        const bool on = ImGui::IsItemFocused() || ImGui::IsItemHovered() || (page_ == item.action && page_ != 0);
        if (on) dl->AddRectFilled(ImVec2(12, y), ImVec2(mw - 12, y + 50), ImGui::IsItemFocused() ? kAccent : kDarkRow, 10);
        dl->AddText(regular(), 21.0f, ImVec2(30, y + 13), kWhite, item.label);
        if (pressed) {
            menu_focus_ = item.action;
            if (item.action == 0) home_.store(false);
            else if (item.action == 1) {
                page_ = 1, setting_message_.clear();
                Settings fresh_settings;
                if (auto text = nx::fs::file_read_text(nx::fs::path_view(options_.settings_file)))
                    (void)apply_file_text(&fresh_settings, text->view());
                settings_ = describe(fresh_settings);
                next_focus = 1;
            } else {
                page_ = 2;
                next_focus = 2;
            }
        }
        y += 58;
    }
    if (focus_page_ == 0) focus_page_ = -1;
    if (page_ == 1) draw_settings(c, mw + 20, 20, kW - mw - 40, kH - 40);
    if (page_ == 2) {
        // (are you sure)
        const f32 w = 420, h = 170, x = mw + (kW - mw - w) * 0.5f, py = (kH - h) * 0.5f;
        dl->AddRectFilled(ImVec2(x, py), ImVec2(x + w, py + h), kPanel, 14);
        text_block(dl, regular(), 21.0f, "Quit the game?\nWhat has not been saved is lost.", x + 20, py + 24, w - 40, kInk, true);
        if (focus_page_ == 2) ImGui::SetKeyboardFocusHere(), focus_page_ = -1;
        if (pill("##quit_no", "Cancel", ImVec2(x + 30, py + 106), ImVec2(170, 44))) page_ = 0, next_focus = 0;
        if (pill("##quit_yes", "Quit", ImVec2(x + w - 200, py + 106), ImVec2(170, 44))) quit_.store(true);
    }
    if (c.pressed & kCircle) {
        if (page_ != 0) page_ = 0, next_focus = 0;
        else home_.store(false);
    }
    if (next_focus >= 0) focus_page_ = next_focus;
    if (page_ == 0) hints(dl, {{'x', "Select"}, {'o', "Resume"}}, kW - 24, kH - 34, IM_COL32(200, 204, 210, 255), enter_circle_.load());
    ImGui::End();
}

void SystemScreens::draw_settings(const Controls& c, f32 x, f32 y, f32 w, f32 h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kDark, 14);
    dl->AddText(bold(), 22.0f, ImVec2(x + 20, y + 14), kWhite, "Settings");
    ImGui::SetCursorScreenPos(ImVec2(x + 10, y + 50));
    ImGui::BeginChild("##settings", ImVec2(w - 20, h - 50 - 70), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    ImDrawList* ld = ImGui::GetWindowDrawList();
    const f32 rw = ImGui::GetContentRegionAvail().x;
    nx::string_view section;
    i32 focused = -1;
    for (i32 i = 0; i < static_cast<i32>(settings_.size()); ++i) {
        SettingInfo& s = settings_[i];
        if (!options_.desktop && s.desktop_only) continue;
        if (s.kind == SettingInfo::Kind::Folder) continue;  // (folders: the settings file or the command line)
        if (s.section.view() != section) {
            section = s.section.view();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rw, 30));
            ld->AddText(bold(), 17.0f, ImVec2(at.x + 8, at.y + 8), IM_COL32(140, 170, 220, 255), section_of(section).c_str());
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const nx::string id = nx::format("##set{}", i);
        if (focus_page_ == 1) ImGui::SetKeyboardFocusHere(), focus_page_ = -1;
        ImGui::InvisibleButton(id.c_str(), ImVec2(rw, 40), ImGuiButtonFlags_EnableNav);
        const bool on = ImGui::IsItemFocused();
        if (on) focused = i;
        if (on || ImGui::IsItemHovered()) ld->AddRectFilled(at, ImVec2(at.x + rw, at.y + 40), on ? kAccent : kDarkRow, 8);
        ld->AddText(regular(), 19.0f, ImVec2(at.x + 12, at.y + 9), kWhite, label_of(s.key.view()).c_str());
        nx::string shown = s.value;
        if (s.kind == SettingInfo::Kind::Bool) shown = s.value == "true" ? "On" : "Off";
        const bool editable = s.kind != SettingInfo::Kind::Text;
        const nx::string value = editable ? nx::format("<  {}  >", shown) : shown;
        const f32 vw = regular()->CalcTextSizeA(19.0f, FLT_MAX, 0, value.c_str()).x;
        ld->AddText(regular(), 19.0f, ImVec2(at.x + rw - vw - 14, at.y + 9), editable ? kWhite : IM_COL32(170, 174, 180, 255),
                    value.c_str());
        // (changed: left and right on the focused row, a click on the value)
        i32 step = 0;
        if (on && (c.pressed & kLeft)) step = -1;
        if (on && (c.pressed & kRight)) step = 1;
        if (ImGui::IsItemClicked()) step = ImGui::GetIO().MousePos.x < at.x + rw - vw * 0.5f - 14 ? -1 : 1;
        if (!editable || !step) continue;
        nx::string next = s.value;
        switch (s.kind) {
            case SettingInfo::Kind::Bool: next = s.value == "true" ? "false" : "true"; break;
            case SettingInfo::Kind::Choice: {
                const auto it = std::find(s.choices.begin(), s.choices.end(), s.value);
                const i32 n = static_cast<i32>(s.choices.size());
                const i32 at_index = it == s.choices.end() ? 0 : static_cast<i32>(it - s.choices.begin());
                if (n) next = s.choices[static_cast<usize>(((at_index + step) % n + n) % n)];
                break;
            }
            case SettingInfo::Kind::Int: {
                const i32 cur = std::atoi(s.value.c_str());
                const i32 d = s.hi - s.lo > 100 ? 10 : 1;
                next = nx::format("{}", std::clamp(cur + step * d, static_cast<i32>(s.lo), static_cast<i32>(s.hi)));
                break;
            }
            case SettingInfo::Kind::Float: {
                const f32 cur = static_cast<f32>(std::atof(s.value.c_str()));
                const f32 v = std::clamp(std::round((cur + static_cast<f32>(step) * 0.1f) * 10.0f) / 10.0f, s.lo, s.hi);
                next = nx::format("{}", v);
                break;
            }
            default: break;
        }
        if (next == s.value) continue;
        if (auto r = set_in_file(options_.settings_file, s.section.view(), s.key.view(), next.view()); !r) {
            setting_message_ = r.error().message;
        } else {
            s.value = next;
            setting_message_ = s.live ? nx::string() : nx::string("This applies when the game is started again.");
            if (options_.settings_changed) options_.settings_changed();
        }
    }
    ImGui::EndChild();
    if (focused >= 0) {
        setting_ = focused;
        const SettingInfo& s = settings_[static_cast<usize>(focused)];
        const nx::string note = setting_message_.empty() ? s.help : setting_message_;
        text_block(dl, regular(), 16.0f, note.c_str(), x + 20, y + h - 64, w - 40, IM_COL32(190, 194, 200, 255), false);
    }
    if (c.pressed & (kUp | kDown)) setting_message_.clear();
}

}  // namespace psvrecore::frontend
