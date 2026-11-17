#include "font.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psvrecore::runtime::media {

namespace {

struct Reader {
    std::span<const u8> d;
    bool ok(usize at, usize n) const { return at <= d.size() && n <= d.size() - at; }
    u32 u8_(usize at) const { return ok(at, 1) ? d[at] : 0; }
    u32 u16_(usize at) const { return ok(at, 2) ? (u32{d[at]} << 8) | d[at + 1] : 0; }
    i32 i16_(usize at) const { return static_cast<i16>(u16_(at)); }
    u32 u32_(usize at) const { return ok(at, 4) ? (u32{d[at]} << 24) | (u32{d[at + 1]} << 16) | (u32{d[at + 2]} << 8) | d[at + 3] : 0; }
    // (a CFF offset of `size` bytes)
    u32 off(usize at, u32 size) const {
        u32 v = 0;
        for (u32 i = 0; i < size; ++i) v = (v << 8) | u8_(at + i);
        return v;
    }
};

// Flattening curves: segments per curve, from its rough length in units at the size drawn (enough for 1/4 pixel).
constexpr u32 kCurveSteps = 8;

void add_quad(FontOutline* o, FontPoint p0, FontPoint c, FontPoint p1) {
    for (u32 i = 1; i <= kCurveSteps; ++i) {
        const f32 t = static_cast<f32>(i) / kCurveSteps, u = 1 - t;
        o->points.push_back({u * u * p0.x + 2 * u * t * c.x + t * t * p1.x, u * u * p0.y + 2 * u * t * c.y + t * t * p1.y});
    }
}
void add_cubic(FontOutline* o, FontPoint p0, FontPoint c0, FontPoint c1, FontPoint p1) {
    for (u32 i = 1; i <= kCurveSteps; ++i) {
        const f32 t = static_cast<f32>(i) / kCurveSteps, u = 1 - t;
        const f32 a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
        o->points.push_back({a * p0.x + b * c0.x + c * c1.x + e * p1.x, a * p0.y + b * c0.y + c * c1.y + e * p1.y});
    }
}

// ---- CFF INDEX: count, offset size, offsets, data

struct Index {
    u32 at = 0, count = 0, off_size = 0, data = 0, end = 0;
};
Index read_index(const Reader& r, u32 at) {
    Index ix;
    ix.at = at;
    ix.count = r.u16_(at);
    if (!ix.count) {
        ix.end = at + 2;
        return ix;
    }
    ix.off_size = r.u8_(at + 2);
    ix.data = at + 3 + (ix.count + 1) * ix.off_size - 1;
    ix.end = ix.data + r.off(at + 3 + ix.count * ix.off_size, ix.off_size);
    return ix;
}
bool index_item(const Reader& r, const Index& ix, u32 i, u32* start, u32* end) {
    if (i >= ix.count) return false;
    *start = ix.data + r.off(ix.at + 3 + i * ix.off_size, ix.off_size);
    *end = ix.data + r.off(ix.at + 3 + (i + 1) * ix.off_size, ix.off_size);
    return *start <= *end && r.ok(*start, *end - *start);
}

// A DICT's operands for one operator (12 x as 1200 + x); false when absent.
bool dict_find(const Reader& r, u32 start, u32 end, u32 op, f32* operands, u32* count) {
    f32 stack[48];
    u32 n = 0;
    for (u32 p = start; p < end;) {
        const u32 b = r.u8_(p);
        if (b <= 21) {
            u32 o = b;
            ++p;
            if (b == 12) o = 1200 + r.u8_(p++);
            if (o == op) {
                std::copy(stack, stack + n, operands);
                *count = n;
                return true;
            }
            n = 0;
            continue;
        }
        f32 v = 0;
        if (b == 28) v = static_cast<f32>(static_cast<i16>(r.u16_(p + 1))), p += 3;
        else if (b == 29) v = static_cast<f32>(static_cast<i32>(r.u32_(p + 1))), p += 5;
        else if (b == 30) {  // (a real: nibbles, skipped to its end; its value is not needed here)
            ++p;
            while (p < end) {
                const u32 x = r.u8_(p++);
                if ((x & 0xF) == 0xF || (x >> 4) == 0xF) break;
            }
        } else if (b >= 32 && b <= 246) v = static_cast<f32>(static_cast<i32>(b) - 139), ++p;
        else if (b >= 247 && b <= 250) v = static_cast<f32>((static_cast<i32>(b) - 247) * 256 + static_cast<i32>(r.u8_(p + 1)) + 108), p += 2;
        else if (b >= 251 && b <= 254) v = static_cast<f32>(-(static_cast<i32>(b) - 251) * 256 - static_cast<i32>(r.u8_(p + 1)) - 108), p += 2;
        else ++p;
        if (n < 48) stack[n++] = v;
    }
    return false;
}

u32 subr_bias(u32 count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

}  // namespace

// ---- loading

u32 Font::table(const char* tag, u32* size) const {
    const Reader r{data_};
    const u32 n = r.u16_(base_ + 4);
    for (u32 i = 0; i < n; ++i) {
        const u32 rec = base_ + 12 + 16 * i;
        if (r.ok(rec, 16) && std::memcmp(&data_[rec], tag, 4) == 0) {
            const u32 at = r.u32_(rec + 8), len = r.u32_(rec + 12);
            if (!r.ok(at, len)) return 0;
            if (size) *size = len;
            return at;
        }
    }
    return 0;
}

bool Font::load(std::span<const u8> data, u32 index) {
    data_.assign(data.begin(), data.end());
    const Reader r{data_};
    base_ = 0;
    if (r.ok(0, 12) && std::memcmp(data_.data(), "ttcf", 4) == 0) {
        if (index >= r.u32_(8)) return false;
        base_ = r.u32_(12 + 4 * index);
    }
    const u32 version = r.u32_(base_);
    if (version != 0x00010000 && version != 0x4F54544F && version != 0x74727565) return false;
    const u32 head = table("head"), hhea = table("hhea"), maxp = table("maxp");
    hmtx_ = table("hmtx"), cmap_ = 0;
    if (!head || !hhea || !maxp || !hmtx_) return false;
    units_per_em_ = std::max<u32>(16, r.u16_(head + 18));
    long_loca_ = r.i16_(head + 50) != 0;
    ascender_ = r.i16_(hhea + 4), descender_ = r.i16_(hhea + 6);
    advance_max_ = r.u16_(hhea + 10);
    x_min_ = r.i16_(head + 36), y_min_ = r.i16_(head + 38), x_max_ = r.i16_(head + 40), y_max_ = r.i16_(head + 42);
    hmetrics_ = r.u16_(hhea + 34);
    glyph_count_ = r.u16_(maxp + 4);
    // the character map: Unicode, full repertoire (format 12) before the basic plane (format 4)
    if (const u32 cmap = table("cmap")) {
        const u32 n = r.u16_(cmap + 2);
        u32 best = 0, best_rank = 0;
        for (u32 i = 0; i < n; ++i) {
            const u32 platform = r.u16_(cmap + 4 + 8 * i), encoding = r.u16_(cmap + 6 + 8 * i);
            const u32 sub = cmap + r.u32_(cmap + 8 + 8 * i), format = r.u16_(sub);
            u32 rank = 0;
            if (format == 12 && (platform == 0 || (platform == 3 && encoding == 10))) rank = 3;
            else if (format == 4 && (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 0)))) rank = 2;
            else if (format == 4) rank = 1;
            if (rank > best_rank) best = sub, best_rank = rank;
        }
        cmap_ = best, cmap_format_ = best ? r.u16_(best) : 0;
    }
    if (const u32 glyf = table("glyf", &glyf_size_)) {
        glyf_ = glyf, loca_ = table("loca");
        cff_ = false;
        return loca_ != 0;
    }
    u32 cff_size = 0;
    cff_at_ = table("CFF ", &cff_size);
    if (!cff_at_) return false;
    cff_ = true;
    // header, Name INDEX, Top DICT INDEX, String INDEX, Global Subr INDEX
    const Index names = read_index(r, cff_at_ + r.u8_(cff_at_ + 2));
    const Index top = read_index(r, names.end);
    const Index strings = read_index(r, top.end);
    gsubrs_ = read_index(r, strings.end).at;
    u32 ts, te;
    if (!index_item(r, top, 0, &ts, &te)) return false;
    f32 ops[48];
    u32 n = 0;
    if (!dict_find(r, ts, te, 17, ops, &n) || n < 1) return false;
    charstrings_ = cff_at_ + static_cast<u32>(ops[0]);
    const auto private_subrs = [&](u32 ds, u32 de) -> u32 {
        f32 p[48];
        u32 pn = 0;
        if (!dict_find(r, ds, de, 18, p, &pn) || pn < 2) return 0;
        const u32 at = cff_at_ + static_cast<u32>(p[1]), size = static_cast<u32>(p[0]);
        f32 s[48];
        u32 sn = 0;
        if (!dict_find(r, at, at + size, 19, s, &sn) || sn < 1) return 0;
        return at + static_cast<u32>(s[0]);
    };
    subrs_ = private_subrs(ts, te);
    fdarray_ = fdselect_ = 0;
    fd_subrs_.clear();
    if (dict_find(r, ts, te, 1236, ops, &n) && n >= 1) {  // (FDArray: a CID-keyed font)
        fdarray_ = cff_at_ + static_cast<u32>(ops[0]);
        if (dict_find(r, ts, te, 1237, ops, &n) && n >= 1) fdselect_ = cff_at_ + static_cast<u32>(ops[0]);
        const Index fds = read_index(r, fdarray_);
        for (u32 i = 0; i < fds.count; ++i) {
            u32 s, e;
            fd_subrs_.push_back(index_item(r, fds, i, &s, &e) ? private_subrs(s, e) : 0);
        }
    }
    return true;
}

u32 Font::glyph(u32 code) const {
    const Reader r{data_};
    if (!cmap_) return 0;
    if (cmap_format_ == 4) {
        if (code > 0xFFFF) return 0;
        const u32 segs = r.u16_(cmap_ + 6) / 2;
        const u32 ends = cmap_ + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
        for (u32 i = 0; i < segs; ++i) {
            if (code > r.u16_(ends + 2 * i)) continue;
            const u32 start = r.u16_(starts + 2 * i);
            if (code < start) return 0;
            const u32 delta = r.u16_(deltas + 2 * i), range = r.u16_(ranges + 2 * i);
            if (!range) return (code + delta) & 0xFFFF;
            const u32 g = r.u16_(ranges + 2 * i + range + 2 * (code - start));
            return g ? (g + delta) & 0xFFFF : 0;
        }
        return 0;
    }
    if (cmap_format_ == 12) {
        const u32 groups = r.u32_(cmap_ + 12);
        for (u32 i = 0; i < groups; ++i) {
            const u32 g = cmap_ + 16 + 12 * i, first = r.u32_(g), last = r.u32_(g + 4);
            if (code >= first && code <= last) return r.u32_(g + 8) + (code - first);
        }
    }
    return 0;
}

u32 Font::advance(u32 glyph) const {
    const Reader r{data_};
    if (!hmetrics_) return 0;
    return r.u16_(hmtx_ + 4 * std::min(glyph, hmetrics_ - 1));
}

// ---- TrueType outlines

bool Font::glyf_outline(u32 glyph, FontOutline* out, u32 depth) const {
    const Reader r{data_};
    if (glyph >= glyph_count_ || depth > 8) return false;
    const u32 start = long_loca_ ? r.u32_(loca_ + 4 * glyph) : r.u16_(loca_ + 2 * glyph) * 2;
    const u32 end = long_loca_ ? r.u32_(loca_ + 4 * glyph + 4) : r.u16_(loca_ + 2 * glyph + 2) * 2;
    if (end <= start) return true;  // (an empty glyph: a space)
    const u32 g = glyf_ + start;
    const i32 contours = r.i16_(g);
    if (contours < 0) {
        // composite: components, each a glyph placed by an offset (or matched points: not supported) and a transform
        u32 p = g + 10;
        for (u32 guard = 0; guard < 64; ++guard) {
            const u32 flags = r.u16_(p), component = r.u16_(p + 2);
            p += 4;
            f32 dx = 0, dy = 0;
            if (flags & 1) dx = static_cast<f32>(r.i16_(p)), dy = static_cast<f32>(r.i16_(p + 2)), p += 4;
            else dx = static_cast<f32>(static_cast<i8>(r.u8_(p))), dy = static_cast<f32>(static_cast<i8>(r.u8_(p + 1))), p += 2;
            if (!(flags & 2)) dx = dy = 0;
            f32 a = 1, b = 0, c = 0, d = 1;
            const auto f2dot14 = [&](u32 at) { return static_cast<f32>(r.i16_(at)) / 16384.0f; };
            if (flags & 8) a = d = f2dot14(p), p += 2;
            else if (flags & 0x40) a = f2dot14(p), d = f2dot14(p + 2), p += 4;
            else if (flags & 0x80) a = f2dot14(p), b = f2dot14(p + 2), c = f2dot14(p + 4), d = f2dot14(p + 6), p += 8;
            FontOutline part;
            if (!glyf_outline(component, &part, depth + 1)) return false;
            const u32 first = static_cast<u32>(out->points.size());
            for (const FontPoint& q : part.points) out->points.push_back({a * q.x + c * q.y + dx, b * q.x + d * q.y + dy});
            for (u32 e : part.contour_ends) out->contour_ends.push_back(first + e);
            if (!(flags & 0x20)) break;
        }
        return true;
    }
    const u32 ends_at = g + 10;
    const u32 n_points = contours ? r.u16_(ends_at + 2 * (contours - 1)) + 1 : 0;
    u32 p = ends_at + 2 * contours;
    p += 2 + r.u16_(p);  // (instructions)
    nx::vector<u8> flags(n_points);
    for (u32 i = 0; i < n_points;) {
        const u8 f = static_cast<u8>(r.u8_(p++));
        flags[i++] = f;
        if (f & 8) {
            u32 repeat = r.u8_(p++);
            while (repeat-- && i < n_points) flags[i++] = f;
        }
    }
    nx::vector<f32> xs(n_points), ys(n_points);
    i32 v = 0;
    for (u32 i = 0; i < n_points; ++i) {
        const u8 f = flags[i];
        if (f & 2) v += (f & 0x10) ? static_cast<i32>(r.u8_(p)) : -static_cast<i32>(r.u8_(p)), p += 1;
        else if (!(f & 0x10)) v += r.i16_(p), p += 2;
        xs[i] = static_cast<f32>(v);
    }
    v = 0;
    for (u32 i = 0; i < n_points; ++i) {
        const u8 f = flags[i];
        if (f & 4) v += (f & 0x20) ? static_cast<i32>(r.u8_(p)) : -static_cast<i32>(r.u8_(p)), p += 1;
        else if (!(f & 0x20)) v += r.i16_(p), p += 2;
        ys[i] = static_cast<f32>(v);
    }
    u32 first = 0;
    for (i32 c = 0; c < contours; ++c) {
        const u32 last = r.u16_(ends_at + 2 * c);
        if (last < first || last >= n_points) return false;
        const u32 count = last - first + 1;
        const auto pt = [&](u32 i) { return FontPoint{xs[first + i % count], ys[first + i % count]}; };
        const auto on = [&](u32 i) { return (flags[first + i % count] & 1) != 0; };
        // (start at an on-curve point, or the midpoint of the first two off-curve ones)
        u32 s = 0;
        while (s < count && !on(s)) ++s;
        FontPoint start_pt;
        if (s == count) {
            start_pt = {(pt(0).x + pt(1).x) / 2, (pt(0).y + pt(1).y) / 2}, s = 0;
        } else {
            start_pt = pt(s);
        }
        out->points.push_back(start_pt);
        FontPoint cur = start_pt;
        for (u32 k = 1; k <= count; ++k) {
            const u32 i = s + k;
            const FontPoint q = pt(i);
            if (on(i)) {
                out->points.push_back(q);
                cur = q;
                continue;
            }
            // an off-curve control point: to the next on-curve point, or the midpoint of two controls
            const FontPoint next = pt(i + 1);
            const FontPoint to = on(i + 1) ? next : FontPoint{(q.x + next.x) / 2, (q.y + next.y) / 2};
            add_quad(out, cur, q, to);
            cur = to;
            if (on(i + 1)) ++k;
        }
        out->contour_ends.push_back(static_cast<u32>(out->points.size()));
        first = last + 1;
    }
    return true;
}

// ---- CFF outlines (Type 2 charstrings)

struct CffRunner {
    const Font& font;
    Reader r;
    FontOutline* out;
    u32 local = 0;
    f32 stack[48];
    u32 sp = 0;
    u32 stems = 0;
    bool width_done = false, open = false;
    f32 x = 0, y = 0;
    u32 depth = 0;
    bool ended = false;

    void close() {
        if (open) out->contour_ends.push_back(static_cast<u32>(out->points.size()));
        open = false;
    }
    void move(f32 dx, f32 dy) {
        close();
        x += dx, y += dy;
        out->points.push_back({x, y});
        open = true;
    }
    void line(f32 dx, f32 dy) {
        if (!open) move(0, 0);
        x += dx, y += dy;
        out->points.push_back({x, y});
    }
    void curve(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f) {
        if (!open) move(0, 0);
        const FontPoint p0{x, y}, c0{x + a, y + b}, c1{x + a + c, y + b + d}, p1{x + a + c + e, y + b + d + f};
        add_cubic(out, p0, c0, c1, p1);
        x = p1.x, y = p1.y;
    }
    // (the first stack-clearing operator may carry the advance width first)
    void width(u32 expected_even) {
        if (!width_done && sp % 2 != expected_even && sp > 0) {
            std::memmove(stack, stack + 1, (sp - 1) * sizeof(f32));
            --sp;
        }
        width_done = true;
    }
    bool run(u32 start, u32 end) {
        if (++depth > 10) return false;
        for (u32 p = start; p < end && !ended;) {
            const u32 b = r.u8_(p++);
            if (b >= 32 || b == 28) {
                f32 v;
                if (b == 28) v = static_cast<f32>(static_cast<i16>(r.u16_(p))), p += 2;
                else if (b <= 246) v = static_cast<f32>(static_cast<i32>(b) - 139);
                else if (b <= 250) v = static_cast<f32>((static_cast<i32>(b) - 247) * 256 + static_cast<i32>(r.u8_(p++)) + 108);
                else if (b <= 254) v = static_cast<f32>(-(static_cast<i32>(b) - 251) * 256 - static_cast<i32>(r.u8_(p++)) - 108);
                else v = static_cast<f32>(static_cast<i32>(r.u32_(p))) / 65536.0f, p += 4;
                if (sp < 48) stack[sp++] = v;
                continue;
            }
            switch (b) {
            case 1: case 3: case 18: case 23:  // (stem hints)
                width(0);
                stems += sp / 2, sp = 0;
                break;
            case 19: case 20:  // (hint and counter masks: implied vertical stems first)
                width(0);
                stems += sp / 2, sp = 0;
                p += (stems + 7) / 8;
                break;
            case 21:
                width(0);
                if (sp >= 2) move(stack[sp - 2], stack[sp - 1]);
                sp = 0;
                break;
            case 22:
                width(1);
                if (sp >= 1) move(stack[sp - 1], 0);
                sp = 0;
                break;
            case 4:
                width(1);
                if (sp >= 1) move(0, stack[sp - 1]);
                sp = 0;
                break;
            case 5:
                for (u32 i = 0; i + 1 < sp; i += 2) line(stack[i], stack[i + 1]);
                sp = 0;
                break;
            case 6: case 7: {
                bool horizontal = b == 6;
                for (u32 i = 0; i < sp; ++i, horizontal = !horizontal) horizontal ? line(stack[i], 0) : line(0, stack[i]);
                sp = 0;
                break;
            }
            case 8:
                for (u32 i = 0; i + 5 < sp; i += 6) curve(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                sp = 0;
                break;
            case 24: {  // rcurveline
                u32 i = 0;
                for (; i + 7 < sp; i += 6) curve(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                if (i + 1 < sp) line(stack[i], stack[i + 1]);
                sp = 0;
                break;
            }
            case 25: {  // rlinecurve
                u32 i = 0;
                for (; i + 7 < sp; i += 2) line(stack[i], stack[i + 1]);
                if (i + 5 < sp) curve(stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4], stack[i + 5]);
                sp = 0;
                break;
            }
            case 26: {  // vvcurveto
                u32 i = 0;
                f32 dx1 = 0;
                if (sp % 4 == 1) dx1 = stack[0], i = 1;
                for (; i + 3 < sp; i += 4, dx1 = 0) curve(dx1, stack[i], stack[i + 1], stack[i + 2], 0, stack[i + 3]);
                sp = 0;
                break;
            }
            case 27: {  // hhcurveto
                u32 i = 0;
                f32 dy1 = 0;
                if (sp % 4 == 1) dy1 = stack[0], i = 1;
                for (; i + 3 < sp; i += 4, dy1 = 0) curve(stack[i], dy1, stack[i + 1], stack[i + 2], stack[i + 3], 0);
                sp = 0;
                break;
            }
            case 30: case 31: {  // vhcurveto, hvcurveto
                bool horizontal = b == 31;
                for (u32 i = 0; i + 3 < sp; i += 4, horizontal = !horizontal) {
                    const f32 last = (sp - i == 5) ? stack[i + 4] : 0;
                    if (horizontal) curve(stack[i], 0, stack[i + 1], stack[i + 2], last, stack[i + 3]);
                    else curve(0, stack[i], stack[i + 1], stack[i + 2], stack[i + 3], last);
                }
                sp = 0;
                break;
            }
            case 10: case 29: {  // callsubr, callgsubr
                if (!sp) return false;
                const Index subrs = read_index(r, b == 10 ? local : font.gsubrs_);
                const i32 n = static_cast<i32>(stack[--sp]) + static_cast<i32>(subr_bias(subrs.count));
                u32 s, e;
                if (n < 0 || !index_item(r, subrs, static_cast<u32>(n), &s, &e)) return false;
                if (!run(s, e)) return false;
                break;
            }
            case 11: --depth; return true;  // return
            case 14:  // endchar
                width(0);
                close();
                ended = true;
                break;
            case 12: {
                const u32 op = r.u8_(p++);
                f32* s = stack;
                if (op == 35 && sp >= 12) curve(s[0], s[1], s[2], s[3], s[4], s[5]), curve(s[6], s[7], s[8], s[9], s[10], s[11]);
                else if (op == 34 && sp >= 7) curve(s[0], 0, s[1], s[2], s[3], 0), curve(s[4], 0, s[5], -s[2], s[6], 0);
                else if (op == 36 && sp >= 9)
                    curve(s[0], s[1], s[2], s[3], s[4], 0), curve(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
                else if (op == 37 && sp >= 11) {
                    const f32 dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
                    const bool horizontal = std::fabs(dx) > std::fabs(dy);
                    curve(s[0], s[1], s[2], s[3], s[4], s[5]);
                    curve(s[6], s[7], s[8], s[9], horizontal ? s[10] : -dx, horizontal ? -dy : s[10]);
                }
                sp = 0;  // (the other escaped operators, arithmetic, are not used by fonts drawn here)
                break;
            }
            default: sp = 0; break;
            }
        }
        --depth;
        return true;
    }
};

bool Font::cff_outline(u32 glyph, FontOutline* out) const {
    const Reader r{data_};
    const Index cs = read_index(r, charstrings_);
    u32 s, e;
    if (!index_item(r, cs, glyph, &s, &e)) return false;
    CffRunner run{*this, r, out};
    run.local = subrs_;
    if (fdarray_ && fdselect_) {
        // FDSelect: format 0 (a byte per glyph) or 3 (ranges)
        u32 fd = 0;
        const u32 format = r.u8_(fdselect_);
        if (format == 0) fd = r.u8_(fdselect_ + 1 + glyph);
        else if (format == 3) {
            const u32 n = r.u16_(fdselect_ + 1);
            for (u32 i = 0; i < n; ++i) {
                const u32 at = fdselect_ + 3 + 3 * i;
                if (glyph >= r.u16_(at) && glyph < r.u16_(at + 3)) {
                    fd = r.u8_(at + 2);
                    break;
                }
            }
        }
        run.local = fd < fd_subrs_.size() ? fd_subrs_[fd] : 0;
    }
    if (!run.run(s, e)) return false;
    run.close();
    return true;
}

bool Font::outline(u32 glyph, FontOutline* out) const {
    out->points.clear(), out->contour_ends.clear();
    return cff_ ? cff_outline(glyph, out) : glyf_outline(glyph, out, 0);
}

bool Font::bounds(u32 glyph, f32* x0, f32* y0, f32* x1, f32* y1) const {
    FontOutline o;
    if (!outline(glyph, &o) || o.points.empty()) return false;
    *x0 = *x1 = o.points[0].x, *y0 = *y1 = o.points[0].y;
    for (const FontPoint& p : o.points) *x0 = std::min(*x0, p.x), *x1 = std::max(*x1, p.x), *y0 = std::min(*y0, p.y), *y1 = std::max(*y1, p.y);
    return true;
}

// ---- rasterizing: each line adds its signed area to the cells it crosses; a running sum along each row gives each
// pixel's coverage.

namespace {

void accumulate(nx::vector<f32>& acc, u32 w, u32 h, f32 x0, f32 y0, f32 x1, f32 y1) {
    if (y0 == y1) return;
    f32 dir = 1;
    if (y0 > y1) std::swap(x0, x1), std::swap(y0, y1), dir = -1;
    const f32 dxdy = (x1 - x0) / (y1 - y0);
    f32 x = x0;
    if (y0 < 0) x -= y0 * dxdy, y0 = 0;
    y1 = std::min(y1, static_cast<f32>(h));
    for (i32 row = static_cast<i32>(std::floor(y0)); row < static_cast<i32>(std::ceil(y1)); ++row) {
        if (row < 0 || row >= static_cast<i32>(h)) continue;
        const usize line = static_cast<usize>(row) * w;
        const f32 dy = std::min(static_cast<f32>(row + 1), y1) - std::max(static_cast<f32>(row), y0);
        const f32 xnext = x + dxdy * dy;
        const f32 d = dy * dir;
        f32 a = x, b = xnext;
        if (a > b) std::swap(a, b);
        a = std::clamp(a, 0.0f, static_cast<f32>(w) - 1.0f), b = std::clamp(b, 0.0f, static_cast<f32>(w) - 1.0f);
        const f32 af = std::floor(a), bc = std::ceil(b);
        const i32 ai = static_cast<i32>(af), bi = static_cast<i32>(bc);
        if (bi <= ai + 1) {
            const f32 mid = 0.5f * (x + xnext) - af;
            acc[line + ai] += d - d * std::clamp(mid, 0.0f, 1.0f);
            acc[line + ai + 1] += d * std::clamp(mid, 0.0f, 1.0f);
        } else {
            const f32 s = 1.0f / (b - a);
            const f32 a_frac = a - af;
            const f32 a0 = 0.5f * s * (1 - a_frac) * (1 - a_frac);
            const f32 b_frac = b - bc + 1;
            const f32 am = 0.5f * s * b_frac * b_frac;
            acc[line + ai] += d * a0;
            if (bi == ai + 2) {
                acc[line + ai + 1] += d * (1 - a0 - am);
            } else {
                const f32 a1 = s * (1.5f - a_frac);
                acc[line + ai + 1] += d * (a1 - a0);
                for (i32 xi = ai + 2; xi < bi - 1; ++xi) acc[line + xi] += d * s;
                const f32 a2 = a1 + static_cast<f32>(bi - ai - 3) * s;
                acc[line + bi - 1] += d * (1 - a2 - am);
            }
            acc[line + bi] += d * am;
        }
        x = xnext;
    }
}

}  // namespace

bool Font::rasterize(u32 glyph, f32 sx, f32 sy, f32 dx, f32 dy, FontBitmap* out) const {
    *out = FontBitmap{};
    FontOutline o;
    if (!outline(glyph, &o)) return false;
    if (o.points.empty()) return true;
    // to pixels, y down, then the bounds
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (FontPoint& p : o.points) {
        p.x = p.x * sx + dx, p.y = -p.y * sy + dy;
        x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    }
    const i32 left = static_cast<i32>(std::floor(x0)), top = static_cast<i32>(std::floor(y0));
    const u32 w = static_cast<u32>(std::ceil(x1) - left) + 2, h = static_cast<u32>(std::ceil(y1) - top) + 1;
    if (w > 4096 || h > 4096) return false;
    nx::vector<f32> acc(static_cast<usize>(w) * h + 1, 0.0f);
    u32 first = 0;
    for (u32 end : o.contour_ends) {
        for (u32 i = first; i < end; ++i) {
            const FontPoint a = o.points[i], b = o.points[i + 1 < end ? i + 1 : first];
            accumulate(acc, w, h, a.x - static_cast<f32>(left), a.y - static_cast<f32>(top), b.x - static_cast<f32>(left),
                       b.y - static_cast<f32>(top));
        }
        first = end;
    }
    nx::vector<u8> full(static_cast<usize>(w) * h);
    u32 c0 = w, c1 = 0, r0 = h, r1 = 0;  // (the inked part)
    for (u32 row = 0; row < h; ++row) {
        f32 sum = 0;
        for (u32 col = 0; col < w; ++col) {
            sum += acc[static_cast<usize>(row) * w + col];
            const u8 c = static_cast<u8>(std::lround(std::min(1.0f, std::fabs(sum)) * 255.0f));
            full[static_cast<usize>(row) * w + col] = c;
            if (c) c0 = std::min(c0, col), c1 = std::max(c1, col + 1), r0 = std::min(r0, row), r1 = std::max(r1, row + 1);
        }
    }
    if (c0 >= c1) return true;  // (nothing inked)
    out->left = left + static_cast<i32>(c0), out->top = -(top + static_cast<i32>(r0));
    out->width = c1 - c0, out->height = r1 - r0;
    out->coverage.resize(static_cast<usize>(out->width) * out->height);
    for (u32 row = 0; row < out->height; ++row)
        std::memcpy(&out->coverage[static_cast<usize>(row) * out->width], &full[static_cast<usize>(row + r0) * w + c0], out->width);
    return true;
}

}  // namespace psvrecore::runtime::media
