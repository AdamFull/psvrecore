// Advanced SIMD against hand-computed results, for both implementations: the interpreter (through the instruction)
// and guest_simd.h's helpers (as generated code calls them). Expected values follow from the ARM ARM's pseudocode;
// the reasoning is in the comments. Element lists are lowest element first.
//
// Encodings were produced by clang's integrated assembler (armv7a, cortex-a9, neon).

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/guest.h"
#include "core/foundation/strings/utf8_string.h"

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

// A float's bits for testing the fast paths: mostly the cases at their edges (zeros, denormals, the smallest
// normals' binades, infinities, NaNs, exponents that cancel, overflow or underflow together), else random.
u32 edge_float(u32& seed) {
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    const u32 r = next(), sign = next() & 0x80000000u, mant = next() & 0x7FFFFFu;
    switch (r % 12) {
        case 0: return sign;                                     // zero
        case 1: return sign | mant;                              // denormal (or zero)
        case 2: return sign | (1u << 23) | mant;                 // the smallest normals
        case 3: return sign | (2u << 23) | mant;
        case 4: return sign | 0x7F800000u | ((r >> 8) & 1 ? mant : 0);  // infinity or NaN
        case 5: return sign | ((60u + (r >> 4) % 8) << 23) | mant;     // ~2^-64: products near the smallest normal
        case 6: return sign | ((190u + (r >> 4) % 8) << 23) | mant;    // ~2^64: products near overflow
        case 7: return sign | (127u << 23) | (mant & 0x7Fu);           // near 1: sums that cancel
        default: return next();
    }
}

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

TEST_CASE("simd: the float arithmetic's fast path gives the interpreter's results") {
    // psv_nf3 computes the arithmetic forms four lanes at once when no lane needs the exact path: against the
    // interpreter, Q and D registers, by vector and by scalar, on values at the edges.
    Neon v;
    u32 seed = 777;
    struct Case {
        u32 word;
        int op;
        u32 regs, d, n, m;
        int scalar;
    };
    const Case cases[] = {
        {0xF2020D44, PSV_NF_ADD, 2, 0, 2, 4, -1},  // vadd.f32 q0, q1, q2
        {0xF2220D44, PSV_NF_SUB, 2, 0, 2, 4, -1},  // vsub.f32 q0, q1, q2
        {0xF3020D54, PSV_NF_MUL, 2, 0, 2, 4, -1},  // vmul.f32 q0, q1, q2
        {0xF2020D54, PSV_NF_MLA, 2, 0, 2, 4, -1},  // vmla.f32 q0, q1, q2
        {0xF2220D54, PSV_NF_MLS, 2, 0, 2, 4, -1},  // vmls.f32 q0, q1, q2
        {0xF2010D02, PSV_NF_ADD, 1, 0, 1, 2, -1},  // vadd.f32 d0, d1, d2
        {0xF2010D12, PSV_NF_MLA, 1, 0, 1, 2, -1},  // vmla.f32 d0, d1, d2
        {0xF3A20964, PSV_NF_MUL, 2, 0, 2, 4, 1},   // vmul.f32 q0, q1, d4[1]
        {0xF3A20144, PSV_NF_MLA, 2, 0, 2, 4, 0},   // vmla.f32 q0, q1, d4[0]
        {0xF2A10564, PSV_NF_MLS, 1, 0, 1, 4, 1},   // vmls.f32 d0, d1, d4[1]
    };
    u32 differ = 0;
    for (u32 i = 0; i < 100000; ++i)
        for (const Case& c : cases) {
            // (the lanes: often all normal, so the fast path runs, sometimes one at an edge)
            const bool plain = (i & 3) != 0;
            for (u32 reg = 0; reg < 6; ++reg) {
                u32 lane[2];
                for (u32& x : lane) {
                    x = edge_float(seed);
                    if (plain) x = (x & 0x807FFFFFu) | ((100u + (x >> 23) % 50) << 23);
                }
                v.d(reg, lanes32(lane[0], lane[1]));
            }
            v.run(c.word);
            psv_nf3(&v.gen, c.op, c.regs, c.d, c.n, c.m, c.scalar);
            for (u32 r = 0; r < c.regs; ++r)
                if (v.ctx.vfp.d[c.d + r] != v.gen.vfp.d[c.d + r] && differ++ < 5)
                    std::fprintf(stderr, "word %08x d%u: %016llx, the interpreter's %016llx\n", c.word, c.d + r,
                                 static_cast<unsigned long long>(v.gen.vfp.d[c.d + r]),
                                 static_cast<unsigned long long>(v.ctx.vfp.d[c.d + r]));
        }
    CHECK(differ == 0u);
}

// ---- guest_neon.h: the helpers of Advanced SIMD on values (generated code with the registers in locals) ----------

namespace {

PsvQ q_of(const PsvContext& c, u32 d) { return psv_ctx_qget(&c, d); }
void set_q(Neon& v, u32 d, PsvQ q) {
    psv_ctx_qput(&v.ctx, d, q);
    psv_ctx_qput(&v.gen, d, q);
}
bool same_q(PsvQ a, PsvQ b) { return std::memcmp(&a, &b, 16) == 0; }
nx::string q_text(PsvQ q) {
    char t[64];
    std::snprintf(t, sizeof t, "%08x %08x %08x %08x", q[0], q[1], q[2], q[3]);
    return t;
}
bool denormal_lane(PsvQ q) {
    for (int i = 0; i < 4; ++i)
        if ((q[i] & 0x7F800000u) == 0 && (q[i] & 0x7FFFFFFFu) != 0) return true;
    return false;
}

// Single-precision values at every edge: zeros, denormals, the smallest normals and their neighbours, values whose
// products and sums land at the smallest normal (2^-63 times 2^-63 and kin), ones, the largest finite values,
// infinities, quiet and signalling NaNs with payloads of both signs, 2^31 and 2^32 (the integer conversions' bounds)
// and their neighbours, values between -1 and 0.
constexpr u32 kSpecialF32[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000, 0x00800001,
    0x00FFFFFF, 0x01000000, 0x20000000, 0x1FFFFFFF, 0x20000001, 0xA0000000, 0x1F800000, 0x203504F3, 0x3F800000, 0xBF800000,
    0x3F000000, 0xBF000000, 0x40000000, 0x40400000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F000000, 0x7F800000, 0xFF800000, 0x7FC00000,
    0xFFC00000, 0x7FC00001, 0x7F800001, 0xFF800001, 0x7FA5A5A5, 0xFFFFFFFF, 0x4F000000, 0x4EFFFFFF, 0x4F800000, 0x4F7FFFFF,
    0xCF000000, 0xCF000001, 0xBF7FFFFF, 0x3EFFFFFF, 0x5F000000, 0x3E800000,
};
constexpr u32 kSpecials = sizeof kSpecialF32 / sizeof kSpecialF32[0];
#ifdef PSV_NATIVE_FLOAT
constexpr int kNativeModes = 1;
#else
constexpr int kNativeModes = 0;
#endif

}  // namespace

TEST_CASE("simd: the single-precision helpers on values give the interpreter's results on every pair of edge values") {
    Neon v;
    // (Q forms: q0 = op(q1, q2) or q0 = q0 + op(q1, q2); each pair (i, j) of edge values in lane 0, others in the
    // other lanes; then the same with the operands known to hold no denormal, where none does)
    using Bin = PsvQ (*)(uint32_t, PsvQ, PsvQ, int);
    using Acc = PsvQ (*)(uint32_t, PsvQ, PsvQ, PsvQ, int);
    struct Case {
        const char* name;
        u32 word;
        Bin bin;
        Acc acc;
    };
    const Case cases[] = {
        {"vadd", 0xF2020D44, psv_nfadd_q, nullptr},     {"vsub", 0xF2220D44, psv_nfsub_q, nullptr},
        {"vmul", 0xF3020D54, psv_nfmul_q, nullptr},     {"vmla", 0xF2020D54, nullptr, psv_nfmla_q},
        {"vmls", 0xF2220D54, nullptr, psv_nfmls_q},     {"vabd", 0xF3220D44, psv_nfabd_q, nullptr},
        {"vceq", 0xF2020E44, psv_nfceq_q, nullptr},     {"vcge", 0xF3020E44, psv_nfcge_q, nullptr},
        {"vcgt", 0xF3220E44, psv_nfcgt_q, nullptr},     {"vacge", 0xF3020E54, psv_nfacge_q, nullptr},
        {"vacgt", 0xF3220E54, psv_nfacgt_q, nullptr},   {"vmax", 0xF2020F44, psv_nfmax_q, nullptr},
        {"vmin", 0xF2220F44, psv_nfmin_q, nullptr},     {"vrecps", 0xF2020F54, psv_nfrecps_q, nullptr},
        {"vrsqrts", 0xF2220F54, psv_nfrsqrts_q, nullptr},
    };
    u32 seed = 4242;
    usize runs = 0, differ = 0;
    for (const Case& c : cases)
        for (u32 i = 0; i < kSpecials; ++i)
            for (u32 j = 0; j < kSpecials; ++j) {
                const PsvQ a = {kSpecialF32[i], kSpecialF32[(i + 7) % kSpecials], edge_float(seed), kSpecialF32[(i * 3 + 1) % kSpecials]};
                const PsvQ b = {kSpecialF32[j], kSpecialF32[(j + 11) % kSpecials], edge_float(seed), kSpecialF32[(j * 5 + 2) % kSpecials]};
                const PsvQ d = {kSpecialF32[(i + j) % kSpecials], edge_float(seed), kSpecialF32[(i * j) % kSpecials], edge_float(seed)};
                set_q(v, 2, a);
                set_q(v, 4, b);
                set_q(v, 0, d);
                v.run(c.word);
                const PsvQ want = q_of(v.ctx, 0);
                // (with PSV_NATIVE_FLOAT also the native path: the guest FPSCR in the standard mode, the FPCR with it)
                for (int known = 0; known < 2 + kNativeModes; ++known) {
                    if (known == 1 && (denormal_lane(a) || denormal_lane(b) || denormal_lane(d))) continue;
                    const int k = known == 1 ? 7 : 0;
                    const u32 fpscr = known == 2 ? PSV_FPSCR_STANDARD : 0u;
                    if (kNativeModes) psv_native_set_fpcr(fpscr);
                    const PsvQ got = c.bin ? c.bin(fpscr, a, b, k) : c.acc(fpscr, d, a, b, k);
                    if (kNativeModes) psv_native_set_fpcr(0);
                    ++runs;
                    if (!same_q(got, want) && differ++ < 8)
                        std::printf("%s (known %d) %s, %s, %s: %s, the interpreter's %s\n", c.name, k, q_text(a).c_str(), q_text(b).c_str(),
                                    q_text(d).c_str(), q_text(got).c_str(), q_text(want).c_str());
                }
            }
    std::printf("single-precision helpers: %zu runs, %zu disagreements\n", runs, differ);
    CHECK(differ == 0u);
}

TEST_CASE("simd: D-register forms, pairwise forms, compares with zero, estimates and conversions on edge values") {
    Neon v;
    u32 seed = 99;
    usize differ = 0;
    struct Case {
        const char* name;
        u32 word;
        int kind;  // 0 binary D, 1 accumulate D, 2 pairwise D, 3 unary with the FPSCR (Q), 4 unary (Q)
        PsvD (*bin)(uint32_t, PsvD, PsvD, int);
        PsvD (*acc)(uint32_t, PsvD, PsvD, PsvD, int);
        PsvQ (*un)(uint32_t, PsvQ, int);
        PsvQ (*plain)(PsvQ);
    };
    const Case cases[] = {
        {"vadd.f32 d", 0xF2020D04, 0, psv_nfadd_d, nullptr, nullptr, nullptr},
        {"vmul.f32 d", 0xF3020D14, 0, psv_nfmul_d, nullptr, nullptr, nullptr},
        {"vmla.f32 d", 0xF2020D14, 1, nullptr, psv_nfmla_d, nullptr, nullptr},
        {"vpadd.f32", 0xF3020D04, 2, psv_nfpadd_d, nullptr, nullptr, nullptr},
        {"vpmax.f32", 0xF3020F04, 2, psv_nfpmax_d, nullptr, nullptr, nullptr},
        {"vpmin.f32", 0xF3220F04, 2, psv_nfpmin_d, nullptr, nullptr, nullptr},
        {"vcgt #0", 0xF3B90442, 3, nullptr, nullptr, psv_nfcgt0_q, nullptr},
        {"vcge #0", 0xF3B904C2, 3, nullptr, nullptr, psv_nfcge0_q, nullptr},
        {"vceq #0", 0xF3B90542, 3, nullptr, nullptr, psv_nfceq0_q, nullptr},
        {"vcle #0", 0xF3B905C2, 3, nullptr, nullptr, psv_nfcle0_q, nullptr},
        {"vclt #0", 0xF3B90642, 3, nullptr, nullptr, psv_nfclt0_q, nullptr},
        {"vrecpe.f32", 0xF3BB0542, 3, nullptr, nullptr, psv_nfrecpe_q, nullptr},
        {"vrsqrte.f32", 0xF3BB05C2, 3, nullptr, nullptr, psv_nfrsqrte_q, nullptr},
        {"vrecpe.u32", 0xF3BB0442, 4, nullptr, nullptr, nullptr, psv_nurecpe_q},
        {"vrsqrte.u32", 0xF3BB04C2, 4, nullptr, nullptr, nullptr, psv_nursqrte_q},
        {"vcvt.s32.f32", 0xF3BB0742, 4, nullptr, nullptr, nullptr, psv_ncvt_s32_f32_q},
        {"vcvt.u32.f32", 0xF3BB07C2, 4, nullptr, nullptr, nullptr, psv_ncvt_u32_f32_q},
        {"vcvt.f32.s32", 0xF3BB0642, 4, nullptr, nullptr, nullptr, psv_ncvt_f32_s32_q},
        {"vcvt.f32.u32", 0xF3BB06C2, 4, nullptr, nullptr, nullptr, psv_ncvt_f32_u32_q},
        {"vcvt.s32.f32 #8", 0xF2B80F52, 4, nullptr, nullptr, nullptr, [](PsvQ a) { return psv_ncvtx_s32_f32_q(a, 8); }},
        {"vcvt.u32.f32 #32", 0xF3A00F52, 4, nullptr, nullptr, nullptr, [](PsvQ a) { return psv_ncvtx_u32_f32_q(a, 32); }},
        {"vcvt.f32.s32 #1", 0xF2BF0E52, 4, nullptr, nullptr, nullptr, [](PsvQ a) { return psv_ncvtx_f32_s32_q(a, 1); }},
        {"vcvt.f32.u32 #16", 0xF3B00E52, 4, nullptr, nullptr, nullptr, [](PsvQ a) { return psv_ncvtx_f32_u32_q(a, 16); }},
    };
    for (const Case& c : cases)
        for (u32 i = 0; i < kSpecials; ++i)
            for (u32 j = 0; j < kSpecials; j += c.kind >= 3 ? kSpecials : 1) {
                const PsvQ a = {kSpecialF32[i], kSpecialF32[(i + 5) % kSpecials], edge_float(seed), edge_float(seed)};
                const PsvQ b = {kSpecialF32[j], kSpecialF32[(j + 13) % kSpecials], edge_float(seed), edge_float(seed)};
                const PsvQ d = {kSpecialF32[(i + 3 * j) % kSpecials], edge_float(seed), edge_float(seed), edge_float(seed)};
                set_q(v, 2, a);
                set_q(v, 4, b);
                set_q(v, 0, d);
                v.run(c.word);
                bool ok;
                nx::string got;
                if (c.kind <= 2) {
                    const PsvD r = c.kind == 1 ? c.acc(0, psv_qlo(d), psv_qlo(a), psv_qlo(b), 0) : c.bin(0, psv_qlo(a), psv_qlo(b), 0);
                    ok = psv_vdb(r) == v.ctx.vfp.d[0];
                    got = q_text(psv_qcat(r, psv_dvb(0)));
                } else {
                    const PsvQ r = c.un ? c.un(0, a, 0) : c.plain(a);
                    ok = same_q(r, q_of(v.ctx, 0));
                    got = q_text(r);
                }
                if (!ok && differ++ < 8)
                    std::printf("%s %s, %s: %s, the interpreter's %s\n", c.name, q_text(a).c_str(), q_text(b).c_str(), got.c_str(),
                                q_text(q_of(v.ctx, 0)).c_str());
            }
    CHECK(differ == 0u);
}

TEST_CASE("simd: the estimates on values are ARM's for every input that selects an estimate") {
    // The x64 formulas' integer estimates for all 256 inputs of each table; then VRECPE and VRSQRTE (single precision
    // and unsigned) for every exponent with every table index (random low bits), both signs, against the
    // interpreter, four lanes at a time, by the target's helper and by the formula.
    for (u32 q = 256; q < 512; ++q) {
        const psv_u32_q s = psv_nrecip_s_q((psv_u32_q){q, q, q, q});
        CHECK(s[0] == psv_recip_s(q));
    }
    for (u32 odd = 0; odd < 2; ++odd)
        for (u32 q = 128; q < 256; ++q) {
            const psv_u32_q s = psv_nrsqrt_s_q((psv_u32_q){q, q, q, q}, (psv_u32_q){0, 0, 0, 0} - odd);
            CHECK(s[0] == psv_rsqrt_s(2 * q + 1, odd ? 268435456u : 134217728u));
        }
    Neon v;
    u32 seed = 1234;
    usize differ = 0, runs = 0;
    struct Case {
        const char* name;
        u32 word;
        PsvQ (*f)(PsvQ);
        PsvQ (*x)(PsvQ);
    };
    const Case cases[] = {
        {"vrecpe.f32", 0xF3BB0542, [](PsvQ a) { return psv_nfrecpe_q(0, a, 0); }, psv_nfrecpe_x_q},
        {"vrsqrte.f32", 0xF3BB05C2, [](PsvQ a) { return psv_nfrsqrte_q(0, a, 0); }, psv_nfrsqrte_x_q},
        {"vrecpe.u32", 0xF3BB0442, psv_nurecpe_q, psv_nurecpe_x_q},
        {"vrsqrte.u32", 0xF3BB04C2, psv_nursqrte_q, psv_nursqrte_x_q},
    };
    for (const Case& c : cases)
        for (u32 e = 0; e < 256; ++e)
            for (u32 idx = 0; idx < 256; idx += 2) {
                PsvQ a;
                for (u32 l = 0; l < 4; ++l) {
                    const u32 index = idx + (l & 1), r = edge_float(seed);
                    a[l] = ((l & 2) ? 0x80000000u : 0u) | (e << 23) | (index << 15) | (r & 0x7FFFu);
                }
                set_q(v, 2, a);
                v.run(c.word);
                const PsvQ want = q_of(v.ctx, 0), got = c.f(a), formula = c.x(a);
                ++runs;
                if ((!same_q(got, want) || !same_q(formula, want)) && differ++ < 8)
                    std::printf("%s %s: %s (formula %s), the interpreter's %s\n", c.name, q_text(a).c_str(), q_text(got).c_str(),
                                q_text(formula).c_str(), q_text(want).c_str());
            }
    std::printf("estimates: %zu runs, %zu disagreements\n", runs, differ);
    CHECK(differ == 0u);
}

TEST_CASE("simd: a product at the smallest normal is flushed exactly when its exact value is below it") {
    // Products that round to 2^-126 from below (ARM: zero) and from above or exactly (ARM: 2^-126), in every lane,
    // through vmul and vmla.
    Neon v;
    usize differ = 0, edges = 0;
    u32 seed = 7;
    for (u32 t = 0; t < 20000; ++t) {
        PsvQ a, b;
        for (u32 l = 0; l < 4; ++l) {
            const float x = std::ldexp(1.0f + static_cast<float>(edge_float(seed) & 0xFFFFFF) / 16777216.0f, -63);
            const float y = std::ldexp(1.0f, -126) / x;
            u32 yb;
            std::memcpy(&yb, &y, 4);
            yb += static_cast<u32>(static_cast<int>(edge_float(seed) % 5) - 2);
            u32 xb;
            std::memcpy(&xb, &x, 4);
            a[l] = xb;
            b[l] = yb ^ (edge_float(seed) & 0x80000000u);
        }
        const PsvQ d = {0, 0x80000000u, 0x00800000u, 0x3F800000u};
        set_q(v, 2, a);
        set_q(v, 4, b);
        set_q(v, 0, d);
        v.run(0xF3020D54);  // vmul.f32 q0, q1, q2
        const PsvQ want = q_of(v.ctx, 0), got = psv_nfmul_q(0, a, b, 0);
        for (u32 l = 0; l < 4; ++l) edges += (want[l] & 0x7FFFFFFFu) == 0x00800000u || (want[l] & 0x7FFFFFFFu) == 0;
        if (!same_q(got, want) && differ++ < 8)
            std::printf("vmul.f32 %s, %s: %s, the interpreter's %s\n", q_text(a).c_str(), q_text(b).c_str(), q_text(got).c_str(), q_text(want).c_str());
        set_q(v, 0, d);
        v.run(0xF2020D54);  // vmla.f32 q0, q1, q2
        const PsvQ want2 = q_of(v.ctx, 0), got2 = psv_nfmla_q(0, d, a, b, 3);
        if (!same_q(got2, want2) && differ++ < 8)
            std::printf("vmla.f32 %s, %s: %s, the interpreter's %s\n", q_text(a).c_str(), q_text(b).c_str(), q_text(got2).c_str(), q_text(want2).c_str());
    }
    std::printf("products near the smallest normal: %zu edge lanes, %zu disagreements\n", edges, differ);
    CHECK(edges > 1000u);
    CHECK(differ == 0u);
}
