// Textures (SceGxmTexture): the control words in the hardware's layout (gxm/texture.hpp), set up and read back.

#include "psvrecore/gxm/texture.hpp"
#include "state.hpp"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;

using Words = TextureWords;

u32 base_of(u32 format) { return format & 0x9F000000u; }
u32 swizzle_of(u32 format) { return (format >> 12) & 7; }
bool pow2(u32 v) { return v && !(v & (v - 1)); }

i32 init(Ptr<Words> out, u32 data, u32 format, u32 width, u32 height, u32 mips, u32 type, u32 stride = 0) {
    if (!out) return err::kInvalidPointer;
    if (!width || !height || width > 4096 || height > 4096 || mips > 13) return err::kInvalidValue;
    if (!texel_bits(base_of(format))) return err::kInvalidValue;
    const bool pow2_only = type == texture_type::kSwizzled || type == texture_type::kCube;
    if (pow2_only && (!pow2(width) || !pow2(height))) return err::kInvalidValue;
    if (type == texture_type::kTiled && (width < 32 || height < 32)) return err::kInvalidValue;
    if (data & 3) return err::kInvalidAlignment;
    TextureInfo t;
    t.type = type;
    t.base_format = base_of(format);
    t.swizzle = swizzle_of(format);
    t.width = width;
    t.height = height;
    t.address = data;
    t.mipmapped = mips != 0;
    t.levels = nx::max(mips, 1u);
    t.stride = type == texture_type::kLinearStrided ? stride : ((width + 7) & ~7u) * texel_bits(t.base_format) / 8;
    encode_texture(t, out->words);
    return 0;
}

i32 sceGxmTextureInitLinear(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kLinear);
}
i32 sceGxmTextureInitLinearStrided(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 stride) {
    if (stride & 3) return err::kInvalidAlignment;
    return init(t, data, format, w, h, 0, texture_type::kLinearStrided, stride);
}
i32 sceGxmTextureInitSwizzled(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kSwizzled);
}
i32 sceGxmTextureInitSwizzledArbitrary(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kSwizzledArbitrary);
}
i32 sceGxmTextureInitTiled(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kTiled);
}
i32 sceGxmTextureInitCube(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kCube);
}
i32 sceGxmTextureInitCubeArbitrary(Ptr<Words> t, u32 data, u32 format, u32 w, u32 h, u32 mips) {
    return init(t, data, format, w, h, mips, texture_type::kCubeArbitrary);
}

// Reads a texture's description, changes it, writes it back.
template <class F>
i32 change(Ptr<Words> t, F&& f) {
    if (!t) return err::kInvalidPointer;
    TextureInfo info = decode_texture(t->words);
    if (const i32 e = f(info)) return e;
    encode_texture(info, t->words);
    return 0;
}
TextureInfo info_of(Ptr<const Words> t) { return t ? decode_texture(t->words) : TextureInfo{}; }

i32 sceGxmTextureValidate(Ptr<const Words> t) {
    if (!t) return err::kInvalidPointer;
    const TextureInfo i = decode_texture(t->words);
    return texel_bits(i.base_format) && i.address ? 0 : err::kInvalidTexture;
}
u32 sceGxmTextureGetType(Ptr<const Words> t) { return info_of(t).type << 29; }
u32 sceGxmTextureGetFormat(Ptr<const Words> t) {
    const TextureInfo i = info_of(t);
    return i.base_format | i.swizzle << 12;
}
i32 sceGxmTextureSetFormat(Ptr<Words> t, u32 format) {
    return change(t, [&](TextureInfo& i) {
        if (!texel_bits(base_of(format))) return err::kInvalidValue;
        i.base_format = base_of(format);
        i.swizzle = swizzle_of(format);
        return 0;
    });
}
u32 sceGxmTextureGetWidth(Ptr<const Words> t) { return info_of(t).width; }
u32 sceGxmTextureGetHeight(Ptr<const Words> t) { return info_of(t).height; }
i32 sceGxmTextureSetWidth(Ptr<Words> t, u32 w) {
    return change(t, [&](TextureInfo& i) { return w && w <= 4096 ? (i.width = w, 0) : err::kInvalidValue; });
}
i32 sceGxmTextureSetHeight(Ptr<Words> t, u32 h) {
    return change(t, [&](TextureInfo& i) { return h && h <= 4096 ? (i.height = h, 0) : err::kInvalidValue; });
}
u32 sceGxmTextureGetData(Ptr<const Words> t) { return info_of(t).address; }
i32 sceGxmTextureSetData(Ptr<Words> t, u32 data) {
    return change(t, [&](TextureInfo& i) { return data & 3 ? err::kInvalidAlignment : (i.address = data, 0); });
}
u32 sceGxmTextureGetStride(Ptr<const Words> t) { return info_of(t).stride; }
i32 sceGxmTextureSetStride(Ptr<Words> t, u32 stride) {
    return change(t, [&](TextureInfo& i) {
        if (i.type != texture_type::kLinearStrided) return err::kInvalidValue;
        return stride & 3 ? err::kInvalidAlignment : (i.stride = stride, 0);
    });
}
u32 sceGxmTextureGetMipmapCount(Ptr<const Words> t) {
    const TextureInfo i = info_of(t);
    return i.mipmapped ? i.levels : 0;
}
i32 sceGxmTextureSetMipmapCount(Ptr<Words> t, u32 count) {
    return change(t, [&](TextureInfo& i) {
        if (count > 13) return err::kInvalidValue;
        i.mipmapped = count != 0;
        i.levels = nx::max(count, 1u);
        return 0;
    });
}
u32 sceGxmTextureGetMinFilter(Ptr<const Words> t) { return info_of(t).min_linear ? 1 : 0; }
u32 sceGxmTextureGetMagFilter(Ptr<const Words> t) { return info_of(t).mag_linear ? 1 : 0; }
u32 sceGxmTextureGetMipFilter(Ptr<const Words> t) { return info_of(t).mip_linear ? 0x200 : 0; }
i32 sceGxmTextureSetMinFilter(Ptr<Words> t, u32 f) { return change(t, [&](TextureInfo& i) { return i.min_linear = f != 0, 0; }); }
i32 sceGxmTextureSetMagFilter(Ptr<Words> t, u32 f) { return change(t, [&](TextureInfo& i) { return i.mag_linear = f != 0, 0; }); }
i32 sceGxmTextureSetMipFilter(Ptr<Words> t, u32 f) { return change(t, [&](TextureInfo& i) { return i.mip_linear = f != 0, 0; }); }
u32 sceGxmTextureGetUAddrMode(Ptr<const Words> t) { return info_of(t).u_address; }
u32 sceGxmTextureGetVAddrMode(Ptr<const Words> t) { return info_of(t).v_address; }
i32 sceGxmTextureSetUAddrMode(Ptr<Words> t, u32 m) {
    return change(t, [&](TextureInfo& i) { return m > 7 ? err::kInvalidValue : (i.u_address = m, 0); });
}
i32 sceGxmTextureSetVAddrMode(Ptr<Words> t, u32 m) {
    return change(t, [&](TextureInfo& i) { return m > 7 ? err::kInvalidValue : (i.v_address = m, 0); });
}
u32 sceGxmTextureGetLodBias(Ptr<const Words> t) { return info_of(t).lod_bias; }
i32 sceGxmTextureSetLodBias(Ptr<Words> t, u32 bias) {
    return change(t, [&](TextureInfo& i) { return bias > 63 ? err::kInvalidValue : (i.lod_bias = bias, 0); });
}
u32 sceGxmTextureGetLodMin(Ptr<const Words> t) { return info_of(t).lod_min; }
i32 sceGxmTextureSetLodMin(Ptr<Words> t, u32 lod) {
    return change(t, [&](TextureInfo& i) { return lod > 15 ? err::kInvalidValue : (i.lod_min = lod, 0); });
}
u32 sceGxmTextureGetGammaMode(Ptr<const Words> t) { return info_of(t).gamma << 27; }
i32 sceGxmTextureSetGammaMode(Ptr<Words> t, u32 mode) {
    return change(t, [&](TextureInfo& i) { return i.gamma = (mode >> 27) & 3, 0; });
}
u32 sceGxmTextureGetNormalizeMode(Ptr<const Words> t) { return info_of(t).normalize ? 0x80000000u : 0; }
i32 sceGxmTextureSetNormalizeMode(Ptr<Words> t, u32 mode) {
    return change(t, [&](TextureInfo& i) { return i.normalize = mode != 0, 0; });
}
u32 sceGxmTextureGetPalette(Ptr<const Words> t) { return info_of(t).palette; }
i32 sceGxmTextureSetPalette(Ptr<Words> t, u32 palette) {
    return change(t, [&](TextureInfo& i) { return palette & 63 ? err::kInvalidAlignment : (i.palette = palette, 0); });
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0x4811AECB, sceGxmTextureInitLinear),
    HLE_FUNCTION(0x6679BEF0, sceGxmTextureInitLinearStrided),
    HLE_FUNCTION(0xD572D547, sceGxmTextureInitSwizzled),
    HLE_FUNCTION(0x5DBFBA2C, sceGxmTextureInitSwizzledArbitrary),
    HLE_FUNCTION(0xE6F0DB27, sceGxmTextureInitTiled),
    HLE_FUNCTION(0x11DC8DC9, sceGxmTextureInitCube),
    HLE_FUNCTION(0xE3DF5E3B, sceGxmTextureInitCubeArbitrary),
    HLE_FUNCTION(0x5331BED3, sceGxmTextureValidate),
    HLE_FUNCTION(0xF65D4917, sceGxmTextureGetType),
    HLE_FUNCTION(0xE868D2B3, sceGxmTextureGetFormat),
    HLE_FUNCTION(0xFC943596, sceGxmTextureSetFormat),
    HLE_FUNCTION(0x126A3EB3, sceGxmTextureGetWidth),
    HLE_FUNCTION(0x5420A086, sceGxmTextureGetHeight),
    HLE_FUNCTION(0x2EA178BE, sceGxmTextureSetWidth),
    HLE_FUNCTION(0x5A690B60, sceGxmTextureSetWidth),  // (_sceGxmTextureSetWidth)
    HLE_FUNCTION(0xAEE7FDD1, sceGxmTextureSetHeight),
    HLE_FUNCTION(0x1B20D5DF, sceGxmTextureSetHeight),  // (_sceGxmTextureSetHeight)
    HLE_FUNCTION(0x5341BD46, sceGxmTextureGetData),
    HLE_FUNCTION(0x855814C4, sceGxmTextureSetData),
    HLE_FUNCTION(0xB0BD52F3, sceGxmTextureGetStride),
    HLE_FUNCTION(0x58D0EB0A, sceGxmTextureSetStride),
    HLE_FUNCTION(0xF7B7B1E4, sceGxmTextureGetMipmapCount),
    HLE_FUNCTION(0x4CC42929, sceGxmTextureGetMipmapCount),  // (sceGxmTextureGetMipmapCountUnsafe)
    HLE_FUNCTION(0xD2DC4643, sceGxmTextureSetMipmapCount),
    HLE_FUNCTION(0x920666C6, sceGxmTextureGetMinFilter),
    HLE_FUNCTION(0xAE7FBB51, sceGxmTextureGetMagFilter),
    HLE_FUNCTION(0xCE94CA15, sceGxmTextureGetMipFilter),
    HLE_FUNCTION(0x416764E3, sceGxmTextureSetMinFilter),
    HLE_FUNCTION(0xFA695FD7, sceGxmTextureSetMagFilter),
    HLE_FUNCTION(0x1CA9FE0B, sceGxmTextureSetMipFilter),
    HLE_FUNCTION(0x2AE22788, sceGxmTextureGetUAddrMode),
    HLE_FUNCTION(0xC037DA83, sceGxmTextureGetUAddrMode),  // (sceGxmTextureGetUAddrModeSafe)
    HLE_FUNCTION(0x46136CA9, sceGxmTextureGetVAddrMode),
    HLE_FUNCTION(0xD2F0D9C1, sceGxmTextureGetVAddrMode),  // (sceGxmTextureGetVAddrModeSafe)
    HLE_FUNCTION(0x4281763E, sceGxmTextureSetUAddrMode),
    HLE_FUNCTION(0x8699ECF4, sceGxmTextureSetUAddrMode),  // (sceGxmTextureSetUAddrModeSafe)
    HLE_FUNCTION(0x126CDAA3, sceGxmTextureSetVAddrMode),
    HLE_FUNCTION(0xFA22F6CC, sceGxmTextureSetVAddrMode),  // (sceGxmTextureSetVAddrModeSafe)
    HLE_FUNCTION(0x2DE55DA5, sceGxmTextureGetLodBias),
    HLE_FUNCTION(0xB65EE6F7, sceGxmTextureSetLodBias),
    HLE_FUNCTION(0xBE524A2C, sceGxmTextureGetLodMin),
    HLE_FUNCTION(0xB79E43DD, sceGxmTextureSetLodMin),
    HLE_FUNCTION(0xF23FCE81, sceGxmTextureGetGammaMode),
    HLE_FUNCTION(0xA6D9F4DA, sceGxmTextureSetGammaMode),
    HLE_FUNCTION(0x512BB86C, sceGxmTextureGetNormalizeMode),
    HLE_FUNCTION(0xCE8DDAD0, sceGxmTextureSetNormalizeMode),
    HLE_FUNCTION(0x0D189C30, sceGxmTextureGetPalette),
    HLE_FUNCTION(0xDD6AABFA, sceGxmTextureSetPalette),
};

}  // namespace

void register_textures() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
