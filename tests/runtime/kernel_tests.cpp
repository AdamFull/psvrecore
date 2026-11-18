// The kernel through its system library functions, called as recompiled code calls them: thread life cycle,
// scheduling (priorities, CPUs, no time slicing), synchronisation objects, delays, callbacks.
//
// Expected behaviour is the one the SDK's kernel documentation specifies (paraphrased in each test). Scheduling
// tests use one CPU where the order of events is then fully determined.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "nxtest.h"
#include "runtime_support.hpp"
#include "core/foundation/threading/thread.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

extern "C" {
extern PsvImport test_exit_import;
extern volatile int test_returned_after_exit;
void test_exit_entry(PsvContext* ctx, uint32_t ret);
}

namespace {

// NIDs (library SceLibKernel unless noted).
constexpr u32 kCreateThread = 0xC5C11EE7, kStartThread = 0xF08DE149, kWaitThreadEnd = 0xDDB395A9,
              kGetThreadId = 0x0FB972F9, kGetThreadExitStatus = 0xD5DC26C4, kGetThreadInfo = 0x8D9C5461,
              kGetTlsAddr = 0xB295EB61, kCreateSema = 0x1BD67366, kWaitSema = 0x0C7B834B, kGetSemaInfo = 0x595D3FA6,
              kCreateEventFlag = 0x8516D040, kWaitEventFlag = 0x83C0E2AF, kPollEventFlag = 0x1FBB0FE1,
              kCancelEventFlag = 0x2A12D9B7, kCreateMutex = 0xED53334A, kLockMutex = 0x1D8D7945, kGetMutexInfo = 0x9A6C43CA,
              kCreateCond = 0x50572FDA, kWaitCond = 0xC88D44AD, kCreateLwMutex = 0xDA6EC8EF, kLockLwMutex = 0x46E7BE7B,
              kUnlockLwMutex = 0x91FA6614, kTryLockLwMutex = 0xA6A2C915, kCreateLwCond = 0x48C7EAE6, kWaitLwCond = 0xE1878282,
              kSignalLwCond = 0x3AC63B9A, kProcessTimeWide = 0xB110C123;
// SceThreadmgr
constexpr u32 kDeleteThread = 0x1BBDE3D9, kDelayThread = 0x4B675D05, kDelayThreadCB = 0x9C0180E1, kDeleteSema = 0xDB32948A,
              kPollSema = 0x866EF048, kSignalSema = 0xE6B761D1, kSetEventFlag = 0xEC94DFF7, kClearEventFlag = 0x4CB87CA7,
              kUnlockMutex = 0x1A372EC8, kTryLockMutex = 0x72FC1F54, kSignalCond = 0x6ED2E2DC, kCreateCallback = 0xB19CF7E9,
              kNotifyCallback = 0xA4683592, kCheckCallback = 0xE53E41F6;
constexpr u32 kExitThread = 0x0C8A38E1;  // SceThreadmgrCoredumpTime

constexpr u32 kDefault = 0x10000100;  // SCE_KERNEL_DEFAULT_PRIORITY_USER
constexpr u32 kCpu0 = 0x10000;

u32 u(i32 v) { return static_cast<u32>(v); }

i32 create_thread(PsvContext* ctx, const char* name, u32 entry, u32 priority, u32 affinity = 0, u32 stack = 0x4000) {
    return call32(ctx, kSceLibKernel, kCreateThread, {guest_string(name), entry, priority, stack, 0, affinity, 0});
}
i32 start_thread(PsvContext* ctx, i32 uid, u32 size = 0, u32 argp = 0) {
    return call32(ctx, kSceLibKernel, kStartThread, {u(uid), size, argp});
}
i32 wait_end(PsvContext* ctx, i32 uid, i32* status = nullptr) {
    const u32 at = scratch(4);
    const i32 r = call32(ctx, kSceLibKernel, kWaitThreadEnd, {u(uid), at, 0});
    if (status) *status = static_cast<i32>(psv_ld32(at));
    return r;
}
i32 delay(PsvContext* ctx, u32 usec) { return call32(ctx, kSceThreadmgr, kDelayThread, {usec}); }

void spin_ms(u32 ms) {
    const u64 end = nx::os::ticks_ns() + u64{ms} * 1'000'000;
    while (nx::os::ticks_ns() < end) {
    }
}

Log g_log;
std::atomic<u32> g_value{0};
i32 g_object = 0;

}  // namespace

TEST_CASE("kernel: a thread starts with its argument block on its stack and ends with an exit status") {
    TestRuntime t;
    REQUIRE(t.rt.ok());
    GuestCode code;
    static u32 seen_size, seen_word, seen_sp_ok;
    const u32 entry = code.add([](PsvContext* ctx, uint32_t) {
        seen_size = ctx->r[0];
        seen_word = psv_ld32(ctx->r[1]);
        seen_sp_ok = (ctx->r[13] & 7) == 0 && ctx->r[1] >= ctx->r[13];
        ctx->r[0] = 42;
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const u32 block = scratch(8);
    psv_st32(block, 0xC0FFEE11u);
    const i32 uid = create_thread(ctx, "worker", entry, kDefault);
    REQUIRE(uid > 0);
    CHECK(start_thread(ctx, uid, 4, block) == 0);
    i32 status = 0;
    CHECK(wait_end(ctx, uid, &status) == 0);
    CHECK(status == 42);
    CHECK(seen_size == 4);
    CHECK(seen_word == 0xC0FFEE11u);
    CHECK(seen_sp_ok == 1);
    const u32 at = scratch(4);
    CHECK(call32(ctx, kSceLibKernel, kGetThreadExitStatus, {u(uid), at}) == 0);
    CHECK(psv_ld32(at) == 42);
    // A dormant thread can be started again, then deleted.
    CHECK(start_thread(ctx, uid, 4, block) == 0);
    CHECK(wait_end(ctx, uid) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteThread, {u(uid)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteThread, {u(uid)}) == err::kUnknownThreadId);
}

TEST_CASE("kernel: create checks priorities, stack sizes and affinity masks") {
    TestRuntime t;
    GuestCode code;
    const u32 entry = code.add([](PsvContext*, uint32_t) {});
    code.install();
    PsvContext* ctx = t.ctx();
    CHECK(create_thread(ctx, "p", entry, 63) == err::kIllegalPriority);
    CHECK(create_thread(ctx, "p", entry, 192) == err::kIllegalPriority);
    CHECK(create_thread(ctx, "p", entry, kDefault + 32) == err::kIllegalPriority);
    CHECK(create_thread(ctx, "p", entry, 64) > 0);
    CHECK(create_thread(ctx, "p", entry, kDefault - 32) > 0);
    CHECK(create_thread(ctx, "p", entry, 100, 0, 0x800) == err::kIllegalStackSize);
    CHECK(create_thread(ctx, "p", entry, 100, 0x80000) == err::kIllegalCpuAffinityMask);
    // The default priority is the game's (160) +/- an offset.
    const i32 uid = create_thread(ctx, "p", entry, kDefault + 5);
    REQUIRE(uid > 0);
    const u32 info = scratch(128);
    psv_st32(info, 128);
    CHECK(call32(ctx, kSceLibKernel, kGetThreadInfo, {u(uid), info}) == 0);
    CHECK(psv_ld32(info + 60) == 165);   // initPriority
    CHECK(psv_ld32(info + 44) == 0x10);  // status: DORMANT
    CHECK(std::strcmp(reinterpret_cast<const char*>(t.rt.memory().host(info + 8)), "p") == 0);
}

TEST_CASE("kernel: a higher-priority thread takes the CPU when started; a lower one waits") {
    TestRuntime t(1);  // one CPU: the order is fully determined
    g_log.events.clear();
    GuestCode code;
    const u32 high = code.add([](PsvContext*, uint32_t) { g_log.add("high"); });
    const u32 low = code.add([](PsvContext*, uint32_t) { g_log.add("low"); });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 a = create_thread(ctx, "high", high, 100);
    const i32 b = create_thread(ctx, "low", low, 180);
    start_thread(ctx, a);  // runs at once: the main thread (160) loses the CPU at the end of this call
    g_log.add("main");
    start_thread(ctx, b);  // waits until the main thread waits
    g_log.add("main2");
    wait_end(ctx, b);
    wait_end(ctx, a);
    CHECK(g_log.joined() == "high main main2 low");
}

TEST_CASE("kernel: no time slicing among threads of equal priority") {
    TestRuntime t(1, 100);
    g_log.events.clear();
    GuestCode code;
    // Each runs 30 ms without a kernel call: on a time-slicing scheduler the second would start before the first
    // ends.
    const u32 w1 = code.add([](PsvContext*, uint32_t) {
        g_log.add("w1+");
        spin_ms(30);
        g_log.add("w1-");
    });
    const u32 w2 = code.add([](PsvContext*, uint32_t) {
        g_log.add("w2+");
        spin_ms(30);
        g_log.add("w2-");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 a = create_thread(ctx, "w1", w1, 150), b = create_thread(ctx, "w2", w2, 150);
    start_thread(ctx, a);
    start_thread(ctx, b);
    wait_end(ctx, a);
    wait_end(ctx, b);
    CHECK(g_log.joined() == "w1+ w1- w2+ w2-");
}

TEST_CASE("kernel: a thread lending its CPU for host work takes it back ahead of a thread of its priority") {
    TestRuntime t(1, 100);
    g_log.events.clear();
    g_value = 0;
    GuestCode code;
    // w1 lends its CPU for 30 ms of host work: w2 (the same priority) runs meanwhile, and gives the CPU back at its
    // next kernel call once w1 takes it back, though threads of equal priority do not preempt each other otherwise.
    const u32 w1 = code.add([](PsvContext*, uint32_t) {
        g_log.add("w1+");
        Kernel& k = runtime::runtime().kernel();
        CHECK(k.lend_cpu());
        spin_ms(30);
        k.reclaim_cpu();
        g_log.add("w1 back");
        const u32 before = g_value.load();
        spin_ms(20);
        CHECK(g_value.load() - before <= 1);  // (w2 stopped at its next call)
        g_log.add("w1-");
    });
    const u32 w2 = code.add([](PsvContext* ctx, uint32_t) {
        g_log.add("w2+");
        for (int i = 0; i < 100; ++i) {
            spin_ms(1);
            g_value.fetch_add(1);
            call32(ctx, kSceLibKernel, kGetThreadId, {});
        }
        g_log.add("w2-");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 a = create_thread(ctx, "w1", w1, 150), b = create_thread(ctx, "w2", w2, 150);
    start_thread(ctx, a);
    start_thread(ctx, b);
    wait_end(ctx, a);
    wait_end(ctx, b);
    CHECK(g_value.load() == 100);
    CHECK(g_log.joined() == "w1+ w2+ w1 back w1- w2-");
}

TEST_CASE("kernel: a thread preempted while it runs that waits before its next kernel call gets no CPU while it waits") {
    // Two CPUs: the test's thread keeps one. w1 lends the other to w2 and takes it back while w2 runs (w2 then is
    // preempted, ready again, until its next kernel call); w2's next call waits on a semaphore. When w1 ends, its CPU
    // must stay free: w2 waits, and a CPU given to it would be held by a thread asleep (the game froze so).
    TestRuntime t(2, 100);
    g_log.events.clear();
    GuestCode code;
    const u32 w1 = code.add([](PsvContext*, uint32_t) {
        Kernel& k = runtime::runtime().kernel();
        k.lend_cpu();
        spin_ms(20);
        k.reclaim_cpu();
        spin_ms(60);  // (w2 waits meanwhile)
        g_log.add("w1-");
    });
    const u32 w2 = code.add([](PsvContext* ctx, uint32_t) {
        g_log.add("w2+");
        spin_ms(50);
        call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 1, 0});
        g_log.add("w2-");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("s"), 0, 0, 1, 0});
    REQUIRE(g_object > 0);
    const i32 a = create_thread(ctx, "w1", w1, 150), b = create_thread(ctx, "w2", w2, 150);
    start_thread(ctx, a);
    start_thread(ctx, b);
    spin_ms(150);  // (the test's thread keeps its CPU)
    const u32 info = scratch(128);
    psv_st32(info, 128);
    CHECK(call32(ctx, kSceLibKernel, kGetThreadInfo, {u(b), info}) == 0);
    CHECK(psv_ld32(info + 44) == 0x08);  // status: WAITING, not given a CPU
    CHECK(call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 1}) == 0);
    wait_end(ctx, a);
    wait_end(ctx, b);
    CHECK(g_log.joined() == "w2+ w1- w2-");
}

TEST_CASE("kernel: a thread spinning in guest code after losing its CPU sees what the thread that took it does") {
    // One CPU. The spinner loops without kernel calls until a flag is set; the setter, of a higher priority, takes
    // the CPU when its delay ends and sets it. The spinner stops only at its next kernel call, so until then both run
    // (with fibers too: the setter does not wait for the spinner's host thread).
    TestRuntime t(1, 100);
    static std::atomic<u32> flag{0}, seen{0};
    flag = 0;
    seen = 0;
    GuestCode code;
    const u32 setter = code.add([](PsvContext* ctx, uint32_t) {
        delay(ctx, 5000);
        flag = 1;
    });
    const u32 spinner = code.add([](PsvContext*, uint32_t) {
        const u64 give_up = nx::os::ticks_ns() + 3'000'000'000ull;
        while (!flag.load() && nx::os::ticks_ns() < give_up) {
        }
        seen = flag.load();
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 a = create_thread(ctx, "setter", setter, 120), b = create_thread(ctx, "spinner", spinner, 160);
    start_thread(ctx, a);
    start_thread(ctx, b);
    wait_end(ctx, b);
    wait_end(ctx, a);
    CHECK(seen.load() == 1);
}

TEST_CASE("kernel: three CPUs run three threads at once, never four") {
    TestRuntime t(3, 100);
    static std::atomic<u32> running{0}, most{0}, arrived{0};
    running = 0;
    most = 0;
    arrived = 0;
    GuestCode code;
    const u32 worker = code.add([](PsvContext*, uint32_t) {
        const u32 now = ++running;
        u32 seen = most.load();
        while (now > seen && !most.compare_exchange_weak(seen, now)) {
        }
        ++arrived;
        spin_ms(200);
        --running;
    });
    // Three threads that wait for each other without the kernel: they finish only if all three run together.
    static std::atomic<u32> rendezvous{0};
    rendezvous = 0;
    const u32 meet = code.add([](PsvContext* ctx, uint32_t) {
        ++rendezvous;
        const u64 give_up = nx::os::ticks_ns() + 5'000'000'000ull;
        while (rendezvous.load() < 3 && nx::os::ticks_ns() < give_up) {
        }
        ctx->r[0] = rendezvous.load() >= 3 ? 1 : 0;
    });
    code.install();
    PsvContext* ctx = t.ctx();
    i32 uids[4];
    for (auto& uid : uids) uid = create_thread(ctx, "worker", worker, 150);
    for (auto uid : uids) start_thread(ctx, uid);
    for (auto uid : uids) wait_end(ctx, uid);
    CHECK(most.load() == 3);
    CHECK(arrived.load() == 4);
    i32 met[3];
    for (auto& uid : met) uid = create_thread(ctx, "meet", meet, 150);
    for (auto uid : met) start_thread(ctx, uid);
    for (auto uid : met) {
        i32 status = 0;
        wait_end(ctx, uid, &status);
        CHECK(status == 1);
    }
}

TEST_CASE("kernel: a thread pinned to a CPU runs only there") {
    TestRuntime t(3, 100);
    static std::atomic<i32> cpu_seen{-2};
    GuestCode code;
    const u32 entry = code.add([](PsvContext*, uint32_t) { cpu_seen = Kernel::current()->cpu; });
    code.install();
    PsvContext* ctx = t.ctx();
    for (u32 cpu = 0; cpu < 3; ++cpu) {
        const i32 uid = create_thread(ctx, "pinned", entry, 120, kCpu0 << cpu);
        start_thread(ctx, uid);
        wait_end(ctx, uid);
        CHECK(cpu_seen.load() == static_cast<i32>(cpu));
    }
}

TEST_CASE("kernel: semaphores give resources in queue order, time out, and wake waiters on delete") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    const u32 consumer = code.add([](PsvContext* ctx, uint32_t) {
        const i32 r = call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 2, 0});
        g_log.add(r == 0 ? "got2" : "error");
    });
    const u32 doomed = code.add([](PsvContext* ctx, uint32_t) {
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 1, 0}));
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("s"), 0, 0, 10, 0});
    REQUIRE(g_object > 0);
    const i32 c = create_thread(ctx, "consumer", consumer, 100);
    start_thread(ctx, c);  // waits for 2
    const u32 info = scratch(60);
    psv_st32(info, 60);
    CHECK(call32(ctx, kSceLibKernel, kGetSemaInfo, {u(g_object), info}) == 0);
    CHECK(psv_ld32(info + 56) == 1);  // one waiting thread
    CHECK(call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 1}) == 0);
    g_log.add("signalled1");
    CHECK(call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 1}) == 0);  // now 2: the consumer wakes and runs
    g_log.add("signalled2");
    wait_end(ctx, c);
    CHECK(g_log.joined() == "signalled1 got2 signalled2");

    CHECK(call32(ctx, kSceThreadmgr, kPollSema, {u(g_object), 1}) == err::kSemaphoreZero);
    CHECK(call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 11}) == err::kSemaphoreOverflow);

    // A timeout: WAIT_TIMEOUT after at least the time asked, and the remaining time is set to 0.
    const u32 timeout = scratch(4);
    psv_st32(timeout, 20000);
    const u64 before = nx::os::ticks_ns();
    CHECK(call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 1, timeout}) == err::kWaitTimeout);
    CHECK(nx::os::ticks_ns() - before >= 20'000'000ull);
    CHECK(psv_ld32(timeout) == 0);
    // Satisfied before the timeout: the remaining time is what is left.
    call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 1});
    psv_st32(timeout, 1000000);
    CHECK(call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 1, timeout}) == 0);
    CHECK(psv_ld32(timeout) > 900000);

    const i32 d = create_thread(ctx, "doomed", doomed, 100);
    start_thread(ctx, d);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteSema, {u(g_object)}) == 0);
    i32 status = 0;
    wait_end(ctx, d, &status);
    CHECK(status == err::kWaitDelete);
    CHECK(call32(ctx, kSceThreadmgr, kSignalSema, {u(g_object), 1}) == err::kUnknownSemaphoreId);
}

namespace {
template <u32 Priority>
void mutex_waiter(PsvContext* ctx, uint32_t) {
    call32(ctx, kSceLibKernel, kLockMutex, {u(g_object), 1, 0});
    g_log.add(nx::format("{}", Priority));
    call32(ctx, kSceThreadmgr, kUnlockMutex, {u(g_object), 1});
}

nx::string mutex_order(u32 attr) {
    TestRuntime t(1, 100);
    g_log.events.clear();
    GuestCode code;
    const u32 w150 = code.add(&mutex_waiter<150>), w120 = code.add(&mutex_waiter<120>), w130 = code.add(&mutex_waiter<130>);
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("m"), attr, 1, 0});  // locked by main
    i32 uids[3] = {create_thread(ctx, "150", w150, 150), create_thread(ctx, "120", w120, 120), create_thread(ctx, "130", w130, 130)};
    for (i32 uid : uids) {
        start_thread(ctx, uid);
        delay(ctx, 2000);  // (lets it run and block on the mutex: they arrive in this order)
    }
    call32(ctx, kSceThreadmgr, kUnlockMutex, {u(g_object), 1});
    for (i32 uid : uids) wait_end(ctx, uid);
    return g_log.joined();
}
}  // namespace

TEST_CASE("kernel: a released mutex goes to the first waiter, in FIFO or priority order") {
    const nx::string fifo = mutex_order(0), prio = mutex_order(0x2000);
    CHECK(fifo == "150 120 130");  // SCE_KERNEL_MUTEX_ATTR_TH_FIFO: in arrival order
    CHECK(prio == "120 130 150");  // SCE_KERNEL_MUTEX_ATTR_TH_PRIO: highest priority first
}

TEST_CASE("kernel: mutex ownership, recursion and lock counts") {
    TestRuntime t;
    GuestCode code;
    const u32 other = code.add([](PsvContext* ctx, uint32_t) {
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(g_object), 1}));
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 plain = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("plain"), 0, 0, 0});
    CHECK(call32(ctx, kSceLibKernel, kLockMutex, {u(plain), 1, 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kLockMutex, {u(plain), 1, 0}) == err::kMutexRecursive);
    CHECK(call32(ctx, kSceLibKernel, kLockMutex, {u(plain), 2, 0}) == err::kIllegalCount);
    g_object = plain;
    const i32 o = create_thread(ctx, "other", other, 100);
    start_thread(ctx, o);
    i32 status = 0;
    wait_end(ctx, o, &status);
    CHECK(status == err::kMutexNotOwned);  // only the owner unlocks
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(plain), 1}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(plain), 1}) == err::kMutexNotOwned);

    const i32 rec = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("rec"), 0x2, 0, 0});
    CHECK(call32(ctx, kSceLibKernel, kLockMutex, {u(rec), 2, 0}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kTryLockMutex, {u(rec), 1}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(rec), 1}) == 0);
    const u32 info = scratch(64);
    psv_st32(info, 64);
    CHECK(call32(ctx, kSceLibKernel, kGetMutexInfo, {u(rec), info}) == 0);
    CHECK(psv_ld32(info + 48) == 2);  // currentCount
    CHECK(psv_ld32(info + 52) == u(call32(ctx, kSceLibKernel, kGetThreadId, {})));
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(rec), 3}) == err::kMutexUnlockUnderflow);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(rec), 2}) == 0);
}

TEST_CASE("kernel: a thread that ends releases the mutexes it owns") {
    TestRuntime t;
    GuestCode code;
    const u32 locker = code.add([](PsvContext* ctx, uint32_t) { call32(ctx, kSceLibKernel, kLockMutex, {u(g_object), 1, 0}); });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("m"), 0, 0, 0});
    const i32 l = create_thread(ctx, "locker", locker, 100);
    start_thread(ctx, l);
    wait_end(ctx, l);
    CHECK(call32(ctx, kSceThreadmgr, kTryLockMutex, {u(g_object), 1}) == 0);
}

TEST_CASE("kernel: event flags wait for all or any bits, clear them, and wake every matching waiter") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    static u32 result_a, result_b;
    const u32 and_waiter = code.add([](PsvContext* ctx, uint32_t) {
        const u32 out = scratch(4);
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceLibKernel, kWaitEventFlag, {u(g_object), 0x3, 0x0 /*AND*/, out, 0}));
        result_a = psv_ld32(out);
        g_log.add("and");
    });
    const u32 or_waiter = code.add([](PsvContext* ctx, uint32_t) {
        const u32 out = scratch(4);
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceLibKernel, kWaitEventFlag, {u(g_object), 0x6, 0x1 | 0x4 /*OR, CLEAR_PAT*/, out, 0}));
        result_b = psv_ld32(out);
        g_log.add("or");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateEventFlag, {guest_string("e"), 0x1000 /*MULTI*/, 0, 0});
    REQUIRE(g_object > 0);
    const i32 a = create_thread(ctx, "and", and_waiter, 100), b = create_thread(ctx, "or", or_waiter, 100);
    start_thread(ctx, a);
    start_thread(ctx, b);
    CHECK(call32(ctx, kSceThreadmgr, kSetEventFlag, {u(g_object), 0x1}) == 0);  // neither condition yet
    g_log.add("set1");
    CHECK(call32(ctx, kSceThreadmgr, kSetEventFlag, {u(g_object), 0x2}) == 0);  // both wake: AND (3 of 3), OR (bit 1)
    g_log.add("set2");
    wait_end(ctx, a);
    wait_end(ctx, b);
    CHECK(g_log.joined() == "set1 and or set2");
    CHECK(result_a == 0x3);
    CHECK(result_b == 0x3);  // (the value before its clear)
    const u32 out = scratch(4);
    // The OR waiter cleared bits 1-2: bit 0 is left.
    CHECK(call32(ctx, kSceLibKernel, kPollEventFlag, {u(g_object), 0x2, 0x1, out}) == err::kEventFlagCond);
    CHECK(psv_ld32(out) == 0x1);
    CHECK(call32(ctx, kSceLibKernel, kPollEventFlag, {u(g_object), 0x1, 0x2 /*CLEAR_ALL*/, out}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kClearEventFlag, {u(g_object), 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kPollEventFlag, {u(g_object), 0x1, 0, out}) == err::kEventFlagCond);
    CHECK(call32(ctx, kSceLibKernel, kWaitEventFlag, {u(g_object), 0, 0, out, 0}) == err::kEventFlagCond);
}

TEST_CASE("kernel: a single-wait event flag refuses a second waiter; cancel wakes waiters") {
    TestRuntime t(1);
    GuestCode code;
    const u32 waiter = code.add([](PsvContext* ctx, uint32_t) {
        const u32 out = scratch(4);
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceLibKernel, kWaitEventFlag, {u(g_object), 0x1, 0, out, 0}));
        ctx->r[0] ^= 0;  // (status: the wait's result)
        g_value = psv_ld32(out);
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateEventFlag, {guest_string("single"), 0, 0, 0});
    const i32 w = create_thread(ctx, "waiter", waiter, 100);
    start_thread(ctx, w);
    const u32 out = scratch(4);
    CHECK(call32(ctx, kSceLibKernel, kPollEventFlag, {u(g_object), 0x1, 0, out}) == err::kEventFlagMulti);
    const u32 count = scratch(4);
    CHECK(call32(ctx, kSceLibKernel, kCancelEventFlag, {u(g_object), 0x80, count}) == 0);
    CHECK(psv_ld32(count) == 1);
    i32 status = 0;
    wait_end(ctx, w, &status);
    CHECK(status == err::kWaitCancel);
    CHECK(g_value.load() == 0x80);
}

TEST_CASE("kernel: a condition variable releases its mutex while waiting and owns it again on wake-up") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    static i32 mutex, cond;
    const u32 waiter = code.add([](PsvContext* ctx, uint32_t) {
        call32(ctx, kSceLibKernel, kLockMutex, {u(mutex), 1, 0});
        const i32 r = call32(ctx, kSceLibKernel, kWaitCond, {u(cond), 0});
        g_log.add(r == 0 ? "woke" : "error");
        // It owns the mutex again.
        ctx->r[0] = static_cast<u32>(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(mutex), 1}));
    });
    code.install();
    PsvContext* ctx = t.ctx();
    mutex = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("m"), 0, 0, 0});
    cond = call32(ctx, kSceLibKernel, kCreateCond, {guest_string("c"), 0, u(mutex), 0});
    REQUIRE(cond > 0);
    const i32 w = create_thread(ctx, "waiter", waiter, 100);
    start_thread(ctx, w);  // locks, waits (releasing the mutex)
    CHECK(call32(ctx, kSceThreadmgr, kTryLockMutex, {u(mutex), 1}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kSignalCond, {u(cond)}) == 0);  // it wakes, then waits for the mutex
    g_log.add("signalled");
    CHECK(call32(ctx, kSceThreadmgr, kUnlockMutex, {u(mutex), 1}) == 0);  // now it runs
    i32 status = -1;
    wait_end(ctx, w, &status);
    CHECK(status == 0);
    CHECK(g_log.joined() == "signalled woke");
}

TEST_CASE("kernel: lightweight mutexes and condition variables live in guest work areas") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    static u32 mutex_work, cond_work;
    const u32 waiter = code.add([](PsvContext* ctx, uint32_t) {
        call32(ctx, kSceLibKernel, kLockLwMutex, {mutex_work, 1, 0});
        const i32 r = call32(ctx, kSceLibKernel, kWaitLwCond, {cond_work, 0});
        g_log.add(r == 0 ? "woke" : "error");
        call32(ctx, kSceLibKernel, kUnlockLwMutex, {mutex_work, 1});
    });
    code.install();
    PsvContext* ctx = t.ctx();
    mutex_work = scratch(32);
    cond_work = scratch(16);
    CHECK(call32(ctx, kSceLibKernel, kCreateLwMutex, {mutex_work, guest_string("lw"), 0, 0, 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kCreateLwCond, {cond_work, guest_string("lwc"), 0, mutex_work, 0}) == 0);
    const i32 w = create_thread(ctx, "waiter", waiter, 100);
    start_thread(ctx, w);
    CHECK(call32(ctx, kSceLibKernel, kTryLockLwMutex, {mutex_work, 1}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kSignalLwCond, {cond_work}) == 0);
    g_log.add("signalled");
    CHECK(call32(ctx, kSceLibKernel, kUnlockLwMutex, {mutex_work, 1}) == 0);
    wait_end(ctx, w);
    CHECK(g_log.joined() == "signalled woke");
    CHECK(call32(ctx, kSceLibKernel, kLockLwMutex, {scratch(32), 1, 0}) == err::kUnknownLwMutexId);
}

TEST_CASE("kernel: delays last at least as asked; zero is an invalid argument") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const u64 before = call(ctx, kSceLibKernel, kProcessTimeWide, {});
    CHECK(delay(ctx, 15000) == 0);
    const u64 after = call(ctx, kSceLibKernel, kProcessTimeWide, {});
    CHECK(after - before >= 15000);
    CHECK(after - before < 1000000);
    CHECK(delay(ctx, 0) == err::kInvalidArgument);
}

TEST_CASE("kernel: each thread has its own TLS area") {
    TestRuntime t;
    GuestCode code;
    static u32 other_tls;
    const u32 entry = code.add([](PsvContext* ctx, uint32_t) { other_tls = static_cast<u32>(call(ctx, kSceLibKernel, kGetTlsAddr, {3})); });
    code.install();
    PsvContext* ctx = t.ctx();
    const u32 mine = static_cast<u32>(call(ctx, kSceLibKernel, kGetTlsAddr, {3}));
    const i32 uid = create_thread(ctx, "tls", entry, 100);
    start_thread(ctx, uid);
    wait_end(ctx, uid);
    CHECK(mine != 0);
    CHECK(other_tls != 0);
    CHECK(mine != other_tls);
    CHECK(mine == ctx->tpidruro + 12);
}

TEST_CASE("kernel: sceKernelExitThread ends a thread from inside nested guest calls") {
    TestRuntime t;
    test_exit_import.function = hle::find_function(kSceThreadmgrCoredumpTime, "", kExitThread)->function;
    test_returned_after_exit = 0;
    GuestCode code;
    const u32 entry = code.add(&test_exit_entry);
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 uid = create_thread(ctx, "exits", entry, 100);
    start_thread(ctx, uid);
    i32 status = 0;
    CHECK(wait_end(ctx, uid, &status) == 0);
    CHECK(status == 7);
    CHECK(test_returned_after_exit == 0);
}

TEST_CASE("kernel: a notified callback runs in its thread during a CB wait") {
    TestRuntime t(1, 100);
    g_log.events.clear();
    GuestCode code;
    static u32 callback_fn;
    callback_fn = code.add([](PsvContext* ctx, uint32_t) {
        g_log.add(nx::format("cb{}", ctx->r[2]));  // (notifyArg)
        ctx->r[0] = 0;                              // stays registered
    });
    const u32 entry = code.add([](PsvContext* ctx, uint32_t) {
        g_object = call32(ctx, kSceThreadmgr, kCreateCallback, {guest_string("cb"), 0, callback_fn, 0});
        g_log.add(nx::format("created{}", g_object > 0 ? "" : " (error)"));
        call32(ctx, kSceThreadmgr, kDelayThreadCB, {100000});
        g_log.add("delay done");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 uid = create_thread(ctx, "cbthread", entry, 120);
    start_thread(ctx, uid);
    delay(ctx, 10000);  // (it creates its callback and starts waiting)
    CHECK(call32(ctx, kSceThreadmgr, kNotifyCallback, {u(g_object), 5}) == 0);
    wait_end(ctx, uid);
    CHECK(g_log.joined() == "created cb5 delay done");
    CHECK(call32(ctx, kSceThreadmgr, kCheckCallback, {}) == 0);  // (the main thread has none)
}

TEST_CASE("kernel: shutting down ends threads blocked in the kernel") {
    const u64 before = nx::os::ticks_ns();
    {
        TestRuntime t;
        GuestCode code;
        const u32 entry = code.add([](PsvContext* ctx, uint32_t) { call32(ctx, kSceLibKernel, kWaitSema, {u(g_object), 1, 0}); });
        code.install();
        PsvContext* ctx = t.ctx();
        g_object = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("never"), 0, 0, 1, 0});
        for (int i = 0; i < 5; ++i) start_thread(ctx, create_thread(ctx, "blocked", entry, 100));
        delay(ctx, 5000);
    }
    CHECK(nx::os::ticks_ns() - before < 1'500'000'000ull);  // (well before the shutdown's give-up time)
}

TEST_CASE("kernel: delays end soon after their deadline (how late, printed)") {
    TestRuntime t(kernel::kUserCpuCount, 100);
    static std::atomic<u64> late_ns{0}, worst_ns{0};
    late_ns = 0;
    worst_ns = 0;
    GuestCode code;
    const u32 entry = code.add([](PsvContext* ctx, uint32_t) {
        for (int i = 0; i < 200; ++i) {
            const u64 from = nx::os::ticks_ns();
            delay(ctx, 1000);
            const u64 took = nx::os::ticks_ns() - from;
            CHECK(took >= 1'000'000);
            late_ns += took - 1'000'000;
            worst_ns = nx::max(worst_ns.load(), took - 1'000'000);
        }
    });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 uid = create_thread(ctx, "sleeper", entry, 120);
    start_thread(ctx, uid);
    wait_end(ctx, uid);
    std::printf("kernel: 200 delays of 1 ms, each late by %.0f us on average, %.0f us at worst\n",
                static_cast<f64>(late_ns.load()) / 200e3, static_cast<f64>(worst_ns.load()) / 1e3);
}

TEST_CASE("kernel: a thread started again and again runs each time (the rate printed: what a start costs)") {
    TestRuntime t(kernel::kUserCpuCount, 100);
    static std::atomic<u32> ran{0};
    ran = 0;
    GuestCode code;
    const u32 entry = code.add([](PsvContext*, uint32_t) { ++ran; });
    code.install();
    PsvContext* ctx = t.ctx();
    const i32 uid = create_thread(ctx, "again", entry, 120);
    constexpr u32 kStarts = 300;
    const u64 from = nx::os::ticks_ns();
    for (u32 i = 0; i < kStarts; ++i) {
        CHECK(start_thread(ctx, uid) == 0);
        CHECK(wait_end(ctx, uid) == 0);
    }
    const f64 ms = static_cast<f64>(nx::os::ticks_ns() - from) / 1e6;
    CHECK(ran.load() == kStarts);
    std::printf("kernel: %u starts of a thread, each waited for, in %.1f ms: %.0f a second\n", kStarts, ms,
                kStarts * 1000.0 / ms);
}

TEST_CASE("kernel: suspended, no thread runs and the clock stands still; resumed, both go on from there") {
    TestRuntime t;
    g_value = 0;
    GuestCode code;
    // (counts, a kernel call each time round, until told to stop)
    const u32 entry = code.add([](PsvContext* ctx, uint32_t) {
        while (g_object == 0) {
            g_value.fetch_add(1);
            delay(ctx, 1000);
        }
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = 0;
    const i32 counter = create_thread(ctx, "counter", entry, 100);
    start_thread(ctx, counter);
    delay(ctx, 30'000);
    CHECK(g_value.load() > 5);

    Kernel& k = t.rt.kernel();
    k.suspend();
    CHECK(k.suspended());
    nx::this_thread::sleep_for(std::chrono::milliseconds(20));  // (the counter's delay ends; it gets no CPU)
    const u32 v1 = g_value.load();
    const u64 c1 = k.clock_ns();
    nx::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(g_value.load() == v1);
    CHECK(k.clock_ns() == c1);
    k.resume();
    CHECK_FALSE(k.suspended());
    // (the clock goes on from where it stood: the 150 ms are not in it)
    CHECK(k.clock_ns() - c1 < 20'000'000ull);
    delay(ctx, 30'000);
    CHECK(g_value.load() > v1 + 5);
    g_object = 1;
    CHECK(wait_end(ctx, counter) == 0);
    g_object = 0;
}

namespace {
constexpr u32 kCreateRWLock = 0x8667951D, kLockReadRWLock = 0xFA670F0F, kLockWriteRWLock = 0x67A187BB,
              kTryLockReadRWLock = 0xEFDDA456, kTryLockWriteRWLock = 0x206CBB66, kUnlockReadRWLock = 0x3EF91145,
              kUnlockWriteRWLock = 0xB4151397, kDeleteRWLock = 0xE73649CA;
constexpr u32 kCreateMsgPipe = 0x0A10C1C8, kSendMsgPipe = 0x0CA71EA2, kReceiveMsgPipe = 0x4E81DD5C,
              kTrySendMsgPipe = 0xDFC670E0, kTryReceiveMsgPipe = 0x5615B006, kCancelMsgPipe = 0xEF14BA37,
              kDeleteMsgPipe = 0xE78BCCF7;
constexpr u32 kCreateSimpleEvent = 0xE6DB2494, kWaitEvent = 0x120F03AF, kSetEvent = 0x324218CD,
              kClearEvent = 0x7B2A4B28, kDeleteSimpleEvent = 0x208CFE28, kOpenSema = 0xCBE235C7, kCloseSema = 0xA2D81F9E;
u32 g_buffer = 0, g_result = 0;
}  // namespace

TEST_CASE("kernel: reader-writer locks share reading, give writing alone, and hand over in queue order") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    const u32 writer = code.add([](PsvContext* ctx, uint32_t) {
        g_log.add(call32(ctx, kSceLibKernel, kLockWriteRWLock, {u(g_object), 0}) == 0 ? "write" : "error");
        call32(ctx, kSceThreadmgr, kUnlockWriteRWLock, {u(g_object)});
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateRWLock, {guest_string("rw"), 0, 0});
    REQUIRE(g_object > 0);
    REQUIRE(call32(ctx, kSceLibKernel, kLockReadRWLock, {u(g_object), 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kLockReadRWLock, {u(g_object), 0}) == err::kRWLockRecursive);  // (not recursive)
    CHECK(call32(ctx, kSceThreadmgr, kTryLockWriteRWLock, {u(g_object)}) == err::kRWLockFailedToLock);
    const i32 w = create_thread(ctx, "writer", writer, 100);
    start_thread(ctx, w);  // (waits: a reader holds it)
    g_log.add("reading");
    // (a writer waits: a new read lock waits behind it, so a try fails)
    CHECK(call32(ctx, kSceThreadmgr, kTryLockReadRWLock, {u(g_object)}) == err::kRWLockRecursive);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockReadRWLock, {u(g_object)}) == 0);  // (the writer has it, and runs)
    g_log.add("unlocked");
    wait_end(ctx, w);
    CHECK(g_log.joined() == "reading write unlocked");
    CHECK(call32(ctx, kSceThreadmgr, kUnlockReadRWLock, {u(g_object)}) == err::kRWLockNotOwned);
    CHECK(call32(ctx, kSceThreadmgr, kTryLockReadRWLock, {u(g_object)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockReadRWLock, {u(g_object)}) == 0);
    // Recursive: the writer may read and write again.
    const i32 r = call32(ctx, kSceLibKernel, kCreateRWLock, {guest_string("rec"), 0x2, 0});
    REQUIRE(call32(ctx, kSceLibKernel, kLockWriteRWLock, {u(r), 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kLockWriteRWLock, {u(r), 0}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kLockReadRWLock, {u(r), 0}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockReadRWLock, {u(r)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockWriteRWLock, {u(r)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockWriteRWLock, {u(r)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kUnlockWriteRWLock, {u(r)}) == err::kRWLockNotOwned);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteRWLock, {u(r)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteRWLock, {u(g_object)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteRWLock, {u(g_object)}) == err::kUnknownRWLockId);
}

TEST_CASE("kernel: message pipes carry bytes through their buffer, or straight from sender to receiver") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    const u32 receiver = code.add([](PsvContext* ctx, uint32_t) {
        const u32 moved = scratch(4);
        // (FULL: six bytes or wait)
        const i32 r = call32(ctx, kSceLibKernel, kReceiveMsgPipe, {u(g_object), g_buffer, 6, 0x1, moved, 0});
        g_log.add(r == 0 && psv_ld32(moved) == 6 ? "received6" : "error");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    Runtime& rt = t.rt;
    g_object = call32(ctx, kSceLibKernel, kCreateMsgPipe, {guest_string("pipe"), 64, 0, 8, 0});
    REQUIRE(g_object > 0);
    g_buffer = scratch(16);
    const u32 moved = scratch(4), out = scratch(16);
    REQUIRE(call32(ctx, kSceLibKernel, kSendMsgPipe, {u(g_object), guest_string("abcd"), 4, 0x1, moved, 0}) == 0);
    CHECK(psv_ld32(moved) == 4);
    const i32 rt_thread = create_thread(ctx, "receiver", receiver, 100);
    start_thread(ctx, rt_thread);  // (has four: waits for two more)
    g_log.add("sent4");
    REQUIRE(call32(ctx, kSceLibKernel, kSendMsgPipe, {u(g_object), guest_string("efgh"), 4, 0x1, moved, 0}) == 0);
    wait_end(ctx, rt_thread);
    CHECK(g_log.joined() == "sent4 received6");
    CHECK(std::memcmp(rt.memory().host(g_buffer), "abcdef", 6) == 0);
    // Two left: an ASAP receive of four gets them; then the pipe is empty.
    REQUIRE(call32(ctx, kSceLibKernel, kTryReceiveMsgPipe, {u(g_object), out, 4, 0, moved}) == 0);
    CHECK(psv_ld32(moved) == 2);
    CHECK(std::memcmp(rt.memory().host(out), "gh", 2) == 0);
    CHECK(call32(ctx, kSceLibKernel, kTryReceiveMsgPipe, {u(g_object), out, 4, 0, moved}) == err::kMsgPipeEmpty);
    // Full: a FULL send that does not fit, without waiting, fails; DONT_REMOVE looks without taking.
    REQUIRE(call32(ctx, kSceLibKernel, kTrySendMsgPipe, {u(g_object), guest_string("12345678"), 8, 0x1, moved}) == 0);
    CHECK(call32(ctx, kSceLibKernel, kTrySendMsgPipe, {u(g_object), guest_string("9"), 1, 0x1, moved}) == err::kMsgPipeFull);
    REQUIRE(call32(ctx, kSceLibKernel, kReceiveMsgPipe, {u(g_object), out, 3, 0x101, moved, 0}) == 0);
    CHECK(std::memcmp(rt.memory().host(out), "123", 3) == 0);
    REQUIRE(call32(ctx, kSceLibKernel, kReceiveMsgPipe, {u(g_object), out, 8, 0x1, moved, 0}) == 0);
    CHECK(std::memcmp(rt.memory().host(out), "12345678", 8) == 0);
    // A timeout, with nothing to receive.
    const u32 timeout = scratch(4);
    psv_st32(timeout, 10000);
    CHECK(call32(ctx, kSceLibKernel, kReceiveMsgPipe, {u(g_object), out, 1, 0, moved, timeout}) == err::kWaitTimeout);
    const u32 counts = scratch(8);
    CHECK(call32(ctx, kSceLibKernel, kCancelMsgPipe, {u(g_object), counts, counts + 4}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteMsgPipe, {u(g_object)}) == 0);

    // Without a buffer: the sender's bytes go straight to the waiting receiver.
    g_log.events.clear();
    g_object = call32(ctx, kSceLibKernel, kCreateMsgPipe, {guest_string("direct"), 64, 0, 0, 0});
    REQUIRE(g_object > 0);
    std::memset(rt.memory().host(g_buffer), 0, 16);
    CHECK(call32(ctx, kSceLibKernel, kTrySendMsgPipe, {u(g_object), guest_string("zz"), 2, 0x1, moved}) == err::kMsgPipeFull);  // (nobody receives)
    const i32 r2 = create_thread(ctx, "receiver", receiver, 100);
    start_thread(ctx, r2);
    REQUIRE(call32(ctx, kSceLibKernel, kSendMsgPipe, {u(g_object), guest_string("uvwxyz"), 6, 0x1, moved, 0}) == 0);
    wait_end(ctx, r2);
    CHECK(g_log.joined() == "received6");
    CHECK(std::memcmp(rt.memory().host(g_buffer), "uvwxyz", 6) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteMsgPipe, {u(g_object)}) == 0);
}

TEST_CASE("kernel: simple events release a waiter on any of its bits, with the user data; auto reset clears them") {
    TestRuntime t(1);
    g_log.events.clear();
    GuestCode code;
    const u32 waiter = code.add([](PsvContext* ctx, uint32_t) {
        const u32 result = scratch(4), data = scratch(8);
        const i32 r = call32(ctx, kSceLibKernel, kWaitEvent, {u(g_object), 0x2, result, data, 0});
        g_result = psv_ld32(result);
        g_log.add(r == 0 && psv_ld32(data) == 0x55667788u && psv_ld32(data + 4) == 0x11223344u ? "released" : "error");
    });
    code.install();
    PsvContext* ctx = t.ctx();
    g_object = call32(ctx, kSceLibKernel, kCreateSimpleEvent, {guest_string("ev"), 0x100, 0, 0});  // (auto reset)
    REQUIRE(g_object > 0);
    const i32 w = create_thread(ctx, "waiter", waiter, 100);
    start_thread(ctx, w);
    g_log.add("set");
    // (bits 1 and 2; the user data a 64-bit argument in r2:r3)
    REQUIRE(call32(ctx, kSceThreadmgr, kSetEvent, {u(g_object), 0x6, 0x55667788u, 0x11223344u}) == 0);
    wait_end(ctx, w);
    CHECK(g_log.joined() == "set released");
    CHECK(g_result == 0x2);
    const u32 result = scratch(4), timeout = scratch(4);
    REQUIRE(call32(ctx, kSceLibKernel, kWaitEvent, {u(g_object), 0x6, result, 0, 0}) == 0);
    CHECK(psv_ld32(result) == 0x4);  // (bit 2 was cleared by the waiter's release)
    psv_st32(timeout, 10000);
    CHECK(call32(ctx, kSceLibKernel, kWaitEvent, {u(g_object), 0x6, result, 0, timeout}) == err::kWaitTimeout);
    CHECK(call32(ctx, kSceLibKernel, kWaitEvent, {u(g_object), 0, result, 0, 0}) == err::kEventCond);
    // Manual reset: the bits stay until cleared (a clear keeps the bits given).
    const i32 manual = call32(ctx, kSceLibKernel, kCreateSimpleEvent, {guest_string("m"), 0, 0x3, 0});
    REQUIRE(call32(ctx, kSceLibKernel, kWaitEvent, {u(manual), 0x1, result, 0, 0}) == 0);
    REQUIRE(call32(ctx, kSceLibKernel, kWaitEvent, {u(manual), 0x1, result, 0, 0}) == 0);
    REQUIRE(call32(ctx, kSceThreadmgr, kClearEvent, {u(manual), 0x2}) == 0);
    psv_st32(timeout, 1000);
    CHECK(call32(ctx, kSceLibKernel, kWaitEvent, {u(manual), 0x1, result, 0, timeout}) == err::kWaitTimeout);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteSimpleEvent, {u(manual)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteSimpleEvent, {u(g_object)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kDeleteSimpleEvent, {u(g_object)}) == err::kUnknownEventId);
}

TEST_CASE("kernel: an openable object is found by its name; others are not") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    const i32 shared = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("shared"), 0x80, 0, 1, 0});
    const i32 hidden = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("hidden"), 0, 0, 1, 0});
    REQUIRE(shared > 0);
    REQUIRE(hidden > 0);
    CHECK(call32(ctx, kSceThreadmgr, kOpenSema, {guest_string("shared")}) == shared);
    CHECK(call32(ctx, kSceThreadmgr, kOpenSema, {guest_string("hidden")}) == err::kUnknownSemaphoreId);
    CHECK(call32(ctx, kSceThreadmgr, kOpenSema, {guest_string("none")}) == err::kUnknownSemaphoreId);
    CHECK(call32(ctx, kSceThreadmgr, kCloseSema, {u(shared)}) == 0);
    CHECK(call32(ctx, kSceThreadmgr, kCloseSema, {0x1234}) == err::kUnknownSemaphoreId);
}

TEST_CASE("kernel: firmware 1.x's NIDs for the current priority and the system time's low word") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    CHECK(call32(ctx, kSceThreadmgr, 0x01414F0B, {}) == call32(ctx, kSceLibKernel, 0xC53A9848, {}));
    const u32 low = u(call32(ctx, kSceThreadmgr, 0x47F6DE49, {}));
    CHECK(low <= static_cast<u32>(t.rt.kernel().process_time()));
    CHECK(low > 0);
}

namespace {
u32 g_stack_seen = 0;
void on_other_stack(PsvContext* ctx, u32) {
    g_stack_seen = ctx->r[13];
    ctx->r[0] = ctx->r[0] + 1;  // (its common argument, plus one)
}
}  // namespace

TEST_CASE("kernel more: atomics, C helpers, events polled, pulsed and cancelled, several waited on, infos, timers") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    // atomics
    const u32 cell = scratch(8);
    psv_st32(cell, 10);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x2E84A93B, {cell, 5}) == 15);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x115C516F, {cell, 3}) == 15);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x1124A1D4, {cell, 12, 40}) == 12);
    CHECK(psv_ld32(cell) == 40);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x4B33FD3C, {cell, 1, 40}) == 0);
    CHECK(psv_ld32(cell) == 40);
    psv_st32(cell, 0);
    CHECK(call32(ctx, 0xCAE9ACE6, 0xCA3294F1, {cell}) == -1);
    CHECK(psv_ld32(cell) == 0);
    psv_st32(cell, 0xFF), psv_st32(cell + 4, 0);
    call32(ctx, 0xCAE9ACE6, 0xD7D49E36, {cell, 0x0F});
    CHECK(psv_ld32(cell) == 0xF0);
    CHECK(call(ctx, 0xCAE9ACE6, 0x63DAF37D, {cell, 0, 0, 1}) == 0xF0);  // (adds 1 << 32)
    CHECK(psv_ld32(cell + 4) == 1);
    // C helpers
    const u32 hay = guest_string("hello world"), needle = guest_string("world"), dst = scratch(8);
    CHECK(u(call32(ctx, 0xCAE9ACE6, 0xE265498B, {hay, needle})) == hay + 6);
    CHECK(call32(ctx, 0xCAE9ACE6, 0xB0335388, {'q'}) == 'Q');
    CHECK(call32(ctx, 0xCAE9ACE6, 0xB9B6B05A, {dst, hay, 8}) == 0);  // (too long: nothing written)
    CHECK(u(call32(ctx, 0xCAE9ACE6, 0xB9B6B05A, {dst, needle, 8})) == dst);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x100091AF, {hay + 6, needle, 5}) == 0);
    // events
    const i32 ev = call32(ctx, 0xCAE9ACE6, 0xE6DB2494, {guest_string("ev"), 0, 0, 0});
    REQUIRE(ev > 0);
    const u32 got = scratch(4), data = scratch(8), info = scratch(64), waiting = scratch(4);
    CHECK(u(call32(ctx, 0xCAE9ACE6, 0x241F3634, {u(ev), 1, got, data})) == 0x80028202u);
    CHECK(call32(ctx, 0x859A24B1, 0x324218CD, {u(ev), 0x6, 0x1234, 0}) == 0);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x241F3634, {u(ev), 2, got, data}) == 0);
    CHECK(psv_ld32(got) == 2);
    CHECK(psv_ld32(data) == 0x1234);
    REQUIRE(call32(ctx, 0xCAE9ACE6, 0xE35F19D6, {u(ev), info}) == 0);
    CHECK(psv_ld32(info + 44) == 0x6);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(info + 8))) == "ev");
    CHECK(call32(ctx, 0x859A24B1, 0x8D27BAD6, {u(ev), 0x1, 0, 0}) == 0);
    const u32 pattern = scratch(4);
    CHECK(call32(ctx, 0xCAE9ACE6, 0xE5A2AC66, {u(ev), pattern}) == 0);
    CHECK(psv_ld32(pattern) == 0x6);  // (a pulse leaves the pattern as it was)
    const u32 list = scratch(16), results = scratch(48), tmo = scratch(4);
    psv_st32(list, u(ev)), psv_st32(list + 4, 0x4), psv_st32(list + 8, u(ev)), psv_st32(list + 12, 0x8);
    psv_st32(tmo, 2000);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x10586418, {list, 2, 1, results, tmo}) == 0);  // (OR: the first)
    CHECK(psv_ld32(results + 8) == 0x4);
    psv_st32(tmo, 2000);
    CHECK(u(call32(ctx, 0xCAE9ACE6, 0x10586418, {list, 2, 2, results, tmo})) == 0x80028005u);  // (AND: timeout)
    CHECK(call32(ctx, 0xCAE9ACE6, 0xB01387D5, {u(ev), 0x10, 0, 0, waiting}) == 0);
    CHECK(psv_ld32(waiting) == 0);
    CHECK(call32(ctx, 0xCAE9ACE6, 0xE5A2AC66, {u(ev), pattern}) == 0);
    CHECK(psv_ld32(pattern) == 0x10);
    CHECK(call32(ctx, 0x859A24B1, 0xC9678F7F, {u(ev)}) == 14);  // (the kernel's class number of a simple event)
    // a message pipe through two buffers
    const i32 pipe = call32(ctx, 0xCAE9ACE6, 0x0A10C1C8, {guest_string("pipe"), 0x40, 0, 64, 0});
    REQUIRE(pipe > 0);
    const u32 a = guest_string("abc"), b = guest_string("defg"), v = scratch(16), moved = scratch(4), out = scratch(16);
    psv_st32(v, a), psv_st32(v + 4, 3), psv_st32(v + 8, b), psv_st32(v + 12, 4);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x60DB346F, {u(pipe), v, 2, 0, moved}) == 0);
    CHECK(psv_ld32(moved) == 7);
    const u32 pinfo = scratch(60);
    REQUIRE(call32(ctx, 0xCAE9ACE6, 0x4046D16B, {u(pipe), pinfo}) == 0);
    CHECK(psv_ld32(pinfo + 48) == 64 - 7);
    psv_st32(v, out), psv_st32(v + 4, 7);
    CHECK(call32(ctx, 0xCAE9ACE6, 0x86ECC0FF, {u(pipe), v, 1, 0, moved}) == 0);
    CHECK(std::memcmp(t.rt.memory().host(out), "abcdefg", 7) == 0);
    // timers
    const i32 timer = call32(ctx, 0xCAE9ACE6, 0x2255B2A5, {guest_string("tm"), 0, 0});
    REQUIRE(timer > 0);
    CHECK(call(ctx, 0x859A24B1, 0x273B4A4D, {u(timer), 0, 5000, 0}) == 0);
    CHECK(call(ctx, 0x859A24B1, 0x3EFD3165, {u(timer)}) == 5000);
    const u32 tinfo = scratch(0x60);
    REQUIRE(call32(ctx, 0xCAE9ACE6, 0x7E35E10A, {u(timer), tinfo}) == 0);
    CHECK(psv_ld32(tinfo + 56) == 5000);
    CHECK(u(call32(ctx, 0xCAE9ACE6, 0x621D293B, {u(timer), 0, 0, 0})) == 0x80020004u);
    // another stack
    GuestCode code;
    const u32 fn = code.add(&on_other_stack);
    code.install();
    const u32 stack = scratch(0x1000);
    CHECK(call32(ctx, 0xCAE9ACE6, 0xAFE2B300, {stack, 0x1000, fn, 41}) == 42);
    CHECK(g_stack_seen <= stack + 0x1000);
    CHECK(g_stack_seen > stack);
}

#if PSVRECORE_GUEST_FIBERS
namespace {
i32 g_det_sema = 0, g_det_mutex = 0;
u32 g_det_worker = 0;

// Six threads of three priorities on three CPUs: each logs a step under a mutex, signals or waits for a semaphore
// (three give, three take) and sleeps a little, four times; a root thread starts them and waits for their ends.
nx::string deterministic_run() {
    Runtime::Config c;
    c.kernel.cpu_count = kernel::kUserCpuCount;
    c.kernel.deterministic = true;
    Runtime rt(c);
    if (!rt.ok()) return "(no runtime)";
    if (std::string_view(rt.kernel().backend_name()) != "fibers (deterministic)") return rt.kernel().backend_name();
    g_log.events.clear();
    GuestCode code;
    g_det_worker = code.add([](PsvContext* ctx, uint32_t) {
        const u32 id = psv_ld32(ctx->r[1]);
        for (u32 i = 0; i < 4; ++i) {
            call32(ctx, kSceLibKernel, kLockMutex, {u(g_det_mutex), 1, 0});
            g_log.add(nx::format("{}{}", static_cast<char>('a' + id), i));
            call32(ctx, kSceThreadmgr, kUnlockMutex, {u(g_det_mutex), 1});
            if (id % 2) call32(ctx, kSceThreadmgr, kSignalSema, {u(g_det_sema), 1});
            else call32(ctx, kSceLibKernel, kWaitSema, {u(g_det_sema), 1, 0});
            call32(ctx, kSceThreadmgr, kDelayThread, {100 + 37 * id});
        }
    });
    const u32 root = code.add([](PsvContext* ctx, uint32_t) {
        g_det_sema = call32(ctx, kSceLibKernel, kCreateSema, {guest_string("det"), 0, 0, 16, 0});
        g_det_mutex = call32(ctx, kSceLibKernel, kCreateMutex, {guest_string("det"), 0, 0, 0});
        const u32 block = scratch(4);
        i32 uids[6];
        for (u32 id = 0; id < 6; ++id) {
            uids[id] = create_thread(ctx, "det", g_det_worker, 120 + id % 3);
            psv_st32(block, id);
            start_thread(ctx, uids[id], 4, block);
        }
        for (i32 uid : uids) wait_end(ctx, uid);
        g_log.add("end");
    });
    code.install();
    Kernel& k = rt.kernel();
    const Uid r = k.create_thread("root", root, 110, 0x4000, 0, 0);
    k.start_thread(r, 0, 0);
    i32 status = 0;
    k.wait_thread_end(r, &status, kernel::Timeout{}, false);
    return g_log.joined();
}
}  // namespace

TEST_CASE("kernel: deterministic scheduling (fibers) runs threads in the same order run after run") {
    const nx::string first = deterministic_run();
    CHECK(first.size() > 6 * 4 * 3);  // (every step logged, and the end)
    CHECK(first.view().ends_with("end"));
    for (int i = 0; i < 4; ++i) CHECK(deterministic_run() == first);
}
#endif
