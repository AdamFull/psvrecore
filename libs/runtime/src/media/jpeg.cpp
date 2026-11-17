#include "jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psvrecore::runtime::media {

namespace {

// SCE_JPEG_ERROR_* (the decoder's codes for what it refuses)
constexpr i32 code(u32 v) { return static_cast<i32>(v); }
constexpr i32 kImageEmpty = code(0x80650003), kBadMarkerLength = code(0x80650004), kBadDhtCounts = code(0x80650005),
              kBadDhtIndex = code(0x80650006), kBadDqtIndex = code(0x80650007), kDecodeError = code(0x80650009),
              kBadComponentId = code(0x80650011), kBadPrecision = code(0x80650015),
              kUnsupportSampling = code(0x80650016), kComponentCount = code(0x80650017),
              kNoHuffTable = code(0x80650021), kNoQuantTable = code(0x80650022), kNoSoi = code(0x80650023),
              kBadSofMarker = code(0x80650027), kBadSosMarker = code(0x80650028), kNoLossless = code(0x80650031),
              kNoArith = code(0x80650032);

// The order of a block's coefficients in the stream (zig-zag), as natural indices.
constexpr u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// cos((2x + 1) u pi / 16) * C(u) / 2, for the inverse and forward DCTs.
struct Cosines {
    f32 c[8][8];
    Cosines() {
        for (u32 x = 0; x < 8; ++x)
            for (u32 u = 0; u < 8; ++u)
                c[x][u] = static_cast<f32>((u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2.0 * x + 1.0) * u * 3.14159265358979 / 16.0) / 2.0);
    }
};
const Cosines& cosines() {
    static const Cosines c;
    return c;
}

void idct(const f32 in[64], u8* out, u32 pitch) {
    const auto& c = cosines().c;
    f32 tmp[64];
    for (u32 y = 0; y < 8; ++y)  // (rows: over u)
        for (u32 x = 0; x < 8; ++x) {
            f32 s = 0;
            for (u32 u = 0; u < 8; ++u) s += c[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (u32 x = 0; x < 8; ++x)
        for (u32 y = 0; y < 8; ++y) {
            f32 s = 0;
            for (u32 v = 0; v < 8; ++v) s += c[y][v] * tmp[v * 8 + x];
            const i32 p = static_cast<i32>(std::lround(s + 128.0f));
            out[y * pitch + x] = static_cast<u8>(std::clamp(p, 0, 255));
        }
}

void fdct(const f32 in[64], f32 out[64]) {
    const auto& c = cosines().c;
    f32 tmp[64];
    for (u32 y = 0; y < 8; ++y)
        for (u32 u = 0; u < 8; ++u) {
            f32 s = 0;
            for (u32 x = 0; x < 8; ++x) s += c[x][u] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (u32 u = 0; u < 8; ++u)
        for (u32 v = 0; v < 8; ++v) {
            f32 s = 0;
            for (u32 y = 0; y < 8; ++y) s += c[y][v] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

struct Huffman {
    bool present = false;
    // (canonical codes: per length, the first code, the count and where its values start)
    i32 maxcode[18] = {}, valptr[17] = {}, mincode[17] = {};
    u8 values[256] = {};
    bool build(const u8 bits[16], const u8* vals, u32 count) {
        std::memcpy(values, vals, count);
        i32 code = 0, k = 0;
        for (u32 l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += bits[l - 1];
            k += bits[l - 1];
            maxcode[l] = bits[l - 1] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        present = true;
        return static_cast<u32>(k) == count;
    }
};

struct Reader {
    std::span<const u8> d;
    usize pos = 0;
    u32 bits = 0, count = 0;
    bool marker = false;  // (a marker reached: zeros from here)
    u32 bit() {
        if (count == 0) {
            u8 b = 0;
            if (!marker && pos < d.size()) {
                b = d[pos];
                if (b == 0xFF) {
                    const u8 next = pos + 1 < d.size() ? d[pos + 1] : 0;
                    if (next == 0) pos += 2;
                    else marker = true, b = 0;
                } else {
                    ++pos;
                }
            }
            bits = b, count = 8;
        }
        --count;
        return (bits >> count) & 1;
    }
    u32 take(u32 n) {
        u32 v = 0;
        for (u32 i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    i32 decode(const Huffman& h) {
        i32 code = 0;
        for (u32 l = 1; l <= 16; ++l) {
            code = (code << 1) | static_cast<i32>(bit());
            if (h.maxcode[l] >= 0 && code <= h.maxcode[l] && code >= h.mincode[l]) return h.values[h.valptr[l] + code - h.mincode[l]];
        }
        return -1;
    }
    // (a restart marker: the bits left in the byte dropped, the marker skipped)
    void restart() {
        count = 0;
        marker = false;
        while (pos + 1 < d.size() && !(d[pos] == 0xFF && d[pos + 1] >= 0xD0 && d[pos + 1] <= 0xD7)) ++pos;
        if (pos + 1 < d.size()) pos += 2;
    }
};

i32 extend(u32 v, u32 n) { return n && v < (1u << (n - 1)) ? static_cast<i32>(v) - static_cast<i32>((1u << n) - 1) : static_cast<i32>(v); }

struct Parser {
    u16 quant[4][64] = {};
    bool quant_present[4] = {};
    Huffman dc[4], ac[4];
    u32 restart_interval = 0;
    u8 id[3] = {}, tq[3] = {};
};

u32 be16(std::span<const u8> d, usize at) { return at + 1 < d.size() ? (u32{d[at]} << 8) | d[at + 1] : 0; }

// Reads the headers up to the first scan; `scan` its offset (after the SOS segment) when decoding.
i32 headers(std::span<const u8> d, JpegImage* img, Parser* p, usize* scan_at, u8 scan_td[3], u8 scan_ta[3], u32* scan_count) {
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return d.empty() ? kImageEmpty : kNoSoi;
    usize at = 2;
    bool sof = false;
    while (at + 4 <= d.size()) {
        if (d[at] != 0xFF) {
            ++at;
            continue;
        }
        const u8 m = d[at + 1];
        if (m == 0xFF) {
            ++at;
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            at += 2;
            continue;
        }
        if (m == 0xD9) break;
        const u32 len = be16(d, at + 2);
        if (len < 2 || at + 2 + len > d.size()) return kBadMarkerLength;
        const usize s = at + 4, e = at + 2 + len;
        if (m == 0xDB) {
            for (usize q = s; q < e;) {
                const u32 pq = d[q] >> 4, tqi = d[q] & 15;
                if (tqi > 3) return kBadDqtIndex;
                ++q;
                for (u32 k = 0; k < 64; ++k) {
                    const u32 v = pq ? be16(d, q + 2 * k) : d[q + k];
                    p->quant[tqi][kZigzag[k]] = static_cast<u16>(v);
                }
                q += pq ? 128 : 64;
                p->quant_present[tqi] = true;
            }
        } else if (m == 0xC4) {
            for (usize q = s; q < e;) {
                const u32 tc = d[q] >> 4, th = d[q] & 15;
                if (th > 3 || tc > 1) return kBadDhtIndex;
                u32 count = 0;
                for (u32 i = 0; i < 16; ++i) count += d[q + 1 + i];
                if (count > 256 || q + 17 + count > e) return kBadDhtCounts;
                if (!(tc ? p->ac[th] : p->dc[th]).build(&d[q + 1], &d[q + 17], count)) return kBadDhtCounts;
                q += 17 + count;
            }
        } else if (m == 0xDD) {
            p->restart_interval = be16(d, s);
        } else if (m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF) {
            return kNoLossless;
        } else if (m >= 0xC9 && m <= 0xCF && m != 0xCC) {
            return kNoArith;
        } else if (m == 0xC2 || m == 0xC6) {
            return kDecodeError;  // (progressive: the hardware decoder's baseline only)
        } else if (m == 0xC0 || m == 0xC1) {
            if (sof) return kBadSofMarker;
            sof = true;
            if (d[s] != 8) return kBadPrecision;
            img->height = be16(d, s + 1);
            img->width = be16(d, s + 3);
            img->components = d[s + 5];
            if (img->components != 1 && img->components != 3) return kComponentCount;
            if (len < 8 + 3 * img->components || !img->width || !img->height) return kBadSofMarker;
            img->hmax = img->vmax = 1;
            for (u32 c = 0; c < img->components; ++c) {
                p->id[c] = d[s + 6 + 3 * c];
                img->h[c] = d[s + 7 + 3 * c] >> 4;
                img->v[c] = d[s + 7 + 3 * c] & 15;
                p->tq[c] = d[s + 8 + 3 * c];
                if (!img->h[c] || !img->v[c] || img->h[c] > 4 || img->v[c] > 4) return kUnsupportSampling;
                if (img->components == 1) img->h[c] = img->v[c] = 1;
                img->hmax = std::max(img->hmax, img->h[c]);
                img->vmax = std::max(img->vmax, img->v[c]);
            }
            const u32 mx = (img->width + 8 * img->hmax - 1) / (8 * img->hmax), my = (img->height + 8 * img->vmax - 1) / (8 * img->vmax);
            for (u32 c = 0; c < img->components; ++c) {
                img->pitch[c] = mx * 8 * img->h[c];
                img->rows[c] = my * 8 * img->v[c];
            }
        } else if (m == 0xDA) {
            if (!sof) return kBadSosMarker;
            if (!scan_at) return 0;
            const u32 n = d[s];
            if (n < 1 || n > img->components || len < 6 + 2 * n) return kBadSosMarker;
            *scan_count = n;
            for (u32 i = 0; i < n; ++i) {
                const u8 cid = d[s + 1 + 2 * i];
                u32 c = 0;
                while (c < img->components && p->id[c] != cid) ++c;
                if (c == img->components) return kBadComponentId;
                scan_td[i] = static_cast<u8>((c << 4) | (d[s + 2 + 2 * i] >> 4));
                scan_ta[i] = d[s + 2 + 2 * i] & 15;
            }
            *scan_at = e;
            return 0;
        }
        at = e;
    }
    return sof ? kBadSosMarker : kBadSofMarker;
}

// (the standard tables, for motion JPEG frames that leave theirs out)
void standard_tables(Parser* p);

}  // namespace

i32 jpeg_info(std::span<const u8> data, JpegImage* out) {
    Parser p;
    return headers(data, out, &p, nullptr, nullptr, nullptr, nullptr);
}

i32 jpeg_decode(std::span<const u8> data, JpegImage* img) {
    Parser p;
    usize scan = 0;
    u8 td[3] = {}, ta[3] = {};
    u32 n = 0;
    if (const i32 r = headers(data, img, &p, &scan, td, ta, &n); r < 0) return r;
    if (n != img->components && n != 1) return kBadSosMarker;
    for (u32 c = 0; c < img->components; ++c) {
        if (!p.quant_present[p.tq[c] & 3]) return kNoQuantTable;
        img->plane[c].assign(static_cast<usize>(img->pitch[c]) * img->rows[c], 0);
    }
    standard_tables(&p);
    for (u32 i = 0; i < n; ++i)
        if (!p.dc[td[i] & 15].present || !p.ac[ta[i]].present) return kNoHuffTable;
    Reader r{data.subspan(scan)};
    i32 pred[3] = {};
    // (one block of component c at block (bx, by) of its plane)
    const auto block = [&](u32 i, u32 bx, u32 by) -> bool {
        const u32 c = td[i] >> 4;
        f32 coef[64] = {};
        const u16* q = p.quant[p.tq[c] & 3];
        const i32 t = r.decode(p.dc[td[i] & 15]);
        if (t < 0 || t > 11) return false;
        pred[c] += extend(r.take(static_cast<u32>(t)), static_cast<u32>(t));
        coef[0] = static_cast<f32>(pred[c] * q[0]);
        for (u32 k = 1; k < 64;) {
            const i32 rs = r.decode(p.ac[ta[i]]);
            if (rs < 0) return false;
            const u32 run = static_cast<u32>(rs) >> 4, size = static_cast<u32>(rs) & 15;
            if (!size) {
                if (run != 15) break;  // (end of block)
                k += 16;
                continue;
            }
            k += run;
            if (k > 63) return false;
            coef[kZigzag[k]] = static_cast<f32>(extend(r.take(size), size) * q[kZigzag[k]]);
            ++k;
        }
        if (bx * 8 + 8 <= img->pitch[c] && by * 8 + 8 <= img->rows[c])
            idct(coef, img->plane[c].data() + static_cast<usize>(by) * 8 * img->pitch[c] + bx * 8, img->pitch[c]);
        return true;
    };
    const u32 mx = img->pitch[0] / (8 * img->h[0]), my = img->rows[0] / (8 * img->v[0]);
    u32 since_restart = 0;
    const auto restart = [&] {
        if (p.restart_interval && since_restart == p.restart_interval) {
            r.restart();
            pred[0] = pred[1] = pred[2] = 0;
            since_restart = 0;
        }
    };
    if (n == 1) {
        const u32 c = td[0] >> 4;
        const u32 bw = (img->width * img->h[c] / img->hmax + 7) / 8, bh = (img->height * img->v[c] / img->vmax + 7) / 8;
        for (u32 by = 0; by < bh; ++by)
            for (u32 bx = 0; bx < bw; ++bx) {
                restart();
                if (!block(0, bx, by)) return kDecodeError;
                ++since_restart;
            }
    } else {
        for (u32 y = 0; y < my; ++y)
            for (u32 x = 0; x < mx; ++x) {
                restart();
                for (u32 i = 0; i < n; ++i) {
                    const u32 c = td[i] >> 4;
                    for (u32 v = 0; v < img->v[c]; ++v)
                        for (u32 h = 0; h < img->h[c]; ++h)
                            if (!block(i, x * img->h[c] + h, y * img->v[c] + v)) return kDecodeError;
                }
                ++since_restart;
            }
    }
    return 0;
}

void jpeg_to_rgba(const JpegImage& img, u8* out, u32 out_pitch, bool bgra, bool video_range) {
    for (u32 y = 0; y < img.height; ++y)
        for (u32 x = 0; x < img.width; ++x) {
            const f32 Y = img.plane[0][static_cast<usize>(y) * img.pitch[0] + x];
            f32 cb = 128, cr = 128;
            if (img.components == 3) {
                const usize at1 = static_cast<usize>(y * img.v[1] / img.vmax) * img.pitch[1] + x * img.h[1] / img.hmax;
                const usize at2 = static_cast<usize>(y * img.v[2] / img.vmax) * img.pitch[2] + x * img.h[2] / img.hmax;
                cb = img.plane[1][at1], cr = img.plane[2][at2];
            }
            f32 r, g, b;
            if (video_range) {
                const f32 l = 1.164f * (Y - 16);
                r = l + 1.596f * (cr - 128), g = l - 0.391f * (cb - 128) - 0.813f * (cr - 128), b = l + 2.018f * (cb - 128);
            } else {
                r = Y + 1.402f * (cr - 128), g = Y - 0.344136f * (cb - 128) - 0.714136f * (cr - 128), b = Y + 1.772f * (cb - 128);
            }
            u8* px = out + (static_cast<usize>(y) * out_pitch + x) * 4;
            const auto clamp8 = [](f32 v) { return static_cast<u8>(std::clamp(static_cast<i32>(std::lround(v)), 0, 255)); };
            px[bgra ? 2 : 0] = clamp8(r);
            px[1] = clamp8(g);
            px[bgra ? 0 : 2] = clamp8(b);
            px[3] = 255;
        }
}

namespace {

// ITU T.81 Annex K: the example quantization and Huffman tables.
constexpr u8 kQuantLuma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                               14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                               18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                               49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr u8 kQuantChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
constexpr u8 kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr u8 kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr u8 kDcValues[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr u8 kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D};
constexpr u8 kAcLumaValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
constexpr u8 kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr u8 kAcChromaValues[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

void standard_tables(Parser* p) {
    if (!p->dc[0].present) p->dc[0].build(kDcLumaBits, kDcValues, 12);
    if (!p->dc[1].present) p->dc[1].build(kDcChromaBits, kDcValues, 12);
    if (!p->ac[0].present) p->ac[0].build(kAcLumaBits, kAcLumaValues, 162);
    if (!p->ac[1].present) p->ac[1].build(kAcChromaBits, kAcChromaValues, 162);
}

// A table's codes for each value: code, length.
struct Codes {
    u16 code[256] = {};
    u8 length[256] = {};
    Codes(const u8 bits[16], const u8* values) {
        u32 c = 0, k = 0;
        for (u32 l = 1; l <= 16; ++l) {
            for (u32 i = 0; i < bits[l - 1]; ++i, ++k) {
                code[values[k]] = static_cast<u16>(c++);
                length[values[k]] = static_cast<u8>(l);
            }
            c <<= 1;
        }
    }
};

struct Writer {
    nx::vector<u8>& out;
    u32 acc = 0, count = 0;
    void put(u32 v, u32 n) {
        for (u32 i = n; i-- > 0;) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++count == 8) {
                out.push_back(static_cast<u8>(acc));
                if (acc == 0xFF) out.push_back(0);
                acc = count = 0;
            }
        }
    }
    void flush() {
        while (count) put(1, 1);
    }
};

u32 magnitude_bits(i32 v) {
    u32 a = static_cast<u32>(v < 0 ? -v : v), n = 0;
    while (a) ++n, a >>= 1;
    return n;
}

}  // namespace

nx::vector<u8> jpeg_encode(const u8* y, u32 y_pitch, const u8* cb, const u8* cr, u32 c_pitch, u32 width, u32 height,
                           u32 hs, u32 vs, u32 quality) {
    quality = std::clamp<u32>(quality, 1, 100);
    const u32 scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    u8 q[2][64];
    for (u32 i = 0; i < 64; ++i) {
        q[0][i] = static_cast<u8>(std::clamp<u32>((kQuantLuma[i] * scale + 50) / 100, 1, 255));
        q[1][i] = static_cast<u8>(std::clamp<u32>((kQuantChroma[i] * scale + 50) / 100, 1, 255));
    }
    nx::vector<u8> out;
    const auto seg = [&](u8 marker, std::initializer_list<u8> head, const u8* body = nullptr, usize body_size = 0) {
        out.push_back(0xFF), out.push_back(marker);
        const usize len = 2 + head.size() + body_size;
        out.push_back(static_cast<u8>(len >> 8)), out.push_back(static_cast<u8>(len));
        out.insert(out.end(), head.begin(), head.end());
        if (body) out.insert(out.end(), body, body + body_size);
    };
    out.push_back(0xFF), out.push_back(0xD8);
    seg(0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    for (u32 t = 0; t < 2; ++t) {
        u8 zz[64];
        for (u32 k = 0; k < 64; ++k) zz[k] = q[t][kZigzag[k]];
        seg(0xDB, {static_cast<u8>(t)}, zz, 64);
    }
    seg(0xC0, {8, static_cast<u8>(height >> 8), static_cast<u8>(height), static_cast<u8>(width >> 8), static_cast<u8>(width), 3, 1,
               static_cast<u8>((hs << 4) | vs), 0, 2, 0x11, 1, 3, 0x11, 1});
    const auto dht = [&](u8 index, const u8 bits[16], const u8* values, u32 count) {
        nx::vector<u8> body(bits, bits + 16);
        body.insert(body.end(), values, values + count);
        seg(0xC4, {index}, body.data(), body.size());
    };
    dht(0x00, kDcLumaBits, kDcValues, 12);
    dht(0x10, kAcLumaBits, kAcLumaValues, 162);
    dht(0x01, kDcChromaBits, kDcValues, 12);
    dht(0x11, kAcChromaBits, kAcChromaValues, 162);
    seg(0xDA, {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});
    static const Codes dc_luma(kDcLumaBits, kDcValues), ac_luma(kAcLumaBits, kAcLumaValues),
        dc_chroma(kDcChromaBits, kDcValues), ac_chroma(kAcChromaBits, kAcChromaValues);
    Writer w{out};
    i32 pred[3] = {};
    // (a block at (x0, y0) of a plane, edges repeated past its size)
    const auto encode_block = [&](const u8* plane, u32 pitch, u32 pw, u32 ph, u32 x0, u32 y0, u32 c) {
        f32 in[64], coef[64];
        for (u32 j = 0; j < 8; ++j)
            for (u32 i = 0; i < 8; ++i)
                in[j * 8 + i] = static_cast<f32>(plane[static_cast<usize>(std::min(y0 + j, ph - 1)) * pitch + std::min(x0 + i, pw - 1)]) - 128.0f;
        fdct(in, coef);
        const u8* qt = q[c ? 1 : 0];
        const Codes& dc = c ? dc_chroma : dc_luma;
        const Codes& ac = c ? ac_chroma : ac_luma;
        i32 zz[64];
        for (u32 k = 0; k < 64; ++k) zz[k] = static_cast<i32>(std::lround(coef[kZigzag[k]] / qt[kZigzag[k]]));
        const i32 diff = zz[0] - pred[c];
        pred[c] = zz[0];
        const u32 dn = magnitude_bits(diff);
        w.put(dc.code[dn], dc.length[dn]);
        if (dn) w.put(static_cast<u32>(diff < 0 ? diff - 1 : diff) & ((1u << dn) - 1), dn);
        u32 run = 0;
        for (u32 k = 1; k < 64; ++k) {
            if (!zz[k]) {
                ++run;
                continue;
            }
            while (run > 15) {
                w.put(ac.code[0xF0], ac.length[0xF0]);
                run -= 16;
            }
            const u32 n = magnitude_bits(zz[k]);
            const u32 sym = (run << 4) | n;
            w.put(ac.code[sym], ac.length[sym]);
            w.put(static_cast<u32>(zz[k] < 0 ? zz[k] - 1 : zz[k]) & ((1u << n) - 1), n);
            run = 0;
        }
        if (run) w.put(ac.code[0], ac.length[0]);
    };
    const u32 cw = (width + hs - 1) / hs, ch = (height + vs - 1) / vs;
    for (u32 my = 0; my < (height + 8 * vs - 1) / (8 * vs); ++my)
        for (u32 mx = 0; mx < (width + 8 * hs - 1) / (8 * hs); ++mx) {
            for (u32 v = 0; v < vs; ++v)
                for (u32 h = 0; h < hs; ++h) encode_block(y, y_pitch, width, height, (mx * hs + h) * 8, (my * vs + v) * 8, 0);
            encode_block(cb, c_pitch, cw, ch, mx * 8, my * 8, 1);
            encode_block(cr, c_pitch, cw, ch, mx * 8, my * 8, 2);
        }
    w.flush();
    out.push_back(0xFF), out.push_back(0xD9);
    return out;
}

}  // namespace psvrecore::runtime::media
