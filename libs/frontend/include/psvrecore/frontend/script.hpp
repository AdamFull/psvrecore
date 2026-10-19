#pragma once

// Scripted controls (--input): a timeline of the host's events, put in the window's event queue at their times as if
// they came from the host, and frames to save. For smoke tests and replays. Items are `<ms> <what> <arguments>`,
// separated by ';' or new lines, the time counted from the boot; `#` starts a comment to the end of the line:
//
//     key <SDL key name> down|up             a key (the keyboard's controls)
//     pad <button> down|up                   a gamepad button: the console's name (cross, circle, square, triangle,
//                                            up, down, left, right, l, r, start, select, l3, r3) or SDL's
//     axis <SDL gamepad axis> <-32768..32767> a gamepad axis (leftx, lefty, rightx, righty, lefttrigger, ...)
//     touch <x> <y> down|move|up             the mouse at a point of the screen (960 x 544) where it is drawn
//     shot <name>                            the next frame shown, saved as <name>.png
//     quit                                   ends the program

#include "psvrecore/error.hpp"
#include "psvrecore/platform/window.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::frontend {

struct ScriptItem {
    enum class What : u8 { Key, Pad, Axis, Touch, Shot, Quit };
    u64 at_ms = 0;
    What what = What::Key;
    i32 code = 0;       // the key, button or axis (SDL's)
    i32 value = 0;      // an axis's value
    bool down = false;  // a key's or button's
    u8 phase = 0;       // a touch's: 0 down, 1 move, 2 up
    f32 x = 0, y = 0;   // a touch's point on the screen
    nx::string name;    // a shot's
};

// The items in time order; an error names the item that is wrong.
Result<nx::vector<ScriptItem>> parse_script(nx::string_view text);

class Script {
public:
    Script() = default;
    explicit Script(nx::vector<ScriptItem> items) : items_(std::move(items)) {}

    // Plays the items due by `ms` (from the boot): controls into the window's event queue, shots to `shot`. False
    // once a `quit` has been played.
    bool play(u64 ms, platform::Window& window, nx::function_ref<void(const nx::string&)> shot);
    bool finished() const { return next_ >= items_.size(); }

private:
    nx::vector<ScriptItem> items_;
    usize next_ = 0;
};

}  // namespace psvrecore::frontend
