#pragma once

// The game's frame times (the time between the frames it displays, on the host's clock) for the frame rate overlay
// (its numbers and a graph of the last frames, drawn by the window) and for a periodic summary in the log
// (PSVRECORE_FRAME_STATS=<seconds>: frames, frame rate, the frame times' median, 99th percentile and worst, and the
// hitches: frames that took half again as long as the median or more).

#include "psvrecore/platform/quad.hpp"
#include "psvrecore/runtime/kernel.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::platform {

class FrameStats {
public:
    static constexpr usize kHistory = 240;  // (frame times kept: the graph shows the last 120)

    FrameStats();
    // A frame displayed at `ns` (host clock).
    void record(u64 ns);

    // The overlay's atlas: a 5x7 font of the characters the overlay writes, and a white cell (RGBA, straight alpha).
    static void atlas(nx::vector<u8>& rgba, u32& width, u32& height);
    // The overlay's quads for a window `width` pixels wide and `height` high, cut from atlas().
    void quads(u32 width, u32 height, nx::vector<Quad>& out) const;

private:
    u64 times_[kHistory] = {};  // (frame times in nanoseconds, a ring; times_[(next_ - 1) % kHistory] the last)
    u64 at_[kHistory] = {};     // (when each frame came)
    usize next_ = 0, count_ = 0;
    u64 last_ = 0;
    // The summary in the log.
    u64 log_period_ = 0, log_from_ = 0;
    nx::vector<u64> period_;
    nx::vector<runtime::Kernel::CpuUse> cpu_before_;  // (with PSVRECORE_HITCH_TRACE: at the frame before)
    void log_summary(u64 now);
};

}  // namespace psvrecore::platform
