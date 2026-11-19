// Fibers (SceFiber): cooperative contexts within a thread. A thread runs a fiber (sceFiberRun), fibers switch to one
// another (sceFiberSwitch), and one returns to the thread that ran it (sceFiberReturnToThread); each call returns
// when its side is resumed, with the value the resumer passed.
//
// Recompiled code keeps a guest call chain on the host's stack as well as the guest's, so each fiber has a host
// stack of its own (nx::fiber's contexts) besides the guest stack the game gives it (its context memory). A switch
// saves the guest registers of the side that leaves, switches host stacks, and the side that resumes restores its
// own. The kernel's current thread is the same throughout; its OS thread may not be (with fibers the kernel moves a
// guest thread between host threads at its waits), so what a thread knows of its fibers is the guest thread's
// (GuestLocal), read again after every switch.

#include <array>
#include <atomic>
#include <cstring>

#include "hle_common.hpp"
#include "psvrecore/profile.hpp"
#include "psvrecore/runtime/float_env.hpp"
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
// (the guest stack of a fiber made without context memory that is not given one later: see initialize)
constexpr u32 kOwnStackSize = 0x20000;

struct Fiber {
    u32 guest = 0;
    nx::string name;
    u32 entry = 0, arg_on_initialize = 0, context = 0, context_size = 0;
    u32 own_stack = 0;  // (guest memory mapped for it as its context, unmapped with it; 0: the game's)
    nx::fiber::Stack stack;
    nx::fiber::Context host;
    bool started = false, running = false;
    PsvContext regs{};  // (its guest registers while it is not running)
    profile::Track* track = nullptr;  // (its lane in the profiler)
    ~Fiber() { profile::release(track); }
};

// What a thread knows of its fibers: its own host context and registers while a fiber runs, and which one runs.
struct ThreadFibers {
    nx::fiber::Context host;
    PsvContext regs{};
    PsvContext* ctx = nullptr;  // (the thread's register context, which every fiber of it runs in)
    Fiber* current = nullptr;
    profile::Track* track = nullptr;  // (the thread's own lane in the profiler, while a fiber runs)
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

GuestLocal<ThreadFibers> g_thread_fibers;  // (one per guest thread's run that runs a fiber)

ThreadFibers& this_thread(PsvContext* ctx) {
    ThreadFibers& t = g_thread_fibers.get();
    t.ctx = ctx;
    return t;
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
    // (the profiler follows: what runs next is in its own lane)
    if (!from) t.track = profile::current();
    profile::enter(to ? to->track : t.track);
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
    const bool guest_float = in_guest_float();
    const nx::fiber::Transfer back = nx::fiber::switch_to(from_host, to_host, &hand);
    // (resumed: this side's registers back, its FPSCR's mirror with native floating point)
    ThreadFibers& now = g_thread_fibers.get();
    *now.ctx = from ? from->regs : now.regs;
    if (guest_float) enter_guest_float(now.ctx);
    return static_cast<const Hand*>(back.data)->value;
}

// A fiber's first run: its entry, on its own guest stack, with its two arguments. It must never return.
void fiber_main(nx::fiber::Transfer t) {
    const Hand* hand = static_cast<const Hand*>(t.data);
    Fiber* f = hand->to;
    const u32 arg_on_run = hand->value;
    ThreadFibers& tf = g_thread_fibers.get();
    PsvContext* ctx = tf.ctx;
    // (its registers start as the resumer's were, its stack at the top of its context memory; a fiber made without
    // context memory runs on the stack of the side that resumed it, below where that side waits: libult's workers
    // run their first fiber so)
    ctx->r[13] = f->context ? (f->context + f->context_size) & ~7u : (ctx->r[13] - 64) & ~7u;
    nx::logd(logs::hle, "fiber '{}' (0x{:08x}) starts at 0x{:08x}, sp 0x{:08x}", f->name, f->guest, f->entry, ctx->r[13]);
    call_guest(ctx, f->entry, std::array<u32, 2>{f->arg_on_initialize, arg_on_run});
    nx::loge(logs::hle, "fiber '{}' returned from its entry 0x{:08x} (a fiber must switch away instead)", f->name, f->entry);
    psv_trap(ctx, f->entry, PSV_TRAP_RETURNED);
}

// A fiber's end: its host stack given back, and the guest stack mapped for it, if any.
void forget(Fibers& fs, Fiber& f) {
    fs.stacks.release(f.stack);
    if (f.own_stack) memory().unmap(f.own_stack);
    f.own_stack = 0;
}

// A fiber may be made without its context memory (0 and 0). Through the internal-option form (`attached_later`) it
// is given one when it is first switched to (_sceFiberAttachContextAndSwitch: libult makes its threads' fibers so).
// Through the plain form it gets a guest stack of its own here (Gravity Rush runs its tasks of no stack size so, and
// its loading waits on them): on the stack of the side that resumes it, a fiber that switches away part way would
// have its frames overwritten by that side's.
i32 initialize(Ptr<u8> fiber, Ptr<const char> name, u32 entry, u32 arg_on_initialize, u32 context, u32 context_size,
               bool attached_later) {
    if (!fiber || !entry) return kNull;
    const bool none = !context && !context_size;
    if (!context && !none) return kNull;
    if ((fiber.address & 7) || (context & 7) || (context_size & 7)) return kAlignment;
    if (!none && context_size < kContextMinimum) return kRange;
    if (!writable(fiber.address, 128) || (!none && !writable(context, context_size))) return kInvalid;
    Fibers& fs = fibers();
    auto stack = fs.stacks.acquire();
    if (!stack) return kAgain;
    auto f = nx::make_unique<Fiber>();
    f->guest = fiber.address;
    f->name = name ? str(name, 31) : nx::string();
    if constexpr (profile::kEnabled) f->track = profile::acquire(f->name.empty() ? nx::string_view("fiber") : f->name);
    f->entry = entry, f->arg_on_initialize = arg_on_initialize, f->context = context, f->context_size = context_size;
    f->stack = *stack;
    if (none && !attached_later) {
        f->own_stack = memory().map_anywhere(layout::kStacks, kOwnStackSize, GuestMemory::kPageSize, Access::ReadWrite,
                                             BlockKind::Stack, f->name.empty() ? nx::string_view("fiber") : f->name);
        if (!f->own_stack) {
            fs.stacks.release(*stack);
            return kAgain;
        }
        f->context = f->own_stack, f->context_size = kOwnStackSize;
    }
    std::memset(fiber.get(), 0, 128);
    nx::scoped_lock<nx::mutex> held(fs.lock);
    if (auto it = fs.fibers.find(fiber.address); it != fs.fibers.end()) {
        if (it->second->running) {
            forget(fs, *f);
            return kState;
        }
        forget(fs, *it->second);
        fs.fibers.erase(it);
    }
    fs.fibers[fiber.address] = std::move(f);
    return 0;
}

i32 _sceFiberInitializeImpl(Ptr<u8> fiber, Ptr<const char> name, u32 entry, u32 arg_on_initialize, u32 context,
                            u32 context_size, Ptr<const void> /*opt*/, u32 /*build_version*/) {
    return initialize(fiber, name, entry, arg_on_initialize, context, context_size, false);
}

// The same with an internal option word before the build version (libult passes 2 for a fiber without context
// memory, 3 with): nothing else is told apart here.
i32 _sceFiberInitializeWithInternalOptionImpl(Ptr<u8> fiber, Ptr<const char> name, u32 entry, u32 arg_on_initialize,
                                              u32 context, u32 context_size, Ptr<const void> /*opt*/,
                                              u32 /*internal_option*/, u32 /*build_version*/) {
    return initialize(fiber, name, entry, arg_on_initialize, context, context_size, true);
}

i32 sceFiberFinalize(Ptr<u8> fiber) {
    if (!fiber) return kNull;
    Fibers& fs = fibers();
    nx::scoped_lock<nx::mutex> held(fs.lock);
    auto it = fs.fibers.find(fiber.address);
    if (it == fs.fibers.end()) return kInvalid;
    if (it->second->running) return kState;
    // (a fiber suspended part way keeps nothing alive but its host stack, and the guest stack mapped for it: given back)
    forget(fs, *it->second);
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

// Gives a fiber made without context memory its memory, then resumes it as sceFiberRun (from a thread: `from_thread`)
// or sceFiberSwitch (from a fiber) would.
i32 attach_and_resume(PsvContext* ctx, Ptr<u8> fiber, u32 context, u32 context_size, u32 arg_on_run_to, Ptr<u32> arg_back,
                      bool from_thread) {
    if (!fiber || !context) return kNull;
    if ((context & 7) || (context_size & 7)) return kAlignment;
    if (context_size < kContextMinimum) return kRange;
    if (!writable(context, context_size)) return kInvalid;
    ThreadFibers& t = this_thread(ctx);
    if (from_thread && t.current) return kPermission;
    Fiber* f = find(fiber.address);
    if (!f) return kInvalid;
    if (f->running || f->started) return kState;
    f->context = context, f->context_size = context_size;
    nx::logd(logs::hle, "fiber '{}' (0x{:08x}): context 0x{:08x}, 0x{:x} bytes attached", f->name, f->guest, context, context_size);
    const u32 back = resume(t, f, arg_on_run_to);
    if (arg_back && writable(arg_back.address, 4)) *arg_back = back;
    return 0;
}
i32 _sceFiberAttachContextAndSwitch(PsvContext* ctx, Ptr<u8> fiber, u32 context, u32 context_size, u32 arg_on_run_to,
                                    Ptr<u32> arg_on_run) {
    return attach_and_resume(ctx, fiber, context, context_size, arg_on_run_to, arg_on_run, false);
}
i32 _sceFiberAttachContextAndRun(PsvContext* ctx, Ptr<u8> fiber, u32 context, u32 context_size, u32 arg_on_run_to,
                                 Ptr<u32> arg_on_return) {
    return attach_and_resume(ctx, fiber, context, context_size, arg_on_run_to, arg_on_return, true);
}

// SceFiberOptParam: 128 reserved bytes, zeroed.
i32 sceFiberOptParamInitialize(Ptr<u8> opt) {
    if (!opt) return kNull;
    if (opt.address & 7) return kAlignment;
    if (!writable(opt.address, 128)) return kInvalid;
    std::memset(opt.get(), 0, 128);
    return 0;
}

// The running fiber's name (what sceFiberGetInfo reports).
i32 sceFiberRenameSelf(PsvContext* ctx, Ptr<const char> name) {
    if (!name) return kNull;
    ThreadFibers& t = this_thread(ctx);
    if (!t.current) return kPermission;
    Fibers& fs = fibers();
    nx::scoped_lock<nx::mutex> held(fs.lock);
    t.current->name = str(name, 31);
    return 0;
}

// The context memory's use is not measured (sceFiberGetInfo's margin stays 0); profiler markers are not recorded.
i32 sceFiberStartContextSizeCheck(u32 /*flags*/) { return 0; }
i32 sceFiberStopContextSizeCheck() { return 0; }
// The fiber's markers: zones on its lane in the profiler (nothing without one; docs/PROFILING.md).
i32 sceFiberPushUserMarkerWithHud(Ptr<const char> name, u32 color, u32 /*flags*/) {
    if constexpr (profile::kEnabled) profile::push(str(name, 256), profile::rgb_of_abgr(color));
    (void)name, (void)color;
    return 0;
}
i32 sceFiberPopUserMarkerWithHud() {
    profile::pop();
    return 0;
}

// SceFiberInfo (128 bytes): the entry, its argument, the context memory and its size, the name (32 bytes), and the
// context's unused margin (not measured: 0).
i32 sceFiberGetInfo(Ptr<u8> fiber, Ptr<u8> info) {
    if (!fiber || !info) return kNull;
    if ((fiber.address & 7) || (info.address & 7)) return kAlignment;
    if (!writable(info.address, 128)) return kInvalid;
    Fiber* f = find(fiber.address);
    if (!f) return kInvalid;
    u8* out = info.get();
    std::memset(out, 0, 128);
    const u32 words[4] = {f->entry, f->arg_on_initialize, f->context, f->context_size};
    std::memcpy(out, words, sizeof words);
    std::memcpy(out + 16, f->name.data(), std::min<usize>(f->name.size(), 31));
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
    HLE_FUNCTION(0xC6A3F9BB, _sceFiberInitializeWithInternalOptionImpl),
    HLE_FUNCTION(0xE00B9AFE, _sceFiberAttachContextAndSwitch),
    HLE_FUNCTION(0x189599B4, sceFiberGetInfo),
    HLE_FUNCTION(0x7D0C7DDB, _sceFiberAttachContextAndRun),
    HLE_FUNCTION(0x801AB334, sceFiberOptParamInitialize),
    HLE_FUNCTION(0xDD50DF3D, sceFiberRenameSelf),
    HLE_FUNCTION(0xCD543A71, sceFiberStartContextSizeCheck),
    HLE_FUNCTION(0x67F70867, sceFiberStopContextSizeCheck),
    HLE_FUNCTION(0x9C414B09, sceFiberPushUserMarkerWithHud),
    HLE_FUNCTION(0x4EC01F9C, sceFiberPopUserMarkerWithHud),
};

// libult's hooks into the system's user-level object manager and its debugger support: accepted, nothing registers.
// (Its Razor profiler packets are ScePerf's, debug.cpp.)
i32 _sceUlobjMgrRegisterLibultProtocolRevision(u32 /*revision*/) { return 0; }
i32 ulobj_dbg_ignored() {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) nx::logd(logs::hle, "SceUlobjDbg: libult's debugger hooks accepted (no debugger)");
    return 0;
}

constexpr u32 kSceUlobjMgr = 0xDB18BB68, kSceUlobjDbg = 0x49F67FCF;
const Function kUlobjMgr[] = {
    HLE_FUNCTION(0x50F2F2AA, _sceUlobjMgrRegisterLibultProtocolRevision),
};
const Function kUlobjDbg[] = {
    Function{0xD7F0F610, "(SceUlobjDbg 0xD7F0F610)", &entry<&ulobj_dbg_ignored>},
    Function{0xF9C0F5DA, "(SceUlobjDbg 0xF9C0F5DA)", &entry<&ulobj_dbg_ignored>},
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
    register_library({"SceUlobjMgr", kSceUlobjMgr, kUlobjMgr});
    register_library({"SceUlobjDbg", kSceUlobjDbg, kUlobjDbg});
}

}  // namespace psvrecore::runtime::hle
