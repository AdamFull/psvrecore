// The fiber thread backend (PSVRECORE_GUEST_FIBERS): each guest thread is a fiber (nx-foundation's contexts and
// stacks), and the host threads that run them are fixed:
//   - workers, one per console CPU: a thread holding a CPU runs on a free worker (which one does not matter: the
//     scheduler has already put it on a CPU its affinity mask allows);
//   - helper threads: they run a thread that holds no CPU (the kernel's own work after a wake: a wait that ended
//     goes on to wait for a CPU; host work that blocks, Kernel::Blocking and lend_cpu), and a CPU's holder when no
//     worker is free, as many as are needed at once;
//   - a timer thread: it wakes parked threads at their deadlines.
// A thread moves between them only where the kernel calls the backend: when it parks, gets a CPU, or begins or ends
// host work. Every switch happens with the kernel's lock held, which the host thread switched to holds on (its own OS
// thread took it), so a lock is always let go by the OS thread that took it.
//
// A thread that loses its CPU while it runs guest code stops at its next kernel call, as with native threads, and
// keeps its worker until then. The CPU's new holder does not wait for it: with no free worker it runs on a helper
// meanwhile, so both run, as two native threads would (a game's thread may spin, without kernel calls, until the one
// that took its CPU has done something). At its next wait it goes back to a worker.
//
// Code that runs on guest threads keeps no thread_local state across a kernel call: the OS thread may be another
// afterwards (Kernel::current() and GuestLocal are read again after every switch).
//
// Deterministic (Config::deterministic, PSVRECORE_DETERMINISTIC=1): one worker runs everything (the CPUs' holders in
// turn, switching at every kernel call, CPU by CPU; then the threads without a CPU, in the order they were woken), host
// work included, and the kernel's clock is the backend's own: a microsecond more at each kernel call, and when nothing
// can run, the next deadline. The same program then runs in the same order run after run (host threads aside); a
// thread that spins without kernel calls for another to go on never ends there.

#if PSVRECORE_GUEST_FIBERS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "backend.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/runtime/float_env.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/fibers/context.h"
#include "core/foundation/fibers/stack.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime::kernel {

namespace {

class FiberBackend;

// A guest thread's run, as a fiber.
struct GuestFiber {
    FiberBackend* owner = nullptr;
    Thread* thread = nullptr;      // (null once its run ended: end())
    nx::fiber::StackPool* pool = nullptr;
    nx::fiber::Stack stack;
    nx::fiber::Context context;
    struct Host* on = nullptr;     // the host thread running it, or null while suspended
    bool parked = true;            // suspended in park() (or not started): resumed once woken or at its deadline
    bool woken = false;            // an unpark not consumed yet
    bool timed_out = false;        // its deadline passed while it was parked
    std::optional<u64> deadline;   // (while parked; the kernel's clock)
    u32 host_work = 0;             // begin_host_work depth: it runs on a helper meanwhile
    bool routed = false;           // given to a host thread to run (a worker's `assigned`, the helpers' queue, `next`)
    bool ended = false;
    u64 woken_order = 0;           // (deterministic: the order threads without a CPU run in)
};

// A host thread that runs fibers: a worker, or a helper.
struct Host {
    bool worker = false;
    nx::thread_parker parker;
    nx::fiber::Context context;     // the host thread's own, while one of its fibers runs
    GuestFiber* running = nullptr;
    GuestFiber* assigned = nullptr;  // (a worker) the fiber it was woken to run
    nx::vector<GuestFiber*> next;    // (the deterministic worker) the fibers it may run
    i32 last_cpu = -1;               // (the deterministic worker) the CPU whose holder it ran last
    bool waking = false;             // (a helper) woken, or made, for the helpers' queue, and not back in its loop yet
    nx::thread thread;
    nx::string name;
};

constexpr u32 kMaxHelpers = 64;
constexpr u32 kSpareHelpers = 2;  // (made at the start: the first blocking work needs no new OS thread)

GuestFiber* fiber_of(const Thread* t) { return static_cast<GuestFiber*>(t->backend); }

class FiberBackend final : public ThreadBackend {
public:
    FiberBackend(const BackendHost& host, bool deterministic) : host_(host), deterministic_(deterministic) {
        nx::scoped_lock<nx::mutex> held(*host_.lock);
        const u32 workers = deterministic_ ? 1 : host_.cpu_count;
        for (u32 c = 0; c < workers; ++c) {
            auto w = nx::make_unique<Host>();
            w->worker = true;
            w->name = deterministic_ ? nx::string("guest CPUs") : nx::format("guest worker {}", c);
            Host* raw = w.get();
            workers_.push_back(std::move(w));
            launch(raw);
        }
        if (!deterministic_) {
            for (u32 i = 0; i < kSpareHelpers; ++i) spawn_helper();
            nx::thread_options options;
            options.name = "guest timers";
            ++alive_;
            ++made_;
            timer_ = nx::thread([this] { timer_loop(); }, options);
        }
    }

    ~FiberBackend() override {
        // (stop() has ended every loop, or the kernel leaks this: its host threads would use it)
        for (auto& w : workers_)
            if (w->thread.joinable()) w->thread.join();
        for (auto& h : helpers_)
            if (h->thread.joinable()) h->thread.join();
        if (timer_.joinable()) timer_.join();
    }

    const char* name() const override { return deterministic_ ? "fibers (deterministic)" : "fibers"; }

    void start(Thread* t, usize host_stack) override {
        auto* f = new GuestFiber;
        f->owner = this;
        f->thread = t;
        f->pool = pool_for(host_stack);
        auto stack = f->pool->acquire();
        if (!stack) {
            nx::loge(logs::kernel, "thread '{}': no fiber stack of {} MiB", host_.name_of(t), host_stack >> 20);
            std::abort();
        }
        f->stack = *stack;
        f->context.reset(f->stack, &fiber_entry);
        t->backend = f;
        // (parked until given a CPU, or woken: it then runs run_thread from the start)
    }

    void park(Thread* t, std::optional<u64> deadline_ns) override {
        GuestFiber* f = fiber_of(t);
        if (!f) return park_native(t, deadline_ns);
        if (f->woken) {
            f->woken = false;
            return;
        }
        if (deadline_ns && now() >= *deadline_ns) return;
        f->parked = true;
        f->timed_out = false;
        f->deadline = deadline_ns;
        if (deadline_ns) add_timed(f);
        suspend(f);
        // (resumed, with the lock, maybe on another host thread)
        f->woken = false;
        if (f->deadline) remove_timed(f);
        f->deadline.reset();
        f->timed_out = false;
    }

    void unpark(Thread* t) override {
        GuestFiber* f = fiber_of(t);
        if (!f) return t->parker.unpark();
        f->woken = true;
        f->woken_order = ++order_;
        route(f);
    }

    void give_cpu(Thread* t) override {
        GuestFiber* f = fiber_of(t);
        if (!f) return t->parker.unpark();
        if (!f->on) unpark(t);  // (one running goes on where it is)
    }

    bool on_cpu(Thread* t) override {
        GuestFiber* f = fiber_of(t);
        if (!f || deterministic_) return t->cpu >= 0;
        if (f->host_work == 0 && t->cpu >= 0 && f->on && !f->on->worker && !idle_workers_.empty()) {
            // (holding a CPU on a helper: back to a free worker, leaving the helpers to host work)
            f->parked = false;
            suspend(f);
        }
        return t->cpu >= 0;
    }

    void begin_host_work(Thread* t) override {
        GuestFiber* f = fiber_of(t);
        if (!f || f->host_work++ > 0 || deterministic_) return;
        if (f->on && f->on->worker) {
            f->parked = false;
            suspend(f);  // (on to a helper: its worker free for the CPUs' holders)
        }
    }

    void end_host_work(Thread* t) override {
        // (back to a worker when it next goes on to guest code holding a CPU: on_cpu)
        if (GuestFiber* f = fiber_of(t); f && f->host_work) --f->host_work;
    }

    void yield(Thread* t) override {
        // (deterministic: the next CPU's holder runs now, if one can)
        GuestFiber* f = fiber_of(t);
        if (!f || !deterministic_ || !f->on) return;
        bool other = false;
        for (GuestFiber* g : workers_[0]->next) other |= g != f && runnable(g);
        if (!other) return;
        f->parked = false;
        suspend(f);
    }

    void end(Thread* t) override {
        if (GuestFiber* f = fiber_of(t)) {
            f->thread = nullptr;
            t->backend = nullptr;
        }
    }

    void stop() override {
        quit_ = true;
        for (auto& w : workers_) w->parker.unpark();
        for (auto& h : helpers_) h->parker.unpark();
        timer_parker_.unpark();
    }

    bool stopped() const override { return alive_ == 0; }
    u64 host_threads_made() const override { return made_; }

    nx::string where(const Thread* t) const override {
        const GuestFiber* f = fiber_of(t);
        if (!f) return {};
        if (f->on) return nx::format("on '{}'", f->on->name);
        if (f->routed) return nx::string("fiber about to run");
        return f->parked ? nx::string("fiber parked") : nx::string("fiber suspended");
    }

private:
    BackendHost host_;
    const bool deterministic_;
    nx::vector<nx::unique_ptr<Host>> workers_;
    nx::vector<nx::unique_ptr<Host>> helpers_;
    nx::vector<Host*> idle_workers_, idle_helpers_;
    nx::vector<GuestFiber*> helper_queue_;
    nx::vector<GuestFiber*> timed_;
    nx::thread timer_;
    nx::thread_parker timer_parker_;
    u64 timer_target_ = ~0ull;  // (the deadline the timer sleeps until)
    nx::flat_hash_map<usize, nx::unique_ptr<nx::fiber::StackPool>> pools_;
    bool quit_ = false;
    u32 alive_ = 0;  // (its host threads still in their loops)
    u64 made_ = 0;
    u64 order_ = 0;
    usize helpers_waking_ = 0;  // (helpers woken or made for the queue that have not taken the lock since)

    u64 now() const { return host_.now(host_.impl); }

    nx::fiber::StackPool* pool_for(usize size) {
        auto& pool = pools_[size];
        if (!pool)
            pool = nx::make_unique<nx::fiber::StackPool>(
                nx::fiber::StackPool::Config{.stack_size = size, .initial_commit = 64 * 1024, .max_cached = 4});
        return pool.get();
    }

    // ---- where a fiber runs

    static bool runnable(const GuestFiber* f) {
        return !f->on && !f->ended && (!f->parked || f->woken || f->timed_out);
    }

    // A fiber that may run given to a host thread: holding a CPU (and no host work), a free worker; otherwise, or with
    // no worker free, a helper. (Woken threads without a CPU sent to free workers too were measured no faster in P4D.)
    void route(GuestFiber* f) {
        if (!runnable(f) || f->routed) return;
        f->routed = true;
        if (deterministic_) {
            Host* w = workers_[0].get();
            w->next.push_back(f);
            w->parker.unpark();
            return;
        }
        if (f->host_work == 0 && f->thread->cpu >= 0 && !idle_workers_.empty()) {
            Host* w = idle_workers_.back();
            idle_workers_.pop_back();
            w->assigned = f;
            w->parker.unpark();
            return;
        }
        helper_queue_.push_back(f);
        if (helper_queue_.size() <= helpers_waking_) return;  // (enough helpers on their way)
        if (!idle_helpers_.empty()) {
            Host* h = idle_helpers_.back();
            idle_helpers_.pop_back();
            h->waking = true;
            ++helpers_waking_;
            h->parker.unpark();
        } else if (helpers_.size() < kMaxHelpers && !quit_) {
            spawn_helper()->waking = true;
            ++helpers_waking_;
        }
    }

    // The calling fiber leaves its host thread (lock held), back when resumed.
    // (its floating-point state, the FPCR mirror of native floating point, back on whichever host thread resumes it)
    static void suspend(GuestFiber* f) {
        const FloatState state = float_state();
        nx::fiber::switch_to(f->context, f->on->context, nullptr);
        set_float_state(state);
    }

    // A host thread runs `f` until it suspends or ends (lock held throughout).
    void run(Host* h, GuestFiber* f) {
        f->routed = false;
        f->on = h;
        f->parked = false;
        h->running = f;
        host_.set_current(f->thread);
        host_.enter_lane(f->thread);
        nx::fiber::switch_to(h->context, f->context, f);
        leave_guest_float();
        host_.enter_lane(nullptr);
        host_.set_current(nullptr);
        h->running = nullptr;
        f->on = nullptr;
        if (f->ended) {
            f->pool->release(f->stack);
            delete f;
            return;
        }
        route(f);  // (it moves, or was woken while it suspended)
    }

    static void fiber_entry(nx::fiber::Transfer transfer) {
        auto* f = static_cast<GuestFiber*>(transfer.data);
        FiberBackend* b = f->owner;
        // (first resumed with the lock held, as always; run_thread takes it itself)
        b->host_.lock->unlock();
        b->host_.run(b->host_.impl, f->thread);
        b->host_.lock->lock();
        // (the thread may be gone, or started again with a new fiber: only this run's own state is left)
        f->ended = true;
        nx::fiber::exit_to(f->context, f->on->context, nullptr);
    }

    // ---- host threads

    void launch(Host* h) {
        ++alive_;
        ++made_;
        nx::thread_options options;
        options.name = h->name.c_str();
        h->thread = nx::thread([this, h] { host_loop(h); }, options);
    }

    Host* spawn_helper() {
        auto h = nx::make_unique<Host>();
        h->name = nx::format("guest helper {}", helpers_.size());
        Host* raw = h.get();
        helpers_.push_back(std::move(h));
        launch(raw);
        return raw;
    }

    GuestFiber* next_for(Host* h) {
        if (deterministic_) return next_deterministic(h);
        if (h->worker) {
            GuestFiber* f = h->assigned;
            h->assigned = nullptr;
            return f;
        }
        if (helper_queue_.empty()) return nullptr;
        GuestFiber* f = helper_queue_.front();
        helper_queue_.erase(helper_queue_.begin());
        return f;
    }

    // The deterministic worker's next: the CPUs' holders in turn after the one it ran last, then the threads without
    // a CPU in the order they were woken.
    GuestFiber* next_deterministic(Host* h) {
        GuestFiber* best = nullptr;
        const i32 cpus = static_cast<i32>(host_.cpu_count);
        for (i32 k = 1; k <= cpus && !best; ++k) {
            const i32 cpu = (h->last_cpu + k + cpus) % cpus;
            for (GuestFiber* f : h->next)
                if (f->thread && f->thread->cpu == cpu) best = f;
        }
        if (best) {
            h->last_cpu = best->thread->cpu;
        } else {
            for (GuestFiber* f : h->next)
                if (!best || f->woken_order < best->woken_order) best = f;
        }
        if (best) h->next.erase(std::find(h->next.begin(), h->next.end(), best));
        return best;
    }

    void host_loop(Host* h) {
        host::frame_thread();  // (guest threads run here: the frames' work)
        nx::vector<Host*>& idle = h->worker ? idle_workers_ : idle_helpers_;
        nx::scoped_lock<nx::mutex> held(*host_.lock);
        while (!quit_) {
            if (h->waking) {
                h->waking = false;
                --helpers_waking_;
            }
            if (GuestFiber* f = next_for(h)) {
                run(h, f);
                continue;
            }
            if (deterministic_ && advance_clock()) continue;
            if (!deterministic_) idle.push_back(h);
            host_.lock->unlock();
            h->parker.park();
            host_.lock->lock();
            idle.erase(std::remove(idle.begin(), idle.end(), h), idle.end());  // (unless a route took it)
        }
        --alive_;
        host_.changed->notify_all();
    }

    // ---- deadlines

    void add_timed(GuestFiber* f) {
        timed_.push_back(f);
        if (!deterministic_ && *f->deadline < timer_target_) timer_parker_.unpark();
    }
    void remove_timed(GuestFiber* f) { timed_.erase(std::remove(timed_.begin(), timed_.end(), f), timed_.end()); }

    // The parked threads whose deadline has come woken; the earliest deadline still to come (~0: none).
    u64 expire(u64 n) {
        u64 earliest = ~0ull;
        for (usize i = 0; i < timed_.size();) {
            GuestFiber* f = timed_[i];
            if (*f->deadline <= n) {
                timed_.erase(timed_.begin() + static_cast<std::ptrdiff_t>(i));
                f->timed_out = true;
                f->woken_order = ++order_;
                route(f);
                continue;
            }
            earliest = nx::min(earliest, *f->deadline);
            ++i;
        }
        return earliest;
    }

    void timer_loop() {
        nx::scoped_lock<nx::mutex> held(*host_.lock);
        while (!quit_) {
            const u64 n = now();
            const u64 earliest = expire(n);
            timer_target_ = earliest;
            host_.lock->unlock();
            if (earliest == ~0ull) {
                timer_parker_.park();
            } else {
                timer_parker_.park_until(std::chrono::steady_clock::now() + std::chrono::nanoseconds(earliest - n));
            }
            host_.lock->lock();
        }
        --alive_;
        host_.changed->notify_all();
    }

    // (deterministic) Nothing can run: the clock moves on to the next deadline. False if there is none.
    bool advance_clock() {
        if (timed_.empty()) return false;
        u64 earliest = ~0ull;
        for (GuestFiber* f : timed_) earliest = nx::min(earliest, *f->deadline);
        if (host_.clock && host_.clock->load(std::memory_order_relaxed) < earliest)
            host_.clock->store(earliest, std::memory_order_relaxed);
        expire(earliest);
        return true;
    }

    // ---- threads that are not fibers (adopted host threads) wait natively

    void park_native(Thread* t, std::optional<u64> deadline_ns) {
        std::chrono::steady_clock::time_point until = std::chrono::steady_clock::time_point::max();
        if (deadline_ns) {
            const u64 n = now();
            until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(*deadline_ns > n ? *deadline_ns - n : 0);
        }
        host_.lock->unlock();
        t->parker.park_until(until);
        host_.lock->lock();
    }
};

}  // namespace

nx::unique_ptr<ThreadBackend> make_fiber_backend(const BackendHost& host, bool deterministic) {
    return nx::make_unique<FiberBackend>(host, deterministic);
}

}  // namespace psvrecore::runtime::kernel

#endif  // PSVRECORE_GUEST_FIBERS
