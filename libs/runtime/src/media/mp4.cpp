#include "mp4.hpp"

#include <cstring>

#include "core/foundation/strings/format.h"

namespace psvrecore::runtime::media {

namespace {

constexpr u32 fourcc(const char (&s)[5]) {
    return (u32(u8(s[0])) << 24) | (u32(u8(s[1])) << 16) | (u32(u8(s[2])) << 8) | u32(u8(s[3]));
}

// A box's contents, read big-endian (out-of-range reads give 0 and mark the cursor bad).
struct Cursor {
    const u8* data = nullptr;
    usize size = 0, at = 0;
    bool bad = false;

    bool has(usize n) const { return at + n <= size; }
    u64 be(u32 n) {
        if (!has(n)) {
            bad = true;
            at = size;
            return 0;
        }
        u64 v = 0;
        for (u32 i = 0; i < n; ++i) v = (v << 8) | data[at + i];
        at += n;
        return v;
    }
    u8 u8_() { return static_cast<u8>(be(1)); }
    u16 u16_() { return static_cast<u16>(be(2)); }
    u32 u32_() { return static_cast<u32>(be(4)); }
    u64 u64_() { return be(8); }
    void skip(usize n) {
        if (!has(n)) bad = true;
        at = nx::min(at + n, size);
    }
    Cursor sub(usize n) {
        Cursor c{data + at, has(n) ? n : size - at, 0, !has(n)};
        skip(n);
        return c;
    }
};

struct Box {
    u32 type = 0;
    Cursor body;
};

// The next child box of a cursor (type 0 at the end).
Box next_box(Cursor& c) {
    Box b;
    if (!c.has(8)) {
        c.at = c.size;
        return b;
    }
    u64 size = c.u32_();
    b.type = c.u32_();
    usize header = 8;
    if (size == 1) {
        size = c.u64_();
        header = 16;
    } else if (size == 0) {
        size = c.size - c.at + 8;
    }
    if (size < header) {
        c.bad = true;
        c.at = c.size;
        b.type = 0;
        return b;
    }
    b.body = c.sub(static_cast<usize>(size - header));
    return b;
}

template <typename F>
void each_box(Cursor c, F&& f) {
    for (;;) {
        Box b = next_box(c);
        if (!b.type) break;
        f(b);
    }
}

// An MPEG-4 descriptor's tag and contents.
bool descriptor(Cursor& c, u8* tag, Cursor* body) {
    if (!c.has(2)) return false;
    *tag = c.u8_();
    u32 size = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u8 b = c.u8_();
        size = (size << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    *body = c.sub(size);
    return !c.bad;
}

// The AudioSpecificConfig in an esds box (ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo).
nx::vector<u8> audio_specific_config(Cursor esds) {
    esds.skip(4);  // (version and flags)
    u8 tag;
    Cursor es;
    if (!descriptor(esds, &tag, &es) || tag != 3) return {};
    es.skip(2);  // ES_ID
    const u8 flags = es.u8_();
    if (flags & 0x80) es.skip(2);
    if (flags & 0x40) es.skip(es.u8_());
    if (flags & 0x20) es.skip(2);
    Cursor dc;
    while (descriptor(es, &tag, &dc)) {
        if (tag != 4) continue;
        dc.skip(13);  // (object type, stream type, buffer size, bit rates)
        Cursor info;
        while (descriptor(dc, &tag, &info))
            if (tag == 5) return nx::vector<u8>(info.data, info.data + info.size);
    }
    return {};
}

struct TrackTables {
    nx::vector<std::pair<u32, u32>> stts;  // (count, delta)
    nx::vector<std::pair<u32, i32>> ctts;  // (count, offset)
    nx::vector<u32> stss;
    bool has_stss = false;
    struct Run {
        u32 first_chunk, samples;
    };
    nx::vector<Run> stsc;
    u32 uniform_size = 0;
    nx::vector<u32> sizes;
    u32 sample_count = 0;
    nx::vector<u64> chunks;
};

void parse_stbl(Cursor stbl, Track& t, TrackTables& tt) {
    each_box(stbl, [&](Box& b) {
        Cursor& c = b.body;
        switch (b.type) {
            case fourcc("stsd"): {
                c.skip(4);
                const u32 entries = c.u32_();
                if (!entries) break;
                Box e = next_box(c);
                t.codec = e.type;
                Cursor& s = e.body;
                if (e.type == fourcc("avc1") || e.type == fourcc("avc3")) {
                    s.skip(24);
                    t.width = s.u16_();
                    t.height = s.u16_();
                    s.skip(50);
                    each_box(s.sub(s.size - s.at), [&](Box& child) {
                        if (child.type == fourcc("avcC"))
                            t.avcc.assign(child.body.data, child.body.data + child.body.size);
                    });
                } else if (e.type == fourcc("mp4a")) {
                    s.skip(8);
                    const u16 version = s.u16_();  // (QuickTime's sound description versions add fields)
                    s.skip(6);
                    t.channels = s.u16_();
                    s.skip(6);
                    t.sample_rate = s.u32_() >> 16;
                    if (version == 1) s.skip(16);
                    if (version == 2) s.skip(36);
                    each_box(s.sub(s.size - s.at), [&](Box& child) {
                        if (child.type == fourcc("esds")) t.audio_config = audio_specific_config(child.body);
                    });
                }
                break;
            }
            case fourcc("stts"): {
                c.skip(4);
                for (u32 n = c.u32_(); n && !c.bad; --n) {
                    const u32 count = c.u32_(), delta = c.u32_();
                    tt.stts.push_back({count, delta});
                }
                break;
            }
            case fourcc("ctts"): {
                c.skip(4);
                for (u32 n = c.u32_(); n && !c.bad; --n) {
                    const u32 count = c.u32_();
                    tt.ctts.push_back({count, static_cast<i32>(c.u32_())});
                }
                break;
            }
            case fourcc("stss"): {
                c.skip(4);
                tt.has_stss = true;
                for (u32 n = c.u32_(); n && !c.bad; --n) tt.stss.push_back(c.u32_());
                break;
            }
            case fourcc("stsc"): {
                c.skip(4);
                for (u32 n = c.u32_(); n && !c.bad; --n) {
                    const u32 first = c.u32_(), samples = c.u32_();
                    c.skip(4);
                    tt.stsc.push_back({first, samples});
                }
                break;
            }
            case fourcc("stsz"): {
                c.skip(4);
                tt.uniform_size = c.u32_();
                tt.sample_count = c.u32_();
                if (!tt.uniform_size)
                    for (u32 i = 0; i < tt.sample_count && !c.bad; ++i) tt.sizes.push_back(c.u32_());
                break;
            }
            case fourcc("stco"):
            case fourcc("co64"): {
                c.skip(4);
                for (u32 n = c.u32_(); n && !c.bad; --n) tt.chunks.push_back(b.type == fourcc("co64") ? c.u64_() : c.u32_());
                break;
            }
        }
    });
}

// The samples from the tables: chunk by chunk, each run of chunks with its samples per chunk.
bool build_samples(Track& t, const TrackTables& tt, i64 shift) {
    const u32 n = tt.sample_count;
    if (!tt.uniform_size && tt.sizes.size() != n) return false;
    t.samples.resize(n);
    u32 s = 0;
    for (usize r = 0; r < tt.stsc.size() && s < n; ++r) {
        const u32 first = tt.stsc[r].first_chunk;
        const u32 last = r + 1 < tt.stsc.size() ? tt.stsc[r + 1].first_chunk : static_cast<u32>(tt.chunks.size()) + 1;
        if (!first || last < first) return false;
        for (u32 chunk = first; chunk < last && s < n; ++chunk) {
            if (chunk - 1 >= tt.chunks.size()) return false;
            u64 offset = tt.chunks[chunk - 1];
            for (u32 k = 0; k < tt.stsc[r].samples && s < n; ++k, ++s) {
                const u32 size = tt.uniform_size ? tt.uniform_size : tt.sizes[s];
                t.samples[s].offset = offset;
                t.samples[s].size = size;
                offset += size;
            }
        }
    }
    if (s != n) return false;
    i64 time = 0;
    s = 0;
    for (const auto& [count, delta] : tt.stts)
        for (u32 k = 0; k < count && s < n; ++k, ++s) {
            t.samples[s].decode = time;
            time += delta;
        }
    for (; s < n; ++s) t.samples[s].decode = time;  // (tables shorter than the samples)
    s = 0;
    for (const auto& [count, offset] : tt.ctts)
        for (u32 k = 0; k < count && s < n; ++k, ++s) t.samples[s].present = offset;
    for (u32 i = 0; i < n; ++i) t.samples[i].present += t.samples[i].decode - shift;
    if (tt.has_stss) {
        for (u32 number : tt.stss)
            if (number >= 1 && number <= n) t.samples[number - 1].sync = true;
    } else {
        for (Sample& smp : t.samples) smp.sync = true;
    }
    if (!t.duration && n) t.duration = static_cast<u64>(time);
    return true;
}

}  // namespace

Result<Movie> parse_mp4(u64 size, ReadAt read) {
    // The movie box, wherever it is among the top-level boxes.
    u64 at = 0;
    nx::vector<u8> moov;
    while (at + 8 <= size) {
        u8 header[16];
        if (!read(at, header, 8)) return failure("mp4: cannot read a box header");
        u64 box = (u64{header[0]} << 24) | (u64{header[1]} << 16) | (u64{header[2]} << 8) | header[3];
        const u32 type = (u32{header[4]} << 24) | (u32{header[5]} << 16) | (u32{header[6]} << 8) | header[7];
        u64 head = 8;
        if (box == 1) {
            if (!read(at + 8, header + 8, 8)) return failure("mp4: cannot read a box header");
            box = 0;
            for (u32 i = 8; i < 16; ++i) box = (box << 8) | header[i];
            head = 16;
        } else if (box == 0) {
            box = size - at;
        }
        if (box < head || at + box > size) return failure("mp4: a box runs past the end of the file");
        if (type == fourcc("moov")) {
            if (box - head > 64ull * 1024 * 1024) return failure("mp4: the movie box is too large");
            moov.resize(static_cast<usize>(box - head));
            if (!read(at + head, moov.data(), static_cast<u32>(moov.size()))) return failure("mp4: cannot read the movie box");
            break;
        }
        at += box;
    }
    if (moov.empty()) return failure("mp4: no movie box");

    Movie movie;
    u32 movie_timescale = 1000;
    Cursor root{moov.data(), moov.size()};
    each_box(root, [&](Box& b) {
        if (b.type == fourcc("mvhd")) {
            Cursor c = b.body;
            const u8 version = c.u8_();
            c.skip(3);
            c.skip(version == 1 ? 16 : 8);
            movie_timescale = nx::max(c.u32_(), 1u);
            const u64 duration = version == 1 ? c.u64_() : c.u32_();
            movie.duration_ms = duration * 1000 / movie_timescale;
            return;
        }
        if (b.type != fourcc("trak")) return;
        Track t;
        TrackTables tt;
        i64 media_start = 0, empty = 0;  // (edit list: where the media starts, and an empty edit before it)
        bool parsed = false;
        each_box(b.body, [&](Box& tb) {
            if (tb.type == fourcc("tkhd")) {
                Cursor c = tb.body;
                const u8 version = c.u8_();
                c.skip(3);
                c.skip(version == 1 ? 16 : 8);
                t.id = c.u32_();
            } else if (tb.type == fourcc("edts")) {
                each_box(tb.body, [&](Box& eb) {
                    if (eb.type != fourcc("elst")) return;
                    Cursor c = eb.body;
                    const u8 version = c.u8_();
                    c.skip(3);
                    bool media_found = false;
                    for (u32 n = c.u32_(); n && !c.bad && !media_found; --n) {
                        const u64 duration = version == 1 ? c.u64_() : c.u32_();
                        const i64 time = version == 1 ? static_cast<i64>(c.u64_()) : static_cast<i32>(c.u32_());
                        c.skip(4);
                        if (time < 0) {
                            empty += static_cast<i64>(duration);
                        } else {
                            media_start = time;
                            media_found = true;
                        }
                    }
                });
            } else if (tb.type == fourcc("mdia")) {
                each_box(tb.body, [&](Box& mb) {
                    Cursor c = mb.body;
                    if (mb.type == fourcc("mdhd")) {
                        const u8 version = c.u8_();
                        c.skip(3);
                        c.skip(version == 1 ? 16 : 8);
                        t.timescale = nx::max(c.u32_(), 1u);
                        t.duration = version == 1 ? c.u64_() : c.u32_();
                        const u16 lang = c.u16_();
                        if (lang) {
                            t.language[0] = static_cast<char>(((lang >> 10) & 31) + 0x60);
                            t.language[1] = static_cast<char>(((lang >> 5) & 31) + 0x60);
                            t.language[2] = static_cast<char>((lang & 31) + 0x60);
                        }
                    } else if (mb.type == fourcc("hdlr")) {
                        c.skip(8);
                        const u32 handler = c.u32_();
                        t.kind = handler == fourcc("vide") ? Track::Video : handler == fourcc("soun") ? Track::Audio : Track::Other;
                    } else if (mb.type == fourcc("minf")) {
                        each_box(c, [&](Box& ib) {
                            if (ib.type == fourcc("stbl")) {
                                parse_stbl(ib.body, t, tt);
                                parsed = true;
                            }
                        });
                    }
                });
            }
        });
        if (!parsed) return;
        // (an empty edit, in the movie's timescale, delays the media; the media's own start is skipped)
        const i64 shift = media_start - empty * static_cast<i64>(t.timescale) / static_cast<i64>(movie_timescale);
        if (!build_samples(t, tt, shift)) t.samples.clear();
        movie.tracks.push_back(std::move(t));
    });
    if (movie.tracks.empty()) return failure("mp4: no tracks");
    return movie;
}

}  // namespace psvrecore::runtime::media
