// Floating point against hand-computed results, for both implementations: the interpreter's integer soft float
// (through the instruction) and guest.h's host-float helpers (which generated code calls). The expected values
// follow from the ARM ARM's FPRound and FPProcessNaN; the reasoning is in the comments.
//
// Encodings were produced by clang's integrated assembler (armv7a, cortex-a9, neon-fp16).

#include <cstring>

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/guest.h"

using namespace psvrecore;

namespace {

constexpr u32 kFz = PSV_FPSCR_FZ, kDn = PSV_FPSCR_DN;

struct Vfp {
    PsvContext ctx;
    interp::Interpreter it{ctx};
    Vfp() { std::memset(&ctx, 0, sizeof ctx); }
    void run(u32 word) {
        it.thumb = false;
        it.pc = 0x1000;
        REQUIRE(it.execute(arm::decode_arm(word, 0x1000)) == interp::Result::Next);
    }
    // s0 = op(s1, s2) through the interpreter
    u32 s3(u32 word, u32 a, u32 b, u32 fpscr) {
        ctx.fpscr = fpscr;
        ctx.vfp.s[1] = a;
        ctx.vfp.s[2] = b;
        run(word);
        return ctx.vfp.s[0];
    }
    u64 d3(u32 word, u64 a, u64 b, u32 fpscr) {
        ctx.fpscr = fpscr;
        ctx.vfp.d[1] = a;
        ctx.vfp.d[2] = b;
        run(word);
        return ctx.vfp.d[0];
    }
};

constexpr u32 kVaddF32 = 0xEE300A81, kVmulF32 = 0xEE200A81, kVsubF32 = 0xEE300AC1, kVdivF32 = 0xEE800A81;
constexpr u32 kVnmulF32 = 0xEE200AC1, kVmulF64 = 0xEE210B02, kVsqrtF64 = 0xEEB10BC1;

}  // namespace

TEST_CASE("float: round to nearest, ties to even") {
    Vfp v;
    // 1 + 2^-24 is halfway between 1 and the next float: the even one (1.0) wins.
    CHECK(v.s3(kVaddF32, 0x3F800000, 0x33800000, 0) == 0x3F800000u);
    CHECK(psv_fadd32(0, 0x3F800000, 0x33800000) == 0x3F800000u);
    // 1 + 3 * 2^-25 is three quarters of an ulp above 1: rounds up.
    CHECK(v.s3(kVaddF32, 0x3F800000, 0x33C00000, 0) == 0x3F800001u);
    CHECK(psv_fadd32(0, 0x3F800000, 0x33C00000) == 0x3F800001u);
    // 1 / 3
    CHECK(v.s3(kVdivF32, 0x3F800000, 0x40400000, 0) == 0x3EAAAAABu);
    CHECK(psv_fdiv32(0, 0x3F800000, 0x40400000) == 0x3EAAAAABu);
    // sqrt(2) in double precision
    v.ctx.vfp.d[1] = 0x4000000000000000ull;
    v.run(kVsqrtF64);
    CHECK(v.ctx.vfp.d[0] == 0x3FF6A09E667F3BCDull);
    CHECK(psv_fsqrt64(0, 0x4000000000000000ull) == 0x3FF6A09E667F3BCDull);
}

TEST_CASE("float: flush-to-zero detects tininess before rounding") {
    Vfp v;
    // (1 - 2^-24) * 2^-126 = 2^-126 - 2^-150: exactly halfway between the smallest normal and the largest denormal.
    // Without FZ it rounds to the even one, the smallest normal. With FZ the exact value is below the smallest normal,
    // so it is flushed, although rounding alone would have made it normal.
    CHECK(v.s3(kVmulF32, 0x3F7FFFFF, 0x00800000, 0) == 0x00800000u);
    CHECK(psv_fmul32(0, 0x3F7FFFFF, 0x00800000) == 0x00800000u);
    CHECK(v.s3(kVmulF32, 0x3F7FFFFF, 0x00800000, kFz) == 0u);
    CHECK(psv_fmul32(kFz, 0x3F7FFFFF, 0x00800000) == 0u);
    // The same in double precision: (1 - 2^-53) * 2^-1022.
    CHECK(v.d3(kVmulF64, 0x3FEFFFFFFFFFFFFFull, 0x0010000000000000ull, 0) == 0x0010000000000000ull);
    CHECK(psv_fmul64(0, 0x3FEFFFFFFFFFFFFFull, 0x0010000000000000ull) == 0x0010000000000000ull);
    CHECK(v.d3(kVmulF64, 0x3FEFFFFFFFFFFFFFull, 0x0010000000000000ull, kFz) == 0ull);
    CHECK(psv_fmul64(kFz, 0x3FEFFFFFFFFFFFFFull, 0x0010000000000000ull) == 0ull);
    // A denormal input is flushed (keeping its sign) before the operation: -denormal + 0 = -0 + +0 = +0.
    CHECK(v.s3(kVaddF32, 0x80400000, 0x00000000, kFz) == 0u);
    CHECK(v.s3(kVaddF32, 0x00400000, 0x00000000, 0) == 0x00400000u);
    CHECK(psv_fadd32(kFz, 0x80400000, 0) == 0u);
}

TEST_CASE("float: NaN propagation and the default NaN") {
    Vfp v;
    // A signalling NaN wins over a quiet one, even as the second operand, and is quietened.
    CHECK(v.s3(kVaddF32, 0x7FC00001, 0x7F800002, 0) == 0x7FC00002u);
    CHECK(psv_fadd32(0, 0x7FC00001, 0x7F800002) == 0x7FC00002u);
    // In DN mode any NaN result is the default NaN.
    CHECK(v.s3(kVaddF32, 0x7FC00001, 0x7F800002, kDn) == 0x7FC00000u);
    CHECK(psv_fadd32(kDn, 0x7FC00001, 0x7F800002) == 0x7FC00000u);
    // An invalid operation (inf - inf) gives the default NaN: positive, unlike x86's.
    CHECK(v.s3(kVsubF32, 0x7F800000, 0x7F800000, 0) == 0x7FC00000u);
    CHECK(psv_fsub32(0, 0x7F800000, 0x7F800000) == 0x7FC00000u);
    // vnmul negates after NaN processing: the default NaN comes out negative.
    CHECK(v.s3(kVnmulF32, 0x7F800001, 0x3F800000, kDn) == 0xFFC00000u);
    // vcmp with a NaN: unordered, NZCV = 0011.
    v.ctx.fpscr = 0;
    v.ctx.vfp.s[0] = 0x7FC00000;
    v.ctx.vfp.s[1] = 0x3F800000;
    v.run(0xEEB40A60);
    CHECK((v.ctx.fpscr >> 28) == 0x3u);
    CHECK(psv_fcmp32(0, 0x7FC00000, 0x3F800000) == 0x30000000u);
}

TEST_CASE("float: conversions") {
    Vfp v;
    // vcvt.s32.f32 rounds toward zero and saturates; vcvtr uses the FPSCR mode (nearest, ties to even).
    const auto cvt = [&](u32 word, u32 a) {
        v.ctx.vfp.s[1] = a;
        v.run(word);
        return v.ctx.vfp.s[0];
    };
    CHECK(cvt(0xEEBD0AE0, 0x4F32D05E) == 0x7FFFFFFFu);  // 3e9
    CHECK(cvt(0xEEBD0AE0, 0xBFC00000) == 0xFFFFFFFFu);  // -1.5 -> -1
    CHECK(cvt(0xEEBD0A60, 0x40200000) == 2u);           // 2.5 -> 2
    CHECK(cvt(0xEEBD0A60, 0x40600000) == 4u);           // 3.5 -> 4
    CHECK(cvt(0xEEBD0A60, 0xC0200000) == 0xFFFFFFFEu);  // -2.5 -> -2
    CHECK(psv_f32_to_int(0, 0x4F32D05E, 1, 0) == 0x7FFFFFFFu);
    CHECK(psv_f32_to_int(0, 0xBFC00000, 1, 0) == 0xFFFFFFFFu);
    CHECK(psv_f32_to_int(0, 0x40200000, 1, 1) == 2u);
    CHECK(psv_f32_to_int(0, 0x40600000, 1, 1) == 4u);
    // vcvt.f32.f64: 1 + 2^-24 (exact in double) is a tie; just above it rounds up.
    const auto narrow = [&](u64 a) {
        v.ctx.vfp.d[1] = a;
        v.run(0xEEB70BC1);
        return v.ctx.vfp.s[0];
    };
    CHECK(narrow(0x3FF0000010000000ull) == 0x3F800000u);
    CHECK(narrow(0x3FF0000010000001ull) == 0x3F800001u);
    CHECK(psv_f64_to_f32(0, 0x3FF0000010000000ull) == 0x3F800000u);
    CHECK(psv_f64_to_f32(0, 0x3FF0000010000001ull) == 0x3F800001u);
    // A signalling NaN keeps the top of its payload, quietened.
    CHECK(narrow(0x7FF0000020000000ull) == 0x7FC00001u);
    CHECK(psv_f64_to_f32(0, 0x7FF0000020000000ull) == 0x7FC00001u);
    // vcvtb.f16.f32 s0, s1: 65520 is the overflow threshold (half way above the largest half, 65504); 65519 rounds
    // down to 65504. The top half of s0 is kept.
    const auto to_half = [&](u32 a) {
        v.ctx.vfp.s[0] = 0xABCD0000;
        v.ctx.vfp.s[1] = a;
        v.run(0xEEB30A60);
        return v.ctx.vfp.s[0];
    };
    CHECK(to_half(0x477FF000) == 0xABCD7C00u);
    CHECK(to_half(0x477FEF00) == 0xABCD7BFFu);
    CHECK(psv_f32_to_f16(0, 0x477FF000) == 0x7C00u);
    CHECK(psv_f32_to_f16(0, 0x477FEF00) == 0x7BFFu);
    // The smallest half denormal (2^-24) and half of it (a tie to zero).
    CHECK(psv_f32_to_f16(0, 0x33800000) == 0x0001u);
    CHECK(psv_f32_to_f16(0, 0x33000000) == 0x0000u);
    CHECK(psv_f16_to_f32(0, 0x0001) == 0x33800000u);
}
