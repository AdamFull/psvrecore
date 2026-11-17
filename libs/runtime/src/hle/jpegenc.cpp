// The JPEG encoder (SceJpegEncUser): an encoder's set-up, kept here by its context's address (the size, the pixel
// format, the output buffer, the compression ratio, the region encoded, JPEG or motion JPEG headers). Its input is
// YCbCr planes (Y at the frame's width, then Cb and Cr at half its width, half or all its height: 4:2:0 or 4:2:2),
// which sceJpegEncoderCsc makes from RGBA or BGRA; the compression ratio (0-255) maps to a quality of 100 down to 1.
// Encoding gives a baseline JFIF file (media/jpeg.cpp); motion JPEG headers leave the Huffman tables out (the
// standard ones are understood).
//
// The motion JPEG decoder (SceJpegUser): baseline JPEG, any sampling, Huffman tables optional (the standard ones
// when left out). Its YCbCr output is the planes one after another (Y, Cb, Cr), each padded to whole MCUs (the pitches
// sceJpegGetOutputInfo gives); its RGBA output has the padded luma width as its pitch. A decode returns the image's
// size (width << 16 | height). The colour conversions take those planes for a size and sampling, JFIF (full range)
// or BT.601 (video range), to RGBA or BGRA. Downscaled decoding and the split decoder are not implemented.

#include "../media/jpeg.hpp"
#include "hle_common.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceJpegEncUser = 0x3748BFD3, kSceJpegUser = 0x880BF710;
constexpr i32 kJpegInitDone = err::code(0x80650063), kJpegInvalidInitParam = err::code(0x80650067);
constexpr i32 kJpegInvalidPointer = err::code(0x80650010), kJpegInvalidState = err::code(0x80650039),
              kJpegMemorySize = err::code(0x80650041), kJpegInvalidColorFormat = err::code(0x80650050),
              kJpegInvalidDecodeMode = err::code(0x80650060);
constexpr u32 kContextSize = 0x100;
constexpr i32 kImageSize = err::code(0x80650200), kInsufficientBuffer = err::code(0x80650201),
              kInvalidRatio = err::code(0x80650202), kInvalidPixelFormat = err::code(0x80650203),
              kInvalidHeaderMode = err::code(0x80650204), kInvalidPointer = err::code(0x80650205),
              kInvalidInitParam = err::code(0x80650207);

struct Encoder {
    u32 width = 0, height = 0, pixel_format = 0, output = 0, output_size = 0, ratio = 64;
    u32 valid_width = 0, valid_height = 0, header_mode = 0;
};
struct Encoders {
    nx::mutex lock;
    nx::flat_hash_map<u32, Encoder> by_context;
};
Encoders& encoders() {
    static Encoders e;
    return e;
}
// (with the lock held)
Encoder* encoder(u32 context) {
    auto it = encoders().by_context.find(context);
    return it == encoders().by_context.end() ? nullptr : &it->second;
}

bool known_format(u32 f) { return f == 0 || f == 4 || f == 8 || f == 9; }  // (RGBA, BGRA, YCbCr 4:2:0, 4:2:2)
bool planar(u32 f) { return f == 8 || f == 9; }
// The chroma planes' rows: 4:2:2 keeps all of them, 4:2:0 half.
u32 chroma_rows(const Encoder& e) { return e.pixel_format == 9 ? e.height : (e.height + 1) / 2; }

u8 clamp8(f32 v) { return static_cast<u8>(std::clamp(static_cast<i32>(std::lround(v)), 0, 255)); }

i32 sceJpegEncoderGetContextSize() { return kContextSize; }

i32 sceJpegEncoderInit(u32 context, i32 width, i32 height, u32 pixel_format, u32 output, u32 output_size) {
    if (!context || !writable(context, kContextSize) || !output) return kInvalidPointer;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return kImageSize;
    if (!known_format(pixel_format)) return kInvalidPixelFormat;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    all.by_context[context] = Encoder{static_cast<u32>(width), static_cast<u32>(height), pixel_format, output, output_size, 64,
                                      static_cast<u32>(width), static_cast<u32>(height), 0};
    nx::logd(logs::hle, "JPEG encoder at 0x{:08x}: {}x{}, pixel format {}", context, width, height, pixel_format);
    return 0;
}

// SceJpegEncoderInitParam: size, width, height, pixel format, output, its size, option.
i32 sceJpegEncoderInitWithParam(u32 context, Ptr<const u32> param) {
    if (!param || !readable(param.address, 28)) return kInvalidPointer;
    const u32* p = param.get();
    if (p[0] != 28) return kInvalidInitParam;
    return sceJpegEncoderInit(context, static_cast<i32>(p[1]), static_cast<i32>(p[2]), p[3], p[4], p[5]);
}

i32 sceJpegEncoderEnd(u32 context) {
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    return all.by_context.erase(context) ? 0 : kInvalidPointer;
}

i32 sceJpegEncoderSetOutputAddr(u32 context, u32 output, u32 output_size) {
    if (!output) return kInvalidPointer;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    e->output = output, e->output_size = output_size;
    return 0;
}

i32 sceJpegEncoderSetCompressionRatio(u32 context, i32 ratio) {
    if (ratio < 0 || ratio > 255) return kInvalidRatio;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    e->ratio = static_cast<u32>(ratio);
    return 0;
}

// (the part of the frame encoded: its top left)
i32 sceJpegEncoderSetValidRegion(u32 context, i32 width, i32 height) {
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    if (width <= 0 || height <= 0 || static_cast<u32>(width) > e->width || static_cast<u32>(height) > e->height) return kImageSize;
    e->valid_width = static_cast<u32>(width), e->valid_height = static_cast<u32>(height);
    return 0;
}

i32 sceJpegEncoderSetHeaderMode(u32 context, i32 mode) {
    if (mode != 0 && mode != 1) return kInvalidHeaderMode;
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    e->header_mode = static_cast<u32>(mode);
    return 0;
}

// RGBA or BGRA (`pitch` pixels a row) to the encoder's YCbCr planes (JFIF: full range; chroma averaged).
i32 sceJpegEncoderCsc(u32 context, u32 out, u32 in, i32 pitch, i32 input_format) {
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    if ((input_format != 0 && input_format != 4) || !planar(e->pixel_format)) return kInvalidPixelFormat;
    const u32 w = e->width, h = e->height, cw = (w + 1) / 2, ch = chroma_rows(*e);
    if (pitch < static_cast<i32>(w)) return kImageSize;
    if (!readable(in, static_cast<u32>(pitch) * h * 4) || !writable(out, w * h + 2 * cw * ch)) return kInvalidPointer;
    const u8* src = memory().host(in);
    u8* y = memory().host(out);
    u8* cb = y + w * h;
    u8* cr = cb + cw * ch;
    const bool bgra = input_format == 4;
    const auto rgb = [&](u32 x, u32 row, f32* r, f32* g, f32* b) {
        const u8* px = src + (static_cast<usize>(row) * static_cast<u32>(pitch) + x) * 4;
        *r = px[bgra ? 2 : 0], *g = px[1], *b = px[bgra ? 0 : 2];
    };
    for (u32 j = 0; j < h; ++j)
        for (u32 i = 0; i < w; ++i) {
            f32 r, g, b;
            rgb(i, j, &r, &g, &b);
            y[j * w + i] = clamp8(0.299f * r + 0.587f * g + 0.114f * b);
        }
    const u32 vs = e->pixel_format == 9 ? 1 : 2;
    for (u32 j = 0; j < ch; ++j)
        for (u32 i = 0; i < cw; ++i) {
            f32 sr = 0, sg = 0, sb = 0, n = 0;
            for (u32 dy = 0; dy < vs; ++dy)
                for (u32 dx = 0; dx < 2; ++dx) {
                    f32 r, g, b;
                    rgb(std::min(i * 2 + dx, w - 1), std::min(j * vs + dy, h - 1), &r, &g, &b);
                    sr += r, sg += g, sb += b, n += 1;
                }
            sr /= n, sg /= n, sb /= n;
            cb[j * cw + i] = clamp8(128.0f - 0.168736f * sr - 0.331264f * sg + 0.5f * sb);
            cr[j * cw + i] = clamp8(128.0f + 0.5f * sr - 0.418688f * sg - 0.081312f * sb);
        }
    return 0;
}

// The encoder's YCbCr planes to a JPEG in its output buffer: the file's size, or INSUFFICIENT_BUFFER.
i32 sceJpegEncoderEncode(u32 context, u32 in) {
    Encoders& all = encoders();
    nx::scoped_lock<nx::mutex> held(all.lock);
    Encoder* e = encoder(context);
    if (!e) return kInvalidPointer;
    if (!planar(e->pixel_format)) return kInvalidPixelFormat;
    const u32 w = e->width, h = e->height, cw = (w + 1) / 2, ch = chroma_rows(*e);
    if (!readable(in, w * h + 2 * cw * ch)) return kInvalidPointer;
    const u8* y = memory().host(in);
    const u8* cb = y + w * h;
    const u8* cr = cb + cw * ch;
    const u32 quality = 100 - e->ratio * 99 / 255;
    nx::vector<u8> file =
        media::jpeg_encode(y, w, cb, cr, cw, e->valid_width, e->valid_height, 2, e->pixel_format == 9 ? 1 : 2, quality);
    if (e->header_mode == 1) {
        // (motion JPEG: the Huffman table segments left out)
        nx::vector<u8> kept;
        for (usize i = 0; i < file.size();) {
            if (i + 4 <= file.size() && file[i] == 0xFF && file[i + 1] == 0xC4) {
                i += 2 + ((usize{file[i + 2]} << 8) | file[i + 3]);
                continue;
            }
            if (i + 1 < file.size() && file[i] == 0xFF && file[i + 1] == 0xDA) {
                kept.insert(kept.end(), file.begin() + static_cast<isize>(i), file.end());
                break;
            }
            kept.push_back(file[i++]);
        }
        file = std::move(kept);
    }
    if (file.size() > e->output_size) return kInsufficientBuffer;
    if (!writable(e->output, static_cast<u32>(file.size()))) return kInvalidPointer;
    std::memcpy(memory().host(e->output), file.data(), file.size());
    return static_cast<i32>(file.size());
}

std::atomic<bool> g_mjpeg{false};
std::atomic<i32> g_max_split{0};
// (0 split decoders is allowed: the SDK's samples start it so)
i32 sceJpegInitMJpeg(i32 split_decoders) {
    if (split_decoders < 0) return kJpegInvalidInitParam;
    if (g_mjpeg.exchange(true)) return kJpegInitDone;
    g_max_split = split_decoders;
    return 0;
}
// SceJpegMJpegInitParam: size, the most split decoders, option.
i32 sceJpegInitMJpegWithParam(Ptr<const i32> param) {
    if (!param || !readable(param.address, 12)) return kJpegInvalidPointer;
    if (param.get()[0] != 12) return kJpegInvalidInitParam;
    return sceJpegInitMJpeg(param.get()[1]);
}
i32 sceJpegFinishMJpeg() {
    g_mjpeg = false;
    return 0;
}

i32 read_image(u32 jpeg, u32 size, u32 mode, media::JpegImage* img, bool decode) {
    if (!g_mjpeg) return kJpegInvalidState;
    if (!jpeg || !readable(jpeg, size)) return kJpegInvalidPointer;
    if ((mode & 0xF) > 3 || (mode & 0x70)) return kJpegInvalidDecodeMode;  // (downscaling: not implemented)
    const std::span<const u8> data(memory().host(jpeg), size);
    return decode ? media::jpeg_decode(data, img) : media::jpeg_info(data, img);
}
u32 planes_size(const media::JpegImage& img) {
    u32 n = 0;
    for (u32 c = 0; c < img.components; ++c) n += img.pitch[c] * img.rows[c];
    return n;
}

// SceJpegOutputInfo: colour space | sampling, width, height (16 bits each), the output, temporary and coefficient
// buffers' sizes, and four pitches (x, y): the planes' (no colour conversion), or the RGBA picture's.
void write_output_info(u8* p, const media::JpegImage& img, i32 format);
i32 sceJpegGetOutputInfo(u32 jpeg, u32 size, i32 format, i32 mode, Ptr<u8> info) {
    if (!info || !writable(info.address, 52)) return kJpegInvalidPointer;
    if (format != -1 && format != 0 && format != 4) return kJpegInvalidColorFormat;
    media::JpegImage img;
    if (const i32 r = read_image(jpeg, size, static_cast<u32>(mode), &img, false); r < 0) return r;
    write_output_info(info.get(), img, format);
    return 0;
}
void write_output_info(u8* p, const media::JpegImage& img, i32 format) {
    std::memset(p, 0, 52);
    const u32 space = img.components == 1 ? (1u << 16) | 0x101 : (2u << 16) | img.sampling();
    const u16 w = static_cast<u16>(img.width), h = static_cast<u16>(img.height);
    const u32 out = format == -1 ? planes_size(img) : img.pitch[0] * img.rows[0] * 4;
    std::memcpy(p, &space, 4);
    std::memcpy(p + 4, &w, 2);
    std::memcpy(p + 6, &h, 2);
    std::memcpy(p + 8, &out, 4);
    for (u32 c = 0; c < (format == -1 ? img.components : 1u); ++c) {
        std::memcpy(p + 20 + 8 * c, &img.pitch[c], 4);
        std::memcpy(p + 24 + 8 * c, &img.rows[c], 4);
    }
}

// The split decoder (one frame's data fed in parts): SceJpegSplitDecodeCtrl: the stream buffer (pointer, size), the
// write buffer (pointer, size), end of stream, decode mode, the output information (52 bytes), the output and
// coefficient buffers, internal words. The first call takes the stream buffer's bytes; once the header is in, the
// output information is filled and INIT_DONE answered (the game then gives its output buffer); while the end
// marker has not come, INPUT_SUSPENDED asks for more in the write buffer (the stream buffer again, its whole size;
// the game sets the size it loaded and the end flag at the end). Then the whole frame is decoded, as
// sceJpegDecodeMJpegYCbCr writes it, and its size returned. Data is kept here, by the control block's address.
constexpr i32 kJpegExceedMaxSplit = err::code(0x80650062), kJpegInputSuspended = err::code(0x80650064);
struct Split {
    nx::vector<u8> data;
    bool started = false, header = false, asked = false;
};
struct Splits {
    nx::mutex lock;
    nx::flat_hash_map<u32, Split> by_ctrl;
};
Splits& splits() {
    static Splits s;
    return s;
}
i32 sceJpegCreateSplitDecoder(u32 ctrl) {
    if (!g_mjpeg) return kJpegInvalidState;
    if (!ctrl || !writable(ctrl, 0x60)) return kJpegInvalidPointer;
    Splits& all = splits();
    nx::scoped_lock<nx::mutex> held(all.lock);
    if (static_cast<i32>(all.by_ctrl.size()) >= g_max_split) return kJpegExceedMaxSplit;
    all.by_ctrl[ctrl] = Split{};
    return 0;
}
i32 sceJpegDeleteSplitDecoder(u32 ctrl) {
    Splits& all = splits();
    nx::scoped_lock<nx::mutex> held(all.lock);
    return all.by_ctrl.erase(ctrl) ? 0 : kJpegInvalidPointer;
}
bool has_end(const nx::vector<u8>& d) {
    for (usize i = d.size(); i >= 2; --i)
        if (d[i - 2] == 0xFF && d[i - 1] == 0xD9) return true;
    return false;
}
i32 sceJpegSplitDecodeMJpeg(u32 ctrl) {
    if (!ctrl || !writable(ctrl, 0x60)) return kJpegInvalidPointer;
    Splits& all = splits();
    nx::scoped_lock<nx::mutex> held(all.lock);
    auto it = all.by_ctrl.find(ctrl);
    if (it == all.by_ctrl.end()) return kJpegInvalidPointer;
    Split& sp = it->second;
    const u32 stream = psv_ld32(ctrl), stream_size = psv_ld32(ctrl + 4), mode = psv_ld32(ctrl + 20);
    const auto take = [&](u32 at, u32 n) {
        if (!n) return true;
        if (!at || !readable(at, n)) return false;
        const u8* b = memory().host(at);
        sp.data.insert(sp.data.end(), b, b + n);
        return true;
    };
    if (!sp.started) {
        sp.started = true;
        if (!take(stream, stream_size)) return kJpegInvalidPointer;
    } else if (sp.asked) {
        sp.asked = false;
        if (!take(psv_ld32(ctrl + 8), psv_ld32(ctrl + 12))) return kJpegInvalidPointer;
    }
    if ((mode & 0xF) > 3 || (mode & 0x70)) return kJpegInvalidDecodeMode;
    const bool end = psv_ld32(ctrl + 16) != 0 || has_end(sp.data);
    media::JpegImage img;
    if (!sp.header) {
        if (media::jpeg_info(sp.data, &img) == 0) {
            sp.header = true;
            write_output_info(memory().host(ctrl + 24), img, -1);
            return kJpegInitDone;
        }
        if (end) return media::jpeg_info(sp.data, &img);
    }
    if (!end) {
        psv_st32(ctrl + 8, stream), psv_st32(ctrl + 12, stream_size), psv_st32(ctrl + 16, 0);
        sp.asked = true;
        return kJpegInputSuspended;
    }
    if (const i32 r = media::jpeg_decode(sp.data, &img); r < 0) return r;
    const u32 need = planes_size(img), out = psv_ld32(ctrl + 76);
    if (!out || !writable(out, need)) return kJpegInvalidPointer;
    u8* p = memory().host(out);
    for (u32 c = 0; c < img.components; ++c) {
        std::memcpy(p, img.plane[c].data(), img.plane[c].size());
        p += img.plane[c].size();
    }
    return static_cast<i32>((img.width << 16) | img.height);
}

i32 sceJpegDecodeMJpegYCbCr(u32 jpeg, u32 size, u32 out, u32 out_size, i32 mode, u32 /*coef*/, u32 /*coef_size*/) {
    media::JpegImage img;
    if (const i32 r = read_image(jpeg, size, static_cast<u32>(mode), &img, true); r < 0) return r;
    const u32 need = planes_size(img);
    if (out_size < need) return kJpegMemorySize;
    if (!writable(out, need)) return kJpegInvalidPointer;
    u8* p = memory().host(out);
    for (u32 c = 0; c < img.components; ++c) {
        std::memcpy(p, img.plane[c].data(), img.plane[c].size());
        p += img.plane[c].size();
    }
    return static_cast<i32>((img.width << 16) | img.height);
}

i32 sceJpegDecodeMJpeg(u32 jpeg, u32 size, u32 out, u32 out_size, i32 mode, u32 /*temp*/, u32 /*temp_size*/,
                       u32 /*coef*/, u32 /*coef_size*/) {
    media::JpegImage img;
    if (const i32 r = read_image(jpeg, size, static_cast<u32>(mode), &img, true); r < 0) return r;
    const u32 need = img.pitch[0] * img.rows[0] * 4;
    if (out_size < need) return kJpegMemorySize;
    if (!writable(out, need)) return kJpegInvalidPointer;
    media::jpeg_to_rgba(img, memory().host(out), img.pitch[0], false, false);
    return static_cast<i32>((img.width << 16) | img.height);
}

// Planes laid out as the decoder writes them, for a size (width << 16 | height) and sampling (h << 8 | v of luma),
// to RGBA or BGRA at `pitch` pixels a row; option: the pixel format (0, 4) | 0x10 for BT.601's video range.
i32 csc(u32 rgba, u32 ycbcr, i32 xysize, i32 pitch, i32 option, i32 sampling) {
    media::JpegImage img;
    img.width = static_cast<u32>(xysize) >> 16, img.height = static_cast<u32>(xysize) & 0xFFFF;
    img.hmax = (static_cast<u32>(sampling) >> 8) & 0xF, img.vmax = static_cast<u32>(sampling) & 0xF;
    if (!img.width || !img.height || img.hmax < 1 || img.hmax > 4 || img.vmax < 1 || img.vmax > 4) return kJpegInvalidDecodeMode;
    if ((option & 0xF) != 0 && (option & 0xF) != 4) return kJpegInvalidColorFormat;
    if (pitch < static_cast<i32>(img.width)) return kJpegMemorySize;
    img.components = 3;
    img.h[0] = img.hmax, img.v[0] = img.vmax, img.h[1] = img.h[2] = img.v[1] = img.v[2] = 1;
    const u32 mx = (img.width + 8 * img.hmax - 1) / (8 * img.hmax), my = (img.height + 8 * img.vmax - 1) / (8 * img.vmax);
    img.pitch[0] = mx * 8 * img.hmax, img.rows[0] = my * 8 * img.vmax;
    img.pitch[1] = img.pitch[2] = mx * 8, img.rows[1] = img.rows[2] = my * 8;
    if (!readable(ycbcr, planes_size(img)) || !writable(rgba, static_cast<u32>(pitch) * img.height * 4)) return kJpegInvalidPointer;
    const u8* p = memory().host(ycbcr);
    for (u32 c = 0; c < 3; ++c) {
        img.plane[c].assign(p, p + img.pitch[c] * img.rows[c]);
        p += img.pitch[c] * img.rows[c];
    }
    media::jpeg_to_rgba(img, memory().host(rgba), static_cast<u32>(pitch), (option & 0xF) == 4, (option & 0x10) != 0);
    return 0;
}
i32 sceJpegCsc(u32 rgba, u32 ycbcr, i32 xysize, i32 pitch, i32 option, i32 sampling) {
    return csc(rgba, ycbcr, xysize, pitch, option, sampling);
}
i32 sceJpegMJpegCsc(u32 rgba, u32 ycbcr, i32 xysize, i32 pitch, i32 option, i32 sampling) {
    return csc(rgba, ycbcr, xysize, pitch, option, sampling);
}

const Function kDecoder[] = {
    HLE_FUNCTION(0xB030773B, sceJpegInitMJpeg),
    HLE_FUNCTION(0x518163A7, sceJpegInitMJpegWithParam),
    HLE_FUNCTION(0x62842598, sceJpegFinishMJpeg),
    HLE_FUNCTION(0x353BA9B0, sceJpegGetOutputInfo),
    HLE_FUNCTION(0x2A769BD8, sceJpegDecodeMJpegYCbCr),
    HLE_FUNCTION(0x6215B095, sceJpegDecodeMJpeg),
    HLE_FUNCTION(0x6263AEC2, sceJpegCsc),
    HLE_FUNCTION(0xC2380E3A, sceJpegMJpegCsc),
    HLE_FUNCTION(0x123B4734, sceJpegCreateSplitDecoder),
    HLE_FUNCTION(0xDE8D5FA1, sceJpegDeleteSplitDecoder),
    HLE_FUNCTION(0x4598EC9C, sceJpegSplitDecodeMJpeg),
};

const Function kFunctions[] = {
    HLE_FUNCTION(0x2B55844D, sceJpegEncoderGetContextSize),
    HLE_FUNCTION(0x88DA92B4, sceJpegEncoderInit),
    HLE_FUNCTION(0x2E8FE45D, sceJpegEncoderInitWithParam),
    HLE_FUNCTION(0xC87AA849, sceJpegEncoderEnd),
    HLE_FUNCTION(0x25D52D97, sceJpegEncoderSetOutputAddr),
    HLE_FUNCTION(0xB2B828EC, sceJpegEncoderSetCompressionRatio),
    HLE_FUNCTION(0x9511F3BC, sceJpegEncoderSetValidRegion),
    HLE_FUNCTION(0x2F58B12C, sceJpegEncoderSetHeaderMode),
    HLE_FUNCTION(0x824A7D4F, sceJpegEncoderCsc),
    HLE_FUNCTION(0xC60DE94C, sceJpegEncoderEncode),
};

}  // namespace

void register_jpegenc() {
    {
        Encoders& all = encoders();
        nx::scoped_lock<nx::mutex> held(all.lock);
        all.by_context.clear();
    }
    g_mjpeg = false;
    {
        Splits& all = splits();
        nx::scoped_lock<nx::mutex> held(all.lock);
        all.by_ctrl.clear();
    }
    register_library({"SceJpegEncUser", kSceJpegEncUser, kFunctions});
    register_library({"SceJpegUser", kSceJpegUser, kDecoder});
}

}  // namespace psvrecore::runtime::hle
