// Advanced SIMD against hand-computed results, for both implementations: the interpreter (through the instruction)
// and guest_simd.h's helpers (as generated code calls them). Expected values follow from the ARM ARM's pseudocode;
// the reasoning is in the comments. Element lists are lowest element first.
//
// Encodings were produced by clang's integrated assembler (armv7a, cortex-a9, neon).

#include <cstring>

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/guest.h"

using namespace psvrecore;

namespace {

struct Neon {
    PsvContext ctx;  // the interpreter's
    PsvContext gen;  // the helpers'
    nx::vector<u8> memory = nx::vector<u8>(0x10000 + 16, 0);
    interp::Interpreter it{ctx};
    Neon() {
        std::memset(&ctx, 0, sizeof ctx);
        std::memset(&gen, 0, sizeof gen);
        psv_membase = memory.data();
        it.address_mask = 0xFFFF;
    }
    ~Neon() { psv_membase = nullptr; }
    void d(u32 n, u64 v) { ctx.vfp.d[n] = gen.vfp.d[n] = v; }
    void run(u32 word) {
        it.thumb = false;
        it.pc = 0x1000;
        REQUIRE(it.execute(arm::decode_arm(word, 0x1000)) == interp::Result::Next);
    }
    // The register in both implementations.
    bool both(u32 n, u64 expected) const { return ctx.vfp.d[n] == expected && gen.vfp.d[n] == expected; }
    bool qc() const { return (ctx.fpscr & PSV_FPSCR_QC) && (gen.fpscr & PSV_FPSCR_QC); }
};

u64 lanes32(u32 lo, u32 hi) { return (u64{hi} << 32) | lo; }

}  // namespace

TEST_CASE("simd: reciprocal and reciprocal square root estimates") {
    Neon v;
    // vrecpe.f32 d0, d1: 2.0 scales to 0.5: q = 256, s = RoundDown(256 / (256.5 / 512) + 0.5) = 511, exponent
    // 253 - 128 = 125: 511/256 * 2^-2 = 0x3EFF8000. -0 gives -infinity.
    v.d(1, lanes32(0x40000000, 0x80000000));
    v.run(0xF3BB0501);
    psv_nmisc(&v.gen, PSV_NM_FRECPE, 32, 0, 1, 0, 1);
    CHECK(v.both(0, lanes32(0x3EFF8000, 0xFF800000)));
    // vrsqrte.f32 d0, d1: 4.0 (odd exponent: scaled into [0.25, 0.5), q = 128): s = 511, exponent (380 - 129) / 2.
    // 2.0 (even: [0.5, 1), q = 128): s = RoundDown(256 / sqrt(128.5 / 256) + 0.5) = 361, about 0.705.
    v.d(1, lanes32(0x40800000, 0x40000000));
    v.run(0xF3BB0581);
    psv_nmisc(&v.gen, PSV_NM_FRSQRTE, 32, 0, 1, 0, 1);
    CHECK(v.both(0, lanes32(0x3EFF8000, 0x3F348000)));
    // 1.0 gives 0.998 (0x3F7F8000); a negative number the default NaN.
    v.d(1, lanes32(0x3F800000, 0xBF800000));
    v.run(0xF3BB0581);
    psv_nmisc(&v.gen, PSV_NM_FRSQRTE, 32, 0, 1, 0, 1);
    CHECK(v.both(0, lanes32(0x3F7F8000, 0x7FC00000)));
}

TEST_CASE("simd: Newton-Raphson steps and maximum") {
    Neon v;
    // vrecps.f32 d0, d1, d2: infinity * 0 counts as a product of 0: 2 - 0 = 2; 2 * 0.5: 2 - 1 = 1.
    v.d(1, lanes32(0x7F800000, 0x40000000));
    v.d(2, lanes32(0x00000000, 0x3F000000));
    v.run(0xF2010F12);
    psv_nf3(&v.gen, PSV_NF_RECPS, 1, 0, 1, 2, -1);
    CHECK(v.both(0, lanes32(0x40000000, 0x3F800000)));
    // infinity * 2: 2 - infinity = -infinity.
    v.d(1, lanes32(0x7F800000, 0x7F800000));
    v.d(2, lanes32(0x40000000, 0x40000000));
    v.run(0xF2010F12);
    psv_nf3(&v.gen, PSV_NF_RECPS, 1, 0, 1, 2, -1);
    CHECK(v.both(0, lanes32(0xFF800000, 0xFF800000)));
    // vmax.f32 d0, d1, d2: of two zeros the larger is +0 unless both are -0.
    v.d(1, lanes32(0x00000000, 0x80000000));
    v.d(2, lanes32(0x80000000, 0x80000000));
    v.run(0xF2010F02);
    psv_nf3(&v.gen, PSV_NF_MAX, 1, 0, 1, 2, -1);
    CHECK(v.both(0, lanes32(0x00000000, 0x80000000)));
}

TEST_CASE("simd: saturating doubling multiply, narrowing, rounding shift") {
    Neon v;
    // vqdmulh.s16 d0, d1, d2: (2 * a * b) >> 16. -32768 * -32768 saturates (QC); 0x4000^2: 0x2000; 256 * 256: 2;
    // -1 * 1: -1.
    v.d(1, 0xFFFF010040008000ull);
    v.d(2, 0x0001010040008000ull);
    v.run(0xF2110B02);
    psv_n3(&v.gen, PSV_N_QDMULH, 16, 0, 1, 0, 1, 2, -1);
    CHECK(v.both(0, 0xFFFF000220007FFFull));
    CHECK(v.qc());
    // vqmovn.s32 d0, q1: 70000 and -70000 saturate to 32767 and -32768; 5 and -5 fit.
    v.d(2, lanes32(70000, static_cast<u32>(-70000)));
    v.d(3, lanes32(5, static_cast<u32>(-5)));
    v.ctx.fpscr = v.gen.fpscr = 0;
    v.run(0xF3B60282);
    psv_nmisc(&v.gen, PSV_NM_QMOVN, 16, 0, 2, 0, 2);
    CHECK(v.both(0, 0xFFFB000580007FFFull));
    CHECK(v.qc());
    // vrshr.s32 d0, d1, #1: (3 + 1) >> 1 = 2, (-3 + 1) >> 1 = -1.
    v.d(1, lanes32(3, static_cast<u32>(-3)));
    v.run(0xF2BF0211);
    psv_nshift(&v.gen, PSV_NS_RSHR, 32, 0, 1, 0, 1, 1);
    CHECK(v.both(0, lanes32(2, 0xFFFFFFFF)));
    // vcvt.s32.f32 d0, d1: toward zero: 2.9 -> 2, -2.9 -> -2.
    v.d(1, lanes32(0x4039999A, 0xC039999A));
    v.run(0xF3BB0701);
    psv_nmisc(&v.gen, PSV_NM_F2S, 32, 0, 1, 0, 1);
    CHECK(v.both(0, lanes32(2, static_cast<u32>(-2))));
}

TEST_CASE("simd: permutations, table lookup, extraction, structure loads") {
    Neon v;
    // vzip.8 d0, d1: interleaved bytes, the first half to d0.
    v.d(0, 0x0706050403020100ull);
    v.d(1, 0x1716151413121110ull);
    v.run(0xF3B20181);
    psv_nperm(&v.gen, PSV_NP_ZIP, 8, 1, 0, 1);
    CHECK(v.both(0, 0x1303120211011000ull));
    CHECK(v.both(1, 0x1707160615051404ull));
    // vtbl.8 d0, {d1}, d2: indices 0 7 8 1 255 2 3 4; 8 and 255 are out of the one-register table: 0.
    v.d(1, 0x7766554433221100ull);
    v.d(2, 0x040302FF01080700ull);
    v.run(0xF3B10802);
    psv_ntbl(&v.gen, 0, 0, 1, 1, 2);
    CHECK(v.both(0, 0x4433220011007700ull));
    // vext.8 d0, d1, d2, #3: bytes 3 to 10 of d2:d1.
    v.d(1, 0x0706050403020100ull);
    v.d(2, 0x0F0E0D0C0B0A0908ull);
    v.run(0xF2B10302);
    psv_next(&v.gen, 1, 0, 1, 2, 3);
    CHECK(v.both(0, 0x0A09080706050403ull));
    // vld2.16 {d0, d1}, [r0]: halfwords 0x1000..0x1007 de-interleaved: even ones to d0, odd ones to d1.
    for (u16 i = 0; i < 8; ++i) {
        const u16 h = static_cast<u16>(0x1000 + i);
        std::memcpy(&v.memory[0x200 + 2 * i], &h, 2);
    }
    v.ctx.r[0] = 0x200;
    v.run(0xF420084F);
    CHECK(v.ctx.vfp.d[0] == 0x1006100410021000ull);
    CHECK(v.ctx.vfp.d[1] == 0x1007100510031001ull);
    CHECK(v.ctx.r[0] == 0x200);  // (no writeback: Rm = PC)
}
