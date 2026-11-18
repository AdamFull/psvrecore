// The display (SceDisplay): the frame buffer shown, and vertical blanks at the display's refresh rate on the host
// clock. Also the thread of GXM's display queue, which calls the game's display callback for each queued frame.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/profile.hpp"
#include "state.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceDisplay = 0x5ED8F994, kSceDisplayUser = 0x4FAACD11;
constexpr i32 kErrorInvalidValue = static_cast<i32>(0x80290001u), kErrorInvalidAddr = static_cast<i32>(0x80290002u),
              kErrorInvalidPixelFormat = static_cast<i32>(0x80290003u), kErrorInvalidPitch = static_cast<i32>(0x80290004u),
              kErrorInvalidResolution = static_cast<i32>(0x80290005u),
              kErrorInvalidUpdateTiming = static_cast<i32>(0x80290006u);

struct Display {
    nx::mutex lock;
    DisplayFrameBuf current{};
    bool shown = false;
    // A frame set for the next vertical blank, given to the sink once that blank has come (when the game next waits
    // for a blank or sets a frame): what is drawn into it meanwhile is shown, as the console scans it out then.
    bool pending = false;
    u64 pending_vcount = 0;
};

Display& display() {
    static Display d;
    return d;
}

f64 period_ns() { return 1e9 / runtime::runtime().config().refresh_hz; }

// (the kernel's clock: vertical blanks count from the process's start, the epoch every guest-visible time shares)
u64 now_ns() { return runtime::runtime().kernel().clock_ns(); }

u64 vcount_at(u64 t) { return static_cast<u64>(static_cast<f64>(t) / period_ns()); }

void show(DisplaySink* sink, const DisplayFrameBuf& f, u64 vcount) {
    PSV_PROFILE_ZONE("display: frame shown");
    Frame frame{f.base, f.pitch, f.pixel_format, f.width, f.height, vcount};
    nx::vector<u8> scaled;
    if (state().options->render_scale > 1) {
        // (on the GPU when the sink takes it so: no readback, nor the CPU's copies)
        if (scaled_frame_to(f, *sink, &frame.scaled_width, &frame.scaled_height)) frame.gpu = true;
        else if (scaled_frame(f, scaled, &frame.scaled_width, &frame.scaled_height)) frame.scaled = scaled.data();
    }
    sink->show(frame);
    profile::frame();  // (a frame displayed: Tracy's frame mark)
}

// The frame set for a blank that has come, to the sink.
void latch() {
    DisplaySink* sink = state().options ? state().options->display : nullptr;
    if (!sink) return;
    Display& d = display();
    DisplayFrameBuf f;
    u64 vcount;
    {
        nx::scoped_lock<nx::mutex> held(d.lock);
        if (!d.pending || vcount_at(now_ns()) < d.pending_vcount) return;
        f = d.current;
        vcount = d.pending_vcount;
        d.pending = false;
    }
    show(sink, f, vcount);
}

// Waits (giving up the guest CPU) until `count` more vertical blanks have started.
void wait_vblanks(u32 count, bool callbacks) {
    const u64 now = now_ns();
    const u64 target = vcount_at(now) + count;
    const u64 at = static_cast<u64>(std::ceil(static_cast<f64>(target) * period_ns()));
    {
        runtime::Kernel::Blocking blocking(runtime::runtime().kernel());
        while (true) {
            const u64 t = now_ns();
            if (t >= at) break;
            nx::this_thread::sleep_for(std::chrono::nanoseconds(at - t));
        }
    }
    latch();
    if (callbacks) runtime::runtime().kernel().check_callbacks();
}

i32 set_frame_buf(Ptr<const DisplayFrameBuf> p, i32 sync) {
    // (a frame set by the game itself, not from the display queue's callback: its scenes all ended)
    const u64 frame_scenes = g_frame_scenes.get();
    if (frame_scenes == ~0ull && p) frame_boundary();
    finish_scenes(frame_scenes);  // (the frame drawn into guest memory before it is shown)
    if (sync != 0 && sync != 1) return kErrorInvalidUpdateTiming;  // (immediate, next frame)
    Display& d = display();
    latch();  // (one set before, whose blank has come)
    if (!p) {  // (no frame buffer: the display shows black)
        nx::scoped_lock<nx::mutex> held(d.lock);
        d.shown = false;
        d.pending = false;
        return 0;
    }
    const DisplayFrameBuf f = *p;
    if (f.size != sizeof(DisplayFrameBuf)) return kErrorInvalidValue;
    if (!f.base || (f.base & 0xFF)) return kErrorInvalidAddr;
    if (f.pixel_format != 0 && f.pixel_format != 0x50000000u) return kErrorInvalidPixelFormat;  // A8B8G8R8, A2B10G10R10
    if (f.pitch < f.width) return kErrorInvalidPitch;
    if (!f.width || !f.height || f.width > 960 || f.height > 544) return kErrorInvalidResolution;
    if (!runtime::runtime().memory().valid(f.base, f.pitch * f.height * 4)) return kErrorInvalidAddr;
    const u64 vcount = vcount_at(now_ns());
    {
        nx::scoped_lock<nx::mutex> held(d.lock);
        d.current = f;
        d.shown = true;
        // (at the next blank; a frame set again before it replaces this one, as the console latches the last)
        d.pending = sync == 1;
        d.pending_vcount = vcount + 1;
    }
    if (sync == 0) {
        if (DisplaySink* sink = state().options->display) show(sink, f, vcount);
    }
    return 0;
}

i32 sceDisplaySetFrameBuf(Ptr<const DisplayFrameBuf> p, i32 sync) { return set_frame_buf(p, sync); }

i32 sceDisplayGetFrameBuf(Ptr<DisplayFrameBuf> p, i32 sync) {
    if (!p) return kErrorInvalidValue;
    if (sync != 0 && sync != 1) return kErrorInvalidUpdateTiming;
    if (p->size != sizeof(DisplayFrameBuf)) return kErrorInvalidValue;
    Display& d = display();
    nx::scoped_lock<nx::mutex> held(d.lock);
    if (!d.shown) {
        std::memset(p.get(), 0, sizeof(DisplayFrameBuf));
        p->size = sizeof(DisplayFrameBuf);
        return 0;
    }
    *p = d.current;
    return 0;
}

i32 sceDisplayWaitVblankStart() {
    wait_vblanks(1, false);
    return 0;
}
i32 sceDisplayWaitVblankStartCB() {
    wait_vblanks(1, true);
    return 0;
}
i32 sceDisplayWaitVblankStartMulti(u32 count) {
    wait_vblanks(count ? count : 1, false);
    return 0;
}
i32 sceDisplayWaitVblankStartMultiCB(u32 count) {
    wait_vblanks(count ? count : 1, true);
    return 0;
}
// (a frame buffer takes effect at the next vertical blank at the latest: waiting for it is waiting for that blank)
i32 sceDisplayWaitSetFrameBuf() { return sceDisplayWaitVblankStart(); }
// (the frame set has been shown for `count` blanks: as many blanks as that)
i32 sceDisplayWaitSetFrameBufMulti(u32 count) { return sceDisplayWaitVblankStartMulti(count); }
i32 sceDisplayWaitSetFrameBufCB() { return sceDisplayWaitVblankStartCB(); }

i32 sceDisplayWaitSetFrameBufMultiCB(u32 count) { return sceDisplayWaitVblankStartMultiCB(count); }

// Vertical blank callbacks: each registered callback is notified at every blank, by a thread of the runtime's (as
// the display driver notifies them); it runs when its thread checks callbacks (a CB wait).
struct VblankCallbacks {
    nx::mutex lock;
    nx::vector<i32> uids;
    bool stop = false;
    nx::thread thread;
};
VblankCallbacks& vblank_callbacks() {
    static VblankCallbacks v;
    return v;
}

void vblank_main() {
    VblankCallbacks& v = vblank_callbacks();
    runtime::Kernel& k = runtime::runtime().kernel();
    k.adopt_host_thread("SceDisplayVblank", 64);
    try {
        u64 next = vcount_at(now_ns()) + 1;
        while (true) {
            nx::vector<i32> uids;
            {
                nx::scoped_lock<nx::mutex> held(v.lock);
                if (v.stop || v.uids.empty()) break;
            }
            const u64 at = static_cast<u64>(std::ceil(static_cast<f64>(next) * period_ns()));
            if (const u64 t = now_ns(); t < at) k.delay_thread(static_cast<u32>((at - t + 999) / 1000), false);
            const u64 vcount = vcount_at(now_ns());
            if (vcount < next) continue;
            next = vcount + 1;
            {
                nx::scoped_lock<nx::mutex> held(v.lock);
                uids = v.uids;
            }
            for (i32 uid : uids) k.notify_callback(uid, 0);
        }
    } catch (...) {
        // (the kernel shutting down ends the thread's wait)
    }
    k.release_host_thread();
}

// (the thread ended, waited for without the guest CPU the caller holds)
void stop_vblank_thread() {
    VblankCallbacks& v = vblank_callbacks();
    nx::thread ended;
    {
        nx::scoped_lock<nx::mutex> held(v.lock);
        v.stop = true;
        ended = std::move(v.thread);
    }
    if (ended.joinable()) {
        if (runtime::Kernel::current()) {
            runtime::Kernel::Blocking blocking(runtime::runtime().kernel());
            ended.join();
        } else {
            ended.join();
        }
    }
}

i32 sceDisplayRegisterVblankStartCallback(i32 uid) {
    if (runtime::runtime().kernel().callback_count(uid) < 0) return kErrorInvalidValue;
    VblankCallbacks& v = vblank_callbacks();
    bool start = false;
    {
        nx::scoped_lock<nx::mutex> held(v.lock);
        if (std::find(v.uids.begin(), v.uids.end(), uid) != v.uids.end()) return kErrorInvalidValue;
        v.uids.push_back(uid);
        start = v.uids.size() == 1;
    }
    if (start) {
        stop_vblank_thread();  // (one that ended, joined)
        nx::scoped_lock<nx::mutex> held(v.lock);
        v.stop = false;
        nx::thread_options options;
        options.name = "display vblank callbacks";
        v.thread = nx::thread([] { vblank_main(); }, options);
    }
    return 0;
}
i32 sceDisplayUnregisterVblankStartCallback(i32 uid) {
    VblankCallbacks& v = vblank_callbacks();
    bool last = false;
    {
        nx::scoped_lock<nx::mutex> held(v.lock);
        auto it = std::find(v.uids.begin(), v.uids.end(), uid);
        if (it == v.uids.end()) return kErrorInvalidValue;
        v.uids.erase(it);
        last = v.uids.empty();
    }
    if (last) stop_vblank_thread();
    return 0;
}

// The output's resolution (the LCD's: 960x544 at the refresh rate), for display head 0 (the main display).
struct ResolutionInfo {
    u32 size, screen_mode, width, height, output_format, scan_mode;
    f32 fps;
};
static_assert(sizeof(ResolutionInfo) == 28);
i32 sceDisplayGetResolutionInfoInternal(i32 head, Ptr<ResolutionInfo> info, Ptr<const u32> opt) {
    if (!info || !opt || !runtime::runtime().memory().valid(info.address, sizeof(ResolutionInfo), true)) return kErrorInvalidValue;
    if (head != 0 || info->size != sizeof(ResolutionInfo)) return kErrorInvalidValue;
    ResolutionInfo r{sizeof(ResolutionInfo), 0, 960, 544, 0, 0, static_cast<f32>(runtime::runtime().config().refresh_hz)};
    *info = r;
    return 0;
}

i32 sceDisplayGetVcount() { return static_cast<i32>(vcount_at(now_ns()) & 0xFFFF); }

i32 sceDisplayGetRefreshRate(Ptr<f32> hz) {
    if (!hz) return kErrorInvalidValue;
    *hz = static_cast<f32>(runtime::runtime().config().refresh_hz);
    return 0;
}

i32 sceDisplayGetMaximumFrameBufResolution(Ptr<i32> width, Ptr<i32> height) {
    if (width) *width = 960;
    if (height) *height = 544;
    return 0;
}

const runtime::hle::Function kDisplayFunctions[] = {
    HLE_FUNCTION(0x5795E898, sceDisplayWaitVblankStart),
    HLE_FUNCTION(0x78B41B92, sceDisplayWaitVblankStartCB),
    HLE_FUNCTION(0xDD0A13B8, sceDisplayWaitVblankStartMulti),
    HLE_FUNCTION(0x05F27764, sceDisplayWaitVblankStartMultiCB),
    HLE_FUNCTION(0x9423560C, sceDisplayWaitSetFrameBuf),
    HLE_FUNCTION(0x7D9864A8, sceDisplayWaitSetFrameBufMulti),
    HLE_FUNCTION(0x814C90AF, sceDisplayWaitSetFrameBufCB),
    HLE_FUNCTION(0x3E796EF5, sceDisplayWaitSetFrameBufMultiCB),
    HLE_FUNCTION(0x6BDF4C4D, sceDisplayRegisterVblankStartCallback),
    HLE_FUNCTION(0x98436A80, sceDisplayUnregisterVblankStartCallback),
    HLE_FUNCTION(0xB6FDE0BA, sceDisplayGetVcount),
    HLE_FUNCTION(0xA08CA60D, sceDisplayGetRefreshRate),
    HLE_FUNCTION(0xF51523CB, sceDisplaySetFrameBuf),  // (_sceDisplaySetFrameBuf)
    HLE_FUNCTION(0xA753B0CA, sceDisplayGetFrameBuf),  // (_sceDisplayGetFrameBuf)
};
const runtime::hle::Function kDisplayUserFunctions[] = {
    HLE_FUNCTION(0x7A410B64, sceDisplaySetFrameBuf),
    HLE_FUNCTION(0x42AE6BBC, sceDisplayGetFrameBuf),
    HLE_FUNCTION(0x8848ADDD, sceDisplayGetMaximumFrameBufResolution),
    HLE_FUNCTION(0x0BFB78C6, sceDisplayGetResolutionInfoInternal),
};

// ---- The display queue thread ----------------------------------------------------------------------------------

// (PSVRECORE_FRAME_STATS: for a frame shown late, a frame time over 1.5 of 33 ms after the one before, why: the
// game queued it late, or its scenes finished late after it was queued)
void late_frame_why(const DisplayEntry& e, u64 finishing) {
    static const bool on = [] {
        const char* v = std::getenv("PSVRECORE_FRAME_STATS");
        return v && *v;
    }();
    if (!on) return;
    static u64 last_done = 0;
    const u64 done = nx::os::ticks_ns();
    const u64 interval = last_done ? done - last_done : 0;
    last_done = done;
    if (interval < 50'000'000) return;
    nx::logi(logs::display, "frames: a late one, {:.1f} ms after the one before: the game queued it {:.1f} ms after "
             "the one before; the queue took it {:.1f} ms after, its scenes then done in {:.1f} ms (the renderer's lock "
             "{:.1f}, the GPU {:.1f}, writing back {:.1f})",
             static_cast<f64>(interval) / 1e6, static_cast<f64>(e.since_last_ns) / 1e6,
             static_cast<f64>(finishing - e.queued_ns) / 1e6, static_cast<f64>(done - finishing) / 1e6,
             static_cast<f64>(g_finish_times.get().lock_ns) / 1e6, static_cast<f64>(g_finish_times.get().gpu_ns) / 1e6,
             static_cast<f64>(g_finish_times.get().finish_ns) / 1e6);
}

void display_queue_main() {
    State& s = state();
    runtime::Runtime& rt = runtime::runtime();
    rt.kernel().adopt_host_thread("SceGxmDisplayQueue", 64);  // (a high priority, as libgxm's thread has)
    while (true) {
        DisplayEntry entry;
        {
            runtime::Kernel::Blocking blocking(rt.kernel());
            nx::scoped_lock<nx::mutex> held(s.queue_lock);
            s.queue_changed.wait(s.queue_lock, [&] { return s.stopping || !s.queue.empty(); });
            if (s.stopping) break;
            entry = s.queue.front();
            s.queue.erase(s.queue.begin());
            s.queue_busy = 1;
        }
        // (the new buffer complete in guest memory: the frame's scenes the GPU still has finished, here rather than
        // on the game's thread, which goes on meanwhile; not the next frame's, which it may have begun)
        const u64 finishing = nx::os::ticks_ns();
        finish_scenes(entry.scenes);
        late_frame_why(entry, finishing);
        if (s.init.display_queue_callback) {
            const u32 args[] = {entry.data};
            g_frame_scenes.get() = entry.scenes;
            runtime::hle::call_guest(&runtime::Kernel::current()->ctx, s.init.display_queue_callback, args);
            g_frame_scenes.get() = ~0ull;
        }
        {
            nx::scoped_lock<nx::mutex> held(s.queue_lock);
            s.queue_data_free.push_back(entry.data);
            // (the old buffer released: drawn into again from now on; a frame shown again (the old buffer the new
            // one) releases its own hold with it, as the console's flip signals the buffer it leaves)
            for (u32 k = entry.old_buffer == entry.new_buffer ? 2 : 1; k; --k)
                if (auto it = s.display_holds.find(entry.old_buffer); it != s.display_holds.end() && it->second) {
                    if (--it->second == 0) s.display_holds.erase(it);
                    ++s.holds_released[entry.old_buffer];
                }
            s.queue_busy = 0;
        }
        s.queue_changed.notify_all();
    }
    rt.kernel().release_host_thread();
}

}  // namespace

void start_display_queue() {
    State& s = state();
    s.stopping = false;
    // (high on the host: it finishes the frame's scenes, writing them back under the renderer's lock, on time for the
    // blank; not left on a slow core meanwhile)
    nx::thread_options options;
    options.name = "gxm display queue";
    options.priority = nx::thread_priority::High;
    s.display_thread = nx::thread([] { display_queue_main(); }, options);
}

void stop_display_queue() {
    State& s = state();
    {
        nx::scoped_lock<nx::mutex> held(s.queue_lock);
        s.stopping = true;
    }
    s.queue_changed.notify_all();
    if (s.display_thread.joinable()) s.display_thread.join();
}

void register_display() {
    {
        // (a new runtime: callbacks of the one before forgotten, its thread ended with its kernel)
        VblankCallbacks& v = vblank_callbacks();
        nx::scoped_lock<nx::mutex> held(v.lock);
        v.uids.clear();
    }
    stop_vblank_thread();
    runtime::hle::register_library({"SceDisplay", kSceDisplay, kDisplayFunctions});
    runtime::hle::register_library({"SceDisplayUser", kSceDisplayUser, kDisplayUserFunctions});
}

}  // namespace psvrecore::gxm::hle
