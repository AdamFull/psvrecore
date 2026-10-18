/* Integer arithmetic: 32- and 64-bit operations (the compiler's division and 64-bit helpers included), shifts and
 * rotations, comparisons, bit manipulation, and loops the compiler optimizes (unrolling, strength reduction). */
#include "probe.h"

static volatile const uint32_t kValues[] = {
    0u, 1u, 2u, 3u, 7u, 31u, 32u, 100u, 255u, 256u, 1000u, 65535u, 65536u, 0x12345678u, 0x7FFFFFFFu, 0x80000000u,
    0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xDEADBEEFu, 0xCAFEBABEu, 0x00FF00FFu, 123456789u, 987654321u,
};
#define VALUE_COUNT (sizeof kValues / sizeof kValues[0])

static uint32_t rotl(uint32_t x, uint32_t n) { return n ? (x << n) | (x >> (32 - n)) : x; }

static uint32_t popcount(uint32_t x) {
    uint32_t n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}

static uint32_t leading_zeros(uint32_t x) {
    uint32_t n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        ++n;
    }
    return n;
}

static uint32_t reverse_bytes(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}

/* A CRC-32 (reflected, 0xEDB88320) over a buffer: shifts, conditionals, loops. */
static uint32_t crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t probe_main(uint32_t* out) {
    ProbeOut o = {out, 0};
    for (uint32_t i = 0; i < VALUE_COUNT; ++i) {
        const uint32_t a = kValues[i];
        const int32_t sa = (int32_t)a;
        put(&o, popcount(a));
        put(&o, leading_zeros(a));
        put(&o, reverse_bytes(a));
        put(&o, (uint32_t)(sa >> (i % 32)));
        put(&o, a >> (i % 32));
        put(&o, a << (i % 32));
        put(&o, rotl(a, i % 32));
        put(&o, (uint32_t)(int32_t)(int8_t)a);
        put(&o, (uint32_t)(int32_t)(int16_t)a);
        put(&o, sa < 0 ? 0u - a : a);  /* (negated as unsigned: -INT_MIN would be undefined) */
        for (uint32_t j = 0; j < VALUE_COUNT; ++j) {
            const uint32_t b = kValues[j];
            const int32_t sb = (int32_t)b;
            put(&o, a + b);
            put(&o, a - b);
            put(&o, a * b);
            put(&o, (uint32_t)(((uint64_t)a * b) >> 32));
            put(&o, (uint32_t)(((int64_t)sa * sb) >> 32));
            put(&o, (a < b) | ((sa < sb) << 1) | ((a == b) << 2) | ((a >= b) << 3) | ((sa > sb) << 4));
            put(&o, a > b ? a : b);
            put(&o, sa < sb ? (uint32_t)sa : (uint32_t)sb);
            put(&o, a & ~b);
            put(&o, a ^ (b >> 3));
            if (b != 0) {
                put(&o, a / b);
                put(&o, a % b);
                if (!(sa == (int32_t)0x80000000 && sb == -1)) {
                    put(&o, (uint32_t)(sa / sb));
                    put(&o, (uint32_t)(sa % sb));
                }
            }
            /* 64-bit */
            const uint64_t wa = ((uint64_t)a << 32) | b, wb = ((uint64_t)b << 17) ^ a;
            const int64_t swa = (int64_t)wa, swb = (int64_t)wb;
            put64(&o, wa + wb);
            put64(&o, wa - wb);
            put64(&o, wa * wb);
            put64(&o, wa >> (j % 64));
            put64(&o, wa << (j % 64));
            put64(&o, (uint64_t)(swa >> (i % 64)));
            put(&o, (wa < wb) | ((swa < swb) << 1));
            if (wb != 0) {
                put64(&o, wa / wb);
                put64(&o, wa % wb);
                if (!(swa == INT64_MIN && swb == -1)) {
                    put64(&o, (uint64_t)(swa / swb));
                    put64(&o, (uint64_t)(swa % swb));
                }
            }
        }
    }
    {
        uint8_t buffer[256];
        for (uint32_t i = 0; i < 256; ++i) buffer[i] = (uint8_t)(kValues[i % VALUE_COUNT] >> (i % 24));
        put(&o, crc32(buffer, 256));
        put(&o, crc32(buffer + 3, 77));
    }
    /* A multiply-accumulate loop and a strength-reduced index loop. */
    {
        uint32_t acc = 0;
        int64_t sacc = 0;
        for (uint32_t i = 0; i < 1000; ++i) {
            acc += kValues[i % VALUE_COUNT] * (i + 1);
            sacc += (int64_t)(int32_t)kValues[(i * 7) % VALUE_COUNT] * (int32_t)(i - 500);
        }
        put(&o, acc);
        put64(&o, (uint64_t)sacc);
    }
    return o.n;
}
