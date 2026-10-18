// The code generator against the reference interpreter: blocks of random instructions (fuzzgen's corpus, made at
// build time with a fixed seed) run as generated code and in the interpreter from the same random states; registers,
// flags, the exclusive monitor and memory must end up the same.
//
// Memory accesses are folded into a 64 KiB buffer (PSV_ADDRESS_MASK in the generated code, the interpreter's
// address mask), so random addresses are safe.

#include <csetjmp>
#include <cstdio>
#include <cstring>

#include "fuzz_blocks.h"
#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/arm/format.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/support.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

constexpr u32 kMask = 0xFFFF;
constexpr usize kMemory = 0x10000 + 64;  // (an access at the end of the window may run past it)

struct Rng {
    u64 s;
    u64 next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    u32 u32v() { return static_cast<u32>(next() >> 32); }
};

// Register values that reach the edge cases: zero, one, the extremes, small shift amounts, and random words.
u32 interesting(Rng& rng) {
    static constexpr u32 kSpecial[] = {0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x8000, 0x7FFF, 0xFF, 0x80, 31, 32, 33};
    const u32 pick = rng.u32v() % 8;
    if (pick == 0) return kSpecial[rng.u32v() % (sizeof kSpecial / sizeof kSpecial[0])];
    if (pick == 1) return rng.u32v() % 64;
    if (pick == 2) return static_cast<u32>(-static_cast<i32>(rng.u32v() % 64));
    return rng.u32v();
}

// Floating-point values that reach the edge cases: zeros, infinities, quiet and signalling NaNs, denormals, the
// smallest normal and its neighbours, values whose products and sums land near the flush-to-zero threshold.
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
    // Sometimes an exclusive access outstanding (stores after an ldrex in the block itself are covered anyway).
    if (rng.u32v() % 4 == 0) {
        c.excl_address = rng.u32v() & kMask;
        c.excl_size = 4;
        c.excl_value = rng.u32v();
    }
    return c;
}

jmp_buf g_trap_jump;
recomp::TrapInfo g_trap;
[[noreturn]] void on_trap(PsvContext*, const recomp::TrapInfo& trap) {
    g_trap = trap;
    std::longjmp(g_trap_jump, 1);
}

nx::string disassembly(const FuzzBlock& b) {
    nx::string out;
    arm::ItState it;
    u32 at = b.address;
    for (u32 i = 0; i < b.count; ++i) {
        const u32 raw = b.raw[i];
        arm::Instruction in;
        if (b.thumb) {
            const bool wide = raw > 0xFFFF;
            in = arm::decode_thumb(static_cast<u16>(wide ? raw >> 16 : raw), static_cast<u16>(wide ? raw : 0), at, it);
        } else {
            in = arm::decode_arm(raw, at);
        }
        out += nx::format("    {:08x}  {:08x}  {}\n", at, raw, arm::format(in));
        at += in.size;
    }
    return out;
}

// The differences between two end states (empty: none).
nx::string differences(const PsvContext& a, const PsvContext& b, const nx::vector<u8>& ma, const nx::vector<u8>& mb) {
    nx::string out;
    for (int r = 0; r < 15; ++r)
        if (a.r[r] != b.r[r]) out += nx::format(" r{}: {:08x} vs {:08x};", r, a.r[r], b.r[r]);
    const auto flag = [&](const char* name, u32 x, u32 y) {
        if (x != y) out += nx::format(" {}: {} vs {};", name, x, y);
    };
    flag("n", a.n, b.n);
    flag("z", a.z, b.z);
    flag("c", a.c, b.c);
    flag("v", a.v, b.v);
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
    for (usize i = 0; i < ma.size(); ++i)
        if (ma[i] != mb[i]) {
            out += nx::format(" memory at 0x{:04x}: {:02x} vs {:02x};", i, ma[i], mb[i]);
            break;
        }
    return out;
}

}  // namespace

TEST_CASE("recomp: generated code agrees with the interpreter on random instruction blocks") {
    REQUIRE(fuzz_block_count > 0);
    const auto previous = recomp::set_trap_handler(on_trap);
    Rng rng{0x5EED5EED12345ull};
    nx::vector<u8> initial(kMemory), generated(kMemory), interpreted(kMemory);
    usize runs = 0, failures = 0;
    constexpr int kStatesPerBlock = 4;
    for (u32 b = 0; b < fuzz_block_count; ++b) {
        const FuzzBlock& block = fuzz_blocks[b];
        for (int trial = 0; trial < kStatesPerBlock; ++trial) {
            for (usize i = 0; i < kMemory; i += 4) {
                const u32 w = rng.u32v();
                std::memcpy(&initial[i], &w, std::min<usize>(4, kMemory - i));
            }
            const PsvContext start = random_state(rng);

            // Generated code.
            PsvContext g = start;
            generated = initial;
            psv_membase = generated.data();
            bool trapped = false;
            if (setjmp(g_trap_jump) == 0) block.function(&g, 0xFEED0001u);
            else trapped = true;

            // The interpreter, on the same instructions.
            PsvContext s = start;
            interpreted = initial;
            psv_membase = interpreted.data();
            interp::Interpreter it(s);
            it.address_mask = kMask;
            it.thumb = block.thumb != 0;
            it.pc = block.address;
            bool interp_trapped = false;
            for (u32 i = 0; i < block.count; ++i) {
                const u32 raw = block.raw[i];
                arm::Instruction in;
                if (block.thumb) {
                    const bool wide = raw > 0xFFFF;
                    in = arm::decode_thumb(static_cast<u16>(wide ? raw >> 16 : raw), static_cast<u16>(wide ? raw : 0), it.pc, it.it);
                } else {
                    in = arm::decode_arm(raw, it.pc);
                }
                const auto r = it.execute(in);
                if (r == interp::Result::Trap) {
                    interp_trapped = true;
                    break;
                }
                if (r != interp::Result::Next) std::printf("interpreter branched at %s\n", arm::format(in).c_str());
                REQUIRE(r == interp::Result::Next);
            }
            ++runs;
            nx::string diff;
            if (trapped != interp_trapped || (trapped && g_trap.kind != it.trap_kind))
                diff = nx::format(" trapped: generated {} ({}), interpreter {} ({})", trapped, trapped ? recomp::describe(g_trap) : "",
                                  interp_trapped, it.trap_kind);
            else
                diff = differences(g, s, generated, interpreted);  // (both trapped at the same point: the same state too)
            if (!diff.empty()) {
                if (++failures <= 10) {
                    std::printf("%s", nx::format("block {} ({}), state {}:{}\n{}", b, block.thumb ? "thumb" : "arm", trial, diff, disassembly(block)).c_str());
                    nx::string initial = nx::format("    initial: fpscr {:08x}", start.fpscr);
                    for (int d = 0; d < 32; ++d) initial += nx::format(" d{}={:016x}", d, start.vfp.d[d]);
                    std::printf("%s\n", initial.c_str());
                }
            }
        }
    }
    recomp::set_trap_handler(previous);
    psv_membase = nullptr;
    std::printf("%s\n", nx::format("generator vs interpreter: {} blocks, {} runs, {} disagreements", fuzz_block_count, runs, failures).c_str());
    CHECK(failures == 0);
}
