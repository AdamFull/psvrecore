#pragma once

// TrueType and OpenType (CFF) fonts for ScePvf: a font's character map, metrics and glyph outlines (TrueType's
// quadratic glyf outlines, simple and composite; CFF's Type 2 charstrings, with subroutines, and CID-keyed fonts'
// per-glyph private dictionaries), and an anti-aliased rasterizer (exact area coverage, nonzero filling).
// Collections (ttcf) give their fonts by index.

#include <span>

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::media {

struct FontPoint {
    f32 x, y;
};
// An outline in font units (y up): contours of line and curve segments, flattened to lines.
struct FontOutline {
    nx::vector<FontPoint> points;
    nx::vector<u32> contour_ends;  // (one past each contour's last point)
};

struct FontBitmap {
    i32 left = 0, top = 0;  // the bitmap's top left, in pixels from the glyph's origin (x right, y up)
    u32 width = 0, height = 0;
    nx::vector<u8> coverage;  // (0-255, rows top down)
};

class Font {
public:
    // The font's data is copied. False when it is not a font read here.
    bool load(std::span<const u8> data, u32 index = 0);

    u32 units_per_em() const { return units_per_em_; }
    i32 ascender() const { return ascender_; }
    i32 descender() const { return descender_; }
    u32 glyph_count() const { return glyph_count_; }
    // The font's bounds (all glyphs) and widest advance, in units.
    i32 x_min() const { return x_min_; }
    i32 y_min() const { return y_min_; }
    i32 x_max() const { return x_max_; }
    i32 y_max() const { return y_max_; }
    u32 advance_max() const { return advance_max_; }
    // (0: no glyph for the character)
    u32 glyph(u32 code) const;
    u32 advance(u32 glyph) const;
    bool outline(u32 glyph, FontOutline* out) const;
    // The glyph at `scale` pixels a unit, its origin moved by (dx, dy) pixels (x right, y down).
    bool rasterize(u32 glyph, f32 scale_x, f32 scale_y, f32 dx, f32 dy, FontBitmap* out) const;
    // The glyph's outline bounds in units (false: empty).
    bool bounds(u32 glyph, f32* x0, f32* y0, f32* x1, f32* y1) const;

private:
    nx::vector<u8> data_;
    u32 base_ = 0;
    u32 units_per_em_ = 1000;
    i32 ascender_ = 0, descender_ = 0;
    i32 x_min_ = 0, y_min_ = 0, x_max_ = 0, y_max_ = 0;
    u32 advance_max_ = 0;
    u32 glyph_count_ = 0, hmetrics_ = 0;
    u32 cmap_ = 0, cmap_format_ = 0, hmtx_ = 0, loca_ = 0, glyf_ = 0, glyf_size_ = 0;
    bool long_loca_ = false;
    // CFF
    bool cff_ = false;
    u32 cff_at_ = 0, charstrings_ = 0, gsubrs_ = 0, subrs_ = 0, fdarray_ = 0, fdselect_ = 0;
    nx::vector<u32> fd_subrs_;

    u32 table(const char* tag, u32* size = nullptr) const;
    bool glyf_outline(u32 glyph, FontOutline* out, u32 depth) const;
    bool cff_outline(u32 glyph, FontOutline* out) const;
    friend struct CffRunner;
};

}  // namespace psvrecore::runtime::media
