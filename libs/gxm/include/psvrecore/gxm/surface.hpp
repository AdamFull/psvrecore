#pragma once

// Where a colour surface's pixels are in memory, in each of its types: rows of the stride (linear), 32 x 32 tiles in
// rows of tiles, each tile's pixels in rows (tiled), or in Morton order over the power-of-two size that holds it
// (swizzled). The tiled and swizzled layouts are the ones textures of those types read, so a texture can sample what
// a scene drew.

#include <bit>

#include "psvrecore/gxm/texture.hpp"

namespace psvrecore::gxm {

struct SurfaceLayout {
    static constexpr u32 kLinear = 0, kTiled = 1, kSwizzled = 2;  // (SCE_GXM_COLOR_SURFACE_* >> 26)
    u32 kind = kLinear;
    u32 stride = 0;  // (pixels)
    u32 bytes = 0;   // (a pixel's)
    u32 width = 0, height = 0;

    // The byte offset of pixel (x, y) from the surface's start.
    u64 offset(u32 x, u32 y) const {
        switch (kind) {
            case kTiled: return (((u64{y} / 32) * ((stride + 31) / 32) + x / 32) * 1024 + (y % 32) * 32 + x % 32) * bytes;
            case kSwizzled: return swizzled_index(x, y, width, height) * bytes;
            default: return (u64{y} * stride + x) * bytes;
        }
    }
    // The bytes from the first pixel to past the last.
    u64 span() const {
        switch (kind) {
            case kTiled: return u64{(height + 31) / 32} * ((stride + 31) / 32) * 1024 * bytes;
            case kSwizzled: return u64{std::bit_ceil(width)} * std::bit_ceil(height) * bytes;
            default: return (u64{height - 1} * stride + width) * bytes;
        }
    }
    // How many pixels from (x, y) along the row are next to each other in memory too.
    u32 run(u32 x) const {
        switch (kind) {
            case kTiled: return (32 - x % 32) < width - x ? 32 - x % 32 : width - x;
            case kSwizzled: return 1;
            default: return width - x;
        }
    }
    bool operator==(const SurfaceLayout&) const = default;
};

}  // namespace psvrecore::gxm
