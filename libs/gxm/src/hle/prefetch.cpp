// Textures decoded ahead. A game sets its textures up (sceGxmTextureInit*, then their palette, gamma and the rest)
// well before a scene draws them: a song in Persona 4: Dancing All Night set its 53 new ones up 3.7 s and more
// before, their texels loaded already, and decoding them when the first scene drew them took a phone's render thread
// 10-16 ms of that scene. Here each is decoded on a thread of its own once the game has left it alone a moment, and
// kept by its description and contents; the renderer takes them when it would decode them (render_scene), if their
// contents are still what was decoded.

#include "prefetch.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/core/hash.h"
#include "core/foundation/platform/os.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u64 kSettle = 50'000'000;   // (ns after the last change: the game done setting it up)
constexpr u64 kBudget = 160ull << 20;  // (decoded bytes kept; the oldest dropped past it: a song kept 110 MiB at most)
constexpr usize kMaxWaiting = 4096;   // (set-ups waiting to be decoded, at most)
constexpr u32 kChanging = 3;          // (decoded this often within kChangingNs: one the game rewrites, skipped for then)
constexpr u64 kChangingNs = 1'000'000'000;

struct Decoded {
    u64 described = 0, contents = 0;
    Texels texels;
};

struct Prefetcher {
    nx::mutex lock;
    nx::hash_map<u32, u64> waiting;  // (control words' address: when to decode)
    // (a description: when it was first decoded lately, and how often since: a texture the game rewrites every
    // frame is not decoded ahead each time; one whose memory holds another texture later still is)
    struct Times {
        u64 since = 0;
        u32 count = 0;
    };
    nx::hash_map<u64, Times> times;
    nx::vector<Decoded> decoded;     // (oldest first)
    PrefetchStats stats;
    bool off = false, native_bc = false;
    std::atomic<bool> started{false};
};

Prefetcher& prefetcher() {
    static Prefetcher* p = new Prefetcher;  // (its thread runs to the process's end)
    return *p;
}

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

// (what decoding reads of the description: the rest, filters and addressing, is the sampler's)
u64 described(const TextureInfo& t) {
    nx::fnv1a64 h;
    for (u32 v : {t.type, t.base_format, t.swizzle, t.width, t.height, t.levels, t.mipmapped ? 1u : 0u, t.address, t.stride,
                  t.palette, t.gamma})
        h.combine(v);
    return h.value();
}

bool paletted(const TextureInfo& t) {
    const u32 code = ((t.base_format >> 24) & 0x1F) | ((t.base_format >> 31) << 7);
    return code == 0x94 || code == 0x95;
}

void decode(Prefetcher& p, u32 words_at) {
    u32 words[4];
    if (!memory().valid(words_at, sizeof words)) return;
    std::memcpy(words, memory().host(words_at), sizeof words);
    const TextureInfo info = decode_texture(words);
    if (!info.address || is_yuv420(info)) return;  // (movie frames: converted on the GPU)
    const u32 size = texture_size(info);
    if (!size || !memory().valid(info.address, size)) return;
    if (memory().deferred(info.address, size)) return;  // (a surface's write-back not made: left so, as long as it may)
    const u32 palette = info.palette && memory().valid(info.palette, 1024) ? info.palette : 0;
    if (paletted(info) && !palette) return;
    const u64 key = described(info);
    const u64 before = texture_contents(info, size, palette);
    {
        const nx::scoped_lock<nx::mutex> held(p.lock);
        for (const Decoded& d : p.decoded)
            if (d.described == key && d.contents == before) return;
        const u64 now = nx::os::ticks_ns();
        Prefetcher::Times& t = p.times[key];
        if (now - t.since > kChangingNs) t.since = now, t.count = 0;
        if (++t.count > kChanging) return;
        if (p.times.size() > 65536) p.times.clear();
    }
    auto read = [](u32 address, void* out, u32 bytes) {
        if (!memory().valid(address, bytes)) return false;
        std::memcpy(out, memory().host(address), bytes);
        return true;
    };
    Result<Texels> texels = read_texels(info, read, p.native_bc, true);
    if (!texels || texels->data.empty()) return;
    // (written while it was decoded: not kept, the renderer decodes it)
    if (texture_contents(info, size, palette) != before) {
        const nx::scoped_lock<nx::mutex> held(p.lock);
        ++p.stats.changed;
        return;
    }
    const nx::scoped_lock<nx::mutex> held(p.lock);
    p.stats.bytes += texels->data.size();
    ++p.stats.decoded;
    p.decoded.push_back({key, before, std::move(*texels)});
    while (p.stats.bytes > kBudget && !p.decoded.empty()) {
        p.stats.bytes -= p.decoded.front().texels.data.size();
        p.decoded.erase(p.decoded.begin());
        ++p.stats.dropped;
    }
}

void run(Prefetcher& p) {
    nx::vector<u32> due;
    for (;;) {
        nx::this_thread::sleep_for(std::chrono::milliseconds(10));
        due.clear();
        {
            const nx::scoped_lock<nx::mutex> held(p.lock);
            if (p.waiting.empty()) continue;
            const u64 now = nx::os::ticks_ns();
            for (const auto& [at, when] : p.waiting)
                if (when <= now) due.push_back(at);
            for (u32 at : due) p.waiting.erase(at);
        }
        for (u32 at : due) decode(p, at);
    }
}

}  // namespace

void prefetch_texture(u32 words) {
    Prefetcher& p = prefetcher();
    const nx::scoped_lock<nx::mutex> held(p.lock);
    if (p.off || (p.waiting.size() >= kMaxWaiting && !p.waiting.contains(words))) return;
    p.waiting[words] = nx::os::ticks_ns() + kSettle;
}

void prefetch_start(bool native_bc) {
    Prefetcher& p = prefetcher();
    if (p.started.exchange(true)) return;
    if (const char* v = std::getenv("PSVRECORE_TEXTURE_PREFETCH"); v && std::atoi(v) == 0) {
        const nx::scoped_lock<nx::mutex> held(p.lock);
        p.off = true;
        p.waiting.clear();
        nx::logi(logs::gpu, "textures: not decoded ahead (PSVRECORE_TEXTURE_PREFETCH=0)");
        return;
    }
    p.native_bc = native_bc;
    nx::thread_options options;
    options.name = "texture prefetch";
    options.priority = nx::thread_priority::BelowNormal;  // (the game's threads first: it has seconds)
    nx::thread worker([&p] { run(p); }, options);
    worker.detach();
}

std::optional<Texels> take_prefetched(const TextureInfo& info, u64 contents) {
    Prefetcher& p = prefetcher();
    const u64 key = described(info);
    const nx::scoped_lock<nx::mutex> held(p.lock);
    for (usize i = 0; i < p.decoded.size(); ++i) {
        if (p.decoded[i].described != key || p.decoded[i].contents != contents) continue;
        std::optional<Texels> t = std::move(p.decoded[i].texels);
        p.stats.bytes -= t->data.size();
        p.decoded.erase(p.decoded.begin() + static_cast<std::ptrdiff_t>(i));
        ++p.stats.taken;
        return t;
    }
    return std::nullopt;
}

PrefetchStats prefetch_stats() {
    Prefetcher& p = prefetcher();
    const nx::scoped_lock<nx::mutex> held(p.lock);
    return p.stats;
}

}  // namespace psvrecore::gxm::hle
