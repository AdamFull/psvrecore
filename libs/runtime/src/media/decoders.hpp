#pragma once

// AvPlayer's decoders: H.264 (openh264) to pictures as AvPlayer hands them out, and AAC (the PacketVideo decoder) to
// interleaved 16-bit stereo.

#include <memory>
#include <span>

#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::media {

// A decoded picture: YUV 4:2:0 in two planes (Y, then U and V interleaved, U first: GXM's YVU420P2), its size
// rounded up to whole macroblocks; the crop offsets say what of it is the picture.
struct Picture {
    i64 time = 0;  // (the sample's presentation time, in its track's timescale)
    u32 width = 0, height = 0;  // (multiples of 16)
    u32 crop_right = 0, crop_bottom = 0;
    nx::vector<u8> data;  // width * height bytes of Y, then width * height / 2 of U and V
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Starts a stream described by its avcC record (the parameter sets, the NAL length size).
    bool open(std::span<const u8> avcc);
    // Starts a stream given as an Annex B byte stream (start codes; the parameter sets in it).
    bool open_annexb();
    // Decodes one access unit of an Annex B stream, as decode() does.
    bool decode_annexb(std::span<const u8> au, i64 time, nx::vector<Picture>& out);
    // Decodes one sample (length-prefixed NAL units); the pictures it completes, in presentation order, are added
    // to `out` (with B-frames a picture can come out a few samples later).
    bool decode(std::span<const u8> sample, i64 time, nx::vector<Picture>& out);
    // The pictures still held back (at the end of the stream).
    void flush(nx::vector<Picture>& out);
    // Starts over (after a seek), with the same parameter sets.
    void reset();

private:
    struct State;
    std::unique_ptr<State> s_;
};

class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    // Starts a stream described by its AudioSpecificConfig.
    bool open(std::span<const u8> config);
    // Decodes one frame into interleaved stereo (1024 samples per channel, 2048 with SBR); false on an error.
    bool decode(std::span<const u8> frame, nx::vector<i16>& pcm);
    u32 sample_rate() const;
    void reset();

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace psvrecore::runtime::media
