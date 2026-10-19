#include "psvrecore/platform/audio.hpp"

#include <SDL3/SDL.h>

#include "psvrecore/log.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::platform {

Result<nx::unique_ptr<AudioDevice>> AudioDevice::open(const Options& options) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return failure(nx::format("audio: SDL: {}", SDL_GetError()));
    nx::unique_ptr<AudioDevice> d = nx::make_unique<AudioDevice>();
    d->options_ = options;
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(options.rate)};
    d->device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!d->device_) {
        const nx::string error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return failure(nx::format("audio: no playback device: {}", error));
    }
    nx::logi(logs::audio, "audio: playing on '{}'", SDL_GetAudioDeviceName(d->device_));
    return d;
}

AudioDevice::~AudioDevice() {
    {
        nx::scoped_lock<nx::mutex> held(lock_);
        for (auto& [port, p] : ports_) SDL_DestroyAudioStream(p.stream);
        ports_.clear();
    }
    if (device_) SDL_CloseAudioDevice(device_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioDevice::play(const runtime::AudioBlock& block) {
    nx::scoped_lock<nx::mutex> held(lock_);
    Port& p = ports_[block.port];
    if (!p.stream || p.rate != block.rate || p.channels != block.channels) {
        // (a new port, or one opened again with another configuration)
        if (p.stream) SDL_DestroyAudioStream(p.stream);
        const SDL_AudioSpec from{SDL_AUDIO_S16, static_cast<int>(block.channels), static_cast<int>(block.rate)};
        const SDL_AudioSpec to{SDL_AUDIO_S16, 2, static_cast<int>(options_.rate)};
        p.stream = SDL_CreateAudioStream(&from, &to);
        p.rate = block.rate;
        p.channels = block.channels;
        if (!p.stream || !SDL_BindAudioStream(device_, p.stream)) {
            nx::logw(logs::audio, "audio: port {}: {}", block.port, SDL_GetError());
            if (p.stream) SDL_DestroyAudioStream(p.stream);
            p.stream = nullptr;
            return;
        }
    }
    const u64 bytes_per_ms = u64{block.rate} * block.channels * 2 / 1000;
    if (bytes_per_ms && static_cast<u64>(SDL_GetAudioStreamQueued(p.stream)) > bytes_per_ms * options_.max_latency_ms) {
        nx::logw(logs::audio, "audio: port {} is more than {} ms behind: dropping what it holds", block.port,
                 options_.max_latency_ms);
        SDL_ClearAudioStream(p.stream);
    }
    SDL_PutAudioStreamData(p.stream, block.samples.data(), static_cast<int>(block.samples.size_bytes()));
}

void AudioDevice::port_released(u32 port) {
    nx::scoped_lock<nx::mutex> held(lock_);
    auto it = ports_.find(port);
    if (it == ports_.end()) return;
    // (a port is released once its last block has played on the kernel's clock: what the device has not taken yet
    // is a few milliseconds at most, and goes)
    SDL_DestroyAudioStream(it->second.stream);
    ports_.erase(it);
}

void AudioDevice::set_paused(bool paused) {
    if (paused) SDL_PauseAudioDevice(device_);
    else SDL_ResumeAudioDevice(device_);
}

u32 AudioDevice::queued(u32 port) {
    nx::scoped_lock<nx::mutex> held(lock_);
    auto it = ports_.find(port);
    return it == ports_.end() || !it->second.stream ? 0 : static_cast<u32>(SDL_GetAudioStreamQueued(it->second.stream));
}

}  // namespace psvrecore::platform
