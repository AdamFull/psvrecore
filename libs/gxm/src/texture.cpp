#include "psvrecore/gxm/texture.hpp"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

#include "core/foundation/strings/format.h"

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
        case kU8U3U3U2: packed({2, 3, 3, 8}, false); return true;
        case kU16: packed({16}, false); wide = true; return true;
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

// The BC1-BC3 blocks: 4x4 texels as RGBA floats.
void decode_bc(u32 code, const u8* block, f32 out[16][4]) {
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

u64 morton(u32 x, u32 y, u32 w, u32 h) { return swizzled_index(x, y, w, h); }

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

void set_yuv_profile(u32 csc, u32 profile) {
    if (csc < 2 && profile < 4) yuv_profiles[csc].store(profile);
}

Result<Texels> read_texels(const TextureInfo& t, ReadGuest read) {
    const u32 code = code_of(t.base_format);
    const u32 bits = texel_bits(t.base_format);
    if (!bits || !t.width || !t.height || t.width > 4096 || t.height > 4096) return failure("texture: bad format or size");
    if (code >= kPVRT2 && code <= kPVRTII4) return failure("texture: PVRTC formats not implemented");
    if (code >= kYUV420P2 && code <= kYUV422) return read_yuv(t, code, read);
    if (code >= kUBC4 && code <= kSBC5) return failure("texture: format not implemented");
    if (code == kX8S8S8U8 || code == kX8U24 || code == kU32 || code == kS32 || code == kSE5M9M9M9 ||
        code == kF11F11F10 || code == kU2F10F10F10 || code == kU32U32)
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
    bool wide = t.gamma != 0;
    {
        f32 c[4];
        u32 n;
        bool w = false;
        if (components(code, t.swizzle, 0, c, n, w)) wide |= w;
    }
    const bool full = code == kF32 || code == kF32M || code == kF32F32;
    out.bytes = full ? 16 : wide ? 8 : 4;
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

    usize at = 0;
    for (u32 face = 0; face < out.faces; ++face) {
        for (const Level& lv : levels) {
            const u8* base = raw.data() + face * face_bytes + lv.offset;
            const u32 pw = pow2_ceil(lv.width), ph = pow2_ceil(lv.height);
            u8* dst = out.data.data() + at;
            if (block_format(code)) {
                // Blocks of 4x4 texels, in Morton order (swizzled layouts) or in rows of blocks.
                const u32 bx = (lv.width + 3) / 4, by = (lv.height + 3) / 4;
                const u32 block_bytes = code == kUBC1 ? 8 : 16;
                const u32 pbx = nx::max(pw / 4, 1u), pby = nx::max(ph / 4, 1u);
                for (u32 y = 0; y < by; ++y) {
                    for (u32 x = 0; x < bx; ++x) {
                        const u64 index = is_swizzled(t.type) ? morton(x, y, pbx, pby) : u64{y} * bx + x;
                        const u8* block = base + index * block_bytes;
                        if (block + block_bytes > raw.data() + raw.size()) continue;
                        f32 texels[16][4];
                        decode_bc(code, block, texels);
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
                for (u32 y = 0; y < lv.height; ++y) {
                    u8* out_row = dst + usize{y} * lv.width * 4;
                    for (u32 x = 0; x < lv.width; ++x) {
                        u64 index;
                        if (is_swizzled(t.type)) index = morton(x, y, pw, ph) * texel_bytes;
                        else if (t.type == texture_type::kTiled)
                            index = ((u64{y / 32} * tiles_w + x / 32) * 1024 + (y % 32) * 32 + (x % 32)) * texel_bytes;
                        else index = u64{y} * row + u64{x} * texel_bytes;
                        const u8* p = base + index;
                        if (p + 4 > end) continue;
                        u8* o = out_row + usize{x} * 4;
                        for (u32 k = 0; k < 4; ++k) o[k] = byte_of[k] >= 0 ? p[byte_of[k]] : static_cast<u8>(-byte_of[k] - 1);
                    }
                }
            } else {
                const u32 row = lv.offset == 0 && is_linear(t.type) ? t.stride : ((lv.width + 7) & ~7u) * bits / 8;
                for (u32 y = 0; y < lv.height; ++y) {
                    for (u32 x = 0; x < lv.width; ++x) {
                        u64 bit;
                        if (is_swizzled(t.type)) bit = morton(x, y, pw, ph) * bits;
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
            at += usize{lv.width} * lv.height * out_bytes;
        }
    }
    return out;
}

}  // namespace psvrecore::gxm
