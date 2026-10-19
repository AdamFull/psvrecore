#pragma once

// MP4 files (ISO base media, as AvPlayer plays them): the tracks of the movie box with their sample tables, so a
// player can read each sample (its place in the file, its decode and presentation times, whether it is a sync
// sample). AVC video (avc1, with its avcC record) and AAC audio (mp4a, with the AudioSpecificConfig of its esds) are
// described; other tracks are listed as such.

#include "psvrecore/error.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime::media {

// Reads `size` bytes at `offset` of the file; false if they are not all there.
using ReadAt = nx::function_ref<bool(u64 offset, void* out, u32 size)>;

struct Sample {
    u64 offset = 0;
    u32 size = 0;
    i64 decode = 0;   // (track timescale)
    i64 present = 0;  // (track timescale, after the edit list's shift)
    bool sync = false;
};

struct Track {
    enum Kind { Video, Audio, Other } kind = Other;
    u32 id = 0;
    u32 timescale = 1000;
    u64 duration = 0;    // (track timescale)
    char language[4] = {'u', 'n', 'd', 0};
    u32 codec = 0;       // the sample entry's four characters ('avc1', 'mp4a', ...)
    // video
    u32 width = 0, height = 0;
    nx::vector<u8> avcc;  // AVCDecoderConfigurationRecord
    // audio
    u32 channels = 0, sample_rate = 0;
    nx::vector<u8> audio_config;  // AudioSpecificConfig
    nx::vector<Sample> samples;

    // Milliseconds of a time in the track's timescale.
    u64 ms(i64 t) const { return t <= 0 ? 0 : static_cast<u64>(t) * 1000 / timescale; }
};

struct Movie {
    nx::vector<Track> tracks;
    u64 duration_ms = 0;
};

// The movie of a file of `size` bytes.
Result<Movie> parse_mp4(u64 size, ReadAt read);

}  // namespace psvrecore::runtime::media
