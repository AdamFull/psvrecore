#pragma once

// How guest threads run on the host: the kernel's thread backend.
//
// The scheduler (Kernel::Impl) decides which guest thread holds which of the console's CPUs, and when a thread waits;
// a backend makes that so on the host, through these calls only:
//   - start: a started guest thread gets a host context of its own, which runs Impl::run_thread;
//   - park / unpark: the calling thread waits until woken or a deadline; a thread is woken;
//   - give_cpu: a thread was given a CPU;
//   - on_cpu: the calling thread holds a CPU and goes on to guest code;
//   - begin_host_work / end_host_work: the calling thread does host work that may block (file I/O, rendering), which
//     must not hold up the host context its CPU runs on;
//   - a thread's run ends when run_thread returns.
//
// Native (backend_native.cpp): each started guest thread is an OS thread of its own, made for that run and ended
// with it (no reuse); park/unpark are its parker. Fibers (backend_fibers.cpp, PSVRECORE_GUEST_FIBERS): each guest
// thread is a fiber, run by one host worker per CPU while it holds that CPU, and by helper threads while it has none
// (the kernel's own work after a wake, host work that blocks).
//
// Every call but start's thread body runs with the kernel's lock held; park lets it go meanwhile.

#include <atomic>
#include <optional>

#include "psvrecore/runtime/kernel.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime::kernel {

// What a backend needs of the kernel.
struct BackendHost {
    nx::mutex* lock = nullptr;
    u32 cpu_count = 0;
    void* impl = nullptr;
    nx::cond_var* changed = nullptr;  // (notified when one of the backend's host threads ends)
    void (*run)(void* impl, Thread* t) = nullptr;  // Impl::run_thread (no lock held)
    u64 (*now)(const void* impl) = nullptr;       // Impl::now_ns: the kernel's clock, which deadlines are on
    const char* (*name_of)(const Thread* t) = nullptr;
    void (*set_current)(Thread* t) = nullptr;     // the calling host thread runs `t` (null: none)
    void (*enter_lane)(Thread* t) = nullptr;      // the profiler's lane of `t` (null: the host thread's own)
    std::atomic<u64>* clock = nullptr;            // (deterministic) the kernel's clock, which the backend moves on
};

class ThreadBackend {
public:
    virtual ~ThreadBackend() = default;
    virtual const char* name() const = 0;

    // A thread started (Kernel::start_thread; no CPU given yet): it runs `run(t)` on a host context of its own, with
    // a host stack of `host_stack` bytes.
    virtual void start(Thread* t, usize host_stack) = 0;
    // The calling thread `t` waits until unpark(t) or the deadline (the kernel's clock), letting the lock go
    // meanwhile. An unpark before the park is not lost; it may also return early: callers check their condition.
    virtual void park(Thread* t, std::optional<u64> deadline_ns) = 0;
    virtual void unpark(Thread* t) = 0;
    // `t` was given a CPU (t->cpu) by the scheduler.
    virtual void give_cpu(Thread* t) = 0;
    // The calling thread holds its CPU and goes on to guest code (after a wait for one, or taking it back): with
    // fibers it moves to its CPU's worker first. False if it lost the CPU meanwhile (it waits for one again).
    virtual bool on_cpu(Thread* /*t*/) { return true; }
    // Around host work that may block on the calling thread's behalf (nested calls count).
    virtual void begin_host_work(Thread* /*t*/) {}
    virtual void end_host_work(Thread* /*t*/) {}
    // (deterministic) At a kernel call of the calling thread: the next CPU's holder runs now.
    virtual void yield(Thread* /*t*/) {}
    // `t`'s run ended (Impl::finish_thread): the backend keeps nothing of it past the end of run_thread.
    virtual void end(Thread* /*t*/) {}
    // The kernel shuts down (every guest thread asked to end): the backend's own host threads end once no guest
    // thread is left. True once they have.
    virtual void stop() {}
    virtual bool stopped() const { return true; }
    // (diagnostics) The host threads the backend has made so far; where a thread is now (empty: on its own).
    virtual u64 host_threads_made() const = 0;
    virtual nx::string where(const Thread* /*t*/) const { return {}; }
};

nx::unique_ptr<ThreadBackend> make_native_backend(const BackendHost& host);
#if PSVRECORE_GUEST_FIBERS
// `deterministic`: one host worker runs every CPU in turn, switching at each kernel call (see backend_fibers.cpp).
nx::unique_ptr<ThreadBackend> make_fiber_backend(const BackendHost& host, bool deterministic);
#endif

}  // namespace psvrecore::runtime::kernel
