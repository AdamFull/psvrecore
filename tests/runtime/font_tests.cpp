// Fonts: a TrueType font built here (a square glyph and one of off-curve points only), read and drawn by media/font
// and through ScePvf; and, when PSVRECORE_FONT_DIR names a folder of fonts with reference renders made by another
// rasterizer (FreeType, by scripts kept outside the repository), the glyphs of those fonts against them.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "nxtest.h"
#include "runtime_support.hpp"
#include "../../libs/runtime/src/media/font.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"

using namespace psvrecore;
using namespace psvrecore::runtime;
using namespace psvrecore::runtime::test;

namespace {

u32 u(i32 v) { return static_cast<u32>(v); }

struct Bytes {
    nx::vector<u8> b;
    Bytes& u8_(u32 v) {
        b.push_back(static_cast<u8>(v));
        return *this;
    }
    Bytes& u16_(u32 v) { return u8_(v >> 8).u8_(v); }
    Bytes& u32_(u32 v) { return u16_(v >> 16).u16_(v); }
    Bytes& tag(std::string_view s) {
        for (char c : s) u8_(static_cast<u8>(c));
        return *this;
    }
    Bytes& zeros(u32 n) {
        b.insert(b.end(), n, 0);
        return *this;
    }
    Bytes& pad4() {
        while (b.size() % 4) b.push_back(0);
        return *this;
    }
};

// A glyph of one contour: points with their on-curve flags, coordinates as 16-bit deltas.
Bytes glyph(std::initializer_list<std::array<i32, 2>> points, bool on_curve, i32 x0, i32 y0, i32 x1, i32 y1) {
    Bytes g;
    g.u16_(1).u16_(u(x0)).u16_(u(y0)).u16_(u(x1)).u16_(u(y1));
    g.u16_(static_cast<u32>(points.size() - 1)).u16_(0);
    for (usize i = 0; i < points.size(); ++i) g.u8_(on_curve ? 1 : 0);
    i32 last = 0;
    for (const auto& p : points) g.u16_(u(p[0] - last)), last = p[0];
    last = 0;
    for (const auto& p : points) g.u16_(u(p[1] - last)), last = p[1];
    return g.pad4();
}

// 1000 units an em; 'A' a square (0,0)-(500,700), advance 600; 'B' four off-curve points at a square's corners
// (0,0)-(500,500): a rounded shape through the sides' midpoints.
nx::vector<u8> make_font() {
    const Bytes a = glyph({{0, 0}, {500, 0}, {500, 700}, {0, 700}}, true, 0, 0, 500, 700);
    const Bytes b = glyph({{0, 0}, {500, 0}, {500, 500}, {0, 500}}, false, 0, 0, 500, 500);
    Bytes glyf;
    glyf.b = a.b;
    glyf.b.insert(glyf.b.end(), b.b.begin(), b.b.end());
    Bytes loca;
    loca.u16_(0).u16_(0).u16_(static_cast<u32>(a.b.size() / 2)).u16_(static_cast<u32>(glyf.b.size() / 2)).pad4();
    Bytes head;
    head.u32_(0x00010000).u32_(0).u32_(0).u32_(0x5F0F3CF5).u16_(0).u16_(1000).zeros(16);
    head.u16_(0).u16_(0).u16_(500).u16_(700).u16_(0).u16_(8).u16_(2).u16_(0).u16_(0).pad4();
    Bytes hhea;
    hhea.u32_(0x00010000).u16_(800).u16_(u(-200)).u16_(0).u16_(600).zeros(22).u16_(2).pad4();
    Bytes maxp;
    maxp.u32_(0x00005000).u16_(3).pad4();
    Bytes hmtx;
    hmtx.u16_(500).u16_(0).u16_(600).u16_(0).u16_(0).pad4();
    Bytes cmap;
    cmap.u16_(0).u16_(1).u16_(3).u16_(1).u32_(12);
    // format 4: two segments ('A'-'B' to glyphs 1-2, and the closing 0xFFFF)
    cmap.u16_(4).u16_(32).u16_(0).u16_(4).u16_(4).u16_(1).u16_(0);
    cmap.u16_(0x42).u16_(0xFFFF).u16_(0).u16_(0x41).u16_(0xFFFF).u16_(u(1 - 0x41) & 0xFFFF).u16_(1).u16_(0).u16_(0).pad4();
    const std::pair<const char*, const Bytes*> tables[] = {{"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea},
                                                           {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}};
    Bytes f;
    f.u32_(0x00010000).u16_(7).u16_(0).u16_(0).u16_(0);
    u32 at = 12 + 16 * 7;
    for (const auto& [t, data] : tables) {
        f.tag(t).u32_(0).u32_(at).u32_(static_cast<u32>(data->b.size()));
        at += static_cast<u32>(data->b.size());
    }
    for (const auto& [t, data] : tables) f.b.insert(f.b.end(), data->b.begin(), data->b.end());
    return f.b;
}

u32 sum(const media::FontBitmap& b) {
    u32 s = 0;
    for (u8 c : b.coverage) s += c;
    return s;
}

}  // namespace

TEST_CASE("font: a TrueType font's map, metrics and glyphs, drawn with whole and part pixels covered") {
    media::Font font;
    const nx::vector<u8> data = make_font();
    REQUIRE(font.load(data));
    CHECK(font.units_per_em() == 1000);
    CHECK(font.ascender() == 800);
    CHECK(font.descender() == -200);
    CHECK(font.glyph(0x41) == 1);
    CHECK(font.glyph(0x42) == 2);
    CHECK(font.glyph(0x43) == 0);
    CHECK(font.advance(1) == 600);
    CHECK(font.advance(2) == 600);  // (past the long metrics: the last advance)

    media::FontBitmap b;
    REQUIRE(font.rasterize(1, 0.02f, 0.02f, 0, 0, &b));  // (20 pixels an em: the square 10 x 14)
    CHECK(b.width == 10);
    CHECK(b.height == 14);
    CHECK(b.left == 0);
    CHECK(b.top == 14);
    CHECK(sum(b) == 140u * 255u);
    // Half a pixel to the right: the first and last columns half covered.
    REQUIRE(font.rasterize(1, 0.02f, 0.02f, 0.5f, 0, &b));
    CHECK(b.width == 11);
    CHECK(std::abs(static_cast<i32>(b.coverage[0]) - 128) <= 1);
    CHECK(b.coverage[5] == 255);
    CHECK(std::abs(static_cast<i32>(b.coverage[10]) - 128) <= 1);
    CHECK(std::abs(static_cast<i32>(sum(b)) - static_cast<i32>(140u * 255u)) <= 14 * 2);

    // The off-curve shape: within its control square, symmetric, its area between the inscribed diamond's and the square's.
    REQUIRE(font.rasterize(2, 0.04f, 0.04f, 0, 0, &b));
    CHECK(b.width == 20);
    CHECK(b.height == 20);
    const f32 area = static_cast<f32>(sum(b)) / 255.0f;
    CHECK(area > 200.0f);
    CHECK(area < 400.0f);
    for (u32 y = 0; y < 20; ++y)
        for (u32 x = 0; x < 10; ++x) CHECK(std::abs(b.coverage[y * 20 + x] - b.coverage[y * 20 + 19 - x]) <= 2);
}

namespace {
u32 g_font_allocs = 0, g_font_frees = 0;
void font_alloc(PsvContext* ctx, u32) {
    ctx->r[0] = scratch(ctx->r[1]);
    ++g_font_allocs;
}
void font_free(PsvContext* ctx, u32) {
    if (ctx->r[1]) ++g_font_frees;
}
}  // namespace

TEST_CASE("pvf: a font in memory, sized in points, its metrics and its glyphs drawn into 8- and 4-bit buffers") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kScePvf = 0x68D59260, kNewLib = 0x72E58672, kDoneLib = 0xE17717EC, kResolution = 0xC4444FB3,
                  kOpenMemory = 0x9E65E4ED, kClose = 0xD282C23C, kCharSize = 0xF17ADE4D, kCharInfo = 0xA88EEDB0,
                  kImageRect = 0x6C1B9CAF, kGlyphImage = 0x37DA496A, kAltChar = 0x830625C2, kIsElement = 0x9F018F25,
                  kPointToPixelH = 0x91F02F9A;
    GuestCode code;
    const u32 alloc = code.add(&font_alloc), free = code.add(&font_free);
    code.install();
    g_font_allocs = g_font_frees = 0;
    const u32 init = scratch(28), error = scratch(4);
    std::memset(t.rt.memory().host(init), 0, 28);
    psv_st32(init + 4, 4);
    psv_st32(init + 16, alloc);
    psv_st32(init + 24, free);
    const u32 lib = u(call32(ctx, kScePvf, kNewLib, {init, error}));
    REQUIRE(lib != 0);
    CHECK(psv_ld32(error) == 0);
    ctx->vfp.s[0] = std::bit_cast<u32>(144.0f);
    ctx->vfp.s[1] = std::bit_cast<u32>(144.0f);
    REQUIRE(call32(ctx, kScePvf, kResolution, {lib}) == 0);
    ctx->vfp.s[0] = std::bit_cast<u32>(10.0f);
    call32(ctx, kScePvf, kPointToPixelH, {lib, error});
    CHECK(std::bit_cast<f32>(ctx->vfp.s[0]) == 20.0f);  // (10 points at 144 dpi)

    const nx::vector<u8> data = make_font();
    const u32 file = scratch(static_cast<u32>(data.size()));
    std::memcpy(t.rt.memory().host(file), data.data(), data.size());
    const u32 font = u(call32(ctx, kScePvf, kOpenMemory, {lib, file, static_cast<u32>(data.size()), error}));
    REQUIRE(font != 0);
    ctx->vfp.s[0] = std::bit_cast<u32>(10.0f);
    ctx->vfp.s[1] = std::bit_cast<u32>(10.0f);
    REQUIRE(call32(ctx, kScePvf, kCharSize, {font}) == 0);  // (20 pixels an em)

    const u32 info = scratch(64), rect = scratch(4);
    REQUIRE(call32(ctx, kScePvf, kCharInfo, {font, 0x41, info}) == 0);
    CHECK(psv_ld32(info) == 10);
    CHECK(psv_ld32(info + 4) == 14);
    CHECK(static_cast<i32>(psv_ld32(info + 12)) == 0);
    CHECK(static_cast<i32>(psv_ld32(info + 16)) == 14);
    CHECK(psv_ld32(info + 20) == 640);   // (width64)
    CHECK(psv_ld32(info + 24) == 896);   // (height64)
    CHECK(psv_ld32(info + 40) == 896);   // (horizontal bearing y)
    CHECK(psv_ld32(info + 52) == 768);   // (horizontal advance: 600 units)
    REQUIRE(call32(ctx, kScePvf, kImageRect, {font, 0x41, rect}) == 0);
    CHECK(psv_ld16(rect) == 10);
    CHECK(psv_ld16(rect + 2) == 14);
    CHECK(call32(ctx, kScePvf, kIsElement, {font, 0x42}) == 1);
    CHECK(call32(ctx, kScePvf, kIsElement, {font, 0x43}) == 0);

    // 8-bit, the origin at (2, 20): the square over x 2-11, y 6-19.
    const u32 image = scratch(24), pixels = scratch(32 * 32);
    const auto set_image = [&](u32 format, u32 pitch) {
        std::memset(t.rt.memory().host(pixels), 0, 32 * 32);
        psv_st32(image, format);
        psv_st32(image + 4, 2 * 64);
        psv_st32(image + 8, 20 * 64);
        psv_st16(image + 12, 32);
        psv_st16(image + 14, 32);
        psv_st16(image + 16, pitch);
        psv_st16(image + 18, 0);
        psv_st32(image + 20, pixels);
    };
    set_image(2, 32);
    REQUIRE(call32(ctx, kScePvf, kGlyphImage, {font, 0x41, image}) == 0);
    const u8* px = t.rt.memory().host(pixels);
    CHECK(px[6 * 32 + 2] == 255);
    CHECK(px[19 * 32 + 11] == 255);
    CHECK(px[5 * 32 + 2] == 0);
    CHECK(px[6 * 32 + 1] == 0);
    CHECK(px[20 * 32 + 5] == 0);
    CHECK(px[6 * 32 + 12] == 0);
    // 4-bit: the left pixel of each byte in its low nibble.
    set_image(0, 16);
    REQUIRE(call32(ctx, kScePvf, kGlyphImage, {font, 0x41, image}) == 0);
    CHECK(px[6 * 16 + 1] == 0xFF);           // (x 2, low, and 3, high)
    CHECK((px[6 * 16 + 5] & 0xF0) == 0xF0);  // (x 11: high)
    CHECK((px[6 * 16 + 6] & 0x0F) == 0);     // (x 12)

    CHECK(u(call32(ctx, kScePvf, kGlyphImage, {font, 0x43, image})) == 0x8046000Du);  // (no glyph)
    REQUIRE(call32(ctx, kScePvf, kAltChar, {lib, 0x41}) == 0);
    CHECK(call32(ctx, kScePvf, kGlyphImage, {font, 0x43, image}) == 0);
    CHECK(call32(ctx, kScePvf, kClose, {font}) == 0);
    CHECK(u(call32(ctx, kScePvf, kClose, {font})) == 0x80460003u);
    CHECK(call32(ctx, kScePvf, kDoneLib, {lib}) == 0);
    CHECK(g_font_allocs == 2);
    CHECK(g_font_frees == 2);
}


TEST_CASE("pvf: the system fonts: listed, found, opened and drawn, across and down") {
    TestRuntime t;
    PsvContext* ctx = t.ctx();
    constexpr u32 kScePvf = 0x68D59260, kNewLib = 0x72E58672, kDoneLib = 0xE17717EC, kNumList = 0xF6C4A855,
                  kList = 0x66F2D767, kOptimum = 0x2761FEAC, kFind = 0x984E5BFE, kOpen = 0xE35434BB,
                  kJapanese = 0xFEEE373A, kClose = 0xD282C23C, kCharSize = 0xF17ADE4D, kCharInfo = 0xA88EEDB0,
                  kFontInfo = 0xAB0C7CF2, kGlyphImage = 0x37DA496A, kVertImage = 0x8AE8433A, kOutline = 0x17A7873B,
                  kIsElement = 0x9F018F25;
    GuestCode code;
    const u32 alloc = code.add(&font_alloc), free = code.add(&font_free);
    code.install();
    g_font_allocs = g_font_frees = 0;
    const u32 init = scratch(28), error = scratch(4);
    std::memset(t.rt.memory().host(init), 0, 28);
    psv_st32(init + 4, 4);
    psv_st32(init + 16, alloc);
    psv_st32(init + 24, free);
    const u32 lib = u(call32(ctx, kScePvf, kNewLib, {init, error}));
    REQUIRE(lib != 0);

    CHECK(call32(ctx, kScePvf, kNumList, {lib, error}) == 6);
    const u32 list = scratch(216 * 6), style = scratch(216);
    REQUIRE(call32(ctx, kScePvf, kList, {lib, list, 6}) == 0);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 16))) == "Roboto");
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 216 + 80))) == "Bold");
    CHECK(psv_ld16(list + 216 + 6) == 5);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 432 + 16))) == "Noto Sans JP");
    CHECK(psv_ld16(list + 432 + 10) == 1);  // (Japanese)
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 648 + 16))) == "Noto Sans KR");
    CHECK(psv_ld16(list + 648 + 10) == 3);  // (Korean)
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 864 + 16))) == "Noto Sans SC");
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(list + 1080 + 16))) == "Noto Sans TC");
    CHECK(psv_ld16(list + 1080 + 10) == 4);  // (Chinese)
    std::memset(t.rt.memory().host(style), 0, 216);
    psv_st16(style + 6, 5);  // (bold)
    CHECK(call32(ctx, kScePvf, kOptimum, {lib, style, error}) == 1);
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == 1);
    psv_st16(style + 10, 1);  // (Japanese bold: none exactly, the Japanese regular the closest)
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == -1);
    CHECK(psv_ld32(error) == 0x80460004u);
    CHECK(call32(ctx, kScePvf, kOptimum, {lib, style, error}) == 2);
    psv_st16(style + 6, 1);  // (Japanese regular)
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == 2);
    psv_st16(style + 10, 3);  // (Korean)
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == 3);
    psv_st16(style + 10, 4);  // (Chinese: Simplified first)
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == 4);
    psv_st16(style + 10, 5);  // (Japanese and Korean: none exactly, KR the closest: it has kana)
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == -1);
    CHECK(call32(ctx, kScePvf, kOptimum, {lib, style, error}) == 3);
    psv_st16(style + 4, 2);  // (Chinese serif: none exactly, SC the closest)
    psv_st16(style + 10, 4);
    CHECK(call32(ctx, kScePvf, kFind, {lib, style, error}) == -1);
    CHECK(call32(ctx, kScePvf, kOptimum, {lib, style, error}) == 4);

    const u32 font = u(call32(ctx, kScePvf, kOpen, {lib, 0, 0, error}));
    REQUIRE(font != 0);
    CHECK(psv_ld32(error) == 0);
    ctx->vfp.s[0] = std::bit_cast<u32>(20.0f);
    ctx->vfp.s[1] = std::bit_cast<u32>(20.0f);
    REQUIRE(call32(ctx, kScePvf, kCharSize, {font}) == 0);  // (20 pixels an em at 72 dpi)
    const u32 info = scratch(64), font_info = scratch(304);
    REQUIRE(call32(ctx, kScePvf, kCharInfo, {font, 0x48 /*H*/, info}) == 0);
    CHECK(psv_ld32(info + 4) >= 13);  // (a capital's height: about 0.71 em)
    CHECK(psv_ld32(info + 4) <= 16);
    REQUIRE(call32(ctx, kScePvf, kFontInfo, {font, font_info}) == 0);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(font_info + 84 + 16))) == "Roboto");
    CHECK(psv_ld32(font_info + 80) > 1000);  // (glyphs)
    CHECK(u(call32(ctx, kScePvf, kOutline, {font, 0x48, scratch(64)})) == 0x8046000Eu);

    // 'H' across at (4, 24) and down at (16, 2): both ink the buffer, the vertical one centred on x = 16.
    const u32 image = scratch(24), pixels = scratch(64 * 64);
    const auto draw = [&](u32 nid, i32 x, i32 y) {
        std::memset(t.rt.memory().host(pixels), 0, 64 * 64);
        psv_st32(image, 2);
        psv_st32(image + 4, u(x * 64));
        psv_st32(image + 8, u(y * 64));
        psv_st16(image + 12, 64), psv_st16(image + 14, 64), psv_st16(image + 16, 64), psv_st16(image + 18, 0);
        psv_st32(image + 20, pixels);
        REQUIRE(call32(ctx, kScePvf, nid, {font, 0x48, image}) == 0);
        u32 x0 = 64, x1 = 0, y0 = 64, y1 = 0;
        const u8* p = t.rt.memory().host(pixels);
        for (u32 j = 0; j < 64; ++j)
            for (u32 i = 0; i < 64; ++i)
                if (p[j * 64 + i] > 128) x0 = std::min(x0, i), x1 = std::max(x1, i), y0 = std::min(y0, j), y1 = std::max(y1, j);
        return std::array<u32, 4>{x0, y0, x1, y1};
    };
    const auto across = draw(kGlyphImage, 4, 24);
    CHECK(across[0] >= 4);
    CHECK(across[0] <= 7);
    CHECK(across[3] == 23);  // (on the baseline)
    const auto down = draw(kVertImage, 16, 2);
    CHECK(std::abs(static_cast<i32>(down[0] + down[2]) - 32) <= 2);
    CHECK(down[1] >= 2);

    // The default Japanese font: Noto Sans JP, kana and kanji drawn ('漢' across, an em of 20 pixels).
    const u32 japanese = u(call32(ctx, kScePvf, kJapanese, {lib, error}));
    REQUIRE(japanese != 0);
    REQUIRE(call32(ctx, kScePvf, kFontInfo, {japanese, font_info}) == 0);
    CHECK(std::string_view(reinterpret_cast<const char*>(t.rt.memory().host(font_info + 84 + 16))) == "Noto Sans JP");
    CHECK(psv_ld32(font_info + 80) > 10000);  // (glyphs)
    CHECK(call32(ctx, kScePvf, kIsElement, {japanese, 0x3042 /*あ*/}) == 1);
    CHECK(call32(ctx, kScePvf, kIsElement, {japanese, 0x6F22 /*漢*/}) == 1);
    CHECK(call32(ctx, kScePvf, kIsElement, {font, 0x3042}) == 0);  // (Roboto: no kana)
    ctx->vfp.s[0] = std::bit_cast<u32>(20.0f);
    ctx->vfp.s[1] = std::bit_cast<u32>(20.0f);
    REQUIRE(call32(ctx, kScePvf, kCharSize, {japanese}) == 0);
    REQUIRE(call32(ctx, kScePvf, kCharInfo, {japanese, 0x6F22, info}) == 0);
    CHECK(psv_ld32(info) >= 16);  // (a kanji: nearly an em wide and high)
    CHECK(psv_ld32(info) <= 20);
    CHECK(psv_ld32(info + 4) >= 16);
    CHECK(psv_ld32(info + 52) == 20 * 64);  // (advance: a full em)
    std::memset(t.rt.memory().host(pixels), 0, 64 * 64);
    psv_st32(image, 2);
    psv_st32(image + 4, 4 * 64);
    psv_st32(image + 8, 24 * 64);
    psv_st32(image + 20, pixels);
    REQUIRE(call32(ctx, kScePvf, kGlyphImage, {japanese, 0x6F22, image}) == 0);
    u32 ink = 0;
    for (u32 i = 0; i < 64 * 64; ++i) ink += t.rt.memory().host(pixels)[i] > 128;
    CHECK(ink > 60);
    CHECK(call32(ctx, kScePvf, kClose, {japanese}) == 0);
    // Korean, Simplified and Traditional Chinese by their index: each draws its own script.
    const std::pair<u32, u32> others[] = {{3, 0xD55C /*한*/}, {4, 0x6C49 /*汉*/}, {5, 0x9AD4 /*體*/}};
    for (const auto& [index, code] : others) {
        const u32 other = u(call32(ctx, kScePvf, kOpen, {lib, index, 0, error}));
        REQUIRE(other != 0);
        CHECK(call32(ctx, kScePvf, kIsElement, {other, code}) == 1);
        ctx->vfp.s[0] = std::bit_cast<u32>(20.0f);
        ctx->vfp.s[1] = std::bit_cast<u32>(20.0f);
        REQUIRE(call32(ctx, kScePvf, kCharSize, {other}) == 0);
        std::memset(t.rt.memory().host(pixels), 0, 64 * 64);
        REQUIRE(call32(ctx, kScePvf, kGlyphImage, {other, code, image}) == 0);
        u32 other_ink = 0;
        for (u32 i = 0; i < 64 * 64; ++i) other_ink += t.rt.memory().host(pixels)[i] > 128;
        CHECK(other_ink > 40);
        CHECK(call32(ctx, kScePvf, kClose, {other}) == 0);
    }
    CHECK(call32(ctx, kScePvf, kIsElement, {font, 0xD55C}) == 0);  // (Roboto: no Hangul)
    CHECK(call32(ctx, kScePvf, kClose, {font}) == 0);
    CHECK(call32(ctx, kScePvf, kDoneLib, {lib}) == 0);
    CHECK(g_font_allocs == g_font_frees);
}

// Reference renders: <font>.ref beside each font: records of a character (u32), its advance (26.6), the bitmap's
// left and top (pixels from the origin, y up), its width and height, and its coverage (bytes, rows top down), at
// 32 pixels an em. Another rasterizer's glyphs differ a little (its hinting, its rounding): the advance within a
// pixel, the box within 2 pixels, and nine tenths of each one's ink within a pixel of the other's.
TEST_CASE("font: glyphs of real fonts match another rasterizer's") {
    const auto dir = nx::os::environment_variable("PSVRECORE_FONT_DIR");
    if (!dir) {
        std::printf("(PSVRECORE_FONT_DIR not set: no fonts to check)\n");
        return;
    }
    auto entries = nx::fs::directory_entries(nx::fs::path_view(*dir));
    REQUIRE(entries.has_value());
    u32 fonts = 0;
    for (const auto& e : *entries) {
        const nx::string path(e.path);
        if (path.size() < 4 || path.substr(path.size() - 4) != ".ref") continue;
        const nx::string font_path = path.substr(0, path.size() - 4);
        auto font_data = nx::fs::file_read(nx::fs::path_view(font_path));
        auto ref = nx::fs::file_read(nx::fs::path_view(path));
        REQUIRE(font_data.has_value());
        REQUIRE(ref.has_value());
        media::Font font;
        REQUIRE(font.load(std::span<const u8>(font_data->data(), font_data->size())));
        const f32 scale = 32.0f / static_cast<f32>(font.units_per_em());
        u32 glyphs = 0, poor = 0;
        f32 worst = 0;
        for (usize at = 0; at + 24 <= ref->size();) {
            i32 rec[6];
            std::memcpy(rec, ref->data() + at, 24);
            at += 24;
            const u32 code = static_cast<u32>(rec[0]), w = static_cast<u32>(rec[4]), h = static_cast<u32>(rec[5]);
            const u8* cov = ref->data() + at;
            at += static_cast<usize>(w) * h;
            const u32 g = font.glyph(code);
            CHECK(g != 0);
            if (!g) continue;
            CHECK(std::abs(static_cast<i32>(std::lround(font.advance(g) * scale * 64)) - rec[1]) <= 64);
            media::FontBitmap b;
            REQUIRE(font.rasterize(g, scale, scale, 0, 0, &b));
            CHECK(std::abs(b.left - rec[2]) <= 2);
            CHECK(std::abs(b.top - rec[3]) <= 2);
            CHECK(std::abs(static_cast<i32>(b.width) - static_cast<i32>(w)) <= 2);
            CHECK(std::abs(static_cast<i32>(b.height) - static_cast<i32>(h)) <= 2);
            // The ink each puts within a pixel of the other's (hinting moves stems and bars by up to a pixel), placed
            // by the origin: theirs at (x, y) is ours at (x + their left - our left, our top - their top + y).
            const i32 ox = rec[2] - b.left, oy = b.top - rec[3];
            const auto ours_at = [&](i32 x, i32 y) -> f64 {
                const i32 bx = x + ox, by = y + oy;
                if (bx < 0 || by < 0 || bx >= static_cast<i32>(b.width) || by >= static_cast<i32>(b.height)) return 0.0;
                return b.coverage[static_cast<usize>(by) * b.width + static_cast<usize>(bx)] / 255.0;
            };
            const auto theirs_at = [&](i32 x, i32 y) -> f64 {
                if (x < 0 || y < 0 || x >= static_cast<i32>(w) || y >= static_cast<i32>(h)) return 0.0;
                return cov[static_cast<usize>(y) * w + static_cast<usize>(x)] / 255.0;
            };
            const auto near = [](auto at, i32 x, i32 y) {
                f64 m = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                    for (i32 dx = -1; dx <= 1; ++dx) m = std::max(m, at(x + dx, y + dy));
                return m;
            };
            f64 theirs = 0, theirs_met = 0, ours = 0, ours_met = 0;
            for (i32 y = -1; y <= static_cast<i32>(std::max(h, b.height)) + 2; ++y)
                for (i32 x = -2; x <= static_cast<i32>(std::max(w, b.width)) + 2; ++x) {
                    const f64 t = theirs_at(x, y), o = ours_at(x, y);
                    theirs += t, ours += o;
                    theirs_met += std::min(t, near(ours_at, x, y));
                    ours_met += std::min(o, near(theirs_at, x, y));
                }
            const f64 overlap = std::min(theirs > 0 ? theirs_met / theirs : 1.0, ours > 0 ? ours_met / ours : 1.0);
            worst = std::max(worst, static_cast<f32>(1.0 - overlap));
            poor += overlap < 0.9;
            if (overlap < 0.9) std::printf("  U+%04X: %.0f%% of the ink near the other's\n", code, overlap * 100.0);
            ++glyphs;
        }
        std::printf("%s: %u glyphs, the worst %.0f%% of the ink away from the other's, %u under 90%% near\n",
                    font_path.c_str(), glyphs,
                    worst * 100.0f, poor);
        CHECK(glyphs > 0);
        CHECK(poor == 0);
        ++fonts;
    }
    CHECK(fonts > 0);
}
