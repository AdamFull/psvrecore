// The kernel's core: objects and UIDs, the CPUs and who runs on them, waiting and waking.

#include <algorithm>

#include "kernel_impl.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

using namespace kernel;

namespace {
thread_local Thread* t_current = nullptr;
}

void set_current_thread(Thread* t) { t_current = t; }

// ---- wait queues

void WaitQueue::add(Thread* t) {
    if (!by_priority) {
        threads.push_back(t);
        return;
    }
    auto it = std::find_if(threads.begin(), threads.end(), [&](const Thread* o) { return o->priority > t->priority; });
    threads.insert(it, t);
}

void WaitQueue::remove(Thread* t) {
    auto it = std::find(threads.begin(), threads.end(), t);
    if (it != threads.end()) threads.erase(it);
}

// ---- Impl

Kernel::Impl::Impl(Kernel& k, GuestMemory& m, const Config& c) : kernel(k), memory(m), config(c) {
    if (config.cpu_count == 0 || config.cpu_count > kUserCpuCount) config.cpu_count = kUserCpuCount;
    cpus.assign(config.cpu_count, nullptr);
    start_ns = nx::os::ticks_ns();
}

Uid Kernel::Impl::new_uid() {
    // Positive and odd, as the console's are; never reused.
    ++uid_counter;
    return static_cast<Uid>(0x40010001u + 2 * uid_counter);
}

Thread* Kernel::Impl::thread(Uid uid) {
    if (uid == 0) return t_current;
    return get<Thread>(uid, ObjectClass::Thread);
}

u64 Kernel::Impl::now_ns() const { return nx::os::ticks_ns(); }

bool Kernel::Impl::allowed(const Thread* t, i32 cpu) {
    const i32 mask = t->affinity ? t->affinity : kAllUserCpus;
    return (mask >> (kCpuMaskShift + cpu)) & 1;
}

void Kernel::Impl::make_ready(Thread* t, bool front) {
    t->state = ThreadState::Ready;
    t->ready_order = static_cast<u64>(front ? --front_order : ++back_order);
    if (std::find(ready.begin(), ready.end(), t) == ready.end()) ready.push_back(t);
    dispatch();
}

void Kernel::Impl::release_cpu(Thread* t) {
    if (t->cpu < 0) return;
    cpus[static_cast<usize>(t->cpu)] = nullptr;
    t->last_cpu = t->cpu;
    t->cpu = -1;
}

void Kernel::Impl::dispatch() {
    // Highest priority first; FIFO among equals (a preempted thread goes to the front of its priority).
    for (bool changed = true; changed;) {
        changed = false;
        std::sort(ready.begin(), ready.end(), [](const Thread* a, const Thread* b) {
            if (a->priority != b->priority) return a->priority < b->priority;
            return static_cast<i64>(a->ready_order) < static_cast<i64>(b->ready_order);
        });
        for (usize i = 0; i < ready.size(); ++i) {
            Thread* t = ready[i];
            i32 cpu = -1;
            if (t->last_cpu >= 0 && t->last_cpu < static_cast<i32>(cpus.size()) && !cpus[static_cast<usize>(t->last_cpu)] &&
                allowed(t, t->last_cpu))
                cpu = t->last_cpu;
            for (i32 c = 0; cpu < 0 && c < static_cast<i32>(cpus.size()); ++c)
                if (!cpus[static_cast<usize>(c)] && allowed(t, c)) cpu = c;
            Thread* victim = nullptr;
            if (cpu < 0) {
                // The CPU (of those it may use) running the lowest-priority thread below its own.
                for (i32 c = 0; c < static_cast<i32>(cpus.size()); ++c) {
                    Thread* o = cpus[static_cast<usize>(c)];
                    if (!o || !allowed(t, c) || o->priority <= t->priority) continue;
                    if (!victim || o->priority > victim->priority) {
                        victim = o;
                        cpu = c;
                    }
                }
                if (!victim) continue;
            }
            ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(i));
            if (victim) {
                release_cpu(victim);
                victim->state = ThreadState::Ready;
                victim->ready_order = static_cast<u64>(--front_order);
                victim->lost_cpu.store(true, std::memory_order_release);
                ready.push_back(victim);
            }
            cpus[static_cast<usize>(cpu)] = t;
            t->cpu = cpu;
            t->state = ThreadState::Running;
            t->lost_cpu.store(false, std::memory_order_relaxed);
            wake(t);
            changed = true;
            break;  // (the queue changed: sort again)
        }
    }
}

void Kernel::Impl::wait_for_cpu(Thread* t) {
    while (t->cpu < 0 && !t->terminate) park(t);
}

void Kernel::Impl::park(Thread* t, std::optional<u64> deadline_ns) {
    lock.unlock();
    if (deadline_ns) {
        const u64 now = now_ns();
        const u64 left = *deadline_ns > now ? *deadline_ns - now : 0;
        t->parker.park_until(std::chrono::steady_clock::now() + std::chrono::nanoseconds(left));
    } else {
        t->parker.park();
    }
    lock.lock();
}

i32 Kernel::Impl::block(Thread* t, WaitQueue* queue, std::optional<u64> deadline_ns, bool callbacks) {
    t->state = ThreadState::Waiting;
    t->wait.queue = queue;
    t->wait.done = false;
    t->wait.result = err::kOk;
    release_cpu(t);
    dispatch();
    while (!t->wait.done) {
        i32 own = 0;
        if (t->terminate) own = err::kWaitCancel;
        else if (callbacks && has_callbacks(t)) own = kRunCallbacks;
        else if (deadline_ns && now_ns() >= *deadline_ns) own = err::kWaitTimeout;
        if (own) {
            if (queue) queue->remove(t);
            t->wait.queue = nullptr;
            t->wait.done = true;
            t->wait.result = own;
            make_ready(t);
            break;
        }
        park(t, deadline_ns);
    }
    wait_for_cpu(t);
    t->wait.type = WaitType::None;
    t->wait.object = 0;
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
    return t->wait.result;
}

void Kernel::Impl::end_wait(Thread* t, i32 result) {
    if (t->wait.queue) t->wait.queue->remove(t);
    t->wait.queue = nullptr;
    t->wait.done = true;
    t->wait.result = result;
    make_ready(t);
    wake(t);
}

std::optional<u64> Kernel::Impl::deadline(const Timeout& timeout) const {
    if (!timeout.remaining) return std::nullopt;
    return now_ns() + u64{*timeout.remaining} * 1000;
}

void Kernel::Impl::write_remaining(const Timeout& timeout, const std::optional<u64>& deadline_ns) const {
    if (!timeout.remaining || !deadline_ns) return;
    const u64 now = now_ns();
    *timeout.remaining = now >= *deadline_ns ? 0 : static_cast<u32>((*deadline_ns - now) / 1000);
}

bool Kernel::Impl::has_callbacks(const Thread* t) const {
    for (Uid id : t->callbacks) {
        auto it = objects.find(id);
        if (it != objects.end() && static_cast<const Callback*>(it->second.get())->notify_count > 0) return true;
    }
    return false;
}

i32 Kernel::Impl::run_callbacks(Thread* t) {
    i32 ran = 0;
    for (bool again = true; again;) {
        again = false;
        for (Uid id : t->callbacks) {
            Callback* cb = get<Callback>(id, ObjectClass::Callback);
            if (!cb || cb->notify_count <= 0) continue;
            const i32 count = cb->notify_count, arg = cb->notify_arg;
            cb->notify_count = 0;
            cb->notify_arg = 0;
            const u32 function = cb->function, common = cb->common;
            // The callback runs in this thread, as guest code: without the kernel lock.
            lock.unlock();
            const u32 args[4] = {static_cast<u32>(id), static_cast<u32>(count), static_cast<u32>(arg), common};
            u32 result = 0;
            try {
                result = hle::call_guest(&t->ctx, function, args);
            } catch (...) {
                lock.lock();
                throw;
            }
            lock.lock();
            ++ran;
            if (result != 0) {
                // A callback that returns nonzero is deleted.
                if (Callback* still = get<Callback>(id, ObjectClass::Callback)) {
                    auto& list = t->callbacks;
                    list.erase(std::remove(list.begin(), list.end(), id), list.end());
                    (void)still;
                    objects.erase(id);
                    --sync_count;
                }
            }
            again = true;
            break;  // (the list may have changed)
        }
    }
    return ran;
}

i32 resolve_priority(i32 priority) {
    if ((static_cast<u32>(priority) >> 16) == 0x1000) {  // (0x10000100 +/- 32)
        const i32 offset = priority - kDefaultPriority;
        if (offset < -32 || offset > 31) return -1;
        return kGamePriority + offset;
    }
    if (priority < kHighestPriority || priority > kLowestPriority) return -1;
    return priority;
}

// ---- Kernel

Kernel::Kernel(GuestMemory& memory, const Config& config)
    : impl_(std::make_unique<Impl>(*this, memory, config)), memory_(memory) {
    Lock guard(impl_->lock);
    for (u32 i = 0; i < 4; ++i) impl_->spawn_worker();
    // (until they run: the first guest threads should not wait for OS threads to start either)
    const u64 give_up = nx::os::ticks_ns() + 2'000'000'000ull;
    while (impl_->workers_starting > 0 && nx::os::ticks_ns() < give_up) {
        impl_->lock.unlock();
        nx::this_thread::sleep_for(std::chrono::microseconds(200));
        impl_->lock.lock();
    }
}

Kernel::~Kernel() {
    if (!impl_) return;
    shutdown();
    if (!shutdown_complete()) {
        // A host thread still runs guest code and uses the kernel's state: it cannot be freed under it.
        nx::logw(logs::kernel, "the kernel's state is left allocated: guest threads are still running");
        (void)impl_.release();
    }
}

Thread* Kernel::current() { return t_current; }

void Kernel::checkpoint() {
    Thread* t = t_current;
    if (!t || !(t->lost_cpu.load(std::memory_order_acquire) || t->terminate)) return;
    Lock guard(impl_->lock);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
    impl_->wait_for_cpu(t);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
}

void Kernel::begin_blocking() {
    Thread* t = t_current;
    if (!t) return;
    Lock guard(impl_->lock);
    t->state = ThreadState::Waiting;
    t->wait = Wait{};
    impl_->release_cpu(t);
    impl_->dispatch();
}

void Kernel::end_blocking() {
    Thread* t = t_current;
    if (!t) return;
    Lock guard(impl_->lock);
    impl_->make_ready(t);
    impl_->wait_for_cpu(t);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
}

u64 Kernel::process_time() const { return clock_ns() / 1000; }
u64 Kernel::clock_ns() const { return nx::os::ticks_ns() - impl_->start_ns; }

void Kernel::shutdown() {
    if (!impl_) return;
    Lock guard(impl_->lock);
    impl_->shutting_down = true;
    for (auto& [uid, object] : impl_->objects)
        if (object->cls == ObjectClass::Thread) {
            auto* t = static_cast<Thread*>(object.get());
            t->terminate = true;
            Impl::wake(t);
        }
    for (Impl::HostWorker* w : impl_->idle_workers) {
        w->quit = true;
        w->parker.unpark();
    }
    // Threads blocked in the kernel end now; one running guest code ends at its next kernel call, or never (then
    // its host thread is left to the process exit).
    const u64 give_up = nx::os::ticks_ns() + 2'000'000'000ull;
    while (impl_->host_threads > 0 || impl_->workers_alive > 0) {
        const u64 now = nx::os::ticks_ns();
        if (now >= give_up) {
            nx::logw(logs::kernel, "{} guest threads did not end", impl_->host_threads);
            impl_->stuck = true;
            break;
        }
        impl_->host_threads_changed.wait_for(impl_->lock, std::chrono::nanoseconds(give_up - now));
    }
}

bool Kernel::shutdown_complete() const {
    Lock guard(impl_->lock);
    return impl_->shutting_down && !impl_->stuck;
}

Uid Kernel::adopt_host_thread(nx::string_view name, i32 priority, u32 stack_size) {
    Lock guard(impl_->lock);
    auto t = std::make_unique<Thread>();
    t->init_priority = t->priority = priority;
    t->stack_size = stack_size;
    t->stack = memory_.map_anywhere(impl_->config.stack_region, stack_size, GuestMemory::kPageSize, Access::ReadWrite,
                                    BlockKind::Stack, name);
    t->tls = memory_.map_anywhere(impl_->config.stack_region, kTlsSize, GuestMemory::kPageSize, Access::ReadWrite,
                                  BlockKind::ThreadLocal, name);
    t->ctx.r[13] = t->stack + stack_size;
    t->ctx.tpidruro = t->tls;
    Thread* raw = impl_->add(std::move(t), name, 0);
    raw->ctx.thread = raw;
    raw->started_once = true;
    ++impl_->thread_count;
    t_current = raw;
    impl_->make_ready(raw);
    impl_->wait_for_cpu(raw);
    return raw->uid;
}

void Kernel::release_host_thread() {
    Thread* t = t_current;
    if (!t) return;
    Lock guard(impl_->lock);
    impl_->unlock_owned_mutexes(t);
    impl_->release_cpu(t);
    impl_->ready.erase(std::remove(impl_->ready.begin(), impl_->ready.end(), t), impl_->ready.end());
    t->state = ThreadState::Dormant;
    impl_->dispatch();
    while (!t->end_waiters.empty()) impl_->end_wait(t->end_waiters.front(), err::kOk);
    impl_->free_thread_memory(t);
    impl_->objects.erase(t->uid);
    --impl_->thread_count;
    t_current = nullptr;
}

Uid Kernel::lw_uid(u32 work) const {
    Lock guard(impl_->lock);
    auto it = impl_->lw_objects.find(work);
    return it == impl_->lw_objects.end() ? 0 : it->second;
}

}  // namespace psvrecore::runtime
