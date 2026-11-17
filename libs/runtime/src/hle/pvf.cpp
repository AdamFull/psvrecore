// Vector fonts (ScePvf): TrueType and OpenType fonts the game hands over in its memory (media/font.cpp), sized in
// points at a resolution (pixels = points x dpi / 72; 72 dpi until set), their metrics in 26.6 pixels and their
// glyphs drawn into the game's buffer (8-bit, or 4-bit with the left pixel in the low nibble) at a 26.6 position,
// each pixel the brighter of what is there and the glyph's coverage. The em value is kept but changes nothing
// here. The system's own fonts are not available: the system fonts here are Roboto Regular and Bold
// (third_party/roboto, SIL OFL 1.1, built in), the font list holds those two (Latin sans-serif), every search finds
// one of them, and the default Japanese font is Roboto too (no Japanese glyphs). Fonts open from the game's memory or
// a file. A character the font lacks is drawn as the alternative character when one is set, else NOGLYPH. Vertical
// layout: these fonts have no vertical forms (IsVertElement false); the vertical calls place the same glyph centred
// on the vertical line, a line's height apart. Kerning is 0; emboldening and skew are kept, not applied. The glyph
// outline calls (absent from the SDK's header: their records are unknown) answer NOSUPPORT; the FreeType hooks are
// accepted and unused. The library and font handles are allocated through the game's allocator.

#include "../media/font.hpp"
#include "hle_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace psvrecore::runtime::hle {

// The system fonts' files (generated/system_fonts.cpp, from third_party/roboto).
extern const u8 kRoboto_Regular[];
extern const usize kRoboto_RegularSize;
extern const u8 kRoboto_Bold[];
extern const usize kRoboto_BoldSize;

namespace {

constexpr u32 kScePvf = 0x68D59260;
constexpr i32 kNoMemory = err::code(0x80460001), kLibId = err::code(0x80460002), kArg = err::code(0x80460003),
              kNoFile = err::code(0x80460004), kTooManyOpened = err::code(0x80460009), kNoGlyph = err::code(0x8046000D),
              kNoSupport = err::code(0x8046000E);
constexpr u32 kDirect4L = 0, kDirect8 = 2;

struct Lib {
    u32 user_data = 0, max_fonts = 0, alloc = 0, free = 0;
    f32 em = 1.0f, hres = 72.0f, vres = 72.0f;
    u32 alt_char = 0;
    u32 open_fonts = 0;
};
struct FontState {
    u32 lib = 0;
    media::Font font;
    f32 hsize = 12.0f, vsize = 12.0f;
    f32 embolden = 0.0f, skew_x = 0.0f, skew_y = 0.0f;
    const struct SystemFont* system = nullptr;
};
struct Pvf {
    nx::mutex lock;
    std::map<u32, Lib> libs;
    std::map<u32, std::unique_ptr<FontState>> fonts;
};
Pvf& pvf() {
    static Pvf p;
    return p;
}
using Lock = nx::scoped_lock<nx::mutex>;

struct SystemFont {
    const u8* data;
    const usize* size;
    const char* style_name;
    const char* file_name;
    f32 weight;
    u16 style;  // (SCE_PVF_STYLE_REGULAR, _BOLD)
};
const SystemFont kSystemFonts[] = {
    {kRoboto_Regular, &kRoboto_RegularSize, "Regular", "Roboto-Regular.ttf", 400.0f, 1},
    {kRoboto_Bold, &kRoboto_BoldSize, "Bold", "Roboto-Bold.ttf", 700.0f, 5},
};
constexpr i32 kSystemFontCount = 2;

// ScePvf_t_fontStyleInfo (216 bytes): weight, family, style, sub-style, language, region, country (u16 each), the font,
// style and file names (64 bytes each), extra attributes, expiry date.
void write_style(u32 at, const SystemFont& f) {
    std::memset(memory().host(at), 0, 216);
    u32 weight;
    std::memcpy(&weight, &f.weight, 4);
    psv_st32(at, weight);
    psv_st16(at + 4, 1);  // (sans-serif)
    psv_st16(at + 6, f.style);
    psv_st16(at + 10, 2);  // (Latin)
    std::memcpy(memory().host(at + 16), "Roboto", 7);
    std::memcpy(memory().host(at + 80), f.style_name, std::strlen(f.style_name) + 1);
    std::memcpy(memory().host(at + 144), f.file_name, std::strlen(f.file_name) + 1);
}
// (the bold styles: bold, bold oblique, black, black oblique, and the heavier weights)
bool bold_style(u32 style) { return style == 5 || style == 6 || style == 7 || style == 8 || (style >= 104 && style <= 106); }

void set_error(u32 at, i32 code) {
    if (at && writable(at, 4)) psv_st32(at, static_cast<u32>(code));
}

u32 guest_alloc(PsvContext* ctx, const Lib& l, u32 size) {
    const u32 args[] = {l.user_data, size};
    const u32 p = call_guest(ctx, l.alloc, args);
    if (p && writable(p, size)) std::memset(memory().host(p), 0, size);
    return p;
}
void guest_free(PsvContext* ctx, const Lib& l, u32 p) {
    if (!p || !l.free) return;
    const u32 args[] = {l.user_data, p};
    call_guest(ctx, l.free, args);
}

// ScePvf_t_initRec: user data, the most fonts open, cache, reserved, alloc, realloc, free.
u32 scePvfNewLib(PsvContext* ctx, u32 init, u32 error) {
    if (!init || !memory().valid(init, 28)) return set_error(error, kArg), 0;
    Lib l;
    l.user_data = psv_ld32(init), l.max_fonts = psv_ld32(init + 4), l.alloc = psv_ld32(init + 16), l.free = psv_ld32(init + 24);
    if (!l.alloc || !l.max_fonts) return set_error(error, kArg), 0;
    const u32 handle = guest_alloc(ctx, l, 16);
    if (!handle) return set_error(error, kNoMemory), 0;
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        p.libs[handle] = l;
    }
    set_error(error, 0);
    return handle;
}

i32 scePvfDoneLib(PsvContext* ctx, u32 lib) {
    Lib l;
    nx::vector<u32> handles;
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        auto it = p.libs.find(lib);
        if (it == p.libs.end()) return kLibId;
        l = it->second;
        for (auto f = p.fonts.begin(); f != p.fonts.end();) {
            if (f->second->lib == lib) handles.push_back(f->first), f = p.fonts.erase(f);
            else ++f;
        }
        p.libs.erase(it);
    }
    for (u32 h : handles) guest_free(ctx, l, h);
    guest_free(ctx, l, lib);
    return 0;
}

template <class F>
i32 with_lib(u32 lib, F f) {
    Pvf& p = pvf();
    Lock held(p.lock);
    auto it = p.libs.find(lib);
    if (it == p.libs.end()) return kLibId;
    return f(it->second);
}
template <class F>
i32 with_font(u32 font, F f) {
    Pvf& p = pvf();
    Lock held(p.lock);
    auto it = p.fonts.find(font);
    if (it == p.fonts.end()) return kArg;
    auto lib = p.libs.find(it->second->lib);
    if (lib == p.libs.end()) return kLibId;
    return f(*it->second, lib->second);
}

i32 scePvfSetEM(u32 lib, f32 em) {
    if (!(em > 0.0f)) return kArg;
    return with_lib(lib, [&](Lib& l) { return l.em = em, 0; });
}
i32 scePvfSetResolution(u32 lib, f32 h, f32 v) {
    if (!(h > 0.0f) || !(v > 0.0f)) return kArg;
    return with_lib(lib, [&](Lib& l) {
        l.hres = h, l.vres = v;
        return 0;
    });
}
i32 scePvfSetAltCharacterCode(u32 lib, u32 code) {
    return with_lib(lib, [&](Lib& l) { return l.alt_char = code & 0xFFFF, 0; });
}
i32 scePvfGetNumFontList(u32 lib, u32 error) {
    const i32 r = with_lib(lib, [](Lib&) { return 0; });
    set_error(error, r);
    return r < 0 ? 0 : kSystemFontCount;
}
i32 scePvfGetFontList(u32 lib, u32 styles, i32 count) {
    if (count < 0 || (count && (!styles || !writable(styles, static_cast<u32>(count) * 216)))) return kArg;
    return with_lib(lib, [&](Lib&) {
        for (i32 i = 0; i < count && i < kSystemFontCount; ++i) write_style(styles + 216 * static_cast<u32>(i), kSystemFonts[i]);
        return 0;
    });
}
i32 scePvfGetFontInfoByIndexNumber(u32 lib, u32 style, i32 index) {
    if (!style || !writable(style, 216) || index < 0 || index >= kSystemFontCount) return kArg;
    return with_lib(lib, [&](Lib&) { return write_style(style, kSystemFonts[index]), 0; });
}
// The closest font: bold for the bold styles, else regular.
i32 scePvfFindOptimumFont(u32 lib, u32 style, u32 error) {
    if (!style || !memory().valid(style, 216)) return set_error(error, kArg), -1;
    const i32 r = with_lib(lib, [](Lib&) { return 0; });
    set_error(error, r);
    return r < 0 ? -1 : bold_style(psv_ld16(style + 6)) ? 1 : 0;
}
// A font exactly as asked (the family, style and language given, or 0 for any): one of the two, or -1 and NOFILE.
i32 scePvfFindFont(u32 lib, u32 style, u32 error) {
    if (!style || !memory().valid(style, 216)) return set_error(error, kArg), -1;
    const i32 r = with_lib(lib, [](Lib&) { return 0; });
    if (r < 0) return set_error(error, r), -1;
    const u32 family = psv_ld16(style + 4), wanted = psv_ld16(style + 6), language = psv_ld16(style + 10);
    if ((family != 0 && family != 1) || (language != 0 && language != 2)) return set_error(error, kNoFile), -1;
    for (i32 i = 0; i < kSystemFontCount; ++i)
        if (wanted == 0 || wanted == kSystemFonts[i].style) return set_error(error, 0), i;
    return set_error(error, kNoFile), -1;
}

f32 convert(u32 lib, f32 v, u32 error, bool to_pixels, bool horizontal) {
    f32 out = 0.0f;
    const i32 r = with_lib(lib, [&](Lib& l) {
        const f32 dpi = horizontal ? l.hres : l.vres;
        out = to_pixels ? v * dpi / 72.0f : v * 72.0f / dpi;
        return 0;
    });
    set_error(error, r);
    return out;
}
f32 scePvfPointToPixelH(u32 lib, f32 v, u32 error) { return convert(lib, v, error, true, true); }
f32 scePvfPointToPixelV(u32 lib, f32 v, u32 error) { return convert(lib, v, error, true, false); }
f32 scePvfPixelToPointH(u32 lib, f32 v, u32 error) { return convert(lib, v, error, false, true); }
f32 scePvfPixelToPointV(u32 lib, f32 v, u32 error) { return convert(lib, v, error, false, false); }

u32 open_data(PsvContext* ctx, u32 lib, std::span<const u8> data, u32 index, u32 error) {
    auto state = std::make_unique<FontState>();
    if (!state->font.load(data, index)) return set_error(error, kNoSupport), 0;
    Lib l;
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        auto it = p.libs.find(lib);
        if (it == p.libs.end()) return set_error(error, kLibId), 0;
        if (it->second.open_fonts >= it->second.max_fonts) return set_error(error, kTooManyOpened), 0;
        l = it->second;
    }
    const u32 handle = guest_alloc(ctx, l, 16);
    if (!handle) return set_error(error, kNoMemory), 0;
    state->lib = lib;
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        ++p.libs[lib].open_fonts;
        p.fonts[handle] = std::move(state);
    }
    nx::logi(logs::hle, "ScePvf: a font (0x{:x} bytes) opened", data.size());
    set_error(error, 0);
    return handle;
}
u32 open_memory(PsvContext* ctx, u32 lib, u32 address, u32 size, u32 index, u32 error) {
    if (!address || !size || !memory().valid(address, size)) return set_error(error, kArg), 0;
    return open_data(ctx, lib, std::span<const u8>(memory().host(address), size), index, error);
}
u32 open_system(PsvContext* ctx, u32 lib, i32 index, u32 error) {
    if (index < 0 || index >= kSystemFontCount) return set_error(error, kArg), 0;
    const u32 handle = open_data(ctx, lib, std::span<const u8>(kSystemFonts[index].data, *kSystemFonts[index].size), 0, error);
    if (handle) {
        Pvf& p = pvf();
        Lock held(p.lock);
        if (auto it = p.fonts.find(handle); it != p.fonts.end()) it->second->system = &kSystemFonts[index];
    }
    return handle;
}
u32 scePvfOpen(PsvContext* ctx, u32 lib, i32 index, u32 /*mode*/, u32 error) { return open_system(ctx, lib, index, error); }
u32 scePvfOpenDefaultLatinFontOnSharedMemory(PsvContext* ctx, u32 lib, u32 error) { return open_system(ctx, lib, 0, error); }
u32 scePvfOpenDefaultJapaneseFontOnSharedMemory(PsvContext* ctx, u32 lib, u32 error) {
    nx::logw(logs::hle, "ScePvf: the default Japanese font is Roboto here (no Japanese glyphs)");
    return open_system(ctx, lib, 0, error);
}
// A font file by its path (read whole, through the game's file system).
u32 open_file(PsvContext* ctx, u32 lib, u32 path, u32 index, u32 error) {
    nx::string name;
    for (u32 i = 0; path && i < 1024 && memory().valid(path + i, 1); ++i) {
        const char c = static_cast<char>(memory().host(path + i)[0]);
        if (!c) break;
        name.push_back(c);
    }
    if (name.empty()) return set_error(error, kArg), 0;
    FileSystem& files = runtime().files();
    const i32 fd = files.open(name, 1, 0);
    if (fd < 0) return set_error(error, kNoFile), 0;
    const i64 size = files.seek(fd, 0, 2);
    nx::vector<u8> data(size > 0 ? static_cast<usize>(size) : 0);
    const bool ok = size > 0 && files.pread(fd, data.data(), static_cast<u32>(size), 0) == size;
    files.close(fd);
    if (!ok) return set_error(error, kNoFile), 0;
    return open_data(ctx, lib, data, index, error);
}
u32 scePvfOpenUserFile(PsvContext* ctx, u32 lib, u32 path, u32 /*mode*/, u32 error) { return open_file(ctx, lib, path, 0, error); }
u32 scePvfOpenUserFileWithSubfontIndex(PsvContext* ctx, u32 lib, u32 path, u32 /*mode*/, u32 index, u32 error) {
    return open_file(ctx, lib, path, index, error);
}
u32 scePvfOpenUserMemory(PsvContext* ctx, u32 lib, u32 address, u32 size, u32 error) {
    return open_memory(ctx, lib, address, size, 0, error);
}
u32 scePvfOpenUserMemoryWithSubfontIndex(PsvContext* ctx, u32 lib, u32 address, u32 size, u32 index, u32 error) {
    return open_memory(ctx, lib, address, size, index, error);
}

i32 scePvfClose(PsvContext* ctx, u32 font) {
    Lib l;
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        auto it = p.fonts.find(font);
        if (it == p.fonts.end()) return kArg;
        auto lib = p.libs.find(it->second->lib);
        if (lib != p.libs.end()) {
            l = lib->second;
            --lib->second.open_fonts;
        }
        p.fonts.erase(it);
    }
    guest_free(ctx, l, font);
    return 0;
}
i32 scePvfFlush(u32 font) {
    return with_font(font, [](FontState&, Lib&) { return 0; });
}
i32 scePvfSetCharSize(u32 font, f32 h, f32 v) {
    if (!(h > 0.0f) || !(v > 0.0f)) return kArg;
    return with_font(font, [&](FontState& f, Lib&) {
        f.hsize = h, f.vsize = v;
        return 0;
    });
}
i32 scePvfSetEmboldenRate(u32 font, f32 rate) {
    if (rate < -20.0f || rate > 40.0f) return kArg;
    return with_font(font, [&](FontState& f, Lib&) { return f.embolden = rate, 0; });
}
i32 scePvfSetSkewValue(u32 font, f32 x, f32 y) {
    if (x < -30.0f || x > 30.0f || y < -30.0f || y > 30.0f) return kArg;
    return with_font(font, [&](FontState& f, Lib&) {
        f.skew_x = x, f.skew_y = y;
        return 0;
    });
}

// Pixels a font unit, across and down.
void scales(const FontState& f, const Lib& l, f32* sx, f32* sy) {
    const f32 em = static_cast<f32>(f.font.units_per_em());
    *sx = f.hsize * l.hres / 72.0f / em;
    *sy = f.vsize * l.vres / 72.0f / em;
}
u32 glyph_for(const FontState& f, const Lib& l, u32 code) {
    u32 g = f.font.glyph(code & 0xFFFF);
    if (!g && l.alt_char) g = f.font.glyph(l.alt_char);
    return g;
}
i32 to64(f32 v) { return static_cast<i32>(std::lround(v * 64.0f)); }

// ScePvf_t_iGlyphMetricsInfo for a glyph (10 words).
void glyph_metrics(const FontState& f, const Lib& l, u32 g, u32 at) {
    f32 sx, sy;
    scales(f, l, &sx, &sy);
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    f.font.bounds(g, &x0, &y0, &x1, &y1);
    const i32 width = to64((x1 - x0) * sx), height = to64((y1 - y0) * sy);
    const i32 bearing_x = to64(x0 * sx), bearing_y = to64(y1 * sy), advance = to64(static_cast<f32>(f.font.advance(g)) * sx);
    const i32 line = to64(static_cast<f32>(f.font.ascender() - f.font.descender()) * sy);
    const i32 m[10] = {width, height, bearing_y, bearing_y - height, bearing_x, bearing_y, -width / 2, 0, advance, line};
    for (u32 i = 0; i < 10; ++i) psv_st32(at + 4 * i, static_cast<u32>(m[i]));
}

// ScePvf_t_charInfo: the bitmap's size, pitch, left and top (pixels from the origin), the metrics, reserved.
i32 scePvfGetCharInfo(u32 font, u32 code, u32 info) {
    if (!info || !writable(info, 64)) return kArg;
    return with_font(font, [&](FontState& f, Lib& l) {
        const u32 g = glyph_for(f, l, code);
        if (!g) return kNoGlyph;
        f32 sx, sy;
        scales(f, l, &sx, &sy);
        media::FontBitmap b;
        f.font.rasterize(g, sx, sy, 0, 0, &b);
        std::memset(memory().host(info), 0, 64);
        psv_st32(info, b.width);
        psv_st32(info + 4, b.height);
        psv_st32(info + 8, b.width);
        psv_st32(info + 12, static_cast<u32>(b.left));
        psv_st32(info + 16, static_cast<u32>(b.top));
        glyph_metrics(f, l, g, info + 20);
        return 0;
    });
}
i32 scePvfGetCharImageRect(u32 font, u32 code, u32 rect) {
    if (!rect || !writable(rect, 4)) return kArg;
    return with_font(font, [&](FontState& f, Lib& l) {
        const u32 g = glyph_for(f, l, code);
        if (!g) return kNoGlyph;
        f32 sx, sy;
        scales(f, l, &sx, &sy);
        media::FontBitmap b;
        f.font.rasterize(g, sx, sy, 0, 0, &b);
        psv_st16(rect, b.width);
        psv_st16(rect + 2, b.height);
        return 0;
    });
}

// ScePvf_t_userImageBufferRec: pixel format, x and y of the origin (26.6), width and height (u16), bytes a line
// (u16), reserved, the buffer.
// (vertical: the origin is on the vertical line at the line's top; the glyph is placed by its vertical bearings)
i32 draw(u32 font, u32 code, u32 image, i32 clip_x, i32 clip_y, u32 clip_w, u32 clip_h, bool vertical) {
    if (!image || !memory().valid(image, 24)) return kArg;
    const u32 format = psv_ld32(image);
    const i32 x64 = static_cast<i32>(psv_ld32(image + 4)), y64 = static_cast<i32>(psv_ld32(image + 8));
    const u32 width = psv_ld16(image + 12), height = psv_ld16(image + 14), pitch = psv_ld16(image + 16), buffer = psv_ld32(image + 20);
    if (format != kDirect4L && format != kDirect8) return kArg;
    if (!buffer || !writable(buffer, pitch * height)) return kArg;
    return with_font(font, [&](FontState& f, Lib& l) {
        const u32 g = glyph_for(f, l, code);
        if (!g) return kNoGlyph;
        f32 sx, sy;
        scales(f, l, &sx, &sy);
        media::FontBitmap b;
        i32 vx = x64, vy = y64;
        if (vertical) {
            // (to the horizontal origin: half the glyph's width left of the line, the ascender below its top)
            f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            f.font.bounds(g, &x0, &y0, &x1, &y1);
            vx -= to64((x0 + (x1 - x0) / 2) * sx);
            vy += to64(static_cast<f32>(f.font.ascender()) * sy);
        }
        const i32 ox = vx >> 6, oy = vy >> 6;
        f.font.rasterize(g, sx, sy, static_cast<f32>(vx & 63) / 64.0f, static_cast<f32>(vy & 63) / 64.0f, &b);
        const i64 cx0 = std::max<i64>(0, clip_x), cy0 = std::max<i64>(0, clip_y);
        const i64 cx1 = std::min<i64>(width, i64{clip_x} + clip_w), cy1 = std::min<i64>(height, i64{clip_y} + clip_h);
        u8* out = memory().host(buffer);
        for (u32 j = 0; j < b.height; ++j)
            for (u32 i = 0; i < b.width; ++i) {
                const u8 c = b.coverage[static_cast<usize>(j) * b.width + i];
                if (!c) continue;
                const i64 x = i64{ox} + b.left + i, y = i64{oy} - b.top + j;
                if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1) continue;
                if (format == kDirect8) {
                    u8& px = out[static_cast<usize>(y) * pitch + static_cast<usize>(x)];
                    px = std::max(px, c);
                } else {
                    if (static_cast<u32>(x / 2) >= pitch) continue;
                    u8& px = out[static_cast<usize>(y) * pitch + static_cast<usize>(x / 2)];
                    const u32 shift = (x & 1) ? 4 : 0, old = (px >> shift) & 0xF, now = std::max<u32>(old, c >> 4);
                    px = static_cast<u8>((px & ~(0xF << shift)) | (now << shift));
                }
            }
        return 0;
    });
}
i32 scePvfGetCharGlyphImage(u32 font, u32 code, u32 image) { return draw(font, code, image, 0, 0, 0xFFFF, 0xFFFF, false); }
i32 scePvfGetCharGlyphImage_Clip(u32 font, u32 code, u32 image, i32 x, i32 y, u32 w, u32 h) {
    return draw(font, code, image, x, y, w, h, false);
}
i32 scePvfGetVertCharGlyphImage(u32 font, u32 code, u32 image) { return draw(font, code, image, 0, 0, 0xFFFF, 0xFFFF, true); }
i32 scePvfGetVertCharGlyphImage_Clip(u32 font, u32 code, u32 image, i32 x, i32 y, u32 w, u32 h) {
    return draw(font, code, image, x, y, w, h, true);
}

// Vertical character info: the same glyph and metrics (the vertical bearings and advance are set in both).
i32 scePvfGetVertCharInfo(u32 font, u32 code, u32 info) { return scePvfGetCharInfo(font, code, info); }
i32 scePvfGetVertCharImageRect(u32 font, u32 code, u32 rect) { return scePvfGetCharImageRect(font, code, rect); }
u32 scePvfIsVertElement(u32 font, u32) {
    return 0;  // (no vertical forms)
}
i32 outline_unsupported() { return kNoSupport; }
i32 hook_accepted() { return 0; }

u32 scePvfIsElement(u32 font, u32 code) {
    u32 present = 0;
    with_font(font, [&](FontState& f, Lib&) { return present = f.font.glyph(code & 0xFFFF) ? 1 : 0, 0; });
    return present;
}

// ScePvf_t_fontInfo: the largest metrics (26.6, then as floats), the number of glyphs, the style (weight, codes,
// names), padding.
i32 scePvfGetFontInfo(u32 font, u32 info) {
    if (!info || !writable(info, 304)) return kArg;
    return with_font(font, [&](FontState& f, Lib& l) {
        f32 sx, sy;
        scales(f, l, &sx, &sy);
        const media::Font& t = f.font;
        const f32 w = static_cast<f32>(t.x_max() - t.x_min()) * sx, h = static_cast<f32>(t.y_max() - t.y_min()) * sy;
        const f32 asc = static_cast<f32>(t.ascender()) * sy, desc = static_cast<f32>(t.descender()) * sy;
        const f32 m[10] = {w, h, asc, desc, static_cast<f32>(t.x_min()) * sx, static_cast<f32>(t.y_max()) * sy, -w / 2, 0,
                           static_cast<f32>(t.advance_max()) * sx, asc - desc};
        std::memset(memory().host(info), 0, 304);
        for (u32 i = 0; i < 10; ++i) {
            psv_st32(info + 4 * i, static_cast<u32>(to64(m[i])));
            u32 bits;
            std::memcpy(&bits, &m[i], 4);
            psv_st32(info + 40 + 4 * i, bits);
        }
        psv_st32(info + 80, t.glyph_count());
        const f32 weight = 400.0f;
        u32 bits;
        std::memcpy(&bits, &weight, 4);
        psv_st32(info + 84, bits);
        // (a system font: its style information)
        for (const SystemFont& sf : kSystemFonts)
            if (t.glyph_count() && f.system == &sf) write_style(info + 84, sf);
        return 0;
    });
}
// ScePvf_t_kerningInfo: x and y offsets (26.6), then as floats: none.
i32 scePvfGetKerningInfo(u32 font, u32 /*left*/, u32 /*right*/, u32 info) {
    if (!info || !writable(info, 16)) return kArg;
    return with_font(font, [&](FontState&, Lib&) {
        std::memset(memory().host(info), 0, 16);
        return 0;
    });
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x72E58672, scePvfNewLib),
    HLE_FUNCTION(0xE17717EC, scePvfDoneLib),
    HLE_FUNCTION(0xDFB677C5, scePvfSetEM),
    HLE_FUNCTION(0xC4444FB3, scePvfSetResolution),
    HLE_FUNCTION(0x830625C2, scePvfSetAltCharacterCode),
    HLE_FUNCTION(0xF6C4A855, scePvfGetNumFontList),
    HLE_FUNCTION(0xE35434BB, scePvfOpen),
    HLE_FUNCTION(0x66F2D767, scePvfGetFontList),
    HLE_FUNCTION(0xF3E1E8BD, scePvfGetFontInfoByIndexNumber),
    HLE_FUNCTION(0x2761FEAC, scePvfFindOptimumFont),
    HLE_FUNCTION(0x984E5BFE, scePvfFindFont),
    HLE_FUNCTION(0x9CBC1A46, scePvfOpenDefaultLatinFontOnSharedMemory),
    HLE_FUNCTION(0xFEEE373A, scePvfOpenDefaultJapaneseFontOnSharedMemory),
    HLE_FUNCTION(0xD535520F, scePvfOpenUserFile),
    HLE_FUNCTION(0x10452B86, scePvfOpenUserFileWithSubfontIndex),
    HLE_FUNCTION(0xB8D01915, scePvfGetVertCharInfo),
    HLE_FUNCTION(0x64E0EA8B, scePvfGetVertCharImageRect),
    HLE_FUNCTION(0x8AE8433A, scePvfGetVertCharGlyphImage),
    HLE_FUNCTION(0xEEDAB884, scePvfGetVertCharGlyphImage_Clip),
    HLE_FUNCTION(0xE676A888, scePvfIsVertElement),
    Function{0x17A7873B, "scePvfGetCharGlyphOutline", &entry<&outline_unsupported>},
    Function{0xECCB0CEE, "scePvfGetVertCharGlyphOutline", &entry<&outline_unsupported>},
    Function{0xB6CE89E1, "scePvfReleaseCharGlyphOutline", &entry<&outline_unsupported>},
    Function{0x997161C3, "__scePvfSetFt2LibCHook", &entry<&hook_accepted>},
    Function{0x58142D71, "__scePvfSetFt2DoneLibCHook", &entry<&hook_accepted>},
    HLE_FUNCTION(0x91F02F9A, scePvfPointToPixelH),
    HLE_FUNCTION(0x35465BD7, scePvfPointToPixelV),
    HLE_FUNCTION(0xF56B5B9B, scePvfPixelToPointH),
    HLE_FUNCTION(0xCDA282D2, scePvfPixelToPointV),
    HLE_FUNCTION(0x9E65E4ED, scePvfOpenUserMemory),
    HLE_FUNCTION(0xA81570EB, scePvfOpenUserMemoryWithSubfontIndex),
    HLE_FUNCTION(0xD282C23C, scePvfClose),
    HLE_FUNCTION(0x687FF765, scePvfFlush),
    HLE_FUNCTION(0xF17ADE4D, scePvfSetCharSize),
    HLE_FUNCTION(0x6E787722, scePvfSetEmboldenRate),
    HLE_FUNCTION(0x3DD09BC9, scePvfSetSkewValue),
    HLE_FUNCTION(0xA88EEDB0, scePvfGetCharInfo),
    HLE_FUNCTION(0x6C1B9CAF, scePvfGetCharImageRect),
    HLE_FUNCTION(0x37DA496A, scePvfGetCharGlyphImage),
    HLE_FUNCTION(0xA55F973F, scePvfGetCharGlyphImage_Clip),
    HLE_FUNCTION(0x9F018F25, scePvfIsElement),
    HLE_FUNCTION(0xAB0C7CF2, scePvfGetFontInfo),
    HLE_FUNCTION(0xBC90F661, scePvfGetKerningInfo),
};

}  // namespace

void register_pvf() {
    {
        Pvf& p = pvf();
        Lock held(p.lock);
        p.libs.clear(), p.fonts.clear();
    }
    register_library({"ScePvf", kScePvf, kFunctions});
}

}  // namespace psvrecore::runtime::hle
