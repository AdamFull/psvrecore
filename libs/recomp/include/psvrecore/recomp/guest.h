/* The interface between recompiled code and the runtime: the guest CPU state, guest memory, the helpers generated
 * code calls for the operations C has no operator for, and the services the runtime provides (indirect calls,
 * imports, traps). C17 with Clang's builtins; header-only apart from the services.
 *
 * Every guest function becomes
 *
 *     void f_<address>(PsvContext* restrict ctx, uint32_t ret);
 *
 * where `ret` is the code address the function returns to. Its caller passes the return address it put in LR; a
 * tail call passes its own `ret` on. A branch through a register (bx lr, pop {pc}, ldr pc) to `ret` is a return;
 * to anything else it is a jump the runtime resolves, or reports.
 *
 * `restrict`: the context is never guest memory, so stores to guest memory cannot change the registers. The
 * compiler can keep registers in host registers between calls. */
#ifndef PSVRECORE_RECOMP_GUEST_H
#define PSVRECORE_RECOMP_GUEST_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#define PSV_RESTRICT __restrict
#else
#define PSV_RESTRICT restrict
#endif

#define PSV_INLINE static inline __attribute__((always_inline))
#define PSV_NORETURN __attribute__((noreturn))

/* ---- CPU state ---------------------------------------------------------------------------------------------- */

typedef struct PsvContext {
    uint32_t r[16];             /* r0-r14; r[15] is unused (the PC of recompiled code is static) */
    uint32_t n, z, c, v, q;     /* condition flags and the sticky saturation flag, 0 or 1 */
    uint32_t ge;                /* APSR.GE, bits 3-0 */
    uint32_t fpscr;             /* with its condition flags (bits 31-28) and QC (bit 27) */
    uint32_t reserved0;
    union {
        uint64_t d[32];         /* d0-d31; q<n> is d<2n> (low half) and d<2n+1> */
        uint32_t s[64];         /* s0-s31 alias d0-d15 (s<2n> the low half of d<n>) */
        uint16_t h[128];
        uint8_t b[256];
    } vfp;
    uint32_t tpidrurw;          /* CP15 software thread ID registers: user read/write */
    uint32_t tpidruro;          /* user read-only (written by the runtime) */
    /* The exclusive monitor of LDREX/STREX: the address and value last loaded exclusively. */
    uint32_t excl_address;
    uint32_t excl_size;         /* 0: no exclusive access outstanding */
    uint64_t excl_value;
    void* thread;               /* the runtime's */
} PsvContext;

typedef void (*PsvFunction)(PsvContext* PSV_RESTRICT ctx, uint32_t ret);

/* ---- Runtime services ----------------------------------------------------------------------------------------- */

/* Why recompiled code stopped. */
enum {
    PSV_TRAP_UNDEFINED = 1,      /* an undefined instruction (or a place discovery could not decode) */
    PSV_TRAP_UNPREDICTABLE,      /* an encoding the architecture leaves unpredictable */
    PSV_TRAP_UDF,                /* udf: a permanently undefined instruction (compilers' traps) */
    PSV_TRAP_BKPT,               /* bkpt */
    PSV_TRAP_UNIMPLEMENTED,      /* an instruction the code generator does not translate */
    PSV_TRAP_PRIVILEGED,         /* an instruction user code cannot execute */
    PSV_TRAP_UNDISCOVERED,       /* control reached code discovery did not follow */
    PSV_TRAP_RETURNED,           /* a function found never to return returned */
    PSV_TRAP_BAD_JUMP,           /* a jump table index outside the table */
    PSV_TRAP_FPSCR,              /* an FPSCR setting the runtime does not implement */
    PSV_TRAP_BIG_ENDIAN,         /* setend be */
};

PSV_NORETURN void psv_trap(PsvContext* ctx, uint32_t pc, uint32_t kind);
/* Calls the function at a code address (blx rN, and calls into the middle of nowhere). */
void psv_call(PsvContext* ctx, uint32_t target, uint32_t ret);
/* A jump through a register that is not a return: a tail call of the function at `target`, or a diagnostic. */
void psv_jump(PsvContext* ctx, uint32_t target, uint32_t ret, uint32_t from);
/* svc #imm */
void psv_svc(PsvContext* ctx, uint32_t pc, uint32_t imm);
/* wfe, yield: the thread may let others run. */
void psv_yield(PsvContext* ctx);

/* An imported function: the loader fills in `function` (a system library implementation, or another module's
 * export). */
typedef struct PsvImport {
    uint32_t library_nid;
    uint32_t nid;
    const char* library;
    PsvFunction function;
} PsvImport;

void psv_import_missing(PsvContext* ctx, const PsvImport* import, uint32_t ret);

PSV_INLINE void psv_call_import(PsvContext* PSV_RESTRICT ctx, const PsvImport* import, uint32_t ret) {
    if (__builtin_expect(import->function == 0, 0)) {
        psv_import_missing(ctx, import, ret);
        return;
    }
    import->function(ctx, ret);
}

/* An imported variable: the loader fills in its guest address. Generated code reads it where the module's code builds
 * the variable's address (movw/movt); the loader itself writes it into data that refers to the variable. */
typedef struct PsvVariable {
    uint32_t library_nid;
    uint32_t nid;
    const char* library;
    uint32_t address;
} PsvVariable;

/* A module's code, as generated: its functions by code address (sorted), and its imports. */
typedef struct PsvFunctionEntry {
    uint32_t address;
    PsvFunction function;
} PsvFunctionEntry;

typedef struct PsvModuleCode {
    const char* name;
    const PsvFunctionEntry* functions;
    uint32_t function_count;
    PsvImport* imports;
    uint32_t import_count;
    const uint32_t* import_stubs; /* the stub address of each import */
    PsvVariable* variables;
    uint32_t variable_count;
} PsvModuleCode;

/* ---- Guest memory ---------------------------------------------------------------------------------------------- */

/* The host address of guest address 0: the runtime reserves the whole 32-bit guest space there. */
extern uint8_t* psv_membase;

#ifdef PSV_ADDRESS_MASK /* (tests: every access folded into a small buffer) */
#define PSV_HOST(a) (psv_membase + ((uint32_t)(a) & (uint32_t)PSV_ADDRESS_MASK))
#else
#define PSV_HOST(a) (psv_membase + (uint32_t)(a))
#endif

PSV_INLINE uint32_t psv_ld8(uint32_t a) { return *PSV_HOST(a); }
PSV_INLINE uint32_t psv_ld16(uint32_t a) { uint16_t v; memcpy(&v, PSV_HOST(a), 2); return v; }
PSV_INLINE uint32_t psv_ld32(uint32_t a) { uint32_t v; memcpy(&v, PSV_HOST(a), 4); return v; }
#ifdef PSV_ADDRESS_MASK /* (two word accesses, each folded, as the interpreter makes them) */
PSV_INLINE uint64_t psv_ld64(uint32_t a) { return psv_ld32(a) | ((uint64_t)psv_ld32(a + 4) << 32); }
#else
PSV_INLINE uint64_t psv_ld64(uint32_t a) { uint64_t v; memcpy(&v, PSV_HOST(a), 8); return v; }
#endif
PSV_INLINE uint32_t psv_lds8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)*PSV_HOST(a); }
PSV_INLINE uint32_t psv_lds16(uint32_t a) { int16_t v; memcpy(&v, PSV_HOST(a), 2); return (uint32_t)(int32_t)v; }
PSV_INLINE void psv_st8(uint32_t a, uint32_t v) { *PSV_HOST(a) = (uint8_t)v; }
PSV_INLINE void psv_st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(PSV_HOST(a), &h, 2); }
PSV_INLINE void psv_st32(uint32_t a, uint32_t v) { memcpy(PSV_HOST(a), &v, 4); }
#ifdef PSV_ADDRESS_MASK
PSV_INLINE void psv_st64(uint32_t a, uint64_t v) { psv_st32(a, (uint32_t)v); psv_st32(a + 4, (uint32_t)(v >> 32)); }
#else
PSV_INLINE void psv_st64(uint32_t a, uint64_t v) { memcpy(PSV_HOST(a), &v, 8); }
#endif

/* Exclusive loads and stores. A store succeeds (returns 0) when the monitor holds this address and the memory still
 * holds the value the exclusive load saw; it is then written atomically. (A value changed and changed back in
 * between goes unnoticed, where the hardware would fail the store.) */
PSV_INLINE uint32_t psv_ldrex(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t size) {
    uint64_t v;
    switch (size) {
        case 1: v = __atomic_load_n((uint8_t*)PSV_HOST(a), __ATOMIC_SEQ_CST); break;
        case 2: v = __atomic_load_n((uint16_t*)PSV_HOST(a), __ATOMIC_SEQ_CST); break;
        default: v = __atomic_load_n((uint32_t*)PSV_HOST(a), __ATOMIC_SEQ_CST); break;
    }
    ctx->excl_address = a;
    ctx->excl_size = size;
    ctx->excl_value = v;
    return (uint32_t)v;
}
PSV_INLINE uint64_t psv_ldrexd(PsvContext* PSV_RESTRICT ctx, uint32_t a) {
    const uint64_t v = __atomic_load_n((uint64_t*)PSV_HOST(a), __ATOMIC_SEQ_CST);
    ctx->excl_address = a;
    ctx->excl_size = 8;
    ctx->excl_value = v;
    return v;
}
PSV_INLINE uint32_t psv_strex(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint64_t value, uint32_t size) {
    if (ctx->excl_size != size || ctx->excl_address != a) {
        ctx->excl_size = 0;
        return 1;
    }
    ctx->excl_size = 0;
    int ok;
    switch (size) {
        case 1: {
            uint8_t expected = (uint8_t)ctx->excl_value;
            ok = __atomic_compare_exchange_n((uint8_t*)PSV_HOST(a), &expected, (uint8_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            break;
        }
        case 2: {
            uint16_t expected = (uint16_t)ctx->excl_value;
            ok = __atomic_compare_exchange_n((uint16_t*)PSV_HOST(a), &expected, (uint16_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            break;
        }
        case 4: {
            uint32_t expected = (uint32_t)ctx->excl_value;
            ok = __atomic_compare_exchange_n((uint32_t*)PSV_HOST(a), &expected, (uint32_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            break;
        }
        default: {
            uint64_t expected = ctx->excl_value;
            ok = __atomic_compare_exchange_n((uint64_t*)PSV_HOST(a), &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            break;
        }
    }
    return ok ? 0u : 1u;
}
PSV_INLINE uint32_t psv_swp(uint32_t a, uint32_t value, uint32_t size) {
    if (size == 1) return __atomic_exchange_n((uint8_t*)PSV_HOST(a), (uint8_t)value, __ATOMIC_SEQ_CST);
    return __atomic_exchange_n((uint32_t*)PSV_HOST(a), value, __ATOMIC_SEQ_CST);
}
PSV_INLINE void psv_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* ---- Integer helpers ------------------------------------------------------------------------------------------ */

PSV_INLINE uint32_t psv_ror(uint32_t x, uint32_t n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
PSV_INLINE uint32_t psv_asr(uint32_t x, uint32_t n) { return (uint32_t)((int32_t)x >> (n > 31 ? 31 : n)); }

/* Shifts by a register (the amount is its bottom byte), value and carry out. type: 0 LSL, 1 LSR, 2 ASR, 3 ROR. */
PSV_INLINE uint32_t psv_shift_reg(uint32_t x, uint32_t type, uint32_t s) {
    s &= 0xFF;
    switch (type) {
        case 0: return s >= 32 ? 0u : x << s;
        case 1: return s >= 32 ? 0u : x >> s;
        case 2: return psv_asr(x, s);
        default: return psv_ror(x, s);
    }
}
PSV_INLINE uint32_t psv_shift_reg_c(uint32_t x, uint32_t type, uint32_t s, uint32_t carry) {
    s &= 0xFF;
    if (s == 0) return carry;
    switch (type) {
        case 0: return s > 32 ? 0u : (x >> (32 - s)) & 1u;
        case 1: return s > 32 ? 0u : (x >> (s - 1)) & 1u;
        case 2: return s >= 32 ? x >> 31 : (x >> (s - 1)) & 1u;
        default: return (psv_ror(x, s) >> 31) & 1u;
    }
}

PSV_INLINE void psv_nz(PsvContext* PSV_RESTRICT ctx, uint32_t r) {
    ctx->n = r >> 31;
    ctx->z = r == 0;
}
/* AddWithCarry: a + b + carry_in, setting NZCV. */
PSV_INLINE uint32_t psv_adds(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b, uint32_t carry_in) {
    const uint64_t wide = (uint64_t)a + b + carry_in;
    const uint32_t r = (uint32_t)wide;
    ctx->n = r >> 31;
    ctx->z = r == 0;
    ctx->c = (uint32_t)(wide >> 32);
    ctx->v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

PSV_INLINE uint32_t psv_clz(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }
PSV_INLINE uint32_t psv_rbit(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(x);
}
PSV_INLINE uint32_t psv_rev16(uint32_t x) { return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); }
PSV_INLINE uint32_t psv_revsh(uint32_t x) { return (uint32_t)(int32_t)(int16_t)(((x & 0xFF) << 8) | ((x >> 8) & 0xFF)); }

/* Saturation: to a signed range of `bits` bits (1-32) or unsigned (0-31), setting Q when it saturates. */
PSV_INLINE uint32_t psv_ssat(PsvContext* PSV_RESTRICT ctx, int64_t x, uint32_t bits) {
    const int64_t hi = (bits >= 64) ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1), lo = -hi - 1;
    if (x > hi) { ctx->q = 1; return (uint32_t)hi; }
    if (x < lo) { ctx->q = 1; return (uint32_t)lo; }
    return (uint32_t)x;
}
PSV_INLINE uint32_t psv_usat(PsvContext* PSV_RESTRICT ctx, int64_t x, uint32_t bits) {
    const int64_t hi = (int64_t)((1ull << bits) - 1);
    if (x > hi) { ctx->q = 1; return (uint32_t)hi; }
    if (x < 0) { ctx->q = 1; return 0; }
    return (uint32_t)x;
}
/* The same without touching Q (parallel saturating arithmetic). */
PSV_INLINE int32_t psv_sat_s(int64_t x, uint32_t bits) {
    const int64_t hi = (int64_t)((1ull << (bits - 1)) - 1), lo = -hi - 1;
    return (int32_t)(x > hi ? hi : x < lo ? lo : x);
}
PSV_INLINE uint32_t psv_sat_u(int64_t x, uint32_t bits) {
    const int64_t hi = (int64_t)((1ull << bits) - 1);
    return (uint32_t)(x > hi ? hi : x < 0 ? 0 : x);
}
PSV_INLINE uint32_t psv_ssat16(PsvContext* PSV_RESTRICT ctx, uint32_t x, uint32_t bits) {
    const uint32_t lo = psv_ssat(ctx, (int16_t)x, bits) & 0xFFFF, hi = psv_ssat(ctx, (int16_t)(x >> 16), bits) & 0xFFFF;
    return lo | (hi << 16);
}
PSV_INLINE uint32_t psv_usat16(PsvContext* PSV_RESTRICT ctx, uint32_t x, uint32_t bits) {
    const uint32_t lo = psv_usat(ctx, (int16_t)x, bits), hi = psv_usat(ctx, (int16_t)(x >> 16), bits);
    return lo | (hi << 16);
}

PSV_INLINE uint32_t psv_qadd(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b) { return psv_ssat(ctx, (int64_t)(int32_t)a + (int32_t)b, 32); }
PSV_INLINE uint32_t psv_qsub(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b) { return psv_ssat(ctx, (int64_t)(int32_t)a - (int32_t)b, 32); }
PSV_INLINE uint32_t psv_qdadd(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b) {
    const uint32_t doubled = psv_ssat(ctx, 2 * (int64_t)(int32_t)b, 32);
    return psv_ssat(ctx, (int64_t)(int32_t)a + (int32_t)doubled, 32);
}
PSV_INLINE uint32_t psv_qdsub(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b) {
    const uint32_t doubled = psv_ssat(ctx, 2 * (int64_t)(int32_t)b, 32);
    return psv_ssat(ctx, (int64_t)(int32_t)a - (int32_t)doubled, 32);
}

/* Parallel addition and subtraction. `kind`: 0 signed modular (sets GE), 1 signed saturating, 2 signed halving,
 * 3 unsigned modular (sets GE), 4 unsigned saturating, 5 unsigned halving.
 * `op`: 0 add16, 1 asx, 2 sax, 3 sub16, 4 add8, 5 sub8. */
PSV_INLINE uint32_t psv_parallel(PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b, uint32_t kind, uint32_t op) {
    const int is_signed = kind < 3;
    const uint32_t mode = kind % 3;
    uint32_t result = 0, ge = 0;
    const int lanes = op >= 4 ? 4 : 2;
    const uint32_t width = op >= 4 ? 8 : 16;
    const uint32_t mask = (1u << width) - 1;
    for (int i = 0; i < lanes; ++i) {
        int64_t x, y;
        int sub;
        int src_a = i, src_b = i;
        if (op == 1) sub = i == 0, src_b = 1 - i;       /* asx: lo = a.lo - b.hi, hi = a.hi + b.lo */
        else if (op == 2) sub = i == 1, src_b = 1 - i;  /* sax: lo = a.lo + b.hi, hi = a.hi - b.lo */
        else sub = op == 3 || op == 5;
        const uint32_t ea = (a >> (src_a * width)) & mask, eb = (b >> (src_b * width)) & mask;
        if (is_signed) {
            x = width == 8 ? (int8_t)ea : (int16_t)ea;
            y = width == 8 ? (int8_t)eb : (int16_t)eb;
        } else {
            x = ea;
            y = eb;
        }
        const int64_t r = sub ? x - y : x + y;
        uint32_t lane;
        if (mode == 0) {
            lane = (uint32_t)r & mask;
            const int ge_bit = is_signed ? r >= 0 : (sub ? r >= 0 : r >= (int64_t)(1u << width));
            if (ge_bit) ge |= (width == 8 ? 1u : 3u) << (i * (width / 8));
        } else if (mode == 1) {
            lane = (is_signed ? (uint32_t)psv_sat_s(r, width) : psv_sat_u(r, width)) & mask;
        } else {
            lane = (uint32_t)(r >> 1) & mask;
        }
        result |= lane << (i * width);
    }
    if (mode == 0) ctx->ge = ge;
    return result;
}
PSV_INLINE uint32_t psv_sel(const PsvContext* PSV_RESTRICT ctx, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (((ctx->ge >> i) & 1) ? a : b) & (0xFFu << (8 * i));
    return r;
}
PSV_INLINE uint32_t psv_usad8(uint32_t a, uint32_t b) {
    uint32_t s = 0;
    for (int i = 0; i < 4; ++i) {
        const int32_t x = (int32_t)((a >> (8 * i)) & 0xFF) - (int32_t)((b >> (8 * i)) & 0xFF);
        s += (uint32_t)(x < 0 ? -x : x);
    }
    return s;
}

/* Halfword and dual multiplies. */
PSV_INLINE int32_t psv_half(uint32_t x, int top) { return top ? (int16_t)(x >> 16) : (int16_t)x; }
/* Signed 32-bit accumulation that sets Q on overflow (smlabb, smlad, ...). */
PSV_INLINE uint32_t psv_add_q(PsvContext* PSV_RESTRICT ctx, int64_t x) {
    if (x != (int32_t)x) ctx->q = 1;
    return (uint32_t)x;
}

/* ---- Status registers ---------------------------------------------------------------------------------------- */

/* The APSR as MRS reads it in user mode: N Z C V Q, GE, and the mode field (user). */
PSV_INLINE uint32_t psv_read_apsr(const PsvContext* PSV_RESTRICT ctx) {
    return (ctx->n << 31) | (ctx->z << 30) | (ctx->c << 29) | (ctx->v << 28) | (ctx->q << 27) | (ctx->ge << 16) | 0x10u;
}
/* MSR APSR_<fields>: mask bit 3 the flags (nzcvq), bit 2 GE; other fields are not writable in user mode. */
PSV_INLINE void psv_write_apsr(PsvContext* PSV_RESTRICT ctx, uint32_t value, uint32_t mask) {
    if (mask & 8) {
        ctx->n = value >> 31;
        ctx->z = (value >> 30) & 1;
        ctx->c = (value >> 29) & 1;
        ctx->v = (value >> 28) & 1;
        ctx->q = (value >> 27) & 1;
    }
    if (mask & 4) ctx->ge = (value >> 16) & 0xF;
}

/* ---- Floating point (VFPv3) ---------------------------------------------------------------------------------------
 *
 * Values travel as their bits. Each operation takes the FPSCR that governs it: the context's for VFP instructions,
 * PSV_FPSCR_STANDARD for Advanced SIMD ones (flush-to-zero and default NaN, always).
 *
 * Implemented exactly: round to nearest, flush-to-zero (inputs and results, tininess before rounding, as the VFP
 * detects it), default NaN, the VFP's NaN propagation and its default NaN (positive, unlike x86's). Binary32
 * operations are computed in binary64: exact for add, subtract and multiply, and correctly rounded for divide and
 * square root (binary64's precision exceeds twice binary32's plus two bits, so the second rounding cannot change
 * the result). Other rounding modes, short vectors and the alternative half-precision format trap when FPSCR is
 * written. Cumulative exception flags are not computed (they keep what software wrote). */

#define PSV_FPSCR_FZ 0x01000000u
#define PSV_FPSCR_DN 0x02000000u
#define PSV_FPSCR_AHP 0x04000000u
#define PSV_FPSCR_QC 0x08000000u
#define PSV_FPSCR_STANDARD (PSV_FPSCR_FZ | PSV_FPSCR_DN)
/* The bits an FPSCR write keeps (the trap enables are not implemented by the Cortex-A9 and read as zero). */
#define PSV_FPSCR_WRITABLE 0xFFF7009Fu
/* Settings that trap when written: a rounding mode other than nearest, LEN and STRIDE (short vectors), AHP. */
#define PSV_FPSCR_UNSUPPORTED 0x04F70000u

#define PSV_DEFAULT_NAN32 0x7FC00000u
#define PSV_DEFAULT_NAN64 0x7FF8000000000000ull

PSV_INLINE float psv_f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
PSV_INLINE uint32_t psv_b32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
PSV_INLINE double psv_f64(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
PSV_INLINE uint64_t psv_b64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

PSV_INLINE int psv_nan32(uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
PSV_INLINE int psv_snan32(uint32_t x) { return psv_nan32(x) && !(x & 0x00400000u); }
PSV_INLINE int psv_nan64(uint64_t x) { return (x & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
PSV_INLINE int psv_snan64(uint64_t x) { return psv_nan64(x) && !(x & 0x0008000000000000ull); }

/* Flush-to-zero of an input: a denormal becomes a zero of its sign. */
PSV_INLINE uint32_t psv_flush32(uint32_t fpscr, uint32_t x) {
    return ((fpscr & PSV_FPSCR_FZ) && (x & 0x7F800000u) == 0) ? (x & 0x80000000u) : x;
}
PSV_INLINE uint64_t psv_flush64(uint32_t fpscr, uint64_t x) {
    return ((fpscr & PSV_FPSCR_FZ) && (x & 0x7FF0000000000000ull) == 0) ? (x & 0x8000000000000000ull) : x;
}

/* FPProcessNaN(s): a signalling NaN first, then a quiet one, the first operand before the second; quietened, or the
 * default NaN in DN mode. */
PSV_INLINE uint32_t psv_nan_result32(uint32_t fpscr, uint32_t x) { return (fpscr & PSV_FPSCR_DN) ? PSV_DEFAULT_NAN32 : (x | 0x00400000u); }
PSV_INLINE uint64_t psv_nan_result64(uint32_t fpscr, uint64_t x) {
    return (fpscr & PSV_FPSCR_DN) ? PSV_DEFAULT_NAN64 : (x | 0x0008000000000000ull);
}
PSV_INLINE uint32_t psv_nans32(uint32_t fpscr, uint32_t a, uint32_t b) {
    if (psv_snan32(a)) return psv_nan_result32(fpscr, a);
    if (psv_snan32(b)) return psv_nan_result32(fpscr, b);
    return psv_nan_result32(fpscr, psv_nan32(a) ? a : b);
}
PSV_INLINE uint64_t psv_nans64(uint32_t fpscr, uint64_t a, uint64_t b) {
    if (psv_snan64(a)) return psv_nan_result64(fpscr, a);
    if (psv_snan64(b)) return psv_nan_result64(fpscr, b);
    return psv_nan_result64(fpscr, psv_nan64(a) ? a : b);
}

/* A binary32 result from its value in binary64 (exact, or innocuously rounded; see above). */
PSV_INLINE uint32_t psv_round32(uint32_t fpscr, double r) {
    if (r != r) return PSV_DEFAULT_NAN32;  /* (NaN operands were handled: this is an invalid operation) */
    if ((fpscr & PSV_FPSCR_FZ) && r != 0.0 && __builtin_fabs(r) < 0x1p-126) return r < 0 ? 0x80000000u : 0u;
    return psv_b32((float)r);
}

/* A binary64 result r = round(x) for the exact result x, in flush-to-zero mode: the VFP flushes when |x| is below
 * the smallest normal before rounding, so a result rounded up to exactly the smallest normal needs the sign of
 * x - r. `below` is that test, done only in this rare case. */
PSV_INLINE uint64_t psv_flush_result64(double r, int below) {
    const double m = __builtin_fabs(r);
    if (r != 0.0 && (m < 0x1p-1022 || (m == 0x1p-1022 && below))) return r < 0 ? 0x8000000000000000ull : 0;
    return psv_b64(r);
}

#define PSV_VFP_BINARY32(name, expr)                                                                  \
    PSV_INLINE uint32_t name(uint32_t fpscr, uint32_t a, uint32_t b) {                                \
        a = psv_flush32(fpscr, a);                                                                    \
        b = psv_flush32(fpscr, b);                                                                    \
        if (psv_nan32(a) || psv_nan32(b)) return psv_nans32(fpscr, a, b);                             \
        const double x = (double)psv_f32(a), y = (double)psv_f32(b);                                   \
        return psv_round32(fpscr, expr);                                                              \
    }
PSV_VFP_BINARY32(psv_fadd32, x + y)
PSV_VFP_BINARY32(psv_fsub32, x - y)
PSV_VFP_BINARY32(psv_fmul32, x * y)
PSV_VFP_BINARY32(psv_fdiv32, x / y)
#undef PSV_VFP_BINARY32

PSV_INLINE uint32_t psv_fsqrt32(uint32_t fpscr, uint32_t a) {
    a = psv_flush32(fpscr, a);
    if (psv_nan32(a)) return psv_nan_result32(fpscr, a);
    return psv_round32(fpscr, __builtin_sqrt((double)psv_f32(a)));
}

PSV_INLINE uint64_t psv_fadd64(uint32_t fpscr, uint64_t a, uint64_t b) {
    a = psv_flush64(fpscr, a);
    b = psv_flush64(fpscr, b);
    if (psv_nan64(a) || psv_nan64(b)) return psv_nans64(fpscr, a, b);
    const double x = psv_f64(a), y = psv_f64(b), r = x + y;
    if (r != r) return PSV_DEFAULT_NAN64;
    if (!(fpscr & PSV_FPSCR_FZ)) return psv_b64(r);
    int below = 0;
    if (__builtin_fabs(r) == 0x1p-1022) {  /* TwoSum: the exact error of the sum */
        const double bv = r - x, err = (x - (r - bv)) + (y - bv);
        below = r > 0 ? err < 0 : err > 0;
    }
    return psv_flush_result64(r, below);
}
PSV_INLINE uint64_t psv_fsub64(uint32_t fpscr, uint64_t a, uint64_t b) {
    a = psv_flush64(fpscr, a);
    b = psv_flush64(fpscr, b);
    if (psv_nan64(a) || psv_nan64(b)) return psv_nans64(fpscr, a, b);
    return psv_fadd64(fpscr, a, b ^ 0x8000000000000000ull);  /* (no NaN left: negating b is exact) */
}
PSV_INLINE uint64_t psv_fmul64(uint32_t fpscr, uint64_t a, uint64_t b) {
    a = psv_flush64(fpscr, a);
    b = psv_flush64(fpscr, b);
    if (psv_nan64(a) || psv_nan64(b)) return psv_nans64(fpscr, a, b);
    const double x = psv_f64(a), y = psv_f64(b), r = x * y;
    if (r != r) return PSV_DEFAULT_NAN64;
    if (!(fpscr & PSV_FPSCR_FZ)) return psv_b64(r);
    int below = 0;
    if (__builtin_fabs(r) == 0x1p-1022) {
        /* Scaled up so nothing underflows: the product and its exact error. */
        const double xs = x * 0x1p200, p = xs * y, err = __builtin_fma(xs, y, -p), lim = 0x1p-822;
        const double mp = __builtin_fabs(p);
        below = mp < lim || (mp == lim && (p > 0 ? err < 0 : err > 0));
    }
    return psv_flush_result64(r, below);
}
PSV_INLINE uint64_t psv_fdiv64(uint32_t fpscr, uint64_t a, uint64_t b) {
    a = psv_flush64(fpscr, a);
    b = psv_flush64(fpscr, b);
    if (psv_nan64(a) || psv_nan64(b)) return psv_nans64(fpscr, a, b);
    const double x = psv_f64(a), y = psv_f64(b), r = x / y;
    if (r != r) return PSV_DEFAULT_NAN64;
    if (!(fpscr & PSV_FPSCR_FZ)) return psv_b64(r);
    int below = 0;
    if (__builtin_fabs(r) == 0x1p-1022) {
        /* (|x| is small here, so scaling it cannot overflow) The quotient scaled up and its exact residual. */
        const double xs = x * 0x1p200, q = xs / y, rem = __builtin_fma(-q, y, xs), lim = 0x1p-822;
        const double mq = __builtin_fabs(q);
        /* exact - q = rem / y: the exact quotient is smaller in magnitude when that has the opposite sign to q */
        const int diff_negative = rem != 0 && ((rem < 0) != (y < 0));
        const int diff_positive = rem != 0 && !diff_negative;
        below = mq < lim || (mq == lim && (q > 0 ? diff_negative : diff_positive));
    }
    return psv_flush_result64(r, below);
}
PSV_INLINE uint64_t psv_fsqrt64(uint32_t fpscr, uint64_t a) {
    a = psv_flush64(fpscr, a);
    if (psv_nan64(a)) return psv_nan_result64(fpscr, a);
    const double r = __builtin_sqrt(psv_f64(a));
    return r != r ? PSV_DEFAULT_NAN64 : psv_b64(r);  /* (a square root is never tiny) */
}

/* VFP compare: the FPSCR flags N Z C V (bits 31-28) for less, equal, greater, unordered. */
PSV_INLINE uint32_t psv_fcmp32(uint32_t fpscr, uint32_t a, uint32_t b) {
    a = psv_flush32(fpscr, a);
    b = psv_flush32(fpscr, b);
    if (psv_nan32(a) || psv_nan32(b)) return 0x30000000u;
    const float x = psv_f32(a), y = psv_f32(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : 0x20000000u;
}
PSV_INLINE uint32_t psv_fcmp64(uint32_t fpscr, uint64_t a, uint64_t b) {
    a = psv_flush64(fpscr, a);
    b = psv_flush64(fpscr, b);
    if (psv_nan64(a) || psv_nan64(b)) return 0x30000000u;
    const double x = psv_f64(a), y = psv_f64(b);
    return x == y ? 0x60000000u : x < y ? 0x80000000u : 0x20000000u;
}

/* Precision conversions. A NaN keeps its sign and the top of its payload, quietened (or becomes the default NaN). */
PSV_INLINE uint64_t psv_f32_to_f64(uint32_t fpscr, uint32_t a) {
    a = psv_flush32(fpscr, a);
    if (psv_nan32(a))
        return (fpscr & PSV_FPSCR_DN) ? PSV_DEFAULT_NAN64
                                      : ((uint64_t)(a >> 31) << 63) | 0x7FF8000000000000ull | ((uint64_t)(a & 0x3FFFFFu) << 29);
    return psv_b64((double)psv_f32(a));
}
PSV_INLINE uint32_t psv_f64_to_f32(uint32_t fpscr, uint64_t a) {
    a = psv_flush64(fpscr, a);
    if (psv_nan64(a))
        return (fpscr & PSV_FPSCR_DN) ? PSV_DEFAULT_NAN32 : ((uint32_t)(a >> 63) << 31) | 0x7FC00000u | (uint32_t)((a >> 29) & 0x3FFFFFu);
    return psv_round32(fpscr, psv_f64(a));
}

/* To integers: NaN gives 0; out-of-range values saturate. `nearest`: round to nearest even (vcvtr), else toward zero. */
PSV_INLINE uint32_t psv_to_int(double x, int is_signed, int nearest) {
    if (x != x) return 0;
    const double t = nearest ? __builtin_rint(x) : __builtin_trunc(x);
    if (is_signed) {
        if (t >= 2147483647.0) return 0x7FFFFFFFu;
        if (t <= -2147483648.0) return 0x80000000u;
        return (uint32_t)(int32_t)t;
    }
    if (t >= 4294967295.0) return 0xFFFFFFFFu;
    if (t <= 0.0) return 0;
    return (uint32_t)t;
}
PSV_INLINE uint32_t psv_f32_to_int(uint32_t fpscr, uint32_t a, int is_signed, int nearest) {
    return psv_to_int((double)psv_f32(psv_flush32(fpscr, a)), is_signed, nearest);
}
PSV_INLINE uint32_t psv_f64_to_int(uint32_t fpscr, uint64_t a, int is_signed, int nearest) {
    return psv_to_int(psv_f64(psv_flush64(fpscr, a)), is_signed, nearest);
}
PSV_INLINE uint32_t psv_int_to_f32(uint32_t x, int is_signed) {
    return psv_b32(is_signed ? (float)(int32_t)x : (float)x);
}
PSV_INLINE uint64_t psv_int_to_f64(uint32_t x, int is_signed) {
    return psv_b64(is_signed ? (double)(int32_t)x : (double)x);
}

/* Fixed point (`bits` 16 or 32, `fraction` bits after the point): toward zero, saturated, sign- or zero-extended
 * to 32 bits; and back, rounded to nearest. */
PSV_INLINE uint32_t psv_to_fixed(double x, uint32_t bits, uint32_t fraction, int is_signed) {
    if (x != x) return 0;
    const double t = __builtin_trunc(x * __builtin_ldexp(1.0, (int)fraction));
    if (is_signed) {
        const double hi = __builtin_ldexp(1.0, (int)bits - 1) - 1, lo = -__builtin_ldexp(1.0, (int)bits - 1);
        const int64_t v = t >= hi ? (int64_t)hi : t <= lo ? (int64_t)lo : (int64_t)t;
        return (uint32_t)(int32_t)v;
    }
    const double hi = __builtin_ldexp(1.0, (int)bits) - 1;
    return (uint32_t)(t >= hi ? hi : t <= 0.0 ? 0.0 : t);
}
PSV_INLINE uint32_t psv_f32_to_fixed(uint32_t fpscr, uint32_t a, uint32_t bits, uint32_t fraction, int is_signed) {
    return psv_to_fixed((double)psv_f32(psv_flush32(fpscr, a)), bits, fraction, is_signed);
}
PSV_INLINE uint32_t psv_f64_to_fixed(uint32_t fpscr, uint64_t a, uint32_t bits, uint32_t fraction, int is_signed) {
    return psv_to_fixed(psv_f64(psv_flush64(fpscr, a)), bits, fraction, is_signed);
}
PSV_INLINE double psv_fixed_value(uint32_t x, uint32_t bits, uint32_t fraction, int is_signed) {
    double v;
    if (bits == 16) v = is_signed ? (double)(int16_t)x : (double)(uint16_t)x;
    else v = is_signed ? (double)(int32_t)x : (double)x;
    return v * __builtin_ldexp(1.0, -(int)fraction);  /* (exact) */
}
PSV_INLINE uint32_t psv_fixed_to_f32(uint32_t fpscr, uint32_t x, uint32_t bits, uint32_t fraction, int is_signed) {
    return psv_round32(fpscr, psv_fixed_value(x, bits, fraction, is_signed));
}
PSV_INLINE uint64_t psv_fixed_to_f64(uint32_t fpscr, uint32_t x, uint32_t bits, uint32_t fraction, int is_signed) {
    const double v = psv_fixed_value(x, bits, fraction, is_signed);
    /* (exact unless tiny: a 32-bit value with up to 32 fraction bits is never below 2^-32) */
    (void)fpscr;
    return psv_b64(v);
}

/* Half precision (IEEE format; the alternative format traps when FPSCR selects it). Half-precision results keep their
 * denormals; flush-to-zero applies to single-precision inputs only. */
PSV_INLINE uint32_t psv_round_shift(uint32_t v, uint32_t shift) {
    if (shift == 0) return v;
    if (shift > 31) return 0;
    const uint32_t q = v >> shift, rem = v & ((1u << shift) - 1), half = 1u << (shift - 1);
    return (rem > half || (rem == half && (q & 1))) ? q + 1 : q;
}
PSV_INLINE uint32_t psv_f32_to_f16(uint32_t fpscr, uint32_t a) {
    a = psv_flush32(fpscr, a);
    const uint32_t sign = (a >> 16) & 0x8000u, exp = (a >> 23) & 0xFF, man = a & 0x7FFFFFu;
    if (exp == 0xFF) {
        if (man == 0) return sign | 0x7C00u;
        return (fpscr & PSV_FPSCR_DN) ? 0x7E00u : sign | 0x7E00u | ((man >> 13) & 0x1FFu);
    }
    if (exp == 0) return sign;  /* (a single-precision denormal is far below half precision's range) */
    const int e = (int)exp - 127;
    const uint32_t m24 = man | 0x800000u;
    if (e >= -14) {
        uint32_t r = psv_round_shift(m24, 13);
        int ee = e;
        if (r == 2048) r = 1024, ++ee;
        if (ee > 15) return sign | 0x7C00u;
        return sign | ((uint32_t)(ee + 15) << 10) | (r & 0x3FFu);
    }
    const int shift = -1 - e;  /* the value in units of 2^-24 is m24 >> shift */
    if (shift > 25) return sign;
    return sign | psv_round_shift(m24, (uint32_t)shift);  /* (1024: the smallest normal, encoded as such) */
}
PSV_INLINE uint32_t psv_f16_to_f32(uint32_t fpscr, uint32_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FFu;
    if (exp == 0x1F) {
        if (man == 0) return sign | 0x7F800000u;
        return (fpscr & PSV_FPSCR_DN) ? PSV_DEFAULT_NAN32 : sign | 0x7FC00000u | (man << 13);
    }
    if (exp == 0) {
        if (man == 0) return sign;
        return sign | psv_b32((float)man * 0x1p-24f);  /* (exact) */
    }
    return sign | ((exp - 15 + 127) << 23) | (man << 13);
}

/* VMSR FPSCR: settings this implementation does not support trap; the trap enables read as zero. */
PSV_INLINE void psv_write_fpscr(PsvContext* PSV_RESTRICT ctx, uint32_t value, uint32_t pc) {
    if (value & PSV_FPSCR_UNSUPPORTED) psv_trap(ctx, pc, PSV_TRAP_FPSCR);
    ctx->fpscr = value & PSV_FPSCR_WRITABLE;
}

#include "psvrecore/recomp/guest_simd.h"

#ifdef __cplusplus
}
#endif

#endif /* PSVRECORE_RECOMP_GUEST_H */
