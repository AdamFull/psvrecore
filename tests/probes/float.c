/* Floating point in single and double precision: arithmetic (with special values and denormals), multiply-add
 * patterns, conversions to and from integers and between precisions, comparisons, and a few loops (polynomials,
 * a Newton iteration, a running sum). The guest runs with the IEEE mode (FPSCR 0), as the host does. */
#include "probe.h"

static volatile const uint32_t kFloats[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x40490FDBu, 0x3EAAAAABu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00800000u,
    0x00000001u, 0x807FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x4B000000u, 0x4B7FFFFFu, 0xCF000000u, 0x4F800000u,
    0x3F000000u, 0x42F6E979u, 0xC2F6E979u, 0x3DCCCCCDu, 0x501502F9u, 0x2F800000u,
};
#define FLOAT_COUNT (sizeof kFloats / sizeof kFloats[0])

static volatile const uint64_t kDoubles[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xC000000000000000ull, 0x400921FB54442D18ull,
    0x3FD5555555555555ull, 0x7FEFFFFFFFFFFFFFull, 0x0010000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x4330000000000000ull, 0x41E0000000000000ull,
    0xC1E0000000000000ull, 0x43E0000000000000ull, 0x3FB999999999999Aull, 0x3810000000000000ull, 0x36A0000000000000ull,
};
#define DOUBLE_COUNT (sizeof kDoubles / sizeof kDoubles[0])

static volatile const int32_t kInts[] = {0, 1, -1, 7, -7, 1000000, -1000000, 16777217, 0x7FFFFFFF, (int32_t)0x80000000};
#define INT_COUNT (sizeof kInts / sizeof kInts[0])

/* Saturating conversions written out (C leaves out-of-range conversions undefined). */
static int32_t to_int32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 0x7FFFFFFF;
    if (x <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)x;
}
static uint32_t to_uint32(double x) {
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)x;
}

uint32_t probe_main(uint32_t* out) {
    ProbeOut o = {out, 0};
    for (uint32_t i = 0; i < FLOAT_COUNT; ++i) {
        const float a = as_f32(kFloats[i]);
        putf(&o, -a);
        putf(&o, a < 0 ? -a : a);
        putf(&o, a * a);
        putd(&o, (double)a);
        put(&o, (uint32_t)to_int32(a));
        put(&o, to_uint32(a));
        for (uint32_t j = 0; j < FLOAT_COUNT; ++j) {
            const float b = as_f32(kFloats[j]);
            putf(&o, a + b);
            putf(&o, a - b);
            putf(&o, a * b);
            putf(&o, a / b);
            putf(&o, a * b + a);  /* (both compilers keep the product rounded) */
            putf(&o, a - a * b);
            put(&o, (a < b) | ((a <= b) << 1) | ((a == b) << 2) | ((a != b) << 3) | ((a > b) << 4) | ((a >= b) << 5));
            putf(&o, a > b ? a : b);
        }
    }
    for (uint32_t i = 0; i < DOUBLE_COUNT; ++i) {
        const double a = as_f64(kDoubles[i]);
        putd(&o, -a);
        putf(&o, (float)a);
        put(&o, (uint32_t)to_int32(a));
        put(&o, to_uint32(a));
        for (uint32_t j = 0; j < DOUBLE_COUNT; ++j) {
            const double b = as_f64(kDoubles[j]);
            putd(&o, a + b);
            putd(&o, a - b);
            putd(&o, a * b);
            putd(&o, a / b);
            putd(&o, a * b - b);
            put(&o, (a < b) | ((a <= b) << 1) | ((a == b) << 2) | ((a != b) << 3) | ((a > b) << 4) | ((a >= b) << 5));
        }
    }
    for (uint32_t i = 0; i < INT_COUNT; ++i) {
        const int32_t v = kInts[i];
        putf(&o, (float)v);
        putd(&o, (double)v);
        putf(&o, (float)(uint32_t)v);
        putd(&o, (double)(uint32_t)v);
    }
    /* Square roots by Newton's iteration, a polynomial (sin's Taylor series), a long running sum. */
    for (uint32_t i = 2; i < 12; ++i) {
        const double x = as_f64(kDoubles[i % DOUBLE_COUNT]);
        double r = x > 1.0 ? x : 1.0;
        for (int k = 0; k < 30; ++k) r = 0.5 * (r + x / r);
        putd(&o, r);
        const float t = (float)i * 0.25f;
        const float t2 = t * t;
        putf(&o, t * (1.0f - t2 / 6.0f * (1.0f - t2 / 20.0f * (1.0f - t2 / 42.0f))));
    }
    {
        float sum = 0.0f;
        double dsum = 0.0;
        for (uint32_t i = 1; i <= 10000; ++i) {
            sum += 1.0f / (float)i;
            dsum += 1.0 / ((double)i * (double)i);
        }
        putf(&o, sum);
        putd(&o, dsum);
    }
    return o.n;
}
