#pragma once

// The system's screens over the game (platform::SystemUi): the dialogs the game opens (runtime/dialogs.hpp: messages,
// progress, save data lists and questions, the network check and trophy setup answered as offline, the text input
// dialog with its on-screen keyboard), notices (a trophy unlocked) and the Home menu (resume, the settings, quit).
// Laid out like the console's (dimmed picture, a light panel, pill-shaped buttons; the keyboard in the lower half;
// notices at the top right), drawn with the system fonts, no picture or sound of the console's.
//
// Controls: the d-pad (or the left stick) moves the focus, enter (Cross; Circle where it is the enter button)
// presses, cancel backs out; the mouse and touches press directly. On the keyboard: Square deletes, Triangle is a
// space, L and R move the caret, Select is shift, Start is enter; a desktop's keyboard types (Backspace, the arrows,
// Enter, Escape). The Home menu opens with Guide (the PS button), Escape on a desktop, and closes the same way.

#include <atomic>

#include "psvrecore/frontend/settings.hpp"
#include "psvrecore/platform/system_ui.hpp"
#include "psvrecore/runtime/dialogs.hpp"
#include "core/foundation/threading/sync.h"

namespace psvrecore::frontend {

class SystemScreens final : public platform::SystemUi {
public:
    struct Options {
        nx::string title;          // the game's, at the top of the Home menu
        nx::string settings_file;  // where the Home menu's settings are changed (empty: none shown)
        void (*settings_changed)() = nullptr;  // (from the presenting thread, after a change is written)
        bool desktop = true;
    };
    explicit SystemScreens(Options options);

    // From the thread pumping events: the Home button (opens the menu, or closes it); whether the menu is open (the
    // game is then paused); whether the player chose to quit there.
    void home_button() { home_.store(!home_.load()); }
    bool home_open() const { return home_.load(); }
    bool quit_requested() const { return quit_.load(); }

    bool visible() const override;
    bool takes_controls() const override;
    bool takes_text() const override;
    bool enter_is_circle() const override { return enter_circle_.load(); }
    void build(const Controls& controls) override;

private:
    void draw_dialog(const runtime::DialogView& v, const Controls& c, bool focus);
    void draw_message(const runtime::DialogView& v, const Controls& c);
    void draw_list(const runtime::DialogView& v, const Controls& c);
    void draw_ime(const runtime::DialogView& v, const Controls& c);
    void draw_notice();
    void draw_home(const Controls& c);
    void draw_settings(const Controls& c, f32 x, f32 y, f32 w, f32 h);
    void answer(const runtime::DialogView& v, i32 result, i32 button, u32 slot = 0);
    void type(u32 cp, const runtime::DialogView& v);

    Options options_;
    std::atomic<bool> home_{false}, quit_{false}, enter_circle_{false}, notice_active_{false};
    bool styled_ = false;

    // The dialog drawn (presenting thread): its serial, when it came, and the keyboard's text, caret and page.
    u64 serial_ = 0;
    u32 revision_ = 0;
    f64 since_ = 0;
    nx::vector<u32> text_;
    usize caret_ = 0;
    bool shift_ = false, symbols_ = false;
    bool focus_dialog_ = false;  // (focus it again: the Home menu closed over it)
    bool fresh_ = false;         // (the dialog drawn for the first time: its default focus)

    // The notice shown, and when it came.
    bool notice_ = false;
    runtime::SystemNotice shown_;
    f64 notice_at_ = 0;

    // The Home menu: its page (0 the menu, 1 the settings, 2 quitting?), whether it was open the last frame, the
    // settings as last read, the one focused, and a message about the last change.
    i32 page_ = 0;
    i32 focus_page_ = -1;  // (the page whose first item takes the focus this frame: 0 the menu, 1 the settings, 2 quit)
    i32 menu_focus_ = 0;   // (the menu's item focused when it takes the focus)
    bool home_was_ = false;
    nx::vector<SettingInfo> settings_;
    i32 setting_ = 0;
    nx::string setting_message_;
};

}  // namespace psvrecore::frontend
