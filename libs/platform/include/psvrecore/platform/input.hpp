#pragma once

// The host's controls as the console's (Runtime::input): gamepads, the keyboard, the mouse and a touch screen.
//
// - Gamepads (any number, merged): face buttons by position (south Cross, east Circle, west Square, north
//   Triangle), the d-pad, Back as Select, Start, the shoulders and triggers as L and R, the stick clicks as L3 and R3,
//   and both sticks.
// - Keyboard: arrows the d-pad, Z Cross, X Circle, A Square, S Triangle, Q L, W R, Enter Start, Right Shift Select.
// - The mouse's left button, and fingers on a touch screen, touch the front panel where they are over the picture
//   (the view: where the frame is drawn in the window).
//
// Every change is stamped with the kernel's clock when the event happened.

#include "psvrecore/runtime/input.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"

union SDL_Event;

namespace psvrecore::platform {

class InputMapper {
public:
    // The window's size and the rectangle the picture fills in it (pixels), for touches.
    void set_view(f32 window_w, f32 window_h, f32 x, f32 y, f32 w, f32 h);
    // Takes an event (from the thread that pumps them); true if it changed the controls.
    bool handle(const SDL_Event& event, runtime::Input& input, u64 now_ns);

    const runtime::PadState& pad() const { return pad_; }
    const runtime::TouchState& touch() const { return touch_; }

private:
    struct Gamepad {
        u32 buttons = 0;
        u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        bool l_trigger = false, r_trigger = false;
    };
    nx::flat_hash_map<u32, Gamepad> gamepads_;
    u32 keys_ = 0;
    runtime::PadState pad_;
    // (front panel: the mouse's point and fingers' points by their id)
    bool mouse_down_ = false;
    i16 mouse_x_ = 0, mouse_y_ = 0;
    struct Finger {
        u64 id;
        u8 report_id;
        i16 x, y;
    };
    nx::vector<Finger> fingers_;
    u8 next_report_id_ = 1;
    runtime::TouchState touch_;
    f32 window_w_ = 960, window_h_ = 544, view_x_ = 0, view_y_ = 0, view_w_ = 960, view_h_ = 544;

    bool to_panel(f32 wx, f32 wy, i16* px, i16* py) const;
    runtime::PadState combine() const;
    runtime::TouchState touches() const;
};

}  // namespace psvrecore::platform
