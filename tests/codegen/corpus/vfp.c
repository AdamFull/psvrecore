/* Synthetic corpus: scalar floating point (VFPv3), single and double precision: arithmetic in loops, compares that
 * branch (vcmp, vmrs), conversions, square roots, arguments and results in s/d registers (hard-float calling
 * convention). See integer.c. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* @test dot3_f32 r0=fptr r1=fptr r2=int:0:40 */
NOINLINE float dot3_f32(const float* a, const float* b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; ++i) s += a[3 * i] * b[3 * i] + a[3 * i + 1] * b[3 * i + 1] + a[3 * i + 2] * b[3 * i + 2];
    return s;
}

/* @test poly_f32 s0=any r0=fptr r1=int:0:12 */
NOINLINE float poly_f32(float x, const float* c, uint32_t n) {
    float r = 0.0f;
    for (uint32_t i = 0; i < n; ++i) r = r * x + c[i];
    return r;
}

/* @test lerp_f32 r0=fptr r1=fptr r2=fptr r3=int:0:64 */
NOINLINE void lerp_f32(float* d, const float* a, const float* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) d[i] = a[i] + (b[i] - a[i]) * 0.25f;
}

/* @test clamp_f32 r0=fptr r1=int:0:64 s0=any s1=any */
NOINLINE uint32_t clamp_f32(float* p, uint32_t n, float lo, float hi) {
    uint32_t clamped = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (p[i] < lo) p[i] = lo, ++clamped;
        else if (p[i] > hi) p[i] = hi, ++clamped;
        else if (!(p[i] == p[i])) p[i] = 0.0f, clamped += 16;
    }
    return clamped;
}

/* @test convert_f32 r0=fptr r1=ptr:4 r2=int:0:64 */
NOINLINE int32_t convert_f32(const float* s, int32_t* d, uint32_t n) {
    int32_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = (int32_t)s[i];
        sum += (int32_t)(uint32_t)(s[i] * 3.0f) + (int32_t)(float)(int32_t)i;
    }
    return sum;
}

/* @test length_f32 r0=fptr r1=int:0:32 */
NOINLINE float length_f32(const float* v, uint32_t n) {
    float best = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float l = __builtin_sqrtf(v[2 * i] * v[2 * i] + v[2 * i + 1] * v[2 * i + 1]);
        if (l > best) best = l;
    }
    return best / (float)(n + 1);
}

/* @test div_abs_f32 s0=any s1=any s2=any */
NOINLINE float div_abs_f32(float a, float b, float c) {
    float r = a / b;
    r = __builtin_fabsf(r) - c;
    return -r * 2.0f + (a >= c ? 1.0f : -1.0f);
}

/* @test dot_f64 r0=dptr r1=dptr r2=int:0:32 */
NOINLINE double dot_f64(const double* a, const double* b, uint32_t n) {
    double s = 0.0;
    for (uint32_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

/* @test newton_f64 d0=any r0=int:0:8 */
NOINLINE double newton_f64(double x, uint32_t iterations) {
    double g = x > 1.0 ? x * 0.5 : 1.0;
    for (uint32_t i = 0; i < iterations; ++i) g = 0.5 * (g + x / g);
    return g - __builtin_sqrt(x < 0.0 ? -x : x);
}

/* @test mixed_precision r0=fptr r1=dptr r2=int:0:32 */
NOINLINE double mixed_precision(const float* f, double* d, uint32_t n) {
    double acc = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = (double)f[i] * 1.5 - d[i];
        acc += (float)d[i];
        if (d[i] < -1.0e10) acc -= 1.0;
    }
    return acc;
}

/* @test convert_f64 r0=dptr r1=ptr:4 r2=int:0:32 */
NOINLINE uint32_t convert_f64(const double* s, uint32_t* d, uint32_t n) {
    uint32_t x = 0;
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = (uint32_t)s[i];
        x ^= (uint32_t)(int32_t)(s[i] * -0.5) + (uint32_t)(double)(d[i] >> 3);
    }
    return x;
}

/* 2x2 matrices of doubles through memory, and a compare of doubles that selects. */
/* @test mat2_f64 r0=dptr r1=dptr r2=dptr r3=int:0:16 */
NOINLINE uint32_t mat2_f64(double* out, const double* a, const double* b, uint32_t n) {
    uint32_t neg = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const double* x = a + 4 * i;
        const double* y = b + 4 * i;
        double* o = out + 4 * i;
        o[0] = x[0] * y[0] + x[1] * y[2];
        o[1] = x[0] * y[1] + x[1] * y[3];
        o[2] = x[2] * y[0] + x[3] * y[2];
        o[3] = x[2] * y[1] + x[3] * y[3];
        neg += (o[0] * o[3] - o[1] * o[2]) < 0.0;
    }
    return neg;
}
