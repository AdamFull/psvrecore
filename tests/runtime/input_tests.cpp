// Input (SceCtrl, SceTouch): samples at the vertical blanks, their timestamps, presses between blanks, lag.

#include "nxtest.h"

#include <cmath>
#include <cstring>
#include "runtime_support.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

constexpr u32 kSceCtrl = 0xD197E3C7, kSceTouch = 0x3E4F4A81;
constexpr u32 kPeek = 0xA9C3CED6, kPeekNeg = 0x104ED1A7, kRead = 0x67E7AB83, kRead2 = 0xC4226A3E, kSetMode = 0xA497B150;
constexpr u32 kTouchInfo = 0x10A2CA25, kTouchSampling = 0x1B9C5D14, kTouchRead = 0x169A1D58, kTouchPeek = 0xFF082DF0;
constexpr u32 kCircle = 1u << 13, kCross = 1u << 14, kL = 1u << 8, kL1 = 1u << 10;

struct CtrlData {
    u64 timestamp;
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 reserved[16];
};

CtrlData data(TestRuntime& t, u32 at, u32 i) {
    CtrlData d;
    std::memcpy(&d, t.rt.memory().host(at + i * 32), 32);
    return d;
}

// The period of the vertical blanks in µs (59.94 Hz).
f64 period_us(TestRuntime& t) { return 1e6 / t.rt.config().refresh_hz; }

}  // namespace

TEST_CASE("ctrl: samples are taken at the vertical blanks, stamped with their process time") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 buf = scratch(64 * 32);
    REQUIRE(call32(ctx, kSceCtrl, kPeek, {0, buf, 1}) == 1);
    CtrlData d = data(t, buf, 0);
    CHECK(d.buttons == 0);
    CHECK(d.lx == 0x80);
    CHECK(d.ry == 0x80);
    // (on the grid of blanks: n * period, and no later than now)
    const f64 n = static_cast<f64>(d.timestamp) / period_us(t);
    CHECK(std::fabs(n - std::round(n)) * period_us(t) <= 1.0);
    CHECK(d.timestamp <= t.rt.kernel().process_time());
    // A read waits for the next blank: one period later.
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 1}) >= 1);
    const u64 first = data(t, buf, 0).timestamp;
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 1}) == 1);
    const u64 second = data(t, buf, 0).timestamp;
    CHECK(std::fabs(static_cast<f64>(second - first) - period_us(t)) <= 1.0);
    CHECK(t.rt.kernel().process_time() >= second);
    CHECK(t.rt.kernel().process_time() - second < 4000);  // (it returned at that blank)
}

TEST_CASE("ctrl: a press and release between two blanks still shows; a slow reader gets the samples it missed") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    const u32 buf = scratch(64 * 32);
    REQUIRE(call32(ctx, kSceCtrl, kSetMode, {1}) == 0);  // (analog sticks)
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 64}) >= 1);
    // Circle pressed and released right after a blank, before the next: the next sample shows it, the one after not.
    const u64 now = k.clock_ns();
    PadState pressed;
    pressed.buttons = kCircle;
    pressed.lx = 0x10;
    t.rt.input().set_pad(pressed, now);
    PadState released;
    released.lx = 0x10;
    t.rt.input().set_pad(released, now + 100'000);
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 64}) == 1);
    CHECK(data(t, buf, 0).buttons == kCircle);
    CHECK(data(t, buf, 0).lx == 0x10);
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 64}) == 1);
    CHECK(data(t, buf, 0).buttons == 0);
    // Cross held; three blanks pass without a read: the next read returns them, oldest first, a period apart.
    PadState held;
    held.buttons = kCross;
    t.rt.input().set_pad(held, k.clock_ns());
    k.delay_thread(static_cast<u32>(3.5 * period_us(t)), false);
    const i32 n = call32(ctx, kSceCtrl, kRead, {0, buf, 6});
    CHECK(n >= 3);
    CHECK(n <= 4);
    for (i32 i = 1; i < n; ++i) {
        CHECK(std::fabs(static_cast<f64>(data(t, buf, i).timestamp - data(t, buf, i - 1).timestamp) - period_us(t)) <= 1.0);
        CHECK(data(t, buf, i).buttons == kCross);
    }
    // Negative logic, and L as L1 in the *2 functions.
    REQUIRE(call32(ctx, kSceCtrl, kPeekNeg, {0, buf, 1}) == 1);
    CHECK(data(t, buf, 0).buttons == ~kCross);
    PadState l;
    l.buttons = kL;
    t.rt.input().set_pad(l, k.clock_ns());
    REQUIRE(call32(ctx, kSceCtrl, kRead2, {0, buf, 1}) == 1);
    CHECK(data(t, buf, 0).buttons == kL1);
    // In the buttons-only mode the sticks read as centred.
    REQUIRE(call32(ctx, kSceCtrl, kSetMode, {0}) == 1);
    REQUIRE(call32(ctx, kSceCtrl, kPeek, {0, buf, 1}) == 1);
    CHECK(data(t, buf, 0).lx == 0x80);
    CHECK(call32(ctx, kSceCtrl, kPeek, {0, buf, 65}) < 0);
    CHECK(call32(ctx, kSceCtrl, kPeek, {5, buf, 1}) < 0);
}

TEST_CASE("ctrl: a rapid-fire rule presses its target on and off while its trigger is held, after its delay") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    constexpr u32 kSetRapidFire = 0xE9CB69C8, kClearRapidFire = 0xD8294C9C;
    const u32 buf = scratch(64 * 32), rule = scratch(24);
    const u32 words[6] = {kCross, kCross, kCircle, 2, 2, 1};  // mask, trigger, target, delay, make, break
    std::memcpy(t.rt.memory().host(rule), words, sizeof words);
    CHECK(call32(ctx, kSceCtrl, kSetRapidFire, {1, 0, rule}) < 0);   // (port 0 only)
    CHECK(call32(ctx, kSceCtrl, kSetRapidFire, {0, 16, rule}) < 0);  // (16 rules)
    CHECK(call32(ctx, kSceCtrl, kSetRapidFire, {0, 0, 0}) < 0);
    REQUIRE(call32(ctx, kSceCtrl, kSetRapidFire, {0, 3, rule}) == 0);
    REQUIRE(call32(ctx, kSceCtrl, kRead, {0, buf, 64}) >= 1);
    PadState held;
    held.buttons = kCross;
    t.rt.input().set_pad(held, k.clock_ns());
    k.delay_thread(static_cast<u32>(9.5 * period_us(t)), false);
    const i32 n = call32(ctx, kSceCtrl, kRead, {0, buf, 64});
    REQUIRE(n >= 8);
    nx::string seen;
    for (i32 i = 0; i < 8; ++i) {
        CHECK((data(t, buf, i).buttons & kCross) != 0);
        seen += (data(t, buf, i).buttons & kCircle) ? '1' : '0';
    }
    CHECK(seen == "00110110");  // (two samples' delay, then two on and one off)
    // Cleared: the target reads as it is.
    REQUIRE(call32(ctx, kSceCtrl, kClearRapidFire, {0, 3}) == 0);
    CHECK(call32(ctx, kSceCtrl, kClearRapidFire, {0, 99}) < 0);
    k.delay_thread(static_cast<u32>(3.5 * period_us(t)), false);
    const i32 m = call32(ctx, kSceCtrl, kRead, {0, buf, 64});
    for (i32 i = 1; i < m; ++i) CHECK(data(t, buf, i).buttons == kCross);
}

TEST_CASE("touch: panel information, sampling state, reports") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 info = scratch(48);
    REQUIRE(call32(ctx, kSceTouch, kTouchInfo, {1, info}) == 0);
    i16 v[8];
    std::memcpy(v, t.rt.memory().host(info), 16);
    CHECK(v[1] == 108);   // (rear: minAaY)
    CHECK(v[2] == 1919);  // maxAaX
    CHECK(v[3] == 889);   // maxAaY
    CHECK(v[7] == 1087);  // maxDispY
    TouchState s;
    s.count = 2;
    s.points[0] = TouchPoint{5, 100, 200};
    s.points[1] = TouchPoint{6, 1900, 1000};
    t.rt.input().set_touch(0, s, t.rt.kernel().clock_ns());
    const u32 buf = scratch(144 * 4);
    // (sampling stopped: no reports)
    REQUIRE(call32(ctx, kSceTouch, kTouchRead, {0, buf, 1}) == 1);
    u32 count;
    std::memcpy(&count, t.rt.memory().host(buf + 12), 4);
    CHECK(count == 0);
    CHECK(call32(ctx, kSceTouch, kTouchSampling, {0, 1}) == 0);
    REQUIRE(call32(ctx, kSceTouch, kTouchRead, {0, buf, 1}) == 1);
    std::memcpy(&count, t.rt.memory().host(buf + 12), 4);
    REQUIRE(count == 2);
    u8 id = *t.rt.memory().host(buf + 16);
    i16 x, y;
    std::memcpy(&x, t.rt.memory().host(buf + 16 + 16 + 2), 2);
    std::memcpy(&y, t.rt.memory().host(buf + 16 + 16 + 4), 2);
    CHECK(id == 5);
    CHECK(x == 1900);
    CHECK(y == 1000);
    CHECK(call32(ctx, kSceTouch, kTouchPeek, {0, buf, 4}) >= 1);
    CHECK(call32(ctx, kSceTouch, kTouchSampling, {2, 1}) < 0);
}

TEST_CASE("ctrl: a pad filter changes each sample a read returns, on port 0 only") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u32 buf = scratch(64 * 32);
    REQUIRE(call32(ctx, kSceCtrl, kSetMode, {1}) == 0);
    static u32 calls = 0;
    calls = 0;
    t.rt.input().set_pad_filter(
        [](PadState& s, u64, void* user) {
            ++*static_cast<u32*>(user);
            s.buttons |= kCross;
            s.rx = 0xFF;
        },
        &calls);
    REQUIRE(call32(ctx, kSceCtrl, kPeek, {0, buf, 1}) == 1);
    CHECK(data(t, buf, 0).buttons == kCross);
    CHECK(data(t, buf, 0).rx == 0xFF);
    CHECK(calls == 1);
    REQUIRE(call32(ctx, kSceCtrl, kPeek, {1, buf, 1}) == 1);  // (another controller: idle, unfiltered)
    CHECK(data(t, buf, 0).buttons == 0);
    CHECK(calls == 1);
    t.rt.input().set_pad_filter(nullptr, nullptr);
    REQUIRE(call32(ctx, kSceCtrl, kPeek, {0, buf, 1}) == 1);
    CHECK(data(t, buf, 0).buttons == 0);
}

TEST_CASE("system gestures: a tap, a drag, and touches cancelled when updates stop") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kGesture = 0xAF3FE2D3, kInit = 0x6078A08B, kUpdatePrimitive = 0xDF4C665A, kCreate = 0xC3367370,
                  kUpdate = 0x851FB144, kCount = 0x13AD2218, kByIndex = 0x74724147;
    const u32 touch = scratch(144), tap = scratch(307 * 8), drag = scratch(307 * 8), event = scratch(144);
    // (one finger, report id 7, at x, y; or none) at a time in microseconds
    auto frame = [&](u64 time, bool pressed, i16 x, i16 y) {
        u8 d[144] = {};
        std::memcpy(d, &time, 8);
        const u32 count = pressed ? 1 : 0;
        std::memcpy(d + 12, &count, 4);
        d[16] = 7;
        std::memcpy(d + 18, &x, 2);
        std::memcpy(d + 20, &y, 2);
        std::memcpy(t.rt.memory().host(touch), d, 144);
        CHECK(call32(ctx, kGesture, kUpdatePrimitive, {touch, 0}) == 0);
        CHECK(call32(ctx, kGesture, kUpdate, {tap}) == 0);
        CHECK(call32(ctx, kGesture, kUpdate, {drag}) == 0);
    };
    struct Seen {
        u32 state, type;
        i16 a, b, c, d;
        u8 count;
    };
    auto first = [&](u32 recognizer) {
        Seen s{};
        if (call32(ctx, kGesture, kCount, {recognizer}) < 1) return s;
        CHECK(call32(ctx, kGesture, kByIndex, {recognizer, 0, event}) == 0);
        const u8* e = t.rt.memory().host(event);
        std::memcpy(&s.state, e + 4, 4);
        std::memcpy(&s.type, e + 8, 4);
        std::memcpy(&s.a, e + 24 + 2, 2);
        std::memcpy(&s.b, e + 24 + 4, 2);
        std::memcpy(&s.c, e + 24 + 6, 2);
        std::memcpy(&s.d, e + 24 + 8, 2);
        s.count = e[24 + 6];
        return s;
    };
    CHECK(call32(ctx, kGesture, kInit, {0}) == 0);
    CHECK(call32(ctx, kGesture, kCreate, {tap, 1, 0, 0, 0}) == 0);
    CHECK(call32(ctx, kGesture, kCreate, {drag, 3, 0, 0, 0}) == err::code(0x80890002));  // (no such gesture)
    CHECK(call32(ctx, kGesture, kCreate, {drag, 2, 0, 0, 0}) == 0);
    CHECK(call32(ctx, kGesture, kCreate, {drag, 2, 0, 0, 0}) == 0);

    // A tap: pressed and released in place; the event at the press, once.
    frame(1'000'000, true, 500, 400);
    Seen d = first(drag);
    CHECK(d.state == 1);  // (a drag begins when pressed)
    CHECK(call32(ctx, kGesture, kCount, {tap}) == 0);
    frame(1'016'000, true, 503, 401);
    CHECK(first(drag).state == 2);
    frame(1'033'000, false, 0, 0);
    Seen s = first(tap);
    CHECK(s.type == 1);
    CHECK(s.state == 2);
    CHECK(s.a == 500);
    CHECK(s.b == 400);
    CHECK(s.count == 1);
    CHECK(first(drag).state == 3);
    frame(1'050'000, false, 0, 0);
    CHECK(call32(ctx, kGesture, kCount, {tap}) == 0);
    CHECK(call32(ctx, kGesture, kCount, {drag}) == 0);

    // A drag that moves too far for a tap: its deltas and positions; no tap.
    frame(2'000'000, true, 100, 100);
    frame(2'016'000, true, 300, 100);
    d = first(drag);
    CHECK(d.state == 2);
    CHECK(d.a == 200);  // (delta x)
    CHECK(d.c == 300);  // (current x)
    frame(2'033'000, false, 0, 0);
    CHECK(call32(ctx, kGesture, kCount, {tap}) == 0);

    // Updates more than 200 ms apart cancel what was pressed.
    frame(3'000'000, true, 100, 100);
    frame(3'500'000, true, 100, 100);
    CHECK(first(drag).state == 4);
}
