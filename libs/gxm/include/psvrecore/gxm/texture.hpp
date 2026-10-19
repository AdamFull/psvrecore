#pragma once

// GXM textures: the four control words (the hardware's layout, which libgxm's texture functions fill and the
// GPU reads) and the texels they describe, decoded on the CPU into RGBA images the Vulkan backend samples.
//
// Control words (field positions as the SDK's texture definitions give them):
//   word 0: format extension 31, gamma 27-28, LOD bias 21-26 (31: none), mip clamp 17-20 (15: no mips),
//           mag filter 12, min filter 10, mip filter 9, U address 6-8, V address 3-5, explicit stride 1-2, 9-11
//           and 17-26 (linear strided textures, which have no mips or bias);
//   word 1: type 29-31, format 24-28, then width - 1 at 12-23 and height - 1 at 0-11 (log2 sizes at 16-19 and
//           0-3 for swizzled and cube textures of power-of-two sizes);
//   word 2: the data address (4-byte aligned), LOD minimum high bits 0-1;
//   word 3: normalize 31, swizzle 28-30, LOD minimum low bits 26-27, palette address / 64 at 0-25.
// Texel layouts: linear (rows, implicit stride a multiple of 8 texels), swizzled (Morton order, v in bit 0 of the
// address; for unequal sizes the low bits are interleaved and the rest appended), tiled (32x32 tiles, rows inside),
// cube (six swizzled faces: +x, -x, +y, -y, +z, -z).

#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/core/callable.h"

namespace psvrecore::gxm {

namespace texture_type {
inline constexpr u32 kSwizzled = 0, kCube = 2, kLinear = 3, kTiled = 4, kSwizzledArbitrary = 5, kLinearStrided = 6,
                     kCubeArbitrary = 7;
}

struct TextureInfo {
    u32 type = texture_type::kLinear;  // (word 1's type field)
    u32 base_format = 0;               // SceGxmTextureBaseFormat (the extension bit 31 and format bits 24-28)
    u32 swizzle = 0;                   // the format's swizzle mode (0-7)
    u32 width = 0, height = 0;
    u32 levels = 1;      // mip levels present
    bool mipmapped = false;  // (mip clamp other than "no mips": cube faces then hold full mip chains)
    u32 address = 0, stride = 0;  // stride: bytes per row of level 0 (linear types)
    u32 palette = 0;
    bool mag_linear = false, min_linear = false, mip_linear = false;
    u32 u_address = 0, v_address = 0;  // SceGxmTextureAddrMode
    u32 lod_bias = 31;  // (raw: 31 is no bias)
    u32 lod_min = 0;    // (raw)
    u32 gamma = 0;      // (word 0's gamma field)
    bool normalize = false;
};

// The control words' description (no validation: fields out of range are the caller's to reject).
TextureInfo decode_texture(const u32 words[4]);

// Control words for a texture, as the libgxm init functions set them up.
void encode_texture(const TextureInfo& info, u32 words[4]);

// Bits per texel of a base format (block formats: per texel, 4 for PVRT4/BC1/BC4, 8 for BC2/BC3/BC5); 0 if unknown.
u32 texel_bits(u32 base_format);

// The decoded texels: faces of mip levels, each level's texels as 16-bit floats or 8-bit unorm RGBA.
struct Texels {
    bool half = false;  // (RGBA16F; else RGBA8)
    u32 width = 0, height = 0, levels = 1, faces = 1;
    nx::vector<u8> data;  // face 0's levels, then face 1's, ...: each level width x height of its own, rows packed
};

using ReadGuest = nx::function_ref<bool(u32 address, void* out, u32 size)>;

// Reads and decodes a texture's texels. Fails on formats and layouts not implemented yet.
Result<Texels> read_texels(const TextureInfo& info, ReadGuest read);

// The bytes a texture's data occupies in guest memory (all levels and faces).
u32 texture_size(const TextureInfo& info);

}  // namespace psvrecore::gxm
