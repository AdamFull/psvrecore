#pragma once

// IEEE 754 arithmetic in integers, after the ARM ARM's pseudocode (A2.7: FPUnpack, FPRound, FPAdd, FPMul, ...).
// The interpreter's floating point: it uses no host floating point, so it checks guest.h's host-float helpers
// independently. All four rounding modes, flush-to-zero (tininess before rounding), default NaN and NaN propagation
// are implemented; exception flags are not.
//
// Formats: N = 16, 32 or 64 bits; values travel as their bits in a u64.

#include "core/foundation/core/foundation.h"

namespace psvrecore::interp::fp {

inline constexpr u32 kFz = 1u << 24, kDn = 1u << 25, kAhp = 1u << 26;
inline constexpr u32 kStandard = kFz | kDn;  // StandardFPSCRValue (Advanced SIMD), round to nearest
inline u32 rounding(u32 fpscr) { return (fpscr >> 22) & 3; }  // 0 RN, 1 RP, 2 RM, 3 RZ

u64 add(u64 a, u64 b, int n, u32 fpscr);
u64 sub(u64 a, u64 b, int n, u32 fpscr);
u64 mul(u64 a, u64 b, int n, u32 fpscr);
u64 div(u64 a, u64 b, int n, u32 fpscr);
u64 sqrt(u64 a, int n, u32 fpscr);
u64 neg(u64 a, int n);
u64 abs(u64 a, int n);
// FPCompare: the NZCV flags (bits 3-0: N Z C V).
u32 compare(u64 a, u64 b, int n, u32 fpscr);
// Between formats (FPConvert); `n` and `m` are the source and destination sizes.
u64 convert(u64 a, int n, int m, u32 fpscr);
// FPToFixed: `bits`-bit result (16 or 32), sign- or zero-extended to 64 bits. `round_zero`: toward zero, else the
// FPSCR mode.
u64 to_fixed(u64 a, int n, int bits, int fraction, bool is_unsigned, bool round_zero, u32 fpscr);
// FixedToFP: the low `bits` bits of x. `round_nearest`: to nearest, else the FPSCR mode.
u64 from_fixed(u64 x, int bits, int fraction, bool is_unsigned, int n, bool round_nearest, u32 fpscr);

// Advanced SIMD's estimates (VRECPE, VRSQRTE) and steps (VRECPS, VRSQRTS), single precision, standard FPSCR.
u32 recip_estimate(u32 a);
u32 rsqrt_estimate(u32 a);
u32 recip_step(u32 a, u32 b);
u32 rsqrt_step(u32 a, u32 b);
// Maximum, minimum (VMAX/VMIN.F32: zeros compare as equal, the first operand wins... see the .cpp).
u32 max(u32 a, u32 b);
u32 min(u32 a, u32 b);

}  // namespace psvrecore::interp::fp
