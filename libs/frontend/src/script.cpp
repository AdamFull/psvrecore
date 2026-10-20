#include "psvrecore/frontend/script.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <optional>

#include "psvrecore/log.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::frontend {

namespace {

// The words of an item.
nx::vector<nx::string_view> words(nx::string_view item) {
    nx::vector<nx::string_view> out;
    usize i = 0;
    while (i < item.size()) {
        while (i < item.size() && (item[i] == ' ' || item[i] == '\t' || item[i] == '\r')) ++i;
        const usize start = i;
        while (i < item.size() && item[i] != ' ' && item[i] != '\t' && item[i] != '\r') ++i;
        if (i > start) out.push_back(item.substr(start, i - start));
    }
    return out;
}

template <typename T>
bool number(nx::string_view v, T* out) {
    const auto [end, error] = std::from_chars(v.data(), v.data() + v.size(), *out);
    return error == std::errc{} && end == v.data() + v.size();
}

// A gamepad button by the console's name for it, else by SDL's.
i32 pad_button(const nx::string& name) {
    static constexpr struct {
        const char* name;
        SDL_GamepadButton button;
    } kNames[] = {{"cross", SDL_GAMEPAD_BUTTON_SOUTH},        {"circle", SDL_GAMEPAD_BUTTON_EAST},
                  {"square", SDL_GAMEPAD_BUTTON_WEST},        {"triangle", SDL_GAMEPAD_BUTTON_NORTH},
                  {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},         {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                  {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
                  {"l", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},    {"r", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                  {"start", SDL_GAMEPAD_BUTTON_START},        {"select", SDL_GAMEPAD_BUTTON_BACK},
                  {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},      {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK}};
    for (const auto& n : kNames)
        if (name == n.name) return n.button;
    return SDL_GetGamepadButtonFromString(name.c_str());
}

bool up_down(nx::string_view v, bool* down) {
    if (v == "down") return *down = true, true;
    if (v == "up") return *down = false, true;
    return false;
}

Result<ScriptItem> parse_item(const nx::vector<nx::string_view>& w, nx::string_view text) {
    auto bad = [&](const char* why) { return failure(nx::format("input '{}': {}", text, why)); };
    ScriptItem item;
    if (w.size() < 2 || !number(w[0], &item.at_ms)) return bad("expected <ms> <what> ...");
    const nx::string_view what = w[1];
    const nx::string a = w.size() > 2 ? nx::string(w[2]) : nx::string();
    if (what == "key") {
        item.what = ScriptItem::What::Key;
        item.code = static_cast<i32>(SDL_GetKeyFromName(a.c_str()));
        if (w.size() != 4 || item.code == SDLK_UNKNOWN || !up_down(w[3], &item.down))
            return bad("expected key <SDL key name> down|up");
    } else if (what == "pad") {
        item.what = ScriptItem::What::Pad;
        item.code = pad_button(a);
        if (w.size() != 4 || item.code == SDL_GAMEPAD_BUTTON_INVALID || !up_down(w[3], &item.down))
            return bad("expected pad <button> down|up (cross, circle, ..., or SDL's name)");
    } else if (what == "axis") {
        item.what = ScriptItem::What::Axis;
        item.code = SDL_GetGamepadAxisFromString(a.c_str());
        if (w.size() != 4 || item.code == SDL_GAMEPAD_AXIS_INVALID || !number(w[3], &item.value) ||
            item.value < -32768 || item.value > 32767)
            return bad("expected axis <SDL gamepad axis> <-32768 to 32767>");
    } else if (what == "touch") {
        item.what = ScriptItem::What::Touch;
        if (w.size() != 5 || !number(w[2], &item.x) || !number(w[3], &item.y))
            return bad("expected touch <x> <y> down|move|up");
        if (w[4] == "down") item.phase = 0;
        else if (w[4] == "move") item.phase = 1;
        else if (w[4] == "up") item.phase = 2;
        else return bad("expected touch <x> <y> down|move|up");
    } else if (what == "shot") {
        item.what = ScriptItem::What::Shot;
        item.name = a;
        if (w.size() != 3 || a.view().find_first_of("/\\:") != nx::string_view::npos)
            return bad("expected shot <name> (no folders)");
    } else if (what == "wait") {
        item.what = ScriptItem::What::Wait;
        u32 rgb = 0;
        const auto hex = w.size() > 4 ? std::from_chars(w[4].data(), w[4].data() + w[4].size(), rgb, 16) : std::from_chars_result{};
        auto bad_wait = [&] { return bad("expected wait <x> <y> <rrggbb> [<tolerance 0-255>] [press <button> <ms>]"); };
        if (w.size() < 5 || !number(w[2], &item.x) || !number(w[3], &item.y) || w[4].size() != 6 ||
            hex.ec != std::errc{} || hex.ptr != w[4].data() + w[4].size())
            return bad_wait();
        usize at = 5;
        if (at < w.size() && w[at] != "press") {
            if (!number(w[at], &item.tolerance) || item.tolerance < 0 || item.tolerance > 255) return bad_wait();
            ++at;
        }
        if (at < w.size()) {
            if (w.size() != at + 3 || w[at] != "press") return bad_wait();
            item.press = pad_button(nx::string(w[at + 1]));
            if (item.press == SDL_GAMEPAD_BUTTON_INVALID || !number(w[at + 2], &item.press_every) || item.press_every < 200)
                return bad("wait ... press <button> <ms>: a gamepad button, every 200 ms or more");
        }
        item.rgb = rgb;
    } else if (what == "watch") {
        item.what = ScriptItem::What::Watch;
        u32 rgb = 0;
        const auto hex = w.size() > 4 ? std::from_chars(w[4].data(), w[4].data() + w[4].size(), rgb, 16) : std::from_chars_result{};
        if ((w.size() != 8 && w.size() != 10) || !number(w[2], &item.x) || !number(w[3], &item.y) || w[4].size() != 6 ||
            hex.ec != std::errc{} || hex.ptr != w[4].data() + w[4].size() || !number(w[5], &item.tolerance) ||
            item.tolerance < 0 || item.tolerance > 255 || w[6] != "press" ||
            (w.size() == 10 && (w[8] != "after" || !number(w[9], &item.press_after) || item.press_after > 10000)))
            return bad("expected watch <x> <y> <rrggbb> <tolerance 0-255> press <button> [after <ms, up to 10000>]");
        item.press = pad_button(nx::string(w[7]));
        if (item.press == SDL_GAMEPAD_BUTTON_INVALID) return bad("watch ... press <button>: a gamepad button");
        item.rgb = rgb;
    } else if (what == "unwatch") {
        item.what = ScriptItem::What::Unwatch;
        if (w.size() != 2) return bad("expected unwatch");
    } else if (what == "quit") {
        item.what = ScriptItem::What::Quit;
        if (w.size() != 2) return bad("expected quit");
    } else {
        return bad("expected key, pad, axis, touch, shot, wait, watch, unwatch or quit");
    }
    return item;
}

}  // namespace

Result<nx::vector<ScriptItem>> parse_script(nx::string_view text) {
    nx::vector<ScriptItem> items;
    // Line by line, without comments, then item by item.
    usize line_start = 0;
    while (line_start <= text.size()) {
        usize line_end = text.find('\n', line_start);
        if (line_end == nx::string_view::npos) line_end = text.size();
        nx::string_view line = text.substr(line_start, line_end - line_start);
        if (const usize hash = line.find('#'); hash != nx::string_view::npos) line = line.substr(0, hash);
        usize start = 0;
        while (start <= line.size()) {
            usize end = line.find(';', start);
            if (end == nx::string_view::npos) end = line.size();
            nx::string_view item = line.substr(start, end - start);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item = item.substr(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r'))
                item = item.substr(0, item.size() - 1);
            if (auto w = words(item); !w.empty()) {
                auto parsed = parse_item(w, item);
                if (!parsed) return nx::err(parsed.error());
                items.push_back(*parsed);
            }
            start = end + 1;
        }
        line_start = line_end + 1;
    }
    // (in time order within each stretch; a wait ends its stretch, whatever its time)
    auto by_time = [](const ScriptItem& a, const ScriptItem& b) { return a.at_ms < b.at_ms; };
    usize from = 0;
    for (usize i = 0; i <= items.size(); ++i) {
        if (i < items.size() && items[i].what != ScriptItem::What::Wait) continue;
        std::stable_sort(items.begin() + static_cast<std::ptrdiff_t>(from), items.begin() + static_cast<std::ptrdiff_t>(i), by_time);
        from = i + 1;
    }
    return items;
}

namespace {

void push_pad(i32 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = 0x7FFFFFFF;  // (a gamepad of the script's own)
    e.gbutton.button = static_cast<u8>(button);
    e.gbutton.down = down;
    e.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&e);
}

}  // namespace

nx::vector<std::pair<f32, f32>> Script::probes() const {
    nx::vector<std::pair<f32, f32>> out;
    for (const ScriptItem& s : items_)
        if (s.what == ScriptItem::What::Wait || s.what == ScriptItem::What::Watch) out.push_back({s.x, s.y});
    return out;
}

static bool near_colour(u32 c, u32 want, i32 tolerance) {
    for (u32 shift : {16u, 8u, 0u}) {
        const i32 d = static_cast<i32>((c >> shift) & 0xFF) - static_cast<i32>((want >> shift) & 0xFF);
        if (d < -tolerance || d > tolerance) return false;
    }
    return true;
}

void Script::look(u64 ms, Pixel pixel, nx::function_ref<void(const nx::string&)> shot) {
    for (Watching& w : watching_) {
        if (w.down_at && ms - w.down_at >= 60) {
            push_pad(w.item->press, false);
            w.down_at = 0;
        }
        const std::optional<u32> c = pixel(w.item->x, w.item->y);
        const bool now = c && near_colour(*c, w.item->rgb, w.item->tolerance);
        if (now && !w.matched) w.due.push_back(ms + w.item->press_after);  // (it turned that colour: a press)
        w.matched = now;
        if (!w.due.empty() && w.due.front() <= ms && !w.down_at) {
            w.due.erase(w.due.begin());
            push_pad(w.item->press, true);
            nx::logi(logs::frontend, "script: watch at ({}, {}) pressed at {} ms", w.item->x, w.item->y, ms);
            if (std::getenv("PSVRECORE_WATCH_SHOTS")) shot(nx::format("w{:06d}_{}", ms, w.item->press));
            w.down_at = ms ? ms : 1;
        }
    }
}


bool Script::play(u64 ms, platform::Window* window, nx::function_ref<void(const nx::string&)> shot, Pixel pixel) {
    look(ms, pixel, shot);
    for (; next_ < items_.size() && since_ms_ + items_[next_].at_ms <= ms; ++next_) {
        const ScriptItem& s = items_[next_];
        if (s.what == ScriptItem::What::Watch) {
            watching_.push_back(Watching{&s});
            continue;
        }
        if (s.what == ScriptItem::What::Unwatch) {
            for (Watching& w : watching_)
                if (w.down_at) push_pad(w.item->press, false);
            watching_.clear();
            continue;
        }
        if (s.what == ScriptItem::What::Wait) {
            const std::optional<u32> c = pixel(s.x, s.y);
            auto near = [&](u32 shift) {
                const i32 d = static_cast<i32>((*c >> shift) & 0xFF) - static_cast<i32>((s.rgb >> shift) & 0xFF);
                return d >= -s.tolerance && d <= s.tolerance;
            };
            if (!c || !near(16) || !near(8) || !near(0)) {
                // (held until a frame shows it, pressing meanwhile if asked: down for 100 ms, then up)
                if (s.press >= 0) {
                    if (press_down_ && ms - pressed_at_ >= 100) {
                        push_pad(s.press, false);
                        press_down_ = false;
                    } else if (!press_down_ && (pressed_at_ == 0 || ms - pressed_at_ >= s.press_every)) {
                        push_pad(s.press, true);
                        pressed_at_ = ms;
                        press_down_ = true;
                    }
                }
                return true;
            }
            if (press_down_) push_pad(s.press, false);
            press_down_ = false;
            pressed_at_ = 0;
            since_ms_ = ms;
            nx::logi(logs::frontend, "script: wait for {:06x} at ({}, {}) met at {} ms", s.rgb, s.x, s.y, ms);
            continue;
        }
        SDL_Event e{};
        switch (s.what) {
            case ScriptItem::What::Key:
                e.type = s.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.key = static_cast<SDL_Keycode>(s.code);
                e.key.down = s.down;
                break;
            case ScriptItem::What::Pad:
                e.type = s.down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
                e.gbutton.which = 0x7FFFFFFF;  // (a gamepad of the script's own)
                e.gbutton.button = static_cast<u8>(s.code);
                e.gbutton.down = s.down;
                break;
            case ScriptItem::What::Axis:
                e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                e.gaxis.which = 0x7FFFFFFF;
                e.gaxis.axis = static_cast<u8>(s.code);
                e.gaxis.value = static_cast<i16>(s.value);
                break;
            case ScriptItem::What::Touch: {
                // (the screen's point in the window: where the picture is drawn)
                if (!window) continue;
                int w = 0, h = 0;
                SDL_GetWindowSize(window->sdl(), &w, &h);
                const f32 scale = std::min(static_cast<f32>(w) / platform::kScreenWidth,
                                           static_cast<f32>(h) / platform::kScreenHeight);
                const f32 x = (static_cast<f32>(w) - platform::kScreenWidth * scale) / 2 + s.x * scale;
                const f32 y = (static_cast<f32>(h) - platform::kScreenHeight * scale) / 2 + s.y * scale;
                if (s.phase == 1) {
                    e.type = SDL_EVENT_MOUSE_MOTION;
                    e.motion.which = 1;
                    e.motion.x = x, e.motion.y = y;
                } else {
                    e.type = s.phase == 2 ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
                    e.button.which = 1;
                    e.button.button = SDL_BUTTON_LEFT;
                    e.button.down = s.phase == 0;
                    e.button.x = x, e.button.y = y;
                }
                break;
            }
            case ScriptItem::What::Shot: shot(s.name); continue;
            case ScriptItem::What::Wait:
            case ScriptItem::What::Watch:
            case ScriptItem::What::Unwatch: continue;  // (done above)
            case ScriptItem::What::Quit: ++next_; return false;
        }
        e.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&e);
    }
    return true;
}

}  // namespace psvrecore::frontend
