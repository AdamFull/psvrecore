/* Advanced SIMD (NEON) helpers for recompiled code; included by guest.h.
 *
 * A vector operand is one or two consecutive D registers (a Q register is two), named by its first D register and
 * its register count. Each helper takes the operation and the element size as arguments; at every call site they
 * are constants, so the compiler specializes the loop over elements. Floating-point elements use the standard FPSCR
 * value (flush-to-zero, default NaN, round to nearest), as Advanced SIMD always does. Saturation sets FPSCR.QC. */
#ifndef PSVRECORE_RECOMP_GUEST_SIMD_H
#define PSVRECORE_RECOMP_GUEST_SIMD_H

typedef struct PsvVec {
    uint64_t d[2];
} PsvVec;

typedef __int128 psv_i128;

PSV_INLINE PsvVec psv_vget(const PsvContext* PSV_RESTRICT ctx, uint32_t d, uint32_t regs) {
    PsvVec v;
    v.d[0] = ctx->vfp.d[d];
    v.d[1] = regs == 2 ? ctx->vfp.d[d + 1] : 0;
    return v;
}
PSV_INLINE void psv_vput(PsvContext* PSV_RESTRICT ctx, uint32_t d, uint32_t regs, PsvVec v) {
    ctx->vfp.d[d] = v.d[0];
    if (regs == 2) ctx->vfp.d[d + 1] = v.d[1];
}
PSV_INLINE uint64_t psv_emask(uint32_t esize) { return esize >= 64 ? ~0ull : (1ull << esize) - 1; }
/* Element i of size esize (8, 16, 32, 64). */
PSV_INLINE uint64_t psv_el(PsvVec v, uint32_t i, uint32_t esize) {
    const uint32_t bit = i * esize;
    return (v.d[bit >> 6] >> (bit & 63)) & psv_emask(esize);
}
PSV_INLINE void psv_set_el(PsvVec* v, uint32_t i, uint32_t esize, uint64_t x) {
    const uint32_t bit = i * esize;
    const uint64_t m = psv_emask(esize) << (bit & 63);
    v->d[bit >> 6] = (v->d[bit >> 6] & ~m) | ((x << (bit & 63)) & m);
}
PSV_INLINE int64_t psv_sext(uint64_t x, uint32_t esize) {
    return esize >= 64 ? (int64_t)x : (int64_t)(x << (64 - esize)) >> (64 - esize);
}
/* An element as a (wide) integer, signed or not. */
PSV_INLINE psv_i128 psv_int(uint64_t x, uint32_t esize, int is_unsigned) {
    return is_unsigned ? (psv_i128)x : (psv_i128)psv_sext(x, esize);
}
PSV_INLINE void psv_set_qc(PsvContext* PSV_RESTRICT ctx) { ctx->fpscr |= PSV_FPSCR_QC; }
/* SatQ: to an esize-bit signed or unsigned range, setting QC when it saturates. */
PSV_INLINE uint64_t psv_satq(PsvContext* PSV_RESTRICT ctx, psv_i128 x, uint32_t esize, int is_unsigned) {
    psv_i128 hi, lo;
    if (is_unsigned) {
        hi = esize >= 64 ? (psv_i128)~0ull : (psv_i128)((1ull << esize) - 1);
        lo = 0;
    } else {
        hi = (psv_i128)((1ull << (esize - 1)) - 1);
        lo = -hi - 1;
    }
    if (x > hi) { psv_set_qc(ctx); return (uint64_t)hi & psv_emask(esize); }
    if (x < lo) { psv_set_qc(ctx); return (uint64_t)lo & psv_emask(esize); }
    return (uint64_t)x & psv_emask(esize);
}
/* x shifted by a signed amount (left when positive), as VSHL and kin define it: a right shift is arithmetic for
 * signed elements; `round` adds half of the last bit shifted out. The result is not truncated to the element. */
PSV_INLINE psv_i128 psv_shift_signed(psv_i128 x, int shift, int round) {
    /* (x has at most 64 significant bits. A left shift of 64 or more leaves nothing in any element; it stands for
     * "too large" to the saturating forms.) */
    if (shift >= 64) return x == 0 ? 0 : (x < 0 ? -((psv_i128)1 << 100) : ((psv_i128)1 << 100));
    if (shift >= 0) return x * ((psv_i128)1 << shift);
    const int r = -shift > 120 ? 120 : -shift;  /* (beyond 64 bits the result is 0 or -1, rounded or not, as at 120) */
    const psv_i128 bias = round ? ((psv_i128)1 << (r - 1)) : 0;
    return (x + bias) >> r;
}

/* ---- Integer operations on elements of the same size ---------------------------------------------------------- */

enum {
    PSV_N_HADD, PSV_N_RHADD, PSV_N_HSUB, PSV_N_QADD, PSV_N_QSUB, PSV_N_CGT, PSV_N_CGE, PSV_N_SHL, PSV_N_QSHL, PSV_N_RSHL,
    PSV_N_QRSHL, PSV_N_MAX, PSV_N_MIN, PSV_N_ABD, PSV_N_ABA, PSV_N_ADD, PSV_N_SUB, PSV_N_TST, PSV_N_CEQ, PSV_N_MLA,
    PSV_N_MLS, PSV_N_MUL, PSV_N_MULP, PSV_N_QDMULH, PSV_N_QRDMULH, PSV_N_PADD, PSV_N_PMAX, PSV_N_PMIN,
};

PSV_INLINE uint64_t psv_poly_mul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
        if ((b >> i) & 1) r ^= a << i;
    return r;
}

PSV_INLINE uint64_t psv_n3_element(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, int u, uint64_t xe, uint64_t ye, uint64_t acc) {
    const uint64_t mask = psv_emask(esize);
    const psv_i128 x = psv_int(xe, esize, u), y = psv_int(ye, esize, u);
    switch (op) {
        case PSV_N_HADD: return (uint64_t)((x + y) >> 1) & mask;
        case PSV_N_RHADD: return (uint64_t)((x + y + 1) >> 1) & mask;
        case PSV_N_HSUB: return (uint64_t)((x - y) >> 1) & mask;
        case PSV_N_QADD: return psv_satq(ctx, x + y, esize, u);
        case PSV_N_QSUB: return psv_satq(ctx, x - y, esize, u);
        case PSV_N_CGT: return x > y ? mask : 0;
        case PSV_N_CGE: return x >= y ? mask : 0;
        case PSV_N_SHL:
        case PSV_N_QSHL:
        case PSV_N_RSHL:
        case PSV_N_QRSHL: {
            /* (the shift is the signed bottom byte of the second operand's element) */
            const int shift = (int8_t)(ye & 0xFF);
            const psv_i128 r = psv_shift_signed(x, shift, op == PSV_N_RSHL || op == PSV_N_QRSHL);
            if (op == PSV_N_QSHL || op == PSV_N_QRSHL) return psv_satq(ctx, r, esize, u);
            return (uint64_t)r & mask;
        }
        case PSV_N_MAX: return (uint64_t)(x > y ? x : y) & mask;
        case PSV_N_MIN: return (uint64_t)(x < y ? x : y) & mask;
        case PSV_N_ABD: return (uint64_t)(x > y ? x - y : y - x) & mask;
        case PSV_N_ABA: return (acc + (uint64_t)(x > y ? x - y : y - x)) & mask;
        case PSV_N_ADD: return (xe + ye) & mask;
        case PSV_N_SUB: return (xe - ye) & mask;
        case PSV_N_TST: return (xe & ye) ? mask : 0;
        case PSV_N_CEQ: return xe == ye ? mask : 0;
        case PSV_N_MLA: return (acc + xe * ye) & mask;
        case PSV_N_MLS: return (acc - xe * ye) & mask;
        case PSV_N_MUL: return (xe * ye) & mask;
        case PSV_N_MULP: return psv_poly_mul8(xe, ye) & mask;
        case PSV_N_QDMULH: return psv_satq(ctx, (2 * x * y) >> esize, esize, 0);
        case PSV_N_QRDMULH: return psv_satq(ctx, (2 * x * y + ((psv_i128)1 << (esize - 1))) >> esize, esize, 0);
        case PSV_N_PADD: return (xe + ye) & mask;
        case PSV_N_PMAX: return (uint64_t)(x > y ? x : y) & mask;
        default: return (uint64_t)(x < y ? x : y) & mask; /* PSV_N_PMIN */
    }
}

/* d = op(n, m) element by element; with `scalar` >= 0, m is that element of D register m (by-scalar forms). The
 * pairwise operations (D registers only) combine adjacent elements of n, then of m. */
PSV_INLINE void psv_n3(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, int u, uint32_t regs, uint32_t d, uint32_t n,
                       uint32_t m, int scalar) {
    const PsvVec a = psv_vget(ctx, n, regs), acc = psv_vget(ctx, d, regs);
    const PsvVec b = psv_vget(ctx, m, scalar >= 0 ? 1 : regs);
    PsvVec r = acc;
    const uint32_t lanes = regs * 64 / esize;
    if (op == PSV_N_PADD || op == PSV_N_PMAX || op == PSV_N_PMIN) {
        const uint32_t half = lanes / 2;
        for (uint32_t i = 0; i < half; ++i) {
            psv_set_el(&r, i, esize, psv_n3_element(ctx, op, esize, u, psv_el(a, 2 * i, esize), psv_el(a, 2 * i + 1, esize), 0));
            psv_set_el(&r, half + i, esize, psv_n3_element(ctx, op, esize, u, psv_el(b, 2 * i, esize), psv_el(b, 2 * i + 1, esize), 0));
        }
    } else {
        for (uint32_t i = 0; i < lanes; ++i) {
            const uint64_t y = psv_el(b, scalar >= 0 ? (uint32_t)scalar : i, esize);
            psv_set_el(&r, i, esize, psv_n3_element(ctx, op, esize, u, psv_el(a, i, esize), y, psv_el(acc, i, esize)));
        }
    }
    psv_vput(ctx, d, regs, r);
}

/* Bitwise operations on whole registers. */
enum { PSV_N_AND, PSV_N_BIC, PSV_N_ORR, PSV_N_ORN, PSV_N_EOR, PSV_N_BSL, PSV_N_BIT, PSV_N_BIF };
PSV_INLINE void psv_nlogic(PsvContext* PSV_RESTRICT ctx, int op, uint32_t regs, uint32_t d, uint32_t n, uint32_t m) {
    for (uint32_t i = 0; i < regs; ++i) {
        const uint64_t x = ctx->vfp.d[n + i], y = ctx->vfp.d[m + i], z = ctx->vfp.d[d + i];
        uint64_t r;
        switch (op) {
            case PSV_N_AND: r = x & y; break;
            case PSV_N_BIC: r = x & ~y; break;
            case PSV_N_ORR: r = x | y; break;
            case PSV_N_ORN: r = x | ~y; break;
            case PSV_N_EOR: r = x ^ y; break;
            case PSV_N_BSL: r = (z & x) | (~z & y); break;
            case PSV_N_BIT: r = (x & y) | (z & ~y); break;
            default: r = (z & y) | (x & ~y); break; /* PSV_N_BIF */
        }
        ctx->vfp.d[d + i] = r;
    }
}

/* ---- Floating-point elements (single precision) --------------------------------------------------------------- */

enum {
    PSV_NF_ADD, PSV_NF_SUB, PSV_NF_MUL, PSV_NF_ABD, PSV_NF_MLA, PSV_NF_MLS, PSV_NF_CEQ, PSV_NF_CGE, PSV_NF_CGT, PSV_NF_ACGE,
    PSV_NF_ACGT, PSV_NF_MAX, PSV_NF_MIN, PSV_NF_RECPS, PSV_NF_RSQRTS, PSV_NF_PADD, PSV_NF_PMAX, PSV_NF_PMIN,
};

/* FPMax / FPMin: NaNs give the default NaN; of two zeros, max is +0 unless both are -0, min -0 unless both are +0. */
PSV_INLINE uint32_t psv_fmax32(uint32_t fpscr, uint32_t a, uint32_t b, int is_max) {
    a = psv_flush32(fpscr, a);
    b = psv_flush32(fpscr, b);
    if (psv_nan32(a) || psv_nan32(b)) return psv_nans32(fpscr, a, b);
    const float x = psv_f32(a), y = psv_f32(b);
    if (x == 0.0f && y == 0.0f) return is_max ? (a & b) : (a | b);
    if (x == y) return a;
    return (is_max ? x > y : x < y) ? a : b;
}
/* FPCompareEQ/GE/GT (and on absolute values): unordered compares false. */
PSV_INLINE int psv_fcompare32(uint32_t fpscr, uint32_t a, uint32_t b, int which /* 0 eq, 1 ge, 2 gt */) {
    a = psv_flush32(fpscr, a);
    b = psv_flush32(fpscr, b);
    if (psv_nan32(a) || psv_nan32(b)) return 0;
    const float x = psv_f32(a), y = psv_f32(b);
    return which == 0 ? x == y : which == 1 ? x >= y : x > y;
}
/* VRECPS: 2 - a * b; VRSQRTS: (3 - a * b) / 2 (FPRecipStep, FPRSqrtStep). An infinity times a zero counts as a
 * product of +0. */
PSV_INLINE uint32_t psv_frecps32(uint32_t a, uint32_t b, int rsqrt) {
    const uint32_t fpscr = PSV_FPSCR_STANDARD;
    a = psv_flush32(fpscr, a);
    b = psv_flush32(fpscr, b);
    if (psv_nan32(a) || psv_nan32(b)) return psv_nans32(fpscr, a, b);
    const int inf_a = (a & 0x7FFFFFFFu) == 0x7F800000u, inf_b = (b & 0x7FFFFFFFu) == 0x7F800000u;
    const int zero_a = (a & 0x7FFFFFFFu) == 0, zero_b = (b & 0x7FFFFFFFu) == 0;
    const uint32_t product = (inf_a && zero_b) || (zero_a && inf_b) ? 0u : psv_fmul32(fpscr, a, b);
    if (!rsqrt) return psv_fsub32(fpscr, 0x40000000u, product);
    /* FPHalvedSub(3, product): (3 - p) / 2 rounded once (computed in double: innocuous, see the VFP notes; an
     * infinite product gives the opposite infinity) */
    return psv_round32(fpscr, (3.0 - (double)psv_f32(product)) * 0.5);
}

PSV_INLINE uint32_t psv_nf3_element(int op, uint32_t x, uint32_t y, uint32_t acc) {
    const uint32_t s = PSV_FPSCR_STANDARD;
    switch (op) {
        case PSV_NF_ADD: case PSV_NF_PADD: return psv_fadd32(s, x, y);
        case PSV_NF_SUB: return psv_fsub32(s, x, y);
        case PSV_NF_MUL: return psv_fmul32(s, x, y);
        case PSV_NF_ABD: return psv_fsub32(s, x, y) & 0x7FFFFFFFu;
        case PSV_NF_MLA: return psv_fadd32(s, acc, psv_fmul32(s, x, y));
        case PSV_NF_MLS: return psv_fsub32(s, acc, psv_fmul32(s, x, y));
        case PSV_NF_CEQ: return psv_fcompare32(s, x, y, 0) ? 0xFFFFFFFFu : 0;
        case PSV_NF_CGE: return psv_fcompare32(s, x, y, 1) ? 0xFFFFFFFFu : 0;
        case PSV_NF_CGT: return psv_fcompare32(s, x, y, 2) ? 0xFFFFFFFFu : 0;
        case PSV_NF_ACGE: return psv_fcompare32(s, x & 0x7FFFFFFFu, y & 0x7FFFFFFFu, 1) ? 0xFFFFFFFFu : 0;
        case PSV_NF_ACGT: return psv_fcompare32(s, x & 0x7FFFFFFFu, y & 0x7FFFFFFFu, 2) ? 0xFFFFFFFFu : 0;
        case PSV_NF_MAX: case PSV_NF_PMAX: return psv_fmax32(s, x, y, 1);
        case PSV_NF_MIN: case PSV_NF_PMIN: return psv_fmax32(s, x, y, 0);
        case PSV_NF_RECPS: return psv_frecps32(x, y, 0);
        default: return psv_frecps32(x, y, 1); /* PSV_NF_RSQRTS */
    }
}

PSV_INLINE void psv_nf3(PsvContext* PSV_RESTRICT ctx, int op, uint32_t regs, uint32_t d, uint32_t n, uint32_t m, int scalar) {
    const PsvVec a = psv_vget(ctx, n, regs), acc = psv_vget(ctx, d, regs);
    const PsvVec b = psv_vget(ctx, m, scalar >= 0 ? 1 : regs);
    PsvVec r = acc;
    const uint32_t lanes = regs * 2;
    if (op == PSV_NF_PADD || op == PSV_NF_PMAX || op == PSV_NF_PMIN) {
        psv_set_el(&r, 0, 32, psv_nf3_element(op, (uint32_t)psv_el(a, 0, 32), (uint32_t)psv_el(a, 1, 32), 0));
        psv_set_el(&r, 1, 32, psv_nf3_element(op, (uint32_t)psv_el(b, 0, 32), (uint32_t)psv_el(b, 1, 32), 0));
    } else {
        for (uint32_t i = 0; i < lanes; ++i) {
            const uint32_t y = (uint32_t)psv_el(b, scalar >= 0 ? (uint32_t)scalar : i, 32);
            psv_set_el(&r, i, 32, psv_nf3_element(op, (uint32_t)psv_el(a, i, 32), y, (uint32_t)psv_el(acc, i, 32)));
        }
    }
    psv_vput(ctx, d, regs, r);
}

/* ---- Long, wide and narrow operations ----------------------------------------------------------------------------
 * `esize` is the narrow element size. Long: Q d (2 esize) from D n and D m (esize); wide: Q d from Q n (2 esize)
 * and D m; narrow: D d (esize) from Q n and Q m (2 esize). */

enum {
    PSV_NL_ADDL, PSV_NL_ADDW, PSV_NL_SUBL, PSV_NL_SUBW, PSV_NL_ABAL, PSV_NL_ABDL, PSV_NL_MLAL, PSV_NL_MLSL, PSV_NL_MULL,
    PSV_NL_MULLP, PSV_NL_QDMLAL, PSV_NL_QDMLSL, PSV_NL_QDMULL, PSV_NL_ADDHN, PSV_NL_RADDHN, PSV_NL_SUBHN, PSV_NL_RSUBHN,
};

PSV_INLINE void psv_nlong(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, int u, uint32_t d, uint32_t n, uint32_t m, int scalar) {
    const uint32_t wide = 2 * esize;
    if (op == PSV_NL_ADDHN || op == PSV_NL_RADDHN || op == PSV_NL_SUBHN || op == PSV_NL_RSUBHN) {
        const PsvVec a = psv_vget(ctx, n, 2), b = psv_vget(ctx, m, 2);
        PsvVec r = {{0, 0}};
        const uint32_t lanes = 64 / esize;
        const uint64_t round = (op == PSV_NL_RADDHN || op == PSV_NL_RSUBHN) ? 1ull << (esize - 1) : 0;
        for (uint32_t i = 0; i < lanes; ++i) {
            const uint64_t x = psv_el(a, i, wide), y = psv_el(b, i, wide);
            const uint64_t s = (op == PSV_NL_ADDHN || op == PSV_NL_RADDHN) ? x + y : x - y;
            psv_set_el(&r, i, esize, ((s + round) & psv_emask(wide)) >> esize);
        }
        psv_vput(ctx, d, 1, r);
        return;
    }
    const int wide_n = op == PSV_NL_ADDW || op == PSV_NL_SUBW;
    const PsvVec a = psv_vget(ctx, n, wide_n ? 2 : 1), b = psv_vget(ctx, m, 1), acc = psv_vget(ctx, d, 2);
    PsvVec r = acc;
    const uint32_t lanes = 64 / esize;
    for (uint32_t i = 0; i < lanes; ++i) {
        const psv_i128 x = wide_n ? psv_int(psv_el(a, i, wide), wide, u) : psv_int(psv_el(a, i, esize), esize, u);
        const uint64_t ye = psv_el(b, scalar >= 0 ? (uint32_t)scalar : i, esize);
        const psv_i128 y = psv_int(ye, esize, u), z = psv_int(psv_el(acc, i, wide), wide, u);
        psv_i128 v;
        switch (op) {
            case PSV_NL_ADDL: case PSV_NL_ADDW: v = x + y; break;
            case PSV_NL_SUBL: case PSV_NL_SUBW: v = x - y; break;
            case PSV_NL_ABAL: v = z + (x > y ? x - y : y - x); break;
            case PSV_NL_ABDL: v = x > y ? x - y : y - x; break;
            case PSV_NL_MLAL: v = z + x * y; break;
            case PSV_NL_MLSL: v = z - x * y; break;
            case PSV_NL_MULL: v = x * y; break;
            case PSV_NL_MULLP: v = (psv_i128)psv_poly_mul8(psv_el(a, i, 8), ye); break;
            default: {  /* the doubling saturating forms (signed) */
                const uint64_t product = psv_satq(ctx, 2 * x * y, wide, 0);
                if (op == PSV_NL_QDMULL) {
                    v = (psv_i128)product;
                    break;
                }
                const psv_i128 p = psv_sext(product, wide);
                v = (psv_i128)psv_satq(ctx, op == PSV_NL_QDMLAL ? z + p : z - p, wide, 0);
                break;
            }
        }
        psv_set_el(&r, i, wide, (uint64_t)v & psv_emask(wide));
    }
    psv_vput(ctx, d, 2, r);
}

/* ---- Shifts by an immediate ------------------------------------------------------------------------------------ */

enum {
    PSV_NS_SHR, PSV_NS_SRA, PSV_NS_RSHR, PSV_NS_RSRA, PSV_NS_SRI, PSV_NS_SHL, PSV_NS_SLI, PSV_NS_QSHLU, PSV_NS_QSHL,
    /* narrowing: Q m (2 esize) to D d (esize) */
    PSV_NS_SHRN, PSV_NS_RSHRN, PSV_NS_QSHRUN, PSV_NS_QRSHRUN, PSV_NS_QSHRN, PSV_NS_QRSHRN,
    /* lengthening: D m (esize) to Q d (2 esize) */
    PSV_NS_SHLL,
};

PSV_INLINE void psv_nshift(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, int u, uint32_t regs, uint32_t d, uint32_t m,
                           uint32_t amount) {
    const uint64_t mask = psv_emask(esize);
    if (op >= PSV_NS_SHRN && op <= PSV_NS_QRSHRN) {
        const PsvVec a = psv_vget(ctx, m, 2);
        PsvVec r = {{0, 0}};
        const uint32_t wide = 2 * esize, lanes = 64 / esize;
        const int round = op == PSV_NS_RSHRN || op == PSV_NS_QRSHRUN || op == PSV_NS_QRSHRN;
        /* the source is signed for the "un" forms (to unsigned) and per `u` for the others */
        const int src_unsigned = (op == PSV_NS_QSHRUN || op == PSV_NS_QRSHRUN) ? 0 : u;
        for (uint32_t i = 0; i < lanes; ++i) {
            const psv_i128 x = psv_int(psv_el(a, i, wide), wide, src_unsigned);
            const psv_i128 s = psv_shift_signed(x, -(int)amount, round);
            uint64_t e;
            if (op == PSV_NS_SHRN || op == PSV_NS_RSHRN) e = (uint64_t)s & mask;
            else if (op == PSV_NS_QSHRUN || op == PSV_NS_QRSHRUN) e = psv_satq(ctx, s, esize, 1);
            else e = psv_satq(ctx, s, esize, u);
            psv_set_el(&r, i, esize, e);
        }
        psv_vput(ctx, d, 1, r);
        return;
    }
    if (op == PSV_NS_SHLL) {
        const PsvVec a = psv_vget(ctx, m, 1);
        PsvVec r = {{0, 0}};
        const uint32_t wide = 2 * esize, lanes = 64 / esize;
        for (uint32_t i = 0; i < lanes; ++i)
            psv_set_el(&r, i, wide, ((uint64_t)psv_int(psv_el(a, i, esize), esize, u) << amount) & psv_emask(wide));
        psv_vput(ctx, d, 2, r);
        return;
    }
    const PsvVec a = psv_vget(ctx, m, regs), acc = psv_vget(ctx, d, regs);
    PsvVec r = acc;
    const uint32_t lanes = regs * 64 / esize;
    for (uint32_t i = 0; i < lanes; ++i) {
        const uint64_t xe = psv_el(a, i, esize), de = psv_el(acc, i, esize);
        const psv_i128 x = psv_int(xe, esize, u);
        uint64_t e;
        switch (op) {
            case PSV_NS_SHR: e = (uint64_t)psv_shift_signed(x, -(int)amount, 0) & mask; break;
            case PSV_NS_SRA: e = (de + (uint64_t)psv_shift_signed(x, -(int)amount, 0)) & mask; break;
            case PSV_NS_RSHR: e = (uint64_t)psv_shift_signed(x, -(int)amount, 1) & mask; break;
            case PSV_NS_RSRA: e = (de + (uint64_t)psv_shift_signed(x, -(int)amount, 1)) & mask; break;
            case PSV_NS_SRI: {  /* shift right and insert: the top `amount` bits of the destination stay */
                const uint64_t keep = amount >= esize ? mask : ~(mask >> amount) & mask;
                e = (de & keep) | (amount >= esize ? 0 : (xe >> amount));
                break;
            }
            case PSV_NS_SHL: e = (xe << amount) & mask; break;
            case PSV_NS_SLI: {
                const uint64_t keep = (1ull << amount) - 1;
                e = (de & keep) | ((xe << amount) & mask & ~keep);
                break;
            }
            case PSV_NS_QSHLU: e = psv_satq(ctx, psv_shift_signed(psv_int(xe, esize, 0), (int)amount, 0), esize, 1); break;
            default: e = psv_satq(ctx, psv_shift_signed(x, (int)amount, 0), esize, u); break; /* PSV_NS_QSHL */
        }
        psv_set_el(&r, i, esize, e);
    }
    psv_vput(ctx, d, regs, r);
}

/* ---- Two registers, miscellaneous ------------------------------------------------------------------------------ */

enum {
    PSV_NM_REV64, PSV_NM_REV32, PSV_NM_REV16, PSV_NM_PADDL, PSV_NM_PADAL, PSV_NM_CLS, PSV_NM_CLZ, PSV_NM_CNT, PSV_NM_MVN,
    PSV_NM_QABS, PSV_NM_QNEG, PSV_NM_CGT0, PSV_NM_CGE0, PSV_NM_CEQ0, PSV_NM_CLE0, PSV_NM_CLT0, PSV_NM_ABS, PSV_NM_NEG,
    /* single-precision elements */
    PSV_NM_FCGT0, PSV_NM_FCGE0, PSV_NM_FCEQ0, PSV_NM_FCLE0, PSV_NM_FCLT0, PSV_NM_FABS, PSV_NM_FNEG, PSV_NM_RECPE,
    PSV_NM_RSQRTE, PSV_NM_FRECPE, PSV_NM_FRSQRTE, PSV_NM_F2S, PSV_NM_F2U, PSV_NM_S2F, PSV_NM_U2F,
    /* narrowing: Q m (2 esize) to D d (esize) */
    PSV_NM_MOVN, PSV_NM_QMOVUN, PSV_NM_QMOVN,
};

/* The reciprocal and reciprocal square root estimates (ARM ARM: recip_estimate, recip_sqrt_estimate), in integers:
 * the 8-bit estimate s (256 <= s < 512) for an input whose top nine (eight) bits are q. */
PSV_INLINE uint32_t psv_recip_s(uint32_t q /* 256..511 */) { return (524288u + 2 * q + 1) / (4 * q + 2); }
PSV_INLINE uint32_t psv_rsqrt_s(uint32_t k /* 2q+1 */, uint32_t limit) {
    /* the largest s with (2s - 1)^2 k <= limit: s = RoundDown(256 / sqrt(scaled input) + 0.5) */
    uint32_t s = 511;
    while (s > 0 && (uint64_t)(2 * s - 1) * (2 * s - 1) * k > limit) --s;
    return s;
}
PSV_INLINE uint32_t psv_urecpe(uint32_t x) {
    if (!(x >> 31)) return 0xFFFFFFFFu;
    return 0x80000000u | ((psv_recip_s((x >> 23) & 0x1FF) - 256) << 23);
}
PSV_INLINE uint32_t psv_ursqrte(uint32_t x) {
    if (!(x >> 30)) return 0xFFFFFFFFu;
    uint32_t s;
    if (x >> 31) s = psv_rsqrt_s(2 * ((x >> 24) & 0xFF) + 1, 134217728u);  /* [0.5, 1): q = a * 256 */
    else s = psv_rsqrt_s(2 * ((x >> 23) & 0xFF) + 1, 268435456u);         /* [0.25, 0.5): q = a * 512 */
    return 0x80000000u | ((s - 256) << 23);
}
PSV_INLINE uint32_t psv_frecpe(uint32_t x) {
    const uint32_t fpscr = PSV_FPSCR_STANDARD;
    x = psv_flush32(fpscr, x);
    const uint32_t sign = x & 0x80000000u, exp = (x >> 23) & 0xFF;
    if (psv_nan32(x)) return PSV_DEFAULT_NAN32;
    if ((x & 0x7FFFFFFFu) == 0x7F800000u) return sign;
    if ((x & 0x7FFFFFFFu) == 0) return sign | 0x7F800000u;
    if (exp >= 253) return sign;  /* |x| >= 2^126: the reciprocal is below the normal range */
    const uint32_t q = 256 | ((x >> 15) & 0xFF);  /* a = 0.1fff... in [0.5, 1): RoundDown(a * 512) */
    return sign | ((253 - exp) << 23) | ((psv_recip_s(q) - 256) << 15);
}
PSV_INLINE uint32_t psv_frsqrte(uint32_t x) {
    const uint32_t fpscr = PSV_FPSCR_STANDARD;
    x = psv_flush32(fpscr, x);
    const uint32_t exp = (x >> 23) & 0xFF;
    if (psv_nan32(x)) return PSV_DEFAULT_NAN32;
    if ((x & 0x7FFFFFFFu) == 0) return (x & 0x80000000u) | 0x7F800000u;
    if (x >> 31) return PSV_DEFAULT_NAN32;
    if (x == 0x7F800000u) return 0;
    /* The fraction scaled into [0.5, 1) when the exponent is even, [0.25, 0.5) when it is odd; either way the
     * estimate's index is 128 plus the top seven fraction bits. */
    const uint32_t k = 2 * (128 | ((x >> 16) & 0x7F)) + 1;
    const uint32_t s = (exp & 1) ? psv_rsqrt_s(k, 268435456u) : psv_rsqrt_s(k, 134217728u);
    return (((380 - exp) / 2) << 23) | ((s - 256) << 15);
}

PSV_INLINE void psv_nmisc(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, int u, uint32_t regs, uint32_t d, uint32_t m) {
    const uint64_t mask = psv_emask(esize);
    if (op >= PSV_NM_MOVN) {
        const PsvVec a = psv_vget(ctx, m, 2);
        PsvVec r = {{0, 0}};
        const uint32_t wide = 2 * esize, lanes = 64 / esize;
        for (uint32_t i = 0; i < lanes; ++i) {
            const uint64_t x = psv_el(a, i, wide);
            uint64_t e;
            if (op == PSV_NM_MOVN) e = x & mask;
            else if (op == PSV_NM_QMOVUN) e = psv_satq(ctx, psv_int(x, wide, 0), esize, 1);
            else e = psv_satq(ctx, psv_int(x, wide, u), esize, u);
            psv_set_el(&r, i, esize, e);
        }
        psv_vput(ctx, d, 1, r);
        return;
    }
    const PsvVec a = psv_vget(ctx, m, regs), acc = psv_vget(ctx, d, regs);
    PsvVec r = acc;
    const uint32_t lanes = regs * 64 / esize;
    if (op == PSV_NM_REV64 || op == PSV_NM_REV32 || op == PSV_NM_REV16) {
        const uint32_t group = (op == PSV_NM_REV64 ? 64 : op == PSV_NM_REV32 ? 32 : 16) / esize;  /* elements per group */
        for (uint32_t i = 0; i < lanes; ++i) psv_set_el(&r, i, esize, psv_el(a, (i / group) * group + (group - 1 - i % group), esize));
        psv_vput(ctx, d, regs, r);
        return;
    }
    if (op == PSV_NM_PADDL || op == PSV_NM_PADAL) {
        const uint32_t wide = 2 * esize;
        for (uint32_t i = 0; i < lanes / 2; ++i) {
            psv_i128 v = psv_int(psv_el(a, 2 * i, esize), esize, u) + psv_int(psv_el(a, 2 * i + 1, esize), esize, u);
            if (op == PSV_NM_PADAL) v += psv_int(psv_el(acc, i, wide), wide, u);
            psv_set_el(&r, i, wide, (uint64_t)v & psv_emask(wide));
        }
        psv_vput(ctx, d, regs, r);
        return;
    }
    for (uint32_t i = 0; i < lanes; ++i) {
        const uint64_t xe = psv_el(a, i, esize);
        const psv_i128 x = psv_int(xe, esize, 0);
        const uint32_t f = (uint32_t)xe;
        const uint32_t s = PSV_FPSCR_STANDARD;
        uint64_t e;
        switch (op) {
            case PSV_NM_CLS: {
                uint32_t n = 0;
                const uint64_t top = (xe >> (esize - 1)) & 1;
                for (int b = (int)esize - 2; b >= 0 && ((xe >> b) & 1) == top; --b) ++n;
                e = n;
                break;
            }
            case PSV_NM_CLZ: {
                uint32_t n = 0;
                for (int b = (int)esize - 1; b >= 0 && !((xe >> b) & 1); --b) ++n;
                e = n;
                break;
            }
            case PSV_NM_CNT: e = (uint64_t)__builtin_popcountll(xe); break;
            case PSV_NM_MVN: e = ~xe & mask; break;
            case PSV_NM_QABS: e = psv_satq(ctx, x < 0 ? -x : x, esize, 0); break;
            case PSV_NM_QNEG: e = psv_satq(ctx, -x, esize, 0); break;
            case PSV_NM_CGT0: e = x > 0 ? mask : 0; break;
            case PSV_NM_CGE0: e = x >= 0 ? mask : 0; break;
            case PSV_NM_CEQ0: e = xe == 0 ? mask : 0; break;
            case PSV_NM_CLE0: e = x <= 0 ? mask : 0; break;
            case PSV_NM_CLT0: e = x < 0 ? mask : 0; break;
            case PSV_NM_ABS: e = (uint64_t)(x < 0 ? -x : x) & mask; break;
            case PSV_NM_NEG: e = (uint64_t)(-x) & mask; break;
            case PSV_NM_FCGT0: e = psv_fcompare32(s, f, 0, 2) ? mask : 0; break;
            case PSV_NM_FCGE0: e = psv_fcompare32(s, f, 0, 1) ? mask : 0; break;
            case PSV_NM_FCEQ0: e = psv_fcompare32(s, f, 0, 0) ? mask : 0; break;
            case PSV_NM_FCLE0: e = psv_fcompare32(s, 0, f, 1) ? mask : 0; break;
            case PSV_NM_FCLT0: e = psv_fcompare32(s, 0, f, 2) ? mask : 0; break;
            case PSV_NM_FABS: e = f & 0x7FFFFFFFu; break;
            case PSV_NM_FNEG: e = f ^ 0x80000000u; break;
            case PSV_NM_RECPE: e = psv_urecpe(f); break;
            case PSV_NM_RSQRTE: e = psv_ursqrte(f); break;
            case PSV_NM_FRECPE: e = psv_frecpe(f); break;
            case PSV_NM_FRSQRTE: e = psv_frsqrte(f); break;
            case PSV_NM_F2S: e = psv_f32_to_int(s, f, 1, 0); break;
            case PSV_NM_F2U: e = psv_f32_to_int(s, f, 0, 0); break;
            case PSV_NM_S2F: e = psv_int_to_f32(f, 1); break;
            default: e = psv_int_to_f32(f, 0); break; /* PSV_NM_U2F */
        }
        psv_set_el(&r, i, esize, e);
    }
    psv_vput(ctx, d, regs, r);
}

/* VCVT between single precision and fixed point (`fraction` bits): toward zero to fixed, to nearest from fixed. */
PSV_INLINE void psv_ncvt_fixed(PsvContext* PSV_RESTRICT ctx, int to_fixed, int u, uint32_t regs, uint32_t d, uint32_t m, uint32_t fraction) {
    const uint32_t s = PSV_FPSCR_STANDARD;
    for (uint32_t i = 0; i < regs * 2; ++i) {
        const uint32_t x = ctx->vfp.s[m * 2 + i];
        ctx->vfp.s[d * 2 + i] = to_fixed ? psv_f32_to_fixed(s, x, 32, fraction, !u) : psv_fixed_to_f32(s, x, 32, fraction, !u);
    }
}
/* VCVT between half and single precision: Q d from D m (to single) or D d from Q m (to half). */
PSV_INLINE void psv_ncvt_half(PsvContext* PSV_RESTRICT ctx, int to_single, uint32_t d, uint32_t m) {
    const uint32_t s = PSV_FPSCR_STANDARD | (ctx->fpscr & PSV_FPSCR_AHP);
    if (to_single) {
        const uint64_t h = ctx->vfp.d[m];
        for (uint32_t i = 0; i < 4; ++i) ctx->vfp.s[d * 2 + i] = psv_f16_to_f32(s, (uint32_t)(h >> (16 * i)) & 0xFFFFu);
    } else {
        uint64_t h = 0;
        for (uint32_t i = 0; i < 4; ++i) h |= (uint64_t)psv_f32_to_f16(s, ctx->vfp.s[m * 2 + i]) << (16 * i);
        ctx->vfp.d[d] = h;
    }
}

/* ---- Permutations, extraction, table lookups, duplication ------------------------------------------------------- */

enum { PSV_NP_SWP, PSV_NP_TRN, PSV_NP_UZP, PSV_NP_ZIP };
PSV_INLINE void psv_nperm(PsvContext* PSV_RESTRICT ctx, int op, uint32_t esize, uint32_t regs, uint32_t d, uint32_t m) {
    const PsvVec x = psv_vget(ctx, d, regs), y = psv_vget(ctx, m, regs);
    PsvVec a = x, b = y;
    const uint32_t lanes = regs * 64 / esize;
    switch (op) {
        case PSV_NP_SWP: a = y, b = x; break;
        case PSV_NP_TRN:
            for (uint32_t i = 0; i < lanes; i += 2) {
                psv_set_el(&a, i + 1, esize, psv_el(y, i, esize));
                psv_set_el(&b, i, esize, psv_el(x, i + 1, esize));
            }
            break;
        case PSV_NP_UZP:  /* the even elements of x:y to d, the odd ones to m */
            for (uint32_t i = 0; i < lanes; ++i) {
                const uint32_t even = 2 * i, odd = 2 * i + 1;
                psv_set_el(&a, i, esize, even < lanes ? psv_el(x, even, esize) : psv_el(y, even - lanes, esize));
                psv_set_el(&b, i, esize, odd < lanes ? psv_el(x, odd, esize) : psv_el(y, odd - lanes, esize));
            }
            break;
        default:  /* PSV_NP_ZIP: x0 y0 x1 y1 ..., the first half to d */
            for (uint32_t i = 0; i < lanes; ++i) {
                const uint64_t lo = (i % 2 == 0) ? psv_el(x, i / 2, esize) : psv_el(y, i / 2, esize);
                const uint64_t hi = (i % 2 == 0) ? psv_el(x, (lanes + i) / 2, esize) : psv_el(y, (lanes + i) / 2, esize);
                psv_set_el(&a, i, esize, lo);
                psv_set_el(&b, i, esize, hi);
            }
            break;
    }
    psv_vput(ctx, d, regs, a);
    psv_vput(ctx, m, regs, b);
}

/* VEXT: bytes `start`... of the concatenation m:n (n the low part). */
PSV_INLINE void psv_next(PsvContext* PSV_RESTRICT ctx, uint32_t regs, uint32_t d, uint32_t n, uint32_t m, uint32_t start) {
    uint8_t bytes[32];
    const uint32_t size = regs * 8;
    memcpy(bytes, &ctx->vfp.d[n], size);
    memcpy(bytes + size, &ctx->vfp.d[m], size);
    memcpy(&ctx->vfp.d[d], bytes + start, size);
}
/* VTBL/VTBX: each byte of m indexes the table (`length` D registers from n); out of range gives 0 (VTBL) or keeps
 * the destination byte (VTBX). */
PSV_INLINE void psv_ntbl(PsvContext* PSV_RESTRICT ctx, int extend, uint32_t d, uint32_t n, uint32_t length, uint32_t m) {
    uint8_t table[32];
    for (uint32_t i = 0; i < length; ++i) memcpy(table + 8 * i, &ctx->vfp.d[(n + i) & 31], 8);
    const uint64_t index = ctx->vfp.d[m], old = ctx->vfp.d[d];
    uint64_t r = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t x = (uint32_t)(index >> (8 * i)) & 0xFF;
        const uint64_t byte = x < 8 * length ? table[x] : (extend ? (old >> (8 * i)) & 0xFF : 0);
        r |= byte << (8 * i);
    }
    ctx->vfp.d[d] = r;
}
/* VDUP: a value replicated into every element. */
PSV_INLINE void psv_ndup(PsvContext* PSV_RESTRICT ctx, uint32_t esize, uint32_t regs, uint32_t d, uint64_t value) {
    PsvVec r = {{0, 0}};
    for (uint32_t i = 0; i < regs * 64 / esize; ++i) psv_set_el(&r, i, esize, value);
    psv_vput(ctx, d, regs, r);
}

#endif /* PSVRECORE_RECOMP_GUEST_SIMD_H */
