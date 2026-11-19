/* Synthetic corpus: Advanced SIMD (NEON) through the ACLE intrinsics: VLD1/VST1 of every element size at any
 * alignment (the 8-byte register path), interleaved loads and stores, float and integer lane arithmetic, widening
 * and narrowing, saturation, permutes, table lookups, estimates, conversions. See integer.c. */
#include <arm_neon.h>
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

/* @test neon_add_f32 r0=fptr r1=fptr r2=int:0:16 */
NOINLINE void neon_add_f32(float* a, const float* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) vst1q_f32(a + 4 * i, vaddq_f32(vld1q_f32(a + 4 * i), vld1q_f32(b + 4 * i)));
}

/* A 4x4 matrix times vectors (vmla by lane), as engines transform vertices. */
/* @test neon_mat4 r0=fptr r1=fptr r2=fptr r3=int:0:16 */
NOINLINE void neon_mat4(float* out, const float* m, const float* v, uint32_t n) {
    const float32x4_t c0 = vld1q_f32(m), c1 = vld1q_f32(m + 4), c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
    for (uint32_t i = 0; i < n; ++i) {
        const float32x4_t x = vld1q_f32(v + 4 * i);
        float32x4_t r = vmulq_lane_f32(c0, vget_low_f32(x), 0);
        r = vmlaq_lane_f32(r, c1, vget_low_f32(x), 1);
        r = vmlaq_lane_f32(r, c2, vget_high_f32(x), 0);
        r = vmlaq_lane_f32(r, c3, vget_high_f32(x), 1);
        vst1q_f32(out + 4 * i, r);
    }
}

/* Byte lanes at any alignment: vld1.8 / vst1.8. */
/* @test neon_blend_u8 r0=ptr r1=ptr r2=int:0:16 r3=any */
NOINLINE void neon_blend_u8(uint8_t* d, const uint8_t* s, uint32_t n, uint32_t alpha) {
    const uint8x8_t a = vdup_n_u8((uint8_t)alpha), ia = vdup_n_u8((uint8_t)(255 - alpha));
    for (uint32_t i = 0; i < n; ++i) {
        const uint16x8_t w = vmlal_u8(vmull_u8(vld1_u8(s + 8 * i), a), vld1_u8(d + 8 * i), ia);
        vst1_u8(d + 8 * i, vrshrn_n_u16(w, 8));
    }
}

/* Halfword and word lanes, saturating: vld1.16 / vld1.32. */
/* @test neon_qadd_s16 r0=ptr r1=ptr r2=int:0:16 */
NOINLINE void neon_qadd_s16(int16_t* d, const int16_t* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) vst1q_s16(d + 8 * i, vqaddq_s16(vld1q_s16(d + 8 * i), vshrq_n_s16(vld1q_s16(s + 8 * i), 1)));
}
/* @test neon_sum_u32 r0=ptr r1=int:0:32 */
NOINLINE uint32_t neon_sum_u32(const uint32_t* p, uint32_t n) {
    uint32x4_t acc = vdupq_n_u32(0);
    for (uint32_t i = 0; i < n; ++i) acc = vaddq_u32(acc, vld1q_u32(p + 4 * i));
    const uint32x2_t s = vpadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    return vget_lane_u32(vpadd_u32(s, s), 0);
}

/* 64-bit lanes: vld1.64 / vst1.64 (D registers, any alignment). */
/* @test neon_u64 r0=ptr r1=ptr r2=int:0:16 */
NOINLINE void neon_u64(uint64_t* d, const uint64_t* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) vst1_u64(d + i, vadd_u64(vld1_u64(d + i), vshl_n_u64(vld1_u64(s + i), 3)));
}

/* Interleaved structures: vld2, vld3, vld4 and vst2, vst3, vst4. */
/* @test neon_deinterleave r0=ptr r1=ptr r2=int:0:8 */
NOINLINE void neon_deinterleave(uint8_t* d, const uint8_t* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint8x8x3_t rgb = vld3_u8(s + 24 * i);
        uint8x8x4_t rgba;
        rgba.val[0] = rgb.val[2];
        rgba.val[1] = rgb.val[1];
        rgba.val[2] = rgb.val[0];
        rgba.val[3] = vdup_n_u8(0xFF);
        vst4_u8(d + 32 * i, rgba);
    }
}
/* @test neon_complex r0=fptr r1=fptr r2=int:0:8 */
NOINLINE void neon_complex(float* d, const float* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const float32x4x2_t a = vld2q_f32(s + 8 * i), b = vld2q_f32(d + 8 * i);
        float32x4x2_t r;
        r.val[0] = vsubq_f32(vmulq_f32(a.val[0], b.val[0]), vmulq_f32(a.val[1], b.val[1]));
        r.val[1] = vaddq_f32(vmulq_f32(a.val[0], b.val[1]), vmulq_f32(a.val[1], b.val[0]));
        vst2q_f32(d + 8 * i, r);
    }
}

/* Widening, narrowing, compares and selects, min and max. */
/* @test neon_threshold r0=ptr r1=ptr r2=int:0:16 r3=any */
NOINLINE void neon_threshold(uint8_t* d, const int16_t* s, uint32_t n, uint32_t t) {
    const int16x8_t th = vdupq_n_s16((int16_t)t);
    for (uint32_t i = 0; i < n; ++i) {
        const int16x8_t v = vld1q_s16(s + 8 * i);
        const uint16x8_t m = vcgtq_s16(v, th);
        const int16x8_t c = vmaxq_s16(vminq_s16(v, vdupq_n_s16(255)), vdupq_n_s16(0));
        vst1_u8(d + 8 * i, vmovn_u16(vbslq_u16(m, vreinterpretq_u16_s16(c), vdupq_n_u16(7))));
    }
}

/* Permutes and table lookups. */
/* @test neon_permute r0=ptr r1=ptr r2=ptr r3=int:0:8 */
NOINLINE void neon_permute(uint8_t* d, const uint8_t* s, const uint8_t* idx, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint8x8_t a = vld1_u8(s + 16 * i), b = vld1_u8(s + 16 * i + 8);
        const uint8x8x2_t z = vzip_u8(a, b);
        const uint8x8x2_t tbl = {{z.val[0], z.val[1]}};
        const uint8x8_t t = vtbl2_u8(tbl, vand_u8(vld1_u8(idx + 8 * i), vdup_n_u8(0x1F)));
        vst1_u8(d + 16 * i, vext_u8(t, vrev64_u8(a), 3));
        vst1_u8(d + 16 * i + 8, vtbx1_u8(b, a, vld1_u8(idx + 8 * i)));
    }
}

/* Float estimates and conversions, as normalisation and fixed-point code use them. */
/* @test neon_normalize r0=fptr r1=int:0:16 */
NOINLINE void neon_normalize(float* v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const float32x4_t x = vld1q_f32(v + 4 * i);
        float32x4_t e = vrsqrteq_f32(vmulq_f32(x, x));
        e = vmulq_f32(e, vrsqrtsq_f32(vmulq_f32(x, x), vmulq_f32(e, e)));
        float32x4_t r = vrecpeq_f32(x);
        r = vmulq_f32(r, vrecpsq_f32(x, r));
        vst1q_f32(v + 4 * i, vaddq_f32(vmulq_f32(x, e), r));
    }
}
/* @test neon_fixed r0=fptr r1=ptr r2=int:0:16 */
NOINLINE void neon_fixed(const float* s, int32_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const int32x4_t q = vcvtq_n_s32_f32(vld1q_f32(s + 4 * i), 8);
        const float32x4_t back = vcvtq_f32_s32(vshrq_n_s32(q, 4));
        vst1q_s32(d + 4 * i, vaddq_s32(q, vcvtq_s32_f32(back)));
    }
}

/* Lane moves between core and NEON registers (vmov.32 d[x], vdup from a core register), absolute differences. */
/* @test neon_sad r0=ptr r1=ptr r2=int:0:16 */
NOINLINE uint32_t neon_sad(const uint8_t* a, const uint8_t* b, uint32_t n) {
    uint16x8_t acc = vdupq_n_u16(0);
    for (uint32_t i = 0; i < n; ++i) acc = vabal_u8(acc, vld1_u8(a + 8 * i), vld1_u8(b + 8 * i));
    const uint32x4_t w = vpaddlq_u16(acc);
    return vgetq_lane_u32(w, 0) + vgetq_lane_u32(w, 1) + vgetq_lane_u32(w, 2) * 3 + vgetq_lane_u32(w, 3);
}
