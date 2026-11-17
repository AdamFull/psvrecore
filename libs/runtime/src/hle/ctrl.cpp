// Input (SceCtrl) and system modules (SceSysmodule).
//
// The controller is sampled at every vertical blank (sampling.hpp) from what the platform layer feeds
// (Runtime::input); timestamps are the blank's process time. Port 0 is the system's buttons and sticks; ports 1-4
// (wireless controllers on a PlayStation TV) read as controllers doing nothing. The *2 functions report L and R as
// L1 and R1, with a pressure of 255 for each pressed button. In the buttons-only mode the sticks read as centred.
// Rapid-fire rules (up to 16 a port) apply to port 0's samples as they are taken: while the buttons under a rule's
// mask are its trigger, its target buttons go on as pressed for `make` samples and released for `break` samples,
// after the trigger has been held for `delay` samples (until then they read as they are).

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

struct RapidFireRule {
    u32 mask, trigger, target, delay, make, release;
};
static_assert(sizeof(RapidFireRule) == 24);
constexpr u32 kRapidFireRules = 16;

struct RapidFire {
    bool set = false;
    RapidFireRule rule{};
    u32 held = 0;  // (samples the trigger has been held)
};

struct Ctrl {
    nx::mutex lock;
    u32 mode = 0;  // SCE_CTRL_MODE_DIGITALONLY
    SampleHistory<Sample> history;
    RapidFire rapid_fire[kRapidFireRules];
};

Ctrl& ctrl() {
    static Ctrl c;
    return c;
}

// (each sample once, in order: the rules count samples; called with the lock held)
void apply_rapid_fire(u32& buttons) {
    const u32 pressed = buttons;
    for (RapidFire& f : ctrl().rapid_fire) {
        if (!f.set) continue;
        const RapidFireRule& r = f.rule;
        if ((pressed & r.mask) != r.trigger) {
            f.held = 0;
            continue;
        }
        const u32 n = f.held++;
        if (n < r.delay) continue;
        const u32 period = r.make + r.release;
        const bool on = period == 0 || (n - r.delay) % period < r.make;
        buttons = on ? buttons | r.target : buttons & ~r.target;
    }
}

Sample sample_of(u64 n) {
    Runtime& rt = runtime();
    const u64 t = rt.vblank_ns(n);
    Sample s;
    s.timestamp = t / 1000;
    s.state = rt.input().pad_sample(n ? rt.vblank_ns(n - 1) : 0, t);
    apply_rapid_fire(s.state.buttons);
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

i32 sceCtrlSetRapidFire(i32 port, i32 index, Ptr<const RapidFireRule> rule) {
    if (port != 0 || index < 0 || index >= static_cast<i32>(kRapidFireRules) || !rule ||
        !readable(rule.address, sizeof(RapidFireRule)))
        return kCtrlInvalidArg;
    Ctrl& c = ctrl();
    nx::scoped_lock<nx::mutex> held(c.lock);
    c.rapid_fire[index] = RapidFire{true, *rule, 0};
    return 0;
}
i32 sceCtrlClearRapidFire(i32 port, i32 index) {
    if (port != 0 || index < 0 || index >= static_cast<i32>(kRapidFireRules)) return kCtrlInvalidArg;
    Ctrl& c = ctrl();
    nx::scoped_lock<nx::mutex> held(c.lock);
    c.rapid_fire[index] = RapidFire{};
    return 0;
}

// (one PlayStation Vita controller; no wireless controllers)
i32 sceCtrlIsMultiControllerSupported() { return 0; }
i32 sceCtrlGetWirelessControllerInfo(Ptr<u32> info) {
    if (!info || !writable(info.address, 16)) return kCtrlInvalidArg;  // (SceCtrlWirelessControllerInfo)
    std::memset(info.get(), 0, 16);
    return 0;
}

// System modules are part of the runtime: loading one succeeds, and every one is loaded, but for those the runtime
// does not provide, which are never loaded and fail to load (a game then goes its own way without them):
// - SceMp4 (0x2D), the MP4 demuxer of firmware 1.x that statically linked players use (not implemented: its
//   functions are known only by NID).
constexpr i32 kSysmoduleUnloaded = err::code(0x805A1001), kSysmoduleFatal = err::code(0x805A10FF);
bool provided(u32 id) { return id != 0x2D; }
i32 sceSysmoduleLoadModule(u32 id) {
    if (provided(id)) return 0;
    nx::logw(logs::hle, "system module 0x{:04x}: not provided by this runtime (not loaded)", id);
    return kSysmoduleFatal;
}
i32 sceSysmoduleUnloadModule(u32 id) { return provided(id) ? 0 : kSysmoduleUnloaded; }
i32 sceSysmoduleIsLoaded(u32 id) { return provided(id) ? 0 : kSysmoduleUnloaded; }

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
    HLE_FUNCTION(0xE9CB69C8, sceCtrlSetRapidFire),
    HLE_FUNCTION(0xD8294C9C, sceCtrlClearRapidFire),
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
    for (RapidFire& f : c.rapid_fire) f = RapidFire{};
    register_library({"SceCtrl", kSceCtrl, kCtrl});
    register_library({"SceSysmodule", kSceSysmodule, kSysmodule});
}

}  // namespace psvrecore::runtime::hle
