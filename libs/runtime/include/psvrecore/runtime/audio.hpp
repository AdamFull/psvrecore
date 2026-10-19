#pragma once

// Where the audio output ports' samples go: the platform layer's device, or a capture in tests. The runtime paces
// the guest (sceAudioOutOutput blocks until the previous block has played) on the kernel's clock and hands each
// block over as it is queued, with the time it starts playing; the sink mixes and plays, or keeps, them.

#include <span>

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime {

struct AudioBlock {
    u32 port = 0;       // the port's number, as the guest sees it
    u32 port_type = 0;  // SCE_AUDIO_OUT_PORT_TYPE_MAIN, _BGM, _VOICE
    u32 rate = 48000;   // samples per second (per channel)
    u32 channels = 2;   // 1 or 2
    u64 start_ns = 0;   // when it starts playing (Kernel::clock_ns)
    std::span<const i16> samples;  // interleaved, the port's volume applied
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Called on the guest thread that output the block; the samples are valid for the call only.
    virtual void play(const AudioBlock& block) = 0;
    virtual void port_released(u32 port) { (void)port; }
};

}  // namespace psvrecore::runtime
