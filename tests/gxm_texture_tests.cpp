// GXM texture decoding: YUV textures (planar 4:2:0 in two or three planes, interleaved 4:2:2) through their colour
// space conversions; colour surfaces' layouts as textures read them.

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/gxm/surface.hpp"
#include "psvrecore/gxm/target_texture.hpp"
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

TEST_CASE("gxm surfaces: U2F10F10F10 packed a pixel at a time and many at a time to the same words") {
    // (every half in each channel, with the others varied)
    nx::vector<u16> halves(65536 * 4);
    for (u32 v = 0; v < 65536; ++v) {
        halves[v * 4 + 0] = static_cast<u16>(v);
        halves[v * 4 + 1] = static_cast<u16>(v * 7919u);
        halves[v * 4 + 2] = static_cast<u16>(v ^ 0x5A5Au);
        halves[v * 4 + 3] = static_cast<u16>(v);
    }
    nx::vector<u32> many(65536);
    gxm::pack_u2f10f10f10(halves.data(), many.data(), many.size());
    usize differ = 0;
    for (u32 v = 0; v < 65536; ++v) differ += many[v] != gxm::pack_u2f10f10f10(halves.data() + v * 4);
    CHECK(differ == 0);
    // (an alpha of 1: both of its bits)
    const u16 a[4] = {0, 0, 0, 0x3C00};
    CHECK((gxm::pack_u2f10f10f10(a) >> 30) == 3);
}

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

TEST_CASE("gxm textures: the fast paths give the general path's texels (paletted, blocks, PVRTC, 8-bit, 32-bit floats, each layout)") {
    Memory m;
    m.bytes.resize(1u << 21);
    u32 seed = 12345;
    for (u8& b : m.bytes) b = static_cast<u8>((seed = seed * 1103515245u + 12345u) >> 16);
    auto read = [&](u32 address, void* out, u32 size) { return m.read(address, out, size); };
    struct Format {
        u32 base_format, bits;
    };
    const Format formats[] = {{0x94000000, 4}, {0x95000000, 8}, {0x85000000, 4}, {0x86000000, 8}, {0x87000000, 8}, {0x0C000000, 32},
                              {0x00000000, 8}, {0x07000000, 16}, {0x98000000, 24}, {0x12000000, 32}, {0x13000000, 32},
                              {0x9A000000, 32}, {0x81000000, 4}, {0x83000000, 4}};
    const u32 types[] = {texture_type::kSwizzled, texture_type::kLinear, texture_type::kLinearStrided, texture_type::kTiled};
    const u32 sizes[][2] = {{37, 23}, {64, 64}, {128, 32}, {16, 256}, {1, 1}};
    u32 compared = 0;
    for (const Format& f : formats)
        for (u32 type : types)
            for (const auto& size : sizes)
                for (u32 swizzle = 0; swizzle < 8; ++swizzle)
                    for (u32 levels = 1; levels <= (type == texture_type::kSwizzled ? 3u : 1u); levels += 2) {
                        const bool block = (f.base_format >= 0x85000000 && f.base_format <= 0x87000000) ||
                                           f.base_format == 0x81000000 || f.base_format == 0x83000000;
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

TEST_CASE("gxm textures: F32 and F32M one float a texel give the general path's RGBA through rgba_of") {
    Memory m;
    m.bytes.resize(1u << 20);
    u32 seed = 4242;
    for (u8& b : m.bytes) b = static_cast<u8>((seed = seed * 1103515245u + 12345u) >> 16);
    auto read = [&](u32 address, void* out, u32 size) { return m.read(address, out, size); };
    u32 compared = 0;
    for (u32 format : {0x12000000u, 0x13000000u})
        for (u32 type : {texture_type::kSwizzled, texture_type::kLinearStrided, texture_type::kTiled})
            for (u32 swizzle = 0; swizzle < 8; ++swizzle) {
                TextureInfo t;
                t.base_format = format;
                t.swizzle = swizzle;
                t.type = type;
                t.width = 37, t.height = 23;
                t.address = 0x1000;
                t.stride = ((t.width + 7) & ~7u) * 4 + (type == texture_type::kLinearStrided ? 16 : 0);
                auto single = read_texels(t, read, false, true);
                auto reference = detail::read_texels(t, read, true);
                REQUIRE(single);
                REQUIRE(reference);
                REQUIRE(single->single);
                REQUIRE(single->bytes == 4);
                REQUIRE(reference->bytes == 16);
                REQUIRE(single->data.size() * 4 == reference->data.size());
                bool same = true;
                for (usize i = 0; i < single->data.size() / 4; ++i)
                    for (u32 k = 0; k < 4; ++k) {
                        u32 want, got;
                        std::memcpy(&want, reference->data.data() + i * 16 + k * 4, 4);
                        const u8 of = single->rgba_of[k];
                        if (of == 0) std::memcpy(&got, single->data.data() + i * 4, 4);
                        else got = std::bit_cast<u32>(of == 1 ? 0.0f : 1.0f);
                        same &= want == got;
                    }
                CHECK(same);
                ++compared;
            }
    CHECK(compared == 48);
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

TEST_CASE("gxm textures: BC4 and BC5 (unsigned and signed) decode their endpoints and indices through the swizzle") {
    auto decode = [](u32 base_format, u32 swizzle, std::initializer_list<u8> blocks) {
        Memory m;
        m.bytes.assign(blocks.begin(), blocks.end());
        TextureInfo t;
        t.base_format = base_format;
        t.swizzle = swizzle;
        t.type = texture_type::kLinear;
        t.width = t.height = 4;
        t.stride = static_cast<u32>(blocks.size()) / 4;
        auto texels = read_texels(t, [&](u32 address, void* out, u32 size) { return m.read(address, out, size); });
        REQUIRE(texels);
        return *texels;
    };
    // (indices: texel 0 index 0, 1 index 1, 2 index 2, 3 index 6, the rest 0)
    const u8 i0 = 0x88, i1 = 0x0C;
    // UBC4, R: 255 and 0, eight values: index 2 (6 * 255 + 0) / 7, index 6 (2 * 255) / 7
    auto r = decode(0x88000000, 0, {255, 0, i0, i1, 0, 0, 0, 0});
    REQUIRE(r.bytes == 4);
    CHECK(r.data[0] == 255);
    CHECK(r.data[4] == 0);
    CHECK(r.data[8] == 219);
    CHECK(r.data[12] == 73);
    CHECK(r.data[1] == 0);
    CHECK(r.data[3] == 255);
    // UBC4, RRRR; 0 and 255 the other way round: six values, index 6 is 0 and 7 is 1
    auto rrrr = decode(0x88000000, 3, {0, 255, i0, i1, 0, 0, 0, 0});
    CHECK(rrrr.data[4 * 1 + 0] == 255);
    CHECK(rrrr.data[4 * 1 + 3] == 255);
    CHECK(rrrr.data[4 * 2 + 1] == 51);  // ((6 - 2) * 0 + (2 - 1) * 255) / 5
    CHECK(rrrr.data[4 * 3 + 2] == 0);
    // SBC4: 127 and -127 (1 and -1), as halves
    auto s4 = decode(0x89000000, 0, {0x7F, 0x81, i0, i1, 0, 0, 0, 0});
    REQUIRE(s4.bytes == 8);
    u16 h[4];
    std::memcpy(h, s4.data.data(), 8);
    CHECK(h[0] == 0x3C00);
    std::memcpy(h, s4.data.data() + 8, 8);
    CHECK(h[0] == 0xBC00);
    // UBC5, GR: R the first block, G the second
    auto gr = decode(0x8A000000, 0, {255, 0, i0, i1, 0, 0, 0, 0, 0x40, 0x10, 0, 0, 0, 0, 0, 0});
    REQUIRE(gr.bytes == 4);
    CHECK(gr.data[0] == 255);
    CHECK(gr.data[1] == 0x40);
    CHECK(gr.data[4 * 1 + 0] == 0);
    CHECK(gr.data[4 * 1 + 1] == 0x40);
    CHECK(gr.data[2] == 0);
    CHECK(gr.data[3] == 255);
}

TEST_CASE("gxm textures: unsigned packed floats (U2F10F10F10, F11F11F10) come out as halves") {
    auto halves = [](u32 base_format, u32 word) {
        Memory m;
        m.bytes.resize(4);
        std::memcpy(m.bytes.data(), &word, 4);
        TextureInfo t;
        t.base_format = base_format;
        t.type = texture_type::kLinear;
        t.width = t.height = 1;
        t.stride = 4;
        auto texels = read_texels(t, [&](u32 address, void* out, u32 size) { return m.read(address, out, size); });
        REQUIRE(texels);
        REQUIRE(texels->bytes == 8);
        nx::vector<u16> h(4);
        std::memcpy(h.data(), texels->data.data(), 8);
        return h;
    };
    // (a 10-bit float is a half's top 10 bits: 1.0 0x1E0, 2.0 0x200, 0.5 0x1C0, the largest 0x3DF = 64512)
    auto a = halves(0x9A000000, 0x1E0u | 0x200u << 10 | 0x1C0u << 20 | 3u << 30);
    CHECK(a[0] == 0x3C00);
    CHECK(a[1] == 0x4000);
    CHECK(a[2] == 0x3800);
    CHECK(a[3] == 0x3C00);
    auto b = halves(0x9A000000, 0x3DFu | 1u << 30);
    CHECK(b[0] == 0x7BE0);
    CHECK(b[1] == 0);
    CHECK(b[3] == 0x3555);  // (1/3)
    // (11-bit floats: a half's top 11 bits: 1.0 0x3C0, 0.25 0x340; 4.0 as a 10-bit float 0x220)
    auto c = halves(0x1A000000, 0x3C0u | 0x340u << 11 | 0x220u << 22);
    CHECK(c[0] == 0x3C00);
    CHECK(c[1] == 0x3400);
    CHECK(c[2] == 0x4400);
    CHECK(c[3] == 0x3C00);
}

TEST_CASE("gxm textures: X8U24 (a D24 depth surface as a texture) gives its depth in R and the top byte in G") {
    auto halves = [](u32 swizzle, u32 word) -> nx::vector<u16> {
        Memory m;
        m.bytes.resize(4);
        std::memcpy(m.bytes.data(), &word, 4);
        TextureInfo t;
        t.base_format = 0x15000000;
        t.swizzle = swizzle;
        t.type = texture_type::kLinear;
        t.width = t.height = 1;
        t.stride = 4;
        auto texels = read_texels(t, [&](u32 address, void* out, u32 size) { return m.read(address, out, size); });
        if (!texels) return {};
        REQUIRE(texels->bytes == 8);
        nx::vector<u16> h(4);
        std::memcpy(h.data(), texels->data.data(), 8);
        return h;
    };
    auto far = halves(0, 0xFFFFFFFFu);  // (SD: the depth 1.0, the stencil byte 255)
    REQUIRE(far.size() == 4);
    CHECK(far[0] == 0x3C00);
    CHECK(far[1] == 0x3C00);
    CHECK(far[2] == 0);
    CHECK(far[3] == 0x3C00);
    auto zero = halves(0, 0xAB000000u);  // (depth 0 whatever the top byte)
    REQUIRE(zero.size() == 4);
    CHECK(zero[0] == 0);
    auto half = halves(0, 0x00800000u);  // (0x800000 / 0xFFFFFF: just over 0.5)
    REQUIRE(half.size() == 4);
    CHECK(half[0] == 0x3800);
    CHECK(halves(1, 0).empty());  // (DS: not implemented)
}

TEST_CASE("gxm textures: PVRTC 4bpp (I and II): the weights, punch-through, and the alpha each generation reaches") {
    // An 8 x 8 texture of four identical blocks (so every texel's four blocks are the same): texels 0-3 of the
    // first row with indices 0-3, the rest 0.
    auto texels = [](u32 base_format, u32 hi) {
        Memory m;
        const u64 block = (u64{hi} << 32) | (0u << 0) | (1u << 2) | (2u << 4) | (3u << 6);
        m.bytes.resize(4 * 8);
        for (u32 i = 0; i < 4; ++i) std::memcpy(m.bytes.data() + 8 * i, &block, 8);
        TextureInfo t;
        t.base_format = base_format;
        t.type = texture_type::kSwizzled;
        t.width = t.height = 8;
        auto out = read_texels(t, [&](u32 address, void* o, u32 size) { return m.read(address, o, size); });
        REQUIRE(out);
        REQUIRE(out->bytes == 4);
        nx::vector<u32> row(4);
        std::memcpy(row.data(), out->data.data(), 16);
        return row;
    };
    constexpr u32 kPvrtcII = 0x83000000, kPvrtcI = 0x81000000;
    // Opaque (bit 63): C0 black, C1 white (R5G5B5 31, 31, 31): the weights 0, 3/8, 5/8, 1.
    const u32 opaque = 0x80000000u | (0x7FFFu << 16);
    auto a = texels(kPvrtcII, opaque);
    CHECK(a[0] == 0xFF000000u);
    CHECK(a[1] == 0xFF606060u);  // (96)
    CHECK(a[2] == 0xFF9F9F9Fu);  // (159)
    CHECK(a[3] == 0xFFFFFFFFu);
    // Mode 1 (M, bit 32): index 1 the mean, index 2 transparent black in PVRTC-II.
    auto b = texels(kPvrtcII, opaque | 1u);
    CHECK(b[1] == 0xFF808080u);
    CHECK(b[2] == 0x00000000u);
    // Translucent C1 with alpha 7 (A3R4G4B4 0x7FFF): PVRTC-II's reaches 255, PVRTC-I's 0xEE (and in PVRTC-I, bit 63
    // is C1's own flag: C0 stays opaque with bit 47 set).
    const u32 translucent = 0x7FFFu << 16;
    CHECK((texels(kPvrtcII, translucent)[3] >> 24) == 0xFFu);
    CHECK((texels(kPvrtcI, translucent | 0x8000u)[3] >> 24) == 0xEEu);
    CHECK((texels(kPvrtcI, translucent | 0x8000u)[0] >> 24) == 0xFFu);
}

TEST_CASE("gxm textures: PVRTC 2bpp: direct bits, and the stored texels' neighbours in each interpolation mode") {
    // A 16 x 8 texture of four identical 8x4 blocks (every texel's four blocks the same), C0 black and C1 white,
    // opaque: the first row's first texels.
    auto texels = [](u32 base_format, u32 hi, u32 lo) {
        Memory m;
        const u64 block = (u64{hi} << 32) | lo;
        m.bytes.resize(4 * 8);
        for (u32 i = 0; i < 4; ++i) std::memcpy(m.bytes.data() + 8 * i, &block, 8);
        TextureInfo t;
        t.base_format = base_format;
        t.type = texture_type::kSwizzled;
        t.width = 16;
        t.height = 8;
        auto out = read_texels(t, [&](u32 address, void* o, u32 size) { return m.read(address, o, size); });
        REQUIRE(out);
        REQUIRE(out->bytes == 4);
        nx::vector<u32> row(3);
        std::memcpy(row.data(), out->data.data(), 12);
        return row;
    };
    constexpr u32 kPvrtcII2 = 0x82000000, kPvrtcI2 = 0x80000000;
    // (PVRTC-II: bit 63 both opaque; PVRTC-I: bit 63 C1's flag, bit 47 C0's)
    const u32 opaque = 0x80000000u | (0x7FFFu << 16), opaque_i = opaque | 0x8000u;
    // M clear: a bit a texel.
    auto a = texels(kPvrtcII2, opaque, 0x2u);
    CHECK(a[0] == 0xFF000000u);
    CHECK(a[1] == 0xFFFFFFFFu);
    CHECK(a[2] == 0xFF000000u);
    CHECK(texels(kPvrtcI2, opaque_i, 0x2u)[1] == 0xFFFFFFFFu);
    CHECK(texels(kPvrtcI2, opaque_i, 0x2u)[0] == 0xFF000000u);
    // M set, bit 0 clear: the stored texels 2 bits each (texel 0 index 0, the others 3); texel 1 the mean of its four
    // neighbours' weights (0, 1, 1, 1 -> 6/8: 191).
    auto b = texels(kPvrtcII2, opaque | 1u, 0xFFFFFFFCu);
    CHECK(b[0] == 0xFF000000u);
    CHECK(b[1] == 0xFFBFBFBFu);
    CHECK(b[2] == 0xFFFFFFFFu);
    // Bit 0 set, bit 20 clear: left and right only (texel 0's index from bit 1: 0): 4/8.
    CHECK(texels(kPvrtcII2, opaque | 1u, 0xFFEFFFFDu)[1] == 0xFF808080u);
    // Bit 20 set: above and below only (both 1).
    CHECK(texels(kPvrtcII2, opaque | 1u, 0xFFFFFFFDu)[1] == 0xFFFFFFFFu);
}
