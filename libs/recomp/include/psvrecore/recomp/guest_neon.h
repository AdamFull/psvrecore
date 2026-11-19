/* Advanced SIMD (NEON) on register values: the helpers of generated functions that hold the D and Q registers in
 * locals (ARCHITECTURE.md, "Advanced SIMD"); included by guest_simd.h. guest_simd.h's helpers (psv_n3...) are the
 * context form, element by element, which psvrecomp --neon-in-context and --vfp-in-context still generate.
 *
 * A D register's value is a PsvD (8 bytes), a Q register's a PsvQ (16 bytes), little-endian lanes in register order
 * (lane 0 is the low bits of the low D register). Every helper is element-wise vector code in clang's vector
 * extensions (vector_size, __builtin_shufflevector, __builtin_convertvector, the __builtin_elementwise_* forms), which
 * clang compiles to NEON on arm64 and to SSE2 on x64 (the baseline; nothing here needs SSSE3 or later, though some
 * helpers use a later instruction when the target has it); no __int128, no loop over lanes (except the half-precision
 * conversions and VTBL without SSSE3: see each). Where AArch64 has the instruction with ARMv7's semantics
 * (URECPE, URSQRTE, CNT, CLS, CLZ, TBL/TBX, PMUL, and FRECPE/FRSQRTE on flushed operands) arm64 uses it.
 *
 * Each helper is named psv_n<operation>_<type>_<d|q>: type s8..u64 (integer elements, signed or not), f32, or the
 * element size (8...64) where signedness does not matter; d or q the register size. The long, wide and narrow forms
 * (D and D to Q, Q to D) carry the narrow type and no size. Saturating forms take `uint32_t* fpscr` and set its QC
 * when a lane saturates. Single-precision forms whose result depends on flush-to-zero or default NaN take the FPSCR
 * (`fpscr`) for the native build's mode test (below); Advanced SIMD always computes in the standard FPSCR mode.
 *
 * Floating point (single precision, the standard mode: flush-to-zero of inputs and of results tiny before rounding,
 * the default NaN for every NaN result, round to nearest):
 * - Inputs are flushed (psv_nf_flush: a denormal lane becomes a zero of its sign) and results fixed (psv_nf_fix: a
 *   NaN lane becomes the default NaN 0x7FC00000, a result below the smallest normal a zero of its sign). With no
 *   denormal operand, the host's IEEE operation in round to nearest gives ARM's result in every lane except two
 *   cases: a NaN (the host propagates an operand's payload or makes its own default NaN; ARM gives 0x7FC00000 in
 *   every case where IEEE gives a NaN), and a result whose exact value is below the smallest normal (ARM flushes it;
 *   the host gives a denormal or zero of the same sign, or rounds up to the smallest normal). The fix handles the
 *   first and the denormal/zero part of the second exactly. A sum or difference of two floats that is below the
 *   smallest normal is exact (a multiple of 2^-149 smaller than 2^-126), so it cannot round up to it. A product can:
 *   |r| == 2^-126 is ambiguous, and a vector with such a lane takes the exact path (psv_fmul32 per lane, out of line;
 *   no other case ever leaves the inline code). Everything else (NaN operands, infinities, invalid operations,
 *   overflow, denormal operands and results) is computed inline. This holds whatever the host's FZ/DN settings are
 *   (x64's MXCSR as the runtime leaves it, arm64's FPCR in either build), given round to nearest.
 * - VMLA/VMLS: the product rounded and fixed first, then the sum (two roundings: ARMv7 has no fused NEON forms).
 *   VRECPS/VRSQRTS: 2 - a*b and (3 - a*b)/2 with the product of an infinity and a zero taken as +0; a product below
 *   the smallest normal does not change them (2 - p rounds to 2).
 * - Compares, maximum and minimum: on flushed operands, NaNs unordered (compares false; max/min the default NaN);
 *   of two zeros max is +0 unless both are -0, min -0 unless both are +0.
 * - With PSV_NATIVE_FLOAT (arm64) the host's FPCR has the guest FPSCR's FZ and DN (guest.h): when the guest's FPSCR
 *   has both set (the standard mode), the arithmetic, compares, max/min, pairwise forms and estimates are the host's
 *   instruction alone (volatile assembly, as for VFP); otherwise the checked code above.
 * - Conversions: to integers toward zero, saturating, NaN to 0 (x64's CVTTPS2DQ answer 0x80000000 corrected;
 *   arm64's FCVTZS/FCVTZU are ARM's); from integers to nearest; fixed point by exact scaling. */
#ifndef PSVRECORE_RECOMP_GUEST_NEON_H
#define PSVRECORE_RECOMP_GUEST_NEON_H

#if (defined(__x86_64__) || defined(_M_X64)) && defined(__SSSE3__)
#include <tmmintrin.h>
#endif

/* ---- Types ------------------------------------------------------------------------------------------------------ */

#define PSV_NVEC(tag, T, n)                                         \
    typedef T psv_##tag##_d __attribute__((vector_size(8 * (n) / 8)));  \
    typedef T psv_##tag##_q __attribute__((vector_size(16 * (n) / 8))); \
    typedef T psv_##tag##_o __attribute__((vector_size(32 * (n) / 8)));
PSV_NVEC(s8, int8_t, 8)
PSV_NVEC(u8, uint8_t, 8)
PSV_NVEC(s16, int16_t, 8)
PSV_NVEC(u16, uint16_t, 8)
PSV_NVEC(s32, int32_t, 8)
PSV_NVEC(u32, uint32_t, 8)
PSV_NVEC(s64, int64_t, 8)
PSV_NVEC(u64, uint64_t, 8)
PSV_NVEC(f32, float, 8)
#undef PSV_NVEC
typedef psv_u32_d PsvD;
typedef psv_u32_q PsvQ;
typedef PsvD psv_v_d;
typedef PsvQ psv_v_q;
#define PSV_NV(W) psv_v_##W
#define PSV_NT(t, W) psv_##t##_##W
/* (an unaligned 8- or 16-byte guest access) */
typedef uint64_t psv_u64_un __attribute__((vector_size(8), aligned(1)));
typedef uint64_t psv_u64x2_un __attribute__((vector_size(16), aligned(1)));

/* Per element type: its size, signedness, the unsigned type of that size, the signed one, the type twice as wide. */
#define PSV_E_s8 8
#define PSV_E_u8 8
#define PSV_E_s16 16
#define PSV_E_u16 16
#define PSV_E_s32 32
#define PSV_E_u32 32
#define PSV_E_s64 64
#define PSV_E_u64 64
#define PSV_S_s8 1
#define PSV_S_u8 0
#define PSV_S_s16 1
#define PSV_S_u16 0
#define PSV_S_s32 1
#define PSV_S_u32 0
#define PSV_S_s64 1
#define PSV_S_u64 0
#define PSV_UT_s8 u8
#define PSV_UT_u8 u8
#define PSV_UT_s16 u16
#define PSV_UT_u16 u16
#define PSV_UT_s32 u32
#define PSV_UT_u32 u32
#define PSV_UT_s64 u64
#define PSV_UT_u64 u64
#define PSV_ST_s8 s8
#define PSV_ST_u8 s8
#define PSV_ST_s16 s16
#define PSV_ST_u16 s16
#define PSV_ST_s32 s32
#define PSV_ST_u32 s32
#define PSV_ST_s64 s64
#define PSV_ST_u64 s64
#define PSV_WT_s8 s16
#define PSV_WT_u8 u16
#define PSV_WT_s16 s32
#define PSV_WT_u16 u32
#define PSV_WT_s32 s64
#define PSV_WT_u32 u64
#define PSV_CAT_(a, b) a##b
#define PSV_CAT(a, b) PSV_CAT_(a, b)
#define PSV_E(T) PSV_CAT(PSV_E_, T)
#define PSV_S(T) PSV_CAT(PSV_S_, T)
#define PSV_UT(T) PSV_CAT(PSV_UT_, T)
#define PSV_ST(T) PSV_CAT(PSV_ST_, T)
#define PSV_WT(T) PSV_CAT(PSV_WT_, T)
#define PSV_NTY(T, W) PSV_CAT(PSV_CAT(psv_, T), PSV_CAT(_, W))
/* The largest and smallest value of an element type, as a 64-bit integer constant. */
#define PSV_MAX(T) (PSV_S(T) ? (int64_t)(((uint64_t)1 << (PSV_E(T) - 1)) - 1) : (int64_t)(PSV_E(T) == 64 ? ~0ull : (1ull << (PSV_E(T) & 63)) - 1))
#define PSV_MIN(T) (PSV_S(T) ? (int64_t)(-(int64_t)(((uint64_t)1 << (PSV_E(T) - 1)) - 1) - 1) : (int64_t)0)

/* A value in every lane of a vector type. */
#define PSV_SPLAT(VT, x) ((VT){0} + (__typeof__(((VT){0})[0]))(x))
/* A select by mask (all ones or zeros per lane, any integer vector type of the size). */
#define PSV_SEL(m, a, b) ((((a) & (__typeof__(a))(m))) | ((b) & ~(__typeof__(a))(m)))
/* Any lane of a mask set. */
#if defined(__aarch64__)
/* (UMAXV: one reduction, where the generic form takes two moves and an OR) */
PSV_INLINE int psv_nany_d(PsvD m) { return vmaxv_u32((uint32x2_t)m) != 0; }
PSV_INLINE int psv_nany_q(PsvQ m) { return vmaxvq_u32((uint32x4_t)m) != 0; }
#else
PSV_INLINE int psv_nany_d(PsvD m) { return ((psv_u64_d)m)[0] != 0; }
PSV_INLINE int psv_nany_q(PsvQ m) { return (((psv_u64_q)m)[0] | ((psv_u64_q)m)[1]) != 0; }
#endif
PSV_INLINE void psv_nqc_d(uint32_t* PSV_RESTRICT fpscr, PsvD m) { *fpscr |= psv_nany_d(m) ? PSV_FPSCR_QC : 0u; }
PSV_INLINE void psv_nqc_q(uint32_t* PSV_RESTRICT fpscr, PsvQ m) { *fpscr |= psv_nany_q(m) ? PSV_FPSCR_QC : 0u; }

/* ---- Moves between the views of a register -------------------------------------------------------------------- */

PSV_INLINE PsvD psv_dv(double d) { PsvD v; memcpy(&v, &d, 8); return v; }
PSV_INLINE double psv_vd(PsvD v) { double d; memcpy(&d, &v, 8); return d; }
PSV_INLINE PsvD psv_dvb(uint64_t x) { PsvD v; memcpy(&v, &x, 8); return v; }
PSV_INLINE uint64_t psv_vdb(PsvD v) { uint64_t x; memcpy(&x, &v, 8); return x; }
PSV_INLINE PsvD psv_dvf(float lo, float hi) { return (PsvD)(psv_f32_d){lo, hi}; }
PSV_INLINE float psv_vdf(PsvD v, int i) { return ((psv_f32_d)v)[i]; }
PSV_INLINE PsvQ psv_qcat(PsvD lo, PsvD hi) { return __builtin_shufflevector(lo, hi, 0, 1, 2, 3); }
PSV_INLINE PsvD psv_qlo(PsvQ q) { return __builtin_shufflevector(q, q, 0, 1); }
PSV_INLINE PsvD psv_qhi(PsvQ q) { return __builtin_shufflevector(q, q, 2, 3); }
PSV_INLINE PsvQ psv_qsetlo(PsvQ q, PsvD lo) { return __builtin_shufflevector(q, psv_qcat(lo, lo), 4, 5, 2, 3); }
PSV_INLINE PsvQ psv_qsethi(PsvQ q, PsvD hi) { return __builtin_shufflevector(q, psv_qcat(hi, hi), 0, 1, 4, 5); }
PSV_INLINE float psv_vqf(PsvQ q, int i) { return ((psv_f32_q)q)[i]; }
PSV_INLINE PsvQ psv_vqsetf(PsvQ q, int i, float f) {
    psv_f32_q v = (psv_f32_q)q;
    v[i] = f;
    return (PsvQ)v;
}
PSV_INLINE PsvQ psv_qconst(uint64_t lo, uint64_t hi) { return (PsvQ)(psv_u64_q){lo, hi}; }
PSV_INLINE PsvQ psv_ctx_qget(const PsvContext* PSV_RESTRICT ctx, uint32_t d) {
    PsvQ q;
    memcpy(&q, &ctx->vfp.d[d], 16);
    return q;
}
PSV_INLINE void psv_ctx_qput(PsvContext* PSV_RESTRICT ctx, uint32_t d, PsvQ q) { memcpy(&ctx->vfp.d[d], &q, 16); }

/* ---- Guest memory ------------------------------------------------------------------------------------------------
 * A register's 8 bytes (16 for two) in order at any alignment, elements of `ebytes`. The production form is one host
 * access of the whole size, volatile so that the compiler neither merges, splits nor removes it (as the relaxed
 * atomic scalar accesses): an element is single-copy atomic where the host's access keeps it so (an aligned 8-byte
 * part of it on x64 and arm64). With PSV_ADDRESS_MASK (tests) each element is folded on its own, as the interpreter
 * makes it. */
#ifdef PSV_ADDRESS_MASK
PSV_INLINE PsvD psv_mldd(uint8_t* mb, uint32_t a, uint32_t ebytes) { return psv_dvb(psv_mld64u(mb, a, ebytes)); }
PSV_INLINE void psv_mstd(uint8_t* mb, uint32_t a, PsvD v, uint32_t ebytes) { psv_mst64u(mb, a, psv_vdb(v), ebytes); }
PSV_INLINE PsvQ psv_mldq(uint8_t* mb, uint32_t a, uint32_t ebytes) { return psv_qcat(psv_mldd(mb, a, ebytes), psv_mldd(mb, a + 8, ebytes)); }
PSV_INLINE void psv_mstq(uint8_t* mb, uint32_t a, PsvQ v, uint32_t ebytes) {
    psv_mstd(mb, a, psv_qlo(v), ebytes);
    psv_mstd(mb, a + 8, psv_qhi(v), ebytes);
}
#else
PSV_INLINE PsvD psv_mldd(uint8_t* mb, uint32_t a, uint32_t ebytes) {
    (void)ebytes;
    return (PsvD)*(volatile psv_u64_un*)PSV_HOST_AT(mb, a);
}
PSV_INLINE void psv_mstd(uint8_t* mb, uint32_t a, PsvD v, uint32_t ebytes) {
    (void)ebytes;
    *(volatile psv_u64_un*)PSV_HOST_AT(mb, a) = (psv_u64_un)v;
}
PSV_INLINE PsvQ psv_mldq(uint8_t* mb, uint32_t a, uint32_t ebytes) {
    (void)ebytes;
    return (PsvQ)*(volatile psv_u64x2_un*)PSV_HOST_AT(mb, a);
}
PSV_INLINE void psv_mstq(uint8_t* mb, uint32_t a, PsvQ v, uint32_t ebytes) {
    (void)ebytes;
    *(volatile psv_u64x2_un*)PSV_HOST_AT(mb, a) = (psv_u64x2_un)v;
}
#endif

/* ---- Lanes, duplication ---------------------------------------------------------------------------------------- */

/* Lane i of a D register (as an unsigned value), and the register with lane i replaced. */
#define psv_nget_8(v, i) ((uint32_t)((psv_u8_d)(v))[i])
#define psv_nget_16(v, i) ((uint32_t)((psv_u16_d)(v))[i])
#define psv_nget_32(v, i) ((uint32_t)((psv_u32_d)(v))[i])
#define psv_nget_64(v, i) ((uint64_t)((psv_u64_d)(v))[i])
#define PSV_NSET(E, T)                                                  \
    PSV_INLINE PsvD psv_nset_##E(PsvD v, int i, T x) {                  \
        psv_u##E##_d u = (psv_u##E##_d)v;                               \
        u[i] = (uint##E##_t)x;                                          \
        return (PsvD)u;                                                 \
    }
PSV_NSET(8, uint32_t)
PSV_NSET(16, uint32_t)
PSV_NSET(32, uint32_t)
PSV_NSET(64, uint64_t)
#undef PSV_NSET
/* A value in every element. */
PSV_INLINE PsvD psv_ndup_8_d(uint32_t x) { return (PsvD)(psv_u8_d){(uint8_t)x, (uint8_t)x, (uint8_t)x, (uint8_t)x, (uint8_t)x, (uint8_t)x, (uint8_t)x, (uint8_t)x}; }
PSV_INLINE PsvQ psv_ndup_8_q(uint32_t x) { const PsvD d = psv_ndup_8_d(x); return psv_qcat(d, d); }
PSV_INLINE PsvD psv_ndup_16_d(uint32_t x) { return (PsvD)(psv_u16_d){(uint16_t)x, (uint16_t)x, (uint16_t)x, (uint16_t)x}; }
PSV_INLINE PsvQ psv_ndup_16_q(uint32_t x) { const PsvD d = psv_ndup_16_d(x); return psv_qcat(d, d); }
PSV_INLINE PsvD psv_ndup_32_d(uint32_t x) { return (PsvD){x, x}; }
PSV_INLINE PsvQ psv_ndup_32_q(uint32_t x) { return (PsvQ){x, x, x, x}; }
PSV_INLINE PsvD psv_ndup_64_d(uint64_t x) { return psv_dvb(x); }
PSV_INLINE PsvQ psv_ndup_64_q(uint64_t x) { return (PsvQ)(psv_u64_q){x, x}; }
/* Lane i (a constant) of a D register in every element (VDUP scalar, the by-scalar forms' operand). */
#define psv_ndupl_8_d(v, i) ((PsvD)__builtin_shufflevector((psv_u8_d)(v), (psv_u8_d)(v), i, i, i, i, i, i, i, i))
#define psv_ndupl_8_q(v, i) ((PsvQ)__builtin_shufflevector((psv_u8_d)(v), (psv_u8_d)(v), i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i))
#define psv_ndupl_16_d(v, i) ((PsvD)__builtin_shufflevector((psv_u16_d)(v), (psv_u16_d)(v), i, i, i, i))
#define psv_ndupl_16_q(v, i) ((PsvQ)__builtin_shufflevector((psv_u16_d)(v), (psv_u16_d)(v), i, i, i, i, i, i, i, i))
#define psv_ndupl_32_d(v, i) ((PsvD)__builtin_shufflevector((psv_u32_d)(v), (psv_u32_d)(v), i, i))
#define psv_ndupl_32_q(v, i) ((PsvQ)__builtin_shufflevector((psv_u32_d)(v), (psv_u32_d)(v), i, i, i, i))

/* ---- Bitwise --------------------------------------------------------------------------------------------------- */

#define PSV_NLOGIC(W)                                                                                            \
    PSV_INLINE PSV_NV(W) psv_nand_##W(PSV_NV(W) a, PSV_NV(W) b) { return a & b; }                                 \
    PSV_INLINE PSV_NV(W) psv_nbic_##W(PSV_NV(W) a, PSV_NV(W) b) { return a & ~b; }                                \
    PSV_INLINE PSV_NV(W) psv_norr_##W(PSV_NV(W) a, PSV_NV(W) b) { return a | b; }                                 \
    PSV_INLINE PSV_NV(W) psv_norn_##W(PSV_NV(W) a, PSV_NV(W) b) { return a | ~b; }                                \
    PSV_INLINE PSV_NV(W) psv_neor_##W(PSV_NV(W) a, PSV_NV(W) b) { return a ^ b; }                                 \
    /* VBSL: d selects n where set, m where clear; VBIT: n where m is set; VBIF: n where m is clear */            \
    PSV_INLINE PSV_NV(W) psv_nbsl_##W(PSV_NV(W) d, PSV_NV(W) n, PSV_NV(W) m) { return (d & n) | (~d & m); }       \
    PSV_INLINE PSV_NV(W) psv_nbit_##W(PSV_NV(W) d, PSV_NV(W) n, PSV_NV(W) m) { return (n & m) | (d & ~m); }       \
    PSV_INLINE PSV_NV(W) psv_nbif_##W(PSV_NV(W) d, PSV_NV(W) n, PSV_NV(W) m) { return (d & m) | (n & ~m); }       \
    PSV_INLINE PSV_NV(W) psv_nmvn_##W(PSV_NV(W) a) { return ~a; }
PSV_NLOGIC(d)
PSV_NLOGIC(q)
#undef PSV_NLOGIC

/* ---- Integer elements of one size -------------------------------------------------------------------------------
 * T the element type; arithmetic wraps (unsigned views), compares and shifts by T's signedness. */

#define PSV_NINT(W, T)                                                                                              \
    PSV_INLINE PSV_NV(W) psv_nadd_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(PSV_UT(T), W))a + (PSV_NTY(PSV_UT(T), W))b);                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nsub_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(PSV_UT(T), W))a - (PSV_NTY(PSV_UT(T), W))b);                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nmul_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(PSV_UT(T), W))a * (PSV_NTY(PSV_UT(T), W))b);                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nmla_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a, PSV_NV(W) b) {                              \
        return psv_nadd_##T##_##W(acc, psv_nmul_##T##_##W(a, b));                                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nmls_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a, PSV_NV(W) b) {                              \
        return psv_nsub_##T##_##W(acc, psv_nmul_##T##_##W(a, b));                                                   \
    }                                                                                                               \
    /* (x + y) >> 1 and kin without overflow: halves and the carry of the low bits */                               \
    PSV_INLINE PSV_NV(W) psv_nhadd_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                            \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a, y = (PSV_NTY(T, W))b;                                             \
        return (PSV_NV(W))((x >> 1) + (y >> 1) + (x & y & 1));                                                      \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nrhadd_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                           \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a, y = (PSV_NTY(T, W))b;                                             \
        return (PSV_NV(W))((x >> 1) + (y >> 1) + ((x | y) & 1));                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nhsub_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                            \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a, y = (PSV_NTY(T, W))b;                                             \
        return (PSV_NV(W))((PSV_NTY(PSV_UT(T), W))(x >> 1) - (PSV_NTY(PSV_UT(T), W))(y >> 1) -                       \
                           (PSV_NTY(PSV_UT(T), W))(~x & y & 1));                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqadd_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {              \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a, y = (PSV_NTY(T, W))b;                                             \
        const PSV_NTY(T, W) r = __builtin_elementwise_add_sat(x, y);                                                \
        psv_nqc_##W(fpscr, (PSV_NV(W))(r != (PSV_NTY(T, W))((PSV_NTY(PSV_UT(T), W))x + (PSV_NTY(PSV_UT(T), W))y)));  \
        return (PSV_NV(W))r;                                                                                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqsub_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {              \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a, y = (PSV_NTY(T, W))b;                                             \
        const PSV_NTY(T, W) r = __builtin_elementwise_sub_sat(x, y);                                                \
        psv_nqc_##W(fpscr, (PSV_NV(W))(r != (PSV_NTY(T, W))((PSV_NTY(PSV_UT(T), W))x - (PSV_NTY(PSV_UT(T), W))y)));  \
        return (PSV_NV(W))r;                                                                                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncgt_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(T, W))a > (PSV_NTY(T, W))b);                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncge_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(T, W))a >= (PSV_NTY(T, W))b);                                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nceq_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))((PSV_NTY(T, W))a == (PSV_NTY(T, W))b);                                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ntst_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))(((PSV_NTY(T, W))a & (PSV_NTY(T, W))b) != 0);                                             \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nmax_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))__builtin_elementwise_max((PSV_NTY(T, W))a, (PSV_NTY(T, W))b);                            \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nmin_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return (PSV_NV(W))__builtin_elementwise_min((PSV_NTY(T, W))a, (PSV_NTY(T, W))b);                            \
    }                                                                                                               \
    /* |x - y|: the larger less the smaller, which is exact in the element's bits */                               \
    PSV_INLINE PSV_NV(W) psv_nabd_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) {                                             \
        return psv_nsub_##T##_##W(psv_nmax_##T##_##W(a, b), psv_nmin_##T##_##W(a, b));                              \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_naba_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a, PSV_NV(W) b) {                              \
        return psv_nadd_##T##_##W(acc, psv_nabd_##T##_##W(a, b));                                                   \
    }                                                                                                               \
    /* Shifts by a register: each element of b's signed bottom byte; left when positive, else right (arithmetic   \
     * for signed elements), `round` adds the last bit shifted out, `sat` saturates left shifts (setting QC). The   \
     * shift counts are clamped before each host shift (no lane shifts by its size or more). */                     \
    PSV_INLINE PSV_NV(W) psv_nshl_core_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b, int round, int sat) { \
        typedef PSV_NTY(T, W) VT;                                                                                   \
        typedef PSV_NTY(PSV_ST(T), W) VS;                                                                           \
        typedef PSV_NTY(PSV_UT(T), W) VU;                                                                           \
        const VT x = (VT)a;                                                                                         \
        const VS sh8 = ((VS)((VU)b << (PSV_E(T) - 8))) >> (PSV_E(T) - 8);                                           \
        const VS zero = (VS){0}, top = zero + (PSV_E(T) - 1);                                                     \
        /* (a right shift by size + 1 or more is one by size + 1: so -sh fits the element) */                       \
        const VS sh = PSV_SEL(sh8 < -top - 2, -top - 2, sh8);                                                       \
        const VS left = sh >= 0, big = sh > top;                                                                    \
        const VS ls = PSV_SEL(left & ~big, sh, zero);                                                               \
        const VS rs = PSV_SEL(left, zero + 1, -sh); /* 1 ... 128 */                                                 \
        const VS rr = PSV_SEL(rs > top, top, rs), r1 = PSV_SEL(rs - 1 > top, top, rs - 1);                          \
        const VT sign = PSV_S(T) ? x >> (PSV_E(T) - 1) : (VT){0};                                                 \
        const VT l = (VT)((VU)x << (VU)ls);                                                                         \
        VT rv = PSV_SEL(rs > top, sign, x >> (VT)rr);                                                               \
        if (round) rv = (VT)((VU)rv + (VU)(PSV_SEL(rs - 1 > top, sign, x >> (VT)r1) & 1));                         \
        VT out = PSV_SEL(left, PSV_SEL(big, (VT){0}, l), rv);                                                     \
        if (sat) {                                                                                                  \
            const VS over = left & (PSV_SEL(big, (VS)(x != 0), (VS)((l >> (VT)ls) != x)));                           \
            const VT lim = PSV_S(T) ? PSV_SEL(x < 0, PSV_SPLAT(VT, PSV_MIN(T)), PSV_SPLAT(VT, PSV_MAX(T)))       \
                                    : PSV_SPLAT(VT, PSV_MAX(T));                                                   \
            out = PSV_SEL(over, lim, out);                                                                          \
            psv_nqc_##W(fpscr, (PSV_NV(W))over);                                                                    \
        }                                                                                                           \
        return (PSV_NV(W))out;                                                                                      \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nshl_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) { return psv_nshl_core_##T##_##W(0, a, b, 0, 0); } \
    PSV_INLINE PSV_NV(W) psv_nrshl_##T##_##W(PSV_NV(W) a, PSV_NV(W) b) { return psv_nshl_core_##T##_##W(0, a, b, 1, 0); } \
    PSV_INLINE PSV_NV(W) psv_nqshl_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {              \
        return psv_nshl_core_##T##_##W(fpscr, a, b, 0, 1);                                                         \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqrshl_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {             \
        return psv_nshl_core_##T##_##W(fpscr, a, b, 1, 1);                                                         \
    }                                                                                                               \
    /* Shifts by an immediate (a constant at every call). Right: 1 ... size; left: 0 ... size - 1. */                 \
    PSV_INLINE PSV_NV(W) psv_nshr_##T##_##W(PSV_NV(W) a, int n) {                                                   \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        if (n >= PSV_E(T)) return (PSV_NV(W))(PSV_S(T) ? x >> (PSV_E(T) - 1) : (PSV_NTY(T, W)){0});             \
        return (PSV_NV(W))(x >> n);                                                                                 \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nrshr_##T##_##W(PSV_NV(W) a, int n) {                                                  \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        return psv_nadd_##T##_##W(psv_nshr_##T##_##W(a, n), (PSV_NV(W))((x >> (n - 1)) & 1));                       \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nsra_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a, int n) {                                    \
        return psv_nadd_##T##_##W(acc, psv_nshr_##T##_##W(a, n));                                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nrsra_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a, int n) {                                   \
        return psv_nadd_##T##_##W(acc, psv_nrshr_##T##_##W(a, n));                                                  \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqshli_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, int n) {                   \
        typedef PSV_NTY(T, W) VT;                                                                                   \
        typedef PSV_NTY(PSV_UT(T), W) VU;                                                                           \
        const VT x = (VT)a, l = (VT)((VU)x << n);                                                                   \
        const VT over = (VT)((l >> n) != x);                                                                        \
        const VT lim = PSV_S(T) ? PSV_SEL(x < 0, PSV_SPLAT(VT, PSV_MIN(T)), PSV_SPLAT(VT, PSV_MAX(T)))           \
                                : PSV_SPLAT(VT, PSV_MAX(T));                                                       \
        psv_nqc_##W(fpscr, (PSV_NV(W))over);                                                                        \
        return (PSV_NV(W))PSV_SEL(over, lim, l);                                                                    \
    }
#define PSV_NINT_ALL(W) PSV_NINT(W, s8) PSV_NINT(W, u8) PSV_NINT(W, s16) PSV_NINT(W, u16) PSV_NINT(W, s32) PSV_NINT(W, u32) PSV_NINT(W, s64) PSV_NINT(W, u64)
PSV_NINT_ALL(d)
PSV_NINT_ALL(q)
#undef PSV_NINT_ALL
#undef PSV_NINT

/* Shifts and inserts, shifts left (size-only forms). */
#define PSV_NSIZE(W, E)                                                                                             \
    PSV_INLINE PSV_NV(W) psv_nshli_##E##_##W(PSV_NV(W) a, int n) {                                                  \
        return (PSV_NV(W))((PSV_NTY(u##E, W))a << n);                                                               \
    }                                                                                                               \
    /* VSLI: the bottom n bits of d stay; VSRI (n 1 ... size): the top n bits stay */                               \
    PSV_INLINE PSV_NV(W) psv_nsli_##E##_##W(PSV_NV(W) d, PSV_NV(W) a, int n) {                                      \
        const PSV_NTY(u##E, W) keep = PSV_SPLAT(PSV_NTY(u##E, W), ((uint64_t)1 << n) - 1);             \
        return (PSV_NV(W))(((PSV_NTY(u##E, W))d & keep) | ((PSV_NTY(u##E, W))a << n));                               \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nsri_##E##_##W(PSV_NV(W) d, PSV_NV(W) a, int n) {                                      \
        if (n >= E) return d;                                                                                       \
        const PSV_NTY(u##E, W) keep = PSV_SPLAT(PSV_NTY(u##E, W), ~((uint##E##_t)~(uint##E##_t)0 >> n)); \
        return (PSV_NV(W))(((PSV_NTY(u##E, W))d & keep) | ((PSV_NTY(u##E, W))a >> n));                               \
    }
PSV_NSIZE(d, 8)
PSV_NSIZE(q, 8)
PSV_NSIZE(d, 16)
PSV_NSIZE(q, 16)
PSV_NSIZE(d, 32)
PSV_NSIZE(q, 32)
PSV_NSIZE(d, 64)
PSV_NSIZE(q, 64)
#undef PSV_NSIZE

/* VQSHLU #n: signed elements to unsigned, saturated (a negative one is 0). */
#define PSV_NQSHLU(W, T)                                                                                            \
    PSV_INLINE PSV_NV(W) psv_nqshlu_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, int n) {                   \
        typedef PSV_NTY(T, W) VS;                                                                                   \
        typedef PSV_NTY(PSV_UT(T), W) VU;                                                                           \
        const VS x = (VS)a;                                                                                         \
        const VU u = (VU)x, l = u << n;                                                                             \
        const VS neg = x < 0, over = (VS)((l >> n) != u) & ~neg;                                                    \
        psv_nqc_##W(fpscr, (PSV_NV(W))(neg | over));                                                                \
        return (PSV_NV(W))PSV_SEL(neg, (VU){0}, PSV_SEL(over, ~((VU){0}), l));                                  \
    }
PSV_NQSHLU(d, s8)
PSV_NQSHLU(q, s8)
PSV_NQSHLU(d, s16)
PSV_NQSHLU(q, s16)
PSV_NQSHLU(d, s32)
PSV_NQSHLU(q, s32)
PSV_NQSHLU(d, s64)
PSV_NQSHLU(q, s64)
#undef PSV_NQSHLU

/* Pairwise (D registers only): adjacent elements of a, then of b. */
#define PSV_NPAIRWISE(T, EV, OD)                                                                                    \
    PSV_INLINE PsvD psv_npadd_##T##_d(PsvD a, PsvD b) {                                                             \
        const PSV_NTY(T, d) x = (PSV_NTY(T, d))a, y = (PSV_NTY(T, d))b;                                             \
        return psv_nadd_##T##_d((PsvD)__builtin_shufflevector(x, y, EV), (PsvD)__builtin_shufflevector(x, y, OD));   \
    }                                                                                                               \
    PSV_INLINE PsvD psv_npmax_##T##_d(PsvD a, PsvD b) {                                                             \
        const PSV_NTY(T, d) x = (PSV_NTY(T, d))a, y = (PSV_NTY(T, d))b;                                             \
        return psv_nmax_##T##_d((PsvD)__builtin_shufflevector(x, y, EV), (PsvD)__builtin_shufflevector(x, y, OD));   \
    }                                                                                                               \
    PSV_INLINE PsvD psv_npmin_##T##_d(PsvD a, PsvD b) {                                                             \
        const PSV_NTY(T, d) x = (PSV_NTY(T, d))a, y = (PSV_NTY(T, d))b;                                             \
        return psv_nmin_##T##_d((PsvD)__builtin_shufflevector(x, y, EV), (PsvD)__builtin_shufflevector(x, y, OD));   \
    }
#define PSV_EV8 0, 2, 4, 6, 8, 10, 12, 14
#define PSV_OD8 1, 3, 5, 7, 9, 11, 13, 15
#define PSV_EV16 0, 2, 4, 6
#define PSV_OD16 1, 3, 5, 7
#define PSV_EV32 0, 2
#define PSV_OD32 1, 3
PSV_NPAIRWISE(s8, PSV_EV8, PSV_OD8)
PSV_NPAIRWISE(u8, PSV_EV8, PSV_OD8)
PSV_NPAIRWISE(s16, PSV_EV16, PSV_OD16)
PSV_NPAIRWISE(u16, PSV_EV16, PSV_OD16)
PSV_NPAIRWISE(s32, PSV_EV32, PSV_OD32)
PSV_NPAIRWISE(u32, PSV_EV32, PSV_OD32)
#undef PSV_NPAIRWISE

/* Polynomial multiplication over GF(2) of 8-bit elements (VMUL.P8: the low 8 bits; VMULL.P8: 16-bit products). */
#if defined(__aarch64__)
PSV_INLINE PsvD psv_npmul_p8_d(PsvD a, PsvD b) { return (PsvD)vmul_p8((poly8x8_t)a, (poly8x8_t)b); }
PSV_INLINE PsvQ psv_npmul_p8_q(PsvQ a, PsvQ b) { return (PsvQ)vmulq_p8((poly8x16_t)a, (poly8x16_t)b); }
PSV_INLINE PsvQ psv_nmull_p8(PsvD a, PsvD b) { return (PsvQ)vmull_p8((poly8x8_t)a, (poly8x8_t)b); }
#else
PSV_INLINE PsvQ psv_nmull_p8(PsvD a, PsvD b) {
    const psv_u16_q x = __builtin_convertvector((psv_u8_d)a, psv_u16_q), y = __builtin_convertvector((psv_u8_d)b, psv_u16_q);
    psv_u16_q r = x ^ x;
    for (int i = 0; i < 8; ++i) r ^= (x << i) & (psv_u16_q)(((y >> i) & 1) != 0);  /* (bits, not lanes) */
    return (PsvQ)r;
}
PSV_INLINE PsvD psv_npmul_p8_d(PsvD a, PsvD b) { return (PsvD)__builtin_convertvector((psv_u16_q)psv_nmull_p8(a, b), psv_u8_d); }
PSV_INLINE PsvQ psv_npmul_p8_q(PsvQ a, PsvQ b) {
    return psv_qcat(psv_npmul_p8_d(psv_qlo(a), psv_qlo(b)), psv_npmul_p8_d(psv_qhi(a), psv_qhi(b)));
}
#endif

/* VQDMULH / VQRDMULH (s16, s32): the high half of the doubled product, rounded or not; only -2^(e-1) squared
 * saturates. */
#define PSV_NQDMULH(W, T, WV, E)                                                                                     \
    PSV_INLINE PSV_NV(W) psv_nqdmulh_core_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b, int round) { \
        typedef PSV_NTY(T, W) VS;                                                                                   \
        const VS x = (VS)a, y = (VS)b;                                                                              \
        const WV p = __builtin_convertvector(x, WV) * __builtin_convertvector(y, WV);                               \
        const WV h = (p + PSV_SPLAT(WV, round ? (int64_t)1 << (E - 2) : 0)) >> (E - 1);                    \
        const VS over = (x == PSV_SPLAT(VS, PSV_MIN(T))) & (y == x);                                             \
        psv_nqc_##W(fpscr, (PSV_NV(W))over);                                                                        \
        return (PSV_NV(W))PSV_SEL(over, PSV_SPLAT(VS, PSV_MAX(T)), __builtin_convertvector(h, VS));              \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqdmulh_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {            \
        return psv_nqdmulh_core_##T##_##W(fpscr, a, b, 0);                                                          \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqrdmulh_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a, PSV_NV(W) b) {           \
        return psv_nqdmulh_core_##T##_##W(fpscr, a, b, 1);                                                          \
    }
PSV_NQDMULH(d, s16, psv_s32_q, 16)
PSV_NQDMULH(d, s32, psv_s64_q, 32)
PSV_NQDMULH(q, s16, psv_s32_o, 16)
PSV_NQDMULH(q, s32, psv_s64_o, 32)
#undef PSV_NQDMULH

/* ---- Two registers, miscellaneous (integer) ------------------------------------------------------------------- */

#define PSV_NMISC(W, T)                                                                                             \
    PSV_INLINE PSV_NV(W) psv_nabs_##T##_##W(PSV_NV(W) a) {                                                          \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        return (PSV_NV(W))PSV_SEL(x < 0, (PSV_NTY(T, W))((PSV_NTY(PSV_UT(T), W)){0} - (PSV_NTY(PSV_UT(T), W))x), x);  \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nneg_##T##_##W(PSV_NV(W) a) {                                                          \
        return (PSV_NV(W))((PSV_NTY(PSV_UT(T), W)){0} - (PSV_NTY(PSV_UT(T), W))a);                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqabs_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a) {                           \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        const PSV_NTY(T, W) m = x == PSV_SPLAT(PSV_NTY(T, W), PSV_MIN(T));                             \
        psv_nqc_##W(fpscr, (PSV_NV(W))m);                                                                           \
        return (PSV_NV(W))PSV_SEL(m, PSV_SPLAT(PSV_NTY(T, W), PSV_MAX(T)), (PSV_NTY(T, W))psv_nabs_##T##_##W(a)); \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nqneg_##T##_##W(uint32_t* PSV_RESTRICT fpscr, PSV_NV(W) a) {                           \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        const PSV_NTY(T, W) m = x == PSV_SPLAT(PSV_NTY(T, W), PSV_MIN(T));                             \
        psv_nqc_##W(fpscr, (PSV_NV(W))m);                                                                           \
        return (PSV_NV(W))PSV_SEL(m, PSV_SPLAT(PSV_NTY(T, W), PSV_MAX(T)), (PSV_NTY(T, W))psv_nneg_##T##_##W(a)); \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncgt0_##T##_##W(PSV_NV(W) a) { return (PSV_NV(W))((PSV_NTY(T, W))a > 0); }             \
    PSV_INLINE PSV_NV(W) psv_ncge0_##T##_##W(PSV_NV(W) a) { return (PSV_NV(W))((PSV_NTY(T, W))a >= 0); }            \
    PSV_INLINE PSV_NV(W) psv_nceq0_##T##_##W(PSV_NV(W) a) { return (PSV_NV(W))((PSV_NTY(T, W))a == 0); }            \
    PSV_INLINE PSV_NV(W) psv_ncle0_##T##_##W(PSV_NV(W) a) { return (PSV_NV(W))((PSV_NTY(T, W))a <= 0); }            \
    PSV_INLINE PSV_NV(W) psv_nclt0_##T##_##W(PSV_NV(W) a) { return (PSV_NV(W))((PSV_NTY(T, W))a < 0); }
PSV_NMISC(d, s8)
PSV_NMISC(q, s8)
PSV_NMISC(d, s16)
PSV_NMISC(q, s16)
PSV_NMISC(d, s32)
PSV_NMISC(q, s32)
#undef PSV_NMISC

/* Counting: VCNT (bytes), VCLZ and VCLS (8, 16, 32). */
#if defined(__aarch64__)
PSV_INLINE PsvD psv_ncnt_8_d(PsvD a) { return (PsvD)vcnt_u8((uint8x8_t)a); }
PSV_INLINE PsvQ psv_ncnt_8_q(PsvQ a) { return (PsvQ)vcntq_u8((uint8x16_t)a); }
PSV_INLINE PsvD psv_nclz_8_d(PsvD a) { return (PsvD)vclz_u8((uint8x8_t)a); }
PSV_INLINE PsvQ psv_nclz_8_q(PsvQ a) { return (PsvQ)vclzq_u8((uint8x16_t)a); }
PSV_INLINE PsvD psv_nclz_16_d(PsvD a) { return (PsvD)vclz_u16((uint16x4_t)a); }
PSV_INLINE PsvQ psv_nclz_16_q(PsvQ a) { return (PsvQ)vclzq_u16((uint16x8_t)a); }
PSV_INLINE PsvD psv_nclz_32_d(PsvD a) { return (PsvD)vclz_u32((uint32x2_t)a); }
PSV_INLINE PsvQ psv_nclz_32_q(PsvQ a) { return (PsvQ)vclzq_u32((uint32x4_t)a); }
PSV_INLINE PsvD psv_ncls_8_d(PsvD a) { return (PsvD)vcls_s8((int8x8_t)a); }
PSV_INLINE PsvQ psv_ncls_8_q(PsvQ a) { return (PsvQ)vclsq_s8((int8x16_t)a); }
PSV_INLINE PsvD psv_ncls_16_d(PsvD a) { return (PsvD)vcls_s16((int16x4_t)a); }
PSV_INLINE PsvQ psv_ncls_16_q(PsvQ a) { return (PsvQ)vclsq_s16((int16x8_t)a); }
PSV_INLINE PsvD psv_ncls_32_d(PsvD a) { return (PsvD)vcls_s32((int32x2_t)a); }
PSV_INLINE PsvQ psv_ncls_32_q(PsvQ a) { return (PsvQ)vclsq_s32((int32x4_t)a); }
#else
/* (bit counts in SWAR form: a byte's population from its halves, wider elements from their bytes) */
#define PSV_NCOUNT(W)                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncnt_8_##W(PSV_NV(W) a) {                                                              \
        PSV_NTY(u8, W) v = (PSV_NTY(u8, W))a;                                                                       \
        v = v - ((v >> 1) & 0x55);                                                                                  \
        v = (v & 0x33) + ((v >> 2) & 0x33);                                                                         \
        return (PSV_NV(W))((v + (v >> 4)) & 0x0F);                                                                  \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nclz_8_##W(PSV_NV(W) a) {                                                              \
        PSV_NTY(u8, W) v = (PSV_NTY(u8, W))a;                                                                       \
        v |= v >> 1, v |= v >> 2, v |= v >> 4;                                                                      \
        return (PSV_NV(W))(PSV_SPLAT(PSV_NTY(u8, W), 8) - (PSV_NTY(u8, W))psv_ncnt_8_##W((PSV_NV(W))v));                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nclz_16_##W(PSV_NV(W) a) {                                                             \
        PSV_NTY(u16, W) v = (PSV_NTY(u16, W))a;                                                                     \
        v |= v >> 1, v |= v >> 2, v |= v >> 4, v |= v >> 8;                                                         \
        const PSV_NTY(u16, W) c = (PSV_NTY(u16, W))psv_ncnt_8_##W((PSV_NV(W))v);                                     \
        return (PSV_NV(W))(PSV_SPLAT(PSV_NTY(u16, W), 16) - ((c & 0xFF) + (c >> 8)));                                          \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nclz_32_##W(PSV_NV(W) a) {                                                             \
        PSV_NTY(u32, W) v = (PSV_NTY(u32, W))a;                                                                     \
        v |= v >> 1, v |= v >> 2, v |= v >> 4, v |= v >> 8, v |= v >> 16;                                          \
        PSV_NTY(u32, W) c = (PSV_NTY(u32, W))psv_ncnt_8_##W((PSV_NV(W))v);                                           \
        c = (c & 0x00FF00FFu) + ((c >> 8) & 0x00FF00FFu);                                                           \
        return (PSV_NV(W))(PSV_SPLAT(PSV_NTY(u32, W), 32) - ((c & 0xFFFFu) + (c >> 16)));                                      \
    }                                                                                                               \
    /* the sign bit's copies after it: the leading zeros of x ^ (x >> 1), less one */                               \
    PSV_INLINE PSV_NV(W) psv_ncls_8_##W(PSV_NV(W) a) {                                                              \
        const PSV_NTY(s8, W) x = (PSV_NTY(s8, W))a;                                                                 \
        return (PSV_NV(W))((PSV_NTY(u8, W))psv_nclz_8_##W((PSV_NV(W))(x ^ (x >> 1))) - 1);                          \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncls_16_##W(PSV_NV(W) a) {                                                             \
        const PSV_NTY(s16, W) x = (PSV_NTY(s16, W))a;                                                               \
        return (PSV_NV(W))((PSV_NTY(u16, W))psv_nclz_16_##W((PSV_NV(W))(x ^ (x >> 1))) - 1);                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncls_32_##W(PSV_NV(W) a) {                                                             \
        const PSV_NTY(s32, W) x = (PSV_NTY(s32, W))a;                                                               \
        return (PSV_NV(W))((PSV_NTY(u32, W))psv_nclz_32_##W((PSV_NV(W))(x ^ (x >> 1))) - 1);                        \
    }
PSV_NCOUNT(d)
PSV_NCOUNT(q)
#undef PSV_NCOUNT
#endif

/* VREV64/32/16: the elements reversed in each group (constant patterns). */
#define psv_nrev64_8_d(a) ((PsvD)__builtin_shufflevector((psv_u8_d)(a), (psv_u8_d)(a), 7, 6, 5, 4, 3, 2, 1, 0))
#define psv_nrev64_8_q(a) ((PsvQ)__builtin_shufflevector((psv_u8_q)(a), (psv_u8_q)(a), 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8))
#define psv_nrev64_16_d(a) ((PsvD)__builtin_shufflevector((psv_u16_d)(a), (psv_u16_d)(a), 3, 2, 1, 0))
#define psv_nrev64_16_q(a) ((PsvQ)__builtin_shufflevector((psv_u16_q)(a), (psv_u16_q)(a), 3, 2, 1, 0, 7, 6, 5, 4))
#define psv_nrev64_32_d(a) ((PsvD)__builtin_shufflevector((psv_u32_d)(a), (psv_u32_d)(a), 1, 0))
#define psv_nrev64_32_q(a) ((PsvQ)__builtin_shufflevector((psv_u32_q)(a), (psv_u32_q)(a), 1, 0, 3, 2))
#define psv_nrev32_8_d(a) ((PsvD)__builtin_shufflevector((psv_u8_d)(a), (psv_u8_d)(a), 3, 2, 1, 0, 7, 6, 5, 4))
#define psv_nrev32_8_q(a) ((PsvQ)__builtin_shufflevector((psv_u8_q)(a), (psv_u8_q)(a), 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12))
#define psv_nrev32_16_d(a) ((PsvD)__builtin_shufflevector((psv_u16_d)(a), (psv_u16_d)(a), 1, 0, 3, 2))
#define psv_nrev32_16_q(a) ((PsvQ)__builtin_shufflevector((psv_u16_q)(a), (psv_u16_q)(a), 1, 0, 3, 2, 5, 4, 7, 6))
#define psv_nrev16_8_d(a) ((PsvD)__builtin_shufflevector((psv_u8_d)(a), (psv_u8_d)(a), 1, 0, 3, 2, 5, 4, 7, 6))
#define psv_nrev16_8_q(a) ((PsvQ)__builtin_shufflevector((psv_u8_q)(a), (psv_u8_q)(a), 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14))

/* VEXT: bytes s... of the concatenation b:a (a the low part); s a constant. */
#define psv_next_d(a, b, s)                                                                                         \
    ((PsvD)__builtin_shufflevector((psv_u8_d)(a), (psv_u8_d)(b), (s) + 0, (s) + 1, (s) + 2, (s) + 3, (s) + 4, (s) + 5, (s) + 6, (s) + 7))
#define psv_next_q(a, b, s)                                                                                         \
    ((PsvQ)__builtin_shufflevector((psv_u8_q)(a), (psv_u8_q)(b), (s) + 0, (s) + 1, (s) + 2, (s) + 3, (s) + 4, (s) + 5, (s) + 6, \
                                   (s) + 7, (s) + 8, (s) + 9, (s) + 10, (s) + 11, (s) + 12, (s) + 13, (s) + 14, (s) + 15))

/* VZIP, VUZP, VTRN: the two results (to the first operand and to the second), constant patterns. */
#define PSV_NPERM(name, W, VT, P1, P2)                                                                              \
    PSV_INLINE PSV_NV(W) psv_n##name##_##W(PSV_NV(W) a, PSV_NV(W) b, PSV_NV(W)* second) {                           \
        *second = (PSV_NV(W))__builtin_shufflevector((VT)a, (VT)b, PSV_##P2);                                       \
        return (PSV_NV(W))__builtin_shufflevector((VT)a, (VT)b, PSV_##P1);                                          \
    }
#define PSV_ZIP8D_1 0, 8, 1, 9, 2, 10, 3, 11
#define PSV_ZIP8D_2 4, 12, 5, 13, 6, 14, 7, 15
#define PSV_ZIP8Q_1 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23
#define PSV_ZIP8Q_2 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31
#define PSV_ZIP16D_1 0, 4, 1, 5
#define PSV_ZIP16D_2 2, 6, 3, 7
#define PSV_ZIP16Q_1 0, 8, 1, 9, 2, 10, 3, 11
#define PSV_ZIP16Q_2 4, 12, 5, 13, 6, 14, 7, 15
#define PSV_ZIP32D_1 0, 2
#define PSV_ZIP32D_2 1, 3
#define PSV_ZIP32Q_1 0, 4, 1, 5
#define PSV_ZIP32Q_2 2, 6, 3, 7
#define PSV_UZP8D_1 0, 2, 4, 6, 8, 10, 12, 14
#define PSV_UZP8D_2 1, 3, 5, 7, 9, 11, 13, 15
#define PSV_UZP8Q_1 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30
#define PSV_UZP8Q_2 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31
#define PSV_UZP16D_1 0, 2, 4, 6
#define PSV_UZP16D_2 1, 3, 5, 7
#define PSV_UZP16Q_1 0, 2, 4, 6, 8, 10, 12, 14
#define PSV_UZP16Q_2 1, 3, 5, 7, 9, 11, 13, 15
#define PSV_UZP32D_1 0, 2
#define PSV_UZP32D_2 1, 3
#define PSV_UZP32Q_1 0, 2, 4, 6
#define PSV_UZP32Q_2 1, 3, 5, 7
/* (VTRN: d gets d0 m0 d2 m2 ..., m gets d1 m1 d3 m3 ...) */
#define PSV_TRN8D_1 0, 8, 2, 10, 4, 12, 6, 14
#define PSV_TRN8D_2 1, 9, 3, 11, 5, 13, 7, 15
#define PSV_TRN8Q_1 0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30
#define PSV_TRN8Q_2 1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31
#define PSV_TRN16D_1 0, 4, 2, 6
#define PSV_TRN16D_2 1, 5, 3, 7
#define PSV_TRN16Q_1 0, 8, 2, 10, 4, 12, 6, 14
#define PSV_TRN16Q_2 1, 9, 3, 11, 5, 13, 7, 15
#define PSV_TRN32D_1 0, 2
#define PSV_TRN32D_2 1, 3
#define PSV_TRN32Q_1 0, 4, 2, 6
#define PSV_TRN32Q_2 1, 5, 3, 7
PSV_NPERM(zip_8, d, psv_u8_d, ZIP8D_1, ZIP8D_2)
PSV_NPERM(zip_8, q, psv_u8_q, ZIP8Q_1, ZIP8Q_2)
PSV_NPERM(zip_16, d, psv_u16_d, ZIP16D_1, ZIP16D_2)
PSV_NPERM(zip_16, q, psv_u16_q, ZIP16Q_1, ZIP16Q_2)
PSV_NPERM(zip_32, d, psv_u32_d, ZIP32D_1, ZIP32D_2)
PSV_NPERM(zip_32, q, psv_u32_q, ZIP32Q_1, ZIP32Q_2)
PSV_NPERM(uzp_8, d, psv_u8_d, UZP8D_1, UZP8D_2)
PSV_NPERM(uzp_8, q, psv_u8_q, UZP8Q_1, UZP8Q_2)
PSV_NPERM(uzp_16, d, psv_u16_d, UZP16D_1, UZP16D_2)
PSV_NPERM(uzp_16, q, psv_u16_q, UZP16Q_1, UZP16Q_2)
PSV_NPERM(uzp_32, d, psv_u32_d, UZP32D_1, UZP32D_2)
PSV_NPERM(uzp_32, q, psv_u32_q, UZP32Q_1, UZP32Q_2)
PSV_NPERM(trn_8, d, psv_u8_d, TRN8D_1, TRN8D_2)
PSV_NPERM(trn_8, q, psv_u8_q, TRN8Q_1, TRN8Q_2)
PSV_NPERM(trn_16, d, psv_u16_d, TRN16D_1, TRN16D_2)
PSV_NPERM(trn_16, q, psv_u16_q, TRN16Q_1, TRN16Q_2)
PSV_NPERM(trn_32, d, psv_u32_d, TRN32D_1, TRN32D_2)
PSV_NPERM(trn_32, q, psv_u32_q, TRN32Q_1, TRN32Q_2)
#undef PSV_NPERM

/* VTBL/VTBX: each byte of idx indexes the table (`len` D registers: t0 the first two, t1 the next two, zeros past
 * them); an index past the table gives 0 (VTBL) or keeps the byte of old (VTBX). */
PSV_INLINE PsvD psv_ntblv(PsvQ t0, PsvQ t1, uint32_t len, PsvD idx, PsvD old, int extend) {
    const psv_u8_d i = (psv_u8_d)idx;
    const psv_u8_d in = (psv_u8_d)(i < (uint8_t)(8 * len));
#if defined(__aarch64__)
    const uint8x16x2_t t = {{(uint8x16_t)t0, (uint8x16_t)t1}};
    const psv_u8_d r = (psv_u8_d)vqtbl2_u8(t, (uint8x8_t)i);
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__SSSE3__)
    /* (PSHUFB: an index byte with bit 7 set gives 0; 16-31 select the second half) */
    const __m128i x = _mm_set_epi64x(0, (long long)psv_vdb(idx));
    const __m128i lo = _mm_shuffle_epi8((__m128i)t0, _mm_or_si128(x, _mm_cmpgt_epi8(x, _mm_set1_epi8(15))));
    const __m128i hi = _mm_shuffle_epi8((__m128i)t1, _mm_or_si128(_mm_sub_epi8(x, _mm_set1_epi8(16)),
                                                                  _mm_or_si128(_mm_cmplt_epi8(x, _mm_set1_epi8(16)), _mm_cmpgt_epi8(x, _mm_set1_epi8(31)))));
    psv_u8_d r;
    const uint64_t rv = (uint64_t)_mm_cvtsi128_si64(_mm_or_si128(lo, hi));
    memcpy(&r, &rv, 8);
#else
    /* (SSE2 has no byte shuffle by a vector: the eight bytes one by one; VTBL is rare in games' code) */
    uint8_t table[32];
    memcpy(table, &t0, 16);
    memcpy(table + 16, &t1, 16);
    psv_u8_d r;
    for (int k = 0; k < 8; ++k) r[k] = table[i[k] & 31];
#endif
    return (PsvD)PSV_SEL(in, r, extend ? (psv_u8_d)old : (psv_u8_d)(i ^ i));
}

/* ---- Long, wide and narrow --------------------------------------------------------------------------------------
 * T the narrow element type (s8...u32); D and D to Q, Q and D to Q, Q to D. */
#define PSV_NLONG(T, WT)                                                                                            \
    PSV_INLINE PsvQ psv_nwiden_##T(PsvD a) { return (PsvQ)__builtin_convertvector((PSV_NTY(T, d))a, PSV_NTY(WT, q)); } \
    PSV_INLINE PsvQ psv_naddl_##T(PsvD a, PsvD b) { return psv_nadd_##WT##_q(psv_nwiden_##T(a), psv_nwiden_##T(b)); } \
    PSV_INLINE PsvQ psv_nsubl_##T(PsvD a, PsvD b) { return psv_nsub_##WT##_q(psv_nwiden_##T(a), psv_nwiden_##T(b)); } \
    PSV_INLINE PsvQ psv_naddw_##T(PsvQ a, PsvD b) { return psv_nadd_##WT##_q(a, psv_nwiden_##T(b)); }               \
    PSV_INLINE PsvQ psv_nsubw_##T(PsvQ a, PsvD b) { return psv_nsub_##WT##_q(a, psv_nwiden_##T(b)); }               \
    PSV_INLINE PsvQ psv_nabdl_##T(PsvD a, PsvD b) { return psv_nwiden_##T##_u(psv_nabd_##T##_d(a, b)); }   \
    PSV_INLINE PsvQ psv_nabal_##T(PsvQ acc, PsvD a, PsvD b) { return psv_nadd_##WT##_q(acc, psv_nabdl_##T(a, b)); } \
    PSV_INLINE PsvQ psv_nmull_##T(PsvD a, PsvD b) { return psv_nmul_##WT##_q(psv_nwiden_##T(a), psv_nwiden_##T(b)); } \
    PSV_INLINE PsvQ psv_nmlal_##T(PsvQ acc, PsvD a, PsvD b) { return psv_nadd_##WT##_q(acc, psv_nmull_##T(a, b)); } \
    PSV_INLINE PsvQ psv_nmlsl_##T(PsvQ acc, PsvD a, PsvD b) { return psv_nsub_##WT##_q(acc, psv_nmull_##T(a, b)); } \
    /* VSHLL #n (n 0 ... size; VMOVL is n 0) */                                                                     \
    PSV_INLINE PsvQ psv_nshll_##T(PsvD a, int n) { return (PsvQ)((PSV_NTY(PSV_UT(WT), q))psv_nwiden_##T(a) << n); }
/* (VABDL's difference is unsigned whatever the elements were) */
PSV_INLINE PsvQ psv_nwiden_s8_u(PsvD a) { return (PsvQ)__builtin_convertvector((psv_u8_d)a, psv_u16_q); }
PSV_INLINE PsvQ psv_nwiden_u8_u(PsvD a) { return psv_nwiden_s8_u(a); }
PSV_INLINE PsvQ psv_nwiden_s16_u(PsvD a) { return (PsvQ)__builtin_convertvector((psv_u16_d)a, psv_u32_q); }
PSV_INLINE PsvQ psv_nwiden_u16_u(PsvD a) { return psv_nwiden_s16_u(a); }
PSV_INLINE PsvQ psv_nwiden_s32_u(PsvD a) { return (PsvQ)__builtin_convertvector((psv_u32_d)a, psv_u64_q); }
PSV_INLINE PsvQ psv_nwiden_u32_u(PsvD a) { return psv_nwiden_s32_u(a); }
PSV_NLONG(s8, s16)
PSV_NLONG(u8, u16)
PSV_NLONG(s16, s32)
PSV_NLONG(u16, u32)
PSV_NLONG(s32, s64)
PSV_NLONG(u32, u64)
#undef PSV_NLONG

/* VQDMULL, VQDMLAL, VQDMLSL (s16, s32): the doubled product saturated (only -2^(e-1) squared), then the saturating
 * sum or difference with the accumulator (QC from either). */
#define PSV_NQDL(T, WT)                                                                                             \
    PSV_INLINE PsvQ psv_nqdmull_##T(uint32_t* PSV_RESTRICT fpscr, PsvD a, PsvD b) {                                 \
        const PSV_NTY(WT, q) p = (PSV_NTY(WT, q))psv_nmull_##T(a, b);                                               \
        const PSV_NTY(WT, q) over = p == PSV_SPLAT(PSV_NTY(WT, q), (int64_t)1 << (2 * PSV_E(T) - 2)); \
        psv_nqc_q(fpscr, (PsvQ)over);                                                                               \
        return (PsvQ)PSV_SEL(over, PSV_SPLAT(PSV_NTY(WT, q), PSV_MAX(WT)), (PSV_NTY(WT, q))((PSV_NTY(PSV_UT(WT), q))p << 1)); \
    }                                                                                                               \
    PSV_INLINE PsvQ psv_nqdmlal_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ acc, PsvD a, PsvD b) {                       \
        return psv_nqadd_##WT##_q(fpscr, acc, psv_nqdmull_##T(fpscr, a, b));                                        \
    }                                                                                                               \
    PSV_INLINE PsvQ psv_nqdmlsl_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ acc, PsvD a, PsvD b) {                       \
        return psv_nqsub_##WT##_q(fpscr, acc, psv_nqdmull_##T(fpscr, a, b));                                        \
    }
PSV_NQDL(s16, s32)
PSV_NQDL(s32, s64)
#undef PSV_NQDL

/* Narrowing: T the wide element type (s16...u64), NT the narrow one. */
#define PSV_NNARROW(T, NT)                                                                                          \
    PSV_INLINE PsvD psv_nmovn_##T(PsvQ a) { return (PsvD)__builtin_convertvector((PSV_NTY(PSV_UT(T), q))a, PSV_NTY(PSV_UT(NT), d)); } \
    /* the wide values saturated to the narrow type of their signedness, or (un) signed to unsigned */              \
    PSV_INLINE PsvD psv_nsatn_##T(uint32_t* PSV_RESTRICT fpscr, PSV_NTY(T, q) x, int to_unsigned) {                 \
        const int64_t hi = to_unsigned ? (int64_t)PSV_MAX(PSV_UT(NT)) : (int64_t)PSV_MAX(NT);                       \
        const int64_t lo = to_unsigned ? 0 : (int64_t)PSV_MIN(NT);                                                  \
        PSV_NTY(T, q) r = x;                                                                                        \
        const PSV_NTY(T, q) above = (PSV_NTY(T, q))(x > PSV_SPLAT(PSV_NTY(T, q), hi));                              \
        const PSV_NTY(T, q) below = PSV_S(T) ? (PSV_NTY(T, q))(x < PSV_SPLAT(PSV_NTY(T, q), lo)) : (PSV_NTY(T, q))(x ^ x); \
        r = PSV_SEL(above, PSV_SPLAT(PSV_NTY(T, q), hi), r);                                          \
        r = PSV_SEL(below, PSV_SPLAT(PSV_NTY(T, q), lo), r);                                          \
        psv_nqc_q(fpscr, (PsvQ)(above | below));                                                                    \
        return psv_nmovn_##T((PsvQ)r);                                                                              \
    }                                                                                                               \
    PSV_INLINE PsvD psv_nqmovn_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a) { return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))a, !PSV_S(T)); } \
    PSV_INLINE PsvD psv_nshrn_##T(PsvQ a, int n) { return psv_nmovn_##T(psv_nshr_##T##_q(a, n)); }                  \
    PSV_INLINE PsvD psv_nrshrn_##T(PsvQ a, int n) { return psv_nmovn_##T(psv_nrshr_##T##_q(a, n)); }                \
    PSV_INLINE PsvD psv_nqshrn_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a, int n) {                                   \
        return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))psv_nshr_##T##_q(a, n), !PSV_S(T));                               \
    }                                                                                                               \
    PSV_INLINE PsvD psv_nqrshrn_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a, int n) {                                  \
        return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))psv_nrshr_##T##_q(a, n), !PSV_S(T));                              \
    }
PSV_NNARROW(s16, s8)
PSV_NNARROW(u16, u8)
PSV_NNARROW(s32, s16)
PSV_NNARROW(u32, u16)
PSV_NNARROW(s64, s32)
PSV_NNARROW(u64, u32)
#undef PSV_NNARROW
/* (signed wide to unsigned narrow: VQMOVUN, VQSHRUN, VQRSHRUN) */
#define PSV_NNARROWU(T)                                                                                             \
    PSV_INLINE PsvD psv_nqmovun_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a) { return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))a, 1); } \
    PSV_INLINE PsvD psv_nqshrun_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a, int n) {                                  \
        return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))psv_nshr_##T##_q(a, n), 1);                                      \
    }                                                                                                               \
    PSV_INLINE PsvD psv_nqrshrun_##T(uint32_t* PSV_RESTRICT fpscr, PsvQ a, int n) {                                 \
        return psv_nsatn_##T(fpscr, (PSV_NTY(T, q))psv_nrshr_##T##_q(a, n), 1);                                     \
    }
PSV_NNARROWU(s16)
PSV_NNARROWU(s32)
PSV_NNARROWU(s64)
#undef PSV_NNARROWU

/* VADDHN, VRADDHN, VSUBHN, VRSUBHN: the top halves of wide sums and differences (rounded: plus half the bottom's
 * unit). T the wide unsigned type. */
#define PSV_NHN(T, NT, H)                                                                                           \
    PSV_INLINE PsvD psv_naddhn_##T(PsvQ a, PsvQ b, int round) {                                                     \
        const PSV_NTY(T, q) s = (PSV_NTY(T, q))a + (PSV_NTY(T, q))b + PSV_SPLAT(PSV_NTY(T, q), round ? (uint64_t)1 << (H - 1) : 0); \
        return (PsvD)__builtin_convertvector(s >> H, PSV_NTY(NT, d));                                               \
    }                                                                                                               \
    PSV_INLINE PsvD psv_nsubhn_##T(PsvQ a, PsvQ b, int round) {                                                     \
        const PSV_NTY(T, q) s = (PSV_NTY(T, q))a - (PSV_NTY(T, q))b + PSV_SPLAT(PSV_NTY(T, q), round ? (uint64_t)1 << (H - 1) : 0); \
        return (PsvD)__builtin_convertvector(s >> H, PSV_NTY(NT, d));                                               \
    }
PSV_NHN(u16, u8, 8)
PSV_NHN(u32, u16, 16)
PSV_NHN(u64, u32, 32)
#undef PSV_NHN

/* VPADDL / VPADAL: adjacent elements summed into elements twice as wide (wrapping), added to the destination's. */
#define PSV_NPADDL(W, T, WT, EV, OD)                                                                                \
    PSV_INLINE PSV_NV(W) psv_npaddl_##T##_##W(PSV_NV(W) a) {                                                        \
        const PSV_NTY(T, W) x = (PSV_NTY(T, W))a;                                                                   \
        return psv_nadd_##WT##_##W((PSV_NV(W))__builtin_convertvector(__builtin_shufflevector(x, x, EV), PSV_NTY(WT, W)), \
                                   (PSV_NV(W))__builtin_convertvector(__builtin_shufflevector(x, x, OD), PSV_NTY(WT, W))); \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_npadal_##T##_##W(PSV_NV(W) acc, PSV_NV(W) a) { return psv_nadd_##WT##_##W(acc, psv_npaddl_##T##_##W(a)); }
#define PSV_E8D 0, 2, 4, 6
#define PSV_O8D 1, 3, 5, 7
#define PSV_E8Q 0, 2, 4, 6, 8, 10, 12, 14
#define PSV_O8Q 1, 3, 5, 7, 9, 11, 13, 15
#define PSV_E16D 0, 2
#define PSV_O16D 1, 3
#define PSV_E16Q 0, 2, 4, 6
#define PSV_O16Q 1, 3, 5, 7
#define PSV_E32D 0
#define PSV_O32D 1
#define PSV_E32Q 0, 2
#define PSV_O32Q 1, 3
PSV_NPADDL(d, s8, s16, PSV_E8D, PSV_O8D)
PSV_NPADDL(q, s8, s16, PSV_E8Q, PSV_O8Q)
PSV_NPADDL(d, u8, u16, PSV_E8D, PSV_O8D)
PSV_NPADDL(q, u8, u16, PSV_E8Q, PSV_O8Q)
PSV_NPADDL(d, s16, s32, PSV_E16D, PSV_O16D)
PSV_NPADDL(q, s16, s32, PSV_E16Q, PSV_O16Q)
PSV_NPADDL(d, u16, u32, PSV_E16D, PSV_O16D)
PSV_NPADDL(q, u16, u32, PSV_E16Q, PSV_O16Q)
PSV_NPADDL(q, s32, s64, PSV_E32Q, PSV_O32Q)
PSV_NPADDL(q, u32, u64, PSV_E32Q, PSV_O32Q)
#undef PSV_NPADDL
/* (one 64-bit element: a D register's two words) */
PSV_INLINE PsvD psv_npaddl_s32_d(PsvD a) { const psv_s32_d x = (psv_s32_d)a; return psv_dvb((uint64_t)((int64_t)x[0] + (int64_t)x[1])); }
PSV_INLINE PsvD psv_npaddl_u32_d(PsvD a) { const psv_u32_d x = (psv_u32_d)a; return psv_dvb((uint64_t)x[0] + x[1]); }
PSV_INLINE PsvD psv_npadal_s32_d(PsvD acc, PsvD a) { return psv_nadd_u64_d(acc, psv_npaddl_s32_d(a)); }
PSV_INLINE PsvD psv_npadal_u32_d(PsvD acc, PsvD a) { return psv_nadd_u64_d(acc, psv_npaddl_u32_d(a)); }

/* ---- Single precision (the standard mode; see the top of this file) --------------------------------------------- */

/* (PSV_NATIVE_FLOAT: the guest FPSCR's FZ and DN both set, so the host's FPCR has them: the host's instruction is
 * ARM's) */
#ifdef PSV_NATIVE_FLOAT
#define PSV_NF_NATIVE(fpscr) __builtin_expect(((fpscr) & PSV_FPSCR_STANDARD) == PSV_FPSCR_STANDARD, 0)
#define PSV_NF_LANES_d "2s"
#define PSV_NF_LANES_q "4s"
#define PSV_NF_ASM2(W, insn, a, b)                                                                                  \
    do {                                                                                                            \
        PSV_NV(W) r_;                                                                                               \
        __asm__ volatile(insn " %0." PSV_NF_LANES_##W ", %1." PSV_NF_LANES_##W ", %2." PSV_NF_LANES_##W           \
                         : "=w"(r_) : "w"(a), "w"(b));                                                              \
        return r_;                                                                                                  \
    } while (0)
#define PSV_NF_ASM1(W, insn, a)                                                                                     \
    do {                                                                                                            \
        PSV_NV(W) r_;                                                                                               \
        __asm__ volatile(insn " %0." PSV_NF_LANES_##W ", %1." PSV_NF_LANES_##W : "=w"(r_) : "w"(a));                \
        return r_;                                                                                                  \
    } while (0)
#define PSV_NF_ASM0(W, insn, a)                                                                                     \
    do {                                                                                                            \
        PSV_NV(W) r_;                                                                                               \
        __asm__ volatile(insn " %0." PSV_NF_LANES_##W ", %1." PSV_NF_LANES_##W ", #0.0" : "=w"(r_) : "w"(a));       \
        return r_;                                                                                                  \
    } while (0)
#define PSV_NF_IF_NATIVE(fpscr, stmt) if (PSV_NF_NATIVE(fpscr)) stmt
#else
#define PSV_NF_IF_NATIVE(fpscr, stmt) (void)(fpscr)
#endif

/* Operands the generated code knows to hold no denormal lane (a result of an operation below, which never gives one;
 * `known`: bit 0 the first operand, bit 1 the second, bit 2 the accumulator) are not flushed again. */
typedef double psv_f64_x2 __attribute__((vector_size(16)));
typedef double psv_f64_x4 __attribute__((vector_size(32)));
typedef int64_t psv_s64_x2 __attribute__((vector_size(16)));
typedef int64_t psv_s64_x4 __attribute__((vector_size(32)));
#define PSV_NF64_d psv_f64_x2
#define PSV_NF64_q psv_f64_x4
#define PSV_NS64_d psv_s64_x2
#define PSV_NS64_q psv_s64_x4

/* The bodies of the flush, the fix and the product's exactness test, per host. x64 (SSE2): integer compares of the
 * magnitude bits. arm64: FACGT against 2^-126 (true for zeros and denormals, false for NaNs) and FCMEQ for NaNs;
 * for a product at 2^-126 the fused residual p*2^48 - x*2^48*y (scaled: a nonzero one is at least 2^-124, so it
 * cannot underflow whatever the FPCR's FZ; |x| <= 1 there) has the sign of p minus the exact product. Elsewhere the
 * double-precision product (24 by 24 bits: exact). */
#if defined(__aarch64__)
/* (intrinsics: FACGT, BIC/BSL and FCMEQ + BIF, which clang does not pick for the generic forms) */
#define PSV_NF_FLUSH_BODY(W) return PSV_NF_FLUSH_##W(x);
#define PSV_NF_FLUSH_d(x) ((psv_u32_d)vbic_u32((uint32x2_t)(x), vand_u32(vcagt_f32(vdup_n_f32(0x1p-126f), (float32x2_t)(x)), vdup_n_u32(0x7FFFFFFFu))))
#define PSV_NF_FLUSH_q(x) ((psv_u32_q)vbicq_u32((uint32x4_t)(x), vandq_u32(vcagtq_f32(vdupq_n_f32(0x1p-126f), (float32x4_t)(x)), vdupq_n_u32(0x7FFFFFFFu))))
#define PSV_NF_FIX_BODY(W) return (PSV_NV(W))PSV_NF_FIX_##W(f);
#define PSV_NF_FIX_d(f)                                                                                             \
    vbsl_u32(vceq_f32((float32x2_t)(f), (float32x2_t)(f)), (uint32x2_t)PSV_NF_FLUSH_d(f), vdup_n_u32(0x7FC00000u))
#define PSV_NF_FIX_q(f)                                                                                             \
    vbslq_u32(vceqq_f32((float32x4_t)(f), (float32x4_t)(f)), (uint32x4_t)PSV_NF_FLUSH_q(f), vdupq_n_u32(0x7FC00000u))
#define PSV_NF_BELOW_BODY(W)                                                                                        \
    const PSV_NT(f32, W) xs = x * 0x1p48f, ps = p * 0x1p48f;                                                        \
    const PSV_NT(f32, W) d = (PSV_NT(f32, W))PSV_NF_FMS_##W(ps, xs, y); /* ps - xs * y, fused */                   \
    const PSV_NT(u32, W) b = (PSV_NT(u32, W))((d != 0.0f) & ((((PSV_NT(u32, W))d ^ (PSV_NT(u32, W))p) & 0x80000000u) == 0));
#define PSV_NF_FMS_d(a, b, c) vfms_f32((float32x2_t)(a), (float32x2_t)(b), (float32x2_t)(c))
#define PSV_NF_FMS_q(a, b, c) vfmsq_f32((float32x4_t)(a), (float32x4_t)(b), (float32x4_t)(c))
#else
#define PSV_NF_FLUSH_BODY(W) return x & ~((PSV_NT(u32, W))((x & 0x7F800000u) == 0) & 0x7FFFFFFFu);
#define PSV_NF_FIX_BODY(W)                                                                                          \
    PSV_NT(u32, W) r = (PSV_NT(u32, W))f;                                                                           \
    const PSV_NT(s32, W) m = (PSV_NT(s32, W))(r & 0x7FFFFFFFu);                                                     \
    r &= ~((PSV_NT(u32, W))(m < 0x00800000) & 0x7FFFFFFFu);                                                         \
    const PSV_NT(u32, W) nan = (PSV_NT(u32, W))(m > 0x7F800000);                                                    \
    return (PSV_NV(W))((r & ~nan) | (nan & 0x7FC00000u));
#define PSV_NF_BELOW_BODY(W)                                                                                        \
    const PSV_NF64_##W e = __builtin_convertvector(x, PSV_NF64_##W) * __builtin_convertvector(y, PSV_NF64_##W);     \
    const PSV_NS64_##W below = __builtin_elementwise_abs(e) < 0x1p-126;                                             \
    const PSV_NT(u32, W) b = (PSV_NT(u32, W))__builtin_convertvector(below, PSV_NT(s32, W));
#endif

#define PSV_NFLOAT(W)                                                                                               \
    /* a denormal lane flushed to a zero of its sign (arm64: |x| < 2^-126 by FACGT, zeros unchanged by it) */       \
    PSV_INLINE PSV_NT(u32, W) psv_nf_flush_##W(PSV_NV(W) a) {                                                       \
        const PSV_NT(u32, W) x = (PSV_NT(u32, W))a;                                                                 \
        PSV_NF_FLUSH_BODY(W)                                                                                        \
    }                                                                                                               \
    PSV_INLINE PSV_NT(f32, W) psv_nf_in_##W(PSV_NV(W) a, int known) {                                               \
        return known ? (PSV_NT(f32, W))a : (PSV_NT(f32, W))psv_nf_flush_##W(a);                                     \
    }                                                                                                               \
    /* a NaN lane the default NaN, a lane below the smallest normal a zero of its sign */                           \
    PSV_INLINE PSV_NV(W) psv_nf_fix_##W(PSV_NT(f32, W) f) {                                                         \
        PSV_NF_FIX_BODY(W)                                                                                          \
    }                                                                                                               \
    /* A product at the smallest normal may have an exact value below it (which ARM flushes): the exact products in \
     * double precision (24 by 24 bits: exact), and every lane whose exact product is below 2^-126 a zero of its    \
     * sign. Rare, and inline (no call: a call would cost the spills of every vector live across it). */            \
    PSV_INLINE PSV_NT(f32, W) psv_nf_product_##W(PSV_NT(f32, W) x, PSV_NT(f32, W) y) {                              \
        const PSV_NT(f32, W) p = x * y;                                                                             \
        const PSV_NT(u32, W) edge = (PSV_NT(u32, W))(((PSV_NT(u32, W))p & 0x7FFFFFFFu) == 0x00800000u);             \
        if (__builtin_expect(psv_nany_##W((PSV_NV(W))edge), 0)) {                                                   \
            PSV_NF_BELOW_BODY(W)                                                                                    \
            return (PSV_NT(f32, W))((PSV_NT(u32, W))p & ~(b & edge & 0x7FFFFFFFu));                                 \
        }                                                                                                           \
        return p;                                                                                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfadd_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fadd", a, b));                                                      \
        return psv_nf_fix_##W(psv_nf_in_##W(a, known & 1) + psv_nf_in_##W(b, known & 2));                           \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfsub_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fsub", a, b));                                                      \
        return psv_nf_fix_##W(psv_nf_in_##W(a, known & 1) - psv_nf_in_##W(b, known & 2));                           \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfmul_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fmul", a, b));                                                      \
        return psv_nf_fix_##W(psv_nf_product_##W(psv_nf_in_##W(a, known & 1), psv_nf_in_##W(b, known & 2)));        \
    }                                                                                                               \
    /* VMLA, VMLS: the product rounded and flushed (a NaN stays one: the sum's fix makes it the default NaN), then  \
     * the sum */                                                                                                   \
    PSV_INLINE PSV_NV(W) psv_nfmla_##W(uint32_t fpscr, PSV_NV(W) acc, PSV_NV(W) a, PSV_NV(W) b, int known) {        \
        PSV_NF_IF_NATIVE(fpscr, return psv_nfadd_##W(fpscr, acc, psv_nfmul_##W(fpscr, a, b, 0), 0));                \
        const PSV_NT(f32, W) p = psv_nf_product_##W(psv_nf_in_##W(a, known & 1), psv_nf_in_##W(b, known & 2));      \
        return psv_nf_fix_##W(psv_nf_in_##W(acc, known & 4) + psv_nf_in_##W((PSV_NV(W))p, 0));                      \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfmls_##W(uint32_t fpscr, PSV_NV(W) acc, PSV_NV(W) a, PSV_NV(W) b, int known) {        \
        PSV_NF_IF_NATIVE(fpscr, return psv_nfsub_##W(fpscr, acc, psv_nfmul_##W(fpscr, a, b, 0), 0));                \
        const PSV_NT(f32, W) p = psv_nf_product_##W(psv_nf_in_##W(a, known & 1), psv_nf_in_##W(b, known & 2));      \
        return psv_nf_fix_##W(psv_nf_in_##W(acc, known & 4) - psv_nf_in_##W((PSV_NV(W))p, 0));                      \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfabd_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fabd", a, b));                                                      \
        return psv_nf_fix_##W(psv_nf_in_##W(a, known & 1) - psv_nf_in_##W(b, known & 2)) & 0x7FFFFFFFu;             \
    }                                                                                                               \
    /* compares: ordered (a NaN compares false) on flushed operands */                                              \
    PSV_INLINE PSV_NV(W) psv_nfceq_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fcmeq", a, b));                                                     \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) == psv_nf_in_##W(b, known & 2));                             \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfcge_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fcmge", a, b));                                                     \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) >= psv_nf_in_##W(b, known & 2));                             \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfcgt_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fcmgt", a, b));                                                     \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) > psv_nf_in_##W(b, known & 2));                              \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfacge_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                      \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "facge", a, b));                                                     \
        return (PSV_NV(W))(psv_nf_in_##W(a & 0x7FFFFFFFu, known & 1) >= psv_nf_in_##W(b & 0x7FFFFFFFu, known & 2)); \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfacgt_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                      \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "facgt", a, b));                                                     \
        return (PSV_NV(W))(psv_nf_in_##W(a & 0x7FFFFFFFu, known & 1) > psv_nf_in_##W(b & 0x7FFFFFFFu, known & 2));  \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfcgt0_##W(uint32_t fpscr, PSV_NV(W) a, int known) {                                   \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM0(W, "fcmgt", a));                                                        \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) > 0.0f);                                                     \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfcge0_##W(uint32_t fpscr, PSV_NV(W) a, int known) {                                   \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM0(W, "fcmge", a));                                                        \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) >= 0.0f);                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfceq0_##W(uint32_t fpscr, PSV_NV(W) a, int known) {                                   \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM0(W, "fcmeq", a));                                                        \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) == 0.0f);                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfcle0_##W(uint32_t fpscr, PSV_NV(W) a, int known) {                                   \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM0(W, "fcmle", a));                                                        \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) <= 0.0f);                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfclt0_##W(uint32_t fpscr, PSV_NV(W) a, int known) {                                   \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM0(W, "fcmlt", a));                                                        \
        return (PSV_NV(W))(psv_nf_in_##W(a, known & 1) < 0.0f);                                                     \
    }                                                                                                               \
    /* FPMax / FPMin */                                                                                             \
    PSV_INLINE PSV_NV(W) psv_nfmaxmin_##W(PSV_NV(W) a, PSV_NV(W) b, int is_max, int known) {                        \
        const PSV_NT(f32, W) x = psv_nf_in_##W(a, known & 1), y = psv_nf_in_##W(b, known & 2);                      \
        const PSV_NT(u32, W) xb = (PSV_NT(u32, W))x, yb = (PSV_NT(u32, W))y;                                        \
        const PSV_NT(u32, W) pick = (PSV_NT(u32, W))(is_max ? x > y : x < y);                                       \
        PSV_NT(u32, W) r = PSV_SEL(pick, xb, yb);                                                                   \
        const PSV_NT(u32, W) zeros = (PSV_NT(u32, W))((x == 0.0f) & (y == 0.0f));                                   \
        r = PSV_SEL(zeros, is_max ? (xb & yb) : (xb | yb), r);                                                      \
        const PSV_NT(u32, W) nan = (PSV_NT(u32, W))((x != x) | (y != y));                                           \
        return (PSV_NV(W))((r & ~nan) | (nan & 0x7FC00000u));                                                       \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfmax_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fmax", a, b));                                                      \
        return psv_nfmaxmin_##W(a, b, 1, known);                                                                    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfmin_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                       \
        PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(W, "fmin", a, b));                                                      \
        return psv_nfmaxmin_##W(a, b, 0, known);                                                                    \
    }                                                                                                               \
    /* VRECPS / VRSQRTS: an infinity times a zero is +0; a tiny product changes nothing (2 - p rounds to 2) */       \
    PSV_INLINE PSV_NT(f32, W) psv_nf_step_##W(PSV_NV(W) a, PSV_NV(W) b, int known) {                                \
        const PSV_NT(f32, W) x = psv_nf_in_##W(a, known & 1), y = psv_nf_in_##W(b, known & 2), p = x * y;           \
        const PSV_NT(u32, W) inf0 = (PSV_NT(u32, W))((p != p) & (x == x) & (y == y));                               \
        return (PSV_NT(f32, W))((PSV_NT(u32, W))p & ~inf0);                                                         \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfrecps_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                     \
        (void)fpscr;                                                                                                \
        return psv_nf_fix_##W(2.0f - psv_nf_step_##W(a, b, known));                                                 \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfrsqrts_##W(uint32_t fpscr, PSV_NV(W) a, PSV_NV(W) b, int known) {                    \
        (void)fpscr;                                                                                                \
        /* (3 - p) rounded, then halved exactly (or to a denormal, which the fix flushes as ARM does) */            \
        return psv_nf_fix_##W((3.0f - psv_nf_step_##W(a, b, known)) * 0.5f);                                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfabs_##W(PSV_NV(W) a) { return a & 0x7FFFFFFFu; }                                     \
    PSV_INLINE PSV_NV(W) psv_nfneg_##W(PSV_NV(W) a) { return a ^ 0x80000000u; }
PSV_NFLOAT(d)
PSV_NFLOAT(q)
#undef PSV_NFLOAT
/* VPADD, VPMAX, VPMIN (D registers): adjacent lanes of a, then of b. */
PSV_INLINE PsvD psv_nfpadd_d(uint32_t fpscr, PsvD a, PsvD b, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(d, "faddp", a, b));
    const int k = (known & 1) && (known & 2) ? 3 : 0;
    return psv_nfadd_d(fpscr, __builtin_shufflevector(a, b, 0, 2), __builtin_shufflevector(a, b, 1, 3), k);
}
PSV_INLINE PsvD psv_nfpmax_d(uint32_t fpscr, PsvD a, PsvD b, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(d, "fmaxp", a, b));
    const int k = (known & 1) && (known & 2) ? 3 : 0;
    return psv_nfmax_d(fpscr, __builtin_shufflevector(a, b, 0, 2), __builtin_shufflevector(a, b, 1, 3), k);
}
PSV_INLINE PsvD psv_nfpmin_d(uint32_t fpscr, PsvD a, PsvD b, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM2(d, "fminp", a, b));
    const int k = (known & 1) && (known & 2) ? 3 : 0;
    return psv_nfmin_d(fpscr, __builtin_shufflevector(a, b, 0, 2), __builtin_shufflevector(a, b, 1, 3), k);
}

/* ---- Estimates -------------------------------------------------------------------------------------------------
 * ARM's 8-bit estimates (recip_estimate, recip_sqrt_estimate; guest_simd.h's psv_recip_s, psv_rsqrt_s). On arm64
 * FRECPE, FRSQRTE, URECPE and URSQRTE compute the same estimates (ARMv8's RecipEstimate and RecipSqrtEstimate are
 * defined to give ARMv7's); FRECPE/FRSQRTE on a flushed operand, their result fixed (with FZ clear in the FPCR a
 * reciprocal of |x| >= 2^126 is a denormal, which ARM flushes; NaNs the default NaN). On x64 the estimate is computed
 * in single precision: s = (2^19 + 2q + 1) / (4q + 2) for the reciprocal's 9-bit q, and (sqrt(2^27 or 2^28 / (2q+1))
 * + 1) / 2 for the square root's 8-bit q, rounded down; each is the integer formula's value for every one of its 256
 * inputs (tests/simd_tests.cpp checks all of them), and IEEE single precision gives the same result on every host. */
#define PSV_NEST(W)                                                                                                 \
    PSV_INLINE PSV_NT(u32, W) psv_nrecip_s_##W(PSV_NT(u32, W) q) {                                                  \
        const PSV_NT(f32, W) n = __builtin_convertvector((PSV_NT(s32, W))(2 * q + 524289u), PSV_NT(f32, W));          \
        const PSV_NT(f32, W) d = __builtin_convertvector((PSV_NT(s32, W))(4 * q + 2u), PSV_NT(f32, W));              \
        return (PSV_NT(u32, W))__builtin_convertvector(n / d, PSV_NT(s32, W));                                      \
    }                                                                                                               \
    PSV_INLINE PSV_NT(u32, W) psv_nrsqrt_s_##W(PSV_NT(u32, W) q, PSV_NT(u32, W) odd) {                              \
        const PSV_NT(f32, W) k = __builtin_convertvector((PSV_NT(s32, W))(2 * q + 1u), PSV_NT(f32, W));              \
        const PSV_NT(f32, W) l = (PSV_NT(f32, W))PSV_SEL(odd, (PSV_NT(u32, W)){0} + 0x4D800000u, (PSV_NT(u32, W)){0} + 0x4D000000u); \
        const PSV_NT(f32, W) t = __builtin_elementwise_sqrt(l / k);                                                 \
        PSV_NT(u32, W) s = (PSV_NT(u32, W))__builtin_convertvector((t + 1.0f) * 0.5f, PSV_NT(s32, W));              \
        return PSV_SEL((PSV_NT(u32, W))(s > 511u), (PSV_NT(u32, W)){0} + 511u, s);                                   \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nurecpe_x_##W(PSV_NV(W) a) {                                                           \
        const PSV_NT(u32, W) x = (PSV_NT(u32, W))a;                                                                 \
        const PSV_NT(u32, W) r = 0x80000000u | ((psv_nrecip_s_##W((x >> 23) & 0x1FF) - 256) << 23);                 \
        return (PSV_NV(W))PSV_SEL((PSV_NT(u32, W))((PSV_NT(s32, W))x < 0), r, (PSV_NT(u32, W)){0} + 0xFFFFFFFFu);    \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nursqrte_x_##W(PSV_NV(W) a) {                                                          \
        const PSV_NT(u32, W) x = (PSV_NT(u32, W))a;                                                                 \
        const PSV_NT(u32, W) top = (PSV_NT(u32, W))((PSV_NT(s32, W))x < 0);                                         \
        /* [0.5, 1): q = the top 8 bits, limit 2^27; [0.25, 0.5): q = the 8 bits after the top one, limit 2^28 */   \
        const PSV_NT(u32, W) q = PSV_SEL(top, (x >> 24) & 0xFF, (x >> 23) & 0xFF);                                  \
        const PSV_NT(u32, W) r = 0x80000000u | ((psv_nrsqrt_s_##W(q, ~top) - 256) << 23);                            \
        return (PSV_NV(W))PSV_SEL((PSV_NT(u32, W))(x >= 0x40000000u), r, (PSV_NT(u32, W)){0} + 0xFFFFFFFFu);         \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfrecpe_x_##W(PSV_NV(W) a) {                                                           \
        const PSV_NT(u32, W) x = psv_nf_flush_##W(a), sign = x & 0x80000000u, e = (x >> 23) & 0xFF;                 \
        const PSV_NT(u32, W) q = 256 | ((x >> 15) & 0xFF);                                                          \
        PSV_NT(u32, W) r = sign | ((253 - e) << 23) | ((psv_nrecip_s_##W(q) - 256) << 15);                          \
        const PSV_NT(u32, W) mag = x & 0x7FFFFFFFu;                                                                 \
        r = PSV_SEL((PSV_NT(u32, W))(e >= 253), sign, r);                     /* |x| >= 2^126 (and infinity): 0 */  \
        r = PSV_SEL((PSV_NT(u32, W))(mag == 0), sign | 0x7F800000u, r);       /* zero: infinity */                  \
        return (PSV_NV(W))PSV_SEL((PSV_NT(u32, W))(mag > 0x7F800000u), (PSV_NT(u32, W)){0} + 0x7FC00000u, r);        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_nfrsqrte_x_##W(PSV_NV(W) a) {                                                          \
        const PSV_NT(u32, W) x = psv_nf_flush_##W(a), e = (x >> 23) & 0xFF;                                         \
        const PSV_NT(u32, W) q = 128 | ((x >> 16) & 0x7F);                                                          \
        PSV_NT(u32, W) r = (((380 - e) / 2) << 23) | ((psv_nrsqrt_s_##W(q, (PSV_NT(u32, W))((e & 1) != 0)) - 256) << 15); \
        const PSV_NT(u32, W) mag = x & 0x7FFFFFFFu;                                                                 \
        r = PSV_SEL((PSV_NT(u32, W))(x == 0x7F800000u), (PSV_NT(u32, W)){0}, r);       /* +infinity: +0 */          \
        r = PSV_SEL((PSV_NT(u32, W))((PSV_NT(s32, W))x < 0), (PSV_NT(u32, W)){0} + 0x7FC00000u, r); /* negative */  \
        r = PSV_SEL((PSV_NT(u32, W))(mag == 0), (x & 0x80000000u) | 0x7F800000u, r);  /* zero: infinity */          \
        return (PSV_NV(W))PSV_SEL((PSV_NT(u32, W))(mag > 0x7F800000u), (PSV_NT(u32, W)){0} + 0x7FC00000u, r);        \
    }
PSV_NEST(d)
PSV_NEST(q)
#undef PSV_NEST
#if defined(__aarch64__)
PSV_INLINE PsvD psv_nurecpe_d(PsvD a) { return (PsvD)vrecpe_u32((uint32x2_t)a); }
PSV_INLINE PsvQ psv_nurecpe_q(PsvQ a) { return (PsvQ)vrecpeq_u32((uint32x4_t)a); }
PSV_INLINE PsvD psv_nursqrte_d(PsvD a) { return (PsvD)vrsqrte_u32((uint32x2_t)a); }
PSV_INLINE PsvQ psv_nursqrte_q(PsvQ a) { return (PsvQ)vrsqrteq_u32((uint32x4_t)a); }
PSV_INLINE PsvD psv_nfrecpe_d(uint32_t fpscr, PsvD a, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM1(d, "frecpe", a));
    return psv_nf_fix_d((psv_f32_d)vrecpe_f32((float32x2_t)psv_nf_in_d(a, known & 1)));
}
PSV_INLINE PsvQ psv_nfrecpe_q(uint32_t fpscr, PsvQ a, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM1(q, "frecpe", a));
    return psv_nf_fix_q((psv_f32_q)vrecpeq_f32((float32x4_t)psv_nf_in_q(a, known & 1)));
}
PSV_INLINE PsvD psv_nfrsqrte_d(uint32_t fpscr, PsvD a, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM1(d, "frsqrte", a));
    return psv_nf_fix_d((psv_f32_d)vrsqrte_f32((float32x2_t)psv_nf_in_d(a, known & 1)));
}
PSV_INLINE PsvQ psv_nfrsqrte_q(uint32_t fpscr, PsvQ a, int known) {
    PSV_NF_IF_NATIVE(fpscr, PSV_NF_ASM1(q, "frsqrte", a));
    return psv_nf_fix_q((psv_f32_q)vrsqrteq_f32((float32x4_t)psv_nf_in_q(a, known & 1)));
}
#else
PSV_INLINE PsvD psv_nurecpe_d(PsvD a) { return psv_nurecpe_x_d(a); }
PSV_INLINE PsvQ psv_nurecpe_q(PsvQ a) { return psv_nurecpe_x_q(a); }
PSV_INLINE PsvD psv_nursqrte_d(PsvD a) { return psv_nursqrte_x_d(a); }
PSV_INLINE PsvQ psv_nursqrte_q(PsvQ a) { return psv_nursqrte_x_q(a); }
PSV_INLINE PsvD psv_nfrecpe_d(uint32_t fpscr, PsvD a, int known) { (void)fpscr; (void)known; return psv_nfrecpe_x_d(a); }
PSV_INLINE PsvQ psv_nfrecpe_q(uint32_t fpscr, PsvQ a, int known) { (void)fpscr; (void)known; return psv_nfrecpe_x_q(a); }
PSV_INLINE PsvD psv_nfrsqrte_d(uint32_t fpscr, PsvD a, int known) { (void)fpscr; (void)known; return psv_nfrsqrte_x_d(a); }
PSV_INLINE PsvQ psv_nfrsqrte_q(uint32_t fpscr, PsvQ a, int known) { (void)fpscr; (void)known; return psv_nfrsqrte_x_q(a); }
#endif

/* ---- Conversions -------------------------------------------------------------------------------------------------
 * To 32-bit integers toward zero, saturating, NaN to 0 (a denormal gives 0 flushed or not); from them to nearest. */
#if defined(__aarch64__)
PSV_INLINE PsvD psv_ncvt_s32_f32_d(PsvD a) { return (PsvD)vcvt_s32_f32((float32x2_t)a); }
PSV_INLINE PsvQ psv_ncvt_s32_f32_q(PsvQ a) { return (PsvQ)vcvtq_s32_f32((float32x4_t)a); }
PSV_INLINE PsvD psv_ncvt_u32_f32_d(PsvD a) { return (PsvD)vcvt_u32_f32((float32x2_t)a); }
PSV_INLINE PsvQ psv_ncvt_u32_f32_q(PsvQ a) { return (PsvQ)vcvtq_u32_f32((float32x4_t)a); }
#elif defined(__x86_64__) || defined(_M_X64)
/* CVTTPS2DQ gives 0x80000000 for NaNs and values out of range: 2^31 and above become 0x7FFFFFFF, NaNs 0. Unsigned:
 * values from 2^31 converted less 2^31 (exact), 2^32 and above all ones, -1 and below (and NaNs) 0. */
PSV_INLINE PsvQ psv_ncvt_s32_f32_q(PsvQ a) {
    const __m128 x = (__m128)a;
    __m128i r = _mm_cvttps_epi32(x);
    r = _mm_xor_si128(r, _mm_castps_si128(_mm_cmple_ps(_mm_set1_ps(2147483648.0f), x)));
    return (PsvQ)_mm_and_si128(r, _mm_castps_si128(_mm_cmpord_ps(x, x)));
}
PSV_INLINE PsvQ psv_ncvt_u32_f32_q(PsvQ a) {
    const __m128 x = (__m128)a;
    const __m128 big = _mm_cmple_ps(_mm_set1_ps(2147483648.0f), x);
    __m128i r = _mm_cvttps_epi32(_mm_sub_ps(x, _mm_and_ps(big, _mm_set1_ps(2147483648.0f))));
    r = _mm_or_si128(r, _mm_and_si128(_mm_castps_si128(big), _mm_set1_epi32((int)0x80000000u)));
    r = _mm_or_si128(r, _mm_castps_si128(_mm_cmple_ps(_mm_set1_ps(4294967296.0f), x)));
    return (PsvQ)_mm_and_si128(r, _mm_castps_si128(_mm_cmplt_ps(_mm_set1_ps(-1.0f), x)));
}
PSV_INLINE PsvD psv_ncvt_s32_f32_d(PsvD a) { return psv_qlo(psv_ncvt_s32_f32_q(psv_qcat(a, a))); }
PSV_INLINE PsvD psv_ncvt_u32_f32_d(PsvD a) { return psv_qlo(psv_ncvt_u32_f32_q(psv_qcat(a, a))); }
#else
PSV_INLINE PsvQ psv_ncvt_s32_f32_q(PsvQ a) {
    PsvQ r;
    for (int i = 0; i < 4; ++i) r[i] = psv_f32_to_int(0, a[i], 1, 0);
    return r;
}
PSV_INLINE PsvQ psv_ncvt_u32_f32_q(PsvQ a) {
    PsvQ r;
    for (int i = 0; i < 4; ++i) r[i] = psv_f32_to_int(0, a[i], 0, 0);
    return r;
}
PSV_INLINE PsvD psv_ncvt_s32_f32_d(PsvD a) { return psv_qlo(psv_ncvt_s32_f32_q(psv_qcat(a, a))); }
PSV_INLINE PsvD psv_ncvt_u32_f32_d(PsvD a) { return psv_qlo(psv_ncvt_u32_f32_q(psv_qcat(a, a))); }
#endif
#define PSV_NCVT(W)                                                                                                 \
    PSV_INLINE PSV_NV(W) psv_ncvt_f32_s32_##W(PSV_NV(W) a) {                                                        \
        return (PSV_NV(W))__builtin_convertvector((PSV_NT(s32, W))a, PSV_NT(f32, W));                               \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncvt_f32_u32_##W(PSV_NV(W) a) {                                                        \
        return (PSV_NV(W))__builtin_convertvector((PSV_NT(u32, W))a, PSV_NT(f32, W));                               \
    }                                                                                                               \
    /* Fixed point (`fraction` 1-32, a constant): the flushed value scaled by 2^fraction exactly (or to an infinity,  \
     * which saturates as the exact value would), then converted; back: the integer rounded, then scaled exactly. */ \
    PSV_INLINE PSV_NV(W) psv_ncvtx_s32_f32_##W(PSV_NV(W) a, int fraction) {                                          \
        return psv_ncvt_s32_f32_##W((PSV_NV(W))(psv_nf_in_##W(a, 0) * psv_vpow2f(fraction)));                         \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncvtx_u32_f32_##W(PSV_NV(W) a, int fraction) {                                          \
        return psv_ncvt_u32_f32_##W((PSV_NV(W))(psv_nf_in_##W(a, 0) * psv_vpow2f(fraction)));                         \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncvtx_f32_s32_##W(PSV_NV(W) a, int fraction) {                                          \
        return (PSV_NV(W))((PSV_NT(f32, W))psv_ncvt_f32_s32_##W(a) * psv_vpow2f(-fraction));                        \
    }                                                                                                               \
    PSV_INLINE PSV_NV(W) psv_ncvtx_f32_u32_##W(PSV_NV(W) a, int fraction) {                                          \
        return (PSV_NV(W))((PSV_NT(f32, W))psv_ncvt_f32_u32_##W(a) * psv_vpow2f(-fraction));                        \
    }
PSV_NCVT(d)
PSV_NCVT(q)
#undef PSV_NCVT
/* Between half and single precision (Q from D, D from Q): lane by lane with guest.h's exact conversions (rare; x64's
 * baseline has no half-precision instructions). `fpscr` for AHP. */
PSV_INLINE PsvQ psv_ncvt_f32_f16(uint32_t fpscr, PsvD a) {
    const uint32_t s = PSV_FPSCR_STANDARD | (fpscr & PSV_FPSCR_AHP);
    const psv_u16_d h = (psv_u16_d)a;
    return (PsvQ){psv_f16_to_f32(s, h[0]), psv_f16_to_f32(s, h[1]), psv_f16_to_f32(s, h[2]), psv_f16_to_f32(s, h[3])};
}
PSV_INLINE PsvD psv_ncvt_f16_f32(uint32_t fpscr, PsvQ a) {
    const uint32_t s = PSV_FPSCR_STANDARD | (fpscr & PSV_FPSCR_AHP);
    return (PsvD)(psv_u16_d){(uint16_t)psv_f32_to_f16(s, a[0]), (uint16_t)psv_f32_to_f16(s, a[1]), (uint16_t)psv_f32_to_f16(s, a[2]),
                             (uint16_t)psv_f32_to_f16(s, a[3])};
}

#endif /* PSVRECORE_RECOMP_GUEST_NEON_H */
