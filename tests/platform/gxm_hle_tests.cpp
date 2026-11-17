// SceGxm's queries on programs, parameters and fragment programs, and the calls that need no device: on small
// GXP images in guest memory, through the guest calling convention.

#include <cstring>

#include "nxtest.h"
#include "psvrecore/gxm/hle.hpp"
#include "runtime_support.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;
constexpr i32 kUninitialized = static_cast<i32>(0x805B0000u), kInvalidValue = static_cast<i32>(0x805B0003u),
              kInvalidPointer = static_cast<i32>(0x805B0004u);

// A GXP image (a header, 0x00-0x9F, and what the test puts after it) copied into guest memory.
struct Gxp {
    u8 data[0x100] = {};
    Gxp() {
        std::memcpy(data, "GXP\0\x01\x05", 6);
        put32(0x08, sizeof data);
    }
    void put32(u32 at, u32 v) { std::memcpy(data + at, &v, 4); }
    void offset(u32 field, u32 target) { put32(field, target - field); }  // (self-relative)
    u32 place() const {
        const u32 at = scratch(sizeof data);
        std::memcpy(current_runtime()->memory().host(at), data, sizeof data);
        return at;
    }
};

// A vertex program with two attributes (records at 0xC0, resources 0 and 4), the second read in its own format.
Gxp vertex_program() {
    Gxp g;
    g.put32(0x24, 2);
    g.offset(0x28, 0xC0);
    g.offset(0x2C, 0xA0);
    g.put32(0xA0 + 0x08, 1u << 4);    // register format mask: resource 4
    g.put32(0xC4, 0x00000400);        // attribute, f32, 4 components
    g.put32(0xC8, 1), g.put32(0xCC, 0);
    g.put32(0xD4, 0x00000210 | 0);    // attribute, f16, 2 components
    g.put32(0xD8, 1), g.put32(0xDC, 4);
    return g;
}

// A fragment program writing f16x4.
Gxp fragment_program() {
    Gxp g;
    g.put32(0x14, 1);
    g.offset(0x2C, 0xA0);
    g.put32(0xA0 + 0x08, 0x04010000);
    return g;
}

}  // namespace

TEST_CASE("gxm: parameters' indices and register formats, programs compared, a fragment program's output format") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    constexpr u32 kGetParameter = 0x06FF9151, kGetIndex = 0x6E61DDF5, kIsRegFormat = 0x871E5009,
                  kIsEquivalent = 0xE23C838C, kOutputFormat = 0xE11603B1;

    const Gxp vg = vertex_program();
    const u32 v = vg.place();
    const u32 p0 = call32(ctx, kSceGxm, kGetParameter, {v, 0}), p1 = call32(ctx, kSceGxm, kGetParameter, {v, 1});
    REQUIRE(p0 == v + 0xC0);
    REQUIRE(p1 == v + 0xD0);
    CHECK(call32(ctx, kSceGxm, kGetIndex, {v, p0}) == 0);
    CHECK(call32(ctx, kSceGxm, kGetIndex, {v, p1}) == 1);
    CHECK(call32(ctx, kSceGxm, kGetIndex, {v, v}) == 2);  // (not a record: the count)
    CHECK(call32(ctx, kSceGxm, kIsRegFormat, {v, p0}) == 0);
    CHECK(call32(ctx, kSceGxm, kIsRegFormat, {v, p1}) == 1);

    // The same bytes elsewhere are the same program; one byte changed, another.
    const u32 copy = vg.place();
    CHECK(call32(ctx, kSceGxm, kIsEquivalent, {v, copy}) == 1);
    CHECK(call32(ctx, kSceGxm, kIsEquivalent, {v, v}) == 1);
    current_runtime()->memory().host(copy)[0xCC] = 1;
    CHECK(call32(ctx, kSceGxm, kIsEquivalent, {v, copy}) == 0);
    CHECK(call32(ctx, kSceGxm, kIsEquivalent, {v, 0}) == 0);

    const u32 f = fragment_program().place();
    const u32 out = scratch(8);
    CHECK(call32(ctx, kSceGxm, kOutputFormat, {f, out, out + 4}) == 0);
    CHECK(psv_ld32(out) == 1);  // SCE_GXM_PARAMETER_TYPE_F16
    CHECK(psv_ld32(out + 4) == 4);
    CHECK(call32(ctx, kSceGxm, kOutputFormat, {v, out, out + 4}) == kInvalidValue);  // (a vertex program)
    CHECK(call32(ctx, kSceGxm, kOutputFormat, {f, 0, out + 4}) == kInvalidPointer);
}

TEST_CASE("gxm: pass types, the parameter buffer's threshold, vertex fences and waits for events") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    constexpr u32 kPassType = 0xCE0B0A76, kPatcherCreate = 0x05032658, kMaskUpdate = 0xA4433427,
                  kPatcherDestroy = 0xEAA5B100, kThreshold = 0x6ABF3F76, kVertexFence = 0xE05277D6,
                  kWaitEvent = 0x8BD94593;

    const u32 params = scratch(0x40), out = scratch(8);
    REQUIRE(call32(ctx, kSceGxm, kPatcherCreate, {params, out}) == 0);
    const u32 patcher = psv_ld32(out);
    REQUIRE(call32(ctx, kSceGxm, kMaskUpdate, {patcher, out + 4}) == 0);
    CHECK(static_cast<u32>(call32(ctx, kSceGxm, kPassType, {psv_ld32(out + 4)})) == 0x06000000u);  // mask update
    CHECK(call32(ctx, kSceGxm, kPassType, {params}) == 0);  // (not a fragment program: opaque)
    CHECK(call32(ctx, kSceGxm, kPatcherDestroy, {patcher}) == 0);

    // (before sceGxmInitialize)
    CHECK(call32(ctx, kSceGxm, kThreshold, {out}) == kUninitialized);
    CHECK(call32(ctx, kSceGxm, kWaitEvent, {}) == kUninitialized);
    CHECK(call32(ctx, kSceGxm, kVertexFence, {0}) == kInvalidPointer);
    CHECK(call32(ctx, kSceGxm, kVertexFence, {params}) == 0);
}

TEST_CASE("display: vertical blank callbacks notified at each blank until unregistered; the output's resolution") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    constexpr u32 kSceDisplay = 0x5ED8F994, kSceDisplayUser = 0x4FAACD11;
    constexpr u32 kRegister = 0x6BDF4C4D, kUnregister = 0x98436A80, kWaitMultiCB = 0x3E796EF5, kResolution = 0x0BFB78C6;
    constexpr i32 kDisplayInvalidValue = static_cast<i32>(0x80290001u);
    const i32 cb = k.create_callback("vblank", 0, 0x82000001u, 0);
    REQUIRE(cb > 0);
    CHECK(call32(ctx, kSceDisplay, kRegister, {0x12345}) == kDisplayInvalidValue);  // (not a callback)
    REQUIRE(call32(ctx, kSceDisplay, kRegister, {static_cast<u32>(cb)}) == 0);
    CHECK(call32(ctx, kSceDisplay, kRegister, {static_cast<u32>(cb)}) == kDisplayInvalidValue);  // (registered)
    k.delay_thread(80'000, false);  // (about five blanks)
    CHECK(k.callback_count(cb) >= 3);
    REQUIRE(call32(ctx, kSceDisplay, kUnregister, {static_cast<u32>(cb)}) == 0);
    CHECK(call32(ctx, kSceDisplay, kUnregister, {static_cast<u32>(cb)}) == kDisplayInvalidValue);
    k.cancel_callback(cb);
    k.delay_thread(50'000, false);
    CHECK(k.callback_count(cb) == 0);
    // Two blanks, waited for with callbacks.
    const u64 before = k.clock_ns();
    CHECK(call32(ctx, kSceDisplay, kWaitMultiCB, {2}) == 0);
    CHECK(k.clock_ns() - before >= 16'000'000);

    const u32 info = scratch(28), opt = scratch(8);
    psv_st32(info, 28);
    psv_st32(opt, 28);
    REQUIRE(call32(ctx, kSceDisplayUser, kResolution, {0, info, opt}) == 0);
    CHECK(psv_ld32(info + 8) == 960);
    CHECK(psv_ld32(info + 12) == 544);
    CHECK(call32(ctx, kSceDisplayUser, kResolution, {1, info, opt}) == kDisplayInvalidValue);
}
