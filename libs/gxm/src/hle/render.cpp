// Rendering a scene with the Vulkan backend, when it ends (synchronously: the scene's results are in guest memory
// when sceGxmEndScene returns).
//
// - The colour surface is an image of the surface's format, loaded from guest memory at the start (GXM loads
//   colour at the start of every scene) and written back at the end.
// - Each draw's secondary programs run first, one compute dispatch each, on the draw's secondary attributes (set
//   up as the driver does: gxm::fill_secondary). Guest memory the programs read (uniform buffers) is copied into
//   one buffer per scene, and its addresses in the secondary attributes are offsets into that buffer.
// - Then one render pass: per draw a pipeline (cached) from the translated programs and the draw's state, its
//   vertex and index data copied from guest memory.
//
// - Textures are decoded on the CPU (gxm/texture.hpp) into RGBA images, cached by their control words and a hash
//   of their data; units a program declares but the draw leaves unset read a white texel.
// - A texture over a surface the GPU drew (a render target sampled later) is made from the surface's image on the GPU
//   instead: copied (8-bit colour surfaces: texture_from_target) or converted by a compute shader to the bytes the
//   decoder would make of guest memory (HDR colour, depth: converted_texture, gxm/target_texture.hpp).
//
// - Depth and stencil: an image per scene, cleared to the surface's background values; a surface in force load
//   or force store mode is loaded from guest memory at the start or stored back at the end (linear or tiled,
//   every format but the DF32M mask bits, which are not tracked: stored as the background mask).
// - A scene without a colour surface (depth only) renders at the render target's size.
//
// - Visibility testing: an occlusion query per draw that tests it; when the scene is done, its count is added to
//   (or, SET, sets to 1) the counter in core 0's visibility buffer (the game sums the cores' buffers).
//
// - Colour surfaces linear, tiled or swizzled: loaded from and written back to guest memory in their layout, drawn
//   as images in rows.
//
// Not yet: multisampling.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#include "psvrecore/gxm/surface.hpp"
#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/gxm/half_check.hpp"
#include "psvrecore/gxm/target_texture.hpp"
#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/profile.hpp"
#include "psvrecore/runtime/host.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "psvrecore/vulkan/renderdoc.hpp"
#include "prefetch.hpp"
#include "shader_cache.hpp"
#include "core/foundation/platform/os.h"
#include "state.hpp"
#include "core/foundation/core/hash.h"
#include "core/foundation/strings/format.h"

#if PSVRECORE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

namespace psvrecore::gxm::hle {

namespace {

namespace vk = vulkan;

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

// ---- Formats ---------------------------------------------------------------------------------------------------

// How a surface's pixels are kept in its image when no image format has their layout: converted from guest memory
// when the scene loads them and back when it writes them back (the image a wider format that holds every value).
enum class Packing : u8 {
    None,
    U2F10F10F10,  // (A in the top 2 bits, then B, G and R as unsigned 10-bit floats from bit 20, 10 and 0) as F16x4
    U8U8U8,       // (three bytes, R first: no 24-bit attachments) as U8x4, A 1
    U4U4U4U4,     // (a 16-bit word, R in the low four bits, then G, B and A: no device must draw into 4444) as U8x4
};

struct ColorFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 bytes = 0;  // per pixel in the image
    u8 swizzle[4] = {0, 1, 2, 3};  // (FragmentShader::swizzle)
    Packing packing = Packing::None;
    u8 gamma = 0;  // (FragmentShader::gamma, a lane mask: the surface's gamma mode, set where the scene's surface is known)
    // The surface is drawn through its image's sRGB view (`gamma` is 0 then: the hardware encodes what is written,
    // and blends and reads the colour in linear light), set where the scene's surface is known.
    bool srgb = false;
    VkFormat attachment_format() const { return srgb ? vk::srgb_format_of(format) : format; }
    u32 guest_bytes() const {  // per pixel in guest memory
        return packing == Packing::None       ? bytes
               : packing == Packing::U8U8U8   ? 3
               : packing == Packing::U4U4U4U4 ? 2
                                              : 4;
    }
};

// The image format whose memory layout is the surface's (or that holds it: `packing`), and the order the output
// register's lanes go to it.
bool color_format(u32 gxm, ColorFormat& out) {
    switch (gxm) {
        case 0x00000000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {0, 1, 2, 3}}; return true;      // U8U8U8U8_ABGR
        case 0x00100000u: out = {VK_FORMAT_B8G8R8A8_UNORM, 4, {0, 1, 2, 3}}; return true;      // U8U8U8U8_ARGB
        case 0x00200000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {3, 2, 1, 0}}; return true;      // U8U8U8U8_RGBA
        case 0x00300000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {3, 0, 1, 2}}; return true;      // U8U8U8U8_BGRA
        case 0x01000000u: out = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, {0, 1, 2, 3}}; return true;  // F16x4 ABGR
        case 0x01100000u: out = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, {2, 1, 0, 3}}; return true;  // F16x4 ARGB
        case 0x00800000u: out = {VK_FORMAT_R16G16_SFLOAT, 4, {0, 1, 4, 5}}; return true;       // F16F16_GR
        case 0x10800000u: out = {VK_FORMAT_R32_SFLOAT, 4, {0, 4, 4, 5}}; return true;          // F32_R
        case 0x80800000u: out = {VK_FORMAT_R8_UNORM, 1, {0, 4, 4, 5}}; return true;            // U8_R
        case 0x80900000u: out = {VK_FORMAT_R8_UNORM, 1, {3, 4, 4, 5}}; return true;            // U8_A
        case 0x11000000u: out = {VK_FORMAT_R32G32_SFLOAT, 8, {0, 1, 4, 5}}; return true;       // F32F32_GR
        // (U2U10U10U10_ABGR: A in the top two bits, then B, G and R from bit 20, 10 and 0, as Vulkan's packed format,
        // which every device can draw into and blend)
        case 0x60800000u: out = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, {0, 1, 2, 3}}; return true;
        case 0x40800000u: out = {VK_FORMAT_R16_UNORM, 2, {0, 4, 4, 5}}; return true;           // U16_R
        // (F11F11F10_BGR: unsigned floats, R from bit 0, G from 11, B from 22, as Vulkan's packed format)
        case 0x21000000u: out = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, {0, 1, 2, 5}}; return true;
        case 0x10000000u:  // U8U8U8_BGR (LittleBigPlanet)
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {0, 1, 2, 3}, Packing::U8U8U8};
            return true;
        case 0x50000000u:  // U4U4U4U4_ABGR (Killzone Mercenary)
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {0, 1, 2, 3}, Packing::U4U4U4U4};
            return true;
        case 0x50100000u:  // U4U4U4U4_ARGB
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {2, 1, 0, 3}, Packing::U4U4U4U4};
            return true;
        case 0x41000000u:  // U2F10F10F10_ABGR
            out = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, {0, 1, 2, 3}, Packing::U2F10F10F10};
            return true;
        default: return false;
    }
}

// An unsigned 10-bit float (a half's exponent and top five mantissa bits) as a half.
u16 f10_to_half(u32 v) { return static_cast<u16>((v & 0x3FF) << 5); }
// A surface's pixels from guest memory into its image's (`packing`), and back: `count` pixels.
void unpack_pixels(Packing packing, const u8* guest, u8* image, usize count) {
    if (packing == Packing::U8U8U8) {
        for (usize i = 0; i < count; ++i) {
            image[i * 4] = guest[i * 3], image[i * 4 + 1] = guest[i * 3 + 1], image[i * 4 + 2] = guest[i * 3 + 2];
            image[i * 4 + 3] = 0xFF;
        }
    }
    if (packing == Packing::U2F10F10F10) {
        static const u16 kAlpha[4] = {0x0000, 0x3555, 0x3955, 0x3C00};  // (0, 1/3, 2/3, 1)
        for (usize i = 0; i < count; ++i) {
            u32 v;
            std::memcpy(&v, guest + i * 4, 4);
            const u16 h[4] = {f10_to_half(v), f10_to_half(v >> 10), f10_to_half(v >> 20), kAlpha[v >> 30]};
            std::memcpy(image + i * 8, h, 8);
        }
    }
    if (packing == Packing::U4U4U4U4) {
        for (usize i = 0; i < count; ++i) {
            const u32 v = guest[i * 2] | u32{guest[i * 2 + 1]} << 8;
            for (u32 c = 0; c < 4; ++c) image[i * 4 + c] = static_cast<u8>((v >> (c * 4) & 0xF) * 17);
        }
    }
}
void pack_pixels(Packing packing, const u8* image, u8* guest, usize count) {
    if (packing == Packing::U8U8U8) {
        for (usize i = 0; i < count; ++i)
            guest[i * 3] = image[i * 4], guest[i * 3 + 1] = image[i * 4 + 1], guest[i * 3 + 2] = image[i * 4 + 2];
    }
    if (packing == Packing::U2F10F10F10) {
        // (as the GPU's textures from such a surface pack; a row at a time, aligned)
        constexpr usize kRun = 1024;
        u16 h[kRun * 4];
        u32 v[kRun];
        for (usize i = 0; i < count; i += kRun) {
            const usize n = nx::min(kRun, count - i);
            std::memcpy(h, image + i * 8, n * 8);
            pack_u2f10f10f10(h, v, n);
            std::memcpy(guest + i * 4, v, n * 4);
        }
    }
    if (packing == Packing::U4U4U4U4) {
        for (usize i = 0; i < count; ++i) {
            u32 v = 0;
            for (u32 c = 0; c < 4; ++c) v |= (u32{image[i * 4 + c]} * 15 + 127) / 255 << (c * 4);
            guest[i * 2] = static_cast<u8>(v), guest[i * 2 + 1] = static_cast<u8>(v >> 8);
        }
    }
}

// The Vulkan input format of an attribute (vertex_input_read: how the shader takes it). Three 8- or 16-bit
// components are fetched as four (vertex formats with three such components are often not supported).
VkFormat attribute_format(const VertexInput& in) {
    const u32 n = nx::clamp<u32>(in.components, 1, 4);
    const VertexRead read = vertex_input_read(in);
    auto pick = [&](const VkFormat* four) { return four[n - 1]; };
    static const VkFormat u8[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT};
    static const VkFormat s8[] = {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT};
    static const VkFormat u16[] = {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT};
    static const VkFormat s16[] = {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT};
    static const VkFormat un8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat sn8[] = {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM};
    static const VkFormat un16[] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
    static const VkFormat sn16[] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat f16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    static const VkFormat f32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat u32f[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    if (read == VertexRead::Raw) {
        const u32 bits = vertex_input_bits(in.format);
        return pick(bits == 8 ? u8 : bits == 16 ? u16 : u32f);
    }
    switch (in.format) {
        case 0: return pick(u8);
        case 1: return pick(s8);
        case 2: return pick(u16);
        case 3: return pick(s16);
        case 4: return pick(un8);
        case 5: return pick(sn8);
        case 6: return pick(un16);
        case 7: return pick(sn16);
        case 8: return pick(f16);
        default: return pick(f32);
    }
}

// Bytes an attribute reads from a vertex (as its Vulkan format fetches them).
u32 attribute_bytes(const VertexInput& in) {
    const u32 n = nx::clamp<u32>(in.components, 1, 4);
    const u32 bits = vertex_input_bits(in.format);
    return bits == 32 ? 4 * n : (bits / 8) * (n == 3 ? 4 : n);
}

VkBlendFactor blend_factor(u32 f) {
    static const VkBlendFactor kMap[] = {
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,           VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        VK_BLEND_FACTOR_ONE,  // (DST_ALPHA_SATURATE: Vulkan has none)
    };
    return f < std::size(kMap) ? kMap[f] : VK_BLEND_FACTOR_ONE;
}

// ---- Depth/stencil surfaces in guest memory --------------------------------------------------------------------

struct DepthLayout {
    u32 depth_bytes = 0;  // per sample at the depth address (0: no depth)
    bool stencil = false;  // a stencil byte per sample at the stencil address
    bool packed = false;   // S8D24: the stencil is the depth word's top byte
    bool mask = false;     // DF32M: the sign bit is the mask
};

bool depth_layout(u32 format, DepthLayout& out) {
    switch (format) {
        case 0x0044000u: out = {4, false, false, false}; return true;  // DF32
        case 0x00CC000u: out = {4, false, false, true}; return true;   // DF32M
        case 0x0066000u: out = {4, true, false, false}; return true;   // DF32_S8
        case 0x00EE000u: out = {4, true, false, true}; return true;    // DF32M_S8
        case 0x0022000u: out = {0, true, false, false}; return true;   // S8
        case 0x1266000u: out = {4, false, true, false}; return true;   // S8D24
        case 0x2444000u: out = {2, false, false, false}; return true;  // D16
        default: return false;
    }
}

// The index of a sample: rows of the stride (in samples), or 32 x 32 tiles in rows, each tile's samples in rows.
u32 depth_sample(const DepthStencilSurface& s, u32 x, u32 y) {
    const u32 stride = (s.background_control >> 16) * 32;
    if (!(s.control & kDsTiled)) return y * stride + x;
    return ((y / 32) * (stride / 32) + x / 32) * 1024 + (y % 32) * 32 + x % 32;
}

// Samples a surface of this size spans.
u32 depth_samples(const DepthStencilSurface& s, u32 width, u32 height) {
    const u32 stride = (s.background_control >> 16) * 32;
    if (s.control & kDsTiled) return (height + 31) / 32 * (stride / 32) * 1024;
    return (height - 1) * stride + width;
}

bool depth_surface_valid(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height) {
    const u32 n = depth_samples(s, width, height);
    if (l.depth_bytes && !memory().valid(s.depth_data, n * l.depth_bytes, true)) return false;
    if (l.stencil && !memory().valid(s.stencil_data, n, true)) return false;
    return true;
}

// The guest memory a depth/stencil surface of this size spans: its depth samples and its stencil bytes (either may be
// none).
struct DepthRanges {
    u32 depth = 0, depth_bytes = 0, stencil = 0, stencil_bytes = 0;
};
DepthRanges depth_ranges(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height) {
    const u32 n = depth_samples(s, width, height);
    DepthRanges out;
    if (l.depth_bytes) out.depth = s.depth_data, out.depth_bytes = n * l.depth_bytes;
    if (l.stencil) out.stencil = s.stencil_data, out.stencil_bytes = n;
    return out;
}

// What guest memory holds there: the depth's bytes, then the stencil's.
void depth_contents(const DepthRanges& g, nx::vector<u8>& out) {
    out.clear();
    out.reserve(usize{g.depth_bytes} + g.stencil_bytes);
    if (g.depth_bytes) out.insert(out.end(), memory().host(g.depth), memory().host(g.depth) + g.depth_bytes);
    if (g.stencil_bytes) out.insert(out.end(), memory().host(g.stencil), memory().host(g.stencil) + g.stencil_bytes);
}

bool depth_contents_equal(const DepthRanges& g, const nx::vector<u8>& had) {
    if (had.empty() || had.size() != usize{g.depth_bytes} + g.stencil_bytes) return false;
    return (!g.depth_bytes || std::memcmp(memory().host(g.depth), had.data(), g.depth_bytes) == 0) &&
           (!g.stencil_bytes || std::memcmp(memory().host(g.stencil), had.data() + g.depth_bytes, g.stencil_bytes) == 0);
}

// Whether the depth image's stencil aspect holds the mask bit (bit 0): DF32M surfaces, which have no stencil.
bool mask_in_stencil(const DepthLayout& l) { return l.mask && !l.stencil; }

// The surface's samples as the depth image's aspects take them: depth (f32, or 24-bit unorm words for a
// D24_UNORM_S8_UINT image) and stencil bytes (or the mask bits).
// Samples of a row from x on that follow each other in the surface: to the end of the row inside its 32x32 tile, or
// of the row.
u32 depth_run(const DepthStencilSurface& s, u32 x, u32 width) {
    return (s.control & kDsTiled) ? nx::min(32 - x % 32, width - x) : width - x;
}

// 32-bit float depth, the image's words the surface's (but for a mask bit that is the background's): a run at a time.
bool plain_depth(const DepthLayout& l, bool unorm24) { return l.depth_bytes == 4 && !l.packed && !unorm24 && !mask_in_stencil(l); }

void read_depth_surface(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height, bool unorm24,
                        u32* depth, u8* stencil) {
    if (plain_depth(l, unorm24)) {
        const u8* d = memory().host(s.depth_data);
        const u8* st = l.stencil ? memory().host(s.stencil_data) : nullptr;
        const u8 background = static_cast<u8>(s.background_control & 0xFF);
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width;) {
                const u32 n = depth_run(s, x, width), i = depth_sample(s, x, y), at = y * width + x;
                for (u32 k = 0; k < n; ++k) {
                    u32 w;
                    std::memcpy(&w, d + (usize{i} + k) * 4, 4);
                    f32 z = std::bit_cast<f32>(l.mask ? w & 0x7FFFFFFFu : w);
                    z = nx::clamp(std::isnan(z) ? 0.0f : z, 0.0f, 1.0f);
                    depth[at + k] = std::bit_cast<u32>(z);
                }
                if (st) std::memcpy(stencil + at, st + i, n);
                else std::memset(stencil + at, background, n);
                x += n;
            }
        return;
    }
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 i = depth_sample(s, x, y), at = y * width + x;
            f32 z = s.background_depth;
            u8 st = static_cast<u8>(mask_in_stencil(l) ? (s.background_control >> 8) & 1 : s.background_control & 0xFF);
            if (l.depth_bytes == 4) {
                u32 w;
                std::memcpy(&w, memory().host(s.depth_data + i * 4), 4);
                if (l.packed) z = static_cast<f32>(w & 0xFFFFFF) / 16777215.0f, st = static_cast<u8>(w >> 24);
                else z = std::bit_cast<f32>(l.mask ? w & 0x7FFFFFFFu : w);
                if (mask_in_stencil(l)) st = static_cast<u8>(w >> 31);
            } else if (l.depth_bytes == 2) {
                u16 h;
                std::memcpy(&h, memory().host(s.depth_data + i * 2), 2);
                z = static_cast<f32>(h) / 65535.0f;
            }
            if (l.stencil) st = *memory().host(s.stencil_data + i);
            z = nx::clamp(std::isnan(z) ? 0.0f : z, 0.0f, 1.0f);
            depth[at] = unorm24 ? static_cast<u32>(std::lround(z * 16777215.0f)) : std::bit_cast<u32>(z);
            stencil[at] = st;
        }
    }
}

// And back. (DF32M_S8: the mask bit of every sample is the surface's background mask.) Into guest memory, or into
// `depth_out` (and `stencil_out`): the surface's bytes from its depth address (and its stencil's), elsewhere.
// `keep`: only the samples (indices in the surface) it takes.
void write_depth_surface(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height, bool unorm24,
                         const u32* depth, const u8* stencil, u8* depth_out = nullptr, u8* stencil_out = nullptr,
                         nx::function_ref<bool(u32)> keep = {}) {
    const u32 mask = l.mask && (s.background_control & 0x100) ? 0x80000000u : 0;
    u8* const d = depth_out ? depth_out : l.depth_bytes ? memory().host(s.depth_data) : nullptr;
    u8* const st = !l.stencil ? nullptr : stencil_out ? stencil_out : memory().host(s.stencil_data);
    if (plain_depth(l, unorm24) && !keep) {
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width;) {
                const u32 n = depth_run(s, x, width), i = depth_sample(s, x, y), at = y * width + x;
                if (!mask) {
                    std::memcpy(d + usize{i} * 4, depth + at, usize{n} * 4);
                } else {
                    for (u32 k = 0; k < n; ++k) {
                        const u32 w = depth[at + k] | mask;
                        std::memcpy(d + (usize{i} + k) * 4, &w, 4);
                    }
                }
                if (st) std::memcpy(st + i, stencil + at, n);
                x += n;
            }
        return;
    }
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 i = depth_sample(s, x, y), at = y * width + x;
            if (keep && !keep(i)) continue;
            const f32 z = unorm24 ? static_cast<f32>(depth[at] & 0xFFFFFF) / 16777215.0f : std::bit_cast<f32>(depth[at]);
            if (l.depth_bytes == 4) {
                const u32 w = l.packed ? static_cast<u32>(std::lround(z * 16777215.0f)) | u32{stencil[at]} << 24
                                       : std::bit_cast<u32>(z) | (mask_in_stencil(l) ? u32{stencil[at] & 1u} << 31 : mask);
                std::memcpy(d + usize{i} * 4, &w, 4);
            } else if (l.depth_bytes == 2) {
                const u16 h = depth16_of(z);
                std::memcpy(d + usize{i} * 2, &h, 2);
            }
            if (st) st[i] = stencil[at];
        }
    }
}

// ---- The renderer ----------------------------------------------------------------------------------------------

// The surface's pixels as rows of its width (an image's), from guest memory, and back.
void gather(const SurfaceLayout& l, u32 address, u8* out) {
    const usize row = usize{l.width} * l.bytes;
    for (u32 y = 0; y < l.height; ++y)
        for (u32 x = 0; x < l.width;) {
            const u32 n = l.run(x);
            std::memcpy(out + y * row + usize{x} * l.bytes, memory().host(address + static_cast<u32>(l.offset(x, y))),
                        usize{n} * l.bytes);
            x += n;
        }
}
void scatter(const SurfaceLayout& l, u32 address, const u8* in) {
    const usize row = usize{l.width} * l.bytes;
    for (u32 y = 0; y < l.height; ++y)
        for (u32 x = 0; x < l.width;) {
            const u32 n = l.run(x);
            std::memcpy(memory().host(address + static_cast<u32>(l.offset(x, y))), in + y * row + usize{x} * l.bytes,
                        usize{n} * l.bytes);
            x += n;
        }
}

// A range of guest memory a scene's write-back wrote, watched for writes since (GuestMemory::watch_writes): while
// its pages' write counts are what they were once watched, guest memory holds what was written back, without
// comparing it with a copy (`written`). Writes the counts do not see: one by the game during the write-back itself
// (between the pages being opened for it and watched again). PSVRECORE_WRITE_WATCH=0: compared as before;
// =verify: compared too, a difference the watch did not see logged.
struct WrittenWatch {
    u32 address = 0, size = 0;
    u64 writes = 0;
    bool armed = false;
};
u32 write_watch_mode() {  // (0 off, 1 on, 2 verify)
    static const u32 mode = [] {
        const char* v = std::getenv("PSVRECORE_WRITE_WATCH");
        if (v && *v == '0') return 0u;
        if (v && std::strcmp(v, "verify") == 0) return 2u;
        return 1u;
    }();
    return mode;
}
// Watched from now (before guest memory there is read or relied on: a write after this is counted).
void watch_range(WrittenWatch& w, u32 address, u32 size) {
    w.address = address, w.size = size;
    w.armed = write_watch_mode() && size && memory().watch_writes(address, size);
    w.writes = w.armed ? memory().writes(address, size) : 0;
}
// Nothing has written [address, address + size) since it was watched (the same range).
std::atomic<u64> g_watch_seen{0}, g_watch_compared{0};  // (PSVRECORE_GXM_PROFILE: answered by the watch, compared)
bool unwritten(const WrittenWatch& w, u32 address, u32 size) {
    const bool yes = w.armed && w.address == address && w.size == size && memory().writes(address, size) == w.writes;
    (yes ? g_watch_seen : g_watch_compared).fetch_add(1, std::memory_order_relaxed);
    return yes;
}
// (PSVRECORE_WRITE_WATCH=verify: a range the watch found unwritten, which a comparison found changed)
void unseen_write(const char* what, u32 address, u32 size) {
    static u32 reported = 0;
    if (reported++ < 20)
        nx::logw(logs::gpu, "write watching: {} at 0x{:08x} ({} bytes) changed without a write being seen", what, address,
                 size);
}

// What a colour surface's write-back left in guest memory: its pixels in order, as guest memory has them. The
// read-back buffer they came in, kept rather than copied (given back to the free buffers when replaced), or bytes of
// its own.
struct Snapshot {
    nx::vector<u8> bytes;
    vk::Buffer buffer{};
    usize buffer_bytes = 0;
    bool empty() const { return buffer.buffer ? buffer_bytes == 0 : bytes.empty(); }
    usize size() const { return buffer.buffer ? buffer_bytes : bytes.size(); }
    const u8* data() const { return buffer.buffer ? static_cast<const u8*>(buffer.mapped) : bytes.data(); }
    void clear();
    void assign(const u8* from, const u8* to) {
        clear();
        bytes.assign(from, to);
    }
    void adopt(vk::Buffer& b, usize n) {  // (b given up: this holds it now)
        clear();
        buffer = b, buffer_bytes = n;
        b = {};
    }
    nx::vector<u8> take_bytes() {  // (the bytes' storage, for the next ones; empty)
        clear();
        return std::move(bytes);
    }
};

struct Target {
    u32 address = 0, format = 0, width = 0, height = 0;
    SurfaceLayout layout;
    vk::Image image;  // the surface's size: loaded from guest memory and read back to it
    // At an internal resolution above 1: the surface as drawn (scale times its size).
    vk::Image scaled;
    u32 scale = 1;
    // The surface's bytes as last written back. While guest memory still holds them, `image` (and `scaled`) is the
    // surface and is not loaded again; once anything else wrote there, it is loaded from guest memory.
    Snapshot written;
    WrittenWatch watch;  // (the surface's span, once written back)
    u64 generation = 0;  // (a scene drawing it submitted: +1)
};

// What a scene's store of a depth surface left in guest memory: how many bytes, and the bytes themselves only where
// they may be compared with guest memory (PSVRECORE_WRITE_WATCH=0 or verify, or a range that could not be watched).
// Watched, nothing compares them: a copy of a 960 x 544 surface (2 MiB) each scene was a third of the copying the
// thread that ends WipEout 2048's scenes did.
struct StoredDepth {
    nx::vector<u8> bytes;
    usize size_ = 0;
    bool empty() const { return size_ == 0; }
    usize size() const { return size_; }
    bool held() const { return size_ && bytes.size() == size_; }  // (`bytes` are what was stored)
    void clear() { size_ = 0, bytes.clear(); }
};
// Whether a store's bytes are kept for comparing: not when every range of it is watched (and not verified).
bool keep_stored_bytes(std::initializer_list<const WrittenWatch*> watches) {
    if (write_watch_mode() != 1) return true;
    for (const WrittenWatch* w : watches)
        if (w->size && !w->armed) return true;
    return false;
}

// A depth surface a depth-only scene stores (a shadow map), at the internal resolution: its depth image, kept from
// scene to scene, a view of its depth for textures, and the bytes last stored to guest memory (while they are still
// there, the image is the surface).
struct DepthTarget {
    u32 address = 0, control = 0, stride = 0, width = 0, height = 0;
    u32 scale = 1;
    vk::Image image;
    VkImageView depth_view = VK_NULL_HANDLE;
    StoredDepth written;
    u32 stencil_address = 0;  // (a kept one's: its key has the stencil's address too)
    u64 used = 0;             // (a kept one's: the scene it was last used in)
    u64 generation = 0;       // (a kept one's: +1 each time a scene's store of it is written back)
    WrittenWatch watch[2];    // (its depth's bytes and its stencil's, once stored: `written`)
    WrittenWatch background_watch;  // (background_bytes' range, once compared or taken)
    // (a kept one's, for textures made on the GPU from it that read past what the scenes store (converted_texture):
    // guest memory's bytes from its address as they were, on the CPU and in `background` once uploaded; `version` +1
    // each time they are taken again)
    nx::vector<u8> background_bytes;
    vk::DeviceBuffer background{};
    bool background_uploaded = false;
    u64 background_version = 0;
};

struct CachedTexture {
    vk::Image image;
    VkSampler sampler = VK_NULL_HANDLE;
    u64 used = 0;   // (the scene it was last used in)
    u64 bytes = 0;  // (its texels)
};

// How long a texture no scene uses is kept (scenes: at a game's usual 2-4 a frame), and the most the cache holds (the
// least recently used go first past it): a texture's content changing every frame (a render target read back as a
// texture) makes a new one each time. Under kTextureKeep, kept a minute: a song in Persona 4: Dancing All Night
// went back to a stage of 44 textures 45 s after, and kept for a second or two they were decoded again each time
// (10-18 ms of that scene on a phone); past it, a second or two. The budget: an eighth of the device's largest
// device-local heap, from 256 MiB to 512 MiB (texture_budget): WipEout 2048's races sample up to about 400 MiB of
// textures (PVRTC decoded to RGBA8, 8 bytes a texel for each half byte), and under 256 MiB a stretch of track evicted
// and decoded its textures again each frame (100-120 ms frames on a PC for seconds).
constexpr u64 kTextureUnusedScenes = 180, kTextureKeptScenes = 3600;
constexpr u64 kTextureKeep = 96ull << 20, kTextureBudgetLeast = 256ull << 20, kTextureBudgetMost = 512ull << 20;

struct Renderer {
    bool tried = false;
    vk::Device* device = nullptr;
    nx::unique_ptr<vk::Device> owned;  // (the device, unless the options gave one)
    nx::unique_ptr<vk::RenderPasses> passes;
    nx::unique_ptr<vk::Pipelines> pipelines;
    nx::unique_ptr<vk::Samplers> samplers;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    nx::hash_map<u64, VkShaderModule> modules;      // (by shader key)
    // The modules and secondary pipelines again by a quick hash of the program's content and what else the shader
    // key takes: found without a SHA-256 for each draw.
    nx::hash_map<u64, VkShaderModule> quick_modules;
    nx::hash_map<u64, VkPipeline> quick_compute;
    nx::hash_map<u64, ShaderKey> module_keys;       // (by module: a pipeline's shaders, to record it)
    nx::hash_map<u64, VkPipeline> compute;          // secondary programs (by shader key)
    nx::hash_map<u64, nx::string> failed_programs;  // (reported once)
    ShaderCache cache;
    bool warming = false;
    // Made while the game ran (not by warm_up): what the cache did not have.
    std::atomic<u32> translated{0}, made_pipelines{0};
    std::atomic<bool> pipelines_changed{false};
    nx::mutex targets_lock;  // (a scene being rendered; the display reading a scaled surface)
    nx::vector<nx::unique_ptr<Target>> targets;
    nx::vector<nx::unique_ptr<DepthTarget>> depth_targets;
    // The depth/stencil images of the surfaces scenes store, kept for the next scene that loads the surface (one
    // drawn over in more than one scene of a frame): never sampled as textures, unlike depth_targets.
    nx::vector<nx::unique_ptr<DepthTarget>> kept_depths;
    nx::hash_map<u32, bool> scalable;  // (by VkFormat: blits to and from it, and it is drawn into)
    vk::Image white, white_cube;
    VkSampler sampler = VK_NULL_HANDLE;
    nx::hash_map<u64, CachedTexture> textures;
    u64 texture_bytes = 0;  // (the cached textures' texels)
    u64 texture_budget = 0;  // (texture_budget: found once)
    // (by a hash of the control words: the content key last cached for them, whose image the next content those
    // words show is uploaded into: a render target read as a texture is one image, not one a frame)
    nx::hash_map<u64, u64> by_words;
    // Textures made on the GPU from a surface's image (texture_from_target), to be recorded at the start of the
    // scene's commands: the cached texture's key, the surface, and the columns past the surface's width (from guest
    // memory, in a buffer) when the texture is wider. `scale` above 1: made from the surface's scaled image, at its
    // scale (the strip's texels repeated over scale x scale blocks).
    struct TargetCopy {
        u64 key = 0;
        Target* from = nullptr;
        u32 x = 0, y = 0;  // (where in the surface the texture starts)
        u32 width = 0, height = 0;
        u32 scale = 1;
        vk::Buffer strip{};
        u32 strip_x = 0, strip_w = 0;
    };
    nx::vector<TargetCopy> target_copies;
    // Textures made on the GPU from a surface's image by a conversion (converted_texture: target_texture.hpp), recorded
    // with the copies: the image's region copied into a work buffer, the shader run, its texels copied into the
    // texture. `depth`: a kept depth image (its middle samples at a scale: rows `step` apart from `first`), else the
    // colour surface's image (its scaled one at a scale). A background to upload first: its staging buffer.
    // `from_image`: an HDR colour surface's, made from its image directly (target_image_shader: no work buffer).
    struct TargetConversion {
        u64 key = 0;
        Target* color = nullptr;
        DepthTarget* depth = nullptr;
        u32 x = 0, y = 0, w = 0, h = 0;  // (the image's texels copied: rows of w; depth at a scale: rows y * step + first)
        u32 step = 1, first = 0;
        TargetTextureConstants k{};
        vk::DeviceBuffer work{};
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool from_image = false;
        vk::Buffer background_staging{};
        vk::DeviceBuffer retired_background{};  // (a smaller one this replaced: earlier scenes may read it)
        // (PSVRECORE_TARGET_TEXTURES=verify: the decoder's texels from guest memory, and the GPU's read back)
        // A texture made from the image of a surface a pending scene draws (`pending`): what guest memory holds once
        // that scene is written back, decoded when this one is done.
        nx::vector<u8> expected;
        vk::Buffer verify{};
        bool pending = false;
        u64 generation = 0;  // (a depth surface's: the kept image's generation the texture is of)
        usize verify_bytes = 0;
        TextureInfo info{};
    };
    nx::vector<TargetConversion> target_conversions;
    TargetConverter converter;  // (not ready: such textures are read back and decoded)
    nx::hash_map<u64, std::optional<TargetDecode>> target_decodes;  // (by format, swizzle and source)
    nx::vector<vk::DeviceBuffer> free_work;  // (the conversions' work buffers, for the next)
    // New textures' texels (texture_for), uploaded by the scene's commands before its draws: not waited for, as a
    // copy submitted alone would be (behind the scenes the GPU still has). Their buffers go when the scene is done.
    // A YUV420 texture (a movie's frame) is converted by the commands too (yuv_set: the staging buffer holds the
    // planes and the conversion's tables, and the pixels it writes, copied from there).
    struct TextureUpload {
        u64 key;  // (the texture's in `textures`)
        vk::Buffer staging;
        nx::vector<VkBufferImageCopy> regions;
        VkDescriptorSet yuv_set = VK_NULL_HANDLE;
        Yuv420Constants yuv{};
    };
    nx::vector<TextureUpload> texture_uploads;
    // The YUV420 conversion's pipeline (none: converted on the CPU, by read_texels), and its sets' pool.
    struct YuvPipeline {
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
    } yuv;
    // Depth/stencil images a scene drew with and no longer needs, for the next scene of their size: destroying one
    // (its memory freed, its framebuffers forgotten) took 3-25 ms on a desktop's driver, scene after scene.
    nx::vector<vk::Image> free_depths;
    // Host buffers scenes are done with (their vertices, indices, uniforms, secondary attributes, readbacks; new
    // textures' texels), for the next scenes: not made and freed six times or more a scene.
    nx::vector<vk::Buffer> free_buffers;
    u64 free_buffer_bytes = 0;
    // A texture's content key, by its control words: its memory watched for writes once hashed (watch_writes), and
    // hashed again once written, not for each scene (a song's scenes hashed 7 MB each: a sixth of the thread that
    // ends them, on a phone). One written each time it is looked at, three times running, is hashed each time for
    // a while (a frame's CPU writes would fault for each page); a movie's frames are.
    struct ContentMemo {
        u64 key = 0, contents = 0, writes = 0, hash_until = 0;  // (contents: texture_contents; hash_until: a scene number)
        u32 address = 0, size = 0, palette = 0;
        u8 strikes = 0;
        bool watched = false;
    };
    nx::hash_map<u64, ContentMemo> content_memo;
    // (shaders compiled ahead whose SPIR-V the cache does not have: kept here, packed, and added to it when a draw
    // uses them; the others are only marked prepared there)
    nx::hash_map<u64, std::pair<ShaderKey, nx::vector<u8>>> unsaved;
    // (descriptor pools reset, for the next scenes: by size, kPoolSets sets and each power of two above it)
    nx::vector<VkDescriptorPool> free_pools[4];
    // Scenes submitted to the GPU whose results are not in guest memory yet, oldest first: what each writes there,
    // the surface whose image it draws, and what finishes it (the write-back, the notifications, the clean-up) once
    // the GPU is done. A scene is finished when a later one has been submitted (the GPU runs it while the CPU
    // prepares the next), when something reads what it writes, at the library's points of synchronisation, or by
    // the flusher thread as soon as the GPU is done with it.
    struct Pending {
        u64 number = 0;  // (in submission order, from 1)
        u64 index = 0;   // (Renderer::scenes when it was rendered: what CachedTexture::used counts)
        DepthTarget* kept_depth = nullptr;  // (the kept depth image it drew with, and whether it stores it)
        bool stores_kept_depth = false;
        DepthTarget* depth_sources[2] = {};  // (the kept images it copies its depth and stencil from)
        nx::vector<DepthTarget*> converted_from;  // (kept images its textures were made from)
        nx::unique_ptr<vk::OneShot> shot;
        nx::vector<std::pair<u32, u32>> writes;  // (address, bytes)
        Target* target = nullptr;
        nx::unique_function<void(bool ok)> finish;
    };
    nx::vector<Pending> pending;
    std::atomic<u32> pending_count{0};
    // Cached textures' images let go while a pending scene may still sample them (retire_image), with the scene
    // count (`scenes`) then: destroyed once no scene rendered by then is pending.
    nx::vector<std::pair<vk::Image, u64>> retired;
    // The scenes ended and not yet rendered, in order (end_scene; the renderer's thread renders them), numbered from
    // 1 as they end; `rendered` the last that thread is done with.
    struct Ended {
        Scene scene;
        nx::vector<Notification> notes;
        u64 number = 0;
    };
    nx::mutex scenes_lock;
    nx::cond_var scenes_changed;
    nx::vector<Ended> ended_scenes;
    // (a scene's, while it is rendered, kept from scene to scene: render_scene) The guest memory its vertex streams
    // read, each range at its offset in the scene's vertex buffer; its indices.
    struct VertexRange {
        u32 address = 0, bytes = 0;
        VkDeviceSize offset = 0;
        u32 repack = ~0u;  // (laid out again: scene_repacks')
    };
    nx::vector<VertexRange> scene_vertex_ranges;
    // A stream whose attributes are not where Vulkan can read them (an F32 attribute at an offset, or a stride, that
    // is no multiple of four: what Vulkan reads there is undefined, and drivers read other vertices' bytes), copied
    // vertex by vertex, each attribute at an aligned offset.
    struct Repack {
        u32 count = 0, from_stride = 0, to_stride = 0, fields = 0;
        struct Field {
            u32 from, to, bytes;
        } field[16];
    };
    nx::vector<Repack> scene_repacks;
    nx::vector<u8> scene_indices;
    u64 ended = 0, rendered = 0;
    // (scenes rendered ahead of one ended before them, by another of the game's threads: `rendered` counts up to
    // a scene once every scene before it is rendered too)
    nx::vector<u64> rendered_ahead;
    bool worker = false;
    nx::vector<nx::unique_ptr<vk::OneShot>> display_copies;  // (the frames copied to the sink's image, in flight)
    u64 submitted = 0, finished = 0;  // (the numbers of the last scene submitted and the last finished)
    bool flusher = false;
    nx::hash_map<u64, nx::string> failed_textures;
    u64 scenes = 0;
    // (deferred depth stores' samples, by the surface's depth address: the read-back buffer, or at a scale the
    // middle samples; given back once their write-back is no longer deferred. Under g_lazy_lock.)
    nx::hash_map<u32, Snapshot> lazy_depths;
#if PSVRECORE_TRACY
    tracy::VkCtx* gpu_zones = nullptr;  // (Tracy's GPU zones: the scenes' and the game's markers; null: none)
#endif
};

// The conversion of render targets' images into textures (target_texture.hpp), checked on the device against the
// texture decoder: unless PSVRECORE_TARGET_TEXTURES=0, or the device's results differ.
void make_target_converter(Renderer& r) {
    if (const char* v = std::getenv("PSVRECORE_TARGET_TEXTURES"); v && *v == '0') return;
    if (auto made = create_target_converter(*r.device, r.converter); !made) {
        nx::logw(logs::gpu, "{}: render targets read as textures are decoded on the CPU", made.error().message);
        return;
    }
    const TargetCheck check = check_target_conversion(*r.device, r.converter);
    if (!check.ok) {
        nx::logw(logs::gpu, "target textures: the device's conversion differs from the decoder's ({}): render targets read "
                 "as textures are decoded on the CPU", check.what);
        destroy_target_converter(*r.device, r.converter);
        return;
    }
    nx::logi(logs::gpu, "target textures: made on the GPU (the conversion checked in {:.0f} ms)", check.ms);
    // (HDR surfaces' textures made from their images directly, unless PSVRECORE_TARGET_TEXTURES=buffers)
    if (const char* v = std::getenv("PSVRECORE_TARGET_TEXTURES"); v && std::strcmp(v, "buffers") == 0) {
        destroy_target_image_converter(*r.device, r.converter);
        return;
    }
    const TargetCheck images = check_target_image_conversion(*r.device, r.converter);
    if (images.ran && !images.ok)
        nx::logw(logs::gpu, "target textures: the device's conversion from images differs from the decoder's ({}): HDR "
                 "surfaces' textures are made through buffers", images.what);
    else if (images.ran)
        nx::logi(logs::gpu, "target textures: HDR surfaces' made from their images (checked in {:.0f} ms)", images.ms);
}

// A write-back of the renderer's into guest memory (`ranges`, `write`): their watched pages opened at once (one call,
// not a fault for each page), and the watches of other surfaces sharing a page with them, not their bytes (and those
// in `exempt`, whose bytes it may write: a depth surface's background), watched again after it when nothing else
// had written there. (WipEout 2048's HDR colour surface ends on the page its depth surface starts on.)
struct GuestRange {
    u32 address = 0, size = 0;
};
// The watches of other surfaces sharing a page with `ranges`, not their bytes (or `exempt`), which nothing has
// written since they were watched.
nx::vector<WrittenWatch*> sharing_watches(Renderer& r, std::initializer_list<GuestRange> ranges, const WrittenWatch* exempt) {
    constexpr u64 kGranule = 1ull << 16;  // (as large as any host's page: pages shared are a subset)
    nx::vector<WrittenWatch*> keep;
    auto consider = [&](WrittenWatch& w) {
        if (!w.armed || !unwritten(w, w.address, w.size)) return;
        bool page = false, bytes = false;
        for (const GuestRange& g : ranges) {
            if (!g.size) continue;
            const u64 a0 = g.address, a1 = u64{g.address} + g.size, w0 = w.address, w1 = u64{w.address} + w.size;
            page |= a0 / kGranule <= (w1 - 1) / kGranule && w0 / kGranule <= (a1 - 1) / kGranule;
            bytes |= a0 < w1 && w0 < a1;
        }
        if (page && (!bytes || &w == exempt)) keep.push_back(&w);
    };
    for (auto& t : r.targets) consider(t->watch);
    for (auto& t : r.kept_depths) consider(t->watch[0]), consider(t->watch[1]), consider(t->background_watch);
    for (auto& t : r.depth_targets) consider(t->watch[0]);
    return keep;
}
template <typename F>
void write_back(Renderer& r, std::initializer_list<GuestRange> ranges, const WrittenWatch* exempt, F&& write) {
    if (!write_watch_mode()) {
        write();
        return;
    }
    const nx::vector<WrittenWatch*> keep = sharing_watches(r, ranges, exempt);
    for (const GuestRange& g : ranges)
        if (g.size) memory().note_write(g.address, g.size);
    write();
    for (WrittenWatch* w : keep) watch_range(*w, w->address, w->size);
}

// Lazy write-backs (PSVRECORE_LAZY_WRITEBACK=0: off): a colour surface's bytes, read back after its scene, are not
// written into guest memory then. Its pages are deferred (GuestMemory::defer: inaccessible) and the bytes kept (the
// surface's `written`, the read-back buffer itself); whatever touches those pages first (the game's code on the
// CPU, the renderer reading guest memory, the display, the OS through a file written from there) has them written
// then (lazy_materialize, from the fault). A surface drawn again before anything touched it is never written: a
// race's surfaces (1024 x 1024, 960 x 544 HDR, ...) that only the GPU reads, as textures made from their images.
// A depth surface's store likewise (without a stencil): its samples kept as read back (Renderer::lazy_depths),
// converted into the surface's format when written; WipEout 2048's 960 x 544 depth, stored each frame, was two fifths
// of the copying the thread that ends its scenes did, and nothing on the CPU reads it.
struct LazyWriteBack {
    Target* target = nullptr;
    u32 address = 0, span = 0;
    SurfaceLayout layout;
    const u8* bytes = nullptr;  // (the target's `written`; a depth store's samples)
    const u8* stencil = nullptr;  // (a depth store's stencil samples, after its depth ones: S8D24's, a mask's)
    bool depth = false;  // (a depth store's: the surface, its layout and size, how its samples are read)
    DepthStencilSurface ds{};
    DepthLayout dl;
    u32 width = 0, height = 0;
    bool unorm24 = false;
};
std::mutex g_lazy_lock;  // (taken from fault handlers: held only around the list and the writing, never around
                         // anything that touches deferred guest memory or takes GuestMemory's own lock)
nx::vector<LazyWriteBack> g_lazy;
std::atomic<u64> g_lazy_deferred{0}, g_lazy_written{0};  // (PSVRECORE_GXM_PROFILE)

bool lazy_write_backs() {
    static const bool on = [] {
        const char* v = std::getenv("PSVRECORE_LAZY_WRITEBACK");
        return !(v && *v == '0') && write_watch_mode() != 0;
    }();
    return on;
}

// The deferred write-backs with a page in [address, address + size) written, their pages watched again. The surface
// a fault is in (`address` the faulting byte) whole: what is read next is likely the rest (a fault a page would cost
// a surface hundreds); before a write the OS or the renderer makes there, a linear surface's bytes on those pages
// only, a tiled, swizzled or depth one's whole. Another surface with bytes on those pages is written there only and
// stays deferred for the rest (WipEout 2048's D16 shadow map ends on the page its 128 x 128 surface starts on, read
// each frame: written whole, the shadow map's 128 KiB were converted and written each frame for that page).
void write_lazy_depth(const LazyWriteBack& e, u64 w0, u64 w1) {
    // (the samples landing on the pages the materializer opened, converted into the surface's bytes there; the rest
    // of those pages as guest memory holds them)
    const u64 page = memory().host_page_size();
    const u64 lo = std::max<u64>(e.address, w0), hi = std::min<u64>(u64{e.address} + e.span, w1);
    if (lo >= hi) return;
    const u64 first = lo & ~(page - 1);
    nx::vector<u8> opened(static_cast<usize>((hi - first + page - 1) / page), 0);
    for (usize k = 0; k < opened.size(); ++k) opened[k] = memory().opened_deferred(static_cast<u32>(first + k * page));
    auto open_at = [&](u64 a) { return a >= lo && a < hi && opened[static_cast<usize>((a - first) / page)]; };
    nx::vector<u8> out(e.span);
    for (u64 at = lo; at < hi;) {
        const u64 to = std::min(hi, (at & ~(page - 1)) + page);
        if (open_at(at)) std::memcpy(out.data() + (at - e.address), memory().host(static_cast<u32>(at)), static_cast<usize>(to - at));
        at = to;
    }
    const u32 bytes = e.dl.depth_bytes;
    write_depth_surface(e.ds, e.dl, e.width, e.height, e.unorm24, reinterpret_cast<const u32*>(e.bytes), e.stencil, out.data(),
                        nullptr, [&](u32 i) { return open_at(u64{e.address} + u64{i} * bytes); });
    for (u64 at = lo; at < hi;) {
        const u64 to = std::min(hi, (at & ~(page - 1)) + page);
        if (open_at(at)) std::memcpy(memory().host(static_cast<u32>(at)), out.data() + (at - e.address), static_cast<usize>(to - at));
        at = to;
    }
}
void write_lazy_part(const LazyWriteBack& e, u64 w0, u64 w1) {
    if (e.depth) return write_lazy_depth(e, w0, w1);
    const SurfaceLayout& l = e.layout;
    const usize row = usize{l.width} * l.bytes;
    const u64 page = memory().host_page_size();
    for (u32 y = 0; y < l.height; ++y)
        for (u32 x = 0; x < l.width;) {
            const u32 n = l.run(x);
            const u64 a0 = u64{e.address} + l.offset(x, y), a1 = a0 + u64{n} * l.bytes;
            const u8* from = e.bytes + y * row + usize{x} * l.bytes;
            x += n;
            // (the run's bytes in the window, page by page: those the materializer opened)
            for (u64 at = std::max(a0, w0); at < std::min(a1, w1);) {
                const u64 to = std::min({a1, w1, (at & ~(page - 1)) + page});
                if (memory().opened_deferred(static_cast<u32>(at)))
                    std::memcpy(memory().host(static_cast<u32>(at)), from + (at - a0), static_cast<usize>(to - at));
                at = to;
            }
        }
}
void lazy_materialize(u32 address, u32 size, bool whole) {
    std::lock_guard<std::mutex> held(g_lazy_lock);
    if (g_lazy.empty()) return;
    const u64 page = memory().host_page_size();
    u64 w0 = u64{address} & ~(page - 1), w1 = (u64{address} + size + page - 1) & ~(page - 1);
    auto overlaps = [&](const LazyWriteBack& e) { return e.address < w1 && w0 < u64{e.address} + e.span; };
    // (widened to the surfaces the asked bytes are in: whole after a fault, and a tiled, swizzled or depth one's
    // whole always; the others with bytes on the window's pages written there only, and kept for their other pages)
    for (const LazyWriteBack& e : g_lazy) {
        const bool asked = e.address < u64{address} + size && address < u64{e.address} + e.span;
        if (!asked || (!whole && !e.depth && e.layout.kind == SurfaceLayout::kLinear)) continue;
        w0 = std::min(w0, u64{e.address} & ~(page - 1)), w1 = std::max(w1, (u64{e.address} + e.span + page - 1) & ~(page - 1));
    }
    bool any = false;
    for (const LazyWriteBack& e : g_lazy) any |= overlaps(e);
    if (!any) return;
    memory().open_deferred(static_cast<u32>(w0), static_cast<u32>(w1 - w0));
    for (const LazyWriteBack& e : g_lazy)
        if (overlaps(e)) write_lazy_part(e, w0, w1);
    memory().close_deferred(static_cast<u32>(w0), static_cast<u32>(w1 - w0));
    // (those written whole forgotten; a linear one written in part stays for its other pages)
    usize kept = 0;
    for (usize i = 0; i < g_lazy.size(); ++i) {
        const LazyWriteBack& e = g_lazy[i];
        const bool whole = overlaps(e) && w0 <= (u64{e.address} & ~(page - 1)) && u64{e.address} + e.span <= w1;
        if (whole) g_lazy_written.fetch_add(1, std::memory_order_relaxed);
        else g_lazy[kept++] = e;
    }
    g_lazy.resize(kept);
}
// (their memory unmapped, or mapped again: forgotten)
void lazy_discard(u32 address, u32 size) {
    std::lock_guard<std::mutex> held(g_lazy_lock);
    std::erase_if(g_lazy, [&](const LazyWriteBack& e) { return e.address < u64{address} + size && address < u64{e.address} + e.span; });
}
// A surface's deferred write-back, if any, written (before its `written` changes otherwise).
void settle_lazy(const Target& t) {
    if (lazy_write_backs() && t.layout.span()) memory().materialize(t.address, static_cast<u32>(t.layout.span()));

}
// A write-back deferred (its bytes registered before this): the watches of the surfaces sharing its pages kept, as
// write_back keeps them (and `exempt`). False: it could not be (some page not read/write): written as usual.
bool defer_write_back(Renderer& r, u32 address, u32 size, const WrittenWatch* exempt = nullptr) {
    const nx::vector<WrittenWatch*> keep = sharing_watches(r, {{address, size}}, exempt);
    if (!memory().defer(address, size)) return false;
    for (WrittenWatch* w : keep) watch_range(*w, w->address, w->size);
    g_lazy_deferred.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The YUV420 conversion's pipeline (yuv420_shader): unless PSVRECORE_YUV_CPU is set, or the device has none.
void make_yuv_pipeline(Renderer& r) {
    if (const char* v = std::getenv("PSVRECORE_YUV_CPU"); v && *v && *v != '0') return;
    const VkDevice dev = r.device->device();
    Renderer::YuvPipeline& y = r.yuv;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 1;
    si.pBindings = &binding;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Yuv420Constants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &y.set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    const nx::vector<u32> code = yuv420_shader();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4;
    mi.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 64;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    bool ok = vkCreateDescriptorSetLayout(dev, &si, nullptr, &y.set_layout) == VK_SUCCESS &&
              vkCreatePipelineLayout(dev, &li, nullptr, &y.layout) == VK_SUCCESS &&
              vkCreateShaderModule(dev, &mi, nullptr, &module) == VK_SUCCESS &&
              vkCreateDescriptorPool(dev, &pi, nullptr, &y.pool) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = y.layout;
        ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &y.pipeline) == VK_SUCCESS;
    }
    if (module) vkDestroyShaderModule(dev, module, nullptr);
    if (!ok) {
        nx::logw(logs::gpu, "gxm: no YUV conversion on the GPU: movies' frames converted on the CPU");
        if (y.pipeline) vkDestroyPipeline(dev, y.pipeline, nullptr);
        if (y.pool) vkDestroyDescriptorPool(dev, y.pool, nullptr);
        if (y.layout) vkDestroyPipelineLayout(dev, y.layout, nullptr);
        if (y.set_layout) vkDestroyDescriptorSetLayout(dev, y.set_layout, nullptr);
        y = {};
    }
}

Renderer& renderer() {
    // (never destroyed: the flusher thread may look at it until the process ends)
    static Renderer* r = new Renderer;
    return *r;
}

// (PSVRECORE_GXM_PROFILE: why the next scenes are finished)
u32 g_finish_why = 0;

// A guest thread's CPU given up (Kernel::Blocking) while the host works or waits on its behalf, and taken back at the
// end (which may wait for a CPU): once, nothing inside an outer one. (The guest thread's: read again after each of
// these, which may move it to another host thread.)
runtime::GuestLocal<bool> g_cpu_given_up{false};
struct CpuGivenUp {
    std::optional<runtime::Kernel::Blocking> blocking;
    CpuGivenUp() {
        if (!g_cpu_given_up.get() && runtime::Kernel::current()) {
            blocking.emplace(runtime::runtime().kernel());
            g_cpu_given_up.get() = true;
        }
    }
    void reset() {
        if (!blocking) return;
        g_cpu_given_up.get() = false;
        blocking.reset();
    }
    ~CpuGivenUp() noexcept(false) { reset(); }
    CpuGivenUp(const CpuGivenUp&) = delete;
    CpuGivenUp& operator=(const CpuGivenUp&) = delete;
};

// A guest thread's CPU lent while it renders (Kernel::lend_cpu): the console's GPU renders a scene, not one of the
// game's CPUs, which its other threads have meanwhile; the thread takes it back after, ahead of them. (Its waits
// inside give nothing up again.)
struct CpuLent {
    bool lent = false;
    CpuLent() {
        if (g_cpu_given_up.get() || !runtime::Kernel::current()) return;
        lent = runtime::runtime().kernel().lend_cpu();
        g_cpu_given_up.get() = lent;
    }
    ~CpuLent() noexcept(false) {
        if (!lent) return;
        g_cpu_given_up.get() = false;
        runtime::runtime().kernel().reclaim_cpu();
    }
    CpuLent(const CpuLent&) = delete;
    CpuLent& operator=(const CpuLent&) = delete;
};

// The renderer's lock, held: a guest thread waiting for it (another renders a scene, or writes one back: milliseconds)
// gives up its CPU meanwhile, for the game's other threads, instead of keeping it idle.
struct TargetsHeld {
    nx::mutex& m;
    explicit TargetsHeld(Renderer& r) : m(r.targets_lock) {
        // (a CPU is never waited for with the lock held: the game's other threads may keep every CPU, and the
        // renderer, wanting the lock, then could not go on)
        while (!m.try_lock()) {
            CpuGivenUp blocking;
            if (!blocking.blocking) {
                m.lock();  // (no CPU to give up: the renderer's thread, or one given up already)
                return;
            }
            m.lock();  // (until the one holding it is done)
            m.unlock();
        }  // (a CPU again, then the lock; a thread ending meanwhile unwinds from the CPU's wait, holding nothing)
    }
    ~TargetsHeld() { m.unlock(); }
    TargetsHeld(const TargetsHeld&) = delete;
    TargetsHeld& operator=(const TargetsHeld&) = delete;
};

// The pending scenes finished, oldest first: all of them but the newest `keep`, or only those the GPU is done with
// (`all` false). Under targets_lock.
void finish_pending(Renderer& r, bool all, usize keep = 0);

// A cached texture's image given up (evicted, or a reused one whose scene failed): destroyed now only if no pending
// scene can sample it, else once the scenes rendered up to now are finished. A scene's descriptors name the image's
// view until the GPU is done with it, and up to scenes_in_flight() - 1 scenes are still on the GPU when the next
// starts: destroyed earlier, the GPU read freed memory and descriptors (the validation layer's
// VUID-vkDestroyImage-image-01000).
void retire_image(Renderer& r, vk::Image& image) {
    if (!image.image) return;
    if (r.pending.empty()) {
        vk::destroy_image(*r.device, image);
        return;
    }
    r.retired.push_back({image, r.scenes});
    image = {};
}

// The retired images no pending scene can sample any more, destroyed.
void destroy_retired(Renderer& r) {
    if (r.retired.empty()) return;
    const u64 oldest = r.pending.empty() ? ~u64{0} : r.pending.front().index;
    std::erase_if(r.retired, [&](std::pair<vk::Image, u64>& x) {
        if (x.second >= oldest) return false;
        vk::destroy_image(*r.device, x.first);
        return true;
    });
}

// How many scenes the GPU may have in flight (PSVRECORE_GXM_SCENES_IN_FLIGHT, 1 to 64; 8): most of a frame's, the
// CPU preparing them while the GPU draws them one after another; a scene is waited for only when the GPU is that far
// behind. (WipEout 2048 has 11 scenes a frame; at 4 its render thread waited for the GPU in every other one on an
// Odin 3, 57.1 frames a second in a race; 57.6 at 8, as at 12 and 16.)
usize scenes_in_flight() {
    static const usize n = [] {
        const char* v = std::getenv("PSVRECORE_GXM_SCENES_IN_FLIGHT");
        return v && *v ? nx::clamp<usize>(std::strtoul(v, nullptr, 10), 1, 64) : usize{8};
    }();
    return n;
}

// Whether a pending scene writes any of [address, address + bytes) in guest memory.
bool pending_writes(const Renderer& r, u32 address, u32 bytes) {
    for (const Renderer::Pending& p : r.pending)
        for (const auto& [at, n] : p.writes)
            if (address < at + n && at < address + bytes) return true;
    return false;
}

bool pending_target(const Renderer& r, const Target* t) {
    for (const Renderer::Pending& p : r.pending)
        if (p.target == t) return true;
    return false;
}

// Before reading guest memory a pending scene writes: that scene (and those before it) finished.
void depend(u32 address, u32 bytes, u32 why = 1) {
    Renderer& r = renderer();
    if (!r.pending.empty() && pending_writes(r, address, bytes)) g_finish_why = why, finish_pending(r, true);
}

// How this device's shaders round to half precision: check_half_conversion's answer, kept in the cache folder for
// this device and driver (the check takes a fifth of a second on a phone), else checked now.
HalfConversion half_conversion_for(vk::Device& d, const nx::string& folder) {
    struct Kept {
        char magic[8];
        u8 uuid[VK_UUID_SIZE];
        u32 vendor, device, driver;
        u32 conversion;
    };
    const VkPhysicalDeviceProperties& p = d.properties();
    Kept want{};
    std::memcpy(want.magic, "PSVHALF1", 8);
    std::memcpy(want.uuid, p.pipelineCacheUUID, VK_UUID_SIZE);
    want.vendor = p.vendorID, want.device = p.deviceID, want.driver = p.driverVersion;
    const char* forced = std::getenv("PSVRECORE_HALF");
    const nx::string path = folder.empty() || forced ? nx::string() : folder + "/half.bin";
    if (!path.empty()) {
        if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
            Kept got{};
            const bool read = std::fread(&got, sizeof got, 1, in) == 1;
            std::fclose(in);
            want.conversion = got.conversion;
            if (read && std::memcmp(&got, &want, sizeof got) == 0 && got.conversion <= u32{2}) {
                const auto c = static_cast<HalfConversion>(got.conversion);
                nx::logi(logs::gpu, "half precision: {} (checked on an earlier run)", half_conversion_name(c));
                return c;
            }
        }
    }
    const HalfCheck c = check_half_conversion(d);
    if (!path.empty() && c.ran) {
        (void)nx::fs::create_directories(nx::fs::path_view(folder));
        want.conversion = static_cast<u32>(c.conversion);
        if (std::FILE* out = std::fopen(path.c_str(), "wb")) {
            std::fwrite(&want, sizeof want, 1, out);
            std::fclose(out);
        }
    }
    return c.conversion;
}

// ---- Debugging and profiling: the game's markers, Tracy's GPU zones, RenderDoc captures ---------------------------

// The game's user markers are recorded (sceGxmPushUserMarker ...) while something shows them: the device's debug
// labels, or Tracy.
std::atomic<bool> g_markers_on{profile::kEnabled};
void markers_on(bool labels) { g_markers_on.store(labels || profile::kEnabled, std::memory_order_relaxed); }

constexpr u32 kSceneRgb = 0x3070C0, kGameRgb = 0xC08030, kPassRgb = 0x60A060;

#if PSVRECORE_TRACY
// Tracy's GPU context on the device's queue: the scenes' commands timed, each scene a zone and the game's markers
// zones in it, on the CPU's timeline (calibrated where the device can read its clock with the host's).
void make_gpu_zones(Renderer& r) {
    vk::Device& d = *r.device;
    if (!d.caps().timestamp_period) return;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = d.queue_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(d.device(), &pi, nullptr, &pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d.device(), &ai, &cmd) == VK_SUCCESS) {
        // (the context submits to the queue and waits for it while it is made)
        nx::scoped_lock<nx::mutex> held(d.queue_lock());
        r.gpu_zones = TracyVkContextCalibrated(d.physical(), d.device(), d.queue(), cmd, d.caps().time_domains,
                                               d.caps().calibrated_timestamps);
    }
    vkDestroyCommandPool(d.device(), pool, nullptr);
    if (!r.gpu_zones) return;
    static constexpr char kName[] = "GXM scenes";
    TracyVkContextName(r.gpu_zones, kName, sizeof kName - 1);
    nx::logi(logs::gpu, "Tracy: GPU zones on{}", d.caps().calibrated_timestamps ? " (calibrated)" : "");
}
#endif

// The game's markers of a scene into its commands, as its draws are recorded: debug labels and, with Tracy, GPU
// zones. Balanced within the scene: a pop without its push is left out, a push left open ends with the scene's draws.
struct MarkerReplay {
    vk::Device& d;
    VkCommandBuffer cmd;
    const nx::vector<UserMarker>& markers;
    usize next = 0;
    u32 open = 0;
#if PSVRECORE_TRACY
    tracy::VkCtx* zones_ctx = nullptr;
    nx::vector<nx::unique_ptr<tracy::VkCtxScope>> zones;
#endif

    // The markers placed before draw `at` (~0u: all that are left).
    void upto(u32 at) {
        for (; next < markers.size() && markers[next].at <= at; ++next) {
            const UserMarker& m = markers[next];
            switch (m.kind) {
                case UserMarker::Kind::Push:
                    d.begin_label(cmd, m.tag.c_str(), kGameRgb);
                    ++open;
#if PSVRECORE_TRACY
                    if (zones_ctx) {
                        static constexpr char kFile[] = "guest", kFunction[] = "(game GPU marker)";
                        zones.push_back(nx::make_unique<tracy::VkCtxScope>(
                            zones_ctx, 0, kFile, sizeof kFile - 1, kFunction, sizeof kFunction - 1, m.tag.data(),
                            m.tag.size(), cmd, true));
                    }
#endif
                    break;
                case UserMarker::Kind::Pop:
                    if (!open) break;
                    d.end_label(cmd);
                    --open;
#if PSVRECORE_TRACY
                    if (!zones.empty()) zones.pop_back();
#endif
                    break;
                case UserMarker::Kind::Set: d.insert_label(cmd, m.tag.c_str(), kGameRgb); break;
            }
        }
    }
    void close() {
        upto(~0u);
        for (; open; --open) d.end_label(cmd);
#if PSVRECORE_TRACY
        while (!zones.empty()) zones.pop_back();  // (innermost first)
#endif
    }
};

// GPU captures (RenderDoc, when it is in the process: sceRazorGpuCaptureSetTrigger*, the frontend's key, its
// capture_frame setting): the scenes of one frame of the game's, from the first scene after the frame boundary that
// follows the request to the last before the next boundary. A frame boundary: a frame queued for the display
// (sceGxmDisplayQueueAddEntry) or set directly (sceDisplaySetFrameBuf outside the display queue's callback).
struct GpuCapture {
    nx::mutex lock;
    bool armed = false;    // (requested: from the next frame on)
    u64 from = 0, to = 0;  // (the frame's first scene and its last: 0 while not known yet)
    bool active = false;
    nx::string title;
    u64 frames = 0;  // (frame boundaries so far)
};
GpuCapture& gpu_capture() {
    static GpuCapture c;
    return c;
}

VkInstance capture_instance() {
    Renderer& r = renderer();
    return r.device ? r.device->instance() : VK_NULL_HANDLE;
}

void capture_end(GpuCapture& c) {
    vk::renderdoc::end(capture_instance());
    c.active = false;
    c.from = c.to = 0;
}

// Around each scene rendered, by its number.
void capture_before(u64 number) {
    GpuCapture& c = gpu_capture();
    nx::scoped_lock<nx::mutex> held(c.lock);
    if (!c.from || c.active || number < c.from) return;
    vk::renderdoc::start(capture_instance(), c.title);
    c.active = true;
}
void capture_after(u64 number) {
    GpuCapture& c = gpu_capture();
    nx::scoped_lock<nx::mutex> held(c.lock);
    if (c.active && c.to && number >= c.to) capture_end(c);
}

// Whether textures of gamma mode 1 are kept encoded and sampled through sRGB formats (read_texels' `srgb`; a gamma-mode
// colour surface read as such a texture copied from its image into an sRGB one), which decode them as they sample: the
// device's formats allow it, and not PSVRECORE_SRGB_TEXTURES=0 (decoded to half floats on the CPU, as before).
bool srgb_textures(const vk::Device& d) {
    static const bool enabled = [] {
        const char* v = std::getenv("PSVRECORE_SRGB_TEXTURES");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return enabled && d.caps().texture_srgb;
}

bool ensure(Renderer& r) {
    if (r.tried) return static_cast<bool>(r.device);
    r.tried = true;
    if (state().options->shared_device) {
        r.device = state().options->shared_device;
    } else {
        auto device = vk::Device::create(state().options->device);
        if (!device) {
            nx::loge(logs::gpu, "no Vulkan device: {}; scenes are not rendered", device.error().message);
            return false;
        }
        r.owned = std::move(*device);
        r.device = r.owned.get();
    }
    vk::Device& d = *r.device;
    markers_on(d.caps().labels);
#if PSVRECORE_TRACY
    make_gpu_zones(r);
#endif
    prefetch_start(d.caps().texture_bc, srgb_textures(d));
    if (!r.flusher) {
        // (the scenes the GPU is done with finished when nothing else has a while after: their notifications written
        // for a game that polls them. Not at once: the game's next scene or frame finishes them, and this, finishing
        // under the renderer's lock, would hold up that scene)
        r.flusher = true;
        nx::thread_options options;
        options.name = "gxm scenes";
        options.priority = nx::thread_priority::High;  // (it writes scenes back holding the renderer's lock: not left on a slow core meanwhile)
        nx::thread flusher(
            [&r] {
                runtime::host::frame_thread();
                constexpr u64 kAfter = 4'000'000;  // (ns)
                u64 seen = 0, since = 0;  // (the oldest scene pending found done, and when)
                for (;;) {
                    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (!r.pending_count.load(std::memory_order_acquire) || !r.targets_lock.try_lock()) continue;
                    if (!r.pending.empty() && r.pending.front().shot->done()) {
                        const u64 now = nx::os::ticks_ns();
                        if (r.pending.front().number != seen) {
                            seen = r.pending.front().number;
                            since = now;
                        } else if (now - since >= kAfter) {
                            g_finish_why = 3;
                            finish_pending(r, false);
                        }
                    }
                    r.targets_lock.unlock();
                }
            },
            options);
        flusher.detach();
    }
    r.passes = nx::make_unique<vk::RenderPasses>(d);
    r.pipelines = nx::make_unique<vk::Pipelines>(d, *r.passes);
    // (how the shaders round to half precision, before the cache of what they were translated to is opened)
    set_half_conversion(half_conversion_for(d, state().options->cache_folder));
    // (what an earlier run made, before anything is made now)
    r.cache.open(state().options->cache_folder);
    if (!r.cache.folder().empty()) {
        const nx::string path = r.cache.folder() + "/pipelines.vkcache";
        if (nx::fs::exists(nx::fs::path_view(path))) {
            if (auto loaded = r.pipelines->load_cache(nx::fs::path_view(path)); !loaded)
                nx::logi(logs::gpu, "{}", loaded.error().message);
        }
    }
    r.samplers = nx::make_unique<vk::Samplers>(d);
    VkDescriptorSetLayoutBinding bindings[kDrawBindings]{};
    for (u32 i = 0; i < kDrawBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i < kDrawBindingFragmentTextures ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : i == kDrawBindingColor           ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = i == kDrawBindingColor ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                        : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                                              VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = kDrawBindings;
    si.pBindings = bindings;
    vkCreateDescriptorSetLayout(d.device(), &si, nullptr, &r.set_layout);
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(DrawConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &r.set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    vkCreatePipelineLayout(d.device(), &li, nullptr, &r.layout);
    make_yuv_pipeline(r);
    make_target_converter(r);
    if (lazy_write_backs()) memory().set_deferral(lazy_materialize, lazy_discard);
    vk::ImageDesc wd;
    wd.width = wd.height = 1;
    if (auto img = vk::create_image(d, wd)) {
        r.white = *img;
        const u8 texel[4] = {255, 255, 255, 255};
        (void)vk::upload_image(d, r.white, texel);
    }
    vk::ImageDesc cd;
    cd.width = cd.height = 1;
    cd.layers = 6;
    cd.cube = true;
    if (auto img = vk::create_image(d, cd)) {
        r.white_cube = *img;
        const u8 texel[4] = {255, 255, 255, 255};
        for (u32 f = 0; f < 6; ++f) (void)vk::upload_image(d, r.white_cube, texel, f);
    }
    vk::SamplerDesc sd;
    if (auto s = r.samplers->get(sd)) r.sampler = *s;
    return true;
}

// Threads compiling pipelines at once: a phone's driver takes up to a tenth of a second for each whose shaders are
// new, on one core. PSVRECORE_PIPELINE_THREADS sets it (1: on the calling thread only).
u32 pipeline_threads() {
    static const u32 n = [] {
        if (const char* v = std::getenv("PSVRECORE_PIPELINE_THREADS"); v && std::atoi(v) > 0)
            return static_cast<u32>(std::atoi(v));
        return nx::clamp<u32>(nx::cpu::logical_core_count() / 2, 1, 4);
    }();
    return n;
}

// fn(0) to fn(count - 1), on up to pipeline_threads() threads (the caller's one of them), returned when all are done.
void in_parallel(usize count, auto&& fn) {
    std::atomic<usize> next{0};
    auto work = [&] {
        for (usize i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;) fn(i);
    };
    const u32 helpers = static_cast<u32>(nx::min<usize>(pipeline_threads(), count)) - (count ? 1 : 0);
    nx::vector<nx::thread> threads;
    threads.reserve(helpers);
    nx::thread_options options;
    options.name = "pipelines";
    for (u32 t = 0; t < helpers; ++t) threads.emplace_back(work, options);
    work();
    for (nx::thread& t : threads) t.join();
}

// A program's shader module for a variant: made from the cache's SPIR-V, else translated (and kept in the cache).
// `translate` empty: from the cache only (warm_up).
VkShaderModule module_for(Renderer& r, const ShaderKey& full, auto&& translate) {
    const u64 key = short_key(full);
    if (auto it = r.modules.find(key); it != r.modules.end()) return it->second;
    if (r.failed_programs.contains(key)) return VK_NULL_HANDLE;
    nx::vector<u32> words;
    const u64 t0 = nx::os::ticks_ns();
    bool translated = false;
    if (const nx::vector<u32>* cached = r.cache.spirv(full)) {
        words = *cached;
    } else {
        translated = true;
        auto spirv = translate();
        if (!spirv) {
            nx::logw(logs::gxm, "program not translated: {}", spirv.error().message);
            r.failed_programs[key] = spirv.error().message;
            return VK_NULL_HANDLE;
        }
        words = std::move(*spirv);
        r.cache.add_spirv(full, words);
        if (!r.warming) r.translated.fetch_add(1);
    }
    const u64 t1 = nx::os::ticks_ns();
    auto m = vk::create_shader(*r.device, words);
    if (!m) {
        r.failed_programs[key] = m.error().message;
        return VK_NULL_HANDLE;
    }
    // (PSVRECORE_GXM_PROFILE: a shader that took a while to make)
    if (const u64 t2 = nx::os::ticks_ns(); t2 - t0 >= 5'000'000 && std::getenv("PSVRECORE_GXM_PROFILE"))
        nx::logi(logs::gpu, "gxm profile: a slow shader, {:.1f} ms: {} {:.1f}, the module {:.1f}; {} words of SPIR-V",
                 static_cast<f64>(t2 - t0) / 1e6, translated ? "translated" : "from the cache", static_cast<f64>(t1 - t0) / 1e6,
                 static_cast<f64>(t2 - t1) / 1e6, words.size());
    r.modules[key] = *m;
    r.module_keys[reinterpret_cast<u64>(*m)] = full;
    return *m;
}

// A graphics pipeline to make (make_graphics).
struct GraphicsJob {
    vk::GraphicsPipelineDesc desc;
    VkPipeline pipeline = VK_NULL_HANDLE;
    nx::string error;
    u64 ns = 0;  // (its own time)
};

// The jobs' pipelines, on several threads. First one for each vertex shader, those with the most SPIR-V first, then
// the rest: a driver compiles a shader the first time a pipeline has it, and the others with it take a fraction of
// that (a phone's: 83 ms for a new vertex shader on average, 17 ms for a new fragment shader with one made before).
// (a phone's driver took two fifths longer for each pipeline with four at once, its threads on the fast cores or
// not: a scene's new pipelines take their sum over about three)
void pipeline_whole(const vk::GraphicsPipelineDesc& desc);
nx::vector<u32> unpack_words(std::span<const u8> bytes);

void make_graphics(Renderer& r, std::span<GraphicsJob> jobs) {
    auto words = [&](VkShaderModule m) -> usize {
        const auto it = r.module_keys.find(reinterpret_cast<u64>(m));
        if (it == r.module_keys.end()) return 0;
        const nx::vector<u32>* v = r.cache.spirv(it->second);
        return v ? v->size() : 0;
    };
    nx::vector<usize> first, rest;
    for (usize i = 0; i < jobs.size(); ++i) {
        bool seen = false;
        for (const usize f : first) seen |= jobs[f].desc.vertex == jobs[i].desc.vertex;
        (seen ? rest : first).push_back(i);
    }
    std::stable_sort(first.begin(), first.end(),
                     [&](usize a, usize b) { return words(jobs[a].desc.vertex) > words(jobs[b].desc.vertex); });
    // (with pipeline libraries, a scene's new ones linked: quick when their shaders were compiled before; the whole
    // pipeline then made on the threads ahead, in its place, warm_up's own made whole)
    const bool link = r.device->caps().pipeline_library && !r.warming;
    auto make = [&](usize i) {
        const u64 t0 = nx::os::ticks_ns();
        if (auto p = link ? r.pipelines->linked(jobs[i].desc) : r.pipelines->graphics(jobs[i].desc)) jobs[i].pipeline = *p;
        else jobs[i].error = p.error().message;
        jobs[i].ns = nx::os::ticks_ns() - t0;
    };
    in_parallel(first.size(), [&](usize k) { make(first[k]); });
    in_parallel(rest.size(), [&](usize k) { make(rest[k]); });
    if (link)
        for (const GraphicsJob& j : jobs)
            if (j.pipeline) pipeline_whole(j.desc);
}

// ---- Pipelines made asynchronously -----------------------------------------------------------------------------
// Without pipeline libraries (an Adreno 830's driver: 43-390 ms for a pair of shaders not made before, whatever its
// state 0.2-0.6 ms once it was; a song's first play there stalled up to 0.6 s for its dancers' pipelines), a scene's
// new pipelines are made on threads of their own: the scene waits for them a few milliseconds (kAsyncWaitNs: those
// in the driver's cache), and is drawn without the draws of the others, which the scenes after draw once they are
// made. PSVRECORE_ASYNC_PIPELINES=0 waits for them all.
struct AsyncPipelines {
    nx::mutex lock;
    nx::semaphore work, made;
    nx::vector<GraphicsJob> queue, done;                   // (to make; made, for a scene to record)
    nx::vector<vk::GraphicsPipelineDesc> pending, failed;  // (queued or being made; failed: not tried again)
    bool started = false;
};
AsyncPipelines& async_pipelines() {
    static AsyncPipelines* a = new AsyncPipelines;  // (its threads run to the process's end)
    return *a;
}
constexpr u64 kAsyncWaitNs = 6'000'000;

bool async_on(const Renderer& r) {
    static const bool off = [] {
        const char* v = std::getenv("PSVRECORE_ASYNC_PIPELINES");
        return v && std::atoi(v) == 0;
    }();
    return !off && !r.device->caps().pipeline_library && !r.warming;
}

bool listed(const nx::vector<vk::GraphicsPipelineDesc>& list, const vk::GraphicsPipelineDesc& d) {
    for (const vk::GraphicsPipelineDesc& e : list)
        if (e.same(d)) return true;
    return false;
}

void async_run(Renderer& r) {
    AsyncPipelines& a = async_pipelines();
    for (;;) {
        a.work.acquire();
        GraphicsJob j;
        {
            const nx::scoped_lock<nx::mutex> held(a.lock);
            if (a.queue.empty()) continue;
            j = std::move(a.queue.front());
            a.queue.erase(a.queue.begin());
        }
        const u64 t0 = nx::os::ticks_ns();
        if (auto p = r.pipelines->graphics(j.desc)) j.pipeline = *p;
        else j.error = p.error().message;
        j.ns = nx::os::ticks_ns() - t0;
        {
            const nx::scoped_lock<nx::mutex> held(a.lock);
            for (usize i = 0; i < a.pending.size(); ++i)
                if (a.pending[i].same(j.desc)) {
                    a.pending.erase(a.pending.begin() + static_cast<isize>(i));
                    break;
                }
            if (!j.pipeline) a.failed.push_back(j.desc);
            a.done.push_back(std::move(j));
        }
        a.made.release();
    }
}

// The jobs queued (those neither queued nor failed before), then waited for until made or kAsyncWaitNs has passed:
// those made have their pipelines.
void make_async(Renderer& r, std::span<GraphicsJob> jobs) {
    AsyncPipelines& a = async_pipelines();
    {
        const nx::scoped_lock<nx::mutex> held(a.lock);
        if (!a.started) {
            a.started = true;
            for (u32 t = 0; t < pipeline_threads(); ++t) {
                nx::thread_options options;
                options.name = "pipelines";
                nx::thread worker([&r] { async_run(r); }, options);
                worker.detach();
            }
        }
        u32 queued = 0;
        for (const GraphicsJob& j : jobs) {
            if (listed(a.pending, j.desc) || listed(a.failed, j.desc)) continue;
            a.pending.push_back(j.desc);
            a.queue.push_back({j.desc});
            ++queued;
        }
        if (queued) a.work.release(queued);
    }
    const u64 until = nx::os::ticks_ns() + kAsyncWaitNs;
    for (;;) {
        bool all = true;
        for (GraphicsJob& j : jobs) {
            if (!j.pipeline) j.pipeline = r.pipelines->find(j.desc);
            all &= j.pipeline != VK_NULL_HANDLE;
        }
        const u64 now = nx::os::ticks_ns();
        if (all || now >= until) break;
        (void)a.made.try_acquire_for(std::chrono::nanoseconds(until - now));
    }
}

// The pipelines made asynchronously since the last call.
nx::vector<GraphicsJob> async_made() {
    AsyncPipelines& a = async_pipelines();
    const nx::scoped_lock<nx::mutex> held(a.lock);
    nx::vector<GraphicsJob> out = std::move(a.done);
    a.done.clear();
    return out;
}

// The variants' keys: the program's bytes, and what else its translation reads.
ShaderKey secondary_key(const Program& p) {
    const u8 kind = 's';
    return shader_key({p.content, std::span<const u8>(&kind, 1)});
}

ShaderKey vertex_key(const Program& p, const VertexShader& vs) {
    nx::vector<u8> v{'v'};
    auto put = [&](u32 x) { v.insert(v.end(), reinterpret_cast<const u8*>(&x), reinterpret_cast<const u8*>(&x) + 4); };
    for (const VertexInput& in : vs.inputs) {
        put(in.location), put(in.reg);
        v.push_back(in.format), v.push_back(in.components), v.push_back(in.raw ? 1 : 0);
    }
    return shader_key({p.content, v});
}

ShaderKey fragment_key(const Program& p, const FragmentShader& fs, bool inline_secondary) {
    nx::vector<u8> v{'f'};
    v.insert(v.end(), reinterpret_cast<const u8*>(&fs.output_format), reinterpret_cast<const u8*>(&fs.output_format) + 4);
    v.insert(v.end(), fs.swizzle, fs.swizzle + 4);
    v.push_back(fs.color_output), v.push_back(fs.fragment_program_enabled), v.push_back(inline_secondary);
    v.push_back(fs.blend_read), v.push_back(fs.gamma);
    return shader_key({p.content, v});
}

// A compute pipeline for a secondary program's module.
VkPipeline make_compute(Renderer& r, VkShaderModule m) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = r.layout;
    if (vkCreateComputePipelines(r.device->device(), r.pipelines->cache(), 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
        pipeline = VK_NULL_HANDLE;
    return pipeline;
}

// The compute pipeline of a secondary program (VK_NULL_HANDLE: none to run, or not translated).
VkPipeline secondary_pipeline_keyed(Renderer& r, const Program& p);
// (a quick key: the program's content, as secondary_key takes it)
u64 secondary_quick(const Program& p) {
    return nx::fnv1a64().combine_bytes(reinterpret_cast<const char*>(p.content.data()), p.content.size()).value();
}
VkPipeline secondary_pipeline(Renderer& r, const Program& p) {
    if (p.secondary.empty()) return VK_NULL_HANDLE;
    const u64 quick = secondary_quick(p);
    if (auto it = r.quick_compute.find(quick); it != r.quick_compute.end()) return it->second;
    const VkPipeline pipeline = secondary_pipeline_keyed(r, p);
    r.quick_compute[quick] = pipeline;
    return pipeline;
}
VkPipeline secondary_pipeline_keyed(Renderer& r, const Program& p) {
    const ShaderKey full = secondary_key(p);
    const u64 key = short_key(full);
    if (auto it = r.compute.find(key); it != r.compute.end()) return it->second;
    const u64 t0 = nx::os::ticks_ns();
    const VkShaderModule m = module_for(r, full, [&] { return translate_secondary(p.gxp, p.secondary); });
    const u64 t1 = nx::os::ticks_ns();
    const VkPipeline pipeline = m ? make_compute(r, m) : VK_NULL_HANDLE;
    // (PSVRECORE_GXM_PROFILE: a secondary program's pipeline that took a while to make)
    if (const u64 t2 = nx::os::ticks_ns(); t2 - t0 >= 5'000'000 && std::getenv("PSVRECORE_GXM_PROFILE"))
        nx::logi(logs::gpu, "gxm profile: a slow compute pipeline, {:.1f} ms: the module {:.1f}, the pipeline {:.1f}",
                 static_cast<f64>(t2 - t0) / 1e6, static_cast<f64>(t1 - t0) / 1e6, static_cast<f64>(t2 - t1) / 1e6);
    if (pipeline) {
        r.cache.add_compute(full);
        if (!r.warming) r.made_pipelines.fetch_add(1);
        r.pipelines_changed.store(true);
    }
    r.compute[key] = pipeline;
    return pipeline;
}

Result<vk::Image> target_image(Renderer& r, VkFormat format, u32 width, u32 height, bool srgb_view) {
    vk::ImageDesc d;
    d.format = format;
    d.srgb_view = srgb_view;
    d.width = width;
    d.height = height;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return vk::create_image(*r.device, d);
}

Target* color_target(Renderer& r, u32 address, u32 format, VkFormat vk_format, const SurfaceLayout& layout, bool srgb_view) {
    const u32 width = layout.width, height = layout.height;
    for (auto& t : r.targets) {
        if (t->address != address || t->format != format || t->width != width || t->height != height) continue;
        if (t->layout != layout) settle_lazy(*t), t->layout = layout, t->written.clear();  // (the same memory, laid out otherwise)
        return t.get();
    }
    auto img = target_image(r, vk_format, width, height, srgb_view);
    if (!img) return nullptr;
    auto t = nx::make_unique<Target>();
    t->address = address, t->format = format, t->width = width, t->height = height;
    t->layout = layout;
    t->image = *img;
    r.targets.push_back(std::move(t));
    return r.targets.back().get();
}

// The internal resolution a surface of this format is drawn at: the options' (1 to 4), or 1 where the device cannot
// blit it with filtering (to scale it up and down) or draw into it.
u32 surface_scale(Renderer& r, VkFormat format) {
    const u32 wanted = nx::clamp<u32>(state().options->render_scale, 1, 4);
    if (wanted == 1) return 1;
    auto it = r.scalable.find(static_cast<u32>(format));
    if (it == r.scalable.end()) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(r.device->physical(), format, &fp);
        constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        const bool ok = (fp.optimalTilingFeatures & kNeeded) == kNeeded;
        if (!ok)
            nx::logw(logs::gpu, "surfaces of format {} are drawn at their own size: the device cannot scale them",
                     static_cast<u32>(format));
        it = r.scalable.emplace(static_cast<u32>(format), ok).first;
    }
    return it->second ? wanted : 1;
}

// The target's scaled image, at `scale` (made or made again; what it held is forgotten).
bool ensure_scaled(Renderer& r, Target& t, u32 scale) {
    if (t.scaled.image && t.scale == scale) return true;
    if (t.scaled.image) {
        r.passes->forget(t.scaled.view);
        if (t.scaled.srgb_view) r.passes->forget(t.scaled.srgb_view);
        vk::destroy_image(*r.device, t.scaled);
        t.scaled = {};
    }
    settle_lazy(t);
    t.written.clear();
    auto img = target_image(r, t.image.desc.format, t.width * scale, t.height * scale, t.image.srgb_view != VK_NULL_HANDLE);
    if (!img) return false;
    t.scaled = *img;
    t.scale = scale;
    return true;
}

// The parts of a surface (in guest memory, laid out as the target's) that differ from what was last written back:
// what the CPU drew over it since (text, a cleared area), or all of it when nothing was written back.
constexpr u32 kTile = 32;
struct Changes {
    nx::vector<VkRect2D> tiles;  // kTile x kTile tiles (cut at the edges) with a changed pixel
    usize pixels = 0;            // changed pixels
    bool all = false;
};
Changes compare_surface(const Target& t);
Changes surface_changes(const Target& t) {
    // (nothing written there since the write-back: its pages' write counts)
    if (!t.written.empty() && unwritten(t.watch, t.address, static_cast<u32>(t.layout.span()))) {
        if (write_watch_mode() != 2) return Changes{};
        Changes c = compare_surface(t);
        if (c.all || c.pixels) unseen_write("a colour surface", t.address, static_cast<u32>(t.layout.span()));
        return c;
    }
    return compare_surface(t);
}
Changes compare_surface(const Target& t) {
    Changes c;
    const SurfaceLayout& l = t.layout;
    const u32 bytes = l.bytes;
    if (t.written.empty()) {
        c.all = true;
        c.pixels = usize{t.width} * t.height;
        for (u32 y = 0; y < t.height; y += kTile)
            for (u32 x = 0; x < t.width; x += kTile)
                c.tiles.push_back({{static_cast<i32>(x), static_cast<i32>(y)}, {nx::min(kTile, t.width - x), nx::min(kTile, t.height - y)}});
        return c;
    }
    const usize row = usize{t.width} * bytes;
    const u32 tiles_x = (t.width + kTile - 1) / kTile;
    nx::vector<u8> dirty(usize{tiles_x} * ((t.height + kTile - 1) / kTile), 0);
    for (u32 y = 0; y < t.height; ++y) {
        const u8* had = t.written.data() + y * row;
        for (u32 x = 0; x < t.width;) {
            // (a run of pixels next to each other in both: compared at once, then pixel by pixel if it differs)
            const u32 n = l.run(x);
            const u8* guest = memory().host(t.address + static_cast<u32>(l.offset(x, y)));
            if (std::memcmp(guest, had + usize{x} * bytes, usize{n} * bytes) != 0) {
                for (u32 i = 0; i < n; ++i)
                    if (std::memcmp(guest + usize{i} * bytes, had + usize{x + i} * bytes, bytes) != 0) {
                        ++c.pixels;
                        dirty[usize{y / kTile} * tiles_x + (x + i) / kTile] = 1;
                    }
            }
            x += n;
        }
    }
    for (usize i = 0; i < dirty.size(); ++i) {
        if (!dirty[i]) continue;
        const u32 x = static_cast<u32>(i % tiles_x) * kTile, y = static_cast<u32>(i / tiles_x) * kTile;
        c.tiles.push_back({{static_cast<i32>(x), static_cast<i32>(y)}, {nx::min(kTile, t.width - x), nx::min(kTile, t.height - y)}});
    }
    return c;
}

// A whole-image blit between two colour images (in TRANSFER_SRC and TRANSFER_DST).
void blit(VkCommandBuffer cmd, const vk::Image& from, const vk::Image& to, VkFilter filter) {
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {static_cast<i32>(from.desc.width), static_cast<i32>(from.desc.height), 1};
    b.dstOffsets[1] = {static_cast<i32>(to.desc.width), static_cast<i32>(to.desc.height), 1};
    vkCmdBlitImage(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   filter);
}

VkSamplerAddressMode address_mode(u32 m) {
    switch (m) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 0:
        case 4: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // (clamp; mirror clamp needs an extension)
    }
}

Result<vk::Buffer> take_host_buffer(Renderer& r, VkDeviceSize bytes);
void give_host_buffer(Renderer& r, vk::Buffer& b);

// Decoded texels into a staging buffer, to be uploaded into the texture `key` by the scene's commands (each face's
// levels).
Result<void> queue_texels(Renderer& r, u64 key, const Texels& t) {
    vk::Device& d = *r.device;
    auto staging = take_host_buffer(r, t.data.size());
    if (!staging) return nx::err(staging.error());
    std::memcpy(staging->mapped, t.data.data(), t.data.size());
    vk::flush(d, *staging, t.data.size());
    Renderer::TextureUpload u{key, *staging, {}};
    VkDeviceSize at = 0;
    for (u32 face = 0; face < t.faces; ++face) {
        for (u32 l = 0; l < t.levels; ++l) {
            const u32 w = nx::max(1u, t.width >> l), h = nx::max(1u, t.height >> l);
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1};
            c.imageExtent = {w, h, 1};
            u.regions.push_back(c);
            at += t.bc ? VkDeviceSize{(w + 3) / 4} * ((h + 3) / 4) * (t.bc == 1 ? 8 : 16) : VkDeviceSize{w} * h * t.bytes;
        }
    }
    r.texture_uploads.push_back(std::move(u));
    return {};
}

// A YUV420 texture's planes into a staging buffer, to be converted into the texture `key` by the scene's commands
// (false: no set for it now: read_texels then).
Result<bool> queue_yuv420(Renderer& r, u64 key, const Yuv420& y) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = r.yuv.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r.yuv.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(r.device->device(), &ai, &set) != VK_SUCCESS) return false;
    const usize bytes = yuv420_buffer_bytes(y);
    auto staging = take_host_buffer(r, bytes);
    if (!staging) {
        vkFreeDescriptorSets(r.device->device(), r.yuv.pool, 1, &set);
        return nx::err(staging.error());
    }
    Renderer::TextureUpload u{key, *staging, {}};
    u.yuv = yuv420_buffer(y, static_cast<u8*>(staging->mapped));
    vk::flush(*r.device, *staging);
    u.yuv_set = set;
    VkDescriptorBufferInfo bi{staging->buffer, 0, bytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(r.device->device(), 1, &w, 0, nullptr);
    VkBufferImageCopy c{};
    c.bufferOffset = VkDeviceSize{u.yuv.out} * 4;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {y.width, y.height, 1};
    u.regions.push_back(c);
    r.texture_uploads.push_back(std::move(u));
    return true;
}

// An upload's buffer (and set) given back.
void give_upload(Renderer& r, Renderer::TextureUpload& u) {
    give_host_buffer(r, u.staging);
    if (u.yuv_set) vkFreeDescriptorSets(r.device->device(), r.yuv.pool, 1, &u.yuv_set);
    u.yuv_set = VK_NULL_HANDLE;
}

// Uploads decoded texels and waits: each face's levels.
[[maybe_unused]] Result<void> upload_texels(vk::Device& d, vk::Image& image, const Texels& t) {
    auto staging = vk::create_host_buffer(d, t.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging) return nx::err(staging.error());
    std::memcpy(staging->mapped, t.data.data(), t.data.size());
    vk::flush(d, *staging);
    nx::vector<VkBufferImageCopy> regions;
    VkDeviceSize at = 0;
    const u32 bytes = t.bytes;
    for (u32 face = 0; face < t.faces; ++face) {
        for (u32 l = 0; l < t.levels; ++l) {
            const u32 w = nx::max(1u, t.width >> l), h = nx::max(1u, t.height >> l);
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1};
            c.imageExtent = {w, h, 1};
            regions.push_back(c);
            at += VkDeviceSize{w} * h * bytes;
        }
    }
    vk::OneShot shot(d);
    auto cmd = shot.begin();
    Result<void> result;
    if (!cmd) {
        result = nx::err(cmd.error());
    } else {
        vk::transition(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(*cmd, staging->buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(regions.size()), regions.data());
        vk::transition(*cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        result = shot.submit_and_wait();
    }
    vk::destroy_buffer(d, *staging);
    return result;
}

// The sampler a texture's control words ask for, for an image of this format and levels.
VkSampler texture_sampler(Renderer& r, const TextureInfo& info, VkFormat format, u32 levels) {
    vk::SamplerDesc sd;
    // (32-bit float images are not filterable everywhere: nearest there)
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r.device->physical(), format, &fp);
    const bool filterable = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    sd.mag = info.mag_linear && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.min = info.min_linear && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mip = info.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sd.u = address_mode(info.u_address);
    sd.v = address_mode(info.v_address);
    sd.w = sd.u;
    sd.lod_bias = (static_cast<f32>(info.lod_bias) - 31.0f) / 8.0f;  // (assumed: eighths of a level, 31 none)
    sd.max_lod = static_cast<f32>(levels - 1);
    // (the texture's minimum LOD, sceGxmTextureSetLodMin: the levels above it are never sampled. Killzone: Mercenary
    // sets 1 on its 512x512 BC1/BC3 chains, whose level 0 holds stale memory, not texels)
    sd.min_lod = static_cast<f32>(nx::min(info.lod_min, levels - 1));
    auto smp = r.samplers->get(sd);
    return smp ? *smp : VK_NULL_HANDLE;
}

// Whether a texture's texels are what sampling an image of this format gives for the same bytes in memory (the
// texture decoder, on one texel of known bytes): 8-bit RGBA images, and half-float RGBA images whose texels the decoder
// gives as the bytes are (an F16x4 surface read as an F16x4 texture). With `srgb` (srgb_textures), an 8-bit texture of
// gamma mode 1 too, whose texels are the image's bytes still encoded (Texels::srgb): sampled through the sRGB format
// of the image's (texture_format), which decodes them.
bool texels_as_image(const TextureInfo& info, VkFormat format, bool srgb) {
    if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        if (texel_bits(info.base_format) != 64 || info.gamma || info.palette) return false;
        TextureInfo one = info;
        one.width = one.height = 1;
        one.levels = 1;
        one.stride = 8;
        one.type = texture_type::kLinear;
        const u8 probe[8] = {0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44};  // (halves 1, 2, 3, 4)
        auto texels = read_texels(one, [&](u32, void* out, u32 bytes) {
            if (bytes > 8) return false;
            std::memcpy(out, probe, bytes);
            return true;
        });
        return texels && texels->bytes == 8 && !texels->single && texels->data.size() >= 8 &&
               std::memcmp(texels->data.data(), probe, 8) == 0;
    }
    u32 order[4];
    if (format == VK_FORMAT_R8G8B8A8_UNORM) order[0] = 0, order[1] = 1, order[2] = 2, order[3] = 3;
    else if (format == VK_FORMAT_B8G8R8A8_UNORM) order[0] = 2, order[1] = 1, order[2] = 0, order[3] = 3;
    else return false;
    if (texel_bits(info.base_format) != 32 || info.palette) return false;
    if (info.gamma && (!srgb || vk::srgb_format_of(format) == VK_FORMAT_UNDEFINED)) return false;
    TextureInfo one = info;
    one.width = one.height = 1;
    one.levels = 1;
    one.stride = 4;
    one.type = texture_type::kLinear;  // (the order of the bytes, whatever the layout: a tiled texel would read a tile)
    const u8 probe[4] = {0x11, 0x55, 0x99, 0xDD};
    auto texels = read_texels(one, [&](u32, void* out, u32 bytes) {
        if (bytes > 4) return false;
        std::memcpy(out, probe, bytes);
        return true;
    }, false, false, srgb);
    if (!texels || texels->bytes != 4 || texels->data.size() < 4 || texels->srgb != (info.gamma != 0)) return false;
    for (u32 k = 0; k < 4; ++k)
        if (texels->data[k] != probe[order[k]]) return false;
    return true;
}

// The format a texture that texels_as_image takes from an image of this format is sampled as: the image's, or its
// sRGB format for a texture with a gamma mode.
VkFormat texture_format(const TextureInfo& info, VkFormat format) { return info.gamma ? vk::srgb_format_of(format) : format; }

// Whether a texture reads its texels where a colour surface of this layout has its pixels (texels as large as the
// surface's pixels: texels_as_image checks the texture's size of a texel against the image's).
bool texture_reads_layout(const TextureInfo& info, const SurfaceLayout& l) {
    if (texel_bits(info.base_format) != l.bytes * 8) return false;
    switch (info.type) {
        case texture_type::kLinear:
        case texture_type::kLinearStrided: return l.kind == SurfaceLayout::kLinear && info.stride == l.stride * l.bytes;
        case texture_type::kTiled: return l.kind == SurfaceLayout::kTiled && (l.stride + 31) / 32 == (info.width + 31) / 32;
        case texture_type::kSwizzled:
        case texture_type::kSwizzledArbitrary: return l.kind == SurfaceLayout::kSwizzled;
        default: return false;
    }
}

// A texture that reads a colour surface drawn at the internal resolution: its scaled image, when the texture is
// that surface (its address, size and layout, one level, texels as the image holds them), guest memory still holds
// what its last scene wrote back, and it is not what this scene draws into.
Target* scaled_texture(Renderer& r, const TextureInfo& info, const Target* drawing) {
    if (info.levels != 1 || !info.address) return nullptr;
    for (auto& t : r.targets) {
        if (t->scale <= 1 || t.get() == drawing || t->address != info.address || t->width != info.width ||
            t->height != info.height)
            continue;
        // (a texture with a gamma mode: read through the scaled image's sRGB view, made with a gamma-mode surface's)
        const bool srgb = srgb_textures(*r.device) && t->scaled.srgb_view != VK_NULL_HANDLE;
        if (!texture_reads_layout(info, t->layout) || !texels_as_image(info, t->image.desc.format, srgb)) return nullptr;
        const Changes changes = surface_changes(*t);
        return changes.all || changes.pixels ? nullptr : t.get();
    }
    return nullptr;
}

// The depth surface's bytes in guest memory (the depth words its samples span).
std::span<const u8> depth_bytes(const DepthStencilSurface& s, u32 width, u32 height) {
    return {memory().host(s.depth_data), usize{depth_samples(s, width, height)} * 4};
}

// The kept depth image of a depth surface a depth-only scene stores, at `scale` (made, or made again at another
// scale), or none: the device cannot sample its depth format, or the surface is not 4 bytes of depth a sample
// without stencil (DF32, DF32M).
DepthTarget* depth_target_for(Renderer& r, const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height,
                              u32 scale) {
    if (l.depth_bytes != 4 || l.stencil || l.packed) return nullptr;
    const VkFormat format = r.device->caps().depth_stencil_format;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r.device->physical(), format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return nullptr;
    const u32 control = s.control & (0x03FFF000u | kDsTiled), stride = s.background_control >> 16;
    DepthTarget* t = nullptr;
    for (auto& d : r.depth_targets)
        if (d->address == s.depth_data && d->control == control && d->stride == stride && d->width == width &&
            d->height == height)
            t = d.get();
    if (t && t->image.image && t->scale == scale) return t;
    if (!t) {
        r.depth_targets.push_back(nx::make_unique<DepthTarget>());
        t = r.depth_targets.back().get();
        t->address = s.depth_data, t->control = control, t->stride = stride, t->width = width, t->height = height;
    }
    if (t->image.image) {
        vkDestroyImageView(r.device->device(), t->depth_view, nullptr);
        r.passes->forget(t->image.view);
        vk::destroy_image(*r.device, t->image);
        t->image = {}, t->depth_view = VK_NULL_HANDLE;
    }
    t->written.clear();
    vk::ImageDesc dd;
    dd.format = format;
    dd.width = width * scale;
    dd.height = height * scale;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    auto img = vk::create_image(*r.device, dd);
    if (!img) return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(r.device->device(), &vi, nullptr, &t->depth_view) != VK_SUCCESS) {
        vk::destroy_image(*r.device, *img);
        return nullptr;
    }
    t->image = *img;
    t->scale = scale;
    return t;
}

constexpr usize kFreeDepths = 6;  // (kept for reuse; more are destroyed)
constexpr u64 kFreeBufferBytes = 64ull << 20;  // (host buffers kept for reuse at most)
constexpr VkBufferUsageFlags kHostBufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// A mapped host buffer of at least `bytes`, for any of a scene's uses: one given back (no more than four times as
// large), else a new one (of a power of two, 64 KiB or more).
Result<vk::Buffer> take_host_buffer(Renderer& r, VkDeviceSize bytes) {
    usize best = r.free_buffers.size();
    for (usize i = 0; i < r.free_buffers.size(); ++i) {
        const VkDeviceSize s = r.free_buffers[i].size;
        if (s >= bytes && s <= nx::max<VkDeviceSize>(bytes * 4, 65536) &&
            (best == r.free_buffers.size() || s < r.free_buffers[best].size))
            best = i;
    }
    if (best < r.free_buffers.size()) {
        vk::Buffer b = r.free_buffers[best];
        r.free_buffers.erase(r.free_buffers.begin() + static_cast<std::ptrdiff_t>(best));
        r.free_buffer_bytes -= b.size;
        return b;
    }
    return vk::create_host_buffer(*r.device, std::bit_ceil(nx::max<VkDeviceSize>(bytes, 65536)), kHostBufferUsage);
}

// A scene's descriptor pool: of kPoolSets sets, or the power of two above them its draws need (one given back,
// else new), else its own. (Made and freed a scene, a pool for a song's scenes of 160 draws took a phone's driver a
// fifth of the thread that ends the game's scenes: Adreno allocates each in the GPU's memory.)
constexpr u32 kPoolSets = 128, kFreePools = 16;
constexpr u32 pool_class(u32 sets) {
    u32 c = 0;
    while (c < 4 && sets > kPoolSets << c) ++c;
    return c;  // (4: none, its own)
}
VkDescriptorPool take_descriptor_pool(Renderer& r, u32 sets) {
    const u32 c = pool_class(sets);
    if (c < 4 && !r.free_pools[c].empty()) {
        const VkDescriptorPool p = r.free_pools[c].back();
        r.free_pools[c].pop_back();
        return p;
    }
    const u32 n = c < 4 ? kPoolSets << c : sets;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * n},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kDrawBindings - 3) * n},
                                          {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, n}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = n;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(r.device->device(), &pi, nullptr, &pool);
    return pool;
}
void give_descriptor_pool(Renderer& r, VkDescriptorPool pool, u32 sets) {
    if (!pool) return;
    if (const u32 c = pool_class(sets); c < 4 && r.free_pools[c].size() < kFreePools) {
        vkResetDescriptorPool(r.device->device(), pool, 0);
        r.free_pools[c].push_back(pool);
    } else {
        vkDestroyDescriptorPool(r.device->device(), pool, nullptr);
    }
}

// And back, once the GPU is done with it.
void Snapshot::clear() {
    if (buffer.buffer) give_host_buffer(renderer(), buffer);
    buffer_bytes = 0;
    bytes.clear();
}

void give_host_buffer(Renderer& r, vk::Buffer& b) {
    if (!b.buffer) return;
    r.free_buffers.push_back(b);
    r.free_buffer_bytes += b.size;
    b = {};
    // (more at an internal resolution: its scenes' buffers are larger)
    const u64 cap = kFreeBufferBytes * nx::max<u32>(state().options ? state().options->render_scale : 1, 1);
    while (r.free_buffer_bytes > cap && !r.free_buffers.empty()) {
        r.free_buffer_bytes -= r.free_buffers.front().size;
        vk::destroy_buffer(*r.device, r.free_buffers.front());
        r.free_buffers.erase(r.free_buffers.begin());
    }
}

// A depth/stencil image of this size: one a scene gave back, else a new one.
Result<vk::Image> take_depth_image(Renderer& r, VkFormat format, u32 width, u32 height) {
    for (usize i = 0; i < r.free_depths.size(); ++i) {
        const vk::Image& im = r.free_depths[i];
        if (im.desc.format == format && im.desc.width == width && im.desc.height == height) {
            vk::Image out = im;
            r.free_depths.erase(r.free_depths.begin() + static_cast<std::ptrdiff_t>(i));
            return out;
        }
    }
    vk::ImageDesc dd;
    dd.format = format;
    dd.width = width;
    dd.height = height;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return vk::create_image(*r.device, dd);
}

// A scene's depth/stencil image given back once the GPU is done with it (its contents no matter: a scene clears or
// loads its depth).
void free_depth_image(Renderer& r, vk::Image& image) {
    r.free_depths.push_back(image);
    image = {};
    if (r.free_depths.size() > kFreeDepths) {
        vk::Image& oldest = r.free_depths.front();
        r.passes->forget(oldest.view);
        vk::destroy_image(*r.device, oldest);
        r.free_depths.erase(r.free_depths.begin());
    }
}

constexpr usize kKeptDepths = 8;  // (the most kept: the least recently used one no pending scene draws with goes)

// The kept depth image of a depth/stencil surface at `scale`: found, or (`make`) made.
DepthTarget* kept_depth_for(Renderer& r, const DepthStencilSurface& s, u32 width, u32 height, u32 scale, bool make) {
    const u32 control = s.control & (0x03FFF000u | kDsTiled), stride = s.background_control >> 16;
    for (auto& t : r.kept_depths)
        if (t->address == s.depth_data && t->stencil_address == s.stencil_data && t->control == control &&
            t->stride == stride && t->width == width && t->height == height && t->scale == scale) {
            t->used = r.scenes;
            return t.get();
        }
    if (!make) return nullptr;
    if (r.kept_depths.size() >= kKeptDepths) {
        usize oldest = r.kept_depths.size();
        for (usize i = 0; i < r.kept_depths.size(); ++i) {
            bool drawing = false;
            for (const Renderer::Pending& p : r.pending)
                drawing |= p.kept_depth == r.kept_depths[i].get() || p.depth_sources[0] == r.kept_depths[i].get() ||
                           p.depth_sources[1] == r.kept_depths[i].get() ||
                           std::find(p.converted_from.begin(), p.converted_from.end(), r.kept_depths[i].get()) !=
                               p.converted_from.end();
            for (const Renderer::TargetConversion& c : r.target_conversions) drawing |= c.depth == r.kept_depths[i].get();
            if (!drawing && (oldest == r.kept_depths.size() || r.kept_depths[i]->used < r.kept_depths[oldest]->used))
                oldest = i;
        }
        if (oldest == r.kept_depths.size()) return nullptr;
        DepthTarget& t = *r.kept_depths[oldest];
        r.passes->forget(t.image.view);
        vk::destroy_image(*r.device, t.image);
        if (t.background.buffer) vk::destroy_buffer(*r.device, t.background);
        r.kept_depths.erase(r.kept_depths.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    vk::ImageDesc dd;
    dd.format = r.device->caps().depth_stencil_format;
    dd.width = width * scale;
    dd.height = height * scale;
    dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto img = vk::create_image(*r.device, dd);
    if (!img) return nullptr;
    auto t = nx::make_unique<DepthTarget>();
    t->address = s.depth_data, t->stencil_address = s.stencil_data, t->control = control, t->stride = stride;
    t->width = width, t->height = height, t->scale = scale, t->used = r.scenes;
    t->image = *img;
    r.kept_depths.push_back(std::move(t));
    return r.kept_depths.back().get();
}

// Whether guest memory holds what was last stored from a kept depth image (`written`, its depth's bytes then its
// stencil's): watched, else compared.
bool kept_holds(const DepthTarget& t, const DepthRanges& g) {
    if (t.written.empty() || t.written.size() != usize{g.depth_bytes} + g.stencil_bytes) return false;
    const bool watched = (!g.depth_bytes || unwritten(t.watch[0], g.depth, g.depth_bytes)) &&
                         (!g.stencil_bytes || unwritten(t.watch[1], g.stencil, g.stencil_bytes));
    if (watched && write_watch_mode() != 2) return true;
    if (!t.written.held()) return false;  // (written there since: not compared)
    const bool equal = depth_contents_equal(g, t.written.bytes);
    if (watched && !equal) unseen_write("a depth surface", g.depth ? g.depth : g.stencil, g.depth_bytes + g.stencil_bytes);
    return equal;
}

// Whether a kept depth image holds what the pending scenes leave in its surface's memory: the latest of them to write
// there (or to draw with the image) stored it from the image. 1: it does; 0: it does not (a scene drew with the image
// without storing it, or wrote there otherwise); -1: no pending scene writes there.
int pending_kept_depth(const Renderer& r, const DepthTarget* t, const DepthRanges& g) {
    auto overlaps = [&](u32 at, u32 n) {
        return (g.depth_bytes && g.depth < at + n && at < g.depth + g.depth_bytes) ||
               (g.stencil_bytes && g.stencil < at + n && at < g.stencil + g.stencil_bytes);
    };
    for (usize i = r.pending.size(); i-- > 0;) {
        const Renderer::Pending& p = r.pending[i];
        if (p.kept_depth == t) return p.stores_kept_depth ? 1 : 0;
        for (const auto& [at, n] : p.writes)
            if (overlaps(at, n)) return 0;
    }
    return -1;
}

// A kept image holding the depth (`stencil` false) or the stencil of a surface loaded: one stored as another surface
// over the same memory (a stencil shared by two formats, say: DF32M_S8 stored, S8 loaded), laid out alike (stride,
// tiling, size, scale; for the depth, the same format), which holds what the pending scenes leave there or what guest
// memory holds.
DepthTarget* kept_aspect(Renderer& r, const DepthStencilSurface& s, const DepthRanges& g, u32 width, u32 height, u32 scale,
                         bool stencil, const DepthTarget* drawing) {
    const u32 at = stencil ? g.stencil : g.depth, bytes = stencil ? g.stencil_bytes : g.depth_bytes;
    if (!bytes) return nullptr;
    const u32 control = s.control & (0x03FFF000u | kDsTiled), stride = s.background_control >> 16;
    for (auto& t : r.kept_depths) {
        if (t.get() == drawing || t->stride != stride || t->width != width || t->height != height || t->scale != scale ||
            (t->control & kDsTiled) != (control & kDsTiled) || t->image.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            continue;
        DepthLayout tl;
        if (!depth_layout(t->control & 0x03FFF000u, tl)) continue;
        if (stencil ? !tl.stencil || t->stencil_address != at : !tl.depth_bytes || t->control != control || t->address != at)
            continue;
        DepthRanges only;
        if (stencil) only.stencil = at, only.stencil_bytes = bytes;
        else only.depth = at, only.depth_bytes = bytes;
        const int pending = pending_kept_depth(r, t.get(), only);
        if (pending == 1) return t.get();
        if (pending != -1) continue;
        // (what was stored: its depth's bytes, then its stencil's)
        const usize offset = stencil ? usize{depth_samples(s, width, height)} * tl.depth_bytes : 0;
        if (t->written.size() < offset + bytes) continue;
        const bool watched = unwritten(t->watch[stencil ? 1 : 0], at, bytes);
        if (watched && write_watch_mode() != 2) return t.get();
        if (!t->written.held()) continue;
        if (std::memcmp(memory().host(at), t->written.bytes.data() + offset, bytes) == 0) return t.get();
        if (watched) unseen_write("a depth surface's aspect", at, bytes);
    }
    return nullptr;
}

// Whether a texture reads a depth surface as sampling its depth aspect gives it: one float of the depth, the other
// components 0, 0, 1 (the texture decoder, on one texel).
bool texels_as_depth(const TextureInfo& info) {
    if (texel_bits(info.base_format) != 32 || info.gamma || info.palette) return false;
    TextureInfo one = info;
    one.type = texture_type::kLinear;
    one.width = one.height = 1;
    one.levels = 1;
    one.stride = 4;
    const f32 probe = 0.375f;
    auto texels = read_texels(one, [&](u32, void* out, u32 bytes) {
        if (bytes > 4) return false;
        std::memcpy(out, &probe, bytes);
        return true;
    });
    if (!texels || texels->bytes != 16 || texels->data.size() < 16) return false;
    f32 c[4];
    std::memcpy(c, texels->data.data(), 16);
    return c[0] == probe && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f;
}

// Whether guest memory holds the depth a depth-only scene last stored from its image (`written`): watched, else
// compared.
bool depth_target_holds(const DepthTarget& t) {
    const u32 n = static_cast<u32>(t.written.size());
    if (!n || !memory().valid(t.address, n)) return false;
    const bool watched = unwritten(t.watch[0], t.address, n);
    if (watched && write_watch_mode() != 2) return true;
    if (!t.written.held()) return false;
    const bool equal = std::memcmp(memory().host(t.address), t.written.bytes.data(), n) == 0;
    if (watched && !equal) unseen_write("a stored depth surface", t.address, n);
    return equal;
}

// A texture that reads a depth surface stored at the internal resolution: its kept image's depth, when the texture
// is that surface (address, size, tiled or rows as it was stored), guest memory still holds what was stored, and it
// is not this scene's depth.
DepthTarget* scaled_depth_texture(Renderer& r, const TextureInfo& info, const DepthTarget* drawing) {
    if (info.levels != 1 || !info.address) return nullptr;
    for (auto& t : r.depth_targets) {
        if (t->scale <= 1 || t.get() == drawing || t->address != info.address || t->width != info.width ||
            t->height != info.height || t->written.empty())
            continue;
        const bool tiled = t->control & kDsTiled;
        if (tiled ? info.type != texture_type::kTiled
                  : (info.type != texture_type::kLinear && info.type != texture_type::kLinearStrided) ||
                        info.stride != t->stride * 32 * 4)
            return nullptr;
        if (!texels_as_depth(info)) return nullptr;
        if (!depth_target_holds(*t)) return nullptr;
        return t.get();
    }
    return nullptr;
}

// A texture as the draw's control words describe it (cached), or none if it cannot be read.
// PSVRECORE_GXM_PROFILE=1: every 200 scenes, the time the scenes spent in each part of render_scene and in the
// textures (looked up, hashed, new ones decoded and uploaded), and every 50th new texture's description (to stderr).
struct SceneProfile {
    static constexpr u32 kParts = 11;
    static constexpr const char* kNames[kParts] = {"depth in", "draws", "buffers", "descriptors and textures",
                                                    "commands", "earlier scenes finished (GPU, memory)", "submit",
                                                    "queued", "the lock and eviction", "colour surface compared",
                                                    "colour uploaded"};
    static constexpr u32 kOrder[kParts] = {8, 9, 10, 0, 1, 2, 3, 4, 6, 5, 7};
    u64 colour_uploads = 0;
    u64 began = 0, ns_before[kParts] = {}, misses_before = 0, miss_ns_before = 0;  // (this scene's start)
    u64 pipelines_made = 0, pipeline_ns = 0, pipelines_before = 0, pipeline_ns_before = 0;  // (made while drawing)
    u64 skipped = 0, skipped_before = 0;  // (draws left out: their pipelines being made asynchronously)
    // (shader modules and secondary programs' compute pipelines made while drawing: translated or from the cache)
    u64 shaders_made = 0, shader_ns = 0, shaders_before = 0, shader_ns_before = 0;
    u64 finish_wait_before = 0, finish_work_before = 0, finished_before = 0;
    // (the write-back's parts: the colour, the uniform buffers and counters, the depth and stencil, the rest)
    u64 back_ns[4] = {}, back_before[4] = {}, back_printed[4] = {};
    u64 tex_before[5] = {};  // (hash_ns, decode_ns, create_ns, upload_ns, ahead at this scene's start)
    u64 buffer_wait_ns = 0;  // (sceGxmEndScene's wait for the display to let the scene's buffer go)
    bool on = false;
    bool read = false;  // (the switch, read at the first scene: --env sets it after the program starts)
    u64 ns[kParts] = {};
    // (the scenes' commands on the GPU, by timestamps: before the render pass (from the start of the scene's
    // commands, which may include waiting for the scene before), the pass, and what follows it (the readbacks))
    u64 gpu_before_ns = 0, gpu_pass_ns = 0, gpu_after_ns = 0, gpu_measured = 0;
    u64 draws = 0, colour_reads = 0, barriers = 0;  // (draws, those reading the colour, barriers between them)
    // (the GPU's timeline: idle between one scene's end and the next one's start, and the scenes' own spans from
    // the later of their start and the scene before's end)
    u64 gpu_idle_ns = 0, gpu_span_ns = 0, gpu_last_end = 0;
    // (the GPU's gaps between scenes by length: under 0.1 ms, 0.5, 2, longer; how many and their time)
    u64 gap_count[4] = {}, gap_ns[4] = {};
    u64 finished = 0, finish_wait_ns = 0, finish_work_ns = 0;  // (scenes finished: waiting for the GPU, the rest)
    // (by why they were: at a submission, read by a later scene, a point of synchronisation, the flusher)
    // (reads: memory copied (uniforms, vertices), indices, a colour surface, a depth surface, a texture)
    static constexpr u32 kWhys = 8;
    static constexpr const char* kWhy[kWhys] = {"submit", "read (copied)", "sync", "flusher", "read (indices)",
                                                "read (colour surface)", "read (depth surface)", "read (texture)"};
    u64 why_count[kWhys] = {}, why_wait_ns[kWhys] = {};

    u64 unchanged = 0, write_faults = 0;  // (textures found unwritten; writes to watched pages, by the process)
    u64 ahead = 0;  // (new textures whose texels were decoded ahead)
    u64 converted = 0, converted_bg = 0;  // (textures made on the GPU from surfaces' images; backgrounds taken)
    u64 from_targets = 0, lookups = 0, hashed = 0, hash_ns = 0, misses = 0, miss_ns = 0, miss_bytes = 0, decode_ns = 0, create_ns = 0, upload_ns = 0;
    u64 at = 0;
    u32 part = 0, scenes = 0;
    void start() {
        if (!read) {
            const char* v = std::getenv("PSVRECORE_GXM_PROFILE");
            on = v && *v && *v != '0';
            read = true;
        }
        part = 0;
        at = nx::os::ticks_ns();
        if (on) {
            began = at;
            std::memcpy(ns_before, ns, sizeof ns);
            misses_before = misses, miss_ns_before = miss_ns, pipelines_before = pipelines_made, pipeline_ns_before = pipeline_ns;
            skipped_before = skipped;
            shaders_before = shaders_made, shader_ns_before = shader_ns;
            finish_wait_before = finish_wait_ns, finish_work_before = finish_work_ns, finished_before = finished;
            std::memcpy(back_before, back_ns, sizeof back_ns);
            tex_before[0] = hash_ns, tex_before[1] = decode_ns, tex_before[2] = create_ns, tex_before[3] = upload_ns;
            tex_before[4] = ahead;
        }
    }
    void mark(u32 i) {
        if (!on) return;
        const u64 now = nx::os::ticks_ns();
        ns[i] += now - at;
        at = now;
    }
    void done() {
        if (!on) return;
        // (a scene that took longer than a frame at 60 frames a second: what took the time, at once)
        if (const u64 took = nx::os::ticks_ns() - began; took > 16'000'000) {
            nx::string parts;
            for (u32 i : kOrder)
                if (ns[i] - ns_before[i] >= 1'000'000)
                    parts += nx::format(" {} {:.1f};", kNames[i], static_cast<f64>(ns[i] - ns_before[i]) / 1e6);
            nx::logi(logs::gpu, "gxm profile: a slow scene, {:.1f} ms:{} {} new textures ({:.1f} ms), {} shaders and secondary "
                     "pipelines made ({:.1f} ms), {} pipelines made ({:.1f} ms), {} earlier scenes finished ({:.1f} ms waiting "
                     "for the GPU, {:.1f} writing back), {} draws left out",
                     static_cast<f64>(took) / 1e6, parts, misses - misses_before, static_cast<f64>(miss_ns - miss_ns_before) / 1e6,
                     shaders_made - shaders_before, static_cast<f64>(shader_ns - shader_ns_before) / 1e6,
                     pipelines_made - pipelines_before, static_cast<f64>(pipeline_ns - pipeline_ns_before) / 1e6,
                     finished - finished_before, static_cast<f64>(finish_wait_ns - finish_wait_before) / 1e6,
                     static_cast<f64>(finish_work_ns - finish_work_before) / 1e6, skipped - skipped_before);
            if (ns[3] - ns_before[3] >= 4'000'000)
                nx::logi(logs::gpu, "gxm profile: its textures: hashed {:.1f} ms, decoded {:.1f} ({} decoded ahead), images "
                         "made {:.1f}, uploads queued {:.1f}", static_cast<f64>(hash_ns - tex_before[0]) / 1e6,
                         static_cast<f64>(decode_ns - tex_before[1]) / 1e6, ahead - tex_before[4],
                         static_cast<f64>(create_ns - tex_before[2]) / 1e6, static_cast<f64>(upload_ns - tex_before[3]) / 1e6);
            if (finish_work_ns - finish_work_before >= 2'000'000)
                nx::logi(logs::gpu, "gxm profile: written back: colour {:.1f} ms, buffers and counters {:.1f}, depth {:.1f}, the rest {:.1f}",
                         static_cast<f64>(back_ns[0] - back_before[0]) / 1e6, static_cast<f64>(back_ns[1] - back_before[1]) / 1e6,
                         static_cast<f64>(back_ns[2] - back_before[2]) / 1e6, static_cast<f64>(back_ns[3] - back_before[3]) / 1e6);
        }
        if (++scenes < 200) return;
        u64 all = 0;
        for (u64 n : ns) all += n;
        static const u64 since = nx::os::ticks_ns();
        std::fprintf(stderr, "gxm profile: %u scenes, %.1f ms each (%.1f s):", scenes, static_cast<f64>(all) / 1e6 / scenes,
                     static_cast<f64>(nx::os::ticks_ns() - since) / 1e9);
        for (u32 i : kOrder) std::fprintf(stderr, " %s %.2f;", kNames[i], static_cast<f64>(ns[i]) / 1e6 / scenes);
        std::fprintf(stderr, " %.2f colour surfaces uploaded a scene; %.2f ms waiting for the display to let the buffer go;",
                     static_cast<f64>(colour_uploads) / scenes, static_cast<f64>(buffer_wait_ns) / 1e6 / scenes);
        if (finished)
            std::fprintf(stderr, " finishing a scene: %.2f ms waiting, %.2f ms the rest;",
                         static_cast<f64>(finish_wait_ns) / 1e6 / finished, static_cast<f64>(finish_work_ns) / 1e6 / finished);
        if (finished) {
            std::fprintf(stderr, " written back: colour %.2f, buffers and counters %.2f, depth %.2f, the rest %.2f ms a scene;",
                         static_cast<f64>(back_ns[0] - back_printed[0]) / 1e6 / finished, static_cast<f64>(back_ns[1] - back_printed[1]) / 1e6 / finished,
                         static_cast<f64>(back_ns[2] - back_printed[2]) / 1e6 / finished, static_cast<f64>(back_ns[3] - back_printed[3]) / 1e6 / finished);
            std::memcpy(back_printed, back_ns, sizeof back_ns);
        }
        for (u32 i = 0; i < kWhys; ++i)
            if (why_count[i])
                std::fprintf(stderr, " %s %.2f a scene (%.2f ms waiting);", kWhy[i], static_cast<f64>(why_count[i]) / scenes,
                             static_cast<f64>(why_wait_ns[i]) / 1e6 / scenes);
        std::fprintf(stderr, " %.1f draws (%.1f reading the colour, %.1f barriers) a scene;",
                     static_cast<f64>(draws) / scenes, static_cast<f64>(colour_reads) / scenes,
                     static_cast<f64>(barriers) / scenes);
        if (gpu_measured)
            std::fprintf(stderr, " GPU: idle %.2f ms, busy %.2f ms a scene;", static_cast<f64>(gpu_idle_ns) / 1e6 / gpu_measured,
                         static_cast<f64>(gpu_span_ns) / 1e6 / gpu_measured);
        if (gpu_measured) {
            std::fprintf(stderr, " GPU gaps between scenes:");
            static constexpr const char* kGaps[4] = {"under 0.1 ms", "0.1-0.5", "0.5-2", "over 2"};
            for (u32 i = 0; i < 4; ++i)
                std::fprintf(stderr, " %s %.2f (%.2f ms)%s", kGaps[i], static_cast<f64>(gap_count[i]) / gpu_measured,
                             static_cast<f64>(gap_ns[i]) / 1e6 / gpu_measured, i < 3 ? "," : " a scene;");
        }
        if (gpu_measured)
            std::fprintf(stderr, " GPU: %.2f ms before the pass, %.2f the pass, %.2f after it a scene;",
                         static_cast<f64>(gpu_before_ns) / 1e6 / gpu_measured, static_cast<f64>(gpu_pass_ns) / 1e6 / gpu_measured,
                         static_cast<f64>(gpu_after_ns) / 1e6 / gpu_measured);
        std::fprintf(stderr, " surfaces written back found unchanged: %.2f by watching, %.2f compared a scene;",
                     static_cast<f64>(g_watch_seen.exchange(0)) / scenes, static_cast<f64>(g_watch_compared.exchange(0)) / scenes);
        if (const DisplayQueueTimes q = take_display_queue_times(); q.entries)
            std::fprintf(stderr, " display queue: %llu frames, a frame: %.2f ms the game waited for room, %.2f its scenes "
                         "finishing, %.2f the game's callback;", static_cast<unsigned long long>(q.entries),
                         static_cast<f64>(q.room_ns) / 1e6 / q.entries, static_cast<f64>(q.finish_ns) / 1e6 / q.entries,
                         static_cast<f64>(q.callback_ns) / 1e6 / q.entries);
        std::fprintf(stderr, " write-backs deferred %.2f, written when touched %.2f a scene;",
                     static_cast<f64>(g_lazy_deferred.exchange(0)) / scenes, static_cast<f64>(g_lazy_written.exchange(0)) / scenes);
        std::fprintf(stderr, " cached %u textures, %.0f MiB; %.2f made from surfaces a scene (%.2f converted, %.3f backgrounds);",
                     static_cast<u32>(renderer().textures.size()), static_cast<f64>(renderer().texture_bytes) / 1048576.0,
                     static_cast<f64>(from_targets + converted) / scenes, static_cast<f64>(converted) / scenes,
                     static_cast<f64>(converted_bg) / scenes);
        std::fprintf(stderr,
                     " textures: %.1f lookups, %.1f unwritten, %.1f hashed (%.2f ms), %.1f writes to watched pages, %.2f "
                     "new (%.0f KiB, %.2f ms: decoded %.2f, made %.2f, uploaded %.2f) a scene\n",
                     static_cast<f64>(lookups) / scenes, static_cast<f64>(unchanged) / scenes, static_cast<f64>(hashed) / scenes,
                     static_cast<f64>(hash_ns) / 1e6 / scenes, static_cast<f64>(memory().write_faults() - write_faults) / scenes,
                     static_cast<f64>(misses) / scenes, static_cast<f64>(miss_bytes) / 1024.0 / scenes,
                     static_cast<f64>(miss_ns) / 1e6 / scenes, static_cast<f64>(decode_ns) / 1e6 / scenes,
                     static_cast<f64>(create_ns) / 1e6 / scenes, static_cast<f64>(upload_ns) / 1e6 / scenes);
        {
            const PrefetchStats ps = prefetch_stats();
            std::fprintf(stderr, "gxm profile: textures decoded ahead: %llu, %llu taken (%llu in these scenes), %llu changed "
                         "while decoded, %llu dropped; %.0f MiB kept\n", static_cast<unsigned long long>(ps.decoded),
                         static_cast<unsigned long long>(ps.taken), static_cast<unsigned long long>(ahead),
                         static_cast<unsigned long long>(ps.changed), static_cast<unsigned long long>(ps.dropped),
                         static_cast<f64>(ps.bytes) / 1048576.0);
        }
        const u64 last_end = gpu_last_end;
        *this = SceneProfile{};
        on = read = true;
        gpu_last_end = last_end;
        write_faults = memory().write_faults();
    }
};
SceneProfile g_profile;

// A 64-bit hash of bytes, for knowing whether a texture's data changed: four lanes of 64-bit words at a time (a
// texture of megabytes in a fraction of a millisecond), then the rest, then a final mix. Not for anything an
// adversary chooses.
u64 content_hash(const u8* p, usize n, u64 seed) {
    constexpr u64 k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    u64 a = seed ^ k1, b = seed + k2, c = ~seed, e = seed * k1;
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 w[4];
        std::memcpy(w, p + i, 32);
        a = std::rotl(a ^ (w[0] * k2), 31) * k1;
        b = std::rotl(b ^ (w[1] * k2), 31) * k1;
        c = std::rotl(c ^ (w[2] * k2), 31) * k1;
        e = std::rotl(e ^ (w[3] * k2), 31) * k1;
    }
    u64 h = std::rotl(a, 1) + std::rotl(b, 7) + std::rotl(c, 12) + std::rotl(e, 18) + n;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = std::rotl(h ^ (w * k2), 27) * k1;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 33);
}

}  // namespace

u64 texture_contents(const TextureInfo& info, u32 size, u32 palette) {
    u64 h = content_hash(static_cast<const u8*>(memory().host(info.address)), size, 0);
    if (palette) h = content_hash(static_cast<const u8*>(memory().host(palette)), 1024, h);
    return h;
}

namespace {

// Where a texture starts inside a colour surface (at its address, or past it, smaller): laid out alike (rows of the surface's
// stride, or rows of tiles as wide as its own: a tile row's bytes from the surface's start), wholly within its size.
// 4- and 8-byte pixels.
bool texture_in_surface(const TextureInfo& info, const Target& t, u32& ox, u32& oy) {
    const SurfaceLayout& l = t.layout;
    const u32 b = l.bytes;
    if (info.address < t.address || info.address - t.address >= l.span() || (b != 4 && b != 8)) return false;
    const u32 delta = info.address - t.address;
    if (l.kind == SurfaceLayout::kLinear &&
        (info.type == texture_type::kLinear || info.type == texture_type::kLinearStrided) && info.stride == l.stride * b &&
        delta % b == 0) {
        oy = delta / (l.stride * b), ox = delta % (l.stride * b) / b;
    } else if (l.kind == SurfaceLayout::kTiled && info.type == texture_type::kTiled &&
               (l.stride + 31) / 32 == (info.width + 31) / 32) {
        const u32 tile_row = (l.stride + 31) / 32 * 1024 * b;
        if (delta % tile_row) return false;
        ox = 0, oy = delta / tile_row * 32;
    } else {
        return false;
    }
    return u64{ox} + info.width <= t.width && u64{oy} + info.height <= t.height;
}

// A texture that reads a colour surface whose image holds what guest memory does (the surface drawn and written
// back, nothing else written there since): made on the GPU from that image instead of being read back, decoded and
// uploaded (a render target read as a texture each frame). One level, texels as the image holds them, the surface's
// layout, as tall; wider for a linear surface (its stride's columns past the width come from guest memory). A surface
// drawn at the internal resolution gives a texture at that resolution, from its scaled image (a draw reading the
// surface it draws into, or wider than the surface, still sees what was drawn there at that resolution).
//
// Or a texture inside the surface, laid out as it is (rows of its stride, or rows of tiles as wide; WipEout 2048 reads
// a 256 x 128 texture from a 256 x 1536 surface each frame): copied from that part of the image.
//
// `only`: that surface, which a pending scene draws (its image will hold what guest memory will: not compared).
CachedTexture* texture_from_target(Renderer& r, const TextureInfo& info, u64 key, u64 words_key, Target* only = nullptr) {
    if (info.levels != 1 || !info.address) return nullptr;  // (cube types: texture_reads_layout refuses them)
    const bool srgb = srgb_textures(*r.device);
    for (auto& t : r.targets) {
        if (only && t.get() != only) continue;
        // (the surface this scene draws into too: the copy is recorded before the draws, so it holds what the scene
        // starts from, as the console's texture unit reads it)
        if (!only && t->written.empty()) continue;
        // (the surface, or as tall and wider when linear: the columns past it from guest memory; else a part of it)
        u32 ox = 0, oy = 0;
        const bool whole = t->address == info.address && info.height == t->height && info.width >= t->width &&
                           (info.width == t->width || t->layout.kind == SurfaceLayout::kLinear);
        if (!whole && !texture_in_surface(info, *t, ox, oy)) continue;
        if (!texture_reads_layout(info, t->layout) || !texels_as_image(info, t->image.desc.format, srgb)) continue;
        if (!only) {
            const Changes changes = surface_changes(*t);
            if (changes.all || changes.pixels) continue;
        }
        const u32 s = t->scale > 1 && t->scaled.image ? t->scale : 1;
        const u32 pb = t->layout.bytes;  // (a pixel's bytes, the image's as guest memory's: texture_reads_layout)
        vk::ImageDesc d;
        // (a texture with a gamma mode: the image's bytes copied into an image of its sRGB format, of the same size of
        // texel, which decodes them as it is sampled)
        d.format = texture_format(info, t->image.desc.format);
        d.width = info.width * s;
        d.height = info.height * s;
        CachedTexture c;
        bool reused = false;
        if (auto was = r.by_words.find(words_key); was != r.by_words.end()) {
            auto old = r.textures.find(was->second);
            if (old != r.textures.end() && old->second.used != r.scenes) {
                const vk::ImageDesc& od = old->second.image.desc;
                if (od.format == d.format && od.width == d.width && od.height == d.height && od.mips == 1 &&
                    od.layers == 1 && !od.cube) {
                    c = old->second;
                    r.textures.erase(old);
                    reused = true;
                }
            }
        }
        if (!reused) {
            auto image = vk::create_image(*r.device, d);
            if (!image) return nullptr;
            c.image = *image;
            c.sampler = texture_sampler(r, info, d.format, 1);
            c.bytes = usize{d.width} * d.height * pb;
            r.texture_bytes += c.bytes;
        }
        Renderer::TargetCopy copy;
        copy.key = key;
        copy.from = t.get();
        copy.x = ox, copy.y = oy;
        copy.width = whole ? t->width : info.width, copy.height = whole ? t->height : info.height;
        copy.scale = s;
        if (info.width > t->width) {
            // (the columns past the surface's width: as they are in guest memory)
            TextureInfo strip = info;
            strip.address = info.address + t->width * pb;
            strip.width = info.width - t->width;
            auto texels = read_texels(strip, [](u32 address, void* out, u32 bytes) {
                if (!memory().valid(address, bytes)) return false;
                std::memcpy(out, memory().host(address), bytes);
                return true;
            }, false, false, srgb);
            if (texels && (texels->bytes != pb || texels->srgb != (info.gamma != 0)))
                texels = decltype(texels)(failure("texture: the columns past the surface decode otherwise"));
            if (texels && s > 1) {
                // (each texel over an s x s block, as the scaled image's pixels are)
                const u32 w = strip.width, h = info.height;
                nx::vector<u8> up(usize{w} * s * h * s * pb);
                for (u32 y = 0; y < h * s; ++y)
                    for (u32 x = 0; x < w * s; ++x)
                        std::memcpy(up.data() + (usize{y} * w * s + x) * pb, texels->data.data() + (usize{y / s} * w + x / s) * pb, pb);
                texels->data = std::move(up);
            }
            auto buffer = texels ? vk::create_host_buffer(*r.device, texels->data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
                                 : Result<vk::Buffer>(failure("texture: the columns past the surface"));
            if (!buffer) {
                retire_image(r, c.image);
                r.texture_bytes -= c.bytes;
                return nullptr;
            }
            std::memcpy(buffer->mapped, texels->data.data(), texels->data.size());
            vk::flush(*r.device, *buffer);
            copy.strip = *buffer;
            copy.strip_x = t->width, copy.strip_w = strip.width;
        }
        r.target_copies.push_back(copy);
        c.used = r.scenes;
        r.by_words[words_key] = key;
        auto [it, added] = r.textures.emplace(key, c);
        return &it->second;
    }
    return nullptr;
}

// ---- Textures converted on the GPU from a surface's image -------------------------------------------------------

bool read_guest_memory(u32 address, void* out, u32 bytes) {
    if (!memory().valid(address, bytes)) return false;
    std::memcpy(out, memory().host(address), bytes);
    return true;
}

// The shader's decode of a texture's format and swizzle from an image of `source` (target_decode, probed once).
const std::optional<TargetDecode>& decode_for(Renderer& r, const TextureInfo& info, TargetSource source) {
    const u64 key = u64{info.base_format} ^ (u64{info.swizzle} << 32) ^ (u64{static_cast<u32>(source)} << 40) ^
                    (u64{info.gamma} << 48) ^ (u64{info.levels} << 52);
    auto it = r.target_decodes.find(key);
    if (it == r.target_decodes.end()) it = r.target_decodes.emplace(key, target_decode(info, source)).first;
    return it->second;
}

// A device buffer for a conversion's work (one given back, no more than four times as large, else a new one).
Result<vk::DeviceBuffer> take_work(Renderer& r, VkDeviceSize bytes) {
    usize best = r.free_work.size();
    for (usize i = 0; i < r.free_work.size(); ++i) {
        const VkDeviceSize s = r.free_work[i].size;
        if (s >= bytes && s <= bytes * 4 && (best == r.free_work.size() || s < r.free_work[best].size)) best = i;
    }
    if (best < r.free_work.size()) {
        const vk::DeviceBuffer b = r.free_work[best];
        r.free_work.erase(r.free_work.begin() + static_cast<std::ptrdiff_t>(best));
        return b;
    }
    return vk::create_device_buffer(*r.device, std::bit_ceil(nx::max<VkDeviceSize>(bytes, 65536)),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}
void give_work(Renderer& r, vk::DeviceBuffer& b) {
    if (!b.buffer) return;
    r.free_work.push_back(b);
    b = {};
    VkDeviceSize total = 0;
    for (const vk::DeviceBuffer& f : r.free_work) total += f.size;
    while (total > (96ull << 20) && !r.free_work.empty()) {
        total -= r.free_work.front().size;
        vk::destroy_buffer(*r.device, r.free_work.front());
        r.free_work.erase(r.free_work.begin());
    }
}

// A conversion's buffers and set given back (once the scene that recorded it is done, or it was not recorded).
void release_conversion(Renderer& r, Renderer::TargetConversion& c) {
    give_work(r, c.work);
    if (c.from_image) free_target_image_set(*r.device, r.converter, c.set);
    else free_target_converter_set(*r.device, r.converter, c.set);
    c.set = VK_NULL_HANDLE;
    give_host_buffer(r, c.background_staging);
    give_host_buffer(r, c.verify);
    if (c.retired_background.buffer) vk::destroy_buffer(*r.device, c.retired_background);
}

// PSVRECORE_TARGET_TEXTURES=verify: each texture made on the GPU from a surface's image compared, once its scene is
// done, with what the decoder makes of guest memory when it was made (not at an internal resolution above 1, where
// a colour surface's texture is made at that resolution). Differences are logged.
bool verify_conversions() {
    static const bool on = [] {
        const char* v = std::getenv("PSVRECORE_TARGET_TEXTURES");
        return v && std::strcmp(v, "verify") == 0;
    }();
    return on;
}
void verified_conversion(Renderer::TargetConversion& c, vk::Device& d, const Target* drawn) {
    static u64 checked = 0, differed = 0;
    if (!c.verify.buffer) return;
    if (c.pending) {
        // (unless this scene drew over that surface since, or stored that depth again)
        if (c.color == drawn || (c.depth && c.depth->generation != c.generation)) return;
        auto texels = read_texels(c.info, read_guest_memory, false, true);
        if (!texels || texels->data.size() != c.verify_bytes) return;
        c.expected = std::move(texels->data);
    }
    if (c.expected.empty()) return;
    vk::invalidate(d, c.verify);
    const u8* got = static_cast<const u8*>(c.verify.mapped);
    ++checked;
    if (std::memcmp(got, c.expected.data(), c.expected.size()) != 0) {
        usize at = 0, bytes = 0;
        while (got[at] == c.expected[at]) ++at;
        for (usize i = 0; i < c.expected.size(); ++i) bytes += got[i] != c.expected[i];
        ++differed;
        const usize texel = at / 8 * 8;
        u64 g = 0, e = 0;
        std::memcpy(&g, got + texel, nx::min<usize>(8, c.expected.size() - texel));
        std::memcpy(&e, c.expected.data() + texel, nx::min<usize>(8, c.expected.size() - texel));
        nx::logw(logs::gpu, "target textures: a texture {} x {} from a {} surface differs from the decoder's at byte {} "
                 "({} bytes; the GPU's {:016x}, the decoder's {:016x}) ({} of {} differed){}", c.k.width, c.k.height,
                 c.depth ? "depth" : "colour", at, bytes, g, e, differed, checked,
                 c.pending ? " (made from a pending scene's image)" : "");
    } else if (checked % 500 == 0) {
        nx::logi(logs::gpu, "target textures: {} checked against the decoder, {} differed", checked, differed);
    }
}

// PSVRECORE_TARGET_TEXTURES=verify: an HDR surface's words packed on the GPU compared with pack_u2f10f10f10 of its
// texels. Differences are logged.
void verify_packing(vk::Device& d, vk::Buffer& texels, const u8* words, u32 width, u32 height) {
    static u64 checked = 0, differed = 0;
    vk::invalidate(d, texels);
    const usize n = usize{width} * height;
    nx::vector<u32> cpu(n);
    pack_u2f10f10f10(static_cast<const u16*>(texels.mapped), cpu.data(), n);
    ++checked;
    if (std::memcmp(cpu.data(), words, n * 4) != 0) {
        usize at = 0;
        u32 g = 0;
        for (;; ++at) {
            std::memcpy(&g, words + at * 4, 4);
            if (g != cpu[at]) break;
        }
        ++differed;
        nx::logw(logs::gpu, "target textures: a {} x {} surface packed on the GPU differs from the CPU's at pixel {} (the GPU's "
                 "{:08x}, the CPU's {:08x}) ({} of {} differed)", width, height, at, g, cpu[at], differed, checked);
    } else if (checked % 500 == 0) {
        nx::logi(logs::gpu, "target textures: {} surfaces packed on the GPU checked, {} differed", checked, differed);
    }
}

// The texture made: its image (the one these words showed before, of this size and format, when no draw of this
// scene samples it), the conversion queued for the scene's commands.
CachedTexture* queue_conversion(Renderer& r, const TextureInfo& info, u64 key, u64 words_key, const TargetDecode& dec,
                                Renderer::TargetConversion conv) {
    TargetTextureConstants& k = conv.k;
    k.kind = dec.kind, k.mask = dec.mask, k.select = dec.select, k.alpha_low = dec.alpha_low ? 1 : 0;
    target_packing(k);
    vk::ImageDesc d;
    d.format = dec.single ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
    d.width = k.width, d.height = k.height;
    if (dec.single) {
        constexpr VkComponentSwizzle of[3] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
        d.components = {of[dec.rgba_of[0]], of[dec.rgba_of[1]], of[dec.rgba_of[2]], of[dec.rgba_of[3]]};
    }
    // (an HDR surface's: from its image into the texture, a storage image, where the device's shader makes the same)
    conv.from_image = conv.color && r.converter.images_ready() && !dec.single && k.a_width == k.width &&
                      k.a_height == k.height;
    if (conv.from_image) {
        d.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        conv.set = target_image_set(*r.device, r.converter);
        if (!conv.set) conv.from_image = false, d.usage = vk::ImageDesc{}.usage;
    }
    // (else the work buffer: the image's texels, then the texels made, 16-byte aligned)
    const u32 src_words = conv.w * conv.h * target_source_words(static_cast<TargetSource>(k.source));
    k.src = 0;
    k.out = (src_words + 3) & ~3u;
    const VkDeviceSize work_bytes = (VkDeviceSize{k.out} + VkDeviceSize{k.width} * k.height * target_out_words(dec)) * 4;
    if (!conv.from_image) {
        auto work = take_work(r, work_bytes);
        if (!work) return nullptr;
        conv.work = *work;
    }
    DepthTarget* bg = k.a_width < k.width || k.a_height < k.height ? conv.depth : nullptr;
    const bool upload = bg && !bg->background_uploaded;
    if (upload) {
        // (guest memory's bytes, uploaded by the scene's commands; into a larger buffer when they grew)
        const VkDeviceSize bytes = (bg->background_bytes.size() + 3) & ~VkDeviceSize{3};
        auto staging = take_host_buffer(r, bytes);
        if (!staging) {
            give_work(r, conv.work);
            return nullptr;
        }
        conv.background_staging = *staging;
        if (bg->background.size < bytes) {
            auto b = vk::create_device_buffer(*r.device, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (!b) {
                release_conversion(r, conv);
                return nullptr;
            }
            conv.retired_background = bg->background;
            bg->background = *b;
        }
        std::memcpy(conv.background_staging.mapped, bg->background_bytes.data(), bg->background_bytes.size());
        vk::flush(*r.device, conv.background_staging);
    }
    if (!conv.from_image)
        conv.set = target_converter_set(*r.device, r.converter, conv.work.buffer, work_bytes,
                                        bg ? bg->background.buffer : VK_NULL_HANDLE, bg ? bg->background.size : 0);
    CachedTexture c;
    bool reused = false;
    if (conv.set) {
        if (auto was = r.by_words.find(words_key); was != r.by_words.end()) {
            auto old = r.textures.find(was->second);
            if (old != r.textures.end() && old->second.used != r.scenes) {
                const vk::ImageDesc& od = old->second.image.desc;
                if (od.format == d.format && od.width == d.width && od.height == d.height && od.mips == 1 &&
                    od.layers == 1 && !od.cube && od.usage == d.usage &&
                    std::memcmp(&od.components, &d.components, sizeof d.components) == 0) {
                    c = old->second;
                    r.textures.erase(old);
                    reused = true;
                }
            }
        }
        if (!reused) {
            if (auto image = vk::create_image(*r.device, d)) {
                c.image = *image;
                c.sampler = texture_sampler(r, info, d.format, 1);
                c.bytes = u64{d.width} * d.height * vk::texel_size(d.format);
                r.texture_bytes += c.bytes;
            }
        }
    }
    if (!c.image.image) {
        // (the old, smaller background stays the surface's until a conversion uploads a new one)
        if (conv.retired_background.buffer) std::swap(conv.retired_background, bg->background);
        release_conversion(r, conv);
        return nullptr;
    }
    if (upload) bg->background_uploaded = true;
    if (verify_conversions() && (conv.depth || conv.color->scale <= 1 || !conv.color->scaled.image)) {
        auto texels = read_texels(info, read_guest_memory, false, true);
        auto readback = texels ? take_host_buffer(r, texels->data.size()) : Result<vk::Buffer>(failure("verify"));
        if (texels && readback && texels->data.size() == usize{k.width} * k.height * target_out_words(dec) * 4) {
            if (!conv.pending) conv.expected = std::move(texels->data);
            conv.verify = *readback;
            conv.verify_bytes = usize{k.width} * k.height * target_out_words(dec) * 4;
            conv.info = info;
        } else if (readback) {
            give_host_buffer(r, *readback);
        }
    }
    conv.key = key;
    r.target_conversions.push_back(conv);
    c.used = r.scenes;
    r.by_words[words_key] = key;
    if (g_profile.on) ++g_profile.converted, g_profile.converted_bg += upload;
    return &r.textures.emplace(key, c).first->second;
}

// A texture that reads a surface the GPU drew, in a format whose texels the shader makes from its image
// (target_texture.hpp): an HDR colour surface (U2F10F10F10, read as such), a depth surface a scene stores (DF32,
// DF32M read as F32 or F32M; D16 as U16). Made from the image while guest memory still holds what was written back
// from it (and, for a texture that reads past what the scenes store, what it held when it was taken as the
// background): keyed by the surface's generation, not hashed, decoded and uploaded.
//
// A colour surface: the texture inside it (its address in it, the same layout: rows of the same stride, or tiles of
// as many a row from a tile row), at the internal resolution when it is drawn at one. A depth surface: at its
// address, in its layout, its kept image's samples (at a scale, the middle samples guest memory gets) and the
// background past them; also while pending scenes store that image (the GPU runs them first: keyed by the generation
// their write-backs make). `only`: that colour surface, which a pending scene draws (not compared). `depth_only`:
// depth surfaces alone (a texture over memory pending scenes write: a colour surface's is `only`'s).
CachedTexture* converted_texture(Renderer& r, const TextureInfo& info, u64 words_key, u64* key, Target* only = nullptr,
                                 bool depth_only = false) {
    if (!r.converter.ready() || info.levels != 1 || !info.address || info.gamma) return nullptr;
    const u32 code = ((info.base_format >> 24) & 0x1F) | ((info.base_format >> 31) << 7);
    const bool hdr = code == 0x9A, d16 = code == 0x09, d32 = code == 0x12 || code == 0x13;
    if (!hdr && !d16 && !d32) return nullptr;
    if (hdr && depth_only) return nullptr;
    const bool linear = info.type == texture_type::kLinear || info.type == texture_type::kLinearStrided;
    const bool tiled = info.type == texture_type::kTiled;
    if (!linear && !tiled) return nullptr;
    const u32 tw = info.width, th = info.height;
    auto mix = [&](const void* p, u64 generation, u64 extra) {
        return (words_key * 0x9E3779B97F4A7C15ull) ^ (reinterpret_cast<u64>(p) * 0xC2B2AE3D27D4EB4Full) ^
               (generation * 0x165667B19E3779F9ull) ^ (extra * 0x27D4EB2F165667C5ull) ^ 0x636F6E7665727465ull;
    };
    auto cached = [&]() -> CachedTexture* {
        auto it = r.textures.find(*key);
        if (it == r.textures.end()) return nullptr;
        it->second.used = r.scenes;
        return &it->second;
    };
    if (hdr) {
        for (auto& t : r.targets) {
            if (only && t.get() != only) continue;
            ColorFormat cf;
            if (!color_format(t->format, cf) || cf.packing != Packing::U2F10F10F10) continue;
            if (info.address < t->address || info.address - t->address >= t->layout.span()) continue;
            const u32 delta = info.address - t->address;
            const SurfaceLayout& l = t->layout;
            const u32 tile_row = (l.stride + 31) / 32 * 1024 * 32 * 4;  // (bytes of a row of tiles)
            u32 ox = 0, oy = 0;
            if (linear && l.kind == SurfaceLayout::kLinear && info.stride == l.stride * 4 && delta % 4 == 0) {
                oy = delta / (l.stride * 4), ox = delta % (l.stride * 4) / 4;
            } else if (tiled && l.kind == SurfaceLayout::kTiled && (l.stride + 31) / 32 == (tw + 31) / 32 &&
                       delta % tile_row == 0) {
                oy = delta / tile_row * 32;
            } else {
                continue;
            }
            if (u64{ox} + tw > t->width || u64{oy} + th > t->height) continue;
            if (!only) {
                if (t->written.empty()) continue;
                const Changes changes = surface_changes(*t);
                if (changes.all || changes.pixels) continue;
            }
            const auto& dec = decode_for(r, info, TargetSource::U2F10F10F10);
            if (!dec) return nullptr;
            *key = mix(t.get(), t->generation, u64{ox} << 32 | oy);
            if (CachedTexture* c = cached()) return c;
            const u32 s = t->scale > 1 && t->scaled.image ? t->scale : 1;
            Renderer::TargetConversion conv;
            conv.color = t.get();
            conv.pending = only != nullptr;
            conv.x = ox * s, conv.y = oy * s, conv.w = tw * s, conv.h = th * s;
            TargetTextureConstants& k = conv.k;
            k.width = k.a_width = tw * s, k.height = k.a_height = th * s;
            k.src_pitch = tw * s;
            k.source = static_cast<u32>(TargetSource::U2F10F10F10);
            return queue_conversion(r, info, *key, words_key, *dec, conv);
        }
        return nullptr;
    }
    if (only) return nullptr;
    // (a D32 float image: a D24 one's values are converted on storing)
    const VkFormat depth_format = r.device->caps().depth_stencil_format;
    if (depth_format != VK_FORMAT_D32_SFLOAT && depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) return nullptr;
    for (auto& kt : r.kept_depths) {
        DepthTarget* t = kt.get();
        if (t->address != info.address || t->image.layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
        DepthLayout l;
        if (!depth_layout(t->control & 0x03FFF000u, l) || l.packed) continue;
        if (d16 ? l.depth_bytes != 2 : (l.depth_bytes != 4 || (code == 0x12 && l.mask))) continue;
        const u32 bytes = l.depth_bytes;
        if (((t->control & kDsTiled) != 0) != tiled) continue;
        if (linear ? info.stride != t->stride * 32 * bytes : (tw + 31) / 32 != t->stride) continue;
        const u32 aw = nx::min(tw, t->width), ah = nx::min(th, t->height);
        const bool past = aw < tw || ah < th;
        if (past && !linear) continue;
        DepthStencilSurface s{};
        s.control = t->control, s.depth_data = t->address, s.stencil_data = t->stencil_address;
        s.background_control = t->stride << 16;
        const DepthRanges ranges = depth_ranges(s, l, t->width, t->height);
        // (pending scenes that store the image: the last of them to write there or to draw with the image stores it,
        // so the image is what guest memory will hold once they are written back, each a generation; the GPU runs
        // them before this scene's conversion. Else what guest memory holds now, if no pending scene changes it.)
        u64 stores = 0;
        if (const int pk = pending_kept_depth(r, t, ranges); pk == 1) {
            for (const Renderer::Pending& p : r.pending) stores += p.kept_depth == t && p.stores_kept_depth;
        } else if (pk == 0 || t->written.empty() || !kept_holds(*t, ranges)) {
            continue;
        }
        // (what pending scenes write of the texture's memory: only those stores, into the surface's depth and stencil)
        bool others = false;
        const u32 tsize = texture_size(info);
        for (const Renderer::Pending& p : r.pending)
            for (const auto& [at, n] : p.writes) {
                if (!(info.address < at + n && at < info.address + tsize)) continue;
                const bool within = (at >= ranges.depth && u64{at} + n <= u64{ranges.depth} + ranges.depth_bytes) ||
                                    (ranges.stencil_bytes && at >= ranges.stencil &&
                                     u64{at} + n <= u64{ranges.stencil} + ranges.stencil_bytes);
                others |= !(p.kept_depth == t && p.stores_kept_depth && within);
            }
        if (others) continue;
        const auto& dec = decode_for(r, info, d16 ? TargetSource::Depth16 : TargetSource::Depth32);
        if (!dec) return nullptr;
        if (past) {
            // (guest memory past what the scenes store, row by row: as it was taken, else taken again)
            const u32 size = texture_size(info);
            if (!memory().valid(info.address, size)) return nullptr;
            const u8* guest = memory().host(info.address);
            const usize row = info.stride;
            const u8* had = t->background_bytes.data();
            bool same = t->background_bytes.size() == size;
            // (watched: nothing written there since it was compared or taken, but the stores of this surface, which
            // write_back lets keep the watch)
            const bool watched = same && unwritten(t->background_watch, info.address, size);
            if (!watched || write_watch_mode() == 2) {
                watch_range(t->background_watch, info.address, size);  // (before it is compared or taken)
                for (u32 y = 0; same && y < ah; ++y) {
                    const usize from = y * row + usize{aw} * bytes, to = nx::min<usize>((y + 1) * row, size);
                    same = to <= from || std::memcmp(guest + from, had + from, to - from) == 0;
                }
                if (same && usize{ah} * row < size)
                    same = std::memcmp(guest + ah * row, had + ah * row, size - ah * row) == 0;
                if (watched && !same) unseen_write("a depth texture's background", info.address, size);
            }
            if (!same) {
                t->background_bytes.assign(guest, guest + size);
                t->background_uploaded = false;
                ++t->background_version;
            }
        }
        *key = mix(t, t->generation + stores, past ? t->background_version + 1 : 0);
        if (CachedTexture* c = cached()) return c;
        Renderer::TargetConversion conv;
        conv.depth = t;
        conv.pending = stores > 0;
        conv.generation = t->generation + stores;
        const u32 sc = t->scale;
        conv.w = aw * sc, conv.h = ah;
        conv.step = sc, conv.first = sc / 2;
        TargetTextureConstants& k = conv.k;
        k.width = tw, k.height = th, k.a_width = aw, k.a_height = ah;
        k.src_pitch = aw * sc, k.src_step = sc, k.src_first = sc / 2;
        k.source = static_cast<u32>(d16 ? TargetSource::Depth16 : TargetSource::Depth32);
        k.bg_tiled = 0, k.bg_pitch = linear ? info.stride / bytes : 0, k.bg_bits = bytes * 8;
        return queue_conversion(r, info, *key, words_key, *dec, conv);
    }
    return nullptr;
}

// A texture over the surface a pending scene draws: made from its image (the GPU runs the scenes in order), keyed
// by the surface's generation (guest memory does not hold that scene's pixels yet).
CachedTexture* texture_from_pending_target(Renderer& r, const TextureInfo& info, u64 words_key, u64* key) {
    for (const Renderer::Pending& p : r.pending) {
        Target* t = p.target;
        u32 ox = 0, oy = 0;
        if (!t || (t->address != info.address && !texture_in_surface(info, *t, ox, oy))) continue;
        if (CachedTexture* c = converted_texture(r, info, words_key, key, t)) return c;
        *key = (words_key * 0x9E3779B97F4A7C15ull) ^ (t->generation * 0xC2B2AE3D27D4EB4Full) ^ 0x70656E64696E67ull;
        if (auto it = r.textures.find(*key); it != r.textures.end()) {
            it->second.used = r.scenes;
            return &it->second;
        }
        return texture_from_target(r, info, *key, words_key, t);
    }
    return nullptr;
}

// The conversions converted_texture asked for: the backgrounds uploaded, the images' texels copied into the work
// buffers, the shader run, its texels copied into the textures.
void record_target_conversions(Renderer& r, VkCommandBuffer cmd) {
    nx::vector<Renderer::TargetConversion*> live;
    for (Renderer::TargetConversion& c : r.target_conversions)
        if (r.textures.find(c.key) != r.textures.end()) live.push_back(&c);
    if (live.empty()) return;
    auto barrier = [&](VkPipelineStageFlags from, VkAccessFlags from_access, VkPipelineStageFlags to, VkAccessFlags to_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = from_access;
        mb.dstAccessMask = to_access;
        vkCmdPipelineBarrier(cmd, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    // (a kept depth image pending scenes store: their writes before its copy, whatever layout they left it in)
    bool from_pending = false;
    for (Renderer::TargetConversion* c : live) from_pending |= c->depth && c->pending;
    if (from_pending)
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    // (a background earlier scenes' conversions may still read, written over)
    bool uploads = false;
    for (Renderer::TargetConversion* c : live) uploads |= c->background_staging.buffer != VK_NULL_HANDLE;
    if (uploads) barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    for (Renderer::TargetConversion* c : live) {
        if (c->background_staging.buffer) {
            const VkDeviceSize bytes = (c->depth->background_bytes.size() + 3) & ~VkDeviceSize{3};
            const VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(cmd, c->background_staging.buffer, c->depth->background.buffer, 1, &region);
        }
        if (c->depth) {
            vk::Image& from = c->depth->image;
            const VkImageLayout was = from.layout;
            vk::transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            nx::vector<VkBufferImageCopy> rows;
            if (c->step == 1) {
                rows.push_back({0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {c->w, c->h, 1}});
            } else {
                // (the rows of middle samples, as the write-back takes them)
                for (u32 y = 0; y < c->h; ++y)
                    rows.push_back({VkDeviceSize{y} * c->w * 4, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                    {0, static_cast<i32>(y * c->step + c->first), 0}, {c->w, 1, 1}});
            }
            vkCmdCopyImageToBuffer(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->work.buffer,
                                   static_cast<u32>(rows.size()), rows.data());
            vk::transition(cmd, from, was);  // (as the scene's own use of it expects it)
        } else if (!c->from_image) {
            vk::Image& from = c->color->scale > 1 && c->color->scaled.image ? c->color->scaled : c->color->image;
            vk::transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                           {static_cast<i32>(c->x), static_cast<i32>(c->y), 0}, {c->w, c->h, 1}};
            vkCmdCopyImageToBuffer(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->work.buffer, 1, &region);
        }
    }
    // (HDR surfaces' from their images: the image read by the shader, the texture its storage image. A transition of
    // an image already in SHADER_READ_ONLY adds no barrier: the copies' barrier (record_target_copies) orders a
    // pending scene's drawing before these reads too)
    for (Renderer::TargetConversion* c : live) {
        if (!c->from_image) continue;
        vk::Image& from = c->color->scale > 1 && c->color->scaled.image ? c->color->scaled : c->color->image;
        vk::Image& to = r.textures.find(c->key)->second.image;
        vk::transition(cmd, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_GENERAL);
        write_target_image_set(*r.device, r.converter, c->set, from.view, to.view);
    }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    for (Renderer::TargetConversion* c : live) {
        if (!c->from_image) {
            record_target_conversion(cmd, r.converter, c->set, c->k);
            continue;
        }
        TargetImageConstants k;
        k.width = c->k.width, k.height = c->k.height, k.x0 = c->x, k.y0 = c->y;
        k.select = c->k.select, k.alpha_low = c->k.alpha_low;
        k.alpha1 = c->k.alpha1, k.alpha2 = c->k.alpha2, k.alpha3 = c->k.alpha3;
        record_target_image_conversion(cmd, r.converter, c->set, k);
    }
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    for (Renderer::TargetConversion* c : live) {
        vk::Image& to = r.textures.find(c->key)->second.image;
        if (c->from_image) {
            if (c->verify.buffer) {
                vk::transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const VkBufferImageCopy back{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {c->k.width, c->k.height, 1}};
                vkCmdCopyImageToBuffer(cmd, to.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->verify.buffer, 1, &back);
            }
            vk::transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            continue;
        }
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkBufferImageCopy region{VkDeviceSize{c->k.out} * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                       {c->k.width, c->k.height, 1}};
        vkCmdCopyBufferToImage(cmd, c->work.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (c->verify.buffer) {
            const VkBufferCopy back{VkDeviceSize{c->k.out} * 4, 0, c->verify_bytes};
            vkCmdCopyBuffer(cmd, c->work.buffer, c->verify.buffer, 1, &back);  // (seen by the host after the scene's barrier)
        }
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// The copies texture_from_target asked for, and the new textures' uploads, recorded before the scene's draws.
void record_target_copies(Renderer& r, VkCommandBuffer cmd) {
    if (!r.target_copies.empty() || !r.target_conversions.empty()) {
        // (a surface a pending scene draws, read here: its drawing (and its write-back's copy) before these copies.
        // Its image may already be in TRANSFER_SRC, where a transition adds no barrier; the scenes are separate
        // submissions, whose order alone does not order their commands)
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    bool converted = false;
    for (Renderer::TextureUpload& u : r.texture_uploads) {
        if (!u.yuv_set || r.textures.find(u.key) == r.textures.end()) continue;
        if (!converted) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.yuv.pipeline);
        converted = true;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.yuv.layout, 0, 1, &u.yuv_set, 0, nullptr);
        vkCmdPushConstants(cmd, r.yuv.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof u.yuv, &u.yuv);
        vkCmdDispatch(cmd, (u.yuv.width + 7) / 8, (u.yuv.height + 7) / 8, 1);
    }
    if (converted) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    for (Renderer::TextureUpload& u : r.texture_uploads) {
        auto it = r.textures.find(u.key);
        if (it == r.textures.end()) continue;
        vk::Image& to = it->second.image;
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, u.staging.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(u.regions.size()), u.regions.data());
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (Renderer::TargetCopy& c : r.target_copies) {
        auto it = r.textures.find(c.key);
        if (it == r.textures.end()) continue;
        vk::Image& to = it->second.image;
        vk::Image& from = c.scale > 1 ? c.from->scaled : c.from->image;
        vk::transition(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {static_cast<i32>(c.x * c.scale), static_cast<i32>(c.y * c.scale), 0};
        region.extent = {c.width * c.scale, c.height * c.scale, 1};
        vkCmdCopyImage(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (c.strip_w) {
            VkBufferImageCopy b{};
            b.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.imageOffset = {static_cast<i32>(c.strip_x * c.scale), 0, 0};
            b.imageExtent = {c.strip_w * c.scale, c.height * c.scale, 1};
            vkCmdCopyBufferToImage(cmd, c.strip.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b);
        }
        vk::transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    record_target_conversions(r, cmd);
}

// The copies' buffers given back once the scene is done; `recorded` false (the scene ended before its commands):
// the textures they were to fill are dropped from the cache, never sampled.
void finish_target_copies(Renderer& r, bool recorded) {
    if (!recorded) {
        for (Renderer::TextureUpload& u : r.texture_uploads) {
            give_upload(r, u);
            if (auto it = r.textures.find(u.key); it != r.textures.end()) {
                r.texture_bytes -= it->second.bytes;
                retire_image(r, it->second.image);
                r.textures.erase(it);
            }
        }
        r.texture_uploads.clear();
    }
    for (Renderer::TargetCopy& c : r.target_copies) {
        if (c.strip.buffer) vk::destroy_buffer(*r.device, c.strip);
        if (recorded) continue;
        if (auto it = r.textures.find(c.key); it != r.textures.end()) {
            r.texture_bytes -= it->second.bytes;
            retire_image(r, it->second.image);
            r.textures.erase(it);
        }
    }
    r.target_copies.clear();
    // (conversions not recorded: a recorded scene's go with its finish)
    for (Renderer::TargetConversion& c : r.target_conversions) {
        if (c.background_staging.buffer) c.depth->background_uploaded = false;
        if (c.retired_background.buffer) {
            // (the larger background never written: the one earlier scenes read stays)
            vk::destroy_buffer(*r.device, c.depth->background);
            c.depth->background = c.retired_background;
            c.retired_background = {};
        }
        release_conversion(r, c);
        if (auto it = r.textures.find(c.key); it != r.textures.end()) {
            r.texture_bytes -= it->second.bytes;
            retire_image(r, it->second.image);
            r.textures.erase(it);
        }
    }
    r.target_conversions.clear();
}

// A texture to decode (texture_for, collecting): decoded on several threads, then made (make_texture).
struct TextureMiss {
    TextureInfo info;
    u32 size = 0;
    u64 key = 0, words_key = 0;
    u64 contents = 0;  // (texture_for's: what the texels decoded ahead are found by)
    Result<Texels> texels = Texels{};
    bool ahead = false;  // (its texels decoded ahead: taken)
};


CachedTexture* make_texture(Renderer& r, const TextureInfo& info, u32 size, u64 key, u64 words_key, Result<Texels>* given);

// The texture whose control words these are, decoded and uploaded once per content (cached by the words and a hash
// of the texels and palette). `seen` remembers, for a scene, each set of words' content key: a texture used by many
// draws is hashed once a scene. `misses`: one whose texels are to be decoded is added there instead (none returned;
// `seen` has its key, for when it is made).
CachedTexture* texture_for(Renderer& r, const TextureWords& words, nx::hash_map<u64, u64>& seen,
                           nx::vector<TextureMiss>* misses = nullptr) {
    ++g_profile.lookups;
    nx::fnv1a64 wh;
    for (u32 w : words.words) wh.combine(w);
    if (auto it = seen.find(wh.value()); it != seen.end()) {
        auto found = r.textures.find(it->second);
        return found != r.textures.end() ? &found->second : nullptr;
    }
    const TextureInfo info = decode_texture(words.words);
    if (!info.address) return nullptr;
    const u32 size = texture_size(info);
    if (!size || !memory().valid(info.address, size)) return nullptr;
    if (pending_writes(r, info.address, size)) {
        u64 pending_key = 0;
        if (CachedTexture* c = texture_from_pending_target(r, info, wh.value(), &pending_key)) {
            seen[wh.value()] = pending_key;
            if (g_profile.on) ++g_profile.from_targets;
            return c;
        }
        // (a depth surface's texture, made from its kept image that pending scenes store)
        if (u64 converted_key = 0; CachedTexture* c = converted_texture(r, info, wh.value(), &converted_key, nullptr, true)) {
            seen[wh.value()] = converted_key;
            return c;
        }
        g_finish_why = 7, finish_pending(r, true);
    }
    // (a render target's texels made on the GPU, before anything is hashed)
    if (u64 converted_key = 0; CachedTexture* c = converted_texture(r, info, wh.value(), &converted_key)) {
        seen[wh.value()] = converted_key;
        return c;
    }
    // (a colour surface's image, the texture exactly over it, unchanged since its write-back: copied from the image,
    // keyed by the surface's generation, before guest memory is read: a deferred write-back stays unwritten)
    for (auto& t : r.targets) {
        u32 ox = 0, oy = 0;
        if (t->written.empty() || info.levels != 1) continue;
        if (!texture_in_surface(info, *t, ox, oy)) continue;
        if (!texture_reads_layout(info, t->layout) || !texels_as_image(info, t->image.desc.format, srgb_textures(*r.device)))
            break;
        const Changes changes = surface_changes(*t);
        if (changes.all || changes.pixels) break;
        const u64 key = (wh.value() * 0x9E3779B97F4A7C15ull) ^ (reinterpret_cast<u64>(t.get()) * 0xC2B2AE3D27D4EB4Full) ^
                        (t->generation * 0x165667B19E3779F9ull) ^ 0x696D616765ull;
        seen[wh.value()] = key;
        if (auto it = r.textures.find(key); it != r.textures.end()) {
            it->second.used = r.scenes;
            return &it->second;
        }
        if (CachedTexture* c = texture_from_target(r, info, key, wh.value(), t.get())) {
            if (g_profile.on) ++g_profile.from_targets;
            return c;
        }
        seen.erase(wh.value());
        break;
    }
    const u32 palette = info.palette && memory().valid(info.palette, 1024) ? info.palette : 0;
    auto writes = [&] { return memory().writes(info.address, size) + (palette ? memory().writes(palette, 1024) : 0); };
    Renderer::ContentMemo& memo = r.content_memo[wh.value()];
    u64 key = 0, contents = 0;
    if (memo.watched && memo.address == info.address && memo.size == size && memo.palette == palette &&
        writes() == memo.writes) {
        key = memo.key;
        contents = memo.contents;
        memo.strikes = 0;
        if (g_profile.on) ++g_profile.unchanged;
    } else {
        const u64 hash_start = g_profile.on ? nx::os::ticks_ns() : 0;
        if (memo.watched && ++memo.strikes >= 3) memo.hash_until = r.scenes + 256, memo.strikes = 0;
        // (watched before it is hashed: a write while it is, is seen next time)
        const bool watched = !is_yuv420(info) && memo.hash_until <= r.scenes && memory().watch_writes(info.address, size) &&
                             (!palette || memory().watch_writes(palette, 1024));
        const u64 seen_writes = watched ? writes() : 0;
        contents = texture_contents(info, size, palette);
        key = content_hash(reinterpret_cast<const u8*>(words.words), sizeof words.words, contents);
        memo.key = key, memo.contents = contents, memo.writes = seen_writes, memo.watched = watched;
        memo.address = info.address, memo.size = size, memo.palette = palette;
        if (r.content_memo.size() > 8192) r.content_memo.clear();  // (the words of textures long gone)
        if (g_profile.on) ++g_profile.hashed, g_profile.hash_ns += nx::os::ticks_ns() - hash_start;
    }
    seen[wh.value()] = key;
    if (auto it = r.textures.find(key); it != r.textures.end()) {
        it->second.used = r.scenes;
        return &it->second;
    }
    if (r.failed_textures.contains(key)) return nullptr;
    if (CachedTexture* c = texture_from_target(r, info, key, wh.value())) {
        if (g_profile.on) ++g_profile.from_targets;
        return c;
    }
    if (misses && !(r.yuv.pipeline && is_yuv420(info))) {
        bool listed = false;
        for (const TextureMiss& m : *misses) listed |= m.key == key;
        if (!listed) misses->push_back(TextureMiss{info, size, key, wh.value(), contents});
        return nullptr;
    }
    return make_texture(r, info, size, key, wh.value(), nullptr);
}

// A texture not cached, made: its texels decoded here, unless `given` (or a movie frame's planes read for the GPU to
// convert), an image made (or the one these words showed before reused) and the upload queued.
CachedTexture* make_texture(Renderer& r, const TextureInfo& info, u32 size, u64 key, u64 words_key, Result<Texels>* given) {
    const u64 miss_start = g_profile.on ? nx::os::ticks_ns() : 0;
    struct MissTime {
        u64 start;
        u32 bytes;
        const TextureInfo& info;
        u64 decoded = 0;
        ~MissTime() {
            if (!g_profile.on) return;
            const u64 took = nx::os::ticks_ns() - start;
            ++g_profile.misses, g_profile.miss_bytes += bytes, g_profile.miss_ns += took;
            // (a texture that took a while: which)
            if (took >= 2'000'000)
                std::fprintf(stderr, "gxm profile: a slow new texture, %.1f ms (%.1f decoding): format %08x type %u, %u x %u, "
                             "%u levels, %u bytes\n", static_cast<f64>(took) / 1e6,
                             decoded ? static_cast<f64>(decoded - start) / 1e6 : 0.0, info.base_format, info.type, info.width,
                             info.height, info.levels, bytes);
        }
    } miss_time{miss_start, size, info};
    if (g_profile.on && g_profile.misses % 50 == 0)
        std::fprintf(stderr, "gxm profile: new texture at %08x: format %08x type %u, %u x %u, %u levels, %u bytes\n",
                     info.address, info.base_format, info.type, info.width, info.height, info.levels, size);
    auto read_guest = read_guest_memory;
    // (a movie's frame: its planes, for the GPU to convert; else the texels, decoded here)
    std::optional<Yuv420> yuv;
    if (!given && r.yuv.pipeline && is_yuv420(info)) {
        if (auto planes = read_yuv420(info, read_guest)) yuv = std::move(*planes);
    }
    Result<Texels> texels = Texels{};
    if (given) {
        texels = std::move(*given);
    } else if (yuv) {
        texels->width = yuv->width, texels->height = yuv->height;
    } else {
        texels = read_texels(info, read_guest, r.device->caps().texture_bc, true, srgb_textures(*r.device));
    }
    if (!texels) {
        nx::logw(logs::gxm, "{} (format 0x{:08x}, type {})", texels.error().message, info.base_format, info.type);
        r.failed_textures[key] = texels.error().message;
        return nullptr;
    }
    vk::ImageDesc d;
    // (texels of a gamma mode kept encoded, Texels::srgb: the sRGB formats, which decode R, G and B as they are sampled)
    const bool srgb = texels->srgb;
    d.format = texels->bc == 1   ? (srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
               : texels->bc == 2 ? (srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK)
               : texels->bc == 3 ? (srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK)
               : texels->single      ? VK_FORMAT_R32_SFLOAT
               : texels->bytes == 16 ? VK_FORMAT_R32G32B32A32_SFLOAT
               : texels->bytes == 8 ? VK_FORMAT_R16G16B16A16_SFLOAT
               : srgb               ? VK_FORMAT_R8G8B8A8_SRGB
                                    : VK_FORMAT_R8G8B8A8_UNORM;
    d.width = texels->width;
    d.height = texels->height;
    d.mips = texels->levels;
    d.layers = texels->faces;
    d.cube = texels->faces == 6;
    if (texels->single) {
        // (the float where the swizzle puts it, the constants elsewhere)
        constexpr VkComponentSwizzle of[3] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
        d.components = {of[texels->rgba_of[0]], of[texels->rgba_of[1]], of[texels->rgba_of[2]], of[texels->rgba_of[3]]};
    }
    const u64 decoded_at = g_profile.on ? nx::os::ticks_ns() : 0;
    if (g_profile.on) g_profile.decode_ns += decoded_at - miss_start, miss_time.decoded = decoded_at;
    // (the texels' upload, or the planes' conversion: decoded here when no set is free for it)
    auto queue = [&]() -> Result<void> {
        if (yuv) {
            auto queued = queue_yuv420(r, key, *yuv);
            if (!queued) return nx::err(queued.error());
            if (*queued) return {};
            auto decoded = read_texels(info, read_guest, r.device->caps().texture_bc, true, srgb_textures(*r.device));
            if (!decoded) return nx::err(decoded.error());
            *texels = std::move(*decoded);
        }
        return queue_texels(r, key, *texels);
    };
    const u64 texel_bytes = u64{d.width} * d.height * 4;  // (both forms: RGBA8, one level)
    // (the image these words showed before, when no draw of this scene samples it and it has this size and format)
    if (auto was = r.by_words.find(words_key); was != r.by_words.end()) {
        auto old = r.textures.find(was->second);
        if (old != r.textures.end() && old->second.used != r.scenes) {
            const vk::ImageDesc& od = old->second.image.desc;
            if (od.format == d.format && od.width == d.width && od.height == d.height && od.mips == d.mips &&
                std::memcmp(&od.components, &d.components, sizeof d.components) == 0 &&
                od.layers == d.layers && od.cube == d.cube) {
                CachedTexture c = old->second;
                r.textures.erase(old);
                if (!queue()) {
                    r.texture_bytes -= c.bytes;
                    retire_image(r, c.image);
                    r.by_words.erase(words_key);
                    return nullptr;
                }
                if (g_profile.on) g_profile.upload_ns += nx::os::ticks_ns() - decoded_at;
                c.used = r.scenes;
                r.by_words[words_key] = key;
                auto [it, added] = r.textures.emplace(key, c);
                return &it->second;
            }
        }
    }
    auto image = vk::create_image(*r.device, d);
    const u64 created_at = g_profile.on ? nx::os::ticks_ns() : 0;
    if (g_profile.on) g_profile.create_ns += created_at - decoded_at;
    if (!image) return nullptr;
    if (!queue()) {
        vk::destroy_image(*r.device, *image);
        return nullptr;
    }
    if (g_profile.on) g_profile.upload_ns += nx::os::ticks_ns() - created_at;
    CachedTexture c;
    c.image = *image;
    c.sampler = texture_sampler(r, info, d.format, texels->levels);
    c.used = r.scenes;
    c.bytes = yuv ? texel_bytes : texels->data.size();
    r.texture_bytes += c.bytes;
    r.by_words[words_key] = key;
    auto [it, added] = r.textures.emplace(key, c);
    return &it->second;
}

// The cached textures no scene has used for a while, and the least recently used past the budget, let go (at a
// scene's start; the scenes before may still be on the GPU: retire_image keeps what they sample until they are done).
u64 texture_budget(Renderer& r) {
    if (r.texture_budget) return r.texture_budget;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(r.device->physical(), &mp);
    VkDeviceSize heap = 0;
    for (u32 i = 0; i < mp.memoryHeapCount; ++i)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = nx::max(heap, mp.memoryHeaps[i].size);
    r.texture_budget = nx::clamp<u64>(heap / 8, kTextureBudgetLeast, kTextureBudgetMost);
    nx::logi(logs::gxm, "texture cache: {} MiB at most", r.texture_budget >> 20);
    return r.texture_budget;
}

void evict_textures(Renderer& r) {
    const u64 budget = texture_budget(r);
    if (r.scenes % 32 != 0 && r.texture_bytes <= budget) return;
    nx::vector<std::pair<u64, u64>> by_age;  // (last used, key)
    for (const auto& [key, t] : r.textures) by_age.push_back({t.used, key});
    std::sort(by_age.begin(), by_age.end());
    for (const auto& [used, key] : by_age) {
        const u64 kept = r.texture_bytes > kTextureKeep ? kTextureUnusedScenes : kTextureKeptScenes;
        if (used + kept >= r.scenes && r.texture_bytes <= budget) break;
        auto it = r.textures.find(key);
        r.texture_bytes -= it->second.bytes;
        retire_image(r, it->second.image);
        r.textures.erase(it);
    }
}

// Whether a fragment program's secondary program reads per-tile special registers (pixelX, pixelY, tileXY): it runs
// in the fragment shader then, in each instance (translate_fragment).
bool tile_secondary_scan(const Program& p) {
    for (const usse::Decoded& d : p.secondary) {
        if (!d.executed) continue;
        for (u32 k = 0; k < d.in.src_count; ++k) {
            const usse::Operand& o = d.in.src[k];
            if (o.bank == usse::Bank::Special && (o.number == 87 || o.number == 88 || o.number == 107)) return true;
        }
    }
    return false;
}

// Whether a program stores to memory (its buffers are copied back after the scene).
bool stores_scan(const Program& p) {
    for (const usse::Decoded& d : p.primary)
        if (d.executed && d.in.op == usse::Op::Store) return true;
    for (const usse::Decoded& d : p.secondary)
        if (d.executed && d.in.op == usse::Op::Store) return true;
    return false;
}
// (both scans once a program: draws ask for every draw)
bool stores(const Program& p) {
    if (p.stores < 0) const_cast<Program&>(p).stores = stores_scan(p) ? 1 : 0;
    return p.stores == 1;
}
bool tile_secondary(const Program& p) {
    if (p.tile_secondary < 0) const_cast<Program&>(p).tile_secondary = tile_secondary_scan(p) ? 1 : 0;
    return p.tile_secondary == 1;
}

// The words of a uniform buffer a program reads (from its parameters in that buffer).
u32 buffer_words(const usse::Gxp& g, u32 index) {
    u32 words = 0;
    for (const usse::Parameter& p : g.parameters) {
        if (p.container != index || p.category != usse::ParameterCategory::Uniform) continue;
        const u32 per = (p.components + 1u) & ~1u;  // (vectors take 64-bit aligned room: an upper bound)
        words = nx::max(words, p.resource + nx::max<u32>(p.array_size, 1) * nx::max<u32>(per, 1) + 4);
    }
    return words;
}

// The bytes of a uniform buffer: the size the program declares for it (its UniformBuffer parameter), else an upper
// bound from the uniforms it reads there; at least the words it keeps in secondary attributes (its container).
u32 buffer_bytes(const usse::Gxp& g, u32 index) {
    u32 bytes = 0;
    for (const usse::Parameter& p : g.parameters)
        if (p.category == usse::ParameterCategory::UniformBuffer && p.resource == index && p.array_size) bytes = p.array_size;
    if (!bytes) bytes = buffer_words(g, index) * 4;
    if (const usse::Container* c = g.container(static_cast<u16>(index))) bytes = nx::max<u32>(bytes, u32{c->size} * 4);
    return bytes;
}

// The bytes of [address, address + bytes) that are guest memory, from the start (in pages past the first): a buffer
// a program declares larger than what follows it in memory (Killzone's depth reconstruction reads the rows of a colour
// surface through uniform buffers declared 17 MB long) is copied as far as memory goes, not left out.
u32 mapped_prefix(u32 address, u32 bytes) {
    if (!bytes || memory().valid(address, bytes)) return bytes;
    constexpr u32 kPage = 4096;
    const u32 first = nx::min(bytes, kPage - (address & (kPage - 1)));
    if (!memory().valid(address, first)) return 0;
    u32 lo = 0, hi = (bytes - first) / kPage;  // (whole pages past the first known mapped / not yet known)
    while (lo < hi) {
        const u32 mid = (lo + hi + 1) / 2;
        if (memory().valid(address, first + mid * kPage)) lo = mid;
        else hi = mid - 1;
    }
    return first + lo * kPage;
}

void host_copy(nx::vector<u8>& out, u32 address, u32 bytes) {
    depend(address, bytes);
    const usize at = out.size();
    out.resize(at + ((bytes + 15) & ~15u), 0);
    if (address && bytes && memory().valid(address, bytes)) std::memcpy(out.data() + at, memory().host(address), bytes);
}

// Depth and stencil samples (the depth words, then the stencil bytes) of a w x h surface at the internal
// resolution: each repeated over a scale x scale block; and back, the middle sample of each block.
// (a row of samples for each row of the surface, each sample s times, depth words then stencil bytes: the copy into
// the image repeats each row s times)
nx::vector<u8> scale_up_samples(const nx::vector<u8>& in, u32 w, u32 h, u32 s) {
    const usize n = usize{w} * h, rows = n * s;
    nx::vector<u8> out(rows * 5);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w * s; ++x) {
            const usize from = usize{y} * w + x / s, to = usize{y} * w * s + x;
            std::memcpy(out.data() + to * 4, in.data() + from * 4, 4);
            out[rows * 4 + to] = in[n * 4 + from];
        }
    return out;
}

// (from the rows the scene stored: row y*s + s/2 of the samples for each row of the surface, depth words then
// stencil bytes; the middle sample of each s x s block)
nx::vector<u8> scale_down_samples(const u8* in, u32 w, u32 h, u32 s) {
    const usize n = usize{w} * h, rows = n * s;
    nx::vector<u8> out(n * 5);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const usize from = usize{y} * w * s + x * s + s / 2, to = usize{y} * w + x;
            std::memcpy(out.data() + to * 4, in + from * 4, 4);
            out[n * 4 + to] = in[rows * 4 + from];
        }
    return out;
}

struct DrawPlan {
    const Draw* draw = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // (its rasterization, depth and stencil state, for a linked pipeline's dynamic state: set_dynamic_state reads
    // only those fields)
    vk::GraphicsPipelineDesc state;
    u32 missing = 0;  // (a pipeline not made yet: 1 + its index in the scene's list of them)
    VkPipeline vertex_secondary = VK_NULL_HANDLE, fragment_secondary = VK_NULL_HANDLE;
    nx::vector<std::pair<u32, VkDeviceSize>> streams;  // (binding, offset in the vertex buffer)
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    DrawConstants constants;
    VkRect2D scissor{};
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

// PSVRECORE_GXM_TRACE=<first scene>[-<last scene>] (or 1: all): each scene logged before it is rendered (its surfaces
// and draws: their programs, primitives, textures, depth and stencil state), for finding what a frame draws. Scene
// numbers vary between runs: *<colour address>[:<count>] traces <count> scenes (default 16) from the first that draws
// to that colour surface.
struct Trace {
    u64 first = 0, last = 0;
    u32 surface = 0;
    bool on = false;
};
const Trace& trace() {
    static const Trace t = [] {
        Trace t;
        const char* spec = std::getenv("PSVRECORE_GXM_TRACE");
        if (!spec || !*spec) return t;
        char* end = nullptr;
        t.on = true;
        if (*spec == '*') {
            t.surface = static_cast<u32>(std::strtoul(spec + 1, &end, 0));
            t.last = *end == ':' ? std::strtoull(end + 1, nullptr, 0) : 16;  // (a count until the surface is seen)
            return t;
        }
        t.first = std::strtoull(spec, &end, 0);
        t.last = *end == '-' ? std::strtoull(end + 1, nullptr, 0) : t.first;
        if (t.first <= 1 && *end != '-') t.first = 0, t.last = ~u64{0};
        return t;
    }();
    return t;
}
// Whether this scene is traced (the first scene drawing to PSVRECORE_GXM_TRACE's surface starts the count).
bool traced(u64 number, const Scene& scene) {
    const Trace& t = trace();
    if (!t.on) return false;
    if (!t.surface) return number >= t.first && number <= t.last;
    static u64 start = 0;
    if (!start && scene.has_color && scene.color.data == t.surface) start = number + 1;
    return start && number + 1 - start < t.last;
}

// (PSVRECORE_GXM_TRACE_PROGRAMS=<folder>: a traced program's GXP written there, for psvusse)
void write_traced_program(const Program* p) {
    const char* folder = std::getenv("PSVRECORE_GXM_TRACE_PROGRAMS");
    if (!folder || !*folder || !p || !p->gxp.size || !memory().valid(p->gxp_address, p->gxp.size)) return;
    const nx::string path = nx::format("{}/{:08x}.gxp", folder, p->gxp_address);
    (void)io::write_file(nx::fs::path_view(path), {memory().host(p->gxp_address), p->gxp.size});
}

// A program whose translation failed: which one (its GXP's address), and its GXP written to
// PSVRECORE_GXM_TRACE_PROGRAMS's folder when that is set, traced or not (for psvusse).
Result<nx::vector<u32>> failed_noted(const Program* p, Result<nx::vector<u32>> spirv) {
    if (!spirv && p) {
        nx::logw(logs::gxm, "program at 0x{:08x} (GXP 0x{:08x}) not translated", p->guest, p->gxp_address);
        write_traced_program(p);
    }
    return spirv;
}

void trace_scene(u64 number, const Scene& scene) {
    const ColorSurface& cs = scene.color;
    const DepthStencilSurface& ds = scene.depth;
    nx::logi(logs::gxm, "trace: scene {}: colour {} (0x{:08x}, format 0x{:08x}, {} x {}, stride {}, type {}, gamma {}), depth {} "
             "(0x{:08x}, stencil 0x{:08x}, control 0x{:08x}, background {}/0x{:x}), {} draws", number, scene.has_color,
             cs.data, cs.format, cs.size & 0xFFFF, cs.size >> 16, cs.stride, cs.modes & 3, (cs.modes >> 8) & 0xF,
             scene.has_depth, ds.depth_data,
             ds.stencil_data, ds.control, ds.background_depth, ds.background_control, scene.draws.size());
    for (usize i = 0; i < scene.draws.size(); ++i) {
        const Draw& d = scene.draws[i];
        nx::string textures;
        for (u32 u = 0; u < 16; ++u) {
            const TextureWords& w = d.fragment_textures[u];
            if (!w.words[0] && !w.words[1] && !w.words[2] && !w.words[3]) continue;
            const TextureInfo t = decode_texture(w.words);
            textures += nx::format(" t{}=0x{:08x}:{:08x}/type {}/{}x{}", u, t.address, t.base_format, t.type, t.width, t.height);
            if (t.levels > 1 || t.swizzle || t.gamma)
                textures += nx::format("/levels {} swizzle {} gamma {}", t.levels, t.swizzle, t.gamma);
        }
        nx::logi(logs::gxm, "trace:   draw {}: vertex 0x{:08x} fragment 0x{:08x} (blend {}, mask update {}), primitive {}, "
                 "{} indices x {}{}", i, d.vertex ? d.vertex->guest : 0, d.fragment ? d.fragment->guest : 0,
                 d.fragment && d.fragment->blend_enabled, d.fragment && d.fragment->mask_update, d.primitive,
                 d.index_count, d.instances, textures);
        {
            const DrawState& st = d.state;
            const StencilState& f = st.stencil[0];
            nx::logi(logs::gxm, "trace:     depth func 0x{:08x} write {}, stencil func 0x{:08x} ops 0x{:x}/0x{:x}/0x{:x} "
                     "ref {} masks 0x{:02x}/0x{:02x}, cull {}, fragment {}, region clip {} ({},{})-({},{})",
                     st.depth_func[0], st.depth_write[0], f.func, f.fail, f.depth_fail, f.pass, f.ref, f.compare_mask,
                     f.write_mask, st.cull, st.fragment_enabled[0], st.region_clip_mode, st.region_min[0],
                     st.region_min[1], st.region_max[0], st.region_max[1]);
        }
        // (each vertex program's attributes, the first time it is seen: its parameters' names, and the formats the
        // program was made with)
        static nx::hash_map<u32, bool> described;
        if (d.vertex && d.vertex->program && described.emplace(d.vertex->guest, true).second) {
            nx::string attributes;
            for (const VertexAttribute& a : d.vertex->attributes) {
                nx::string name;
                for (const usse::Parameter& p : d.vertex->program->gxp.parameters)
                    if (p.category == usse::ParameterCategory::Attribute && p.resource == a.reg) name = p.name;
                const u32 stride = a.stream < d.vertex->streams.size() ? d.vertex->streams[a.stream].stride : 0;
                attributes += nx::format(" {}(reg {}, stream {} stride {}, offset {}, format {}, {} components)", name, a.reg,
                                         a.stream, stride, a.offset, a.format, a.components);
            }
            nx::logi(logs::gxm, "trace:     vertex program 0x{:08x} (GXP at 0x{:08x}; fragment 0x{:08x}):{}", d.vertex->guest,
                     d.vertex->program->gxp_address, d.fragment && d.fragment->program ? d.fragment->program->gxp_address : 0, attributes);
            nx::string uniforms;
            for (const usse::Parameter& p : d.vertex->program->gxp.parameters)
                if (p.category != usse::ParameterCategory::Attribute)
                    uniforms += nx::format(" {}(category {}, type {}, {} components, array {}, resource {}, container {})", p.name,
                                           static_cast<u32>(p.category), static_cast<u32>(p.type), p.components, p.array_size,
                                           p.resource, p.container);
            nx::logi(logs::gxm, "trace:     its uniforms:{}", uniforms);
            write_traced_program(d.vertex->program);
        }
        // (each fragment program the first time it is seen, whichever vertex program it is drawn with: a vertex program
        // is often drawn with several)
        static nx::hash_map<u32, bool> described_fragments;
        if (d.fragment && d.fragment->program && described_fragments.emplace(d.fragment->guest, true).second) {
            const BlendInfo& b = d.fragment->blend;
            nx::logi(logs::gxm, "trace:     fragment program 0x{:08x} (GXP at 0x{:08x}), output format 0x{:08x}, blend {}: "
                     "mask 0x{:x}, colour func {} src {} dst {}, alpha func {} src {} dst {}", d.fragment->guest,
                     d.fragment->program->gxp_address, d.fragment->output_format, d.fragment->blend_enabled,
                     b.color_mask, b.color_func(), b.color_src(), b.color_dst(), b.alpha_func(), b.alpha_src(),
                     b.alpha_dst());
            write_traced_program(d.fragment->program);
        }
    }
}

}  // namespace


namespace {

// PSVRECORE_GXM_SKIP=<address>[,<address>...]: the draws whose vertex or fragment program is at one of these guest
// addresses (as PSVRECORE_GXM_TRACE gives them) left out, for finding which draw makes what.
const nx::vector<u32>& skipped_programs() {
    static const nx::vector<u32> list = [] {
        nx::vector<u32> out;
        const char* spec = std::getenv("PSVRECORE_GXM_SKIP");
        for (const char* p = spec; p && *p;) {
            char* end = nullptr;
            const u32 v = static_cast<u32>(std::strtoul(p, &end, 0));
            if (end == p) break;
            out.push_back(v);
            p = *end == ',' ? end + 1 : end;
        }
        return out;
    }();
    return list;
}

// PSVRECORE_GXM_CAPTURE=<scene>:<draw>:<folder>: that draw (by its index; or, an address, the first whose vertex
// program is there, in that scene or a later one) written to the folder as a psvrender scene (scene.txt, its
// programs' GXP files, its vertices decoded as the programs get them, its indices, uniforms and uniform buffers), for
// comparing the translation's drawing with the reference renderer's (tools/psvrender). A triangle list, without its
// textures or states.
struct CaptureAt {
    u64 scene = 0;
    u32 draw = 0;
    u32 surface = 0;  // (`*<address>`: every draw of the first scene drawing to the colour surface at that address)
    nx::string folder;
};

void capture_one(u64 number, const Scene& scene, usize which, const nx::string& folder);

void capture_draw(u64 number, const Scene& scene) {
    static const CaptureAt at = [] {
        CaptureAt out;
        const char* spec = std::getenv("PSVRECORE_GXM_CAPTURE");
        if (!spec || !*spec) return out;
        char* end = nullptr;
        out.scene = std::strtoull(spec, &end, 0);
        if (*end == ':' && end[1] == '*') out.surface = static_cast<u32>(std::strtoul(end + 2, &end, 0));
        else if (*end == ':') out.draw = static_cast<u32>(std::strtoul(end + 1, &end, 0));
        if (*end == ':') out.folder = nx::string(end + 1);
        return out;
    }();
    static bool done = false;
    if (done || !at.scene || number < at.scene || at.folder.empty()) return;
    if (at.surface) {
        if (!scene.has_color || scene.color.data != at.surface) return;
        done = true;
        for (usize i = 0; i < scene.draws.size(); ++i) {
            const nx::string folder = nx::format("{}/d{}", at.folder, i);
            (void)nx::fs::create_directories(nx::fs::path_view(folder));
            capture_one(number, scene, i, folder);
        }
        return;
    }
    usize which = at.draw;
    if (at.draw >= 0x10000) {
        which = scene.draws.size();
        for (usize i = 0; i < scene.draws.size(); ++i)
            if (scene.draws[i].vertex && scene.draws[i].vertex->guest == at.draw) {
                which = i;
                break;
            }
    } else if (number != at.scene) {
        return;
    }
    if (which >= scene.draws.size()) return;
    done = true;
    capture_one(number, scene, which, at.folder);
}

void capture_one(u64 number, const Scene& scene, usize which, const nx::string& folder) {
    struct {
        const nx::string& folder;
    } at{folder};
    const Draw& d = scene.draws[which];
    if (!d.vertex || !d.vertex->program || !d.fragment || !d.fragment->program) return;
    auto write_gxp = [&](const Program& p, const char* name) {
        if (memory().valid(p.gxp_address, p.gxp.size))
            (void)io::write_file(nx::fs::path_view(nx::format("{}/{}", at.folder, name)), {memory().host(p.gxp_address), p.gxp.size});
    };
    write_gxp(*d.vertex->program, "vertex.gxp");
    write_gxp(*d.fragment->program, "fragment.gxp");
    const u32 index_size = d.index_format ? 4 : 2;
    if (!memory().valid(d.index_address, d.index_count * index_size)) return;
    nx::vector<u32> indices(d.index_count);
    u32 count = 0;
    for (u32 k = 0; k < d.index_count; ++k) {
        u32 v = 0;
        std::memcpy(&v, memory().host(d.index_address + k * index_size), index_size);
        indices[k] = v;
        count = nx::max(count, v + 1);
    }
    auto half = [](u16 h) {
        const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
        f32 v = exp == 0 ? std::ldexp(static_cast<f32>(mant), -24)
                : exp == 31 ? (mant ? NAN : INFINITY)
                            : std::ldexp(static_cast<f32>(mant | 1024), static_cast<i32>(exp) - 25);
        return sign ? -v : v;
    };
    nx::string text = nx::format("# scene {} draw {} (PSVRECORE_GXM_CAPTURE)\ntarget {} {} ff404040\nvertex vertex.gxp\n"
                                 "fragment fragment.gxp\n", number, which, nx::min(scene.color.size & 0xFFFFu, 1024u),
                                 nx::min(scene.color.size >> 16, 1024u));
    {
        const BlendInfo& b = d.fragment->blend;
        text += nx::format("# vertex program 0x{:08x}, fragment program 0x{:08x}: blend {}, mask 0x{:x}, colour func {} src {} "
                           "dst {}, alpha func {} src {} dst {}\n", d.vertex->guest, d.fragment->guest,
                           d.fragment->blend_enabled, b.color_mask, b.color_func(), b.color_src(), b.color_dst(),
                           b.alpha_func(), b.alpha_src(), b.alpha_dst());
    }
    for (const VertexAttribute& a : d.vertex->attributes) {
        nx::string name;
        for (const usse::Parameter& p : d.vertex->program->gxp.parameters)
            if (p.category == usse::ParameterCategory::Attribute && p.resource == a.reg) name = p.name;
        if (name.empty() || a.stream >= d.vertex->streams.size()) continue;
        const u32 stride = d.vertex->streams[a.stream].stride, n = nx::clamp<u32>(a.components, 1, 4);
        static constexpr u32 kBytes[10] = {1, 1, 2, 2, 1, 1, 2, 2, 2, 4};
        const u32 size = a.format < 10 ? kBytes[a.format] : 4;
        text += nx::format("attribute {} {}", name, n);
        for (u32 v = 0; v < count; ++v)
            for (u32 c = 0; c < n; ++c) {
                const u32 addr = d.streams[a.stream] + v * stride + a.offset + c * size;
                f32 x = 0;
                if (memory().valid(addr, size)) {
                    const u8* p = memory().host(addr);
                    u16 h;
                    std::memcpy(&h, p, 2);
                    switch (a.format) {
                        case 0: x = p[0]; break;
                        case 1: x = static_cast<i8>(p[0]); break;
                        case 2: x = h; break;
                        case 3: x = static_cast<i16>(h); break;
                        case 4: x = p[0] / 255.0f; break;
                        case 5: x = nx::max(-1.0f, static_cast<i8>(p[0]) / 127.0f); break;
                        case 6: x = h / 65535.0f; break;
                        case 7: x = nx::max(-1.0f, static_cast<i16>(h) / 32767.0f); break;
                        case 8: x = half(h); break;
                        default: std::memcpy(&x, p, 4); break;
                    }
                }
                text += nx::format(" {:.9g}", x);
            }
        text += "\n";
    }
    text += "indices";
    for (u32 i : indices) text += nx::format(" {}", i);
    text += "\n";
    auto words = [&](const char* stage, const char* which, const u32* w, usize n) {
        if (!n) return;
        text += nx::format("words {} {}", stage, which);
        for (usize i = 0; i < n; ++i) text += nx::format(" {:x}", w[i]);
        text += "\n";
    };
    words("vertex", "uniforms", d.vertex_uniforms.data(), d.vertex_uniforms.size());
    words("fragment", "uniforms", d.fragment_uniforms.data(), d.fragment_uniforms.size());
    for (u32 stage = 0; stage < 2; ++stage) {
        const usse::Gxp& g = stage ? d.fragment->program->gxp : d.vertex->program->gxp;
        const u32* buffers = stage ? d.fragment_buffers : d.vertex_buffers;
        for (u32 b = 0; b < 14; ++b) {
            const u32 bytes = buffer_bytes(g, b);
            if (!buffers[b] || !bytes || !memory().valid(buffers[b], bytes)) continue;
            words(stage ? "fragment" : "vertex", nx::format("{}", b).c_str(),
                  reinterpret_cast<const u32*>(memory().host(buffers[b])), bytes / 4);
        }
    }
    // The fragment textures as guest memory holds them now, decoded (level 0, face 0): t<unit>.bin, described in a
    // comment (psvrender draws with its own textures; these are for looking at).
    for (u32 u = 0; u < 16; ++u) {
        const TextureWords& w = d.fragment_textures[u];
        if (!w.words[0] && !w.words[1] && !w.words[2] && !w.words[3]) continue;
        const TextureInfo t = decode_texture(w.words);
        auto texels = read_texels(t, read_guest_memory);
        if (!texels) {
            text += nx::format("# texture {}: 0x{:08x}:{:08x} not read ({})\n", u, t.address, t.base_format,
                               texels.error().message);
            continue;
        }
        const usize level0 = usize{texels->width} * texels->height * texels->bytes;
        if (texels->bc || texels->data.size() < level0) continue;
        (void)io::write_file(nx::fs::path_view(nx::format("{}/t{}.bin", at.folder, u)), {texels->data.data(), level0});
        text += nx::format("# texture {}: 0x{:08x}:{:08x}/type {} {}x{}, {} bytes a texel{}: t{}.bin\n", u, t.address,
                           t.base_format, t.type, texels->width, texels->height, texels->bytes,
                           texels->single ? " (one float)" : "", u);
    }
    text += "draw\n";
    (void)io::write_file(nx::fs::path_view(nx::format("{}/scene.txt", at.folder)),
                         {reinterpret_cast<const u8*>(text.data()), text.size()});
    nx::logi(logs::gxm, "capture: scene {} draw {} written to {} ({} vertices, {} indices)", number, which, at.folder,
             count, d.index_count);
}

// A draw's vertex shader variant: its inputs, from the vertex program's attributes and the program's parameters.
VertexShader vertex_shader_of(const Draw& dr) {
    const Program& vp = *dr.vertex->program;
    VertexShader vs;
    for (usize a = 0; a < dr.vertex->attributes.size(); ++a) {
        const VertexAttribute& attr = dr.vertex->attributes[a];
        VertexInput in;
        in.location = static_cast<u32>(a);
        in.reg = attr.reg;
        in.format = attr.format;
        in.components = attr.components;
        for (const usse::Parameter& p : vp.gxp.parameters)
            if (p.category == usse::ParameterCategory::Attribute && p.resource == attr.reg) in.raw = p.reg_format;
        vs.inputs.push_back(in);
    }
    return vs;
}

// (a quick key: the program's content and the inputs, as vertex_key takes them)
u64 vertex_quick(const Program& vp, const VertexShader& vs) {
    nx::fnv1a64 q;
    q.combine_bytes(reinterpret_cast<const char*>(vp.content.data()), vp.content.size()).combine('v');
    for (const VertexInput& in : vs.inputs) q.combine_all(in.location, in.reg, in.format, in.components, in.raw);
    return q.value();
}

// The output register's lanes an 8-bit colour surface gamma-corrects on writing them (FragmentShader::gamma, a mask), for
// its gamma mode: SCE_GXM_COLOR_SURFACE_GAMMA_R / _BGR (0x1000, stored as 1) is the three colour lanes of an 8888 surface
// (never alpha, wherever its swizzle puts it), _GR (0x3000, stored as 3) the first two of a two-component one. The game
// then reads such a surface back as a texture with a gamma mode, which decodes it, and draws in linear light.
//
// Where the surface's image can be drawn through an sRGB view (gamma_view), the hardware does this instead of the
// shader: the lanes are written linear, blending runs in linear light and the blended colour is encoded as it is
// stored, as the console does (a translucent fill over a dark surface is much brighter than one blended in encoded
// space: Killzone: Mercenary's selection highlight is that). The shader's own encode (FragmentShader::gamma) stays
// for the others (a colour lane order other than RGB(A), an image made without the view).
u8 surface_gamma(const ColorSurface& cs, const ColorFormat& cf) {
    const u32 mode = (cs.modes >> 8) & 0xF;
    if (!mode) return 0;
    const bool rgba8 = cf.packing == Packing::None && (cf.format == VK_FORMAT_R8G8B8A8_UNORM || cf.format == VK_FORMAT_B8G8R8A8_UNORM);
    // (warned of once for each mode and format)
    auto drop = [&](const char* why) {
        const u64 key = (u64{mode} << 32) ^ cs.format;
        static std::mutex lock;
        static nx::vector<u64> keys;
        const std::lock_guard<std::mutex> guard(lock);
        for (u64 k : keys)
            if (k == key) return;
        keys.push_back(key);
        nx::logw(logs::gxm, "colour surface format 0x{:08x}: gamma mode 0x{:x} dropped ({}): its colours are written as they are",
                 cs.format, mode << 12, why);
    };
    if (mode == 1 && rgba8) return 0x7;
    if (mode == 1) return drop("only the 8888 formats are gamma-corrected"), u8{0};
    if (mode == 3) return drop("no two-component 8-bit format is drawn"), u8{0};
    return drop("an unknown mode"), u8{0};
}

// Whether a colour surface with a gamma mode (`cf.gamma` set) is drawn through its image's sRGB view: not with
// PSVRECORE_GAMMA_VIEW=0 (the fragment shader encodes, blending in encoded space), nor for a lane order other than the
// attachment's own (the view encodes components 0 to 2), nor for an image of the surface made without the view.
bool gamma_view(Renderer& r, const ColorFormat& cf, u32 address, u32 format, u32 width, u32 height) {
    static const bool enabled = [] {
        const char* v = std::getenv("PSVRECORE_GAMMA_VIEW");
        return !v || std::strcmp(v, "0") != 0;
    }();
    if (!enabled || !cf.gamma || cf.packing != Packing::None || vk::srgb_format_of(cf.format) == VK_FORMAT_UNDEFINED) return false;
    for (int k = 0; k < 4; ++k)
        if (cf.swizzle[k] != k) return false;
    for (const auto& t : r.targets)
        if (t->address == address && t->format == format && t->width == width && t->height == height)
            return t->image.srgb_view != VK_NULL_HANDLE;
    return true;  // (made with the view)
}

// A draw's read of the colour done by the pipeline's blending instead (color_read_blend), or none: a program whose
// read is such a blend, of an 8-bit colour surface whose alpha is the output register's alpha lane, without the
// patcher's blending. (Fragments reading the colour run in no order where the device has no rasterization-order
// attachment access, nor do two reading one pixel in one draw wait for each other; blending is ordered, and the
// draw then reads no attachment.)
std::optional<ColorReadBlend> blend_read_of(const Draw& dr, const ColorFormat& cf, bool color) {
    const Program& p = *dr.fragment->program;
    if (!color || !p.gxp.native_color || !p.gxp.frag_color || dr.fragment->blend_enabled) return std::nullopt;
    if (cf.format != VK_FORMAT_R8G8B8A8_UNORM && cf.format != VK_FORMAT_B8G8R8A8_UNORM) return std::nullopt;
    const u8* z = cf.swizzle;
    if (z[3] != 3 || z[0] > 2 || z[1] > 2 || z[2] > 2 || z[0] == z[1] || z[0] == z[2] || z[1] == z[2]) return std::nullopt;
    if (p.blend_read < 0) {
        const auto found = color_read_blend(p.gxp, p.primary);
        Program& q = const_cast<Program&>(p);  // (found once a program, as stores())
        if (found) q.blend_read_found = *found;
        q.blend_read = found ? 1 : 0;
    }
    return p.blend_read == 1 ? std::optional<ColorReadBlend>(p.blend_read_found) : std::nullopt;
}

// A draw's fragment shader variant, for a colour surface of that format (or none).
FragmentShader fragment_shader_of(const Draw& dr, const ColorFormat& cf, bool color) {
    FragmentShader fs;
    fs.output_format = dr.fragment->output_format;
    std::memcpy(fs.swizzle, cf.swizzle, 4);
    fs.color_output = color;
    fs.blend_read = blend_read_of(dr, cf, color).has_value();
    fs.gamma = color ? cf.gamma : 0;
    return fs;
}

// Whether a draw reads the colour attachment (as an input attachment).
bool reads_attachment(const Draw& dr, const ColorFormat& cf, bool color) {
    const usse::Gxp& g = dr.fragment->program->gxp;
    return color && g.native_color && g.frag_color && !blend_read_of(dr, cf, color);
}

u64 fragment_quick(const Program& fp, const FragmentShader& fs, bool inline_secondary) {
    nx::fnv1a64 q;
    q.combine_bytes(reinterpret_cast<const char*>(fp.content.data()), fp.content.size()).combine('f');
    q.combine_all(fs.output_format, fs.swizzle[0], fs.swizzle[1], fs.swizzle[2], fs.swizzle[3], fs.color_output,
                  fs.fragment_program_enabled, inline_secondary, fs.blend_read, fs.gamma);
    return q.value();
}

// The shaders the draws need that neither were made nor are in the shader cache: translated on several threads
// before the draws are planned (a phone took 96 ms for the 24 of a song's first scene, one after another); and the
// secondary programs' compute pipelines not made yet, made on several threads.
void prepare_shaders(Renderer& r, std::span<const Draw> draws, const ColorFormat& cf, bool color) {
    enum class Kind { Vertex, Fragment, Secondary };
    struct Task {
        ShaderKey key;
        const Program* program = nullptr;
        Kind kind = Kind::Vertex;
        bool translate = true;
        VertexShader vs;
        FragmentShader fs;
        bool inline_secondary = false;
        nx::vector<u32> words;
        nx::string error;
    };
    nx::vector<Task> tasks;
    nx::hash_map<u64, bool> seen;
    auto add = [&](Task t) {
        const u64 k = short_key(t.key);
        if (seen.contains(k) || r.failed_programs.contains(k)) return;
        seen[k] = true;
        if (t.kind == Kind::Secondary ? r.compute.contains(k) : r.modules.contains(k)) return;
        t.translate = !r.modules.contains(k) && !r.cache.spirv(t.key);
        if (t.kind != Kind::Secondary && !t.translate) return;  // (a module from the cache's SPIR-V: quick, when planned)
        tasks.push_back(std::move(t));
    };
    for (const Draw& dr : draws) {
        if (!dr.vertex || !dr.fragment || !dr.vertex->program || !dr.fragment->program) continue;
        const Program& vp = *dr.vertex->program;
        const Program& fp = *dr.fragment->program;
        const bool inline_secondary = tile_secondary(fp);
        for (const Program* p : {&vp, inline_secondary ? nullptr : &fp}) {
            if (!p || p->secondary.empty() || r.quick_compute.contains(secondary_quick(*p))) continue;
            Task t;
            t.key = secondary_key(*p), t.program = p, t.kind = Kind::Secondary;
            add(std::move(t));
        }
        VertexShader vs = vertex_shader_of(dr);
        if (!r.quick_modules.contains(vertex_quick(vp, vs))) {
            Task t;
            t.key = vertex_key(vp, vs), t.program = &vp, t.kind = Kind::Vertex;
            t.vs = std::move(vs);
            add(std::move(t));
        }
        const FragmentShader fs = fragment_shader_of(dr, cf, color);
        if (!r.quick_modules.contains(fragment_quick(fp, fs, inline_secondary))) {
            Task t;
            t.key = fragment_key(fp, fs, inline_secondary), t.program = &fp, t.kind = Kind::Fragment;
            t.fs = fs;
            t.inline_secondary = inline_secondary;
            add(std::move(t));
        }
    }
    if (tasks.empty()) return;
    in_parallel(tasks.size(), [&](usize i) {
        Task& t = tasks[i];
        if (!t.translate) return;
        const Program& p = *t.program;
        const std::span<const usse::Decoded> inlined =
            t.inline_secondary ? std::span<const usse::Decoded>(p.secondary) : std::span<const usse::Decoded>{};
        auto spirv = t.kind == Kind::Secondary ? translate_secondary(p.gxp, p.secondary)
                     : t.kind == Kind::Vertex  ? translate_vertex(p.gxp, p.primary, t.vs)
                                               : translate_fragment(p.gxp, p.primary, t.fs, inlined);
        if (spirv) t.words = std::move(*spirv);
        else t.error = spirv.error().message;
    });
    // (the modules, one after another: they are kept in the renderer's maps and the shader cache)
    nx::vector<std::pair<usize, VkShaderModule>> computes;
    for (usize i = 0; i < tasks.size(); ++i) {
        Task& t = tasks[i];
        const VkShaderModule m = module_for(r, t.key, [&]() -> Result<nx::vector<u32>> {
            if (!t.error.empty()) return failed_noted(t.program, failure(t.error));
            return std::move(t.words);
        });
        if (m && t.kind == Kind::Secondary) computes.push_back({i, m});
    }
    nx::vector<VkPipeline> made(computes.size(), VK_NULL_HANDLE);
    in_parallel(computes.size(), [&](usize k) { made[k] = make_compute(r, computes[k].second); });
    for (usize k = 0; k < computes.size(); ++k) {
        const ShaderKey& key = tasks[computes[k].first].key;
        if (made[k]) {
            r.cache.add_compute(key);
            r.made_pipelines.fetch_add(1);
            r.pipelines_changed.store(true);
        }
        r.compute[short_key(key)] = made[k];
    }
}

}  // namespace

namespace {

// ---- Shaders compiled ahead ------------------------------------------------------------------------------------
// A phone's driver compiles a whole pipeline's shaders together, once for each pair of them (25-630 ms; a song's first
// scenes with an empty shader cache waited 0.2-0.47 s for 6-21 new pairs at once), and the game pairs a fragment
// program with vertex programs other than the one it was made for. With VK_EXT_graphics_pipeline_library a shader is
// compiled once on its own (its library: 20-70 ms), and a pipeline of any pair of compiled shaders is linked in a
// millisecond or two (make_graphics; the whole pipeline made after, on these threads, in its place). The game makes
// its programs seconds before it draws with them (a song's at the start, some 40 s before): each program made is
// translated here as a draw would translate it (the colour of the last scene drawn), on threads of its own, and its
// shader's library made, into the driver's cache; the shader cache marks it prepared (its SPIR-V kept only once a
// draw uses it), and the next runs skip it.
struct AheadPair {
    u32 vertex = 0, fragment = 0;  // (the guest's handles; 0: a program of that kind to compile it with, any)
};
struct Ahead {
    nx::mutex lock;
    nx::vector<AheadPair> queue;
    nx::vector<vk::GraphicsPipelineDesc> whole;  // (linked pipelines a scene used: made whole, in their place)
    nx::hash_map<u64, bool> vertex, fragment;  // (shaders compiled in a pipeline, by quick key and the formats)
    ColorFormat cf;                // (the last scene's colour)
    bool cf_known = false, off = false;
    std::atomic<bool> started{false};
    u64 made = 0, ns = 0;  // (programs whose shaders were compiled, and their time added up)
};
Ahead& ahead() {
    static Ahead* a = new Ahead;  // (its threads run to the process's end)
    return *a;
}

// A pair copied out of the game's state (its programs may be released meanwhile).
struct AheadWork {
    Program vp, fp;
    nx::vector<VertexAttribute> attributes;
    nx::vector<VertexStream> streams;
    u32 output_format = 0;
    bool blend_enabled = false;
    BlendInfo blend{};
};

bool ahead_copy(const AheadPair& pair, AheadWork& w) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    const VertexProgram* vp = nullptr;
    const FragmentProgram* fp = nullptr;
    if (auto v = s.vertex_programs.find(pair.vertex); v != s.vertex_programs.end()) vp = v->second.get();
    if (auto f = s.fragment_programs.find(pair.fragment); f != s.fragment_programs.end()) fp = f->second.get();
    if ((pair.vertex && !vp) || (pair.fragment && !fp)) return false;  // (released meanwhile)
    // (the other: the one a fragment program was made for, or one made for the vertex program's; else any)
    if (!vp) {
        for (const auto& [guest, v] : s.vertex_programs) {
            if (!v->program) continue;
            if (!vp || v->program->gxp_address == fp->vertex_gxp) vp = v.get();
            if (v->program->gxp_address == fp->vertex_gxp) break;
        }
    }
    if (!fp) {
        for (const auto& [guest, f] : s.fragment_programs) {
            if (f->mask_update || !f->program) continue;
            if (!fp || f->vertex_gxp == vp->program->gxp_address) fp = f.get();
            if (f->vertex_gxp == vp->program->gxp_address) break;
        }
    }
    if (!vp || !fp || fp->mask_update || !vp->program || !fp->program) return false;
    w.vp = *vp->program, w.fp = *fp->program;
    w.attributes = vp->attributes, w.streams = vp->streams;
    w.output_format = fp->output_format, w.blend_enabled = fp->blend_enabled, w.blend = fp->blend;
    return true;
}

// SPIR-V words as LEB128 bytes (most of its words are small: half the size), and back.
nx::vector<u8> pack_words(std::span<const u32> words) {
    nx::vector<u8> out;
    out.reserve(words.size() * 2);
    for (u32 w : words) {
        while (w >= 0x80) out.push_back(static_cast<u8>(w | 0x80)), w >>= 7;
        out.push_back(static_cast<u8>(w));
    }
    return out;
}
nx::vector<u32> unpack_words(std::span<const u8> bytes) {
    nx::vector<u32> out;
    u32 w = 0, shift = 0;
    for (u8 b : bytes) {
        w |= static_cast<u32>(b & 0x7F) << shift;
        if (b & 0x80) {
            shift += 7;
        } else {
            out.push_back(w);
            w = 0, shift = 0;
        }
    }
    return out;
}

// A shader's module for compiling ahead: the renderer's, else made from the cache's SPIR-V or translated (outside its
// lock), and kept in its maps; translated SPIR-V kept unsaved until a draw uses it.
VkShaderModule ahead_module(Renderer& r, const ShaderKey& key, u64 quick, auto&& translate) {
    {
        TargetsHeld held(r);
        if (auto it = r.modules.find(short_key(key)); it != r.modules.end()) return it->second;
        if (r.failed_programs.contains(short_key(key))) return VK_NULL_HANDLE;
    }
    nx::vector<u32> words;
    const bool cached = static_cast<bool>(r.cache.spirv(key));
    if (cached) {
        words = *r.cache.spirv(key);
    } else {
        auto spirv = translate();
        if (!spirv) return VK_NULL_HANDLE;
        words = std::move(*spirv);
    }
    auto m = vk::create_shader(*r.device, words);
    if (!m) return VK_NULL_HANDLE;
    TargetsHeld held(r);
    if (auto it = r.modules.find(short_key(key)); it != r.modules.end()) {
        vkDestroyShaderModule(r.device->device(), *m, nullptr);
        return it->second;
    }
    r.modules[short_key(key)] = *m;
    r.module_keys[reinterpret_cast<u64>(*m)] = key;
    r.quick_modules[quick] = *m;
    if (!cached) r.unsaved[short_key(key)] = {key, pack_words(words)};
    return *m;
}

void ahead_make(Renderer& r, const AheadPair& pair, const ColorFormat& cf) {
    AheadWork w;
    if (!ahead_copy(pair, w)) return;
    // The shaders, as a draw of the last scene's colour would have them (vertex_shader_of, fragment_shader_of).
    VertexShader vs;
    for (usize a = 0; a < w.attributes.size(); ++a) {
        const VertexAttribute& attr = w.attributes[a];
        VertexInput in;
        in.location = static_cast<u32>(a);
        in.reg = attr.reg;
        in.format = attr.format;
        in.components = attr.components;
        for (const usse::Parameter& p : w.vp.gxp.parameters)
            if (p.category == usse::ParameterCategory::Attribute && p.resource == attr.reg) in.raw = p.reg_format;
        vs.inputs.push_back(in);
    }
    std::optional<ColorReadBlend> br;
    const bool rgba8 = cf.format == VK_FORMAT_R8G8B8A8_UNORM || cf.format == VK_FORMAT_B8G8R8A8_UNORM;
    const u8* z = cf.swizzle;
    if (w.fp.gxp.native_color && w.fp.gxp.frag_color && !w.blend_enabled && rgba8 && z[3] == 3 && z[0] <= 2 && z[1] <= 2 &&
        z[2] <= 2 && z[0] != z[1] && z[0] != z[2] && z[1] != z[2])
        br = color_read_blend(w.fp.gxp, w.fp.primary);
    FragmentShader fs;
    fs.output_format = w.output_format;
    std::memcpy(fs.swizzle, cf.swizzle, 4);
    fs.color_output = true;
    fs.blend_read = br.has_value();
    fs.gamma = cf.gamma;
    const bool inline_secondary = tile_secondary_scan(w.fp);
    const bool reads_color = w.fp.gxp.native_color && w.fp.gxp.frag_color && !br;
    const u64 vq = vertex_quick(w.vp, vs), fq = fragment_quick(w.fp, fs, inline_secondary);
    vk::PassFormats formats;
    formats.color = cf.attachment_format();
    formats.depth = r.device->caps().depth_stencil_format;
    formats.color_input = reads_color ? 1 : 0;
    nx::fnv1a64 vk_, fk;
    vk_.combine_all(vq, static_cast<u32>(formats.color), static_cast<u32>(formats.depth));
    fk.combine_all(fq, static_cast<u32>(formats.color), static_cast<u32>(formats.depth), formats.color_input);
    {
        Ahead& a = ahead();
        const nx::scoped_lock<nx::mutex> held(a.lock);
        if (a.vertex.contains(vk_.value()) && a.fragment.contains(fk.value())) return;
        a.vertex[vk_.value()] = true, a.fragment[fk.value()] = true;
    }
    // (a shader compiled ahead in a run before: in the driver's cache, its library made quickly when a draw needs it)
    const ShaderKey vkey = vertex_key(w.vp, vs), fkey = fragment_key(w.fp, fs, inline_secondary);
    // (a colour-reading shader's mark its own: before local read, those were marked with no library made)
    ShaderKey fmark = fkey;
    if (reads_color) fmark[0] ^= 0x5A;
    const bool need_v = !r.cache.prepared(vkey), need_f = !r.cache.prepared(fmark);
    if (!need_v && !need_f) return;
    const u64 from = nx::os::ticks_ns();
    const VkShaderModule vm = need_v ? ahead_module(r, vkey, vq, [&] { return translate_vertex(w.vp.gxp, w.vp.primary, vs); })
                                     : VK_NULL_HANDLE;
    const VkShaderModule fm = need_f ? ahead_module(r, fkey, fq, [&] {
        const std::span<const usse::Decoded> inlined =
            inline_secondary ? std::span<const usse::Decoded>(w.fp.secondary) : std::span<const usse::Decoded>{};
        return translate_fragment(w.fp.gxp, w.fp.primary, fs, inlined);
    }) : VK_NULL_HANDLE;
    if (!vm && !fm) return;  // (not translated: the draw finds the error again, and reports it)
    // The shaders' libraries: their state as any draw's (the driver compiles a shader once for all of them).
    auto desc = vk::GraphicsPipelineDesc::defaults();
    desc.vertex = vm, desc.fragment = fm, desc.layout = r.layout;
    desc.depth_test = 1, desc.depth_write = 1, desc.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    desc.formats = formats;
    const auto made = r.pipelines->prepare(desc);
    if (!made || !*made) return;  // (a render pass's: made whole when a draw needs it)
    if (vm) r.cache.add_prepared(vkey);
    if (fm) r.cache.add_prepared(fmark);
    r.pipelines_changed.store(true);  // (the driver's cache has them now: saved with the rest)
    Ahead& a = ahead();
    const nx::scoped_lock<nx::mutex> held(a.lock);
    ++a.made, a.ns += nx::os::ticks_ns() - from;
}

void ahead_run(Renderer& r) {
    Ahead& a = ahead();
    u64 reported = 0;
    for (;;) {
        nx::this_thread::sleep_for(std::chrono::milliseconds(20));
        AheadPair pair;
        ColorFormat cf;
        // (the whole pipelines first: scenes draw with their linked ones meanwhile)
        for (;;) {
            vk::GraphicsPipelineDesc desc;
            {
                const nx::scoped_lock<nx::mutex> held(a.lock);
                if (a.whole.empty()) break;
                desc = a.whole.front();
                a.whole.erase(a.whole.begin());
            }
            if (r.pipelines->graphics(desc)) r.pipelines_changed.store(true);
        }
        {
            const nx::scoped_lock<nx::mutex> held(a.lock);
            if (a.queue.empty() || !a.cf_known) {
                if (a.queue.empty() && a.made != reported) {
                    nx::logi(logs::gpu, "shaders compiled ahead: {} programs' so far ({:.0f} ms of their threads)", a.made,
                             static_cast<f64>(a.ns) / 1e6);
                    reported = a.made;
                }
                continue;
            }
            pair = a.queue.front();
            a.queue.erase(a.queue.begin());
            cf = a.cf;
        }
        ahead_make(r, pair, cf);
        // (the next at once: the queue is drained without the pause)
        for (;;) {
            {
                const nx::scoped_lock<nx::mutex> held(a.lock);
                if (a.queue.empty()) break;
                pair = a.queue.front();
                a.queue.erase(a.queue.begin());
                cf = a.cf;
            }
            ahead_make(r, pair, cf);
        }
    }
}

// The threads start (once warm_up is done with the renderer's maps). PSVRECORE_PIPELINES_AHEAD=0 turns it off;
// without pipeline libraries it is off (a pair compiled ahead helps only that pair, and the game pairs a fragment
// program with vertex programs other than the one it was made for: an Adreno 830's new pipelines are made
// asynchronously instead, make_async).
void ahead_start(Renderer& r) {
    Ahead& a = ahead();
    if (a.started.exchange(true)) return;
    if (!r.device->caps().pipeline_library) {
        const nx::scoped_lock<nx::mutex> held(a.lock);
        a.off = true;
        a.queue.clear();
        return;
    }
    if (const char* v = std::getenv("PSVRECORE_PIPELINES_AHEAD"); v && std::atoi(v) == 0) {
        const nx::scoped_lock<nx::mutex> held(a.lock);
        a.off = true;
        a.queue.clear();
        nx::logi(logs::gpu, "pipelines: not made ahead (PSVRECORE_PIPELINES_AHEAD=0)");
        return;
    }
    const u32 threads = nx::max<u32>(pipeline_threads() - 1, 1);
    for (u32 t = 0; t < threads; ++t) {
        nx::thread_options options;
        options.name = "pipelines ahead";
        options.priority = nx::thread_priority::BelowNormal;  // (the game's threads first)
        nx::thread worker([&r] { ahead_run(r); }, options);
        worker.detach();
    }
}

void ahead_color(const ColorFormat& cf) {
    Ahead& a = ahead();
    const nx::scoped_lock<nx::mutex> held(a.lock);
    a.cf = cf, a.cf_known = true;
}

// (a linked pipeline a scene used: made whole on the threads ahead)
void pipeline_whole(const vk::GraphicsPipelineDesc& desc) {
    Ahead& a = ahead();
    const nx::scoped_lock<nx::mutex> held(a.lock);
    for (const vk::GraphicsPipelineDesc& d : a.whole)
        if (d.same(desc)) return;
    a.whole.push_back(desc);
}

}  // namespace

void pipeline_ahead(u32 vertex_program, u32 fragment_program) {
    Ahead& a = ahead();
    const nx::scoped_lock<nx::mutex> held(a.lock);
    if (a.off || a.queue.size() >= 8192) return;
    a.queue.push_back({vertex_program, fragment_program});
}

bool render_scene(Scene& scene, nx::vector<Notification> notifications, u64 number) {
    PSV_PROFILE_ZONE_COLOR("gxm: render scene", kSceneRgb);
    g_profile.start();
    Renderer& r = renderer();
    if ((!scene.has_color && !scene.has_depth) || !ensure(r)) return false;
    TargetsHeld targets_held(r);
    ++r.scenes;
    evict_textures(r);
    g_profile.mark(8);
    // (the copies this scene's textures ask for: recorded with its commands, their buffers given back when it ends)
    bool copies_recorded = false;
    struct Copies {
        Renderer& r;
        bool& recorded;
        ~Copies() { finish_target_copies(r, recorded); }
    } copies{r, copies_recorded};
    if (traced(r.scenes, scene)) trace_scene(r.scenes, scene);
    if (const auto& skip = skipped_programs(); !skip.empty()) {
        auto skipped = [&](const Draw& dr) {
            return std::find(skip.begin(), skip.end(), dr.vertex ? dr.vertex->guest : 0) != skip.end() ||
                   std::find(skip.begin(), skip.end(), dr.fragment ? dr.fragment->guest : 0) != skip.end();
        };
        // (the markers stay before the draws they were before)
        for (UserMarker& m : scene.markers) {
            u32 gone = 0;
            for (u32 i = 0; i < m.at && i < scene.draws.size(); ++i) gone += skipped(scene.draws[i]);
            m.at -= gone;
        }
        std::erase_if(scene.draws, skipped);
    }
    // A scene without draws leaves guest memory as it is: its colour loaded and stored unchanged, its depth too (or
    // not stored). Nothing is rendered or written back (a depth surface loaded and scaled up takes milliseconds); its
    // notifications are written after the scenes before it, an empty submission in their order. One that stores a
    // depth surface it does not load clears it, and one with visibility counters writes them: rendered.
    if (scene.draws.empty() && !scene.visibility_buffer && !scene.store_elsewhere &&
        !(scene.has_depth && (scene.depth.control & kDsForceStore) && !(scene.depth.control & kDsForceLoad))) {
        auto shot = nx::make_unique<vk::OneShot>(*r.device);
        if (auto cmd = shot->begin(); cmd && shot->submit()) {
            copies_recorded = true;
            Renderer::Pending pending;
            pending.shot = std::move(shot);
            pending.number = number;
            pending.index = r.scenes;
            r.submitted = number;
            pending.finish = [notes = std::move(notifications)](bool) {
                for (const Notification& n : notes) notify(n);
            };
            g_finish_why = 0;
            finish_pending(r, false);
            finish_pending(r, true, scenes_in_flight() - 1);
            r.pending.push_back(std::move(pending));
            r.pending_count.store(static_cast<u32>(r.pending.size()), std::memory_order_release);
            return true;
        }
    }
    capture_draw(r.scenes, scene);
    vk::Device& d = *r.device;
    const ColorSurface& cs = scene.color;
    const DepthStencilSurface& ds = scene.depth;
    // (where the depth/stencil is stored: the surface's own, or another's when the scene loads one and stores one)
    DepthStencilSurface ds_store = ds;
    if (scene.store_elsewhere) {
        ds_store.depth_data = scene.store_depth_data, ds_store.stencil_data = scene.store_stencil_data;
        ds_store.control |= kDsForceStore;
    }
    ColorFormat cf;
    // (without a colour surface, a depth-only scene: the render target's size)
    u32 width = scene.target->width, height = scene.target->height, stride = 0;
    SurfaceLayout layout;  // (where the colour surface's pixels are in guest memory)
    vk::Image* color = nullptr;
    // The internal resolution: drawn into `color` at `scale` times the surface's size (the target's scaled image),
    // and blitted down into the surface's image for guest memory. A depth-only scene is drawn at its own size.
    Target* target = nullptr;
    u32 scale = 1;
    Changes changes;  // (at the internal resolution: what the scaled image does not hold yet)
    if (scene.has_color) {
        if (!color_format(cs.format, cf)) {
            nx::logw(logs::gxm, "colour surface format 0x{:08x} not implemented: scene not rendered", cs.format);
            return false;
        }
        width = cs.size & 0xFFFF, height = cs.size >> 16, stride = cs.stride;
        cf.gamma = surface_gamma(cs, cf);
        if (gamma_view(r, cf, cs.data, cs.format, cs.size & 0xFFFF, cs.size >> 16)) cf.srgb = true, cf.gamma = 0;
        ahead_color(cf);
        ahead_start(r);  // (once: warm_up, when there is one, is done by now)
        layout.kind = cs.modes & 3;
        if (layout.kind > SurfaceLayout::kSwizzled) {
            nx::logw(logs::gxm, "colour surface type {} not implemented: scene not rendered", layout.kind);
            return false;
        }
        layout.stride = stride, layout.bytes = cf.guest_bytes(), layout.width = width, layout.height = height;
        if (!width || !height || layout.span() > 0xFFFFFFFFu || !memory().valid(cs.data, static_cast<u32>(layout.span()), true))
            return false;
        target = color_target(r, cs.data, cs.format, cf.format, layout, cf.srgb);
        if (!target) return false;
        color = &target->image;
        if (const u32 s = surface_scale(r, cf.format); s > 1 && ensure_scaled(r, *target, s)) {
            scale = s;
            color = &target->scaled;
            changes = surface_changes(*target);
        }

        // The colour as it is in guest memory (GXM loads it at the start of every scene). At the internal
        // resolution, the tiles that changed since the last scene wrote it back are scaled up into the scaled image
        // in the commands below (the CPU's text over a frame, a cleared area); the rest of it is kept. At the
        // surface's own size, the image is kept as it is while guest memory still holds what the last scene wrote
        // back (a surface drawn scene after scene, or drawn and then read as a texture).
        bool kept = false;
        if (pending_target(r, target)) {
            kept = true;  // (its image holds, or will when the GPU gets to this scene, what a pending scene leaves)
        } else {
            depend(cs.data, static_cast<u32>(layout.span()), 5);  // (the same memory as another surface's)
            if (scale == 1 && !target->written.empty()) {
                const Changes now = surface_changes(*target);
                kept = !now.all && now.pixels == 0;
            }
        }
        g_profile.mark(9);
        if ((scale == 1 && !kept) || (scale > 1 && !changes.tiles.empty())) {
            if (g_profile.on) ++g_profile.colour_uploads;
            nx::vector<u8> texels(usize{width} * height * cf.bytes);
            if (cf.packing == Packing::None) {
                gather(layout, cs.data, texels.data());
            } else {
                nx::vector<u8> packed(usize{width} * height * cf.guest_bytes());
                gather(layout, cs.data, packed.data());
                unpack_pixels(cf.packing, packed.data(), texels.data(), usize{width} * height);
            }
            const VkImageLayout layout = scale > 1 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (auto done = vk::upload_image(d, target->image, texels, 0, layout); !done) {
                nx::logw(logs::gpu, "{}", done.error().message);
                return false;
            }
        }
        g_profile.mark(10);
    }
    const u32 target_w = nx::min(width, scene.target->width), target_h = nx::min(height, scene.target->height);
    // A depth-only scene whose depth is stored (a shadow map): at the internal resolution too, in a depth image kept
    // for the surface, which the textures reading that surface then sample.
    DepthLayout kl;
    const bool depth_surface = scene.has_depth && d.caps().depth_stencil_format != VK_FORMAT_UNDEFINED &&
                               depth_layout(ds.control & 0x03FFF000u, kl) && depth_surface_valid(ds, kl, width, height);
    const DepthRanges ranges = depth_surface ? depth_ranges(ds, kl, width, height) : DepthRanges{};
    // (4 bytes of depth a sample without stencil, as depth_target_for takes: others, a D16 shadow map say, stored
    // through a kept image as at the surface's own size, their textures made from it on the GPU)
    const bool stores_sampled_depth = !scene.has_color && (ds.control & kDsForceStore) && state().options->render_scale > 1 &&
                                      depth_surface && kl.depth_bytes == 4 && !kl.stencil && !kl.packed;
    // A surface loaded or stored by a scene with colour (or at the surface's own size): drawn with its kept image,
    // which holds the surface when the scene that last stored it did so from the image (and none wrote there since),
    // or when guest memory still holds what was stored; a scene storing one keeps its image for the next.
    DepthTarget* kept_depth = nullptr;
    bool depth_kept = false;  // (its image still holds the surface: nothing to load)
    if (depth_surface && !stores_sampled_depth && (ds.control & (kDsForceLoad | kDsForceStore)))
        kept_depth = kept_depth_for(r, ds, width, height, scale, (ds.control & kDsForceStore) != 0);
    if (kept_depth && (ds.control & kDsForceLoad)) {
        const int pending = pending_kept_depth(r, kept_depth, ranges);
        depth_kept = pending == 1 || (pending == -1 && kept_holds(*kept_depth, ranges));
    }
    // (else each aspect from a kept image of another surface over the same memory, copied on the GPU)
    DepthTarget* aspect_from[2] = {};  // (the depth, the stencil)
    if (depth_surface && !depth_kept && (ds.control & kDsForceLoad) && !stores_sampled_depth) {
        aspect_from[0] = kept_aspect(r, ds, ranges, width, height, scale, false, kept_depth);
        aspect_from[1] = kept_aspect(r, ds, ranges, width, height, scale, true, kept_depth);
    }
    // (depth loaded otherwise, or compared with the kept image of one stored at the internal resolution: read from
    // guest memory, so what pending scenes write there written first. Only stored, it waits for none: scenes are
    // finished in order, so this one's store comes after theirs)
    if (depth_surface && !depth_kept && ((ds.control & kDsForceLoad) || stores_sampled_depth)) {
        if (ranges.depth_bytes && !aspect_from[0]) depend(ranges.depth, ranges.depth_bytes, 6);
        if (ranges.stencil_bytes && !aspect_from[1]) depend(ranges.stencil, ranges.stencil_bytes, 6);
    }
    DepthTarget* depth_target = nullptr;
    if (!scene.has_color && scene.has_depth && (ds.control & kDsForceStore) &&
        d.caps().depth_stencil_format != VK_FORMAT_UNDEFINED) {
        DepthLayout l;
        const u32 s = nx::clamp<u32>(state().options->render_scale, 1, 4);
        if (s > 1 && depth_layout(ds.control & 0x03FFF000u, l) && depth_surface_valid(ds, l, width, height)) {
            depth_target = depth_target_for(r, ds, l, width, height, s);
            if (depth_target) {
                scale = s;
                depth_kept = depth_target->written.size() == depth_bytes(ds, width, height).size() &&
                             depth_target_holds(*depth_target);
            }
        }
    }
    // (what is drawn: the surface's pixels at the internal resolution)
    const u32 draw_w = width * scale, draw_h = height * scale;

    // Depth and stencil: an image of the scene, cleared to the surface's background values, or loaded from the
    // surface (force load) and stored back to it (force store).
    vk::Image depth;
    DepthLayout dl;
    bool has_depth = scene.has_depth && d.caps().depth_stencil_format != VK_FORMAT_UNDEFINED;
    if (has_depth && !depth_layout(ds.control & 0x03FFF000u, dl)) {
        nx::logw(logs::gxm, "depth/stencil format 0x{:08x} not implemented: no depth test", ds.control & 0x03FFF000u);
        has_depth = false;
    }
    const bool unorm24 = d.caps().depth_stencil_format == VK_FORMAT_D24_UNORM_S8_UINT;
    // The mask bit (DF32M): in the stencil aspect, tested by every draw and set by mask update draws. (DF32M_S8
    // needs the stencil aspect for the stencil: its mask is not implemented, every sample passes.)
    const bool mask_bits = has_depth && mask_in_stencil(dl);
    if (has_depth && dl.mask && dl.stencil) {
        static bool reported = false;  // (once: the scenes of a frame may each have one)
        if (!std::exchange(reported, true)) nx::logw(logs::gxm, "the mask bit of DF32M_S8 surfaces is not implemented");
    }
    bool load_depth = has_depth && (ds.control & kDsForceLoad);
    bool store_depth = has_depth && (ds_store.control & kDsForceStore);
    if ((load_depth && !depth_surface_valid(ds, dl, width, height)) ||
        (store_depth && !depth_surface_valid(ds_store, dl, width, height))) {
        nx::logw(logs::gxm, "depth/stencil surface outside guest memory: not loaded or stored");
        load_depth = store_depth = false;
    }
    if (!has_depth || !(load_depth || store_depth)) kept_depth = nullptr, depth_kept = depth_target ? depth_kept : false;
    if (kept_depth && !(store_depth && !scene.store_elsewhere)) kept_depth->written.clear();  // (drawn over: no longer the surface)
    // (a kept image, the scene's own or the surface's: its layout copied back when the scene is done)
    DepthTarget* const depth_keeper = depth_target ? depth_target : kept_depth;
    if (has_depth && depth_keeper) {
        depth = depth_keeper->image;
    } else if (has_depth) {
        if (auto img = take_depth_image(r, d.caps().depth_stencil_format, draw_w, draw_h)) depth = *img;
    }
    if (!color && !depth.image) return false;
    g_profile.mark(0);

    // ---- What the draws need: secondary attributes, memory, vertices and indices -----------------------------
    const u32 draws = static_cast<u32>(scene.draws.size());
    // (a pass with a draw that reads the colour reads it as an input attachment: every pipeline of the pass is
    // made for that render pass)
    bool reads_color = false;
    for (const Draw& dr : scene.draws)
        reads_color |= reads_attachment(dr, cf, color != nullptr);
    nx::vector<u32> sa(usize{nx::max(draws, 1u)} * kDrawSecondaryWords, 0);
    constexpr u32 kMemoryBase = 0x1000;  // (the guest addresses the programs see for the memory buffer's words)
    nx::vector<u8> mem;  // (the memory buffer's words, after its two-word header)
    // Uniform buffers: one copy each per scene, shared by the draws that use it (so that what one draw stores, a
    // later one reads, and the copies of the buffers programs store to go back to guest memory after the scene).
    struct BufferCopy {
        u32 address = 0, bytes = 0, offset = 0;  // (offset: in mem)
        u32 region = 0;
        bool placed = false, written = false;
    };
    nx::hash_map<u32, BufferCopy> buffer_copies;
    // (buffers whose ranges overlap share one copy of their union: Killzone's depth reconstruction binds a uniform
    // buffer at each of many rows of a surface, each declared 17 MB long; copied one by one, a scene's memory buffer
    // took 300-500 MiB, a new allocation each scene, and a phone killed the game within a minute)
    struct BufferRegion {
        u32 start = 0, offset = 0;
        u64 end = 0;
        bool placed = false;
    };
    nx::vector<BufferRegion> buffer_regions;
    for (const Draw& dr : scene.draws) {
        for (u32 stage = 0; stage < 2; ++stage) {
            const Program& p = stage ? *dr.fragment->program : *dr.vertex->program;
            const u32* buffers = stage ? dr.fragment_buffers : dr.vertex_buffers;
            const bool written = stores(p);
            for (u32 b = 0; b < 14; ++b) {
                if (!buffers[b]) continue;
                BufferCopy& c = buffer_copies[buffers[b]];
                c.address = buffers[b];
                c.bytes = nx::max(c.bytes, buffer_bytes(p.gxp, b));
                c.written |= written;
            }
        }
    }
    {
        nx::vector<BufferCopy*> by_address;
        by_address.reserve(buffer_copies.size());
        for (auto& [address, c] : buffer_copies) {
            c.bytes = mapped_prefix(c.address, c.bytes);
            by_address.push_back(&c);
        }
        std::sort(by_address.begin(), by_address.end(),
                  [](const BufferCopy* a, const BufferCopy* b) { return a->address < b->address; });
        for (BufferCopy* c : by_address) {
            const u64 end = u64{c->address} + c->bytes;
            // (in the last region if it starts in it, at a whole word from its start: the words stay aligned)
            if (!buffer_regions.empty() && c->bytes && c->address < buffer_regions.back().end &&
                (c->address - buffer_regions.back().start) % 4 == 0) {
                buffer_regions.back().end = nx::max(buffer_regions.back().end, end);
            } else {
                buffer_regions.push_back({c->address, 0, end, false});
            }
            c->region = static_cast<u32>(buffer_regions.size() - 1);
        }
    }
    auto place = [&](u32 address) {
        BufferCopy& c = buffer_copies[address];
        if (!c.placed) {
            BufferRegion& g = buffer_regions[c.region];
            if (!g.placed) {
                g.placed = true;
                g.offset = static_cast<u32>(mem.size());
                host_copy(mem, g.start, static_cast<u32>(g.end - g.start));
            }
            c.placed = true;
            c.offset = g.offset + (c.address - g.start);
        }
        return kMemoryBase + c.offset;
    };
    // (the renderer's, kept from scene to scene: their memory reused, not allocated and touched afresh)
    // (the vertices: the ranges of guest memory the streams read, copied into the vertex buffer once it is made; a
    // range a stream read before in the scene, which holds this one's, is read again, not copied again; guest
    // memory is as it is now for all of the scene's draws)
    nx::vector<Renderer::VertexRange>& vertex_ranges = r.scene_vertex_ranges;
    nx::vector<Renderer::Repack>& repacks = r.scene_repacks;
    VkDeviceSize vertex_bytes = 0;
    vertex_ranges.clear();
    repacks.clear();
    auto vertex_range = [&](u32 address, u32 bytes) -> VkDeviceSize {
        const bool readable = address && bytes && memory().valid(address, bytes);
        if (readable) {
            // (at an offset a multiple of four from the range's: its attributes as aligned as in guest memory)
            for (const Renderer::VertexRange& v : vertex_ranges)
                if (v.address && v.repack == ~0u && address >= v.address && u64{address} + bytes <= u64{v.address} + v.bytes &&
                    (address - v.address) % 4 == 0)
                    return v.offset + (address - v.address);
            depend(address, bytes);
        }
        const VkDeviceSize at = vertex_bytes;
        vertex_ranges.push_back({readable ? address : 0, bytes, at});  // (address 0: zeros)
        vertex_bytes += (VkDeviceSize{bytes} + 15) & ~VkDeviceSize{15};
        return at;
    };
    auto repacked_range = [&](u32 address, u32 bytes, const Renderer::Repack& p) -> VkDeviceSize {
        const bool readable = address && bytes && memory().valid(address, bytes);
        if (readable) depend(address, bytes);
        u32 out = p.count * p.to_stride;
        for (u32 f = 0; f < p.fields; ++f) out = nx::max(out, p.field[f].to + p.field[f].bytes);
        const VkDeviceSize at = vertex_bytes;
        vertex_ranges.push_back({readable ? address : 0, out, at, static_cast<u32>(repacks.size())});
        repacks.push_back(p);
        vertex_bytes += (VkDeviceSize{out} + 15) & ~VkDeviceSize{15};
        return at;
    };
    nx::vector<u8>& indices = r.scene_indices;
    indices.clear();
    {
        const usize made_before = r.modules.size();
        const u64 shaders_from = g_profile.on ? nx::os::ticks_ns() : 0;
        prepare_shaders(r, scene.draws, cf, color != nullptr);
        if (g_profile.on && r.modules.size() != made_before)
            g_profile.shaders_made += r.modules.size() - made_before, g_profile.shader_ns += nx::os::ticks_ns() - shaders_from;
    }
    nx::vector<DrawPlan> plans;
    plans.reserve(draws);
    nx::vector<GraphicsJob> missing;
    for (u32 i = 0; i < draws; ++i) {
        const Draw& dr = scene.draws[i];
        DrawPlan plan;
        plan.draw = &dr;
        const Program& vp = *dr.vertex->program;
        const Program& fp = *dr.fragment->program;
        // Secondary attributes: the default uniform buffers and the uniform buffers, copied into memory.
        for (u32 stage = 0; stage < 2; ++stage) {
            const Program& p = stage ? fp : vp;
            const nx::vector<u32>& uniforms = stage ? dr.fragment_uniforms : dr.vertex_uniforms;
            const u32* buffers = stage ? dr.fragment_buffers : dr.vertex_buffers;
            u32 addresses[15] = {};
            addresses[usse::kDefaultUniformContainer] = kMemoryBase + static_cast<u32>(mem.size());
            const usize at = mem.size();
            mem.resize(at + ((uniforms.size() * 4 + 15) & ~usize{15}), 0);
            if (!uniforms.empty()) std::memcpy(mem.data() + at, uniforms.data(), uniforms.size() * 4);
            for (u32 b = 0; b < 14; ++b)
                if (buffers[b]) addresses[b] = place(buffers[b]);
            // (the words of the buffers a program keeps in registers: their copies in memory, made above)
            std::span<const u32> words[14];
            for (u32 b = 0; b < 14; ++b)
                if (buffers[b]) {
                    const BufferCopy& c = buffer_copies[buffers[b]];
                    words[b] = {reinterpret_cast<const u32*>(mem.data() + c.offset), c.bytes / 4};
                }
            fill_secondary(p.gxp, uniforms, addresses,
                           std::span<u32, 256>(sa.data() + usize{i} * kDrawSecondaryWords + 256 * stage, 256), words);
        }
        if ((stores(vp) || stores(fp)) && !d.caps().shader_stores) {
            nx::logw(logs::gpu, "the device has no vertex or fragment stage stores: a draw whose program stores is skipped");
            continue;
        }
        const bool inline_secondary = tile_secondary(fp);
        const usize made_before = r.modules.size();
        const u64 shaders_from = g_profile.on ? nx::os::ticks_ns() : 0;
        plan.vertex_secondary = secondary_pipeline(r, vp);
        plan.fragment_secondary = inline_secondary ? VK_NULL_HANDLE : secondary_pipeline(r, fp);

        // The shaders (by a quick key first).
        const VertexShader vs = vertex_shader_of(dr);
        const u64 vq = vertex_quick(vp, vs);
        VkShaderModule vmod = VK_NULL_HANDLE;
        if (auto it = r.quick_modules.find(vq); it != r.quick_modules.end()) {
            vmod = it->second;
        } else {
            vmod = module_for(r, vertex_key(vp, vs), [&] { return failed_noted(&vp, translate_vertex(vp.gxp, vp.primary, vs)); });
            if (vmod) r.quick_modules[vq] = vmod;
        }
        const FragmentShader fs = fragment_shader_of(dr, cf, color != nullptr);
        const u64 fq = fragment_quick(fp, fs, inline_secondary);
        VkShaderModule fmod = VK_NULL_HANDLE;
        if (auto it = r.quick_modules.find(fq); it != r.quick_modules.end()) {
            fmod = it->second;
        } else {
            fmod = module_for(r, fragment_key(fp, fs, inline_secondary), [&] {
                const std::span<const usse::Decoded> secondary =
                    inline_secondary ? std::span<const usse::Decoded>(fp.secondary) : std::span<const usse::Decoded>{};
                return failed_noted(&fp, translate_fragment(fp.gxp, fp.primary, fs, secondary));
            });
            if (fmod) r.quick_modules[fq] = fmod;
        }
        if (g_profile.on && r.modules.size() != made_before)
            g_profile.shaders_made += r.modules.size() - made_before, g_profile.shader_ns += nx::os::ticks_ns() - shaders_from;
        if (!vmod || !fmod) continue;

        // Indices, and the vertices they reach (each stream from its first vertex to the last one used).
        const u32 index_size = dr.index_format ? 4 : 2;
        if (!memory().valid(dr.index_address, dr.index_count * index_size)) continue;
        depend(dr.index_address, dr.index_count * index_size, 4);
        u32 max_index = 0;
        for (u32 k = 0; k < dr.index_count; ++k) {
            const u8* p = memory().host(dr.index_address + k * index_size);
            u32 v;
            if (index_size == 4) std::memcpy(&v, p, 4);
            else {
                u16 h;
                std::memcpy(&h, p, 2);
                v = h;
            }
            max_index = nx::max(max_index, v);
        }
        plan.index_offset = indices.size();
        plan.index_type = index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        host_copy(indices, dr.index_address, dr.index_count * index_size);

        auto desc = vk::GraphicsPipelineDesc::defaults();
        desc.vertex = vmod;
        desc.fragment = fmod;
        desc.layout = r.layout;
        bool ok = true;
        for (usize a = 0; a < vs.inputs.size() && ok; ++a) {
            const VertexAttribute& attr = dr.vertex->attributes[a];
            if (attr.stream >= vk::GraphicsPipelineDesc::kMaxBindings || a >= vk::GraphicsPipelineDesc::kMaxAttributes) {
                ok = false;
                break;
            }
            desc.attributes[desc.attribute_count++] = {static_cast<u32>(a), attr.stream, attribute_format(vs.inputs[a]), attr.offset};
            desc.binding_count = nx::max<u32>(desc.binding_count, attr.stream + 1u);
        }
        if (!ok) continue;
        for (u32 s = 0; s < desc.binding_count; ++s) {
            const VertexStream& st = s < dr.vertex->streams.size() ? dr.vertex->streams[s] : VertexStream{};
            const bool instanced = st.index_source >= 2;
            desc.bindings[s] = {st.stride, instanced ? 1u : 0u};
            u32 reach = 0;  // (bytes the attributes of this stream read from a vertex)
            for (usize a = 0; a < vs.inputs.size(); ++a)
                if (dr.vertex->attributes[a].stream == s)
                    reach = nx::max<u32>(reach, dr.vertex->attributes[a].offset + attribute_bytes(vs.inputs[a]));
            const u32 count = instanced ? dr.instances : max_index + 1;
            const u32 bytes = st.stride ? (count - 1) * st.stride + reach : reach;
            // (an attribute Vulkan cannot read where it is: the stream laid out again)
            bool offsets = true, strides = true;  // (aligned)
            for (usize a = 0; a < vs.inputs.size(); ++a) {
                if (dr.vertex->attributes[a].stream != s) continue;
                const u32 align = nx::max<u32>(vertex_input_bits(vs.inputs[a].format) / 8, 1);
                offsets &= dr.vertex->attributes[a].offset % align == 0;
                strides &= st.stride % align == 0;
            }
            if (offsets && strides) {
                plan.streams.push_back({s, vertex_range(dr.streams[s], bytes)});
                continue;
            }
            Renderer::Repack p;
            u32 cursor = 0;
            if (offsets) {
                // (only the stride: each vertex copied whole, its stride rounded up to four bytes)
                p.field[p.fields++] = {0, 0, nx::min<u32>(reach, st.stride ? st.stride : reach)};
                cursor = nx::max<u32>(reach, st.stride);
            } else {
                for (usize a = 0; a < vs.inputs.size() && p.fields < std::size(p.field); ++a) {
                    if (dr.vertex->attributes[a].stream != s) continue;
                    const u32 to = (cursor + 3) & ~3u, n = attribute_bytes(vs.inputs[a]);
                    p.field[p.fields++] = {dr.vertex->attributes[a].offset, to, n};
                    desc.attributes[a].offset = to;
                    cursor = to + n;
                }
            }
            p.from_stride = st.stride;
            p.to_stride = st.stride ? (cursor + 3) & ~3u : 0;
            p.count = st.stride ? count : 1;
            desc.bindings[s].stride = p.to_stride;
            plan.streams.push_back({s, repacked_range(dr.streams[s], bytes, p)});
        }
        switch (dr.primitive) {
            case 0x04000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case 0x08000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
            case 0x0C000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case 0x10000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
            default: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        const DrawState& st = dr.state;
        // (GXM's CW/CCW are the windings culled as they appear on the render target, which Vulkan's framebuffer
        // coordinates share: assumed until a sample with culling is checked)
        desc.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        desc.cull = st.cull == 1 ? VK_CULL_MODE_BACK_BIT : st.cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        const bool mask_update = dr.fragment->mask_update;
        if (has_depth && depth.image && mask_update) {
            // (no depth or stencil test or write; the stencil function's bit 0 is the mask bit to write)
            if (mask_bits) {
                desc.stencil_test = 1;
                desc.stencil_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS};
                desc.stencil_back = desc.stencil_front;
            }
        } else if (has_depth && depth.image) {
            desc.depth_test = 1;
            desc.depth_compare = static_cast<VkCompareOp>((st.depth_func[0] >> 22) & 7);
            desc.depth_write = st.depth_write[0] ? 1 : 0;
            auto stencil_state = [](const StencilState& s) {
                VkStencilOpState o{};
                o.compareOp = static_cast<VkCompareOp>((s.func >> 25) & 7);
                o.failOp = static_cast<VkStencilOp>(s.fail & 7);
                o.depthFailOp = static_cast<VkStencilOp>(s.depth_fail & 7);
                o.passOp = static_cast<VkStencilOp>(s.pass & 7);
                return o;
            };
            desc.stencil_front = stencil_state(st.stencil[0]);
            desc.stencil_back = stencil_state(st.stencil[st.two_sided ? 1 : 0]);
            auto active = [](const StencilState& s) { return ((s.func >> 25) & 7) != 7 || s.fail || s.depth_fail || s.pass; };
            desc.stencil_test = active(st.stencil[0]) || (st.two_sided && active(st.stencil[1])) ? 1 : 0;
            if (mask_bits) {
                // (the mask test: samples whose mask bit is 0 are not drawn)
                desc.stencil_test = 1;
                desc.stencil_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL};
                desc.stencil_back = desc.stencil_front;
            }
        }
        if (dr.fragment->blend_enabled && dr.fragment->blend.color_func()) {
            const BlendInfo& b = dr.fragment->blend;
            desc.blend.enable = 1;
            desc.blend.color_op = static_cast<VkBlendOp>(b.color_func() - 1);
            desc.blend.alpha_op = static_cast<VkBlendOp>((b.alpha_func() ? b.alpha_func() : b.color_func()) - 1);
            desc.blend.src_color = blend_factor(b.color_src());
            desc.blend.dst_color = blend_factor(b.color_dst());
            desc.blend.src_alpha = blend_factor(b.alpha_src());
            desc.blend.dst_alpha = blend_factor(b.alpha_dst());
        }
        if (const auto br = blend_read_of(dr, cf, color != nullptr)) {
            // (the program's read of the colour: its blend, of the output register's lanes, here)
            static_assert(u32{VK_BLEND_FACTOR_SRC_ALPHA_SATURATE} == u32{ColorReadBlend::kSrcAlphaSaturate} &&
                          u32{VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA} == u32{ColorReadBlend::kOneMinusDstAlpha} &&
                          u32{VK_BLEND_OP_MAX} == u32{ColorReadBlend::kMax} &&
                          u32{VK_BLEND_OP_REVERSE_SUBTRACT} == u32{ColorReadBlend::kReverseSubtract});
            desc.blend.enable = 1;
            desc.blend.src_color = static_cast<VkBlendFactor>(br->color_src);
            desc.blend.dst_color = static_cast<VkBlendFactor>(br->color_dst);
            desc.blend.color_op = static_cast<VkBlendOp>(br->color_op);
            desc.blend.src_alpha = static_cast<VkBlendFactor>(br->alpha_src);
            desc.blend.dst_alpha = static_cast<VkBlendFactor>(br->alpha_dst);
            desc.blend.alpha_op = static_cast<VkBlendOp>(br->alpha_op);
            u32 mask = 0;  // (the components whose lanes it writes; the others keep the colour)
            for (u32 c = 0; c < 4; ++c)
                if ((br->lanes >> cf.swizzle[c]) & 1) mask |= 1u << c;
            desc.blend.write_mask = mask;
        }
        if (dr.fragment->blend_enabled) {
            const u8 m = dr.fragment->blend.color_mask;
            desc.blend.write_mask = ((m & 2) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((m & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                    ((m & 8) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((m & 1) ? VK_COLOR_COMPONENT_A_BIT : 0);
        }
        // (a mask update program, or a disabled fragment program, writes no colour)
        if (mask_update || !st.fragment_enabled[0]) desc.blend.write_mask = 0;
        vk::Pass pass_formats;
        pass_formats.color.image = color;
        pass_formats.color.srgb = cf.srgb;
        pass_formats.color_input = reads_color;
        if (has_depth && depth.image) pass_formats.depth.image = &depth;
        desc.formats = pass_formats.formats();
        if (d.caps().dynamic_state) plan.state = desc;
        plan.topology = desc.topology;
        // (one not made yet: made with the scene's others after its draws are planned, on several threads)
        plan.pipeline = r.pipelines->find(desc);
        if (!plan.pipeline) {
            usize m = 0;
            while (m < missing.size() && !missing[m].desc.same(desc)) ++m;
            if (m == missing.size()) missing.push_back({desc});
            plan.missing = static_cast<u32>(m + 1);
        }

        DrawConstants& c = plan.constants;
        c.draw = i;
        c.flags = (st.viewport_enabled ? kDrawViewport : 0) | (desc.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? kDrawPoints : 0);
        c.target[0] = static_cast<f32>(target_w);
        c.target[1] = static_cast<f32>(target_h);
        std::memcpy(c.viewport, st.viewport, sizeof c.viewport);
        c.w_clamp = st.w_clamp_enabled ? st.w_clamp : 0.0f;
        c.scale = static_cast<f32>(scale);
        // Region clip (tile-aligned, as the hardware applies it) and the surface's clip, as a scissor.
        i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(width) - 1, y1 = static_cast<i32>(height) - 1;
        if (color) {
            x0 = static_cast<i32>(cs.clip_min & 0xFFFF), y0 = static_cast<i32>(cs.clip_min >> 16);
            x1 = static_cast<i32>(cs.clip_max & 0xFFFF), y1 = static_cast<i32>(cs.clip_max >> 16);
        }
        if (st.region_clip_mode == 0x80000000u) {
            x0 = nx::max<i32>(x0, static_cast<i32>(st.region_min[0] & ~31u));
            y0 = nx::max<i32>(y0, static_cast<i32>(st.region_min[1] & ~31u));
            x1 = nx::min<i32>(x1, static_cast<i32>(st.region_max[0] | 31u));
            y1 = nx::min<i32>(y1, static_cast<i32>(st.region_max[1] | 31u));
        }
        x1 = nx::min<i32>(x1, static_cast<i32>(target_w) - 1);
        y1 = nx::min<i32>(y1, static_cast<i32>(target_h) - 1);
        if (x1 < x0 || y1 < y0) continue;
        const i32 si = static_cast<i32>(scale);
        plan.scissor = {{x0 * si, y0 * si}, {static_cast<u32>(x1 - x0 + 1) * scale, static_cast<u32>(y1 - y0 + 1) * scale}};
        plans.push_back(std::move(plan));
    }
    // (a phone's driver: up to a tenth of a second for each pipeline whose shaders are new; a scene of a song's start
    // had 9 of them; made asynchronously without pipeline libraries, async_on)
    const bool async = async_on(r);
    if (!missing.empty()) {
        const u64 pipeline_from = nx::os::ticks_ns();
        if (async) make_async(r, missing);
        else make_graphics(r, missing);
        if (g_profile.on)
            g_profile.pipelines_made += static_cast<u32>(missing.size()), g_profile.pipeline_ns += nx::os::ticks_ns() - pipeline_from;
    }
    // (the new ones recorded: those made now, or asynchronously by now)
    nx::vector<GraphicsJob> made;
    if (async) made = async_made();
    for (const GraphicsJob& m : async ? std::span<const GraphicsJob>(made) : std::span<const GraphicsJob>(missing)) {
        if (!m.pipeline) {
            nx::logw(logs::gpu, "{}", m.error);
            continue;
        }
        // (a new one: recorded, to be made before the game starts next time)
        ShaderCache::Graphics g;
        g.desc = m.desc;
        g.desc.vertex = VK_NULL_HANDLE, g.desc.fragment = VK_NULL_HANDLE, g.desc.layout = VK_NULL_HANDLE;
        g.vertex = r.module_keys[reinterpret_cast<u64>(m.desc.vertex)];
        g.fragment = r.module_keys[reinterpret_cast<u64>(m.desc.fragment)];
        // (shaders compiled ahead: their SPIR-V saved now, for warm_up to make this one next time)
        for (const ShaderKey* k : {&g.vertex, &g.fragment})
            if (auto it = r.unsaved.find(short_key(*k)); it != r.unsaved.end()) {
                r.cache.add_spirv(*k, unpack_words(it->second.second));
                r.unsaved.erase(it);
            }
        r.cache.add_graphics(g);
        r.made_pipelines.fetch_add(1);
        r.pipelines_changed.store(true);
    }
    if (!missing.empty()) {
        for (DrawPlan& plan : plans)
            if (plan.missing) plan.pipeline = missing[plan.missing - 1].pipeline;
        // (those not made: a failure, or still being made asynchronously, drawn by the scenes after)
        const usize planned = plans.size();
        std::erase_if(plans, [](const DrawPlan& plan) { return !plan.pipeline; });
        if (g_profile.on) g_profile.skipped += planned - plans.size();
    }

    g_profile.mark(1);
    // ---- Buffers ---------------------------------------------------------------------------------------------
    nx::vector<u32> mem_words(2 + mem.size() / 4 + 2, 0);
    mem_words[0] = kMemoryBase;
    mem_words[1] = static_cast<u32>(mem.size() / 4);
    if (!mem.empty()) std::memcpy(mem_words.data() + 2, mem.data(), mem.size());
    nx::vector<vk::Buffer> buffers;
    auto make = [&](const void* data, usize bytes, VkBufferUsageFlags usage) -> vk::Buffer* {
        (void)usage;
        auto b = take_host_buffer(r, nx::max<usize>(bytes, 16));
        if (!b) return nullptr;
        if (data && bytes) std::memcpy(b->mapped, data, bytes);
        vk::flush(d, *b, nx::max<usize>(bytes, 16));
        buffers.push_back(*b);
        return &buffers.back();
    };
    buffers.reserve(6);
    // The secondary programs, a dispatch for each program (and stage) the scene's draws run, a workgroup a draw: the
    // draws' indices in a table after their secondary attributes (a dispatch a draw took 3 ms of a phone's GPU a scene)
    struct SecondaryBatch {
        VkPipeline pipeline;
        u32 stage, table, count;
        usize first;  // (a draw of the batch: its descriptor set binds the scene's buffers)
    };
    nx::vector<SecondaryBatch> secondary_batches;
    {
        nx::vector<u32> table;
        for (u32 stage = 0; stage < 2; ++stage)
            for (usize i = 0; i < plans.size(); ++i) {
                const VkPipeline p = stage ? plans[i].fragment_secondary : plans[i].vertex_secondary;
                if (!p) continue;
                bool found = false;
                for (const SecondaryBatch& b : secondary_batches) found |= b.pipeline == p && b.stage == stage;
                if (found) continue;
                SecondaryBatch b{p, stage, static_cast<u32>(sa.size() + table.size()), 0, i};
                for (usize j = i; j < plans.size(); ++j)
                    if ((stage ? plans[j].fragment_secondary : plans[j].vertex_secondary) == p)
                        table.push_back(static_cast<u32>(plans[j].constants.draw)), ++b.count;
                secondary_batches.push_back(b);
            }
        sa.insert(sa.end(), table.begin(), table.end());
    }
    vk::Buffer* sa_buf = make(sa.data(), sa.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // (past the device's storage buffer range, what the programs read there is undefined: zeros on an Adreno)
    if (static bool told = false;
        !told && u64{mem_words.size()} * 4 > d.properties().limits.maxStorageBufferRange) {
        told = true;
        nx::logw(logs::gpu, "a scene's memory buffer of {} MiB is past the device's storage buffer range ({} MiB)",
                 u64{mem_words.size()} * 4 >> 20, d.properties().limits.maxStorageBufferRange >> 20);
    }
    vk::Buffer* mem_buf = make(mem_words.data(), mem_words.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vk::Buffer* vb = make(nullptr, static_cast<usize>(vertex_bytes), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (vb) {
        // (straight from guest memory into the buffer)
        u8* out = static_cast<u8*>(vb->mapped);
        for (const Renderer::VertexRange& v : vertex_ranges) {
            if (v.address && v.repack != ~0u) {
                const Renderer::Repack& p = repacks[v.repack];
                const u8* from = memory().host(v.address);
                u8* to = out + v.offset;
                std::memset(to, 0, v.bytes);
                for (u32 i = 0; i < p.count; ++i)
                    for (u32 f = 0; f < p.fields; ++f)
                        std::memcpy(to + usize{i} * p.to_stride + p.field[f].to, from + usize{i} * p.from_stride + p.field[f].from,
                                    p.field[f].bytes);
            } else if (v.address) {
                std::memcpy(out + v.offset, memory().host(v.address), v.bytes);
            } else {
                std::memset(out + v.offset, 0, v.bytes);
            }
        }
        vk::flush(d, *vb, vertex_bytes);
    }
    vk::Buffer* ib = make(indices.data(), indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    const VkDeviceSize readback_bytes = color ? VkDeviceSize{width} * height * cf.bytes : 0;
    vk::Buffer* readback = make(nullptr, readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // (depth and stencil, to load and store: the depth words, then the stencil bytes)
    // (at the internal resolution: the surface's samples scaled up on loading, down on storing)
    const VkDeviceSize samples = VkDeviceSize{draw_w} * draw_h;
    vk::Buffer* depth_buf = nullptr;
    if (load_depth || store_depth) {
        nx::vector<u8> init;
        if (load_depth && !depth_kept) {
            const usize n = usize{width} * height;
            init.resize(n * 5);
            read_depth_surface(ds, dl, width, height, unorm24, reinterpret_cast<u32*>(init.data()), init.data() + n * 4);
            if (scale > 1) init = scale_up_samples(init, width, height, scale);
        }
        // (at the internal resolution, a row of samples for each of the surface's rows: scale_up_samples,
        // scale_down_samples)
        depth_buf = make(!init.empty() ? init.data() : nullptr, VkDeviceSize{draw_w} * height * 5,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    auto cleanup = [&] {
        for (vk::Buffer& b : buffers) give_host_buffer(r, b);
        if (depth_keeper) {
            depth_keeper->image.layout = depth.layout;  // (kept)
        } else if (depth.image) {
            free_depth_image(r, depth);  // (nothing submitted uses it)
        }
    };
    if (!sa_buf || !mem_buf || !vb || !ib || !readback || ((load_depth || store_depth) && !depth_buf)) {
        cleanup();
        return false;
    }

    g_profile.mark(2);
    nx::hash_map<u64, u64> seen_textures;  // (texture_for's, for this scene)
    // ---- Descriptor sets (one per draw: the same buffers, the textures to come) ------------------------------
    const u32 sets = nx::max<u32>(static_cast<u32>(plans.size()), 1);
    const VkDescriptorPool pool = take_descriptor_pool(r, sets);
    nx::vector<VkDescriptorSet> descriptor_sets(plans.size(), VK_NULL_HANDLE);
    nx::vector<Target*> sampled;  // (surfaces drawn at the internal resolution the draws read as textures)
    nx::vector<DepthTarget*> sampled_depth;
    // (draws binding the same textures share a set: made once a scene, not written again for each)
    nx::hash_map<u64, VkDescriptorSet> same_sets;
    // (at the internal resolution: the scaled surface a texture's words read, found once a scene: finding it compares
    // the surface with guest memory)
    struct Scaled {
        DepthTarget* depth;
        Target* color;
    };
    nx::hash_map<u64, Scaled> scaled_of;
    auto scaled_for = [&](const TextureWords& words) {
        nx::fnv1a64 wh;
        for (u32 w : words.words) wh.combine(w);
        if (auto it = scaled_of.find(wh.value()); it != scaled_of.end()) return it->second;
        const TextureInfo info = decode_texture(words.words);
        Scaled sc{nullptr, nullptr};
        sc.depth = scaled_depth_texture(r, info, depth_target);
        sc.color = sc.depth ? nullptr : scaled_texture(r, info, target);
        scaled_of[wh.value()] = sc;
        return sc;
    };
    // The textures the draws sample, looked up as the sets below do, in their order: those to decode are decoded on
    // several threads, then made, and the sets find them made (a song's start decoded about 50 new ones, 40-65 ms on
    // a phone one after another).
    {
        nx::vector<TextureMiss> misses;
        for (usize i = 0; i < plans.size(); ++i) {
            const Draw& dr = *plans[i].draw;
            for (u32 stage = 0; stage < 2; ++stage) {
                const Program& p = stage ? *dr.fragment->program : *dr.vertex->program;
                for (const usse::Parameter& q : p.gxp.parameters) {
                    if (q.category != usse::ParameterCategory::Sampler || q.resource >= (stage ? 16u : 4u)) continue;
                    const TextureWords& words = stage ? dr.fragment_textures[q.resource] : dr.vertex_textures[q.resource];
                    if (state().options->render_scale > 1 && !q.sampler_cube) {
                        const Scaled sc = scaled_for(words);
                        if (sc.depth || sc.color) continue;
                    }
                    texture_for(r, words, seen_textures, &misses);
                }
            }
        }
        if (!misses.empty()) {
            const bool bc = r.device->caps().texture_bc, srgb = srgb_textures(*r.device);
            const u64 decode_from = g_profile.on ? nx::os::ticks_ns() : 0;
            // (those decoded ahead, as they were set up, taken; the rest decoded here)
            nx::vector<usize> order;
            for (usize k = 0; k < misses.size(); ++k) {
                if (std::optional<Texels> t = take_prefetched(misses[k].info, misses[k].contents)) {
                    misses[k].texels = std::move(*t);
                    misses[k].ahead = true;
                    if (g_profile.on) ++g_profile.ahead;
                } else {
                    order.push_back(k);
                }
            }
            // (the largest first: the threads finish together, not one of them with a large one left at the end)
            std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return misses[a].size > misses[b].size; });
            in_parallel(order.size(), [&](usize i) {
                const usize k = order[i];
                const u64 from = g_profile.on ? nx::os::ticks_ns() : 0;
                const TextureInfo& info = misses[k].info;
                misses[k].texels = read_texels(info, read_guest_memory, bc, true, srgb);
                // (one that took a while to decode: which)
                if (const u64 took = g_profile.on ? nx::os::ticks_ns() - from : 0; took >= 1'000'000)
                    std::fprintf(stderr, "gxm profile: a slow decode, %.1f ms: format %08x type %u gamma %u, %u x %u, %u levels, "
                                 "%u bytes\n", static_cast<f64>(took) / 1e6, info.base_format, info.type, info.gamma,
                                 info.width, info.height, info.levels, misses[k].size);
            });
            if (g_profile.on) g_profile.decode_ns += nx::os::ticks_ns() - decode_from;
            for (TextureMiss& m : misses) make_texture(r, m.info, m.size, m.key, m.words_key, &m.texels);
        }
    }
    // (the colour as an input attachment: in the layout the pass leaves it in)
    vk::PassFormats input_formats;
    input_formats.color_input = reads_color ? 1 : 0;
    const VkImageLayout input_layout = reads_color ? r.passes->color_layout(input_formats) : VK_IMAGE_LAYOUT_GENERAL;
    for (usize i = 0; i < plans.size(); ++i) {
        VkDescriptorBufferInfo bi[2] = {{sa_buf->buffer, 0, VK_WHOLE_SIZE}, {mem_buf->buffer, 0, VK_WHOLE_SIZE}};
        // The units each program declares: the draw's texture there, or a white texel of the declared kind.
        VkDescriptorImageInfo ii[kDrawBindings];
        for (u32 b = kDrawBindingFragmentTextures; b < kDrawBindings; ++b)
            ii[b] = {r.sampler, r.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (color) ii[kDrawBindingColor] = {VK_NULL_HANDLE, cf.srgb ? color->srgb_view : color->view, input_layout};
        for (u32 stage = 0; stage < 2; ++stage) {
            const Draw& dr = *plans[i].draw;
            const Program& p = stage ? *dr.fragment->program : *dr.vertex->program;
            for (const usse::Parameter& q : p.gxp.parameters) {
                if (q.category != usse::ParameterCategory::Sampler) continue;
                const u32 unit = q.resource;
                if (unit >= (stage ? 16u : 4u)) continue;
                const u32 binding = (stage ? kDrawBindingFragmentTextures : kDrawBindingVertexTextures) + unit;
                ii[binding].imageView = q.sampler_cube ? r.white_cube.view : r.white.view;
                const TextureWords& words = stage ? dr.fragment_textures[unit] : dr.vertex_textures[unit];
                const Scaled sc = state().options->render_scale > 1 && !q.sampler_cube ? scaled_for(words) : Scaled{nullptr, nullptr};
                // (a depth surface stored at the internal resolution: its depth read there)
                if (DepthTarget* dt = sc.depth) {
                    const TextureInfo info = decode_texture(words.words);
                    ii[binding].imageView = dt->depth_view;
                    const VkSampler smp = texture_sampler(r, info, dt->image.desc.format, 1);
                    ii[binding].sampler = smp ? smp : r.sampler;
                    if (std::find(sampled_depth.begin(), sampled_depth.end(), dt) == sampled_depth.end()) sampled_depth.push_back(dt);
                    continue;
                }
                // (a surface drawn at the internal resolution: read there)
                if (Target* st = sc.color) {
                    const TextureInfo info = decode_texture(words.words);
                    // (a texture with a gamma mode: its sRGB view, which scaled_texture found it has)
                    ii[binding].imageView = info.gamma ? st->scaled.srgb_view : st->scaled.view;
                    const VkSampler smp = texture_sampler(r, info, texture_format(info, st->scaled.desc.format), 1);
                    ii[binding].sampler = smp ? smp : r.sampler;
                    if (std::find(sampled.begin(), sampled.end(), st) == sampled.end()) sampled.push_back(st);
                    continue;
                }
                if (CachedTexture* t = texture_for(r, words, seen_textures); t && t->image.desc.cube == q.sampler_cube) {
                    ii[binding].imageView = t->image.view;
                    ii[binding].sampler = t->sampler ? t->sampler : r.sampler;
                }
            }
        }
        const u32 writes = reads_color ? kDrawBindings : kDrawBindingColor;  // (no input attachment outside such passes)
        nx::fnv1a64 same;
        for (u32 b = kDrawBindingFragmentTextures; b < writes; ++b)
            same.combine_all(reinterpret_cast<u64>(ii[b].imageView), reinterpret_cast<u64>(ii[b].sampler));
        if (auto it = same_sets.find(same.value()); it != same_sets.end()) {
            descriptor_sets[i] = it->second;
            continue;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r.set_layout;
        vkAllocateDescriptorSets(d.device(), &ai, &descriptor_sets[i]);
        same_sets[same.value()] = descriptor_sets[i];
        VkWriteDescriptorSet w[kDrawBindings]{};
        for (u32 b = 0; b < writes; ++b) {
            w[b] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[b].dstSet = descriptor_sets[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            if (b < kDrawBindingFragmentTextures) {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[b].pBufferInfo = &bi[b];
            } else if (b == kDrawBindingColor) {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                w[b].pImageInfo = &ii[b];
            } else {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &ii[b];
            }
        }
        vkUpdateDescriptorSets(d.device(), writes, w, 0, nullptr);
    }

    g_profile.mark(3);
    // ---- Visibility queries ----------------------------------------------------------------------------------
    VkQueryPool queries = VK_NULL_HANDLE;
    nx::vector<u32> query_of(plans.size(), ~0u);
    u32 query_count = 0;
    if (scene.visibility_buffer)
        for (usize i = 0; i < plans.size(); ++i)
            if (plans[i].draw->state.visibility[0]) query_of[i] = query_count++;
    if (query_count) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = query_count;
        if (vkCreateQueryPool(d.device(), &qi, nullptr, &queries) != VK_SUCCESS) {
            queries = VK_NULL_HANDLE;
            query_count = 0;
        }
    }

    // ---- Commands --------------------------------------------------------------------------------------------
    auto shot = nx::make_unique<vk::OneShot>(d);
    auto cmd = shot->begin();
    // (PSVRECORE_GXM_PROFILE: the start and end of the scene's commands on the GPU)
    VkQueryPool stamps = VK_NULL_HANDLE;
    if (g_profile.on && cmd && d.caps().timestamp_period > 0) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;
        if (vkCreateQueryPool(d.device(), &qi, nullptr, &stamps) != VK_SUCCESS) stamps = VK_NULL_HANDLE;
        if (stamps) {
            vkCmdResetQueryPool(*cmd, stamps, 0, 4);
            vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamps, 0);
        }
    }
    if (!cmd) {
        if (queries) vkDestroyQueryPool(d.device(), queries, nullptr);
        give_descriptor_pool(r, pool, sets);
        cleanup();
        return false;
    }
    // (the scene in a GPU debugger's list of commands, and in Tracy: its number, surface and internal resolution)
    if (d.caps().labels) {
        const nx::string label = nx::format("scene {}: {}x{} {} 0x{:08x}{}", number, width, height,
                                            scene.has_color ? "colour" : "depth", scene.has_color ? cs.data : ds.depth_data,
                                            scale > 1 ? nx::format(" at {}x", scale) : nx::string());
        d.begin_label(*cmd, label.c_str(), kSceneRgb);
    }
#if PSVRECORE_TRACY
    std::optional<tracy::VkCtxScope> gpu_scene;
    if (r.gpu_zones) {
        TracyVkCollect(r.gpu_zones, *cmd);
        PSV_PROFILE_LOCATION(kSceneZone, "scene", kSceneRgb);
        gpu_scene.emplace(r.gpu_zones, &kSceneZone, *cmd, true);
    }
#endif
    if (queries) vkCmdResetQueryPool(*cmd, queries, 0, query_count);
    const VkShaderStageFlags push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    bool computed = false;
    for (const SecondaryBatch& b : secondary_batches) {
        DrawConstants c = plans[b.first].constants;
        c.draw = b.table;
        c.stage = b.stage;
        vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipeline);
        vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0, 1, &descriptor_sets[b.first], 0, nullptr);
        vkCmdPushConstants(*cmd, r.layout, push_stages, 0, sizeof c, &c);
        vkCmdDispatch(*cmd, b.count, 1, 1);
        computed = true;
    }
    if (computed) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    if (scale > 1) {
        // (the tiles of the surface's colour that changed, scaled up into the scaled image: before the copies, so a
        // texture made from the surface this scene draws holds what the scene starts from)
        if (!changes.tiles.empty()) {
            vk::transition(*cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vk::transition(*cmd, target->scaled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (changes.all) {
                blit(*cmd, target->image, target->scaled, VK_FILTER_NEAREST);
            } else {
                nx::vector<VkImageBlit> regions;
                regions.reserve(changes.tiles.size());
                const i32 s = static_cast<i32>(scale);
                for (const VkRect2D& t : changes.tiles) {
                    VkImageBlit b{};
                    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    const i32 x1 = t.offset.x + static_cast<i32>(t.extent.width), y1 = t.offset.y + static_cast<i32>(t.extent.height);
                    b.srcOffsets[0] = {t.offset.x, t.offset.y, 0};
                    b.srcOffsets[1] = {x1, y1, 1};
                    b.dstOffsets[0] = {t.offset.x * s, t.offset.y * s, 0};
                    b.dstOffsets[1] = {x1 * s, y1 * s, 1};
                    regions.push_back(b);
                }
                vkCmdBlitImage(*cmd, target->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->scaled.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(regions.size()), regions.data(),
                               VK_FILTER_NEAREST);
            }
        }
    }
    record_target_copies(r, *cmd);
    copies_recorded = true;
    // (the scaled surfaces the draws read, as textures)
    for (Target* t : sampled) vk::transition(*cmd, t->scaled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (DepthTarget* t : sampled_depth) vk::transition(*cmd, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkBufferImageCopy depth_regions[2] = {
        {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {draw_w, draw_h, 1}},
        {samples * 4, 0, 0, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0}, {draw_w, draw_h, 1}}};
    if (load_depth && !depth_kept && depth.image) {
        vk::transition(*cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        nx::vector<VkBufferImageCopy> from_memory;
        for (u32 a = 0; a < 2; ++a) {
            if (aspect_from[a]) continue;
            if (scale == 1) {
                from_memory.push_back(depth_regions[a]);
                continue;
            }
            // (each row of the buffer into scale rows of samples)
            const VkDeviceSize base = a ? VkDeviceSize{draw_w} * height * 4 : 0, bytes = a ? 1 : 4;
            const VkImageAspectFlags aspect = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
            for (u32 y = 0; y < draw_h; ++y)
                from_memory.push_back({base + VkDeviceSize{y / scale} * draw_w * bytes, 0, 0, {aspect, 0, 0, 1},
                                       {0, static_cast<i32>(y), 0}, {draw_w, 1, 1}});
        }
        if (!from_memory.empty())
            vkCmdCopyBufferToImage(*cmd, depth_buf->buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<u32>(from_memory.size()), from_memory.data());
        for (u32 a = 0; a < 2; ++a) {
            if (!aspect_from[a]) continue;
            const VkImageAspectFlags aspect = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
            vk::transition(*cmd, aspect_from[a]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const VkImageCopy copy{{aspect, 0, 0, 1}, {0, 0, 0}, {aspect, 0, 0, 1}, {0, 0, 0}, {draw_w, draw_h, 1}};
            vkCmdCopyImage(*cmd, aspect_from[a]->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
    }
    if (kept_depth && depth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        // (drawn with by a scene before, without a transition after: its tests and writes before this one's)
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        constexpr VkPipelineStageFlags tests =
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        vkCmdPipelineBarrier(*cmd, tests, tests, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    vk::Pass pass;
    if (color) pass.color = {color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {}, nullptr, cf.srgb};
    pass.color_input = reads_color;
    if (has_depth && depth.image) {
        pass.depth.image = &depth;
        pass.depth.clear_depth = nx::clamp(ds.background_depth, 0.0f, 1.0f);
        pass.depth.clear_stencil = mask_bits ? (ds.background_control >> 8) & 1 : ds.background_control & 0xFF;
        if (load_depth) pass.depth.load = pass.depth.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (store_depth) pass.depth.store = pass.depth.stencil_store = VK_ATTACHMENT_STORE_OP_STORE;
    }
    pass.area = {{0, 0}, {target_w * scale, target_h * scale}};
    if (stamps) vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps, 1);
    d.begin_label(*cmd, "draws", kPassRgb);
    MarkerReplay markers{d, *cmd, scene.markers};
#if PSVRECORE_TRACY
    markers.zones_ctx = r.gpu_zones;
#endif
    if (auto begun = r.passes->begin(*cmd, pass); begun) {
        const VkViewport viewport{0, 0, static_cast<f32>(target_w * scale), static_cast<f32>(target_h * scale), 0, 1};
        vkCmdSetViewport(*cmd, 0, 1, &viewport);
        const f32 blend[4] = {0, 0, 0, 0};
        vkCmdSetBlendConstants(*cmd, blend);
        // (the state the draw before set: what a draw would set again the same is not recorded again)
        struct Last {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkRect2D scissor{{-1, -1}, {0, 0}};
            u32 ref[2] = {~0u, ~0u}, compare[2] = {~0u, ~0u}, write[2] = {~0u, ~0u};
            f32 bias[2] = {-1e30f, -1e30f};
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDeviceSize streams[16];
            VkDeviceSize index_offset = ~VkDeviceSize{0};
            VkIndexType index_type = VK_INDEX_TYPE_MAX_ENUM;
        } last;
        for (VkDeviceSize& s : last.streams) s = ~VkDeviceSize{0};
        auto stencil = [&](u32 face, u32 ref, u32 compare, u32 write) {
            const VkStencilFaceFlags f = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            if (last.ref[face] != ref) vkCmdSetStencilReference(*cmd, f, ref), last.ref[face] = ref;
            if (last.compare[face] != compare) vkCmdSetStencilCompareMask(*cmd, f, compare), last.compare[face] = compare;
            if (last.write[face] != write) vkCmdSetStencilWriteMask(*cmd, f, write), last.write[face] = write;
        };
        for (usize i = 0; i < plans.size(); ++i) {
            const DrawPlan& p = plans[i];
            const DrawState& st = p.draw->state;
            if (!scene.markers.empty()) markers.upto(static_cast<u32>(p.draw - scene.draws.data()));
            const bool reads = reads_attachment(*p.draw, cf, color != nullptr);
            if (g_profile.on) ++g_profile.draws, g_profile.colour_reads += reads;
            if (reads && i > 0 && !d.caps().raster_order_attachment) {
                if (g_profile.on) ++g_profile.barriers;
                // (what the draws before wrote, for this one to read: the render pass's self-dependency)
                vk::color_read_barrier(*cmd);
            }
            if (last.pipeline != p.pipeline) vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline), last.pipeline = p.pipeline;
            if (d.caps().dynamic_state) vk::set_dynamic_state(*cmd, p.state);
            if (std::memcmp(&last.scissor, &p.scissor, sizeof p.scissor) != 0)
                vkCmdSetScissor(*cmd, 0, 1, &p.scissor), last.scissor = p.scissor;
            const StencilState& front = st.stencil[0];
            const StencilState& back = st.stencil[st.two_sided ? 1 : 0];
            if (mask_bits) {
                const bool update = p.draw->fragment->mask_update;
                stencil(0, update ? (front.func >> 25) & 1 : 1, 1, update ? 1 : 0);
                stencil(1, update ? (back.func >> 25) & 1 : 1, 1, update ? 1 : 0);
            } else {
                stencil(0, front.ref, front.compare_mask, front.write_mask);
                stencil(1, back.ref, back.compare_mask, back.write_mask);
            }
            const f32 bias[2] = {static_cast<f32>(st.depth_bias_units[0]), static_cast<f32>(st.depth_bias_factor[0])};
            if (bias[0] != last.bias[0] || bias[1] != last.bias[1]) {
                vkCmdSetDepthBias(*cmd, bias[0], 0.0f, bias[1]);
                last.bias[0] = bias[0], last.bias[1] = bias[1];
            }
            if (last.set != descriptor_sets[i]) {
                vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 1, &descriptor_sets[i], 0, nullptr);
                last.set = descriptor_sets[i];
            }
            vkCmdPushConstants(*cmd, r.layout, push_stages, 0, sizeof p.constants, &p.constants);
            for (const auto& [binding, offset] : p.streams) {
                if (binding < 16 && last.streams[binding] == offset) continue;
                vkCmdBindVertexBuffers(*cmd, binding, 1, &vb->buffer, &offset);
                if (binding < 16) last.streams[binding] = offset;
            }
            if (last.index_offset != p.index_offset || last.index_type != p.index_type) {
                vkCmdBindIndexBuffer(*cmd, ib->buffer, p.index_offset, p.index_type);
                last.index_offset = p.index_offset, last.index_type = p.index_type;
            }
            const bool query = queries && query_of[i] != ~0u;
            // (precise: increments count samples; a set needs only whether any passed)
            if (query)
                vkCmdBeginQuery(*cmd, queries, query_of[i],
                                !st.visibility_set[0] && d.caps().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
            // (a draw whose primitives read the colour they overlap is ordered by hand without the device's help)
            if (reads && p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                vk::draw_color_reading(*cmd, d.caps().raster_order_attachment, true, p.draw->index_count, p.draw->instances);
            else
                vkCmdDrawIndexed(*cmd, p.draw->index_count, p.draw->instances, 0, 0, 0);
            if (query) vkCmdEndQuery(*cmd, queries, query_of[i]);
        }
        markers.close();  // (the labels end in the render pass they began in)
        r.passes->end(*cmd);
        if (stamps) vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps, 2);
    } else {
        nx::logw(logs::gpu, "{}", begun.error().message);
    }
    d.end_label(*cmd);  // (draws)
    d.begin_label(*cmd, "write-back", kPassRgb);
    vk::DeviceBuffer pack_work{};  // (an HDR surface packed on the GPU: the work buffer and set; the texels to verify)
    VkDescriptorSet pack_set = VK_NULL_HANDLE;
    vk::Buffer pack_verify{};
    if (color) {
        // (at the internal resolution, blitted down to the surface's size first: what guest memory gets)
        vk::transition(*cmd, *color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (scale > 1) {
            vk::transition(*cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            blit(*cmd, target->scaled, target->image, VK_FILTER_LINEAR);
            vk::transition(*cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        // (an HDR surface: its words packed on the GPU, as pack_u2f10f10f10 packs them, half the bytes read back and
        // none packed on the CPU; else the image's texels, packed when the scene is finished)
        if (cf.packing == Packing::U2F10F10F10 && r.converter.ready()) {
            const TargetTextureConstants k = target_pack_constants(width, height);
            const VkDeviceSize work_bytes = (VkDeviceSize{k.out} + VkDeviceSize{width} * height) * 4;
            if (auto w = take_work(r, work_bytes)) {
                pack_work = *w;
                pack_set = target_converter_set(d, r.converter, pack_work.buffer, work_bytes, VK_NULL_HANDLE, 0);
                if (!pack_set) give_work(r, pack_work);
            }
            if (pack_set) {
                auto barrier = [&](VkPipelineStageFlags from, VkAccessFlags from_access, VkPipelineStageFlags to,
                                   VkAccessFlags to_access) {
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = from_access;
                    mb.dstAccessMask = to_access;
                    vkCmdPipelineBarrier(*cmd, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
                };
                // (the work buffer: earlier scenes' commands may have used it)
                barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                vkCmdCopyImageToBuffer(*cmd, target->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pack_work.buffer, 1, &copy);
                barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                record_target_conversion(*cmd, r.converter, pack_set, k);
                barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
                const VkBufferCopy words{VkDeviceSize{k.out} * 4, 0, VkDeviceSize{width} * height * 4};
                vkCmdCopyBuffer(*cmd, pack_work.buffer, readback->buffer, 1, &words);
                // (PSVRECORE_TARGET_TEXTURES=verify: the texels too, packed on the CPU when the scene is done)
                if (verify_conversions()) {
                    if (auto v = take_host_buffer(r, VkDeviceSize{width} * height * cf.bytes)) {
                        pack_verify = *v;
                        vkCmdCopyImageToBuffer(*cmd, target->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               pack_verify.buffer, 1, &copy);
                    }
                }
            }
        }
        if (!pack_set)
            vkCmdCopyImageToBuffer(*cmd, target->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
    }
    if (store_depth && depth.image) {
        vk::transition(*cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (scale > 1) {
            // (at the internal resolution, only the rows of samples guest memory gets one of: scale times less)
            nx::vector<VkBufferImageCopy> rows(usize{height} * 2);
            const VkDeviceSize stencil = VkDeviceSize{draw_w} * height * 4;
            for (u32 y = 0; y < height; ++y) {
                const i32 at = static_cast<i32>(y * scale + scale / 2);
                rows[y] = {VkDeviceSize{y} * draw_w * 4, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, at, 0}, {draw_w, 1, 1}};
                rows[height + y] = {stencil + VkDeviceSize{y} * draw_w, 0, 0, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
                                    {0, at, 0}, {draw_w, 1, 1}};
            }
            vkCmdCopyImageToBuffer(*cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_buf->buffer,
                                   static_cast<u32>(rows.size()), rows.data());
        } else {
            vkCmdCopyImageToBuffer(*cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_buf->buffer, 2,
                                   depth_regions);
        }
    }
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;  // (readbacks; stores)
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    if (stamps) vkCmdWriteTimestamp(*cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps, 3);
    d.end_label(*cmd);  // (write-back)
#if PSVRECORE_TRACY
    gpu_scene.reset();
#endif
    d.end_label(*cmd);  // (the scene)
    g_profile.mark(4);
    // (the layouts as recorded: the kept depth image's goes with its surface)
    if (depth_keeper) depth_keeper->image.layout = depth.layout;
    if (color) ++target->generation;
    // What the scene leaves in guest memory, and the visibility counters it adds to or sets.
    Renderer::Pending pending;
    if (color) pending.writes.push_back({cs.data, static_cast<u32>(layout.span())});
    if (store_depth && depth.image) {
        // (the bytes the store writes: the layout's a sample, of the depth and of the stencil. Taken as 4 of depth
        // and one of stencil whatever the layout, WipEout 2048's D16 shadow map (256 x 256) covered the 128 x 128
        // surface after it, which the next scene draws: that scene waited for the GPU to finish all it had, a frame.)
        const DepthRanges g = depth_ranges(ds_store, dl, width, height);
        if (g.depth_bytes) pending.writes.push_back({g.depth, g.depth_bytes});
        if (g.stencil_bytes) pending.writes.push_back({g.stencil, g.stencil_bytes});
    }
    for (const auto& [address, c] : buffer_copies)
        if (c.written && c.placed) pending.writes.push_back({address, c.bytes});
    struct Counter {
        u32 at, query;
        bool set;
    };
    nx::vector<Counter> counters;
    for (usize i = 0; i < plans.size(); ++i) {
        if (query_of[i] == ~0u) continue;
        const DrawState& st = plans[i].draw->state;
        const u32 at = scene.visibility_buffer + st.visibility_index[0] * 4;
        counters.push_back({at, query_of[i], st.visibility_set[0] != 0});
        pending.writes.push_back({at, 4});
    }
    pending.target = color ? target : nullptr;
    pending.kept_depth = kept_depth;
    pending.stores_kept_depth = kept_depth && store_depth && !scene.store_elsewhere;
    pending.depth_sources[0] = aspect_from[0], pending.depth_sources[1] = aspect_from[1];
    for (const Renderer::TargetConversion& c : r.target_conversions)
        if (c.depth && std::find(pending.converted_from.begin(), pending.converted_from.end(), c.depth) == pending.converted_from.end())
            pending.converted_from.push_back(c.depth);
    const bool stores_depth = store_depth && depth.image;
    pending.finish = [&r, &d, stamps, color = color != nullptr, readback, mem_buf, depth_buf, buffers = std::move(buffers),
                      layout, data = cs.data, width, height, bytes = cf.guest_bytes(), image_bytes = cf.bytes, packing = cf.packing, target,
                      buffer_copies = std::move(buffer_copies), queries, query_count, counters = std::move(counters), scale,
                      stores_depth, ds = ds_store, dl, unorm24, samples, depth_target,
                      kept_depth, kept_stores = !scene.store_elsewhere, depth, pool, sets,
                      notifications = std::move(notifications), copies = std::move(r.target_copies),
                      uploads = std::move(r.texture_uploads), conversions = std::move(r.target_conversions), pack_work,
                      pack_set, pack_verify](bool ok) mutable {
        if (ok && stamps) {
            u64 ts[4] = {};
            if (vkGetQueryPoolResults(d.device(), stamps, 0, 4, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const f64 period = d.caps().timestamp_period;
                g_profile.gpu_before_ns += static_cast<u64>(static_cast<f64>(ts[1] - ts[0]) * period);
                g_profile.gpu_pass_ns += static_cast<u64>(static_cast<f64>(ts[2] - ts[1]) * period);
                g_profile.gpu_after_ns += static_cast<u64>(static_cast<f64>(ts[3] - ts[2]) * period);
                const u64 last = g_profile.gpu_last_end;
                if (last && ts[0] > last) {
                    const u64 gap = static_cast<u64>(static_cast<f64>(ts[0] - last) * period);
                    g_profile.gpu_idle_ns += gap;
                    const u32 bucket = gap < 100000 ? 0 : gap < 500000 ? 1 : gap < 2000000 ? 2 : 3;
                    ++g_profile.gap_count[bucket], g_profile.gap_ns[bucket] += gap;
                }
                const u64 from = last && last > ts[0] ? last : ts[0];
                if (ts[3] > from) g_profile.gpu_span_ns += static_cast<u64>(static_cast<f64>(ts[3] - from) * period);
                g_profile.gpu_last_end = ts[3];
                ++g_profile.gpu_measured;
            }
        }
        u64 back_at = g_profile.on ? nx::os::ticks_ns() : 0;
        auto back_mark = [&](u32 part) {
            if (!g_profile.on) return;
            const u64 n = nx::os::ticks_ns();
            g_profile.back_ns[part] += n - back_at;
            back_at = n;
        };
        if (ok && color) {
            vk::invalidate(d, *readback, VkDeviceSize{width} * height * (pack_set ? 4 : image_bytes));
            const u8* src = static_cast<const u8*>(readback->mapped);
            nx::vector<u8> packed;
            const bool gpu_packed = pack_set != VK_NULL_HANDLE;  // (the words guest memory gets, read back)
            if (gpu_packed && pack_verify.buffer) verify_packing(d, pack_verify, src, width, height);
            const u32 span = static_cast<u32>(layout.span());
            const bool lazy = lazy_write_backs();
            // (the bytes become `written`: the packed ones, or the read-back buffer itself, kept rather than copied;
            // what guest memory holds once written back, to know whether the image (or the scaled one) is still the
            // surface. A deferred write-back of the surface not written yet is superseded: dropped with its bytes.)
            std::unique_lock<std::mutex> held(g_lazy_lock, std::defer_lock);
            if (lazy) {
                held.lock();
                std::erase_if(g_lazy, [&](const LazyWriteBack& e) { return e.target == target; });
            }
            if (packing != Packing::None && !gpu_packed) {
                packed = target->written.take_bytes();  // (its memory, packed into)
                packed.resize(usize{width} * height * bytes);
                pack_pixels(packing, src, packed.data(), usize{width} * height);
                target->written.clear();
                target->written.bytes = std::move(packed);
            } else {
                target->written.adopt(*readback, usize{width} * height * bytes);
            }
            const u8* written = target->written.data();
            bool deferred = false;
            if (lazy) {
                g_lazy.push_back(LazyWriteBack{target, data, span, layout, written});
                held.unlock();
                deferred = defer_write_back(r, data, span);
                if (!deferred) {
                    held.lock();
                    std::erase_if(g_lazy, [&](const LazyWriteBack& e) { return e.target == target; });
                    held.unlock();
                }
            }
            if (!deferred) write_back(r, {{data, span}}, nullptr, [&] { scatter(layout, data, written); });
            watch_range(target->watch, data, span);
        } else if (!ok && target) {
            settle_lazy(*target);
            target->written.clear();  // (the image may hold what guest memory does not)
        }
        back_mark(0);
        if (!ok && kept_depth) kept_depth->written.clear();
        // (the uniform buffers programs stored to)
        bool invalidated = false;
        for (const auto& [address, c] : buffer_copies) {
            if (!ok || !c.written || !c.placed || !memory().valid(address, c.bytes, true)) continue;
            if (!invalidated) vk::invalidate(d, *mem_buf), invalidated = true;
            std::memcpy(memory().host(address), static_cast<const u8*>(mem_buf->mapped) + 8 + c.offset, c.bytes);
        }
        if (ok && queries) {
            nx::vector<u64> counts(query_count, 0);
            if (vkGetQueryPoolResults(d.device(), queries, 0, query_count, query_count * 8, counts.data(), 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                for (const Counter& c : counters) {
                    if (!memory().valid(c.at, 4, true)) continue;
                    u32 v;
                    std::memcpy(&v, memory().host(c.at), 4);
                    // (samples at the internal resolution counted as the surface's: scale squared of them each)
                    const u64 drawn = counts[c.query], n = drawn ? nx::max<u64>(1, drawn / (u64{scale} * scale)) : 0;
                    v = c.set ? (n ? 1u : v) : v + static_cast<u32>(n);
                    std::memcpy(memory().host(c.at), &v, 4);
                }
            }
        }
        back_mark(1);
        if (ok && stores_depth) {
            vk::invalidate(d, *depth_buf);
            const u8* src = static_cast<const u8*>(depth_buf->mapped);
            const DepthRanges g = depth_ranges(ds, dl, width, height);
            const WrittenWatch* exempt = kept_depth && kept_stores ? &kept_depth->background_watch : nullptr;
            bool deferred = false;
            if (lazy_write_backs() && g.depth_bytes && !g.stencil_bytes) {
                std::unique_lock<std::mutex> held(g_lazy_lock);
                std::erase_if(g_lazy, [&](const LazyWriteBack& e) { return e.depth && e.address == g.depth; });
                Snapshot& kept = r.lazy_depths[g.depth];
                if (scale > 1) kept.clear(), kept.bytes = scale_down_samples(src, width, height, scale);
                else kept.adopt(*depth_buf, static_cast<usize>(samples) * 4);
                LazyWriteBack e;
                e.depth = true, e.address = g.depth, e.span = g.depth_bytes, e.bytes = kept.data();
                e.stencil = e.bytes + (scale > 1 ? usize{width} * height : static_cast<usize>(samples)) * 4;
                e.ds = ds, e.dl = dl, e.width = width, e.height = height, e.unorm24 = unorm24;
                g_lazy.push_back(e);
                // (the samples of stores written since, or forgotten, given back)
                nx::vector<u32> done;
                for (const auto& [at, k] : r.lazy_depths)
                    if (std::none_of(g_lazy.begin(), g_lazy.end(), [&](const LazyWriteBack& x) { return x.depth && x.address == at; }))
                        done.push_back(at);
                for (u32 at : done) r.lazy_depths[at].clear(), r.lazy_depths.erase(at);
                held.unlock();
                deferred = defer_write_back(r, g.depth, g.depth_bytes, exempt);
                if (!deferred) {
                    held.lock();
                    std::erase_if(g_lazy, [&](const LazyWriteBack& x) { return x.depth && x.address == g.depth; });
                    held.unlock();
                }
            }
            if (!deferred) {
                write_back(r, {{g.depth, g.depth_bytes}, {g.stencil, g.stencil_bytes}}, exempt, [&] {
                    if (scale > 1) {
                        const nx::vector<u8> down = scale_down_samples(src, width, height, scale);
                        write_depth_surface(ds, dl, width, height, unorm24, reinterpret_cast<const u32*>(down.data()),
                                            down.data() + usize{width} * height * 4);
                    } else {
                        write_depth_surface(ds, dl, width, height, unorm24, reinterpret_cast<const u32*>(src),
                                            src + samples * 4);
                    }
                });
            }
            // (what guest memory holds now, to know whether the kept depth image is still the surface)
            // (watched first: a copy of the bytes kept only if they may be compared)
            if (depth_target) {
                const auto now = depth_bytes(ds, width, height);
                watch_range(depth_target->watch[0], ds.depth_data, static_cast<u32>(now.size()));
                StoredDepth& w = depth_target->written;
                w.size_ = now.size();
                if (keep_stored_bytes({&depth_target->watch[0]})) w.bytes.assign(now.begin(), now.end());
                else w.bytes.clear();
            }
            // (a scene that stores elsewhere drew over its kept image: not the surface it stored)
            if (kept_depth && kept_stores) {
                watch_range(kept_depth->watch[0], g.depth, g.depth_bytes);
                watch_range(kept_depth->watch[1], g.stencil, g.stencil_bytes);
                StoredDepth& w = kept_depth->written;
                w.size_ = usize{g.depth_bytes} + g.stencil_bytes;
                if (keep_stored_bytes({&kept_depth->watch[0], &kept_depth->watch[1]})) depth_contents(g, w.bytes);
                else w.bytes.clear();
                ++kept_depth->generation;
            }
        }
        back_mark(2);
        // (the scene's notifications, now that guest memory holds what it drew)
        for (const Notification& n : notifications) notify(n);
        if (stamps) vkDestroyQueryPool(d.device(), stamps, nullptr);
        if (queries) vkDestroyQueryPool(d.device(), queries, nullptr);
        give_descriptor_pool(r, pool, sets);
        for (vk::Buffer& b : buffers) give_host_buffer(r, b);
        if (!depth_target && !kept_depth && depth.image) free_depth_image(r, depth);
        for (Renderer::TargetCopy& c : copies)
            if (c.strip.buffer) vk::destroy_buffer(d, c.strip);
        for (Renderer::TextureUpload& u : uploads) give_upload(r, u);
        for (Renderer::TargetConversion& c : conversions) {
            if (!ok && c.background_staging.buffer) c.depth->background_uploaded = false;
            if (ok) verified_conversion(c, d, target);
            release_conversion(r, c);
        }
        if (pack_set) free_target_converter_set(d, r.converter, pack_set), give_work(r, pack_work);
        give_host_buffer(r, pack_verify);
        back_mark(3);
    };
    r.target_copies.clear();  // (moved into the scene's finish: the guard finds none)
    r.texture_uploads.clear();
    r.target_conversions.clear();
    if (auto submitted = shot->submit(); !submitted) {
        nx::loge(logs::gpu, "scene: {}", submitted.error().message);
        finish_pending(r, true);
        pending.finish(false);
        return true;
    }
    pending.shot = std::move(shot);
    pending.number = number;
    pending.index = r.scenes;
    r.submitted = number;
    g_profile.mark(6);
    // (the scenes the GPU is done with; and the oldest, waited for, when it has scenes_in_flight() with this one)
    g_finish_why = 0;
    finish_pending(r, false);
    finish_pending(r, true, scenes_in_flight() - 1);
    g_profile.mark(5);
    r.pending.push_back(std::move(pending));
    r.pending_count.store(static_cast<u32>(r.pending.size()), std::memory_order_release);
    g_profile.mark(7);
    g_profile.done();
    if (d.validation_errors()) nx::logw(logs::gpu, "{} validation errors so far", d.validation_errors());
    return true;
}

namespace {

void finish_pending(Renderer& r, bool all, usize keep) {
    while (r.pending.size() > (all ? keep : 0)) {
        Renderer::Pending& p = r.pending.front();
        if (!all && !p.shot->done()) break;
        const u64 t0 = g_profile.on ? nx::os::ticks_ns() : 0;
        Result<void> waited;
        if (p.shot->done()) {
            waited = p.shot->wait();
        } else {
            // (a guest thread waiting for the GPU gives up its CPU meanwhile)
            CpuGivenUp blocking;
            waited = p.shot->wait();
        }
        if (!waited) nx::loge(logs::gpu, "scene: {}", waited.error().message);
        const u64 t1 = g_profile.on ? nx::os::ticks_ns() : 0;
        p.finish(static_cast<bool>(waited));
        r.finished = p.number;
        if (g_profile.on) {
            ++g_profile.finished;
            g_profile.finish_wait_ns += t1 - t0;
            ++g_profile.why_count[g_finish_why];
            g_profile.why_wait_ns[g_finish_why] += t1 - t0;
            g_profile.finish_work_ns += nx::os::ticks_ns() - t1;
        }
        r.pending.erase(r.pending.begin());
    }
    destroy_retired(r);
    r.pending_count.store(static_cast<u32>(r.pending.size()), std::memory_order_release);
}

}  // namespace

void profile_buffer_wait(u64 ns) {
    if (g_profile.on) g_profile.buffer_wait_ns += ns;
}

namespace {

thread_local bool t_scene_thread = false;  // (the renderer's thread)
// (ended and not yet rendered, at most: the game's thread waits for the renderer beyond. One: P4D rewrites what a
// scene reads about a frame after it ends, which the console's GPU has read by then; with two frames' worth (8)
// its frames showed what later ones wrote)
constexpr usize kEndedScenes = 1;

// An ended scene rendered: once the display has let its buffer go (the GPU's wait for its fragment sync object);
// one not rendered (nothing to draw into, no device) has its notifications written now, after the scenes before.
void render_ended(Renderer& r, Renderer::Ended& e) {
    State& st = state();
    static const bool keep_cpu = [] {
        const char* v = std::getenv("PSVRECORE_GXM_KEEP_CPU");
        return v && *v && *v != '0';
    }();
    std::optional<CpuLent> lent;
    if (!keep_cpu) lent.emplace();
    if (e.scene.fragment_sync) {
        const u64 t0 = nx::os::ticks_ns();
        CpuGivenUp blocking;
        nx::scoped_lock<nx::mutex> held(st.queue_lock);
        st.queue_changed.wait(st.queue_lock, [&] {
            auto it = st.holds_released.find(e.scene.fragment_sync);
            return st.stopping || (it != st.holds_released.end() ? it->second : 0) >= e.scene.fragment_holds;
        });
        profile_buffer_wait(nx::os::ticks_ns() - t0);
    }
    capture_before(e.number);
    const bool rendered = render_scene(e.scene, e.notes, e.number);
    capture_after(e.number);
    if (!rendered) {
        {
            TargetsHeld held(r);
            finish_pending(r, true);  // (the scenes before it first: notifications are written in order)
        }
        for (const Notification& n : e.notes) notify(n);
    }
}

// Scene `number` rendered (under the scenes' lock).
void mark_rendered(Renderer& r, u64 number) {
    if (number != r.rendered + 1) {
        r.rendered_ahead.push_back(number);
        return;
    }
    r.rendered = number;
    for (bool more = true; more;) {
        more = false;
        for (usize i = 0; i < r.rendered_ahead.size(); ++i)
            if (r.rendered_ahead[i] == r.rendered + 1) {
                r.rendered = r.rendered_ahead[i];
                r.rendered_ahead.erase(r.rendered_ahead.begin() + static_cast<isize>(i));
                more = true;
                break;
            }
    }
}

void scene_thread(Renderer& r) {
    t_scene_thread = true;
    for (;;) {
        Renderer::Ended e;
        {
            nx::scoped_lock<nx::mutex> held(r.scenes_lock);
            r.scenes_changed.wait(r.scenes_lock, [&] { return !r.ended_scenes.empty(); });
            e = std::move(r.ended_scenes.front());
            r.ended_scenes.erase(r.ended_scenes.begin());
        }
        render_ended(r, e);
        {
            nx::scoped_lock<nx::mutex> held(r.scenes_lock);
            mark_rendered(r, e.number);
        }
        r.scenes_changed.notify_all();
    }
}

// The renderer's thread done with the scenes ended up to `limit` (not those ended while this waits); a guest
// thread gives up its CPU meanwhile.
void wait_rendered(Renderer& r, u64 limit) {
    if (t_scene_thread) return;
    u64 upto;
    {
        nx::scoped_lock<nx::mutex> held(r.scenes_lock);
        upto = nx::min(limit, r.ended);
        if (r.rendered >= upto) return;
    }
    CpuGivenUp blocking;
    nx::scoped_lock<nx::mutex> held(r.scenes_lock);
    r.scenes_changed.wait(r.scenes_lock, [&] { return r.rendered >= upto; });
}

}  // namespace

void end_scene(Scene&& scene, nx::vector<Notification> notifications) {
    Renderer& r = renderer();
    // (on the renderer's own thread only with PSVRECORE_GXM_SCENE_THREAD=1: there, a scene's copies run on a thread
    // that sleeps between scenes, with cold caches; a song on a Pixel 8a ran at 27.3 frames a second so, 28.1 on the
    // game's thread)
    static const bool sync = [] {
        const char* v = std::getenv("PSVRECORE_GXM_SCENE_THREAD");
        return !(v && *v && *v != '0');
    }();
    Renderer::Ended e{std::move(scene), std::move(notifications), 0};
    if (sync) {
        {
            nx::scoped_lock<nx::mutex> held(r.scenes_lock);
            e.number = ++r.ended;
        }
        render_ended(r, e);
        // (and those waiting for it told: a thread that ends no scene, a transfer's, may wait in wait_rendered)
        {
            nx::scoped_lock<nx::mutex> held(r.scenes_lock);
            mark_rendered(r, e.number);
        }
        r.scenes_changed.notify_all();
        return;
    }
    bool start = false, queued = false;
    {
        nx::scoped_lock<nx::mutex> held(r.scenes_lock);
        if (!r.worker) r.worker = start = true;
        if (r.ended_scenes.size() < kEndedScenes) {
            e.number = ++r.ended;
            r.ended_scenes.push_back(std::move(e));
            queued = true;
        }
    }
    if (start) {
        nx::thread_options options;
        options.name = "gxm render";
        nx::thread worker([&r] { scene_thread(r); }, options);
        worker.detach();
    }
    if (!queued) {
        // (the renderer that far behind: the game's thread waits for it, without its CPU)
        CpuGivenUp blocking;
        nx::scoped_lock<nx::mutex> held(r.scenes_lock);
        r.scenes_changed.wait(r.scenes_lock, [&] { return r.ended_scenes.size() < kEndedScenes; });
        e.number = ++r.ended;
        r.ended_scenes.push_back(std::move(e));
    }
    r.scenes_changed.notify_all();
}

u64 scenes_submitted() {
    Renderer& r = renderer();
    nx::scoped_lock<nx::mutex> held(r.scenes_lock);
    return r.ended;
}

u64 scenes_rendered() {
    Renderer& r = renderer();
    nx::scoped_lock<nx::mutex> held(r.scenes_lock);
    return r.rendered;
}

bool user_markers_wanted() { return g_markers_on.load(std::memory_order_relaxed); }

void request_gpu_capture(nx::string_view why) {
    if (!vk::renderdoc::loaded()) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            nx::logi(logs::gpu, "a GPU capture was asked for ({}): RenderDoc is not loaded (start the game from it)", why);
        return;
    }
    GpuCapture& c = gpu_capture();
    nx::scoped_lock<nx::mutex> held(c.lock);
    if (c.armed || c.from) return;  // (one at a time)
    c.armed = true;
    c.title = nx::format("{} (frame {})", why, c.frames + 1);
    nx::logi(logs::gpu, "RenderDoc: capturing the next frame ({})", why);
}

void frame_boundary() {
    PSV_PROFILE_ZONE("gxm: frame queued");
    const u64 ended = scenes_submitted();
    const u64 rendered = scenes_rendered();
    GpuCapture& c = gpu_capture();
    bool at_frame = false;
    {
        nx::scoped_lock<nx::mutex> held(c.lock);
        ++c.frames;
        at_frame = state().options && state().options->capture_frame && c.frames + 1 == state().options->capture_frame;
        if (c.armed) {
            c.armed = false;
            c.from = ended + 1, c.to = 0;
        } else if (c.from && !c.to && ended >= c.from) {
            c.to = ended;
            // (its scenes all rendered by now: the capture ends here, else after its last scene)
            if (c.active && rendered >= c.to) capture_end(c);
        }
    }
    // (the capture_frame setting: the frame after this boundary)
    if (at_frame) request_gpu_capture("capture_frame");
}

bool gpu_capture_in_progress() {
    GpuCapture& c = gpu_capture();
    nx::scoped_lock<nx::mutex> held(c.lock);
    return c.armed || c.from;
}

void finish_scenes(u64 limit) {
    Renderer& r = renderer();
    g_finish_times.get() = {};
    wait_rendered(r, limit);
    if (!r.pending_count.load(std::memory_order_acquire)) return;
    // The scenes pending now up to `limit` (not those submitted while this waits), waited for without the renderer's
    // lock: the game's thread prepares its next scene meanwhile; each finished, under the lock, once the GPU is done
    // with it.
    u64 upto;
    {
        const u64 t0 = nx::os::ticks_ns();
        TargetsHeld held(r);
        g_finish_times.get().lock_ns += nx::os::ticks_ns() - t0;
        if (r.pending.empty()) return;
        upto = nx::min(limit, r.pending.back().number);
        // (numbers of scenes not submitted, with nothing to draw, are skipped)
        if (r.finished >= upto || r.pending.front().number > upto) return;
    }
    PSV_PROFILE_ZONE_COLOR("gxm: waiting for scenes on the GPU", kSceneRgb);
    CpuGivenUp blocking;
    for (;;) {
        {
            const u64 t0 = nx::os::ticks_ns();
            nx::scoped_lock<nx::mutex> held(r.targets_lock);
            const u64 t1 = nx::os::ticks_ns();
            g_finish_why = 2;
            const u64 before = r.finished;
            finish_pending(r, false);
            const u64 t2 = nx::os::ticks_ns();
            g_finish_times.get().lock_ns += t1 - t0;
            (r.finished != before ? g_finish_times.get().finish_ns : g_finish_times.get().gpu_ns) += t2 - t1;
            if (r.finished >= upto || r.pending.empty() || r.pending.front().number > upto) return;
        }
        const u64 t3 = nx::os::ticks_ns();
        nx::this_thread::sleep_for(std::chrono::microseconds(100));
        g_finish_times.get().gpu_ns += nx::os::ticks_ns() - t3;
    }
}

// At the surface's own size too (the image, in the sink's format): no readback into guest memory, nor the CPU's copy
// of it to the window (on an Odin 3 at 1x, the display's thread spent ~3 ms a frame on that, which the game waits for).
bool scaled_frame_to(const DisplayFrameBuf& f, DisplaySink& sink, u32* width, u32* height) {
    Renderer& r = renderer();
    if (!r.device || f.pixel_format != 0 || r.device != state().options->shared_device) return false;
    TargetsHeld held(r);
    for (auto& t : r.targets) {
        if (t->address != f.base || t->width != f.width || t->height != f.height) continue;
        const bool scaled = t->scale > 1 && t->scaled.image;
        vk::Image& from = scaled ? t->scaled : t->image;
        if (!from.image) return false;
        ColorFormat cf;
        if (!color_format(t->format, cf) || cf.bytes != 4) return false;
        if (t->layout.kind != SurfaceLayout::kLinear || t->layout.stride != f.pitch) return false;
        // (only as the scene left it: what the CPU drew over it since is in guest memory, scaled_frame's)
        const Changes changes = surface_changes(*t);
        if (changes.all || changes.pixels) return false;
        vk::Image* out = sink.gpu_frame(from.desc.width, from.desc.height);
        if (!out || out->desc.width != from.desc.width || out->desc.height != from.desc.height ||
            vk::texel_size(out->desc.format) != 4 || (!scaled && out->desc.format != from.desc.format))
            return false;
        // (the copies the GPU is done with let go; the frame's after the scenes drawing it, on the same queue)
        for (usize i = 0; i < r.display_copies.size();) {
            if (r.display_copies[i]->done()) r.display_copies.erase(r.display_copies.begin() + static_cast<isize>(i));
            else ++i;
        }
        auto shot = nx::make_unique<vk::OneShot>(*r.device);
        auto cmd = shot->begin();
        if (!cmd) return false;
        vk::transition(*cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk::transition(*cmd, *out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {from.desc.width, from.desc.height, 1};
        vkCmdCopyImage(*cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        vk::transition(*cmd, *out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (auto submitted = shot->submit(); !submitted) {
            nx::logw(logs::gpu, "the scaled frame: {}", submitted.error().message);
            return false;
        }
        r.display_copies.push_back(std::move(shot));
        *width = from.desc.width, *height = from.desc.height;
        return true;
    }
    return false;
}

bool scaled_frame(const DisplayFrameBuf& f, nx::vector<u8>& pixels, u32* width, u32* height) {
    Renderer& r = renderer();
    if (!r.device) return false;
    TargetsHeld held(r);
    for (auto& t : r.targets) {
        if (t->address != f.base || t->width != f.width || t->height != f.height || t->scale <= 1) continue;
        ColorFormat cf;
        if (!color_format(t->format, cf) || cf.bytes != 4) return false;
        if (t->layout.kind != SurfaceLayout::kLinear || t->layout.stride != f.pitch) return false;
        // (what the CPU drew over the frame since its scene, scaled up over it; the frame as it is in guest memory
        // when that is most of it)
        const Changes changes = surface_changes(*t);
        if (changes.all || changes.pixels * 4 > usize{t->width} * t->height) return false;
        auto read = vk::read_image(*r.device, t->scaled);
        if (!read) {
            nx::logw(logs::gpu, "the scaled frame: {}", read.error().message);
            return false;
        }
        pixels = std::move(*read);
        const u32 s = t->scale, sw = t->width * s;
        if (changes.pixels) {
            for (u32 y = 0; y < t->height; ++y) {
                const u8* guest = memory().host(t->address + y * f.pitch * 4);
                const u8* had = t->written.data() + usize{y} * t->width * 4;
                for (u32 x = 0; x < t->width; ++x) {
                    if (std::memcmp(guest + x * 4, had + x * 4, 4) == 0) continue;
                    for (u32 dy = 0; dy < s; ++dy)
                        for (u32 dx = 0; dx < s; ++dx)
                            std::memcpy(pixels.data() + (usize{y * s + dy} * sw + x * s + dx) * 4, guest + x * 4, 4);
                }
            }
        }
        *width = t->scaled.desc.width, *height = t->scaled.desc.height;
        return true;
    }
    return false;
}

}  // namespace psvrecore::gxm::hle

namespace psvrecore::gxm {

WarmUp warm_up() {
    using namespace hle;
    WarmUp w;
    Renderer& r = renderer();
    if (!ensure(r)) return w;
    const u64 start = nx::os::ticks_ns();
    r.warming = true;
    auto none = [] { return Result<nx::vector<u32>>(failure("not in the shader cache")); };
    // (the secondary programs' compute pipelines: their modules, then the pipelines on several threads)
    nx::vector<std::pair<u64, VkShaderModule>> computes;
    for (const ShaderKey& key : r.cache.compute()) {
        if (r.compute.contains(short_key(key))) continue;
        if (const VkShaderModule m = module_for(r, key, none)) {
            r.compute[short_key(key)] = VK_NULL_HANDLE;  // (once each)
            computes.push_back({short_key(key), m});
        }
    }
    nx::vector<VkPipeline> made(computes.size(), VK_NULL_HANDLE);
    in_parallel(computes.size(), [&](usize k) { made[k] = make_compute(r, computes[k].second); });
    for (usize k = 0; k < computes.size(); ++k) {
        r.compute[computes[k].first] = made[k];
        ++w.pipelines;
    }
    const u64 compute_end = nx::os::ticks_ns();
    // (the graphics pipelines: their modules, then the pipelines on several threads)
    nx::vector<const ShaderCache::Graphics*> recorded;
    nx::vector<GraphicsJob> jobs;
    for (const ShaderCache::Graphics& g : r.cache.graphics()) {
        GraphicsJob job{g.desc};
        job.desc.vertex = module_for(r, g.vertex, none);
        job.desc.fragment = module_for(r, g.fragment, none);
        job.desc.layout = r.layout;
        if (job.desc.vertex && job.desc.fragment) jobs.push_back(job), recorded.push_back(&g);
    }
    const u64 modules_end = nx::os::ticks_ns();
    make_graphics(r, jobs);
    u32 graphics = 0;
    u64 graphics_ns = 0;  // (the graphics pipelines' own times, added up: more than the time they took together)
    auto words = [&](const ShaderKey& k) {
        const nx::vector<u32>* v = r.cache.spirv(k);
        return v ? v->size() : usize{0};
    };
    for (usize k = 0; k < jobs.size(); ++k) {
        if (jobs[k].pipeline) ++w.pipelines, ++graphics;
        graphics_ns += jobs[k].ns;
        // (PSVRECORE_GXM_PROFILE: each pipeline, by its shaders; one whose shaders were compiled together before
        // takes a fraction of a millisecond)
        if (std::getenv("PSVRECORE_GXM_PROFILE"))
            nx::logi(logs::gpu, "gxm profile: warm-up pipeline, {:.1f} ms: vertex shader {:016x} ({} words), fragment {:016x} ({})",
                     static_cast<f64>(jobs[k].ns) / 1e6, short_key(recorded[k]->vertex), words(recorded[k]->vertex),
                     short_key(recorded[k]->fragment), words(recorded[k]->fragment));
    }
    r.warming = false;
    // (what warm_up could not make is tried again when the game draws with it)
    r.failed_programs.clear();
    w.shaders = static_cast<u32>(r.modules.size());
    const u64 end = nx::os::ticks_ns();
    w.ms = static_cast<f64>(end - start) / 1e6;
    r.pipelines_changed.store(true);  // (the driver's cache now has them all: saved with the rest)
    // (an empty driver cache, its file removed: how long a phone's driver takes to compile what the game used)
    nx::logi(logs::gpu,
             "warm-up: {} shaders and {} pipelines from the cache in {:.0f} ms (compute {:.0f}; graphics modules {:.0f}; "
             "{} graphics pipelines {:.0f} on {} threads, {:.0f} one after another)",
             w.shaders, w.pipelines, w.ms, static_cast<f64>(compute_end - start) / 1e6,
             static_cast<f64>(modules_end - compute_end) / 1e6, graphics, static_cast<f64>(end - modules_end) / 1e6,
             pipeline_threads(), static_cast<f64>(graphics_ns) / 1e6);
    return w;
}

void save_caches() {
    using namespace hle;
    Renderer& r = renderer();
    if (!r.device || r.cache.folder().empty() || !r.pipelines_changed.exchange(false)) return;
    const nx::string path = r.cache.folder() + "/pipelines.vkcache";
    if (auto saved = r.pipelines->save_cache(nx::fs::path_view(path)); !saved)
        nx::logw(logs::gpu, "pipeline cache: {}", saved.error().message);
}

PlayStats play_stats() {
    hle::Renderer& r = hle::renderer();
    return PlayStats{r.translated.load(), r.made_pipelines.load()};
}

RendererState renderer_state() {
    hle::Renderer& r = hle::renderer();
    RendererState s;
    s.pending = r.pending_count.load(std::memory_order_acquire);
    if (r.scenes_lock.try_lock()) {
        s.ended = r.ended, s.rendered = r.rendered;
        r.scenes_lock.unlock();
    } else {
        s.busy = true;
    }
    if (r.targets_lock.try_lock()) {
        s.submitted = r.submitted, s.finished = r.finished;
        r.targets_lock.unlock();
    } else {
        s.busy = true;
    }
    return s;
}

}  // namespace psvrecore::gxm
