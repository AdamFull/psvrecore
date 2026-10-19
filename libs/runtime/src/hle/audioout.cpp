// Audio output (SceAudio): the MAIN (8), BGM (1) and VOICE (1) ports, paced on the kernel's clock.
//
// Each port has a timeline: a block it is given starts playing when the blocks before it have played (or at once
// if the port has run dry), and sceAudioOutOutput returns when it starts, as the console's does: so a thread that
// outputs in a loop is paced by playback, one block per grain. The block goes to the runtime's audio sink (the
// platform's device, or a capture) as it is queued, with its start time and the port's volume applied
// (sample * volume >> 15; 32768 is 0 dB). sceAudioOutGetRestSample counts what has not played yet.
//
// Not modelled: the BGM port's dynamic normalizer (ALC) and effects, the system taking the BGM port over.

#include <chrono>
#include <cstring>
#include <thread>

#include "hle_common.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAudio = 0x438BB957;

namespace audio_err {
constexpr i32 kNotOpened = err::code(0x80260001), kBusy = err::code(0x80260002), kInvalidPort = err::code(0x80260003),
              kInvalidPointer = err::code(0x80260004), kPortFull = err::code(0x80260005),
              kInvalidSize = err::code(0x80260006), kInvalidFormat = err::code(0x80260007),
              kInvalidSampleFreq = err::code(0x80260008), kInvalidVolume = err::code(0x80260009),
              kInvalidPortType = err::code(0x8026000A), kInvalidConfType = err::code(0x8026000C);
}  // namespace audio_err

enum : u32 { kMain = 0, kBgm = 1, kVoice = 2 };
constexpr u32 kMainPorts = 8, kPorts = kMainPorts + 2;  // (slots: MAIN 0-7, BGM 8, VOICE 9)
constexpr i32 kVolumeMax = 32768;

struct Port {
    bool open = false;
    u32 type = kMain;
    u32 len = 0, freq = 48000, param = 0;
    i32 volume[2] = {kVolumeMax, kVolumeMax};
    // The timeline: `queued` samples have been given since `origin_ns`; they end playing at end_ns().
    u64 origin_ns = 0;
    u64 queued = 0;
    u32 channels() const { return (param & 0xFF) == 1 ? 2u : 1u; }
    u64 at(u64 samples) const { return origin_ns + samples / freq * 1'000'000'000ull + samples % freq * 1'000'000'000ull / freq; }
    u64 end_ns() const { return at(queued); }
};

struct Audio {
    nx::mutex lock;
    Port ports[kPorts];
    u32 alc_mode = 0;
};

Audio& audio() {
    static Audio a;
    return a;
}

u64 now() { return kernel().clock_ns(); }

// Waits (giving up the guest CPU) until the clock reaches `t`: a sleep, then a short spin for the last half
// millisecond (host sleeps are coarse).
void wait_until(u64 t) {
    if (now() >= t) return;
    Kernel::Blocking blocking(kernel());
    while (true) {
        const u64 n = now();
        if (n >= t) break;
        if (t - n > 700'000) nx::this_thread::sleep_for(std::chrono::nanoseconds(t - n - 500'000));
        else std::this_thread::yield();
    }
}

bool valid_freq(u32 type, i32 freq) {
    if (type == kMain) return freq == 48000;
    switch (freq) {
        case 8000: case 11025: case 12000: case 16000: case 22050: case 24000: case 32000: case 44100: case 48000: return true;
        default: return false;
    }
}

i32 check_config(u32 type, i32 len, i32 freq, i32 param) {
    if (len < 64 || len > 64 * 1023 || len % 64) return audio_err::kInvalidSize;
    if (!valid_freq(type, freq)) return audio_err::kInvalidSampleFreq;
    const u32 p = static_cast<u32>(param);
    if ((p & 0xFF) > 1 || (p & 0xFFFF0000u)) return audio_err::kInvalidFormat;
    return 0;
}

// The port of a number the guest holds (open or not), or null.
Port* port_of(i32 port) {
    if (port < 1 || port > static_cast<i32>(kPorts)) return nullptr;
    return &audio().ports[port - 1];
}

i32 sceAudioOutOpenPort(i32 type, i32 len, i32 freq, i32 param) {
    if (type < 0 || type > 2) return audio_err::kInvalidPortType;
    if (const i32 e = check_config(static_cast<u32>(type), len, freq, param)) return e;
    Audio& a = audio();
    nx::scoped_lock<nx::mutex> held(a.lock);
    const u32 first = type == kMain ? 0 : type == kBgm ? kMainPorts : kMainPorts + 1;
    const u32 count = type == kMain ? kMainPorts : 1;
    for (u32 i = first; i < first + count; ++i) {
        Port& p = a.ports[i];
        if (p.open) continue;
        p = Port{};
        p.open = true;
        p.type = static_cast<u32>(type);
        p.len = static_cast<u32>(len), p.freq = static_cast<u32>(freq), p.param = static_cast<u32>(param);
        p.origin_ns = now();
        return static_cast<i32>(i + 1);
    }
    return audio_err::kPortFull;
}

i32 sceAudioOutReleasePort(i32 port) {
    Audio& a = audio();
    {
        nx::scoped_lock<nx::mutex> held(a.lock);
        Port* p = port_of(port);
        if (!p) return audio_err::kInvalidPort;
        if (!p->open) return audio_err::kNotOpened;
        if (now() < p->end_ns()) return audio_err::kBusy;  // (still playing)
        p->open = false;
    }
    if (AudioSink* sink = runtime().config().audio) sink->port_released(static_cast<u32>(port));
    return 0;
}

i32 sceAudioOutOutput(i32 port, Ptr<const i16> data) {
    Audio& a = audio();
    u64 start = 0, end = 0;
    nx::vector<i16> block;
    AudioBlock info;
    {
        nx::scoped_lock<nx::mutex> held(a.lock);
        Port* p = port_of(port);
        if (!p) return audio_err::kInvalidPort;
        if (!p->open) return audio_err::kNotOpened;
        end = p->end_ns();
        if (data) {
            const u32 channels = p->channels();
            const u32 count = p->len * channels;
            if (!memory().valid(data.address, count * 2)) return audio_err::kInvalidPointer;
            // (a port that has run dry starts again now)
            const u64 t = now();
            if (end <= t) p->origin_ns = t, p->queued = 0;
            start = p->end_ns();
            p->queued += p->len;
            block.resize(count);
            std::memcpy(block.data(), data.get(), count * 2);
            for (u32 i = 0; i < count; ++i)
                block[i] = static_cast<i16>((i32{block[i]} * p->volume[channels == 2 ? i & 1 : 0]) >> 15);
            info.port = static_cast<u32>(port), info.port_type = p->type, info.rate = p->freq, info.channels = channels;
            info.start_ns = start;
        }
    }
    if (!data) {
        // (no data: wait until everything given has played)
        wait_until(end);
        kernel().checkpoint();
        return 0;
    }
    if (AudioSink* sink = runtime().config().audio) {
        info.samples = block;
        sink->play(info);
    }
    wait_until(start);
    kernel().checkpoint();
    return 0;
}

i32 sceAudioOutSetVolume(i32 port, i32 flag, Ptr<const i32> volume) {
    if (!volume) return audio_err::kInvalidPointer;
    if (flag & ~3) return audio_err::kInvalidVolume;
    Audio& a = audio();
    nx::scoped_lock<nx::mutex> held(a.lock);
    Port* p = port_of(port);
    if (!p) return audio_err::kInvalidPort;
    if (!p->open) return audio_err::kNotOpened;
    for (u32 k = 0; k < 2; ++k)
        if ((flag >> k) & 1)
            if (volume.get()[k] < 0 || volume.get()[k] > kVolumeMax) return audio_err::kInvalidVolume;
    for (u32 k = 0; k < 2; ++k)
        if ((flag >> k) & 1) p->volume[k] = volume.get()[k];
    return 0;
}

// (a negative argument keeps that setting)
i32 sceAudioOutSetConfig(i32 port, i32 len, i32 freq, i32 param) {
    Audio& a = audio();
    nx::scoped_lock<nx::mutex> held(a.lock);
    Port* p = port_of(port);
    if (!p) return audio_err::kInvalidPort;
    if (!p->open) return audio_err::kNotOpened;
    const i32 nlen = len < 0 ? static_cast<i32>(p->len) : len, nfreq = freq < 0 ? static_cast<i32>(p->freq) : freq;
    const i32 nparam = param < 0 ? static_cast<i32>(p->param) : param;
    if (const i32 e = check_config(p->type, nlen, nfreq, nparam)) return e;
    if (now() < p->end_ns()) return audio_err::kBusy;
    p->len = static_cast<u32>(nlen), p->freq = static_cast<u32>(nfreq), p->param = static_cast<u32>(nparam);
    p->origin_ns = now(), p->queued = 0;
    return 0;
}

i32 sceAudioOutGetConfig(i32 port, i32 type) {
    Audio& a = audio();
    nx::scoped_lock<nx::mutex> held(a.lock);
    Port* p = port_of(port);
    if (!p) return audio_err::kInvalidPort;
    if (!p->open) return audio_err::kNotOpened;
    switch (type) {
        case 0: return static_cast<i32>(p->len);
        case 1: return static_cast<i32>(p->freq);
        case 2: return static_cast<i32>(p->param);
        default: return audio_err::kInvalidConfType;
    }
}

i32 sceAudioOutGetRestSample(i32 port) {
    Audio& a = audio();
    nx::scoped_lock<nx::mutex> held(a.lock);
    Port* p = port_of(port);
    if (!p) return audio_err::kInvalidPort;
    if (!p->open) return audio_err::kNotOpened;
    const u64 t = now();
    if (t >= p->end_ns()) return 0;
    // (the samples whose start is still ahead: those given, less those played by now)
    const u64 played = (t - p->origin_ns) / 1'000'000'000ull * p->freq + (t - p->origin_ns) % 1'000'000'000ull * p->freq / 1'000'000'000ull;
    return static_cast<i32>(p->queued > played ? p->queued - played : 0);
}

i32 sceAudioOutGetAdopt(i32 type) { return type >= 0 && type <= 2 ? 1 : audio_err::kInvalidPortType; }
i32 sceAudioOutSetAlcMode(i32 mode) {
    if (mode < 0 || mode >= 2) return err::kInvalidArgument;
    audio().alc_mode = static_cast<u32>(mode);
    return 0;
}
i32 sceAudioOutSetEffectType(i32 port, i32 type) {
    (void)type;
    return port_of(port) ? 0 : audio_err::kInvalidPort;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x5BC341E4, sceAudioOutOpenPort),
    HLE_FUNCTION(0x69E2E6B5, sceAudioOutReleasePort),
    HLE_FUNCTION(0x02DB3F5F, sceAudioOutOutput),
    HLE_FUNCTION(0x64167F11, sceAudioOutSetVolume),
    HLE_FUNCTION(0xB8BA0D07, sceAudioOutSetConfig),
    HLE_FUNCTION(0x9C8EDAEA, sceAudioOutGetConfig),
    HLE_FUNCTION(0x9A5370C4, sceAudioOutGetRestSample),
    HLE_FUNCTION(0x12FB1767, sceAudioOutGetAdopt),
    HLE_FUNCTION(0x940CE469, sceAudioOutSetAlcMode),
    HLE_FUNCTION(0xB41FACCE, sceAudioOutSetEffectType),
};

}  // namespace

void register_audioout() {
    // (a new runtime starts with every port closed)
    Audio& a = audio();
    for (Port& p : a.ports) p = Port{};
    register_library({"SceAudio", kSceAudio, kFunctions});
}

}  // namespace psvrecore::runtime::hle
