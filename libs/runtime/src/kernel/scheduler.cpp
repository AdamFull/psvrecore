// The kernel's core: objects and UIDs, the CPUs and who runs on them, waiting and waking.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <optional>

#include "kernel_impl.hpp"
#include "psvrecore/profile.hpp"
#include "psvrecore/runtime/host.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

using namespace kernel;

namespace {
thread_local Thread* t_current = nullptr;
thread_local LocalSlots t_host_locals;  // (GuestLocal on a host thread that runs no guest thread)
std::atomic<u32> g_local_slots{0};

#if PSVRECORE_TRACY
// The profiler's zone for a wait, by what it waits for.
const tracy::SourceLocationData* wait_location(WaitType type) {
    PSV_PROFILE_LOCATION(kDelay, "wait: delay", 0x707070);
    PSV_PROFILE_LOCATION(kThreadEnd, "wait: thread end", 0x8C6E4A);
    PSV_PROFILE_LOCATION(kSignal, "wait: signal", 0x8C6E4A);
    PSV_PROFILE_LOCATION(kEventFlag, "wait: event flag", 0xB0503C);
    PSV_PROFILE_LOCATION(kSemaphore, "wait: semaphore", 0xB0503C);
    PSV_PROFILE_LOCATION(kMutex, "wait: mutex", 0xC03030);
    PSV_PROFILE_LOCATION(kRWLock, "wait: reader-writer lock", 0xC03030);
    PSV_PROFILE_LOCATION(kCond, "wait: condition variable", 0xB0503C);
    PSV_PROFILE_LOCATION(kEvent, "wait: event", 0xB0503C);
    PSV_PROFILE_LOCATION(kMsgPipe, "wait: message pipe", 0xB0503C);
    PSV_PROFILE_LOCATION(kLwMutex, "wait: lightweight mutex", 0xC03030);
    PSV_PROFILE_LOCATION(kLwCond, "wait: lightweight condition variable", 0xB0503C);
    PSV_PROFILE_LOCATION(kOther, "wait", 0x707070);
    switch (type) {
        case WaitType::Delay: return &kDelay;
        case WaitType::ThreadEnd: return &kThreadEnd;
        case WaitType::Signal: return &kSignal;
        case WaitType::EventFlag: return &kEventFlag;
        case WaitType::Semaphore: return &kSemaphore;
        case WaitType::Mutex:
        case WaitType::CondMutex: return &kMutex;
        case WaitType::RWLock: return &kRWLock;
        case WaitType::CondSignal: return &kCond;
        case WaitType::Event: return &kEvent;
        case WaitType::MsgPipe: return &kMsgPipe;
        case WaitType::LwMutex:
        case WaitType::LwCondMutex: return &kLwMutex;
        case WaitType::LwCondSignal: return &kLwCond;
        default: return &kOther;
    }
}
#endif
}  // namespace

NX_NO_INLINE void set_current_thread(Thread* t) { t_current = t; }

namespace kernel {

u32 new_local_slot() {
    const u32 slot = g_local_slots.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kLocalSlots) {
        nx::loge(logs::kernel, "more than {} GuestLocal variables: raise kernel::kLocalSlots", kLocalSlots);
        std::abort();
    }
    return slot;
}

NX_NO_INLINE LocalSlots& current_local_slots() {
    Thread* t = t_current;
    return t ? t->locals : t_host_locals;
}

}  // namespace kernel

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
    if (const char* v = std::getenv("PSVRECORE_THREAD_STATS"); v && *v && *v != '0') thread_stats = true;
    const char* determinism = std::getenv("PSVRECORE_DETERMINISTIC");
    const bool wants_determinism = config.deterministic || (determinism && *determinism && *determinism != '0');
#if PSVRECORE_GUEST_FIBERS
    const char* which = std::getenv("PSVRECORE_GUEST_THREADS");
    if (which && nx::string_view(which) == "native") {
        if (wants_determinism) nx::logw(logs::kernel, "deterministic scheduling needs the fiber backend: not with native threads");
        backend = make_native_backend(backend_host());
    } else {
        deterministic = wants_determinism;
        if (deterministic) virtual_ns.store(start_ns, std::memory_order_relaxed);
        backend = make_fiber_backend(backend_host(), deterministic);
    }
#else
    if (wants_determinism)
        nx::logw(logs::kernel, "deterministic scheduling needs a build with PSVRECORE_GUEST_FIBERS: not in this one");
    backend = make_native_backend(backend_host());
#endif
    nx::logd(logs::kernel, "guest threads: {}", backend->name());
}

BackendHost Kernel::Impl::backend_host() {
    BackendHost h;
    h.lock = &lock;
    h.cpu_count = config.cpu_count;
    h.impl = this;
    h.changed = &host_threads_changed;
    h.run = [](void* impl, Thread* t) { static_cast<Impl*>(impl)->run_thread(t); };
    h.now = [](const void* impl) { return static_cast<const Impl*>(impl)->now_ns(); };
    h.name_of = [](const Thread* t) { return t->name.c_str(); };
    h.set_current = [](Thread* t) { set_current_thread(t); };
    h.enter_lane = [](Thread* t) { profile::enter(t ? t->track : nullptr); };
    h.clock = deterministic ? &virtual_ns : nullptr;
    return h;
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

u64 Kernel::Impl::now_ns() const {
    if (deterministic) return virtual_ns.load(std::memory_order_acquire);
    const u64 at = suspended_at.load(std::memory_order_acquire);
    return (at ? at : nx::os::ticks_ns()) - paused_ns.load(std::memory_order_acquire);
}

bool Kernel::Impl::allowed(const Thread* t, i32 cpu) {
    const i32 mask = t->affinity ? t->affinity : kAllUserCpus;
    return (mask >> (kCpuMaskShift + cpu)) & 1;
}

void Kernel::Impl::make_ready(Thread* t, bool front) {
    t->state = ThreadState::Ready;
    if (!t->ready_since) t->ready_since = now_ns();
    t->ready_order = static_cast<u64>(front ? --front_order : ++back_order);
    if (std::find(ready.begin(), ready.end(), t) == ready.end()) ready.push_back(t);
    dispatch();
}

void Kernel::Impl::stop_running(Thread* t) {
    release_cpu(t);
    if (auto it = std::find(ready.begin(), ready.end(), t); it != ready.end()) {
        ready.erase(it);
        if (t->ready_since) t->ready_ns += now_ns() - t->ready_since, t->ready_since = 0;
    }
    t->lost_cpu.store(false, std::memory_order_relaxed);
}

void Kernel::Impl::release_cpu(Thread* t) {
    if (t->cpu < 0) return;
    if (t->held_since) t->held_ns += now_ns() - t->held_since, t->held_since = 0;
    cpus[static_cast<usize>(t->cpu)] = nullptr;
    t->last_cpu = t->cpu;
    t->cpu = -1;
}

void Kernel::Impl::dispatch() {
    if (suspended) return;  // (no CPU for anyone until resume())
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
                victim->ready_since = now_ns();
                victim->ready_order = static_cast<u64>(--front_order);
                victim->lost_cpu.store(true, std::memory_order_release);
                ready.push_back(victim);
            }
            cpus[static_cast<usize>(cpu)] = t;
            t->cpu = cpu;
            t->state = ThreadState::Running;
            {
                const u64 n = now_ns();
                if (t->ready_since) t->ready_ns += n - t->ready_since, t->ready_since = 0;
                t->held_since = n;
            }
            t->lost_cpu.store(false, std::memory_order_relaxed);
            backend->give_cpu(t);
            changed = true;
            break;  // (the queue changed: sort again)
        }
    }
#if PSVRECORE_TRACY
    i64 running = 0;
    for (const Thread* c : cpus) running += c != nullptr;
    PSV_PROFILE_PLOT("kernel: CPUs running a thread", running);
    PSV_PROFILE_PLOT("kernel: threads ready without a CPU", static_cast<i64>(ready.size()));
#endif
}

void Kernel::Impl::wait_for_cpu(Thread* t) {
    for (;;) {
        if (t->cpu < 0 && !t->terminate) {
            // (ready, behind threads of higher priority or its own: a CPU handed over to it when one is free)
            PSV_PROFILE_ZONE_COLOR("waiting for a CPU", 0xD0A020);
            while (t->cpu < 0 && !t->terminate) park(t);
        }
        if (t->terminate) return;
        // (on to guest code where its CPU runs: a fiber moves to the CPU's worker, and may lose the CPU meanwhile)
        if (backend->on_cpu(t)) break;
    }
    t->lost_cpu.store(false, std::memory_order_relaxed);
}

i32 Kernel::Impl::block(Thread* t, WaitQueue* queue, std::optional<u64> deadline_ns, bool callbacks) {
#if PSVRECORE_TRACY
    profile::announce();
    tracy::ScopedZone zone(wait_location(t->wait.type), -1, TracyIsConnected);
    if (t->wait.object)
        if (auto it = objects.find(t->wait.object); it != objects.end() && !it->second->name.empty())
            zone.Text(it->second->name.data(), it->second->name.size());
#endif
    t->state = ThreadState::Waiting;
    t->wait.queue = queue;
    t->wait.done = false;
    t->wait.result = err::kOk;
    stop_running(t);
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
    : impl_(nx::make_unique<Impl>(*this, memory, config)), memory_(memory) {}

Kernel::~Kernel() {
    if (!impl_) return;
    shutdown();
    if (!shutdown_complete()) {
        // A host thread still runs guest code and uses the kernel's state: it cannot be freed under it.
        nx::logw(logs::kernel, "the kernel's state is left allocated: guest threads are still running");
        (void)impl_.release();
    }
}

NX_NO_INLINE Thread* Kernel::current() { return t_current; }

void Kernel::checkpoint() {
    Thread* t = t_current;
    if (impl_->deterministic && t) {
        // (a microsecond a kernel call, and the next CPU's holder runs now)
        Lock guard(impl_->lock);
        impl_->virtual_ns.fetch_add(1000, std::memory_order_acq_rel);
        impl_->backend->yield(t);
    }
    if (!t || !(t->lost_cpu.load(std::memory_order_acquire) || t->terminate)) return;
    Lock guard(impl_->lock);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
    impl_->wait_for_cpu(t);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
}

void Kernel::begin_blocking() {
    Thread* t = t_current;
    if (!t) return;
#if PSVRECORE_TRACY
    PSV_PROFILE_LOCATION(kBlocking, "blocked on the host", 0x5070A0);
    profile::begin(&kBlocking);
#endif
    Lock guard(impl_->lock);
    t->state = ThreadState::Waiting;
    t->wait = Wait{};
    impl_->stop_running(t);
    impl_->dispatch();
    impl_->backend->begin_host_work(t);  // (with fibers: off its CPU's worker, onto a helper)
}

bool Kernel::lend_cpu() {
    Thread* t = t_current;
    if (!t || t->cpu < 0) return false;
    Lock guard(impl_->lock);
    const i32 cpu = t->cpu;
    t->state = ThreadState::Waiting;
    t->wait = Wait{};
    impl_->release_cpu(t);
    t->lent_cpu = cpu;
    impl_->dispatch();
    impl_->backend->begin_host_work(t);
#if PSVRECORE_TRACY
    PSV_PROFILE_LOCATION(kLent, "CPU lent (host work)", 0x5090A0);
    profile::begin(&kLent);
#endif
    return true;
}

void Kernel::reclaim_cpu() {
    Thread* t = t_current;
    if (!t || t->lent_cpu < 0) return;
    profile::pop();  // (lend_cpu's zone)
    PSV_PROFILE_ZONE_COLOR("taking the CPU back", 0xD0A020);
    Lock guard(impl_->lock);
    const i32 cpu = t->lent_cpu;
    t->lent_cpu = -1;
    impl_->backend->end_host_work(t);
    if (t->cpu >= 0) {
        // (one again already: a wait inside the work ended with one)
        impl_->wait_for_cpu(t);
        if (t->terminate) throw ThreadExit{err::kWaitCancel};
        return;
    }
    if (impl_->suspended) {
        // (no CPU for anyone until resume(): in line as any thread)
        impl_->make_ready(t);
        impl_->wait_for_cpu(t);
    } else {
        // The borrower (if any) to the front of the ready threads, giving the CPU up at its next kernel call.
        if (Thread* o = impl_->cpus[static_cast<usize>(cpu)]) {
            impl_->release_cpu(o);
            o->state = ThreadState::Ready;
            o->ready_since = impl_->now_ns();
            o->ready_order = static_cast<u64>(--impl_->front_order);
            o->lost_cpu.store(true, std::memory_order_release);
            impl_->ready.push_back(o);
        }
        impl_->cpus[static_cast<usize>(cpu)] = t;
        t->cpu = cpu;
        t->state = ThreadState::Running;
        t->held_since = impl_->now_ns();
        t->lost_cpu.store(false, std::memory_order_relaxed);
        impl_->dispatch();  // (the borrower to another CPU, if one is free)
        impl_->wait_for_cpu(t);  // (with fibers: onto the CPU's worker, once the borrower has left it)
    }
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
}

void Kernel::end_blocking() {
    Thread* t = t_current;
    if (!t) return;
    profile::pop();  // (begin_blocking's zone)
    Lock guard(impl_->lock);
    impl_->backend->end_host_work(t);
    impl_->make_ready(t);
    impl_->wait_for_cpu(t);
    if (t->terminate) throw ThreadExit{err::kWaitCancel};
}

const char* Kernel::backend_name() const { return impl_->backend->name(); }

u64 Kernel::process_time() const { return clock_ns() / 1000; }
u64 Kernel::clock_ns() const { return impl_->now_ns() - impl_->start_ns; }

void Kernel::suspend() {
    Lock guard(impl_->lock);
    if (impl_->suspended) return;
    impl_->suspended = true;
    impl_->suspended_at.store(nx::os::ticks_ns(), std::memory_order_release);
    // (every running thread loses its CPU, and stops at its next kernel call)
    for (Thread*& running : impl_->cpus) {
        Thread* t = running;
        if (!t) continue;
        impl_->release_cpu(t);
        t->state = ThreadState::Ready;
        t->ready_order = static_cast<u64>(--impl_->front_order);
        t->lost_cpu.store(true, std::memory_order_release);
        impl_->ready.push_back(t);
    }
}

void Kernel::resume() {
    Lock guard(impl_->lock);
    if (!impl_->suspended) return;
    impl_->paused_ns.fetch_add(nx::os::ticks_ns() - impl_->suspended_at.load(), std::memory_order_acq_rel);
    impl_->suspended_at.store(0, std::memory_order_release);
    impl_->suspended = false;
    impl_->dispatch();
}

bool Kernel::suspended() const { return impl_->suspended_at.load(std::memory_order_acquire) != 0; }

void Kernel::shutdown() {
    if (!impl_) return;
    Lock guard(impl_->lock);
    if (!impl_->shutting_down) impl_->log_thread_stats();
    impl_->shutting_down = true;
    for (auto& [uid, object] : impl_->objects)
        if (object->cls == ObjectClass::Thread) {
            auto* t = static_cast<Thread*>(object.get());
            t->terminate = true;
            impl_->wake(t);
        }
    // Threads blocked in the kernel end now; one running guest code ends at its next kernel call, or never (then
    // its host thread is left to the process exit). Then the backend's own host threads end.
    const u64 give_up = nx::os::ticks_ns() + 2'000'000'000ull;
    bool stopping = false;
    while (impl_->host_threads > 0 || !impl_->backend->stopped()) {
        if (impl_->host_threads == 0 && !stopping) {
            impl_->backend->stop();
            stopping = true;
            continue;
        }
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
    host::frame_thread();
    Lock guard(impl_->lock);
    auto t = nx::make_unique<Thread>();
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
