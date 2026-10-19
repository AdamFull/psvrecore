#include "psvrecore/platform/input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>

namespace psvrecore::platform {

namespace {

// SCE_CTRL_* bits.
constexpr u32 kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3, kUp = 1u << 4, kRight = 1u << 5,
              kDown = 1u << 6, kLeft = 1u << 7, kL = 1u << 8, kR = 1u << 9, kTriangle = 1u << 12, kCircle = 1u << 13,
              kCross = 1u << 14, kSquare = 1u << 15;

// The front panel's size in its coordinates.
constexpr f32 kPanelW = 1920.0f, kPanelH = 1088.0f;

u32 gamepad_bit(u8 button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return kCross;
        case SDL_GAMEPAD_BUTTON_EAST: return kCircle;
        case SDL_GAMEPAD_BUTTON_WEST: return kSquare;
        case SDL_GAMEPAD_BUTTON_NORTH: return kTriangle;
        case SDL_GAMEPAD_BUTTON_BACK: return kSelect;
        case SDL_GAMEPAD_BUTTON_START: return kStart;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return kL3;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return kR3;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return kL;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return kR;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return kUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return kDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return kLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return kRight;
        default: return 0;
    }
}

u32 key_bit(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return kUp;
        case SDLK_DOWN: return kDown;
        case SDLK_LEFT: return kLeft;
        case SDLK_RIGHT: return kRight;
        case SDLK_Z: return kCross;
        case SDLK_X: return kCircle;
        case SDLK_A: return kSquare;
        case SDLK_S: return kTriangle;
        case SDLK_Q: return kL;
        case SDLK_W: return kR;
        case SDLK_RETURN: return kStart;
        case SDLK_RSHIFT: return kSelect;
        default: return 0;
    }
}

// A stick axis (-32768 to 32767) as the console's (0 to 255, 128 at rest).
u8 stick(i16 v) { return static_cast<u8>((static_cast<i32>(v) + 32768) >> 8); }

}  // namespace

void InputMapper::set_view(f32 window_w, f32 window_h, f32 x, f32 y, f32 w, f32 h) {
    window_w_ = window_w, window_h_ = window_h;
    view_x_ = x, view_y_ = y, view_w_ = w, view_h_ = h;
}

bool InputMapper::to_panel(f32 wx, f32 wy, i16* px, i16* py) const {
    if (view_w_ <= 0 || view_h_ <= 0) return false;
    const f32 u = (wx - view_x_) / view_w_, v = (wy - view_y_) / view_h_;
    if (u < 0 || u >= 1 || v < 0 || v >= 1) return false;
    *px = static_cast<i16>(u * kPanelW);
    *py = static_cast<i16>(v * kPanelH);
    return true;
}

runtime::PadState InputMapper::combine() const {
    runtime::PadState s;
    s.buttons = keys_;
    // (the stick furthest from rest wins, over all gamepads)
    auto further = [](u8 a, u8 b) { return std::abs(static_cast<i32>(b) - 128) > std::abs(static_cast<i32>(a) - 128) ? b : a; };
    for (const auto& [id, g] : gamepads_) {
        s.buttons |= g.buttons | (g.l_trigger ? kL : 0) | (g.r_trigger ? kR : 0);
        s.lx = further(s.lx, g.lx), s.ly = further(s.ly, g.ly);
        s.rx = further(s.rx, g.rx), s.ry = further(s.ry, g.ry);
    }
    return s;
}

runtime::TouchState InputMapper::touches() const {
    runtime::TouchState t;
    if (mouse_down_) t.points[t.count++] = runtime::TouchPoint{0, mouse_x_, mouse_y_};
    for (const Finger& f : fingers_) {
        if (t.count >= 6) break;
        t.points[t.count++] = runtime::TouchPoint{f.report_id, f.x, f.y};
    }
    return t;
}

bool InputMapper::handle(const SDL_Event& e, runtime::Input& input, u64 now_ns) {
    bool pad = false, touch = false;
    switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: gamepads_[e.gdevice.which] = Gamepad{}; break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            gamepads_.erase(e.gdevice.which);
            pad = true;
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            Gamepad& g = gamepads_[e.gbutton.which];
            const u32 bit = gamepad_bit(e.gbutton.button);
            g.buttons = e.gbutton.down ? g.buttons | bit : g.buttons & ~bit;
            pad = bit != 0;
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            Gamepad& g = gamepads_[e.gaxis.which];
            const i16 v = e.gaxis.value;
            switch (e.gaxis.axis) {
                case SDL_GAMEPAD_AXIS_LEFTX: g.lx = stick(v); break;
                case SDL_GAMEPAD_AXIS_LEFTY: g.ly = stick(v); break;
                case SDL_GAMEPAD_AXIS_RIGHTX: g.rx = stick(v); break;
                case SDL_GAMEPAD_AXIS_RIGHTY: g.ry = stick(v); break;
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: g.l_trigger = v > 16384; break;
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: g.r_trigger = v > 16384; break;
                default: break;
            }
            pad = true;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const u32 bit = key_bit(e.key.key);
            if (!bit || e.key.repeat) break;
            keys_ = e.key.down ? keys_ | bit : keys_ & ~bit;
            pad = true;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION: {
            if (e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.which == SDL_TOUCH_MOUSEID : e.button.which == SDL_TOUCH_MOUSEID)
                break;  // (a touch screen's own events come as fingers)
            if (e.type != SDL_EVENT_MOUSE_MOTION && e.button.button != SDL_BUTTON_LEFT) break;
            const f32 x = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
            const f32 y = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
            const bool was = mouse_down_;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                mouse_down_ = false;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || mouse_down_) {
                // (a press outside the picture touches nothing; a drag leaving it lifts the finger)
                mouse_down_ = to_panel(x, y, &mouse_x_, &mouse_y_);
            }
            touch = was || mouse_down_;
            break;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            const u64 id = e.tfinger.fingerID;
            auto it = std::find_if(fingers_.begin(), fingers_.end(), [&](const Finger& f) { return f.id == id; });
            i16 px, py;
            const bool inside = to_panel(e.tfinger.x * window_w_, e.tfinger.y * window_h_, &px, &py);
            if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED || !inside) {
                if (it != fingers_.end()) fingers_.erase(it), touch = true;
            } else if (it != fingers_.end()) {
                it->x = px, it->y = py;
                touch = true;
            } else if (e.type == SDL_EVENT_FINGER_DOWN) {
                // (report ids 1-127, the same while the finger stays down; 0 is the mouse's)
                fingers_.push_back(Finger{id, next_report_id_, px, py});
                next_report_id_ = static_cast<u8>(next_report_id_ % 127 + 1);
                touch = true;
            }
            break;
        }
        default: break;
    }
    if (pad) {
        const runtime::PadState s = combine();
        if (!(s == pad_)) {
            pad_ = s;
            input.set_pad(s, now_ns);
        } else {
            pad = false;
        }
    }
    if (touch) {
        touch_ = touches();
        input.set_touch(0, touch_, now_ns);
    }
    return pad || touch;
}

}  // namespace psvrecore::platform
