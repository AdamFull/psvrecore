#include "frame_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::platform {

namespace {

// The font: each character's seven rows, five pixels each (the high bit the left one).
struct Glyph {
    char c;
    u8 rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}}, {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};
constexpr usize kGlyphs = sizeof kFont / sizeof kFont[0];
constexpr u32 kCell = 8;                                 // (pixels a character's cell is wide and high in the atlas)
constexpr u32 kAtlasWidth = kCell * (kGlyphs + 1), kAtlasHeight = kCell;  // (the last cell white)

i32 glyph_of(char c) {
    for (usize i = 0; i < kGlyphs; ++i)
        if (kFont[i].c == c) return static_cast<i32>(i);
    return -1;
}

constexpr usize kShown = 120;  // (frames the graph shows)
constexpr f32 kGraphMs = 50.0f;  // (the graph's top)

}  // namespace

FrameStats::FrameStats() {
    if (const char* v = std::getenv("PSVRECORE_FRAME_STATS"); v && *v) {
        const f64 s = std::atof(v);
        if (s > 0) log_period_ = static_cast<u64>(s * 1e9);
    }
}

void FrameStats::record(u64 ns) {
    if (last_ && ns > last_) {
        times_[next_] = ns - last_;
        at_[next_] = ns;
        next_ = (next_ + 1) % kHistory;
        count_ = nx::min(count_ + 1, kHistory);
        if (log_period_) {
            if (!log_from_) log_from_ = ns;
            // (a frame that took half again as long as the period's median so far, or as 50 ms: when, and how long)
            if (!period_.empty()) {
                nx::vector<u64> sorted = period_;
                std::nth_element(sorted.begin(), sorted.begin() + static_cast<isize>(sorted.size() / 2), sorted.end());
                const u64 median = sorted[sorted.size() / 2];
                if ((ns - last_) * 2 >= median * 3 && sorted.size() >= 10) {
                    // (and with PSVRECORE_HITCH_TRACE, what the game's threads did since the frame before the late one)
                    const u64 from = last_ > median ? last_ - median : 0;
                    nx::string threads = runtime::hle::describe_window(from, ns);
                    if (!threads.empty() && !cpu_before_.empty()) {
                        // (what the scheduler gave the threads since the frame before: the console's CPUs held, and the
                        // time ready without one)
                        threads += "  CPUs since the frame before (held, waited for):";
                        for (const auto& now_use : runtime::runtime().kernel().cpu_use())
                            for (const auto& was : cpu_before_)
                                if (was.uid == now_use.uid && (now_use.held_ns - was.held_ns >= 2'000'000 ||
                                                               now_use.ready_ns - was.ready_ns >= 2'000'000))
                                    threads += nx::format(" {} (priority {}) {:.1f}, {:.1f};", now_use.name, now_use.priority,
                                                          static_cast<f64>(now_use.held_ns - was.held_ns) / 1e6,
                                                          static_cast<f64>(now_use.ready_ns - was.ready_ns) / 1e6);
                        threads += "\n";
                    }
                    nx::logi(logs::display, "frames: a hitch of {:.1f} ms (the median {:.1f}){}",
                             static_cast<f64>(ns - last_) / 1e6, static_cast<f64>(median) / 1e6,
                             threads.empty() ? "" : "; the threads since the frame before:");
                    // (a line each: a log line has a length it is cut at)
                    for (usize at = 0; at < threads.size();) {
                        usize end = threads.view().find('\n', at);
                        if (end == nx::string_view::npos) end = threads.size();
                        for (usize piece = at; piece < end; piece += 800)
                            nx::logi(logs::display, "frames: {}", threads.view().substr(piece, nx::min<usize>(800, end - piece)));
                        at = end + 1;
                    }
                }
            }
            period_.push_back(ns - last_);
            if (runtime::hle::tracing()) {
                if (runtime::Runtime* rt = runtime::current_runtime()) cpu_before_ = rt->kernel().cpu_use();
            }
            if (ns - log_from_ >= log_period_) log_summary(ns);
        }
    }
    last_ = ns;
}

void FrameStats::log_summary(u64 now) {
    nx::vector<u64> sorted = period_;
    std::sort(sorted.begin(), sorted.end());
    const auto ms = [](u64 ns) { return static_cast<f64>(ns) / 1e6; };
    const u64 median = sorted[sorted.size() / 2];
    const u64 p99 = sorted[nx::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    usize hitches = 0;
    for (u64 t : period_) hitches += t * 2 >= median * 3;
    const f64 seconds = static_cast<f64>(now - log_from_) / 1e9;
    nx::logi(logs::display, "frames: {} in {:.1f} s ({:.1f} a second); frame times: median {:.1f} ms, 99% {:.1f}, worst {:.1f}; "
             "{} hitches (1.5 times the median or more)", period_.size(), seconds,
             static_cast<f64>(period_.size()) / seconds, ms(median), ms(p99), ms(sorted.back()), hitches);
    period_.clear();
    log_from_ = now;
}

void FrameStats::atlas(nx::vector<u8>& rgba, u32& width, u32& height) {
    width = kAtlasWidth, height = kAtlasHeight;
    rgba.assign(usize{width} * height * 4, 0);
    auto set = [&](u32 x, u32 y) {
        u8* p = &rgba[(usize{y} * width + x) * 4];
        p[0] = p[1] = p[2] = p[3] = 255;
    };
    for (usize g = 0; g < kGlyphs; ++g)
        for (u32 y = 0; y < 7; ++y)
            for (u32 x = 0; x < 5; ++x)
                if (kFont[g].rows[y] & (0x10 >> x)) set(static_cast<u32>(g) * kCell + x, y);
    for (u32 y = 0; y < kCell; ++y)
        for (u32 x = 0; x < kCell; ++x) set(static_cast<u32>(kGlyphs) * kCell + x, y);
}

void FrameStats::quads(u32 width, u32 height, nx::vector<Quad>& out) const {
    (void)width;
    // (one pixel of the font this many of the window's: the overlay as large on a phone as on a desktop's window)
    const f32 s = std::max(2.0f, std::floor(static_cast<f32>(height) / 272.0f));
    const f32 fw = 1.0f / kAtlasWidth, white_u = (static_cast<f32>(kGlyphs) * kCell + kCell / 2) * fw;
    auto rect = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a) {
        Quad q;
        q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
        q.u0 = q.u1 = white_u, q.v0 = q.v1 = 0.5f;
        q.r = r, q.g = g, q.b = b, q.a = a;
        out.push_back(q);
    };
    auto text = [&](f32 x, f32 y, nx::string_view t, f32 r, f32 g, f32 b) {
        for (char c : t) {
            if (const i32 i = glyph_of(c); i >= 0 && c != ' ') {
                Quad q;
                q.x0 = x, q.y0 = y, q.x1 = x + 5 * s, q.y1 = y + 7 * s;
                q.u0 = static_cast<f32>(i * kCell) * fw, q.u1 = static_cast<f32>(i * kCell + 5) * fw;
                q.v0 = 0, q.v1 = 7.0f / kAtlasHeight;
                q.r = r, q.g = g, q.b = b, q.a = 1;
                out.push_back(q);
            }
            x += 6 * s;
        }
    };

    // The numbers, over the last second's frames.
    u64 sum = 0, worst = 0;
    usize n = 0;
    const u64 last_at = count_ ? at_[(next_ + kHistory - 1) % kHistory] : 0;
    for (usize k = 0; k < count_; ++k) {
        const usize i = (next_ + kHistory - 1 - k) % kHistory;
        if (n && last_at - at_[i] > 1'000'000'000) break;
        sum += times_[i], worst = nx::max(worst, times_[i]), ++n;
    }
    const f32 avg = n ? static_cast<f32>(sum) / 1e6f / static_cast<f32>(n) : 0;
    const f32 fps = n && sum ? static_cast<f32>(n) * 1e9f / static_cast<f32>(sum) : 0;

    const f32 x = 8 * s, y = 8 * s, pad = 3 * s;
    const f32 graph_w = static_cast<f32>(kShown) * s, graph_h = 40 * s;
    const f32 box_w = nx::max(graph_w, 22 * 6 * s) + 2 * pad, box_h = 2 * 9 * s + graph_h + 3 * pad;
    rect(x, y, x + box_w, y + box_h, 0, 0, 0, 0.6f);
    text(x + pad, y + pad, nx::format("FPS {:.1f}", fps).view(), 1, 1, 1);
    text(x + pad, y + pad + 9 * s, nx::format("AVG {:.1f} MAX {:.1f} MS", avg, static_cast<f32>(worst) / 1e6f).view(), 0.8f, 0.8f, 0.8f);

    // The graph: a bar a frame, the newest on the right, its height the frame's time (to 50 ms); lines at 16.7 and
    // 33.3 ms (60 and 30 frames a second).
    const f32 gx = x + pad, gy = y + 2 * pad + 18 * s, bottom = gy + graph_h;
    rect(gx, gy, gx + graph_w, bottom, 1, 1, 1, 0.08f);
    const usize shown = nx::min(count_, kShown);
    for (usize k = 0; k < shown; ++k) {
        const usize i = (next_ + kHistory - 1 - k) % kHistory;
        const f32 ms = static_cast<f32>(times_[i]) / 1e6f;
        const f32 h = std::min(ms, kGraphMs) / kGraphMs * graph_h;
        const f32 bx = gx + graph_w - static_cast<f32>(k + 1) * s;
        if (ms <= 34.5f) rect(bx, bottom - h, bx + s, bottom, 0.35f, 0.9f, 0.35f, 0.9f);
        else if (ms <= 51.0f) rect(bx, bottom - h, bx + s, bottom, 1.0f, 0.8f, 0.2f, 0.9f);
        else rect(bx, bottom - h, bx + s, bottom, 1.0f, 0.3f, 0.3f, 0.9f);
    }
    for (f32 ms : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        const f32 ly = bottom - ms / kGraphMs * graph_h;
        rect(gx, ly, gx + graph_w, ly + std::max(1.0f, s / 2), 1, 1, 1, 0.35f);
    }
}

}  // namespace psvrecore::platform
