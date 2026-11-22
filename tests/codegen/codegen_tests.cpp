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
// Functions named obs_* are reached only through psv_call (code outside the generated code's view, which reads the
// whole context): the test also compares the context at each of their entries with the interpreter's state there,
// r0-r14, Q, GE, d0-d31, the FPSCR and the condition flags the modules give at every call to code they do not know
// (flags_given_at_entry; the others there are the caller's to give or not). That is the
// property internal entries put at risk: callers up the host stack keep registers in locals (ARCHITECTURE.md,
// "Internal entries").
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

#ifndef CODEGEN_TEST_MASK
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

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
constexpr int kRunsPerFunction = 48;  // (CODEGEN_RUNS=<n>: n instead)
int runs_per_function() {
    if (const char* n = std::getenv("CODEGEN_RUNS")) return std::max(1, std::atoi(n));
    return kRunsPerFunction;
}

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

// The generated code's memory in the production configuration: a copy of the buffer whose end, kCorpusEnd, is
// followed by a page that faults when touched (the end of mapped memory: an access of the generated code that reads or
// writes past the guest's bytes there crashes the test). The masked configuration folds every address.
#ifndef CODEGEN_TEST_MASK
constexpr usize kGuardedSize = usize(kCorpusEnd - kCorpusBase);
u8* guarded_memory() {
    static u8* memory = [] {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const usize page = info.dwPageSize;
#else
        const usize page = static_cast<usize>(sysconf(_SC_PAGESIZE));
#endif
        const usize size = (kGuardedSize + page - 1) / page * page;
#ifdef _WIN32
        u8* m = static_cast<u8*>(VirtualAlloc(nullptr, size + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (m) VirtualProtect(m + size, page, PAGE_NOACCESS, &old);
#else
        void* v = mmap(nullptr, size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        u8* m = v == MAP_FAILED ? nullptr : static_cast<u8*>(v);
        if (m) mprotect(m + size, page, PROT_NONE);
#endif
        // (the guest's last byte right before the page)
        return m ? m + (size - kGuardedSize) : nullptr;
    }();
    return memory;
}
#endif
// Before and after a run of generated code.
uint8_t* run_memory(nx::vector<u8>& buffer) {
#ifdef CODEGEN_TEST_MASK
    return membase_for(buffer);
#else
    u8* m = guarded_memory();
    if (!m) return membase_for(buffer);
    std::memcpy(m, buffer.data(), kGuardedSize);
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(m) - kCorpusBase);
#endif
}
void after_run(nx::vector<u8>& buffer) {
#ifndef CODEGEN_TEST_MASK
    if (u8* m = guarded_memory()) std::memcpy(buffer.data(), m, kGuardedSize);
#else
    (void)buffer;
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
        if (kind.starts_with("tail:")) {  // (K bytes before the end of the corpus' memory: the stack's top)
            c.r[reg] = kCorpusEnd - static_cast<u32>(std::atoi(kind.c_str() + 5));
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

// The host's FPCR for generated code built with native floating point (guest.h, PSV_NATIVE_FLOAT): FZ and DN as the
// guest's FPSCR has them (the runtime's mirror, runtime/float_env.hpp); elsewhere nothing.
void mirror_fpscr(u32 fpscr) {
#ifdef PSV_NATIVE_FLOAT
    psv_native_set_fpcr(fpscr);
#else
    (void)fpscr;
#endif
}

jmp_buf g_trap_jump;
recomp::TrapInfo g_trap;
[[noreturn]] void on_trap(PsvContext*, const recomp::TrapInfo& trap) {
    g_trap = trap;
    std::longjmp(g_trap_jump, 1);
}

// The obs_* functions: each replaced (override_function: what psv_call finds) by a stand-in that records the context
// and calls the generated function.
constexpr usize kMaxObserved = 8;
struct Observed {
    u32 address = 0;
    PsvFunction original = nullptr;
    const char* name = nullptr;
};
Observed g_observed[kMaxObserved];
usize g_observed_count = 0;
bool g_recording = false;
struct Seen {
    u32 address;
    PsvContext ctx;
};
nx::vector<Seen> g_seen;
template <usize N>
void observe(PsvContext* ctx, uint32_t ret) {
    if (g_recording) g_seen.push_back(Seen{g_observed[N].address, *ctx});
    g_observed[N].original(ctx, ret);
}
using Observer = void (*)(PsvContext*, uint32_t);
constexpr Observer kObservers[kMaxObserved] = {observe<0>, observe<1>, observe<2>, observe<3>,
                                               observe<4>, observe<5>, observe<6>, observe<7>};

void install_observers() {
    g_observed_count = 0;
    for (const CorpusModule& m : kCorpusModules)
        for (u32 i = 0; i < m.function_count; ++i) {
            if (std::strncmp(m.functions[i].name, "obs_", 4) != 0 || g_observed_count == kMaxObserved) continue;
            Observed& o = g_observed[g_observed_count];
            o.address = m.functions[i].address;
            o.name = m.functions[i].name;
            o.original = recomp::find_function(o.address);
            if (!o.original) continue;
            recomp::override_function(o.address, kObservers[g_observed_count++]);
        }
}

const char* observed_name(u32 address) {
    for (usize i = 0; i < g_observed_count; ++i)
        if (g_observed[i].address == address) return g_observed[i].name;
    return "?";
}

// The flags every module puts in the context at a call to code it does not know (PsvModuleCode::flags_given_at_entry;
// corpusgen gives every module the same), bit 3 N to bit 0 V: those compared at the obs_* entries. (A call there may
// also give the flags its caller reads after it returns; those the result compares.)
u32 flags_given_at_unknown_calls() {
    u32 given = 15;
    for (unsigned i = 0; i < corpus_module_count; ++i)
        if (corpus_modules[i]->flag_contract) given &= corpus_modules[i]->flags_given_at_entry;
    return given;
}

// The contexts at the obs_* entries, the generated code's against the interpreter's.
nx::string observed_differences(const nx::vector<Seen>& a, const nx::vector<Seen>& b) {
    if (a.size() != b.size()) return nx::format(" calls through psv_call to obs_*: {} vs {};", a.size(), b.size());
    static const u32 given = flags_given_at_unknown_calls();
    nx::string out;
    for (usize k = 0; k < a.size() && out.empty(); ++k) {
        const PsvContext& x = a[k].ctx;
        const PsvContext& y = b[k].ctx;
        nx::string d;
        if (a[k].address != b[k].address) d += nx::format(" called 0x{:08x} vs 0x{:08x};", a[k].address, b[k].address);
        for (int r = 0; r < 15; ++r)
            if (x.r[r] != y.r[r]) d += nx::format(" r{}: {:08x} vs {:08x};", r, x.r[r], y.r[r]);
        if (x.q != y.q) d += " q differs;";
        if (x.ge != y.ge) d += " ge differs;";
        const auto flag = [&](u32 bit, const char* name, u32 fx, u32 fy) {
            if ((given & bit) && (fx != 0) != (fy != 0)) d += nx::format(" {}: {} vs {};", name, fx != 0, fy != 0);
        };
        flag(8, "n", x.n, y.n);
        flag(4, "z", x.z, y.z);
        flag(2, "c", x.c, y.c);
        flag(1, "v", x.v, y.v);
        if (x.fpscr != y.fpscr) d += nx::format(" fpscr: {:08x} vs {:08x};", x.fpscr, y.fpscr);
        for (int r = 0; r < 32; ++r)
            if (x.vfp.d[r] != y.vfp.d[r]) d += nx::format(" d{}: {:016x} vs {:016x};", r, x.vfp.d[r], y.vfp.d[r]);
        if (!d.empty()) out = nx::format(" context at {} (call {} through psv_call):{}", observed_name(a[k].address), k, d);
    }
    return out;
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
    install_observers();
    std::printf("observed through psv_call: %zu functions\n", g_observed_count);
    nx::vector<Seen> interp_seen;
    usize observed_calls = 0;

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
        const int trials = runs_per_function();
        for (int trial = 0; trial < trials; ++trial) {
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
            psv_membase = run_memory(generated);
            bool trapped = false;
            // (native floating point: the FPCR mirrors the FPSCR the function starts with, as the runtime keeps it)
            mirror_fpscr(start.fpscr);
            g_seen.clear();
            g_recording = true;
            if (setjmp(g_trap_jump) == 0) function(&g, kReturn);
            else trapped = true;
            g_recording = false;
            mirror_fpscr(0);
            after_run(generated);

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
            interp_seen.clear();
            while (!(it.pc == (kReturn & ~1u) && it.thumb)) {
                // (at an obs_* function's first instruction: its context)
                const u32 code = it.pc | (it.thumb ? 1u : 0u);
                for (usize k = 0; k < g_observed_count; ++k)
                    if (g_observed[k].address == code) interp_seen.push_back(Seen{code, s});
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
            else diff = differences(g, s, generated, interpreted, exit_flags) + observed_differences(g_seen, interp_seen);
            observed_calls += interp_seen.size();
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
            mirror_fpscr(start.fpscr);
            for (int round = 0; round < rounds; ++round) {
                const u64 t0 = nx::os::ticks_ns();
                for (int k = 0; k < kCalls; ++k) {
                    PsvContext g = start;
                    function(&g, kReturn);
                }
                best = std::min(best, nx::os::ticks_ns() - t0);
            }
            mirror_fpscr(0);
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
    std::printf("%s\n", nx::format("contexts compared at calls through psv_call to obs_*: {} (flags {:x} among them)", observed_calls,
                                   flags_given_at_unknown_calls()).c_str());
    CHECK((g_observed_count == 0 || observed_calls > 0));
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

namespace {

// Stand-ins for the indirect-call table's test: each leaves its number in r0.
void t_thumb_a(PsvContext* ctx, uint32_t) { ctx->r[0] = 1; }
void t_thumb_b(PsvContext* ctx, uint32_t) { ctx->r[0] = 2; }
void t_arm(PsvContext* ctx, uint32_t) { ctx->r[0] = 3; }
void t_other(PsvContext* ctx, uint32_t) { ctx->r[0] = 4; }
void t_override(PsvContext* ctx, uint32_t) { ctx->r[0] = 5; }
void t_import(PsvContext* ctx, uint32_t) { ctx->r[0] = 6; }

}  // namespace

TEST_CASE("recomp: indirect calls through a module's table find what the registry finds") {
    recomp::clear_modules();
    const auto previous = recomp::set_trap_handler(on_trap);
    // Module a: Thumb functions at 0x81000000 and 0x81000100, an ARM one at 0x81000010, an import stub at 0x81000200;
    // module b: a Thumb function at 0x83000000.
    static const PsvFunctionEntry fa[] = {{0x81000001u, t_thumb_a}, {0x81000010u, t_arm}, {0x81000101u, t_thumb_b}};
    static PsvImport ia[] = {{1, 2, "lib", t_import}};
    static const uint32_t sa[] = {0x81000200u};
    static PsvDispatch da, db;
    const PsvModuleCode ma = {"a", fa, 3, ia, 1, sa, nullptr, 0, {}, nullptr, nullptr, 0, PSV_FLAG_CONTRACT, 0, 0, 0, 0, &da};
    static const PsvFunctionEntry fb[] = {{0x83000001u, t_other}};
    const PsvModuleCode mb = {"b", fb, 1, nullptr, 0, nullptr, nullptr, 0, {}, nullptr, nullptr, 0, PSV_FLAG_CONTRACT, 0, 0, 0, 0, &db};
    recomp::register_module(ma);
    recomp::register_module(mb);
    CHECK(da.span == 0x102u);
    CHECK(da.base == 0x81000000u);
    CHECK(da.index != nullptr);
    const auto call = [&](u32 target) -> u32 {
        PsvContext c;
        std::memset(&c, 0, sizeof c);
        c.r[0] = 0xFFFFFFFFu;
        g_trap = {};
        if (setjmp(g_trap_jump) == 0) psv_call_in(&da, &c, target, 0x81000301u);
        else return 0xDEADu;  // (trapped)
        return c.r[0];
    };
    CHECK(call(0x81000001u) == 1);  // the table
    CHECK(call(0x81000101u) == 2);
    CHECK(call(0x81000010u) == 3);  // ARM code: psv_call
    CHECK(call(0x83000001u) == 4);  // another module: psv_call, through that module's table
    CHECK(call(0x81000200u) == 6);  // an import stub: psv_call
    // Not function starts: an ARM address where a Thumb function starts, a Thumb address inside one, outside the code.
    CHECK(call(0x81000000u) == 0xDEADu);
    CHECK(g_trap.kind == recomp::kTrapUnknownTarget);
    CHECK(call(0x81000003u) == 0xDEADu);
    CHECK(call(0x81000201u) == 0xDEADu);
    CHECK(call(0x80FFFFFFu) == 0xDEADu);
    CHECK(call(0x81000103u) == 0xDEADu);
    // override_function replaces the function in the table as in the registry; an address no function starts at is
    // found through the registry.
    recomp::override_function(0x81000101u, t_override);
    CHECK(call(0x81000101u) == 5);
    recomp::override_function(0x81000081u, t_override);
    CHECK(call(0x81000081u) == 5);
    CHECK(recomp::call_table_bytes() > 0);
    recomp::clear_modules();
    CHECK(da.span == 0);
    CHECK(db.span == 0);
    CHECK(call(0x81000001u) == 0xDEADu);
    recomp::set_trap_handler(previous);
}

namespace {

void ov_replacement(PsvContext* ctx, uint32_t) { ctx->r[0] += 100; }

}  // namespace

TEST_CASE("recomp: override_function replaces a function for calls through the runtime only, inlined or not") {
    recomp::clear_modules();
    for (unsigned i = 0; i < corpus_module_count; ++i) recomp::register_module(*corpus_modules[i]);
    u32 caller = 0, leaf = 0;
    for (const CorpusModule& m : kCorpusModules)
        for (u32 i = 0; i < m.function_count; ++i) {
            if (std::strcmp(m.functions[i].name, "ie_ov_caller") == 0) caller = m.functions[i].address;
            if (std::strcmp(m.functions[i].name, "ie_ov_leaf") == 0) leaf = m.functions[i].address;
        }
    REQUIRE(caller != 0);
    REQUIRE(leaf != 0);
    nx::vector<u8> memory(kBufferSize, 0);
    for (const CorpusModule& m : kCorpusModules) std::memcpy(memory.data() + offset_of(m.base), m.bytes, m.size);
    psv_membase = membase_for(memory);
    const auto run = [&](u32 x) {
        PsvContext c;
        std::memset(&c, 0, sizeof c);
        c.r[0] = x;
        c.r[13] = kCorpusStackBase + kCorpusStackSize - 256;
        c.r[14] = kReturn;
        recomp::find_function(caller)(&c, kReturn);
        return c.r[0];
    };
    // ie_ov_caller: r4 = leaf(x) directly, then leaf(r4) through psv_call, r0 = that + r4.
    CHECK(run(5) == 6u + 7u);
    recomp::override_function(leaf, ov_replacement);
    CHECK(run(5) == 6u + 106u);  // (the direct call: the generated function, as before overrides; through psv_call: it)
    recomp::clear_modules();
    psv_membase = nullptr;
}
