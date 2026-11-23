// SceGxm's queries on programs, parameters and fragment programs, and the calls that need no device: on small
// GXP images in guest memory, through the guest calling convention.

#include <cstring>

#include "nxtest.h"
#include "psvrecore/gxm/hle.hpp"
#include "runtime_support.hpp"
#include "state.hpp"

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

TEST_CASE("display: a thread polling the vertical blank count waits for the next blank instead of spinning") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    constexpr u32 kSceDisplay = 0x5ED8F994, kGetVcount = 0xB6FDE0BA;
    // (as WipEout 2048's display queue callback waits: the count read until it changes, three blanks in a row)
    for (int blank = 0; blank < 3; ++blank) {
        const u32 from = static_cast<u32>(call32(ctx, kSceDisplay, kGetVcount, {}));
        u32 calls = 1, v = from;
        while (v == from && calls < 100'000) v = static_cast<u32>(call32(ctx, kSceDisplay, kGetVcount, {})), ++calls;
        CHECK(v == ((from + 1) & 0xFFFF));  // (the next count: none skipped)
        CHECK(calls <= 200);                // (a wait, not millions of reads)
    }
    // (reads further apart than a blank change each time, and never wait)
    const u64 before = k.clock_ns();
    u32 last = static_cast<u32>(call32(ctx, kSceDisplay, kGetVcount, {}));
    for (int i = 0; i < 3; ++i) {
        k.delay_thread(20'000, false);
        const u32 v = static_cast<u32>(call32(ctx, kSceDisplay, kGetVcount, {}));
        CHECK(v != last);
        last = v;
    }
    CHECK(k.clock_ns() - before < 200'000'000);
}

TEST_CASE("display: waiting for a frame buffer counts the blanks from the one it was set in, not from the call") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    Kernel& k = t.rt.kernel();
    constexpr u32 kSceDisplay = 0x5ED8F994, kSceDisplayUser = 0x4FAACD11, kGetVcount = 0xB6FDE0BA,
                  kWaitVblank = 0x5795E898, kWaitSetMulti = 0x7D9864A8, kSetFrameBuf = 0x7A410B64;
    const u32 pixels = (scratch(64 * 64 * 4 + 256) + 255) & ~255u;
    const u32 fb = scratch(24);
    const u32 words[6] = {24, pixels, 64, 0, 64, 64};  // (size, base, pitch, A8B8G8R8, width, height)
    std::memcpy(t.rt.memory().host(fb), words, sizeof words);
    auto vcount = [&] { return static_cast<u32>(call32(ctx, kSceDisplay, kGetVcount, {})); };
    // (as Lumines' display queue callback paces itself to 30 frames a second: wait two, then set the next frame for
    // the next blank: each frame shown for two blanks)
    REQUIRE(call32(ctx, kSceDisplay, kWaitVblank, {}) == 0);
    REQUIRE(call32(ctx, kSceDisplayUser, kSetFrameBuf, {fb, 1}) == 0);
    const u32 set = vcount();
    k.delay_thread(4'000, false);  // (some of the period gone before the wait, as the callback's own work takes)
    REQUIRE(call32(ctx, kSceDisplay, kWaitSetMulti, {2}) == 0);
    CHECK(vcount() == ((set + 2) & 0xFFFF));
    // (again, with no new frame: the next blank, not at once)
    REQUIRE(call32(ctx, kSceDisplay, kWaitSetMulti, {2}) == 0);
    CHECK(vcount() == ((set + 3) & 0xFFFF));
    // (a frame set long enough ago: no wait)
    REQUIRE(call32(ctx, kSceDisplayUser, kSetFrameBuf, {fb, 1}) == 0);
    REQUIRE(call32(ctx, kSceDisplay, kWaitVblank, {}) == 0);
    REQUIRE(call32(ctx, kSceDisplay, kWaitVblank, {}) == 0);
    const u64 before = k.clock_ns();
    REQUIRE(call32(ctx, kSceDisplay, kWaitSetMulti, {2}) == 0);
    CHECK(k.clock_ns() - before < 8'000'000);
}

TEST_CASE("gxm: user markers only within a scene or a command list; Razor's capture triggers without RenderDoc") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    constexpr u32 kPush = 0x3276C475, kPop = 0x4FA073A6, kSet = 0xC7A8CB77;
    const u32 tag = guest_string("shadows");
    // (no context: refused; a context that is not one: refused too)
    CHECK(call32(ctx, kSceGxm, kPush, {0, tag}) == kInvalidPointer);
    CHECK(call32(ctx, kSceGxm, kPop, {0}) == kInvalidPointer);
    CHECK(call32(ctx, kSceGxm, kSet, {0x81000000u, tag}) == kInvalidPointer);
    CHECK(call32(ctx, kSceGxm, kPush, {0x81000000u, 0}) == kInvalidPointer);
    // (a context outside a scene, and the markers replayed in one: an SDK-built program, docs/PROFILING.md)

    CHECK(call32(ctx, kSceGxm, 0x873B07C0 /*sceGxmIsDebugVersion*/, {}) == 0);
    const u32 buffer = scratch(4096);
    CHECK(call32(ctx, kSceGxm, 0xE9E81073 /*AddRazorGpuCaptureBuffer*/, {buffer, 4096}) == 0);
    CHECK(call32(ctx, kSceGxm, 0x0468E3F2 /*RemoveRazorGpuCaptureBuffer*/, {buffer}) == 0);

    // Razor's GPU capture: no RenderDoc in a test, so nothing is in progress after a trigger.
    constexpr u32 kSceRazorCapture = 0x7B2A3F18;
    const u32 file = guest_string("app0:capture.sgx");
    call(ctx, kSceRazorCapture, 0x9DAA7E9C /*SetTriggerNextFrame*/, {file});
    call(ctx, kSceRazorCapture, 0x472492DA /*SetTrigger*/, {10, file});
    call(ctx, kSceRazorCapture, 0xE916B538 /*sceRazorCaptureSetTrigger*/, {10, file});
    CHECK(call32(ctx, kSceRazorCapture, 0xD2358BFB /*IsInProgress*/, {}) == 0);
    CHECK(call32(ctx, kSceRazorCapture, 0x911E0AA0 /*sceRazorCaptureIsInProgress*/, {}) == 0);
    CHECK(call32(ctx, kSceRazorCapture, 0x3F0913CF /*StartSalvageMode*/, {}) == 0);
}

// sceGxmBeginSceneEx with a load surface and a store surface at different addresses (Killzone: Mercenary's depth
// reductions load one 240x136 depth image and store the next): the scene's depth is the one loaded, and what is
// drawn is stored at the store surface's addresses, not dropped.
TEST_CASE("gxm: a scene that loads depth from one surface and stores it to another stores to the second") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    gxm::Options options;
    gxm::register_libraries(options);
    PsvContext* ctx = t.ctx();
    constexpr u32 kInitialize = 0xB0F1E4EC, kCreateTarget = 0x207AF96B, kCreateContext = 0xE84CE5B4,
                  kBeginSceneEx = 0x4709CF5A;
    using namespace gxm::hle;

    const u32 init = scratch(sizeof(InitializeParams));
    const InitializeParams ip{0, 2, 0, 0, 0};
    std::memcpy(current_runtime()->memory().host(init), &ip, sizeof ip);
    REQUIRE(call32(ctx, kSceGxm, kInitialize, {init}) == 0);

    const u32 tp = scratch(sizeof(RenderTargetParams)), target_out = scratch(4);
    const RenderTargetParams rp{0, 240, 136, 1, 0, 0, 0};
    std::memcpy(current_runtime()->memory().host(tp), &rp, sizeof rp);
    REQUIRE(call32(ctx, kSceGxm, kCreateTarget, {tp, target_out}) == 0);
    const u32 target = psv_ld32(target_out);

    const u32 cp = scratch(sizeof(ContextParams)), context_out = scratch(4), host_mem = scratch(4096);
    const ContextParams cparams{host_mem, 4096, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(current_runtime()->memory().host(cp), &cparams, sizeof cparams);
    REQUIRE(call32(ctx, kSceGxm, kCreateContext, {cp, context_out}) == 0);
    const u32 context = psv_ld32(context_out);

    constexpr u32 kFormat = 0x00026000;  // (D24S8, as Killzone's: only the format bits must match)
    auto surface = [&](u32 control, u32 depth) {
        const u32 at = scratch(sizeof(DepthStencilSurface));
        const DepthStencilSurface s{control | kFormat, depth, 0, 1.0f, 0};
        std::memcpy(current_runtime()->memory().host(at), &s, sizeof s);
        return at;
    };
    const u32 load = surface(kDsEnabled | kDsTiled | kDsForceLoad, 0x65480000 + 0x28000);
    const u32 store = surface(kDsEnabled | kDsTiled | kDsForceStore, 0x65480000);

    auto scene_of = [&]() -> const Scene& { return state().contexts.at(context)->scene; };
    REQUIRE(call32(ctx, kSceGxm, kBeginSceneEx, {context, 0, target, 0, 0, 0, 0, load, store}) == 0);
    CHECK(scene_of().has_depth);
    CHECK((scene_of().depth.control & kDsForceLoad) != 0);
    CHECK(scene_of().depth.depth_data == 0x65480000u + 0x28000);  // (loaded from the load surface)
    CHECK(scene_of().store_elsewhere);
    CHECK(scene_of().store_depth_data == 0x65480000u);  // (stored to the store surface)

    // (the same surface to load and to store: the scene stores in place)
    const u32 both = surface(kDsEnabled | kDsTiled | kDsForceLoad | kDsForceStore, 0x65480000);
    state().contexts.at(context)->in_scene = false;  // (no renderer here to end the first scene with)
    REQUIRE(call32(ctx, kSceGxm, kBeginSceneEx, {context, 0, target, 0, 0, 0, 0, both, both}) == 0);
    CHECK(!scene_of().store_elsewhere);
    CHECK((scene_of().depth.control & kDsForceStore) != 0);

    state().contexts.at(context)->in_scene = false;
    CHECK(call32(ctx, kSceGxm, 0xEDDC5FB2 /*sceGxmDestroyContext*/, {context}) == 0);
    CHECK(call32(ctx, kSceGxm, 0x0B94C50A /*sceGxmDestroyRenderTarget*/, {target}) == 0);
    CHECK(call32(ctx, kSceGxm, 0xB627DE66 /*sceGxmTerminate*/, {}) == 0);
}
