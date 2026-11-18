// Guest threads: creation, the host context each one runs on (through the thread backend), ending, waiting for an end, delays, priorities.

#include <algorithm>
#include <cstring>

#include "kernel_impl.hpp"
#include "psvrecore/profile.hpp"
#include "psvrecore/runtime/host.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "core/foundation/strings/format.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

using namespace kernel;

void set_current_thread(Thread* t);

namespace {

constexpr u32 kMaxThreads = 128;

// The host stack a guest thread's host context gets: recompiled functions use more stack than the guest's (their
// frames hold temporaries), and guest recursion is host recursion.
usize host_stack_size(u32 guest_stack) {
    const u64 size = std::max<u64>(u64{4} << 20, u64{guest_stack} * 16);
    return static_cast<usize>(std::min<u64>(size, u64{256} << 20));
}

}  // namespace

// ---- host side

void Kernel::Impl::run_thread(Thread* t) {
    set_current_thread(t);
    if (thread_stats) {
        const u64 n = nx::os::ticks_ns();
        Lock guard(lock);
        start_latency_us.push_back(static_cast<u32>(nx::min<u64>((n - t->start_called_ns) / 1000, ~0u)));
    }
    // (its own lane in the profiler, named after it, whichever host thread runs it)
    t->track = profile::acquire(t->name);
    profile::enter(t->track);
    {
        Lock guard(lock);
        wait_for_cpu(t);
    }
    i32 status = 0;
    bool delete_thread = false;
    bool ended = false;
    try {
        if (!t->terminate) {
            const PsvFunction entry = recomp::find_function(t->entry);
            if (!entry) {
                nx::loge(logs::kernel, "thread '{}': no recompiled function at its entry 0x{:08x}", t->name, t->entry);
                psv_trap(&t->ctx, t->entry, recomp::kTrapUnknownTarget);
            }
            entry(&t->ctx, hle::kHostReturn);
            status = static_cast<i32>(t->ctx.r[0]);
        }
        ended = true;
    } catch (const ThreadExit& e) {
        status = e.status;
        delete_thread = e.delete_thread;
        ended = true;
    }
    (void)ended;
    Lock guard(lock);
    profile::release(t->track);
    t->track = nullptr;
    profile::enter(nullptr);
    finish_thread(t, status, delete_thread);
    set_current_thread(nullptr);
    --host_threads;
    host_threads_changed.notify_all();
    // (t may be gone now)
}

void Kernel::Impl::finish_thread(Thread* t, i32 status, bool delete_thread) {
    unlock_owned_mutexes(t);
    release_cpu(t);
    ready.erase(std::remove(ready.begin(), ready.end(), t), ready.end());
    t->state = ThreadState::Dormant;
    t->exit_status = status;
    t->lost_cpu.store(false, std::memory_order_relaxed);
    backend->end(t);
    while (!t->end_waiters.empty()) {
        Thread* w = t->end_waiters.front();
        w->wait.restore_count = static_cast<u32>(status);  // (the exit status, for the waiter)
        end_wait(w, err::kOk);
    }
    if (delete_thread || t->delete_on_exit) {
        free_thread_memory(t);
        objects.erase(t->uid);
        --thread_count;
    }
    dispatch();
}

void Kernel::Impl::free_thread_memory(Thread* t) {
    for (Uid id : t->callbacks) {
        objects.erase(id);
        --sync_count;
    }
    t->callbacks.clear();
    if (t->stack) memory.unmap(t->stack);
    if (t->tls) memory.unmap(t->tls);
    t->stack = t->tls = 0;
}

void Kernel::Impl::log_thread_stats() {
    // (lock held)
    if (!thread_stats || start_latency_us.empty()) return;
    nx::vector<u32> sorted = start_latency_us;
    std::sort(sorted.begin(), sorted.end());
    u64 total = 0;
    for (u32 v : sorted) total += v;
    const f64 seconds = static_cast<f64>(nx::os::ticks_ns() - start_ns) / 1e9;
    const f64 span = static_cast<f64>(last_start_ns - first_start_ns) / 1e9;
    nx::logi(logs::kernel, "threads: {} started in {:.1f} s ({:.2f} a second; {:.1f} s from the first start to the last); "
             "from the start call to running on a host thread: mean {:.0f} us, median {}, 99% {}, worst {}; backend {}, "
             "{} host threads made",
             sorted.size(), seconds, static_cast<f64>(sorted.size()) / seconds, span,
             static_cast<f64>(total) / static_cast<f64>(sorted.size()), sorted[sorted.size() / 2],
             sorted[nx::min(sorted.size() - 1, sorted.size() * 99 / 100)], sorted.back(), backend->name(),
             backend->host_threads_made());
}

// ---- threads

Uid Kernel::create_thread(nx::string_view name, u32 entry, i32 priority, u32 stack_size, u32 attr, i32 affinity) {
    Lock guard(impl_->lock);
    const i32 resolved = resolve_priority(priority);
    if (resolved < 0) return err::kIllegalPriority;
    if (stack_size == 0) stack_size = kStackSizeMin;
    if (stack_size < kStackSizeMin || stack_size > kStackSizeMax) return err::kIllegalStackSize;
    stack_size = (stack_size + GuestMemory::kPageSize - 1) & ~(GuestMemory::kPageSize - 1);
    if (affinity != 0 && ((affinity & ~kAllUserCpus) != 0)) return err::kIllegalCpuAffinityMask;
    if (!entry) return err::kInvalidArgument;
    if (impl_->thread_count >= kMaxThreads) return err::kUidOverflow;

    auto t = nx::make_unique<Thread>();
    t->entry = entry;
    t->init_priority = t->priority = resolved;
    t->init_affinity = t->affinity = affinity;
    t->stack_size = stack_size;
    t->stack = memory_.map_anywhere(impl_->config.stack_region, stack_size, GuestMemory::kPageSize, Access::ReadWrite,
                                    BlockKind::Stack, name);
    if (!t->stack) return err::kNoMemory;
    t->tls = memory_.map_anywhere(impl_->config.stack_region, kTlsSize, GuestMemory::kPageSize, Access::ReadWrite,
                                  BlockKind::ThreadLocal, name);
    if (!t->tls) {
        memory_.unmap(t->stack);
        return err::kNoMemory;
    }
    t->exit_status = err::kDormant;
    Thread* raw = impl_->add(std::move(t), name, attr | 0x80000000u);  // (the console marks user threads so)
    ++impl_->thread_count;
    return raw->uid;
}

i32 Kernel::start_thread(Uid uid, u32 arg_size, u32 arg_block) {
    Lock guard(impl_->lock);
    Thread* t = impl_->get<Thread>(uid, ObjectClass::Thread);
    if (!t) return uid == 0 ? err::kIllegalThreadId : err::kUnknownThreadId;
    if (t->state != ThreadState::Dormant) return err::kNotDormant;
    if (impl_->shutting_down) return err::kError;
    if (arg_size && !arg_block) return err::kIllegalAddress;
    if (arg_size > t->stack_size / 2) return err::kInvalidArgument;

    // A fresh context: the argument block copied to the top of the stack.
    std::memset(&t->ctx, 0, sizeof t->ctx);
    u32 sp = t->stack + t->stack_size;
    u32 argp = 0;
    if (arg_size) {
        sp = (sp - arg_size) & ~7u;
        std::memmove(memory_.host(sp), memory_.host(arg_block), arg_size);
        argp = sp;
    }
    t->ctx.r[0] = arg_size;
    t->ctx.r[1] = argp;
    t->ctx.r[13] = sp & ~7u;
    t->ctx.r[14] = hle::kHostReturn;
    t->ctx.tpidruro = t->tls;
    t->ctx.thread = t;
    std::memset(memory_.host(t->tls), 0, kTlsSize);
    t->priority = t->init_priority;
    t->affinity = t->init_affinity;
    t->exit_status = err::kNotDormant;
    t->started_once = true;
    t->terminate = false;
    t->signal = 0;
    t->lost_cpu.store(false, std::memory_order_relaxed);
    if (impl_->thread_stats) {
        t->start_called_ns = nx::os::ticks_ns();
        if (!impl_->first_start_ns) impl_->first_start_ns = t->start_called_ns;
        impl_->last_start_ns = t->start_called_ns;
    }

    t->locals.clear();  // (a new run: nothing of the last)

    ++impl_->host_threads;
    impl_->backend->start(t, host_stack_size(t->stack_size));
    impl_->make_ready(t);
    return err::kOk;
}

void Kernel::exit_thread(i32 status, bool delete_thread) { throw ThreadExit{status, delete_thread}; }

i32 Kernel::delete_thread(Uid uid) {
    Lock guard(impl_->lock);
    Thread* t = impl_->get<Thread>(uid, ObjectClass::Thread);
    if (!t) return uid == 0 ? err::kIllegalThreadId : err::kUnknownThreadId;
    if (t == current()) return err::kIllegalThreadId;
    if (t->state != ThreadState::Dormant) return err::kNotDormant;
    impl_->free_thread_memory(t);
    impl_->objects.erase(uid);
    --impl_->thread_count;
    return err::kOk;
}

i32 Kernel::wait_thread_end(Uid uid, i32* status, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    Thread* self = current();
    Thread* t = impl_->get<Thread>(uid, ObjectClass::Thread);
    if (!t) return uid == 0 ? err::kIllegalThreadId : err::kUnknownThreadId;
    if (t == self) return err::kIllegalThreadId;
    const auto deadline = impl_->deadline(timeout);
    for (;;) {
        if (t->state == ThreadState::Dormant) {
            if (status) *status = t->exit_status;
            impl_->write_remaining(timeout, deadline);
            return err::kOk;
        }
        if (!self) {
            // A host thread (the process start-up) waits without being a guest thread.
            impl_->host_threads_changed.wait(impl_->lock);
            t = impl_->get<Thread>(uid, ObjectClass::Thread);
            if (!t) return err::kWaitDelete;
            continue;
        }
        if (deadline && impl_->now_ns() >= *deadline) {
            impl_->write_remaining(timeout, deadline);
            return err::kWaitTimeout;
        }
        self->wait = Wait{};
        self->wait.type = WaitType::ThreadEnd;
        self->wait.object = uid;
        t->end_waiters.add(self);
        const i32 r = impl_->block(self, &t->end_waiters, deadline, callbacks);
        if (r == kRunCallbacks) {
            impl_->run_callbacks(self);
            t = impl_->get<Thread>(uid, ObjectClass::Thread);
            if (!t) return err::kWaitDelete;
            continue;
        }
        impl_->write_remaining(timeout, deadline);
        if (r == err::kOk && status) *status = static_cast<i32>(self->wait.restore_count);
        return r;
    }
}

i32 Kernel::delay_thread(u32 microseconds, bool callbacks) {
    if (microseconds == 0) return err::kInvalidArgument;
    Lock guard(impl_->lock);
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    const u64 deadline = impl_->now_ns() + u64{microseconds} * 1000;
    for (;;) {
        self->wait = Wait{};
        self->wait.type = WaitType::Delay;
        const i32 r = impl_->block(self, nullptr, deadline, callbacks);
        if (r == kRunCallbacks) {
            impl_->run_callbacks(self);
            if (impl_->now_ns() >= deadline) return err::kOk;
            continue;
        }
        return r == err::kWaitTimeout ? err::kOk : r;
    }
}

i32 Kernel::change_priority(Uid uid, i32 priority) {
    Lock guard(impl_->lock);
    Thread* t = impl_->thread(uid);
    if (!t) return err::kUnknownThreadId;
    const i32 p = priority == 0 ? (current() ? current()->priority : -1) : resolve_priority(priority);
    if (p < 0) return err::kIllegalPriority;
    if (t->state == ThreadState::Dormant) return err::kDormant;
    t->priority = p;
    if (t->wait.queue && t->wait.queue->by_priority) {
        t->wait.queue->remove(t);
        t->wait.queue->add(t);
    }
    if (t->state == ThreadState::Running && t->cpu >= 0) {
        // A running thread whose priority dropped below a ready one's gives way at its next kernel call.
        bool outranked = false;
        for (Thread* r : impl_->ready)
            if (r->priority < p && Impl::allowed(r, t->cpu)) outranked = true;
        if (outranked) {
            impl_->release_cpu(t);
            impl_->make_ready(t, true);
            if (t->cpu < 0) t->lost_cpu.store(true, std::memory_order_release);
        }
    } else {
        impl_->dispatch();
    }
    return err::kOk;
}

i32 Kernel::change_affinity(Uid uid, i32 mask) {
    Lock guard(impl_->lock);
    Thread* t = impl_->thread(uid);
    if (!t) return err::kUnknownThreadId;
    if (mask != 0 && (mask & ~kAllUserCpus) != 0) return err::kIllegalCpuAffinityMask;
    t->affinity = mask;
    if (t->state == ThreadState::Running && t->cpu >= 0 && !Impl::allowed(t, t->cpu)) {
        impl_->release_cpu(t);
        impl_->make_ready(t, true);
        if (t->cpu < 0) t->lost_cpu.store(true, std::memory_order_release);
    } else {
        impl_->dispatch();
    }
    return err::kOk;
}

i32 Kernel::get_affinity(Uid uid) {
    Lock guard(impl_->lock);
    Thread* t = impl_->thread(uid);
    if (!t) return err::kUnknownThreadId;
    return t->affinity;
}

i32 Kernel::current_priority() {
    Thread* t = current();
    return t ? t->priority : err::kIllegalContext;
}

Uid Kernel::current_thread_id() {
    Thread* t = current();
    return t ? t->uid : err::kIllegalContext;
}

i32 Kernel::get_exit_status(Uid uid, i32* status) {
    Lock guard(impl_->lock);
    Thread* t = impl_->thread(uid);
    if (!t) return err::kUnknownThreadId;
    if (t->state != ThreadState::Dormant) return err::kNotDormant;
    if (!t->started_once) return err::kDormant;
    if (status) *status = t->exit_status;
    return err::kOk;
}

i32 Kernel::thread_info(Uid uid, ThreadInfo* out) {
    Lock guard(impl_->lock);
    Thread* t = impl_->thread(uid);
    if (!t) return err::kUnknownThreadId;
    out->name = t->name;
    out->attr = t->attr;
    out->status = static_cast<u32>(t->state);
    out->entry = t->entry;
    out->stack = t->stack;
    out->stack_size = t->stack_size;
    out->init_priority = t->init_priority;
    out->priority = t->priority;
    out->init_affinity = t->init_affinity;
    out->affinity = t->affinity;
    out->cpu = t->cpu;
    out->last_cpu = t->last_cpu;
    out->wait_type = t->state == ThreadState::Waiting ? static_cast<u32>(t->wait.type) : 0;
    out->wait_id = t->state == ThreadState::Waiting ? t->wait.object : 0;
    out->exit_status = t->exit_status;
    return err::kOk;
}

i32 Kernel::send_signal(Uid uid) {
    Lock guard(impl_->lock);
    Thread* t = impl_->get<Thread>(uid, ObjectClass::Thread);
    if (!t) return err::kUnknownThreadId;
    if (t->state == ThreadState::Waiting && t->wait.type == WaitType::Signal) {
        impl_->end_wait(t, err::kOk);
        return err::kOk;
    }
    if (t->signal) return err::kAlreadySent;
    t->signal = 1;
    return err::kOk;
}

i32 Kernel::wait_signal(Timeout timeout) {
    Lock guard(impl_->lock);
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    const auto deadline = impl_->deadline(timeout);
    if (self->signal) {
        self->signal = 0;
        impl_->write_remaining(timeout, deadline);
        return err::kOk;
    }
    self->wait = Wait{};
    self->wait.type = WaitType::Signal;
    const i32 r = impl_->block(self, nullptr, deadline, false);
    impl_->write_remaining(timeout, deadline);
    return r;
}

u32 Kernel::tls_address(u32 key) {
    Thread* t = current();
    if (!t || !t->tls || key >= kTlsSize / 4) return 0;
    return t->tls + 4 * key;
}

nx::vector<Kernel::CpuUse> Kernel::cpu_use() {
    Lock guard(impl_->lock);
    const u64 n = impl_->now_ns();
    nx::vector<CpuUse> out;
    for (const auto& [uid, object] : impl_->objects) {
        if (object->cls != ObjectClass::Thread) continue;
        const auto* t = static_cast<const Thread*>(object.get());
        out.push_back({uid, t->name, t->priority, t->held_ns + (t->held_since ? n - t->held_since : 0),
                       t->ready_ns + (t->ready_since ? n - t->ready_since : 0)});
    }
    return out;
}

void Kernel::log_thread_stats() {
    Lock guard(impl_->lock);
    impl_->log_thread_stats();
}

nx::string Kernel::describe_threads() {
    Lock guard(impl_->lock);
    nx::string out;
    for (const auto& [uid, object] : impl_->objects) {
        if (object->cls != ObjectClass::Thread) continue;
        const auto* t = static_cast<const Thread*>(object.get());
        const char* state = t->state == ThreadState::Running ? "running"
                            : t->state == ThreadState::Ready  ? "ready"
                            : t->state == ThreadState::Waiting ? "waiting"
                            : t->state == ThreadState::Dormant ? "dormant"
                                                               : "deleted";
        out += nx::format("  0x{:08x} '{}': {}, priority {}, affinity 0x{:x}, cpu {}, lr {:08x} sp {:08x}", static_cast<u32>(uid),
                          t->name, state, t->priority, static_cast<u32>(t->affinity), t->cpu, t->ctx.r[14], t->ctx.r[13]);
        if (const nx::string where = impl_->backend->where(t); !where.empty()) out += nx::format(" [{}]", where);
        if (t->state == ThreadState::Waiting) {
            const auto it = impl_->objects.find(t->wait.object);
            out += nx::format(" (wait type 0x{:x} on 0x{:08x} '{}'", static_cast<u32>(t->wait.type),
                              static_cast<u32>(t->wait.object), it != impl_->objects.end() ? it->second->name.c_str() : "");
            // (a mutex's owner and count: who the thread waits for)
            if (it != impl_->objects.end() &&
                (it->second->cls == ObjectClass::Mutex || it->second->cls == ObjectClass::LwMutex)) {
                const auto* m = static_cast<const Mutex*>(it->second.get());
                out += m->owner ? nx::format(", held by 0x{:08x} '{}' count {}", static_cast<u32>(m->owner->uid), m->owner->name, m->count)
                                : nx::format(", not held, count {}", m->count);
            }
            out += ")";
        }
        out += "\n";
    }
    return out;
}

}  // namespace psvrecore::runtime
