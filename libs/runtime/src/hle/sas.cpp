// The software synthesizer (SceSas): 32 voices, each a sound source (ADPCM .vag data, 16-bit mono PCM, or noise) at a
// pitch, shaped by an envelope and mixed by four volumes (dry left and right, wet left and right) into a grain of
// 48 kHz samples per sceSasCore call: stereo (the dry mix, plus the wet mix through the effect), or four channels
// (the four mixes, no effect).
//
// - ADPCM: 16-byte blocks (a shift and filter byte, a flags byte, 28 4-bit samples). With looping, flags 6 marks the
//   loop's start and 3 its end (decoding goes back to the start, or to the data's start when none was met); 7 ends
//   the sound before its block, as does the end of the data.
// - Pitch: 0x1000 plays the source at 48 kHz; samples between are interpolated linearly.
// - The envelope's height runs 0 to 0x40000000; each sample it moves by its state's rate as its shape says: linear
//   up or down by the rate; bent, as linear up but at a quarter of the rate above three quarters; exponential down by
//   the height times rate / 2^30; exponential up by the room left times rate / 2^30 (our reading of the shape); direct,
//   to the rate. Attack runs to the top, decay down to the sustain level, sustain until key off, release to 0; at 0
//   in sustain or release the voice ends. The simple ADSR words are the two 16-bit envelope registers of Sony's
//   earlier sound chips, their rates (a 2-bit step and a shift, smaller for faster) converted to the rates above.
//   A voice whose envelope was never set plays at full height and stops at key off.
// - Key on, key off and the end states take effect at the next sceSasCore, as on the console.
// - The effect: OFF passes the wet mix through; the reverbs are our own (comb and all-pass filters, a room size and
//   decay per type, not the console's presets); echo is a short reverb with the delay line; delay is the delay line
//   alone (its time and feedback from sceSasSetEffectParam, 0-127 each).
// - Noise: a 15-bit linear feedback shift register, clocked faster for higher noise clocks (our rate mapping).
// The peak meters and distortion are not implemented (no game calls them).

#include "hle_common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceSas = 0xEE47EC43;
constexpr i32 kFail = err::code(0x80420000), kNsmpl = err::code(0x80420001), kMfmt = err::code(0x80420003),
              kAddress = err::code(0x80420005), kVoiceIndex = err::code(0x80420010), kNoiseClock = err::code(0x80420011),
              kPitchVal = err::code(0x80420012), kAdsrMode = err::code(0x80420013), kAdpcmSize = err::code(0x80420014),
              kLoopMode = err::code(0x80420015), kInvalidState = err::code(0x80420016), kVolumeVal = err::code(0x80420018),
              kAdsrVal = err::code(0x80420019), kPcmSize = err::code(0x8042001A), kFxType = err::code(0x80420020),
              kFxFeedback = err::code(0x80420021), kFxDelay = err::code(0x80420022), kFxVolumeVal = err::code(0x80420023),
              kFxUnavailable = err::code(0x80420024), kBusy = err::code(0x80420030), kNotInit = err::code(0x80420100),
              kAlreadyInit = err::code(0x80420101);

constexpr u32 kVoices = 32, kDefaultGrain = 256, kNeededMemory = 0x10000;
constexpr i32 kVolumeMax = 0x1000;
constexpr u32 kHeightMax = 0x40000000, kRateMax = 0x7FFFFFFF;
constexpr u32 kPitchBase = 0x1000;

enum Shape : u32 { kLinearInc = 0, kLinearDec = 1, kBent = 2, kExpDec = 3, kExpInc = 4, kDirect = 5 };
enum Stage : u32 { kAttack, kDecay, kSustain, kRelease };
enum Source : u32 { kNone, kAdpcm, kPcm, kNoise };

struct Envelope {
    Shape shape[4] = {kLinearInc, kExpDec, kLinearDec, kLinearDec};
    u32 rate[4] = {kRateMax, 0, 0, kRateMax};
    u32 sustain_level = kHeightMax;
};

struct Voice {
    // set by the game
    Source source = kNone;
    u32 address = 0, size = 0;  // (ADPCM: bytes; PCM: samples)
    bool loop = false;
    i32 pcm_loop = -1;
    u32 noise_clock = 0;
    u32 pitch = kPitchBase;
    i32 volume[4] = {};  // dry L, dry R, wet L, wet R
    Envelope env;
    bool paused = false;
    bool key_on = false, key_off = false;  // (pending until the next synthesis)
    // playing
    bool playing = false, ended = true;
    Source play_source = kNone;
    u32 play_address = 0, play_size = 0;
    bool play_loop = false;
    i32 play_pcm_loop = -1;
    Stage stage = kAttack;
    u32 height = 0;
    u32 offset = 0, loop_offset = 0;  // (ADPCM: the next block; PCM: the next sample)
    bool has_loop_start = false;
    i32 hist1 = 0, hist2 = 0;
    i16 block[28] = {};
    u32 block_at = 28;
    bool source_done = false;
    i32 cur = 0, nxt = 0;  // (the source's samples either side of the playing position)
    bool have_next = false;
    u32 frac = 0;
    u32 noise = 1, noise_count = 0;
};

// ---- the effect

constexpr std::array<u32, 4> kCombBase = {1557, 1617, 1491, 1422};
constexpr std::array<u32, 2> kAllpassBase = {556, 441};
constexpr u32 kMaxScale16 = 26;  // (the largest room scale, in sixteenths)
constexpr u32 kDelayMax = 128 * 256 + 256;

struct Reverb {
    struct Line {
        nx::vector<f32> buf;
        u32 len = 1, at = 0;
        f32 store = 0;
        void reset(u32 n) {
            buf.assign(n, 0.0f), len = n, at = 0, store = 0;
        }
    };
    Line comb[2][4], allpass[2][2];
    f32 feedback = 0.7f, damp = 0.25f;
    bool on = false;
    void configure(u32 scale16, f32 fb) {
        on = true, feedback = fb;
        for (u32 c = 0; c < 2; ++c) {
            for (u32 i = 0; i < 4; ++i) comb[c][i].reset(std::max<u32>(1, (kCombBase[i] + 23 * c) * scale16 / 16));
            for (u32 i = 0; i < 2; ++i) allpass[c][i].reset(std::max<u32>(1, (kAllpassBase[i] + 23 * c) * scale16 / 16));
        }
    }
    f32 process(u32 c, f32 in) {
        f32 out = 0;
        for (Line& l : comb[c]) {
            const f32 y = l.buf[l.at];
            l.store = y * (1 - damp) + l.store * damp;
            l.buf[l.at] = in + l.store * feedback;
            l.at = (l.at + 1) % l.len;
            out += y;
        }
        out *= 0.25f;
        for (Line& l : allpass[c]) {
            const f32 b = l.buf[l.at];
            l.buf[l.at] = out + b * 0.5f;
            l.at = (l.at + 1) % l.len;
            out = b - out;
        }
        return out;
    }
};

struct Effect {
    i32 type = -1;
    u32 delay_time = 0, feedback = 0;
    i32 volume[2] = {0, 0};
    bool dry = true, wet = false;
    Reverb reverb;
    nx::vector<f32> delay[2];
    u32 delay_at = 0;
    void reset() {
        reverb = Reverb{};
        // room scale (sixteenths) and decay per type: room, studio A, B, C, hall, space echo, echo, delay, pipe
        static constexpr u32 kScale[9] = {8, 6, 10, 13, 20, kMaxScale16, 6, 0, 4};
        static constexpr f32 kDecay[9] = {0.70f, 0.62f, 0.74f, 0.78f, 0.84f, 0.88f, 0.60f, 0.0f, 0.90f};
        if (type >= 0 && type != 7) reverb.configure(kScale[type], kDecay[type]);
        for (auto& d : delay) d.assign(type == 6 || type == 7 ? kDelayMax : 0, 0.0f);
        delay_at = 0;
    }
    f32 process(u32 c, f32 in) {
        if (type < 0) return in;
        f32 out = reverb.on ? reverb.process(c, in) : 0.0f;
        if (!delay[c].empty()) {
            const u32 len = delay_time * 256 + 256;
            const u32 read = (delay_at + kDelayMax - len) % kDelayMax;
            const f32 d = delay[c][read];
            delay[c][delay_at] = in + d * static_cast<f32>(feedback) / 128.0f;
            out += d;
        }
        return out;
    }
};

struct Sas {
    nx::mutex lock;
    bool initialized = false;
    u32 buffer = 0, buffer_size = 0;
    u32 grain = kDefaultGrain, output_mode = 0;
    Voice voices[kVoices];
    Effect effect;
    nx::vector<i32> mix;  // (grain x dry L, dry R, wet L, wet R)
};
Sas& sas() {
    static Sas s;
    return s;
}
std::atomic<bool> g_in_core{false};

using Lock = nx::scoped_lock<nx::mutex>;

// ---- envelope

// A 7-bit rate of the simple form (2-bit step, 5-bit shift; 0x7F: never moves), linear: per sample, in height units.
u32 simple_rate(u32 n, u32 step_base) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const u64 r = (u64{step_base - (n & 3)} << 26) >> (n >> 2);
    return static_cast<u32>(std::max<u64>(1, std::min<u64>(r, kRateMax)));
}

bool step_envelope(Voice& v) {  // false: the envelope reached 0 (the voice ends)
    const Stage s = v.stage;
    const Shape shape = v.env.shape[s];
    const u32 rate = v.env.rate[s];
    i64 h = v.height;
    switch (shape) {
    case kLinearInc: h += rate; break;
    case kLinearDec: h -= rate; break;
    case kBent: h += h < kHeightMax / 4 * 3 ? rate : rate / 4; break;
    case kExpDec:
        if (rate) h -= std::max<i64>(1, static_cast<i64>((static_cast<u64>(h) * rate) >> 30));
        break;
    case kExpInc:
        if (rate) h += std::max<i64>(1, static_cast<i64>((static_cast<u64>(kHeightMax - h) * rate) >> 30));
        break;
    case kDirect: h = std::min<u32>(rate, kHeightMax); break;
    }
    h = std::clamp<i64>(h, 0, kHeightMax);
    v.height = static_cast<u32>(h);
    switch (s) {
    case kAttack:
        if (v.height >= kHeightMax || shape == kDirect) v.stage = kDecay;
        break;
    case kDecay:
        if (v.height <= v.env.sustain_level || shape == kDirect) {
            if (shape != kDirect) v.height = v.env.sustain_level;
            v.stage = kSustain;
        }
        break;
    case kSustain:
    case kRelease:
        if (v.height == 0) return false;
        break;
    }
    return true;
}

// ---- sources

constexpr i32 kFilters[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

// The next ADPCM block into v.block; false at the sound's end.
bool decode_block(Voice& v) {
    for (u32 guard = 0; guard < 2; ++guard) {
        if (v.offset + 16 > v.play_size || !memory().valid(v.play_address + v.offset, 16)) return false;
        const u8* b = memory().host(v.play_address + v.offset);
        const u32 flags = b[1] & 0x0F;
        if (flags == 7) return false;
        if (v.play_loop && flags == 6) v.loop_offset = v.offset, v.has_loop_start = true;
        const u32 shift = b[0] & 0x0F, filter = std::min<u32>((b[0] >> 4) & 0x7, 4);
        for (u32 i = 0; i < 28; ++i) {
            const u32 nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
            i32 s = static_cast<i32>(static_cast<i16>(nibble << 12)) >> std::min<u32>(shift, 12);
            s += (v.hist1 * kFilters[filter][0] + v.hist2 * kFilters[filter][1]) / 64;
            s = std::clamp(s, -32768, 32767);
            v.hist2 = v.hist1, v.hist1 = s;
            v.block[i] = static_cast<i16>(s);
        }
        v.block_at = 0;
        v.offset += 16;
        if (v.play_loop && flags == 3) v.offset = v.has_loop_start ? v.loop_offset : 0;
        return true;
    }
    return false;
}

// The source's next sample; false at its end.
bool next_sample(Voice& v, i32* out) {
    switch (v.play_source) {
    case kAdpcm:
        if (v.block_at >= 28 && !decode_block(v)) return false;
        *out = v.block[v.block_at++];
        return true;
    case kPcm:
        if (v.offset >= v.play_size) {
            if (v.play_pcm_loop < 0) return false;
            v.offset = static_cast<u32>(v.play_pcm_loop);
        }
        if (!memory().valid(v.play_address + 2 * v.offset, 2)) return false;
        *out = static_cast<i16>(psv_ld16(v.play_address + 2 * v.offset));
        ++v.offset;
        return true;
    case kNoise: {
        // (clock c: the register steps every 2^(15 - c/4) / (4 + c%4) * 4 samples' worth, at least every sample)
        const u32 c = v.noise_clock;
        const u32 add = (4 + (c & 3)) << (c >> 2);
        v.noise_count += add;
        while (v.noise_count >= (1u << 17)) {
            v.noise_count -= 1u << 17;
            const u32 bit = ((v.noise >> 15) ^ (v.noise >> 12) ^ (v.noise >> 11) ^ (v.noise >> 10) ^ 1) & 1;
            v.noise = ((v.noise << 1) | bit) & 0xFFFF;
        }
        *out = static_cast<i16>(v.noise);
        return true;
    }
    case kNone: break;
    }
    return false;
}

void start_voice(Voice& v) {
    v.play_source = v.source, v.play_address = v.address, v.play_size = v.size;
    v.play_loop = v.loop, v.play_pcm_loop = v.pcm_loop;
    v.stage = kAttack, v.height = 0;
    v.offset = 0, v.loop_offset = 0, v.has_loop_start = false;
    v.hist1 = v.hist2 = 0, v.block_at = 28, v.source_done = false;
    v.cur = v.nxt = 0, v.frac = 0;
    v.noise = 1, v.noise_count = 0;
    v.playing = true, v.ended = false;
    // (the first two samples ready: the position starts at the first)
    v.source_done = !next_sample(v, &v.cur);
    v.have_next = !v.source_done && next_sample(v, &v.nxt);
}

void synthesize(Sas& s) {
    const u32 grain = s.grain;
    s.mix.assign(static_cast<usize>(grain) * 4, 0);
    for (Voice& v : s.voices) {
        if (v.key_on) {
            v.key_on = false, v.key_off = false;
            start_voice(v);
        } else if (v.key_off) {
            v.key_off = false;
            if (v.playing) v.stage = kRelease;
        }
        if (!v.playing || v.paused) continue;
        for (u32 i = 0; i < grain && v.playing; ++i) {
            if (v.source_done) {
                v.playing = false;
                break;
            }
            const i32 sample =
                v.have_next ? v.cur + static_cast<i32>((static_cast<i64>(v.nxt - v.cur) * v.frac) >> 12) : v.cur;
            if (!step_envelope(v)) v.playing = false;
            const i32 amp = static_cast<i32>((static_cast<i64>(sample) * v.height) >> 30);
            for (u32 c = 0; c < 4; ++c) s.mix[i * 4 + c] += (amp * v.volume[c]) >> 12;
            v.frac += v.pitch;
            while (v.frac >= 0x1000 && !v.source_done) {
                v.frac -= 0x1000;
                if (!v.have_next) {
                    v.source_done = true;
                    break;
                }
                v.cur = v.nxt;
                v.have_next = next_sample(v, &v.nxt);
            }
        }
        if (!v.playing) v.height = 0;
    }
    for (Voice& v : s.voices) v.ended = !v.playing;
}

i16 clamp16(i32 v) { return static_cast<i16>(std::clamp(v, -32768, 32767)); }

// The grain to `out` (stereo: L R ...; four channels: 0 1 2 3 ...), mixed with what is there at (lvol, rvol) when
// `with_mix`.
i32 core(u32 out, bool with_mix, i32 lvol, i32 rvol) {
    if (with_mix && (lvol < 0 || lvol > kVolumeMax || rvol < 0 || rvol > kVolumeMax)) return kVolumeVal;
    if (g_in_core.exchange(true)) return kBusy;
    struct Done {
        ~Done() { g_in_core = false; }
    } done;
    Sas& s = sas();
    Lock held(s.lock);
    if (!s.initialized) return kNotInit;
    const u32 channels = s.output_mode == 1 ? 4 : 2;
    if (!out || (out & 1)) return kAddress;
    if (!writable(out, s.grain * channels * 2)) return kAddress;
    synthesize(s);
    for (u32 i = 0; i < s.grain; ++i) {
        const i32* m = &s.mix[static_cast<usize>(i) * 4];
        i32 o[4];
        if (channels == 4) {
            for (u32 c = 0; c < 4; ++c) o[c] = m[c];
        } else {
            for (u32 c = 0; c < 2; ++c) {
                i32 v = s.effect.dry ? m[c] : 0;
                if (s.effect.wet) {
                    const f32 w = s.effect.process(c, static_cast<f32>(m[2 + c]));
                    v += static_cast<i32>(w * static_cast<f32>(s.effect.volume[c]) / 4096.0f);
                } else if (!s.effect.delay[c].empty()) {
                    s.effect.process(c, 0.0f);  // (the effect keeps running, its input silent)
                }
                o[c] = v;
            }
            if (!s.effect.delay[0].empty()) s.effect.delay_at = (s.effect.delay_at + 1) % kDelayMax;
        }
        for (u32 c = 0; c < channels; ++c) {
            const u32 at = out + (i * channels + c) * 2;
            i32 v = o[c];
            if (with_mix) v += (static_cast<i16>(psv_ld16(at)) * ((c & 1) ? rvol : lvol)) >> 12;
            psv_st16(at, static_cast<u16>(clamp16(v)));
        }
    }
    return 0;
}

// ---- the calls

i32 sceSasGetNeededMemorySize(u32 /*config*/, Ptr<u32> size) {
    if (!size || !writable(size.address, 4)) return kAddress;
    *size = kNeededMemory;
    return 0;
}

i32 init(u32 grain, u32 buffer, u32 size) {
    if (grain < 64 || grain > 2048 || grain % 32) return kNsmpl;
    if (!buffer) return kAddress;
    if (size < kNeededMemory) return kFail;
    Sas& s = sas();
    Lock held(s.lock);
    if (s.initialized) return kAlreadyInit;
    s.initialized = true;
    s.buffer = buffer, s.buffer_size = size;
    s.grain = grain, s.output_mode = 0;
    for (Voice& v : s.voices) v = Voice{};
    s.effect = Effect{};
    s.effect.reset();
    nx::logi(logs::hle, "SceSas: started, {} samples a grain", grain);
    return 0;
}
i32 sceSasInit(u32 /*config*/, u32 buffer, u32 size) { return init(kDefaultGrain, buffer, size); }
i32 sceSasInitWithGrain(u32 /*config*/, u32 grain, u32 buffer, u32 size) { return init(grain, buffer, size); }

i32 sceSasExit(Ptr<u32> buffer, Ptr<u32> size) {
    Sas& s = sas();
    Lock held(s.lock);
    if (!s.initialized) return kNotInit;
    if (buffer && writable(buffer.address, 4)) *buffer = s.buffer;
    if (size && writable(size.address, 4)) *size = s.buffer_size;
    s.initialized = false;
    return 0;
}

template <class F>
i32 with_sas(F f) {
    Sas& s = sas();
    Lock held(s.lock);
    if (!s.initialized) return kNotInit;
    return f(s);
}
template <class F>
i32 with_voice(i32 n, F f) {
    if (n < 0 || n >= static_cast<i32>(kVoices)) return kVoiceIndex;
    return with_sas([&](Sas& s) { return f(s.voices[n]); });
}

i32 sceSasSetGrain(u32 grain) {
    if (grain < 64 || grain > 2048 || grain % 32) return kNsmpl;
    return with_sas([&](Sas& s) { return s.grain = grain, 0; });
}
i32 sceSasGetGrain() {
    return with_sas([](Sas& s) { return static_cast<i32>(s.grain); });
}
i32 sceSasSetOutputmode(u32 mode) {
    if (mode > 1) return kMfmt;
    return with_sas([&](Sas& s) { return s.output_mode = mode, 0; });
}
i32 sceSasGetOutputmode() {
    return with_sas([](Sas& s) { return static_cast<i32>(s.output_mode); });
}

i32 sceSasCore(u32 out) { return core(out, false, 0, 0); }
i32 sceSasCoreWithMix(u32 in_out, i32 lvol, i32 rvol) { return core(in_out, true, lvol, rvol); }

i32 sceSasSetVoice(i32 n, u32 data, u32 size, u32 loop) {
    if (loop > 1) return kLoopMode;
    if (!size || (size & 0xF)) return kAdpcmSize;
    if (!data || !memory().valid(data, size)) return kAddress;
    return with_voice(n, [&](Voice& v) {
        v.source = kAdpcm, v.address = data, v.size = size, v.loop = loop == 1;
        return 0;
    });
}
i32 sceSasSetVoicePCM(i32 n, u32 data, u32 size, i32 loop) {
    if (!size || size > 0x10000) return kPcmSize;
    if (loop >= static_cast<i32>(size)) return kLoopMode;
    if (!data || (data & 1) || !memory().valid(data, size * 2)) return kAddress;
    return with_voice(n, [&](Voice& v) {
        v.source = kPcm, v.address = data, v.size = size, v.pcm_loop = loop < 0 ? -1 : loop;
        return 0;
    });
}
i32 sceSasSetNoise(i32 n, u32 clock) {
    if (clock > 0x3F) return kNoiseClock;
    return with_voice(n, [&](Voice& v) {
        v.source = kNoise, v.noise_clock = clock;
        return 0;
    });
}
i32 sceSasSetVolume(i32 n, i32 l, i32 r, i32 wl, i32 wr) {
    for (i32 x : {l, r, wl, wr})
        if (x < -kVolumeMax || x > kVolumeMax) return kVolumeVal;
    return with_voice(n, [&](Voice& v) {
        v.volume[0] = l, v.volume[1] = r, v.volume[2] = wl, v.volume[3] = wr;
        return 0;
    });
}
i32 sceSasSetPitch(i32 n, i32 pitch) {
    if (pitch < 1 || pitch > 0x4000) return kPitchVal;
    return with_voice(n, [&](Voice& v) { return v.pitch = static_cast<u32>(pitch), 0; });
}

i32 sceSasSetADSR(i32 n, u32 flag, u32 a, u32 d, u32 s, u32 r) {
    const u32 rates[4] = {a, d, s, r};
    for (u32 i = 0; i < 4; ++i)
        if ((flag & (1u << i)) && rates[i] > kRateMax) return kAdsrVal;
    return with_voice(n, [&](Voice& v) {
        for (u32 i = 0; i < 4; ++i)
            if (flag & (1u << i)) v.env.rate[i] = rates[i];
        return 0;
    });
}
i32 sceSasSetADSRmode(i32 n, u32 flag, i32 a, i32 d, i32 s, i32 r) {
    const i32 modes[4] = {a, d, s, r};
    // (attack rises; decay and release fall; sustain may do anything)
    const auto allowed = [](u32 stage, i32 m) {
        if (m < 0 || m > 5) return false;
        if (stage == kAttack) return m == kLinearInc || m == kBent || m == kExpInc;
        if (stage == kSustain) return true;
        return m == kLinearDec || m == kExpDec || m == kDirect;
    };
    for (u32 i = 0; i < 4; ++i)
        if ((flag & (1u << i)) && !allowed(i, modes[i])) return kAdsrMode;
    return with_voice(n, [&](Voice& v) {
        for (u32 i = 0; i < 4; ++i)
            if (flag & (1u << i)) v.env.shape[i] = static_cast<Shape>(modes[i]);
        return 0;
    });
}
i32 sceSasSetSL(i32 n, u32 level) {
    if (level > kHeightMax) return kAdsrVal;
    return with_voice(n, [&](Voice& v) { return v.env.sustain_level = level, 0; });
}
// ADSR1: attack shape (bit 15: bent), attack rate (14-8), decay rate (7-4), sustain level (3-0); ADSR2: sustain shape
// (15-13), sustain rate (12-6), release shape (bit 5: exponential), release rate (4-0).
i32 sceSasSetSimpleADSR(i32 n, u32 adsr1, u32 adsr2) {
    adsr1 &= 0xFFFF, adsr2 &= 0xFFFF;
    Envelope e;
    e.shape[kAttack] = (adsr1 & 0x8000) ? kBent : kLinearInc;
    e.rate[kAttack] = simple_rate(adsr1 >> 8, 7);
    e.shape[kDecay] = kExpDec;
    e.rate[kDecay] = simple_rate(((adsr1 >> 4) & 0xF) * 4, 8);
    e.sustain_level = ((adsr1 & 0xF) + 1) << 26;
    const u32 sustain = (adsr2 >> 13) & 7, sr = (adsr2 >> 6) & 0x7F;
    switch (sustain) {
    case 0: e.shape[kSustain] = kLinearInc, e.rate[kSustain] = simple_rate(sr, 7); break;
    case 2: e.shape[kSustain] = kLinearDec, e.rate[kSustain] = simple_rate(sr, 8); break;
    case 4: e.shape[kSustain] = kBent, e.rate[kSustain] = simple_rate(sr, 7); break;
    case 6: e.shape[kSustain] = kExpDec, e.rate[kSustain] = simple_rate(sr, 8); break;
    default: return kAdsrMode;
    }
    e.shape[kRelease] = (adsr2 & 0x20) ? kExpDec : kLinearDec;
    e.rate[kRelease] = simple_rate((adsr2 & 0x1F) * 4, 8);
    return with_voice(n, [&](Voice& v) { return v.env = e, 0; });
}

i32 sceSasSetKeyOn(i32 n) {
    return with_voice(n, [](Voice& v) {
        if (v.paused) return kInvalidState;
        v.key_on = true, v.key_off = false;
        return 0;
    });
}
i32 sceSasSetKeyOff(i32 n) {
    return with_voice(n, [](Voice& v) {
        if (v.paused) return kInvalidState;
        if (!v.key_on) v.key_off = true;
        return 0;
    });
}
i32 sceSasSetPause(i32 n, u32 pause) {
    return with_voice(n, [&](Voice& v) { return v.paused = pause != 0, 0; });
}
i32 sceSasGetPauseState(i32 n) {
    return with_voice(n, [](Voice& v) { return v.paused ? 1 : 0; });
}
i32 sceSasGetEndState(i32 n) {
    return with_voice(n, [](Voice& v) { return v.ended ? 1 : 0; });
}
i32 sceSasGetEnvelope(i32 n) {
    return with_voice(n, [](Voice& v) { return static_cast<i32>(v.height); });
}

template <class F>
i32 with_effect(F f) {
    return with_sas([&](Sas& s) {
        if (s.output_mode == 1) return kFxUnavailable;
        return f(s.effect);
    });
}
i32 sceSasSetEffectType(i32 type) {
    if (type < -1 || type > 8) return kFxType;
    return with_effect([&](Effect& e) {
        e.type = type;
        e.reset();
        return 0;
    });
}
i32 sceSasSetEffectParam(u32 delay, u32 feedback) {
    if (delay > 127) return kFxDelay;
    if (feedback > 127) return kFxFeedback;
    return with_effect([&](Effect& e) {
        e.delay_time = delay, e.feedback = feedback;
        return 0;
    });
}
i32 sceSasSetEffectVolume(i32 l, i32 r) {
    if (l < 0 || l > kVolumeMax || r < 0 || r > kVolumeMax) return kFxVolumeVal;
    return with_effect([&](Effect& e) {
        e.volume[0] = l, e.volume[1] = r;
        return 0;
    });
}
i32 sceSasSetEffect(i32 dry, i32 wet) {
    return with_effect([&](Effect& e) {
        e.dry = dry != 0, e.wet = wet != 0;
        return 0;
    });
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x180C6824, sceSasGetNeededMemorySize),
    HLE_FUNCTION(0x449B5974, sceSasInit),
    HLE_FUNCTION(0x820D5F82, sceSasInitWithGrain),
    HLE_FUNCTION(0xBB7D6790, sceSasExit),
    HLE_FUNCTION(0x2B4A207C, sceSasSetGrain),
    HLE_FUNCTION(0x2BEA45BC, sceSasGetGrain),
    HLE_FUNCTION(0x44DDB3C4, sceSasSetOutputmode),
    HLE_FUNCTION(0x2C36E150, sceSasGetOutputmode),
    HLE_FUNCTION(0x7A4672B2, sceSasCore),
    HLE_FUNCTION(0xBD496983, sceSasCoreWithMix),
    HLE_FUNCTION(0x2B75F9BC, sceSasSetVoice),
    HLE_FUNCTION(0xB1756EFC, sceSasSetVoicePCM),
    HLE_FUNCTION(0xF1C63CB9, sceSasSetNoise),
    HLE_FUNCTION(0x0BE8204D, sceSasSetVolume),
    HLE_FUNCTION(0x2C48A08C, sceSasSetPitch),
    HLE_FUNCTION(0x18A5EFA2, sceSasSetADSR),
    HLE_FUNCTION(0x5207F9D2, sceSasSetADSRmode),
    HLE_FUNCTION(0xDE6227B8, sceSasSetSL),
    HLE_FUNCTION(0xECCE0DB8, sceSasSetSimpleADSR),
    HLE_FUNCTION(0xC838DB6F, sceSasSetKeyOn),
    HLE_FUNCTION(0x5E42ADAB, sceSasSetKeyOff),
    HLE_FUNCTION(0x59C7A9DF, sceSasSetPause),
    HLE_FUNCTION(0xFD1A0CBF, sceSasGetPauseState),
    HLE_FUNCTION(0x007E63E6, sceSasGetEndState),
    HLE_FUNCTION(0x296A9910, sceSasGetEnvelope),
    HLE_FUNCTION(0xCDF2DDD5, sceSasSetEffectType),
    HLE_FUNCTION(0xBAD546A0, sceSasSetEffectParam),
    HLE_FUNCTION(0x55EDDBFA, sceSasSetEffectVolume),
    HLE_FUNCTION(0xB0444E69, sceSasSetEffect),
};

}  // namespace

void register_sas() {
    {
        Sas& s = sas();
        Lock held(s.lock);
        s.initialized = false;
    }
    g_in_core = false;
    register_library({"SceSas", kSceSas, kFunctions});
}

}  // namespace psvrecore::runtime::hle
