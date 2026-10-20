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
//     wait <x> <y> <rrggbb> [<tolerance>] [press <button> <ms>]
//                                            from then on, holds the script until a frame shown has that colour (each
//                                            component within the tolerance, 24 by default) at that point of the
//                                            screen, pressing the gamepad button every so many ms meanwhile if asked;
//                                            the items after a wait count their times from when it was met
//     watch <x> <y> <rrggbb> <tolerance> press <button> [after <ms>]
//                                            from then on, presses the gamepad button (for 60 ms) each time that
//                                            point turns that colour, or so many ms after, alongside what else the
//                                            script does (a player for a rhythm game's lanes, say)
//     unwatch                                ends the watches
//     quit                                   ends the program

#include <optional>
#include <utility>

#include "psvrecore/error.hpp"
#include "psvrecore/platform/window.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::frontend {

struct ScriptItem {
    enum class What : u8 { Key, Pad, Axis, Touch, Shot, Wait, Watch, Unwatch, Quit };
    u64 at_ms = 0;
    What what = What::Key;
    i32 code = 0;       // the key, button or axis (SDL's)
    i32 value = 0;      // an axis's value
    bool down = false;  // a key's or button's
    u8 phase = 0;       // a touch's: 0 down, 1 move, 2 up
    f32 x = 0, y = 0;   // a touch's or a wait's point on the screen
    u32 rgb = 0;        // a wait's colour (0xRRGGBB)
    i32 tolerance = 24; // a wait's: per component
    i32 press = -1;     // a wait's button to press while it waits, a watch's to press (SDL's), or none
    u32 press_every = 0;  // a wait's: how often
    u32 press_after = 0;  // a watch's: how long after the colour came
    nx::string name;    // a shot's
};

// The items in time order (within the stretches a wait ends); an error names the item that is wrong.
Result<nx::vector<ScriptItem>> parse_script(nx::string_view text);

class Script {
public:
    Script() = default;
    explicit Script(nx::vector<ScriptItem> items) : items_(std::move(items)) {}

    // The colour of the last frame shown at a point of the screen (0xRRGGBB), or none yet.
    using Pixel = nx::function_ref<std::optional<u32>(f32 x, f32 y)>;

    // Plays the items due by `ms` (from the boot): controls into the window's event queue (touches need the window
    // to place them), shots to `shot`, waits against `pixel`. False once a `quit` has been played.
    bool play(u64 ms, platform::Window* window, nx::function_ref<void(const nx::string&)> shot, Pixel pixel);
    bool finished() const { return next_ >= items_.size(); }
    // The points the waits look at (for the display to keep their colours).
    nx::vector<std::pair<f32, f32>> probes() const;

private:
    nx::vector<ScriptItem> items_;
    usize next_ = 0;
    u64 since_ms_ = 0;  // (when the last wait was met: the items after it count from then)
    u64 pressed_at_ = 0;  // (a waiting press: when its button went down, and whether it is down)
    bool press_down_ = false;
    // The watches going on: the item, whether its point had its colour at the last look, and when its button went
    // down (0: up).
    struct Watching {
        const ScriptItem* item;
        bool matched = false;
        u64 down_at = 0;
        nx::vector<u64> due;  // (the presses still to come, in time order)
    };
    nx::vector<Watching> watching_;
    void look(u64 ms, Pixel pixel, nx::function_ref<void(const nx::string&)> shot);
};

}  // namespace psvrecore::frontend
