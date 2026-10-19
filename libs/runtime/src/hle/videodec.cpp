// The video decoder (SceVideodecUser: sceVideodec*, sceAvcdec*): H.264 access units in Annex B form decoded to
// pictures in the caller's frame memory, with openh264 (media::H264Decoder) in place of the console's hardware
// decoder. Pictures come out in display order, one per sceAvcdecDecode at most (later ones wait for the next call,
// or for sceAvcdecDecodeStop at the end), as RGBA, BGRA, or YUV 4:2:0 in three planes or two (Y, then Cb and Cr
// interleaved), at the caller's pitch, cut to the frame it gives.
//
// The frame memory a decoder is given (SceAvcdecCtrl::frameBuf) is not used: the decoder's own is the host's.
// Progressive streams only (interlaced stream mode is not implemented).

#include <algorithm>
#include <deque>

#include "hle_common.hpp"
#include "../media/decoders.hpp"
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceVideodecUser = 0x163C3727;
constexpr u32 kTypeHwAvcdec = 0x1001;
constexpr i32 kInvalidType = err::code(0x80620001), kInvalidParam = err::code(0x80620002),
              kInvalidState = err::code(0x80620004), kInvalidPointer = err::code(0x80620009),
              kEsBufferFull = err::code(0x8062000A), kNotInitialize = err::code(0x8062000C),
              kInvalidStream = err::code(0x8062000D), kVideodecInvalidType = err::code(0x80620801);
constexpr u32 kEsBuffer = 1u << 20;  // (what one access unit may hold)
constexpr u32 kPixelRgba = 0x0, kPixelBgra = 0x8, kPixelYuv420 = 0x10, kPixelYuv420Packed = 0x20;
constexpr u32 kOptionEnable = 1u << 31;

struct QueryDecoderInfo {
    u32 horizontal, vertical, ref_frames;
};
struct Buf {
    u32 address, size;
};
struct Ctrl {
    u32 handle;
    Buf frame_buf;
};
struct TimeStamp {
    u32 upper, lower;
};
struct Au {
    TimeStamp pts, dts;
    Buf es;
};
struct Frame {
    u32 pixel_type, pitch, width, height;
    u32 horizontal, vertical;
    u32 crop_left, crop_right, crop_top, crop_bottom;
    u8 opt[16];  // (RGBA: alpha, the colour space coefficient)
    u32 picture[2];
};
static_assert(sizeof(Frame) == 64);
struct Info {
    u32 units_in_tick, time_scale;
    u8 fixed_frame_rate, aspect_ratio_idc;
    u16 sar_width, sar_height;
    u8 colour_primaries, transfer, matrix, full_range;
    u8 reserved[3];
    u8 flag;
    u8 padding[2];
    TimeStamp output_pts;
};
static_assert(sizeof(Info) == 32);
struct Picture {
    u32 size;
    Frame frame;
    Info info;
};
static_assert(sizeof(Picture) == 100);
struct ArrayPicture {
    u32 outputs, elements, pictures;  // (pictures: the guest's array of pointers to Picture)
};

struct Decoder {
    media::H264Decoder h264;
    std::deque<media::Picture> waiting;  // (decoded, not handed out yet)
    bool reported = false;               // (the first picture handed out is logged)
    u32 units = 0, failed = 0;           // (access units given, and those that did not decode)
};

struct Videodec {
    nx::mutex lock;
    bool initialized = false;
    nx::hash_map<u32, nx::unique_ptr<Decoder>> decoders;
    u32 next = 1;
};
Videodec& videodec() {
    static Videodec v;
    return v;
}

u8 clamp8(i32 v) { return static_cast<u8>(std::clamp(v, 0, 255)); }

// A picture into the guest's frame: its pixel type, at its pitch, cut to its size. False if the frame is not one.
bool write_picture(const media::Picture& p, Frame& f) {
    const u32 type = f.pixel_type & ~kOptionEnable;
    const u32 w = std::min(p.width - p.crop_right, f.width), h = std::min(p.height - p.crop_bottom, f.height);
    if (!f.picture[0] || !f.pitch || f.pitch < f.width) return false;
    const u8* y = p.data.data();
    const u8* uv = y + usize{p.width} * p.height;  // (U and V interleaved, U first)
    const u32 bytes = type == kPixelRgba || type == kPixelBgra ? 4 : 0;
    const u32 frame_bytes = bytes ? f.pitch * f.height * 4 : f.pitch * f.height * 3 / 2;
    if (!writable(f.picture[0], frame_bytes)) return false;
    u8* out = memory().host(f.picture[0]);
    if (bytes) {
        // (BT.601, limited range; the alpha the option gives)
        const bool option = f.pixel_type & kOptionEnable;
        const u8 alpha = option ? f.opt[0] : 255;
        const bool bgra = type == kPixelBgra;
        for (u32 row = 0; row < h; ++row) {
            u8* dst = out + usize{row} * f.pitch * 4;
            for (u32 x = 0; x < w; ++x) {
                const i32 c = (i32{y[usize{row} * p.width + x]} - 16) * 298;
                const usize at = usize{row / 2} * p.width + (x / 2) * 2;
                const i32 d = i32{uv[at]} - 128, e = i32{uv[at + 1]} - 128;
                const u8 r = clamp8((c + 409 * e + 128) >> 8), g = clamp8((c - 100 * d - 208 * e + 128) >> 8),
                         b = clamp8((c + 516 * d + 128) >> 8);
                dst[x * 4 + 0] = bgra ? b : r;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = bgra ? r : b;
                dst[x * 4 + 3] = alpha;
            }
        }
    } else if (type == kPixelYuv420 || type == kPixelYuv420Packed) {
        for (u32 row = 0; row < h; ++row) std::memcpy(out + usize{row} * f.pitch, y + usize{row} * p.width, w);
        u8* chroma = out + usize{f.pitch} * f.height;
        for (u32 row = 0; row < h / 2; ++row) {
            const u8* src = uv + usize{row} * p.width;
            if (type == kPixelYuv420Packed) {
                std::memcpy(chroma + usize{row} * f.pitch, src, w & ~1u);
            } else {
                u8* cb = chroma + usize{row} * (f.pitch / 2);
                u8* cr = chroma + usize{f.pitch / 2} * (f.height / 2) + usize{row} * (f.pitch / 2);
                for (u32 x = 0; x < w / 2; ++x) cb[x] = src[2 * x], cr[x] = src[2 * x + 1];
            }
        }
    } else {
        return false;
    }
    f.horizontal = p.width - p.crop_right;
    f.vertical = p.height - p.crop_bottom;
    f.crop_left = f.crop_top = 0;
    f.crop_right = f.crop_bottom = 0;
    return true;
}

// Hands out the next waiting picture, if any, into the guest's first picture slot.
i32 hand_out(Decoder& d, Ptr<ArrayPicture> array) {
    if (!array || !writable(array.address, sizeof(ArrayPicture))) return kInvalidPointer;
    array->outputs = 0;
    if (d.waiting.empty()) return 0;
    if (array->elements < 1 || !array->pictures || !memory().valid(array->pictures, 4)) return kInvalidPointer;
    const u32 at = psv_ld32(array->pictures);
    if (!at || !writable(at, sizeof(Picture))) return kInvalidPointer;
    Picture* pic = memory().host_as<Picture>(at);
    const media::Picture p = std::move(d.waiting.front());
    d.waiting.pop_front();
    if (!d.reported) {
        d.reported = true;
        nx::logi(logs::hle, "H.264: first picture {} x {}, into a frame of type 0x{:x}, pitch {}, {} x {} at 0x{:08x}",
                 p.width, p.height, pic->frame.pixel_type, pic->frame.pitch, pic->frame.width, pic->frame.height,
                 pic->frame.picture[0]);
    }
    if (!write_picture(p, pic->frame)) return kInvalidParam;
    std::memset(&pic->info, 0, sizeof pic->info);
    pic->info.output_pts = {static_cast<u32>(static_cast<u64>(p.time) >> 32), static_cast<u32>(p.time)};
    array->outputs = 1;
    return 0;
}

Decoder* decoder_of(Ptr<const Ctrl> ctrl) {
    if (!ctrl || !memory().valid(ctrl.address, sizeof(Ctrl))) return nullptr;
    auto it = videodec().decoders.find(ctrl->handle);
    return it == videodec().decoders.end() ? nullptr : it->second.get();
}

i32 sceVideodecInitLibrary(u32 type, Ptr<const void> /*info*/) {
    if (type != kTypeHwAvcdec) return kVideodecInvalidType;
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    videodec().initialized = true;
    return 0;
}

i32 sceVideodecTermLibrary(u32 type) {
    if (type != kTypeHwAvcdec) return kVideodecInvalidType;
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    videodec().initialized = false;
    videodec().decoders.clear();
    return 0;
}

i32 sceAvcdecQueryDecoderMemSize(u32 type, Ptr<const QueryDecoderInfo> info, Ptr<u32> size) {
    if (type != kTypeHwAvcdec) return kInvalidType;
    if (!info || !size || !memory().valid(info.address, sizeof(QueryDecoderInfo)) || !writable(size.address, 4))
        return kInvalidPointer;
    // (frames of the largest size it was asked for, the reference frames and two more; 1 MiB aligned)
    const u64 frame = u64{info->horizontal} * info->vertical * 3 / 2;
    const u64 bytes = frame * (info->ref_frames + 2) + (1u << 20);
    *size = static_cast<u32>((bytes + 0xFFFFF) & ~u64{0xFFFFF});
    return 0;
}

i32 sceAvcdecCreateDecoder(u32 type, Ptr<Ctrl> ctrl, Ptr<const QueryDecoderInfo> info) {
    if (type != kTypeHwAvcdec) return kInvalidType;
    if (!ctrl || !info || !writable(ctrl.address, sizeof(Ctrl))) return kInvalidPointer;
    Videodec& v = videodec();
    nx::scoped_lock<nx::mutex> held(v.lock);
    if (!v.initialized) return kNotInitialize;
    auto d = nx::make_unique<Decoder>();
    if (!d->h264.open_annexb()) return kInvalidState;
    const u32 handle = v.next++;
    v.decoders[handle] = std::move(d);
    ctrl->handle = handle;
    nx::logi(logs::hle, "H.264 decoder {} ({} x {}, {} reference frames)", handle, info->horizontal, info->vertical,
             info->ref_frames);
    return 0;
}

i32 sceAvcdecDeleteDecoder(Ptr<const Ctrl> ctrl) {
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    if (!decoder_of(ctrl)) return kInvalidPointer;
    videodec().decoders.erase(ctrl->handle);
    return 0;
}

i32 sceAvcdecDecodeAvailableSize(Ptr<const Ctrl> ctrl) {
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    return decoder_of(ctrl) ? static_cast<i32>(kEsBuffer) : kInvalidPointer;
}

i32 sceAvcdecDecode(Ptr<const Ctrl> ctrl, Ptr<const Au> au, Ptr<ArrayPicture> array) {
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    Decoder* d = decoder_of(ctrl);
    if (!d || !au || !memory().valid(au.address, sizeof(Au))) return kInvalidPointer;
    const Au a = *au;
    if (a.es.size > kEsBuffer) return kEsBufferFull;
    if (a.es.size && !memory().valid(a.es.address, a.es.size)) return kInvalidPointer;
    const i64 pts = static_cast<i64>((u64{a.pts.upper} << 32) | a.pts.lower);
    nx::vector<media::Picture> decoded;
    const bool ok = d->h264.decode_annexb({memory().host(a.es.address), a.es.size}, pts, decoded);
    for (auto& p : decoded) d->waiting.push_back(std::move(p));
    ++d->units;
    if (!ok && d->failed++ == 0)
        nx::logw(logs::hle, "H.264: access unit {} ({} bytes) did not decode", d->units, a.es.size);
    const i32 r = hand_out(*d, array);
    if (!ok && r == 0 && array->outputs == 0) return kInvalidStream;
    return r;
}

i32 sceAvcdecDecodeStop(Ptr<const Ctrl> ctrl, Ptr<ArrayPicture> array) {
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    Decoder* d = decoder_of(ctrl);
    if (!d) return kInvalidPointer;
    nx::vector<media::Picture> rest;
    d->h264.flush(rest);
    for (auto& p : rest) d->waiting.push_back(std::move(p));
    return hand_out(*d, array);
}

i32 sceAvcdecDecodeFlush(Ptr<const Ctrl> ctrl) {
    nx::scoped_lock<nx::mutex> held(videodec().lock);
    Decoder* d = decoder_of(ctrl);
    if (!d) return kInvalidPointer;
    d->waiting.clear();
    d->h264.reset();
    return 0;
}

const Function kVideodec[] = {
    HLE_FUNCTION(0xF1AF65A3, sceVideodecInitLibrary),
    HLE_FUNCTION(0x3A5F4924, sceVideodecTermLibrary),
    HLE_FUNCTION(0x97E95EDB, sceAvcdecQueryDecoderMemSize),
    HLE_FUNCTION(0xE82BB69B, sceAvcdecCreateDecoder),
    HLE_FUNCTION(0x8A0E359E, sceAvcdecDeleteDecoder),
    HLE_FUNCTION(0x441673E3, sceAvcdecDecodeAvailableSize),
    HLE_FUNCTION(0xD6190A06, sceAvcdecDecode),
    HLE_FUNCTION(0x9648D853, sceAvcdecDecodeStop),
    HLE_FUNCTION(0x25F31020, sceAvcdecDecodeFlush),
};

}  // namespace

void register_videodec() {
    {
        nx::scoped_lock<nx::mutex> held(videodec().lock);
        videodec().initialized = false;
        videodec().decoders.clear();
    }
    register_library({"SceVideodecUser", kSceVideodecUser, kVideodec});
}

}  // namespace psvrecore::runtime::hle
