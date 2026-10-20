// Guest threads: creation, the host thread each one runs on, ending, waiting for an end, delays, priorities.

#include <algorithm>
#include <cstring>

#include "kernel_impl.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

using namespace kernel;

void set_current_thread(Thread* t);

namespace {

constexpr u32 kMaxThreads = 128;
constexpr usize kWorkerStack = usize{4} << 20;  // the host stack of pooled host threads
constexpr u32 kSpareWorkers = 4;                // kept waiting

// The host stack a guest thread's host thread gets: recompiled functions use more stack than the guest's (their
// frames hold temporaries), and guest recursion is host recursion.
usize host_stack_size(u32 guest_stack) {
    const u64 size = std::max<u64>(u64{4} << 20, u64{guest_stack} * 16);
    return static_cast<usize>(std::min<u64>(size, u64{256} << 20));
}

}  // namespace

// ---- host side

void Kernel::Impl::spawn_worker() {
    // (lock held) The worker joins the idle list once its OS thread runs.
    ++workers_alive;
    ++workers_starting;
    auto* w = new HostWorker;
    nx::thread_options options;
    options.name = "guest thread";
    options.stack_size = kWorkerStack;
    nx::thread host([this, w] { worker_loop(w); }, options);
    host.detach();
}

void Kernel::Impl::worker_loop(HostWorker* w) {
    {
        Lock guard(lock);
        --workers_starting;
        if (!shutting_down) idle_workers.push_back(w);
        else w->quit = true;
    }
    for (;;) {
        Thread* job = nullptr;
        {
            Lock guard(lock);
            while (!w->job && !w->quit) {
                lock.unlock();
                w->parker.park();
                lock.lock();
            }
            if (!w->job) break;
            job = w->job;
        }
        nx::this_thread::set_name(job->name.c_str());
        run_thread(job);
        Lock guard(lock);
        w->job = nullptr;
        if (shutting_down) break;
        idle_workers.push_back(w);
    }
    Lock guard(lock);
    idle_workers.erase(std::remove(idle_workers.begin(), idle_workers.end(), w), idle_workers.end());
    delete w;
    --workers_alive;
    host_threads_changed.notify_all();
}

void Kernel::Impl::run_thread(Thread* t) {
    set_current_thread(t);
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

    ++impl_->host_threads;
    const usize host_stack = host_stack_size(t->stack_size);
    if (host_stack <= kWorkerStack && !impl_->idle_workers.empty()) {
        Impl::HostWorker* w = impl_->idle_workers.back();
        impl_->idle_workers.pop_back();
        w->job = t;
        w->parker.unpark();
    } else {
        // (a stack larger than the pool's, or no worker waiting: an OS thread of its own)
        nx::thread_options options;
        options.name = t->name.c_str();
        options.stack_size = host_stack;
        Impl* impl = impl_.get();
        nx::thread host([impl, t] { impl->run_thread(t); }, options);
        host.detach();
    }
    if (impl_->idle_workers.size() + impl_->workers_starting < kSpareWorkers && !impl_->shutting_down) impl_->spawn_worker();
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
        if (t->state == ThreadState::Waiting) {
            const auto it = impl_->objects.find(t->wait.object);
            out += nx::format(" (wait type 0x{:x} on 0x{:08x} '{}')", static_cast<u32>(t->wait.type),
                              static_cast<u32>(t->wait.object), it != impl_->objects.end() ? it->second->name.c_str() : "");
        }
        out += "\n";
    }
    return out;
}

}  // namespace psvrecore::runtime
