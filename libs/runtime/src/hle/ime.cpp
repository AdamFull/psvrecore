// The in-game keyboard (SceIme): opened over the game's own text field, it reports to the game's handler from
// sceImeUpdate. There is no keyboard here: as with the text input dialog, the text is confirmed as the game gave it.
// The first update after opening reports the keyboard open (its area: the bottom of the screen), the next one Enter
// pressed, with the text in the game's buffer (its initial text, or what it set since). Nothing more follows until it
// is closed and opened again.

#include "hle_common.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceIme = 0xD2553017;
constexpr i32 kAlreadyOpened = err::code(0x80100700), kNotOpened = err::code(0x80100701),
              kInvalidPointer = err::code(0x80100702), kInvalidType = err::code(0x80100714),
              kInvalidWork = err::code(0x80100716), kInvalidHandler = err::code(0x80100718),
              kInvalidMaxTextLength = err::code(0x80100719), kInvalidInputTextBuffer = err::code(0x8010071A),
              kInvalidEnterLabel = err::code(0x8010071C);
constexpr u32 kMaxTextLength = 2048;
constexpr u32 kEventOpen = 0, kEventPressEnter = 5;
// (the keyboard's area as reported on opening: the bottom half of the screen)
constexpr u32 kKeyboardX = 0, kKeyboardY = 272, kKeyboardWidth = 960, kKeyboardHeight = 272;

struct Ime {
    nx::mutex lock;
    bool open = false;
    u32 updates = 0;
    u32 arg = 0, handler = 0, max_length = 0, buffer = 0;
    u32 event = 0;  // (guest memory for the event handed to the handler: id, then 40 bytes of parameters)
};
Ime& ime() {
    static Ime i;
    return i;
}

// (UTF-16 text to the game's buffer, cut to the longest allowed, terminated)
void store_text(const Ime& s, u32 text, u32 length) {
    u32 n = 0;
    for (; text && n < length && n < s.max_length && memory().valid(text + 2 * n, 2); ++n) {
        const u32 c = psv_ld16(text + 2 * n);
        if (!c) break;
        psv_st16(s.buffer + 2 * n, c);
    }
    psv_st16(s.buffer + 2 * n, 0);
}

// SceImeParam (64 bytes): SDK version, input method, languages (u64 at 8), languages forced, type, option, work area,
// handler's argument, handler, text filter, initial text, longest text, the text buffer, the Enter key's label.
i32 sceImeOpen(Ptr<const u8> param) {
    if (!param || !memory().valid(param.address, 64)) return kInvalidPointer;
    const u32 p = param.address;
    const u32 type = psv_ld32(p + 20), work = psv_ld32(p + 28), arg = psv_ld32(p + 32), handler = psv_ld32(p + 36);
    const u32 initial = psv_ld32(p + 44), max_length = psv_ld32(p + 48), buffer = psv_ld32(p + 52);
    const u32 enter_label = memory().host(p + 56)[0];
    if (type > 5) return kInvalidType;
    if (!work) return kInvalidWork;
    if (!handler) return kInvalidHandler;
    if (!max_length || max_length > kMaxTextLength) return kInvalidMaxTextLength;
    if (!buffer || !writable(buffer, 2 * (max_length + 1))) return kInvalidInputTextBuffer;
    if (enter_label > 3) return kInvalidEnterLabel;
    Ime& s = ime();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (s.open) return kAlreadyOpened;
    if (!s.event) s.event = runtime().allocate_system(44, "IME event");
    s.open = true, s.updates = 0;
    s.arg = arg, s.handler = handler, s.max_length = max_length, s.buffer = buffer;
    store_text(s, initial, max_length);
    nx::logi(logs::hle, "in-game keyboard opened (type {}, up to {} characters): its text confirmed as given", type, max_length);
    return 0;
}

i32 sceImeUpdate(PsvContext* ctx) {
    u32 handler, arg, event;
    {
        Ime& s = ime();
        nx::scoped_lock<nx::mutex> held(s.lock);
        if (!s.open) return kNotOpened;
        if (s.updates >= 2) return 0;
        const u32 id = s.updates++ == 0 ? kEventOpen : kEventPressEnter;
        u8* e = memory().host(s.event);
        std::memset(e, 0, 44);
        psv_st32(s.event, id);
        if (id == kEventOpen) {
            psv_st32(s.event + 4, kKeyboardX);
            psv_st32(s.event + 8, kKeyboardY);
            psv_st32(s.event + 12, kKeyboardWidth);
            psv_st32(s.event + 16, kKeyboardHeight);
        }
        handler = s.handler, arg = s.arg, event = s.event;
    }
    const u32 args[] = {arg, event};
    call_guest(ctx, handler, args);
    return 0;
}

i32 sceImeSetText(u32 text, u32 length) {
    Ime& s = ime();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.open) return kNotOpened;
    if (!text) return kInvalidPointer;
    store_text(s, text, length);
    return 0;
}

// SceImeCaret: x, y, height, index (the keyboard has no caret to show).
i32 sceImeSetCaret(Ptr<const u8> caret) {
    Ime& s = ime();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.open) return kNotOpened;
    if (!caret || !memory().valid(caret.address, 16)) return kInvalidPointer;
    return 0;
}
// SceImePreeditGeometry: x, y, height.
i32 sceImeSetPreeditGeometry(Ptr<const u8> geometry) {
    Ime& s = ime();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.open) return kNotOpened;
    if (!geometry || !memory().valid(geometry.address, 12)) return kInvalidPointer;
    return 0;
}

i32 sceImeClose() {
    Ime& s = ime();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.open) return kNotOpened;
    s.open = false;
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x0E050613, sceImeOpen),
    HLE_FUNCTION(0x71D6898A, sceImeUpdate),
    HLE_FUNCTION(0xF3BD9A76, sceImeSetText),
    HLE_FUNCTION(0xD8342D2A, sceImeSetCaret),
    HLE_FUNCTION(0x7B1EFAA5, sceImeSetPreeditGeometry),
    HLE_FUNCTION(0x889A8421, sceImeClose),
};

}  // namespace

void register_ime() {
    {
        Ime& s = ime();
        nx::scoped_lock<nx::mutex> held(s.lock);
        s.open = false, s.event = 0;
    }
    register_library({"SceIme", kSceIme, kFunctions});
}

}  // namespace psvrecore::runtime::hle
