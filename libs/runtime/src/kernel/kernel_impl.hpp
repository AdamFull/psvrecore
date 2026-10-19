#pragma once

// The kernel's shared state and the scheduler primitives every kernel source uses. Everything here runs with
// Impl::lock held, unless a comment says otherwise.

#include <atomic>
#include <optional>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/kernel.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime {

namespace kernel {
// Results of block() that are not guest error codes (never returned to the guest).
inline constexpr i32 kRunCallbacks = 1;
}  // namespace kernel

using Lock = nx::scoped_lock<nx::mutex>;

struct Kernel::Impl {
    Impl(Kernel& k, GuestMemory& m, const Config& c);

    Kernel& kernel;
    GuestMemory& memory;
    Config config;

    nx::mutex lock;
    nx::flat_hash_map<Uid, nx::unique_ptr<kernel::Object>> objects;
    nx::flat_hash_map<u32, Uid> lw_objects;  // lightweight mutexes and condition variables by work area
    u32 uid_counter = 0;
    u32 thread_count = 0;
    u32 sync_count = 0;

    nx::vector<kernel::Thread*> cpus;   // the thread each CPU runs
    nx::vector<kernel::Thread*> ready;  // ready threads without a CPU
    i64 back_order = 0, front_order = 0;

    u64 start_ns = 0;
    // Suspended (Kernel::suspend): no CPU is given out, and the clock stands at `suspended_at`; the time spent
    // suspended is taken off the clock (`paused_ns`), so the guest sees none of it.
    bool suspended = false;
    std::atomic<u64> suspended_at{0};  // (0: running)
    std::atomic<u64> paused_ns{0};
    bool shutting_down = false;
    bool stuck = false;  // shutdown gave up waiting for host threads
    u32 host_threads = 0;  // host threads running a guest thread
    nx::cond_var host_threads_changed;

    // Host threads kept running for guest threads to start on: starting an OS thread can take tens of
    // milliseconds, starting a guest thread on a waiting one takes microseconds, as on the console.
    struct HostWorker {
        nx::thread_parker parker;
        kernel::Thread* job = nullptr;
        bool quit = false;
    };
    nx::vector<HostWorker*> idle_workers;
    u32 workers_alive = 0, workers_starting = 0;
    void spawn_worker();
    void worker_loop(HostWorker* w);

    // ---- objects
    Uid new_uid();
    template <class T>
    T* get(Uid uid, kernel::ObjectClass cls) {
        auto it = objects.find(uid);
        if (it == objects.end() || it->second->cls != cls) return nullptr;
        return static_cast<T*>(it->second.get());
    }
    kernel::Thread* thread(Uid uid);  // 0: the calling thread
    template <class T>
    T* add(nx::unique_ptr<T> object, nx::string_view name, u32 attr) {
        T* raw = object.get();
        raw->uid = new_uid();
        raw->name = nx::string(name);
        raw->attr = attr;
        objects[raw->uid] = std::move(object);
        return raw;
    }

    // ---- scheduling
    static bool allowed(const kernel::Thread* t, i32 cpu);
    void make_ready(kernel::Thread* t, bool front = false);
    void release_cpu(kernel::Thread* t);
    void dispatch();
    void wait_for_cpu(kernel::Thread* t);
    // The calling thread (t) waits, without the lock, until woken or the deadline; then takes the lock again.
    void park(kernel::Thread* t, std::optional<u64> deadline_ns = std::nullopt);
    static void wake(kernel::Thread* t) { t->parker.unpark(); }
    // The current thread waits: gives up its CPU until the wait ends (end_wait, a timeout, a callback notification
    // when `callbacks`, or shutdown) and it has a CPU again. Returns the wait's result. `queue` (if any) must already
    // hold the thread. Throws ThreadExit on shutdown.
    i32 block(kernel::Thread* t, kernel::WaitQueue* queue, std::optional<u64> deadline_ns, bool callbacks);
    // Ends a thread's wait with `result` (it leaves its queue and becomes ready).
    void end_wait(kernel::Thread* t, i32 result);
    bool has_callbacks(const kernel::Thread* t) const;
    // Runs the current thread's notified callbacks (unlocks while they run). Returns how many ran.
    i32 run_callbacks(kernel::Thread* t);

    // A timeout's deadline from the guest's remaining-time variable; and the remaining time written back.
    std::optional<u64> deadline(const kernel::Timeout& timeout) const;
    void write_remaining(const kernel::Timeout& timeout, const std::optional<u64>& deadline) const;
    u64 now_ns() const;

    // ---- threads
    void run_thread(kernel::Thread* t);       // a guest thread's host thread (no lock held)
    void finish_thread(kernel::Thread* t, i32 status, bool delete_thread);
    void free_thread_memory(kernel::Thread* t);
    void unlock_owned_mutexes(kernel::Thread* t);
    // (sync.cpp) A mutex the thread owns is released as a whole: ownership passes to the next waiter.
    void hand_over_mutex(kernel::Mutex* m);
};

// The priority a create or change call asks for: SCE_KERNEL_DEFAULT_PRIORITY_USER (+/- offset) means the game
// default; otherwise 64-191. -1 if invalid.
i32 resolve_priority(i32 priority);

}  // namespace psvrecore::runtime
