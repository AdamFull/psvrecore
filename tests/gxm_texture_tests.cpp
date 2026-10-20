// GXM texture decoding: YUV textures (planar 4:2:0 in two or three planes, interleaved 4:2:2) through their colour
// space conversions; colour surfaces' layouts as textures read them.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/gxm/surface.hpp"
#include "psvrecore/gxm/texture.hpp"

using namespace psvrecore;
using namespace psvrecore::gxm;

namespace {

// The reference: standard-range BT.601 or BT.709, written out separately from the decoder's.
void expect_rgb(u8 y, u8 u, u8 v, bool bt709, u8 out[3]) {
    const f64 yy = (y - 16.0) * 255.0 / 219.0, cb = (u - 128.0) * 255.0 / 224.0, cr = (v - 128.0) * 255.0 / 224.0;
    const f64 kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    const f64 r = yy + 2 * (1 - kr) * cr;
    const f64 b = yy + 2 * (1 - kb) * cb;
    const f64 g = (yy - kr * r - kb * b) / kg;
    const f64 c[3] = {r, g, b};
    for (u32 k = 0; k < 3; ++k) out[k] = static_cast<u8>(std::lround(std::fmin(std::fmax(c[k], 0.0), 255.0)));
}

struct Memory {
    nx::vector<u8> bytes;
    bool read(u32 address, void* out, u32 size) const {
        if (address + size > bytes.size()) return false;
        std::memcpy(out, bytes.data() + address, size);
        return true;
    }
};

bool near(const u8* got, const u8 want[3]) {
    for (u32 k = 0; k < 3; ++k)
        if (std::abs(got[k] - want[k]) > 1) return false;
    return got[3] == 255;
}

}  // namespace

TEST_CASE("gxm textures: YUV420 in two planes (YVU: U then V in memory), BT.709 through CSC1") {
    // A 16x4 image (each plane's rows are rounded up to 8 texels: none here): Y a ramp, a U/V pair per 2x2 block.
    constexpr u32 w = 16, h = 4;
    Memory m;
    m.bytes.assign(w * h + w * h / 2, 0);
    for (u32 i = 0; i < w * h; ++i) m.bytes[i] = static_cast<u8>(20 + i * 3);
    for (u32 i = 0; i < w * h / 4; ++i) {
        m.bytes[w * h + 2 * i] = static_cast<u8>(60 + i * 9);        // U
        m.bytes[w * h + 2 * i + 1] = static_cast<u8>(200 - i * 7);  // V
    }
    TextureInfo t;
    t.type = texture_type::kLinear;
    t.base_format = 0x90000000u;  // SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2
    t.swizzle = 3;                // SCE_GXM_TEXTURE_SWIZZLE_YVU_CSC1
    t.width = w, t.height = h;
    auto texels = read_texels(t, [&](u32 a, void* o, u32 s) { return m.read(a, o, s); });
    REQUIRE(texels.has_value());
    REQUIRE(texels->bytes == 4);
    bool all = true;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const u32 c = (y / 2) * (w / 2) + x / 2;
            u8 want[3];
            expect_rgb(m.bytes[y * w + x], m.bytes[w * h + 2 * c], m.bytes[w * h + 2 * c + 1], true, want);
            all &= near(texels->data.data() + (y * w + x) * 4, want);
        }
    CHECK(all);
    // The same data as YUV (U in each texel's high byte) reads the chroma the other way round.
    t.swizzle = 2;  // SCE_GXM_TEXTURE_SWIZZLE_YUV_CSC1
    texels = read_texels(t, [&](u32 a, void* o, u32 s) { return m.read(a, o, s); });
    REQUIRE(texels.has_value());
    u8 want[3];
    expect_rgb(m.bytes[0], m.bytes[w * h + 1], m.bytes[w * h], true, want);
    CHECK(near(texels->data.data(), want));
}

TEST_CASE("gxm textures: YUV420 in three planes and YUV422, BT.601 through CSC0; profiles can be changed") {
    constexpr u32 w = 16, h = 2;
    Memory m;
    m.bytes.assign(w * h + 2 * (w / 2) * (h / 2), 0);
    for (u32 i = 0; i < w * h; ++i) m.bytes[i] = static_cast<u8>(16 + i * 7);
    for (u32 i = 0; i < w / 2; ++i) {
        m.bytes[w * h + i] = static_cast<u8>(90 + i * 9);            // first chroma plane
        m.bytes[w * h + w / 2 + i] = static_cast<u8>(170 - i * 11);  // second
    }
    TextureInfo t;
    t.base_format = 0x91000000u;  // SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3
    t.swizzle = 0;                // YUV_CSC0: Y, U, V
    t.width = w, t.height = h;
    auto texels = read_texels(t, [&](u32 a, void* o, u32 s) { return m.read(a, o, s); });
    REQUIRE(texels.has_value());
    bool all = true;
    for (u32 x = 0; x < w; ++x) {
        u8 want[3];
        expect_rgb(m.bytes[w + x], m.bytes[w * h + x / 2], m.bytes[w * h + w / 2 + x / 2], false, want);
        all &= near(texels->data.data() + (w + x) * 4, want);
    }
    CHECK(all);

    // YUYV: one 32-bit word for two pixels, Y1 U Y0 V from its top byte down (bytes V, Y0, U, Y1).
    Memory p;
    p.bytes = {100, 50, 150, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // (a row of 8 texels)
    TextureInfo q;
    q.base_format = 0x92000000u;  // SCE_GXM_TEXTURE_BASE_FORMAT_YUV422
    q.swizzle = 0;                // YUYV_CSC0
    q.width = 2, q.height = 1;
    texels = read_texels(q, [&](u32 a, void* o, u32 s) { return p.read(a, o, s); });
    REQUIRE(texels.has_value());
    u8 left[3], right[3];
    expect_rgb(50, 150, 100, false, left);
    expect_rgb(200, 150, 100, false, right);
    CHECK(near(texels->data.data(), left));
    CHECK(near(texels->data.data() + 4, right));

    // CSC0 switched to full-range BT.601: Y 0 is black, 255 white.
    set_yuv_profile(0, 2);
    p.bytes = {128, 0, 128, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    texels = read_texels(q, [&](u32 a, void* o, u32 s) { return p.read(a, o, s); });
    set_yuv_profile(0, 0);
    REQUIRE(texels.has_value());
    CHECK(texels->data[0] == 0);
    CHECK(texels->data[4] == 255);
    CHECK(texels->data[6] == 255);
}

TEST_CASE("gxm colour surfaces: what a scene writes in each layout reads back as a texture of that type") {
    // Pixels (x, y, x + y, 255) written where a surface of each layout has them; a 4-byte RGBA texture of the same
    // type over the same memory reads them back in place.
    struct Case {
        u32 kind, texture_type, width, height, stride;
    };
    const Case cases[] = {
        {SurfaceLayout::kLinear, texture_type::kLinearStrided, 40, 20, 48},
        {SurfaceLayout::kTiled, texture_type::kTiled, 40, 36, 64},  // (2 x 2 tiles, the last ones cut)
        {SurfaceLayout::kSwizzled, texture_type::kSwizzled, 32, 16, 32},
        {SurfaceLayout::kSwizzled, texture_type::kSwizzled, 16, 64, 16},
    };
    for (const Case& c : cases) {
        SurfaceLayout l;
        l.kind = c.kind, l.stride = c.stride, l.bytes = 4, l.width = c.width, l.height = c.height;
        TextureInfo t;
        t.type = c.texture_type;
        t.base_format = 0x0C000000u;  // SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 (ABGR: memory order R, G, B, A)
        t.width = c.width, t.height = c.height;
        t.stride = c.stride * 4;
        Memory m;  // (as much as the texture reads: its footprint, at least the surface's)
        m.bytes.assign(nx::max(static_cast<usize>(l.span()), static_cast<usize>(texture_size(t))) + 8, 0);
        for (u32 y = 0; y < c.height; ++y)
            for (u32 x = 0; x < c.width;) {
                // (in runs, as the renderer writes them back)
                const u32 n = l.run(x);
                for (u32 i = 0; i < n; ++i) {
                    u8* p = m.bytes.data() + l.offset(x, y) + i * 4;
                    p[0] = static_cast<u8>(x + i), p[1] = static_cast<u8>(y), p[2] = static_cast<u8>(x + i + y), p[3] = 255;
                }
                x += n;
            }
        auto texels = read_texels(t, [&](u32 a, void* o, u32 s) { return m.read(a, o, s); });
        REQUIRE(texels.has_value());
        REQUIRE(texels->bytes == 4);
        bool all = true;
        for (u32 y = 0; y < c.height; ++y)
            for (u32 x = 0; x < c.width; ++x) {
                const u8* p = texels->data.data() + (usize{y} * c.width + x) * 4;
                all &= p[0] == static_cast<u8>(x) && p[1] == static_cast<u8>(y) && p[2] == static_cast<u8>(x + y) && p[3] == 255;
            }
        CHECK(all);
    }
}

TEST_CASE("gxm textures: 8-bit components in every swizzle come out as their bytes, 0 or 255") {
    Memory m;
    m.bytes = {0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44};
    auto read = [&](u32 address, void* out, u32 size) { return m.read(address, out, size); };
    auto texel = [&](u32 base_format, u32 swizzle) {
        TextureInfo t;
        t.base_format = base_format;
        t.swizzle = swizzle;
        t.type = texture_type::kLinear;
        t.width = 2, t.height = 1;
        t.stride = 8;
        auto texels = read_texels(t, read);
        REQUIRE(texels);
        REQUIRE(texels->bytes == 4);
        u32 v;
        std::memcpy(&v, texels->data.data() + 4, 4);  // (the second texel)
        return v;
    };
    // (U8U8U8U8: c0 the lowest byte; RGBA out, as R in the lowest byte)
    CHECK(texel(0x0C000000, 0) == 0x44332211u);  // ABGR
    CHECK(texel(0x0C000000, 1) == 0x44112233u);  // ARGB
    CHECK(texel(0x0C000000, 2) == 0x11223344u);  // RGBA
    CHECK(texel(0x0C000000, 3) == 0x11443322u);  // BGRA
    CHECK(texel(0x0C000000, 4) == 0xFF332211u);  // 1BGR
    CHECK(texel(0x0C000000, 5) == 0xFF112233u);  // 1RGB
    CHECK(texel(0x0C000000, 6) == 0xFF223344u);  // RGB1
    CHECK(texel(0x0C000000, 7) == 0xFF443322u);  // BGR1
}

TEST_CASE("gxm textures: the fast paths give the general path's texels (paletted, blocks, 8-bit, each layout)") {
    Memory m;
    m.bytes.resize(1u << 21);
    u32 seed = 12345;
    for (u8& b : m.bytes) b = static_cast<u8>((seed = seed * 1103515245u + 12345u) >> 16);
    auto read = [&](u32 address, void* out, u32 size) { return m.read(address, out, size); };
    struct Format {
        u32 base_format, bits;
    };
    const Format formats[] = {{0x94000000, 4}, {0x95000000, 8}, {0x85000000, 4}, {0x86000000, 8}, {0x87000000, 8}, {0x0C000000, 32}};
    const u32 types[] = {texture_type::kSwizzled, texture_type::kLinear, texture_type::kLinearStrided, texture_type::kTiled};
    const u32 sizes[][2] = {{37, 23}, {64, 64}, {128, 32}, {16, 256}, {1, 1}};
    u32 compared = 0;
    for (const Format& f : formats)
        for (u32 type : types)
            for (const auto& size : sizes)
                for (u32 swizzle = 0; swizzle < 8; ++swizzle)
                    for (u32 levels = 1; levels <= (type == texture_type::kSwizzled ? 3u : 1u); levels += 2) {
                        const bool block = f.base_format >= 0x85000000 && f.base_format <= 0x87000000;
                        if (block && (type == texture_type::kTiled || swizzle > 0)) continue;
                        if (type == texture_type::kLinearStrided && block) continue;
                        TextureInfo t;
                        t.base_format = f.base_format;
                        t.swizzle = swizzle;
                        t.type = type;
                        t.width = size[0], t.height = size[1];
                        t.levels = levels;
                        t.mipmapped = levels > 1;
                        t.address = 0x1000;
                        t.palette = 0x100000;
                        t.stride = ((t.width + 7) & ~7u) * f.bits / 8 + (type == texture_type::kLinearStrided ? 16 : 0);
                        auto fast = read_texels(t, read);
                        auto reference = detail::read_texels(t, read, true);
                        REQUIRE(static_cast<bool>(fast) == static_cast<bool>(reference));
                        if (!fast) continue;
                        CHECK(fast->bytes == reference->bytes);
                        CHECK(fast->levels == reference->levels);
                        if (fast->data != reference->data) {
                            usize first = 0;
                            while (first < fast->data.size() && fast->data[first] == reference->data[first]) ++first;
                            std::fprintf(stderr, "the fast and general paths differ: format %08x type %u %ux%u swizzle %u levels %u at byte %zu of %zu\n",
                                         f.base_format, type, t.width, t.height, swizzle, levels, first, fast->data.size());
                        }
                        CHECK(fast->data == reference->data);
                        ++compared;
                    }
    CHECK(compared > 300);
}

TEST_CASE("gxm textures: BC blocks kept for the device are the texture's blocks in rows") {
    Memory m;
    m.bytes.resize(1u << 20);
    u32 seed = 777;
    for (u8& b : m.bytes) b = static_cast<u8>((seed = seed * 1103515245u + 12345u) >> 16);
    auto read = [&](u32 address, void* out, u32 size) { return m.read(address, out, size); };
    for (u32 format : {0x85000000u, 0x86000000u, 0x87000000u}) {
        for (u32 type : {texture_type::kSwizzled, texture_type::kLinear}) {
            TextureInfo t;
            t.base_format = format;
            t.type = type;
            t.width = 64, t.height = 32;
            t.address = 0x1000;
            t.stride = 64 * (format == 0x85000000u ? 4 : 8) / 8;
            auto kept = read_texels(t, read, true);
            REQUIRE(kept);
            CHECK(kept->bc == (format == 0x85000000u ? 1u : format == 0x86000000u ? 2u : 3u));
            // (the kept blocks as a linear texture of the same size decode as the original does)
            Memory linear;
            linear.bytes.assign(0x1000, 0);
            linear.bytes.insert(linear.bytes.end(), kept->data.begin(), kept->data.end());
            linear.bytes.resize(linear.bytes.size() + 64, 0);
            TextureInfo l = t;
            l.type = texture_type::kLinear;
            auto from_kept = read_texels(l, [&](u32 a, void* o, u32 s) { return linear.read(a, o, s); });
            auto original = read_texels(t, read);
            REQUIRE(from_kept);
            REQUIRE(original);
            CHECK(from_kept->data == original->data);
        }
    }
}
