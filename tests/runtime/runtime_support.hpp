#pragma once

// What the runtime tests share: a runtime for each test, "guest" functions written in C++ (registered at code
// addresses like recompiled ones, so threads and callbacks can run them), and calls to the system libraries through
// the guest calling convention, as recompiled code makes them.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::runtime::test {

inline constexpr u32 kSceLibKernel = 0xCAE9ACE6, kSceThreadmgr = 0x859A24B1, kSceSysmem = 0x37FE725A,
                     kSceIofilemgr = 0xF2FF276E, kSceProcessmgr = 0x2DD91812, kSceThreadmgrCoredumpTime = 0x5E8D0E22;

// A runtime, with the calling (test) thread adopted as a guest thread so it can make kernel calls.
struct TestRuntime {
    Runtime rt;
    explicit TestRuntime(u32 cpus = kernel::kUserCpuCount, i32 priority = kernel::kGamePriority, AudioSink* audio = nullptr)
        : rt(config(cpus, audio)) {
        if (rt.ok()) rt.kernel().adopt_host_thread("test main", priority);
    }
    explicit TestRuntime(const Runtime::Config& c, i32 priority = kernel::kGamePriority) : rt(c) {
        if (rt.ok()) rt.kernel().adopt_host_thread("test main", priority);
    }
    ~TestRuntime() {
        if (rt.ok()) rt.kernel().release_host_thread();
    }
    static Runtime::Config config(u32 cpus, AudioSink* audio) {
        Runtime::Config c;
        c.kernel.cpu_count = cpus;
        c.audio = audio;
        return c;
    }
    PsvContext* ctx() { return &Kernel::current()->ctx; }
};

// Guest functions: host functions registered at code addresses (each test module gets its own range).
class GuestCode {
public:
    u32 add(PsvFunction f) {
        const u32 address = 0x82000001u + 0x10u * static_cast<u32>(entries_.size());
        entries_.push_back(PsvFunctionEntry{address, f});
        return address;
    }
    void install() {
        code_ = PsvModuleCode{};
        code_.name = "test";
        code_.functions = entries_.data();
        code_.function_count = static_cast<u32>(entries_.size());
        recomp::register_module(code_);
    }

private:
    nx::vector<PsvFunctionEntry> entries_;
    PsvModuleCode code_{};
};

// A call to a system library function, as recompiled code makes it: arguments in r0-r3 and on the stack (given as
// words; 64-bit arguments as two words, already aligned by the caller), the result in r0 (and r1).
inline u64 call(PsvContext* ctx, u32 library_nid, u32 nid, std::initializer_list<u32> args) {
    const hle::Function* f = hle::find_function(library_nid, "", nid);
    if (!f) {
        std::fprintf(stderr, "no HLE function 0x%08x in library 0x%08x\n", nid, library_nid);
        std::abort();
    }
    const u32 saved_sp = ctx->r[13];
    u32 i = 0;
    const u32 extra = args.size() > 4 ? static_cast<u32>(args.size() - 4) : 0;
    u32 sp = (saved_sp - 4 * extra) & ~7u;
    for (u32 a : args) {
        if (i < 4) ctx->r[i] = a;
        else psv_st32(sp + 4 * (i - 4), a);
        ++i;
    }
    ctx->r[13] = sp;
    f->function(ctx, 0);
    ctx->r[13] = saved_sp;
    return ctx->r[0] | (u64{ctx->r[1]} << 32);
}
inline i32 call32(PsvContext* ctx, u32 library_nid, u32 nid, std::initializer_list<u32> args) {
    return static_cast<i32>(static_cast<u32>(call(ctx, library_nid, nid, args)));
}

// Scratch guest memory (a memory block of the runtime's own).
inline u32 scratch(u32 size) { return current_runtime()->allocate_system(size, "test scratch"); }
inline u32 guest_string(const char* s) {
    const u32 n = static_cast<u32>(std::strlen(s)) + 1;
    const u32 at = scratch(n);
    std::memcpy(current_runtime()->memory().host(at), s, n);
    return at;
}

// An event log the guest threads of a test append to, in order.
struct Log {
    nx::mutex lock;
    nx::vector<nx::string> events;
    void add(nx::string_view e) {
        nx::scoped_lock<nx::mutex> guard(lock);
        events.push_back(nx::string(e));
    }
    nx::string joined() {
        nx::scoped_lock<nx::mutex> guard(lock);
        nx::string s;
        for (const auto& e : events) {
            if (!s.empty()) s += ' ';
            s += e;
        }
        return s;
    }
};

}  // namespace psvrecore::runtime::test
