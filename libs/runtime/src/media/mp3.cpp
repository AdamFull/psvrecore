#include "mp3.hpp"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

namespace psvrecore::runtime::media {

Mp3Decoder::Mp3Decoder() : state_(new mp3dec_t) { reset(); }
Mp3Decoder::~Mp3Decoder() { delete static_cast<mp3dec_t*>(state_); }

void Mp3Decoder::reset() { mp3dec_init(static_cast<mp3dec_t*>(state_)); }

Mp3Decoder::Frame Mp3Decoder::decode(std::span<const u8> data, i16* pcm) {
    mp3dec_frame_info_t info{};
    const int samples =
        mp3dec_decode_frame(static_cast<mp3dec_t*>(state_), data.data(), static_cast<int>(data.size()), pcm, &info);
    Frame f;
    f.used = static_cast<u32>(info.frame_bytes);
    f.samples = static_cast<u32>(samples > 0 ? samples : 0);
    f.channels = static_cast<u32>(info.channels);
    f.rate = static_cast<u32>(info.hz);
    return f;
}

}  // namespace psvrecore::runtime::media
