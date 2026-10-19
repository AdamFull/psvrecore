#include "modules.hpp"

#include <cmath>
#include <cstring>

#include "libatrac9.h"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::ngs {

namespace {

constexpr f64 kPi = 3.14159265358979323846;

// ---- the guest's parameter structures

struct Desc {
    u32 id, size;
};

struct PlayerBuffer {
    u32 buffer;
    i32 bytes;
    i16 loop_count;
    i16 next;
};
struct PlayerParams {
    Desc desc;
    PlayerBuffer buffs[4];
    f32 frequency, scalar;
    i32 lead_in, limit, start_byte;
    i8 channels;
    i8 map[2];
    i8 type;
    i8 reserved;
    i8 start_buffer;
    i8 pad[2];
};
static_assert(sizeof(PlayerParams) == 84);

struct At9Buffer {
    u32 buffer;
    i32 bytes;
    i16 loop_count, next, discard_start, discard_end;
};
struct At9Params {
    Desc desc;
    At9Buffer buffs[4];
    f32 frequency, scalar;
    i32 lead_in, limit;
    i8 channels;
    i8 map[2];
    i8 reserved;
    i32 config;
};
static_assert(sizeof(At9Params) == 96);

struct PlayerStates {
    i32 position, buffer, samples_since_key_on, bytes_since_key_on, samples_total, bytes_total;
};

struct MixerParams {
    Desc desc;
    f32 gain[2];
    u32 lead_in, lead_out;
};
static_assert(sizeof(MixerParams) == 24);

struct EnvelopePoint {
    u32 msecs;
    f32 amplitude;
    u32 curve;
};
struct EnvelopeParams {
    Desc desc;
    EnvelopePoint points[4];
    u32 release_msecs, num_points, loop_start;
    i32 loop_end;
};
static_assert(sizeof(EnvelopeParams) == 72);
struct EnvelopeStates {
    f32 height, position, release_scale;
    i32 point, releasing;
};

struct FilterParams {
    Desc desc;
    u32 mode;
    f32 frequency, resonance, gain;
};
static_assert(sizeof(FilterParams) == 24);
struct Coefficients {
    f32 b0, b1, b2, a1, a2;
};
struct FilterCoeffParams {
    Desc desc;
    Coefficients c;
};
static_assert(sizeof(FilterCoeffParams) == 28);
struct EqFilter {
    u32 mode;
    f32 frequency, resonance, gain;
};
struct EqParams {
    Desc desc;
    EqFilter filter[4];
};
static_assert(sizeof(EqParams) == 72);
struct EqCoeffParams {
    Desc desc;
    Coefficients c[4];
};
static_assert(sizeof(EqCoeffParams) == 88);

struct PauserParams {
    Desc desc;
    u32 lead_in, lead_out;
};

struct GeneratorParams {
    Desc desc;
    u32 mode;
    i32 frequency;
    f32 amplitude, pulse_width;
    u32 sample_offset, phase_angle;
};
static_assert(sizeof(GeneratorParams) == 32);

struct DistortionParams {
    Desc desc;
    f32 a, b, clip, gate, wet, dry;
};

struct CompressorParams {
    Desc desc;
    f32 ratio, threshold, attack, release, makeup;
    i32 stereo_link, peak_mode;
    f32 soft_knee;
};
static_assert(sizeof(CompressorParams) == 40);
struct CompressorStates {
    f32 input[2], output[2];
};

struct DelayTap {
    f32 msecs, volume, feedback;
    u32 filter;
    f32 cutoff, phase_deg, width_msecs;
};
struct DelayParams {
    Desc desc;
    f32 dry, mod_rate;
    DelayTap taps[4];
};
static_assert(sizeof(DelayParams) == 128);

struct PitchShiftParams {
    Desc desc;
    f32 cents;
};

struct ReverbParams {
    Desc desc;
    f32 room, room_hf, decay_time, decay_hf_ratio, reflections, reflections_delay, reverb, reverb_delay, diffusion,
        density, hf_reference;
    i32 pattern[2];
    f32 early_scalar, lf_reference, room_lf, dry_mb;
};
static_assert(sizeof(ReverbParams) == 76);

// ---- the types

constexpr Interface kPlayerIf[] = {{params_id(kPlayer, 1, 1), sizeof(PlayerParams)}};
constexpr Interface kAt9If[] = {{params_id(kAt9Player, 1, 1), sizeof(At9Params)}};
constexpr Interface kMixerIf[] = {{params_id(kMixer, 1, 1), sizeof(MixerParams)}};
constexpr Interface kEnvelopeIf[] = {{params_id(kEnvelope, 1, 1), sizeof(EnvelopeParams)}};
constexpr Interface kFilterIf[] = {{params_id(kFilter, 1, 1), sizeof(FilterParams)},
                                   {params_id(kFilter, 2, 1), sizeof(FilterCoeffParams)}};
constexpr Interface kEqIf[] = {{params_id(kParamEq, 1, 1), sizeof(EqParams)},
                               {params_id(kParamEq, 2, 1), sizeof(EqCoeffParams)}};
constexpr Interface kPauserIf[] = {{params_id(kPauser, 1, 1), sizeof(PauserParams)}};
constexpr Interface kGeneratorIf[] = {{params_id(kGenerator, 1, 1), sizeof(GeneratorParams)}};
constexpr Interface kDistortionIf[] = {{params_id(kDistortion, 1, 1), sizeof(DistortionParams)}};
constexpr Interface kCompressorIf[] = {{params_id(kCompressor, 1, 1), sizeof(CompressorParams)},
                                       {params_id(kCompressor, 1, 2), sizeof(CompressorParams)}};
constexpr Interface kDelayIf[] = {{params_id(kDelay, 1, 1), sizeof(DelayParams)}};
constexpr Interface kPitchShiftIf[] = {{params_id(kPitchShift, 1, 1), sizeof(PitchShiftParams)}};
constexpr Interface kReverbIf[] = {{params_id(kReverb, 1, 1), sizeof(ReverbParams)},
                                   {params_id(kReverb, 1, 2), sizeof(ReverbParams)}};

template <usize N>
constexpr TypeInfo info(u32 type, const Interface (&i)[N], bool per_channel, u32 state_size) {
    return {type, i, static_cast<u32>(N), per_channel, state_size};
}

constexpr TypeInfo kTypes[] = {
    info(kPlayer, kPlayerIf, false, sizeof(PlayerStates)),
    info(kAt9Player, kAt9If, false, sizeof(PlayerStates)),
    info(kMixer, kMixerIf, false, 0),
    info(kEnvelope, kEnvelopeIf, false, sizeof(EnvelopeStates)),
    info(kFilter, kFilterIf, true, 0),
    info(kParamEq, kEqIf, true, 0),
    info(kPauser, kPauserIf, false, 0),
    info(kGenerator, kGeneratorIf, false, 0),
    info(kDistortion, kDistortionIf, true, 0),
    info(kCompressor, kCompressorIf, false, sizeof(CompressorStates)),
    info(kDelay, kDelayIf, true, 0),
    info(kPitchShift, kPitchShiftIf, true, 0),
    info(kReverb, kReverbIf, false, 0),  // (one set for both channels: eEarlyReflectionPattern is per channel)
    {kInputMixer, nullptr, 0, false, 0},
    {kOutput, nullptr, 0, false, 0},
};

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }
f32 db(f32 decibels) { return std::pow(10.0f, decibels / 20.0f); }
f32 millibels(f32 mb) { return std::pow(10.0f, mb / 2000.0f); }

template <typename... T>
bool fail(nx::string* message, std::format_string<T...> f, T&&... args) {
    if (message) *message = nx::format(f, std::forward<T>(args)...);
    return false;
}

// ---- biquads

struct Biquad {
    f32 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    f32 x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    bool pass = true;

    void set(const Coefficients& c) {
        b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
        pass = false;
    }
    // The filter of a mode (SCE_NGS_FILTER_*), as the usual designs (bilinear transform; one-pole for the
    // one-pole modes).
    void design(u32 mode, f32 frequency, f32 q, f32 gain_db, u32 rate) {
        pass = mode == 0 || mode > 12;
        if (pass) return;
        const f64 fs = rate;
        const f64 f = std::fmin(std::fmax(frequency, 1.0), fs * 0.499);
        const f64 w0 = 2 * kPi * f / fs, cw = std::cos(w0), sw = std::sin(w0);
        const f64 qq = std::fmax(q, 0.1);
        const f64 alpha = sw / (2 * qq);
        const f64 a = std::pow(10.0, gain_db / 40.0);
        f64 nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
        switch (mode) {
            case 1:   // lowpass, resonant
            case 12:  // (normalized: below the cut-off reduced by the resonance)
                nb0 = (1 - cw) / 2, nb1 = 1 - cw, nb2 = (1 - cw) / 2, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                if (mode == 12 && qq > 1) nb0 /= qq, nb1 /= qq, nb2 /= qq;
                break;
            case 2:  // highpass, resonant
                nb0 = (1 + cw) / 2, nb1 = -(1 + cw), nb2 = (1 + cw) / 2, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                break;
            case 3: {  // bandpass with a gain at the centre
                const f64 g = std::pow(10.0, gain_db / 20.0);
                nb0 = alpha * g, nb1 = 0, nb2 = -alpha * g, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                break;
            }
            case 4:  // bandpass, 0 dB at the centre
                nb0 = alpha, nb1 = 0, nb2 = -alpha, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                break;
            case 5:  // notch
                nb0 = 1, nb1 = -2 * cw, nb2 = 1, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                break;
            case 6:  // peak
                nb0 = 1 + alpha * a, nb1 = -2 * cw, nb2 = 1 - alpha * a, na0 = 1 + alpha / a, na1 = -2 * cw,
                na2 = 1 - alpha / a;
                break;
            case 7:    // high shelf
            case 8: {  // low shelf (slope 1)
                const f64 sa = sw / 2 * std::sqrt(2.0), r = 2 * std::sqrt(a) * sa;
                if (mode == 7) {
                    nb0 = a * ((a + 1) + (a - 1) * cw + r), nb1 = -2 * a * ((a - 1) + (a + 1) * cw),
                    nb2 = a * ((a + 1) + (a - 1) * cw - r), na0 = (a + 1) - (a - 1) * cw + r,
                    na1 = 2 * ((a - 1) - (a + 1) * cw), na2 = (a + 1) - (a - 1) * cw - r;
                } else {
                    nb0 = a * ((a + 1) - (a - 1) * cw + r), nb1 = 2 * a * ((a - 1) - (a + 1) * cw),
                    nb2 = a * ((a + 1) - (a - 1) * cw - r), na0 = (a + 1) + (a - 1) * cw + r,
                    na1 = -2 * ((a - 1) + (a + 1) * cw), na2 = (a + 1) + (a - 1) * cw - r;
                }
                break;
            }
            case 9:     // lowpass, one pole
            case 10: {  // highpass, one pole
                const f64 k = std::tan(w0 / 2);
                na0 = 1, na1 = (k - 1) / (k + 1), na2 = 0;
                if (mode == 9) nb0 = k / (1 + k), nb1 = nb0, nb2 = 0;
                else nb0 = 1 / (1 + k), nb1 = -nb0, nb2 = 0;
                break;
            }
            case 11:  // allpass
                nb0 = 1 - alpha, nb1 = -2 * cw, nb2 = 1 + alpha, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha;
                break;
        }
        b0 = static_cast<f32>(nb0 / na0), b1 = static_cast<f32>(nb1 / na0), b2 = static_cast<f32>(nb2 / na0);
        a1 = static_cast<f32>(na1 / na0), a2 = static_cast<f32>(na2 / na0);
    }
    f32 run(f32 x) {
        const f32 y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

// ---- player (PCM and VAG ADPCM)

// The VAG predictors: 128 filters of up to 4 taps on the samples before, in 1/8192ths. The first five are the
// PlayStation ADPCM filters (in 1/64ths there). The HE-VAG ones were measured against the SDK's vagconv2: the
// coefficients that make its output decode closest to its input. Every 2-tap filter (5-28) lands on this grid exactly;
// the 4-tap ones are the nearest grid points, within a step.
// clang-format off
constexpr i16 kVagCoef[128][4] = {
    {     0,      0,      0,      0}, {  7680,      0,      0,      0}, { 14720,  -6656,      0,      0}, { 12544,  -7040,      0,      0},
    { 15616,  -7680,      0,      0}, { 14731,  -7059,      0,      0}, { 14507,  -7366,      0,      0}, { 13920,  -7522,      0,      0},
    { 13133,  -7680,      0,      0}, { 12028,  -7680,      0,      0}, { 10764,  -7680,      0,      0}, {  9359,  -7680,      0,      0},
    {  7832,  -7680,      0,      0}, {  6201,  -7680,      0,      0}, {  4488,  -7680,      0,      0}, {  2717,  -7680,      0,      0},
    {   910,  -7680,      0,      0}, {  -910,  -7680,      0,      0}, { -2717,  -7680,      0,      0}, { -4488,  -7680,      0,      0},
    { -6201,  -7680,      0,      0}, { -7832,  -7680,      0,      0}, { -9359,  -7680,      0,      0}, {-10764,  -7680,      0,      0},
    {-12028,  -7680,      0,      0}, {-13133,  -7680,      0,      0}, {-13920,  -7522,      0,      0}, {-14507,  -7366,      0,      0},
    {-14731,  -7059,      0,      0}, {  5376,  -9216,   3328,  -3072}, { -6400,  -7168,  -3328,  -2304}, {-10496,  -7424,  -3584,  -1024},
    {  -167,  -2722,   -494,   -541}, { -7430,  -2221,  -2298,    424}, { -8000,  -3165,  -2814,    289}, {  6018,  -4750,   2649,  -1298},
    {  3798,  -6946,   3875,  -1216}, { -8237,  -2596,  -2071,    227}, {  9199,   1982,  -1382,  -2316}, { 13021,  -3044,  -3792,   1267},
    { 13112,  -4487,  -2250,   1665}, { -1668,  -3744,  -6456,    840}, {  7819,  -4328,   2111,   -506}, {  9571,  -1336,   -757,    487},
    { 10032,  -2562,    300,    199}, { -4745,  -4122,  -5486,  -1493}, { -5896,   2378,  -4787,  -6947}, { -1193,  -9117,  -1237,  -3114},
    {  2783,  -7108,  -1575,  -1447}, { -7334,  -2062,  -2212,    446}, {  6127,  -2577,   -315,    -18}, {  9457,  -1858,    102,    258},
    {  7876,  -4483,   2126,   -538}, { -7172,  -1795,  -2069,    482}, { -7358,  -2102,  -2233,    440}, { -9170,  -3509,  -2674,   -391},
    { -2638,  -2647,  -1929,  -1637}, {  1873,   9183,   1860,  -5746}, {  9214,   1859,  -1124,  -2427}, { 13204,  -3012,  -4139,   1370},
    { 12437,  -4792,   -256,    622}, { -2653,  -1144,  -3182,  -6878}, {  9331,  -1048,   -828,    507}, {  1642,   -620,   -946,  -4229},
    {  4246,  -7585,   -533,  -2259}, { -8988,  -3891,  -2807,     44}, { -2562,  -2735,  -1730,  -1899}, {  3182,   -483,   -714,  -1421},
    {  7936,  -3842,   2820,  -1019}, { 10069,  -2609,    314,    195}, {  8400,  -3297,   1551,   -155}, { -8529,  -2775,  -2432,   -336},
    {  9477,  -1882,    108,    256}, {    75,  -2241,   -298,  -6937}, { -9143,  -4160,  -2963,      5}, { -7270,  -1958,  -2156,    460},
    { -2740,   3745,   5936,  -1089}, {  8993,   1948,   -683,  -2704}, { 13101,  -2835,  -3854,   1055}, {  9543,  -1961,    130,    250},
    {  5272,  -4270,   3124,  -3157}, { -7696,  -3383,  -2907,   -456}, {  7309,   2523,    434,  -2461}, { 10275,  -2867,    391,    172},
    { 10940,  -3721,    665,     97}, {    24,   -310,  -1261,    320}, { -8122,  -2411,  -2311,   -271}, { -8511,  -3067,  -2337,    163},
    {   326,  -3846,    419,   -933}, {  8895,   2194,   -541,  -2880}, { 12073,  -1876,  -2017,   -601}, {  8729,  -3423,   1674,   -169},
    { 12950,  -3847,  -3007,   1946}, { 10038,  -2570,    302,    198}, {  9385,  -2757,   1008,     41}, { -4720,  -5006,  -2852,  -1161},
    {  7869,  -4326,   2135,   -501}, {  2450,  -8597,   1299,  -2780}, { 10192,  -2763,    360,    181}, { 11313,  -4213,    833,     53},
    { 10154,  -2716,    345,    185}, {  9638,  -1417,   -737,    482}, {  3854,  -4554,   2843,  -3397}, {  6699,  -5659,   2249,  -1074},
    { 11082,  -3908,    728,     80}, { -1026,  -9810,   -805,  -3462}, { 10396,  -3746,   1367,    -96}, { 10287,    988,  -1915,  -1437},
    {  7953,   3878,   -764,  -3263}, { 12689,  -3375,  -3354,   2079}, {  6641,   3166,    231,  -2089}, { -2348,  -7354,  -1944,  -4122},
    {  9290,  -4039,   1885,   -246}, {  4633,  -6403,   1748,  -1619}, { 11247,  -4125,    802,     61}, {  9807,  -2284,    219,    222},
    {  9736,  -1536,   -706,    473}, {  8440,  -3436,   1562,   -176}, {  9307,  -1021,   -835,    509}, {  1698,  -9025,    688,  -3037},
    { 10214,  -2791,    368,    179}, {  8390,   3248,   -758,  -2989}, {  7201,   3316,     46,  -2614}, {   -88,  -7809,   -538,  -4571},
    {  6193,  -5189,   2760,  -1245}, { 12325,  -1290,  -3284,    253}, { 13064,  -4075,  -2824,   1877}, {  5333,   2999,    775,  -1132},
};
// clang-format on

// What both players do: up to four buffers (loops, swaps, the end), lead-in, the sample limit, resampling to the
// system's rate, and the counters. Reading frames out of a buffer is the format's (PCM and VAG, or ATRAC9).
struct Streamer : Module {
    using Module::Module;

    struct Buffer {
        u32 address;
        i32 bytes;
        i16 loop_count, next;
    };
    // A frame read from the data: one sample per source channel, where it ends in which buffer, the bytes it took.
    struct Frame {
        f32 s[2] = {};
        i32 buffer = 0;
        u32 end = 0;
        u32 bytes = 0;
        bool real = false;
    };
    struct Settings {
        f32 frequency, scalar;
        i32 lead_in, limit, start_byte;
        i8 channels;
        i8 map[2];
        i8 start_buffer;
    };

    virtual Buffer buffer_at(i32 index) = 0;
    virtual Settings settings() = 0;
    virtual i32 looped_code() const = 0;
    // The next frame of buffer `b` at the read position (advancing it), or false at the buffer's end.
    virtual bool read(const Buffer& b, u32 channels, Frame& f) = 0;
    // A buffer starts (at its start or again for a loop).
    virtual void buffer_started(bool looped) { (void)looped; }
    // The voice starts.
    virtual void started() {}

    // (the read side: where the next frame comes from)
    i32 buffer = -1;
    u32 byte_pos = 0;
    i32 loops_done = 0;
    // (the play side: the frame being played and the next, for interpolation)
    Frame x0, x1;
    f64 frac = 0;
    bool began = false, ended = true;
    i64 lead_in = 0;
    // (the counters: samples generated, and the bytes of the frames played through)
    i64 generated = 0;
    i32 samples_total = 0, bytes_total = 0, bytes_since_key_on = 0;
    i32 played_buffer = 0;
    u32 played_end = 0;

    void reset(const Context&) override {
        const Settings s = settings();
        buffer = s.start_buffer;
        byte_pos = static_cast<u32>(s.start_byte);
        loops_done = 0;
        x0 = x1 = Frame{};
        frac = 0;
        began = false;
        ended = false;
        lead_in = s.lead_in;
        generated = 0;
        bytes_since_key_on = 0;
        played_buffer = buffer;
        played_end = byte_pos;
        started();
        buffer_started(false);
    }
    bool state(void* out, u32 size) const override {
        if (size < sizeof(PlayerStates)) return false;
        PlayerStates s{static_cast<i32>(played_end), played_buffer, static_cast<i32>(generated), bytes_since_key_on,
                       samples_total, bytes_total};
        std::memcpy(out, &s, sizeof s);
        return true;
    }

    void raise(i32 data, i32 data2, u32 ptr) {
        event.raised = true;
        event.data = data, event.data2 = data2, event.ptr = ptr;
    }

    // The end of the current buffer: loop it, go to the next, or end (false).
    bool next_buffer() {
        const Buffer b = buffer_at(buffer);
        if (b.loop_count == -1 || loops_done < b.loop_count) {
            ++loops_done;
            byte_pos = 0;
            raise(looped_code(), loops_done, b.address);
            buffer_started(true);
            return b.bytes > 0;
        }
        const i32 previous = buffer;
        buffer = b.next;
        loops_done = 0;
        byte_pos = 0;
        if (buffer < 0 || buffer > 3) {
            buffer = previous;
            raise(0, 0, 0);  // SCE_NGS_PLAYER_END_OF_DATA, SCE_NGS_AT9_END_OF_DATA
            return false;
        }
        raise(1, previous, buffer_at(buffer).address);  // SCE_NGS_PLAYER_SWAPPED_BUFFER, SCE_NGS_AT9_SWAPPED_BUFFER
        buffer_started(false);
        return true;
    }

    // The next frame of the data; not real at its end.
    Frame fetch() {
        Frame f;
        const u32 nch = settings().channels == 2 ? 2 : 1;
        for (u32 guard = 0; guard < 16; ++guard) {
            if (ended || buffer < 0 || buffer > 3) return f;
            if (read(buffer_at(buffer), nch, f)) {
                f.buffer = buffer, f.end = byte_pos, f.real = true;
                return f;
            }
            if (ended || !next_buffer()) return Frame{};
        }
        return Frame{};  // (a chain of empty buffers)
    }

    // The frame being played is played through: count it, move to the next.
    void advance() {
        bytes_since_key_on += static_cast<i32>(x0.bytes);
        bytes_total += static_cast<i32>(x0.bytes);
        played_buffer = x0.buffer;
        played_end = x0.end;
        x0 = x1;
        if (x0.real) x1 = fetch();
    }

    void render(const Context& c, Packet& out, bool* end) {
        const u32 g = c.granularity;
        out.clear(g);
        if (ended) {
            *end = true;
            return;
        }
        const Settings s = settings();
        u32 i = 0;
        if (lead_in > 0) {
            const u32 skip = static_cast<u32>(nx::min<i64>(lead_in, g));
            lead_in -= skip;
            i = skip;
        }
        if (i < g && !began) {
            began = true;
            x0 = fetch();
            if (x0.real) x1 = fetch();
        }
        const f64 step = std::fmin(std::fmax(static_cast<f64>(s.frequency) * s.scalar / c.sample_rate, 0.0), 4.0);
        const u32 nch = s.channels == 2 ? 2 : 1;
        for (; i < g; ++i) {
            if (!x0.real || (s.limit > 0 && generated >= s.limit)) {
                if (x0.real) raise(0, 0, 0);  // (the limit reached)
                ended = true;
                break;
            }
            for (u32 ch = 0; ch < c.channels; ++ch) {
                const i32 src = s.map[ch];
                if (src < 0 || static_cast<u32>(src) >= nch) continue;
                const f32 a = x0.s[src], b = x1.real ? x1.s[src] : 0.0f;
                out.s[ch][i] = frac == 0 ? a : static_cast<f32>(a + (b - a) * frac);
            }
            ++generated;
            ++samples_total;
            frac += step;
            while (frac >= 1.0 && x0.real) {
                frac -= 1.0;
                advance();
            }
        }
        *end = ended;
    }
};

// The player: 16-bit PCM, or VAG ADPCM (16-byte blocks of 28 samples; stereo takes a block of each channel in turn,
// the left first, as vagconv2 makes it from a stereo WAV).
struct Player : Streamer {
    using Streamer::Streamer;

    f64 hist[2][4] = {};  // (the samples before, the last first)
    f32 decoded[2][28] = {};
    u32 decoded_index = 28;
    u32 block_bytes = 0;  // (owed to the block's first sample)

    PlayerParams& p() { return *param<PlayerParams>(); }

    void defaults() override {
        write_header(kPlayerIf[0].id, sizeof(PlayerParams));
        PlayerParams& q = p();
        for (auto& b : q.buffs) b = PlayerBuffer{0, 0, 0, -1};
        q.frequency = 48000.0f;
        q.scalar = 1.0f;
        q.channels = 1;
        q.map[0] = 0, q.map[1] = 0;
    }
    bool check(const u8* raw, nx::string* m) const override {
        PlayerParams q;
        std::memcpy(&q, raw, sizeof q);
        for (u32 i = 0; i < 4; ++i) {
            if (q.buffs[i].bytes < 0) return fail(m, "buffs[{}].nNumBytes {} < 0", i, q.buffs[i].bytes);
            if (q.buffs[i].loop_count < -1) return fail(m, "buffs[{}].nLoopCount {} < -1", i, q.buffs[i].loop_count);
            if (q.buffs[i].next < -1 || q.buffs[i].next > 3)
                return fail(m, "buffs[{}].nNextBuff {} out of range", i, q.buffs[i].next);
        }
        if (!(q.frequency >= 0 && q.frequency <= 192000)) return fail(m, "fPlaybackFrequency {} out of range", q.frequency);
        if (!(q.scalar >= 0 && q.scalar <= 10)) return fail(m, "fPlaybackScalar {} out of range", q.scalar);
        if (q.lead_in < 0 || q.limit < 0 || q.start_byte < 0) return fail(m, "negative sample or byte count");
        if (q.channels < 1 || q.channels > 2) return fail(m, "nChannels {} out of range", q.channels);
        for (i8 c : q.map)
            if (c < -1 || c > 1) return fail(m, "nChannelMap {} out of range", c);
        if (q.type < 0 || q.type > 1) return fail(m, "nType {} out of range", q.type);
        if (q.start_buffer < 0 || q.start_buffer > 3) return fail(m, "nStartBuffer {} out of range", q.start_buffer);
        return true;
    }

    Buffer buffer_at(i32 index) override {
        const PlayerBuffer& b = p().buffs[index];
        return Buffer{b.buffer, b.bytes, b.loop_count, b.next};
    }
    Settings settings() override {
        const PlayerParams& q = p();
        return Settings{q.frequency, q.scalar, q.lead_in, q.limit, q.start_byte, q.channels, {q.map[0], q.map[1]}, q.start_buffer};
    }
    i32 looped_code() const override { return 2; }  // SCE_NGS_PLAYER_LOOPED_BUFFER
    void started() override {
        for (auto& h : hist)
            for (f64& v : h) v = 0;
    }
    // A swap carries on with the samples before (a stream split over buffers decodes as one); a loop starts again.
    void buffer_started(bool looped) override {
        if (looped) started();
        decoded_index = 28;
        block_bytes = 0;
    }

    // The prediction is not rounded: the encoder tracks a decoder that keeps the fraction, and an integer one drifts
    // a few steps away from what it meant.
    void decode_block(const u8* block, u32 ch) {
        const u32 shift = block[0] & 0x0F;
        // (HE-VAG takes 3 more bits of the filter from byte 1)
        const u32 filter = ((block[0] >> 4) | ((block[1] >> 4) << 4)) & 0x7F;
        const i16* c = kVagCoef[filter];
        f64* h = hist[ch];
        for (u32 i = 0; i < 28; ++i) {
            const u8 byte = block[2 + i / 2];
            const i32 nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            const i32 d = static_cast<i32>(static_cast<i16>(static_cast<u16>(nibble << 12))) >> (shift > 12 ? 12 : shift);
            f64 s = d + (c[0] * h[0] + c[1] * h[1] + c[2] * h[2] + c[3] * h[3]) / 8192.0;
            s = s < -32768.0 ? -32768.0 : s > 32767.0 ? 32767.0 : s;
            h[3] = h[2], h[2] = h[1], h[1] = h[0], h[0] = s;
            decoded[ch][i] = static_cast<f32>(s);
        }
    }

    bool read(const Buffer& b, u32 nch, Frame& f) override {
        GuestMemory& mem = runtime().memory();
        const u32 size = b.bytes > 0 ? static_cast<u32>(b.bytes) : 0;
        if (p().type == 0) {
            const u32 frame = 2 * nch;
            if (byte_pos + frame > size || !b.address || !mem.valid(b.address + byte_pos, frame)) return false;
            const u8* src = mem.host(b.address + byte_pos);
            for (u32 c = 0; c < nch; ++c) {
                i16 s;
                std::memcpy(&s, src + 2 * c, 2);
                f.s[c] = static_cast<f32>(s) / 32768.0f;
            }
            byte_pos += frame;
            f.bytes = frame;
            return true;
        }
        if (decoded_index >= 28) {
            const u32 block = 16 * nch;
            if (byte_pos + block > size || !b.address || !mem.valid(b.address + byte_pos, block)) return false;
            const u8* src = mem.host(b.address + byte_pos);
            for (u32 c = 0; c < nch; ++c) decode_block(src + 16 * c, c);
            byte_pos += block;
            block_bytes = block;
            decoded_index = 0;
        }
        for (u32 c = 0; c < nch; ++c) f.s[c] = decoded[c][decoded_index] / 32768.0f;
        ++decoded_index;
        f.bytes = block_bytes;
        block_bytes = 0;
        return true;
    }
};

// The ATRAC9 player: superframes of 1 or 4 frames decoded by LibAtrac9; each buffer can drop samples at its start
// and end (sceNgsAT9GetSectionDetails says how many, for seeking and looping on any sample).
struct At9Player : Streamer {
    using Streamer::Streamer;

    struct Decoder {
        void* handle = nullptr;
        ~Decoder() {
            if (handle) Atrac9ReleaseHandle(handle);
        }
    } decoder;
    u32 config = 0;  // (the configuration the decoder has)
    bool config_ok = false;
    Atrac9CodecInfo codec{};
    nx::vector<i16> pcm;  // (the superframe decoded)
    u32 pcm_index = 0, pcm_count = 0;
    u32 superframe_bytes_owed = 0;
    i64 sample_in_buffer = 0;  // (decoded samples of this buffer handed out or dropped)

    At9Params& p() { return *param<At9Params>(); }

    void defaults() override {
        write_header(kAt9If[0].id, sizeof(At9Params));
        At9Params& q = p();
        for (auto& b : q.buffs) b = At9Buffer{0, 0, 0, -1, 0, 0};
        q.frequency = 48000.0f;
        q.scalar = 1.0f;
        q.channels = 1;
    }
    bool check(const u8* raw, nx::string* m) const override {
        At9Params q;
        std::memcpy(&q, raw, sizeof q);
        for (u32 i = 0; i < 4; ++i) {
            const At9Buffer& b = q.buffs[i];
            if (b.bytes < 0) return fail(m, "buffs[{}].nNumBytes {} < 0", i, b.bytes);
            if (b.loop_count < -1) return fail(m, "buffs[{}].nLoopCount {} < -1", i, b.loop_count);
            if (b.next < -1 || b.next > 3) return fail(m, "buffs[{}].nNextBuff {} out of range", i, b.next);
            if (b.discard_start < 0 || b.discard_start > 2048 || b.discard_end < 0 || b.discard_end > 2048)
                return fail(m, "buffs[{}] samples to discard out of range", i);
        }
        if (!(q.frequency >= 0 && q.frequency <= 192000)) return fail(m, "fPlaybackFrequency {} out of range", q.frequency);
        if (!(q.scalar >= 0 && q.scalar <= 10)) return fail(m, "fPlaybackScalar {} out of range", q.scalar);
        if (q.lead_in < 0 || q.limit < 0) return fail(m, "negative sample count");
        if (q.channels < 1 || q.channels > 2) return fail(m, "nChannels {} out of range", q.channels);
        for (i8 c : q.map)
            if (c < -1 || c > 1) return fail(m, "nChannelMap {} out of range", c);
        return true;
    }

    Buffer buffer_at(i32 index) override {
        const At9Buffer& b = p().buffs[index];
        return Buffer{b.buffer, b.bytes, b.loop_count, b.next};
    }
    Settings settings() override {
        const At9Params& q = p();
        return Settings{q.frequency, q.scalar, q.lead_in, q.limit, 0, q.channels, {q.map[0], q.map[1]}, 0};
    }
    i32 looped_code() const override { return 4; }  // SCE_NGS_AT9_LOOPED_BUFFER

    void started() override {
        const u32 cfg = static_cast<u32>(p().config);
        if (!decoder.handle || cfg != config) {
            if (decoder.handle) Atrac9ReleaseHandle(decoder.handle);
            decoder.handle = Atrac9GetHandle();
            config = cfg;
            u8 bytes[4];
            std::memcpy(bytes, &cfg, 4);
            config_ok = decoder.handle && Atrac9InitDecoder(decoder.handle, bytes) == 0 &&
                        Atrac9GetCodecInfo(decoder.handle, &codec) == 0;
            if (config_ok) pcm.assign(static_cast<usize>(codec.frameSamples) * codec.framesInSuperframe * codec.channels, 0);
        }
        if (!config_ok) {
            raise(2, 0, 0);  // SCE_NGS_AT9_HEADER_ERROR
            ended = true;
        }
    }
    void buffer_started(bool) override {
        pcm_index = pcm_count = 0;
        superframe_bytes_owed = 0;
        sample_in_buffer = 0;
    }

    // Decodes the superframe at the read position; false at the buffer's end (or on an error, which ends the voice).
    bool decode(const Buffer& b) {
        GuestMemory& mem = runtime().memory();
        const u32 size = b.bytes > 0 ? static_cast<u32>(b.bytes) : 0;
        const u32 sf = static_cast<u32>(codec.superframeSize);
        if (byte_pos + sf > size || !b.address || !mem.valid(b.address + byte_pos, sf)) return false;
        const u8* src = mem.host(b.address + byte_pos);
        u32 used_total = 0;
        for (i32 k = 0; k < codec.framesInSuperframe; ++k) {
            int used = 0;
            if (used_total >= sf || Atrac9Decode(decoder.handle, src + used_total,
                                                 pcm.data() + static_cast<usize>(k) * codec.frameSamples * codec.channels,
                                                 &used) != 0) {
                raise(3, static_cast<i32>(byte_pos + used_total), b.address);  // SCE_NGS_AT9_DECODE_ERROR
                ended = true;
                return false;
            }
            used_total += static_cast<u32>(used);
        }
        byte_pos += sf;
        superframe_bytes_owed += sf;
        pcm_index = 0;
        pcm_count = static_cast<u32>(codec.frameSamples * codec.framesInSuperframe);
        return true;
    }

    bool read(const Buffer& b, u32 nch, Frame& f) override {
        if (!config_ok) return false;
        const At9Buffer& raw = p().buffs[buffer];
        const u32 sf = static_cast<u32>(codec.superframeSize);
        const i64 total = sf ? static_cast<i64>(b.bytes > 0 ? b.bytes : 0) / sf * codec.frameSamples * codec.framesInSuperframe : 0;
        const i64 last = total - raw.discard_end;
        for (;;) {
            if (sample_in_buffer >= last) return false;
            if (pcm_index >= pcm_count && !decode(b)) return false;
            const i16* s = pcm.data() + static_cast<usize>(pcm_index) * codec.channels;
            ++pcm_index;
            const bool keep = sample_in_buffer >= raw.discard_start;
            ++sample_in_buffer;
            if (!keep) continue;
            for (u32 c = 0; c < nch; ++c) f.s[c] = static_cast<f32>(s[nx::min<u32>(c, static_cast<u32>(codec.channels) - 1)]) / 32768.0f;
            f.bytes = superframe_bytes_owed;
            superframe_bytes_owed = 0;
            return true;
        }
    }
};

// ---- generator

struct Generator : Module {
    using Module::Module;
    f64 phase = 0;
    u32 noise = 0x12345678;

    void defaults() override {
        write_header(kGeneratorIf[0].id, sizeof(GeneratorParams));
        GeneratorParams& q = *param<GeneratorParams>();
        q.mode = 0;
        q.frequency = 440;
        q.amplitude = 1.0f;
        q.pulse_width = 0.5f;
    }
    bool check(const u8* raw, nx::string* m) const override {
        GeneratorParams q;
        std::memcpy(&q, raw, sizeof q);
        if (q.mode > 5) return fail(m, "eGeneratorMode {} out of range", q.mode);
        if (q.frequency < 0 || q.frequency > 12000) return fail(m, "nFrequency {} out of range", q.frequency);
        if (!(q.amplitude >= 0 && q.amplitude <= 1)) return fail(m, "fAmplitude {} out of range", q.amplitude);
        if (!(q.pulse_width >= 0 && q.pulse_width <= 1)) return fail(m, "fPulseWidth {} out of range", q.pulse_width);
        if (q.phase_angle > 360) return fail(m, "uPhaseAngle {} out of range", q.phase_angle);
        return true;
    }
    void reset(const Context& c) override {
        const GeneratorParams& q = *param<GeneratorParams>();
        phase = static_cast<f64>(q.sample_offset) * q.frequency / c.sample_rate;
        phase -= std::floor(phase);
        noise = 0x12345678;
    }
    void render(const Context& c, Packet& out) {
        const GeneratorParams& q = *param<GeneratorParams>();
        const f64 inc = static_cast<f64>(q.frequency) / c.sample_rate;
        const f64 offset = q.phase_angle / 360.0;
        for (u32 i = 0; i < c.granularity; ++i) {
            f64 ph = phase + offset;
            ph -= std::floor(ph);
            f64 v = 0;
            switch (q.mode) {
                case 0: v = std::sin(2 * kPi * ph); break;
                case 1: v = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4; break;
                case 2: v = 2 * ph - 1; break;
                case 3:
                case 4:
                    noise = noise * 1664525u + 1013904223u;
                    v = static_cast<f64>(static_cast<i32>(noise)) / 2147483648.0;
                    break;
                case 5: v = ph < q.pulse_width ? 1 : -1; break;
            }
            const f32 s = static_cast<f32>(v * q.amplitude);
            for (u32 ch = 0; ch < c.channels; ++ch) out.s[ch][i] = s;
            phase += inc;
            phase -= std::floor(phase);
        }
    }
};

// ---- mixer and pauser

struct Mixer : Module {
    using Module::Module;
    void defaults() override {
        write_header(kMixerIf[0].id, sizeof(MixerParams));
        MixerParams& q = *param<MixerParams>();
        q.gain[0] = q.gain[1] = 1.0f;
    }
    bool check(const u8* raw, nx::string* m) const override {
        MixerParams q;
        std::memcpy(&q, raw, sizeof q);
        for (u32 i = 0; i < 2; ++i)
            if (!(q.gain[i] >= 0 && q.gain[i] <= 8)) return fail(m, "fGainIn[{}] {} out of range", i, q.gain[i]);
        if (q.lead_in > 256 || q.lead_out > 256) return fail(m, "pause lead in or out above 256 samples");
        return true;
    }
};

struct Pauser : Module {
    using Module::Module;
    void defaults() override {
        write_header(kPauserIf[0].id, sizeof(PauserParams));
    }
    bool check(const u8* raw, nx::string* m) const override {
        PauserParams q;
        std::memcpy(&q, raw, sizeof q);
        if (q.lead_in > 256 || q.lead_out > 256) return fail(m, "pause lead in or out above 256 samples");
        return true;
    }
};

// ---- envelope

f32 curve(f32 x, u32 type) { return type == 1 ? x * x * (3 - 2 * x) : x; }

struct Envelope : Module {
    using Module::Module;
    bool releasing = false, release_started = false, done = false;
    f64 release_elapsed_ms = 0;
    f32 release_from = 0;
    f32 height = 1.0f, position = 0, release_scale = 1.0f;
    i32 point = 0;

    void defaults() override {
        write_header(kEnvelopeIf[0].id, sizeof(EnvelopeParams));
        EnvelopeParams& q = *param<EnvelopeParams>();
        q.points[0] = EnvelopePoint{0, 1.0f, 0};
        q.num_points = 1;
        q.loop_end = -1;  // SCE_NGS_ENVELOPE_NO_LOOP
    }
    bool check(const u8* raw, nx::string* m) const override {
        EnvelopeParams q;
        std::memcpy(&q, raw, sizeof q);
        if (q.num_points > 4) return fail(m, "uNumPoints {} above 4", q.num_points);
        if (q.loop_start > 3) return fail(m, "uLoopStart {} above 3", q.loop_start);
        if (q.loop_end < -3 || q.loop_end > 3) return fail(m, "nLoopEnd {} out of range", q.loop_end);
        for (u32 i = 0; i < 4; ++i) {
            if (!(q.points[i].amplitude >= 0 && q.points[i].amplitude <= 100))
                return fail(m, "envelopePoints[{}].fAmplitude {} out of range", i, q.points[i].amplitude);
            if (q.points[i].curve > 1) return fail(m, "envelopePoints[{}].eCurveType {} out of range", i, q.points[i].curve);
        }
        return true;
    }
    void reset(const Context&) override {
        releasing = release_started = done = false;
        release_elapsed_ms = 0;
        height = 1.0f, position = 0, release_scale = 1.0f, point = 0;
    }
    bool state(void* out, u32 size) const override {
        if (size < sizeof(EnvelopeStates)) return false;
        EnvelopeStates s{height, position, release_scale, point, releasing ? 1 : 0};
        std::memcpy(out, &s, sizeof s);
        return true;
    }

    // The amplitude at a time (ms since the voice became active); `end` says if a terminating end was reached.
    f32 amplitude(f64 t, EnvelopeEnd* end) {
        const EnvelopeParams& q = *param<EnvelopeParams>();
        const u32 n = nx::min<u32>(q.num_points, 4);
        if (n == 0) return 1.0f;
        f64 starts[5] = {};
        for (u32 i = 0; i < n; ++i) starts[i + 1] = starts[i] + q.points[i].msecs;
        if (q.loop_end >= 0 && static_cast<u32>(q.loop_end) < n && q.loop_start < static_cast<u32>(q.loop_end)) {
            const f64 from = starts[q.loop_start], to = starts[q.loop_end];
            if (t >= to && to > from) t = from + std::fmod(t - to, to - from);
        }
        for (u32 i = 0; i + 1 < n; ++i) {
            if (t < starts[i + 1]) {
                point = static_cast<i32>(i);
                const f64 len = q.points[i].msecs;
                const f32 x = len > 0 ? static_cast<f32>((t - starts[i]) / len) : 1.0f;
                const f32 a = q.points[i].amplitude, b = q.points[i + 1].amplitude;
                return a + (b - a) * curve(x, q.points[i].curve);
            }
        }
        point = static_cast<i32>(n - 1);
        if ((q.loop_end == -2 || q.loop_end == -3) && t >= starts[n]) *end = q.loop_end == -2 ? EnvelopeEnd::Kill : EnvelopeEnd::Release;
        return q.points[n - 1].amplitude;
    }

    EnvelopeEnd run(const Context& c, Packet& io) {
        const EnvelopeParams& q = *param<EnvelopeParams>();
        EnvelopeEnd result = EnvelopeEnd::None;
        const f64 ms_per_sample = 1000.0 / c.sample_rate;
        for (u32 i = 0; i < c.granularity; ++i) {
            const f64 t = static_cast<f64>(c.active_samples + i) * ms_per_sample;
            position = static_cast<f32>(t);
            EnvelopeEnd end = EnvelopeEnd::None;
            f32 a = amplitude(t, &end);
            if (releasing) {
                if (!release_started) {
                    release_started = true;
                    release_from = 1.0f;
                }
                release_scale = q.release_msecs ? static_cast<f32>(1.0 - release_elapsed_ms / q.release_msecs) : 0.0f;
                if (release_scale <= 0) {
                    release_scale = 0;
                    done = true;
                }
                release_elapsed_ms += ms_per_sample;
                a *= release_scale;
            } else if (end != EnvelopeEnd::None && result == EnvelopeEnd::None) {
                result = end;
                if (end == EnvelopeEnd::Release) releasing = true;
            }
            height = a;
            for (u32 ch = 0; ch < c.channels; ++ch) io.s[ch][i] *= a;
            if (done) {
                for (u32 k = i + 1; k < c.granularity; ++k)
                    for (u32 ch = 0; ch < c.channels; ++ch) io.s[ch][k] = 0;
                return EnvelopeEnd::Finished;
            }
        }
        return result == EnvelopeEnd::Kill ? EnvelopeEnd::Kill : EnvelopeEnd::None;
    }
};

// ---- filters

struct Filter : Module {
    using Module::Module;
    Biquad bq[kMaxChannels];

    void defaults() override {
        write_header(kFilterIf[0].id, sizeof(FilterParams));
        for (u32 ch = 0; ch < param_channels(); ++ch) {
            FilterParams& q = *param<FilterParams>(ch);
            q.desc = Desc{kFilterIf[0].id, sizeof(FilterParams)};
            q.mode = 0;
            q.frequency = 1000.0f;
            q.resonance = 0.707f;
            q.gain = 0.0f;
        }
    }
    bool check(const u8* raw, nx::string* m) const override {
        Desc d;
        std::memcpy(&d, raw, sizeof d);
        if (d.id != kFilterIf[0].id) return true;
        FilterParams q;
        std::memcpy(&q, raw, sizeof q);
        if (q.mode > 12) return fail(m, "eFilterMode {} out of range", q.mode);
        if (!(q.frequency >= 20 && q.frequency <= 23500)) return fail(m, "fFrequency {} out of range", q.frequency);
        if (!(q.resonance >= 0.5f && q.resonance <= 10)) return fail(m, "fResonance {} out of range", q.resonance);
        if (!(q.gain >= -90 && q.gain <= 24)) return fail(m, "fGain {} out of range", q.gain);
        return true;
    }
    void apply(const Context& c) override {
        for (u32 ch = 0; ch < kMaxChannels; ++ch) {
            const u32 pc = info_->per_channel ? nx::min(ch, param_channels() - 1) : 0;
            if (iface_ == kFilterIf[1].id) {
                bq[ch].set(param<FilterCoeffParams>(pc)->c);
            } else {
                const FilterParams& q = *param<FilterParams>(pc);
                bq[ch].design(q.mode, q.frequency, q.resonance, q.gain, c.sample_rate);
            }
        }
    }
    void reset(const Context&) override {
        for (auto& b : bq) b.clear();
    }
    void process(const Context& c, Packet& io) override {
        for (u32 ch = 0; ch < c.channels; ++ch) {
            if (bq[ch].pass) continue;
            for (u32 i = 0; i < c.granularity; ++i) io.s[ch][i] = bq[ch].run(io.s[ch][i]);
        }
    }
};

struct ParamEq : Module {
    using Module::Module;
    Biquad bq[kMaxChannels][4];

    void defaults() override {
        for (u32 ch = 0; ch < param_channels(); ++ch) {
            EqParams& q = *param<EqParams>(ch);
            q.desc = Desc{kEqIf[0].id, sizeof(EqParams)};
            for (auto& f : q.filter) f = EqFilter{0, 1000.0f, 0.707f, 0.0f};
        }
        iface_ = kEqIf[0].id;
    }
    bool check(const u8* raw, nx::string* m) const override {
        Desc d;
        std::memcpy(&d, raw, sizeof d);
        if (d.id != kEqIf[0].id) return true;
        EqParams q;
        std::memcpy(&q, raw, sizeof q);
        for (u32 i = 0; i < 4; ++i) {
            const EqFilter& f = q.filter[i];
            if (f.mode > 12) return fail(m, "filter[{}].eFilterMode {} out of range", i, f.mode);
            if (!(f.frequency >= 20 && f.frequency <= 23500)) return fail(m, "filter[{}].fFrequency {} out of range", i, f.frequency);
            if (!(f.resonance >= 0.5f && f.resonance <= 10)) return fail(m, "filter[{}].fResonance {} out of range", i, f.resonance);
            if (!(f.gain >= -90 && f.gain <= 24)) return fail(m, "filter[{}].fGain {} out of range", i, f.gain);
        }
        return true;
    }
    void apply(const Context& c) override {
        for (u32 ch = 0; ch < kMaxChannels; ++ch) {
            const u32 pc = nx::min(ch, param_channels() - 1);
            for (u32 k = 0; k < 4; ++k) {
                if (iface_ == kEqIf[1].id) {
                    bq[ch][k].set(param<EqCoeffParams>(pc)->c[k]);
                } else {
                    const EqFilter& f = param<EqParams>(pc)->filter[k];
                    bq[ch][k].design(f.mode, f.frequency, f.resonance, f.gain, c.sample_rate);
                }
            }
        }
    }
    void reset(const Context&) override {
        for (auto& ch : bq)
            for (auto& b : ch) b.clear();
    }
    void process(const Context& c, Packet& io) override {
        for (u32 ch = 0; ch < c.channels; ++ch)
            for (auto& b : bq[ch]) {
                if (b.pass) continue;
                for (u32 i = 0; i < c.granularity; ++i) io.s[ch][i] = b.run(io.s[ch][i]);
            }
    }
};

// ---- distortion

struct Distortion : Module {
    using Module::Module;
    void defaults() override {
        for (u32 ch = 0; ch < param_channels(); ++ch)
            *param<DistortionParams>(ch) = DistortionParams{{kDistortionIf[0].id, sizeof(DistortionParams)}, 1, 0, 1, 0, 1, 0};
        iface_ = kDistortionIf[0].id;
    }
    bool check(const u8* raw, nx::string* m) const override {
        DistortionParams q;
        std::memcpy(&q, raw, sizeof q);
        const f32 v[] = {q.a, q.b, q.clip, q.gate, q.wet, q.dry};
        const f32 hi[] = {10, 10, 4, 4, 4, 4};
        for (u32 i = 0; i < 6; ++i)
            if (!(v[i] >= 0 && v[i] <= hi[i])) return fail(m, "distortion parameter {} = {} out of range", i, v[i]);
        return true;
    }
    void process(const Context& c, Packet& io) override {
        for (u32 ch = 0; ch < c.channels; ++ch) {
            const DistortionParams& q = *param<DistortionParams>(nx::min(ch, param_channels() - 1));
            for (u32 i = 0; i < c.granularity; ++i) {
                const f32 x = io.s[ch][i];
                f32 y = x >= 0 ? x * q.a - x * x * q.b : x * q.a + x * x * q.b;
                if (std::fabs(y) < q.gate) y = 0;
                if (y > q.clip) y = q.clip;
                if (y < -q.clip) y = -q.clip;
                io.s[ch][i] = y * q.wet + x * q.dry;
            }
        }
    }
};

// ---- compressor

struct Compressor : Module {
    using Module::Module;
    f32 envelope_[kMaxChannels] = {};
    f32 gain_db[kMaxChannels] = {};
    CompressorStates levels{};

    void defaults() override {
        write_header(kCompressorIf[0].id, sizeof(CompressorParams));
        CompressorParams& q = *param<CompressorParams>();
        q.ratio = 1.0f;
        q.threshold = 0.0f;
        q.attack = 0.01f;
        q.release = 0.1f;
        q.makeup = 0.0f;
        q.peak_mode = 1;
    }
    bool check(const u8* raw, nx::string* m) const override {
        CompressorParams q;
        std::memcpy(&q, raw, sizeof q);
        if (!(q.ratio >= 0 && q.ratio <= 1)) return fail(m, "fRatio {} out of range", q.ratio);
        if (!(q.threshold >= -90 && q.threshold <= 0)) return fail(m, "fThreshold {} out of range", q.threshold);
        if (!(q.attack >= 0.001f && q.attack <= 10)) return fail(m, "fAttack {} out of range", q.attack);
        if (!(q.release >= 0.001f && q.release <= 20)) return fail(m, "fRelease {} out of range", q.release);
        if (!(q.makeup >= -90 && q.makeup <= 24)) return fail(m, "fMakeupGain {} out of range", q.makeup);
        if (!(q.soft_knee >= 0 && q.soft_knee <= 10)) return fail(m, "fSoftKnee {} out of range", q.soft_knee);
        return true;
    }
    void reset(const Context&) override {
        for (u32 ch = 0; ch < kMaxChannels; ++ch) envelope_[ch] = 0, gain_db[ch] = 0;
        levels = {};
    }
    bool state(void* out, u32 size) const override {
        if (size < sizeof(CompressorStates)) return false;
        std::memcpy(out, &levels, sizeof levels);
        return true;
    }
    void run(const Context& c, Packet& io, const Packet* side) {
        const CompressorParams& q = *param<CompressorParams>();
        const f32 attack = std::exp(-1.0f / (q.attack * c.sample_rate));
        const f32 release = std::exp(-1.0f / (q.release * c.sample_rate));
        const f32 makeup = db(q.makeup);
        const f32 knee = q.soft_knee;
        const bool chain = side && !(iface_ == kCompressorIf[1].id && side == nullptr);
        for (u32 i = 0; i < c.granularity; ++i) {
            f32 level[kMaxChannels] = {};
            for (u32 ch = 0; ch < c.channels; ++ch) {
                const f32 x = chain ? side->s[ch][i] : io.s[ch][i];
                const f32 det = q.peak_mode ? std::fabs(x) : x * x;
                const f32 coef = det > envelope_[ch] ? attack : release;
                envelope_[ch] = det + (envelope_[ch] - det) * coef;
                level[ch] = q.peak_mode ? envelope_[ch] : std::sqrt(envelope_[ch]);
            }
            if (q.stereo_link && c.channels == 2) level[0] = level[1] = nx::max(level[0], level[1]);
            for (u32 ch = 0; ch < c.channels; ++ch) {
                const f32 in_db = 20.0f * std::log10(nx::max(level[ch], 1e-9f));
                const f32 over = in_db - q.threshold;
                f32 reduce = 0;
                if (knee > 0 && 2 * std::fabs(over) <= knee) {
                    const f32 k = over + knee / 2;
                    reduce = (q.ratio - 1.0f) * k * k / (2 * knee);
                } else if (over > 0) {
                    reduce = (q.ratio - 1.0f) * over;
                }
                gain_db[ch] = reduce;
                const f32 g = db(reduce) * makeup;
                io.s[ch][i] *= g;
                levels.input[ch] = level[ch];
                levels.output[ch] = chain ? g : level[ch] * g;
            }
        }
    }
    void process(const Context& c, Packet& io) override { run(c, io, nullptr); }
};

// ---- delay

struct Delay : Module {
    using Module::Module;
    nx::vector<f32> line[kMaxChannels];
    u32 write = 0;
    f64 lfo = 0;
    f32 tap_state[kMaxChannels][4] = {};

    void defaults() override {
        for (u32 ch = 0; ch < param_channels(); ++ch) {
            DelayParams& q = *param<DelayParams>(ch);
            q = DelayParams{};
            q.desc = Desc{kDelayIf[0].id, sizeof(DelayParams)};
            q.dry = 1.0f;
            q.mod_rate = 1.0f;
            for (auto& t : q.taps) t = DelayTap{0, 0, 0, 0, 20000.0f, 0, 0};
        }
        iface_ = kDelayIf[0].id;
    }
    bool check(const u8* raw, nx::string* m) const override {
        DelayParams q;
        std::memcpy(&q, raw, sizeof q);
        if (!(q.dry >= 0 && q.dry <= 1)) return fail(m, "fDryVol {} out of range", q.dry);
        if (!(q.mod_rate >= 1 && q.mod_rate <= 20)) return fail(m, "fModRate {} out of range", q.mod_rate);
        for (u32 i = 0; i < 4; ++i) {
            const DelayTap& t = q.taps[i];
            if (!(t.msecs >= 0 && t.msecs <= 1000)) return fail(m, "taps[{}].fDelayMillisecs {} out of range", i, t.msecs);
            if (!(t.volume >= -1 && t.volume <= 1)) return fail(m, "taps[{}].fVolume {} out of range", i, t.volume);
            if (!(t.feedback >= -0.95f && t.feedback <= 0.95f)) return fail(m, "taps[{}].fFeedback {} out of range", i, t.feedback);
            if (t.filter > 3) return fail(m, "taps[{}].eFilterMode {} out of range", i, t.filter);
            if (!(t.width_msecs >= 0 && t.width_msecs <= 500)) return fail(m, "taps[{}].fModWidthMillisecs {} out of range", i, t.width_msecs);
        }
        return true;
    }
    void reset(const Context& c) override {
        const u32 n = c.sample_rate * 3 / 2 + 4;  // (1000 ms and 500 ms of modulation)
        for (auto& l : line) l.assign(n, 0.0f);
        write = 0;
        lfo = 0;
        std::memset(tap_state, 0, sizeof tap_state);
    }
    void process(const Context& c, Packet& io) override {
        if (line[0].empty()) reset(c);
        const u32 n = static_cast<u32>(line[0].size());
        for (u32 i = 0; i < c.granularity; ++i) {
            for (u32 ch = 0; ch < c.channels; ++ch) {
                const DelayParams& q = *param<DelayParams>(nx::min(ch, param_channels() - 1));
                const f32 x = io.s[ch][i];
                f32 wet = 0, feedback = 0;
                for (u32 k = 0; k < 4; ++k) {
                    const DelayTap& t = q.taps[k];
                    if (t.volume == 0 && t.feedback == 0) continue;
                    const f64 mod = 0.5 * (1 + std::sin(2 * kPi * lfo + t.phase_deg * kPi / 180));
                    const f64 d = (t.msecs + t.width_msecs * mod) * c.sample_rate / 1000.0;
                    const f64 pos = static_cast<f64>(write) - nx::max(d, 1.0) + n;
                    const u32 i0 = static_cast<u32>(pos) % n, i1 = (i0 + 1) % n;
                    const f32 f = static_cast<f32>(pos - std::floor(pos));
                    f32 y = line[ch][i0] + (line[ch][i1] - line[ch][i0]) * f;
                    if (t.filter == 1 || t.filter == 2) {
                        const f32 a = static_cast<f32>(std::exp(-2 * kPi * t.cutoff / c.sample_rate));
                        tap_state[ch][k] = y + (tap_state[ch][k] - y) * a;
                        y = t.filter == 1 ? tap_state[ch][k] : y - tap_state[ch][k];
                    }
                    wet += y * t.volume;
                    feedback += y * t.feedback;
                }
                line[ch][write] = x + feedback;
                io.s[ch][i] = x * q.dry + wet;
            }
            write = (write + 1) % n;
            lfo += param<DelayParams>()->mod_rate / c.sample_rate;
            lfo -= std::floor(lfo);
        }
    }
};

// ---- pitch shift (two crossfading read heads over a delay line)

struct PitchShift : Module {
    using Module::Module;
    static constexpr u32 kWindow = 2048;
    f32 line[kMaxChannels][kWindow * 2] = {};
    u32 write = 0;
    f64 offset = 0;

    void defaults() override {
        for (u32 ch = 0; ch < param_channels(); ++ch)
            *param<PitchShiftParams>(ch) = PitchShiftParams{{kPitchShiftIf[0].id, sizeof(PitchShiftParams)}, 0.0f};
        iface_ = kPitchShiftIf[0].id;
    }
    bool check(const u8* raw, nx::string* m) const override {
        PitchShiftParams q;
        std::memcpy(&q, raw, sizeof q);
        if (!(q.cents >= -1200 && q.cents <= 1200)) return fail(m, "fPitchOffsetInCents {} out of range", q.cents);
        return true;
    }
    void reset(const Context&) override {
        std::memset(line, 0, sizeof line);
        write = 0;
        offset = 0;
    }
    void process(const Context& c, Packet& io) override {
        const f64 ratio = std::pow(2.0, param<PitchShiftParams>()->cents / 1200.0);
        if (ratio == 1.0) return;
        constexpr u32 n = kWindow * 2;
        for (u32 i = 0; i < c.granularity; ++i) {
            for (u32 ch = 0; ch < c.channels; ++ch) line[ch][write] = io.s[ch][i];
            for (u32 ch = 0; ch < c.channels; ++ch) {
                f32 y = 0;
                for (u32 h = 0; h < 2; ++h) {
                    f64 d = std::fmod(offset + h * kWindow * 0.5, static_cast<f64>(kWindow));
                    const f64 pos = static_cast<f64>(write) - d - 1 + n;
                    const u32 i0 = static_cast<u32>(pos) % n, i1 = (i0 + 1) % n;
                    const f32 f = static_cast<f32>(pos - std::floor(pos));
                    const f32 s = line[ch][i0] + (line[ch][i1] - line[ch][i0]) * f;
                    const f32 w = static_cast<f32>(std::sin(kPi * d / kWindow));
                    y += s * w * w;
                }
                io.s[ch][i] = y;
            }
            write = (write + 1) % n;
            offset += 1.0 - ratio;
            offset = std::fmod(offset + kWindow, static_cast<f64>(kWindow));
        }
    }
};

// ---- reverb (I3DL2: filtered input, pre-delay, early reflections, a feedback delay network for the late part)

struct ReverbPreset {
    f32 room, room_hf, decay_time, decay_hf_ratio, reflections, reflections_delay, reverb, reverb_delay, diffusion,
        density;
};
// The I3DL2 environment presets (Interactive 3D Audio Rendering Guidelines, Level 2.0), in the order of
// SCE_NGS_REVERB_PRESET_*.
constexpr ReverbPreset kReverbPresets[] = {
    {-1000, -100, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100, 100},     // default
    {-1000, -100, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100, 100},     // generic
    {-1000, -6000, 0.17f, 0.10f, -1204, 0.001f, 207, 0.002f, 100, 100},    // padded cell
    {-1000, -454, 0.40f, 0.83f, -1646, 0.002f, 53, 0.003f, 100, 100},      // room
    {-1000, -1200, 1.49f, 0.54f, -370, 0.007f, 1030, 0.011f, 100, 60},     // bathroom
    {-1000, -6000, 0.50f, 0.10f, -1376, 0.003f, -1104, 0.004f, 100, 100},  // living room
    {-1000, -300, 2.31f, 0.64f, -711, 0.012f, 83, 0.017f, 100, 100},       // stone room
    {-1000, -476, 4.32f, 0.59f, -789, 0.020f, -289, 0.030f, 100, 100},     // auditorium
    {-1000, -500, 3.92f, 0.70f, -1230, 0.020f, -2, 0.029f, 100, 100},      // concert hall
    {-1000, 0, 2.91f, 1.30f, -602, 0.015f, -302, 0.022f, 100, 100},        // cave
    {-1000, -698, 7.24f, 0.33f, -1166, 0.020f, 16, 0.030f, 100, 100},      // arena
    {-1000, -1000, 10.05f, 0.23f, -602, 0.020f, 198, 0.030f, 100, 100},    // hangar
    {-1000, -4000, 0.30f, 0.10f, -1831, 0.002f, -1630, 0.030f, 100, 100},  // carpeted hallway
    {-1000, -300, 1.49f, 0.59f, -1219, 0.007f, 441, 0.011f, 100, 100},     // hallway
    {-1000, -237, 2.70f, 0.79f, -1214, 0.013f, 395, 0.020f, 100, 100},     // stone corridor
    {-1000, -270, 1.49f, 0.86f, -1204, 0.007f, -4, 0.011f, 100, 100},      // alley
    {-1000, -3300, 1.49f, 0.54f, -2560, 0.162f, -613, 0.088f, 79, 100},    // forest
    {-1000, -800, 1.49f, 0.67f, -2273, 0.007f, -2217, 0.011f, 50, 100},    // city
    {-1000, -2500, 1.49f, 0.21f, -2780, 0.300f, -2014, 0.100f, 27, 100},   // mountains
    {-1000, -1000, 1.49f, 0.83f, -10000, 0.061f, 500, 0.025f, 100, 100},   // quarry
    {-1000, -2000, 1.49f, 0.50f, -2466, 0.179f, -2514, 0.100f, 21, 100},   // plain
    {-1000, 0, 1.65f, 1.50f, -1363, 0.008f, -1153, 0.012f, 100, 100},      // parking lot
    {-1000, -1000, 2.81f, 0.14f, 429, 0.014f, 648, 0.021f, 80, 60},        // sewer pipe
    {-1000, -4000, 1.49f, 0.10f, -449, 0.007f, 1700, 0.011f, 100, 100},    // underwater
    {-1000, -600, 1.10f, 0.83f, -400, 0.005f, 500, 0.010f, 100, 100},      // small room
    {-1000, -600, 1.30f, 0.83f, -1000, 0.010f, -200, 0.020f, 100, 100},    // medium room
    {-1000, -600, 1.50f, 0.83f, -1600, 0.020f, -1000, 0.040f, 100, 100},   // large room
    {-1000, -600, 1.80f, 0.70f, -1300, 0.015f, -800, 0.030f, 100, 100},    // medium hall
    {-1000, -600, 1.80f, 0.70f, -2000, 0.030f, -1400, 0.060f, 100, 100},   // large hall
    {-1000, -200, 1.30f, 0.90f, 0, 0.002f, 0, 0.010f, 100, 75},            // plate
};

void reverb_params(ReverbParams& q, const ReverbPreset& p, u32 id) {
    q = ReverbParams{};
    q.desc = Desc{id, sizeof(ReverbParams)};
    q.room = p.room, q.room_hf = p.room_hf, q.decay_time = p.decay_time, q.decay_hf_ratio = p.decay_hf_ratio;
    q.reflections = p.reflections, q.reflections_delay = p.reflections_delay, q.reverb = p.reverb;
    q.reverb_delay = p.reverb_delay, q.diffusion = p.diffusion, q.density = p.density;
    q.hf_reference = 5000.0f;
    q.pattern[0] = 0, q.pattern[1] = 1;
    q.early_scalar = 100.0f;
    q.lf_reference = 250.0f;
    q.room_lf = 0.0f;
    q.dry_mb = -10000.0f;
}

struct Reverb : Module {
    using Module::Module;
    static constexpr u32 kLines = 4;
    struct Channel {
        nx::vector<f32> pre;  // pre-delay and early reflections
        u32 pre_write = 0;
        nx::vector<f32> late[kLines];
        u32 late_write[kLines] = {};
        f32 damp[kLines] = {};
        f32 hf = 0, lf = 0;
    } ch_[kMaxChannels];

    void defaults() override {
        for (u32 ch = 0; ch < param_channels(); ++ch) reverb_params(*param<ReverbParams>(ch), kReverbPresets[0], kReverbIf[1].id);
        iface_ = kReverbIf[1].id;
    }
    bool check(const u8* raw, nx::string* m) const override {
        ReverbParams q;
        std::memcpy(&q, raw, sizeof q);
        struct Range {
            f32 v, lo, hi;
            const char* name;
        } r[] = {{q.room, -10000, 0, "fRoom"},
                 {q.room_hf, -10000, 0, "fRoomHF"},
                 {q.decay_time, 0.1f, 20, "fDecayTime"},
                 {q.decay_hf_ratio, 0.1f, 2, "fDecayHFRatio"},
                 {q.reflections, -10000, 1000, "fReflections"},
                 {q.reflections_delay, 0, 0.3f, "fReflectionsDelay"},
                 {q.reverb, -10000, 2000, "fReverb"},
                 {q.reverb_delay, 0, 0.1f, "fReverbDelay"},
                 {q.diffusion, 0, 100, "fDiffusion"},
                 {q.density, 0, 100, "fDensity"},
                 {q.hf_reference, 20, 20000, "fHFReference"},
                 {q.early_scalar, 0, 100, "fEarlyReflectionScalar"},
                 {q.lf_reference, 20, 20000, "fLFReference"},
                 {q.room_lf, -10000, 0, "fRoomLF"},
                 {q.dry_mb, -10000, 0, "fDryMB"}};
        for (const Range& x : r)
            if (!(x.v >= x.lo && x.v <= x.hi)) return fail(m, "{} {} out of range", x.name, x.v);
        for (i32 p : q.pattern)
            if (p < 0 || p > 5) return fail(m, "eEarlyReflectionPattern {} out of range", p);
        return true;
    }
    void reset(const Context& c) override {
        static constexpr f32 kLateMs[kLines] = {29.7f, 37.1f, 41.1f, 43.7f};
        for (auto& ch : ch_) {
            ch.pre.assign(c.sample_rate * 2 / 5 + 4, 0.0f);  // (0.3 s of pre-delay and 0.1 s of reflections)
            ch.pre_write = 0;
            for (u32 k = 0; k < kLines; ++k) {
                ch.late[k].assign(static_cast<u32>(kLateMs[k] * 2 * c.sample_rate / 1000) + 4, 0.0f);
                ch.late_write[k] = 0;
                ch.damp[k] = 0;
            }
            ch.hf = ch.lf = 0;
        }
    }
    void process(const Context& c, Packet& io) override {
        if (ch_[0].pre.empty()) reset(c);
        static constexpr f32 kEarly[6][6] = {{0.10f, 0.23f, 0.37f, 0.52f, 0.71f, 0.93f}, {0.13f, 0.27f, 0.41f, 0.58f, 0.76f, 0.97f},
                                             {0.07f, 0.19f, 0.34f, 0.49f, 0.68f, 0.89f}, {0.09f, 0.21f, 0.39f, 0.55f, 0.73f, 0.95f},
                                             {0.12f, 0.31f, 0.45f, 0.61f, 0.79f, 0.91f}, {0.15f, 0.29f, 0.43f, 0.63f, 0.81f, 0.99f}};
        static constexpr f32 kLateMs[kLines] = {29.7f, 37.1f, 41.1f, 43.7f};
        for (u32 chn = 0; chn < c.channels; ++chn) {
            const ReverbParams& q = *param<ReverbParams>(nx::min(chn, param_channels() - 1));
            Channel& s = ch_[chn];
            const f32 room = millibels(q.room);
            const f32 hf_gain = millibels(q.room_hf), lf_gain = millibels(q.room_lf);
            const f32 hf_a = static_cast<f32>(std::exp(-2 * kPi * q.hf_reference / c.sample_rate));
            const f32 lf_a = static_cast<f32>(std::exp(-2 * kPi * q.lf_reference / c.sample_rate));
            const f32 early_gain = millibels(q.reflections) * room / 6.0f;
            const f32 late_gain = millibels(q.reverb) * room * 0.5f;
            const f32 dry = millibels(q.dry_mb);
            const u32 pre_n = static_cast<u32>(s.pre.size());
            const f32 pre_delay = q.reflections_delay * c.sample_rate;
            const f32 span = q.reverb_delay * c.sample_rate * q.early_scalar / 100.0f;
            const u32 pattern = static_cast<u32>(q.pattern[nx::min<u32>(chn, 1)]) % 6;
            const f32 density = 0.5f + q.density / 200.0f;
            const f32 diffusion = q.diffusion / 100.0f;
            for (u32 i = 0; i < c.granularity; ++i) {
                const f32 x = io.s[chn][i];
                // (room HF and LF: one-pole splits, the bands scaled)
                s.hf = x + (s.hf - x) * hf_a;
                s.lf = x + (s.lf - x) * lf_a;
                const f32 filtered = s.lf * lf_gain + (s.hf - s.lf) + (x - s.hf) * hf_gain;
                s.pre[s.pre_write] = filtered;
                f32 early = 0;
                for (f32 tap : kEarly[pattern]) {
                    const u32 d = static_cast<u32>(pre_delay + tap * span) + 1;
                    early += s.pre[(s.pre_write + pre_n - nx::min(d, pre_n - 1)) % pre_n];
                }
                const u32 late_in_d = static_cast<u32>(pre_delay + span) + 1;
                const f32 late_in = s.pre[(s.pre_write + pre_n - nx::min(late_in_d, pre_n - 1)) % pre_n];
                s.pre_write = (s.pre_write + 1) % pre_n;
                // (four lines, a Householder mix between them)
                f32 outs[kLines], sum = 0;
                for (u32 k = 0; k < kLines; ++k) {
                    const u32 n = static_cast<u32>(s.late[k].size());
                    const u32 d = nx::min<u32>(static_cast<u32>(kLateMs[k] * density * c.sample_rate / 1000), n - 1);
                    outs[k] = s.late[k][(s.late_write[k] + n - d) % n];
                    sum += outs[k];
                }
                f32 late = 0;
                for (u32 k = 0; k < kLines; ++k) {
                    const u32 n = static_cast<u32>(s.late[k].size());
                    const f32 d_s = kLateMs[k] * density / 1000.0f;
                    const f32 g = std::pow(10.0f, -3.0f * d_s / q.decay_time);
                    const f32 g_hf = std::pow(10.0f, -3.0f * d_s / (q.decay_time * q.decay_hf_ratio));
                    const f32 damping = clampf(1.0f - g_hf / nx::max(g, 1e-6f), 0.0f, 0.95f);
                    const f32 mixed = outs[k] - diffusion * 0.5f * sum;
                    s.damp[k] = mixed + (s.damp[k] - mixed) * damping;
                    s.late[k][s.late_write[k]] = late_in + s.damp[k] * g;
                    s.late_write[k] = (s.late_write[k] + 1) % n;
                    late += outs[k];
                }
                io.s[chn][i] = x * dry + early * early_gain + late * late_gain;
            }
        }
    }
};

}  // namespace

// ---- Module

const TypeInfo* type_info(u32 type) {
    for (const TypeInfo& t : kTypes)
        if (t.type == type) return &t;
    return nullptr;
}

u32 max_params_size(u32 type) {
    const TypeInfo* t = type_info(type);
    u32 size = 0;
    for (u32 i = 0; t && i < t->interface_count; ++i) size = nx::max(size, t->interfaces[i].size);
    return size;
}

Module::Module(u32 type, u32 channels) : type_(type), info_(type_info(type)), channels_(channels) {
    params_.assign(max_params_size(type) * (info_->per_channel ? channels : 1), 0);
    if (info_->interface_count) iface_ = info_->interfaces[0].id;
}

u32 Module::interface_size() const {
    for (u32 i = 0; i < info_->interface_count; ++i)
        if (info_->interfaces[i].id == iface_) return info_->interfaces[i].size;
    return 0;
}

void Module::write_header(u32 id, u32 size) {
    iface_ = id;
    for (u32 ch = 0; ch < param_channels(); ++ch) {
        u8* p = params_.data() + ch * size;
        std::memset(p, 0, size);
        const Desc d{id, size};
        std::memcpy(p, &d, sizeof d);
    }
}

void Module::load(const u8* bytes, u32 interface_id) {
    iface_ = interface_id;
    std::memcpy(params_.data(), bytes, params_bytes());
}

bool Module::select_interface(u32 id) {
    for (u32 i = 0; i < info_->interface_count; ++i) {
        if (info_->interfaces[i].id != id) continue;
        if (iface_ == id) return true;
        // (another interface of the same layout keeps the values; another layout starts from neutral values)
        const u32 old_size = interface_size();
        if (old_size == info_->interfaces[i].size) {
            iface_ = id;
            for (u32 ch = 0; ch < param_channels(); ++ch) std::memcpy(params_.data() + ch * old_size, &id, 4);
            return true;
        }
        write_header(id, info_->interfaces[i].size);
        if (type_ == kFilter && id == kFilterIf[1].id)
            for (u32 ch = 0; ch < param_channels(); ++ch) param<FilterCoeffParams>(ch)->c.b0 = 1.0f;
        if (type_ == kParamEq && id == kEqIf[1].id)
            for (u32 ch = 0; ch < param_channels(); ++ch)
                for (auto& c : param<EqCoeffParams>(ch)->c) c.b0 = 1.0f;
        return true;
    }
    return false;
}

void Module::defaults() {
    if (info_->interface_count) write_header(info_->interfaces[0].id, info_->interfaces[0].size);
}

bool Module::check(const u8*, nx::string*) const { return true; }

bool Module::state(void*, u32) const { return false; }

u32 preset_count(u32 type) {
    if (type == kReverb) return static_cast<u32>(std::size(kReverbPresets));
    if (type == kDelay) return 6;
    return type_info(type) && type_info(type)->interface_count ? 1 : 0;
}

bool preset(u32 type, u32 index, u8* out, u32 size) {
    if (index >= preset_count(type)) return false;
    const u32 need = type_info(type)->interfaces[0].size;
    if (size < need) return false;
    if (type == kReverb) {
        ReverbParams q;
        reverb_params(q, kReverbPresets[index], kReverbIf[1].id);
        std::memcpy(out, &q, sizeof q);
        return true;
    }
    std::unique_ptr<Module> m = make_module(type, 1);
    m->defaults();
    std::memcpy(out, m->params(), need);
    if (type == kDelay && index > 0) {
        // (chorus and flanger: short modulated taps)
        DelayParams q;
        std::memcpy(&q, out, sizeof q);
        const bool flanger = index >= 4;
        const bool right = index == 3 || index == 5;
        q.dry = 0.7f;
        q.mod_rate = flanger ? 1.0f : 1.5f;
        q.taps[0] = DelayTap{flanger ? 1.0f : 15.0f, 0.7f, flanger ? 0.6f : 0.0f, 0, 20000.0f, right ? 90.0f : 0.0f,
                             flanger ? 3.0f : 6.0f};
        std::memcpy(out, &q, sizeof q);
    }
    return true;
}

std::unique_ptr<Module> make_module(u32 type, u32 channels) {
    std::unique_ptr<Module> m;
    switch (type) {
        case kPlayer: m = std::make_unique<Player>(type, channels); break;
        case kAt9Player: m = std::make_unique<At9Player>(type, channels); break;
        case kGenerator: m = std::make_unique<Generator>(type, channels); break;
        case kMixer: m = std::make_unique<Mixer>(type, channels); break;
        case kPauser: m = std::make_unique<Pauser>(type, channels); break;
        case kEnvelope: m = std::make_unique<Envelope>(type, channels); break;
        case kFilter: m = std::make_unique<Filter>(type, channels); break;
        case kParamEq: m = std::make_unique<ParamEq>(type, channels); break;
        case kDistortion: m = std::make_unique<Distortion>(type, channels); break;
        case kCompressor: m = std::make_unique<Compressor>(type, channels); break;
        case kDelay: m = std::make_unique<Delay>(type, channels); break;
        case kPitchShift: m = std::make_unique<PitchShift>(type, channels); break;
        case kReverb: m = std::make_unique<Reverb>(type, channels); break;
        default: m = std::make_unique<Module>(type, channels); break;  // (input mixer, output)
    }
    m->defaults();
    return m;
}

void render_player(Module& m, const Context& c, Packet& out, bool* ended) {
    static_cast<Streamer&>(m).render(c, out, ended);
}

void render_generator(Module& m, const Context& c, Packet& out) { static_cast<Generator&>(m).render(c, out); }

void mix(Module& m, const Context& c, const Packet& a, const Packet& b, Packet& out) {
    const MixerParams& q = *reinterpret_cast<const MixerParams*>(m.params());
    for (u32 ch = 0; ch < c.channels; ++ch)
        for (u32 i = 0; i < c.granularity; ++i) out.s[ch][i] = a.s[ch][i] * q.gain[0] + b.s[ch][i] * q.gain[1];
}

void compress(Module& m, const Context& c, Packet& io, const Packet* side_chain) {
    static_cast<Compressor&>(m).run(c, io, side_chain);
}

EnvelopeEnd envelope(Module& m, const Context& c, Packet& io) { return static_cast<Envelope&>(m).run(c, io); }

void envelope_key_off(Module& m) { static_cast<Envelope&>(m).releasing = true; }

u32 pause_lead_in(const Module& m) {
    if (m.type() == kPauser) return reinterpret_cast<const PauserParams*>(m.params())->lead_in;
    if (m.type() == kMixer) return reinterpret_cast<const MixerParams*>(m.params())->lead_in;
    return 0;
}

u32 pause_lead_out(const Module& m) {
    if (m.type() == kPauser) return reinterpret_cast<const PauserParams*>(m.params())->lead_out;
    if (m.type() == kMixer) return reinterpret_cast<const MixerParams*>(m.params())->lead_out;
    return 0;
}

}  // namespace psvrecore::runtime::ngs
