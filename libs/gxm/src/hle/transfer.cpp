// The transfer unit (sceGxmTransfer*): copies between layouts and formats, 2x2 box downscales and fills, done on
// the CPU when they are called. Scenes are rendered when they end (render.cpp), so a transfer runs after every
// scene ended before it and before every scene ended after it, as the hardware orders them with sync objects.
//
// RGB pixels convert through U8U8U8U8 (components narrower than 8 bits are widened by replicating their high
// bits, as libgxm documents for colour keys; wider ones keep their high bits); a copy between equal formats, RAW
// and YUV formats among them, moves the pixels' bytes. YUV conversions are not implemented.

#include <cstring>

#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/log.hpp"
#include "state.hpp"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

enum : u32 {
    kU8R = 0x00000, kU4x4 = 0x10000, kU1U5x3 = 0x20000, kU5U6U5 = 0x30000, kU8x2 = 0x40000, kU8x3 = 0x50000,
    kU8x4 = 0x60000, kVYUY = 0x70000, kYVYU = 0x80000, kUYVY = 0x90000, kYUYV = 0xA0000, kU2U10x3 = 0xD0000,
    kRaw16 = 0xF0000, kRaw32 = 0x110000, kRaw64 = 0x120000, kRaw128 = 0x130000,
};
enum : u32 { kLinear = 0, kTiled = 0x400000, kSwizzled = 0x800000 };
enum : u32 { kKeyNone = 0, kKeyPass = 1, kKeyReject = 2 };

u32 bytes_of(u32 format) {
    switch (format) {
        case kU8R: return 1;
        case kU4x4: case kU1U5x3: case kU5U6U5: case kU8x2: case kVYUY: case kYVYU: case kUYVY: case kYUYV: case kRaw16: return 2;
        case kU8x3: return 3;
        case kU8x4: case kU2U10x3: case kRaw32: return 4;
        case kRaw64: return 8;
        case kRaw128: return 16;
        default: return 0;
    }
}
bool is_rgb(u32 f) {
    switch (f) {
        case kU8R: case kU4x4: case kU1U5x3: case kU5U6U5: case kU8x2: case kU8x3: case kU8x4: case kU2U10x3: return true;
        default: return false;
    }
}

// A component of `bits` bits widened to 8 (its high bits replicated into the low ones) or narrowed (its high bits).
u32 widen(u32 v, u32 bits) {
    if (bits >= 8) return v >> (bits - 8);
    u32 out = 0;
    for (i32 shift = 8 - static_cast<i32>(bits); shift > -static_cast<i32>(bits); shift -= static_cast<i32>(bits))
        out |= shift >= 0 ? v << shift : v >> -shift;
    return out & 0xFF;
}
u32 narrow(u32 v8, u32 bits) { return bits >= 8 ? (v8 << (bits - 8)) | (v8 >> (16 - bits)) : v8 >> (8 - bits); }

// RGB pixels to and from U8U8U8U8_ABGR (red in the low byte).
u32 to_abgr(u32 format, u64 p) {
    auto pack = [](u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; };
    switch (format) {
        case kU8R: return pack(p & 0xFF, 0, 0, 0xFF);
        case kU4x4: return pack(widen(p & 15, 4), widen((p >> 4) & 15, 4), widen((p >> 8) & 15, 4), widen((p >> 12) & 15, 4));
        case kU1U5x3: return pack(widen(p & 31, 5), widen((p >> 5) & 31, 5), widen((p >> 10) & 31, 5), (p >> 15) & 1 ? 0xFF : 0);
        case kU5U6U5: return pack(widen(p & 31, 5), widen((p >> 5) & 63, 6), widen((p >> 11) & 31, 5), 0xFF);
        case kU8x2: return pack(p & 0xFF, (p >> 8) & 0xFF, 0, 0xFF);
        case kU8x3: return pack(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, 0xFF);
        case kU2U10x3:
            return pack(widen(p & 1023, 10), widen((p >> 10) & 1023, 10), widen((p >> 20) & 1023, 10), widen((p >> 30) & 3, 2));
        default: return static_cast<u32>(p);  // U8U8U8U8_ABGR
    }
}
u64 from_abgr(u32 format, u32 c) {
    const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (format) {
        case kU8R: return r;
        case kU4x4: return narrow(r, 4) | narrow(g, 4) << 4 | narrow(b, 4) << 8 | narrow(a, 4) << 12;
        case kU1U5x3: return narrow(r, 5) | narrow(g, 5) << 5 | narrow(b, 5) << 10 | (a >> 7) << 15;
        case kU5U6U5: return narrow(r, 5) | narrow(g, 6) << 5 | narrow(b, 5) << 11;
        case kU8x2: return r | g << 8;
        case kU8x3: return r | g << 8 | b << 16;
        case kU2U10x3: return narrow(r, 10) | narrow(g, 10) << 10 | u64{narrow(b, 10)} << 20 | u64{a >> 6} << 30;
        default: return c;
    }
}

// A surface of the transfer: where pixel (x, y) is.
struct Surface {
    u32 format, type, address, x, y;
    i32 stride;  // bytes
    u32 bytes;   // per pixel
    u32 width, height;  // (tiled and swizzled: the whole surface's)
    // The guest address of pixel (px, py) of the area: tiles of 32 x 32 pixels in rows (each tile's pixels in
    // rows), the swizzled order, or rows of the stride (which may be negative).
    u32 at(u32 px, u32 py) const {
        if (type == kTiled) {
            const u32 tiles_w = (width + 31) / 32;
            return address + ((py / 32 * tiles_w + px / 32) * 1024 + py % 32 * 32 + px % 32) * bytes;
        }
        if (type == kSwizzled) return address + static_cast<u32>(swizzled_index(px, py, width, height)) * bytes;
        return static_cast<u32>(static_cast<i64>(address) + static_cast<i64>(y + py) * stride + i64{x + px} * bytes);
    }
    bool valid(u32 w, u32 h) const {
        if (type != kLinear) return memory().valid(address, width * height * bytes, true);
        return memory().valid(at(0, 0), w * bytes, true) && memory().valid(at(0, h - 1), w * bytes, true);
    }
    u64 read(u32 px, u32 py) const {
        u64 v = 0;
        std::memcpy(&v, memory().host(at(px, py)), nx::min<u32>(bytes, 8));
        return v;
    }
    void write(u32 px, u32 py, u64 v) const { std::memcpy(memory().host(at(px, py)), &v, nx::min<u32>(bytes, 8)); }
    void copy_from(const Surface& s, u32 px, u32 py) const { std::memcpy(memory().host(at(px, py)), memory().host(s.at(px, py)), bytes); }
};

i32 sceGxmTransferCopy(u32 width, u32 height, u32 key_value, u32 key_mask, u32 key_mode, u32 src_format, u32 src_type,
                       u32 src_address, u32 src_x, u32 src_y, i32 src_stride, u32 dst_format, u32 dst_type,
                       u32 dst_address, u32 dst_x, u32 dst_y, i32 dst_stride, u32 sync, u32 sync_flags,
                       Ptr<const Notification> notification) {
    finish_scenes();  // (the CPU reads and writes what scenes drew)
    (void)sync, (void)sync_flags;
    if (!src_address || !dst_address) return err::kInvalidPointer;
    if (!width || !height || width > 1024 || height > 1024 || key_mode > kKeyReject) return err::kInvalidValue;
    const u32 sb = bytes_of(src_format), db = bytes_of(dst_format);
    if (!sb || !db || src_type > kSwizzled || dst_type > kSwizzled || (src_type & 0x3FFFFF) || (dst_type & 0x3FFFFF))
        return err::kInvalidValue;
    const bool same = src_format == dst_format && key_mode == kKeyNone;
    if (!same && !(is_rgb(src_format) && is_rgb(dst_format))) {
        nx::logw(logs::gxm, "transfer copy from format 0x{:x} to 0x{:x} not implemented", src_format, dst_format);
        return err::kUnsupported;
    }
    // (tiled and swizzled surfaces: the stride is the width; the area starts at 0, 0)
    const Surface src{src_format, src_type, src_address, src_x, src_y, src_stride, sb, width, height};
    const Surface dst{dst_format, dst_type, dst_address, dst_x, dst_y, dst_stride, db, width, height};
    if (!src.valid(width, height) || !dst.valid(width, height)) return err::kInvalidPointer;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (same) {
                dst.copy_from(src, x, y);
                continue;
            }
            const u32 c = to_abgr(src_format, src.read(x, y));
            if (key_mode != kKeyNone) {
                const bool match = (c & key_mask) == (key_value & key_mask);
                if (match == (key_mode == kKeyReject)) continue;
            }
            dst.write(x, y, src_format == dst_format ? src.read(x, y) : from_abgr(dst_format, c));
        }
    }
    notify(notification);
    return 0;
}

i32 sceGxmTransferDownscale(u32 src_format, u32 src_address, u32 src_x, u32 src_y, u32 src_width, u32 src_height,
                            i32 src_stride, u32 dst_format, u32 dst_address, u32 dst_x, u32 dst_y, i32 dst_stride,
                            u32 sync, u32 sync_flags, Ptr<const Notification> notification) {
    finish_scenes();  // (the CPU reads and writes what scenes drew)
    (void)sync, (void)sync_flags;
    if (!src_address || !dst_address) return err::kInvalidPointer;
    if (src_width < 2 || src_height < 2 || src_width > 1024 || src_height > 1024 || (src_width & 1) || (src_height & 1))
        return err::kInvalidValue;
    if (!is_rgb(src_format) || !is_rgb(dst_format)) return err::kUnsupported;
    const u32 w = src_width / 2, h = src_height / 2;
    const Surface src{src_format, kLinear, src_address, src_x, src_y, src_stride, bytes_of(src_format), src_width, src_height};
    const Surface dst{dst_format, kLinear, dst_address, dst_x, dst_y, dst_stride, bytes_of(dst_format), w, h};
    if (!src.valid(src_width, src_height) || !dst.valid(w, h)) return err::kInvalidPointer;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            // (the 2 x 2 box's average per component, rounded to nearest)
            u32 sum[4] = {};
            for (u32 k = 0; k < 4; ++k) {
                const u32 c = to_abgr(src_format, src.read(2 * x + (k & 1), 2 * y + (k >> 1)));
                for (u32 j = 0; j < 4; ++j) sum[j] += (c >> (8 * j)) & 0xFF;
            }
            u32 c = 0;
            for (u32 j = 0; j < 4; ++j) c |= ((sum[j] + 2) / 4) << (8 * j);
            dst.write(x, y, from_abgr(dst_format, c));
        }
    }
    notify(notification);
    return 0;
}

i32 sceGxmTransferFill(u32 color, u32 dst_format, u32 dst_address, u32 dst_x, u32 dst_y, u32 width, u32 height,
                       i32 dst_stride, u32 sync, u32 sync_flags, Ptr<const Notification> notification) {
    finish_scenes();  // (the CPU reads and writes what scenes drew)
    (void)sync, (void)sync_flags;
    if (!dst_address) return err::kInvalidPointer;
    if (!width || !height || width > 1024 || height > 1024) return err::kInvalidValue;
    if (!is_rgb(dst_format)) {
        nx::logw(logs::gxm, "transfer fill of format 0x{:x} not implemented", dst_format);
        return err::kUnsupported;
    }
    const Surface dst{dst_format, kLinear, dst_address, dst_x, dst_y, dst_stride, bytes_of(dst_format), width, height};
    if (!dst.valid(width, height)) return err::kInvalidPointer;
    // (U2U10U10U10: the colour is in that format already)
    const u64 v = dst_format == kU2U10x3 ? color : from_abgr(dst_format, color);
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) dst.write(x, y, v);
    notify(notification);
    return 0;
}

i32 sceGxmTransferFinish() {
    finish_scenes();
    return 0;
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0x62312BF8, sceGxmTransferCopy),
    HLE_FUNCTION(0xD10F7EAD, sceGxmTransferDownscale),
    HLE_FUNCTION(0x69DDFF5E, sceGxmTransferFill),
    HLE_FUNCTION(0x45229C39, sceGxmTransferFinish),
};

}  // namespace

void register_transfer() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
