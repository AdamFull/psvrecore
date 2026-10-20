#pragma once

// The kernel: guest threads, their scheduling, and the objects they synchronise with.
//
// Every guest thread runs on a host thread of its own, but runs only while it holds one of the console's CPUs (the
// three a game has). The kernel hands CPUs out as the console's scheduler does:
//   - fixed priorities (a lower number is a higher priority), FIFO among equal priorities;
//   - no time slicing: a running thread keeps its CPU until it waits, ends, or a thread of higher priority needs it;
//   - a thread runs only on the CPUs its affinity mask allows.
// A thread that becomes ready takes a free CPU, or the CPU of the lowest-priority running thread below its own
// priority. The thread that loses its CPU stops at its next kernel call: a host thread cannot be stopped between two
// guest instructions, so until then both run (KNOWN_ISSUES.md).
//
// One lock guards all kernel state. A thread waits on a host condition variable of its own; whoever ends the wait
// (a signal, a timeout, deletion of the object) records the result and makes the thread ready.

#include <atomic>
#include <memory>

#include "psvrecore/recomp/guest.h"
#include "psvrecore/runtime/errors.hpp"
#include "psvrecore/runtime/memory.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

using Uid = i32;

namespace kernel {

inline constexpr i32 kHighestPriority = 64;
inline constexpr i32 kLowestPriority = 191;
inline constexpr i32 kDefaultPriority = 0x10000100;  // SCE_KERNEL_DEFAULT_PRIORITY_USER (+/- an offset)
inline constexpr i32 kGamePriority = 160;            // what the default priority means for a game
inline constexpr u32 kCpuMaskShift = 16;
inline constexpr u32 kUserCpuCount = 3;
inline constexpr i32 kAllUserCpus = 0x7 << kCpuMaskShift;
inline constexpr u32 kStackSizeMin = 0x1000, kStackSizeMax = 0x2000000, kMainStackSize = 0x40000;
inline constexpr u32 kTlsSize = 0x1000;  // per thread: the thread's TLS slots (sceKernelGetTLSAddr) and more

// Wait queue order.
inline constexpr u32 kAttrThreadPriority = 0x2000;  // SCE_KERNEL_ATTR_TH_PRIO (otherwise FIFO)
inline constexpr u32 kAttrMulti = 0x1000;           // event flags: several threads may wait

enum class ObjectClass : u32 {
    Thread = 1,
    Semaphore = 2,
    EventFlag = 3,
    Mutex = 4,
    Cond = 5,
    Callback = 8,
    LwMutex = 10,
    LwCond = 11,
};

enum class ThreadState : u32 {
    Running = 0x01,
    Ready = 0x02,
    Waiting = 0x08,
    Dormant = 0x10,
    Deleted = 0x20,
};

// Wait types (SCE_KERNEL_WAITTYPE_*), as reported in thread information.
enum class WaitType : u32 {
    None = 0,
    Delay = 0x1,
    ThreadEnd = 0x2,
    Signal = 0x4,
    EventFlag = 0x10,
    Semaphore = 0x20,
    Mutex = 0x40,
    CondSignal = 0x100,
    CondMutex = 0x200,
    LwMutex = 0x100000,
    LwCondSignal = 0x200000,
    LwCondMutex = 0x400000,
};

struct Thread;

struct Object {
    ObjectClass cls;
    Uid uid = 0;
    nx::string name;
    u32 attr = 0;
    explicit Object(ObjectClass c) : cls(c) {}
    virtual ~Object() = default;
};

// Threads waiting on an object, in wake-up order: FIFO, or by priority (FIFO among equal priorities).
struct WaitQueue {
    bool by_priority = false;
    nx::vector<Thread*> threads;

    void add(Thread* t);
    void remove(Thread* t);
    bool empty() const { return threads.empty(); }
    Thread* front() const { return threads.empty() ? nullptr : threads.front(); }
};

// What a waiting thread waits for, and the result its wait ends with.
struct Wait {
    WaitType type = WaitType::None;
    Uid object = 0;
    WaitQueue* queue = nullptr;
    bool done = false;
    i32 result = 0;
    // (per object kind)
    u32 count = 0;          // semaphore: resources wanted; mutex: lock count wanted
    u32 pattern = 0;        // event flag: bits; on wake: the flag's pattern
    u32 mode = 0;           // event flag: wait mode
    u32 restore_count = 0;  // condition variable: the mutex lock count to take back
};

struct Callback;

struct Thread : Object {
    Thread() : Object(ObjectClass::Thread) {}

    // As created.
    u32 entry = 0;
    i32 init_priority = 0;
    i32 init_affinity = 0;
    u32 stack_size = 0;
    u32 stack = 0;  // guest address of the stack's lowest byte
    u32 tls = 0;    // guest address of the TLS area

    // Scheduling.
    ThreadState state = ThreadState::Dormant;
    i32 priority = 0;
    i32 affinity = 0;  // CPU mask (bits 16-18)
    i32 cpu = -1;      // the CPU it holds, or -1
    i32 last_cpu = -1;
    u64 ready_order = 0;  // FIFO position among ready threads of its priority
    std::atomic<bool> lost_cpu{false};  // set when its CPU was taken while it runs guest code
    // (the time it has held a CPU, and been ready without one: host clock, ns)
    u64 held_ns = 0, held_since = 0, ready_ns = 0, ready_since = 0;
    nx::thread_parker parker;  // its host thread waits here (to the microsecond, which a futex timeout is not)

    Wait wait;
    i32 exit_status = 0;
    bool started_once = false;
    bool delete_on_exit = false;
    bool terminate = false;  // the kernel is shutting down: end at the next kernel call
    u32 signal = 0;          // sceKernelSendSignal: one pending signal
    nx::vector<Uid> callbacks;
    nx::vector<Uid> owned_mutexes;
    WaitQueue end_waiters;   // sceKernelWaitThreadEnd

    PsvContext ctx{};
};

struct Semaphore : Object {
    Semaphore() : Object(ObjectClass::Semaphore) {}
    i32 count = 0, max = 0, init = 0;
    WaitQueue waiters;
};

struct EventFlag : Object {
    EventFlag() : Object(ObjectClass::EventFlag) {}
    u32 pattern = 0, init = 0;
    WaitQueue waiters;
};

struct Mutex : Object {
    explicit Mutex(ObjectClass c = ObjectClass::Mutex) : Object(c) {}
    Thread* owner = nullptr;
    i32 count = 0, init = 0;
    WaitQueue waiters;
    u32 work = 0;  // lightweight mutexes: the guest work area
    bool recursive() const { return attr & 0x2; }
};

struct Cond : Object {
    explicit Cond(ObjectClass c = ObjectClass::Cond) : Object(c) {}
    Uid mutex = 0;
    WaitQueue waiters;
    u32 work = 0;
};

struct Callback : Object {
    Callback() : Object(ObjectClass::Callback) {}
    Thread* thread = nullptr;
    u32 function = 0, common = 0;
    i32 notify_count = 0, notify_arg = 0;
};

// Thrown through guest code to end the current thread (sceKernelExitThread, a kernel shutdown).
struct ThreadExit {
    i32 status;
    bool delete_thread = false;
};

// A timeout from a guest pointer to microseconds: none (wait forever) or a count, updated on return.
struct Timeout {
    u32* remaining = nullptr;  // host view of the guest variable, or nullptr
};

}  // namespace kernel

class Kernel {
public:
    struct Config {
        u32 cpu_count = kernel::kUserCpuCount;
        Region stack_region{0xE0000000u, 0xF0000000u};  // thread stacks and TLS areas
    };

    Kernel(GuestMemory& memory, const Config& config);
    explicit Kernel(GuestMemory& memory) : Kernel(memory, Config{}) {}
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    GuestMemory& memory() { return memory_; }

    // The guest thread the calling host thread runs, or nullptr.
    static kernel::Thread* current();
    // At the end of each kernel call: give up a CPU the scheduler took, and end if the kernel is shutting down.
    void checkpoint();
    // Microseconds since the kernel started (the process time).
    u64 process_time() const;
    // Nanoseconds since the kernel started: the clock every guest-visible time comes from (process time, the
    // display's vertical blanks, audio output, input timestamps), so they share one epoch.
    u64 clock_ns() const;
    // Around host work that blocks (file I/O): the calling thread gives its CPU to others meanwhile, as a thread
    // waiting for a device does on the console, and takes one back after.
    void begin_blocking();
    void end_blocking();
    struct Blocking {
        Kernel& k;
        explicit Blocking(Kernel& kernel) : k(kernel) { k.begin_blocking(); }
        ~Blocking() noexcept(false) { k.end_blocking(); }
    };

    // ---- threads
    Uid create_thread(nx::string_view name, u32 entry, i32 priority, u32 stack_size, u32 attr, i32 affinity);
    i32 start_thread(Uid uid, u32 arg_size, u32 arg_block);
    // Ends the calling thread: never returns (unwinds the thread's host stack).
    [[noreturn]] void exit_thread(i32 status, bool delete_thread = false);
    i32 delete_thread(Uid uid);
    i32 wait_thread_end(Uid uid, i32* status, kernel::Timeout timeout, bool callbacks);
    i32 delay_thread(u32 microseconds, bool callbacks);
    i32 change_priority(Uid uid, i32 priority);
    i32 change_affinity(Uid uid, i32 mask);
    i32 get_affinity(Uid uid);
    i32 current_priority();
    Uid current_thread_id();
    i32 get_exit_status(Uid uid, i32* status);
    struct ThreadInfo {
        nx::string name;
        u32 attr, status, entry, stack, stack_size;
        i32 init_priority, priority, init_affinity, affinity, cpu, last_cpu;
        u32 wait_type;
        Uid wait_id;
        i32 exit_status;
    };
    i32 thread_info(Uid uid, ThreadInfo* out);
    i32 send_signal(Uid uid);
    i32 wait_signal(kernel::Timeout timeout);
    // The TLS slot `key` of the calling thread (0 if it has none).
    u32 tls_address(u32 key);

    // ---- semaphores
    Uid create_semaphore(nx::string_view name, u32 attr, i32 init, i32 max);
    i32 delete_semaphore(Uid uid);
    i32 wait_semaphore(Uid uid, i32 count, kernel::Timeout timeout, bool callbacks);
    i32 poll_semaphore(Uid uid, i32 count);
    i32 signal_semaphore(Uid uid, i32 count);
    i32 cancel_semaphore(Uid uid, i32 count, u32* waiting);
    i32 semaphore_info(Uid uid, kernel::Semaphore* out, u32* waiting);

    // ---- event flags
    Uid create_event_flag(nx::string_view name, u32 attr, u32 pattern);
    i32 delete_event_flag(Uid uid);
    i32 wait_event_flag(Uid uid, u32 bits, u32 mode, u32* result, kernel::Timeout timeout, bool callbacks);
    i32 poll_event_flag(Uid uid, u32 bits, u32 mode, u32* result);
    i32 set_event_flag(Uid uid, u32 bits);
    i32 clear_event_flag(Uid uid, u32 bits);  // pattern &= bits
    i32 cancel_event_flag(Uid uid, u32 pattern, u32* waiting);
    i32 event_flag_info(Uid uid, kernel::EventFlag* out, u32* waiting);

    // ---- mutexes (lightweight ones are the same objects, keyed by their guest work area)
    Uid create_mutex(nx::string_view name, u32 attr, i32 init, kernel::ObjectClass cls = kernel::ObjectClass::Mutex,
                     u32 work = 0);
    i32 delete_mutex(Uid uid, kernel::ObjectClass cls = kernel::ObjectClass::Mutex);
    i32 lock_mutex(Uid uid, i32 count, kernel::Timeout timeout, bool callbacks,
                   kernel::ObjectClass cls = kernel::ObjectClass::Mutex);
    i32 try_lock_mutex(Uid uid, i32 count, kernel::ObjectClass cls = kernel::ObjectClass::Mutex);
    i32 unlock_mutex(Uid uid, i32 count, kernel::ObjectClass cls = kernel::ObjectClass::Mutex);
    i32 cancel_mutex(Uid uid, i32 count, u32* waiting);
    i32 mutex_info(Uid uid, kernel::ObjectClass cls, kernel::Mutex* out, Uid* owner, u32* waiting);

    // ---- condition variables
    Uid create_cond(nx::string_view name, u32 attr, Uid mutex, kernel::ObjectClass cls = kernel::ObjectClass::Cond,
                    u32 work = 0);
    i32 delete_cond(Uid uid, kernel::ObjectClass cls = kernel::ObjectClass::Cond);
    i32 wait_cond(Uid uid, kernel::Timeout timeout, bool callbacks, kernel::ObjectClass cls = kernel::ObjectClass::Cond);
    i32 signal_cond(Uid uid, bool all, Uid to, kernel::ObjectClass cls = kernel::ObjectClass::Cond);

    // ---- callbacks
    Uid create_callback(nx::string_view name, u32 attr, u32 function, u32 common);
    i32 delete_callback(Uid uid);
    i32 notify_callback(Uid uid, i32 arg);
    i32 cancel_callback(Uid uid);
    i32 callback_count(Uid uid);
    // Runs the calling thread's notified callbacks; returns how many ran.
    i32 check_callbacks();

    // The process suspended, as the console suspends a game (the application sent to the background): no thread
    // runs (those running stop at their next kernel call; a thread in a loop that makes none runs on), and the
    // clock stands still, so no guest-visible time passes until resume().
    void suspend();
    void resume();
    bool suspended() const;
    // Asks every thread to end and waits for those blocked in the kernel; threads running guest code end at their
    // next kernel call. Called by the destructor.
    void shutdown();
    bool shutdown_complete() const;
    // Every thread: its state, priority, what it waits for, and its link register (where its last call returns to),
    // one line each. For debugging (PSVRECORE_THREADS).
    nx::string describe_threads();
    // Each thread's time so far holding a CPU and ready without one (host clock, ns): what the scheduler gave it.
    struct CpuUse {
        Uid uid;
        nx::string name;
        i32 priority;
        u64 held_ns, ready_ns;
    };
    nx::vector<CpuUse> cpu_use();
    // Called on a host thread that runs no guest thread (a test, the process start-up): adopt it as a guest thread
    // with this name, priority and stack, holding a CPU, so it can make kernel calls. Ended by release_host_thread().
    Uid adopt_host_thread(nx::string_view name, i32 priority = kernel::kGamePriority, u32 stack_size = kernel::kMainStackSize);
    void release_host_thread();

    // The UID's object, if it is of class `cls` (used by the HLE layer for lightweight objects' work areas).
    Uid lw_uid(u32 work) const;

private:
    struct Impl;
    nx::unique_ptr<Impl> impl_;
    GuestMemory& memory_;
};

}  // namespace psvrecore::runtime
