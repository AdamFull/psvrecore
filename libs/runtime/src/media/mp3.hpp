#pragma once

// MPEG audio (layers I-III, MPEG-1, 2 and 2.5) by minimp3 (third_party/minimp3, CC0): one frame at a time.

#include <span>

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::media {

class Mp3Decoder {
public:
    Mp3Decoder();
    ~Mp3Decoder();
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    void reset();
    struct Frame {
        u32 used = 0;      // bytes read (the frame, and anything skipped before it; 0: not a whole frame there)
        u32 samples = 0;   // per channel (0: nothing decoded, e.g. a tag skipped)
        u32 channels = 0;  // of the frame
        u32 rate = 0;
    };
    // One frame into `pcm` (up to 1152 samples x the frame's channels, interleaved).
    Frame decode(std::span<const u8> data, i16* pcm);

private:
    void* state_;
};

}  // namespace psvrecore::runtime::media
