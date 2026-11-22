// System functions with little state of their own: memory block ranges, local time, I/O priorities, fibers' context
// attachment, renaming and options.

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string_view>
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

    // The plain initialisation without context memory too (0 and 0); a size without memory is refused.
    constexpr u32 kInit = 0xF24A298C;
    const u32 plain = scratch(128);
    CHECK(call32(ctx, kSceFiber, kInit, {plain, guest_string("task"), entry, 30, 0, 0x4000, 0, 0}) == kFiberNull);
    REQUIRE(call32(ctx, kSceFiber, kInit, {plain, guest_string("task"), entry, 30, 0, 0, 0, 0}) == 0);
    CHECK(call32(ctx, kSceFiber, kFinalize, {plain}) == 0);

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
    // An overlay over "app0:game_data/lumps" takes "app0:/game_data/lumps/..." too (the same folder), and not
    // "app0:/game_data/lumpsx".
    {
        constexpr u32 kAdd = 0x6C4BE9CD, kRemove = 0xE5D1B6F5;
        const u32 overlay = scratch(0x258), id = scratch(4);
        u8* o = t.rt.memory().host(overlay);
        std::memset(o, 0, 0x258);
        std::strcpy(reinterpret_cast<char*>(o + 0x10), "app0:game_data/lumps");        // (dst)
        std::strcpy(reinterpret_cast<char*>(o + 0x10 + 292), "data_ngp/lumps");      // (src)
        REQUIRE(call32(ctx, kSceFios2User, kAdd, {0, overlay, id}) == 0);
        const auto resolved = [&](const char* path) {
            REQUIRE(call32(ctx, kSceFios2User, kResolveSync, {0, 0, guest_string(path), out, 64}) == 0);
            return nx::string(reinterpret_cast<const char*>(t.rt.memory().host(out)));
        };
        CHECK(resolved("app0:/game_data/lumps/a.core") == "data_ngp/lumps/a.core");
        CHECK(resolved("app0:game_data/lumps/a.core") == "data_ngp/lumps/a.core");
        CHECK(resolved("app0:/game_data/lumpsx") == "app0:/game_data/lumpsx");
        u32 overlay_id = 0;
        std::memcpy(&overlay_id, t.rt.memory().host(id), 4);
        CHECK(call32(ctx, kSceFios2User, kRemove, {0, overlay_id}) == 0);
    }
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
    // (the SKU flag: a licensed, full copy; any other parameter is refused)
    psv_st32(out, 0);
    CHECK(call32(ctx, kSceAppUtil, 0xCD7FD67A /*AppParamGetInt*/, {0, out}) == 0);
    CHECK(psv_ld32(out) == 3);
    CHECK(u(call32(ctx, kSceAppUtil, 0xCD7FD67A /*AppParamGetInt*/, {1, out})) == 0x80100600u);

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
    // (none: not mounted, but the size of the resolution asked for is filled in, as games size their buffers by it)
    const u32 camera_info = scratch(64);
    std::memset(t.rt.memory().host(camera_info), 0, 64);
    psv_st32(camera_info, 64);
    psv_st16(camera_info + 8, 6);  // (SCE_CAMERA_RESOLUTION_PSP1)
    CHECK(u(call32(ctx, kSceCamera, 0xA462F801 /*Open*/, {0, camera_info})) == 0x802E0010u);
    CHECK((psv_ld16(camera_info + 12) == 480 && psv_ld16(camera_info + 14) == 272));
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

TEST_CASE("jpeg: an encoded picture decodes back to close to the colours given, as planes and as RGBA") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceJpegEncUser = 0x3748BFD3, kSceJpegUser = 0x880BF710;
    constexpr u32 kEncSize = 0x2B55844D, kEncInitParam = 0x2E8FE45D, kEncCsc = 0x824A7D4F, kEncEncode = 0xC60DE94C,
                  kEncHeader = 0x2F58B12C, kEncEnd = 0xC87AA849;
    constexpr u32 kInit = 0xB030773B, kFinish = 0x62842598, kInfo = 0x353BA9B0, kYCbCr = 0x2A769BD8, kRgba = 0x6215B095,
                  kCsc = 0xC2380E3A;
    constexpr u32 w = 40, h = 24;  // (not whole 16 x 16 MCUs)
    // Four flat areas: red, green, blue, grey.
    const u32 rgba = scratch(w * h * 4);
    u8* px = t.rt.memory().host(rgba);
    const auto colour = [](u32 x, u32 y, u32 c) -> u8 {
        static constexpr u8 k[4][3] = {{200, 30, 30}, {30, 200, 30}, {30, 30, 200}, {128, 128, 128}};
        return k[(y >= 16) * 2 + (x >= 16)][c];  // (edges on 16 x 16 MCUs: no block straddles one)
    };
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            for (u32 c = 0; c < 4; ++c) px[(y * w + x) * 4 + c] = c == 3 ? 255 : colour(x, y, c);

    const u32 ctx_size = u(call32(ctx, kSceJpegEncUser, kEncSize, {}));
    const u32 enc = scratch(ctx_size), planes = scratch(w * h * 2), out = scratch(0x10000), param = scratch(28);
    const u32 p[7] = {28, w, h, 8 /*YCbCr 4:2:0*/, out, 0x10000, 0};
    std::memcpy(t.rt.memory().host(param), p, 28);
    REQUIRE(call32(ctx, kSceJpegEncUser, kEncInitParam, {enc, param}) == 0);
    CHECK(u(call32(ctx, kSceJpegEncUser, kEncHeader, {enc, 2})) == 0x80650204u);
    REQUIRE(call32(ctx, kSceJpegEncUser, kEncCsc, {enc, planes, rgba, w, 0}) == 0);
    const i32 size = call32(ctx, kSceJpegEncUser, kEncEncode, {enc, planes});
    REQUIRE(size > 0);
    const u8* file = t.rt.memory().host(out);
    CHECK(file[0] == 0xFF);
    CHECK(file[1] == 0xD8);
    CHECK(file[size - 2] == 0xFF);
    CHECK(file[size - 1] == 0xD9);
    // (motion JPEG headers: smaller, the Huffman tables left out)
    REQUIRE(call32(ctx, kSceJpegEncUser, kEncHeader, {enc, 1}) == 0);
    const u32 mjpeg = scratch(0x10000);
    std::memcpy(t.rt.memory().host(mjpeg), file, static_cast<usize>(size));
    const i32 mjpeg_size = call32(ctx, kSceJpegEncUser, kEncEncode, {enc, planes});
    CHECK(mjpeg_size > 0);
    CHECK(mjpeg_size < size);
    CHECK(call32(ctx, kSceJpegEncUser, kEncEnd, {enc}) == 0);

    const u32 info = scratch(52);
    CHECK(u(call32(ctx, kSceJpegUser, kInfo, {mjpeg, u(size), u(-1), 0, info})) == 0x80650039u);  // (not started)
    REQUIRE(call32(ctx, kSceJpegUser, kInit, {1}) == 0);
    REQUIRE(call32(ctx, kSceJpegUser, kInfo, {mjpeg, u(size), u(-1), 0, info}) == 0);
    CHECK(psv_ld32(info) == ((2u << 16) | 0x202));
    CHECK(psv_ld32(info + 4) == (h << 16 | w));
    CHECK(psv_ld32(info + 20) == 48);  // (Y padded to 16 x 16 MCUs)
    CHECK(psv_ld32(info + 24) == 32);
    CHECK(psv_ld32(info + 28) == 24);
    CHECK(psv_ld32(info + 32) == 16);
    const u32 planes_size = psv_ld32(info + 8);
    CHECK(planes_size == 48 * 32 + 2 * 24 * 16);

    const u32 ycbcr = scratch(planes_size), decoded = scratch(48 * 32 * 4), converted = scratch(w * h * 4);
    CHECK(u(call32(ctx, kSceJpegUser, kYCbCr, {mjpeg, u(size), ycbcr, 16, 0, 0, 0})) == 0x80650041u);
    REQUIRE(u(call32(ctx, kSceJpegUser, kYCbCr, {mjpeg, u(size), ycbcr, planes_size, 0, 0, 0})) == (w << 16 | h));
    REQUIRE(u(call32(ctx, kSceJpegUser, kRgba, {mjpeg, u(size), decoded, 48 * 32 * 4, 0, 0, 0, 0, 0})) == (w << 16 | h));
    REQUIRE(call32(ctx, kSceJpegUser, kCsc, {converted, ycbcr, w << 16 | h, w, 0, 0x202}) == 0);
    // The motion JPEG frame (no Huffman tables) decodes the same.
    const u32 ycbcr2 = scratch(planes_size);
    REQUIRE(u(call32(ctx, kSceJpegUser, kYCbCr, {out, u(mjpeg_size), ycbcr2, planes_size, 1, 0, 0, 0})) == (w << 16 | h));
    CHECK(std::memcmp(t.rt.memory().host(ycbcr), t.rt.memory().host(ycbcr2), planes_size) == 0);

    // Each colour within a few steps of the original.
    u32 worst = 0;
    const u8* a = t.rt.memory().host(decoded);
    const u8* b = t.rt.memory().host(converted);
    for (u32 y : {3u, 8u, 15u, 16u, 20u, 23u})
        for (u32 x : {0u, 4u, 12u, 15u, 16u, 25u, 34u, 39u})
            for (u32 c = 0; c < 3; ++c) {
                const i32 want = colour(x, y, c);
                worst = std::max(worst, static_cast<u32>(std::abs(a[(y * 48 + x) * 4 + c] - want)));
                worst = std::max(worst, static_cast<u32>(std::abs(b[(y * w + x) * 4 + c] - want)));
            }
    CHECK(worst <= 12);
    CHECK(a[3] == 255);
    CHECK(call32(ctx, kSceJpegUser, kFinish, {}) == 0);
}

TEST_CASE("codec engine: allocations from a block are aligned, within it, apart, and the block closes when empty") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceCodecEngineUser = 0x469AB062, kOpen = 0xF0B4C892, kClose = 0x95EA3B3E, kAlloc = 0x1A3A53E5,
                  kFree = 0x6A31831D;
    const u32 block = scratch(0x10000);
    CHECK(u(call32(ctx, kSceCodecEngineUser, kOpen, {0, 0x10000})) == 0x80600000u);
    const i32 uid = call32(ctx, kSceCodecEngineUser, kOpen, {block, 0x10000});
    REQUIRE(uid > 0);
    const u32 a = u(call32(ctx, kSceCodecEngineUser, kAlloc, {u(uid), 0x100, 0x1000}));
    const u32 b = u(call32(ctx, kSceCodecEngineUser, kAlloc, {u(uid), 0x3000, 0x100}));
    CHECK(a % 0x1000 == 0);
    CHECK(b % 0x100 == 0);
    CHECK(a >= block);
    CHECK(a + 0x100 <= block + 0x10000);
    CHECK(b >= block);
    CHECK(b + 0x3000 <= block + 0x10000);
    CHECK((a + 0x100 <= b || b + 0x3000 <= a));
    CHECK(u(call32(ctx, kSceCodecEngineUser, kAlloc, {u(uid), 0x20000, 0})) == 0x80600001u);  // (too big)
    CHECK(u(call32(ctx, kSceCodecEngineUser, kAlloc, {u(uid), 0x100, 3})) == 0x80600002u);
    CHECK(u(call32(ctx, kSceCodecEngineUser, kClose, {u(uid)})) == 0x8060000Bu);  // (in use)
    CHECK(call32(ctx, kSceCodecEngineUser, kFree, {u(uid), a}) == 0);
    CHECK(u(call32(ctx, kSceCodecEngineUser, kFree, {u(uid), a})) == 0x8060000Au);
    // (the room freed is used again)
    CHECK(u(call32(ctx, kSceCodecEngineUser, kAlloc, {u(uid), 0x100, 0x1000})) == a);
    CHECK(call32(ctx, kSceCodecEngineUser, kFree, {u(uid), a}) == 0);
    CHECK(call32(ctx, kSceCodecEngineUser, kFree, {u(uid), b}) == 0);
    CHECK(call32(ctx, kSceCodecEngineUser, kClose, {u(uid)}) == 0);
    CHECK(u(call32(ctx, kSceCodecEngineUser, kClose, {u(uid)})) == 0x80600005u);
}

namespace {
nx::vector<u32> g_ime_events;
u32 g_ime_arg = 0;
void ime_handler(PsvContext* ctx, u32) {
    g_ime_arg = ctx->r[0];
    g_ime_events.push_back(psv_ld32(ctx->r[1]));
}
}  // namespace

TEST_CASE("ime: the keyboard opens, then Enter is pressed with the game's text") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceIme = 0xD2553017, kOpen = 0x0E050613, kUpdate = 0x71D6898A, kSetText = 0xF3BD9A76,
                  kSetCaret = 0xD8342D2A, kClose = 0x889A8421;
    GuestCode code;
    const u32 handler = code.add(&ime_handler);
    code.install();
    g_ime_events.clear();
    const u32 param = scratch(64), work = scratch(20 * 1024), buffer = scratch(2 * 17), initial = scratch(8), caret = scratch(16);
    std::memset(t.rt.memory().host(param), 0, 64);
    const u16 hi[] = {0x48, 0x69, 0};  // ("Hi")
    std::memcpy(t.rt.memory().host(initial), hi, sizeof hi);
    psv_st32(param + 28, work);
    psv_st32(param + 32, 0x1234);
    psv_st32(param + 36, handler);
    psv_st32(param + 44, initial);
    psv_st32(param + 48, 16);
    psv_st32(param + 52, buffer);
    CHECK(u(call32(ctx, kSceIme, kUpdate, {})) == 0x80100701u);  // (not open)
    psv_st32(param + 48, 0);
    CHECK(u(call32(ctx, kSceIme, kOpen, {param})) == 0x80100719u);
    psv_st32(param + 48, 16);
    REQUIRE(call32(ctx, kSceIme, kOpen, {param}) == 0);
    CHECK(u(call32(ctx, kSceIme, kOpen, {param})) == 0x80100700u);
    CHECK(psv_ld16(buffer) == 0x48);
    CHECK(psv_ld16(buffer + 4) == 0);
    const u16 longer[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0};  // ("Hello")
    const u32 text = scratch(sizeof longer);
    std::memcpy(t.rt.memory().host(text), longer, sizeof longer);
    CHECK(call32(ctx, kSceIme, kSetText, {text, 5}) == 0);
    CHECK(call32(ctx, kSceIme, kSetCaret, {caret}) == 0);
    for (int i = 0; i < 4; ++i) CHECK(call32(ctx, kSceIme, kUpdate, {}) == 0);
    REQUIRE(g_ime_events.size() == 2);
    CHECK(g_ime_events[0] == 0);  // (open)
    CHECK(g_ime_events[1] == 5);  // (Enter)
    CHECK(g_ime_arg == 0x1234);
    CHECK(psv_ld16(buffer + 8) == 0x6F);
    CHECK(psv_ld16(buffer + 10) == 0);
    CHECK(call32(ctx, kSceIme, kClose, {}) == 0);
    CHECK(u(call32(ctx, kSceIme, kClose, {})) == 0x80100701u);
}

TEST_CASE("voice: PCM written to an input port reaches the PCM output ports connected to it; the microphone is silent") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceVoice = 0xCE56E214, kInit = 0x805CC20F, kEnd = 0xAC98853E, kStart = 0xB2ED725B, kStop = 0xC3868DF6,
                  kCreate = 0xFA4E57B1, kConnect = 0x698BDAAE, kDisconnect = 0x5F0260F4, kDelete = 0xAE46564D,
                  kWrite = 0x0A22EC0E, kRead = 0x09E4D18C, kInfo = 0x5933CCFB, kGetAttr = 0x73F72940,
                  kSetVolume = 0xD93769E6, kGetVolume = 0x875CC80D, kGetBitRate = 0xD02C00B4;
    const u32 init = scratch(32), start = scratch(32), id = scratch(4), param = scratch(32);
    std::memset(t.rt.memory().host(init), 0, 32);
    std::memset(t.rt.memory().host(start), 0, 32);
    CHECK(u(call32(ctx, kSceVoice, kInit, {init, 99})) == 0x804E0805u);
    REQUIRE(call32(ctx, kSceVoice, kInit, {init, 100}) == 0);
    CHECK(u(call32(ctx, kSceVoice, kInit, {init, 100})) == 0x804E0802u);
    const auto create = [&](u32 type, u32 extra0, u32 extra1, u32 extra2) {
        std::memset(t.rt.memory().host(param), 0, 32);
        psv_st32(param, type);
        psv_st32(param + 8, std::bit_cast<u32>(1.0f));
        psv_st32(param + 12, extra0);
        psv_st32(param + 16, extra1);
        psv_st32(param + 20, extra2);
        CHECK(call32(ctx, kSceVoice, kCreate, {id, param}) == 0);
        return psv_ld32(id);
    };
    const u32 mic = create(0, 0, 0, 0), pcm_in = create(1, 4096, 0, 16000), pcm_out = create(3, 4096, 0, 16000),
              voice_out = create(4, 3850, 0, 0);
    CHECK(call32(ctx, kSceVoice, kConnect, {pcm_in, pcm_out}) == 0);
    CHECK(call32(ctx, kSceVoice, kConnect, {pcm_in, voice_out}) == 0);
    CHECK(call32(ctx, kSceVoice, kConnect, {mic, voice_out}) == 0);
    CHECK(u(call32(ctx, kSceVoice, kConnect, {pcm_out, pcm_in})) == 0x804E0807u);
    REQUIRE(call32(ctx, kSceVoice, kStart, {start}) == 0);

    const u32 data = scratch(2048), size = scratch(4), out = scratch(2048);
    for (u32 i = 0; i < 2048; ++i) t.rt.memory().host(data)[i] = static_cast<u8>(i * 7);
    psv_st32(size, 1100);  // (two whole frames taken)
    REQUIRE(call32(ctx, kSceVoice, kWrite, {pcm_in, data, size, 0}) == 0);
    CHECK(psv_ld32(size) == 1024);
    const u32 info = scratch(32), edges = scratch(16);
    psv_st32(info + 8, edges);
    psv_st16(info + 20, 4);
    REQUIRE(call32(ctx, kSceVoice, kInfo, {pcm_out, info}) == 0);
    CHECK(psv_ld32(info) == 3);
    CHECK(psv_ld32(info + 4) == 3);  // (running)
    CHECK(psv_ld32(info + 12) == 1024);
    CHECK(psv_ld32(info + 16) == 512);
    CHECK(psv_ld16(info + 20) == 1);
    CHECK(psv_ld32(edges) == pcm_in);
    psv_st32(size, 2048);
    REQUIRE(call32(ctx, kSceVoice, kRead, {pcm_out, out, size}) == 0);
    CHECK(psv_ld32(size) == 1024);
    CHECK(std::memcmp(t.rt.memory().host(out), t.rt.memory().host(data), 1024) == 0);
    // (nothing reaches the voice data port: no codec, and the microphone is silent)
    psv_st32(size, 2048);
    REQUIRE(call32(ctx, kSceVoice, kRead, {voice_out, out, size}) == 0);
    CHECK(psv_ld32(size) == 0);
    const u32 active = scratch(4);
    t.rt.memory().host(active)[0] = 1;
    CHECK(call32(ctx, kSceVoice, kGetAttr, {mic, 1000, active}) == 0);
    CHECK(t.rt.memory().host(active)[0] == 0);

    ctx->vfp.s[0] = std::bit_cast<u32>(0.5f);
    CHECK(call32(ctx, kSceVoice, kSetVolume, {pcm_out}) == 0);
    const u32 volume = scratch(4), bitrate = scratch(4);
    CHECK(call32(ctx, kSceVoice, kGetVolume, {pcm_out, volume}) == 0);
    CHECK(std::bit_cast<f32>(psv_ld32(volume)) == 0.5f);
    CHECK(call32(ctx, kSceVoice, kGetBitRate, {voice_out, bitrate}) == 0);
    CHECK(psv_ld32(bitrate) == 3850);
    CHECK(call32(ctx, kSceVoice, kDisconnect, {pcm_in, pcm_out}) == 0);
    CHECK(u(call32(ctx, kSceVoice, kDisconnect, {pcm_in, pcm_out})) == 0x804E0807u);
    CHECK(call32(ctx, kSceVoice, kDelete, {pcm_in}) == 0);
    CHECK(u(call32(ctx, kSceVoice, kDelete, {pcm_in})) == 0x804E0804u);
    CHECK(call32(ctx, kSceVoice, kStop, {}) == 0);
    CHECK(call32(ctx, kSceVoice, kEnd, {}) == 0);
    CHECK(u(call32(ctx, kSceVoice, kEnd, {})) == 0x804E0801u);
}

namespace {
u32 g_json_allocs = 0, g_json_frees = 0, g_json_user = 0;
void json_allocate(PsvContext* ctx, u32) {
    g_json_user = ctx->r[2];
    ctx->r[0] = scratch(ctx->r[1]);
    ++g_json_allocs;
}
void json_deallocate(PsvContext* ctx, u32) {
    if (ctx->r[1]) ++g_json_frees;
}
nx::string guest_text(u32 at) { return nx::string(reinterpret_cast<const char*>(current_runtime()->memory().host(at))); }
}  // namespace

TEST_CASE("json: parsed text reads back through values, arrays and objects, and everything allocated is freed") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kJson = 0x0854B69C;
    constexpr u32 kInitC1 = 0x5405EE04, kInitialize = 0xA3EA44E9, kTerminate = 0x30AD8C6A, kAllocC2 = 0x5EC4F6CB;
    constexpr u32 kStrC1 = 0x692125B6, kStrC1Text = 0x70703FC7, kStrD1 = 0xB70F12A8, kStrSize = 0xA15F63C1,
                  kCStr = 0x327D74CE, kCompare = 0x267A86A3;
    constexpr u32 kValC1 = 0x7A9DECC0, kValC1Int = 0x1B23D989, kValC1Obj = 0xE1DFAB01, kValD1 = 0xE07648EC,
                  kValAssign = 0x705ED2BB, kType = 0xA95EF0EC, kGetString = 0xBC3391BE, kGetArray = 0x000926DB,
                  kGetObject = 0xF793E30D, kGetInt = 0x6861C3CC, kGetUInt = 0x35EAAEDA, kGetReal = 0xFC0C699A,
                  kGetBool = 0x7DDFCAE2, kKey = 0xD5B3E509, kCount = 0xF14E07A2, kSerialize = 0xCFC4D75F;
    constexpr u32 kArrBegin = 0x82074D93, kArrEnd = 0x0F533297, kArrSize = 0x9DC07B95, kArrItNext = 0x24413A1E,
                  kArrItDeref = 0x81EF83DC, kArrItNe = 0x5467A38D;
    constexpr u32 kObjC1 = 0xB669382C, kObjD1 = 0x3CF6AF04, kObjIndex = 0x8F2E1720, kObjBegin = 0x192FE2BF,
                  kObjEnd = 0xCB50C919, kObjItNext = 0xAEC3AD59, kObjItNe = 0x7B74D109, kObjItArrow = 0x8FA78056;
    constexpr u32 kParse = 0x59B1A5B8;

    GuestCode code;
    const u32 allocate = code.add(&json_allocate), deallocate = code.add(&json_deallocate);
    code.install();
    g_json_allocs = g_json_frees = 0;
    // The game's allocator: an object whose virtual table has allocate and deallocate after the two destructors.
    const u32 vtable = scratch(20), allocator = scratch(4), init = scratch(4), param = scratch(12);
    std::memset(t.rt.memory().host(vtable), 0, 20);
    psv_st32(vtable + 8, allocate);
    psv_st32(vtable + 12, deallocate);
    call32(ctx, kJson, kAllocC2, {allocator});
    psv_st32(allocator, vtable);  // (as the derived class's constructor does)
    psv_st32(param, allocator), psv_st32(param + 4, 0x55), psv_st32(param + 8, 0);
    const u32 v = scratch(24), text = guest_string(
        "{\"name\":\"Steve\",\"n\":-5,\"big\":18446744073709551615,\"pi\":3.5,\"ok\":true,\"list\":[1,2,3],\"o\":{\"k\":\"v\\u00e9\"}}");
    const u32 text_size = static_cast<u32>(guest_text(text).size());
    call32(ctx, kJson, kValC1, {v});
    CHECK(u(call32(ctx, kJson, kParse, {v, text, text_size})) == 0x80920110u);  // (not initialized)
    call32(ctx, kJson, kInitC1, {init});
    REQUIRE(call32(ctx, kJson, kInitialize, {init, param}) == 0);
    CHECK(u(call32(ctx, kJson, kInitialize, {init, param})) == 0x80920111u);
    REQUIRE(call32(ctx, kJson, kParse, {v, text, text_size}) == 0);
    CHECK(g_json_user == 0x55);
    CHECK(call32(ctx, kJson, kType, {v}) == 7);
    CHECK(call32(ctx, kJson, kCount, {v}) == 7);
    const auto key = [&](u32 value, const char* k) { return u(call32(ctx, kJson, kKey, {value, guest_string(k)})); };
    CHECK(guest_text(u(call32(ctx, kJson, kCStr, {u(call32(ctx, kJson, kGetString, {key(v, "name")}))}))) == "Steve");
    CHECK(static_cast<i64>(psv_ld64(u(call32(ctx, kJson, kGetInt, {key(v, "n")})))) == -5);
    CHECK(call32(ctx, kJson, kType, {key(v, "big")}) == 3);
    CHECK(psv_ld64(u(call32(ctx, kJson, kGetUInt, {key(v, "big")}))) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(std::bit_cast<f64>(psv_ld64(u(call32(ctx, kJson, kGetReal, {key(v, "pi")})))) == 3.5);
    CHECK(t.rt.memory().host(u(call32(ctx, kJson, kGetBool, {key(v, "ok")})))[0] == 1);
    CHECK(call32(ctx, kJson, kType, {key(v, "missing")}) == 0);
    CHECK(psv_ld64(u(call32(ctx, kJson, kGetInt, {key(v, "name")}))) == 0);  // (not a number: the null integer)
    const u32 inner = key(key(v, "o"), "k");
    CHECK(guest_text(u(call32(ctx, kJson, kCStr, {u(call32(ctx, kJson, kGetString, {inner}))}))) == "v\xC3\xA9");

    // The array, by its iterators.
    const u32 list = u(call32(ctx, kJson, kGetArray, {key(v, "list")}));
    CHECK(call32(ctx, kJson, kArrSize, {list}) == 3);
    const u32 it = scratch(4), end = scratch(4);
    call32(ctx, kJson, kArrBegin, {it, list});
    call32(ctx, kJson, kArrEnd, {end, list});
    i64 sum = 0;
    for (u32 guard = 0; guard < 10 && call32(ctx, kJson, kArrItNe, {it, end}); ++guard) {
        sum += static_cast<i64>(psv_ld64(u(call32(ctx, kJson, kGetInt, {u(call32(ctx, kJson, kArrItDeref, {it}))}))));
        call32(ctx, kJson, kArrItNext, {it});
    }
    CHECK(sum == 6);
    // The object's keys, in order.
    const u32 object = u(call32(ctx, kJson, kGetObject, {v}));
    call32(ctx, kJson, kObjBegin, {it, object});
    call32(ctx, kJson, kObjEnd, {end, object});
    nx::string keys;
    for (u32 guard = 0; guard < 10 && call32(ctx, kJson, kObjItNe, {it, end}); ++guard) {
        const u32 pair = u(call32(ctx, kJson, kObjItArrow, {it}));
        keys += guest_text(u(call32(ctx, kJson, kCStr, {pair}))) + ",";
        call32(ctx, kJson, kObjItNext, {it});
    }
    CHECK(keys == "name,n,big,pi,ok,list,o,");

    // Built: an object holding x = 7, as a value, written out.
    const u32 obj = scratch(4), name = scratch(4), seven = scratch(24), built = scratch(24), out = scratch(4);
    call32(ctx, kJson, kObjC1, {obj});
    call32(ctx, kJson, kStrC1Text, {name, guest_string("x")});
    CHECK(call32(ctx, kJson, kStrSize, {name}) == 1);
    CHECK(call32(ctx, kJson, kCompare, {name, guest_string("x")}) == 0);
    CHECK(call32(ctx, kJson, kCompare, {name, guest_string("y")}) < 0);
    call32(ctx, kJson, kValC1Int, {seven, 0, 7, 0});
    call32(ctx, kJson, kValAssign, {u(call32(ctx, kJson, kObjIndex, {obj, name})), seven});
    call32(ctx, kJson, kValC1Obj, {built, obj});
    call32(ctx, kJson, kStrC1, {out});
    REQUIRE(call32(ctx, kJson, kSerialize, {built, out}) == 0);
    CHECK(guest_text(u(call32(ctx, kJson, kCStr, {out}))) == "{\"x\":7}");
    // (a parse of what was written gives it again)
    REQUIRE(call32(ctx, kJson, kSerialize, {v, out}) == 0);
    const nx::string written = guest_text(u(call32(ctx, kJson, kCStr, {out})));
    const u32 again = scratch(24), out2 = scratch(4);
    call32(ctx, kJson, kValC1, {again});
    REQUIRE(call32(ctx, kJson, kParse, {again, guest_string(written.c_str()), static_cast<u32>(written.size())}) == 0);
    call32(ctx, kJson, kStrC1, {out2});
    REQUIRE(call32(ctx, kJson, kSerialize, {again, out2}) == 0);
    CHECK(guest_text(u(call32(ctx, kJson, kCStr, {out2}))) == written);
    CHECK(u(call32(ctx, kJson, kParse, {again, guest_string("{\"a\":}"), 6})) == 0x80920101u);
    CHECK(call32(ctx, kJson, kType, {again}) == 0);

    for (u32 x : {v, seven, built, again}) call32(ctx, kJson, kValD1, {x});
    call32(ctx, kJson, kObjD1, {obj});
    for (u32 s : {name, out, out2}) call32(ctx, kJson, kStrD1, {s});
    CHECK(g_json_allocs > 20);
    CHECK(g_json_frees == g_json_allocs);
    CHECK(call32(ctx, kJson, kTerminate, {init}) == 0);
}

TEST_CASE("NID-only imports: SceMp4 answers as no MP4 reader, the kernel's debug output and SDK version, libc's parameters") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceMp4 = 0x8FEACABD;
    CHECK(call32(ctx, kSceMp4, 0x05474AF0 /*open*/, {guest_string("app0:movie.mp4"), 0, 0, 0}) == 0);
    for (u32 nid : {0x30E49E4Du, 0x8BE0E3D3u, 0xD5B26179u, 0x609E57ADu, 0x20A01112u, 0x920623C8u, 0xA7C09746u})
        CHECK(u(call32(ctx, kSceMp4, nid, {0, 0, 0, 0})) == 0x80020004u);
    call32(ctx, kSceLibKernel, 0x023EAA62, {guest_string("stdin : open failed")});
    CHECK(call32(ctx, kSceLibKernel, 0x9F793F84, {}) == 0);  // (no process parameters in a test)
    u32 at = 0;
    REQUIRE(hle::find_variable(0x5AD9C136, "SceLibcParam", 0xDF084DFA, &at) != nullptr);
    REQUIRE(at != 0);
    CHECK(psv_ld32(at) == 0);  // (an empty block: the libc's defaults)
}

TEST_CASE("rtc: leap years, month lengths, days of the year, valid dates, DOS times, RFC 2822 out and in") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceRtcUser = 0x6704D3C6;
    CHECK(call32(ctx, kSceRtcUser, 0xF17FD8B5 /*IsLeapYear*/, {2000}) == 1);
    CHECK(call32(ctx, kSceRtcUser, 0xF17FD8B5, {1900}) == 0);
    CHECK(call32(ctx, kSceRtcUser, 0x49EB4556 /*GetDaysInMonth*/, {2024, 2}) == 29);
    CHECK(call32(ctx, kSceRtcUser, 0xB5C4E95F /*GetDayOfYear*/, {2023, 3, 1}) == 60);
    const u32 date = scratch(16), dos = scratch(4), back = scratch(16), tick = scratch(8), text = scratch(64);
    const auto set_date = [&](u32 at, u16 y, u16 mo, u16 d, u16 h, u16 mi, u16 s) {
        const u16 v[6] = {y, mo, d, h, mi, s};
        std::memcpy(t.rt.memory().host(at), v, 12);
        psv_st32(at + 12, 0);
    };
    set_date(date, 2014, 2, 29, 10, 0, 0);
    CHECK(u(call32(ctx, kSceRtcUser, 0xD7622935 /*CheckValid*/, {date})) == 0x80251083u);  // (no 29 February in 2014)
    set_date(date, 2014, 1, 1, 9, 30, 58);
    CHECK(call32(ctx, kSceRtcUser, 0xD7622935, {date}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x92ABEBAF /*GetDosTime*/, {date, dos}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0xF8B22B07 /*SetDosTime*/, {back, psv_ld32(dos)}) == 0);
    CHECK(std::memcmp(t.rt.memory().host(date), t.rt.memory().host(back), 12) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x2347CE12 /*ParseDateTime*/, {tick, guest_string("Wed, 01 Jan 2014 09:00:00 +0900")}) == 0);
    REQUIRE(call32(ctx, kSceRtcUser, 0x147F2138 /*FormatRFC2822*/, {text, tick, 0}) == 0);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(text))) == "Wed, 01 Jan 2014 00:00:00 +0000");
    CHECK(u(call32(ctx, kSceRtcUser, 0x2347CE12, {tick, guest_string("not a date")})) == 0x80251080u);
}

TEST_CASE("motion: the angle threshold and the magnetometer's state are kept") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceMotion = 0xDC571B3F;
    ctx->vfp.s[0] = std::bit_cast<u32>(0.25f);
    CHECK(call32(ctx, kSceMotion, 0xDACB2A41 /*SetAngleThreshold*/, {}) == 0);
    ctx->vfp.s[0] = 0;
    call32(ctx, kSceMotion, 0x499B6C87 /*GetAngleThreshold*/, {});
    CHECK(std::bit_cast<f32>(ctx->vfp.s[0]) == 0.25f);
    CHECK(call32(ctx, kSceMotion, 0x122A79F8 /*MagnetometerOn*/, {}) == 0);
    CHECK(call32(ctx, kSceMotion, 0x3D4813AE /*GetMagnetometerState*/, {}) == 1);
    CHECK(call32(ctx, kSceMotion, 0xC1A7395A /*MagnetometerOff*/, {}) == 0);
    CHECK(call32(ctx, kSceMotion, 0x3D4813AE, {}) == 0);
}

TEST_CASE("jpeg: a picture decoded by the split decoder, its data given in two parts") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceJpegEncUser = 0x3748BFD3, kSceJpegUser = 0x880BF710;
    // A picture to decode: the encoder's (64 x 32, grey).
    const u32 enc = scratch(0x100), planes = scratch(64 * 32 * 2), file = scratch(0x8000);
    std::memset(t.rt.memory().host(planes), 128, 64 * 32 * 2);
    // (the hardware colour conversion's pitch flag (16) taken with a format, as WipEout 2048 gives it: 4:2:2 + 16)
    REQUIRE(call32(ctx, kSceJpegEncUser, 0x88DA92B4 /*Init*/, {enc, 64, 32, 9 + 16, file, 0x8000}) == 0);
    CHECK(u(call32(ctx, kSceJpegEncUser, 0x88DA92B4, {enc, 64, 32, 2, file, 0x8000})) == 0x80650203u);
    REQUIRE(call32(ctx, kSceJpegEncUser, 0x88DA92B4 /*Init*/, {enc, 64, 32, 8, file, 0x8000}) == 0);
    const i32 size = call32(ctx, kSceJpegEncUser, 0xC60DE94C /*Encode*/, {enc, planes});
    REQUIRE(size > 200);
    CHECK(u(call32(ctx, kSceJpegUser, 0xB030773B /*InitMJpeg*/, {0})) == 0);  // (no split decoders: allowed)
    CHECK(u(call32(ctx, kSceJpegUser, 0x123B4734 /*CreateSplitDecoder*/, {scratch(0x60)})) == 0x80650062u);
    CHECK(call32(ctx, kSceJpegUser, 0x62842598 /*FinishMJpeg*/, {}) == 0);
    REQUIRE(call32(ctx, kSceJpegUser, 0xB030773B, {1}) == 0);

    const u32 ctrl = scratch(0x60), stream = scratch(128), out = scratch(64 * 32 * 2);
    std::memset(t.rt.memory().host(ctrl), 0, 0x60);
    REQUIRE(call32(ctx, kSceJpegUser, 0x123B4734, {ctrl}) == 0);
    psv_st32(ctrl, stream), psv_st32(ctrl + 4, 128), psv_st32(ctrl + 20, 0);
    const u8* jpeg = t.rt.memory().host(file);
    u32 fed = 128;
    std::memcpy(t.rt.memory().host(stream), jpeg, 128);
    i32 r = 0;
    bool init_done = false;
    for (u32 guard = 0; guard < 64; ++guard) {
        r = call32(ctx, kSceJpegUser, 0x4598EC9C /*SplitDecodeMJpeg*/, {ctrl});
        if (u(r) == 0x80650063u) {  // (INIT_DONE: the header read)
            init_done = true;
            CHECK(psv_ld32(ctrl + 24 + 4) == (32u << 16 | 64u));
            psv_st32(ctrl + 76, out);
        } else if (u(r) == 0x80650064u) {  // (INPUT_SUSPENDED: the next part)
            const u32 n = nx::min<u32>(psv_ld32(ctrl + 12), static_cast<u32>(size) - fed);
            std::memcpy(t.rt.memory().host(psv_ld32(ctrl + 8)), jpeg + fed, n);
            fed += n;
            psv_st32(ctrl + 12, n);
            psv_st32(ctrl + 16, fed == static_cast<u32>(size) ? 1 : 0);
        } else {
            break;
        }
    }
    CHECK(init_done);
    CHECK(u(r) == (64u << 16 | 32u));
    CHECK(std::abs(static_cast<i32>(t.rt.memory().host(out)[100]) - 128) <= 2);
    CHECK(call32(ctx, kSceJpegUser, 0xDE8D5FA1 /*DeleteSplitDecoder*/, {ctrl}) == 0);
}

TEST_CASE("system gesture: the primitive touch events themselves, by count and as an array") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kSceSystemGesture = 0xAF3FE2D3;
    REQUIRE(call32(ctx, kSceSystemGesture, 0x6078A08B /*InitializePrimitiveTouchRecognizer*/, {0}) == 0);
    const u32 front = scratch(144), events = scratch(96 * 4), count = scratch(4);
    std::memset(t.rt.memory().host(front), 0, 144);
    psv_st32(front, 1000), psv_st32(front + 12, 1);  // (one touch: id 7 at 100, 200)
    t.rt.memory().host(front + 16)[0] = 7;
    psv_st16(front + 18, 100), psv_st16(front + 20, 200);
    REQUIRE(call32(ctx, kSceSystemGesture, 0xDF4C665A /*UpdatePrimitiveTouchRecognizer*/, {front, 0}) == 0);
    CHECK(call32(ctx, kSceSystemGesture, 0xBAB8ECCB /*GetPrimitiveTouchEventsCount*/, {}) == 1);
    REQUIRE(call32(ctx, kSceSystemGesture, 0xC750D3DA /*GetPrimitiveTouchEvents*/, {events, 4, count}) == 0);
    CHECK(psv_ld32(count) == 1);
    CHECK(psv_ld16(events + 4) == 7);
    CHECK(psv_ld16(events + 12) == 100);
    CHECK(psv_ld16(events + 14) == 200);
}

TEST_CASE("perf, Razor HUD, dbg, debug LEDs: markers nest, the time base is the kernel's clock, counters read 0") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kScePerf = 0x447F047D, kSceRazorHud = 0x250371C2, kSceDbg = 0x623DF986, kSceDebugLed = 0xAE004C0A;
    constexpr i32 kPerfInvalid = static_cast<i32>(0x80580000u);
    // CPU markers: pushed and popped (zones in Tracy, nested; one popped too many is nothing); no profiler here.
    constexpr u32 kPush = 0xC3DE4C0A, kPushHud = 0x5CDC09EA, kPop = 0xDC3224C3, kIsCapturing = 0x8DA87B35;
    CHECK(call32(ctx, kScePerf, kPush, {guest_string("frame")}) == 0);
    CHECK(call32(ctx, kScePerf, kPushHud, {guest_string("update"), 0x8000ff00u, 0}) == 0);
    CHECK(call32(ctx, kScePerf, kPushHud, {0, 0x800000ffu, 1}) == 0);  // (no label)
    for (int i = 0; i < 4; ++i) CHECK(call32(ctx, kScePerf, kPop, {}) == 0);
    CHECK(call32(ctx, kScePerf, 0xDAB63FC3 /*StartCapture*/, {}) == 0);
    CHECK(call32(ctx, kScePerf, kIsCapturing, {}) == 0);
    CHECK(call32(ctx, kScePerf, 0x9C2C05C2 /*StopCapture*/, {}) == 0);
    CHECK(call32(ctx, kScePerf, 0x4F1385E3 /*Sync*/, {}) == 0);
    // The fibers' markers, the same.
    CHECK(call32(ctx, kSceFiber, 0x9C414B09 /*PushUserMarkerWithHud*/, {guest_string("job"), 0x80ffffffu, 0}) == 0);
    CHECK(call32(ctx, kSceFiber, 0x4EC01F9C /*PopUserMarkerWithHud*/, {}) == 0);

    // The time base: the kernel's clock in nanoseconds, 1000 ticks a microsecond.
    constexpr u32 kTimebase = 0xBD9615E5, kFrequency = 0x78EA4FFB;
    CHECK(call32(ctx, kScePerf, kFrequency, {}) == 1000);
    const u64 a = call(ctx, kScePerf, kTimebase, {});
    t.rt.kernel().delay_thread(2000, false);
    const u64 b = call(ctx, kScePerf, kTimebase, {});
    CHECK(b - a >= 2'000'000);
    CHECK(b <= t.rt.kernel().clock_ns());

    // The ARM performance monitor: accepted, every counter 0; a counter that does not exist refused.
    constexpr u32 kReset = 0x35151735, kSelect = 0x63CBEA8B, kStart = 0xC9D969D5, kStop = 0xD1A40F54,
                  kGet = 0x6132A497, kSet = 0x12F6C708, kIncrement = 0x4264B4E7;
    const u32 value = scratch(4);
    psv_st32(value, 77);
    CHECK(call32(ctx, kScePerf, kReset, {0}) == 0);
    CHECK(call32(ctx, kScePerf, kSelect, {0, 5, 0x03}) == 0);
    CHECK(call32(ctx, kScePerf, kSelect, {0, 6, 0x03}) == kPerfInvalid);
    CHECK(call32(ctx, kScePerf, kStart, {0}) == 0);
    CHECK(call32(ctx, kScePerf, kIncrement, {0x3F}) == 0);
    CHECK(call32(ctx, kScePerf, kStop, {0}) == 0);
    CHECK(call32(ctx, kScePerf, kGet, {0, 31, value}) == 0);
    CHECK(psv_ld32(value) == 0);
    CHECK(call32(ctx, kScePerf, kGet, {0, 7, value}) == kPerfInvalid);
    CHECK(call32(ctx, kScePerf, kSet, {0, 2, 100}) == 0);

    // Razor's trace buffers: taken, and read back empty.
    constexpr u32 kStartTrace = 0xAED36A40, kStopTrace = 0x4B47E144, kGetTrace = 0x0DE231D4;
    const u32 buffer = scratch(4096), out = scratch(4);
    CHECK(call32(ctx, kScePerf, kStartTrace, {0, 4096}) == kPerfInvalid);
    CHECK(call32(ctx, kScePerf, kStartTrace, {buffer, 4096}) == 0);
    CHECK(call32(ctx, kScePerf, kGetTrace, {out}) == 0);
    CHECK(psv_ld32(out) == buffer);
    CHECK(call32(ctx, kScePerf, kStopTrace, {}) == 0);

    // The HUD: no performance mode; the rest accepted.
    CHECK(call32(ctx, kSceRazorHud, 0xE3709B44 /*sceRazorGpuPerfGetMode*/, {}) == 1);
    CHECK(call32(ctx, kSceRazorHud, 0x77F2F606 /*sceRazorHudSetDisplayEnabled*/, {1}) == 0);
    CHECK(call32(ctx, kSceRazorHud, 0xB46382FC /*sceRazorGpuLiveStart*/, {}) == 0);

    // libdbg: the game's messages to the log, formatted with their arguments (file, line, level or stop, component,
    // format, then the format's arguments on the stack).
    constexpr u32 kLog = 0x6605AB19, kAssert = 0x1AF3678B, kMinimum = 0x941622FA;
    const u32 file = guest_string("main.c"), component = guest_string("test");
    CHECK(call32(ctx, kSceDbg, kMinimum, {2}) == 0);
    CHECK(call32(ctx, kSceDbg, kLog, {file, 12, 3, component, guest_string("%d frames, %s"), 30, guest_string("ok")}) == 0);
    CHECK(call32(ctx, kSceDbg, kLog, {file, 13, 0, component, guest_string("dropped below the minimum")}) == 0);
    CHECK(call32(ctx, kSceDbg, kAssert, {file, 14, 0, component, guest_string("x == %d"), 4}) == 0);
    CHECK(call32(ctx, kSceDbg, 0xED4A00BA /*SetBreakOnErrorState*/, {1}) == 0);
    CHECK(call32(ctx, kSceDbg, 0x3DEAECD3 /*SetBreakOnWarningState*/, {1}) == 0);
    CHECK(call32(ctx, kSceDbg, kMinimum, {0}) == 0);

    // The development kit's LEDs: set (and logged when they change).
    CHECK(call32(ctx, kSceDebugLed, 0x78E702D3 /*sceKernelSetGPO*/, {0x05}) == 0);
    CHECK(call32(ctx, kSceDebugLed, 0x78E702D3, {0x05}) == 0);
}
