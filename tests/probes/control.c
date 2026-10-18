/* Control flow the code generator must get right: dense and sparse switches (table branches), calls through
 * function pointers, recursion, variadic functions, structures passed and returned by value, bit fields, and loops
 * with early exits. */
#include <stdarg.h>

#include "probe.h"

static volatile const uint32_t kInputs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 31, 63, 100, 255, 1000, 4096};
#define INPUT_COUNT (sizeof kInputs / sizeof kInputs[0])

/* Dense: a table branch. */
static uint32_t dense(uint32_t x) {
    switch (x) {
        case 0: return 11;
        case 1: return 22;
        case 2: return 3 * x + 1;
        case 3: return x << 4;
        case 4: return 0xABCD;
        case 5: return x * x;
        case 6: return 7;
        case 7: return 0;
        case 8: return 1;
        case 9: return 99;
        case 10: return 42;
        case 11: return x ^ 0x55;
        case 12: return 12345;
        case 13: return x + 1000;
        case 14: return 14;
        case 15: return 0xFFFF;
        default: return 0xDEAD;
    }
}

/* Larger and with gaps (tbh, or a compare tree). */
static uint32_t sparse(uint32_t x) {
    switch (x) {
        case 1: return 1;
        case 3: return 9;
        case 7: return 49;
        case 16: return 256;
        case 31: return 961;
        case 63: return 3969;
        case 100: return 10000;
        case 255: return 65025;
        case 1000: return 1000000;
        case 4096: return 16777216;
        default: return x + 1;
    }
}

typedef uint32_t (*Op)(uint32_t, uint32_t);
static uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
static uint32_t op_sub(uint32_t a, uint32_t b) { return a - b; }
static uint32_t op_mul(uint32_t a, uint32_t b) { return a * b; }
static uint32_t op_xor(uint32_t a, uint32_t b) { return a ^ b; }
static Op volatile kOps[] = {op_add, op_sub, op_mul, op_xor};

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint32_t ackermann(uint32_t m, uint32_t n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static uint32_t sum_varargs(uint32_t count, ...) {
    va_list ap;
    va_start(ap, count);
    uint32_t s = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i % 3 == 2) s += (uint32_t)(va_arg(ap, double) * 10.0);
        else s += va_arg(ap, uint32_t) * (i + 1);
    }
    va_end(ap);
    return s;
}

typedef struct {
    uint32_t a;
    uint16_t b;
    uint8_t c;
    float d;
    uint64_t e;
} Record;

static Record make_record(uint32_t x) {
    Record r;
    r.a = x * 3;
    r.b = (uint16_t)(x + 7);
    r.c = (uint8_t)(x ^ 0xA5);
    r.d = (float)x * 0.5f;
    r.e = (uint64_t)x << 33;
    return r;
}
static uint32_t record_hash(Record r) {
    ProbeF32 f;
    f.f = r.d;
    return r.a * 31 + r.b * 7 + r.c + f.u + (uint32_t)(r.e >> 32);
}

typedef struct {
    unsigned low : 3;
    unsigned mid : 9;
    signed high : 7;
    unsigned flag : 1;
} Bits;

uint32_t probe_main(uint32_t* out) {
    ProbeOut o = {out, 0};
    for (uint32_t i = 0; i < INPUT_COUNT; ++i) {
        const uint32_t x = kInputs[i];
        put(&o, dense(x));
        put(&o, sparse(x));
        for (uint32_t k = 0; k < 4; ++k) put(&o, kOps[k](x, i + 3));
        put(&o, record_hash(make_record(x)));
        Bits b;
        b.low = x;
        b.mid = x * 5;
        b.high = (int)x - 40;
        b.flag = x & 1;
        put(&o, b.low | (b.mid << 3) | ((uint32_t)(int32_t)b.high << 12) | (b.flag << 31));
        /* a search loop with an early exit */
        uint32_t found = 0xFFFFFFFFu;
        for (uint32_t k = 0; k < INPUT_COUNT; ++k)
            if (kInputs[k] > x * 2) {
                found = k;
                break;
            }
        put(&o, found);
    }
    put(&o, fib(20));
    put(&o, ackermann(2, 3));
    put(&o, sum_varargs(6, 1u, 2u, 3.25, 4u, 5u, 6.5));
    return o.n;
}
