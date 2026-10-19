#include "psvrecore/runtime/runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "psvrecore/log.hpp"
#include "psvrecore/recomp/support.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/threading/thread.h"

namespace psvrecore::runtime {

namespace {

std::atomic<Runtime*> g_runtime{nullptr};

// A host fault inside the guest address space: say where, and in which guest thread.
void on_guest_fault(u64 offset, bool write) {
    kernel::Thread* t = Kernel::current();
    std::fprintf(stderr, "guest memory fault: %s guest address 0x%08llx", write ? "write to" : "access to",
                 static_cast<unsigned long long>(offset));
    if (offset >= (1ull << 32)) std::fprintf(stderr, " (past the end of the guest address space)");
    if (t) {
        std::fprintf(stderr, " in thread '%s' (UID 0x%08x)", t->name.c_str(), static_cast<u32>(t->uid));
        std::fprintf(stderr, "\n  sp %08x lr %08x r0 %08x r1 %08x r2 %08x r3 %08x", t->ctx.r[13], t->ctx.r[14], t->ctx.r[0],
                     t->ctx.r[1], t->ctx.r[2], t->ctx.r[3]);
        std::fprintf(stderr, "\n  r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x r9 %08x r10 %08x r11 %08x r12 %08x", t->ctx.r[4],
                     t->ctx.r[5], t->ctx.r[6], t->ctx.r[7], t->ctx.r[8], t->ctx.r[9], t->ctx.r[10], t->ctx.r[11], t->ctx.r[12]);
        // (PSVRECORE_FAULT_DUMP: the words at the addresses the registers hold, where they are mapped; it takes the
        // memory map's lock, so it is for debugging only)
        if (std::getenv("PSVRECORE_FAULT_DUMP")) {
            if (Runtime* rt = current_runtime()) {
                for (u32 k = 0; k < 13; ++k) {
                    const u32 at = t->ctx.r[k] & ~3u;
                    if (!rt->memory().valid(at, 64)) continue;
                    std::fprintf(stderr, "\n  [r%u %08x]", k, at);
                    for (u32 i = 0; i < 16; ++i) std::fprintf(stderr, " %08x", psv_ld32(at + 4 * i));
                }
            }
        }
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// PSVRECORE_WATCH=<address>[:<size>],<function>,...: up to four, for finding what corrupts guest memory (they slow
// nothing until one is hit):
// - every write to a guest address (1, 2, 4 or 8 bytes, 4 by default, aligned to its size) is reported with the
//   value now there, the guest thread and its registers, and the host function that wrote;
// - every call of a host function by its name (a generated one, f_<address>) is reported with the guest registers
//   it is called with.
u32 g_watched[4];
u32 g_watched_size[4];  // (0: a function)
const char* g_watched_name[4];

// The registers that point at text in guest memory: the text (" [r1 \"...\"]").
void print_texts(const u32* r, u32 count) {
    Runtime* rt = current_runtime();
    if (!rt) return;
    for (u32 k = 0; k < count; ++k) {
        const u32 at = r[k];
        if (!rt->memory().valid(at, 4)) continue;
        char text[121];
        u32 n = 0;
        for (; n < 120 && rt->memory().valid(at + n, 1); ++n) {
            const u8 c = static_cast<u8>(psv_ld8(at + n));
            if (c < 0x20 || c > 0x7E) break;
            text[n] = static_cast<char>(c);
        }
        text[n] = '\0';
        if (n >= 4 && rt->memory().valid(at + n, 1) && psv_ld8(at + n) == 0) std::fprintf(stderr, " [r%u \"%s\"]", k, text);
    }
}

void on_watched(u32 index, void* host_pc, void* first_argument) {
    kernel::Thread* t = Kernel::current();
    if (g_watched_size[index] == 0) {
        const auto* ctx = static_cast<const PsvContext*>(first_argument);
        std::fprintf(stderr, "watch: %s in thread '%s' r0 %08x r1 %08x r2 %08x r3 %08x sp %08x lr %08x",
                     g_watched_name[index], t ? t->name.c_str() : "(host)", ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3],
                     ctx->r[13], ctx->r[14]);
        print_texts(ctx->r, 4);
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
        return;
    }
    const u32 at = g_watched[index];
    char where[320];
    host::describe_code(host_pc, where, sizeof where);
    std::fprintf(stderr, "watch: write to 0x%08x, now", at);
    for (u32 i = 0; i < g_watched_size[index]; i += 4) std::fprintf(stderr, " %08x", psv_ld32(at + i));
    std::fprintf(stderr, " by %s", where);
    if (t) {
        std::fprintf(stderr,
                     " in thread '%s' lr %08x sp %08x r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x",
                     t->name.c_str(), t->ctx.r[14], t->ctx.r[13], t->ctx.r[0], t->ctx.r[1], t->ctx.r[2], t->ctx.r[3],
                     t->ctx.r[4], t->ctx.r[5], t->ctx.r[6], t->ctx.r[7], t->ctx.r[8]);
        print_texts(t->ctx.r, 13);
        // (the words at the stack pointer: what the function saved of its caller's registers)
        if (Runtime* rt = current_runtime(); rt && rt->memory().valid(t->ctx.r[13] & ~3u, 32)) {
            std::fprintf(stderr, " [sp");
            for (u32 i = 0; i < 8; ++i) std::fprintf(stderr, " %08x", psv_ld32((t->ctx.r[13] & ~3u) + 4 * i));
            std::fprintf(stderr, "]");
        }
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void watch_from_environment(u8* base) {
    const char* spec = std::getenv("PSVRECORE_WATCH");
    if (!spec || !*spec) return;
    void* addresses[4];
    u32 count = 0;
    static char names[256];
    std::snprintf(names, sizeof names, "%s", spec);
    for (char* p = names; *p && count < 4;) {
        char* end = p;
        while (*end && *end != ',') ++end;
        const bool more = *end == ',';
        *end = '\0';
        if (*p >= '0' && *p <= '9') {
            char* rest = nullptr;
            g_watched[count] = static_cast<u32>(std::strtoul(p, &rest, 0));
            g_watched_size[count] = *rest == ':' ? static_cast<u32>(std::strtoul(rest + 1, nullptr, 0)) : 4;
            if (g_watched_size[count] == 0) g_watched_size[count] = 4;
            addresses[count] = base + g_watched[count];
        } else {
            addresses[count] = host::find_code(p);
            if (!addresses[count]) {
                nx::logw(logs::kernel, "PSVRECORE_WATCH: no host function '{}'", p);
                return;
            }
            g_watched_size[count] = 0;
            g_watched_name[count] = p;
            char where[320];
            host::describe_code(addresses[count], where, sizeof where);
            nx::logi(logs::kernel, "PSVRECORE_WATCH: {} at {} ({})", p, addresses[count], where);
        }
        ++count;
        p = more ? end + 1 : end;
    }
    if (host::watch(addresses, g_watched_size, count, on_watched))
        nx::logi(logs::kernel, "{} watchpoint(s) from PSVRECORE_WATCH", count);
    else
        nx::logw(logs::kernel, "PSVRECORE_WATCH: these cannot be watched on this host");
}

// PSVRECORE_THREADS=<seconds>: every so many seconds, every guest thread's state and what it waits for (to stderr),
// for finding what a game that stands still waits for. PSVRECORE_HLE_COUNTS=<seconds>: as often, the HLE functions
// called since the last report and how often.
void report_threads_from_environment() {
    const char* spec = std::getenv("PSVRECORE_THREADS");
    const bool threads = spec && *spec;
    const char* dump = std::getenv("PSVRECORE_DUMP");
    if (!threads && !hle::counting_calls() && !(dump && *dump)) return;
    const u32 seconds =
        nx::max(1u, static_cast<u32>(std::strtoul(threads ? spec : hle::counting_calls() ? std::getenv("PSVRECORE_HLE_COUNTS") : "10",
                                                  nullptr, 0)));
    nx::thread_options options;
    options.name = "thread report";
    nx::thread reporter([seconds, threads] {
        for (u32 n = 1;; ++n) {
            nx::this_thread::sleep_for(std::chrono::seconds(seconds));
            Runtime* rt = current_runtime();
            if (!rt) return;
            if (threads) {
                const nx::string list = rt->kernel().describe_threads();
                std::fprintf(stderr, "threads after %u s:\n%s", n * seconds, list.c_str());
            }
            if (hle::counting_calls()) {
                const nx::string calls = hle::describe_calls();
                std::fprintf(stderr, "HLE calls in the %u s before %u s:\n%s", seconds, n * seconds, calls.c_str());
            }
            // PSVRECORE_DUMP=<address>:<bytes>,...: guest memory, in words, with each report
            if (const char* dump = std::getenv("PSVRECORE_DUMP")) {
                for (const char* p = dump; *p;) {
                    char* end = nullptr;
                    const u32 at = static_cast<u32>(std::strtoul(p, &end, 0)) & ~3u;
                    if (end == p) break;
                    const u32 bytes = *end == ':' ? static_cast<u32>(std::strtoul(end + 1, &end, 0)) : 64;
                    for (u32 o = 0; o < bytes && rt->memory().valid(at + o, 4); o += 32) {
                        std::fprintf(stderr, "  [%08x]", at + o);
                        for (u32 i = 0; i < 32 && o + i < bytes && rt->memory().valid(at + o + i, 4); i += 4)
                            std::fprintf(stderr, " %08x", psv_ld32(at + o + i));
                        std::fprintf(stderr, "\n");
                    }
                    p = *end == ',' ? end + 1 : end;
                }
            }
            std::fflush(stderr);
        }
    }, options);
    reporter.detach();
}

}  // namespace

Runtime* current_runtime() { return g_runtime.load(std::memory_order_acquire); }

Runtime::Runtime(const Config& config) : config_(config) {
    if (current_runtime()) {
        nx::loge(logs::kernel, "a runtime already exists");
        return;
    }
    if (!memory_.init()) {
        nx::loge(logs::kernel, "cannot reserve the guest address space");
        return;
    }
    host::set_fault_region(memory_.base(), (1ull << 32) + (1ull << 20), on_guest_fault);
    watch_from_environment(memory_.base());
    Kernel::Config kc = config.kernel;
    kc.stack_region = layout::kStacks;
    kernel_ = nx::make_unique<Kernel>(memory_, kc);
    files_ = nx::make_unique<FileSystem>(config.files);
    loader_ = nx::make_unique<Loader>(*this);
    g_runtime.store(this, std::memory_order_release);
    hle::register_system_libraries();
    ok_ = true;
    report_threads_from_environment();
}

Runtime::~Runtime() {
    if (!ok_) return;
    kernel_->shutdown();
    kernel_.reset();
    loader_.reset();
    files_.reset();
    g_runtime.store(nullptr, std::memory_order_release);
    hle::clear_libraries();
    recomp::clear_modules();
    host::set_fault_region(nullptr, 0, nullptr);
    memory_.shutdown();
}

u32 Runtime::allocate_system(u32 size, nx::string_view name) {
    return memory_.map_anywhere(layout::kSystem, size, GuestMemory::kPageSize, Access::ReadWrite, BlockKind::System, name);
}

void Runtime::exit_process(i32 code) {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    if (exited_) return;
    exited_ = true;
    exit_code_ = code;
    exit_changed_.notify_all();
}

bool Runtime::exited() const {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    return exited_;
}

i32 Runtime::exit_code() const {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    return exit_code_;
}

i32 Runtime::wait_for_exit() {
    nx::scoped_lock<nx::mutex> guard(exit_lock_);
    while (!exited_) exit_changed_.wait(exit_lock_);
    return exit_code_;
}

}  // namespace psvrecore::runtime
