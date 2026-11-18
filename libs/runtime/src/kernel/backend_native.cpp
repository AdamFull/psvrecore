// The native thread backend: each started guest thread is an OS thread of its own, made for that run and ended with
// it (no reuse: nothing of one run is left for the next, the host's tools see one thread per guest thread, named
// after it). A thread waits on its own parker.

#include <atomic>
#include <chrono>

#include "backend.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime::kernel {

namespace {

class NativeBackend final : public ThreadBackend {
public:
    explicit NativeBackend(const BackendHost& host) : host_(host) {}
    const char* name() const override { return "native"; }

    void start(Thread* t, usize host_stack) override {
        nx::thread_options options;
        options.name = host_.name_of(t);
        options.stack_size = host_stack;
        const BackendHost host = host_;
        nx::thread thread(
            [host, t] {
                host::frame_thread();  // (guest threads do the frames' work)
                host.run(host.impl, t);
            },
            options);
        thread.detach();
        made_.fetch_add(1, std::memory_order_relaxed);
    }

    void park(Thread* t, std::optional<u64> deadline_ns) override {
        // (the deadline taken from the kernel's clock now: the parker's clock runs on while the kernel's stands still
        // (suspended), so a wait may end early then, and its caller checks again)
        std::chrono::steady_clock::time_point until = std::chrono::steady_clock::time_point::max();
        if (deadline_ns) {
            const u64 now = host_.now(host_.impl);
            until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(*deadline_ns > now ? *deadline_ns - now : 0);
        }
        host_.lock->unlock();
        t->parker.park_until(until);
        host_.lock->lock();
    }

    void unpark(Thread* t) override { t->parker.unpark(); }
    void give_cpu(Thread* t) override { t->parker.unpark(); }
    u64 host_threads_made() const override { return made_.load(std::memory_order_relaxed); }

private:
    BackendHost host_;
    std::atomic<u64> made_{0};
};

}  // namespace

nx::unique_ptr<ThreadBackend> make_native_backend(const BackendHost& host) { return nx::make_unique<NativeBackend>(host); }

}  // namespace psvrecore::runtime::kernel
