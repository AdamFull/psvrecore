#pragma once

// The guest's audio ports played on the host's audio device (SDL3): one audio stream per port, all bound to one
// device that mixes them. The runtime paces the ports (each block arrives as the one before it starts), so a port's
// stream holds about a block. The device's clock is not the kernel's: one running slower gathers sound in the
// stream, so a stream holding more than a quarter of `max_latency_ms` is played up to 0.5% faster until it holds
// less (a pitch change too small to hear); one that has gathered more than `max_latency_ms` all the same (a stall)
// drops what it holds, so sound never falls behind the game.

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
    static Result<nx::unique_ptr<AudioDevice>> open(const Options& options);
    static Result<nx::unique_ptr<AudioDevice>> open() { return open(Options{}); }
    ~AudioDevice() override;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    void play(const runtime::AudioBlock& block) override;
    void port_released(u32 port) override;

    // Stops playing and goes on (the application in the background).
    void set_paused(bool paused);
    // Bytes queued in a port's stream and not played yet (for tests).
    u32 queued(u32 port);

private:
    template <typename T, typename... Args>
    friend nx::unique_ptr<T> nx::make_unique(Args&&...);
    AudioDevice() = default;
    struct Port {
        SDL_AudioStream* stream = nullptr;
        u32 rate = 0, channels = 0;
        f32 queued_ms = 0;  // (what it held as the blocks came, smoothed)
        f32 ratio = 1;      // (how much faster than its rate it plays)
    };
    nx::mutex lock_;
    u32 device_ = 0;
    Options options_;
    nx::flat_hash_map<u32, Port> ports_;
};

}  // namespace psvrecore::platform
