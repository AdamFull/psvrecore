// NGS (SceNgs): voices played through patches into the master buss, sample for sample; the player's counters,
// buffers, loops, lead-in and limit; callbacks; voice states; envelopes; VAG decoding; a filter.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "nxtest.h"
#include "runtime_support.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

constexpr u32 kSceNgs = 0xB01598D9;
constexpr u32 kSysSize = 0x6CE8B36F, kSysInit = 0xED14CF4A, kUpdate = 0x684F080C, kSysRelease = 0x4A25BEBC,
              kRackSize = 0x477318C0, kRackInit = 0x0A92E4EC, kVoiceHandle = 0xFE1A98E9,
              kPlay = 0xFA0A0F34, kKeyOff = 0xBB13373D, kKill = 0x0E291AAD, kPause = 0xD7786E99, kResume = 0x54CFB981,
              kLock = 0xAB6BEF8F, kUnlock = 0x3D46D8A7, kParamsBlock = 0xFB8174B1, kBypass = 0x9AB87E71,
              kModuleCallback = 0x24E909A8, kFinishedCallback = 0x17A6F564, kStateData = 0xC9B8C0B4, kInfo = 0x5551410D,
              kOutOfRange = 0x4CBE08F3, kPatchCreate = 0xD668B49C, kPatchInfo = 0x98703DBC, kVolumesMatrix = 0xA0F5402D,
              kDefMaster = 0x79A121D1, kDefMixer = 0xE0AC8776, kDefTemplate1 = 0xE9B572B7;
constexpr i32 kResourceLocked = static_cast<i32>(0x804A0008u), kParamOutOfRange = static_cast<i32>(0x804A0009u),
              kParamTypeMismatch = static_cast<i32>(0x804A000Fu), kDependency = static_cast<i32>(0x804A0005u);
constexpr u32 kPlayerParamsId = 0x01015CE6, kEnvelopeParamsId = 0x01015CE3, kEqParamsId = 0x01015CEC,
              kGeneratorParamsId = 0x01015CE8;
constexpr u32 kG = 256;

u32 u(i32 v) { return static_cast<u32>(v); }

u32 aligned(u32 size) { return (scratch(size + 16) + 15) & ~15u; }

struct PlayerBuffer {
    u32 buffer;
    i32 bytes;
    i16 loop_count, next;
};
struct PlayerParams {
    u32 id, size;
    PlayerBuffer buffs[4];
    f32 frequency, scalar;
    i32 lead_in, limit, start_byte;
    i8 channels;
    i8 map[2];
    i8 type, reserved, start_buffer;
    i8 pad[2];
};
static_assert(sizeof(PlayerParams) == 84);

PlayerParams player(i8 channels) {
    PlayerParams p{};
    p.id = kPlayerParamsId;
    p.size = sizeof p;
    for (auto& b : p.buffs) b = PlayerBuffer{0, 0, 0, -1};
    p.frequency = 48000.0f;
    p.scalar = 1.0f;
    p.channels = channels;
    p.map[0] = 0;
    p.map[1] = channels == 2 ? 1 : 0;
    return p;
}

// A block of parameters (module, all channels) for sceNgsVoiceSetParamsBlock.
template <typename T>
i32 set_params(PsvContext* ctx, u32 voice, u32 module, const T& params) {
    const u32 at = scratch(8 + sizeof(T));
    const i32 header[2] = {static_cast<i32>(module), -1};
    std::memcpy(current_runtime()->memory().host(at), header, 8);
    std::memcpy(current_runtime()->memory().host(at + 8), &params, sizeof(T));
    const u32 errors = scratch(4);
    return call32(ctx, kSceNgs, kParamsBlock, {voice, at, static_cast<u32>(8 + sizeof(T)), errors});
}

struct Ngs {
    PsvContext* ctx;
    u32 system = 0;

    explicit Ngs(PsvContext* c) : ctx(c) {
        const u32 params = scratch(20);
        const i32 p[5] = {8, 16, static_cast<i32>(kG), 48000, 0};
        std::memcpy(current_runtime()->memory().host(params), p, sizeof p);
        const u32 size = scratch(4);
        REQUIRE(call32(ctx, kSceNgs, kSysSize, {params, size}) == 0);
        const u32 mem = aligned(psv_ld32(size));
        const u32 handle = scratch(4);
        REQUIRE(call32(ctx, kSceNgs, kSysInit, {mem, psv_ld32(size), params, handle}) == 0);
        system = psv_ld32(handle);
    }
    ~Ngs() { call32(ctx, kSceNgs, kSysRelease, {system}); }

    // A rack of a voice definition; returns its voices' handles.
    nx::vector<u32> rack(u32 definition_nid, i32 voices, i32 channels, i32 max_in = 4, i32 per_out = 2) {
        const u32 def = static_cast<u32>(call32(ctx, kSceNgs, definition_nid, {}));
        const u32 desc = scratch(24);
        const i32 d[6] = {static_cast<i32>(def), voices, channels, max_in, per_out, 0};
        std::memcpy(current_runtime()->memory().host(desc), d, sizeof d);
        const u32 size = scratch(4);
        REQUIRE(call32(ctx, kSceNgs, kRackSize, {system, desc, size}) == 0);
        const u32 buffer = scratch(8);
        psv_st32(buffer, aligned(psv_ld32(size)));
        psv_st32(buffer + 4, psv_ld32(size));
        const u32 handle = scratch(4);
        REQUIRE(call32(ctx, kSceNgs, kRackInit, {system, buffer, desc, handle}) == 0);
        nx::vector<u32> out;
        for (i32 i = 0; i < voices; ++i) {
            const u32 v = scratch(4);
            REQUIRE(call32(ctx, kSceNgs, kVoiceHandle, {psv_ld32(handle), u(i), v}) == 0);
            out.push_back(psv_ld32(v));
        }
        return out;
    }
    i32 patch(u32 from, u32 to, u32* handle = nullptr) {
        const u32 setup = scratch(20);
        const i32 s[5] = {static_cast<i32>(from), 0, -1, static_cast<i32>(to), 0};
        std::memcpy(current_runtime()->memory().host(setup), s, sizeof s);
        const u32 h = scratch(4);
        const i32 r = call32(ctx, kSceNgs, kPatchCreate, {setup, h});
        if (handle) *handle = psv_ld32(h);
        return r;
    }
    void update() { REQUIRE(call32(ctx, kSceNgs, kUpdate, {system}) == 0); }
    // The master's packet (interleaved stereo).
    nx::vector<i16> output(u32 master) {
        const u32 at = scratch(kG * 4);
        REQUIRE(call32(ctx, kSceNgs, kStateData, {master, 1, at, kG * 4}) == 0);
        nx::vector<i16> out(kG * 2);
        std::memcpy(out.data(), current_runtime()->memory().host(at), kG * 4);
        return out;
    }
    u32 state(u32 voice) {
        const u32 info = scratch(24);
        REQUIRE(call32(ctx, kSceNgs, kInfo, {voice, info}) == 0);
        return psv_ld32(info);
    }
};

u32 pcm(const nx::vector<i16>& samples) {
    const u32 at = aligned(static_cast<u32>(samples.size() * 2));
    std::memcpy(current_runtime()->memory().host(at), samples.data(), samples.size() * 2);
    return at;
}

struct Call {
    u32 voice, module;
    i32 data, data2;
    u32 ptr, user;
};
nx::vector<Call> calls;

void record(PsvContext* ctx, uint32_t) {
    const u32 info = ctx->r[0];
    calls.push_back(Call{psv_ld32(info), psv_ld32(info + 8), static_cast<i32>(psv_ld32(info + 12)),
                         static_cast<i32>(psv_ld32(info + 16)), psv_ld32(info + 20), psv_ld32(info + 24)});
}

}  // namespace

TEST_CASE("ngs: PCM played through the master buss comes out sample for sample; the counters count exactly") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    GuestCode code;
    const u32 callback = code.add(record);
    code.install();
    calls.clear();
    Ngs ngs(ctx);
    // (the master's rack made first: the patch decides the order, not the creation)
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 2)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    nx::vector<i16> data(2000);
    for (u32 i = 0; i < 1000; ++i) {
        data[2 * i] = static_cast<i16>(static_cast<i32>(i * 37 % 20000) - 10000);
        data[2 * i + 1] = static_cast<i16>(-data[2 * i]);
    }
    PlayerParams p = player(2);
    p.buffs[0] = PlayerBuffer{pcm(data), 4000, 0, -1};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kModuleCallback, {voice, 0, callback, 0x111}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kFinishedCallback, {voice, callback, 0x222}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    CHECK(ngs.state(voice) == 17);  // (active at once, pending until the next update)

    u32 frame = 0;
    bool exact = true;
    for (u32 update = 0; update < 4; ++update) {
        ngs.update();
        const nx::vector<i16> out = ngs.output(master);
        for (u32 i = 0; i < kG; ++i, ++frame) {
            const i16 l = frame < 1000 ? data[2 * frame] : 0, r = frame < 1000 ? data[2 * frame + 1] : 0;
            exact &= out[2 * i] == l && out[2 * i + 1] == r;
        }
        if (update == 0) {
            CHECK(ngs.state(voice) == 1);
            const u32 st = scratch(24);
            REQUIRE(call32(ctx, kSceNgs, kStateData, {voice, 0, st, 24}) == 0);
            CHECK(psv_ld32(st) == 1024);       // position in the buffer
            CHECK(psv_ld32(st + 4) == 0);      // buffer
            CHECK(psv_ld32(st + 8) == 256);    // samples since key on
            CHECK(psv_ld32(st + 12) == 1024);  // bytes since key on
            CHECK(calls.empty());
        }
    }
    CHECK(exact);
    // It ended in the fourth update: the player said so, then the voice finished.
    CHECK(ngs.state(voice) == 0);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].module == 0);
    CHECK(calls[0].data == 0);  // SCE_NGS_PLAYER_END_OF_DATA
    CHECK(calls[0].user == 0x111);
    CHECK(calls[1].voice == voice);
    CHECK(calls[1].user == 0x222);
    const u32 st = scratch(24);
    REQUIRE(call32(ctx, kSceNgs, kStateData, {voice, 0, st, 24}) == 0);
    CHECK(psv_ld32(st + 8) == 1000);
    CHECK(psv_ld32(st + 12) == 4000);
}

TEST_CASE("ngs: streaming swaps buffers with a callback; loops, lead-in and the sample limit are exact") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    GuestCode code;
    const u32 callback = code.add(record);
    code.install();
    calls.clear();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 1)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    nx::vector<i16> a(256), b(256);
    for (u32 i = 0; i < 256; ++i) a[i] = static_cast<i16>(1000 + i), b[i] = static_cast<i16>(-1000 - static_cast<i32>(i));
    const u32 ba = pcm(a), bb = pcm(b);
    PlayerParams p = player(1);
    p.buffs[0] = PlayerBuffer{ba, 512, 0, 1};
    p.buffs[1] = PlayerBuffer{bb, 512, 0, 0};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kModuleCallback, {voice, 0, callback, 0}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    for (u32 k = 0; k < 3; ++k) {
        ngs.update();
        const nx::vector<i16> out = ngs.output(master);
        const nx::vector<i16>& want = k == 1 ? b : a;
        bool same = true;
        for (u32 i = 0; i < kG; ++i) same &= out[2 * i] == want[i] && out[2 * i + 1] == want[i];
        CHECK(same);
        REQUIRE(calls.size() == k + 1);
        CHECK(calls[k].data == 1);                                  // SCE_NGS_PLAYER_SWAPPED_BUFFER
        CHECK(calls[k].data2 == static_cast<i32>(k == 1 ? 1 : 0));  // (the buffer finished)
        CHECK(calls[k].ptr == (k == 1 ? ba : bb));                  // (the buffer now playing)
    }

    // 100 samples of lead-in, buffer 0 looped without end, 300 samples at most.
    REQUIRE(call32(ctx, kSceNgs, kKill, {voice}) == 0);
    p.buffs[0] = PlayerBuffer{ba, 512, -1, -1};
    p.lead_in = 100;
    p.limit = 300;
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    calls.clear();
    nx::vector<i16> got;
    for (u32 k = 0; k < 2; ++k) {
        ngs.update();
        const nx::vector<i16> out = ngs.output(master);
        for (u32 i = 0; i < kG; ++i) got.push_back(out[2 * i]);
    }
    bool same = true;
    for (u32 i = 0; i < 512; ++i) {
        const i16 want = i < 100 ? 0 : i < 400 ? a[(i - 100) % 256] : 0;
        same &= got[i] == want;
    }
    CHECK(same);
    REQUIRE(calls.size() == 1);  // (one per update at most: the first had none, the second ended)
    CHECK(calls[0].data == 0);
    CHECK(ngs.state(voice) == 0);

    // Buffer 0 played three times (two loops): 300 samples, then the end.
    p.buffs[0] = PlayerBuffer{ba, 200, 2, -1};
    p.lead_in = 0;
    p.limit = 0;
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    calls.clear();
    got.clear();
    for (u32 k = 0; k < 2; ++k) {
        ngs.update();
        const nx::vector<i16> out = ngs.output(master);
        for (u32 i = 0; i < kG; ++i) got.push_back(out[2 * i]);
    }
    same = true;
    for (u32 i = 0; i < 512; ++i) same &= got[i] == (i < 300 ? a[i % 100] : 0);
    CHECK(same);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].data == 2);  // SCE_NGS_PLAYER_LOOPED_BUFFER
    CHECK(calls[0].data2 == 2);
    CHECK(calls[1].data == 0);
}

TEST_CASE("ngs: patches mix with their volumes through a mixer buss; pausing, killing; parameter locks and checks") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 buss = ngs.rack(kDefMixer, 1, 1)[0];
    const nx::vector<u32> voices = ngs.rack(kDefTemplate1, 2, 1);
    nx::vector<i16> a(4096), b(4096);
    for (u32 i = 0; i < 4096; ++i) a[i] = static_cast<i16>(4 * (i % 2000)), b[i] = static_cast<i16>(-8 * (i % 1000));
    const u32 ba = pcm(a), bb = pcm(b);
    u32 pa, pb;
    REQUIRE(ngs.patch(voices[0], buss, &pa) == 0);
    REQUIRE(ngs.patch(voices[1], buss, &pb) == 0);
    REQUIRE(ngs.patch(buss, master) == 0);
    CHECK(ngs.patch(master, voices[0]) != 0);    // (the master has no output)
    CHECK(ngs.patch(buss, buss) == kDependency);  // (a voice of the same rack)
    // (0.5 of the first, 0.25 of the second)
    const u32 m = scratch(16);
    const f32 half[4] = {0.5f, 0, 0, 0}, quarter[4] = {0.25f, 0, 0, 0};
    std::memcpy(t.rt.memory().host(m), half, 16);
    REQUIRE(call32(ctx, kSceNgs, kVolumesMatrix, {pa, m}) == 0);
    const u32 m2 = scratch(16);
    std::memcpy(t.rt.memory().host(m2), quarter, 16);
    REQUIRE(call32(ctx, kSceNgs, kVolumesMatrix, {pb, m2}) == 0);
    const u32 route = scratch(24);
    REQUIRE(call32(ctx, kSceNgs, kPatchInfo, {pa, route, 0}) == 0);
    CHECK(psv_ld32(route) == 1);
    CHECK(psv_ld32(route + 4) == 1);

    PlayerParams p = player(1);
    p.buffs[0] = PlayerBuffer{ba, 8192, 0, -1};
    REQUIRE(set_params(ctx, voices[0], 0, p) == 0);
    p.buffs[0] = PlayerBuffer{bb, 8192, 0, -1};
    REQUIRE(set_params(ctx, voices[1], 0, p) == 0);
    for (u32 v : {voices[0], voices[1], buss, master}) REQUIRE(call32(ctx, kSceNgs, kPlay, {v}) == 0);
    ngs.update();
    nx::vector<i16> out = ngs.output(master);
    bool mixed = true;
    for (u32 i = 0; i < kG; ++i) {
        const i16 want = static_cast<i16>(a[i] / 2 + b[i] / 4);
        mixed &= out[2 * i] == want && out[2 * i + 1] == want;
    }
    CHECK(mixed);

    // Paused (no fade: the mixer's lead-out is 0), the first voice is silent; resumed, it goes on where it was.
    REQUIRE(call32(ctx, kSceNgs, kPause, {voices[0]}) == 0);
    CHECK((ngs.state(voices[0]) & 32) != 0);
    ngs.update();
    out = ngs.output(master);
    bool paused = true;
    for (u32 i = 0; i < kG; ++i) paused &= out[2 * i] == static_cast<i16>(b[kG + i] / 4);
    CHECK(paused);
    REQUIRE(call32(ctx, kSceNgs, kResume, {voices[0]}) == 0);
    ngs.update();
    out = ngs.output(master);
    bool resumed = true;
    for (u32 i = 0; i < kG; ++i) resumed &= out[2 * i] == static_cast<i16>(a[kG + i] / 2 + b[2 * kG + i] / 4);
    CHECK(resumed);
    REQUIRE(call32(ctx, kSceNgs, kKill, {voices[1]}) == 0);
    CHECK(ngs.state(voices[1]) == 0);

    // Locks: one at a time, of the module's own interface; what is unlocked is checked.
    const u32 info = scratch(8);
    REQUIRE(call32(ctx, kSceNgs, kLock, {voices[0], 0, kPlayerParamsId, info}) == 0);
    CHECK(psv_ld32(info + 4) == sizeof(PlayerParams));
    CHECK(call32(ctx, kSceNgs, kLock, {voices[0], 0, kPlayerParamsId, info}) == kResourceLocked);
    CHECK(call32(ctx, kSceNgs, kLock, {voices[0], 2, kPlayerParamsId, scratch(8)}) == kParamTypeMismatch);
    t.rt.memory().host(psv_ld32(info))[offsetof(PlayerParams, channels)] = 3;
    CHECK(call32(ctx, kSceNgs, kUnlock, {voices[0], 0}) == kParamOutOfRange);
    const u32 message = scratch(128);
    REQUIRE(call32(ctx, kSceNgs, kOutOfRange, {voices[0], 0, message}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(message))).find("nChannels") != nx::string::npos);
}

TEST_CASE("ngs: a keyed-off envelope releases over its time, then the voice finishes") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    GuestCode code;
    const u32 callback = code.add(record);
    code.install();
    calls.clear();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 1)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    const nx::vector<i16> level(256, 16384);
    PlayerParams p = player(1);
    p.buffs[0] = PlayerBuffer{pcm(level), 512, -1, -1};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    struct Envelope {
        u32 id, size;
        struct {
            u32 msecs;
            f32 amplitude;
            u32 curve;
        } points[4];
        u32 release_msecs, num_points, loop_start;
        i32 loop_end;
    } e{};
    e.id = kEnvelopeParamsId;
    e.size = sizeof e;
    e.points[0] = {0, 1.0f, 0};
    e.num_points = 1;
    e.release_msecs = 10;  // (480 samples)
    e.loop_end = -1;
    REQUIRE(set_params(ctx, voice, 4, e) == 0);
    REQUIRE(call32(ctx, kSceNgs, kBypass, {voice, 4, 0}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kFinishedCallback, {voice, callback, 7}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    ngs.update();
    CHECK(ngs.output(master)[2 * 100] == 16384);
    REQUIRE(call32(ctx, kSceNgs, kKeyOff, {voice}) == 0);
    CHECK((ngs.state(voice) & 64) != 0);
    ngs.update();
    CHECK(ngs.state(voice) == 4);  // finalizing
    nx::vector<i16> out = ngs.output(master);
    CHECK(out[0] == 16384);
    CHECK(std::abs(out[2 * 255] - static_cast<i32>(16384 * (1 - 255.0 / 480))) <= 2);
    bool falling = true;
    for (u32 i = 1; i < kG; ++i) falling &= out[2 * i] <= out[2 * (i - 1)];
    CHECK(falling);
    CHECK(calls.empty());
    ngs.update();
    out = ngs.output(master);
    CHECK(out[2 * 200] > 0);
    CHECK(out[2 * 224] == 0);  // (480 samples after the key off)
    CHECK(out[2 * 255] == 0);
    CHECK(ngs.state(voice) == 0);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].user == 7);
}

namespace {

// The VAG formula: each sample is its 4-bit residual scaled by the block's shift plus the filter's prediction from the
// four samples before, unrounded. coef(filter) gives the filter's taps in 1/8192ths.
template <typename Coef>
nx::vector<f64> vag_reference(const u8* vag, u32 blocks, Coef coef) {
    nx::vector<f64> out;
    f64 h[4] = {};
    for (u32 blk = 0; blk < blocks; ++blk) {
        const u8* bp = vag + 16 * blk;
        const i32 shift = bp[0] & 15;
        const std::array<i32, 4> c = coef(static_cast<u32>((bp[0] >> 4) | ((bp[1] >> 4) << 4)));
        for (u32 i = 0; i < 28; ++i) {
            i32 nib = (i & 1) ? bp[2 + i / 2] >> 4 : bp[2 + i / 2] & 15;
            if (nib >= 8) nib -= 16;
            const f64 s = std::clamp((nib * 4096 >> shift) + (c[0] * h[0] + c[1] * h[1] + c[2] * h[2] + c[3] * h[3]) / 8192.0,
                                     -32768.0, 32767.0);
            h[3] = h[2], h[2] = h[1], h[1] = h[0], h[0] = s;
            out.push_back(s);
        }
    }
    return out;
}

// Whether the master's output is the formula's rounded, sample for sample (give or take the f32 in between).
bool decodes_as(const nx::vector<i16>& out, const nx::vector<f64>& want, u32 channel = 0) {
    bool same = true;
    for (usize i = 0; i < want.size(); ++i) same &= std::fabs(out[2 * i + channel] - want[i]) <= 0.51;
    return same;
}

}  // namespace

TEST_CASE("ngs: VAG blocks decode as the ADPCM filters say (unrounded); the bytes count per block") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 1)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    // Two blocks: filter 0 shift 4, then filter 3 shift 9.
    u8 vag[32] = {};
    vag[0] = 0x04;
    vag[16] = 0x39;
    for (u32 i = 0; i < 14; ++i) {
        vag[2 + i] = static_cast<u8>((i * 0x37 + 0x12) & 0xFF);
        vag[18 + i] = static_cast<u8>((i * 0x5B + 0x8E) & 0xFF);
    }
    const u32 at = aligned(32);
    std::memcpy(t.rt.memory().host(at), vag, 32);
    // (the reference: the PlayStation ADPCM filters, in 1/64ths there)
    const nx::vector<f64> want = vag_reference(vag, 2, [](u32 f) {
        const i32 c[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        return std::array<i32, 4>{c[f][0] * 128, c[f][1] * 128, 0, 0};
    });
    PlayerParams p = player(1);
    p.type = 1;  // SCE_NGS_PLAYER_TYPE_ADPCM
    p.buffs[0] = PlayerBuffer{at, 32, 0, -1};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    ngs.update();
    const nx::vector<i16> out = ngs.output(master);
    CHECK(decodes_as(out, want));
    CHECK(out[2 * 56] == 0);
    const u32 st = scratch(24);
    REQUIRE(call32(ctx, kSceNgs, kStateData, {voice, 0, st, 24}) == 0);
    CHECK(psv_ld32(st + 8) == 56);
    CHECK(psv_ld32(st + 12) == 32);
}

TEST_CASE("ngs: HE-VAG blocks take a 7-bit filter and up to 4 taps") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 1)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    // A classic block to start from (filter 0 shift 2), then HE filters 12 (2 taps) shift 8, 45 and 127 (4 taps)
    // shifts 9 and 10, and classic filter 4 shift 11 on what they left. The filter's bits 4-6 are byte 1's high nibble.
    u8 vag[80] = {};
    const u8 head[5][2] = {{0x02, 0x00}, {0xC8, 0x00}, {0xD9, 0x20}, {0xFA, 0x70}, {0x4B, 0x00}};
    for (u32 b = 0; b < 5; ++b) {
        vag[16 * b] = head[b][0], vag[16 * b + 1] = head[b][1];
        for (u32 i = 0; i < 14; ++i) vag[16 * b + 2 + i] = static_cast<u8>((i * 0x3B + b * 0x55 + 0x17) & 0xFF);
    }
    const u32 at = aligned(80);
    std::memcpy(t.rt.memory().host(at), vag, 80);
    // (the filters as measured against vagconv2, in 1/8192ths)
    const nx::vector<f64> want = vag_reference(vag, 5, [](u32 f) {
        switch (f) {
            case 12: return std::array<i32, 4>{7832, -7680, 0, 0};
            case 45: return std::array<i32, 4>{-4745, -4122, -5486, -1493};
            case 127: return std::array<i32, 4>{5333, 2999, 775, -1132};
            case 4: return std::array<i32, 4>{122 * 128, -60 * 128, 0, 0};
            default: return std::array<i32, 4>{};
        }
    });
    PlayerParams p = player(1);
    p.type = 1;  // SCE_NGS_PLAYER_TYPE_ADPCM
    p.buffs[0] = PlayerBuffer{at, 80, 0, -1};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    ngs.update();
    const nx::vector<i16> out = ngs.output(master);
    CHECK(decodes_as(out, want));
    i32 loudest = 0;  // (not silence)
    for (u32 i = 28; i < 140; ++i) loudest = std::max(loudest, std::abs(static_cast<i32>(out[2 * i])));
    CHECK(loudest > 1000);
    CHECK(out[2 * 140] == 0);
}

TEST_CASE("ngs: stereo VAG takes a block of each channel in turn, the left first; a swap keeps the samples before") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 2)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    // Left: filter 0 shift 2, then classic filter 1 shift 6. Right: filter 0 shift 5, then HE filter 45 shift 9.
    // Their second blocks predict from the first, and each pair is a buffer of its own (0, then 1).
    u8 left[32] = {}, right[32] = {};
    left[0] = 0x02, left[16] = 0x16;
    right[0] = 0x05, right[16] = 0xD9, right[17] = 0x20;
    for (u32 i = 0; i < 14; ++i) {
        left[2 + i] = static_cast<u8>((i * 0x37 + 0x12) & 0xFF), left[18 + i] = static_cast<u8>((i * 0x5B + 0x8E) & 0xFF);
        right[2 + i] = static_cast<u8>((i * 0x29 + 0xC4) & 0xFF), right[18 + i] = static_cast<u8>((i * 0x71 + 0x3D) & 0xFF);
    }
    const u32 at = aligned(64);
    for (u32 b = 0; b < 2; ++b) {
        std::memcpy(t.rt.memory().host(at + 32 * b), left + 16 * b, 16);
        std::memcpy(t.rt.memory().host(at + 32 * b + 16), right + 16 * b, 16);
    }
    const auto coef = [](u32 f) {
        switch (f) {
            case 1: return std::array<i32, 4>{60 * 128, 0, 0, 0};
            case 45: return std::array<i32, 4>{-4745, -4122, -5486, -1493};
            default: return std::array<i32, 4>{};
        }
    };
    PlayerParams p = player(2);
    p.type = 1;  // SCE_NGS_PLAYER_TYPE_ADPCM
    p.buffs[0] = PlayerBuffer{at, 32, 0, 1};
    p.buffs[1] = PlayerBuffer{at + 32, 32, 0, -1};
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    ngs.update();
    const nx::vector<i16> out = ngs.output(master);
    CHECK(decodes_as(out, vag_reference(left, 2, coef), 0));
    CHECK(decodes_as(out, vag_reference(right, 2, coef), 1));
    CHECK(out[2 * 56] == 0);
    CHECK(out[2 * 56 + 1] == 0);
    const u32 st = scratch(24);
    REQUIRE(call32(ctx, kSceNgs, kStateData, {voice, 0, st, 24}) == 0);
    CHECK(psv_ld32(st + 8) == 56);
    CHECK(psv_ld32(st + 12) == 64);
}

TEST_CASE("ngs: a generated tone, and a low-pass EQ on the send taking it away") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, 1)[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    struct Generator {
        u32 id, size, mode;
        i32 frequency;
        f32 amplitude, pulse_width;
        u32 sample_offset, phase_angle;
    } g{kGeneratorParamsId, 32, 0, 8000, 0.5f, 0.5f, 0, 0};
    REQUIRE(set_params(ctx, voice, 1, g) == 0);
    REQUIRE(call32(ctx, kSceNgs, kBypass, {voice, 0, 2}) == 0);  // (no player)
    REQUIRE(call32(ctx, kSceNgs, kBypass, {voice, 1, 0}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    auto rms = [&] {
        f64 sum = 0;
        for (u32 k = 0; k < 4; ++k) {
            ngs.update();
            const nx::vector<i16> out = ngs.output(master);
            if (k < 2) continue;  // (the filter settles)
            for (u32 i = 0; i < kG; ++i) sum += static_cast<f64>(out[2 * i]) * out[2 * i];
        }
        return std::sqrt(sum / (2 * kG));
    };
    const f64 open = rms();
    CHECK(std::fabs(open - 0.5 * 32768 / std::sqrt(2.0)) < 0.01 * open);
    struct Eq {
        u32 id, size;
        struct {
            u32 mode;
            f32 frequency, resonance, gain;
        } f[4];
    } eq{};
    eq.id = kEqParamsId;
    eq.size = sizeof eq;
    for (auto& f : eq.f) f = {0, 1000.0f, 0.707f, 0.0f};
    eq.f[0] = {1, 500.0f, 0.707f, 0.0f};  // (low pass, resonant)
    REQUIRE(set_params(ctx, voice, 6, eq) == 0);
    REQUIRE(call32(ctx, kSceNgs, kBypass, {voice, 6, 0}) == 0);
    const f64 filtered = rms();
    CHECK(filtered < open / 100);
}

namespace {

// A RIFF chunk's data in a file, or empty.
std::span<const u8> riff_chunk(const nx::vector<u8>& file, const char* id) {
    for (usize at = 12; at + 8 <= file.size();) {
        u32 size;
        std::memcpy(&size, file.data() + at + 4, 4);
        if (std::memcmp(file.data() + at, id, 4) == 0 && at + 8 + size <= file.size())
            return std::span<const u8>(file.data() + at + 8, size);
        at += 8 + size + (size & 1);
    }
    return {};
}

nx::vector<u8> read_file(const nx::string& path) {
    nx::vector<u8> out;
    if (auto data = nx::fs::file_read(nx::fs::path_view(path))) out.assign(data->data(), data->data() + data->size());
    return out;
}

}  // namespace

// ATRAC9 data made by the SDK's at9tool (an encoding of a test signal, and at9tool's own decoding of it as the
// reference) is SDK output: it is made locally by scripts/at9_check.py, which runs this test with
// PSVRECORE_AT9_DIR naming its folder. Without it there is nothing to check.
TEST_CASE("ngs: ATRAC9 voices play what at9tool decodes, from the start and from a seek") {
    const auto dir = nx::os::environment_variable("PSVRECORE_AT9_DIR");
    if (!dir) {
        std::printf("(PSVRECORE_AT9_DIR not set: no ATRAC9 data to check)\n");
        return;
    }
    auto entries = nx::fs::directory_entries(nx::fs::path_view(*dir));
    REQUIRE(entries.has_value());
    u32 checked = 0;
    for (const auto& e : *entries) {
        const nx::string path(e.path);
        if (path.size() < 4 || path.substr(path.size() - 4) != ".at9") continue;
        const nx::vector<u8> at9 = read_file(path);
        const nx::vector<u8> wav = read_file(path.substr(0, path.size() - 4) + ".wav");
        const auto fmt = riff_chunk(at9, "fmt "), fact = riff_chunk(at9, "fact"), data = riff_chunk(at9, "data");
        const auto ref_fmt = riff_chunk(wav, "fmt "), ref = riff_chunk(wav, "data");
        REQUIRE(fmt.size() >= 48);
        REQUIRE(fact.size() >= 4);
        REQUIRE(!ref.empty());
        u16 channels;
        i32 config, total;
        std::memcpy(&channels, fmt.data() + 2, 2);
        std::memcpy(&config, fmt.data() + 44, 4);
        std::memcpy(&total, fact.data(), 4);
        u16 ref_channels;
        std::memcpy(&ref_channels, ref_fmt.data() + 2, 2);
        REQUIRE(ref_channels == channels);
        const u32 ref_frames = static_cast<u32>(ref.size() / (2 * channels));

        for (const i32 seek : {0, 48000}) {
            TestRuntime t;
            PsvContext* ctx = t.ctx();
            Ngs ngs(ctx);
            const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
            const u32 voice = ngs.rack(0x14EF65A0 /* sceNgsVoiceDefGetAtrac9Voice */, 1, channels)[0];
            REQUIRE(ngs.patch(voice, master) == 0);
            const i32 count = nx::min<i32>(total - seek, 96000);
            const u32 skip = scratch(16);
            REQUIRE(call32(ctx, kSceNgs, 0x2A9FA501, {u(seek), u(count), u(config), skip}) == 0);
            i32 offset, bytes;
            i16 start_skip, end_skip;
            std::memcpy(&offset, t.rt.memory().host(skip), 4);
            std::memcpy(&bytes, t.rt.memory().host(skip + 4), 4);
            std::memcpy(&start_skip, t.rt.memory().host(skip + 8), 2);
            std::memcpy(&end_skip, t.rt.memory().host(skip + 10), 2);
            REQUIRE(offset + bytes <= static_cast<i32>(data.size()));
            const u32 buffer = aligned(static_cast<u32>(bytes));
            std::memcpy(t.rt.memory().host(buffer), data.data() + offset, static_cast<usize>(bytes));
            struct At9Params {
                u32 id, size;
                struct {
                    u32 buffer;
                    i32 bytes;
                    i16 loop_count, next, discard_start, discard_end;
                } buffs[4];
                f32 frequency, scalar;
                i32 lead_in, limit;
                i8 channels;
                i8 map[2];
                i8 reserved;
                i32 config;
            } p{};
            static_assert(sizeof(At9Params) == 96);
            p.id = 0x01015CAA;
            p.size = sizeof p;
            for (auto& b : p.buffs) b = {0, 0, 0, -1, 0, 0};
            p.buffs[0] = {buffer, bytes, 0, -1, start_skip, end_skip};
            p.frequency = 48000.0f;
            p.scalar = 1.0f;
            p.channels = static_cast<i8>(channels);
            p.map[0] = 0;
            p.map[1] = channels == 2 ? 1 : 0;
            p.config = config;
            REQUIRE(set_params(ctx, voice, 0, p) == 0);
            REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
            REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
            nx::vector<i16> got;
            while (got.size() < static_cast<usize>(count + kG) * 2) {
                ngs.update();
                const nx::vector<i16> out = ngs.output(master);
                got.insert(got.end(), out.begin(), out.end());
            }
            u32 differ = 0, max_diff = 0, compared = 0;
            for (i32 i = 0; i < count && static_cast<u32>(seek + i) < ref_frames; ++i, ++compared)
                for (u32 c = 0; c < 2; ++c) {
                    i16 want;
                    std::memcpy(&want, ref.data() + (static_cast<usize>(seek + i) * channels + nx::min<u32>(c, channels - 1u)) * 2, 2);
                    const u32 d = static_cast<u32>(std::abs(static_cast<i32>(got[2 * static_cast<usize>(i) + c]) - want));
                    differ += d != 0;
                    max_diff = nx::max(max_diff, d);
                }
            bool silent_after = true;
            for (usize i = static_cast<usize>(count) * 2; i < got.size(); ++i) silent_after &= got[i] == 0;
            std::printf("%s from %d: %u frames compared, %u samples differ, at most by %u\n", path.c_str(), seek,
                        compared, differ, max_diff);
            CHECK(compared > 0);
            CHECK(differ == 0);
            CHECK(silent_after);
            ++checked;
        }
    }
    CHECK(checked > 0);
}

namespace {

// A VAG file's blocks played through a player voice: the samples (`channels` a frame), from the first block on. With
// `stream` bytes the data goes through two buffers of that size in turn, each refilled with what comes next once the
// voice has moved on to the other (as the SDK's adpcm_stream sample streams); otherwise it is one buffer.
nx::vector<i16> play_vag(const nx::vector<u8>& file, usize frames, u32 channels = 1, u32 stream = 0) {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    Ngs ngs(ctx);
    const u32 master = ngs.rack(kDefMaster, 1, 2)[0];
    const u32 voice = ngs.rack(kDefTemplate1, 1, static_cast<i32>(channels))[0];
    REQUIRE(ngs.patch(voice, master) == 0);
    const u8* data = file.data() + 48;  // (after the 48-byte header)
    const u32 bytes = static_cast<u32>(file.size() - 48);
    PlayerParams p = player(static_cast<i8>(channels));
    p.type = 1;  // SCE_NGS_PLAYER_TYPE_ADPCM
    u32 at[2] = {}, sent = 0;
    const auto fill = [&](u32 b) {
        const u32 n = nx::min(stream, bytes - sent);
        std::memcpy(t.rt.memory().host(at[b]), data + sent, n);
        sent += n;
        p.buffs[b] = PlayerBuffer{at[b], static_cast<i32>(n), 0, sent < bytes ? static_cast<i16>(1 - b) : i16{-1}};
    };
    if (stream == 0) {
        at[0] = aligned(bytes);
        std::memcpy(t.rt.memory().host(at[0]), data, bytes);
        p.buffs[0] = PlayerBuffer{at[0], static_cast<i32>(bytes), 0, -1};
    } else {
        at[0] = aligned(stream), at[1] = aligned(stream);
        fill(0);
        if (sent < bytes) fill(1);
    }
    REQUIRE(set_params(ctx, voice, 0, p) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {voice}) == 0);
    REQUIRE(call32(ctx, kSceNgs, kPlay, {master}) == 0);
    nx::vector<i16> got;
    u32 playing = 0;
    const u32 st = scratch(24);
    while (got.size() < frames * channels) {
        ngs.update();
        const nx::vector<i16> out = ngs.output(master);
        for (u32 i = 0; i < kG; ++i)
            for (u32 c = 0; c < channels; ++c) got.push_back(out[2 * i + c]);
        if (stream == 0) continue;
        REQUIRE(call32(ctx, kSceNgs, kStateData, {voice, 0, st, 24}) == 0);
        const u32 now = psv_ld32(st + 4);
        if (now != playing && sent < bytes) {
            fill(playing);
            REQUIRE(set_params(ctx, voice, 0, p) == 0);
        }
        playing = now;
    }
    return got;
}

}  // namespace

// VAG made by the SDK's vagconv2 is SDK output: scripts/vag_check.py makes it locally from test signals, in the
// default (HE-VAG) mode as <name>_he.vag and the backwards-compatible one as <name>_cb.vag next to the source
// <name>.wav, and runs this test with PSVRECORE_VAG_DIR naming the folder. Without it there is nothing to check.
TEST_CASE("ngs: VAG made by vagconv2 plays back its source, HE-VAG at least as well as classic") {
    const auto dir = nx::os::environment_variable("PSVRECORE_VAG_DIR");
    if (!dir) {
        std::printf("(PSVRECORE_VAG_DIR not set: no VAG data to check)\n");
        return;
    }
    auto entries = nx::fs::directory_entries(nx::fs::path_view(*dir));
    REQUIRE(entries.has_value());
    u32 checked = 0;
    for (const auto& e : *entries) {
        const nx::string path(e.path);
        if (path.size() < 7 || path.substr(path.size() - 7) != "_he.vag") continue;
        const nx::string stem = path.substr(0, path.size() - 7);
        const nx::vector<u8> source = read_file(stem + ".wav");
        const auto ref = riff_chunk(source, "data");
        REQUIRE(!ref.empty());
        const usize n = ref.size() / 2;
        f64 snr[2] = {};
        u32 he_blocks = 0;
        for (u32 mode = 0; mode < 2; ++mode) {
            const nx::vector<u8> vag = read_file(stem + (mode == 0 ? "_he.vag" : "_cb.vag"));
            REQUIRE(vag.size() > 64);
            REQUIRE(std::memcmp(vag.data(), "VAGp", 4) == 0);
            for (usize at = 48; mode == 0 && at + 16 <= vag.size(); at += 16)
                he_blocks += ((vag[at] >> 4) | ((vag[at + 1] >> 4) << 4)) > 4;
            // (vagconv2 starts with a silent block: the source begins at the second)
            const nx::vector<i16> got = play_vag(vag, 28 + n);
            // (streamed in 4 KB buffers it plays the same: a swap goes on from the samples before)
            if (mode == 0) CHECK(play_vag(vag, 28 + n, 1, 4096) == got);
            f64 signal = 0, noise = 0;
            for (usize i = 0; i < n; ++i) {
                i16 want;
                std::memcpy(&want, ref.data() + 2 * i, 2);
                signal += static_cast<f64>(want) * want;
                noise += std::pow(static_cast<f64>(got[28 + i]) - want, 2);
            }
            snr[mode] = 10 * std::log10(signal / nx::max(noise, 1.0));
        }
        std::printf("%s: HE-VAG %.2f dB (%u blocks with HE filters), classic %.2f dB\n", stem.c_str(), snr[0],
                    he_blocks, snr[1]);
        CHECK(he_blocks > 0);
        CHECK(snr[0] >= 20);
        CHECK(snr[1] >= 20);
        // (above 60 dB both are within a step of the source: how many samples land one off is the encoder's doing)
        CHECK(snr[0] >= nx::min(snr[1] - 1, 60.0));
        ++checked;
    }
    CHECK(checked > 0);
}

// Stereo VAG made by vagconv2 from a stereo WAV: scripts/vag_check.py makes it locally as <name>_st.vag next to its
// source <name>.wav (and copies the SDK's own stereo VAG there the same way). Each channel is measured against the
// source's, played from one buffer and streamed in 4 KB buffers.
TEST_CASE("ngs: stereo VAG made by vagconv2 plays back both channels of its source, from one buffer or streamed") {
    const auto dir = nx::os::environment_variable("PSVRECORE_VAG_DIR");
    if (!dir) {
        std::printf("(PSVRECORE_VAG_DIR not set: no VAG data to check)\n");
        return;
    }
    auto entries = nx::fs::directory_entries(nx::fs::path_view(*dir));
    REQUIRE(entries.has_value());
    u32 checked = 0;
    for (const auto& e : *entries) {
        const nx::string path(e.path);
        if (path.size() < 7 || path.substr(path.size() - 7) != "_st.vag") continue;
        const nx::string stem = path.substr(0, path.size() - 7);
        const nx::vector<u8> source = read_file(stem + ".wav");
        const auto fmt = riff_chunk(source, "fmt "), ref = riff_chunk(source, "data");
        REQUIRE(fmt.size() >= 16);
        REQUIRE(fmt[2] == 2);
        const usize n = ref.size() / 4;
        const nx::vector<u8> vag = read_file(path);
        REQUIRE(vag.size() > 80);
        REQUIRE(std::memcmp(vag.data(), "VAGp", 4) == 0);
        CHECK(vag[0x1E] == 2);  // (the header's channel count)
        // (a silent block of each channel first: the source begins at the second pair)
        const nx::vector<i16> got = play_vag(vag, 28 + n, 2);
        CHECK(play_vag(vag, 28 + n, 2, 4096) == got);
        f64 snr[2] = {};
        for (u32 c = 0; c < 2; ++c) {
            f64 signal = 0, noise = 0;
            for (usize i = 0; i < n; ++i) {
                i16 want;
                std::memcpy(&want, ref.data() + 4 * i + 2 * c, 2);
                signal += static_cast<f64>(want) * want;
                noise += std::pow(static_cast<f64>(got[2 * (28 + i) + c]) - want, 2);
            }
            snr[c] = 10 * std::log10(signal / nx::max(noise, 1.0));
        }
        std::printf("%s: left %.2f dB, right %.2f dB\n", stem.c_str(), snr[0], snr[1]);
        CHECK(snr[0] >= 20);
        CHECK(snr[1] >= 20);
        ++checked;
    }
    CHECK(checked > 0);
}
