// Host implementations of guest functions (psvrecomp --replace; guest.h, psv_host_*; libs/recomp/src/host_functions.cpp):
//
// - each psv_host_* function against a reference model of the checked SceLibc build's function (byte by byte, as its
//   code does it) on random buffers: lengths, alignments, overlapping buffers, NUL positions, and buffers ending at the
//   last byte before a page that faults when touched (a host function reading further than the guest's code would
//   crashes the test); the context but r0 (and the exclusive monitor, for the atomics) must stay as it was;
// - the guards, and the atomics from several threads at once;
// - codegen/replace_module.hpp's module translated with two functions replaced (replacegen): its callers, by a direct
//   call, a tail branch, a table case, an indirect call and a guarded call both ways, against the interpreter running
//   the guest code of the replaced functions;
// - with PSVRECORE_TEST_LIBC=<libc.suprx or its ELF> (local only: no Sony code in the repository), each psv_host_*
//   function against that module's own export in the interpreter, on the same kind of random inputs: r0, r4-r11, sp,
//   d8-d15, the data memory and (atomics) the exclusive monitor must agree. Skipped without it.

#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "nxtest.h"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/support.hpp"
#include "replace_module.hpp"
#include "core/foundation/strings/format.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
extern const PsvModuleCode rep_mod_module;
}

using namespace psvrecore;

namespace {

// Guest memory: [kBase, kEnd), followed by a page that faults when touched. Code at kBase (the synthetic module, or
// the libc module at its linked address), the stack below kStackTop, the test's buffers in [kData, kEnd).
constexpr u32 kBase = 0x81000000u;
constexpr u32 kEnd = 0x81400000u;
constexpr u32 kStackTop = 0x81200000u;
constexpr u32 kData = 0x81300000u;
constexpr u32 kDataSize = kEnd - kData;
constexpr u32 kReturn = 0x81FF0001u;  // (never code: the interpreter stops there)

u8* guest_memory() {
    static u8* memory = [] {
        const usize size = kEnd - kBase;
#ifdef _WIN32
        u8* m = static_cast<u8*>(VirtualAlloc(nullptr, size + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (m) VirtualProtect(m + size, 0x10000, PAGE_NOACCESS, &old);
#else
        void* v = mmap(nullptr, size + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        u8* m = v == MAP_FAILED ? nullptr : static_cast<u8*>(v);
        if (m) mprotect(m + size, 0x10000, PROT_NONE);
#endif
        return m;
    }();
    return memory;
}
u8* at(u32 address) { return guest_memory() + (address - kBase); }
void use_guest_memory() { psv_membase = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(guest_memory()) - kBase); }

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

PsvContext random_context(Rng& rng) {
    PsvContext c;
    std::memset(&c, 0, sizeof c);
    for (int r = 0; r < 15; ++r) c.r[r] = rng.u32v();
    c.n = rng.below(2), c.z = rng.below(2), c.c = rng.below(2), c.v = rng.below(2);
    for (int d = 0; d < 32; ++d) c.vfp.d[d] = rng.next();
    c.r[13] = kStackTop - 16 * rng.below(64);
    c.r[14] = kReturn;
    if (rng.below(2)) c.excl_address = kData, c.excl_size = 4, c.excl_value = rng.u32v();
    return c;
}

// A length: mostly short, some across the guest code's chunked paths (16, 32, 64 bytes, pages).
u32 random_length(Rng& rng) {
    switch (rng.below(8)) {
        case 0: return rng.below(4);
        case 1: return rng.below(17);
        case 2: return rng.below(70);
        case 3: return 60 + rng.below(140);
        case 4: return rng.below(600);
        case 5: return 4000 + rng.below(9000);
        default: return rng.below(300);
    }
}

void fill_random(Rng& rng, u32 address, u32 size) {
    u8* p = at(address);
    for (u32 i = 0; i < size; i += 4) {
        const u32 w = rng.u32v();
        std::memcpy(p + i, &w, std::min<u32>(4, size - i));
    }
}

// `length` non-NUL bytes and a NUL at `address`.
void put_string(Rng& rng, u32 address, u32 length) {
    u8* p = at(address);
    for (u32 i = 0; i < length; ++i) p[i] = static_cast<u8>(1 + rng.below(255));
    p[length] = 0;
}

// ---- The reference models: the checked build's code, byte by byte (tools/psvrecomp/host_functions.tsv).

void ref_forward_copy(u32 d, u32 s, u32 n) {
    for (u32 i = 0; i < n; ++i) *at(d + i) = *at(s + i);
}
void ref_memmove(u32 d, u32 s, u32 n) {
    if (d - s >= n) ref_forward_copy(d, s, n);  // (the destination not inside the source above its start)
    else
        for (u32 i = n; i-- > 0;) *at(d + i) = *at(s + i);
}
u32 ref_memcmp(u32 a, u32 b, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (*at(a + i) != *at(b + i)) return *at(a + i) > *at(b + i) ? 1u : 0xFFFFFFFFu;
    return 0;
}
u32 ref_strcmp(u32 a, u32 b) {
    for (u32 i = 0;; ++i) {
        const u8 x = *at(a + i), y = *at(b + i);
        if (x != y) return x > y ? 1u : 0xFFFFFFFFu;
        if (x == 0) return 0;
    }
}
u32 ref_strlen(u32 s) {
    u32 n = 0;
    while (*at(s + n)) ++n;
    return n;
}
void ref_strncpy(u32 d, u32 s, u32 n) {
    u32 i = 0;
    for (; n; ++i, --n) {
        if (*at(s + i) == 0) break;
        *at(d + i) = *at(s + i);
    }
    for (; n; ++i, --n) *at(d + i) = 0;
}
u32 ref_wcslen(u32 s) {
    u32 p = s;
    for (;;) {
        u16 c;
        std::memcpy(&c, at(p), 2);
        if (c == 0) break;
        p += 2;
    }
    return static_cast<u32>(static_cast<i32>(p - s) >> 1);
}

enum class Fn { Memcpy, Memmove, Memset, Memcmp, Strlen, Strcmp, Strncpy, Wcslen, FetchAdd, FetchSub };
constexpr Fn kAll[] = {Fn::Memcpy, Fn::Memmove, Fn::Memset, Fn::Memcmp, Fn::Strlen, Fn::Strcmp, Fn::Strncpy, Fn::Wcslen, Fn::FetchAdd, Fn::FetchSub};
constexpr const char* kFnNames[] = {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncpy", "wcslen", "_Atomic_fetch_add_4", "_Atomic_fetch_sub_4"};
// (library NID, function NID: SceLibc, SceLibstdcxx)
constexpr u32 kNids[][2] = {{0xBE43BB07, 0x7205BFDB}, {0xBE43BB07, 0xAF5C218D}, {0xBE43BB07, 0x6DC1F0D8}, {0xBE43BB07, 0x7747F6D7},
                            {0xBE43BB07, 0x8AECC873}, {0xBE43BB07, 0x1B58FA3B}, {0xBE43BB07, 0x9F87712D}, {0xBE43BB07, 0xA778A14B},
                            {0xF7629240, 0x67D5334B}, {0xF7629240, 0x2F7BA372}};
constexpr PsvFunction kHost[] = {psv_host_memcpy, psv_host_memmove, psv_host_memset, psv_host_memcmp, psv_host_strlen,
                                 psv_host_strcmp, psv_host_strncpy, psv_host_wcslen, psv_host_atomic_fetch_add_4, psv_host_atomic_fetch_sub_4};
const char* name(Fn fn) { return kFnNames[static_cast<int>(fn)]; }
bool atomic(Fn fn) { return fn == Fn::FetchAdd || fn == Fn::FetchSub; }

// Random arguments for a function, the data region filled to suit them. `edge`: the buffers the function reads end at
// kEnd (the faulting page right after the last byte the guest's code reads there).
void make_case(Fn fn, Rng& rng, PsvContext& c, bool edge) {
    fill_random(rng, kData, kDataSize);
    const auto place = [&](u32 size) { return edge ? kEnd - size : kData + rng.below(kDataSize - size - 64); };
    const auto overlapping = [&](u32 s, u32 n) {  // (an overlapping destination, either way, else one anywhere)
        u32 d = s + rng.below(2 * n + 16) - std::min<u32>(n + 8, s - kData);
        if (d < kData || d + n + 64 > kEnd) d = kData + rng.below(kDataSize / 2 - n - 64);
        return d;
    };
    switch (fn) {
        case Fn::Memcpy:
        case Fn::Memmove: {
            const u32 n = random_length(rng);
            u32 s = place(n), d;
            const u32 kind = rng.below(4);
            if (kind == 0 && !edge) d = overlapping(s, n);
            else if (kind == 1 && !edge) d = s - rng.below(17);  // (just below the source: the forward copy's case)
            else d = kData + rng.below(kDataSize / 2 - n - 64);
            if (edge && rng.below(2)) std::swap(s, d);
            c.r[0] = d, c.r[1] = s, c.r[2] = n;
            break;
        }
        case Fn::Memset: {
            const u32 n = random_length(rng);
            c.r[0] = place(n), c.r[2] = n;
            break;
        }
        case Fn::Memcmp: {
            const u32 n = random_length(rng);
            const u32 a = place(n), b = kData + rng.below(kDataSize / 2 - n - 64);
            std::memmove(at(b), at(a), n);
            if (n && rng.below(4)) *at(b + rng.below(n)) = static_cast<u8>(rng.u32v());  // (a difference, or none)
            if (rng.below(2)) c.r[0] = a, c.r[1] = b;
            else c.r[0] = b, c.r[1] = a;
            c.r[2] = n;
            break;
        }
        case Fn::Strlen: {
            const u32 len = random_length(rng);
            const u32 s = place(len + 1);
            put_string(rng, s, len);
            c.r[0] = s;
            break;
        }
        case Fn::Strcmp: {
            const u32 len = random_length(rng);
            const u32 a = place(len + 1), b = kData + rng.below(kDataSize / 2 - len - 64);
            put_string(rng, a, len);
            std::memmove(at(b), at(a), len + 1);
            if (rng.below(3) == 0) *at(b + rng.below(len + 1)) = static_cast<u8>(rng.u32v());  // (another byte, an earlier end)
            if (rng.below(2)) c.r[0] = a, c.r[1] = b;
            else c.r[0] = b, c.r[1] = a;
            break;
        }
        case Fn::Strncpy: {
            const u32 n = random_length(rng);
            const u32 len = rng.below(2) ? rng.below(n + 1) : random_length(rng);
            const u32 s = edge ? kEnd - len - 1 : kData + kDataSize / 2 + rng.below(kDataSize / 2 - len - 64);
            put_string(rng, s, len);
            u32 d = !edge && rng.below(3) == 0 ? overlapping(s, n) : kData + rng.below(kDataSize / 2 - n - 64);
            if (d + n > kEnd) d = kData;
            c.r[0] = d, c.r[1] = s, c.r[2] = n;
            break;
        }
        case Fn::Wcslen: {
            const u32 len = random_length(rng) / 2;
            const u32 s = (edge ? kEnd - 2 * len - 2 : kData + rng.below(kDataSize - 2 * len - 64)) & ~1u;
            for (u32 i = 0; i < len; ++i) {
                const u16 ch = static_cast<u16>(1 + rng.below(0xFFFF));
                std::memcpy(at(s + 2 * i), &ch, 2);
            }
            std::memset(at(s + 2 * len), 0, 2);
            c.r[0] = s;
            break;
        }
        case Fn::FetchAdd:
        case Fn::FetchSub: {
            c.r[0] = (edge ? kEnd - 4 : kData + rng.below(kDataSize - 64)) & ~3u;
            c.r[1] = rng.below(4) ? rng.u32v() : rng.below(3);
            c.r[2] = rng.below(6);  // (memory orders 0-5: the guard's range)
            break;
        }
    }
}

// What the function does by the reference model: memory (in the data region) changed, and r0.
u32 reference(Fn fn, const PsvContext& c) {
    const u32 r0 = c.r[0], r1 = c.r[1], r2 = c.r[2];
    switch (fn) {
        case Fn::Memcpy: ref_forward_copy(r0, r1, r2); return r0;
        case Fn::Memmove: ref_memmove(r0, r1, r2); return r0;
        case Fn::Memset:
            for (u32 i = 0; i < r2; ++i) *at(r0 + i) = static_cast<u8>(r1);
            return r0;
        case Fn::Memcmp: return ref_memcmp(r0, r1, r2);
        case Fn::Strlen: return ref_strlen(r0);
        case Fn::Strcmp: return ref_strcmp(r0, r1);
        case Fn::Strncpy: ref_strncpy(r0, r1, r2); return r0;
        case Fn::Wcslen: return ref_wcslen(r0);
        case Fn::FetchAdd:
        case Fn::FetchSub: {
            u32 v;
            std::memcpy(&v, at(r0), 4);
            const u32 nv = fn == Fn::FetchAdd ? v + r1 : v - r1;
            std::memcpy(at(r0), &nv, 4);
            return v;
        }
    }
    return 0;
}

// Whether the function's guard (tools/psvrecomp/host_functions.tsv) lets the host function run.
bool guard_holds(Fn fn, const PsvContext& c) {
    if (fn == Fn::Memcpy) return psv_host_memcpy_exact(&c) != 0;
    if (atomic(fn)) return psv_host_atomic_order_valid(&c) != 0;
    return true;
}

// What must agree. `abi`: only what a C caller may read after the call (r0, r4-r11, sp, d8-d15; the guest's code
// changes the others as the AAPCS lets it); else the whole context but r0.
nx::string context_differences(const PsvContext& a, const PsvContext& b, bool monitor, bool abi) {
    nx::string out;
    for (int r = 0; r < 15; ++r) {
        if (abi && (r == 1 || r == 2 || r == 3 || r == 12 || r == 14)) continue;
        if (a.r[r] != b.r[r]) out += nx::format(" r{}: {:08x} vs {:08x};", r, a.r[r], b.r[r]);
    }
    if (!abi && (a.n != b.n || a.z != b.z || a.c != b.c || a.v != b.v)) out += " flags differ;";
    for (int d = abi ? 8 : 0; d < (abi ? 16 : 32); ++d)
        if (a.vfp.d[d] != b.vfp.d[d]) out += nx::format(" d{}: {:016x} vs {:016x};", d, a.vfp.d[d], b.vfp.d[d]);
    if (monitor && (a.excl_size != b.excl_size || a.excl_address != b.excl_address || (a.excl_value & 0xFFFFFFFFu) != (b.excl_value & 0xFFFFFFFFu)))
        out += nx::format(" monitor: {:08x}/{}/{:x} vs {:08x}/{}/{:x};", a.excl_address, a.excl_size, a.excl_value, b.excl_address,
                          b.excl_size, b.excl_value);
    return out;
}

nx::string memory_difference(const nx::vector<u8>& expected, u32 from) {
    const u8* m = at(from);
    for (usize i = 0; i < expected.size(); ++i)
        if (m[i] != expected[i]) return nx::format(" memory at 0x{:08x}: {:02x}, expected {:02x};", from + static_cast<u32>(i), m[i], expected[i]);
    return {};
}

nx::string describe_case(Fn fn, const PsvContext& c, bool edge) {
    return nx::format("{}{} (r0={:08x} r1={:08x} r2={:08x})", name(fn), edge ? " at the end of memory" : "", c.r[0], c.r[1], c.r[2]);
}

// Runs the interpreter from `code` until it returns to kReturn (false: it trapped or did not return).
bool interpret(PsvContext& c, u32 code) {
    interp::Interpreter it(c);
    it.thumb = (code & 1) != 0;
    it.pc = code & ~1u;
    for (u64 steps = 0; !(it.pc == (kReturn & ~1u) && it.thumb); ++steps)
        if (steps > 50'000'000 || it.step() == interp::Result::Trap) return false;
    return true;
}

}  // namespace

TEST_CASE("host functions: psv_host_* against the reference models on random buffers") {
    REQUIRE(guest_memory() != nullptr);
    use_guest_memory();
    Rng rng{0x7A5C0DEull};
    usize runs = 0, failures = 0, unguarded = 0;
    nx::vector<u8> initial(kDataSize), expected;
    for (Fn fn : kAll) {
        usize fn_failures = 0;
        for (int trial = 0; trial < 3000; ++trial) {
            const bool edge = trial % 4 == 3;
            PsvContext c = random_context(rng);
            make_case(fn, rng, c, edge);
            if (!guard_holds(fn, c)) {
                ++unguarded;
                continue;
            }
            std::memcpy(initial.data(), at(kData), kDataSize);
            PsvContext want = c;
            want.r[0] = reference(fn, c);
            if (atomic(fn)) want.excl_address = c.r[0], want.excl_size = 0, want.excl_value = want.r[0];
            expected.assign(at(kData), at(kData) + kDataSize);
            std::memcpy(at(kData), initial.data(), kDataSize);
            PsvContext got = c;
            kHost[static_cast<int>(fn)](&got, kReturn);
            ++runs;
            const nx::string diff = context_differences(got, want, atomic(fn), false) + memory_difference(expected, kData);
            if (!diff.empty() && ++fn_failures <= 3) std::printf("%s:%s\n", describe_case(fn, c, edge).c_str(), diff.c_str());
        }
        failures += fn_failures;
    }
    std::printf("host functions: %zu runs against the reference models, %zu failures (%zu cases outside a guard)\n", runs, failures, unguarded);
    CHECK(failures == 0);
    CHECK(runs > 25000);
}

TEST_CASE("host functions: the guards") {
    PsvContext c{};
    const auto memcpy_exact = [&](u32 d, u32 s, u32 n) {
        c.r[0] = d, c.r[1] = s, c.r[2] = n;
        return psv_host_memcpy_exact(&c) != 0;
    };
    CHECK(memcpy_exact(0x100, 0x200, 0x100));     // apart
    CHECK(memcpy_exact(0x100, 0x110, 0x100));     // below the source, overlapping: what the forward copy does right
    CHECK(memcpy_exact(0x100, 0x100, 0x100));     // the same
    CHECK(memcpy_exact(0x110, 0x100, 0));         // nothing
    CHECK(memcpy_exact(0x200, 0x100, 0x100));     // just above
    CHECK(!memcpy_exact(0x110, 0x100, 0x100));    // above the source, inside it: the guest's code, not memmove's result
    CHECK(!memcpy_exact(0x101, 0x100, 2));
    CHECK(memcpy_exact(0x0, 0xFFFFFFF0u, 0x20));  // (below, as unsigned addresses)
    for (u32 order = 0; order < 8; ++order) {
        c.r[2] = order;
        CHECK((psv_host_atomic_order_valid(&c) != 0) == (order <= 5));
    }
}

TEST_CASE("host functions: the atomics from several threads") {
    REQUIRE(guest_memory() != nullptr);
    use_guest_memory();
    constexpr u32 kWord = kData + 0x40;
    std::memset(at(kWord), 0, 4);
    constexpr int kThreads = 4, kEach = 50000;
    std::atomic<u32> multiples{0};
    nx::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            PsvContext c{};
            u32 seen = 0;
            for (int i = 0; i < kEach; ++i) {
                c.r[0] = kWord, c.r[1] = 3, c.r[2] = 5;
                psv_host_atomic_fetch_add_4(&c, kReturn);
                seen += c.r[0] % 3 == 0;  // (every old value a multiple of 3: no torn update)
            }
            multiples += seen;
        });
    for (auto& t : threads) t.join();
    u32 v;
    std::memcpy(&v, at(kWord), 4);
    CHECK(v == 3u * kThreads * kEach);
    CHECK(multiples.load() == static_cast<u32>(kThreads * kEach));
    threads.clear();
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            PsvContext c{};
            for (int i = 0; i < kEach; ++i) {
                c.r[0] = kWord, c.r[1] = 3, c.r[2] = 5;
                psv_host_atomic_fetch_sub_4(&c, kReturn);
            }
        });
    for (auto& t : threads) t.join();
    std::memcpy(&v, at(kWord), 4);
    CHECK(v == 0u);
}

TEST_CASE("host functions: psv_host_* against a libc module's own exports in the interpreter (PSVRECORE_TEST_LIBC)") {
    const char* path = std::getenv("PSVRECORE_TEST_LIBC");
    if (!path || !*path) SKIP("PSVRECORE_TEST_LIBC is not set (a libc.suprx or its ELF; local only)");
    auto image = analysis::Image::load(nx::fs::path_view(path));
    REQUIRE(image.has_value());
    REQUIRE(guest_memory() != nullptr);
    use_guest_memory();
    nx::vector<u8> module(kStackTop - 0x100000 - kBase, 0);
    for (const auto& seg : image->segments()) {
        if (seg.base < kBase || seg.base + seg.size() > kBase + module.size()) {
            std::printf("segment at 0x%08x (0x%x bytes): outside the test's memory for modules\n", seg.base, seg.size());
            REQUIRE(false);
        }
        std::memcpy(module.data() + (seg.base - kBase), seg.bytes.data(), seg.size());
    }
    nx::string fingerprint;
    for (u8 b : image->fingerprint()) fingerprint += nx::format("{:02x}", b);
    std::printf("module %s\n", fingerprint.c_str());
    Rng rng{0x11BC0DEull};
    usize runs = 0, failures = 0;
    nx::vector<u8> initial(kDataSize), guest;
    for (Fn fn : kAll) {
        u32 code = 0;
        for (const auto& lib : image->module().exports)
            if (lib.nid == kNids[static_cast<int>(fn)][0])
                for (const auto& f : lib.functions)
                    if (f.nid == kNids[static_cast<int>(fn)][1]) code = f.address;
        if (!code) {
            std::printf("%s: not exported\n", name(fn));
            ++failures;
            continue;
        }
        usize fn_runs = 0, fn_failures = 0;
        for (int trial = 0; trial < 1500; ++trial) {
            const bool edge = trial % 4 == 3;
            std::memcpy(guest_memory(), module.data(), module.size());
            PsvContext c = random_context(rng);
            make_case(fn, rng, c, edge);
            if (!guard_holds(fn, c)) continue;
            std::memcpy(initial.data(), at(kData), kDataSize);
            PsvContext s = c;
            if (!interpret(s, code)) {
                if (++fn_failures <= 3) std::printf("%s: the interpreter trapped or did not return\n", describe_case(fn, c, edge).c_str());
                continue;
            }
            guest.assign(at(kData), at(kData) + kDataSize);
            std::memcpy(guest_memory(), module.data(), module.size());
            std::memcpy(at(kData), initial.data(), kDataSize);
            PsvContext h = c;
            kHost[static_cast<int>(fn)](&h, kReturn);
            ++fn_runs;
            const nx::string diff = context_differences(h, s, atomic(fn), true) + memory_difference(guest, kData);
            if (!diff.empty() && ++fn_failures <= 3) std::printf("%s:%s\n", describe_case(fn, c, edge).c_str(), diff.c_str());
        }
        std::printf("%s at 0x%08x: %zu runs, %zu failures\n", name(fn), code, fn_runs, fn_failures);
        runs += fn_runs;
        failures += fn_failures;
    }
    std::printf("host functions: %zu runs against the module's exports, %zu failures\n", runs, failures);
    CHECK(failures == 0);
}

// ---- The synthetic module with replaced functions (replacegen), against the interpreter.

namespace {

usize g_store_calls = 0, g_load_calls = 0, g_other_calls = 0;

jmp_buf g_trap_jump;
recomp::TrapInfo g_trap;
[[noreturn]] void on_trap(PsvContext*, const recomp::TrapInfo& trap) {
    g_trap = trap;
    std::longjmp(g_trap_jump, 1);
}

}  // namespace

extern "C" {
// rep_store's host implementation: what its guest code does but write r2 (str r1, [r0]; adds r0, r0, #4).
void test_host_store(PsvContext* ctx, uint32_t) {
    std::memcpy(PSV_HOST(ctx->r[0]), &ctx->r[1], 4);
    ctx->r[0] += 4;
    ++g_store_calls;
}
// rep_load's (ldr r0, [r0]), where r1 is 0.
void test_host_load(PsvContext* ctx, uint32_t) {
    u32 v;
    std::memcpy(&v, PSV_HOST(ctx->r[0]), 4);
    ctx->r[0] = v;
    ++g_load_calls;
}
int test_host_load_guard(const PsvContext* ctx) { return ctx->r[1] == 0; }
// rep_add3's (r0 + 3), rep_inc's (r0 + 1), rep_dbl's (r1 + 2 r0), rep_tri's (r0 (r0 + 1) / 2); the guard of rep_add3,
// rep_dbl and rep_tri (r0 odd: the guest code runs for even r0, rep_tri's calling itself with an odd one).
void test_host_add3(PsvContext* ctx, uint32_t) { ctx->r[0] += 3, ++g_other_calls; }
void test_host_inc(PsvContext* ctx, uint32_t) { ctx->r[0] += 1, ++g_other_calls; }
void test_host_dbl(PsvContext* ctx, uint32_t) { ctx->r[0] = ctx->r[1] + 2 * ctx->r[0], ++g_other_calls; }
void test_host_tri(PsvContext* ctx, uint32_t) { ctx->r[0] = ctx->r[0] * (ctx->r[0] + 1) / 2, ++g_other_calls; }
int test_host_odd(const PsvContext* ctx) { return ctx->r[0] & 1; }
}

TEST_CASE("host functions: a module's replaced functions, called every way, against the interpreter") {
    using namespace replace_test;
    REQUIRE(guest_memory() != nullptr);
    use_guest_memory();
    recomp::clear_modules();
    recomp::register_module(rep_mod_module);
    const auto previous = recomp::set_trap_handler(on_trap);
    nx::vector<u8> pristine(kEnd - kBase, 0);
    for (usize i = 0; i < sizeof kReplaceCode / sizeof kReplaceCode[0]; ++i) {
        pristine[2 * i] = static_cast<u8>(kReplaceCode[i]);
        pristine[2 * i + 1] = static_cast<u8>(kReplaceCode[i] >> 8);
    }
    struct Caller {
        const char* name;
        u32 code;
        int store_calls, load_calls, other_calls;  // host calls per run (-1: by the arguments)
    };
    // (c_add3: a guarded function whose guest code calls an inlined internal entry; c_into: running into a replaced
    // function; c_ldrpc: a jump to a constant, its address; c_dbl: a guest code's loop to its own entry; c_tri: a guest
    // code calling itself)
    const Caller callers[] = {{"c_table", kTable, -1, 0, 0},       {"c_direct", kDirect, 1, 0, 0}, {"c_tail", kTail, 1, 0, 0},
                              {"c_indirect", kIndirect, 1, 0, 0}, {"c_guarded", kGuarded, 0, -1, 0}, {"c_add3", kCallAdd3, 0, 0, -1},
                              {"c_into", kInto, 0, 0, 1},          {"c_ldrpc", kLdrPc, 0, 0, 1},     {"c_dbl", kCallDbl, 0, 0, -1},
                              {"c_tri", kCallTri, 0, 0, -1}};
    Rng rng{0x5EB1ACEull};
    usize runs = 0, failures = 0, guarded_host = 0, guarded_guest = 0, table_host = 0;
    for (const Caller& caller : callers) {
        const PsvFunction f = recomp::find_function(caller.code);
        REQUIRE(f != nullptr);
        for (int trial = 0; trial < 64; ++trial) {
            std::memcpy(guest_memory(), pristine.data(), pristine.size());
            fill_random(rng, kData, 0x1000);
            const nx::vector<u8> initial(at(kData), at(kData) + 0x1000);
            PsvContext start = random_context(rng);
            start.r[0] = kData + 4 * rng.below(0x100);
            start.r[2] = kRepStore;                                    // (c_indirect's target)
            start.r[3] = rng.below(2) ? rng.below(2) : rng.below(6);  // (c_table's index: 1 is rep_store's case)
            if (caller.code == kGuarded && rng.below(2)) start.r[1] = 0;
            if (caller.code == kCallDbl || caller.code == kCallTri) start.r[0] = rng.below(12);  // (a loop's, a recursion's count)

            const usize stores = g_store_calls, loads = g_load_calls, others = g_other_calls;
            PsvContext g = start;
            bool trapped = false;
            if (setjmp(g_trap_jump) == 0) f(&g, kReturn);
            else trapped = true;
            const int store_calls = static_cast<int>(g_store_calls - stores), load_calls = static_cast<int>(g_load_calls - loads);
            const int other_calls = static_cast<int>(g_other_calls - others);
            const nx::vector<u8> generated(at(kData), at(kData) + 0x1000);
            nx::vector<u8> generated_stack(at(kStackTop - 0x1000), at(kStackTop));

            std::memcpy(guest_memory(), pristine.data(), pristine.size());
            std::memcpy(at(kData), initial.data(), initial.size());
            PsvContext s = start;
            const bool returned = interpret(s, caller.code);
            ++runs;
            // (c_tri: below its own frame, the frames of the guest code's calls of itself, which the host function
            // standing in for one of them does not push; dead memory below sp)
            if (caller.code == kCallTri)
                for (u32 a = kStackTop - 0x1000; a < start.r[13] - 8; ++a) generated_stack[a - (kStackTop - 0x1000)] = *at(a);

            nx::string diff;
            if (trapped) diff += nx::format(" trapped: {};", recomp::describe(g_trap));
            if (!returned) diff += " the interpreter trapped or did not return;";
            if (!trapped && returned) diff += context_differences(g, s, false, true) + memory_difference(generated, kData) + memory_difference(generated_stack, kStackTop - 0x1000);
            const int want_stores = caller.store_calls >= 0 ? caller.store_calls : (start.r[3] == 1 ? 1 : 0);
            const int want_loads = caller.load_calls >= 0 ? caller.load_calls : (start.r[1] == 0 ? 1 : 0);
            // (c_add3, c_dbl: the host function for odd r0; c_tri: for odd r0, or for the guest code's call with r0 - 1)
            const bool odd = start.r[0] & 1;
            const int want_others = caller.other_calls >= 0 ? caller.other_calls : caller.code == kCallTri ? (start.r[0] != 0 ? 1 : 0) : (odd ? 1 : 0);
            if (store_calls != want_stores || load_calls != want_loads || other_calls != want_others)
                diff += nx::format(" host calls: {} stores, {} loads, {} others; expected {}, {}, {};", store_calls, load_calls, other_calls,
                                   want_stores, want_loads, want_others);
            if (caller.code == kGuarded) ++(want_loads ? guarded_host : guarded_guest);
            if (caller.code == kCallAdd3 || caller.code == kCallDbl || caller.code == kCallTri) ++(odd ? guarded_host : guarded_guest);
            if (caller.code == kTable && want_stores) ++table_host;
            if (!diff.empty() && ++failures <= 5) std::printf("%s run %d:%s\n", caller.name, trial, diff.c_str());
        }
    }
    recomp::set_trap_handler(previous);
    recomp::clear_modules();
    std::printf("replaced functions: %zu runs, %zu failures (guarded: %zu by the host function, %zu by the guest code; table case "
                "to the host function: %zu)\n", runs, failures, guarded_host, guarded_guest, table_host);
    CHECK(failures == 0);
    CHECK(guarded_host > 0);
    CHECK(guarded_guest > 0);
    CHECK(table_host > 0);
}
