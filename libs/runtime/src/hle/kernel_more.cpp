// The rest of the kernel's user library (SceLibKernel) and thread manager (SceThreadmgr):
// - atomics on guest memory, 8 to 64 bits (the host's atomic operations: the guest's threads are host threads);
// - the C library helpers: strstr, tolower/toupper, a constant-time compare, the checked copies (the size of the
//   destination last, as in the SDK's memcpy one: a copy that would overrun it fails without writing), printing to a
//   file descriptor. The checked printf forms' arguments are in no header: they fail without writing (-1);
// - simple events: polled, pulsed, cancelled, set with notification of the callbacks registered to them, several
//   waited on at once (by polling every millisecond, callbacks run while waiting in the CB form);
// - information about conditions, lightweight conditions, message pipes, reader-writer locks, events, callbacks;
//   the system (3 cores for the game, idle times not kept) and what runs on each core (the calling thread);
// - message pipe transfers of several buffers (each in turn);
// - a call on another stack; backtraces (none recorded: no frames); the performance monitor's user access (off);
//   the program is a game program; threads made with an options block, delays of at least 200 microseconds,
//   stack space left (the calling thread's from its stack pointer; another's: its whole stack), a UID's class,
//   event flags opened by name.

#include "hle_common.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceThreadmgr = 0x859A24B1;
using kernel::ObjectClass;

// ---- atomics

template <class T>
T* cell(u32 address) {
    return reinterpret_cast<T*>(memory().host(address));
}
template <class T> T add_and_get(u32 p, T v) { return __atomic_add_fetch(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T sub_and_get(u32 p, T v) { return __atomic_sub_fetch(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T and_and_get(u32 p, T v) { return __atomic_and_fetch(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T or_and_get(u32 p, T v) { return __atomic_or_fetch(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T xor_and_get(u32 p, T v) { return __atomic_xor_fetch(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T clear_and_get(u32 p, T v) { return __atomic_and_fetch(cell<T>(p), static_cast<T>(~v), __ATOMIC_SEQ_CST); }
template <class T> T get_and_add(u32 p, T v) { return __atomic_fetch_add(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T get_and_sub(u32 p, T v) { return __atomic_fetch_sub(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T get_and_and(u32 p, T v) { return __atomic_fetch_and(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T get_and_or(u32 p, T v) { return __atomic_fetch_or(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T get_and_xor(u32 p, T v) { return __atomic_fetch_xor(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> T get_and_clear(u32 p, T v) { return __atomic_fetch_and(cell<T>(p), static_cast<T>(~v), __ATOMIC_SEQ_CST); }
template <class T> T get_and_set(u32 p, T v) { return __atomic_exchange_n(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> void set(u32 p, T v) { __atomic_store_n(cell<T>(p), v, __ATOMIC_SEQ_CST); }
template <class T> void clear_mask(u32 p, T v) { __atomic_fetch_and(cell<T>(p), static_cast<T>(~v), __ATOMIC_SEQ_CST); }
// (the old value)
template <class T> T compare_and_set(u32 p, T expected, T desired) {
    __atomic_compare_exchange_n(cell<T>(p), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
// (added unless it holds `unless`: whether it was)
template <class T> u32 add_unless(u32 p, T v, T unless) {
    T old = __atomic_load_n(cell<T>(p), __ATOMIC_SEQ_CST);
    do {
        if (old == unless) return 0;
    } while (!__atomic_compare_exchange_n(cell<T>(p), &old, static_cast<T>(old + v), true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return 1;
}
// (decremented when positive: the old value less one, whether stored or not)
template <class T> T dec_if_positive(u32 p) {
    T old = __atomic_load_n(cell<T>(p), __ATOMIC_SEQ_CST);
    do {
        if (old <= 0) return static_cast<T>(old - 1);
    } while (!__atomic_compare_exchange_n(cell<T>(p), &old, static_cast<T>(old - 1), true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return static_cast<T>(old - 1);
}

// ---- the C library's helpers

nx::string cstr(u32 at, u32 most = 1 << 16) {
    nx::string s;
    for (u32 i = 0; at && i < most && memory().valid(at + i, 1); ++i) {
        const char c = static_cast<char>(memory().host(at + i)[0]);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}
u32 sceClibStrstr(u32 s1, u32 s2) {
    const nx::string a = cstr(s1), b = cstr(s2);
    const usize at = std::string_view(a.data(), a.size()).find(std::string_view(b.data(), b.size()));
    return at == std::string_view::npos ? 0 : s1 + static_cast<u32>(at);
}
i32 sceClibTolower(i32 c) { return std::tolower(c & 0xFF); }
i32 sceClibToupper(i32 c) { return std::toupper(c & 0xFF); }
// (every byte compared, whatever differs first: 0 when equal)
i32 sceClibMemcmpConstTime(u32 a, u32 b, u32 n) {
    if (!n) return 0;
    if (!memory().valid(a, n) || !memory().valid(b, n)) return 1;
    u8 diff = 0;
    for (u32 i = 0; i < n; ++i) diff |= static_cast<u8>(memory().host(a)[i] ^ memory().host(b)[i]);
    return diff ? 1 : 0;
}
u32 sceClibMemcpyChk(u32 dst, u32 src, u32 n, u32 dst_size) {
    if (n > dst_size || !memory().valid(dst, n) || !memory().valid(src, n)) return 0;
    std::memcpy(memory().host(dst), memory().host(src), n);
    return dst;
}
u32 sceClibMemmoveChk(u32 dst, u32 src, u32 n, u32 dst_size) {
    if (n > dst_size || !memory().valid(dst, n) || !memory().valid(src, n)) return 0;
    std::memmove(memory().host(dst), memory().host(src), n);
    return dst;
}
u32 sceClibMemsetChk(u32 dst, i32 c, u32 n, u32 dst_size) {
    if (n > dst_size || !memory().valid(dst, n)) return 0;
    std::memset(memory().host(dst), c, n);
    return dst;
}
// (a string written at `dst`, within `dst_size` with its terminator)
bool put(u32 dst, u32 dst_size, const nx::string& s) {
    if (s.size() + 1 > dst_size || !memory().valid(dst, static_cast<u32>(s.size() + 1))) return false;
    std::memcpy(memory().host(dst), s.c_str(), s.size() + 1);
    return true;
}
u32 sceClibStrcpyChk(u32 dst, u32 src, u32 dst_size) { return put(dst, dst_size, cstr(src)) ? dst : 0; }
u32 sceClibStrcatChk(u32 dst, u32 src, u32 dst_size) { return put(dst, dst_size, cstr(dst) + cstr(src)) ? dst : 0; }
u32 sceClibStrncpyChk(u32 dst, u32 src, u32 n, u32 dst_size) {
    if (n > dst_size || !memory().valid(dst, n)) return 0;
    const nx::string s = cstr(src, n);
    std::memset(memory().host(dst), 0, n);
    std::memcpy(memory().host(dst), s.data(), s.size());
    return dst;
}
u32 sceClibStrncatChk(u32 dst, u32 src, u32 n, u32 dst_size) { return put(dst, dst_size, cstr(dst) + cstr(src, n)) ? dst : 0; }
// (strlcpy/strlcat: what fits in n, terminated; the length it tried to make)
u32 sceClibStrlcpyChk(u32 dst, u32 src, u32 n, u32 dst_size) {
    const nx::string s = cstr(src);
    if (n > dst_size) return 0;
    if (n) put(dst, n, s.substr(0, std::min<usize>(s.size(), n - 1)));
    return static_cast<u32>(s.size());
}
u32 sceClibStrlcatChk(u32 dst, u32 src, u32 n, u32 dst_size) {
    const nx::string d = cstr(dst, n), s = cstr(src);
    if (n > dst_size) return 0;
    const nx::string both = d + s;
    if (n) put(dst, n, both.substr(0, std::min<usize>(both.size(), n - 1)));
    return static_cast<u32>(both.size());
}
i32 printf_chk_unknown() { return -1; }
// ---- simple events

i32 sceKernelPollEvent(i32 uid, u32 bits, Ptr<u32> result, Ptr<u64> user_data) {
    u32 got = 0;
    u64 data = 0;
    const i32 r = kernel().poll_event(uid, bits, &got, &data);
    if (r == 0 && result && writable(result.address, 4)) *result = got;
    if (r == 0 && user_data && writable(user_data.address, 8)) std::memcpy(user_data.get(), &data, 8);
    return r;
}
i32 sceKernelGetEventPattern(i32 uid, Ptr<u32> pattern) {
    if (!pattern || !writable(pattern.address, 4)) return err::kIllegalAddress;
    Kernel::ObjectSummary o;
    if (kernel().object_summary(uid, ObjectClass::SimpleEvent, &o) < 0) return err::kUnknownEventId;
    *pattern = o.a;
    return 0;
}
i32 sceKernelCancelEvent(i32 uid, Ptr<u32> waiting) {
    u32 n = 0;
    const i32 r = kernel().cancel_event(uid, false, 0, 0, &n);
    if (r == 0 && waiting && writable(waiting.address, 4)) *waiting = n;
    return r;
}
i32 sceKernelCancelEventWithSetPattern(i32 uid, u32 pattern, u64 user_data, Ptr<u32> waiting) {
    u32 n = 0;
    const i32 r = kernel().cancel_event(uid, true, pattern, user_data, &n);
    if (r == 0 && waiting && writable(waiting.address, 4)) *waiting = n;
    return r;
}
i32 sceKernelPulseEvent(i32 uid, u32 bits, u64 user_data) { return kernel().pulse_event(uid, bits, user_data); }
i32 sceKernelSetEventWithNotifyCallback(i32 uid, u32 bits, u64 user_data, i32 arg) {
    const i32 r = kernel().set_event(uid, bits, user_data);
    return r < 0 ? r : kernel().notify_event_callbacks(uid, arg);
}
i32 sceKernelPulseEventWithNotifyCallback(i32 uid, u32 bits, u64 user_data, i32 arg) {
    const i32 r = kernel().pulse_event(uid, bits, user_data);
    return r < 0 ? r : kernel().notify_event_callbacks(uid, arg);
}
i32 sceKernelRegisterCallbackToEvent(i32 event, i32 callback) { return kernel().register_event_callback(event, callback, true); }
i32 sceKernelUnregisterCallbackFromEvent(i32 event, i32 callback) { return kernel().register_event_callback(event, callback, false); }
i32 sceKernelUnregisterCallbackFromEventAll(i32 event) { return kernel().unregister_event_callbacks(event); }
// SceKernelWaitEvent (ID, pattern) in; SceKernelResultEvent (ID, result, pattern, reserved, user data) out. OR: until
// one is met; AND: all of them.
i32 wait_multiple(u32 list, u32 count, u32 mode, u32 results, Ptr<u32> t, bool callbacks) {
    if (!count || !memory().valid(list, count * 8) || (results && !writable(results, count * 24))) return err::kIllegalAddress;
    if (mode != 1 && mode != 2) return err::kInvalidArgument;
    u64 left = t && writable(t.address, 4) ? u64{*t} : ~u64{0};
    for (;;) {
        u32 met = 0;
        for (u32 i = 0; i < count; ++i) {
            const i32 uid = static_cast<i32>(psv_ld32(list + 8 * i));
            u32 got = 0;
            u64 data = 0;
            Kernel::ObjectSummary o;
            if (kernel().object_summary(uid, ObjectClass::SimpleEvent, &o) < 0) return err::kUnknownEventId;
            got = o.a & psv_ld32(list + 8 * i + 4), data = o.e;
            if (got) ++met;
            if (results) {
                psv_st32(results + 24 * i, static_cast<u32>(uid));
                psv_st32(results + 24 * i + 4, got ? 0u : static_cast<u32>(err::kEventCond));
                psv_st32(results + 24 * i + 8, got);
                std::memcpy(memory().host(results + 24 * i + 16), &data, 8);
            }
        }
        if ((mode == 1 && met) || (mode == 2 && met == count)) {
            if (t && writable(t.address, 4)) *t = static_cast<u32>(left == ~u64{0} ? 0 : left);
            return 0;
        }
        if (left == 0) return err::kWaitTimeout;
        const u32 step = static_cast<u32>(std::min<u64>(left, 1000));
        if (const i32 r = kernel().delay_thread(step, callbacks); r < 0) return r;
        if (left != ~u64{0}) left -= step;
        if (t && writable(t.address, 4) && left != ~u64{0}) *t = static_cast<u32>(left);
    }
}
i32 sceKernelWaitMultipleEvents(u32 list, u32 count, u32 mode, u32 results, Ptr<u32> t) {
    return wait_multiple(list, count, mode, results, t, false);
}
i32 sceKernelWaitMultipleEventsCB(u32 list, u32 count, u32 mode, u32 results, Ptr<u32> t) {
    return wait_multiple(list, count, mode, results, t, true);
}

// ---- information

// (the common start: size, UID, name (32), attributes)
bool info_head(u32 info, u32 size, i32 uid, const Kernel::ObjectSummary& o) {
    if (!info || !writable(info, size)) return false;
    std::memset(memory().host(info), 0, size);
    psv_st32(info, size);
    psv_st32(info + 4, static_cast<u32>(uid));
    std::memcpy(memory().host(info + 8), o.name.c_str(), std::min<usize>(o.name.size(), 31));
    psv_st32(info + 40, o.attr);
    return true;
}
template <ObjectClass C, i32 Unknown>
bool summary(i32 uid, Kernel::ObjectSummary* o, i32* r) {
    *r = kernel().object_summary(uid, C, o) < 0 ? Unknown : 0;
    return *r == 0;
}
i32 sceKernelGetEventInfo(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::SimpleEvent, err::kUnknownEventId>(uid, &o, &r)) return r;
    if (!info_head(info, 64, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.a);
    std::memcpy(memory().host(info + 48), &o.e, 8);
    psv_st32(info + 56, o.b);
    return 0;
}
i32 sceKernelGetCondInfo(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::Cond, err::kUnknownCondId>(uid, &o, &r)) return r;
    if (!info_head(info, 52, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.a);
    psv_st32(info + 48, o.b);
    return 0;
}
i32 lw_cond_info(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::LwCond, err::kUnknownLwCondId>(uid, &o, &r)) return r;
    if (!info_head(info, 56, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.c);  // (its work area)
    psv_st32(info + 48, 0);    // (the lightweight mutex's work area: not kept)
    psv_st32(info + 52, o.b);
    return 0;
}
i32 sceKernelGetLwCondInfoById(i32 uid, u32 info) { return lw_cond_info(uid, info); }
i32 sceKernelGetLwCondInfo(u32 work, u32 info) {
    const Uid uid = work ? kernel().lw_uid(work) : 0;
    return uid ? lw_cond_info(uid, info) : err::kUnknownLwCondId;
}
i32 sceKernelGetMsgPipeInfo(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::MsgPipe, err::kUnknownMsgPipeId>(uid, &o, &r)) return r;
    if (!info_head(info, 60, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.a), psv_st32(info + 48, o.b), psv_st32(info + 52, o.c), psv_st32(info + 56, o.d);
    return 0;
}
i32 sceKernelGetRWLockInfo(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::RWLock, err::kUnknownRWLockId>(uid, &o, &r)) return r;
    if (!info_head(info, 60, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.a), psv_st32(info + 48, o.b), psv_st32(info + 52, o.c), psv_st32(info + 56, o.d);
    return 0;
}
i32 sceKernelGetCallbackInfo(i32 uid, u32 info) {
    Kernel::ObjectSummary o;
    i32 r;
    if (!summary<ObjectClass::Callback, err::kUnknownCallbackId>(uid, &o, &r)) return r;
    if (!info_head(info, 68, uid, o)) return err::kIllegalAddress;
    psv_st32(info + 44, o.a), psv_st32(info + 48, o.b);
    psv_st32(info + 56, o.d), psv_st32(info + 60, static_cast<u32>(o.e)), psv_st32(info + 64, o.c);
    return 0;
}
// SceKernelSystemInfo: size, active cores, then per core (4) idle time, idle exits, thread switches (not kept: 0).
i32 sceKernelGetSystemInfo(u32 info) {
    if (!info || !writable(info, 72)) return err::kIllegalAddress;
    std::memset(memory().host(info), 0, 72);
    psv_st32(info, 72);
    psv_st32(info + 4, 0x7);
    return 0;
}
// SceKernelThreadRunStatus: size, then per core (4) process, thread, priority: the calling thread on core 0.
i32 sceKernelGetThreadRunStatus(u32 status) {
    if (!status || !writable(status, 52)) return err::kIllegalAddress;
    std::memset(memory().host(status), 0, 52);
    psv_st32(status, 52);
    psv_st32(status + 8, static_cast<u32>(kernel().current_thread_id()));
    psv_st32(status + 12, static_cast<u32>(kernel().current_priority()));
    return 0;
}

// ---- message pipes: several buffers (SceKernelMsgPipeVector: base, size), each in turn

i32 pipe_vector(i32 uid, u32 vectors, u32 count, u32 mode, Ptr<u32> result, Ptr<u32> t, bool send, bool callbacks, bool try_only) {
    if (!count || !memory().valid(vectors, count * 8)) return err::kIllegalAddress;
    u32 total = 0;
    i32 r = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 base = psv_ld32(vectors + 8 * i), size = psv_ld32(vectors + 8 * i + 4);
        u32 moved = 0;
        kernel::Timeout timeout = try_only ? kernel::Timeout{} : kernel::Timeout{t.get()};
        if (try_only) {
            u32 zero = 0;
            timeout = kernel::Timeout{&zero};
            r = send ? kernel().send_msg_pipe(uid, base, size, mode, &moved, timeout, false)
                     : kernel().receive_msg_pipe(uid, base, size, mode, &moved, timeout, false);
            if (r == err::kWaitTimeout) r = send ? err::kMsgPipeFull : err::kMsgPipeEmpty;
        } else {
            r = send ? kernel().send_msg_pipe(uid, base, size, mode, &moved, timeout, callbacks)
                     : kernel().receive_msg_pipe(uid, base, size, mode, &moved, timeout, callbacks);
        }
        total += moved;
        if (r < 0 || moved < size) break;
    }
    if (result && writable(result.address, 4)) *result = total;
    return r < 0 ? r : 0;
}
i32 sceKernelSendMsgPipeVector(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return pipe_vector(uid, v, n, mode, result, t, true, false, false);
}
i32 sceKernelSendMsgPipeVectorCB(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return pipe_vector(uid, v, n, mode, result, t, true, true, false);
}
i32 sceKernelTrySendMsgPipeVector(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result) {
    return pipe_vector(uid, v, n, mode, result, Ptr<u32>(0), true, false, true);
}
i32 sceKernelReceiveMsgPipeVector(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return pipe_vector(uid, v, n, mode, result, t, false, false, false);
}
i32 sceKernelReceiveMsgPipeVectorCB(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result, Ptr<u32> t) {
    return pipe_vector(uid, v, n, mode, result, t, false, true, false);
}
i32 sceKernelTryReceiveMsgPipeVector(i32 uid, u32 v, u32 n, u32 mode, Ptr<u32> result) {
    return pipe_vector(uid, v, n, mode, result, Ptr<u32>(0), false, false, true);
}

// ---- threads and the rest

// A function called on another stack (its top: base + size), its result returned.
i32 sceKernelCallWithChangeStack(PsvContext* ctx, u32 base, u32 size, u32 function, u32 common) {
    if (!function || !size || !memory().valid(base, size)) return err::kIllegalAddress;
    const u32 sp = ctx->r[13];
    ctx->r[13] = (base + size) & ~7u;
    const u32 args[] = {common};
    const u32 r = call_guest(ctx, function, args);
    ctx->r[13] = sp;
    return static_cast<i32>(r);
}
i32 sceKernelBacktrace(i32, u32, u32, Ptr<u32> count, i32) {
    if (count && writable(count.address, 4)) *count = 0;
    return 0;
}
i32 sceKernelPrintBacktrace(u32, u32) { return 0; }
i32 sceKernelChangeCurrentThreadAttr(u32, u32) { return 0; }
u32 sceKernelGetPMUSERENR() { return 0; }
i32 sceSblACMgrIsGameProgram(Ptr<u32> result) {
    if (!result || !writable(result.address, 4)) return err::kIllegalAddress;
    *result = 1;
    return 0;
}
i32 sceKernelWaitSignalCB(u32, u32, Ptr<u32> t) { return kernel().wait_signal(timeout(t)); }
i32 sceKernelChangeActiveCpuMask(u32) { return 0; }
i32 sceKernelCheckWaitableStatus() { return 0; }
i32 sceKernelOpenEventFlag(Ptr<const char> name) {
    return kernel().open_object(str(name, 31), ObjectClass::EventFlag, err::kUnknownEventFlagId);
}
i32 sceKernelCloseEventFlag(i32 uid) { return kernel().close_object(uid, ObjectClass::EventFlag, err::kUnknownEventFlagId); }
// sceKernelCreateThreadForUser's options: size, stack size, attributes, affinity (bits 16-19), options, caller.
i32 sceKernelCreateThreadForUser(Ptr<const char> name, u32 entry, i32 priority, Ptr<const u32> opt) {
    if (!opt || !memory().valid(opt.address, 16)) return err::kIllegalAddress;
    const u32* o = opt.get();
    return kernel().create_thread(str(name, 31), entry, priority, o[1], o[2], static_cast<i32>(o[3]));
}
i32 sceKernelDelayThread200(u32 us) { return kernel().delay_thread(std::max<u32>(us, 200), false); }
i32 sceKernelDelayThreadCB200(u32 us) { return kernel().delay_thread(std::max<u32>(us, 200), true); }
i32 sceKernelGetThreadStackFreeSize(PsvContext* ctx, i32 uid) {
    Kernel::ThreadInfo info;
    const i32 id = uid ? uid : kernel().current_thread_id();
    if (kernel().thread_info(id, &info) < 0) return err::kUnknownThreadId;
    if (id != kernel().current_thread_id()) return static_cast<i32>(info.stack_size);
    const u32 sp = ctx->r[13];
    return sp > info.stack ? static_cast<i32>(sp - info.stack) : 0;
}
i32 sceKernelGetThreadmgrUIDClass(i32 uid) {
    const u32 c = kernel().class_of(uid);
    return c ? static_cast<i32>(c) : err::code(0x80028001);
}
void stack_check_failed(PsvContext* ctx) {
    nx::loge(logs::kernel, "the guest's stack protector found its stack overwritten (lr 0x{:08x})", ctx->r[14]);
    runtime().exit_process(134);
    kernel().exit_thread(134, false);
}

const Function kLibKernel[] = {
    Function{0x8F7BD940, "sceKernelAtomicAddAndGet8", &entry<&add_and_get<i8>>},
    Function{0x495C52EC, "sceKernelAtomicAddAndGet16", &entry<&add_and_get<i16>>},
    Function{0x2E84A93B, "sceKernelAtomicAddAndGet32", &entry<&add_and_get<i32>>},
    Function{0xB6CE9B9A, "sceKernelAtomicAddAndGet64", &entry<&add_and_get<i64>>},
    Function{0x99E1796E, "sceKernelAtomicSubAndGet8", &entry<&sub_and_get<i8>>},
    Function{0xC26BBBB1, "sceKernelAtomicSubAndGet16", &entry<&sub_and_get<i16>>},
    Function{0x01C9CD92, "sceKernelAtomicSubAndGet32", &entry<&sub_and_get<i32>>},
    Function{0x9BB4A94B, "sceKernelAtomicSubAndGet64", &entry<&sub_and_get<i64>>},
    Function{0x86B9170F, "sceKernelAtomicAndAndGet8", &entry<&and_and_get<i8>>},
    Function{0xF9890F7E, "sceKernelAtomicAndAndGet16", &entry<&and_and_get<i16>>},
    Function{0x6709D30C, "sceKernelAtomicAndAndGet32", &entry<&and_and_get<i32>>},
    Function{0xAED2B370, "sceKernelAtomicAndAndGet64", &entry<&and_and_get<i64>>},
    Function{0x51693931, "sceKernelAtomicOrAndGet8", &entry<&or_and_get<i8>>},
    Function{0x8E248EBD, "sceKernelAtomicOrAndGet16", &entry<&or_and_get<i16>>},
    Function{0xC3B2F7F8, "sceKernelAtomicOrAndGet32", &entry<&or_and_get<i32>>},
    Function{0x809BBC7D, "sceKernelAtomicOrAndGet64", &entry<&or_and_get<i64>>},
    Function{0x634AF062, "sceKernelAtomicXorAndGet8", &entry<&xor_and_get<i8>>},
    Function{0x6F524195, "sceKernelAtomicXorAndGet16", &entry<&xor_and_get<i16>>},
    Function{0x46940704, "sceKernelAtomicXorAndGet32", &entry<&xor_and_get<i32>>},
    Function{0xDDC6866E, "sceKernelAtomicXorAndGet64", &entry<&xor_and_get<i64>>},
    Function{0x17B40010, "sceKernelAtomicClearAndGet8", &entry<&clear_and_get<i8>>},
    Function{0xCC36B775, "sceKernelAtomicClearAndGet16", &entry<&clear_and_get<i16>>},
    Function{0x9DAA0DE8, "sceKernelAtomicClearAndGet32", &entry<&clear_and_get<i32>>},
    Function{0x264209F3, "sceKernelAtomicClearAndGet64", &entry<&clear_and_get<i64>>},
    Function{0x27A2AAFA, "sceKernelAtomicGetAndAdd8", &entry<&get_and_add<i8>>},
    Function{0x5674DB0C, "sceKernelAtomicGetAndAdd16", &entry<&get_and_add<i16>>},
    Function{0x2611CB0B, "sceKernelAtomicGetAndAdd32", &entry<&get_and_add<i32>>},
    Function{0x63DAF37D, "sceKernelAtomicGetAndAdd64", &entry<&get_and_add<i64>>},
    Function{0xCDF5DF67, "sceKernelAtomicGetAndSub8", &entry<&get_and_sub<i8>>},
    Function{0xAC51979C, "sceKernelAtomicGetAndSub16", &entry<&get_and_sub<i16>>},
    Function{0x115C516F, "sceKernelAtomicGetAndSub32", &entry<&get_and_sub<i32>>},
    Function{0x4AE9C8E6, "sceKernelAtomicGetAndSub64", &entry<&get_and_sub<i64>>},
    Function{0x53DCA02B, "sceKernelAtomicGetAndAnd8", &entry<&get_and_and<i8>>},
    Function{0x7A0CB056, "sceKernelAtomicGetAndAnd16", &entry<&get_and_and<i16>>},
    Function{0x08266595, "sceKernelAtomicGetAndAnd32", &entry<&get_and_and<i32>>},
    Function{0x4828BC43, "sceKernelAtomicGetAndAnd64", &entry<&get_and_and<i64>>},
    Function{0x107A68DF, "sceKernelAtomicGetAndOr8", &entry<&get_and_or<i8>>},
    Function{0x31E49E73, "sceKernelAtomicGetAndOr16", &entry<&get_and_or<i16>>},
    Function{0x984AD276, "sceKernelAtomicGetAndOr32", &entry<&get_and_or<i32>>},
    Function{0xC39186CD, "sceKernelAtomicGetAndOr64", &entry<&get_and_or<i64>>},
    Function{0x7350B2DF, "sceKernelAtomicGetAndXor8", &entry<&get_and_xor<i8>>},
    Function{0x6E2D0B9E, "sceKernelAtomicGetAndXor16", &entry<&get_and_xor<i16>>},
    Function{0x38739E2F, "sceKernelAtomicGetAndXor32", &entry<&get_and_xor<i32>>},
    Function{0x6A19BBE9, "sceKernelAtomicGetAndXor64", &entry<&get_and_xor<i64>>},
    Function{0xA10873A4, "sceKernelAtomicGetAndClear8", &entry<&get_and_clear<i8>>},
    Function{0x57CD7E7D, "sceKernelAtomicGetAndClear16", &entry<&get_and_clear<i16>>},
    Function{0x0226A017, "sceKernelAtomicGetAndClear32", &entry<&get_and_clear<i32>>},
    Function{0x436AA0B5, "sceKernelAtomicGetAndClear64", &entry<&get_and_clear<i64>>},
    Function{0x2672EA0B, "sceKernelAtomicGetAndSet8", &entry<&get_and_set<i8>>},
    Function{0xF7B9ADFC, "sceKernelAtomicGetAndSet16", &entry<&get_and_set<i16>>},
    Function{0x9B43E667, "sceKernelAtomicGetAndSet32", &entry<&get_and_set<i32>>},
    Function{0xD82B3A7A, "sceKernelAtomicGetAndSet64", &entry<&get_and_set<i64>>},
    Function{0xE960FDA2, "sceKernelAtomicSet8", &entry<&set<i8>>},
    Function{0x450BFECF, "sceKernelAtomicSet16", &entry<&set<i16>>},
    Function{0xB69DA09B, "sceKernelAtomicSet32", &entry<&set<i32>>},
    Function{0xC8A4339C, "sceKernelAtomicSet64", &entry<&set<i64>>},
    Function{0xD7D49E36, "sceKernelAtomicClearMask8", &entry<&clear_mask<i8>>},
    Function{0x5FE7DFF8, "sceKernelAtomicClearMask16", &entry<&clear_mask<i16>>},
    Function{0xE3DF0CB3, "sceKernelAtomicClearMask32", &entry<&clear_mask<i32>>},
    Function{0x953D118A, "sceKernelAtomicClearMask64", &entry<&clear_mask<i64>>},
    Function{0x327DB4C0, "sceKernelAtomicCompareAndSet8", &entry<&compare_and_set<i8>>},
    Function{0xE8C01236, "sceKernelAtomicCompareAndSet16", &entry<&compare_and_set<i16>>},
    Function{0x1124A1D4, "sceKernelAtomicCompareAndSet32", &entry<&compare_and_set<i32>>},
    Function{0x1EBDFCCD, "sceKernelAtomicCompareAndSet64", &entry<&compare_and_set<i64>>},
    Function{0x7FD94393, "sceKernelAtomicAddUnless8", &entry<&add_unless<i8>>},
    Function{0x1CF4AA4B, "sceKernelAtomicAddUnless16", &entry<&add_unless<i16>>},
    Function{0x4B33FD3C, "sceKernelAtomicAddUnless32", &entry<&add_unless<i32>>},
    Function{0xFFCE7438, "sceKernelAtomicAddUnless64", &entry<&add_unless<i64>>},
    Function{0x9DABE6C3, "sceKernelAtomicDecIfPositive8", &entry<&dec_if_positive<i8>>},
    Function{0x323718FB, "sceKernelAtomicDecIfPositive16", &entry<&dec_if_positive<i16>>},
    Function{0xCA3294F1, "sceKernelAtomicDecIfPositive32", &entry<&dec_if_positive<i32>>},
    Function{0x8BE2A007, "sceKernelAtomicDecIfPositive64", &entry<&dec_if_positive<i64>>},
    HLE_FUNCTION(0xE265498B, sceClibStrstr),
    HLE_FUNCTION(0x4C5471BC, sceClibTolower),
    HLE_FUNCTION(0xB0335388, sceClibToupper),
    HLE_FUNCTION(0x100091AF, sceClibMemcmpConstTime),
    HLE_FUNCTION(0x1F94EF45, sceClibMemcpyChk),
    HLE_FUNCTION(0xA18068FC, sceClibMemmoveChk),
    HLE_FUNCTION(0xD19B6DC5, sceClibMemsetChk),
    HLE_FUNCTION(0xB9B6B05A, sceClibStrcpyChk),
    HLE_FUNCTION(0x0F28A495, sceClibStrcatChk),
    HLE_FUNCTION(0x9CBE1475, sceClibStrncpyChk),
    HLE_FUNCTION(0x489EBEDF, sceClibStrncatChk),
    HLE_FUNCTION(0xAC6CF5DB, sceClibStrlcpyChk),
    HLE_FUNCTION(0x4CCB60A9, sceClibStrlcatChk),
    HLE_FUNCTION(0x241F3634, sceKernelPollEvent),
    HLE_FUNCTION(0xE5A2AC66, sceKernelGetEventPattern),
    HLE_FUNCTION(0x603AB770, sceKernelCancelEvent),
    HLE_FUNCTION(0xB01387D5, sceKernelCancelEventWithSetPattern),
    HLE_FUNCTION(0x5FF3EE92, sceKernelSetEventWithNotifyCallback),
    HLE_FUNCTION(0x92F162F3, sceKernelPulseEventWithNotifyCallback),
    HLE_FUNCTION(0x10586418, sceKernelWaitMultipleEvents),
    HLE_FUNCTION(0x4263DBC9, sceKernelWaitMultipleEventsCB),
    HLE_FUNCTION(0xE35F19D6, sceKernelGetEventInfo),
    HLE_FUNCTION(0x6864DCE2, sceKernelGetCondInfo),
    HLE_FUNCTION(0xE4DF36A0, sceKernelGetLwCondInfo),
    HLE_FUNCTION(0x971F1DE8, sceKernelGetLwCondInfoById),
    HLE_FUNCTION(0x4046D16B, sceKernelGetMsgPipeInfo),
    HLE_FUNCTION(0x079A573B, sceKernelGetRWLockInfo),
    HLE_FUNCTION(0x88DD1BC8, sceKernelGetCallbackInfo),
    HLE_FUNCTION(0xE0241FAA, sceKernelGetSystemInfo),
    HLE_FUNCTION(0xD6B01013, sceKernelGetThreadRunStatus),
    HLE_FUNCTION(0x94D506F7, sceKernelSendMsgPipeVector),
    HLE_FUNCTION(0x9C6F7F79, sceKernelSendMsgPipeVectorCB),
    HLE_FUNCTION(0x60DB346F, sceKernelTrySendMsgPipeVector),
    HLE_FUNCTION(0x9F899087, sceKernelReceiveMsgPipeVector),
    HLE_FUNCTION(0xBE5B3E27, sceKernelReceiveMsgPipeVectorCB),
    HLE_FUNCTION(0x86ECC0FF, sceKernelTryReceiveMsgPipeVector),
    HLE_FUNCTION(0xAFE2B300, sceKernelCallWithChangeStack),
    HLE_FUNCTION(0x894037E8, sceKernelBacktrace),
    HLE_FUNCTION(0x1C02A434, sceKernelPrintBacktrace),
    HLE_FUNCTION(0x36833F93, sceKernelChangeCurrentThreadAttr),
    HLE_FUNCTION(0xF8A99FDF, sceKernelGetPMUSERENR),
    HLE_FUNCTION(0x963F4A99, sceSblACMgrIsGameProgram),
    HLE_FUNCTION(0x24460BB3, sceKernelWaitSignalCB),
    Function{0xC2E1A945, "sceClibSnprintfChk", &entry<&printf_chk_unknown>},
    Function{0xB628786B, "sceClibVsnprintfChk", &entry<&printf_chk_unknown>},
    Function{0xEC287338, "sceKernelStackChkFail", &entry<&stack_check_failed>},
};
const Function kThreadmgr[] = {
    HLE_FUNCTION(0x8D27BAD6, sceKernelPulseEvent),
    HLE_FUNCTION(0x76FB37E9, sceKernelRegisterCallbackToEvent),
    HLE_FUNCTION(0x18462B11, sceKernelUnregisterCallbackFromEvent),
    HLE_FUNCTION(0x888A7361, sceKernelUnregisterCallbackFromEventAll),
    HLE_FUNCTION(0x001173F8, sceKernelChangeActiveCpuMask),
    HLE_FUNCTION(0xD9BD74EB, sceKernelCheckWaitableStatus),
    HLE_FUNCTION(0xBC19F8A1, sceKernelOpenEventFlag),
    HLE_FUNCTION(0x9A68F547, sceKernelCloseEventFlag),
    HLE_FUNCTION(0xC0FAF6A3, sceKernelCreateThreadForUser),
    HLE_FUNCTION(0x97C4A7C4, sceKernelDelayThread200),
    HLE_FUNCTION(0x34938242, sceKernelDelayThreadCB200),
    HLE_FUNCTION(0x4F8A3DA0, sceKernelGetThreadStackFreeSize),
    HLE_FUNCTION(0xC9678F7F, sceKernelGetThreadmgrUIDClass),
};
// (the stack protector's guard, as __stack_chk_guard: one fixed value)
const Variable kVariables[] = {
    {0x4458BCF3, "SceKernelStackChkGuard", 4, [](u8* host, u32) {
         const u32 guard = 0x5C3A9E17;
         std::memcpy(host, &guard, 4);
     }},
};

}  // namespace

void register_kernel_more() {
    register_library({"SceLibKernel", kSceLibKernel, kLibKernel, kVariables});
    register_library({"SceThreadmgr", kSceThreadmgr, kThreadmgr});
}

}  // namespace psvrecore::runtime::hle
