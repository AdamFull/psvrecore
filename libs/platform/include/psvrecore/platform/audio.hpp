#pragma once

// The guest's audio ports played on the host's audio device (SDL3): one audio stream per port, all bound to one
// device that mixes them. The runtime paces the ports (each block arrives as the one before it starts), so a port's
// stream holds about a block; a stream that has gathered more than `max_latency_ms` (the device running slower than
// the kernel's clock, or a stall) drops what it holds, so sound never falls behind the game.

#include <memory>

#include "psvrecore/error.hpp"
#include "psvrecore/runtime/audio.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/threading/sync.h"

struct SDL_AudioStream;

namespace psvrecore::platform {

class AudioDevice : public runtime::AudioSink {
public:
    struct Options {
        u32 rate = 48000;  // the device's (the ports' own rates are converted)
        u32 max_latency_ms = 200;
    };

    // Opens the default playback device (initializing SDL's audio).
    static Result<std::unique_ptr<AudioDevice>> open(const Options& options);
    static Result<std::unique_ptr<AudioDevice>> open() { return open(Options{}); }
    ~AudioDevice() override;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    void play(const runtime::AudioBlock& block) override;
    void port_released(u32 port) override;

    // Bytes queued in a port's stream and not played yet (for tests).
    u32 queued(u32 port);

private:
    AudioDevice() = default;
    struct Port {
        SDL_AudioStream* stream = nullptr;
        u32 rate = 0, channels = 0;
    };
    nx::mutex lock_;
    u32 device_ = 0;
    Options options_;
    nx::flat_hash_map<u32, Port> ports_;
};

}  // namespace psvrecore::platform
