#pragma once

// The system's own screens drawn over the game: dialogs, the on-screen keyboard, notifications, the Home menu.
// The window draws them with Dear ImGui (third_party/imgui) on its presenting thread, over the picture, after the
// game's frame. Their coordinates are the screen's (kScreenWidth x kScreenHeight units, where the picture is),
// scaled with it, so they keep their place on the picture at any window size and are drawn at the window's
// resolution.
//
// While a screen takes the controls, the game gets none (a neutral controller, no touches); buttons held when it
// starts or ends count for neither side until they are released. The controller (gamepads, the keyboard's buttons,
// the on-screen controls, a script's presses) moves the focus and presses what has it; the mouse and touches point
// and press directly; while the screen takes typed text (desktop), the keyboard types instead of being buttons.

#include "core/foundation/core/foundation.h"

namespace psvrecore::platform {

class SystemUi {
public:
    virtual ~SystemUi() = default;

    // From any thread. Whether anything is shown (the window then draws without waiting for the game's frames),
    // whether it takes the controls, and whether it takes typed text.
    virtual bool visible() const = 0;
    virtual bool takes_controls() const = 0;
    virtual bool takes_text() const { return false; }
    // Whether the console's enter button is Circle (Cross then cancels), as in Japan; else Cross.
    virtual bool enter_is_circle() const { return false; }

    // The controller as the screens see it in one frame: the buttons held (SCE_CTRL_* bits), and those pressed since
    // the frame before (a press and release between two frames counts), enter and cancel already swapped for
    // enter_is_circle() (Cross's bit is then enter).
    struct Controls {
        u32 held = 0, pressed = 0;
    };
    // On the presenting thread, ImGui's context current, between its NewFrame and Render: the screens for one frame.
    virtual void build(const Controls& controls) = 0;
};

}  // namespace psvrecore::platform
