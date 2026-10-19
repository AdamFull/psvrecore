// Audio output (SceAudio): port arguments and limits, pacing on the kernel's clock, what reaches the sink.

#include "nxtest.h"
#include "runtime_support.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

constexpr u32 kSceAudio = 0x438BB957;
constexpr u32 kOpen = 0x5BC341E4, kRelease = 0x69E2E6B5, kOutput = 0x02DB3F5F, kSetVolume = 0x64167F11,
              kSetConfig = 0xB8BA0D07, kGetConfig = 0x9C8EDAEA, kRest = 0x9A5370C4;
constexpr i32 kNotOpened = static_cast<i32>(0x80260001u), kBusy = static_cast<i32>(0x80260002u),
              kPortFull = static_cast<i32>(0x80260005u), kInvalidSize = static_cast<i32>(0x80260006u),
              kInvalidFreq = static_cast<i32>(0x80260008u), kInvalidVolume = static_cast<i32>(0x80260009u),
              kInvalidType = static_cast<i32>(0x8026000Au);

u32 u(i32 v) { return static_cast<u32>(v); }

struct Capture : AudioSink {
    struct Block {
        u32 port, rate, channels;
        u64 start_ns;
        nx::vector<i16> samples;
    };
    nx::mutex lock;
    nx::vector<Block> blocks;
    nx::vector<u32> released;
    void play(const AudioBlock& b) override {
        nx::scoped_lock<nx::mutex> held(lock);
        blocks.push_back(Block{b.port, b.rate, b.channels, b.start_ns, nx::vector<i16>(b.samples.begin(), b.samples.end())});
    }
    void port_released(u32 port) override {
        nx::scoped_lock<nx::mutex> held(lock);
        released.push_back(port);
    }
};

}  // namespace

TEST_CASE("audio out: port types, arguments, limits and configuration") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    CHECK(call32(ctx, kSceAudio, kOpen, {3, 256, 48000, 1}) == kInvalidType);
    CHECK(call32(ctx, kSceAudio, kOpen, {0, 100, 48000, 1}) == kInvalidSize);       // (not a multiple of 64)
    CHECK(call32(ctx, kSceAudio, kOpen, {0, 64 * 1024, 48000, 1}) == kInvalidSize);
    CHECK(call32(ctx, kSceAudio, kOpen, {0, 256, 44100, 1}) == kInvalidFreq);       // (MAIN is 48 kHz only)
    CHECK(call32(ctx, kSceAudio, kOpen, {1, 256, 22050, 0}) > 0);                  // (BGM converts)
    CHECK(call32(ctx, kSceAudio, kOpen, {1, 256, 48000, 0}) == kPortFull);          // (one BGM port)
    CHECK(call32(ctx, kSceAudio, kOpen, {2, 256, 12345, 0}) == kInvalidFreq);
    i32 main_ports[8];
    for (i32& p : main_ports) {
        p = call32(ctx, kSceAudio, kOpen, {0, 256, 48000, 1});
        CHECK(p > 0);
    }
    CHECK(call32(ctx, kSceAudio, kOpen, {0, 256, 48000, 1}) == kPortFull);          // (eight MAIN ports)
    const i32 p = main_ports[0];
    CHECK(call32(ctx, kSceAudio, kGetConfig, {u(p), 0}) == 256);
    CHECK(call32(ctx, kSceAudio, kGetConfig, {u(p), 1}) == 48000);
    CHECK(call32(ctx, kSceAudio, kGetConfig, {u(p), 2}) == 1);
    CHECK(call32(ctx, kSceAudio, kSetConfig, {u(p), 512, u(-1), 0}) == 0);
    CHECK(call32(ctx, kSceAudio, kGetConfig, {u(p), 0}) == 512);
    CHECK(call32(ctx, kSceAudio, kGetConfig, {u(p), 2}) == 0);
    const u32 vol = scratch(8);
    psv_st32(vol, 40000), psv_st32(vol + 4, 0);
    CHECK(call32(ctx, kSceAudio, kSetVolume, {u(p), 3, vol}) == kInvalidVolume);
    for (i32 q : main_ports) CHECK(call32(ctx, kSceAudio, kRelease, {u(q)}) == 0);
    CHECK(call32(ctx, kSceAudio, kRelease, {u(p)}) == kNotOpened);
    CHECK(call32(ctx, kSceAudio, kOutput, {u(p), 0}) == kNotOpened);
}

TEST_CASE("audio out: output is paced by playback; blocks reach the sink with their start and volume") {
    Capture capture;
    TestRuntime t(kernel::kUserCpuCount, kernel::kGamePriority, &capture);
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    constexpr u32 kGrain = 256, kBlocks = 24;
    const i32 port = call32(ctx, kSceAudio, kOpen, {0, kGrain, 48000, 1});
    REQUIRE(port > 0);
    const u32 vol = scratch(8);
    psv_st32(vol, 16384), psv_st32(vol + 4, 8192);  // (left half, right a quarter)
    REQUIRE(call32(ctx, kSceAudio, kSetVolume, {u(port), 3, vol}) == 0);
    const u32 pcm = scratch(kGrain * 4);
    for (u32 i = 0; i < kGrain * 2; ++i) {
        const i16 v = static_cast<i16>((i & 1) ? -1000 - static_cast<i32>(i) : 1000 + static_cast<i32>(i));
        std::memcpy(t.rt.memory().host(pcm + i * 2), &v, 2);
    }
    nx::vector<u64> returned;
    for (u32 b = 0; b < kBlocks; ++b) {
        REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), pcm}) == 0);
        returned.push_back(k.clock_ns());
    }
    // A block starts when the one before it has played: 256 samples at 48 kHz apart, exactly; the call returns
    // when its block starts (within the host's scheduling slack).
    REQUIRE(capture.blocks.size() == kBlocks);
    const u64 first = capture.blocks[0].start_ns;
    for (u32 b = 0; b < kBlocks; ++b) {
        const u64 want = first + (u64{b} * kGrain * 1'000'000'000ull) / 48000;
        CHECK(capture.blocks[b].start_ns >= want);
        CHECK(capture.blocks[b].start_ns - want <= 1);  // (rounding of the timeline)
        CHECK(returned[b] >= capture.blocks[b].start_ns);
        CHECK(returned[b] - capture.blocks[b].start_ns < 4'000'000);
        CHECK(capture.blocks[b].rate == 48000);
        CHECK(capture.blocks[b].channels == 2);
    }
    // The volume: sample * volume >> 15 per channel.
    const auto& s = capture.blocks[3].samples;
    REQUIRE(s.size() == kGrain * 2);
    CHECK(s[0] == (1000 * 16384) >> 15);
    CHECK(s[1] == ((-1001) * 8192) >> 15);
    CHECK(s[100] == ((1100) * 16384) >> 15);
    // What has not played: at most the block that just started and none of the earlier ones.
    const i32 rest = call32(ctx, kSceAudio, kRest, {u(port)});
    CHECK(rest > 0);
    CHECK(rest <= static_cast<i32>(kGrain));
    // A port still playing cannot be released; output of NULL waits until everything has played.
    CHECK(call32(ctx, kSceAudio, kRelease, {u(port)}) == kBusy);
    REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), 0}) == 0);
    CHECK(k.clock_ns() >= first + (u64{kBlocks} * kGrain * 1'000'000'000ull) / 48000);
    CHECK(call32(ctx, kSceAudio, kRest, {u(port)}) == 0);
    CHECK(call32(ctx, kSceAudio, kRelease, {u(port)}) == 0);
    REQUIRE(capture.released.size() == 1);
    CHECK(capture.released[0] == static_cast<u32>(port));
}

TEST_CASE("audio out: a port that runs dry starts again when it is given more") {
    Capture capture;
    TestRuntime t(kernel::kUserCpuCount, kernel::kGamePriority, &capture);
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    const i32 port = call32(ctx, kSceAudio, kOpen, {1, 64, 8000, 0});  // (BGM, mono, 8 ms blocks)
    REQUIRE(port > 0);
    const u32 pcm = scratch(128);
    REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), pcm}) == 0);
    REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), 0}) == 0);
    // (an idle gap, then a block: it starts at once, not where the timeline left off)
    k.delay_thread(20'000, false);
    const u64 before = k.clock_ns();
    REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), pcm}) == 0);
    REQUIRE(capture.blocks.size() == 2);
    CHECK(capture.blocks[1].start_ns >= before);
    CHECK(capture.blocks[1].start_ns - capture.blocks[0].start_ns >= 28'000'000);
    CHECK(capture.blocks[1].channels == 1);
    CHECK(capture.blocks[1].rate == 8000);
    REQUIRE(call32(ctx, kSceAudio, kOutput, {u(port), 0}) == 0);
    CHECK(call32(ctx, kSceAudio, kRelease, {u(port)}) == 0);
}
