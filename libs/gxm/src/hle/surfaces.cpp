// Colour and depth/stencil surfaces: where a scene's results go. Their opaque words hold this layer's own encoding
// (state.hpp), read back by the getters.

#include <cstring>

#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;

// ColorSurface::modes
constexpr u32 kEnabled = 1u << 31;
u32 modes_of(u32 type, u32 scale, u32 gamma, u32 dither) {
    return (type >> 26) | (scale << 4) | ((gamma >> 12) << 8) | ((dither >> 3) << 12) | kEnabled;
}

// The texture format that reads a colour format's pixels (0: none), with its swizzle as the colour's (exact for
// four components; for one or two, the mode of the same number).
u32 texture_format_of(u32 color) {
    static const struct {
        u32 color, texture;
    } kBases[] = {
        {0x00000000u, 0x0C000000u},  // U8U8U8U8
        {0x10000000u, 0x98000000u},  // U8U8U8
        {0x30000000u, 0x05000000u},  // U5U6U5
        {0x40000000u, 0x04000000u},  // U1U5U5U5
        {0x50000000u, 0x02000000u},  // U4U4U4U4
        {0x60000000u, 0x03000000u},  // U8U3U3U2
        {0xF0000000u, 0x0B000000u},  // F16
        {0x00800000u, 0x11000000u},  // F16F16
        {0x10800000u, 0x12000000u},  // F32
        {0x20800000u, 0x0A000000u},  // S16
        {0x30800000u, 0x10000000u},  // S16S16
        {0x40800000u, 0x09000000u},  // U16
        {0x50800000u, 0x0F000000u},  // U16U16
        {0x60800000u, 0x0E000000u},  // U2U10U10U10
        {0x80800000u, 0x00000000u},  // U8
        {0x90800000u, 0x01000000u},  // S8
        {0xA0800000u, 0x06000000u},  // S5S5U6
        {0xB0800000u, 0x07000000u},  // U8U8
        {0xC0800000u, 0x08000000u},  // S8S8
        {0xE0800000u, 0x0D000000u},  // S8S8S8S8
        {0x01000000u, 0x1B000000u},  // F16F16F16F16
        {0x11000000u, 0x1E000000u},  // F32F32
        {0x21000000u, 0x1A000000u},  // F11F11F10
        {0x31000000u, 0x19000000u},  // SE5M9M9M9
        {0x41000000u, 0x9A000000u},  // U2F10F10F10
    };
    const u32 base = color & 0xF1800000u;
    for (const auto& b : kBases)
        if (b.color == base) return b.texture | ((color >> 8) & 0x7000u);
    return 0;
}

// The surface's background texture (SceGxmColorSurface's last four words, as the console fills them): the surface's
// pixels as a texture (games read the surface's size and data from it).
void set_background(ColorSurface& c) {
    std::memset(c.background, 0, sizeof c.background);
    const u32 format = texture_format_of(c.format);
    if (!format || !(c.modes & kEnabled)) return;
    TextureInfo t;
    const u32 type = (c.modes & 3) << 26;
    t.type = type == 0x04000000u ? texture_type::kTiled : type == 0x08000000u ? texture_type::kSwizzled : texture_type::kLinearStrided;
    t.base_format = format & 0x9F000000u;
    t.swizzle = (format >> 12) & 7;
    t.width = c.size & 0xFFFF;
    t.height = c.size >> 16;
    t.address = c.data;
    t.stride = c.stride * texel_bits(t.base_format) / 8;
    encode_texture(t, c.background);
}

i32 sceGxmColorSurfaceInit(Ptr<ColorSurface> s, u32 format, u32 type, u32 scale, u32 register_size, u32 width,
                           u32 height, u32 stride, u32 data) {
    if (!s) return err::kInvalidPointer;
    const auto refuse = [&](i32 error) {
        nx::logw(logs::gxm, "sceGxmColorSurfaceInit(format 0x{:08x}, type 0x{:x}, scale {}, register size {}, {}x{}, stride {}, "
                 "data 0x{:08x}): refused (0x{:08x})", format, type, scale, register_size, width, height, stride, data,
                 static_cast<u32>(error));
        return error;
    };
    if (!width || !height || width > 4096 || height > 4096 || stride < width || scale > 1 || register_size > 1)
        return refuse(err::kInvalidValue);
    if (type != 0 && type != 0x04000000u && type != 0x08000000u) return refuse(err::kInvalidValue);
    if (data & 3) return refuse(err::kInvalidAlignment);
    ColorSurface c{};
    c.format = format;
    c.data = data;
    c.size = width | height << 16;
    c.stride = stride;
    c.modes = modes_of(type, scale, 0, 0);
    c.clip_min = 0;
    c.clip_max = (width - 1) | (height - 1) << 16;
    c.output_register_size = register_size;
    set_background(c);
    *s = c;
    return 0;
}

i32 sceGxmColorSurfaceInitDisabled(Ptr<ColorSurface> s) {
    if (!s) return err::kInvalidPointer;
    std::memset(s.get(), 0, sizeof(ColorSurface));
    return 0;
}

u32 sceGxmColorSurfaceIsEnabled(Ptr<const ColorSurface> s) { return s && (s->modes & kEnabled) ? 1 : 0; }

void sceGxmColorSurfaceGetClip(Ptr<const ColorSurface> s, Ptr<u32> x0, Ptr<u32> y0, Ptr<u32> x1, Ptr<u32> y1) {
    if (!s) return;
    if (x0) *x0 = s->clip_min & 0xFFFF;
    if (y0) *y0 = s->clip_min >> 16;
    if (x1) *x1 = s->clip_max & 0xFFFF;
    if (y1) *y1 = s->clip_max >> 16;
}

void sceGxmColorSurfaceSetClip(Ptr<ColorSurface> s, u32 x0, u32 y0, u32 x1, u32 y1) {
    if (!s) return;
    s->clip_min = (x0 & 0xFFFF) | (y0 & 0xFFFF) << 16;
    s->clip_max = (x1 & 0xFFFF) | (y1 & 0xFFFF) << 16;
}

u32 sceGxmColorSurfaceGetData(Ptr<const ColorSurface> s) { return s ? s->data : 0; }
i32 sceGxmColorSurfaceSetData(Ptr<ColorSurface> s, u32 data) {
    if (!s) return err::kInvalidPointer;
    if (data & 3) return err::kInvalidAlignment;
    s->data = data;
    set_background(*s);
    return 0;
}
u32 sceGxmColorSurfaceGetFormat(Ptr<const ColorSurface> s) { return s ? s->format : 0; }
i32 sceGxmColorSurfaceSetFormat(Ptr<ColorSurface> s, u32 format) {
    if (!s) return err::kInvalidPointer;
    s->format = format;
    set_background(*s);
    return 0;
}
u32 sceGxmColorSurfaceGetType(Ptr<const ColorSurface> s) { return s ? (s->modes & 3) << 26 : 0; }
u32 sceGxmColorSurfaceGetScaleMode(Ptr<const ColorSurface> s) { return s ? (s->modes >> 4) & 1 : 0; }
void sceGxmColorSurfaceSetScaleMode(Ptr<ColorSurface> s, u32 scale) {
    if (s) s->modes = (s->modes & ~0xF0u) | (scale & 1) << 4;
}
u32 sceGxmColorSurfaceGetGammaMode(Ptr<const ColorSurface> s) { return s ? ((s->modes >> 8) & 0xF) << 12 : 0; }
i32 sceGxmColorSurfaceSetGammaMode(Ptr<ColorSurface> s, u32 gamma) {
    if (!s) return err::kInvalidPointer;
    s->modes = (s->modes & ~0xF00u) | ((gamma >> 12) & 0xF) << 8;
    return 0;
}
u32 sceGxmColorSurfaceGetDitherMode(Ptr<const ColorSurface> s) { return s ? ((s->modes >> 12) & 1) << 3 : 0; }
void sceGxmColorSurfaceSetDitherMode(Ptr<ColorSurface> s, u32 dither) {
    if (s) s->modes = (s->modes & ~0xF000u) | ((dither >> 3) & 1) << 12;
}
u32 sceGxmColorSurfaceGetStrideInPixels(Ptr<const ColorSurface> s) { return s ? s->stride : 0; }

// ---- Depth/stencil ---------------------------------------------------------------------------------------------

// DepthStencilSurface::control: the format (its bits are 12-25) | tiled << 28 | force load << 29 | force store
// << 30 | enabled << 31. background_control: the stencil value | the mask bit << 8 | the stride in tiles << 16.

i32 sceGxmDepthStencilSurfaceInit(Ptr<DepthStencilSurface> s, u32 format, u32 type, u32 stride, u32 depth,
                                  u32 stencil) {
    if (!s) return err::kInvalidPointer;
    if ((stride & 31) || !stride || stride > 4096) return err::kInvalidValue;
    if (type != 0 && type != 0x11000u) return err::kInvalidValue;
    if ((depth & 15) || (stencil & 15)) return err::kInvalidAlignment;
    DepthStencilSurface d{};
    d.control = (format & 0x03FFF000u) | (type ? kDsTiled : 0) | kDsEnabled;
    d.depth_data = depth;
    d.stencil_data = stencil;
    d.background_depth = 1.0f;
    d.background_control = (stride / 32) << 16 | 0x100;  // (the mask set: assumed, as drawing needs it)
    *s = d;
    return 0;
}

i32 sceGxmDepthStencilSurfaceInitDisabled(Ptr<DepthStencilSurface> s) {
    if (!s) return err::kInvalidPointer;
    std::memset(s.get(), 0, sizeof(DepthStencilSurface));
    s->background_depth = 1.0f;
    return 0;
}

u32 sceGxmDepthStencilSurfaceIsEnabled(Ptr<const DepthStencilSurface> s) {
    return s && (s->control & kDsEnabled) ? 1 : 0;
}
u32 sceGxmDepthStencilSurfaceGetFormat(Ptr<const DepthStencilSurface> s) { return s ? s->control & 0x03FFF000u : 0; }
u32 sceGxmDepthStencilSurfaceGetStrideInSamples(Ptr<const DepthStencilSurface> s) {
    return s ? (s->background_control >> 16) * 32 : 0;
}
u32 sceGxmDepthStencilSurfaceGetForceLoadMode(Ptr<const DepthStencilSurface> s) {
    return s && (s->control & kDsForceLoad) ? 2 : 0;
}
void sceGxmDepthStencilSurfaceSetForceLoadMode(Ptr<DepthStencilSurface> s, u32 mode) {
    if (s) s->control = (s->control & ~kDsForceLoad) | (mode == 2 ? kDsForceLoad : 0);
}
u32 sceGxmDepthStencilSurfaceGetForceStoreMode(Ptr<const DepthStencilSurface> s) {
    return s && (s->control & kDsForceStore) ? 4 : 0;
}
void sceGxmDepthStencilSurfaceSetForceStoreMode(Ptr<DepthStencilSurface> s, u32 mode) {
    if (s) s->control = (s->control & ~kDsForceStore) | (mode == 4 ? kDsForceStore : 0);
}
u32 sceGxmDepthStencilSurfaceGetBackgroundStencil(Ptr<const DepthStencilSurface> s) {
    return s ? s->background_control & 0xFF : 0;
}
void sceGxmDepthStencilSurfaceSetBackgroundStencil(Ptr<DepthStencilSurface> s, u32 stencil) {
    if (s) s->background_control = (s->background_control & ~0xFFu) | (stencil & 0xFF);
}
u32 sceGxmDepthStencilSurfaceGetBackgroundMask(Ptr<const DepthStencilSurface> s) {
    return s ? (s->background_control >> 8) & 1 : 0;
}
void sceGxmDepthStencilSurfaceSetBackgroundMask(Ptr<DepthStencilSurface> s, u32 mask) {
    if (s) s->background_control = (s->background_control & ~0x100u) | (mask ? 0x100u : 0);
}
f32 sceGxmDepthStencilSurfaceGetBackgroundDepth(Ptr<const DepthStencilSurface> s) {
    return s ? s->background_depth : 0.0f;
}
void sceGxmDepthStencilSurfaceSetBackgroundDepth(Ptr<DepthStencilSurface> s, f32 depth) {
    if (s) s->background_depth = depth;
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0xED0F6E25, sceGxmColorSurfaceInit),
    HLE_FUNCTION(0x613639FA, sceGxmColorSurfaceInitDisabled),
    HLE_FUNCTION(0x0E0EBB57, sceGxmColorSurfaceIsEnabled),
    HLE_FUNCTION(0x07DFEE4B, sceGxmColorSurfaceGetClip),
    HLE_FUNCTION(0x86456F7B, sceGxmColorSurfaceSetClip),
    HLE_FUNCTION(0x2DB6026C, sceGxmColorSurfaceGetData),
    HLE_FUNCTION(0x537CA400, sceGxmColorSurfaceSetData),
    HLE_FUNCTION(0xF3C1C6C6, sceGxmColorSurfaceGetFormat),
    HLE_FUNCTION(0x5F9A3A16, sceGxmColorSurfaceSetFormat),
    HLE_FUNCTION(0x52FDE962, sceGxmColorSurfaceGetType),
    HLE_FUNCTION(0x6E3FA74D, sceGxmColorSurfaceGetScaleMode),
    HLE_FUNCTION(0x6B96EDF7, sceGxmColorSurfaceSetScaleMode),
    HLE_FUNCTION(0xEE0B4DF0, sceGxmColorSurfaceGetGammaMode),
    HLE_FUNCTION(0xF5C89643, sceGxmColorSurfaceSetGammaMode),
    HLE_FUNCTION(0x200A96E1, sceGxmColorSurfaceGetDitherMode),
    HLE_FUNCTION(0x45027BAB, sceGxmColorSurfaceSetDitherMode),
    HLE_FUNCTION(0xF33D9980, sceGxmColorSurfaceGetStrideInPixels),
    HLE_FUNCTION(0xCA9D41D1, sceGxmDepthStencilSurfaceInit),
    HLE_FUNCTION(0xA41DB0D6, sceGxmDepthStencilSurfaceInitDisabled),
    HLE_FUNCTION(0x082200E1, sceGxmDepthStencilSurfaceIsEnabled),
    HLE_FUNCTION(0x8504038D, sceGxmDepthStencilSurfaceGetFormat),
    HLE_FUNCTION(0x11628789, sceGxmDepthStencilSurfaceGetStrideInSamples),
    HLE_FUNCTION(0x2F5CC20C, sceGxmDepthStencilSurfaceGetForceLoadMode),
    HLE_FUNCTION(0x0C44ACD7, sceGxmDepthStencilSurfaceSetForceLoadMode),
    HLE_FUNCTION(0x544AA05A, sceGxmDepthStencilSurfaceGetForceStoreMode),
    HLE_FUNCTION(0x12AAA7AF, sceGxmDepthStencilSurfaceSetForceStoreMode),
    HLE_FUNCTION(0xAAFC062B, sceGxmDepthStencilSurfaceGetBackgroundStencil),
    HLE_FUNCTION(0xF5D3F3E8, sceGxmDepthStencilSurfaceSetBackgroundStencil),
    HLE_FUNCTION(0x85D4DEFE, sceGxmDepthStencilSurfaceGetBackgroundMask),
    HLE_FUNCTION(0xF28A688A, sceGxmDepthStencilSurfaceSetBackgroundMask),
    HLE_FUNCTION(0x269B56BE, sceGxmDepthStencilSurfaceGetBackgroundDepth),
    HLE_FUNCTION(0x32F280F0, sceGxmDepthStencilSurfaceSetBackgroundDepth),
};

}  // namespace

void register_surfaces() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
