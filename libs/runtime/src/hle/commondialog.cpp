// The system's dialogs (SceCommonDialog): the message, save data, network check and trophy setup dialogs, without
// drawing anything. One dialog at a time, as on the console: Init starts it (running), and the game's calls to
// sceCommonDialogUpdate (each frame, to draw it) let it finish, with the answer a player gives most often (a save
// data dialog can then go on to another step, sceSaveDataDialogContinue, until the game finishes and ends it):
// - messages with buttons: OK or Yes; messages that only wait (progress, "please wait") stay up until the game
//   closes them (sceSaveDataDialogFinish), as on the console;
// - save data: confirmations are confirmed, the fixed slot is the one the game named (it exists if its save data
//   does: savedata0:'s slots), the list's focused slot is chosen;
// - network check: offline, the player declines to connect (user canceled, PSN not reached);
// - trophy setup: done (trophies are local);
// - text input (IME): the text confirmed as the game gave it (its initial text, in the input buffer);
// - PSN, store and photo dialogs (messages, profiles, friends, Facebook, checkout, photo import, a game's custom data,
//   invitations): signed out (and no photos), the player backs out (user canceled).
// Every message's text is logged, to see what the game asks.
//
// Drawn (dialogs::set_drawn, runtime/dialogs.hpp: the frontend draws the system's screens), the message, save data,
// network check, trophy setup and text input dialogs wait instead for the player's answer, given by the host;
// everything else is as above (a message that only waits is still closed by the game, and the PSN dialogs are still
// backed out of at once).

#include "hle_common.hpp"

#include <atomic>
#include <cstring>
#include "psvrecore/runtime/dialogs.hpp"
#include "psvrecore/runtime/savedata.hpp"

namespace psvrecore::runtime::hle {

// (apputil.cpp: a slot's parameters, from the application utility's save data)
bool save_data_slot(u32 id, SaveData::SlotParam* out);
// (ime.cpp: the in-game keyboard, drawn)
bool ime_keyboard_view(DialogView* out);
void ime_keyboard_edit(u64 serial, const nx::string& text, u32 caret);
void ime_keyboard_answer(u64 serial, const DialogAnswer& a);

namespace {

constexpr u32 kSceCommonDialog = 0xE537816C, kSceNpWebApiCommonDialog = 0x883CFD5C;
constexpr i32 kBusy = err::code(0x80020401), kNull = err::code(0x80020402), kNotRunning = err::code(0x80020404),
              kNotFinished = err::code(0x80020410), kNotInUse = err::code(0x80020411);
constexpr i32 kStatusNone = 0, kStatusRunning = 1, kStatusFinished = 2;
constexpr i32 kResultOk = 0, kResultUserCanceled = 1, kResultAborted = 2;
constexpr u32 kCommonParam = 4;  // (every dialog's parameters: the SDK version, then SceCommonDialogParam (76 bytes))
constexpr u32 kParamBody = kCommonParam + 76;

enum class Kind : u8 { None, Message, SaveData, NetCheck, TrophySetup, Ime, Other };

struct Dialog {
    nx::mutex lock;
    Kind kind = Kind::None;
    i32 status = kStatusNone;
    bool waits = false;    // (until the game closes it)
    bool updated = false;  // (shown: an update came since it started)
    i32 mode = 0, result = kResultOk, button = 0;
    u8 other = 0;  // (which of the dialogs answered alike: Kind::Other)
    u32 slot = 0, userdata = 0;
    bool slot_exists = false;
    SaveData::SlotParam slot_param{};
    // Drawn: whether the host answers it (else it is answered as above), whether it has, what it shows, and where a
    // text input's text goes.
    bool host = false, answered = false;
    u64 serial = 0;
    DialogView view;
    u32 ime_buffer = 0, ime_max = 0;
    bool enter_circle = false;  // (sceCommonDialogSetConfigParam)
    u32 list_slots = 0, list_count = 0;  // (the save data list's slots, for a list continued without them)
    i32 save_type = 0;                   // (save, load, delete: as started, for steps continued without it)
};
std::atomic<bool> g_drawn{false};
Dialog& dialog() {
    static Dialog d;
    return d;
}

nx::string text(u32 address, usize max) { return address && memory().valid(address, 1) ? str(Ptr<const char>(address), max) : nx::string(); }
u32 word(u32 address) { return address && memory().valid(address, 4) ? psv_ld32(address) : 0; }

i32 start(Kind kind, bool waits) {
    Dialog& d = dialog();
    if (d.status == kStatusRunning) return kBusy;
    d.kind = kind;
    d.status = kStatusRunning;
    d.waits = waits;
    d.updated = false;
    d.mode = 0, d.result = kResultOk, d.button = 0;
    d.slot = 0, d.userdata = 0, d.slot_exists = false;
    d.host = g_drawn.load() && kind != Kind::Other;
    d.answered = false;
    d.serial += 1;
    d.view = DialogView{};
    d.view.serial = d.serial;
    d.view.enter_circle = d.enter_circle;
    d.ime_buffer = d.ime_max = 0;
    return 0;
}

// (a step the player answers: shown, and answered; a drawn one once the host has)
bool answered(const Dialog& d) { return d.host && !d.waits ? d.answered : d.updated; }

i32 status_of(Kind kind) {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    return d.kind == kind ? d.status : kStatusNone;
}

i32 term(Kind kind) {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != kind || d.status == kStatusNone) return kNotInUse;
    if (d.status == kStatusRunning) return kNotFinished;
    d.kind = Kind::None;
    d.status = kStatusNone;
    return 0;
}

// Whether the slot's save data exists (savedata0:), and its parameters: the application utility's own save data
// (apputil.cpp), read under its lock, never during one of its saves (another SaveData of the folder would finish a
// save's commit it found under way).
bool find_slot(u32 id, SaveData::SlotParam* out) {
    SaveData::SlotParam param{};
    return save_data_slot(id, out ? out : &param);
}

i32 sceCommonDialogUpdate(Ptr<const void> param) {
    if (!param) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.status == kStatusRunning) {
        d.updated = true;
        if (!d.waits && !d.host) d.status = kStatusFinished;
    }
    return 0;
}

// sceCommonDialogIsRunning: whether a dialog is up (started and not yet ended).
i32 sceCommonDialogIsRunning() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    return d.status == kStatusRunning ? 1 : 0;
}

// SceCommonDialogConfigParam: the SDK version, the dialogs' language and enter button (the system's when out of
// range), reserved. Nothing is drawn, so nothing uses them.
i32 sceCommonDialogSetConfigParam(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, 0x2C)) return kNull;
    nx::logd(logs::hle, "dialogs: language {}, enter button {}", word(param.address + 4), word(param.address + 8));
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    d.enter_circle = word(param.address + 8) == 0;  // (SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE; 1 Cross, else the system's)
    return 0;
}

// ---- message dialog

// The system's own wording (English; not the console's) for its messages.
nx::string msg_system_text(i32 type, i32 value) {
    switch (type) {
        case 1: case 5: case 6: return "Please wait...";
        case 2: return nx::format("There is not enough free space on the memory card.\n{} KB more is needed.", value);
        case 3: return "Calibrate the motion sensor: move the system in a figure eight.";
        case 4: case 100: return "The microphone is turned off.";
        case 7: case 8: return "Insert a memory card.";
        case 9:
            return nx::format("There is not enough free space on the memory card ({} KB more is needed).\nYou can "
                              "continue without it.",
                              value);
        case 10: return "Getting location data...";
        case 11: return "Location data could not be obtained.";
        case 12: return "Location data could not be obtained. Try again?";
        case 13: return "An update for this application is available.";
        case 101: case 102: return "This feature needs a Wi-Fi connection.";
        case 103: return "There is no content available.";
        case 104: return "This feature is not available because of the account's age restriction.";
        case 105: return "Chat is not available because of the account's restrictions.";
        case 106: return "The microphone is turned off. You can continue without it.";
        default: return nx::format("(system message {})", type);
    }
}
DialogView::Buttons msg_system_buttons(i32 type) {
    switch (type) {
        case 1: case 5: case 10: return DialogView::Buttons::None;
        case 6: return DialogView::Buttons::Cancel;
        case 12: return DialogView::Buttons::YesNo;
        default: return DialogView::Buttons::Ok;
    }
}
nx::string error_text(u32 code) { return nx::format("An error occurred.\n(Error code: {:08X})", code); }

i32 sceMsgDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, kParamBody + 24)) return kNull;
    const u32 p = param.address;
    const i32 mode = static_cast<i32>(word(p + kParamBody));
    nx::string what;
    bool waits = false;
    DialogView view;
    switch (mode) {
        case 1: {  // (user message: its buttons, its text)
            const u32 user = word(p + kParamBody + 4);
            const u32 buttons = word(user);
            what = nx::format("\"{}\" (buttons {})", text(word(user + 4), 512), buttons);
            waits = buttons == 2;  // (no buttons: until closed)
            view.text = text(word(user + 4), 512);
            static constexpr DialogView::Buttons kButtons[] = {DialogView::Buttons::Ok, DialogView::Buttons::YesNo,
                                                               DialogView::Buttons::None, DialogView::Buttons::OkCancel,
                                                               DialogView::Buttons::Cancel, DialogView::Buttons::Three};
            view.buttons = buttons < 6 ? kButtons[buttons] : DialogView::Buttons::Ok;
            if (buttons == 5) {  // (SceMsgDialogButtonsParam: each button's text, then its font size)
                const u32 b = word(user + 8);
                for (u32 i = 0; i < 3; ++i) view.labels[i] = text(word(b + 8 * i), 64);
            }
            break;
        }
        case 2: {
            const u32 sys = word(p + kParamBody + 8);
            const i32 type = static_cast<i32>(word(sys)), value = static_cast<i32>(word(sys + 4));
            what = nx::format("system message {}", type), waits = true;
            view.text = msg_system_text(type, value);
            view.buttons = msg_system_buttons(type);
            break;
        }
        case 3: {
            const u32 code = word(word(p + kParamBody + 12));
            what = nx::format("error code 0x{:08x}", code);
            view.text = error_text(code);
            break;
        }
        case 4: {  // (SceMsgDialogProgressBarParam: its type, a system message, the text)
            const u32 bar = word(p + kParamBody + 16);
            what = nx::format("progress \"{}\"", text(word(bar + 44), 512)), waits = true;
            view.text = text(word(bar + 44), 512);
            if (view.text.empty() && word(bar + 4)) view.text = msg_system_text(static_cast<i32>(word(bar + 4)), static_cast<i32>(word(bar + 8)));
            view.buttons = DialogView::Buttons::None;
            view.progress = true;
            break;
        }
        default: return err::code(0x80100A01);  // (SCE_MSG_DIALOG_ERROR_PARAM)
    }
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::Message, waits); r < 0) return r;
    d.mode = mode;
    d.button = 1;  // (OK, Yes)
    view.serial = d.serial, view.enter_circle = d.enter_circle;
    view.kind = DialogView::Kind::Message;
    view.mode = mode;
    d.view = std::move(view);
    // (a system message with a button the player presses: drawn, it waits for it)
    if (d.host && mode == 2 && d.view.buttons != DialogView::Buttons::None) d.waits = false;
    nx::logi(logs::hle, "message dialog: {}{}", what,
             d.waits ? " (stays up until closed)" : d.host ? " (shown)" : " (answered OK)");
    return 0;
}

// The progress bar's value (percent) and its text, for the message and save data dialogs.
i32 progress_bar(Kind kind, i32 target, i32 set, i32 add, const char* message) {
    if (target != 0) return kind == Kind::Message ? err::code(0x80100A01) : err::code(0x80100B01);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != kind || d.status != kStatusRunning || !d.view.progress) return kNotRunning;
    if (set >= 0) d.view.percent = static_cast<u32>(std::min(set, 100));
    if (add > 0) d.view.percent = std::min<u32>(d.view.percent + static_cast<u32>(add), 100);
    if (message) d.view.progress_text = message;
    return 0;
}
i32 sceMsgDialogProgressBarInc(i32 target, u32 delta) {
    return progress_bar(Kind::Message, target, -1, static_cast<i32>(std::min<u32>(delta, 100)), nullptr);
}
i32 sceMsgDialogProgressBarSetValue(i32 target, u32 rate) {
    return progress_bar(Kind::Message, target, static_cast<i32>(std::min<u32>(rate, 100)), 0, nullptr);
}
i32 sceMsgDialogProgressBarSetMsg(i32 target, Ptr<const char> message) {
    if (!message) return kNull;
    const nx::string m = str(message, 512);
    return progress_bar(Kind::Message, target, -1, 0, m.c_str());
}
i32 sceSaveDataDialogProgressBarInc(i32 target, u32 delta) {
    return progress_bar(Kind::SaveData, target, -1, static_cast<i32>(std::min<u32>(delta, 100)), nullptr);
}
i32 sceSaveDataDialogProgressBarSetValue(i32 target, u32 rate) {
    return progress_bar(Kind::SaveData, target, static_cast<i32>(std::min<u32>(rate, 100)), 0, nullptr);
}

i32 sceMsgDialogGetStatus() { return status_of(Kind::Message); }
i32 sceMsgDialogTerm() { return term(Kind::Message); }
// SceMsgDialogResult: mode, result, the button chosen, reserved (32 bytes).
i32 sceMsgDialogGetResult(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 44)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::Message || d.status == kStatusNone) return kNotInUse;
    if (d.status != kStatusFinished) return kNotFinished;
    std::memset(result.get(), 0, 44);
    psv_st32(result.address, static_cast<u32>(d.mode));
    psv_st32(result.address + 4, static_cast<u32>(d.result));
    psv_st32(result.address + 8, static_cast<u32>(d.button));
    return 0;
}
// (closed by the game: finished, aborted, no button chosen)
i32 sceMsgDialogAbort() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::Message || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
    d.result = kResultAborted;
    d.button = 0;
    return 0;
}

// ---- text input dialog (IME)

// SceImeDialogParam (0xA8 bytes): ... the title at 0x28, the longest text at 0x2C, the initial text at 0x30 and the
// buffer the text goes to at 0x34 (UTF-16), SceCommonDialogParam at 0x38.
constexpr i32 kImeButtonEnter = 2;

}  // namespace

nx::string utf16_text(u32 address, u32 max) {
    nx::string out;
    for (u32 i = 0; address && i < max && memory().valid(address + 2 * i, 2); ++i) {
        u32 c = static_cast<u16>(psv_ld16(address + 2 * i));
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max && memory().valid(address + 2 * i + 2, 2)) {
            const u32 low = static_cast<u16>(psv_ld16(address + 2 * i + 2));
            if (low >= 0xDC00 && low < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00), ++i;
        }
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) out.push_back(static_cast<char>(0xC0 | c >> 6)), out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        else if (c < 0x10000)
            out.push_back(static_cast<char>(0xE0 | c >> 12)), out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))),
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        else
            out.push_back(static_cast<char>(0xF0 | c >> 18)), out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F))),
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))), out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    return out;
}

// (UTF-8 to UTF-16 into guest memory, cut to `max` units, terminated; a pair is not split)
void store_utf16(u32 address, u32 max, nx::string_view text) {
    u32 n = 0;
    for (usize i = 0; i < text.size() && n < max;) {
        const u8 c = static_cast<u8>(text[i]);
        const u32 len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + len > text.size()) break;
        u32 cp = len == 4 ? c & 0x07u : len == 3 ? c & 0x0Fu : len == 2 ? c & 0x1Fu : c;
        for (u32 k = 1; k < len; ++k) cp = cp << 6 | (static_cast<u8>(text[i + k]) & 0x3Fu);
        i += len;
        if (cp >= 0x10000) {
            if (n + 2 > max) break;
            cp -= 0x10000;
            psv_st16(address + 2 * n++, 0xD800 | (cp >> 10));
            psv_st16(address + 2 * n++, 0xDC00 | (cp & 0x3FF));
        } else {
            psv_st16(address + 2 * n++, cp);
        }
    }
    psv_st16(address + 2 * n, 0);
}

namespace {

i32 sceImeDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, 0xA8)) return kNull;
    const u32 p = param.address;
    const u32 max_length = word(p + 0x2C), initial = word(p + 0x30), buffer = word(p + 0x34);
    if (!buffer || !max_length || !writable(buffer, 2 * (max_length + 1))) return err::code(0x80102001);  // (SCE_IME_DIALOG_ERROR_PARAM)
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::Ime, false); r < 0) return r;
    d.button = kImeButtonEnter;
    // (drawn: what the dialog shows, its text from the initial text; SceImeDialogParam: input method, languages
    // (u64 at 8), forced, type, option, filter, dialog mode, text box mode, ..., the enter key's label at 0x84)
    if (d.host) {
        DialogView& v = d.view;
        v.kind = DialogView::Kind::Ime;
        v.title = utf16_text(word(p + 0x28), 128);
        v.ime_text = utf16_text(initial, max_length);
        v.max_length = max_length;
        v.languages = u64{word(p + 8)} | u64{word(p + 12)} << 32;
        v.ime_type = word(p + 20), v.ime_option = word(p + 24);
        v.with_cancel = word(p + 32) == 1;
        v.password = (word(p + 36) & 1) != 0;
        v.enter_label = memory().host(p + 0x84)[0];
        d.ime_buffer = buffer, d.ime_max = max_length;
        nx::logi(logs::hle, "text input dialog \"{}\": \"{}\" (shown)", v.title, v.ime_text);
        return 0;
    }
    // (the text as given: the initial text, cut to the longest allowed)
    u32 n = 0;
    for (; initial && n < max_length && memory().valid(initial + 2 * n, 2); ++n) {
        const u32 c = psv_ld16(initial + 2 * n);
        if (!c) break;
        psv_st16(buffer + 2 * n, c);
    }
    psv_st16(buffer + 2 * n, 0);
    nx::logi(logs::hle, "text input dialog \"{}\": \"{}\" confirmed", utf16_text(word(p + 0x28), 128), utf16_text(buffer, max_length));
    return 0;
}
i32 sceImeDialogGetStatus() { return status_of(Kind::Ime); }
// SceImeDialogResult: the result, the button (enter), reserved.
i32 sceImeDialogGetResult(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 0x24)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::Ime || d.status == kStatusNone) return kNotInUse;
    if (d.status != kStatusFinished) return kNotFinished;
    std::memset(result.get(), 0, 0x24);
    psv_st32(result.address, static_cast<u32>(d.result));
    psv_st32(result.address + 4, static_cast<u32>(d.button));
    return 0;
}
i32 sceImeDialogAbort() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::Ime || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
    d.result = kResultAborted;
    d.button = 0;
    return 0;
}
i32 sceImeDialogTerm() { return term(Kind::Ime); }

// ---- save data dialog

// The system's own wording (English; not the console's) for the save data dialog's messages, by what it is for
// (1 save, 2 load, 3 delete).
nx::string save_system_text(i32 kind, i32 value, i32 type) {
    const char* doing = type == 2 ? "Loading" : type == 3 ? "Deleting" : "Saving";
    switch (kind) {
        case 1: return "There is no saved data.";
        case 2:
            return type == 2 ? "Do you want to load this saved data?"
                   : type == 3 ? "Do you want to delete this saved data?"
                               : "Do you want to save?";
        case 3: return "Do you want to overwrite this saved data?";
        case 4: return nx::format("There is not enough free space on the memory card.\n{} KB more is needed.", value);
        case 5: return nx::format("{}...\nDo not turn off the system.", doing);
        case 6: return type == 2 ? "Loading complete." : type == 3 ? "Deletion complete." : "Saving complete.";
        case 7: return "Do you want to cancel?";
        case 8: return "The saved data is corrupted.";
        case 9:
            return nx::format("There is not enough free space on the memory card ({} KB more is needed).\nYou can "
                              "continue without saving.",
                              value);
        case 10: case 11: return "Insert a memory card.";
        case 12: return "There is no saved data to import.";
        default: return nx::format("(system message {})", kind);
    }
}
DialogView::Buttons save_system_buttons(i32 kind) {
    switch (kind) {
        case 2: case 3: case 7: return DialogView::Buttons::YesNo;
        case 5: return DialogView::Buttons::None;
        default: return DialogView::Buttons::Ok;
    }
}

// A slot as the dialog shows it: SceAppUtilSaveDataSlot (id, status, user parameter, the empty slot's parameters:
// title, icon path, icon buffer and size) at `address`, its saved data's parameters if it has some.
DialogSlot dialog_slot(u32 address) {
    DialogSlot s;
    s.id = word(address);
    SaveData::SlotParam param{};
    if (find_slot(s.id, &param)) {
        s.exists = true;
        s.broken = param.status == SaveData::kBroken;
        s.title = nx::string(param.title, strnlen(param.title, sizeof param.title));
        s.subtitle = nx::string(param.subtitle, strnlen(param.subtitle, sizeof param.subtitle));
        s.detail = nx::string(param.detail, strnlen(param.detail, sizeof param.detail));
        const io::DateTime& m = param.modified;
        s.date = nx::format("{:04}/{:02}/{:02} {:02}:{:02}", m.year, m.month, m.day, m.hour, m.minute);
        s.size_kib = param.size_kib;
    } else if (const u32 empty = word(address + 12)) {
        s.title = text(word(empty), 128);
    }
    if (!s.exists && s.title.empty()) s.title = "New Saved Data";
    return s;
}

// Init starts the dialog; Continue (`continuing`) gives the running one new contents (a progress message that then
// says it is done, say), as if it had been started with them.
i32 save_data_dialog(Ptr<const u8> param, bool continuing) {
    if (!param || !memory().valid(param.address, kParamBody + 76)) return kNull;
    const u32 p = param.address;
    const i32 mode = static_cast<i32>(word(p + kParamBody));
    i32 type = static_cast<i32>(word(p + kParamBody + 4));
    if (!type && continuing) {
        Dialog& d = dialog();
        nx::scoped_lock<nx::mutex> held(d.lock);
        type = d.save_type;  // (a step continued without saying: what the dialog was started for)
    }
    const u32 fixed = word(p + kParamBody + 8), list = word(p + kParamBody + 12), user = word(p + kParamBody + 16),
              sys = word(p + kParamBody + 20), error = word(p + kParamBody + 24), progress = word(p + kParamBody + 28);
    nx::string what;
    bool waits = false;
    u32 slot = 0, list_used = 0, list_used_count = 0;
    DialogView view;
    view.type = type;
    // (the step's own slot, shown with its message: SceAppUtilSaveDataSlot at `address`)
    auto one_slot = [&](u32 address) {
        if (address) view.slots.push_back(dialog_slot(address));
    };
    switch (mode) {
        case 1:  // (fixed: its target slot)
            slot = word(fixed), what = nx::format("slot {}", slot);
            one_slot(fixed);
            view.text = save_system_text(2, 0, type);
            view.buttons = DialogView::Buttons::YesNo;
            break;
        case 2: {  // (list: the slot it focuses on is chosen)
            // SceAppUtilSaveDataSlot (16 bytes) each: the slot's ID first. The focus: the list's head or tail (0,
            // 1), its first or last empty slot (2, 3), its first or last with data (4, 5; the latest and oldest, 6
            // and 7, taken as the first), or a slot by ID (8).
            u32 slots = word(list), count = std::min<u32>(word(list + 4), 1024);
            const u32 focus = word(list + 8);
            if (!slots || !count) {
                // (continued without slots: the list shown before, as it is now)
                Dialog& d = dialog();
                nx::scoped_lock<nx::mutex> held(d.lock);
                if (continuing) slots = d.list_slots, count = d.list_count;
            }
            list_used = slots, list_used_count = count;
            nx::vector<u32> ids;
            nx::vector<bool> exists;
            nx::string listed;
            for (u32 i = 0; i < count; ++i) {
                const u32 id = word(slots + 16 * i);
                ids.push_back(id);
                exists.push_back(find_slot(id, nullptr));
                listed += nx::format("{}{}{}", i ? " " : "", id, exists.back() ? "*" : "");
            }
            const auto first = [&](bool want, bool from_tail) -> u32 {
                for (u32 k = 0; k < count; ++k) {
                    const u32 i = from_tail ? count - 1 - k : k;
                    if (exists[i] == want) return ids[i];
                }
                return count ? ids[from_tail ? count - 1 : 0] : 0;
            };
            switch (focus) {
                case 1: slot = count ? ids[count - 1] : 0; break;
                case 2: slot = first(false, false); break;
                case 3: slot = first(false, true); break;
                case 4: case 6: case 7: slot = first(true, false); break;
                case 5: slot = first(true, true); break;
                case 8: slot = word(list + 12); break;
                default: slot = count ? ids[0] : 0; break;
            }
            what = nx::format("list \"{}\" of slots {} (* with data), focus {}: slot {} chosen", text(word(list + 16), 128),
                              listed, focus, slot);
            view.title = text(word(list + 16), 128);
            view.item_style = static_cast<i32>(word(list + 20));
            for (u32 i = 0; i < count; ++i) {
                view.slots.push_back(dialog_slot(slots + 16 * i));
                if (ids[i] == slot) view.focus = i;
            }
            view.buttons = DialogView::Buttons::None;
            break;
        }
        case 3: {
            const u32 buttons = word(user);
            slot = word(user + 8);
            what = nx::format("\"{}\" (buttons {})", text(word(user + 4), 256), buttons);
            waits = buttons == 2;
            view.text = text(word(user + 4), 256);
            view.buttons = buttons == 1   ? DialogView::Buttons::YesNo
                           : buttons == 2 ? DialogView::Buttons::None
                                          : DialogView::Buttons::Ok;
            one_slot(user + 8);
            break;
        }
        case 4: {
            const u32 kind = word(sys);
            slot = word(sys + 8);
            what = nx::format("system message {} (value {})", kind, word(sys + 4));
            waits = kind == 5;  // (progress: until the game finishes it)
            view.text = save_system_text(static_cast<i32>(kind), static_cast<i32>(word(sys + 4)), type);
            view.buttons = save_system_buttons(static_cast<i32>(kind));
            if (kind != 1 && kind != 4 && kind != 10 && kind != 11 && kind != 12) one_slot(sys + 8);
            break;
        }
        case 5:
            slot = word(error + 4), what = nx::format("error code 0x{:08x}", word(error));
            view.text = error_text(word(error));
            one_slot(error + 4);
            break;
        case 6: {
            // (SceSaveDataDialogProgressBarParam: its type, a system message (56 bytes), the text, the slot)
            slot = word(progress + 64);
            what = nx::format("progress \"{}\"", text(word(progress + 60), 256)), waits = true;
            view.text = text(word(progress + 60), 256);
            if (view.text.empty()) {
                const i32 kind = static_cast<i32>(word(progress + 4));
                view.text = save_system_text(kind ? kind : 5, static_cast<i32>(word(progress + 8)), type);
            }
            view.buttons = DialogView::Buttons::None;
            view.progress = true;
            one_slot(progress + 64);
            break;
        }
        default: return err::code(0x80100B01);  // (SCE_SAVEDATA_DIALOG_ERROR_PARAM)
    }
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (continuing) {
        // (on the console a step the player answered leaves the dialog running, its sub-status finished; here it
        // finishes, so a finished dialog not yet ended continues too: the next step of the same dialog)
        if (d.kind != Kind::SaveData || d.status == kStatusNone) return kNotRunning;
        d.status = kStatusNone;  // (started again below with the new contents)
    }
    if (const i32 r = start(Kind::SaveData, waits); r < 0) return r;
    d.mode = mode;
    d.button = 1;  // (OK, Yes)
    d.slot = slot;
    d.userdata = word(p + kParamBody + 40);
    d.slot_exists = find_slot(slot, &d.slot_param);
    view.serial = d.serial, view.enter_circle = d.enter_circle;
    if (mode == 2 && list_used_count) d.list_slots = list_used, d.list_count = list_used_count;
    if (type) d.save_type = type;
    view.kind = DialogView::Kind::SaveData;
    view.mode = mode;
    d.view = std::move(view);
    nx::logi(logs::hle, "save data dialog{} ({}): {}{}", continuing ? " continued" : "",
             type == 1 ? "save" : type == 2 ? "load" : type == 3 ? "delete" : "-", what,
             waits ? " (stays up until finished)" : d.host ? " (shown)" : " (answered OK/Yes)");
    return 0;
}
i32 sceSaveDataDialogInit(Ptr<const u8> param) { return save_data_dialog(param, false); }
i32 sceSaveDataDialogContinue(Ptr<const u8> param) { return save_data_dialog(param, true); }

// Closes the part of the dialog shown over the game's own (its sub-status finished); the dialog stays running.
i32 sceSaveDataDialogSubClose() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status == kStatusNone) return kNotRunning;
    // (drawn: a step the player answers is the player's to close; a game's work finished behind a progress step
    // often closes it just after it has gone on to the next)
    d.updated = true;
    if (d.waits) d.answered = true;
    return 0;
}

i32 sceSaveDataDialogGetStatus() { return status_of(Kind::SaveData); }

// (shown: the part of it the game waits for before it goes on, e.g. saving behind a progress message)
i32 sceSaveDataDialogGetSubStatus() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status == kStatusNone) return kStatusNone;
    return answered(d) || d.status == kStatusFinished ? kStatusFinished : kStatusRunning;
}

i32 sceSaveDataDialogFinish(Ptr<const void> param) {
    if (!param) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status == kStatusNone) return kNotRunning;
    d.status = kStatusFinished;  // (already, when its last step was answered)
    return 0;
}

// SceSaveDataDialogResult: mode, result, button, slot, its information (the game's: whether it exists, its
// parameters), the game's user data.
i32 sceSaveDataDialogGetResult(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 56)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status == kStatusNone) return kNotInUse;
    // (a running dialog answers too once the part shown has finished: its sub-status, as on the console)
    if (d.status != kStatusFinished && !(d.status == kStatusRunning && answered(d))) return kNotFinished;
    const u32 r = result.address;
    psv_st32(r, static_cast<u32>(d.mode));
    psv_st32(r + 4, static_cast<u32>(d.result));
    psv_st32(r + 8, static_cast<u32>(d.button));
    psv_st32(r + 12, d.slot);
    if (const u32 info = word(r + 16); info && writable(info, 8)) {
        psv_st32(info, d.slot_exists ? 1 : 0);
        if (const u32 slot_param = word(info + 4); slot_param && d.slot_exists && writable(slot_param, sizeof(SaveData::SlotParam)))
            std::memcpy(memory().host(slot_param), &d.slot_param, sizeof(SaveData::SlotParam));
    }
    psv_st32(r + 20, d.userdata);
    nx::logd(logs::hle, "save data dialog result: mode {}, result {}, button {}, slot {} ({})", d.mode, d.result, d.button,
             d.slot, d.slot_exists ? "with data" : "empty");
    return 0;
}

i32 sceSaveDataDialogTerm() { return term(Kind::SaveData); }

// ---- network check dialog

i32 sceNetCheckDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, kParamBody + 4)) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::NetCheck, false); r < 0) return r;
    d.mode = static_cast<i32>(word(param.address + kParamBody));
    d.result = kResultUserCanceled;
    d.view.kind = DialogView::Kind::NetCheck;
    d.view.mode = d.mode;
    d.view.text = "This system is not connected to a network.\nConnect to a network to use this feature.";
    nx::logi(logs::hle, "network check dialog (mode {}): offline, the player declines to connect", d.mode);
    return 0;
}
i32 sceNetCheckDialogGetStatus() { return status_of(Kind::NetCheck); }
// SceNetCheckDialogResult: the result, whether PSN was reached.
i32 sceNetCheckDialogGetResult(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 8)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::NetCheck || d.status == kStatusNone) return kNotInUse;
    if (d.status != kStatusFinished) return kNotFinished;
    psv_st32(result.address, static_cast<u32>(d.result));
    psv_st32(result.address + 4, 0);
    return 0;
}
i32 sceNetCheckDialogTerm() { return term(Kind::NetCheck); }

// ---- trophy setup dialog

i32 sceNpTrophySetupDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, kParamBody + 8)) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::TrophySetup, false); r < 0) return r;
    d.view.kind = DialogView::Kind::TrophySetup;
    d.view.text = "Preparing trophies...";
    d.view.buttons = DialogView::Buttons::None;
    nx::logi(logs::hle, "trophy setup dialog: done (trophies are local)");
    return 0;
}
i32 sceNpTrophySetupDialogGetStatus() { return status_of(Kind::TrophySetup); }
i32 sceNpTrophySetupDialogGetResult(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 4)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::TrophySetup || d.status == kStatusNone) return kNotInUse;
    if (d.status != kStatusFinished) return kNotFinished;
    psv_st32(result.address, kResultOk);
    return 0;
}
i32 sceNpTrophySetupDialogTerm() { return term(Kind::TrophySetup); }

// ---- aborts, and the message dialog closed by the game

i32 abort_dialog(Kind kind) {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != kind || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
    d.result = kResultAborted;
    return 0;
}
i32 sceNetCheckDialogAbort() { return abort_dialog(Kind::NetCheck); }
i32 sceNpTrophySetupDialogAbort() { return abort_dialog(Kind::TrophySetup); }
i32 sceSaveDataDialogAbort() { return abort_dialog(Kind::SaveData); }
// (a message that waits, closed: finished, no button chosen)
i32 sceMsgDialogClose() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::Message || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
    d.result = kResultOk;
    d.button = 0;
    return 0;
}

// ---- the PSN, store and photo dialogs: the console is signed out (and has no photos), so each is shown and the
// player backs out of it: finished, "user canceled". Their results begin with that result word.

enum class Other : u8 { NpMessage = 1, NpProfile, StoreCheckout, NpFriendList, NpSnsFacebook, PhotoImport, GameCustomData,
                        Invitation };
const char* other_name(Other o) {
    switch (o) {
        case Other::NpMessage: return "NP message";
        case Other::NpProfile: return "NP profile";
        case Other::StoreCheckout: return "store checkout";
        case Other::NpFriendList: return "NP friend list";
        case Other::NpSnsFacebook: return "Facebook";
        case Other::PhotoImport: return "photo import";
        case Other::GameCustomData: return "game custom data";
        case Other::Invitation: return "invitation";
    }
    return "?";
}
template <Other O>
i32 other_init(Ptr<const u8> param) {
    if (!param) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::Other, false); r < 0) return r;
    d.other = static_cast<u8>(O);
    d.result = kResultUserCanceled;
    nx::logi(logs::hle, "{} dialog: signed out, the player backs out of it", other_name(O));
    return 0;
}
template <Other O>
bool is_other(const Dialog& d) {
    return d.kind == Kind::Other && d.other == static_cast<u8>(O);
}
template <Other O>
i32 other_status() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    return is_other<O>(d) ? d.status : kStatusNone;
}
template <Other O>
i32 other_result(Ptr<u8> result) {
    if (!result) return kNull;
    if (!writable(result.address, 4)) return err::code(0x80020403);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (!is_other<O>(d) || d.status == kStatusNone) return kNotInUse;
    if (d.status != kStatusFinished) return kNotFinished;
    psv_st32(result.address, static_cast<u32>(d.result));
    return 0;
}
template <Other O>
i32 other_term() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (!is_other<O>(d) || d.status == kStatusNone) return kNotInUse;
    if (d.status == kStatusRunning) return kNotFinished;
    d.kind = Kind::None;
    d.status = kStatusNone;
    return 0;
}
template <Other O>
i32 other_abort() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (!is_other<O>(d) || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
    d.result = kResultAborted;
    return 0;
}
// (functions known only by NID, imported by a game's own module: no dialog of theirs is in use)
i32 dialog_unnamed() { return kNotInUse; }


const Function kCommonDialogMore[] = {
    HLE_FUNCTION(0x2D8EDF09, sceNetCheckDialogAbort),
    HLE_FUNCTION(0xDC346979, sceNpTrophySetupDialogAbort),
    HLE_FUNCTION(0x013E7F74, sceSaveDataDialogAbort),
    HLE_FUNCTION(0xC296D396, sceMsgDialogClose),
    Function{0x4535A358, "sceNpMessageDialogInit", &entry<&other_init<Other::NpMessage>>},
    Function{0x2A0D060F, "sceNpMessageDialogGetStatus", &entry<&other_status<Other::NpMessage>>},
    Function{0x7EC95C61, "sceNpMessageDialogGetResult", &entry<&other_result<Other::NpMessage>>},
    Function{0x7AB50F63, "sceNpMessageDialogTerm", &entry<&other_term<Other::NpMessage>>},
    Function{0x47AB6D04, "sceNpMessageDialogAbort", &entry<&other_abort<Other::NpMessage>>},
    Function{0x5E0AFDF8, "sceNpProfileDialogInit", &entry<&other_init<Other::NpProfile>>},
    Function{0xFDFE6042, "sceNpProfileDialogGetStatus", &entry<&other_status<Other::NpProfile>>},
    Function{0x749CAFFC, "sceNpProfileDialogGetResult", &entry<&other_result<Other::NpProfile>>},
    Function{0x04B63D6F, "sceNpProfileDialogTerm", &entry<&other_term<Other::NpProfile>>},
    Function{0x2B02BE3F, "sceNpProfileDialogAbort", &entry<&other_abort<Other::NpProfile>>},
    Function{0x52ECD8A5, "sceStoreCheckoutDialogInit", &entry<&other_init<Other::StoreCheckout>>},
    Function{0x7004BB2E, "sceStoreCheckoutDialogGetStatus", &entry<&other_status<Other::StoreCheckout>>},
    Function{0x07ED1E26, "sceStoreCheckoutDialogGetResult", &entry<&other_result<Other::StoreCheckout>>},
    Function{0xB787F4B0, "sceStoreCheckoutDialogTerm", &entry<&other_term<Other::StoreCheckout>>},
    Function{0x93FCFEC6, "sceNpFriendListDialogInit", &entry<&other_init<Other::NpFriendList>>},
    Function{0x1FD5D373, "sceNpFriendListDialogGetStatus", &entry<&other_status<Other::NpFriendList>>},
    Function{0xD29FE607, "sceNpFriendListDialogGetResult", &entry<&other_result<Other::NpFriendList>>},
    Function{0x4A880C6A, "sceNpFriendListDialogTerm", &entry<&other_term<Other::NpFriendList>>},
    Function{0x58FA2062, "sceNpFriendListDialogAbort", &entry<&other_abort<Other::NpFriendList>>},
    Function{0x6821F09B, "sceNpSnsFacebookDialogInit", &entry<&other_init<Other::NpSnsFacebook>>},
    Function{0x147650E8, "sceNpSnsFacebookDialogGetStatus", &entry<&other_status<Other::NpSnsFacebook>>},
    Function{0xA8682304, "sceNpSnsFacebookDialogGetResultLongToken", &entry<&other_result<Other::NpSnsFacebook>>},
    Function{0xDE1F3928, "sceNpSnsFacebookDialogTerm", &entry<&other_term<Other::NpSnsFacebook>>},
    Function{0x73EE7C9C, "scePhotoImportDialogInit", &entry<&other_init<Other::PhotoImport>>},
    Function{0x032206D8, "scePhotoImportDialogGetStatus", &entry<&other_status<Other::PhotoImport>>},
    Function{0xD855414C, "scePhotoImportDialogGetResult", &entry<&other_result<Other::PhotoImport>>},
    Function{0x7FE5BD77, "scePhotoImportDialogTerm", &entry<&other_term<Other::PhotoImport>>},
    Function{0x33626E2C, "(SceCommonDialog 0x33626E2C)", &entry<&dialog_unnamed>},
    Function{0xB55CF0E3, "(SceCommonDialog 0xB55CF0E3)", &entry<&dialog_unnamed>},
    Function{0xB7D4C911, "(SceCommonDialog 0xB7D4C911)", &entry<&dialog_unnamed>},
    Function{0xBEBAEF25, "(SceCommonDialog 0xBEBAEF25)", &entry<&dialog_unnamed>},
    Function{0xD1858A78, "(SceCommonDialog 0xD1858A78)", &entry<&dialog_unnamed>},
};
// (the web API's dialogs: their own library)
const Function kNpWebApiCommonDialog[] = {
    Function{0x23C9840E, "sceGameCustomDataDialogInit", &entry<&other_init<Other::GameCustomData>>},
    Function{0x114F831A, "sceGameCustomDataDialogGetStatus", &entry<&other_status<Other::GameCustomData>>},
    Function{0xC2B39806, "sceGameCustomDataDialogGetResult", &entry<&other_result<Other::GameCustomData>>},
    Function{0xE183B9F3, "sceGameCustomDataDialogTerm", &entry<&other_term<Other::GameCustomData>>},
    Function{0xF5A1FA42, "sceGameCustomDataDialogAbort", &entry<&other_abort<Other::GameCustomData>>},
    Function{0x8723A5F2, "sceInvitationDialogInit", &entry<&other_init<Other::Invitation>>},
    Function{0x5B735F60, "sceInvitationDialogGetStatus", &entry<&other_status<Other::Invitation>>},
    Function{0xEA14FB1E, "sceInvitationDialogGetResult", &entry<&other_result<Other::Invitation>>},
    Function{0x4136382C, "sceInvitationDialogTerm", &entry<&other_term<Other::Invitation>>},
    Function{0x853E5D3B, "sceInvitationDialogAbort", &entry<&other_abort<Other::Invitation>>},
};

const Function kCommonDialog[] = {
    HLE_FUNCTION(0x90530F2F, sceCommonDialogUpdate),
    HLE_FUNCTION(0x755FF270, sceMsgDialogInit),
    HLE_FUNCTION(0x4107019E, sceMsgDialogGetStatus),
    HLE_FUNCTION(0x81ACF695, sceMsgDialogTerm),
    HLE_FUNCTION(0xBB3BFC89, sceMsgDialogGetResult),
    HLE_FUNCTION(0x0CC66115, sceMsgDialogAbort),
    HLE_FUNCTION(0xBECD35C8, sceCommonDialogSetConfigParam),
    HLE_FUNCTION(0x043A353E, sceCommonDialogIsRunning),
    HLE_FUNCTION(0x7BE0E08B, sceMsgDialogProgressBarInc),
    HLE_FUNCTION(0x9CDA5E0D, sceMsgDialogProgressBarSetValue),
    HLE_FUNCTION(0x0150A451, sceMsgDialogProgressBarSetMsg),
    HLE_FUNCTION(0xBDE00A83, sceSaveDataDialogProgressBarInc),
    HLE_FUNCTION(0x5C322D1E, sceSaveDataDialogProgressBarSetValue),
    HLE_FUNCTION(0x1E7043BF, sceImeDialogInit),
    HLE_FUNCTION(0xCF0431FD, sceImeDialogGetStatus),
    HLE_FUNCTION(0x2EB3D046, sceImeDialogGetResult),
    HLE_FUNCTION(0x594A220E, sceImeDialogAbort),
    HLE_FUNCTION(0x838A3AF4, sceImeDialogTerm),
    HLE_FUNCTION(0xBF5248FA, sceSaveDataDialogInit),
    HLE_FUNCTION(0x6E258046, sceSaveDataDialogGetStatus),
    HLE_FUNCTION(0xBA0542CA, sceSaveDataDialogGetSubStatus),
    HLE_FUNCTION(0x6C49924B, sceSaveDataDialogFinish),
    HLE_FUNCTION(0xB2FF576E, sceSaveDataDialogGetResult),
    HLE_FUNCTION(0x2192A10A, sceSaveDataDialogTerm),
    HLE_FUNCTION(0x19192C8B, sceSaveDataDialogContinue),
    HLE_FUNCTION(0x415D6068, sceSaveDataDialogSubClose),
    HLE_FUNCTION(0xA38A4A0D, sceNetCheckDialogInit),
    HLE_FUNCTION(0x8027292A, sceNetCheckDialogGetStatus),
    HLE_FUNCTION(0xB05FCE9E, sceNetCheckDialogGetResult),
    HLE_FUNCTION(0x8BE51C15, sceNetCheckDialogTerm),
    HLE_FUNCTION(0x9E2C02C9, sceNpTrophySetupDialogInit),
    HLE_FUNCTION(0xC3A59547, sceNpTrophySetupDialogGetStatus),
    HLE_FUNCTION(0xE37069D5, sceNpTrophySetupDialogGetResult),
    HLE_FUNCTION(0xA81082DD, sceNpTrophySetupDialogTerm),
};

}  // namespace

// ---- the host's side (runtime/dialogs.hpp)

namespace {

struct Notices {
    nx::mutex lock;
    nx::vector<SystemNotice> waiting;
};
Notices& notices() {
    static Notices n;
    return n;
}

}  // namespace

}  // namespace psvrecore::runtime::hle

namespace psvrecore::runtime::dialogs {

using hle::dialog;
using hle::Dialog;
using hle::Kind;

void set_drawn(bool drawn) { hle::g_drawn.store(drawn); }
bool drawn() { return hle::g_drawn.load(); }

bool current(DialogView* out) {
    {
        Dialog& d = dialog();
        nx::scoped_lock<nx::mutex> held(d.lock);
        // (answered: until the game goes on, nothing more to show)
        if (d.status == hle::kStatusRunning && d.host && d.view.kind != DialogView::Kind::None && !(d.answered && !d.waits)) {
            *out = d.view;
            return true;
        }
    }
    return hle::ime_keyboard_view(out);
}

void edit(u64 serial, const nx::string& text, u32 caret) { hle::ime_keyboard_edit(serial, text, caret); }

void answer(u64 serial, const DialogAnswer& a) {
    if (serial >> 63) return hle::ime_keyboard_answer(serial, a);
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.serial != serial || d.status != hle::kStatusRunning || !d.host || d.answered) return;
    d.answered = d.updated = true;
    d.result = a.result;
    d.button = a.button;
    switch (d.kind) {
        case Kind::Message: break;
        case Kind::SaveData:
            if (d.mode == 2 && a.result == hle::kResultOk) {
                d.slot = a.slot;
                d.slot_exists = hle::find_slot(d.slot, &d.slot_param);
            }
            if (a.result != hle::kResultOk) d.button = 0;
            break;
        case Kind::NetCheck: d.result = hle::kResultUserCanceled; break;  // (offline, whatever is pressed)
        case Kind::TrophySetup: d.result = hle::kResultOk; break;
        case Kind::Ime:
            // (the text typed, Enter or Close; backing out of a dialog with a cancel button leaves the game's text)
            if (d.ime_buffer && !(d.view.with_cancel && a.result != hle::kResultOk))
                hle::store_utf16(d.ime_buffer, d.ime_max, a.text);
            if (a.result != hle::kResultOk) d.button = 0;
            break;
        default: break;
    }
    // (a step answered finishes it, as an answered dialog does at its update)
    if (!d.waits) d.status = hle::kStatusFinished;
    nx::logi(logs::hle, "dialog answered: result {}, button {}{}", d.result, d.button,
             d.kind == Kind::SaveData ? nx::format(", slot {}", d.slot)
             : d.kind == Kind::Ime    ? nx::format(", text \"{}\"", hle::utf16_text(d.ime_buffer, d.ime_max))
                                      : nx::string());
}

DialogView::Kind showing() {
    {
        Dialog& d = dialog();
        nx::scoped_lock<nx::mutex> held(d.lock);
        if (d.status == hle::kStatusRunning && d.host && !(d.answered && !d.waits) && d.view.kind != DialogView::Kind::None)
            return d.view.kind;
    }
    return hle::ime_keyboard_view(nullptr) ? DialogView::Kind::InGameIme : DialogView::Kind::None;
}

void notify(SystemNotice notice) {
    nx::logi(logs::hle, "notice: {}: {}", notice.title, notice.text);
    if (!drawn()) return;
    hle::Notices& n = hle::notices();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (n.waiting.size() < 32) n.waiting.push_back(std::move(notice));
}

bool has_notice() {
    hle::Notices& n = hle::notices();
    nx::scoped_lock<nx::mutex> held(n.lock);
    return !n.waiting.empty();
}

bool take_notice(SystemNotice* out) {
    hle::Notices& n = hle::notices();
    nx::scoped_lock<nx::mutex> held(n.lock);
    if (n.waiting.empty()) return false;
    *out = std::move(n.waiting.front());
    n.waiting.erase(n.waiting.begin());
    return true;
}

}  // namespace psvrecore::runtime::dialogs

namespace psvrecore::runtime::hle {

void register_commondialog() {
    {
        Dialog& d = dialog();
        nx::scoped_lock<nx::mutex> held(d.lock);
        d.kind = Kind::None;
        d.status = kStatusNone;
    }
    register_library({"SceCommonDialog", kSceCommonDialog, kCommonDialog});
    register_library({"SceCommonDialog", kSceCommonDialog, kCommonDialogMore});
    register_library({"SceNpWebApiCommonDialog", kSceNpWebApiCommonDialog, kNpWebApiCommonDialog});
}

}  // namespace psvrecore::runtime::hle
