#pragma once

// Baseline JPEG (ITU T.81, Huffman, 8-bit), decoded and encoded for SceJpegUser and SceJpegEncUser.
//
// Decoding gives the components as planes (each at its own resolution, its width and height padded to whole blocks
// of the MCU: the planes the hardware decoder writes). Encoding takes Y, Cb and Cr planes at 4:2:0 or 4:2:2.

#include <span>

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::media {

struct JpegImage {
    u32 width = 0, height = 0;
    u32 components = 0;  // 1 (grey) or 3 (Y, Cb, Cr)
    u32 h[3] = {}, v[3] = {};  // sampling factors
    u32 hmax = 1, vmax = 1;
    // Planes, each padded to whole MCUs: width pitch[c], rows rows[c].
    nx::vector<u8> plane[3];
    u32 pitch[3] = {}, rows[3] = {};
    // SCE_JPEG_CS_* sampling (h of luma << 8 | v of luma, chroma at 1:1).
    u32 sampling() const { return (hmax << 8) | vmax; }
};

// The image's size and sampling only (no decoding); 0, or a SCE_JPEG_ERROR_* code.
i32 jpeg_info(std::span<const u8> data, JpegImage* out);
// The whole image; 0, or a SCE_JPEG_ERROR_* code.
i32 jpeg_decode(std::span<const u8> data, JpegImage* out);

// YCbCr (JFIF: full range; else BT.601's video range) to RGBA (or BGRA), each pixel's chroma from its planes'
// sample at its position.
void jpeg_to_rgba(const JpegImage& image, u8* out, u32 out_pitch, bool bgra, bool video_range);

// Planes of Y (width x height, pitch y_pitch), Cb and Cr (width / hs x height / vs, pitch c_pitch) to a JPEG; quality
// 1-100. The JFIF file's bytes.
nx::vector<u8> jpeg_encode(const u8* y, u32 y_pitch, const u8* cb, const u8* cr, u32 c_pitch, u32 width, u32 height,
                           u32 hs, u32 vs, u32 quality);

}  // namespace psvrecore::runtime::media
