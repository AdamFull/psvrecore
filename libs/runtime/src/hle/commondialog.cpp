// The system's dialogs (SceCommonDialog): the message, save data, network check and trophy setup dialogs, without
// drawing anything. One dialog at a time, as on the console: Init starts it (running), and the game's calls to
// sceCommonDialogUpdate (each frame, to draw it) let it finish, with the answer a player gives most often:
// - messages with buttons: OK or Yes; messages that only wait (progress, "please wait") stay up until the game
//   closes them (sceSaveDataDialogFinish), as on the console;
// - save data: confirmations are confirmed, the fixed slot is the one the game named (it exists if its save data
//   does: savedata0:'s slots), the list's focused slot is chosen;
// - network check: offline, the player declines to connect (user canceled, PSN not reached);
// - trophy setup: done (trophies are local).
// Every message's text is logged, to see what the game asks.

#include "hle_common.hpp"

#include <cstring>
#include "psvrecore/runtime/savedata.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceCommonDialog = 0xE537816C;
constexpr i32 kBusy = err::code(0x80020401), kNull = err::code(0x80020402), kNotRunning = err::code(0x80020404),
              kNotFinished = err::code(0x80020410), kNotInUse = err::code(0x80020411);
constexpr i32 kStatusNone = 0, kStatusRunning = 1, kStatusFinished = 2;
constexpr i32 kResultOk = 0, kResultUserCanceled = 1;
constexpr u32 kCommonParam = 4;  // (every dialog's parameters: the SDK version, then SceCommonDialogParam (76 bytes))
constexpr u32 kParamBody = kCommonParam + 76;

enum class Kind : u8 { None, Message, SaveData, NetCheck, TrophySetup };

struct Dialog {
    nx::mutex lock;
    Kind kind = Kind::None;
    i32 status = kStatusNone;
    bool waits = false;    // (until the game closes it)
    bool updated = false;  // (shown: an update came since it started)
    i32 mode = 0, result = kResultOk, button = 0;
    u32 slot = 0, userdata = 0;
    bool slot_exists = false;
    SaveData::SlotParam slot_param{};
};
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
    return 0;
}

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

// Whether the slot's save data exists (savedata0:), and its parameters.
bool find_slot(u32 id, SaveData::SlotParam* out) {
    nx::string host;
    if (runtime().files().resolve("savedata0:", &host, nullptr) < 0) return false;
    SaveData data(host);
    return data.ok() && data.slot(id, out);
}

i32 sceCommonDialogUpdate(Ptr<const void> param) {
    if (!param) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.status == kStatusRunning) {
        d.updated = true;
        if (!d.waits) d.status = kStatusFinished;
    }
    return 0;
}

// ---- message dialog

i32 sceMsgDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, kParamBody + 24)) return kNull;
    const u32 p = param.address;
    const i32 mode = static_cast<i32>(word(p + kParamBody));
    nx::string what;
    bool waits = false;
    switch (mode) {
        case 1: {  // (user message: its buttons, its text)
            const u32 user = word(p + kParamBody + 4);
            what = nx::format("\"{}\" (buttons {})", text(word(user + 4), 512), word(user));
            waits = word(user) == 2;  // (no buttons: until closed)
            break;
        }
        case 2: what = nx::format("system message {}", word(word(p + kParamBody + 8))), waits = true; break;
        case 3: what = nx::format("error code 0x{:08x}", word(word(p + kParamBody + 12))); break;
        case 4: what = nx::format("progress \"{}\"", text(word(word(p + kParamBody + 16) + 48), 512)), waits = true; break;
        default: return err::code(0x80100A01);  // (SCE_MSG_DIALOG_ERROR_PARAM)
    }
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::Message, waits); r < 0) return r;
    d.mode = mode;
    d.button = 1;  // (OK, Yes)
    nx::logi(logs::hle, "message dialog: {}{}", what, waits ? " (stays up until closed)" : " (answered OK)");
    return 0;
}
i32 sceMsgDialogGetStatus() { return status_of(Kind::Message); }
i32 sceMsgDialogTerm() { return term(Kind::Message); }

// ---- save data dialog

i32 sceSaveDataDialogInit(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, kParamBody + 76)) return kNull;
    const u32 p = param.address;
    const i32 mode = static_cast<i32>(word(p + kParamBody)), type = static_cast<i32>(word(p + kParamBody + 4));
    const u32 fixed = word(p + kParamBody + 8), list = word(p + kParamBody + 12), user = word(p + kParamBody + 16),
              sys = word(p + kParamBody + 20), error = word(p + kParamBody + 24), progress = word(p + kParamBody + 28);
    nx::string what;
    bool waits = false;
    u32 slot = 0;
    switch (mode) {
        case 1: slot = word(fixed), what = nx::format("slot {}", slot); break;  // (fixed: its target slot)
        case 2: {  // (list: the slot it focuses on, else the first listed)
            const u32 slots = word(list), count = word(list + 4), focus_id = word(list + 12);
            slot = word(list + 8) == 8 ? focus_id : (count ? word(slots) : 0);
            what = nx::format("list of {} \"{}\" (slot {} chosen)", count, text(word(list + 16), 128), slot);
            break;
        }
        case 3: {
            const u32 buttons = word(user);
            slot = word(user + 8);
            what = nx::format("\"{}\" (buttons {})", text(word(user + 4), 256), buttons);
            waits = buttons == 2;
            break;
        }
        case 4: {
            const u32 kind = word(sys);
            slot = word(sys + 8);
            what = nx::format("system message {} (value {})", kind, word(sys + 4));
            waits = kind == 5;  // (progress: until the game finishes it)
            break;
        }
        case 5: slot = word(error + 4), what = nx::format("error code 0x{:08x}", word(error)); break;
        case 6: slot = word(progress + 52), what = nx::format("progress \"{}\"", text(word(progress + 48), 256)), waits = true; break;
        default: return err::code(0x80100B01);  // (SCE_SAVEDATA_DIALOG_ERROR_PARAM)
    }
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (const i32 r = start(Kind::SaveData, waits); r < 0) return r;
    d.mode = mode;
    d.button = 1;  // (OK, Yes)
    d.slot = slot;
    d.userdata = word(p + kParamBody + 40);
    d.slot_exists = find_slot(slot, &d.slot_param);
    nx::logi(logs::hle, "save data dialog ({}): {}{}", type == 1 ? "save" : type == 2 ? "load" : type == 3 ? "delete" : "-", what,
             waits ? " (stays up until finished)" : " (answered OK/Yes)");
    return 0;
}

i32 sceSaveDataDialogGetStatus() { return status_of(Kind::SaveData); }

// (shown: the part of it the game waits for before it goes on, e.g. saving behind a progress message)
i32 sceSaveDataDialogGetSubStatus() {
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status == kStatusNone) return kStatusNone;
    return d.updated || d.status == kStatusFinished ? kStatusFinished : kStatusRunning;
}

i32 sceSaveDataDialogFinish(Ptr<const void> param) {
    if (!param) return kNull;
    Dialog& d = dialog();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (d.kind != Kind::SaveData || d.status != kStatusRunning) return kNotRunning;
    d.status = kStatusFinished;
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
    if (d.status != kStatusFinished) return kNotFinished;
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

const Function kCommonDialog[] = {
    HLE_FUNCTION(0x90530F2F, sceCommonDialogUpdate),
    HLE_FUNCTION(0x755FF270, sceMsgDialogInit),
    HLE_FUNCTION(0x4107019E, sceMsgDialogGetStatus),
    HLE_FUNCTION(0x81ACF695, sceMsgDialogTerm),
    HLE_FUNCTION(0xBF5248FA, sceSaveDataDialogInit),
    HLE_FUNCTION(0x6E258046, sceSaveDataDialogGetStatus),
    HLE_FUNCTION(0xBA0542CA, sceSaveDataDialogGetSubStatus),
    HLE_FUNCTION(0x6C49924B, sceSaveDataDialogFinish),
    HLE_FUNCTION(0xB2FF576E, sceSaveDataDialogGetResult),
    HLE_FUNCTION(0x2192A10A, sceSaveDataDialogTerm),
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

void register_commondialog() {
    {
        Dialog& d = dialog();
        nx::scoped_lock<nx::mutex> held(d.lock);
        d.kind = Kind::None;
        d.status = kStatusNone;
    }
    register_library({"SceCommonDialog", kSceCommonDialog, kCommonDialog});
}

}  // namespace psvrecore::runtime::hle
