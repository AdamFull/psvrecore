#pragma once

// Input as the platform layer gives it: the controller's state and the touch panels' points, each change stamped
// with the kernel's clock when it happened (it may arrive late: the stamp is what counts). SceCtrl and SceTouch
// sample it at every vertical blank, as the console's services do; a sample is the state at the blank's time, with
// every button pressed since the previous sample included (a press and release between two blanks still shows).

#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime {

struct PadState {
    u32 buttons = 0;  // SCE_CTRL_* bits (positive logic)
    u8 lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    bool operator==(const PadState&) const = default;
};

struct TouchPoint {
    u8 id = 0;      // 0-127, the same while the finger stays down
    i16 x = 0, y = 0;  // panel coordinates (front 0-1919 x 0-1087; rear 0-1919 x 108-889)
};

struct TouchState {
    u32 count = 0;  // up to 6 on the front panel, 4 on the rear
    TouchPoint points[8];
};

class Input {
public:
    // From any thread: the controller's (or a panel's) new state, as of `at_ns` (Kernel::clock_ns).
    void set_pad(const PadState& state, u64 at_ns);
    void set_touch(u32 port, const TouchState& state, u64 at_ns);

    // The sample of the blank at `t_ns` whose previous sample was at `previous_ns`.
    PadState pad_sample(u64 previous_ns, u64 t_ns);

    // A function that may change each controller sample as the game reads it (once per sample a read returns,
    // from the reading thread): a replay or an automatic player stands in for the player this way.
    using PadFilter = void (*)(PadState& sample, u64 t_ns, void* user);
    void set_pad_filter(PadFilter filter, void* user) { filter_ = filter, filter_user_ = user; }
    void filter_pad(PadState& sample, u64 t_ns) {
        if (filter_) filter_(sample, t_ns, filter_user_);
    }
    TouchState touch_sample(u32 port, u64 t_ns);

private:
    template <typename T>
    struct Event {
        u64 at_ns;
        T state;
    };
    nx::mutex lock_;
    PadFilter filter_ = nullptr;
    void* filter_user_ = nullptr;
    PadState pad_;  // (the state before the oldest event kept)
    nx::vector<Event<PadState>> pad_events_;
    TouchState touch_[2];
    nx::vector<Event<TouchState>> touch_events_[2];
    // (events older than this are folded into the states above)
    static constexpr u64 kKeepNs = 2'000'000'000ull;
    void prune(u64 now_ns);
};

}  // namespace psvrecore::runtime
