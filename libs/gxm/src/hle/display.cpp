// The display (SceDisplay): the frame buffer shown, and vertical blanks at the display's refresh rate on the host
// clock. Also the thread of GXM's display queue, which calls the game's display callback for each queued frame.

#include <chrono>
#include <cmath>
#include <cstring>

#include "psvrecore/log.hpp"
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
    Frame frame{f.base, f.pitch, f.pixel_format, f.width, f.height, vcount};
    nx::vector<u8> scaled;
    if (state().options->render_scale > 1) {
        // (on the GPU when the sink takes it so: no readback, nor the CPU's copies)
        if (scaled_frame_to(f, *sink, &frame.scaled_width, &frame.scaled_height)) frame.gpu = true;
        else if (scaled_frame(f, scaled, &frame.scaled_width, &frame.scaled_height)) frame.scaled = scaled.data();
    }
    sink->show(frame);
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
    finish_scenes(t_frame_scenes);  // (the frame drawn into guest memory before it is shown)
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
    HLE_FUNCTION(0xB6FDE0BA, sceDisplayGetVcount),
    HLE_FUNCTION(0xA08CA60D, sceDisplayGetRefreshRate),
    HLE_FUNCTION(0xF51523CB, sceDisplaySetFrameBuf),  // (_sceDisplaySetFrameBuf)
    HLE_FUNCTION(0xA753B0CA, sceDisplayGetFrameBuf),  // (_sceDisplayGetFrameBuf)
};
const runtime::hle::Function kDisplayUserFunctions[] = {
    HLE_FUNCTION(0x7A410B64, sceDisplaySetFrameBuf),
    HLE_FUNCTION(0x42AE6BBC, sceDisplayGetFrameBuf),
    HLE_FUNCTION(0x8848ADDD, sceDisplayGetMaximumFrameBufResolution),
};

// ---- The display queue thread ----------------------------------------------------------------------------------

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
        finish_scenes(entry.scenes);
        if (s.init.display_queue_callback) {
            const u32 args[] = {entry.data};
            t_frame_scenes = entry.scenes;
            runtime::hle::call_guest(&runtime::Kernel::current()->ctx, s.init.display_queue_callback, args);
            t_frame_scenes = ~0ull;
        }
        {
            nx::scoped_lock<nx::mutex> held(s.queue_lock);
            s.queue_data_free.push_back(entry.data);
            if (auto it = s.display_holds.find(entry.old_buffer); it != s.display_holds.end() && it->second)
                if (--it->second == 0) s.display_holds.erase(it);
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
    s.display_thread = nx::thread([] { display_queue_main(); });
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
    runtime::hle::register_library({"SceDisplay", kSceDisplay, kDisplayFunctions});
    runtime::hle::register_library({"SceDisplayUser", kSceDisplayUser, kDisplayUserFunctions});
}

}  // namespace psvrecore::gxm::hle
