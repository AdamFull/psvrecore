// guest.h's typed floating-point operations (what generated code with the registers in typed locals calls) against
// the interpreter's integer soft float, instruction by instruction: every arithmetic operation, compare and conversion
// in both precisions, in each FPSCR mode the implementation has (flush-to-zero and default NaN on and off; round to
// nearest, the only rounding mode: a vmsr selecting another one traps, as it does in the interpreter), on directed
// special values (zeros, denormals, the smallest normals, infinities, quiet and signalling NaNs with payloads, values
// at rounding and conversion boundaries) in every pair, and on random values at the edges.
//
// Built into psvrecore_tests (the default operations: the host's with an exact fallback), and on arm64 with
// PSVRECORE_GENERATED_CODE_NATIVE_FLOAT into psvrecore_float_device_tests with PSV_NATIVE_FLOAT (the host's
// instructions under an FPCR that mirrors the FPSCR: psv_native_set_fpcr around each batch). PSV_FLOAT_CASES=<n>: n
// random cases per operation and mode (default 40000).
//
// Encodings: clang's integrated assembler (armv7a, cortex-a9, neon-fp16), ARM state.

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/interp/interpreter.hpp"
#include "psvrecore/recomp/guest.h"
#include "psvrecore/recomp/support.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

constexpr u32 kFz = PSV_FPSCR_FZ, kDn = PSV_FPSCR_DN;
constexpr u32 kModes[4] = {0, kFz, kDn, kFz | kDn};

// How an instruction's operands sit in the registers (the interpreter's side).
enum class Layout : u8 {
    SSS,    // s0 = op(s1, s2)
    SAcc,   // s0 = op(s0, s1, s2)
    SS,     // s0 = op(s1)
    DDD,    // d0 = op(d1, d2)
    DAcc,   // d0 = op(d0, d1, d2)
    DD,     // d0 = op(d1)
    CmpS,   // s0, s1 -> NZCV
    CmpS0,  // s0, #0 -> NZCV
    CmpD,   // d0, d1 -> NZCV
    CmpD0,  // d0, #0 -> NZCV
    DS2,    // d0 = op(s2)
    SD1,    // s0 = op(d1)
    SFix,   // s0 = op(s0)
    DFix,   // d0 = op(d0)
};

void mirror(u32 fpscr);

struct Case {
    u64 a = 0, b = 0, c = 0;
};

struct Op {
    const char* name;
    u32 word;
    Layout layout;
    u64 (*got)(u32 fpscr, const Case& k);
};

float f(u64 x) { return psv_f32(static_cast<u32>(x)); }
double d(u64 x) { return psv_f64(x); }
u64 b(float x) { return psv_b32(x); }
u64 b(double x) { return psv_b64(x); }

// The operations as generated code writes them (emit_vfp_locals.cpp).
const Op kOps[] = {
    {"vadd.f32", 0xEE300A81, Layout::SSS, [](u32 m, const Case& k) { return b(psv_vadd32(m, f(k.a), f(k.b))); }},
    {"vsub.f32", 0xEE300AC1, Layout::SSS, [](u32 m, const Case& k) { return b(psv_vsub32(m, f(k.a), f(k.b))); }},
    {"vmul.f32", 0xEE200A81, Layout::SSS, [](u32 m, const Case& k) { return b(psv_vmul32(m, f(k.a), f(k.b))); }},
    {"vdiv.f32", 0xEE800A81, Layout::SSS, [](u32 m, const Case& k) { return b(psv_vdiv32(m, f(k.a), f(k.b))); }},
    {"vnmul.f32", 0xEE200AC1, Layout::SSS, [](u32 m, const Case& k) { return b(psv_vnmul32(m, f(k.a), f(k.b))); }},
    {"vmla.f32", 0xEE000A81, Layout::SAcc,
     [](u32 m, const Case& k) { return b(psv_vadd32(m, f(k.c), psv_vmul32(m, f(k.a), f(k.b)))); }},
    {"vmls.f32", 0xEE000AC1, Layout::SAcc,
     [](u32 m, const Case& k) { return b(psv_vadd32(m, f(k.c), psv_vneg32(psv_vmul32(m, f(k.a), f(k.b))))); }},
    {"vnmla.f32", 0xEE100AC1, Layout::SAcc,
     [](u32 m, const Case& k) { return b(psv_vadd32(m, psv_vneg32(f(k.c)), psv_vneg32(psv_vmul32(m, f(k.a), f(k.b))))); }},
    {"vnmls.f32", 0xEE100A81, Layout::SAcc,
     [](u32 m, const Case& k) { return b(psv_vadd32(m, psv_vneg32(f(k.c)), psv_vmul32(m, f(k.a), f(k.b)))); }},
    {"vsqrt.f32", 0xEEB10AE0, Layout::SS, [](u32 m, const Case& k) { return b(psv_vsqrt32(m, f(k.a))); }},
    {"vabs.f32", 0xEEB00AE0, Layout::SS, [](u32, const Case& k) { return b(psv_vabs32(f(k.a))); }},
    {"vneg.f32", 0xEEB10A60, Layout::SS, [](u32, const Case& k) { return b(psv_vneg32(f(k.a))); }},
    {"vadd.f64", 0xEE310B02, Layout::DDD, [](u32 m, const Case& k) { return b(psv_vadd64(m, d(k.a), d(k.b))); }},
    {"vsub.f64", 0xEE310B42, Layout::DDD, [](u32 m, const Case& k) { return b(psv_vsub64(m, d(k.a), d(k.b))); }},
    {"vmul.f64", 0xEE210B02, Layout::DDD, [](u32 m, const Case& k) { return b(psv_vmul64(m, d(k.a), d(k.b))); }},
    {"vdiv.f64", 0xEE810B02, Layout::DDD, [](u32 m, const Case& k) { return b(psv_vdiv64(m, d(k.a), d(k.b))); }},
    {"vnmul.f64", 0xEE210B42, Layout::DDD, [](u32 m, const Case& k) { return b(psv_vnmul64(m, d(k.a), d(k.b))); }},
    {"vmla.f64", 0xEE010B02, Layout::DAcc,
     [](u32 m, const Case& k) { return b(psv_vadd64(m, d(k.c), psv_vmul64(m, d(k.a), d(k.b)))); }},
    {"vmls.f64", 0xEE010B42, Layout::DAcc,
     [](u32 m, const Case& k) { return b(psv_vadd64(m, d(k.c), psv_vneg64(psv_vmul64(m, d(k.a), d(k.b))))); }},
    {"vnmla.f64", 0xEE110B42, Layout::DAcc,
     [](u32 m, const Case& k) { return b(psv_vadd64(m, psv_vneg64(d(k.c)), psv_vneg64(psv_vmul64(m, d(k.a), d(k.b))))); }},
    {"vnmls.f64", 0xEE110B02, Layout::DAcc,
     [](u32 m, const Case& k) { return b(psv_vadd64(m, psv_vneg64(d(k.c)), psv_vmul64(m, d(k.a), d(k.b)))); }},
    {"vsqrt.f64", 0xEEB10BC1, Layout::DD, [](u32 m, const Case& k) { return b(psv_vsqrt64(m, d(k.a))); }},
    {"vabs.f64", 0xEEB00BC1, Layout::DD, [](u32, const Case& k) { return b(psv_vabs64(d(k.a))); }},
    {"vneg.f64", 0xEEB10B41, Layout::DD, [](u32, const Case& k) { return b(psv_vneg64(d(k.a))); }},
    // Compares: NZCV (bits 31-28), from psv_vcmp* and from the fused form (operands flushed, then compared).
    {"vcmp.f32", 0xEEB40A60, Layout::CmpS, [](u32 m, const Case& k) -> u64 { return psv_vcmp32(m, f(k.a), f(k.b)); }},
    {"vcmpe.f32", 0xEEB40AE0, Layout::CmpS,
     [](u32 m, const Case& k) -> u64 { return psv_vnzcv32(psv_vflush32(m, f(k.a)), psv_vflush32(m, f(k.b))); }},
    {"vcmp.f32 #0", 0xEEB50A40, Layout::CmpS0, [](u32 m, const Case& k) -> u64 { return psv_vcmp32(m, f(k.a), 0.0f); }},
    {"vcmp.f64", 0xEEB40B41, Layout::CmpD, [](u32 m, const Case& k) -> u64 { return psv_vcmp64(m, d(k.a), d(k.b)); }},
    {"vcmpe.f64", 0xEEB40BC1, Layout::CmpD,
     [](u32 m, const Case& k) -> u64 { return psv_vnzcv64(psv_vflush64(m, d(k.a)), psv_vflush64(m, d(k.b))); }},
    {"vcmp.f64 #0", 0xEEB50B40, Layout::CmpD0,
     [](u32 m, const Case& k) -> u64 { return psv_vnzcv64(psv_vflush64(m, d(k.a)), 0.0); }},
    // Conversions.
    {"vcvt.f64.f32", 0xEEB70AC1, Layout::DS2, [](u32 m, const Case& k) { return b(psv_vcvt_f64_f32(m, f(k.a))); }},
    {"vcvt.f32.f64", 0xEEB70BC1, Layout::SD1, [](u32 m, const Case& k) { return b(psv_vcvt_f32_f64(m, d(k.a))); }},
    {"vcvt.s32.f32", 0xEEBD0AE0, Layout::SS, [](u32 m, const Case& k) -> u64 { return psv_vcvt_s32_f32(m, f(k.a)); }},
    {"vcvtr.s32.f32", 0xEEBD0A60, Layout::SS, [](u32 m, const Case& k) -> u64 { return psv_vcvtr_s32_f32(m, f(k.a)); }},
    {"vcvt.u32.f32", 0xEEBC0AE0, Layout::SS, [](u32 m, const Case& k) -> u64 { return psv_vcvt_u32_f32(m, f(k.a)); }},
    {"vcvtr.u32.f32", 0xEEBC0A60, Layout::SS, [](u32 m, const Case& k) -> u64 { return psv_vcvtr_u32_f32(m, f(k.a)); }},
    {"vcvt.s32.f64", 0xEEBD0BC1, Layout::SD1, [](u32 m, const Case& k) -> u64 { return psv_vcvt_s32_f64(m, d(k.a)); }},
    {"vcvtr.s32.f64", 0xEEBD0B41, Layout::SD1, [](u32 m, const Case& k) -> u64 { return psv_vcvtr_s32_f64(m, d(k.a)); }},
    {"vcvt.u32.f64", 0xEEBC0BC1, Layout::SD1, [](u32 m, const Case& k) -> u64 { return psv_vcvt_u32_f64(m, d(k.a)); }},
    {"vcvtr.u32.f64", 0xEEBC0B41, Layout::SD1, [](u32 m, const Case& k) -> u64 { return psv_vcvtr_u32_f64(m, d(k.a)); }},
    {"vcvt.f32.s32", 0xEEB80AE0, Layout::SS, [](u32, const Case& k) { return b(psv_vcvt_f32_s32(static_cast<u32>(k.a))); }},
    {"vcvt.f32.u32", 0xEEB80A60, Layout::SS, [](u32, const Case& k) { return b(psv_vcvt_f32_u32(static_cast<u32>(k.a))); }},
    {"vcvt.f64.s32", 0xEEB80BC1, Layout::DS2, [](u32, const Case& k) { return b(psv_vcvt_f64_s32(static_cast<u32>(k.a))); }},
    {"vcvt.f64.u32", 0xEEB80B41, Layout::DS2, [](u32, const Case& k) { return b(psv_vcvt_f64_u32(static_cast<u32>(k.a))); }},
    // Fixed point (in place), 32 and 16 bits, both directions, both precisions.
    {"vcvt.s32.f32 #1", 0xEEBE0AEF, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 32, 1, 1); }},
    {"vcvt.s32.f32 #16", 0xEEBE0AC8, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 32, 16, 1); }},
    {"vcvt.s32.f32 #32", 0xEEBE0AC0, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 32, 32, 1); }},
    {"vcvt.u32.f32 #8", 0xEEBF0ACC, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 32, 8, 0); }},
    {"vcvt.s16.f32 #4", 0xEEBE0A46, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 16, 4, 1); }},
    {"vcvt.u16.f32 #16", 0xEEBF0A40, Layout::SFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f32(m, f(k.a), 16, 16, 0); }},
    {"vcvt.f32.s32 #16", 0xEEBA0AC8, Layout::SFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f32_fixed(m, static_cast<u32>(k.a), 32, 16, 1)); }},
    {"vcvt.f32.u32 #32", 0xEEBB0AC0, Layout::SFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f32_fixed(m, static_cast<u32>(k.a), 32, 32, 0)); }},
    {"vcvt.f32.s16 #1", 0xEEBA0A67, Layout::SFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f32_fixed(m, static_cast<u32>(k.a), 16, 1, 1)); }},
    {"vcvt.f32.u16 #16", 0xEEBB0A40, Layout::SFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f32_fixed(m, static_cast<u32>(k.a), 16, 16, 0)); }},
    {"vcvt.s32.f64 #20", 0xEEBE0BC6, Layout::DFix,
     [](u32 m, const Case& k) -> u64 { return static_cast<u64>(static_cast<i64>(static_cast<i32>(psv_vcvt_fixed_f64(m, d(k.a), 32, 20, 1)))); }},
    {"vcvt.u32.f64 #32", 0xEEBF0BC0, Layout::DFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f64(m, d(k.a), 32, 32, 0); }},
    {"vcvt.s16.f64 #3", 0xEEBE0B66, Layout::DFix,
     [](u32 m, const Case& k) -> u64 { return static_cast<u64>(static_cast<i64>(static_cast<i32>(psv_vcvt_fixed_f64(m, d(k.a), 16, 3, 1)))); }},
    {"vcvt.u16.f64 #16", 0xEEBF0B40, Layout::DFix, [](u32 m, const Case& k) -> u64 { return psv_vcvt_fixed_f64(m, d(k.a), 16, 16, 0); }},
    {"vcvt.f64.s32 #31", 0xEEBA0BE0, Layout::DFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f64_fixed(m, static_cast<u32>(k.a), 32, 31, 1)); }},
    {"vcvt.f64.u32 #1", 0xEEBB0BEF, Layout::DFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f64_fixed(m, static_cast<u32>(k.a), 32, 1, 0)); }},
    {"vcvt.f64.s16 #15", 0xEEBA0B60, Layout::DFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f64_fixed(m, static_cast<u32>(k.a), 16, 15, 1)); }},
    {"vcvt.f64.u16 #8", 0xEEBB0B44, Layout::DFix,
     [](u32 m, const Case& k) { return b(psv_vcvt_f64_fixed(m, static_cast<u32>(k.a), 16, 8, 0)); }},
};

bool is_double_input(const Op& op) {
    switch (op.layout) {
        case Layout::DDD: case Layout::DAcc: case Layout::DD: case Layout::CmpD: case Layout::CmpD0: case Layout::SD1: return true;
        case Layout::DFix: return std::strstr(op.name, ".f64 #") == nullptr || std::strncmp(op.name, "vcvt.f64", 8) != 0;
        default: return false;
    }
}
bool is_integer_input(const Op& op) {
    // (conversions from integers and fixed point)
    return std::strncmp(op.name, "vcvt.f32.s", 10) == 0 || std::strncmp(op.name, "vcvt.f32.u", 10) == 0 ||
           std::strncmp(op.name, "vcvt.f64.s", 10) == 0 || std::strncmp(op.name, "vcvt.f64.u", 10) == 0;
}

// Directed values: zeros, denormals, the smallest normals and just above, ones, the largest finite values,
// infinities, quiet and signalling NaNs with payloads, rounding and conversion boundaries.
constexpr u32 kSpecial32[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000,
    0x00800001, 0x00FFFFFF, 0x01000000, 0x3F800000, 0xBF800000, 0x3F800001, 0x3F7FFFFF, 0x7F7FFFFF, 0xFF7FFFFF,
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC00001, 0xFFC12345, 0x7F800001, 0xFF800001, 0x7FA00000,
    0xFFBFFFFF, 0x3F000000, 0xBF000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000, 0x3EFFFFFF, 0x3ECCCCCD,
    0xBECCCCCD, 0x4B000000, 0x4B7FFFFF, 0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001, 0x4F7FFFFF, 0x4F800000,
    0x33800000, 0x33000000, 0x1F800000, 0x5F800000, 0x20000000, 0x1FFFFFFF, 0x3F7FFFFF, 0x46FFFE00, 0x47000000,
    0xC7000100, 0x477FFF00, 0x47800000,
};
constexpr u64 kSpecial64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull,
    0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x8010000000000000ull,
    0x0010000000000001ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FF0000000000001ull,
    0x3FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
    0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF8000000000001ull,
    0xFFF8123456789ABCull, 0x7FF0000000000001ull, 0xFFF0000000000001ull, 0x7FF4000000000000ull,
    0x7FF0000020000000ull, 0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x3FF8000000000000ull,
    0xBFF8000000000000ull, 0x4004000000000000ull, 0xC004000000000000ull, 0x3FD999999999999Aull,
    0x3FF0000010000000ull, 0x3FF0000010000001ull, 0x3FF000000FFFFFFFull, 0x47EFFFFFE0000000ull,
    0x47EFFFFFDFFFFFFFull, 0x47F0000000000000ull, 0x3810000000000000ull, 0x380FFFFFF0000000ull,
    0x380FFFFFEFFFFFFFull, 0x36A0000000000000ull, 0x3690000000000000ull, 0x3690000000000001ull,
    0x41DFFFFFFFC00000ull, 0x41DFFFFFFFE00000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,
    0xC1E0000000200000ull, 0xC1E0000000100000ull, 0x41EFFFFFFFE00000ull, 0x41EFFFFFFFF00000ull,
    0x41F0000000000000ull, 0x2000000000000000ull, 0x1FFFFFFFFFFFFFFFull, 0x5FF0000000000000ull,
    0x1FF0000000000000ull, 0x40DFFFC000000000ull, 0x40E0000000000000ull, 0xC0E0002000000000ull,
};
constexpr u32 kSpecialInt[] = {
    0, 1, 2, 3, 0xFFFFFFFF, 0xFFFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x00FFFFFF, 0x01000000, 0x01000001,
    0x01000003, 0x7FFFFF80, 0x7FFFFFC0, 0xFFFF8000, 0x00007FFF, 0x00008000, 0x0000FFFF, 0x00010000, 0xFFFF7FFF,
    0x12345678, 0xFEDCBA98, 0x00000080, 0xFFFFFF80,
};

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

// A float at an edge or random (see float_tests.cpp's edge_float), and the same for doubles.
u32 edge32(Rng& rng) {
    const u32 r = rng.u32v(), sign = rng.u32v() & 0x80000000u, mant = rng.u32v() & 0x7FFFFFu;
    switch (r % 14) {
        case 0: return sign;
        case 1: return sign | mant;
        case 2: return sign | (1u << 23) | mant;
        case 3: return sign | (2u << 23) | mant;
        case 4: return sign | 0x7F800000u | ((r >> 8) & 1 ? mant : 0);
        case 5: return sign | ((60u + (r >> 4) % 8) << 23) | mant;
        case 6: return sign | ((190u + (r >> 4) % 8) << 23) | mant;
        case 7: return sign | (127u << 23) | (mant & 0x7Fu);
        case 8: return sign | ((150u + (r >> 4) % 10) << 23) | mant;  // ~2^23..2^32: conversion boundaries
        case 9: return sign | ((120u + (r >> 4) % 8) << 23) | (mant & 0x7FF000u);  // halves and quarters
        case 10: return kSpecial32[rng.u32v() % (sizeof kSpecial32 / sizeof kSpecial32[0])];
        default: return rng.u32v();
    }
}
u64 edge64(Rng& rng) {
    const u32 r = rng.u32v();
    const u64 sign = static_cast<u64>(rng.u32v() & 0x80000000u) << 32, mant = rng.next() & 0xFFFFFFFFFFFFFull;
    switch (r % 15) {
        case 0: return sign;
        case 1: return sign | mant;
        case 2: return sign | (1ull << 52) | mant;
        case 3: return sign | (2ull << 52) | mant;
        case 4: return sign | 0x7FF0000000000000ull | ((r >> 8) & 1 ? mant : 0);
        case 5: return sign | (static_cast<u64>(500 + (r >> 4) % 24) << 52) | mant;   // ~2^-512: products near the smallest normal
        case 6: return sign | (static_cast<u64>(1530 + (r >> 4) % 24) << 52) | mant;  // ~2^512: near overflow
        case 7: return sign | (1023ull << 52) | (mant & 0x7Full);
        case 8: return sign | (static_cast<u64>(1045 + (r >> 4) % 12) << 52) | mant;  // ~2^22..2^33: conversions
        case 9: return sign | (static_cast<u64>(895 + (r >> 4) % 6) << 52) | mant;  // ~2^-128: float denormals
        case 10: return sign | (static_cast<u64>(1150 + (r >> 4) % 4) << 52) | (mant & 0xFFFFFF0000000ull);  // float overflow
        case 11: return kSpecial64[rng.u32v() % (sizeof kSpecial64 / sizeof kSpecial64[0])];
        default: return rng.next();
    }
}
u32 edge_int(Rng& rng) {
    const u32 r = rng.u32v();
    switch (r % 6) {
        case 0: return kSpecialInt[rng.u32v() % (sizeof kSpecialInt / sizeof kSpecialInt[0])];
        case 1: return rng.u32v() & 0xFFFF;
        case 2: return rng.u32v() >> (rng.u32v() % 32);
        default: return rng.u32v();
    }
}

usize random_cases() {
    if (const char* n = std::getenv("PSV_FLOAT_CASES")) return static_cast<usize>(std::strtoul(n, nullptr, 0));
    return 40000;
}

// The cases for an operation: every pair of directed values (with a random third operand for the accumulating ones),
// then random edge values.
nx::vector<Case> cases_for(const Op& op, Rng& rng) {
    nx::vector<Case> out;
    const bool dbl = is_double_input(op), integer = is_integer_input(op);
    const auto one = [&]() -> u64 { return integer ? edge_int(rng) : dbl ? edge64(rng) : edge32(rng); };
    if (integer) {
        for (u32 v : kSpecialInt) out.push_back({v, 0, 0});
    } else if (dbl) {
        for (u64 x : kSpecial64)
            for (u64 y : kSpecial64) out.push_back({x, y, edge64(rng)});
    } else {
        for (u32 x : kSpecial32)
            for (u32 y : kSpecial32) out.push_back({x, y, edge32(rng)});
    }
    const usize n = random_cases();
    for (usize i = 0; i < n; ++i) out.push_back({one(), one(), one()});
    return out;
}

// The interpreter's result for one case.
struct Oracle {
    PsvContext ctx;
    interp::Interpreter it{ctx};
    Oracle() { std::memset(&ctx, 0, sizeof ctx); }
    u64 run(const arm::Instruction& in, Layout layout, u32 fpscr, const Case& k) {
        auto& v = ctx.vfp;
        ctx.fpscr = fpscr;
        switch (layout) {
            case Layout::SSS: v.s[1] = static_cast<u32>(k.a), v.s[2] = static_cast<u32>(k.b); break;
            case Layout::SAcc: v.s[0] = static_cast<u32>(k.c), v.s[1] = static_cast<u32>(k.a), v.s[2] = static_cast<u32>(k.b); break;
            case Layout::SS: v.s[1] = static_cast<u32>(k.a); break;
            case Layout::DDD: v.d[1] = k.a, v.d[2] = k.b; break;
            case Layout::DAcc: v.d[0] = k.c, v.d[1] = k.a, v.d[2] = k.b; break;
            case Layout::DD: v.d[1] = k.a; break;
            case Layout::CmpS: v.s[0] = static_cast<u32>(k.a), v.s[1] = static_cast<u32>(k.b); break;
            case Layout::CmpS0: v.s[0] = static_cast<u32>(k.a); break;
            case Layout::CmpD: v.d[0] = k.a, v.d[1] = k.b; break;
            case Layout::CmpD0: v.d[0] = k.a; break;
            case Layout::DS2: v.s[2] = static_cast<u32>(k.a); break;
            case Layout::SD1: v.d[1] = k.a; break;
            case Layout::SFix: v.s[0] = static_cast<u32>(k.a); break;
            case Layout::DFix: v.d[0] = k.a; break;
        }
        it.thumb = false;
        it.pc = 0x1000;
        if (it.execute(in) != interp::Result::Next) return ~0ull;
        switch (layout) {
            case Layout::CmpS: case Layout::CmpS0: case Layout::CmpD: case Layout::CmpD0: return ctx.fpscr & 0xF0000000u;
            case Layout::SSS: case Layout::SAcc: case Layout::SS: case Layout::SD1: case Layout::SFix: return v.s[0];
            default: return v.d[0];
        }
    }
};

// Runs every case of `op` in every mode: the typed operation in a batch under the mode (the FPCR mirrored with
// PSV_NATIVE_FLOAT), then the interpreter; returns the disagreements (printing the first few).
usize check_op(const Op& op, Rng& rng, usize& checked) {
    Oracle oracle;
    const arm::Instruction in = arm::decode_arm(op.word, 0x1000);
    const nx::vector<Case> cases = cases_for(op, rng);
    nx::vector<u64> got(cases.size());
    usize differ = 0;
    for (u32 mode : kModes) {
        mirror(mode);
        for (usize i = 0; i < cases.size(); ++i) got[i] = op.got(mode, cases[i]);
        mirror(0);
        for (usize i = 0; i < cases.size(); ++i) {
            const u64 want = oracle.run(in, op.layout, mode, cases[i]);
            ++checked;
            if (got[i] == want) continue;
            if (differ++ < 4)
                std::fprintf(stderr, "%s, FPSCR %08x: %016llx %016llx %016llx: %016llx, the interpreter's %016llx\n", op.name, mode,
                             static_cast<unsigned long long>(cases[i].a), static_cast<unsigned long long>(cases[i].b),
                             static_cast<unsigned long long>(cases[i].c), static_cast<unsigned long long>(got[i]),
                             static_cast<unsigned long long>(want));
        }
    }
    return differ;
}

// A condition after a fused compare, as the generator writes it (generator.cpp, fused_condition), and from NZCV.
template <class T>
bool fused(u32 cond, T a, T b) {
    switch (cond) {
        case 0: return a == b;                       // eq
        case 1: return !(a == b);                    // ne
        case 2: return !(a < b);                     // cs
        case 3: return a < b;                        // cc
        case 4: return a < b;                        // mi
        case 5: return !(a < b);                     // pl
        case 6: return __builtin_isunordered(a, b);  // vs
        case 7: return !__builtin_isunordered(a, b); // vc
        case 8: return !(a <= b);                    // hi
        case 9: return a <= b;                       // ls
        case 10: return a >= b;                      // ge
        case 11: return !(a >= b);                   // lt
        case 12: return a > b;                       // gt
        default: return !(a > b);                    // le
    }
}
bool holds(u32 cond, u32 nzcv) {
    const bool n = nzcv >> 31, z = (nzcv >> 30) & 1, c = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    switch (cond) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        default: return z || n != v;
    }
}

// The FPCR for a batch of typed operations: the mode's FZ and DN with native floating point (as the runtime's mirror
// sets them), else nothing (the default operations assume the host's default environment).
void mirror(u32 fpscr) {
#ifdef PSV_NATIVE_FLOAT
    psv_native_set_fpcr(fpscr);
#else
    (void)fpscr;
#endif
}

jmp_buf g_jump;
recomp::TrapInfo g_trap;
[[noreturn]] void on_trap(PsvContext*, const recomp::TrapInfo& t) {
    g_trap = t;
    std::longjmp(g_jump, 1);
}

}  // namespace

TEST_CASE("vfp typed: every operation, compare and conversion gives the interpreter's result in every FPSCR mode") {
#ifdef PSV_NATIVE_FLOAT
    std::printf("native floating point (the host's instructions under the FPCR mirror)\n");
#else
    std::printf("host operations with exact fallbacks\n");
#endif
    Rng rng{0x5EEDF00Dull};
    usize checked = 0, differ = 0;
    for (const Op& op : kOps) {
        const usize before = checked;
        const usize d = check_op(op, rng, checked);
        differ += d;
        if (d) std::printf("%s: %zu of %zu differ\n", op.name, d, checked - before);
    }
    std::printf("%zu operations, %zu cases, %zu disagreements\n", sizeof kOps / sizeof kOps[0], checked, differ);
    CHECK(differ == 0u);
    CHECK(checked > 1000000u);
}

TEST_CASE("vfp typed: conditions after a fused compare are those of the compare's NZCV") {
    Rng rng{0xC0FFEEull};
    Oracle oracle;
    const arm::Instruction cmp32 = arm::decode_arm(0xEEB40A60, 0x1000), cmp64 = arm::decode_arm(0xEEB40B41, 0x1000);
    usize differ = 0, checked = 0;
    const usize n = random_cases();
    for (u32 mode : kModes) {
        nx::vector<Case> cs;
        for (u32 x : kSpecial32)
            for (u32 y : kSpecial32) cs.push_back({x, y, 0});
        for (usize i = 0; i < n; ++i) cs.push_back({edge32(rng), edge32(rng), 0});
        nx::vector<u32> got(cs.size());
        mirror(mode);
        for (usize i = 0; i < cs.size(); ++i) {
            const float a = psv_vflush32(mode, f(cs[i].a)), b2 = psv_vflush32(mode, f(cs[i].b));
            u32 bits = 0;
            for (u32 c = 0; c < 14; ++c) bits |= static_cast<u32>(fused(c, a, b2)) << c;
            got[i] = bits;
        }
        mirror(0);
        for (usize i = 0; i < cs.size(); ++i) {
            const u32 nzcv = static_cast<u32>(oracle.run(cmp32, Layout::CmpS, mode, cs[i]));
            for (u32 c = 0; c < 14; ++c) {
                ++checked;
                if (((got[i] >> c) & 1) != static_cast<u32>(holds(c, nzcv)) && differ++ < 4)
                    std::fprintf(stderr, "f32 condition %u, FPSCR %08x: %08llx %08llx\n", c, mode, static_cast<unsigned long long>(cs[i].a),
                                 static_cast<unsigned long long>(cs[i].b));
            }
        }
        cs.clear();
        for (u64 x : kSpecial64)
            for (u64 y : kSpecial64) cs.push_back({x, y, 0});
        for (usize i = 0; i < n; ++i) cs.push_back({edge64(rng), edge64(rng), 0});
        got.assign(cs.size(), 0);
        mirror(mode);
        for (usize i = 0; i < cs.size(); ++i) {
            const double a = psv_vflush64(mode, d(cs[i].a)), b2 = psv_vflush64(mode, d(cs[i].b));
            u32 bits = 0;
            for (u32 c = 0; c < 14; ++c) bits |= static_cast<u32>(fused(c, a, b2)) << c;
            got[i] = bits;
        }
        mirror(0);
        for (usize i = 0; i < cs.size(); ++i) {
            const u32 nzcv = static_cast<u32>(oracle.run(cmp64, Layout::CmpD, mode, cs[i]));
            for (u32 c = 0; c < 14; ++c) {
                ++checked;
                if (((got[i] >> c) & 1) != static_cast<u32>(holds(c, nzcv)) && differ++ < 4)
                    std::fprintf(stderr, "f64 condition %u, FPSCR %08x: %016llx %016llx\n", c, mode, static_cast<unsigned long long>(cs[i].a),
                                 static_cast<unsigned long long>(cs[i].b));
            }
        }
    }
    std::printf("%zu conditions checked, %zu disagreements\n", checked, differ);
    CHECK(differ == 0u);
}

TEST_CASE("vfp typed: moves keep every bit pattern, signalling NaNs included") {
    const u32 nans[] = {0x7F800001, 0xFF800001, 0x7FBFFFFF, 0x7FA00000, 0xFFC12345, 0x00000001, 0x80000000};
    for (u32 lo : nans)
        for (u32 hi : nans) {
            volatile float vlo = psv_f32(lo), vhi = psv_f32(hi);  // (through memory)
            const double packed = psv_vpack64(vlo, vhi);
            CHECK(psv_b64(packed) == ((static_cast<u64>(hi) << 32) | lo));
            CHECK(psv_b32(psv_vlo32(packed)) == lo);
            CHECK(psv_b32(psv_vhi32(packed)) == hi);
            const double back = psv_vsethi64(psv_vsetlo64(psv_f64(0), psv_f32(lo)), psv_f32(hi));
            CHECK(psv_b64(back) == psv_b64(packed));
            float copy = vlo;
            CHECK(psv_b32(copy) == lo);
            CHECK(psv_b32(psv_vneg32(psv_vneg32(copy))) == lo);
            CHECK(psv_b32(psv_vabs32(copy)) == (lo & 0x7FFFFFFFu));
        }
    const u64 dnan[] = {0x7FF0000000000001ull, 0xFFF4000000000000ull, 0x7FF7FFFFFFFFFFFFull, 0x0000000000000001ull};
    for (u64 x : dnan) {
        volatile double v = psv_f64(x);
        const double copy = v;
        CHECK(psv_b64(copy) == x);
        CHECK(psv_b64(psv_vneg64(copy)) == (x ^ 0x8000000000000000ull));
    }
}

TEST_CASE("vfp typed: an FPSCR write selecting another rounding mode, short vectors or AHP traps, as in the interpreter") {
    const auto previous = recomp::set_trap_handler(on_trap);
    PsvContext ctx;
    std::memset(&ctx, 0, sizeof ctx);
    interp::Interpreter it(ctx);
    const arm::Instruction vmsr = arm::decode_arm(0xEEE10A10, 0x1000);  // vmsr fpscr, r0
    // RP, RM, RZ; LEN; STRIDE; AHP; and the settings that are implemented (FZ, DN, flags, QC, cumulative bits).
    const u32 values[] = {0x00400000, 0x00800000, 0x00C00000, 0x00010000, 0x00300000, 0x04000000, 0x00000000,
                          0x01000000, 0x02000000, 0x03000000, 0xF800009F, 0xFB00009F, 0x03C00000};
    for (u32 value : values) {
        const bool unsupported = (value & PSV_FPSCR_UNSUPPORTED) != 0;
        ctx.r[0] = value;
        ctx.fpscr = 0;
        it.thumb = false;
        it.pc = 0x1000;
        const bool interp_traps = it.execute(vmsr) == interp::Result::Trap;
        CHECK(interp_traps == unsupported);
        bool trapped = false;
        u32 written = 0;
        if (setjmp(g_jump) == 0) written = psv_vmsr(&ctx, value, 0x1000);
        else trapped = true;
        CHECK(trapped == unsupported);
        if (!trapped) CHECK(written == ctx.fpscr);
        if (trapped) CHECK(g_trap.kind == static_cast<u32>(PSV_TRAP_FPSCR));
#if defined(PSV_NATIVE_FLOAT)
        // (the FPCR follows FZ and DN)
        if (!trapped) {
            uint64_t fpcr;
            __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
            CHECK((fpcr & PSV_FPSCR_STANDARD) == (written & PSV_FPSCR_STANDARD));
        }
        mirror(0);
#endif
    }
    recomp::set_trap_handler(previous);
}
