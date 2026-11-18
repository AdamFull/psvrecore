#pragma once

// The system's dialogs (SceCommonDialog's message, save data, network check, trophy setup and text input dialogs,
// and SceIme's in-game keyboard) as the host draws them, and the notices the system shows over a game (a trophy
// unlocked).
//
// By default nothing is drawn and every dialog is answered at once with the answer a player gives most often (see
// hle/commondialog.cpp): scripted and automated runs depend on it. With set_drawn(true) (the frontend does it when it
// draws the system's screens), a dialog the game opens waits for the host's answer: the host reads it (current) and
// answers it (answer), from any thread. A dialog that only shows a message the game closes (progress, "please wait")
// is closed by the game as before.

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::runtime {

struct DialogSlot {
    u32 id = 0;
    bool exists = false, broken = false;  // (saved data in it; a save cut short)
    nx::string title, subtitle, detail;  // (an empty slot: the game's title for it, "New saved data" if none)
    nx::string date;                     // (yyyy/mm/dd hh:mm, of the last save)
    u32 size_kib = 0;
};

struct DialogView {
    enum class Kind : u8 { None, Message, SaveData, NetCheck, TrophySetup, Ime, InGameIme };
    enum class Buttons : u8 { None, Ok, YesNo, OkCancel, Cancel, Three };
    u64 serial = 0;  // (each dialog, and each step of one, has its own)
    Kind kind = Kind::None;
    i32 mode = 0;  // (the dialog's own: message dialog user/system/error/progress, save data fixed/list/...)
    i32 type = 0;  // (save data: 1 save, 2 load, 3 delete)
    nx::string title;  // (a list's title, a text input's)
    nx::string text;   // the message, UTF-8 (the system's own wording for system messages and error codes)
    Buttons buttons = Buttons::Ok;
    nx::string labels[3];  // (Three: the game's)
    bool progress = false;  // (a progress bar: percent, and the text beside it)
    u32 percent = 0;
    nx::string progress_text;
    // Save data: the slots (a list's, or the one a step is about), the one focused first, the list's item style.
    nx::vector<DialogSlot> slots;
    u32 focus = 0;
    i32 item_style = 0;
    // Text input: the text (UTF-8), its longest length in UTF-16 units, SceIme's type (0 default, 1 basic Latin,
    // 2 number, 3 extended number, 4 URL, 5 mail), options (1 multiline, 2 no capitals, 4 no assistance), a password,
    // a cancel button, the enter key's label (0 enter, 1 send, 2 search, 3 go), the languages asked for.
    nx::string ime_text;
    u32 max_length = 0, ime_type = 0, ime_option = 0, enter_label = 0;
    bool password = false, with_cancel = false;
    u64 languages = 0;
    bool enter_circle = false;  // (the dialogs' enter button: Circle, as set by sceCommonDialogSetConfigParam)
    // The in-game keyboard: how often the game set its text or caret itself (the host takes them again when it
    // changes), and the caret (UTF-16 units).
    u32 revision = 0, caret = 0;
};

// The host's answer: the result (0 OK, 1 the player backed out), the button (the dialog's own ids: OK and Yes 1,
// No 2, the third 3; the text input's Close 1 and Enter 2), the slot chosen, the text typed (UTF-8).
struct DialogAnswer {
    i32 result = 0;
    i32 button = 1;
    u32 slot = 0;
    nx::string text;
};

// A notice over the game (a trophy unlocked).
struct SystemNotice {
    nx::string title, text;
    i32 grade = 0;  // (a trophy's: 0 unknown, 1 platinum, 2 gold, 3 silver, 4 bronze)
};

namespace dialogs {

// Whether dialogs wait for the host's answer (false by default: answered at once, as without a host).
void set_drawn(bool drawn);
bool drawn();
// The dialog the host is to draw and answer, if one is open (false: none). Its progress, and its text for a dialog
// the game changes while it is up, are read again each time.
bool current(DialogView* out);
// The kind of the dialog current() would give, without copying it (None: none).
DialogView::Kind showing();
// The answer to dialog `serial` (ignored if it is no longer the one open): for the in-game keyboard, Enter
// (button 2) or Close pressed.
void answer(u64 serial, const DialogAnswer& answer);
// The in-game keyboard's text as the player changed it, with the caret (UTF-16 units): the game hears of it at
// its next update.
void edit(u64 serial, const nx::string& text, u32 caret);

// Notices: posted by the system libraries, taken by the host (false: none waiting). Without a host they are only
// logged.
void notify(SystemNotice notice);
bool take_notice(SystemNotice* out);
bool has_notice();

}  // namespace dialogs

}  // namespace psvrecore::runtime
