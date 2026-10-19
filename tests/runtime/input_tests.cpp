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
