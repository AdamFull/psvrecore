// The MP4 demuxer of AvPlayer: a movie built here, box by box (an AVC track with composition offsets, sync samples, an
// edit list and chunks of several samples; an AAC track with an esds and 64-bit chunk offsets).

#include <cstring>
#include <initializer_list>
#include <string_view>

#include "nxtest.h"
#include "../../libs/runtime/src/media/mp4.hpp"

using namespace psvrecore;
using namespace psvrecore::runtime;

namespace {

struct Bytes {
    nx::vector<u8> b;
    Bytes& u8_(u32 v) {
        b.push_back(static_cast<u8>(v));
        return *this;
    }
    Bytes& u16_(u32 v) { return u8_(v >> 8).u8_(v); }
    Bytes& u32_(u32 v) { return u16_(v >> 16).u16_(v); }
    Bytes& u64_(u64 v) { return u32_(static_cast<u32>(v >> 32)).u32_(static_cast<u32>(v)); }
    Bytes& tag(std::string_view s) {
        for (char c : s) u8_(static_cast<u8>(c));
        return *this;
    }
    Bytes& zeros(u32 n) {
        b.insert(b.end(), n, 0);
        return *this;
    }
    Bytes& add(const Bytes& o) {
        b.insert(b.end(), o.b.begin(), o.b.end());
        return *this;
    }
};

Bytes box(std::string_view type, const Bytes& payload) {
    Bytes out;
    out.u32_(static_cast<u32>(8 + payload.b.size())).tag(type).add(payload);
    return out;
}
Bytes boxes(std::string_view type, std::initializer_list<Bytes> children) {
    Bytes payload;
    for (const Bytes& c : children) payload.add(c);
    return box(type, payload);
}
Bytes full(u32 version = 0) { return Bytes{}.u32_(version << 24); }

Bytes media_header(u32 timescale, u32 duration, const char* language) {
    const u32 lang = language ? ((language[0] - 0x60u) << 10) | ((language[1] - 0x60u) << 5) | (language[2] - 0x60u) : 0;
    return box("mdhd", full().u32_(0).u32_(0).u32_(timescale).u32_(duration).u16_(lang).u16_(0));
}
Bytes handler(std::string_view type) { return box("hdlr", full().u32_(0).tag(type).zeros(12).u8_(0)); }

}  // namespace

TEST_CASE("mp4: tracks, sample tables, composition offsets, sync samples and the edit list") {
    // An AVC track: 4 samples in 2 chunks (3, then 1), 1000 ticks apart (timescale 10000), composition offsets, sync
    // samples 1 and 3, an edit list starting the media at tick 2000.
    const Bytes avcc = Bytes{}.u8_(1).u8_(66).u8_(0).u8_(30).u8_(0xFF).u8_(0xE1).u16_(2).u8_(0x67).u8_(0x42).u8_(1).u16_(2).u8_(0x68).u8_(0xCE);
    const Bytes avc1 = box("avc1", Bytes{}.zeros(6).u16_(1).zeros(16).u16_(320).u16_(240).zeros(50).add(box("avcC", avcc)));
    const Bytes video = boxes("trak", {
        box("tkhd", full().u32_(0).u32_(0).u32_(1).zeros(64)),
        boxes("edts", {box("elst", full().u32_(1).u32_(3000).u32_(2000).u32_(1u << 16))}),
        boxes("mdia", {
            media_header(10000, 40000, "eng"),
            handler("vide"),
            boxes("minf", {boxes("stbl", {
                box("stsd", full().u32_(1).add(avc1)),
                box("stts", full().u32_(1).u32_(4).u32_(1000)),
                box("ctts", full().u32_(4).u32_(1).u32_(2000).u32_(1).u32_(4000).u32_(1).u32_(2000).u32_(1).u32_(2000)),
                box("stss", full().u32_(2).u32_(1).u32_(3)),
                box("stsc", full().u32_(2).u32_(1).u32_(3).u32_(1).u32_(2).u32_(1).u32_(1)),
                box("stsz", full().u32_(0).u32_(4).u32_(100).u32_(200).u32_(300).u32_(400)),
                box("stco", full().u32_(2).u32_(5000).u32_(9000)),
            })}),
        }),
    });
    // An AAC track: 3 samples of 50 bytes in one chunk at a 64-bit offset, 1024 ticks apart (48 kHz).
    const Bytes es = Bytes{}.u8_(3).u8_(25).u16_(2).u8_(0)
                         .u8_(4).u8_(17).u8_(0x40).u8_(0x15).zeros(3).u32_(128000).u32_(128000)
                         .u8_(5).u8_(2).u8_(0x11).u8_(0x90)
                         .u8_(6).u8_(1).u8_(2);
    const Bytes mp4a = box("mp4a", Bytes{}.zeros(6).u16_(1).u16_(0).u16_(0).u32_(0).u16_(2).u16_(16).u16_(0).u16_(0)
                                       .u32_(48000u << 16).add(box("esds", full().add(es))));
    const Bytes audio = boxes("trak", {
        box("tkhd", full().u32_(0).u32_(0).u32_(2).zeros(64)),
        boxes("mdia", {
            media_header(48000, 3072, nullptr),
            handler("soun"),
            boxes("minf", {boxes("stbl", {
                box("stsd", full().u32_(1).add(mp4a)),
                box("stts", full().u32_(1).u32_(3).u32_(1024)),
                box("stsc", full().u32_(1).u32_(1).u32_(3).u32_(1)),
                box("stsz", full().u32_(50).u32_(3)),
                box("co64", full().u32_(1).u64_(0x100000007ull)),
            })}),
        }),
    });
    Bytes file;
    file.add(box("ftyp", Bytes{}.tag("mp42").u32_(0).tag("mp42")));
    file.add(boxes("moov", {box("mvhd", full().u32_(0).u32_(0).u32_(1000).u32_(4000).zeros(80)), video, audio}));
    file.add(box("mdat", Bytes{}.zeros(16)));

    auto movie = media::parse_mp4(file.b.size(), [&](u64 off, void* out, u32 n) {
        if (off + n > file.b.size()) return false;
        std::memcpy(out, file.b.data() + off, n);
        return true;
    });
    REQUIRE(movie.has_value());
    CHECK(movie->duration_ms == 4000);
    REQUIRE(movie->tracks.size() == 2);

    const media::Track& v = movie->tracks[0];
    CHECK(v.kind == media::Track::Video);
    CHECK(v.id == 1);
    CHECK(v.codec == 0x61766331);  // avc1
    CHECK(v.width == 320);
    CHECK(v.height == 240);
    CHECK(std::string_view(v.language) == "eng");
    CHECK(v.avcc == avcc.b);
    REQUIRE(v.samples.size() == 4);
    const u64 offsets[4] = {5000, 5100, 5300, 9000};
    const u32 sizes[4] = {100, 200, 300, 400};
    const i64 present[4] = {0, 3000, 2000, 3000};  // (decode time + composition offset - the edit's 2000)
    const bool sync[4] = {true, false, true, false};
    for (u32 i = 0; i < 4; ++i) {
        CHECK(v.samples[i].offset == offsets[i]);
        CHECK(v.samples[i].size == sizes[i]);
        CHECK(v.samples[i].decode == static_cast<i64>(i) * 1000);
        CHECK(v.samples[i].present == present[i]);
        CHECK(v.samples[i].sync == sync[i]);
    }
    CHECK(v.ms(v.samples[1].present) == 300);

    const media::Track& a = movie->tracks[1];
    CHECK(a.kind == media::Track::Audio);
    CHECK(a.codec == 0x6D703461);  // mp4a
    CHECK(a.channels == 2);
    CHECK(a.sample_rate == 48000);
    CHECK(a.audio_config == nx::vector<u8>{0x11, 0x90});
    CHECK(std::string_view(a.language) == "und");
    REQUIRE(a.samples.size() == 3);
    for (u32 i = 0; i < 3; ++i) {
        CHECK(a.samples[i].offset == 0x100000007ull + 50 * i);
        CHECK(a.samples[i].size == 50);
        CHECK(a.samples[i].present == static_cast<i64>(i) * 1024);
        CHECK(a.samples[i].sync);
    }

    // Not a movie: no movie box; a box running past the end.
    Bytes bad;
    bad.add(box("ftyp", Bytes{}.tag("mp42")));
    CHECK(!media::parse_mp4(bad.b.size(), [&](u64 off, void* out, u32 n) {
        std::memcpy(out, bad.b.data() + off, n);
        return off + n <= bad.b.size();
    }).has_value());
    bad.u32_(1000).tag("moov");
    CHECK(!media::parse_mp4(bad.b.size(), [&](u64 off, void* out, u32 n) {
        if (off + n > bad.b.size()) return false;
        std::memcpy(out, bad.b.data() + off, n);
        return true;
    }).has_value());
}
