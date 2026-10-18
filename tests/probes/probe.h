/* Same-source probes: each probe is one C file, built twice: for the Vita with the SDK's compiler (then recompiled
 * by psvrecomp and run by runner.cpp), and natively with the host compiler. Both runs write their results as words;
 * scripts/probe_check.py builds both and the runner compares them.
 *
 * A probe defines
 *
 *     uint32_t probe_main(uint32_t* out);   (returns the number of words written)
 *
 * and takes its inputs from volatile tables so neither compiler can compute the results at compile time. It uses
 * no library function the two sides would implement differently. */
#ifndef PSVRECORE_PROBE_H
#define PSVRECORE_PROBE_H

#include <stdint.h>

typedef struct ProbeOut {
    uint32_t* w;
    uint32_t n;
} ProbeOut;

static void put(ProbeOut* o, uint32_t v) { o->w[o->n++] = v; }
static void put64(ProbeOut* o, uint64_t v) {
    put(o, (uint32_t)v);
    put(o, (uint32_t)(v >> 32));
}

typedef union {
    float f;
    uint32_t u;
} ProbeF32;
typedef union {
    double f;
    uint64_t u;
} ProbeF64;

/* Floating-point results as bits. A NaN is written as one canonical pattern: which NaN an operation produces
 * differs between processors (the default NaN is positive on ARM, negative on x86), and the probes compare values. */
static void putf(ProbeOut* o, float f) {
    ProbeF32 x;
    x.f = f;
    put(o, f != f ? 0x7FC00000u : x.u);
}
static void putd(ProbeOut* o, double f) {
    ProbeF64 x;
    x.f = f;
    put64(o, f != f ? 0x7FF8000000000000ull : x.u);
}
static float as_f32(uint32_t u) {
    ProbeF32 x;
    x.u = u;
    return x.f;
}
static double as_f64(uint64_t u) {
    ProbeF64 x;
    x.u = u;
    return x.f;
}

uint32_t probe_main(uint32_t* out);

#endif
