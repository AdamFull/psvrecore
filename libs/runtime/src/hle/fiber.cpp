// Fibers (SceFiber): cooperative contexts within a thread. A thread runs a fiber (sceFiberRun), fibers switch to one
// another (sceFiberSwitch), and one returns to the thread that ran it (sceFiberReturnToThread); each call returns
// when its side is resumed, with the value the resumer passed.
//
// Recompiled code keeps a guest call chain on the host's stack as well as the guest's, so each fiber has a host
// stack of its own (nx::fiber's contexts) besides the guest stack the game gives it (its context memory). A switch
// saves the guest registers of the side that leaves, switches host stacks, and the side that resumes restores its
// own. Everything stays on the thread's OS thread: the kernel's current thread is the same throughout.

#include <array>

#include "hle_common.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/fibers/context.h"
#include "core/foundation/fibers/stack.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceFiber = 0xF88DAA70;
constexpr i32 kNull = err::code(0x80590001), kAlignment = err::code(0x80590002), kRange = err::code(0x80590003),
              kInvalid = err::code(0x80590004), kPermission = err::code(0x80590005), kState = err::code(0x80590006),
              kAgain = err::code(0x80590008);
constexpr u32 kContextMinimum = 512;

struct Fiber {
    u32 guest = 0;
    nx::string name;
    u32 entry = 0, arg_on_initialize = 0, context = 0, context_size = 0;
    nx::fiber::Stack stack;
    nx::fiber::Context host;
    bool started = false, running = false;
    PsvContext regs{};  // (its guest registers while it is not running)
};

// What a thread knows of its fibers: its own host context and registers while a fiber runs, and which one runs.
struct ThreadFibers {
    nx::fiber::Context host;
    PsvContext regs{};
    PsvContext* ctx = nullptr;  // (the thread's register context, which every fiber of it runs in)
    Fiber* current = nullptr;
};

// What one side hands the other on a switch: the fiber resumed (to start it), and the value.
struct Hand {
    Fiber* to = nullptr;
    u32 value = 0;
};

struct Fibers {
    nx::mutex lock;
    nx::hash_map<u32, nx::unique_ptr<Fiber>> fibers;  // by the guest SceFiber's address
    nx::fiber::StackPool stacks{nx::fiber::StackPool::Config{.stack_size = usize{8} << 20, .initial_commit = 64 * 1024}};
};
Fibers& fibers() {
    static Fibers f;
    return f;
}

thread_local ThreadFibers* t_fibers = nullptr;

ThreadFibers& this_thread(PsvContext* ctx) {
    if (!t_fibers) t_fibers = new ThreadFibers;  // (one per OS thread that ever runs a fiber; kept)
    t_fibers->ctx = ctx;
    return *t_fibers;
}

Fiber* find(u32 guest) {
    Fibers& f = fibers();
    nx::scoped_lock<nx::mutex> held(f.lock);
    auto it = f.fibers.find(guest);
    return it == f.fibers.end() ? nullptr : it->second.get();
}

[[noreturn]] void fiber_main(nx::fiber::Transfer t);

// Leaves the running side (the thread, or its current fiber) for `to` (null: the thread), handing it `value`; returns
// the value handed back when something resumes this side.
u32 resume(ThreadFibers& t, Fiber* to, u32 value) {
    Fiber* from = t.current;
    PsvContext* ctx = t.ctx;
    (from ? from->regs : t.regs) = *ctx;
    if (from) from->running = false;
    if (to) to->running = true;
    t.current = to;
    Hand hand{to, value};
    nx::fiber::Context& from_host = from ? from->host : t.host;
    nx::fiber::Context& to_host = to ? to->host : t.host;
    if (to && !to->started) {
        to->started = true;
        to->host.reset(to->stack, &fiber_main);
    }
    const nx::fiber::Transfer back = nx::fiber::switch_to(from_host, to_host, &hand);
    // (resumed: this side's registers back)
    ThreadFibers& now = *t_fibers;
    *now.ctx = from ? from->regs : now.regs;
    return static_cast<const Hand*>(back.data)->value;
}

// A fiber's first run: its entry, on its own guest stack, with its two arguments. It must never return.
void fiber_main(nx::fiber::Transfer t) {
    const Hand* hand = static_cast<const Hand*>(t.data);
    Fiber* f = hand->to;
    const u32 arg_on_run = hand->value;
    ThreadFibers& tf = *t_fibers;
    PsvContext* ctx = tf.ctx;
    // (its registers start as the thread's were, its stack at the top of its context memory)
    ctx->r[13] = (f->context + f->context_size) & ~7u;
    call_guest(ctx, f->entry, std::array<u32, 2>{f->arg_on_initialize, arg_on_run});
    nx::loge(logs::hle, "fiber '{}' returned from its entry 0x{:08x} (a fiber must switch away instead)", f->name, f->entry);
    psv_trap(ctx, f->entry, PSV_TRAP_RETURNED);
}

i32 _sceFiberInitializeImpl(Ptr<u8> fiber, Ptr<const char> name, u32 entry, u32 arg_on_initialize, u32 context,
                            u32 context_size, Ptr<const void> /*opt*/, u32 /*build_version*/) {
    if (!fiber || !entry || !context) return kNull;
    if ((fiber.address & 7) || (context & 7) || (context_size & 7)) return kAlignment;
    if (context_size < kContextMinimum) return kRange;
    if (!writable(fiber.address, 128) || !writable(context, context_size)) return kInvalid;
    Fibers& fs = fibers();
    auto stack = fs.stacks.acquire();
    if (!stack) return kAgain;
    auto f = nx::make_unique<Fiber>();
    f->guest = fiber.address;
    f->name = name ? str(name, 31) : nx::string();
    f->entry = entry, f->arg_on_initialize = arg_on_initialize, f->context = context, f->context_size = context_size;
    f->stack = *stack;
    std::memset(fiber.get(), 0, 128);
    nx::scoped_lock<nx::mutex> held(fs.lock);
    if (auto it = fs.fibers.find(fiber.address); it != fs.fibers.end()) {
        if (it->second->running) {
            fs.stacks.release(*stack);
            return kState;
        }
        fs.stacks.release(it->second->stack);
        fs.fibers.erase(it);
    }
    fs.fibers[fiber.address] = std::move(f);
    return 0;
}

i32 sceFiberFinalize(Ptr<u8> fiber) {
    if (!fiber) return kNull;
    Fibers& fs = fibers();
    nx::scoped_lock<nx::mutex> held(fs.lock);
    auto it = fs.fibers.find(fiber.address);
    if (it == fs.fibers.end()) return kInvalid;
    if (it->second->running) return kState;
    // (a fiber suspended part way keeps nothing alive but its host stack: given back)
    fs.stacks.release(it->second->stack);
    fs.fibers.erase(it);
    return 0;
}

i32 sceFiberRun(PsvContext* ctx, Ptr<u8> fiber, u32 arg_on_run_to, Ptr<u32> arg_on_return) {
    if (!fiber) return kNull;
    ThreadFibers& t = this_thread(ctx);
    if (t.current) return kPermission;  // (from a thread only)
    Fiber* f = find(fiber.address);
    if (!f) return kInvalid;
    if (f->running) return kState;
    const u32 back = resume(t, f, arg_on_run_to);
    if (arg_on_return && writable(arg_on_return.address, 4)) *arg_on_return = back;
    return 0;
}

i32 sceFiberSwitch(PsvContext* ctx, Ptr<u8> fiber, u32 arg_on_run_to, Ptr<u32> arg_on_run) {
    if (!fiber) return kNull;
    ThreadFibers& t = this_thread(ctx);
    if (!t.current) return kPermission;  // (from a fiber only)
    Fiber* f = find(fiber.address);
    if (!f) return kInvalid;
    if (f->running) return kState;
    const u32 back = resume(t, f, arg_on_run_to);
    if (arg_on_run && writable(arg_on_run.address, 4)) *arg_on_run = back;
    return 0;
}

i32 sceFiberReturnToThread(PsvContext* ctx, u32 arg_on_return, Ptr<u32> arg_on_run) {
    ThreadFibers& t = this_thread(ctx);
    if (!t.current) return kPermission;
    const u32 back = resume(t, nullptr, arg_on_return);
    if (arg_on_run && writable(arg_on_run.address, 4)) *arg_on_run = back;
    return 0;
}

i32 sceFiberGetSelf(PsvContext* ctx, Ptr<u32> out) {
    if (!out) return kNull;
    ThreadFibers& t = this_thread(ctx);
    if (!t.current) return kPermission;
    if (!writable(out.address, 4)) return kInvalid;
    *out = t.current->guest;
    return 0;
}

const Function kFiber[] = {
    HLE_FUNCTION(0xF24A298C, _sceFiberInitializeImpl),
    HLE_FUNCTION(0xE160F844, sceFiberFinalize),
    HLE_FUNCTION(0x7DF23243, sceFiberRun),
    HLE_FUNCTION(0xE4283144, sceFiberSwitch),
    HLE_FUNCTION(0x3B42921F, sceFiberReturnToThread),
    HLE_FUNCTION(0x414D8CA5, sceFiberGetSelf),
};

}  // namespace

void register_fiber() {
    {
        // (a new runtime starts with no fibers)
        Fibers& fs = fibers();
        nx::scoped_lock<nx::mutex> held(fs.lock);
        for (auto& [address, f] : fs.fibers) fs.stacks.release(f->stack);
        fs.fibers.clear();
    }
    register_library({"SceFiber", kSceFiber, kFiber});
}

}  // namespace psvrecore::runtime::hle
