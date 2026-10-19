// The video player (SceAvPlayer): MP4 files with AVC video and AAC audio, played against one base clock. Video
// frames come out as GXM YVU420P2 textures (16-aligned, with crop offsets) in buffers from the game's texture
// allocator, audio as interleaved 16-bit stereo in buffers from its allocator.
//
// Decoding happens in the calls that ask for frames (on the caller's thread), as far ahead as the clock needs:
// sceAvPlayerGetVideoData hands out the newest picture due by the clock (skipping late ones), sceAvPlayerGetAudioData
// the next audio frame once it is due within a short lead. Events (ready, play, pause, stop) are delivered to the
// game's callback from the call that causes them. A player without an event callback starts by itself, as one with
// autoStart does (its first video and audio streams).

#include <algorithm>
#include <deque>
#include <optional>
#include <memory>

#include "../media/decoders.hpp"
#include "../media/mp4.hpp"
#include "hle_common.hpp"
#include "psvrecore/runtime/host.hpp"
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceAvPlayer = 0xFA10CE65;
constexpr i32 kInvalidParams = err::code(0x806A0001), kOperationFailed = err::code(0x806A0002),
              kNoMemory = err::code(0x806A0003), kNotSupported = err::code(0x806A0004);
constexpr i32 kEventStop = 1, kEventReady = 2, kEventPlay = 3, kEventPause = 4;
constexpr u32 kTypeVideo = 0, kTypeAudio = 1, kTypeUnknown = 3;
constexpr u64 kAudioLeadMs = 64;
constexpr u32 kAudioFrame = 1024;  // (samples per channel of an audio frame handed out)
constexpr u32 kAudioBuffers = 4, kAudioBufferBytes = kAudioFrame * 2 * 2;

struct MemoryReplacement {
    u32 object, allocate, deallocate, allocate_texture, deallocate_texture;
};
struct FileReplacement {
    u32 object, open, close, read_offset, size;
};
struct InitData {
    MemoryReplacement memory;
    FileReplacement file;
    u32 event_object, event_callback;
    u32 debug_level, base_priority;
    i32 output_video_buffers;
    u8 auto_start;
    u8 reserved[3];
    u32 default_language;
};
static_assert(sizeof(InitData) == 68);

struct FrameInfo {
    u32 data;
    u8 reserved[4];
    u64 time_stamp;
    u8 details[16];
};
static_assert(sizeof(FrameInfo) == 32);
struct FrameInfoEx {
    u32 data;
    u8 reserved[4];
    u64 time_stamp;
    u8 details[80];
};
static_assert(sizeof(FrameInfoEx) == 96);
struct StreamInfo {
    u32 type;
    u8 reserved[4];
    u8 details[16];
    u64 duration, start_time;
};
static_assert(sizeof(StreamInfo) == 40);

struct AudioDetails {
    u16 channels;
    u8 reserved[2];
    u32 sample_rate, size;
    char language[4];
};
struct VideoDetails {
    u32 width, height;
    f32 aspect_ratio;
    char language[4];
};
struct VideoDetailsEx {
    VideoDetails base;
    u32 frame_rate, crop_left, crop_right, crop_top, crop_bottom;
};

struct Event {
    i32 id, source;
};

struct Player {
    nx::mutex lock;
    u32 handle = 0;
    InitData init{};

    // The playback queue; the current source's file and movie.
    nx::vector<nx::string> queue;
    usize current = 0;
    host::File file;  // (no file replacement)
    bool replacement_open = false;
    u64 file_size = 0;
    u32 bounce = 0, bounce_size = 0;  // (guest memory for the replacement's reads)
    PsvContext* ctx = nullptr;        // (the calling thread, for the guest's callbacks)
    std::unique_ptr<media::Movie> movie;
    nx::vector<bool> enabled;
    i32 video = -1, audio = -1;  // (the enabled streams played)

    enum class State { Idle, Ready, Playing, Paused, Stopped } state = State::Idle;
    bool looping = false;
    u64 start_ns = 0, paused_ns = 0;  // (the clock: where it started, and when it was paused)
    u64 base_ms = 0;                  // (the media time at start_ns)

    media::H264Decoder h264;
    media::AacDecoder aac;
    usize next_video = 0, next_audio = 0;
    std::deque<media::Picture> pictures;
    bool video_end = false, audio_end = false;
    i64 last_video_ms = -1;
    nx::vector<u32> frame_buffers;
    u32 frame_bytes = 0, next_frame_buffer = 0;
    u32 audio_buffers[kAudioBuffers] = {};
    u32 next_audio_buffer = 0;
    nx::vector<i16> pcm;
    std::deque<i16> pcm_queue;  // (decoded, not handed out yet: interleaved stereo)
    f64 pcm_time_ms = 0;        // (the media time of its first sample)

    nx::vector<Event> events;
};

struct Players {
    nx::mutex lock;
    nx::flat_hash_map<u32, std::shared_ptr<Player>> by_handle;
};
Players& players() {
    static Players p;
    return p;
}

std::shared_ptr<Player> player_of(u32 handle) {
    Players& ps = players();
    nx::scoped_lock<nx::mutex> held(ps.lock);
    auto it = ps.by_handle.find(handle);
    return it == ps.by_handle.end() ? nullptr : it->second;
}

u64 now_ns() { return kernel().clock_ns(); }

// ---- the game's callbacks

u32 guest_call(PsvContext* ctx, u32 function, std::initializer_list<u32> args) {
    const nx::vector<u32> a(args);
    return call_guest(ctx, function, a);
}

u32 allocate(Player& p, bool texture, u32 alignment, u32 size) {
    const u32 fn = texture ? p.init.memory.allocate_texture : p.init.memory.allocate;
    if (!fn || !p.ctx) return 0;
    return guest_call(p.ctx, fn, {p.init.memory.object, alignment, size});
}
void deallocate(Player& p, bool texture, u32 address) {
    const u32 fn = texture ? p.init.memory.deallocate_texture : p.init.memory.deallocate;
    if (fn && address && p.ctx) guest_call(p.ctx, fn, {p.init.memory.object, address});
}

// Reads of the current source: through the game's file replacement, or from the host file.
bool read_source(Player& p, u64 offset, void* out, u32 size) {
    if (offset + size > p.file_size) return false;
    if (!p.init.file.open) return p.file.valid() && p.file.read_at(offset, out, size) == static_cast<i64>(size);
    u8* dst = static_cast<u8*>(out);
    while (size) {
        const u32 n = nx::min(size, p.bounce_size);
        // readOffset(object, buffer, position (r2:r3), length (stack))
        const i32 r = static_cast<i32>(guest_call(p.ctx, p.init.file.read_offset,
                                                  {p.init.file.object, p.bounce, static_cast<u32>(offset),
                                                   static_cast<u32>(offset >> 32), n}));
        if (r < static_cast<i32>(n)) return false;
        std::memcpy(dst, memory().host(p.bounce), n);
        dst += n, offset += n, size -= n;
    }
    return true;
}

void close_source(Player& p) {
    p.file.close();
    if (p.replacement_open && p.init.file.close && p.ctx) guest_call(p.ctx, p.init.file.close, {p.init.file.object});
    p.replacement_open = false;
    p.movie.reset();
}

i32 open_source(Player& p, const nx::string& path) {
    close_source(p);
    if (p.init.file.open) {
        if (!p.bounce) {
            p.bounce_size = 64 * 1024;
            p.bounce = allocate(p, false, 64, p.bounce_size);
            if (!p.bounce) return kNoMemory;
        }
        const u32 name = runtime().allocate_system(static_cast<u32>(path.size() + 1), "AvPlayer file name");
        std::memcpy(memory().host(name), path.c_str(), path.size() + 1);
        if (static_cast<i32>(guest_call(p.ctx, p.init.file.open, {p.init.file.object, name})) < 0) return kOperationFailed;
        p.replacement_open = true;
        guest_call(p.ctx, p.init.file.size, {p.init.file.object});
        p.file_size = u64{p.ctx->r[0]} | (u64{p.ctx->r[1]} << 32);
    } else {
        nx::string host_path;
        if (runtime().files().resolve(path, &host_path, nullptr, true) < 0) return kOperationFailed;
        host::FileError e = host::FileError::None;
        p.file = host::File::open(host_path, host::OpenOptions{}, &e);
        if (!p.file.valid()) return kOperationFailed;
        p.file_size = static_cast<u64>(p.file.size());
    }
    auto movie = media::parse_mp4(p.file_size, [&](u64 off, void* out, u32 n) { return read_source(p, off, out, n); });
    if (!movie) {
        nx::logw(logs::hle, "AvPlayer: {}: {}", path, movie.error().message);
        close_source(p);
        return kNotSupported;
    }
    p.movie = std::make_unique<media::Movie>(std::move(*movie));
    p.enabled.assign(p.movie->tracks.size(), false);
    p.video = p.audio = -1;
    return 0;
}

// ---- playback

u64 clock_ms(const Player& p) {
    if (p.state == Player::State::Paused) return p.base_ms + (p.paused_ns - p.start_ns) / 1'000'000;
    if (p.state != Player::State::Playing) return 0;
    return p.base_ms + (now_ns() - p.start_ns) / 1'000'000;
}

// Decoders and positions for the enabled streams, from a media time (the last video sync sample at or before it).
void seek(Player& p, u64 ms) {
    p.pictures.clear();
    p.next_video = p.next_audio = 0;
    p.video_end = p.video < 0;
    p.audio_end = p.audio < 0;
    p.last_video_ms = -1;
    u64 from = ms;
    if (p.video >= 0) {
        const media::Track& t = p.movie->tracks[static_cast<usize>(p.video)];
        usize key = 0;
        for (usize i = 0; i < t.samples.size(); ++i)
            if (t.samples[i].sync && t.ms(t.samples[i].present) <= ms) key = i;
        p.next_video = key;
        from = key < t.samples.size() ? t.ms(t.samples[key].present) : 0;
        p.h264.reset();
    }
    if (p.audio >= 0) {
        const media::Track& t = p.movie->tracks[static_cast<usize>(p.audio)];
        while (p.next_audio < t.samples.size() && t.ms(t.samples[p.next_audio].present) < from) ++p.next_audio;
        p.aac.reset();
        p.pcm_queue.clear();
    }
    p.base_ms = from;
    p.start_ns = p.paused_ns = now_ns();
}

i32 start(Player& p) {
    if (!p.movie) return kOperationFailed;
    p.video = p.audio = -1;
    for (usize i = 0; i < p.movie->tracks.size(); ++i) {
        if (!p.enabled[i]) continue;
        const media::Track& t = p.movie->tracks[i];
        if (t.kind == media::Track::Video && p.video < 0 && t.codec == 0x61766331 /* avc1 */) p.video = static_cast<i32>(i);
        if (t.kind == media::Track::Audio && p.audio < 0 && t.codec == 0x6D703461 /* mp4a */) p.audio = static_cast<i32>(i);
    }
    if (p.video < 0 && p.audio < 0) return kOperationFailed;
    if (p.video >= 0 && !p.h264.open(p.movie->tracks[static_cast<usize>(p.video)].avcc)) p.video = -1;
    if (p.audio >= 0 && !p.aac.open(p.movie->tracks[static_cast<usize>(p.audio)].audio_config)) p.audio = -1;
    if (p.video < 0 && p.audio < 0) return kNotSupported;
    p.state = Player::State::Playing;
    seek(p, 0);
    p.events.push_back({kEventPlay, static_cast<i32>(p.current)});
    return 0;
}

void auto_enable(Player& p) {
    bool video = false, audio = false;
    for (usize i = 0; i < p.movie->tracks.size(); ++i) {
        const media::Track& t = p.movie->tracks[i];
        if (t.kind == media::Track::Video && !video) p.enabled[i] = video = true;
        if (t.kind == media::Track::Audio && !audio) p.enabled[i] = audio = true;
    }
}

// The end of the current source: the next in the queue, the same again when looping, or stopped.
void source_ended(Player& p) {
    if (p.current + 1 < p.queue.size()) {
        ++p.current;
        if (open_source(p, p.queue[p.current]) == 0) {
            auto_enable(p);
            start(p);
            return;
        }
    }
    if (p.looping) {
        seek(p, 0);
        return;
    }
    p.state = Player::State::Stopped;
    p.events.push_back({kEventStop, static_cast<i32>(p.current)});
}

// Decodes video until a picture lies past the clock (or the stream ends): a bounded amount per call.
void decode_video(Player& p, u64 now) {
    if (p.video < 0) return;
    const media::Track& t = p.movie->tracks[static_cast<usize>(p.video)];
    nx::vector<media::Picture> out;
    for (u32 n = 0; n < 16 && !p.video_end; ++n) {
        if (!p.pictures.empty() && t.ms(p.pictures.back().time) > now) break;
        if (p.next_video >= t.samples.size()) {
            p.h264.flush(out);
            p.video_end = true;
        } else {
            const media::Sample& s = t.samples[p.next_video++];
            nx::vector<u8> data(s.size);
            if (read_source(p, s.offset, data.data(), s.size)) p.h264.decode(data, s.present, out);
        }
        for (auto& pic : out) {
            auto at = std::upper_bound(p.pictures.begin(), p.pictures.end(), pic.time,
                                       [](i64 v, const media::Picture& x) { return v < x.time; });
            p.pictures.insert(at, std::move(pic));
        }
        out.clear();
    }
}

bool ended(Player& p, u64 now) {
    if (!p.video_end || !p.audio_end || !p.pcm_queue.empty()) return false;
    if (p.video >= 0 && !p.pictures.empty()) return false;
    (void)now;
    return true;
}

template <typename Info>
bool video_frame(PsvContext* ctx, u32 handle, Ptr<Info> info, bool extended) {
    auto pp = player_of(handle);
    if (!pp || !info || !writable(info.address, sizeof(Info))) return false;
    Player& p = *pp;
    nx::vector<Event> events;
    bool delivered = false;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        if (p.state == Player::State::Playing && p.video >= 0) {
            const u64 now = clock_ms(p);
            decode_video(p, now);
            const media::Track& t = p.movie->tracks[static_cast<usize>(p.video)];
            // (the newest picture due; the older ones are late)
            std::optional<media::Picture> due;
            while (!p.pictures.empty() && t.ms(p.pictures.front().time) <= now) {
                due = std::move(p.pictures.front());
                p.pictures.pop_front();
            }
            if (due && static_cast<i64>(t.ms(due->time)) > p.last_video_ms) {
                const u32 bytes = static_cast<u32>(due->data.size());
                if (bytes != p.frame_bytes) {
                    for (u32 b : p.frame_buffers) deallocate(p, true, b);
                    p.frame_buffers.clear();
                    const i32 count = p.init.output_video_buffers >= 2 && p.init.output_video_buffers <= 6 ? p.init.output_video_buffers : 2;
                    for (i32 i = 0; i < count; ++i) p.frame_buffers.push_back(allocate(p, true, 256, bytes));
                    p.frame_bytes = bytes;
                    p.next_frame_buffer = 0;
                }
                const u32 buffer = p.frame_buffers[p.next_frame_buffer];
                p.next_frame_buffer = (p.next_frame_buffer + 1) % static_cast<u32>(p.frame_buffers.size());
                if (buffer && writable(buffer, bytes)) {
                    std::memcpy(memory().host(buffer), due->data.data(), bytes);
                    p.last_video_ms = static_cast<i64>(t.ms(due->time));
                    nx::logd(logs::hle, "AvPlayer video {} ms at clock {} ms", p.last_video_ms, now);
                    Info& f = *info;
                    std::memset(&f, 0, sizeof f);
                    f.data = buffer;
                    f.time_stamp = t.ms(due->time);
                    VideoDetailsEx d{};
                    d.base.width = due->width;
                    d.base.height = due->height;
                    const u32 shown_w = due->width - due->crop_right, shown_h = due->height - due->crop_bottom;
                    d.base.aspect_ratio = shown_h ? static_cast<f32>(shown_w) / static_cast<f32>(shown_h) : 1.0f;
                    std::memcpy(d.base.language, t.language, 4);
                    if (t.samples.size() > 1 && t.duration)
                        d.frame_rate = static_cast<u32>(static_cast<u64>(t.samples.size()) * t.timescale / t.duration);
                    d.crop_right = due->crop_right;
                    d.crop_bottom = due->crop_bottom;
                    std::memcpy(f.details, &d, extended ? sizeof d : sizeof d.base);
                    delivered = true;
                }
            }
            if (ended(p, now)) source_ended(p);
        }
        events.swap(p.events);
    }
    for (const Event& e : events)
        if (p.init.event_callback) guest_call(ctx, p.init.event_callback, {p.init.event_object, u32(e.id), u32(e.source), 0});
    kernel().checkpoint();
    return delivered;
}

// ---- the functions

u32 sceAvPlayerInit(PsvContext* ctx, Ptr<const InitData> init) {
    (void)ctx;
    if (!init || !memory().valid(init.address, sizeof(InitData))) return 0;
    const InitData d = *init;
    if (!d.memory.allocate || !d.memory.deallocate || !d.memory.allocate_texture || !d.memory.deallocate_texture) return 0;
    auto p = std::make_shared<Player>();
    p->init = d;
    p->handle = runtime().allocate_system(16, "AvPlayer handle");
    Players& ps = players();
    nx::scoped_lock<nx::mutex> held(ps.lock);
    ps.by_handle[p->handle] = p;
    return p->handle;
}

i32 sceAvPlayerPostInit(u32 handle, Ptr<const void>) { return player_of(handle) ? 0 : kInvalidParams; }

void deliver(PsvContext* ctx, Player& p, nx::vector<Event>& events) {
    for (const Event& e : events)
        if (p.init.event_callback) guest_call(ctx, p.init.event_callback, {p.init.event_object, u32(e.id), u32(e.source), 0});
    events.clear();
}

i32 sceAvPlayerAddSource(PsvContext* ctx, u32 handle, Ptr<const char> name) {
    auto pp = player_of(handle);
    if (!pp || !name) return kInvalidParams;
    Player& p = *pp;
    const nx::string path = str(name, 1024);
    nx::vector<Event> events;
    i32 r = 0;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        Kernel::Blocking blocking(kernel());
        p.queue.push_back(path);
        if (p.queue.size() == 1) {
            p.current = 0;
            r = open_source(p, path);
            if (r == 0) {
                p.state = Player::State::Ready;
                p.events.push_back({kEventReady, 0});
                if (p.init.auto_start || !p.init.event_callback) {
                    auto_enable(p);
                    start(p);
                }
            }
        }
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    nx::logi(logs::hle, "sceAvPlayerAddSource('{}') = 0x{:08x}", path, static_cast<u32>(r));
    return r;
}

i32 sceAvPlayerStreamCount(u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    nx::scoped_lock<nx::mutex> held(pp->lock);
    return pp->movie ? static_cast<i32>(pp->movie->tracks.size()) : 0;
}

i32 sceAvPlayerGetStreamInfo(u32 handle, u32 id, Ptr<StreamInfo> info) {
    auto pp = player_of(handle);
    if (!pp || !writable(info.address, sizeof(StreamInfo))) return kInvalidParams;
    Player& p = *pp;
    nx::scoped_lock<nx::mutex> held(p.lock);
    if (!p.movie || id >= p.movie->tracks.size()) return kInvalidParams;
    const media::Track& t = p.movie->tracks[id];
    StreamInfo s{};
    s.type = t.kind == media::Track::Video ? kTypeVideo : t.kind == media::Track::Audio ? kTypeAudio : kTypeUnknown;
    if (t.kind == media::Track::Video) {
        VideoDetails d{t.width, t.height, t.height ? static_cast<f32>(t.width) / static_cast<f32>(t.height) : 1.0f, {}};
        std::memcpy(d.language, t.language, 4);
        std::memcpy(s.details, &d, sizeof d);
    } else if (t.kind == media::Track::Audio) {
        AudioDetails d{2, {}, t.sample_rate, kAudioFrame * 2 * 2, {}};
        std::memcpy(d.language, t.language, 4);
        std::memcpy(s.details, &d, sizeof d);
    }
    s.duration = t.ms(static_cast<i64>(t.duration));
    s.start_time = 0;
    *info = s;
    return 0;
}

i32 enable(u32 handle, u32 id, bool on) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    nx::scoped_lock<nx::mutex> held(pp->lock);
    if (!pp->movie || id >= pp->movie->tracks.size()) return kInvalidParams;
    pp->enabled[id] = on;
    return 0;
}
i32 sceAvPlayerEnableStream(u32 handle, u32 id) { return enable(handle, id, true); }
i32 sceAvPlayerDisableStream(u32 handle, u32 id) { return enable(handle, id, false); }

i32 sceAvPlayerStart(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    nx::vector<Event> events;
    i32 r;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        r = p.state == Player::State::Playing ? 0 : start(p);
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    return r;
}

i32 sceAvPlayerStop(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    nx::vector<Event> events;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        if (p.state == Player::State::Stopped || p.state == Player::State::Idle) return kOperationFailed;
        p.state = Player::State::Stopped;
        p.events.push_back({kEventStop, static_cast<i32>(p.current)});
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    return 0;
}

i32 sceAvPlayerPause(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    nx::vector<Event> events;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        if (p.state != Player::State::Playing) return kOperationFailed;
        p.paused_ns = now_ns();
        p.state = Player::State::Paused;
        p.events.push_back({kEventPause, static_cast<i32>(p.current)});
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    return 0;
}

i32 sceAvPlayerResume(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    nx::vector<Event> events;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        if (p.state != Player::State::Paused) return kOperationFailed;
        p.start_ns += now_ns() - p.paused_ns;  // (the clock resumes where it stopped)
        p.state = Player::State::Playing;
        p.events.push_back({kEventPlay, static_cast<i32>(p.current)});
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    return 0;
}

i32 sceAvPlayerSetLooping(u32 handle, u32 loop) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    nx::scoped_lock<nx::mutex> held(pp->lock);
    pp->looping = (loop & 0xFF) != 0;
    return 0;
}

u32 sceAvPlayerIsActive(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return 0;
    Player& p = *pp;
    nx::vector<Event> events;
    bool active;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        if (p.state == Player::State::Playing && ended(p, clock_ms(p))) source_ended(p);
        active = p.state == Player::State::Ready || p.state == Player::State::Playing || p.state == Player::State::Paused;
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    return active ? 1 : 0;
}

u32 sceAvPlayerGetVideoData(PsvContext* ctx, u32 handle, Ptr<FrameInfo> info) {
    return video_frame(ctx, handle, info, false) ? 1 : 0;
}
u32 sceAvPlayerGetVideoDataEx(PsvContext* ctx, u32 handle, Ptr<FrameInfoEx> info) {
    return video_frame(ctx, handle, info, true) ? 1 : 0;
}

u32 sceAvPlayerGetAudioData(PsvContext* ctx, u32 handle, Ptr<FrameInfo> info) {
    auto pp = player_of(handle);
    if (!pp || !info || !writable(info.address, sizeof(FrameInfo))) return 0;
    Player& p = *pp;
    nx::vector<Event> events;
    bool delivered = false;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        if (p.state == Player::State::Playing && p.audio >= 0 && !(p.audio_end && p.pcm_queue.empty())) {
            const media::Track& t = p.movie->tracks[static_cast<usize>(p.audio)];
            const u64 now = clock_ms(p);
            // (decoded audio is handed out in frames of 1024 samples, as the game's port takes them: a frame with
            // SBR makes two)
            if (p.pcm_queue.size() < kAudioFrame * 2) {
                if (p.next_audio >= t.samples.size()) {
                    p.audio_end = true;
                } else if (t.ms(t.samples[p.next_audio].present) <= now + kAudioLeadMs) {
                    const media::Sample& s = t.samples[p.next_audio++];
                    nx::vector<u8> data(s.size);
                    if (p.pcm_queue.empty()) p.pcm_time_ms = static_cast<f64>(t.ms(s.present));
                    if (read_source(p, s.offset, data.data(), s.size)) p.aac.decode(data, p.pcm);
                    else p.pcm.assign(kAudioFrame * 2, 0);
                    p.pcm_queue.insert(p.pcm_queue.end(), p.pcm.begin(), p.pcm.end());
                }
            }
            const u32 rate = p.aac.sample_rate() ? p.aac.sample_rate() : t.sample_rate;
            const bool full = p.pcm_queue.size() >= kAudioFrame * 2;
            if ((full || (p.audio_end && !p.pcm_queue.empty())) && p.pcm_time_ms <= static_cast<f64>(now + kAudioLeadMs)) {
                if (!p.audio_buffers[0])
                    for (u32& b : p.audio_buffers) b = allocate(p, false, 64, kAudioBufferBytes);
                const u32 buffer = p.audio_buffers[p.next_audio_buffer];
                p.next_audio_buffer = (p.next_audio_buffer + 1) % kAudioBuffers;
                const usize count = nx::min<usize>(p.pcm_queue.size(), kAudioFrame * 2);
                const u32 bytes = kAudioFrame * 2 * 2;
                if (buffer && writable(buffer, bytes)) {
                    i16* dst = reinterpret_cast<i16*>(memory().host(buffer));
                    std::copy(p.pcm_queue.begin(), p.pcm_queue.begin() + static_cast<isize>(count), dst);
                    std::fill(dst + count, dst + kAudioFrame * 2, i16{0});  // (the last frame, made whole)
                    FrameInfo& f = *info;
                    std::memset(&f, 0, sizeof f);
                    f.data = buffer;
                    f.time_stamp = static_cast<u64>(p.pcm_time_ms);
                    nx::logd(logs::hle, "AvPlayer audio {} ms at clock {} ms", f.time_stamp, now);
                    AudioDetails d{2, {}, rate, bytes, {}};
                    std::memcpy(d.language, t.language, 4);
                    std::memcpy(f.details, &d, sizeof d);
                    delivered = true;
                }
                p.pcm_queue.erase(p.pcm_queue.begin(), p.pcm_queue.begin() + static_cast<isize>(count));
                p.pcm_time_ms += static_cast<f64>(kAudioFrame) * 1000.0 / rate;
            }
            if (ended(p, now)) source_ended(p);
        }
        events.swap(p.events);
    }
    deliver(ctx, p, events);
    kernel().checkpoint();
    return delivered ? 1 : 0;
}

u64 sceAvPlayerCurrentTime(u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return 0;
    nx::scoped_lock<nx::mutex> held(pp->lock);
    return clock_ms(*pp);
}

i32 sceAvPlayerJumpToTime(PsvContext* ctx, u32 handle, u64 ms) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    nx::scoped_lock<nx::mutex> held(p.lock);
    p.ctx = ctx;
    if (!p.movie || (p.state != Player::State::Playing && p.state != Player::State::Paused)) return kOperationFailed;
    const bool paused = p.state == Player::State::Paused;
    seek(p, ms);
    if (paused) p.paused_ns = p.start_ns;
    return 0;
}

// Normal speed only (trick play shows sync samples alone: not modelled).
i32 sceAvPlayerSetTrickSpeed(u32 handle, i32 speed) {
    if (!player_of(handle)) return kInvalidParams;
    return speed == 100 ? 0 : kNotSupported;
}

i32 sceAvPlayerClose(PsvContext* ctx, u32 handle) {
    auto pp = player_of(handle);
    if (!pp) return kInvalidParams;
    Player& p = *pp;
    {
        nx::scoped_lock<nx::mutex> held(p.lock);
        p.ctx = ctx;
        for (u32 b : p.frame_buffers) deallocate(p, true, b);
        for (u32 b : p.audio_buffers) deallocate(p, false, b);
        if (p.bounce) deallocate(p, false, p.bounce);
        p.frame_buffers.clear();
        close_source(p);
        p.state = Player::State::Idle;
    }
    Players& ps = players();
    nx::scoped_lock<nx::mutex> held(ps.lock);
    ps.by_handle.erase(handle);
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x4C847ADF, sceAvPlayerInit),
    HLE_FUNCTION(0x3F51D38A, sceAvPlayerPostInit),
    HLE_FUNCTION(0xBC83C8FD, sceAvPlayerAddSource),
    HLE_FUNCTION(0x2CD86FD6, sceAvPlayerStreamCount),
    HLE_FUNCTION(0x34E2D370, sceAvPlayerGetStreamInfo),
    HLE_FUNCTION(0x70127AB8, sceAvPlayerEnableStream),
    HLE_FUNCTION(0x15365ECE, sceAvPlayerDisableStream),
    HLE_FUNCTION(0x2ACB4217, sceAvPlayerStart),
    HLE_FUNCTION(0xBD35E360, sceAvPlayerStop),
    HLE_FUNCTION(0x34FD744B, sceAvPlayerPause),
    HLE_FUNCTION(0x11FF162D, sceAvPlayerResume),
    HLE_FUNCTION(0xEC103ADF, sceAvPlayerSetLooping),
    HLE_FUNCTION(0x2E4FF35F, sceAvPlayerIsActive),
    HLE_FUNCTION(0x8BC3221B, sceAvPlayerGetVideoData),
    HLE_FUNCTION(0x22986CD1, sceAvPlayerGetVideoDataEx),
    HLE_FUNCTION(0xA10FC252, sceAvPlayerGetAudioData),
    HLE_FUNCTION(0x804DCECD, sceAvPlayerCurrentTime),
    HLE_FUNCTION(0x5B7DB4BC, sceAvPlayerJumpToTime),
    HLE_FUNCTION(0x86E0CF55, sceAvPlayerSetTrickSpeed),
    HLE_FUNCTION(0x7AA29B2D, sceAvPlayerClose),
};

}  // namespace

void register_avplayer() {
    Players& ps = players();
    {
        nx::scoped_lock<nx::mutex> held(ps.lock);
        ps.by_handle.clear();
    }
    register_library({"SceAvPlayer", kSceAvPlayer, kFunctions});
}

}  // namespace psvrecore::runtime::hle
