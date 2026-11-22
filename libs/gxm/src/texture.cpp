#include "psvrecore/gxm/texture.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

#include "core/foundation/strings/format.h"
#include "psvrecore/gxm/spirv.hpp"
#include "spirv/unified1/spirv.h"

namespace psvrecore::gxm {

namespace {

u32 log2_ceil(u32 v) { return v <= 1 ? 0 : 32 - static_cast<u32>(std::countl_zero(v - 1)); }
u32 pow2_ceil(u32 v) { return 1u << log2_ceil(v); }
bool is_cube(u32 type) { return type == texture_type::kCube || type == texture_type::kCubeArbitrary; }
bool is_swizzled(u32 type) { return type == texture_type::kSwizzled || type == texture_type::kSwizzledArbitrary || is_cube(type); }
bool is_linear(u32 type) { return type == texture_type::kLinear || type == texture_type::kLinearStrided; }

// Base formats (SceGxmTextureBaseFormat >> 24, with the extension bit as 0x80).
enum : u32 {
    kU8 = 0x00, kS8 = 0x01, kU4U4U4U4 = 0x02, kU8U3U3U2 = 0x03, kU1U5U5U5 = 0x04, kU5U6U5 = 0x05, kS5S5U6 = 0x06,
    kU8U8 = 0x07, kS8S8 = 0x08, kU16 = 0x09, kS16 = 0x0A, kF16 = 0x0B, kU8U8U8U8 = 0x0C, kS8S8S8S8 = 0x0D,
    kU2U10U10U10 = 0x0E, kU16U16 = 0x0F, kS16S16 = 0x10, kF16F16 = 0x11, kF32 = 0x12, kF32M = 0x13, kX8S8S8U8 = 0x14,
    kX8U24 = 0x15, kU32 = 0x17, kS32 = 0x18, kSE5M9M9M9 = 0x19, kF11F11F10 = 0x1A, kF16F16F16F16 = 0x1B,
    kU16U16U16U16 = 0x1C, kS16S16S16S16 = 0x1D, kF32F32 = 0x1E, kU32U32 = 0x1F, kPVRT2 = 0x80, kPVRT4 = 0x81,
    kPVRTII2 = 0x82, kPVRTII4 = 0x83, kUBC1 = 0x85, kUBC2 = 0x86, kUBC3 = 0x87, kUBC4 = 0x88, kSBC4 = 0x89, kUBC5 = 0x8A,
    kSBC5 = 0x8B, kYUV420P2 = 0x90, kYUV420P3 = 0x91, kYUV422 = 0x92, kP4 = 0x94, kP8 = 0x95, kU8U8U8 = 0x98,
    kS8S8S8 = 0x99, kU2F10F10F10 = 0x9A,
};

u32 code_of(u32 base_format) { return ((base_format >> 24) & 0x1F) | ((base_format >> 31) << 7); }

bool block_format(u32 code) { return code >= kPVRT2 && code <= kSBC5; }
// Texels per block (width, height) of a block format.
void block_size(u32 code, u32& bw, u32& bh) {
    bw = code == kPVRT2 || code == kPVRTII2 ? 8 : 4;
    bh = 4;
}

f32 half_to_float(u16 h) {
    const u32 sign = (u32{h} & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1F, mant = h & 0x3FFu;
    if (exp == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<f32>(mant), -24);
    if (exp == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (mant << 13));
    return std::bit_cast<f32>(sign | ((exp + 112) << 23) | (mant << 13));
}

u16 float_to_half(f32 v) {
    const u32 x = std::bit_cast<u32>(v);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 e = static_cast<i32>((x >> 23) & 0xFF) - 112;
    u32 mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<u16>(sign | 0x7C00u | (mant ? 0x200u : 0));
    if (e >= 31) return static_cast<u16>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<u16>(sign);
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        return static_cast<u16>(sign | ((mant + (1u << (shift - 1))) >> shift));
    }
    return static_cast<u16>(sign | ((static_cast<u32>(e) << 10) | (mant >> 13)) + ((mant >> 12) & 1));
}

// The RGBA of four raw components (c0 the lowest bits), for a swizzle mode and a component count.
void swizzle(const f32 c[4], u32 components, u32 mode, f32 out[4]) {
    auto set = [&](f32 r, f32 g, f32 b, f32 a) { out[0] = r, out[1] = g, out[2] = b, out[3] = a; };
    switch (components) {
        case 1:
            switch (mode) {
                case 1: set(c[0], 0, 0, 0); break;  // 000R
                case 2: set(c[0], 1, 1, 1); break;  // 111R
                case 3: set(c[0], c[0], c[0], c[0]); break;  // RRRR
                case 4: set(c[0], c[0], c[0], 0); break;  // 0RRR
                case 5: set(c[0], c[0], c[0], 1); break;  // 1RRR
                case 6: set(0, 0, 0, c[0]); break;  // R000
                case 7: set(1, 1, 1, c[0]); break;  // R111
                default: set(c[0], 0, 0, 1); break;  // R
            }
            break;
        case 2:
            switch (mode) {
                case 1: set(c[0], c[1], 0, 0); break;  // 00GR
                case 2: set(c[0], c[0], c[0], c[1]); break;  // GRRR
                case 3: set(c[1], c[1], c[1], c[0]); break;  // RGGG
                case 4: set(c[0], c[1], c[0], c[1]); break;  // GRGR
                case 5: set(c[1], c[0], 0, 0); break;  // 00RG
                default: set(c[0], c[1], 0, 1); break;  // GR
            }
            break;
        case 3:
            if (mode == 1) set(c[2], c[1], c[0], 1);  // RGB
            else set(c[0], c[1], c[2], 1);           // BGR
            break;
        default:
            switch (mode) {
                case 1: set(c[2], c[1], c[0], c[3]); break;  // ARGB
                case 2: set(c[3], c[2], c[1], c[0]); break;  // RGBA
                case 3: set(c[1], c[2], c[3], c[0]); break;  // BGRA
                case 4: set(c[0], c[1], c[2], 1); break;  // 1BGR
                case 5: set(c[2], c[1], c[0], 1); break;  // 1RGB
                case 6: set(c[3], c[2], c[1], 1); break;  // RGB1
                case 7: set(c[1], c[2], c[3], 1); break;  // BGR1
                default: set(c[0], c[1], c[2], c[3]); break;  // ABGR
            }
            break;
    }
}

f32 unorm(u32 v, u32 bits) { return static_cast<f32>(v) / static_cast<f32>((1u << bits) - 1); }
f32 snorm(u32 v, u32 bits) {
    const i32 s = static_cast<i32>(v << (32 - bits)) >> (32 - bits);
    return nx::max(static_cast<f32>(s) / static_cast<f32>((1u << (bits - 1)) - 1), -1.0f);
}

// A texel's raw components and their count, from its bits (`texel`: up to 64 bits, little endian).
bool components(u32 code, u32 mode, u64 texel, f32 c[4], u32& count, bool& wide) {
    c[0] = c[1] = c[2] = 0, c[3] = 1;
    wide = false;
    auto packed = [&](std::initializer_list<u32> widths, bool sgn) {
        u32 at = 0, k = 0;
        for (u32 w : widths) {
            const u32 v = static_cast<u32>((texel >> at) & ((u64{1} << w) - 1));
            c[k++] = sgn ? snorm(v, w) : unorm(v, w);
            at += w;
        }
        count = k;
    };
    // (formats whose alpha is one bit or two: its place depends on the swizzle, A first or last in the name)
    const bool alpha_low = mode == 2 || mode == 3 || mode == 6 || mode == 7;
    switch (code) {
        case kU8: packed({8}, false); return true;
        case kS8: packed({8}, true); return true;
        case kU8U8: packed({8, 8}, false); return true;
        case kS8S8: packed({8, 8}, true); return true;
        case kU8U8U8: packed({8, 8, 8}, false); return true;
        case kS8S8S8: packed({8, 8, 8}, true); return true;
        case kU8U8U8U8: packed({8, 8, 8, 8}, false); return true;
        case kS8S8S8S8: packed({8, 8, 8, 8}, true); return true;
        case kU4U4U4U4: packed({4, 4, 4, 4}, false); return true;
        case kU5U6U5: packed({5, 6, 5}, false); return true;
        case kS5S5U6: packed({6, 5, 5}, false); return true;
        case kU1U5U5U5: alpha_low ? packed({1, 5, 5, 5}, false) : packed({5, 5, 5, 1}, false); return true;
        case kU2U10U10U10: alpha_low ? packed({2, 10, 10, 10}, false) : packed({10, 10, 10, 2}, false); wide = true; return true;
        case kU2F10F10F10: {
            // (unsigned 10-bit floats: a half's exponent and top five mantissa bits; alpha two bits, as U2U10U10U10's)
            const u32 v = static_cast<u32>(texel);
            const u32 at = alpha_low ? 2 : 0;
            for (u32 k = 0; k < 3; ++k) c[k] = half_to_float(static_cast<u16>(((v >> (at + 10 * k)) & 0x3FF) << 5));
            c[3] = unorm(alpha_low ? v & 3 : v >> 30, 2);
            count = 4;
            wide = true;
            return true;
        }
        case kF11F11F10: {
            // (unsigned floats of 11, 11 and 10 bits from bit 0: a half's exponent and top 6 or 5 mantissa bits)
            const u32 v = static_cast<u32>(texel);
            c[0] = half_to_float(static_cast<u16>((v & 0x7FF) << 4));
            c[1] = half_to_float(static_cast<u16>(((v >> 11) & 0x7FF) << 4));
            c[2] = half_to_float(static_cast<u16>(((v >> 22) & 0x3FF) << 5));
            count = 3;
            wide = true;
            return true;
        }
        case kU8U3U3U2: packed({2, 3, 3, 8}, false); return true;
        case kU16: packed({16}, false); wide = true; return true;
        // (a D24 depth surface read as a texture: the depth in the low 24 bits, the stencil or nothing above)
        case kX8U24: packed({24, 8}, false); wide = true; return true;
        case kS16: packed({16}, true); wide = true; return true;
        case kU16U16: packed({16, 16}, false); wide = true; return true;
        case kS16S16: packed({16, 16}, true); wide = true; return true;
        case kU16U16U16U16: packed({16, 16, 16, 16}, false); wide = true; return true;
        case kS16S16S16S16: packed({16, 16, 16, 16}, true); wide = true; return true;
        case kF16:
        case kF16F16:
        case kF16F16F16F16: {
            count = code == kF16 ? 1 : code == kF16F16 ? 2 : 4;
            for (u32 k = 0; k < count; ++k) c[k] = half_to_float(static_cast<u16>(texel >> (16 * k)));
            wide = true;
            return true;
        }
        case kF32:
        case kF32M:
        case kF32F32: {
            count = code == kF32F32 ? 2 : 1;
            // (F32M: the sign bit is a mask, as in DF32M depth surfaces: not part of the value)
            const u32 sign = code == kF32M ? 0x7FFFFFFFu : 0xFFFFFFFFu;
            for (u32 k = 0; k < count; ++k) c[k] = std::bit_cast<f32>(static_cast<u32>(texel >> (32 * k)) & sign);
            wide = true;
            return true;
        }
        default: return false;
    }
}

// A BC4 block (BC5's two): 4x4 values from two 8-bit endpoints and 3-bit indices, as BC3's alpha; signed ones
// (SBC4, SBC5) from two's-complement endpoints, -128 read as -127, to [-1, 1].
void decode_bc4(const u8* block, bool sign, f32 out[16]) {
    const auto value = [&](u8 b) {
        return sign ? nx::max(static_cast<f32>(static_cast<i8>(b)), -127.0f) / 127.0f : unorm(b, 8);
    };
    const f32 v0 = value(block[0]), v1 = value(block[1]);
    const bool eight = sign ? static_cast<i8>(block[0]) > static_cast<i8>(block[1]) : block[0] > block[1];
    f32 v[8] = {v0, v1};
    for (u32 k = 2; k < 8; ++k)
        v[k] = eight ? ((8 - k) * v0 + (k - 1) * v1) / 7 : k < 6 ? ((6 - k) * v0 + (k - 1) * v1) / 5 : k == 6 ? (sign ? -1.0f : 0.0f) : 1.0f;
    u64 bits = 0;
    for (u32 k = 0; k < 6; ++k) bits |= u64{block[2 + k]} << (8 * k);
    for (u32 i = 0; i < 16; ++i) out[i] = v[(bits >> (3 * i)) & 7];
}

// The BC1-BC5 blocks: 4x4 texels as RGBA floats (BC4 and BC5: their components through the swizzle `mode`).
void decode_bc(u32 code, const u8* block, f32 out[16][4], u32 mode = 0) {
    if (code >= kUBC4 && code <= kSBC5) {
        const bool sign = code == kSBC4 || code == kSBC5, two = code == kUBC5 || code == kSBC5;
        f32 r[16], g[16] = {};
        decode_bc4(block, sign, r);
        if (two) decode_bc4(block + 8, sign, g);
        for (u32 i = 0; i < 16; ++i) {
            const f32 c[4] = {r[i], g[i], 0, 0};
            swizzle(c, two ? 2 : 1, mode, out[i]);
        }
        return;
    }
    const u8* color = code == kUBC1 ? block : block + 8;
    const u16 c0 = static_cast<u16>(color[0] | color[1] << 8), c1 = static_cast<u16>(color[2] | color[3] << 8);
    f32 pal[4][4];
    auto rgb565 = [](u16 v, f32* o) { o[0] = unorm(v >> 11, 5), o[1] = unorm((v >> 5) & 63, 6), o[2] = unorm(v & 31, 5), o[3] = 1; };
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    const bool four = code != kUBC1 || c0 > c1;
    for (u32 k = 0; k < 3; ++k) {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
    }
    pal[2][3] = 1;
    pal[3][3] = four ? 1.0f : 0.0f;
    const u32 bits = static_cast<u32>(color[4] | color[5] << 8 | color[6] << 16 | color[7] << 24);
    for (u32 i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], sizeof out[i]);
    if (code == kUBC2) {
        for (u32 i = 0; i < 16; ++i) out[i][3] = unorm((block[i / 2] >> (4 * (i & 1))) & 15, 4);
    } else if (code == kUBC3) {
        const f32 a0 = unorm(block[0], 8), a1 = unorm(block[1], 8);
        f32 a[8] = {a0, a1};
        for (u32 k = 2; k < 8; ++k)
            a[k] = block[0] > block[1] ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? 0.0f : 1.0f);
        u64 abits = 0;
        for (u32 k = 0; k < 6; ++k) abits |= u64{block[2 + k]} << (8 * k);
        for (u32 i = 0; i < 16; ++i) out[i][3] = a[(abits >> (3 * i)) & 7];
    }
}


u8 to8(f32 v) { return static_cast<u8>(std::lround(nx::clamp(std::isnan(v) ? 0.0f : v, 0.0f, 1.0f) * 255.0f)); }

// decode_bc's colour palettes as 8-bit components, for each pair of 5- or 6-bit components: its floats (the same
// arithmetic) rounded once, made once. (Its arithmetic for each block took a phone a third of a song's start.)
struct BcTables {
    u8 c5[32], c6[64];
    u8 third5[32][32], third6[64][64];  // (2a + b) / 3; (a + 2b) / 3 is third[b][a]: the sum is the same float
    u8 half5[32][32], half6[64][64];    // (a + b) / 2
    BcTables() {
        for (u32 a = 0; a < 64; ++a) {
            if (a < 32) c5[a] = to8(unorm(a, 5));
            c6[a] = to8(unorm(a, 6));
            for (u32 b = 0; b < 64; ++b) {
                if (a < 32 && b < 32) {
                    third5[a][b] = to8((2 * unorm(a, 5) + unorm(b, 5)) / 3);
                    half5[a][b] = to8((unorm(a, 5) + unorm(b, 5)) / 2);
                }
                third6[a][b] = to8((2 * unorm(a, 6) + unorm(b, 6)) / 3);
                half6[a][b] = to8((unorm(a, 6) + unorm(b, 6)) / 2);
            }
        }
    }
};

const BcTables& bc_tables() {
    static const BcTables t;
    return t;
}

// A BC1-BC3 block as 8-bit RGBA: decode_bc's palette and alphas (the same arithmetic), each rounded once as the
// general path rounds each texel's.
void decode_bc8(u32 code, const u8* block, u8 out[16][4]) {
    const BcTables& t = bc_tables();
    const u8* color = code == kUBC1 ? block : block + 8;
    const u16 c0 = static_cast<u16>(color[0] | color[1] << 8), c1 = static_cast<u16>(color[2] | color[3] << 8);
    const u32 r0 = c0 >> 11, g0 = (c0 >> 5) & 63, b0 = c0 & 31, r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    const bool four = code != kUBC1 || c0 > c1;
    const u8 pal8[4][4] = {
        {t.c5[r0], t.c6[g0], t.c5[b0], 255},
        {t.c5[r1], t.c6[g1], t.c5[b1], 255},
        {four ? t.third5[r0][r1] : t.half5[r0][r1], four ? t.third6[g0][g1] : t.half6[g0][g1],
         four ? t.third5[b0][b1] : t.half5[b0][b1], 255},
        {four ? t.third5[r1][r0] : u8{0}, four ? t.third6[g1][g0] : u8{0}, four ? t.third5[b1][b0] : u8{0},
         four ? u8{255} : u8{0}},
    };
    const u32 bits = static_cast<u32>(color[4] | color[5] << 8 | color[6] << 16 | color[7] << 24);
    for (u32 i = 0; i < 16; ++i) std::memcpy(out[i], pal8[(bits >> (2 * i)) & 3], 4);
    if (code == kUBC2) {
        for (u32 i = 0; i < 16; ++i) out[i][3] = static_cast<u8>(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);  // (n / 15, exact)
    } else if (code == kUBC3) {
        const f32 a0 = unorm(block[0], 8), a1 = unorm(block[1], 8);
        u8 a8[8] = {block[0], block[1]};  // (n / 255 rounds back to n)
        for (u32 k = 2; k < 8; ++k)
            a8[k] = to8(block[0] > block[1] ? ((8 - k) * a0 + (k - 1) * a1) / 7
                        : k < 6             ? ((6 - k) * a0 + (k - 1) * a1) / 5
                                            : (k == 6 ? 0.0f : 1.0f));
        u64 abits = 0;
        for (u32 k = 0; k < 6; ++k) abits |= u64{block[2 + k]} << (8 * k);
        for (u32 i = 0; i < 16; ++i) out[i][3] = a8[(abits >> (3 * i)) & 7];
    }
}

// PVRTC 4bpp, the first and second generation (PVRTC-I, PVRTC-II): blocks of 4x4 texels, 64 bits each: the texels'
// 2-bit modulation indices in the low word (row by row), and in the high word two colours C0 and C1 and the flags:
// bit 32 M (the modulation mode); PVRTC-I: bit 47 C0 opaque, bit 63 C1 opaque; PVRTC-II: bit 47 H, bit 63 both
// colours opaque. Opaque colours are R5G5B4 (C0) and R5G5B5 (C1), translucent ones A3R4G4B3 and A3R4G4B4, widened
// to 8 bits (PVRTC-II gives C1's alpha a low bit of 1, so it reaches 255).
//
// A texel's colours come from the four blocks around it (P, Q, R, S: their centres the corners of the square the
// texel lies in, wrapping at the edges), at (s, t) from P's centre. What it is depends on its own block's H and M:
// - 0, 0: C0 and C1 interpolated between the four blocks, mixed by the index's weight (0, 3/8, 5/8, 1);
// - 0, 1: the same colours, the index 0 C0, 1 their mean, 2 their mean with alpha 0 (PVRTC-II: transparent black),
//   3 C1;
// - 1, 0 (PVRTC-II): not interpolated: P's C0 and C1, mixed by the weight;
// - 1, 1 (PVRTC-II): a local palette for each place in the square, from P, Q, R and S's colours.
struct PvrtcColours {
    i32 c[2][4];  // C0, C1 as 8-bit RGBA
};

PvrtcColours pvrtc_colours(u64 block, bool second) {
    const u32 hi = static_cast<u32>(block >> 32);
    const bool opaque1 = (hi >> 31) & 1, opaque0 = second ? opaque1 : ((hi >> 15) & 1);
    const auto x5 = [](u32 v) { return static_cast<i32>((v << 3) | (v >> 2)); };
    const auto x4 = [](u32 v) { return static_cast<i32>((v << 4) | v); };
    const auto w4 = [](u32 v) { return (v << 1) | (v >> 3); };  // (4 bits to 5)
    PvrtcColours out;
    const u32 a = (hi >> 1) & 0x3FFF, b = (hi >> 16) & 0x7FFF;
    if (opaque0) {
        out.c[0][0] = x5(a >> 9), out.c[0][1] = x5((a >> 4) & 31), out.c[0][2] = x4(a & 15), out.c[0][3] = 255;
    } else {
        out.c[0][0] = x5(w4((a >> 7) & 15)), out.c[0][1] = x5(w4((a >> 3) & 15));
        out.c[0][2] = x5(((a & 7) << 2) | ((a & 7) >> 1)), out.c[0][3] = x4(((a >> 11) & 7) << 1);
    }
    if (opaque1) {
        out.c[1][0] = x5(b >> 10), out.c[1][1] = x5((b >> 5) & 31), out.c[1][2] = x5(b & 31), out.c[1][3] = 255;
    } else {
        out.c[1][0] = x5(w4((b >> 8) & 15)), out.c[1][1] = x5(w4((b >> 4) & 15)), out.c[1][2] = x5(w4(b & 15));
        out.c[1][3] = x4((((b >> 12) & 7) << 1) | (second ? 1u : 0u));
    }
    return out;
}

// Mode 3's palette (PVRTC-II): for each index and place (t, s) in the square, the colour: P, Q, R or S (bits 0-1)
// and C0 or C1 (bit 2), or P's 5/8 C0 + 3/8 C1 (8) or 3/8 C0 + 5/8 C1 (9).
constexpr u8 kPvrtcPalette[4][4][4] = {
    {{0, 0, 0, 0}, {0, 0, 0, 3}, {0, 0, 0, 3}, {0, 0, 3, 3}},
    {{8, 4, 4, 4}, {4, 4, 4, 4}, {4, 4, 7, 7}, {4, 7, 7, 7}},
    {{9, 1, 1, 1}, {2, 1, 1, 1}, {2, 2, 2, 1}, {2, 2, 2, 2}},
    {{4, 5, 5, 5}, {6, 6, 5, 5}, {6, 6, 5, 5}, {6, 6, 6, 5}},
};

// A level's texels (8-bit RGBA, as integers in [0, 255]) from its blocks (bx x by, in rows).
template <class Put>
void decode_pvrtc4(const nx::vector<u64>& blocks, u32 bx, u32 by, u32 width, u32 height, bool second, Put&& put) {
    nx::vector<PvrtcColours> colours(blocks.size());
    for (usize i = 0; i < blocks.size(); ++i) colours[i] = pvrtc_colours(blocks[i], second);
    static constexpr i32 kWeight[4] = {0, 3, 5, 8};  // (eighths)
    for (u32 y = 0; y < height; ++y) {
        const u32 py = ((y + 4 * by - 2) / 4) % by, ry = (py + 1) % by, t = (y + 2) % 4;
        for (u32 x = 0; x < width; ++x) {
            const u32 px = ((x + 4 * bx - 2) / 4) % bx, qx = (px + 1) % bx, s = (x + 2) % 4;
            const u64 block = blocks[usize{y / 4} * bx + x / 4];
            const u32 index = static_cast<u32>(block >> (2 * ((y % 4) * 4 + x % 4))) & 3;
            const bool m = (block >> 32) & 1, h = second && ((block >> 47) & 1);
            const PvrtcColours* corner[4] = {&colours[usize{py} * bx + px], &colours[usize{py} * bx + qx],
                                             &colours[usize{ry} * bx + px], &colours[usize{ry} * bx + qx]};
            i32 out[4];
            if (h && m) {
                const u8 e = kPvrtcPalette[index][t][s];
                const PvrtcColours& p = *corner[0];
                for (u32 k = 0; k < 4; ++k)
                    out[k] = e == 8   ? (5 * p.c[0][k] + 3 * p.c[1][k] + 4) / 8
                             : e == 9 ? (3 * p.c[0][k] + 5 * p.c[1][k] + 4) / 8
                                      : corner[e & 3]->c[e >> 2][k];
            } else {
                // (C0 and C1 at this texel, times 16)
                i32 c[2][4];
                const i32 wp = static_cast<i32>((4 - s) * (4 - t)), wq = static_cast<i32>(s * (4 - t)),
                          wr = static_cast<i32>((4 - s) * t), ws = static_cast<i32>(s * t);
                for (u32 j = 0; j < 2; ++j)
                    for (u32 k = 0; k < 4; ++k)
                        c[j][k] = h ? 16 * corner[0]->c[j][k]
                                    : wp * corner[0]->c[j][k] + wq * corner[1]->c[j][k] + wr * corner[2]->c[j][k] +
                                          ws * corner[3]->c[j][k];
                if (m) {
                    for (u32 k = 0; k < 4; ++k) {
                        const i32 v = index == 0 ? c[0][k] : index == 3 ? c[1][k] : (c[0][k] + c[1][k]) / 2;
                        out[k] = (v + 8) / 16;
                    }
                    if (index == 2) {
                        out[3] = 0;
                        if (second) out[0] = out[1] = out[2] = 0;
                    }
                } else {
                    const i32 w = kWeight[index];
                    for (u32 k = 0; k < 4; ++k) out[k] = ((8 - w) * c[0][k] + w * c[1][k] + 64) / 128;
                }
            }
            for (u32 k = 0; k < 4; ++k) out[k] = nx::clamp(out[k], 0, 255);
            put(x, y, out);
        }
    }
}

// PVRTC 2bpp (PVRTC-I and -II): blocks of 8x4 texels, 64 bits each, the colours as in 4bpp (pvrtc_colours) and the
// low word the modulation. M (bit 32) clear: a bit per texel (row by row), C0 or C1. M set: 2 bits for each texel
// with (x ^ y) even (the others' values interpolated from their neighbours, which may be in the next blocks), the
// weights 0, 3/8, 5/8, 1; bit 0 (the first texel's low bit) set picks the neighbours: bit 20 (the centre texel's
// low bit) set, the ones above and below, else the ones to the left and right; clear, all four. Those two texels'
// low bits are then their high bits again (bit 1 and bit 21). The colours are interpolated over the 8x4 square as in
// 4bpp; a PVRTC-II block with H set takes P's colours as they are (as its 4bpp blocks with H and not M). No texel is
// punched through in 2bpp.
template <class Put>
void decode_pvrtc2(const nx::vector<u64>& blocks, u32 bx, u32 by, u32 width, u32 height, bool second, Put&& put) {
    nx::vector<PvrtcColours> colours(blocks.size());
    for (usize i = 0; i < blocks.size(); ++i) colours[i] = pvrtc_colours(blocks[i], second);
    static constexpr i32 kWeight[4] = {0, 3, 5, 8};  // (eighths)
    // Each block's mode (0 direct, 1 four neighbours, 2 left and right, 3 above and below) and its texels' stored
    // weights (direct: 0 or 8; interpolated: the stored ones, -1 for the others).
    struct Modulation {
        u8 mode;
        i8 w[4][8];
    };
    nx::vector<Modulation> mods(blocks.size());
    for (usize i = 0; i < blocks.size(); ++i) {
        u32 bits = static_cast<u32>(blocks[i]);
        Modulation& m = mods[i];
        if (!((blocks[i] >> 32) & 1)) {
            m.mode = 0;
            for (u32 y = 0; y < 4; ++y)
                for (u32 x = 0; x < 8; ++x, bits >>= 1) m.w[y][x] = static_cast<i8>((bits & 1) ? 8 : 0);
            continue;
        }
        m.mode = 1;
        if (bits & 1) {
            m.mode = (bits & (1u << 20)) ? 3 : 2;
            bits = (bits & ~(1u << 20)) | ((bits >> 1) & (1u << 20));
        }
        bits = (bits & ~1u) | ((bits >> 1) & 1u);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 8; ++x) {
                if ((x ^ y) & 1) {
                    m.w[y][x] = -1;
                    continue;
                }
                m.w[y][x] = static_cast<i8>(kWeight[bits & 3]);
                bits >>= 2;
            }
    }
    const u32 tw = bx * 8, th = by * 4;  // (the blocks' texels: neighbours wrap around them)
    const auto stored = [&](u32 x, u32 y) -> i32 {
        x %= tw, y %= th;
        const i32 w = mods[usize{y / 4} * bx + x / 8].w[y % 4][x % 8];
        return w < 0 ? 0 : w;
    };
    for (u32 y = 0; y < height; ++y) {
        const u32 py = ((y + 4 * by - 2) / 4) % by, ry = (py + 1) % by, t = (y + 2) % 4;
        for (u32 x = 0; x < width; ++x) {
            const u32 px = ((x + 8 * bx - 4) / 8) % bx, qx = (px + 1) % bx, s = (x + 4) % 8;
            const usize at = usize{y / 4} * bx + x / 8;
            const Modulation& m = mods[at];
            i32 w = m.w[y % 4][x % 8];
            if (w < 0) {
                const u32 l = x + tw - 1, r = x + 1, u = y + th - 1, d = y + 1;
                w = m.mode == 2   ? (stored(l, y) + stored(r, y) + 1) / 2
                    : m.mode == 3 ? (stored(x, u) + stored(x, d) + 1) / 2
                                  : (stored(l, y) + stored(r, y) + stored(x, u) + stored(x, d) + 2) / 4;
            }
            const bool h = second && ((blocks[at] >> 47) & 1);
            const PvrtcColours* corner[4] = {&colours[usize{py} * bx + px], &colours[usize{py} * bx + qx],
                                             &colours[usize{ry} * bx + px], &colours[usize{ry} * bx + qx]};
            // (C0 and C1 at this texel, times 32)
            const i32 wp = static_cast<i32>((8 - s) * (4 - t)), wq = static_cast<i32>(s * (4 - t)),
                      wr = static_cast<i32>((8 - s) * t), ws = static_cast<i32>(s * t);
            i32 out[4];
            for (u32 k = 0; k < 4; ++k) {
                i32 c[2];
                for (u32 j = 0; j < 2; ++j)
                    c[j] = h ? 32 * corner[0]->c[j][k]
                             : wp * corner[0]->c[j][k] + wq * corner[1]->c[j][k] + wr * corner[2]->c[j][k] +
                                   ws * corner[3]->c[j][k];
                out[k] = nx::clamp(((8 - w) * c[0] + w * c[1] + 128) / 256, 0, 255);
            }
            put(x, y, out);
        }
    }
}

// Swizzled addressing as two tables: swizzled_index(x, y, w, h) is a part from x's bits and a part from y's that do
// not overlap, so the index of (x, y) is xs[x] + ys[y].
struct SwizzleTables {
    nx::vector<u64> xs, ys;
    SwizzleTables(u32 width, u32 height, u32 w, u32 h) : xs(width), ys(height) {
        for (u32 x = 0; x < width; ++x) xs[x] = swizzled_index(x, 0, w, h);
        for (u32 y = 0; y < height; ++y) ys[y] = swizzled_index(0, y, w, h);
    }
};

}  // namespace

u64 swizzled_index(u32 x, u32 y, u32 w, u32 h) {
    const u32 lw = log2_ceil(w), lh = log2_ceil(h), common = nx::min(lw, lh);
    u64 a = 0;
    for (u32 b = 0; b < common; ++b) a |= (u64{(y >> b) & 1} << (2 * b)) | (u64{(x >> b) & 1} << (2 * b + 1));
    // (the larger dimension's remaining bits on top)
    if (lw > lh) a |= u64{x >> common} << (2 * common);
    else if (lh > lw) a |= u64{y >> common} << (2 * common);
    return a;
}

u32 texel_bits(u32 base_format) {
    switch (code_of(base_format)) {
        case kU8: case kS8: case kP8: return 8;
        case kP4: case kPVRT4: case kPVRTII4: case kUBC1: case kUBC4: case kSBC4: return 4;
        case kPVRT2: case kPVRTII2: return 2;
        case kUBC2: case kUBC3: case kUBC5: case kSBC5: return 8;
        case kU4U4U4U4: case kU8U3U3U2: case kU1U5U5U5: case kU5U6U5: case kS5S5U6: case kU8U8: case kS8S8:
        case kU16: case kS16: case kF16: case kYUV422: return 16;
        case kU8U8U8: case kS8S8S8: return 24;
        case kU8U8U8U8: case kS8S8S8S8: case kU2U10U10U10: case kU16U16: case kS16S16: case kF16F16: case kF32:
        case kF32M: case kX8S8S8U8: case kX8U24: case kU32: case kS32: case kSE5M9M9M9: case kF11F11F10:
        case kU2F10F10F10: return 32;
        case kF16F16F16F16: case kU16U16U16U16: case kS16S16S16S16: case kF32F32: case kU32U32: return 64;
        case kYUV420P2: case kYUV420P3: return 12;
        default: return 0;
    }
}

TextureInfo decode_texture(const u32 w[4]) {
    TextureInfo t;
    t.type = w[1] >> 29;
    t.base_format = ((w[1] >> 24) & 0x1F) << 24 | (w[0] >> 31) << 31;
    t.swizzle = (w[3] >> 28) & 7;
    if (is_swizzled(t.type) && t.type != texture_type::kSwizzledArbitrary && t.type != texture_type::kCubeArbitrary) {
        t.width = 1u << ((w[1] >> 16) & 0xF);
        t.height = 1u << (w[1] & 0xF);
    } else {
        t.width = ((w[1] >> 12) & 0xFFF) + 1;
        t.height = (w[1] & 0xFFF) + 1;
    }
    t.address = w[2] & ~3u;
    t.lod_min = (w[2] & 3) << 2 | ((w[3] >> 26) & 3);
    t.normalize = (w[3] >> 31) != 0;
    t.palette = (w[3] & 0x3FFFFFF) << 6;
    t.gamma = (w[0] >> 27) & 3;
    t.mag_linear = (w[0] >> 12) & 1;
    t.min_linear = (w[0] >> 10) & 1;
    t.mip_linear = (w[0] >> 9) & 1;
    t.u_address = (w[0] >> 6) & 7;
    t.v_address = (w[0] >> 3) & 7;
    const u32 bits = texel_bits(t.base_format);
    if (t.type == texture_type::kLinearStrided) {
        // (the stride in 4-byte units: bits 0-2 at 9-11, 3-12 at 17-26, 13-14 at 1-2)
        const u32 units = ((w[0] >> 9) & 7) | ((w[0] >> 17) & 0x3FF) << 3 | ((w[0] >> 1) & 3) << 13;
        t.stride = units * 4;
    } else {
        t.lod_bias = (w[0] >> 21) & 0x3F;
        const u32 clamp = (w[0] >> 17) & 0xF;
        t.mipmapped = clamp != 15;
        t.levels = t.mipmapped ? clamp + 1 : 1;
        t.stride = ((t.width + 7) & ~7u) * bits / 8;
    }
    return t;
}

void encode_texture(const TextureInfo& t, u32 w[4]) {
    w[0] = (t.base_format >> 31) << 31 | (t.gamma & 3) << 27 | (t.mag_linear ? 1u << 12 : 0) | (t.min_linear ? 1u << 10 : 0) |
           (t.mip_linear ? 1u << 9 : 0) | (t.u_address & 7) << 6 | (t.v_address & 7) << 3;
    if (t.type == texture_type::kLinearStrided) {
        const u32 units = t.stride / 4;
        w[0] |= (units & 7) << 9 | ((units >> 3) & 0x3FF) << 17 | ((units >> 13) & 3) << 1;
    } else {
        w[0] |= (t.lod_bias & 0x3F) << 21 | (t.mipmapped ? ((t.levels - 1) & 0xF) : 15u) << 17;
    }
    w[1] = (t.type & 7) << 29 | ((t.base_format >> 24) & 0x1F) << 24;
    if (is_swizzled(t.type) && t.type != texture_type::kSwizzledArbitrary && t.type != texture_type::kCubeArbitrary)
        w[1] |= (log2_ceil(t.width) & 0xF) << 16 | (log2_ceil(t.height) & 0xF);
    else
        w[1] |= ((t.width - 1) & 0xFFF) << 12 | ((t.height - 1) & 0xFFF);
    w[2] = (t.address & ~3u) | ((t.lod_min >> 2) & 3);
    w[3] = (t.normalize ? 1u << 31 : 0) | (t.swizzle & 7) << 28 | (t.lod_min & 3) << 26 | ((t.palette >> 6) & 0x3FFFFFF);
}

namespace {

// Where a level starts, its row pitch and its footprint (bytes), in each layout.
struct Level {
    u32 width, height;  // texels
    u64 offset;         // from the face's start
    u64 bytes;
};

nx::vector<Level> levels_of(const TextureInfo& t, u64& face_bytes) {
    const u32 bits = texel_bits(t.base_format);
    const u32 code = code_of(t.base_format);
    u32 bw = 1, bh = 1;
    if (block_format(code)) block_size(code, bw, bh);
    nx::vector<Level> out;
    u64 at = 0;
    const u32 total = t.mipmapped ? log2_ceil(nx::max(t.width, t.height)) + 1 : 1;  // (down to 1x1)
    for (u32 l = 0; l < (is_cube(t.type) && t.mipmapped ? total : t.levels); ++l) {
        const u32 w = nx::max(1u, t.width >> l), h = nx::max(1u, t.height >> l);
        u64 bytes, step;
        if (is_swizzled(t.type)) {
            // (each level the size of its power-of-two footprint, in whole blocks; the next one right after)
            const u32 pw = nx::max(pow2_ceil(w), bw), ph = nx::max(pow2_ceil(h), bh);
            bytes = u64{pw} * ph * bits / 8;
            step = bytes;
        } else if (t.type == texture_type::kTiled) {
            const u32 tw = (w + 31) / 32, th = (h + 31) / 32;
            bytes = u64{tw} * th * 32 * 32 * bits / 8;
            step = u64{nx::max(32u, pow2_ceil(t.width) >> l)} * nx::max(32u, pow2_ceil(t.height) >> l) * bits / 8;
        } else {
            const u32 pitch = l == 0 ? t.stride : ((w + 7) & ~7u) * bits / 8;
            bytes = u64{pitch} * h;
            step = u64{nx::max(8u, pow2_ceil(t.width) >> l)} * nx::max(1u, pow2_ceil(t.height) >> l) * bits / 8;
        }
        out.push_back(Level{w, h, at, bytes});
        at += nx::max(step, bytes);
    }
    face_bytes = at;
    if (is_cube(t.type)) {
        if (t.mipmapped) {
            const u32 top = nx::min(t.width, t.height);
            const bool aligned = (block_format(code) || bits == 8) ? top >= 32
                                 : bits == 64                      ? top >= 8
                                                                   : top >= 16;
            face_bytes = aligned ? (face_bytes + 2047) & ~u64{2047} : (face_bytes + 3) & ~u64{3};
        }
        out.resize(t.levels);
    }
    return out;
}

}  // namespace

u32 texture_size(const TextureInfo& t) {
    u64 face = 0;
    const auto levels = levels_of(t, face);
    const u32 faces = is_cube(t.type) ? 6 : 1;
    u64 last = 0;
    for (const Level& l : levels) last = nx::max(last, l.offset + l.bytes);
    return static_cast<u32>(faces > 1 ? face * (faces - 1) + last : last);
}

namespace {

std::atomic<u32> yuv_profiles[2] = {0, 1};  // (SCE_GXM_YUV_PROFILE_BT601_STANDARD, SCE_GXM_YUV_PROFILE_BT709_STANDARD)

// One pixel of a YUV texture as RGBA: the profile's matrix (BT.601 or BT.709), from standard range (Y 16-235, U and V
// 16-240) or full range.
void yuv_to_rgba(u32 profile, u8 y, u8 u, u8 v, f32 out[4]) {
    const bool full = profile >= 2;
    const bool bt709 = profile & 1;
    const f32 yy = full ? y / 255.0f : (static_cast<f32>(y) - 16.0f) / 219.0f;
    const f32 cb = full ? (static_cast<f32>(u) - 128.0f) / 255.0f : (static_cast<f32>(u) - 128.0f) / 224.0f;
    const f32 cr = full ? (static_cast<f32>(v) - 128.0f) / 255.0f : (static_cast<f32>(v) - 128.0f) / 224.0f;
    if (bt709) {
        out[0] = yy + 1.5748f * cr;
        out[1] = yy - 0.187324f * cb - 0.468124f * cr;
        out[2] = yy + 1.8556f * cb;
    } else {
        out[0] = yy + 1.402f * cr;
        out[1] = yy - 0.344136f * cb - 0.714136f * cr;
        out[2] = yy + 1.772f * cb;
    }
    out[3] = 1.0f;
}

// (the 16.16 tables of read_yuv's planar conversion: each byte's term of yuv_to_rgba, times 255; the luma's with the
// rounding: within one of the exact value, as the floats are)
void yuv_tables(u32 profile, i32 out[5][256]) {
    const bool full = profile >= 2, bt709 = profile & 1;
    const f64 rv = bt709 ? 1.5748 : 1.402, gu = bt709 ? 0.187324 : 0.344136, gv = bt709 ? 0.468124 : 0.714136,
              bu = bt709 ? 1.8556 : 1.772;
    for (u32 i = 0; i < 256; ++i) {
        const f64 y = full ? i / 255.0 : (static_cast<f64>(i) - 16.0) / 219.0;
        const f64 c = full ? (static_cast<f64>(i) - 128.0) / 255.0 : (static_cast<f64>(i) - 128.0) / 224.0;
        auto fixed = [](f64 v) { return static_cast<i32>(std::lround(v * 255.0 * 65536.0)); };
        out[0][i] = fixed(y) + 32768;
        out[1][i] = fixed(rv * c), out[2][i] = fixed(-gu * c), out[3][i] = fixed(-gv * c), out[4][i] = fixed(bu * c);
    }
}

// YUV textures (linear only): YUV420 with two planes (Y, then U and V interleaved) or three (Y, U, V), and YUV422
// interleaved. The swizzle chooses the order of U and V (or of the four bytes) and the conversion slot. Planes follow
// each other packed, each row its width rounded up to 8 texels; with mip levels, a plane's offset to the next is that
// of its power-of-two footprint. The top level only.
Result<Texels> read_yuv(const TextureInfo& t, u32 code, ReadGuest read) {
    if (!is_linear(t.type)) return failure("texture: YUV textures are linear");
    const u32 w = t.width, h = t.height;
    const u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
    auto align8 = [](u32 v) { return (v + 7) & ~7u; };
    const bool padded = t.mipmapped;
    u32 csc;
    u32 size;
    u32 second = 0, third = 0;  // (plane offsets)
    if (code == kYUV422) {
        csc = (t.swizzle >> 2) & 1;
        const u32 pitch = t.type == texture_type::kLinearStrided ? t.stride : align8(w) * 2;
        size = pitch * h;
    } else if (code == kYUV420P2) {
        csc = (t.swizzle >> 1) & 1;
        second = padded ? pow2_ceil(w) * pow2_ceil(h) : align8(w) * h;
        size = second + align8(cw) * 2 * ch;
    } else {
        csc = (t.swizzle >> 1) & 1;
        second = padded ? pow2_ceil(w) * pow2_ceil(h) : align8(w) * h;
        third = second + (padded ? pow2_ceil(cw) * pow2_ceil(ch) : align8(cw) * ch);
        size = third + align8(cw) * ch;
    }
    nx::vector<u8> raw(size);
    if (!read(t.address, raw.data(), size)) return failure("texture: data outside guest memory");
    const u32 profile = yuv_profiles[csc].load();
    Texels out;
    out.width = w;
    out.height = h;
    out.bytes = 4;
    out.data.resize(usize{w} * h * 4);
    if (code != kYUV422) {
        // (the planar layouts, as below a row at a time, in fixed point: each byte's term of yuv_to_rgba, times 255,
        // from a table in 16.16, the three summed and rounded: within one of the exact value, as the floats are)
        const bool swapped = t.swizzle & 1;
        i32 tables[5][256];
        yuv_tables(profile, tables);
        const i32 *ys = tables[0], *rvs = tables[1], *gus = tables[2], *gvs = tables[3], *bus = tables[4];
        auto byte = [](i32 v) { return static_cast<u8>(nx::clamp(v >> 16, 0, 255)); };
        for (u32 row = 0; row < h; ++row) {
            const u8* yrow = raw.data() + usize{row} * align8(w);
            const usize crow = usize{row / 2} * align8(cw);
            u8* dst = out.data.data() + usize{row} * w * 4;
            for (u32 x = 0; x < w; ++x) {
                u8 u, v;
                if (code == kYUV420P2) {
                    const u8* p = raw.data() + second + (crow + x / 2) * 2;
                    u = swapped ? p[0] : p[1], v = swapped ? p[1] : p[0];
                } else {
                    const u8 first = raw[second + crow + x / 2], last = raw[third + crow + x / 2];
                    u = swapped ? last : first, v = swapped ? first : last;
                }
                const i32 yv = ys[yrow[x]];
                dst[x * 4 + 0] = byte(yv + rvs[v]);
                dst[x * 4 + 1] = byte(yv + gus[u] + gvs[v]);
                dst[x * 4 + 2] = byte(yv + bus[u]);
                dst[x * 4 + 3] = 255;
            }
        }
        return out;
    }
    for (u32 yy = 0; yy < h; ++yy) {
        for (u32 x = 0; x < w; ++x) {
            u8 y, u, v;
            if (code == kYUV422) {
                // (bytes of a pair of pixels by swizzle: YUYV, YVYU, UYVY, VYUY, most significant first)
                const u32 pitch = t.type == texture_type::kLinearStrided ? t.stride : align8(w) * 2;
                const u8* p = raw.data() + usize{yy} * pitch + (x & ~1u) * 2;
                const bool second_pixel = x & 1;
                switch (t.swizzle & 3) {
                    case 0: v = p[0], y = second_pixel ? p[3] : p[1], u = p[2]; break;
                    case 1: u = p[0], y = second_pixel ? p[3] : p[1], v = p[2]; break;
                    case 2: y = second_pixel ? p[2] : p[0], v = p[1], u = p[3]; break;
                    default: y = second_pixel ? p[2] : p[0], u = p[1], v = p[3]; break;
                }
            } else {
                y = raw[usize{yy} * align8(w) + x];
                const u32 cx = x / 2, cy = yy / 2;
                const bool swapped = t.swizzle & 1;  // (YVU)
                if (code == kYUV420P2) {
                    // (a texel of the chroma plane: U in its high byte for YUV, V for YVU)
                    const u8* p = raw.data() + second + (usize{cy} * align8(cw) + cx) * 2;
                    u = swapped ? p[0] : p[1];
                    v = swapped ? p[1] : p[0];
                } else {
                    const usize at = usize{cy} * align8(cw) + cx;
                    const u8 first = raw[second + at], last = raw[third + at];
                    u = swapped ? last : first;
                    v = swapped ? first : last;
                }
            }
            f32 rgba[4];
            yuv_to_rgba(profile, y, u, v, rgba);
            u8* dst = out.data.data() + (usize{yy} * w + x) * 4;
            for (u32 k = 0; k < 4; ++k) dst[k] = static_cast<u8>(std::lround(nx::clamp(rgba[k], 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

}  // namespace

bool is_yuv420(const TextureInfo& t) {
    const u32 code = code_of(t.base_format);
    return (code == kYUV420P2 || code == kYUV420P3) && is_linear(t.type) && t.width && t.height && t.width <= 4096 &&
           t.height <= 4096;
}

Result<Yuv420> read_yuv420(const TextureInfo& t, ReadGuest read) {
    if (!is_yuv420(t)) return failure("texture: not a YUV420 texture");
    const u32 code = code_of(t.base_format);
    const u32 w = t.width, h = t.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    auto align8 = [](u32 v) { return (v + 7) & ~7u; };
    Yuv420 y;
    y.width = w, y.height = h;
    y.y_pitch = align8(w), y.c_pitch = align8(cw);
    y.three = code == kYUV420P3;
    y.swapped = t.swizzle & 1;
    y.second = t.mipmapped ? pow2_ceil(w) * pow2_ceil(h) : align8(w) * h;
    u32 size;
    if (!y.three) {
        size = y.second + align8(cw) * 2 * ch;
    } else {
        y.third = y.second + (t.mipmapped ? pow2_ceil(cw) * pow2_ceil(ch) : align8(cw) * ch);
        size = y.third + align8(cw) * ch;
    }
    y.planes.resize(size);
    if (!read(t.address, y.planes.data(), size)) return failure("texture: data outside guest memory");
    yuv_tables(yuv_profiles[(t.swizzle >> 1) & 1].load(), y.tables);
    return y;
}

usize yuv420_buffer_bytes(const Yuv420& y) {
    return 5 * 256 * 4 + ((y.planes.size() + 3) & ~usize{3}) + usize{y.width} * y.height * 4;
}

Yuv420Constants yuv420_buffer(const Yuv420& y, u8* out) {
    std::memcpy(out, y.tables, 5 * 256 * 4);
    std::memcpy(out + 5 * 256 * 4, y.planes.data(), y.planes.size());
    const u32 planes = 5 * 256, out_at = planes + static_cast<u32>((y.planes.size() + 3) / 4);
    return Yuv420Constants{y.width, y.height, y.y_pitch, y.c_pitch, planes, y.second, y.third,
                           (y.three ? 1u : 0u) | (y.swapped ? 2u : 0u), out_at};
}

nx::vector<u32> yuv420_shader() {
    using spirv::Builder;
    using spirv::Id;
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id tu = b.t_uint(), ti = b.t_int(), tb = b.t_bool(), tv3 = b.t_vector(tu, 3), tvoid = b.t_void();
    // (the buffer: words)
    const Id words = b.t_runtime_array(tu);
    b.decorate(words, SpvDecorationArrayStride, {4});
    const Id block = b.t_struct({words});
    b.decorate(block, SpvDecorationBlock);
    b.member_decorate(block, 0, SpvDecorationOffset, {0});
    const Id buffer = b.variable(b.t_pointer(SpvStorageClassStorageBuffer, block), SpvStorageClassStorageBuffer);
    b.decorate(buffer, SpvDecorationDescriptorSet, {0});
    b.decorate(buffer, SpvDecorationBinding, {0});
    // (the constants)
    const Id pc_block = b.t_struct({tu, tu, tu, tu, tu, tu, tu, tu, tu});
    b.decorate(pc_block, SpvDecorationBlock);
    for (u32 i = 0; i < 9; ++i) b.member_decorate(pc_block, i, SpvDecorationOffset, {i * 4});
    const Id pc = b.variable(b.t_pointer(SpvStorageClassPushConstant, pc_block), SpvStorageClassPushConstant);
    const Id gid_var = b.variable(b.t_pointer(SpvStorageClassInput, tv3), SpvStorageClassInput);
    b.decorate(gid_var, SpvDecorationBuiltIn, {SpvBuiltInGlobalInvocationId});

    const Id fn = b.begin_function(tvoid, b.t_function(tvoid));
    b.block(b.new_label());
    const Id tp_pc = b.t_pointer(SpvStorageClassPushConstant, tu), tp_word = b.t_pointer(SpvStorageClassStorageBuffer, tu);
    auto constant = [&](u32 i) { return b.op(SpvOpLoad, tu, {b.op(SpvOpAccessChain, tp_pc, {pc, b.c_int(static_cast<i32>(i))})}); };
    auto u = [&](u32 v) { return b.c_uint(v); };
    auto bin = [&](u32 op, Id type, Id a, Id c) { return b.op(op, type, {a, c}); };
    auto word_ptr = [&](Id index) { return b.op(SpvOpAccessChain, tp_word, {buffer, b.c_int(0), index}); };
    auto load_word = [&](Id index) { return b.op(SpvOpLoad, tu, {word_ptr(index)}); };
    const Id gid = b.op(SpvOpLoad, tv3, {gid_var});
    const Id x = b.op(SpvOpCompositeExtract, tu, {gid, 0}), yy = b.op(SpvOpCompositeExtract, tu, {gid, 1});
    const Id width = constant(0), height = constant(1);
    const Id inside = bin(SpvOpLogicalAnd, tb, bin(SpvOpULessThan, tb, x, width), bin(SpvOpULessThan, tb, yy, height));
    const Id body = b.new_label(), merge = b.new_label();
    b.op0(SpvOpSelectionMerge, {merge, SpvSelectionControlMaskNone});
    b.op0(SpvOpBranchConditional, {inside, body, merge});
    b.block(body);
    {
        const Id planes = constant(4), flags = constant(7);
        // (a byte of the planes)
        auto byte = [&](Id at) {
            const Id word = load_word(bin(SpvOpIAdd, tu, planes, bin(SpvOpShiftRightLogical, tu, at, u(2))));
            const Id shift = bin(SpvOpShiftLeftLogical, tu, bin(SpvOpBitwiseAnd, tu, at, u(3)), u(3));
            return bin(SpvOpBitwiseAnd, tu, bin(SpvOpShiftRightLogical, tu, word, shift), u(0xFF));
        };
        const Id luma = byte(bin(SpvOpIAdd, tu, bin(SpvOpIMul, tu, yy, constant(2)), x));
        const Id crow = bin(SpvOpIMul, tu, bin(SpvOpShiftRightLogical, tu, yy, u(1)), constant(3));
        const Id cx = bin(SpvOpShiftRightLogical, tu, x, u(1));
        const Id at = bin(SpvOpIAdd, tu, crow, cx);
        const Id three = bin(SpvOpINotEqual, tb, bin(SpvOpBitwiseAnd, tu, flags, u(1)), u(0));
        const Id swapped = bin(SpvOpINotEqual, tb, bin(SpvOpBitwiseAnd, tu, flags, u(2)), u(0));
        // (three planes: U then V at `second` and `third`; two: texels of two bytes at `second`, the low one V)
        const Id pair = bin(SpvOpIAdd, tu, constant(5), bin(SpvOpShiftLeftLogical, tu, at, u(1)));
        const Id first = b.op(SpvOpSelect, tu, {three, byte(bin(SpvOpIAdd, tu, constant(5), at)), byte(bin(SpvOpIAdd, tu, pair, u(1)))});
        const Id last = b.op(SpvOpSelect, tu, {three, byte(bin(SpvOpIAdd, tu, constant(6), at)), byte(pair)});
        const Id cu = b.op(SpvOpSelect, tu, {swapped, last, first});
        const Id cv = b.op(SpvOpSelect, tu, {swapped, first, last});
        auto table = [&](u32 k, Id i) { return b.op(SpvOpBitcast, ti, {load_word(bin(SpvOpIAdd, tu, u(k * 256), i))}); };
        const Id ys = table(0, luma);
        auto channel = [&](Id sum) {
            const Id v = bin(SpvOpShiftRightArithmetic, ti, sum, b.c_int(16));
            const Id lo = b.op(SpvOpSelect, ti, {bin(SpvOpSLessThan, tb, v, b.c_int(0)), b.c_int(0), v});
            const Id hi = b.op(SpvOpSelect, ti, {bin(SpvOpSGreaterThan, tb, lo, b.c_int(255)), b.c_int(255), lo});
            return b.op(SpvOpBitcast, tu, {hi});
        };
        const Id r = channel(bin(SpvOpIAdd, ti, ys, table(1, cv)));
        const Id g = channel(bin(SpvOpIAdd, ti, bin(SpvOpIAdd, ti, ys, table(2, cu)), table(3, cv)));
        const Id bl = channel(bin(SpvOpIAdd, ti, ys, table(4, cu)));
        Id rgba = bin(SpvOpBitwiseOr, tu, r, bin(SpvOpShiftLeftLogical, tu, g, u(8)));
        rgba = bin(SpvOpBitwiseOr, tu, rgba, bin(SpvOpShiftLeftLogical, tu, bl, u(16)));
        rgba = bin(SpvOpBitwiseOr, tu, rgba, u(0xFF000000u));
        const Id index = bin(SpvOpIAdd, tu, constant(8), bin(SpvOpIAdd, tu, bin(SpvOpIMul, tu, yy, width), x));
        b.op0(SpvOpStore, {word_ptr(index), rgba});
        b.op0(SpvOpBranch, {merge});
    }
    b.block(merge);
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {gid_var};  // (SPIR-V 1.3: the inputs and outputs only)
    b.entry_point(SpvExecutionModelGLCompute, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeLocalSize, {8, 8, 1});
    return b.module();
}

void set_yuv_profile(u32 csc, u32 profile) {
    if (csc < 2 && profile < 4) yuv_profiles[csc].store(profile);
}

Result<Texels> read_texels(const TextureInfo& t, ReadGuest read) { return detail::read_texels(t, read, false); }
Result<Texels> read_texels(const TextureInfo& t, ReadGuest read, bool native_bc, bool single_float) {
    return detail::read_texels(t, read, false, native_bc, single_float);
}

Result<Texels> detail::read_texels(const TextureInfo& t, ReadGuest read, bool reference, bool native_bc, bool single_float) {
    const u32 code = code_of(t.base_format);
    const u32 bits = texel_bits(t.base_format);
    if (!bits || !t.width || !t.height || t.width > 4096 || t.height > 4096) return failure("texture: bad format or size");
    if (code >= kYUV420P2 && code <= kYUV422) return read_yuv(t, code, read);
    if (code == kX8S8S8U8 || (code == kX8U24 && t.swizzle != 0) || code == kU32 || code == kS32 ||
        code == kSE5M9M9M9 || code == kU32U32)
        return failure(nx::format("texture: format 0x{:08x} not implemented", t.base_format));
    const u32 size = texture_size(t);
    nx::vector<u8> raw(size + 8, 0);
    if (!read(t.address, raw.data(), size)) return failure("texture: data outside guest memory");
    f32 palette[256][4] = {};
    if (code == kP4 || code == kP8) {
        const u32 entries = code == kP4 ? 16 : 256;
        nx::vector<u32> words(entries);
        if (!t.palette || !read(t.palette, words.data(), entries * 4)) return failure("texture: palette outside guest memory");
        for (u32 i = 0; i < entries; ++i) {
            f32 c[4];
            u32 n;
            bool wide;
            components(kU8U8U8U8, t.swizzle, words[i], c, n, wide);
            swizzle(c, 4, t.swizzle, palette[i]);
        }
    }

    u64 face_bytes = 0;
    const auto levels = levels_of(t, face_bytes);
    Texels out;
    out.width = t.width;
    out.height = t.height;
    out.levels = static_cast<u32>(levels.size());
    out.faces = is_cube(t.type) ? 6 : 1;
    bool wide = t.gamma != 0 || code == kSBC4 || code == kSBC5;  // (signed BC4 and BC5: below 0 too)
    {
        f32 c[4];
        u32 n;
        bool w = false;
        if (components(code, t.swizzle, 0, c, n, w)) wide |= w;
    }
    const bool full = code == kF32 || code == kF32M || code == kF32F32;
    const bool single = single_float && !reference && t.gamma == 0 && (code == kF32 || code == kF32M);
    out.bytes = single ? 4 : full ? 16 : wide ? 8 : 4;
    const u32 out_bytes = out.bytes;
    // 8-bit unsigned components without gamma: where each output byte comes from (a component's byte, or -1 for 0
    // and -256 for 255), found by putting markers through the swizzle.
    const bool bytes8 = out_bytes == 4 && t.gamma == 0 && (code == kU8 || code == kU8U8 || code == kU8U8U8 || code == kU8U8U8U8);
    i32 byte_of[4] = {};
    if (bytes8) {
        const f32 marks[4] = {10, 11, 12, 13};
        f32 m[4];
        swizzle(marks, bits / 8, t.swizzle, m);
        for (u32 k = 0; k < 4; ++k) byte_of[k] = m[k] >= 10 ? static_cast<i32>(m[k]) - 10 : m[k] >= 1 ? -256 : -1;
    }
    // F32, F32M and U2F10F10F10 without gamma (a render target's depth or HDR colour read as a texture, often each
    // frame): each output component a component's value or a constant, found by putting markers through the swizzle,
    // and written as the general path writes it (F32: the float's bits, F32M's without the mask bit; U2F10F10F10:
    // halves from tables of what the general path makes of each 10-bit float and 2-bit alpha), a texel at a time.
    const bool floats32 = !reference && t.gamma == 0 && (code == kF32 || code == kF32M || code == kU2F10F10F10);
    i32 float_of[4] = {};
    f32 float_const[4] = {};
    nx::vector<u16> f10_halves;
    u16 alpha_halves[4] = {}, const_halves[4] = {};
    if (floats32) {
        const u32 count = code == kU2F10F10F10 ? 4 : 1;
        const f32 marks[4] = {10, 11, 12, 13};
        f32 m[4];
        swizzle(marks, count, t.swizzle, m);
        for (u32 k = 0; k < 4; ++k) {
            float_of[k] = m[k] >= 10 ? static_cast<i32>(m[k]) - 10 : -1;
            float_const[k] = m[k] >= 10 ? 0.0f : m[k];
            const_halves[k] = float_to_half(float_const[k]);
            out.rgba_of[k] = static_cast<u8>(float_of[k] >= 0 ? 0 : float_const[k] == 0.0f ? 1 : 2);
        }
        out.single = single;
        if (code == kU2F10F10F10) {
            f10_halves.resize(1024);
            for (u32 i = 0; i < 1024; ++i) f10_halves[i] = float_to_half(half_to_float(static_cast<u16>(i << 5)));
            for (u32 i = 0; i < 4; ++i) alpha_halves[i] = float_to_half(unorm(i, 2));
        }
    }
    usize total = 0;
    for (const Level& l : levels) total += usize{l.width} * l.height * out_bytes;
    out.data.resize(total * out.faces);
    auto put = [&](u8* dst, const f32 rgba[4]) {
        f32 v[4];
        for (u32 k = 0; k < 4; ++k) {
            v[k] = rgba[k];
            // (gamma: sRGB-encoded components decoded to linear; R, GR or BGR)
            const bool srgb = (t.gamma == 1 && k < 3) || (t.gamma == 3 && k < 2);
            if (srgb) v[k] = v[k] <= 0.04045f ? v[k] / 12.92f : std::pow((v[k] + 0.055f) / 1.055f, 2.4f);
        }
        if (full) {
            std::memcpy(dst, v, 16);
        } else if (wide) {
            for (u32 k = 0; k < 4; ++k) {
                const u16 h = float_to_half(v[k]);
                std::memcpy(dst + 2 * k, &h, 2);
            }
        } else {
            for (u32 k = 0; k < 4; ++k)
                dst[k] = static_cast<u8>(std::lround(nx::clamp(std::isnan(v[k]) ? 0.0f : v[k], 0.0f, 1.0f) * 255.0f));
        }
    };

    // P4 and P8 to 8-bit components: each texel its palette entry's bytes, as put() writes them (not with `reference`,
    // which the tests compare with).
    const bool paletted = !reference && (code == kP4 || code == kP8) && out_bytes == 4;
    // BC1-BC3 without gamma: the blocks as they are for a device that samples them, else 8-bit texels a block at a time
    const bool bc_plain = (code == kUBC1 || code == kUBC2 || code == kUBC3) && t.gamma == 0 && out_bytes == 4;
    const bool keep_blocks = native_bc && bc_plain;
    const bool bc_bytes = !reference && bc_plain && !keep_blocks;
    if (keep_blocks) {
        out.bc = code == kUBC1 ? 1 : code == kUBC2 ? 2 : 3;
        out.bytes = 0;
        usize blocks_total = 0;
        for (const Level& l : levels) blocks_total += usize{(l.width + 3) / 4} * ((l.height + 3) / 4);
        out.data.assign(blocks_total * (code == kUBC1 ? 8 : 16) * out.faces, 0);
    }
    u32 palette8[256] = {};
    if (paletted)
        for (u32 i = 0; i < 256; ++i) put(reinterpret_cast<u8*>(&palette8[i]), palette[i]);

    usize at = 0;
    for (u32 face = 0; face < out.faces; ++face) {
        for (const Level& lv : levels) {
            const u8* base = raw.data() + face * face_bytes + lv.offset;
            const u32 pw = pow2_ceil(lv.width), ph = pow2_ceil(lv.height);
            u8* dst = out.data.data() + at;
            if (block_format(code)) {
                // Blocks of 4x4 texels (PVRTC 2bpp: 8x4), in Morton order (swizzled layouts) or in rows of blocks.
                const bool pvrtc2 = code == kPVRT2 || code == kPVRTII2;
                const u32 bw = pvrtc2 ? 8 : 4;
                const u32 bx = (lv.width + bw - 1) / bw, by = (lv.height + 3) / 4;
                if (code == kPVRT4 || code == kPVRTII4 || pvrtc2) {
                    // (a texel needs the blocks around it: the level's blocks gathered first)
                    const u32 pbx = nx::max(pw / bw, 1u), pby = nx::max(ph / 4, 1u);
                    const SwizzleTables sw(is_swizzled(t.type) ? bx : 0, is_swizzled(t.type) ? by : 0, pbx, pby);
                    nx::vector<u64> blocks(usize{bx} * by, 0);
                    for (u32 y = 0; y < by; ++y)
                        for (u32 x = 0; x < bx; ++x) {
                            const u64 index = is_swizzled(t.type) ? sw.xs[x] + sw.ys[y] : u64{y} * bx + x;
                            const u8* block = base + index * 8;
                            if (block + 8 <= raw.data() + raw.size()) std::memcpy(&blocks[usize{y} * bx + x], block, 8);
                        }
                    // (8-bit texels without gamma: the bytes as they are, as put() rounds n / 255 back to n)
                    const bool pvrtc_bytes = !reference && out_bytes == 4 && t.gamma == 0;
                    const auto texel = [&](u32 x, u32 y, const i32* c) {
                        u8* o = dst + (usize{y} * lv.width + x) * out_bytes;
                        if (pvrtc_bytes) {
                            o[0] = static_cast<u8>(c[0]), o[1] = static_cast<u8>(c[1]), o[2] = static_cast<u8>(c[2]);
                            o[3] = static_cast<u8>(c[3]);
                            return;
                        }
                        f32 v[4];
                        for (u32 k = 0; k < 4; ++k) v[k] = static_cast<f32>(c[k]) / 255.0f;
                        put(o, v);
                    };
                    if (pvrtc2) decode_pvrtc2(blocks, bx, by, lv.width, lv.height, code == kPVRTII2, texel);
                    else decode_pvrtc4(blocks, bx, by, lv.width, lv.height, code == kPVRTII4, texel);
                    at += usize{lv.width} * lv.height * out_bytes;
                    continue;
                }
                const u32 block_bytes = code == kUBC1 || code == kUBC4 || code == kSBC4 ? 8 : 16;
                const u32 pbx = nx::max(pw / 4, 1u), pby = nx::max(ph / 4, 1u);
                const SwizzleTables sw(is_swizzled(t.type) ? bx : 0, is_swizzled(t.type) ? by : 0, pbx, pby);
                for (u32 y = 0; y < by; ++y) {
                    for (u32 x = 0; x < bx; ++x) {
                        const u64 index = is_swizzled(t.type) ? sw.xs[x] + sw.ys[y] : u64{y} * bx + x;
                        const u8* block = base + index * block_bytes;
                        if (block + block_bytes > raw.data() + raw.size()) continue;
                        if (keep_blocks) {
                            std::memcpy(out.data.data() + at + (usize{y} * bx + x) * block_bytes, block, block_bytes);
                            continue;
                        }
                        if (bc_bytes) {
                            u8 texels[16][4];
                            decode_bc8(code, block, texels);
                            if (x * 4 + 4 <= lv.width && y * 4 + 4 <= lv.height) {
                                for (u32 row = 0; row < 4; ++row)
                                    std::memcpy(dst + (usize{y * 4 + row} * lv.width + x * 4) * 4, texels[row * 4], 16);
                                continue;
                            }
                            for (u32 i = 0; i < 16; ++i) {
                                const u32 tx = x * 4 + (i & 3), ty = y * 4 + i / 4;
                                if (tx < lv.width && ty < lv.height) std::memcpy(dst + (usize{ty} * lv.width + tx) * 4, texels[i], 4);
                            }
                            continue;
                        }
                        f32 texels[16][4];
                        decode_bc(code, block, texels, t.swizzle);
                        for (u32 i = 0; i < 16; ++i) {
                            const u32 tx = x * 4 + (i & 3), ty = y * 4 + i / 4;
                            if (tx < lv.width && ty < lv.height) put(dst + (usize{ty} * lv.width + tx) * out_bytes, texels[i]);
                        }
                    }
                }
            } else if (bytes8) {
                // (8-bit unsigned components without gamma: each output byte is a component's byte, or 0 or 255, as
                // the general path's floats would round to; a texel at a time, without them)
                const u32 row = lv.offset == 0 && is_linear(t.type) ? t.stride : ((lv.width + 7) & ~7u) * bits / 8;
                const u32 texel_bytes = bits / 8, tiles_w = (lv.width + 31) / 32;
                const u8* end = raw.data() + raw.size();
                const SwizzleTables sw(is_swizzled(t.type) ? lv.width : 0, is_swizzled(t.type) ? lv.height : 0, pw, ph);
                // (an output byte: the four bytes read shifted and masked, or a constant 0 or 255)
                u32 shift[4] = {}, mask[4] = {}, constant = 0;
                for (u32 k = 0; k < 4; ++k) {
                    if (byte_of[k] >= 0) shift[k] = 8 * static_cast<u32>(byte_of[k]), mask[k] = 0xFF;
                    else constant |= static_cast<u32>(-byte_of[k] - 1) << (8 * k);
                }
                const bool same = texel_bytes == 4 && shift[0] == 0 && shift[1] == 8 && shift[2] == 16 && shift[3] == 24 &&
                                  (mask[0] & mask[1] & mask[2] & mask[3]) == 0xFF;
                auto texel = [&](const u8* p) {
                    u32 v;
                    std::memcpy(&v, p, 4);
                    return ((v >> shift[0]) & mask[0]) | ((v >> shift[1]) & mask[1]) << 8 | ((v >> shift[2]) & mask[2]) << 16 |
                           ((v >> shift[3]) & mask[3]) << 24 | constant;
                };
                for (u32 y = 0; y < lv.height; ++y) {
                    u8* out_row = dst + usize{y} * lv.width * 4;
                    if (is_swizzled(t.type)) {
                        const u64 ys = sw.ys[y];
                        for (u32 x = 0; x < lv.width; ++x) {
                            const u8* p = base + (sw.xs[x] + ys) * texel_bytes;
                            if (p + 4 > end) continue;
                            const u32 v = texel(p);
                            std::memcpy(out_row + usize{x} * 4, &v, 4);
                        }
                    } else if (t.type == texture_type::kTiled) {
                        for (u32 x = 0; x < lv.width; ++x) {
                            const u8* p = base + ((u64{y / 32} * tiles_w + x / 32) * 1024 + (y % 32) * 32 + (x % 32)) * texel_bytes;
                            if (p + 4 > end) continue;
                            const u32 v = texel(p);
                            std::memcpy(out_row + usize{x} * 4, &v, 4);
                        }
                    } else {
                        const u8* p = base + u64{y} * row;
                        // (the texels whose four bytes are within the data: the rest left 0, as the general path leaves them)
                        const u64 left = p + 4 <= end ? static_cast<u64>(end - p - 4) / texel_bytes + 1 : 0;
                        const u32 n = static_cast<u32>(nx::min<u64>(lv.width, left));
                        if (same) {
                            std::memcpy(out_row, p, usize{n} * 4);
                            continue;
                        }
                        for (u32 x = 0; x < n; ++x) {
                            const u32 v = texel(p + usize{x} * texel_bytes);
                            std::memcpy(out_row + usize{x} * 4, &v, 4);
                        }
                    }
                }
            } else if (paletted) {
                // (a texel's 4 or 8 bits, its entry's bytes: a table lookup a texel)
                const u32 row = lv.offset == 0 && is_linear(t.type) ? t.stride : ((lv.width + 7) & ~7u) * bits / 8;
                const u32 tiles_w = (lv.width + 31) / 32;
                const usize limit = raw.size() - 8 - static_cast<usize>(base - raw.data());
                const SwizzleTables sw(is_swizzled(t.type) ? lv.width : 0, is_swizzled(t.type) ? lv.height : 0, pw, ph);
                u32* out_texels = reinterpret_cast<u32*>(dst);
                // (as the general path: a texel whose 8 bytes from it are past the data and its padding left 0)
                auto put_texel = [&](u32* o, u64 bit) {
                    if (bit / 8 > limit) return;
                    const u32 index = bits == 8 ? base[bit / 8] : (base[bit / 8] >> (bit % 8)) & 15;
                    std::memcpy(o, &palette8[index], 4);
                };
                for (u32 y = 0; y < lv.height; ++y) {
                    u32* out_row = out_texels + usize{y} * lv.width;
                    if (is_swizzled(t.type)) {
                        const u64 ys = sw.ys[y];
                        for (u32 x = 0; x < lv.width; ++x) put_texel(out_row + x, (sw.xs[x] + ys) * bits);
                    } else if (t.type == texture_type::kTiled) {
                        for (u32 x = 0; x < lv.width; ++x)
                            put_texel(out_row + x, ((u64{y / 32} * tiles_w + x / 32) * 1024 + (y % 32) * 32 + (x % 32)) * bits);
                    } else if (bits == 8) {
                        const u64 from = u64{y} * row;
                        const u32 n = from > limit ? 0 : static_cast<u32>(nx::min<u64>(lv.width, limit - from + 1));
                        const u8* p = base + from;
                        for (u32 x = 0; x < n; ++x) std::memcpy(out_row + x, &palette8[p[x]], 4);
                    } else {
                        const u64 row_bit = u64{y} * row * 8;
                        for (u32 x = 0; x < lv.width; ++x) put_texel(out_row + x, row_bit + u64{x} * bits);
                    }
                }
            } else if (floats32) {
                const u32 row = lv.offset == 0 && is_linear(t.type) ? t.stride : ((lv.width + 7) & ~7u) * 4;
                const u32 tiles_w = (lv.width + 31) / 32;
                const u8* end = raw.data() + raw.size();
                const SwizzleTables sw(is_swizzled(t.type) ? lv.width : 0, is_swizzled(t.type) ? lv.height : 0, pw, ph);
                // (each texel's word, as the general path reads it: one whose 8 bytes from it are past the data left 0)
                auto each = [&](auto&& texel) {
                    for (u32 y = 0; y < lv.height; ++y) {
                        u8* out_row = dst + usize{y} * lv.width * out_bytes;
                        for (u32 x = 0; x < lv.width; ++x) {
                            u64 offset;
                            if (is_swizzled(t.type)) offset = (sw.xs[x] + sw.ys[y]) * 4;
                            else if (t.type == texture_type::kTiled)
                                offset = ((u64{y / 32} * tiles_w + x / 32) * 1024 + (y % 32) * 32 + (x % 32)) * 4;
                            else offset = u64{y} * row + u64{x} * 4;
                            const u8* p = base + offset;
                            if (p + 8 > end) continue;
                            u32 v;
                            std::memcpy(&v, p, 4);
                            texel(out_row + usize{x} * out_bytes, v);
                        }
                    }
                };
                if (code == kU2F10F10F10) {
                    const bool alpha_low = t.swizzle == 2 || t.swizzle == 3 || t.swizzle == 6 || t.swizzle == 7;
                    const u32 at = alpha_low ? 2 : 0;
                    const u16* f10 = f10_halves.data();
                    each([&](u8* o, u32 v) {
                        u16 c[4] = {f10[(v >> at) & 0x3FF], f10[(v >> (at + 10)) & 0x3FF], f10[(v >> (at + 20)) & 0x3FF],
                                    alpha_halves[alpha_low ? v & 3 : v >> 30]};
                        u16 h[4];
                        for (u32 k = 0; k < 4; ++k) h[k] = float_of[k] >= 0 ? c[float_of[k]] : const_halves[k];
                        std::memcpy(o, h, 8);
                    });
                } else {
                    const u32 mask = code == kF32M ? 0x7FFFFFFFu : 0xFFFFFFFFu;
                    if (single) {
                        each([&](u8* o, u32 v) {
                            const u32 w = v & mask;
                            std::memcpy(o, &w, 4);
                        });
                    } else {
                    each([&](u8* o, u32 v) {
                        const f32 f = std::bit_cast<f32>(v & mask);
                        f32 c[4];
                        for (u32 k = 0; k < 4; ++k) c[k] = float_of[k] >= 0 ? f : float_const[k];
                        std::memcpy(o, c, 16);
                    });
                    }
                }
            } else {
                const u32 row = lv.offset == 0 && is_linear(t.type) ? t.stride : ((lv.width + 7) & ~7u) * bits / 8;
                const SwizzleTables sw(is_swizzled(t.type) ? lv.width : 0, is_swizzled(t.type) ? lv.height : 0, pw, ph);
                for (u32 y = 0; y < lv.height; ++y) {
                    for (u32 x = 0; x < lv.width; ++x) {
                        u64 bit;
                        if (is_swizzled(t.type)) bit = (sw.xs[x] + sw.ys[y]) * bits;
                        else if (t.type == texture_type::kTiled) {
                            const u32 tiles_w = (lv.width + 31) / 32;
                            const u64 tile = u64{y / 32} * tiles_w + x / 32;
                            bit = (tile * 1024 + (y % 32) * 32 + (x % 32)) * bits;
                        } else bit = (u64{y} * row) * 8 + u64{x} * bits;
                        const u8* p = base + bit / 8;
                        if (p + 8 > raw.data() + raw.size()) continue;
                        u64 texel = 0;
                        std::memcpy(&texel, p, 8);
                        texel >>= bit % 8;
                        if (bits < 64) texel &= (u64{1} << bits) - 1;
                        f32 rgba[4];
                        if (code == kP4 || code == kP8) {
                            std::memcpy(rgba, palette[texel & 0xFF], sizeof rgba);
                        } else {
                            f32 c[4];
                            u32 n;
                            bool w;
                            components(code, t.swizzle, texel, c, n, w);
                            swizzle(c, n, t.swizzle, rgba);
                        }
                        put(dst + (usize{y} * lv.width + x) * out_bytes, rgba);
                    }
                }
            }
            at += keep_blocks ? usize{(lv.width + 3) / 4} * ((lv.height + 3) / 4) * (code == kUBC1 ? 8 : 16)
                              : usize{lv.width} * lv.height * out_bytes;
        }
    }
    return out;
}

}  // namespace psvrecore::gxm
