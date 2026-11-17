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

TEST_CASE("appmgr, screenshots, debug switches, fios overlays: what has nothing to do on the host answers so") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceAppMgrUser = 0xA6605D6F, kSceDebugLed = 0xAE004C0A, kSceScreenShot = 0xF26FC97D,
                  kSceFios2User = 0xC2ED82D7, kSceFios2Kernel = 0x8757B742;
    // Another executable in place of this one: refused (one executable a game); its path checked first.
    constexpr u32 kLoadExec = 0xE6774ABC, kDisconnectionWarning = 0xE352B29C;
    CHECK(u(call32(ctx, kSceAppMgrUser, kLoadExec, {0, 0, 0})) == 0x80802016u);
    CHECK(u(call32(ctx, kSceAppMgrUser, kLoadExec, {guest_string("ux0:data/a.self"), 0, 0})) == 0x8080201Eu);
    CHECK(u(call32(ctx, kSceAppMgrUser, kLoadExec, {guest_string("app0:sub/a.self"), 0, 0})) == 0x8080201Eu);
    CHECK(u(call32(ctx, kSceAppMgrUser, kLoadExec, {guest_string("app0:other.self"), 0, 0})) == 0x80802013u);
    CHECK(call32(ctx, kSceAppMgrUser, kDisconnectionWarning, {1}) == 0);
    CHECK(call32(ctx, kSceDebugLed, 0x14F582CF /*sceKernelGetGPI*/, {}) == 0);

    constexpr u32 kDisable = 0x50AE9FF9, kEnable = 0x76E674D1, kDisableNotification = 0xDA80A228,
                  kEnableNotification = 0x816633FE, kCapture = 0xD509FB08, kGetParam = 0xBAE9E534;
    const u32 file_info = scratch(1024), info = scratch(1024);
    CHECK(call32(ctx, kSceScreenShot, kDisableNotification, {}) == 0);
    CHECK(call32(ctx, kSceScreenShot, kEnableNotification, {}) == 0);
    CHECK(u(call32(ctx, kSceScreenShot, kCapture, {0, file_info, 0, 0})) == 0x80102F0Bu);  // (no photo storage)
    CHECK(call32(ctx, kSceScreenShot, kDisable, {}) == 0);
    CHECK(u(call32(ctx, kSceScreenShot, kCapture, {0, file_info, 0, 0})) == 0x80102F07u);  // (disallowed)
    CHECK(u(call32(ctx, kSceScreenShot, kCapture, {1, file_info, 0, 0})) == 0x80102F0Bu);  // (forced)
    CHECK(call32(ctx, kSceScreenShot, kEnable, {}) == 0);
    CHECK(u(call32(ctx, kSceScreenShot, kCapture, {2, file_info, 0, 0})) == 0x80102F01u);
    CHECK(u(call32(ctx, kSceScreenShot, kGetParam, {guest_string("ux0:picture/a.jpg"), info})) == 0x80102F03u);

    // FIOS overlays: none, so a path resolves to itself; overlays off for this thread, and on again.
    constexpr u32 kResolveSync = 0x9C3AAAF0, kSetDisabled = 0x70321220, kIsDisabled = 0x23B8DB1D;
    const u32 out = scratch(64);
    REQUIRE(call32(ctx, kSceFios2User, kResolveSync, {0, 0, guest_string("app0:data/x.bin"), out, 64}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(out))) == "app0:data/x.bin");
    CHECK(call32(ctx, kSceFios2User, kIsDisabled, {}) == 0);
    CHECK(call32(ctx, kSceFios2User, kSetDisabled, {1}) == 0);
    CHECK(call32(ctx, kSceFios2User, kIsDisabled, {}) == 1);
    CHECK(call32(ctx, kSceFios2User, kSetDisabled, {0}) == 0);
    // (the overlay directory handles: refused as overlays are)
    for (u32 nid : {0x5D6A1CCEu, 0x2F06ADC6u, 0x021B4AF7u, 0x759EBEE6u, 0xF6A3E335u, 0x2A9724C9u})
        CHECK(call32(ctx, kSceFios2Kernel, nid, {0, 0, 0, 0}) == err::kUnsupported);
}

TEST_CASE("clib: heaps grow and shrink blocks, zero them, report their use; text to numbers; character classes") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kCreate = 0x3B9E301A, kMalloc = 0x86EF7680, kFree = 0x9C56B4D1, kCalloc = 0x678374AD,
                  kRealloc = 0x774891D6, kEmpty = 0xD1D59701, kStats = 0x8CC1D38E, kUsable = 0x46A02279;
    const u32 region = scratch(0x10000);
    std::memset(t.rt.memory().host(region), 0xEE, 0x10000);
    const u32 heap = u(call32(ctx, kSceLibKernel, kCreate, {region, 0x10000}));
    REQUIRE(heap != 0);
    CHECK(call32(ctx, kSceLibKernel, kEmpty, {heap}) == 1);
    const u32 z = u(call32(ctx, kSceLibKernel, kCalloc, {heap, 10, 10}));
    REQUIRE(z != 0);
    CHECK(t.rt.memory().host(z)[99] == 0);
    CHECK(call32(ctx, kSceLibKernel, kUsable, {z}) == 104);  // (rounded up to 8)
    CHECK(call32(ctx, kSceLibKernel, kEmpty, {heap}) == 0);
    std::memcpy(t.rt.memory().host(z), "keep me", 8);
    const u32 big = u(call32(ctx, kSceLibKernel, kRealloc, {heap, z, 4000}));
    REQUIRE(big != 0);
    CHECK(std::memcmp(t.rt.memory().host(big), "keep me", 8) == 0);
    CHECK(u(call32(ctx, kSceLibKernel, kRealloc, {heap, big, 16})) == big);  // (smaller: in place)
    const u32 stats = scratch(16);
    call32(ctx, kSceLibKernel, kStats, {heap, stats});
    CHECK(psv_ld32(stats) == 0x10000);
    CHECK(psv_ld32(stats + 8) >= 4000);  // (the most in use)
    CHECK(psv_ld32(stats + 12) == 4000);
    CHECK(call32(ctx, kSceLibKernel, kRealloc, {heap, big, 0}) == 0);  // (size 0: freed)
    CHECK(call32(ctx, kSceLibKernel, kEmpty, {heap}) == 1);
    const u32 fresh = u(call32(ctx, kSceLibKernel, kRealloc, {heap, 0, 32}));  // (null: a new block)
    CHECK(fresh != 0);
    call32(ctx, kSceLibKernel, kFree, {heap, fresh});
    CHECK(call32(ctx, kSceLibKernel, kMalloc, {heap, 0x20000}) == 0);

    constexpr u32 kStrtoll = 0x2E581B88, kCtype = 0xD8EBBB7E;
    const u32 end = scratch(4);
    const u32 text = guest_string("  -0x1fz");
    CHECK(static_cast<i64>(call(ctx, kSceLibKernel, kStrtoll, {text, end, 0})) == -31);
    CHECK(psv_ld32(end) == text + 7);
    CHECK(static_cast<i64>(call(ctx, kSceLibKernel, kStrtoll, {guest_string("9223372036854775808"), 0, 10})) ==
          0x7FFFFFFFFFFFFFFFll);  // (out of range: the limit)
    CHECK(static_cast<i64>(call(ctx, kSceLibKernel, kStrtoll, {guest_string("0777"), 0, 0})) == 511);
    CHECK(static_cast<i64>(call(ctx, kSceLibKernel, kStrtoll, {guest_string("zz"), end, 36})) == 35 * 36 + 35);
    CHECK(call32(ctx, kSceLibKernel, kCtype, {'A'}) == (0x01 | 0x40));
    CHECK(call32(ctx, kSceLibKernel, kCtype, {'q'}) == 0x02);
    CHECK(call32(ctx, kSceLibKernel, kCtype, {'7'}) == (0x04 | 0x40));
    CHECK(call32(ctx, kSceLibKernel, kCtype, {' '}) == (0x08 | 0x80));
    CHECK(call32(ctx, kSceLibKernel, kCtype, {'!'}) == 0x10);
    CHECK(call32(ctx, kSceLibKernel, kCtype, {'\n'}) == (0x08 | 0x20));

    // Random bytes (at most 64); a backtrace of nothing.
    const u32 random = scratch(64);
    std::memset(t.rt.memory().host(random), 0, 64);
    REQUIRE(call32(ctx, 0xF9AC7CF8 /*SceLibRng*/, 0xB2700165, {random, 64}) == 0);
    u32 nonzero = 0;
    for (u32 i = 0; i < 64; ++i) nonzero += t.rt.memory().host(random)[i] != 0;
    CHECK(nonzero > 32);
    CHECK(call32(ctx, 0xF9AC7CF8, 0xB2700165, {random, 65}) == err::kInvalidArgument);
    const u32 count = scratch(4);
    psv_st32(count, 9);
    CHECK(call32(ctx, kSceLibKernel, 0xD16C03B0 /*sceKernelBacktraceSelf*/, {scratch(64), 64, count, 0}) == 0);
    CHECK(psv_ld32(count) == 0);
    // Modules: one that is not there is not found; stopping and unloading are not supported.
    CHECK(u(call32(ctx, kSceLibKernel, 0xBBE82155 /*sceKernelLoadModule*/, {guest_string("app0:none.suprx"), 0, 0})) ==
          0x8002D01Cu);
    CHECK(call32(ctx, kSceLibKernel, 0x702425D5 /*sceKernelStartModule*/, {0x1234, 0, 0, 0, 0, 0}) < 0);
    CHECK(call32(ctx, kSceLibKernel, 0x3B2CBA09 /*sceKernelStopModule*/, {0x1234, 0, 0, 0, 0, 0}) == err::kUnsupported);
    CHECK(call32(ctx, kSceLibKernel, 0x1987920E /*sceKernelUnloadModule*/, {0x1234, 0, 0}) == err::kUnsupported);
}

TEST_CASE("rtc: ticks compared and added, days of the week, C and Win32 times, RFC 2822 and 3339 written and read") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceRtcUser = 0x6704D3C6;
    const auto tick_at = [&](u64 v) {
        const u32 at = scratch(8);
        std::memcpy(t.rt.memory().host(at), &v, 8);
        return at;
    };
    const auto tick = [&](u32 at) {
        u64 v;
        std::memcpy(&v, t.rt.memory().host(at), 8);
        return v;
    };
    // 2024-03-01 12:00:00 UTC as a tick (µs since 0001-01-01).
    const u32 date = scratch(16), parsed = scratch(8);
    REQUIRE(call32(ctx, kSceRtcUser, 0x2D18AEEC /*ParseRFC3339*/, {parsed, guest_string("2024-03-01T12:00:00Z")}) == 0);
    const u64 noon = tick(parsed);
    REQUIRE(call32(ctx, kSceRtcUser, 0x2D18AEEC, {parsed, guest_string("2024-03-01T21:00:00.5+09:00")}) == 0);
    CHECK(tick(parsed) == noon + 500'000);
    CHECK(u(call32(ctx, kSceRtcUser, 0x2D18AEEC, {parsed, guest_string("yesterday")})) == 0x80251080u);
    CHECK(call32(ctx, kSceRtcUser, 0xC7385158 /*CompareTick*/, {tick_at(noon), tick_at(noon + 1)}) == -1);
    CHECK(call32(ctx, kSceRtcUser, 0xC7385158, {tick_at(noon), tick_at(noon)}) == 0);
    CHECK(call32(ctx, kSceRtcUser, 0x2F3531EB /*GetDayOfWeek*/, {2024, 3, 1}) == 5);  // (a Friday)
    CHECK(call32(ctx, kSceRtcUser, 0x2F3531EB, {1970, 1, 1}) == 4);
    const u32 out = scratch(8);
    REQUIRE(call32(ctx, kSceRtcUser, 0x979AFD79 /*TickAddSeconds*/, {out, tick_at(noon), 90, 0}) == 0);
    CHECK(tick(out) == noon + 90'000'000ull);
    REQUIRE(call32(ctx, kSceRtcUser, 0xAE26D920 /*TickAddMicroseconds*/, {out, tick_at(noon), 7, 0}) == 0);
    CHECK(tick(out) == noon + 7);
    // A month on from 31 January is the end of February.
    REQUIRE(call32(ctx, kSceRtcUser, 0x2D18AEEC, {parsed, guest_string("2023-01-31T00:00:00Z")}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x6321B4AA /*TickAddMonths*/, {out, parsed, 1}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x2D18AEEC, {parsed, guest_string("2023-02-28T00:00:00Z")}) == 0);
    CHECK(tick(out) == tick(parsed));
    // time_t and back; Win32 file time.
    REQUIRE(call32(ctx, kSceRtcUser, 0x3A332F81 /*SetTime_t*/, {date, 0}) == 0);
    const u32 seconds = scratch(4);
    REQUIRE(call32(ctx, kSceRtcUser, 0x8DE6FEB7 /*GetTime_t*/, {date, seconds}) == 0);
    CHECK(psv_ld32(seconds) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x8A95E119 /*GetWin32FileTime*/, {date, out}) == 0);
    CHECK(tick(out) == 116444736000000000ull);  // (1970-01-01 in FILETIME units)
    // Text: RFC 3339 in a time zone, and back.
    const u32 text = scratch(64);
    REQUIRE(call32(ctx, kSceRtcUser, 0xCCEA2B54 /*FormatRFC3339*/, {text, tick_at(noon), static_cast<u32>(-300)}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(text))) == "2024-03-01T07:00:00.00-05:00");
    REQUIRE(call32(ctx, kSceRtcUser, 0x2D18AEEC, {parsed, text}) == 0);
    CHECK(tick(parsed) == noon);
    REQUIRE(call32(ctx, kSceRtcUser, 0x42CA8EB5 /*FormatRFC2822LocalTime*/, {text, tick_at(noon)}) == 0);
    CHECK(nx::string(reinterpret_cast<const char*>(t.rt.memory().host(text))).find(" 2024 ") != nx::string::npos);
    // Local time to UTC is the inverse of UTC to local.
    REQUIRE(call32(ctx, kSceRtcUser, 0x1282C436 /*ConvertUtcToLocalTime*/, {tick_at(noon), out}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x0A05E201 /*ConvertLocalTimeToUtc*/, {out, parsed}) == 0);
    CHECK(tick(parsed) == noon);
    CHECK(call(ctx, 0x3503487E /*SceRtc*/, 0x258BE8EC /*GetAccumulativeTime*/, {}) > 0);
}

TEST_CASE("apputil, power, LiveArea, game update, photo export: the host's answers") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceAppUtil = 0x48E01D74, kScePower = 0x1082DA7F, kSceLiveAreaUtil = 0x5303CB07,
                  kSceGameUpdate = 0x2DD9A62D, kScePhotoExport = 0x79166BD9;
    const u32 event = scratch(1028), out = scratch(1024);
    CHECK(u(call32(ctx, kSceAppUtil, 0xA2496814, {event, out})) == 0x80100601u);  // (not initialized)
    const u32 p = scratch(104);
    std::memset(t.rt.memory().host(p), 0, 104);
    REQUIRE(call32(ctx, kSceAppUtil, 0xDAFFE671 /*sceAppUtilInit*/, {p, p + 64}) == 0);
    for (u32 nid : {0xA2496814u, 0x6BED9B58u, 0x28C7D4F6u, 0x77380601u, 0x0F4EE55Fu, 0xA58E6FCEu})
        CHECK(u(call32(ctx, kSceAppUtil, nid, {event, out})) == 0x80100620u);  // (never such an event)
    CHECK(call32(ctx, kSceAppUtil, 0x96F478D6 /*BgdlGetStatus*/, {out}) == 0);
    CHECK(u(call32(ctx, kSceAppUtil, 0x53B2C020 /*AddcontMount*/, {1, out})) == 0x80100660u);
    CHECK(u(call32(ctx, kSceAppUtil, 0x1B36AF8C /*AddcontUmount*/, {})) == 0x80100604u);
    CHECK(u(call32(ctx, kSceAppUtil, 0xB5EDCBFF /*MusicMount*/, {})) == 0x80100685u);
    CHECK(u(call32(ctx, kSceAppUtil, 0xA7FE1BF7 /*MusicUmount*/, {})) == 0x80100604u);
    CHECK(u(call32(ctx, kSceAppUtil, 0xEE85804D /*PhotoMount*/, {})) == 0x80100680u);
    CHECK(u(call32(ctx, kSceAppUtil, 0x9651B941 /*PhotoUmount*/, {})) == 0x80100604u);
    CHECK(u(call32(ctx, kSceAppUtil, 0xF19D0423 /*LaunchWebBrowser*/, {out})) == 0x80100605u);
    CHECK(u(call32(ctx, kSceAppUtil, 0x85FA94EE /*StoreBrowse*/, {out})) == 0x80100605u);

    CHECK(call32(ctx, kScePower, 0xD3075926 /*IsLowBattery*/, {}) == 0);
    CHECK(call32(ctx, kScePower, 0x87440F5E /*IsPowerOnline*/, {}) == 1);
    CHECK(call32(ctx, kScePower, 0x2085D15D /*GetBatteryLifePercent*/, {}) == 100);
    for (u32 nid : {0x1E490401u, 0x78A1A796u, 0x04B7766Eu, 0xDFA8BAF8u}) CHECK(call32(ctx, kScePower, nid, {1}) == 0);
    for (u32 nid : {0x8EFB3FA2u, 0xFD18A0FFu, 0x94F5A53Fu}) CHECK(call32(ctx, kScePower, nid, {}) > 0);

    const u32 xml = guest_string("<frame/>");
    CHECK(call32(ctx, kSceLiveAreaUtil, 0xCFCEDE95 /*UpdateFrameSync*/, {guest_string("01.00"), xml, 8, 0, 0}) == 0);
    CHECK(call32(ctx, kSceLiveAreaUtil, 0xD330285D /*UpdateFrameAsync*/, {guest_string("01.00"), xml, 8, 0, 0}) == 0);
    CHECK(call32(ctx, kSceLiveAreaUtil, 0x29C65C0B /*ReplaceAllSync*/, {guest_string("app0:la")}) == 0);
    CHECK(call32(ctx, kSceLiveAreaUtil, 0x7FE5B83F /*GetStatus*/, {}) == 0);
    CHECK(u(call32(ctx, kSceLiveAreaUtil, 0xD6EE2214 /*GetFrameRevision*/, {guest_string("f"), out})) == 0x8010400Au);
    CHECK(u(call32(ctx, kSceLiveAreaUtil, 0xD6FD367F /*GetFrameUserData*/, {guest_string("f"), out, 16})) == 0x8010400Au);

    const u32 result = scratch(16);
    psv_st32(result, 16);
    psv_st32(result + 4, 1);
    REQUIRE(call32(ctx, kSceGameUpdate, 0x2F401247 /*Init*/, {0}) == 0);
    REQUIRE(call32(ctx, kSceGameUpdate, 0x3C616238 /*Run*/, {result}) == 0);
    CHECK(psv_ld32(result + 4) == 0);  // (no patch)
    CHECK(call32(ctx, kSceGameUpdate, 0x48D879BA /*Term*/, {}) == 0);
    CHECK(u(call32(ctx, kSceGameUpdate, 0x3C616238, {result})) == 0x80412801u);

    CHECK(u(call32(ctx, kScePhotoExport, 0x70512321 /*FromData*/, {out, 16, out, out, 0, 0, out, 64})) == 0x80101A0Cu);
    CHECK(u(call32(ctx, kScePhotoExport, 0x70512321, {0, 16, out, out, 0, 0, out, 64})) == 0x80101A01u);
}

TEST_CASE("audio in, motion sensors, cameras, video decoder memory: silence at the grain's pace, a still console") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceAudioIn = 0xF8DC61A3, kOpen = 0x39B50DC1, kRelease = 0x3A61B8C4, kInput = 0x638ADD2D,
                  kAdopt = 0x566AC433, kStatus = 0x2F940377;
    CHECK(u(call32(ctx, kSceAudioIn, kOpen, {1, 256, 16000, 0})) == 0x80260104u);  // (no such port type)
    CHECK(u(call32(ctx, kSceAudioIn, kOpen, {0, 300, 16000, 0})) == 0x80260102u);
    CHECK(u(call32(ctx, kSceAudioIn, kOpen, {0, 256, 44100, 0})) == 0x80260103u);
    const i32 port = call32(ctx, kSceAudioIn, kOpen, {0, 256, 16000, 0});
    REQUIRE(port >= 0);
    CHECK(u(call32(ctx, kSceAudioIn, kOpen, {0, 512, 16000, 0})) == 0x80260107u);  // (one voice port)
    const u32 buf = scratch(512);
    std::memset(t.rt.memory().host(buf), 0x55, 512);
    const u64 before = t.rt.kernel().clock_ns();
    for (u32 i = 0; i < 5; ++i) REQUIRE(call32(ctx, kSceAudioIn, kInput, {u(port), buf}) == 0);
    CHECK(t.rt.kernel().clock_ns() - before >= 4 * 16'000'000ull);  // (256 samples at 16 kHz: 16 ms each)
    CHECK(t.rt.memory().host(buf)[511] == 0);
    CHECK(call32(ctx, kSceAudioIn, kAdopt, {0}) == 1);
    CHECK(call32(ctx, kSceAudioIn, kStatus, {1}) == 0);
    CHECK(call32(ctx, kSceAudioIn, kRelease, {u(port)}) == 0);
    CHECK(u(call32(ctx, kSceAudioIn, kInput, {u(port), buf})) == 0x80260109u);

    constexpr u32 kSceMotion = 0xDC571B3F, kStart = 0x28034AC9, kSensor = 0x47D679EA, kMag = 0x122A79F8;
    const u32 states = scratch(64 * 3);
    CHECK(u(call32(ctx, kSceMotion, kSensor, {states, 3})) == 0x80360202u);  // (not sampling)
    REQUIRE(call32(ctx, kSceMotion, kStart, {}) == 0);
    REQUIRE(call32(ctx, kSceMotion, kSensor, {states, 3}) == 0);
    f32 z;
    std::memcpy(&z, t.rt.memory().host(states + 64 * 2 + 8), 4);
    CHECK(z == -1.0f);  // (gravity down the back)
    CHECK(psv_ld32(states + 64 * 2 + 40) == 2);
    CHECK(call32(ctx, kSceMotion, kMag, {}) == 0);

    constexpr u32 kSceCamera = 0xDA91B3ED;
    CHECK(u(call32(ctx, kSceCamera, 0x62AFF0B8 /*SetEV*/, {0, 1})) == 0x802E0004u);  // (not open)
    CHECK(u(call32(ctx, kSceCamera, 0xE9D2CFB1 /*SetEffect*/, {0, 1})) == 0x802E0004u);

    constexpr u32 kSceVideodecUser = 0x163C3727;
    const u32 info = scratch(64), mem = scratch(8), ctrl = scratch(32);
    psv_st32(mem, 0x1234);
    REQUIRE(call32(ctx, kSceVideodecUser, 0x6731BB2D /*QueryMemSize*/, {0x1001, info, mem}) == 0);
    CHECK(psv_ld32(mem) == 0);
    CHECK(call32(ctx, kSceVideodecUser, 0x230A78FF /*InitLibraryWithUnmapMem*/, {0x1001, ctrl, info}) == 0);
    CHECK(call32(ctx, kSceVideodecUser, 0x3A5F4924 /*TermLibrary*/, {0x1001}) == 0);
}
