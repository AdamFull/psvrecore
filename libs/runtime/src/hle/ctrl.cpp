// Input (SceCtrl) and system modules (SceSysmodule).
//
// The controller is sampled at every vertical blank (sampling.hpp) from what the platform layer feeds
// (Runtime::input); timestamps are the blank's process time. Port 0 is the system's buttons and sticks; ports 1-4
// (wireless controllers on a PlayStation TV) read as controllers doing nothing. The *2 functions report L and R as
// L1 and R1, with a pressure of 255 for each pressed button. In the buttons-only mode the sticks read as centred.

#include "sampling.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceCtrl = 0xD197E3C7, kSceSysmodule = 0x03FCF19D;
constexpr i32 kCtrlInvalidArg = err::code(0x80340001), kCtrlInvalidMode = err::code(0x80340022);
constexpr u32 kL = 1u << 8, kR = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11;

struct CtrlData {
    u64 timestamp;
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 reserved[16];
};
static_assert(sizeof(CtrlData) == 32);

struct CtrlData2 {
    u64 timestamp;
    u32 buttons;
    u8 lx, ly, rx, ry;
    u8 up, right, down, left, l2, r2, l1, r1, triangle, circle, cross, square;
    u8 reserved[4];
};
static_assert(sizeof(CtrlData2) == 32);

struct Sample {
    u64 timestamp = 0;  // (µs)
    PadState state;
};

struct Ctrl {
    nx::mutex lock;
    u32 mode = 0;  // SCE_CTRL_MODE_DIGITALONLY
    SampleHistory<Sample> history;
};

Ctrl& ctrl() {
    static Ctrl c;
    return c;
}

Sample sample_of(u64 n) {
    Runtime& rt = runtime();
    const u64 t = rt.vblank_ns(n);
    Sample s;
    s.timestamp = t / 1000;
    s.state = rt.input().pad_sample(n ? rt.vblank_ns(n - 1) : 0, t);
    return s;
}

// The samples a call asks for (none for ports 1-4: they read as idle controllers).
i32 take(i32 port, i32 count, bool block, Sample* out) {
    if (port < 0 || port > 4 || count < 1 || count > 64) return kCtrlInvalidArg;
    Ctrl& c = ctrl();
    nx::scoped_lock<nx::mutex> held(c.lock);
    const u32 n = block ? c.history.read(static_cast<u32>(count), out, sample_of)
                        : c.history.peek(static_cast<u32>(count), out, sample_of);
    for (u32 i = 0; i < n; ++i) {
        if (port != 0) out[i].state = PadState{};
        else runtime().input().filter_pad(out[i].state, out[i].timestamp * 1000);
        if (c.mode == 0) out[i].state.lx = out[i].state.ly = out[i].state.rx = out[i].state.ry = 0x80;
    }
    return static_cast<i32>(n);
}

i32 read(i32 port, Ptr<CtrlData> data, i32 count, bool negative, bool block) {
    if (!data || count < 1 || count > 64 || !writable(data.address, sizeof(CtrlData) * static_cast<u32>(count)))
        return kCtrlInvalidArg;
    Sample samples[64];
    const i32 n = take(port, count, block, samples);
    if (n < 0) return n;
    for (i32 i = 0; i < n; ++i) {
        CtrlData& d = data.get()[i];
        std::memset(&d, 0, sizeof d);
        const PadState& s = samples[i].state;
        d.timestamp = samples[i].timestamp;
        d.buttons = negative ? ~s.buttons : s.buttons;
        d.lx = s.lx, d.ly = s.ly, d.rx = s.rx, d.ry = s.ry;
    }
    kernel().checkpoint();
    return n;
}

i32 read2(i32 port, Ptr<CtrlData2> data, i32 count, bool negative, bool block) {
    if (!data || count < 1 || count > 64 || !writable(data.address, sizeof(CtrlData2) * static_cast<u32>(count)))
        return kCtrlInvalidArg;
    Sample samples[64];
    const i32 n = take(port, count, block, samples);
    if (n < 0) return n;
    for (i32 i = 0; i < n; ++i) {
        CtrlData2& d = data.get()[i];
        std::memset(&d, 0, sizeof d);
        const PadState& s = samples[i].state;
        // (L and R as L1 and R1)
        u32 b = s.buttons & ~(kL | kR);
        if (s.buttons & kL) b |= kL1;
        if (s.buttons & kR) b |= kR1;
        d.timestamp = samples[i].timestamp;
        d.buttons = negative ? ~b : b;
        d.lx = s.lx, d.ly = s.ly, d.rx = s.rx, d.ry = s.ry;
        auto pressure = [&](u32 bit) { return static_cast<u8>((b & bit) ? 255 : 0); };
        d.up = pressure(1u << 4), d.right = pressure(1u << 5), d.down = pressure(1u << 6), d.left = pressure(1u << 7);
        d.l1 = pressure(kL1), d.r1 = pressure(kR1);
        d.triangle = pressure(1u << 12), d.circle = pressure(1u << 13), d.cross = pressure(1u << 14), d.square = pressure(1u << 15);
    }
    kernel().checkpoint();
    return n;
}

i32 sceCtrlPeekBufferPositive(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, false, false); }
i32 sceCtrlPeekBufferNegative(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, true, false); }
i32 sceCtrlReadBufferPositive(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, false, true); }
i32 sceCtrlReadBufferNegative(i32 port, Ptr<CtrlData> data, i32 count) { return read(port, data, count, true, true); }
i32 sceCtrlPeekBufferPositive2(i32 port, Ptr<CtrlData2> data, i32 count) { return read2(port, data, count, false, false); }
i32 sceCtrlPeekBufferNegative2(i32 port, Ptr<CtrlData2> data, i32 count) { return read2(port, data, count, true, false); }
i32 sceCtrlReadBufferPositive2(i32 port, Ptr<CtrlData2> data, i32 count) { return read2(port, data, count, false, true); }
i32 sceCtrlReadBufferNegative2(i32 port, Ptr<CtrlData2> data, i32 count) { return read2(port, data, count, true, true); }

i32 sceCtrlSetSamplingMode(u32 mode) {
    if (mode > 2) return kCtrlInvalidMode;
    Ctrl& c = ctrl();
    nx::scoped_lock<nx::mutex> held(c.lock);
    const u32 old = c.mode;
    c.mode = mode;
    return static_cast<i32>(old);
}
i32 sceCtrlGetSamplingMode(Ptr<u32> mode) {
    if (!mode) return kCtrlInvalidArg;
    *mode = ctrl().mode;
    return 0;
}

// (one PlayStation Vita controller; no wireless controllers)
i32 sceCtrlIsMultiControllerSupported() { return 0; }
i32 sceCtrlGetWirelessControllerInfo(Ptr<u32> info) {
    if (!info || !writable(info.address, 16)) return kCtrlInvalidArg;  // (SceCtrlWirelessControllerInfo)
    std::memset(info.get(), 0, 16);
    return 0;
}

// System modules are part of the runtime: loading one succeeds, and every one is loaded.
i32 sceSysmoduleLoadModule(u32) { return 0; }
i32 sceSysmoduleUnloadModule(u32) { return 0; }
i32 sceSysmoduleIsLoaded(u32) { return 0; }

const Function kCtrl[] = {
    HLE_FUNCTION(0xA9C3CED6, sceCtrlPeekBufferPositive),
    HLE_FUNCTION(0x104ED1A7, sceCtrlPeekBufferNegative),
    HLE_FUNCTION(0x67E7AB83, sceCtrlReadBufferPositive),
    HLE_FUNCTION(0x15F96FB0, sceCtrlReadBufferNegative),
    HLE_FUNCTION(0x15F81E8C, sceCtrlPeekBufferPositive2),
    HLE_FUNCTION(0x81A89660, sceCtrlPeekBufferNegative2),
    HLE_FUNCTION(0xC4226A3E, sceCtrlReadBufferPositive2),
    HLE_FUNCTION(0x27A0C5FB, sceCtrlReadBufferNegative2),
    HLE_FUNCTION(0xA497B150, sceCtrlSetSamplingMode),
    HLE_FUNCTION(0xEC752AAF, sceCtrlGetSamplingMode),
    HLE_FUNCTION(0x1FFFD965, sceCtrlIsMultiControllerSupported),
    HLE_FUNCTION(0xB8844141, sceCtrlGetWirelessControllerInfo),
};
const Function kSysmodule[] = {
    HLE_FUNCTION(0x79A0160A, sceSysmoduleLoadModule),
    HLE_FUNCTION(0x31D87805, sceSysmoduleUnloadModule),
    HLE_FUNCTION(0x53099B7A, sceSysmoduleIsLoaded),
};

}  // namespace

void register_ctrl() {
    // (a new runtime starts with a fresh controller service)
    Ctrl& c = ctrl();
    c.mode = 0;
    c.history = SampleHistory<Sample>{};
    register_library({"SceCtrl", kSceCtrl, kCtrl});
    register_library({"SceSysmodule", kSceSysmodule, kSysmodule});
}

}  // namespace psvrecore::runtime::hle
