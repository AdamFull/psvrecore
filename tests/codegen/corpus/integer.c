/* Synthetic corpus: integer code in Thumb-2 (loops, byte and word memory traffic, 64-bit arithmetic, selects that
 * become IT blocks, switches that become TBB/TBH tables, an exclusive-access loop).
 *
 * Built by scripts/codegen_corpus.py (clang, armv7-a Thumb-2, -O2, freestanding). Each `@test` line names a function
 * the differential test runs and how its arguments are drawn (the script's docstring has the syntax). */
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* @test sum_bytes r0=ptr r1=int:0:300 */
NOINLINE uint32_t sum_bytes(const uint8_t* p, uint32_t n) {
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < n; ++i) {
        a += p[i];
        b += a;
        if (a >= 65521) a -= 65521;
        if (b >= 65521) b -= 65521;
    }
    return (b << 16) | a;
}

/* @test copy_bytes r0=ptr r1=ptr r2=int:0:200 */
NOINLINE void copy_bytes(uint8_t* restrict d, const uint8_t* restrict s, uint32_t n) {
    while (n--) *d++ = *s++;
}

/* @test fill_words r0=ptr:4 r1=any r2=int:0:100 */
NOINLINE void fill_words(uint32_t* d, uint32_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) d[i] = v ^ (i * 0x9E3779B9u);
}

/* @test popcount_words r0=ptr:4 r1=int:0:64 */
NOINLINE uint32_t popcount_words(const uint32_t* p, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = p[i];
        x = x - ((x >> 1) & 0x55555555u);
        x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
        x = (x + (x >> 4)) & 0x0F0F0F0Fu;
        c += (x * 0x01010101u) >> 24;
    }
    return c;
}

/* @test insertion_sort r0=ptr:4 r1=int:0:40 */
NOINLINE void insertion_sort(int32_t* a, uint32_t n) {
    for (uint32_t i = 1; i < n; ++i) {
        int32_t v = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
}

/* @test bounded_strlen r0=ptr r1=int:0:256 */
NOINLINE uint32_t bounded_strlen(const char* s, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s[n] != 0 && s[n] != 0x55) ++n;
    return n;
}

/* @test mul64 r0=any r1=any r2=any r3=any */
NOINLINE uint64_t mul64(uint64_t a, uint64_t b) { return a * b + (a >> 7) - (b << 3); }

/* @test mac64 r0=ptr:4 r1=ptr:4 r2=int:0:64 */
NOINLINE int64_t mac64(const int32_t* a, const int32_t* b, uint32_t n) {
    int64_t acc = 0;
    for (uint32_t i = 0; i < n; ++i) acc += (int64_t)a[i] * b[i];
    return acc;
}

/* @test shifts r0=any r1=int:0:40 r2=any */
NOINLINE uint32_t shifts(uint32_t x, uint32_t s, uint32_t y) {
    uint32_t r = (x << (s & 31)) | (x >> ((32 - s) & 31));
    r ^= (uint32_t)((int32_t)y >> (s & 31));
    r += y >> (s & 15);
    return r;
}

/* @test clamp_select r0=any r1=any r2=any */
NOINLINE int32_t clamp_select(int32_t x, int32_t lo, int32_t hi) {
    if (lo > hi) {
        int32_t t = lo;
        lo = hi;
        hi = t;
    }
    int32_t r = x < lo ? lo : x > hi ? hi : x;
    return r + (x == 0 ? 7 : 0) - ((uint32_t)x > 1000u ? 3 : 0);
}

/* @test small_switch r0=int:0:12 r1=any */
NOINLINE int32_t small_switch(uint32_t k, int32_t y) {
    switch (k) {
        case 0: return y + 1;
        case 1: return y * 3;
        case 2: return y - 7;
        case 3: return y ^ 0x5A5A;
        case 4: return y << 2;
        case 5: return y >> 1;
        case 6: return 9;
        case 7: return -y;
        case 8: return y & 0xFF;
        default: return 0;
    }
}

/* A switch whose cases are far apart: TBH. */
/* @test large_switch r0=int:0:20 r1=any r2=any */
NOINLINE int32_t large_switch(uint32_t k, int32_t a, int32_t b) {
    int32_t r = 0;
    switch (k) {
        case 0: r = a + b; for (int i = 0; i < 3; ++i) r = r * 33 + (a >> i); break;
        case 1: r = a - b; for (int i = 0; i < 4; ++i) r = r * 17 ^ (b << i); break;
        case 2: r = a * b; for (int i = 0; i < 5; ++i) r += (a ^ b) >> i; break;
        case 3: r = a & b; for (int i = 0; i < 6; ++i) r = (r << 1) | ((a >> i) & 1); break;
        case 4: r = a | b; for (int i = 0; i < 7; ++i) r -= (b * i) ^ a; break;
        case 5: r = a ^ b; for (int i = 0; i < 3; ++i) r += r >> 3; break;
        case 6: r = (a << 3) - b; for (int i = 0; i < 9; ++i) r ^= r << 5; break;
        case 7: r = (b << 2) + a; for (int i = 0; i < 2; ++i) r *= 0x45D9F3B; break;
        case 8: r = a > b ? a : b; for (int i = 0; i < 8; ++i) r += i * a; break;
        case 9: r = a < b ? a : b; for (int i = 0; i < 4; ++i) r -= i * b; break;
        case 10: r = (uint32_t)a >> 5; for (int i = 0; i < 6; ++i) r = r * 3 + 1; break;
        case 11: r = b >> 7; for (int i = 0; i < 5; ++i) r = r * 5 - a; break;
        case 12: r = ~a; for (int i = 0; i < 4; ++i) r = (r >> 1) ^ (-(r & 1) & 0xEDB88320); break;
        case 13: r = ~b; for (int i = 0; i < 8; ++i) r = (r >> 1) ^ (-(r & 1) & 0x82F63B78); break;
        default: r = -1; break;
    }
    return r;
}

static const uint32_t crc_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

/* A table in read-only data, read at run time. */
/* @test crc32_nibbles r0=ptr r1=int:0:200 r2=any */
NOINLINE uint32_t crc32_nibbles(const uint8_t* p, uint32_t n, uint32_t crc) {
    crc = ~crc;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = crc_table[crc & 15] ^ (crc >> 4);
        crc = crc_table[crc & 15] ^ (crc >> 4);
    }
    return ~crc;
}

/* Halfword and signed loads and stores, post-increment addressing. */
/* @test widen_halves r0=ptr:2 r1=ptr:4 r2=int:0:80 */
NOINLINE void widen_halves(const int16_t* s, int32_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        int32_t v = s[i];
        d[i] = v * 3 - (int8_t)(v >> 4);
    }
}

/* An exclusive-access (ldrex/strex) loop with barriers. */
/* @test atomic_add r0=ptr:4 r1=any r2=int:1:6 */
NOINLINE uint32_t atomic_add(uint32_t* p, uint32_t v, uint32_t times) {
    uint32_t last = 0;
    for (uint32_t i = 0; i < times; ++i) last = __atomic_fetch_add(p, v + i, __ATOMIC_SEQ_CST);
    return last;
}

/* Bit-field instructions (ubfx, sbfx, bfi, bfc), clz, rbit, rev. */
/* @test bitfields r0=any r1=any */
NOINLINE uint32_t bitfields(uint32_t x, uint32_t y) {
    uint32_t a = (x >> 5) & 0x3FF;
    int32_t b = ((int32_t)(y << 7)) >> 20;
    uint32_t c = (x & ~0x00FF0000u) | ((y & 0xFF) << 16);
    return a + (uint32_t)b + c + (uint32_t)__builtin_clz(x | 1) + __builtin_bswap32(y) + __builtin_bitreverse32(x ^ y);
}

/* Saturating arithmetic as compilers write it, and unsigned compare chains. */
/* @test saturate_add r0=ptr:2 r1=ptr:2 r2=int:0:64 */
NOINLINE void saturate_add(int16_t* d, const int16_t* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        int32_t v = d[i] + s[i];
        d[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* Struct traffic through a pointer: ldrd/strd and offset addressing. */
typedef struct Particle {
    int32_t x, y, vx, vy;
    uint32_t life;
} Particle;

/* @test step_particles r0=ptr:4 r1=int:0:24 */
NOINLINE uint32_t step_particles(Particle* p, uint32_t n) {
    uint32_t alive = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (p[i].life == 0) continue;
        p[i].x += p[i].vx;
        p[i].y += p[i].vy;
        p[i].vy -= 9;
        if (p[i].y < 0) {
            p[i].y = -p[i].y;
            p[i].vy = -p[i].vy / 2;
        }
        --p[i].life;
        ++alive;
    }
    return alive;
}
