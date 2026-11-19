/* Synthetic corpus: integer code in the ARM instruction set (conditional execution, block loads and stores, a
 * switch through a table of addresses, multiply-accumulate). Built with -marm; see integer.c. */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* @test arm_checksum r0=ptr:4 r1=int:0:100 */
NOINLINE uint32_t arm_checksum(const uint32_t* p, uint32_t n) {
    uint32_t s = 0, t = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t v = p[i];
        s += v;
        t ^= (v << (i & 7)) | (s >> 3);
        if (v & 1) s = s * 5 + 1;
    }
    return s ^ t;
}

typedef struct Block {
    uint32_t w[8];
} Block;

/* Struct copies: ldm/stm. */
/* @test arm_copy_blocks r0=ptr:4 r1=ptr:4 r2=int:0:12 */
NOINLINE void arm_copy_blocks(Block* d, const Block* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        Block b = s[i];
        b.w[0] ^= i;
        b.w[7] += b.w[3];
        d[i] = b;
    }
}

/* @test arm_switch r0=int:0:16 r1=any r2=any */
NOINLINE int32_t arm_switch(uint32_t k, int32_t a, int32_t b) {
    switch (k) {
        case 0: return a + b;
        case 1: return a - b;
        case 2: return a * b;
        case 3: return a & b;
        case 4: return a | b;
        case 5: return a ^ b;
        case 6: return a << (b & 31);
        case 7: return a >> (b & 31);
        case 8: return (int32_t)((uint32_t)a >> (b & 31));
        case 9: return a > b ? a : b;
        case 10: return a < b ? a : b;
        case 11: return -a;
        default: return 42;
    }
}

/* @test arm_mla r0=ptr:4 r1=ptr:4 r2=int:0:64 */
NOINLINE uint64_t arm_mla(const uint32_t* a, const uint32_t* b, uint32_t n) {
    uint64_t acc = 0;
    uint32_t lo = 0;
    for (uint32_t i = 0; i < n; ++i) {
        acc += (uint64_t)a[i] * b[i];
        lo = lo * a[i] + b[i];
    }
    return acc ^ lo;
}

/* @test arm_minmax r0=ptr:4 r1=int:1:80 */
NOINLINE int32_t arm_minmax(const int32_t* p, uint32_t n) {
    int32_t lo = p[0], hi = p[0];
    uint32_t at = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (p[i] < lo) lo = p[i], at = i;
        if (p[i] > hi) hi = p[i];
    }
    return (hi - lo) ^ (int32_t)at;
}

/* @test arm_bytes_to_words r0=ptr r1=ptr:4 r2=int:0:64 */
NOINLINE void arm_bytes_to_words(const uint8_t* s, uint32_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) d[i] = s[4 * i] | (s[4 * i + 1] << 8) | (s[4 * i + 2] << 16) | ((uint32_t)s[4 * i + 3] << 24);
}
