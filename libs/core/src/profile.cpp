// The profiler's lanes and the game's markers (Tracy; built only with PSVRECORE_TRACY=ON).

#include "psvrecore/profile.hpp"

#if PSVRECORE_TRACY

#include <cstring>

#include <tracy/TracyC.h>

#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::profile {

struct Track {
    const char* name = nullptr;  // (Tracy identifies a fiber by this pointer: never freed)
    nx::string base;             // (the guest's name it was made for)
    bool in_use = false;
    nx::vector<TracyCZoneCtx> markers;  // (the game's markers open in it, innermost last)
};

namespace {

struct Tracks {
    nx::mutex lock;
    nx::vector<Track*> all;  // (kept for the process)
};
Tracks& tracks() {
    static Tracks* t = new Tracks;  // (never destroyed: host threads may still use their lanes at exit)
    return *t;
}

// What the calling host thread runs, and the connection it last told the profiler of that in.
struct ThreadState {
    Track* track = nullptr;
    u64 connection = 0;
    nx::vector<TracyCZoneCtx> markers;  // (the game's markers on a thread that runs no lane)
};
thread_local ThreadState t_state;

const char* keep(nx::string_view text) {
    char* p = new char[text.size() + 1];
    std::memcpy(p, text.data(), text.size());
    p[text.size()] = 0;
    return p;
}

nx::vector<TracyCZoneCtx>& markers() { return t_state.track ? t_state.track->markers : t_state.markers; }

}  // namespace

Track* acquire(nx::string_view name) {
    if (name.empty()) name = "(unnamed)";
    Tracks& ts = tracks();
    nx::scoped_lock<nx::mutex> held(ts.lock);
    u32 same = 0;
    for (Track* t : ts.all) {
        if (t->base != name) continue;
        ++same;
        if (!t->in_use) {
            t->in_use = true;
            return t;
        }
    }
    auto* t = new Track;
    t->base = nx::string(name);
    t->name = same ? keep(nx::format("{} {}", name, same + 1)) : keep(name);
    t->in_use = true;
    ts.all.push_back(t);
    return t;
}

void release(Track* track) {
    if (!track) return;
    Tracks& ts = tracks();
    nx::scoped_lock<nx::mutex> held(ts.lock);
    // (markers the guest left open end with it, on the thread running it; from another thread they are only
    // dropped: Tracy refuses a zone ended on another thread than its own)
    while (!track->markers.empty()) {
        if (t_state.track == track) ___tracy_emit_zone_end(track->markers.back());
        track->markers.pop_back();
    }
    track->in_use = false;
}

void enter(Track* track) {
    ThreadState& s = t_state;
    if (s.track == track) return;
    s.track = track;
    const tracy::Profiler& p = tracy::GetProfiler();
    if (!p.IsConnected()) {
        s.connection = 0;
        return;
    }
    if (track) TracyFiberEnter(track->name);
    else TracyFiberLeave;
    s.connection = p.ConnectionId();
}

Track* current() { return t_state.track; }

void announce() {
    ThreadState& s = t_state;
    if (!s.track) return;
    const tracy::Profiler& p = tracy::GetProfiler();
    if (!p.IsConnected()) return;
    const u64 id = p.ConnectionId();
    if (s.connection == id) return;
    TracyFiberEnter(s.track->name);
    s.connection = id;
}

bool connected() { return TracyIsConnected; }

void push(nx::string_view label, u32 rgb) {
    if (!TracyIsConnected) {
        markers().push_back(TracyCZoneCtx{});  // (inactive: its end is nothing)
        return;
    }
    announce();
    static constexpr char kFile[] = "guest";
    static constexpr char kFunction[] = "(game marker)";
    const u64 srcloc = ___tracy_alloc_srcloc_name(0, kFile, sizeof kFile - 1, kFunction, sizeof kFunction - 1,
                                                  label.data(), label.size(), rgb);
    markers().push_back(___tracy_emit_zone_begin_alloc(srcloc, 1));
}

void begin(const tracy::SourceLocationData* where) {
    announce();
    // (the C interface's source location is laid out as the C++ one)
    markers().push_back(___tracy_emit_zone_begin(reinterpret_cast<const ___tracy_source_location_data*>(where),
                                                 TracyIsConnected ? 1 : 0));
}

bool pop() {
    auto& open = markers();
    if (open.empty()) return false;
    announce();
    ___tracy_emit_zone_end(open.back());
    open.pop_back();
    return true;
}

void message(nx::string_view text, u32 rgb) {
    announce();
    if (rgb) TracyMessageC(text.data(), text.size(), rgb);
    else TracyMessage(text.data(), text.size());
}

}  // namespace psvrecore::profile

#endif
