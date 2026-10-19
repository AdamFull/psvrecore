// Threads, synchronisation and time: SceThreadmgr, the thread parts of SceLibKernel, SceThreadmgrCoredumpTime.

#include "hle_common.hpp"

#include <cstring>
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

using kernel::ObjectClass;

constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceThreadmgr = 0x859A24B1, kSceThreadmgrCoredumpTime = 0x5E8D0E22;

// ---- guest structures

struct ThreadInfo {
    u32 size;
    i32 process_id;
    char name[32];
    u32 attr, status, entry, stack, stack_size;
    i32 init_priority, current_priority, init_affinity, current_affinity, current_cpu, last_cpu;
    u32 wait_type;
    i32 wait_id, exit_status;
    u64 run_clocks;
    u32 intr_preempt_count, thread_preempt_count, thread_release_count;
    i32 change_cpu_count, notify_callback, reserved;
};
static_assert(sizeof(ThreadInfo) == 128);

struct SemaInfo {
    u32 size;
    i32 uid;
    char name[32];
    u32 attr;
    i32 init_count, current_count, max_count;
    u32 waiting;
};
static_assert(sizeof(SemaInfo) == 60);

struct EventFlagInfo {
    u32 size;
    i32 uid;
    char name[32];
    u32 attr, init_pattern, current_pattern, waiting;
};
static_assert(sizeof(EventFlagInfo) == 56);

struct MutexInfo {
    u32 size;
    i32 uid;
    char name[32];
    u32 attr;
    i32 init_count, current_count, owner;
    u32 waiting;
    i32 ceiling_priority;
};
static_assert(sizeof(MutexInfo) == 64);

struct LwMutexInfo {
    u32 size;
    i32 uid;
    char name[32];
    u32 attr, work;
    i32 init_count, current_count, owner;
    u32 waiting;
};
static_assert(sizeof(LwMutexInfo) == 64);

constexpr i32 kProcessId = 0x00010005;  // (one process; any positive value)

// Copies a filled guest structure, as much of it as the guest's `size` field asks for.
template <class T>
i32 put_info(Ptr<T> out, const T& value) {
    if (!out || !writable(out.address, 4)) return err::kIllegalAddress;
    const u32 size = std::min<u32>(out->size, sizeof(T));
    if (size < 4 || !writable(out.address, size)) return err::kIllegalSize;
    T copy = value;
    copy.size = out->size;
    std::memcpy(out.get(), &copy, size);
    return err::kOk;
}

// ---- threads

i32 sceKernelCreateThread(Ptr<const char> name, u32 entry, i32 priority, u32 stack_size, u32 attr, i32 affinity,
                          Ptr<const void> /*opt*/) {
    return kernel().create_thread(str(name, 31), entry, priority, stack_size, attr, affinity);
}
i32 sceKernelStartThread(i32 uid, u32 arg_size, Ptr<const void> args) { return kernel().start_thread(uid, arg_size, args.address); }
i32 sceKernelExitThread(i32 status) { kernel().exit_thread(status, false); }
i32 sceKernelExitDeleteThread(i32 status) { kernel().exit_thread(status, true); }
i32 sceKernelDeleteThread(i32 uid) { return kernel().delete_thread(uid); }
i32 sceKernelWaitThreadEnd(i32 uid, Ptr<i32> status, Ptr<u32> t) { return kernel().wait_thread_end(uid, status.get(), timeout(t), false); }
i32 sceKernelWaitThreadEndCB(i32 uid, Ptr<i32> status, Ptr<u32> t) { return kernel().wait_thread_end(uid, status.get(), timeout(t), true); }
i32 sceKernelDelayThread(u32 usec) { return kernel().delay_thread(usec, false); }
i32 sceKernelDelayThreadCB(u32 usec) { return kernel().delay_thread(usec, true); }
i32 sceKernelGetThreadId() { return kernel().current_thread_id(); }
i32 sceKernelGetThreadCurrentPriority() { return kernel().current_priority(); }
i32 sceKernelChangeThreadPriority(i32 uid, i32 priority) { return kernel().change_priority(uid, priority); }
i32 sceKernelChangeThreadCpuAffinityMask(i32 uid, i32 mask) { return kernel().change_affinity(uid, mask); }
i32 sceKernelGetThreadCpuAffinityMask(i32 uid) { return kernel().get_affinity(uid); }
i32 sceKernelGetThreadExitStatus(i32 uid, Ptr<i32> status) { return kernel().get_exit_status(uid, status.get()); }
i32 sceKernelGetProcessId() { return kProcessId; }

i32 sceKernelGetThreadInfo(i32 uid, Ptr<ThreadInfo> out) {
    Kernel::ThreadInfo info;
    if (const i32 r = kernel().thread_info(uid, &info); r < 0) return r;
    ThreadInfo g{};
    g.process_id = kProcessId;
    put_name(g.name, info.name);
    g.attr = info.attr;
    g.status = info.status;
    g.entry = info.entry;
    g.stack = info.stack;
    g.stack_size = info.stack_size;
    g.init_priority = info.init_priority;
    g.current_priority = info.priority;
    g.init_affinity = info.init_affinity;
    g.current_affinity = info.affinity;
    g.current_cpu = info.cpu < 0 ? 0 : info.cpu;
    g.last_cpu = info.last_cpu < 0 ? 0 : info.last_cpu;
    g.wait_type = info.wait_type;
    g.wait_id = info.wait_id;
    g.exit_status = info.exit_status;
    return put_info(out, g);
}

i32 sceKernelCheckThreadStack(PsvContext* ctx) {
    // The stack the calling thread has left.
    kernel::Thread* t = Kernel::current();
    if (!t || !t->stack) return 0;
    return static_cast<i32>(ctx->r[13] - t->stack);
}

i32 sceKernelChangeThreadVfpException(i32 clear, i32 set) {
    // (Floating-point exception traps are not modeled: the masks are kept and reported, never acted on.)
    static thread_local i32 mask = 0;
    mask = (mask & ~clear) | set;
    return err::kOk;
}
i32 sceKernelGetCurrentThreadVfpException() { return 0; }

i32 sceKernelSendSignal(i32 uid) { return kernel().send_signal(uid); }
i32 sceKernelWaitSignal(u32 /*unknown0*/, u32 /*unknown1*/, Ptr<u32> t) { return kernel().wait_signal(timeout(t)); }
u32 sceKernelGetTLSAddr(u32 key) { return kernel().tls_address(key); }

i32 sceKernelRegisterThreadEventHandler(Ptr<const char> name, i32 /*thread*/, u32 /*mask*/, u32 /*handler*/, u32 /*common*/) {
    // Thread events are not provided to games (the SDK says so); handlers are accepted and never called.
    nx::logd(logs::hle, "thread event handler '{}' registered (never called)", str(name, 31));
    return 0x40000001;
}
i32 sceKernelUnregisterThreadEventHandler(i32 /*uid*/) { return err::kOk; }

// ---- semaphores

i32 sceKernelCreateSema(Ptr<const char> name, u32 attr, i32 init, i32 max, Ptr<const void>) {
    return kernel().create_semaphore(str(name, 31), attr, init, max);
}
i32 sceKernelDeleteSema(i32 uid) { return kernel().delete_semaphore(uid); }
i32 sceKernelWaitSema(i32 uid, i32 count, Ptr<u32> t) { return kernel().wait_semaphore(uid, count, timeout(t), false); }
i32 sceKernelWaitSemaCB(i32 uid, i32 count, Ptr<u32> t) { return kernel().wait_semaphore(uid, count, timeout(t), true); }
i32 sceKernelPollSema(i32 uid, i32 count) { return kernel().poll_semaphore(uid, count); }
i32 sceKernelSignalSema(i32 uid, i32 count) { return kernel().signal_semaphore(uid, count); }
i32 sceKernelCancelSema(i32 uid, i32 count, Ptr<u32> waiting) { return kernel().cancel_semaphore(uid, count, waiting.get()); }
i32 sceKernelGetSemaInfo(i32 uid, Ptr<SemaInfo> out) {
    kernel::Semaphore s;
    u32 waiting = 0;
    if (const i32 r = kernel().semaphore_info(uid, &s, &waiting); r < 0) return r;
    SemaInfo g{};
    g.uid = s.uid;
    put_name(g.name, s.name);
    g.attr = s.attr;
    g.init_count = s.init;
    g.current_count = s.count;
    g.max_count = s.max;
    g.waiting = waiting;
    return put_info(out, g);
}

// ---- event flags

i32 sceKernelCreateEventFlag(Ptr<const char> name, u32 attr, u32 pattern, Ptr<const void>) {
    return kernel().create_event_flag(str(name, 31), attr, pattern);
}
i32 sceKernelDeleteEventFlag(i32 uid) { return kernel().delete_event_flag(uid); }
i32 sceKernelWaitEventFlag(i32 uid, u32 bits, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return kernel().wait_event_flag(uid, bits, mode, result.get(), timeout(t), false);
}
i32 sceKernelWaitEventFlagCB(i32 uid, u32 bits, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return kernel().wait_event_flag(uid, bits, mode, result.get(), timeout(t), true);
}
i32 sceKernelPollEventFlag(i32 uid, u32 bits, u32 mode, Ptr<u32> result) {
    return kernel().poll_event_flag(uid, bits, mode, result.get());
}
i32 sceKernelSetEventFlag(i32 uid, u32 bits) { return kernel().set_event_flag(uid, bits); }
i32 sceKernelClearEventFlag(i32 uid, u32 bits) { return kernel().clear_event_flag(uid, bits); }
i32 sceKernelCancelEventFlag(i32 uid, u32 pattern, Ptr<u32> waiting) { return kernel().cancel_event_flag(uid, pattern, waiting.get()); }
i32 sceKernelGetEventFlagInfo(i32 uid, Ptr<EventFlagInfo> out) {
    kernel::EventFlag e;
    u32 waiting = 0;
    if (const i32 r = kernel().event_flag_info(uid, &e, &waiting); r < 0) return r;
    EventFlagInfo g{};
    g.uid = e.uid;
    put_name(g.name, e.name);
    g.attr = e.attr;
    g.init_pattern = e.init;
    g.current_pattern = e.pattern;
    g.waiting = waiting;
    return put_info(out, g);
}

// ---- mutexes

i32 sceKernelCreateMutex(Ptr<const char> name, u32 attr, i32 init, Ptr<const void>) {
    return kernel().create_mutex(str(name, 31), attr, init);
}
i32 sceKernelDeleteMutex(i32 uid) { return kernel().delete_mutex(uid); }
i32 sceKernelLockMutex(i32 uid, i32 count, Ptr<u32> t) { return kernel().lock_mutex(uid, count, timeout(t), false); }
i32 sceKernelLockMutexCB(i32 uid, i32 count, Ptr<u32> t) { return kernel().lock_mutex(uid, count, timeout(t), true); }
i32 sceKernelTryLockMutex(i32 uid, i32 count) { return kernel().try_lock_mutex(uid, count); }
i32 sceKernelUnlockMutex(i32 uid, i32 count) { return kernel().unlock_mutex(uid, count); }
i32 sceKernelCancelMutex(i32 uid, i32 count, Ptr<u32> waiting) { return kernel().cancel_mutex(uid, count, waiting.get()); }
i32 sceKernelGetMutexInfo(i32 uid, Ptr<MutexInfo> out) {
    kernel::Mutex m;
    Uid owner = 0;
    u32 waiting = 0;
    if (const i32 r = kernel().mutex_info(uid, ObjectClass::Mutex, &m, &owner, &waiting); r < 0) return r;
    MutexInfo g{};
    g.uid = m.uid;
    put_name(g.name, m.name);
    g.attr = m.attr;
    g.init_count = m.init;
    g.current_count = m.count;
    g.owner = owner;
    g.waiting = waiting;
    return put_info(out, g);
}

// ---- lightweight mutexes: the guest's work area names the object

i32 lw_mutex(u32 work) {
    const Uid uid = work ? kernel().lw_uid(work) : 0;
    return uid ? uid : err::kUnknownLwMutexId;
}

i32 sceKernelCreateLwMutex(Ptr<u8> work, Ptr<const char> name, u32 attr, i32 init, Ptr<const void>) {
    if (!writable(work.address, 32)) return err::kIllegalAddress;
    if (kernel().lw_uid(work.address)) return err::kInvalidArgument;  // (already a lightweight mutex)
    const i32 uid = kernel().create_mutex(str(name, 31), attr, init, ObjectClass::LwMutex, work.address);
    if (uid < 0) return uid;
    std::memset(work.get(), 0, 32);
    std::memcpy(work.get(), &uid, 4);
    return err::kOk;
}
i32 sceKernelDeleteLwMutex(Ptr<u8> work) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return kernel().delete_mutex(uid, ObjectClass::LwMutex);
}
i32 sceKernelLockLwMutex(Ptr<u8> work, i32 count, Ptr<u32> t) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return kernel().lock_mutex(uid, count, timeout(t), false, ObjectClass::LwMutex);
}
i32 sceKernelLockLwMutexCB(Ptr<u8> work, i32 count, Ptr<u32> t) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return kernel().lock_mutex(uid, count, timeout(t), true, ObjectClass::LwMutex);
}
i32 sceKernelTryLockLwMutex(Ptr<u8> work, i32 count) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return kernel().try_lock_mutex(uid, count, ObjectClass::LwMutex);
}
i32 sceKernelUnlockLwMutex(Ptr<u8> work, i32 count) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return kernel().unlock_mutex(uid, count, ObjectClass::LwMutex);
}
i32 lw_mutex_info(i32 uid, Ptr<LwMutexInfo> out) {
    kernel::Mutex m;
    Uid owner = 0;
    u32 waiting = 0;
    if (const i32 r = kernel().mutex_info(uid, ObjectClass::LwMutex, &m, &owner, &waiting); r < 0) return r;
    LwMutexInfo g{};
    g.uid = m.uid;
    put_name(g.name, m.name);
    g.attr = m.attr;
    g.work = m.work;
    g.init_count = m.init;
    g.current_count = m.count;
    g.owner = owner;
    g.waiting = waiting;
    return put_info(out, g);
}
i32 sceKernelGetLwMutexInfo(Ptr<u8> work, Ptr<LwMutexInfo> out) {
    const i32 uid = lw_mutex(work.address);
    if (uid < 0) return uid;
    return lw_mutex_info(uid, out);
}
i32 sceKernelGetLwMutexInfoById(i32 uid, Ptr<LwMutexInfo> out) { return lw_mutex_info(uid, out); }

// ---- condition variables

i32 sceKernelCreateCond(Ptr<const char> name, u32 attr, i32 mutex, Ptr<const void>) {
    return kernel().create_cond(str(name, 31), attr, mutex);
}
i32 sceKernelDeleteCond(i32 uid) { return kernel().delete_cond(uid); }
i32 sceKernelWaitCond(i32 uid, Ptr<u32> t) { return kernel().wait_cond(uid, timeout(t), false); }
i32 sceKernelWaitCondCB(i32 uid, Ptr<u32> t) { return kernel().wait_cond(uid, timeout(t), true); }
i32 sceKernelSignalCond(i32 uid) { return kernel().signal_cond(uid, false, 0); }
i32 sceKernelSignalCondAll(i32 uid) { return kernel().signal_cond(uid, true, 0); }
i32 sceKernelSignalCondTo(i32 uid, i32 thread) { return kernel().signal_cond(uid, false, thread); }

i32 lw_cond(u32 work) {
    const Uid uid = work ? kernel().lw_uid(work) : 0;
    return uid ? uid : err::kUnknownLwCondId;
}

i32 sceKernelCreateLwCond(Ptr<u8> work, Ptr<const char> name, u32 attr, Ptr<u8> mutex_work, Ptr<const void>) {
    if (!writable(work.address, 16)) return err::kIllegalAddress;
    const i32 mutex = lw_mutex(mutex_work.address);
    if (mutex < 0) return mutex;
    const i32 uid = kernel().create_cond(str(name, 31), attr, mutex, ObjectClass::LwCond, work.address);
    if (uid < 0) return uid;
    std::memset(work.get(), 0, 16);
    std::memcpy(work.get(), &uid, 4);
    return err::kOk;
}
i32 sceKernelDeleteLwCond(Ptr<u8> work) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().delete_cond(uid, ObjectClass::LwCond);
}
i32 sceKernelWaitLwCond(Ptr<u8> work, Ptr<u32> t) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().wait_cond(uid, timeout(t), false, ObjectClass::LwCond);
}
i32 sceKernelWaitLwCondCB(Ptr<u8> work, Ptr<u32> t) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().wait_cond(uid, timeout(t), true, ObjectClass::LwCond);
}
i32 sceKernelSignalLwCond(Ptr<u8> work) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().signal_cond(uid, false, 0, ObjectClass::LwCond);
}
i32 sceKernelSignalLwCondAll(Ptr<u8> work) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().signal_cond(uid, true, 0, ObjectClass::LwCond);
}
i32 sceKernelSignalLwCondTo(Ptr<u8> work, i32 thread) {
    const i32 uid = lw_cond(work.address);
    if (uid < 0) return uid;
    return kernel().signal_cond(uid, false, thread, ObjectClass::LwCond);
}

// ---- callbacks

i32 sceKernelCreateCallback(Ptr<const char> name, u32 attr, u32 function, u32 common) {
    return kernel().create_callback(str(name, 31), attr, function, common);
}
i32 sceKernelDeleteCallback(i32 uid) { return kernel().delete_callback(uid); }
i32 sceKernelNotifyCallback(i32 uid, i32 arg) { return kernel().notify_callback(uid, arg); }
i32 sceKernelCancelCallback(i32 uid) { return kernel().cancel_callback(uid); }
i32 sceKernelGetCallbackCount(i32 uid) { return kernel().callback_count(uid); }
i32 sceKernelCheckCallback() { return kernel().check_callbacks(); }

// ---- time (microseconds since the process started)

i32 sceKernelGetProcessTime(Ptr<u64> clock) {
    if (!writable(clock.address, 8)) return err::kIllegalAddress;
    const u64 now = kernel().process_time();
    std::memcpy(clock.get(), &now, 8);
    return err::kOk;
}
u32 sceKernelGetProcessTimeLow() { return static_cast<u32>(kernel().process_time()); }
u64 sceKernelGetProcessTimeWide() { return kernel().process_time(); }
u64 sceKernelGetSystemTimeWide() { return kernel().process_time(); }

// ---- timers: counters of microseconds of the process's time, counting while started

struct Timer {
    nx::string name;
    bool active = false;
    u64 base_us = 0;     // (the process time it started at, while active)
    u64 counted_us = 0;  // (counted before it last stopped)
    u64 now(u64 t) const { return counted_us + (active ? t - base_us : 0); }
};
struct Timers {
    nx::mutex lock;
    nx::hash_map<i32, Timer> timers;
    i32 next = 0x40090001;
};
Timers& timers() {
    static Timers t;
    return t;
}

i32 sceKernelCreateTimer(Ptr<const char> name, u32 /*attr*/, Ptr<const void> /*opt*/) {
    Timers& t = timers();
    nx::scoped_lock<nx::mutex> held(t.lock);
    const i32 uid = t.next;
    t.next += 2;
    t.timers[uid].name = str(name, 31);
    return uid;
}
i32 sceKernelDeleteTimer(i32 uid) {
    Timers& t = timers();
    nx::scoped_lock<nx::mutex> held(t.lock);
    return t.timers.erase(uid) ? err::kOk : err::code(0x80028241);  // (UNKNOWN_TIMER_ID)
}
// 0 started, 1 already counting.
i32 sceKernelStartTimer(i32 uid) {
    Timers& t = timers();
    nx::scoped_lock<nx::mutex> held(t.lock);
    auto it = t.timers.find(uid);
    if (it == t.timers.end()) return err::code(0x80028241);
    if (it->second.active) return 1;
    it->second.active = true;
    it->second.base_us = kernel().process_time();
    return 0;
}
// 1 stopped, 0 already stopped.
i32 sceKernelStopTimer(i32 uid) {
    Timers& t = timers();
    nx::scoped_lock<nx::mutex> held(t.lock);
    auto it = t.timers.find(uid);
    if (it == t.timers.end()) return err::code(0x80028241);
    if (!it->second.active) return 0;
    it->second.counted_us = it->second.now(kernel().process_time());
    it->second.active = false;
    return 1;
}
// Its count in microseconds (-1: no such timer).
u64 sceKernelGetTimerTimeWide(i32 uid) {
    Timers& t = timers();
    nx::scoped_lock<nx::mutex> held(t.lock);
    auto it = t.timers.find(uid);
    return it == t.timers.end() ? ~u64{0} : it->second.now(kernel().process_time());
}

const Function kLibKernel[] = {
    HLE_FUNCTION(0xC5C11EE7, sceKernelCreateThread),
    HLE_FUNCTION(0xF08DE149, sceKernelStartThread),
    HLE_FUNCTION(0xDDB395A9, sceKernelWaitThreadEnd),
    HLE_FUNCTION(0xC54941ED, sceKernelWaitThreadEndCB),
    HLE_FUNCTION(0x0FB972F9, sceKernelGetThreadId),
    HLE_FUNCTION(0xC53A9848, sceKernelGetThreadCurrentPriority),
    HLE_FUNCTION(0x8D9C5461, sceKernelGetThreadInfo),
    HLE_FUNCTION(0xD5DC26C4, sceKernelGetThreadExitStatus),
    HLE_FUNCTION(0x8C57AC2A, sceKernelGetThreadCpuAffinityMask),  // (sceKernelGetThreadCpuAffinityMask2)
    HLE_FUNCTION(0x84092FD0, sceKernelCheckThreadStack),
    HLE_FUNCTION(0xA37A6057, sceKernelGetCurrentThreadVfpException),
    HLE_FUNCTION(0xADCA94E5, sceKernelWaitSignal),
    HLE_FUNCTION(0xB295EB61, sceKernelGetTLSAddr),
    HLE_FUNCTION(0x6D8C0F13, sceKernelRegisterThreadEventHandler),
    HLE_FUNCTION(0x1BD67366, sceKernelCreateSema),
    HLE_FUNCTION(0x0C7B834B, sceKernelWaitSema),
    HLE_FUNCTION(0x174692B4, sceKernelWaitSemaCB),
    HLE_FUNCTION(0x66D6BF05, sceKernelCancelSema),
    HLE_FUNCTION(0x595D3FA6, sceKernelGetSemaInfo),
    HLE_FUNCTION(0x8516D040, sceKernelCreateEventFlag),
    HLE_FUNCTION(0x83C0E2AF, sceKernelWaitEventFlag),
    HLE_FUNCTION(0xE737B1DF, sceKernelWaitEventFlagCB),
    HLE_FUNCTION(0x1FBB0FE1, sceKernelPollEventFlag),
    HLE_FUNCTION(0x2A12D9B7, sceKernelCancelEventFlag),
    HLE_FUNCTION(0x8BA4C0C1, sceKernelGetEventFlagInfo),
    HLE_FUNCTION(0xED53334A, sceKernelCreateMutex),
    HLE_FUNCTION(0x1D8D7945, sceKernelLockMutex),
    HLE_FUNCTION(0x2BDAA524, sceKernelLockMutexCB),
    HLE_FUNCTION(0x2144890D, sceKernelCancelMutex),
    HLE_FUNCTION(0x9A6C43CA, sceKernelGetMutexInfo),
    HLE_FUNCTION(0x50572FDA, sceKernelCreateCond),
    HLE_FUNCTION(0xC88D44AD, sceKernelWaitCond),
    HLE_FUNCTION(0x4CE42CE2, sceKernelWaitCondCB),
    HLE_FUNCTION(0xDA6EC8EF, sceKernelCreateLwMutex),
    HLE_FUNCTION(0x244E76D2, sceKernelDeleteLwMutex),
    HLE_FUNCTION(0x46E7BE7B, sceKernelLockLwMutex),
    HLE_FUNCTION(0x3148C6B6, sceKernelLockLwMutexCB),
    HLE_FUNCTION(0xA6A2C915, sceKernelTryLockLwMutex),
    HLE_FUNCTION(0x91FA6614, sceKernelUnlockLwMutex),
    HLE_FUNCTION(0xF7D8F1FC, sceKernelGetLwMutexInfo),
    HLE_FUNCTION(0x4846613D, sceKernelGetLwMutexInfoById),
    HLE_FUNCTION(0x48C7EAE6, sceKernelCreateLwCond),
    HLE_FUNCTION(0x721F6CB3, sceKernelDeleteLwCond),
    HLE_FUNCTION(0xE1878282, sceKernelWaitLwCond),
    HLE_FUNCTION(0x8FA54B07, sceKernelWaitLwCondCB),
    HLE_FUNCTION(0x3AC63B9A, sceKernelSignalLwCond),
    HLE_FUNCTION(0xE5241A0C, sceKernelSignalLwCondAll),
    HLE_FUNCTION(0xFC1A48EB, sceKernelSignalLwCondTo),
    HLE_FUNCTION(0x4C4672BF, sceKernelGetProcessTime),
    HLE_FUNCTION(0xE9F973B1, sceKernelGetProcessTimeLow),
    HLE_FUNCTION(0xB110C123, sceKernelGetProcessTimeWide),
    HLE_FUNCTION(0x2255B2A5, sceKernelCreateTimer),
};

const Function kThreadmgr[] = {
    HLE_FUNCTION(0x1D17DECF, sceKernelExitDeleteThread),
    HLE_FUNCTION(0x1BBDE3D9, sceKernelDeleteThread),
    HLE_FUNCTION(0x4B675D05, sceKernelDelayThread),
    HLE_FUNCTION(0x9C0180E1, sceKernelDelayThreadCB),
    HLE_FUNCTION(0xBD0139F2, sceKernelChangeThreadPriority),
    HLE_FUNCTION(0xC5FFCA61, sceKernelChangeThreadPriority),  // (sceKernelChangeThreadPriority2)
    HLE_FUNCTION(0x15129174, sceKernelChangeThreadCpuAffinityMask),
    HLE_FUNCTION(0xF1AE5654, sceKernelGetThreadCpuAffinityMask),
    HLE_FUNCTION(0x9DCB4B7A, sceKernelGetProcessId),
    HLE_FUNCTION(0xCC18FBAE, sceKernelChangeThreadVfpException),
    HLE_FUNCTION(0xD4C367B2, sceKernelSendSignal),
    HLE_FUNCTION(0x2C8ED6F0, sceKernelUnregisterThreadEventHandler),
    HLE_FUNCTION(0xDB32948A, sceKernelDeleteSema),
    HLE_FUNCTION(0x866EF048, sceKernelPollSema),
    HLE_FUNCTION(0xE6B761D1, sceKernelSignalSema),
    HLE_FUNCTION(0x5840162C, sceKernelDeleteEventFlag),
    HLE_FUNCTION(0xEC94DFF7, sceKernelSetEventFlag),
    HLE_FUNCTION(0x4CB87CA7, sceKernelClearEventFlag),
    HLE_FUNCTION(0xCB78710D, sceKernelDeleteMutex),
    HLE_FUNCTION(0x72FC1F54, sceKernelTryLockMutex),
    HLE_FUNCTION(0x1A372EC8, sceKernelUnlockMutex),
    HLE_FUNCTION(0x879E6EBD, sceKernelDeleteCond),
    HLE_FUNCTION(0x6ED2E2DC, sceKernelSignalCond),
    HLE_FUNCTION(0xC2E7AC22, sceKernelSignalCondAll),
    HLE_FUNCTION(0x1269F4EC, sceKernelSignalCondTo),
    HLE_FUNCTION(0xB19CF7E9, sceKernelCreateCallback),
    HLE_FUNCTION(0xD469676B, sceKernelDeleteCallback),
    HLE_FUNCTION(0xA4683592, sceKernelNotifyCallback),
    HLE_FUNCTION(0x30741EF2, sceKernelCancelCallback),
    HLE_FUNCTION(0x038644D5, sceKernelGetCallbackCount),
    HLE_FUNCTION(0xE53E41F6, sceKernelCheckCallback),
    HLE_FUNCTION(0xF4EE4FA9, sceKernelGetSystemTimeWide),
    HLE_FUNCTION(0xAB1E42C4, sceKernelDeleteTimer),
    HLE_FUNCTION(0x48091E0C, sceKernelStartTimer),
    HLE_FUNCTION(0x869E9F20, sceKernelStopTimer),
    HLE_FUNCTION(0x3EFD3165, sceKernelGetTimerTimeWide),
};

const Function kThreadmgrCoredumpTime[] = {
    HLE_FUNCTION(0x0C8A38E1, sceKernelExitThread),
};

}  // namespace

void register_threadmgr() {
    {
        // (a new runtime starts with no timers)
        nx::scoped_lock<nx::mutex> held(timers().lock);
        timers().timers.clear();
    }
    register_library({"SceLibKernel", kSceLibKernel, kLibKernel});
    register_library({"SceThreadmgr", kSceThreadmgr, kThreadmgr});
    register_library({"SceThreadmgrCoredumpTime", kSceThreadmgrCoredumpTime, kThreadmgrCoredumpTime});
}

}  // namespace psvrecore::runtime::hle
