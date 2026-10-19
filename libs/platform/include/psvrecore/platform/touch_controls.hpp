#pragma once

// The console's controls drawn on a touch screen, over the picture: the d-pad and the left stick on the left, the
// face buttons and the right stick on the right, L and R at the top corners, Select and Start at the bottom, and a
// menu button (the settings) at the top in the middle. A finger
// that goes down on a control is that control's until it is lifted (sliding between the face buttons, or around the
// d-pad, moves the press; a stick follows it); other fingers touch the front panel as before.
//
// They are laid out from the window's height, inside the area the system leaves free (notches, gesture bars), and
// drawn as quads cut from one atlas made here. Shown: always, never, or (auto) once the screen is touched, until a
// gamepad or the keyboard is used.

#include "psvrecore/platform/quad.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::platform {

class TouchControls {
public:
    enum class Mode : u8 { Auto, Always, Never };

    void set_mode(Mode mode);
    void set_opacity(f32 opacity);  // (0 to 1)
    void set_scale(f32 scale);      // (1: the default size)
    // The window (its coordinates, the ones fingers come in), its pixels for each of them, and the safe area.
    void layout(f32 width, f32 height, f32 pixels_per_unit, f32 safe_x, f32 safe_y, f32 safe_w, f32 safe_h);

    bool visible() const { return visible_; }
    // (auto: touching shows them, a gamepad or a key hides them)
    void touched();
    void other_input();

    // A finger, in the window's coordinates: true when a control has it.
    bool finger_down(u64 id, f32 x, f32 y);
    bool finger_move(u64 id, f32 x, f32 y);
    bool finger_up(u64 id);

    // What the controls press (SCE_CTRL_* bits) and where their sticks are (0 to 255, 128 at rest).
    u32 buttons() const;
    u8 lx() const { return stick_[0][0]; }
    u8 ly() const { return stick_[0][1]; }
    u8 rx() const { return stick_[1][0]; }
    u8 ry() const { return stick_[1][1]; }

    // The menu button was touched (once).
    bool take_menu_request() {
        const bool r = menu_requested_;
        menu_requested_ = false;
        return r;
    }

    // Changes whenever what is drawn does.
    u64 version() const { return version_; }
    // The quads to draw, in the window's pixels (none when hidden).
    nx::vector<Quad> quads() const;
    // The atlas the quads are cut from (RGBA, straight alpha).
    static void atlas(nx::vector<u8>* rgba, u32* width, u32* height);

private:
    // (L, R, Select, Start and Menu in that order: rect_ follows it)
    enum class Control : u8 { None, Dpad, Face, LeftStick, RightStick, L, R, Select, Start, Menu };
    struct Finger {
        u64 id;
        Control control;
        u32 bits;  // what it presses
    };

    Control hit(f32 x, f32 y) const;
    void press(Finger& f, f32 x, f32 y);
    void changed() { ++version_; }

    Mode mode_ = Mode::Auto;
    f32 opacity_ = 0.5f, scale_ = 1.0f;
    bool visible_ = false;
    // The layout, in window coordinates.
    f32 width_ = 0, height_ = 0, ppu_ = 1, unit_ = 1;
    f32 dpad_x_ = 0, dpad_y_ = 0, face_x_ = 0, face_y_ = 0;
    f32 stick_x_[2] = {}, stick_y_[2] = {};
    f32 rect_[5][4] = {};  // L, R, Select, Start, Menu: x0, y0, x1, y1
    bool menu_requested_ = false;
    nx::vector<Finger> fingers_;
    u8 stick_[2][2] = {{0x80, 0x80}, {0x80, 0x80}};
    u64 version_ = 1;
};

}  // namespace psvrecore::platform
