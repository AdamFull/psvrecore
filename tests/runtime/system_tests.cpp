// System functions with little state of their own: memory block ranges, local time, I/O priorities, fibers' context
// attachment, renaming and options.

#include <cstring>
#include <ctime>

#include "nxtest.h"
#include "runtime_support.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

u32 u(i32 v) { return static_cast<u32>(v); }

constexpr u32 kSceFiber = 0xF88DAA70;

// A fiber's entry (a guest function): renames its fiber, then returns to the thread with its two arguments' sum.
constexpr u32 kRenameSelf = 0xDD50DF3D, kReturnToThread = 0x3B42921F;
u32 g_fiber_name = 0;
void fiber_entry(PsvContext* ctx, u32) {
    const u32 sum = ctx->r[0] + ctx->r[1];
    call32(ctx, kSceFiber, kRenameSelf, {g_fiber_name});
    call32(ctx, kSceFiber, kReturnToThread, {sum, 0});
    psv_trap(ctx, 0, PSV_TRAP_RETURNED);  // (never resumed)
}

}  // namespace

TEST_CASE("sysmem: a range's memory block, when the range lies within one") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kAlloc = 0xB9D5EBDE, kBase = 0xB8EF5818, kInfoByRange = 0x006F3DB4, kUserRw = 0x0C20D060;
    const i32 uid = call32(ctx, kSceSysmem, kAlloc, {guest_string("block"), kUserRw, 0x10000, 0});
    REQUIRE(uid > 0);
    const u32 base_out = scratch(4), info = scratch(24);
    REQUIRE(call32(ctx, kSceSysmem, kBase, {u(uid), base_out}) == 0);
    const u32 base = psv_ld32(base_out);
    psv_st32(info, 24);
    REQUIRE(call32(ctx, kSceSysmem, kInfoByRange, {base + 0x100, 0x1000, info}) == 0);
    CHECK(psv_ld32(info + 4) == base);         // (mapped base)
    CHECK(psv_ld32(info + 8) == 0x10000);      // (mapped size)
    CHECK(psv_ld32(info + 20) == kUserRw);     // (type)
    CHECK(call32(ctx, kSceSysmem, kInfoByRange, {base + 0xF000, 0x2000, info}) == err::kUnknownUid);  // (past its end)
    CHECK(call32(ctx, kSceSysmem, kInfoByRange, {base, 0x1000, 0}) == err::kIllegalAddress);
}

TEST_CASE("processmgr: mktime turns a local time into seconds and normalizes it") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceProcessmgr = 0x2DD91812, kMktime = 0x890BDC39, kLocaltime = 0x94F041ED;
    // 2024-02-30 25:00 local: normalized to 2024-03-02 01:00.
    const u32 tm = scratch(36);
    const i32 in[9] = {0, 0, 25, 30, 1, 2024, 0, 0, 0};
    std::memcpy(t.rt.memory().host(tm), in, sizeof in);
    const u32 seconds = u(call32(ctx, kSceProcessmgr, kMktime, {tm}));
    REQUIRE(seconds != ~0u);
    i32 out[9];
    std::memcpy(out, t.rt.memory().host(tm), sizeof out);
    CHECK(out[2] == 1);     // hour
    CHECK(out[3] == 2);     // day
    CHECK(out[4] == 2);     // March
    CHECK(out[5] == 2024);  // (the full year)
    CHECK(out[6] == 6);     // a Saturday
    CHECK(out[7] == 61);    // day of the year
    // Back through localtime_r: the same time.
    const u32 t_at = scratch(4), back = scratch(36);
    psv_st32(t_at, seconds);
    REQUIRE(u(call32(ctx, kSceProcessmgr, kLocaltime, {t_at, back})) == back);
    CHECK(std::memcmp(t.rt.memory().host(back), t.rt.memory().host(tm), 24) == 0);
    CHECK(u(call32(ctx, kSceProcessmgr, kMktime, {0})) == ~0u);
}

TEST_CASE("iofilemgr: I/O priorities of files, of the thread and of the process") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceProcessmgr = 0x2DD91812, kGetStdout = 0xE5AA625C;
    constexpr u32 kSet = 0x14B2D56C, kGet = 0xF2A472A1, kSetProcess = 0x7F2ABBAF, kGetProcess = 0x0DC4F1BB,
                  kSetThread = 0x49312108, kGetThread = 0xA176CD03;
    const i32 fd = call32(ctx, kSceProcessmgr, kGetStdout, {});
    REQUIRE(fd > 0);
    CHECK(call32(ctx, kSceIofilemgr, kGetProcess, {}) == 14);
    CHECK(call32(ctx, kSceIofilemgr, kGetThread, {}) == 0);  // (none: the process's)
    CHECK(call32(ctx, kSceIofilemgr, kGet, {u(fd)}) == 14);
    CHECK(call32(ctx, kSceIofilemgr, kSetProcess, {9}) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kGet, {u(fd)}) == 9);
    CHECK(call32(ctx, kSceIofilemgr, kSetThread, {5}) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kGetThread, {}) == 5);
    CHECK(call32(ctx, kSceIofilemgr, kGet, {u(fd)}) == 5);
    CHECK(call32(ctx, kSceIofilemgr, kSet, {u(fd), 2}) == 0);
    CHECK(call32(ctx, kSceIofilemgr, kGet, {u(fd)}) == 2);
    CHECK(call32(ctx, kSceIofilemgr, kSet, {u(fd), 16}) == err::kInvalidArgument);
    CHECK(call32(ctx, kSceIofilemgr, kSetProcess, {0}) == err::kInvalidArgument);
    CHECK(call32(ctx, kSceIofilemgr, kSet, {0x7777, 3}) == err::kBadFile);
    CHECK(call32(ctx, kSceIofilemgr, kGet, {0x7777}) == err::kBadFile);
}

TEST_CASE("fiber: options zeroed; a fiber without context memory given one and run; it renames itself") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kOptInit = 0x801AB334, kInitInternal = 0xC6A3F9BB, kAttachAndRun = 0x7D0C7DDB, kGetInfo = 0x189599B4,
                  kFinalize = 0xE160F844, kStartCheck = 0xCD543A71, kStopCheck = 0x67F70867, kPush = 0x9C414B09,
                  kPop = 0x4EC01F9C;
    constexpr i32 kFiberNull = static_cast<i32>(0x80590001u), kFiberPermission = static_cast<i32>(0x80590005u),
                  kFiberState = static_cast<i32>(0x80590006u);
    GuestCode code;
    const u32 entry = code.add(&fiber_entry);
    code.install();
    g_fiber_name = guest_string("renamed");

    const u32 opt = scratch(128);
    std::memset(t.rt.memory().host(opt), 0xAB, 128);
    CHECK(call32(ctx, kSceFiber, kOptInit, {opt}) == 0);
    CHECK(t.rt.memory().host(opt)[127] == 0);
    CHECK(call32(ctx, kSceFiber, kOptInit, {0}) == kFiberNull);

    const u32 fiber = scratch(128), context = scratch(0x4000), back = scratch(4), info = scratch(128);
    REQUIRE(call32(ctx, kSceFiber, kInitInternal, {fiber, guest_string("worker"), entry, 30, 0, 0, 0, 2, 0}) == 0);
    REQUIRE(call32(ctx, kSceFiber, kAttachAndRun, {fiber, context, 0x4000, 12, back}) == 0);
    CHECK(psv_ld32(back) == 42);
    REQUIRE(call32(ctx, kSceFiber, kGetInfo, {fiber, info}) == 0);
    CHECK(psv_ld32(info + 8) == context);
    CHECK(psv_ld32(info + 12) == 0x4000);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(info + 16))) == "renamed");
    CHECK(call32(ctx, kSceFiber, kAttachAndRun, {fiber, context, 0x4000, 0, back}) == kFiberState);  // (started)
    CHECK(call32(ctx, kSceFiber, kRenameSelf, {g_fiber_name}) == kFiberPermission);  // (from a thread)
    CHECK(call32(ctx, kSceFiber, kFinalize, {fiber}) == 0);

    // (the context's use is not measured; markers are accepted)
    CHECK(call32(ctx, kSceFiber, kStartCheck, {0}) == 0);
    CHECK(call32(ctx, kSceFiber, kStopCheck, {}) == 0);
    CHECK(call32(ctx, kSceFiber, kPush, {guest_string("mark"), 0, 0}) == 0);
    CHECK(call32(ctx, kSceFiber, kPop, {}) == 0);
}
