// The touch panels (SceTouch): the front (0) and rear (1) panels, sampled at every vertical blank (sampling.hpp)
// from what the platform layer feeds (Runtime::input), once their sampling is started. Coordinates are the panels'
// (front 0-1919 x 0-1087, rear 0-1919 x 108-889); force reads as 0 (its support ended with SDK 2.500).

#include "sampling.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceTouch = 0x3E4F4A81;
constexpr i32 kTouchInvalidArg = err::code(0x80350001);

struct PanelInfo {
    i16 min_aa_x, min_aa_y, max_aa_x, max_aa_y;
    i16 min_disp_x, min_disp_y, max_disp_x, max_disp_y;
    u8 min_force, max_force;
    u8 reserved[30];
};
static_assert(sizeof(PanelInfo) == 48);

struct TouchReport {
    u8 id, force;
    i16 x, y;
    u8 reserved[8];
    u16 info;
};
static_assert(sizeof(TouchReport) == 16);

struct TouchData {
    u64 timestamp;
    u32 status;
    u32 report_count;
    TouchReport reports[8];
};
static_assert(sizeof(TouchData) == 144);

struct Sample {
    u64 timestamp = 0;
    TouchState state;
};

struct Panel {
    bool sampling = false;
    SampleHistory<Sample> history;
};

struct Touch {
    nx::mutex lock;
    Panel panels[2];
};

Touch& touch() {
    static Touch t;
    return t;
}

i32 sceTouchGetPanelInfo(u32 port, Ptr<PanelInfo> info) {
    if (port > 1 || !info || !writable(info.address, sizeof(PanelInfo))) return kTouchInvalidArg;
    PanelInfo p{};
    p.max_aa_x = p.max_disp_x = 1919;
    p.max_disp_y = 1087;
    p.min_aa_y = port ? 108 : 0;
    p.max_aa_y = port ? 889 : 1087;
    p.max_force = 128;
    *info = p;
    return 0;
}

i32 sceTouchSetSamplingState(u32 port, u32 state) {
    if (port > 1 || state > 1) return kTouchInvalidArg;
    Touch& t = touch();
    nx::scoped_lock<nx::mutex> held(t.lock);
    const bool old = t.panels[port].sampling;
    t.panels[port].sampling = state == 1;
    return old ? 1 : 0;
}

i32 sceTouchGetSamplingState(u32 port, Ptr<u32> state) {
    if (port > 1 || !state) return kTouchInvalidArg;
    *state = touch().panels[port].sampling ? 1 : 0;
    return 0;
}

i32 take(u32 port, Ptr<TouchData> data, u32 count, bool block) {
    if (port > 1 || !data || count < 1 || count > 64 || !writable(data.address, sizeof(TouchData) * count))
        return kTouchInvalidArg;
    Touch& t = touch();
    Sample samples[64];
    u32 n;
    {
        nx::scoped_lock<nx::mutex> held(t.lock);
        Panel& p = t.panels[port];
        auto make = [&](u64 k) {
            Sample s;
            s.timestamp = runtime().vblank_ns(k) / 1000;
            // (a stopped panel reports no touch)
            if (p.sampling) s.state = runtime().input().touch_sample(port, runtime().vblank_ns(k));
            return s;
        };
        n = block ? p.history.read(count, samples, make) : p.history.peek(count, samples, make);
    }
    for (u32 i = 0; i < n; ++i) {
        TouchData& d = data.get()[i];
        std::memset(&d, 0, sizeof d);
        d.timestamp = samples[i].timestamp;
        d.report_count = nx::min<u32>(samples[i].state.count, port ? 4 : 6);
        for (u32 r = 0; r < d.report_count; ++r) {
            const TouchPoint& pt = samples[i].state.points[r];
            d.reports[r].id = static_cast<u8>(pt.id & 0x7F);
            d.reports[r].x = pt.x;
            d.reports[r].y = pt.y;
        }
    }
    kernel().checkpoint();
    return static_cast<i32>(n);
}

i32 sceTouchRead(u32 port, Ptr<TouchData> data, u32 count) { return take(port, data, count, true); }
i32 sceTouchPeek(u32 port, Ptr<TouchData> data, u32 count) { return take(port, data, count, false); }
i32 sceTouchEnableTouchForce(u32 port) { return port > 1 ? kTouchInvalidArg : 0; }
i32 sceTouchDisableTouchForce(u32 port) { return port > 1 ? kTouchInvalidArg : 0; }

const Function kFunctions[] = {
    HLE_FUNCTION(0x10A2CA25, sceTouchGetPanelInfo),
    HLE_FUNCTION(0x1B9C5D14, sceTouchSetSamplingState),
    HLE_FUNCTION(0x26531526, sceTouchGetSamplingState),
    HLE_FUNCTION(0x169A1D58, sceTouchRead),
    HLE_FUNCTION(0xFF082DF0, sceTouchPeek),
    HLE_FUNCTION(0xB18370C2, sceTouchEnableTouchForce),
    HLE_FUNCTION(0x41194411, sceTouchDisableTouchForce),
};

}  // namespace

void register_touch() {
    Touch& t = touch();
    for (Panel& p : t.panels) p = Panel{};
    register_library({"SceTouch", kSceTouch, kFunctions});
}

}  // namespace psvrecore::runtime::hle
