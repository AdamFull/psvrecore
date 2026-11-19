// The code generator against the reference interpreter on whole functions: the synthetic corpus (corpus.hpp; loops,
// conditional branches, calls between generated functions, returns by bx lr and pop {pc}, tail branches, TBB/TBH
// and address tables, IT blocks, virtual calls through psv_call, condition flags live at entry and across calls),
// translated by corpusgen at build time. Each test function runs from random states (a fixed seed) as generated C
// and in the interpreter, which fetches the same code from guest memory and follows its calls and returns; r0-r14,
// NZCV, Q, GE, FPSCR, d0-d31, the exclusive monitor and all of the guest memory must end up the same.
//
// Built twice: with the generated code's production memory path (PSV_HOST is psv_membase + address, the 8-byte
// VLD1/VST1 accesses are one host access each), and with CODEGEN_TEST_MASK, the code compiled with
// PSV_ADDRESS_MASK as the random-block test's is (accesses folded into a buffer, element by element).
//
// The condition flags at exit: a function is called here as the runtime calls it (call_guest, an indirect call),
// and its returns leave in the context the flags its callers in the module read (ARCHITECTURE.md, "Condition
// flags"); the others are unspecified, and are not compared. In the production configuration that is what the
// generator gives each function as a game's module has it; the masked configuration's code is generated with every
// test function leaving all four flags at return (as an exported function does), and there all four are compared
// for every function (the test fails if any is not).

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "corpus.hpp"
#include "nxtest.h"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/support.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

extern "C" {
extern const PsvModuleCode* const corpus_modules[];
extern const unsigned corpus_module_count;
extern const uint32_t corpus_exit_flags[][2];
extern const unsigned corpus_exit_flag_count;
extern const int corpus_full_exit;
}

using namespace psvrecore;

namespace {

#ifdef CODEGEN_TEST_MASK
constexpr const char* kConfig = "folded memory (PSV_ADDRESS_MASK)";
constexpr u32 kMask = CODEGEN_TEST_MASK;
static_assert(((kCorpusEnd - 1) & kMask) == kCorpusEnd - 1 - (kCorpusBase & ~kMask), "the mask folds the corpus' memory");
constexpr usize kBufferSize = usize(kMask) + 1 + 64;
#else
constexpr const char* kConfig = "production memory path";
constexpr u32 kMask = 0xFFFFFFFFu;
constexpr usize kBufferSize = usize(kCorpusEnd - kCorpusBase) + 64;
#endif

// The return address every test function is called with: never code, never fetched (the interpreter stops there).
constexpr u32 kReturn = 0x82FF0001u;
constexpr u64 kMaxSteps = 20'000'000;
constexpr int kRunsPerFunction = 48;

// The corpus' memory: guest address -> offset in the buffer.
usize offset_of(u32 address) {
#ifdef CODEGEN_TEST_MASK
    return address & kMask;
#else
    return address - kCorpusBase;
#endif
}
uint8_t* membase_for(nx::vector<u8>& buffer) {
#ifdef CODEGEN_TEST_MASK
    return buffer.data();
#else
    // (psv_membase + guest address lands in the buffer for the corpus' addresses, as with the runtime's reservation)
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buffer.data()) - kCorpusBase);
#endif
}

struct Rng {
    u64 s;
    u64 next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    u32 u32v() { return static_cast<u32>(next() >> 32); }
    u32 below(u32 n) { return static_cast<u32>((next() >> 33) % n); }
};

// (the random-block test's value distributions: generator_tests.cpp)
u32 interesting(Rng& rng) {
    static constexpr u32 kSpecial[] = {0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x8000, 0x7FFF, 0xFF, 0x80, 31, 32, 33};
    const u32 pick = rng.u32v() % 8;
    if (pick == 0) return kSpecial[rng.u32v() % (sizeof kSpecial / sizeof kSpecial[0])];
    if (pick == 1) return rng.u32v() % 64;
    if (pick == 2) return static_cast<u32>(-static_cast<i32>(rng.u32v() % 64));
    return rng.u32v();
}
u32 interesting_f32(Rng& rng) {
    static constexpr u32 kSpecial[] = {
        0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0xFFA00000, 0x00000001,
        0x807FFFFF, 0x00400000, 0x00800000, 0x80800001, 0x007FFFFF, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0x20000000,
        0x1FFFFFFF, 0x20000001, 0x3F000000, 0x4B000000, 0xCF000000, 0x4F800000, 0x47000000, 0x387FC000, 0x33800000,
    };
    const u32 pick = rng.u32v() % 4;
    if (pick == 0) return kSpecial[rng.u32v() % (sizeof kSpecial / sizeof kSpecial[0])];
    if (pick == 1) return (rng.u32v() & 0x807FFFFFu) | ((0x70 + rng.u32v() % 32) << 23);  // moderate magnitudes
    return rng.u32v();
}
u64 interesting_f64(Rng& rng) {
    static constexpr u64 kSpecial[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000000ull, 0xFFF8000000000001ull, 0x7FF0000000000001ull, 0x0000000000000001ull,
        0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x8010000000000001ull, 0x000FFFFFFFFFFFFFull,
        0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x2000000000000000ull,
        0x1FFFFFFFFFFFFFFFull, 0x2000000000000001ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
        0x41F0000000000000ull, 0x3810000000000000ull, 0x380FFFFFFFFFFFFFull, 0x36A0000000000000ull,
    };
    const u32 pick = rng.u32v() % 4;
    if (pick == 0) return kSpecial[rng.u32v() % (sizeof kSpecial / sizeof kSpecial[0])];
    if (pick == 1) return (rng.next() & 0x800FFFFFFFFFFFFFull) | (static_cast<u64>(0x3C0 + rng.u32v() % 128) << 52);
    return rng.next();
}

PsvContext random_state(Rng& rng) {
    PsvContext c;
    std::memset(&c, 0, sizeof c);
    for (int r = 0; r < 15; ++r) c.r[r] = interesting(rng);
    const u32 flags = rng.u32v();
    c.n = flags & 1;
    c.z = (flags >> 1) & 1;
    c.c = (flags >> 2) & 1;
    c.v = (flags >> 3) & 1;
    c.q = (flags >> 4) & 1;
    c.ge = (flags >> 5) & 0xF;
    for (int d = 0; d < 32; ++d) {
        if (rng.u32v() % 2) c.vfp.d[d] = interesting_f64(rng);
        else c.vfp.d[d] = (static_cast<u64>(interesting_f32(rng)) << 32) | interesting_f32(rng);
    }
    // FPSCR: random flags, flush-to-zero and default NaN; round to nearest (the only mode implemented).
    c.fpscr = (rng.u32v() & 0xF800009Fu) | (rng.u32v() & 0x03000000u);
    c.tpidrurw = rng.u32v();
    c.tpidruro = rng.u32v();
    if (rng.u32v() % 4 == 0) {
        c.excl_address = kCorpusDataBase + (rng.u32v() % (kCorpusRegions * kCorpusRegionSize) & ~3u);
        c.excl_size = 4;
        c.excl_value = rng.u32v();
    }
    return c;
}

// Applies a test's argument specification ("r0=ptr r1=int:0:40 ..."): pointers each get a data region of their own
// (filled as their kind says), integers their range.
bool apply_spec(const char* spec, Rng& rng, PsvContext& c, nx::vector<u8>& memory) {
    u32 region = 0;
    for (const char* p = spec; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        const char* end = p;
        while (*end && *end != ' ') ++end;
        const nx::string token(p, static_cast<usize>(end - p));
        p = end;
        if (token[0] != 'r') continue;  // (s and d registers: always random)
        const usize eq = token.find('=');
        const int reg = std::atoi(token.c_str() + 1);
        const nx::string kind = token.substr(eq + 1);
        if (reg < 0 || reg > 12) return false;
        if (kind == "any") continue;
        if (kind.starts_with("int:")) {
            const char* range = kind.c_str() + 4;
            char* next = nullptr;
            const long lo = std::strtol(range, &next, 10);
            const long hi = std::strtol(next + 1, nullptr, 10);
            c.r[reg] = static_cast<u32>(lo + static_cast<long>(rng.below(static_cast<u32>(hi - lo + 1))));
            continue;
        }
        u32 align = 1;
        u8 fill = 0;  // 0 random bytes, 4 binary32 values, 8 binary64 values
        if (kind == "fptr") align = 4, fill = 4;
        else if (kind == "dptr") align = 8, fill = 8;
        else if (kind.starts_with("ptr")) align = kind.size() > 4 ? static_cast<u32>(std::atoi(kind.c_str() + 4)) : 1;
        else return false;
        if (region >= kCorpusRegions || align == 0) return false;
        const u32 base = kCorpusDataBase + region++ * kCorpusRegionSize;
        u8* host = memory.data() + offset_of(base);
        if (fill == 4)
            for (u32 i = 0; i < kCorpusRegionSize; i += 4) {
                const u32 v = interesting_f32(rng);
                std::memcpy(host + i, &v, 4);
            }
        if (fill == 8)
            for (u32 i = 0; i < kCorpusRegionSize; i += 8) {
                const u64 v = interesting_f64(rng);
                std::memcpy(host + i, &v, 8);
            }
        c.r[reg] = base + 512 + rng.below(256) / align * align;
    }
    return true;
}

jmp_buf g_trap_jump;
recomp::TrapInfo g_trap;
[[noreturn]] void on_trap(PsvContext*, const recomp::TrapInfo& trap) {
    g_trap = trap;
    std::longjmp(g_trap_jump, 1);
}

// The condition flags a function's returns leave in the context (bit 3 N, 2 Z, 1 C, 0 V): what the runtime may read
// after calling it, and what is compared at its exit. The others are unspecified there (see ARCHITECTURE.md).
u32 exit_flags_of(u32 address) {
    for (unsigned i = 0; i < corpus_exit_flag_count; ++i)
        if (corpus_exit_flags[i][0] == address) return corpus_exit_flags[i][1];
    return 15;
}

nx::string differences(const PsvContext& a, const PsvContext& b, const nx::vector<u8>& ma, const nx::vector<u8>& mb, u32 exit_flags) {
    nx::string out;
    for (int r = 0; r < 15; ++r)
        if (a.r[r] != b.r[r]) out += nx::format(" r{}: {:08x} vs {:08x};", r, a.r[r], b.r[r]);
    const auto flag = [&](const char* name, u32 x, u32 y) {
        if (x != y) out += nx::format(" {}: {:x} vs {:x};", name, x, y);
    };
    if (exit_flags & 8) flag("n", a.n, b.n);
    if (exit_flags & 4) flag("z", a.z, b.z);
    if (exit_flags & 2) flag("c", a.c, b.c);
    if (exit_flags & 1) flag("v", a.v, b.v);
    flag("q", a.q, b.q);
    flag("ge", a.ge, b.ge);
    flag("fpscr", a.fpscr, b.fpscr);
    flag("tpidrurw", a.tpidrurw, b.tpidrurw);
    flag("exclusive size", a.excl_size, b.excl_size);
    if (a.excl_size && b.excl_size) {
        flag("exclusive address", a.excl_address, b.excl_address);
        if (a.excl_value != b.excl_value) out += " exclusive value differs;";
    }
    for (int d = 0; d < 32; ++d)
        if (a.vfp.d[d] != b.vfp.d[d]) out += nx::format(" d{}: {:016x} vs {:016x};", d, a.vfp.d[d], b.vfp.d[d]);
    for (u32 at = kCorpusBase; at < kCorpusEnd; ++at) {
        const usize i = offset_of(at);
        if (ma[i] != mb[i]) {
            out += nx::format(" memory at 0x{:08x}: {:02x} vs {:02x};", at, ma[i], mb[i]);
            break;
        }
    }
    return out;
}

// Functions whose disagreement is a known, documented bug (docs/KNOWN_ISSUES.md): name -> reason. They are still
// translated and must still be found; they are not run.
struct KnownFailure {
    const char* function;
    const char* reason;
};
constexpr KnownFailure kKnownFailures[] = {
    {nullptr, nullptr},  // (none at present)
};

const char* known_failure(const char* function) {
    for (const auto& k : kKnownFailures)
        if (k.function && std::strcmp(k.function, function) == 0) return k.reason;
    return nullptr;
}

}  // namespace

TEST_CASE("recomp: generated functions agree with the interpreter on the synthetic corpus") {
    std::printf("configuration: %s\n", kConfig);
    REQUIRE(corpus_module_count == sizeof kCorpusModules / sizeof kCorpusModules[0]);
    recomp::clear_modules();
    for (unsigned i = 0; i < corpus_module_count; ++i) recomp::register_module(*corpus_modules[i]);
    // (the modules' condition-flag contracts fit: corpusgen gives each what the others read)
    const nx::string problems = recomp::flag_contract_problems();
    if (!problems.empty()) std::printf("%s", problems.c_str());
    REQUIRE(problems.empty());
    const auto previous = recomp::set_trap_handler(on_trap);

    nx::vector<u8> pristine(kBufferSize, 0), initial, generated, interpreted;
    for (const CorpusModule& m : kCorpusModules) std::memcpy(pristine.data() + offset_of(m.base), m.bytes, m.size);

    usize runs = 0, failures = 0, skipped = 0, all_flags = 0, some_flags = 0;
    u64 steps_total = 0;
    for (usize t = 0; t < sizeof kCorpusTests / sizeof kCorpusTests[0]; ++t) {
        const CorpusTest& test = kCorpusTests[t];
        const PsvFunction function = recomp::find_function(test.address);
        if (!function) std::printf("%s: no generated function at 0x%08x\n", test.function, test.address);
        REQUIRE(function != nullptr);
        const u32 exit_flags = exit_flags_of(test.address);
        all_flags += exit_flags == 15;
        some_flags += exit_flags != 0 && exit_flags != 15;
        // (--full-exit: every test function leaves all four flags, and they are all compared)
        if (corpus_full_exit && exit_flags != 15) std::printf("%s: flags at exit %x, not all four\n", test.function, exit_flags);
        if (corpus_full_exit) REQUIRE(exit_flags == 15);
        if (const char* reason = known_failure(test.function)) {
            std::printf("skipped (known failure) %s: %s\n", test.function, reason);
            ++skipped;
            continue;
        }
        Rng rng{0xC0DE6E4ull ^ (static_cast<u64>(t + 1) * 0x9E3779B97F4A7C15ull)};
        usize function_failures = 0;
        for (int trial = 0; trial < kRunsPerFunction; ++trial) {
            initial = pristine;
            for (u32 at = kCorpusDataBase; at < kCorpusEnd; at += 4) {
                const u32 w = rng.u32v();
                std::memcpy(initial.data() + offset_of(at), &w, 4);
            }
            PsvContext start = random_state(rng);
            REQUIRE(apply_spec(test.spec, rng, start, initial));
            start.r[13] = kCorpusStackBase + kCorpusStackSize - 256 - 8 * rng.below(16);
            start.r[14] = kReturn;

            // Generated code.
            PsvContext g = start;
            generated = initial;
            psv_membase = membase_for(generated);
            bool trapped = false;
            if (setjmp(g_trap_jump) == 0) function(&g, kReturn);
            else trapped = true;

            // The interpreter, from the same entry, until it returns there.
            PsvContext s = start;
            interpreted = initial;
            psv_membase = membase_for(interpreted);
            interp::Interpreter it(s);
            it.address_mask = kMask;
            it.thumb = (test.address & 1) != 0;
            it.pc = test.address & ~1u;
            bool interp_trapped = false, timed_out = false;
            u64 steps = 0;
            while (!(it.pc == (kReturn & ~1u) && it.thumb)) {
                if (++steps > kMaxSteps) {
                    timed_out = true;
                    break;
                }
                if (it.step() == interp::Result::Trap) {
                    interp_trapped = true;
                    break;
                }
            }
            steps_total += steps;
            ++runs;

            nx::string diff;
            if (timed_out) diff = " the interpreter did not return (an unbounded loop: fix the test's specification)";
            else if (trapped || interp_trapped)
                diff = nx::format(" trapped: generated {} ({}), interpreter {} (kind {} at 0x{:08x})", trapped, trapped ? recomp::describe(g_trap) : "",
                                  interp_trapped, it.trap_kind, it.pc);
            else diff = differences(g, s, generated, interpreted, exit_flags);
            if (!diff.empty()) {
                ++failures;
                if (++function_failures <= 3) {
                    std::printf("%s", nx::format("{} ({}), run {}:{}\n", test.function, test.module, trial, diff).c_str());
                    nx::string regs = "    initial:";
                    for (int r = 0; r < 15; ++r) regs += nx::format(" r{}={:08x}", r, start.r[r]);
                    regs += nx::format(" nzcv={}{}{}{} fpscr={:08x}", start.n, start.z, start.c, start.v, start.fpscr);
                    std::printf("%s\n", regs.c_str());
                }
            }
        }
    }
    // (CODEGEN_BENCH=<rounds>: time each test function's generated code too, from its first random state, the state
    // put back before each call and memory not: a measurement for comparing generators, printed, never checked)
    if (const char* bench = std::getenv("CODEGEN_BENCH"); bench && failures == 0) {
        const int rounds = std::max(1, std::atoi(bench));
        f64 log_sum = 0;
        for (usize t = 0; t < sizeof kCorpusTests / sizeof kCorpusTests[0]; ++t) {
            const CorpusTest& test = kCorpusTests[t];
            const PsvFunction function = recomp::find_function(test.address);
            Rng rng{0xC0DE6E4ull ^ (static_cast<u64>(t + 1) * 0x9E3779B97F4A7C15ull)};
            initial = pristine;
            for (u32 at = kCorpusDataBase; at < kCorpusEnd; at += 4) {
                const u32 w = rng.u32v();
                std::memcpy(initial.data() + offset_of(at), &w, 4);
            }
            PsvContext start = random_state(rng);
            apply_spec(test.spec, rng, start, initial);
            start.r[13] = kCorpusStackBase + kCorpusStackSize - 256 - 8 * rng.below(16);
            start.r[14] = kReturn;
            generated = initial;
            psv_membase = membase_for(generated);
            u64 best = ~u64(0);
            constexpr int kCalls = 2000;
            for (int round = 0; round < rounds; ++round) {
                const u64 t0 = nx::os::ticks_ns();
                for (int k = 0; k < kCalls; ++k) {
                    PsvContext g = start;
                    function(&g, kReturn);
                }
                best = std::min(best, nx::os::ticks_ns() - t0);
            }
            const f64 ns = static_cast<f64>(best) / kCalls;
            log_sum += std::log(ns);
            std::printf("bench %-28s %10.1f ns\n", test.function, ns);
        }
        std::printf("bench geometric mean %.2f ns over %zu functions\n",
                    std::exp(log_sum / static_cast<f64>(sizeof kCorpusTests / sizeof kCorpusTests[0])),
                    sizeof kCorpusTests / sizeof kCorpusTests[0]);
    }
    recomp::set_trap_handler(previous);
    recomp::clear_modules();
    psv_membase = nullptr;
    std::printf("%s\n", nx::format("generator vs interpreter ({}): {} functions, {} runs, {} interpreted instructions, {} disagreements, {} skipped",
                                   kConfig, sizeof kCorpusTests / sizeof kCorpusTests[0], runs, steps_total, failures, skipped)
                            .c_str());
    std::printf("%s\n", nx::format("flags compared at exit: all four in {} functions, some in {}, none in {} (those no caller reads)", all_flags,
                                   some_flags, sizeof kCorpusTests / sizeof kCorpusTests[0] - all_flags - some_flags)
                            .c_str());
    CHECK(failures == 0);
}

namespace {

// A callback that returns with another stack pointer (outside AAPCS): what the generated code checks after a call
// to code outside its module's view where its callers rely on the stack pointer (ARCHITECTURE.md, "Registers").
void moves_sp(PsvContext* ctx, uint32_t) {
    ctx->r[0] = ctx->r[0] + ctx->r[1];
    ctx->r[13] -= 8;
}

}  // namespace

TEST_CASE("recomp: a call that returns with another stack pointer stops where callers rely on it") {
    recomp::clear_modules();
    for (unsigned i = 0; i < corpus_module_count; ++i) recomp::register_module(*corpus_modules[i]);
    const auto previous = recomp::set_trap_handler(on_trap);
    // fold_all calls fold directly, fold calls each reducer through a function pointer (psv_call): r_add replaced.
    u32 fold_all = 0, r_add = 0, fold = 0;
    for (const CorpusTest& t : kCorpusTests)
        if (std::strcmp(t.function, "fold_all") == 0) fold_all = t.address;
    for (const CorpusModule& m : kCorpusModules)
        for (u32 i = 0; i < m.function_count; ++i) {
            if (std::strcmp(m.functions[i].name, "r_add") == 0) r_add = m.functions[i].address;
            if (std::strcmp(m.functions[i].name, "fold") == 0) fold = m.functions[i].address;
        }
    REQUIRE(fold_all != 0);
    REQUIRE(r_add != 0);
    recomp::override_function(r_add, moves_sp);
    nx::vector<u8> memory(kBufferSize, 0);
    for (const CorpusModule& m : kCorpusModules) std::memcpy(memory.data() + offset_of(m.base), m.bytes, m.size);
    psv_membase = membase_for(memory);
    PsvContext c;
    std::memset(&c, 0, sizeof c);
    c.r[0] = kCorpusDataBase;
    c.r[1] = 4;
    c.r[13] = kCorpusStackBase + kCorpusStackSize - 256;
    c.r[14] = kReturn;
    bool trapped = false;
    g_trap = {};
    if (setjmp(g_trap_jump) == 0) recomp::find_function(fold_all)(&c, kReturn);
    else trapped = true;
#ifdef CODEGEN_TEST_NO_CALLEE_SAVED
    // (without callee-saved knowledge nothing relies on it: fold reads sp back after the call and goes on, where the
    // moved stack leads it, as the interpreter would)
    (void)fold;
    CHECK((!trapped || g_trap.kind != PSV_TRAP_STACK_POINTER));
#else
    // (fold's callers rely on what it preserves, which relies on the call keeping sp: it checks)
    CHECK(trapped);
    CHECK(g_trap.kind == PSV_TRAP_STACK_POINTER);
    CHECK(g_trap.pc == (fold & ~1u));
#endif
    recomp::set_trap_handler(previous);
    recomp::clear_modules();
    psv_membase = nullptr;
}
