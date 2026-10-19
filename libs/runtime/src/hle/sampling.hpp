#pragma once

// The input services' sample buffers (SceCtrl, SceTouch): one sample per vertical blank, the newest 64 kept, and a
// read pointer. Samples are taken when asked for (every blank since the last one asked about, up to 64), each of the
// state at its blank's time: as if a service had sampled at every blank.

#include <chrono>

#include "hle_common.hpp"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime::hle {

template <typename Sample>
class SampleHistory {
public:
    static constexpr u32 kSize = 64;

    // Takes the samples of the blanks up to now; make(n) is the sample of blank n.
    template <typename Make>
    void update(Make&& make) {
        const i64 now = static_cast<i64>(runtime().vblanks_at(kernel().clock_ns()));
        for (i64 n = nx::max(newest_ + 1, now - static_cast<i64>(kSize) + 1); n <= now; ++n)
            ring_[static_cast<u64>(n) % kSize] = make(static_cast<u64>(n));
        if (now > newest_) newest_ = now;
    }

    // Peek: the newest `count` samples (oldest first), never waiting.
    template <typename Make>
    u32 peek(u32 count, Sample* out, Make&& make) {
        update(make);
        return copy(newest_, count, out);
    }

    // Read: the samples since the last read (the newest `count` of them, oldest first); if there are none yet,
    // waits for the next blank.
    template <typename Make>
    u32 read(u32 count, Sample* out, Make&& make) {
        update(make);
        if (newest_ <= read_) {
            const u64 next = runtime().vblank_ns(static_cast<u64>(read_ + 1));
            Kernel::Blocking blocking(kernel());
            while (kernel().clock_ns() < next) {
                const u64 t = kernel().clock_ns();
                nx::this_thread::sleep_for(std::chrono::nanoseconds(next > t ? next - t : 0));
            }
        }
        update(make);
        const u32 pending = static_cast<u32>(nx::min<i64>(newest_ - read_, kSize));
        const u32 n = copy(newest_, nx::min(count, pending), out);
        read_ = newest_;
        return n;
    }

private:
    i64 newest_ = -1;  // (the newest blank sampled)
    i64 read_ = -1;    // (the newest blank a read returned)
    Sample ring_[kSize] = {};

    u32 copy(i64 newest, u32 count, Sample* out) const {
        const u32 n = static_cast<u32>(nx::min<i64>(count, nx::min<i64>(newest + 1, kSize)));
        for (u32 i = 0; i < n; ++i) out[i] = ring_[static_cast<u64>(newest - (n - 1) + i) % kSize];
        return n;
    }
};

}  // namespace psvrecore::runtime::hle
