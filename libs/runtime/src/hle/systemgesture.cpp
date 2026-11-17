// The system's touch gestures (SceSystemGesture): primitive touch events made from the touch panels' data the game
// passes in, and gesture recognizers (tap, drag, tap and hold, pinch out/in, rotation) that turn them into events.
//
// Primitive touches go Begin (pressed), Active (still pressed), End (released: reported once), and Cancelled when
// the game's updates are more than 200 ms apart. A recognizer looks at the touches of its panel that start inside
// its rectangle (the whole panel by default) and keeps up to 6 events:
// - tap: pressed and released within kTapTime and kTapRange of where it was pressed; reported Active for one update.
//   With a tap count above 1, taps that follow within kTapInterval and kTapRange of the first count together, and the
//   event (at the first tap's place) comes when the count is reached or the interval passes;
// - drag: Begin when pressed, Active while it is, End when released or outside the rectangle;
// - tap and hold: Begin once held for the set time (1000 ms by default) within kTapRange, Active while it stays,
//   End when released or moved away;
// - pinch out/in and rotation: two touches pressed inside the rectangle, Begin when the second is, Active while both
//   are, End when either is released or leaves; the scale and angle are from when they were paired. A touch beyond
//   the two is ignored.
// The distances and times are this layer's choice (the system's are not documented): kTapRange is in panel units
// (front 1920 x 1088), about 25 screen pixels.
//
// Events that ended (End, Cancelled, and taps) are reported by the update that made them and gone after the next.

#include "hle_common.hpp"

#include <cmath>
#include <cstring>
#include "core/foundation/containers/hash_map.h"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceSystemGesture = 0xAF3FE2D3;
constexpr i32 kInvalidArgument = err::code(0x80890001), kNotSupported = err::code(0x80890002),
              kNotInitialized = err::code(0x80890003), kOutOfArray = err::code(0x80890004);
constexpr u32 kInactive = 0, kBegin = 1, kActive = 2, kEnd = 3, kCancelled = 4;
constexpr u32 kTap = 1, kDrag = 2, kTapAndHold = 4, kPinchOutIn = 8, kRotation = 16;
constexpr i32 kTapRange = 50;
constexpr u64 kTapTime = 500'000, kTapInterval = 300'000, kHoldTime = 1'000'000, kCancelGap = 200'000;  // (us)
constexpr usize kMaxEvents = 6;

struct Vec2 {
    i16 x = 0, y = 0;
};

// SceTouchData (touch.cpp's)
struct TouchReport {
    u8 id, force;
    i16 x, y;
    u8 reserved[8];
    u16 info;
};
struct TouchData {
    u64 timestamp;
    u32 status;
    u32 report_count;
    TouchReport reports[8];
};
static_assert(sizeof(TouchData) == 144);

struct Primitive {
    u16 id = 0;  // panel << 8 | report id
    u32 state = kInactive;
    Vec2 pressed, current, delta;
    u64 pressed_time = 0, delta_time = 0, elapsed = 0;
};

// SceSystemGestureTouchEvent: id, state, type, (4), updated time, the type's property (64 bytes), (56).
struct Event {
    u32 id = 0, state = 0, type = 0;
    u64 time = 0;
    u8 property[64] = {};
};
constexpr u32 kEventSize = 144;

struct Rect {
    i32 x = 0, y = 0, w = 0, h = 0;  // (w == 0: the whole panel)
    bool contains(Vec2 p) const { return !w || (p.x >= x && p.x < x + w && p.y >= y && p.y < y + h); }
};

struct Recognizer {
    u32 type = 0;
    u8 port = 0;
    Rect rect;
    u32 max_taps = 1;
    u64 hold_time = kHoldTime;
    u32 next_id = 1;
    nx::vector<Event> events;
    // tap: touches that may still be taps; taps waiting for the next of a count
    struct Press {
        u16 id;
        Vec2 at;
        u64 time;
    };
    nx::vector<Press> presses;
    bool pending = false;
    Vec2 pending_at;
    u16 pending_id = 0;
    u32 pending_count = 0;
    u64 pending_time = 0;
    // drag and hold: the event of each touch (its id), and where and when the touch started
    struct Tracked {
        u16 primitive;
        u32 event;  // (0: none yet)
        Vec2 at;
        u64 time;
    };
    nx::vector<Tracked> tracked;
    // pinch and rotation: the two touches, where they were paired
    u16 pair[2] = {};
    u32 pairs = 0, pair_event = 0;
    Vec2 paired[2];
};

struct Gestures {
    nx::mutex lock;
    bool initialized = false;
    u64 last_update = 0;
    nx::vector<Primitive> primitives;
    nx::flat_hash_map<u32, Recognizer> recognizers;  // (by the guest structure's address)
};
Gestures& gestures() {
    static Gestures g;
    return g;
}

i32 distance2(Vec2 a, Vec2 b) {
    const i32 dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}
bool near(Vec2 a, Vec2 b) { return distance2(a, b) <= kTapRange * kTapRange; }

void put16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
void put_vec(u8* p, Vec2 v) { std::memcpy(p, &v, 4); }

const Primitive* primitive(const Gestures& g, u16 id) {
    for (const Primitive& p : g.primitives)
        if (p.id == id) return &p;
    return nullptr;
}

i32 sceSystemGestureInitializePrimitiveTouchRecognizer(Ptr<const void> /*parameter*/) {
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    g.initialized = true;
    g.last_update = 0;
    g.primitives.clear();
    return 0;
}

// A panel's reports as primitive touches: those pressed at the last update continue (unless `fresh`).
void read_panel(const Gestures& g, u32 port, u32 address, bool fresh, u64& stamp, nx::vector<Primitive>& now) {
    if (!address || !memory().valid(address, sizeof(TouchData))) return;
    TouchData d;
    std::memcpy(&d, memory().host(address), sizeof d);
    stamp = nx::max(stamp, d.timestamp);
    for (u32 i = 0; i < nx::min<u32>(d.report_count, 8); ++i) {
        const TouchReport& r = d.reports[i];
        Primitive p;
        p.id = static_cast<u16>(port << 8 | r.id);
        p.current = {r.x, r.y};
        const Primitive* was = fresh ? nullptr : primitive(g, p.id);
        if (was && (was->state == kBegin || was->state == kActive)) {
            p.state = kActive;
            p.pressed = was->pressed;
            p.pressed_time = was->pressed_time;
            p.delta = {static_cast<i16>(p.current.x - was->current.x), static_cast<i16>(p.current.y - was->current.y)};
        } else {
            p.state = kBegin;
            p.pressed = p.current;
            p.pressed_time = d.timestamp;
        }
        now.push_back(p);
    }
}

i32 sceSystemGestureUpdatePrimitiveTouchRecognizer(Ptr<const TouchData> front, Ptr<const TouchData> back) {
    if (!front && !back) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    if (!g.initialized) return kNotInitialized;
    u64 stamp = 0;
    nx::vector<Primitive> probe;  // (only for the time stamp)
    read_panel(g, 0, front.address, true, stamp, probe);
    read_panel(g, 1, back.address, true, stamp, probe);
    // (updates too far apart: what was pressed is cancelled; a touch pressed again under the same id begins at the
    // next update)
    const bool fresh = g.last_update && stamp > g.last_update + kCancelGap;
    nx::vector<Primitive> now;
    read_panel(g, 0, front.address, fresh, stamp, now);
    read_panel(g, 1, back.address, fresh, stamp, now);
    // (released since the last update: End, once)
    for (const Primitive& was : g.primitives) {
        if (was.state != kBegin && was.state != kActive) continue;
        bool still = false;
        for (const Primitive& p : now) still |= p.id == was.id && p.state == kActive;
        if (still) continue;
        if (fresh) {
            nx::vector<Primitive> others;
            for (const Primitive& p : now)
                if (p.id != was.id) others.push_back(p);
            now = std::move(others);
        }
        Primitive e = was;
        e.state = fresh ? kCancelled : kEnd;
        e.delta = {};
        now.push_back(e);
    }
    for (Primitive& p : now) {
        p.delta_time = g.last_update && stamp > g.last_update ? stamp - g.last_update : 0;
        p.elapsed = stamp > p.pressed_time ? stamp - p.pressed_time : 0;
    }
    g.primitives = std::move(now);
    g.last_update = stamp;
    return 0;
}

Recognizer* recognizer(Gestures& g, u32 address) {
    auto it = g.recognizers.find(address);
    return it == g.recognizers.end() ? nullptr : &it->second;
}

i32 sceSystemGestureCreateTouchRecognizer(Ptr<u8> object, u32 type, u32 port, Ptr<const i16> rect, Ptr<const u8> parameter) {
    if (!object || !writable(object.address, 307 * 8) || port > 1) return kInvalidArgument;
    if (type != kTap && type != kDrag && type != kTapAndHold && type != kPinchOutIn && type != kRotation) return kNotSupported;
    Recognizer r;
    r.type = type;
    r.port = static_cast<u8>(port);
    if (rect && memory().valid(rect.address, 8)) {
        i16 v[4];
        std::memcpy(v, memory().host(rect.address), 8);
        r.rect = {v[0], v[1], v[2], v[3]};
    }
    if (parameter && memory().valid(parameter.address, 64)) {
        if (type == kTap) r.max_taps = nx::max<u32>(1, *memory().host(parameter.address));
        if (type == kTapAndHold) {
            u64 ms;
            std::memcpy(&ms, memory().host(parameter.address), 8);
            if (ms) r.hold_time = ms * 1000;
        }
    }
    std::memset(object.get(), 0, 307 * 8);
    std::memcpy(object.get(), &type, 4);  // (the structure is the system's own: only the type kept there)
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    g.recognizers[object.address] = std::move(r);
    return 0;
}

Event& add_event(Recognizer& r, u32 state, u64 time) {
    Event e;
    e.id = r.next_id++;
    if (!r.next_id) r.next_id = 1;
    e.state = state;
    e.type = r.type;
    e.time = time;
    r.events.push_back(e);
    return r.events.back();
}
Event* event_of(Recognizer& r, u32 id) {
    for (Event& e : r.events)
        if (e.id == id) return &e;
    return nullptr;
}

void emit_tap(Recognizer& r, u16 id, Vec2 at, u32 count, u64 time) {
    Event& e = add_event(r, kActive, time);
    put16(e.property, id);
    put_vec(e.property + 2, at);
    e.property[6] = static_cast<u8>(count);
}

void update_tap(Recognizer& r, const Gestures& g, u64 now) {
    for (const Primitive& p : g.primitives) {
        if ((p.id >> 8) != r.port) continue;
        if (p.state == kBegin && r.rect.contains(p.current)) r.presses.push_back({p.id, p.current, p.pressed_time});
        for (usize i = 0; i < r.presses.size(); ++i) {
            if (r.presses[i].id != p.id) continue;
            const bool moved = !near(p.current, r.presses[i].at);
            if (p.state == kCancelled || moved || now - r.presses[i].time > kTapTime) {
                r.presses.erase(r.presses.begin() + static_cast<isize>(i));
                break;
            }
            if (p.state != kEnd) break;
            const Vec2 at = r.presses[i].at;
            r.presses.erase(r.presses.begin() + static_cast<isize>(i));
            if (r.max_taps <= 1) {
                emit_tap(r, p.id, at, 1, now);
            } else if (r.pending && near(at, r.pending_at) && now - r.pending_time <= kTapInterval + kTapTime) {
                if (++r.pending_count >= r.max_taps) emit_tap(r, r.pending_id, r.pending_at, r.pending_count, now), r.pending = false;
                else r.pending_time = now;
            } else {
                if (r.pending) emit_tap(r, r.pending_id, r.pending_at, r.pending_count, now);
                r.pending = true, r.pending_at = at, r.pending_id = p.id, r.pending_count = 1, r.pending_time = now;
            }
            break;
        }
    }
    // (no next tap in time: the count so far)
    if (r.pending && r.presses.empty() && now - r.pending_time > kTapInterval) {
        emit_tap(r, r.pending_id, r.pending_at, r.pending_count, now);
        r.pending = false;
    }
}

void update_drag(Recognizer& r, const Gestures& g, u64 now) {
    for (const Primitive& p : g.primitives) {
        if ((p.id >> 8) != r.port) continue;
        Recognizer::Tracked* t = nullptr;
        for (auto& x : r.tracked)
            if (x.primitive == p.id) t = &x;
        if (!t) {
            if (p.state != kBegin || !r.rect.contains(p.current) || r.events.size() >= kMaxEvents) continue;
            Event& e = add_event(r, kBegin, now);
            r.tracked.push_back({p.id, e.id, p.pressed, p.pressed_time});
            t = &r.tracked.back();
        }
        Event* e = event_of(r, t->event);
        if (!e) continue;
        e->time = now;
        e->state = p.state == kCancelled ? kCancelled : (p.state == kEnd || !r.rect.contains(p.current)) ? kEnd
                 : p.state == kBegin                                                                 ? kBegin
                                                                                                      : kActive;
        put16(e->property, p.id);
        put_vec(e->property + 2, p.delta);
        put_vec(e->property + 6, p.current);
        put_vec(e->property + 10, t->at);
        if (e->state == kEnd || e->state == kCancelled) t->primitive = 0xFFFF;  // (done with it)
    }
}

void update_hold(Recognizer& r, const Gestures& g, u64 now) {
    for (const Primitive& p : g.primitives) {
        if ((p.id >> 8) != r.port) continue;
        Recognizer::Tracked* t = nullptr;
        for (auto& x : r.tracked)
            if (x.primitive == p.id) t = &x;
        if (!t) {
            if (p.state != kBegin || !r.rect.contains(p.current)) continue;
            r.tracked.push_back({p.id, 0, p.pressed, p.pressed_time});
            t = &r.tracked.back();
        }
        const bool away = !near(p.current, t->at) || p.state == kEnd || p.state == kCancelled;
        if (!t->event) {
            if (away) t->primitive = 0xFFFF;
            else if (now - t->time >= r.hold_time && r.events.size() < kMaxEvents) {
                Event& e = add_event(r, kBegin, now);
                put16(e.property, p.id);
                put_vec(e.property + 2, t->at);
                t->event = e.id;
            }
            continue;
        }
        Event* e = event_of(r, t->event);
        if (!e) continue;
        e->time = now;
        e->state = p.state == kCancelled ? kCancelled : away ? kEnd : kActive;
        if (e->state != kActive) t->primitive = 0xFFFF;
    }
}

void update_pair(Recognizer& r, const Gestures& g, u64 now) {
    // (the touches of this panel inside the rectangle, oldest first: the pair is the first two of them)
    auto alive = [&](u16 id) {
        const Primitive* p = primitive(g, id);
        return p && (p->state == kBegin || p->state == kActive) && r.rect.contains(p->current);
    };
    if (r.pairs == 2 && (!alive(r.pair[0]) || !alive(r.pair[1]))) {
        if (Event* e = event_of(r, r.pair_event)) {
            const Primitive* a = primitive(g, r.pair[0]);
            const Primitive* b = primitive(g, r.pair[1]);
            e->state = (a && a->state == kCancelled) || (b && b->state == kCancelled) ? kCancelled : kEnd;
            e->time = now;
        }
        // (the one still pressed waits for a new partner)
        const u16 keep = alive(r.pair[0]) ? r.pair[0] : alive(r.pair[1]) ? r.pair[1] : 0;
        r.pairs = keep ? 1 : 0;
        r.pair[0] = keep;
        r.pair_event = 0;
    }
    if (r.pairs == 1 && !alive(r.pair[0])) r.pairs = 0;
    for (const Primitive& p : g.primitives) {
        if ((p.id >> 8) != r.port || p.state != kBegin || !r.rect.contains(p.current) || r.pairs >= 2) continue;
        r.pair[r.pairs++] = p.id;
        if (r.pairs == 2 && r.events.size() < kMaxEvents) {
            r.paired[0] = primitive(g, r.pair[0])->current;
            r.paired[1] = p.current;
            r.pair_event = add_event(r, kBegin, now).id;
        }
    }
    if (r.pairs != 2) return;
    Event* e = event_of(r, r.pair_event);
    if (!e) return;
    const Primitive* a = primitive(g, r.pair[0]);
    const Primitive* b = primitive(g, r.pair[1]);
    if (e->state == kActive || (e->state == kBegin && e->time != now)) e->state = kActive;
    e->time = now;
    f32 value = 0;
    const f32 dx0 = static_cast<f32>(r.paired[1].x - r.paired[0].x), dy0 = static_cast<f32>(r.paired[1].y - r.paired[0].y);
    const f32 dx1 = static_cast<f32>(b->current.x - a->current.x), dy1 = static_cast<f32>(b->current.y - a->current.y);
    if (r.type == kPinchOutIn) {
        const f32 d0 = std::sqrt(dx0 * dx0 + dy0 * dy0);
        value = d0 > 0 ? std::sqrt(dx1 * dx1 + dy1 * dy1) / d0 : 1.0f;
    } else {
        value = std::atan2(dy1, dx1) - std::atan2(dy0, dx0);
    }
    std::memcpy(e->property, &value, 4);
    const Primitive* two[2] = {a, b};
    for (u32 k = 0; k < 2; ++k) {
        u8* q = e->property + 4 + 14 * k;
        put16(q, two[k]->id);
        put_vec(q + 2, two[k]->current);
        put_vec(q + 6, two[k]->delta);
        put_vec(q + 10, r.paired[k]);
    }
}

i32 sceSystemGestureUpdateTouchRecognizer(Ptr<u8> object) {
    if (!object) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    if (!g.initialized) return kNotInitialized;
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    // (what ended, and taps, were reported by the last update)
    nx::vector<Event> kept;
    for (const Event& e : r->events)
        if (e.state == kBegin || (e.state == kActive && r->type != kTap)) kept.push_back(e);
    r->events = std::move(kept);
    nx::vector<Recognizer::Tracked> tracked;
    for (const auto& t : r->tracked)
        if (t.primitive != 0xFFFF) tracked.push_back(t);
    r->tracked = std::move(tracked);
    const u64 now = g.last_update;
    switch (r->type) {
        case kTap: update_tap(*r, g, now); break;
        case kDrag: update_drag(*r, g, now); break;
        case kTapAndHold: update_hold(*r, g, now); break;
        default: update_pair(*r, g, now); break;
    }
    // (a drag or hold seen begun last time is active now)
    for (Event& e : r->events)
        if (e.state == kBegin && e.time != now) e.state = kActive;
    if (r->events.size() > kMaxEvents) r->events.erase(r->events.begin(), r->events.end() - kMaxEvents);
    return 0;
}

i32 sceSystemGestureGetTouchEventsCount(Ptr<const u8> object) {
    if (!object) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    return r ? static_cast<i32>(r->events.size()) : kInvalidArgument;
}

i32 sceSystemGestureGetTouchEventByIndex(Ptr<const u8> object, u32 index, Ptr<u8> out) {
    if (!object || !out || !writable(out.address, kEventSize)) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    if (index >= r->events.size()) return kOutOfArray;
    const Event& e = r->events[index];
    u8* p = out.get();
    std::memset(p, 0, kEventSize);
    std::memcpy(p, &e.id, 4);
    std::memcpy(p + 4, &e.state, 4);
    std::memcpy(p + 8, &e.type, 4);
    std::memcpy(p + 16, &e.time, 8);
    std::memcpy(p + 24, e.property, 64);
    return 0;
}

void put_event(const Event& e, u8* p) {
    std::memset(p, 0, kEventSize);
    std::memcpy(p, &e.id, 4);
    std::memcpy(p + 4, &e.state, 4);
    std::memcpy(p + 8, &e.type, 4);
    std::memcpy(p + 16, &e.time, 8);
    std::memcpy(p + 24, e.property, 64);
}

// The events as an array: as many as there are and the buffer holds, their count.
i32 sceSystemGestureGetTouchEvents(Ptr<const u8> object, Ptr<u8> out, u32 capacity, Ptr<u32> count) {
    if (!object || !count || !writable(count.address, 4)) return kInvalidArgument;
    if (capacity && (!out || !writable(out.address, kEventSize * capacity))) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    const u32 n = nx::min<u32>(capacity, static_cast<u32>(r->events.size()));
    for (u32 i = 0; i < n; ++i) put_event(r->events[i], out.get() + kEventSize * i);
    *count = n;
    return 0;
}
i32 sceSystemGestureGetTouchEventByEventID(Ptr<const u8> object, u32 id, Ptr<u8> out) {
    if (!object || !out || !writable(out.address, kEventSize)) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    for (const Event& e : r->events)
        if (e.id == id) {
            put_event(e, out.get());
            return 0;
        }
    return kOutOfArray;
}
// A recognizer starts over: no events, nothing in progress.
i32 sceSystemGestureResetTouchRecognizer(Ptr<u8> object) {
    if (!object) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    Recognizer fresh;
    fresh.type = r->type, fresh.port = r->port, fresh.rect = r->rect, fresh.max_taps = r->max_taps,
    fresh.hold_time = r->hold_time;
    *r = std::move(fresh);
    return 0;
}
// The area it recognizes in (SceSystemGestureRectangle: x, y, width, height; 0 width: the whole panel).
i32 sceSystemGestureUpdateTouchRecognizerRectangle(Ptr<u8> object, Ptr<const i16> rect) {
    if (!object || !rect || !readable(rect.address, 8)) return kInvalidArgument;
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    Recognizer* r = recognizer(g, object.address);
    if (!r) return kInvalidArgument;
    i16 v[4];
    std::memcpy(v, rect.get(), 8);
    r->rect = {v[0], v[1], v[2], v[3]};
    return 0;
}
// The primitive recognizer ends: touches forgotten (it must be initialized again before an update).
i32 sceSystemGestureFinalizePrimitiveTouchRecognizer() {
    Gestures& g = gestures();
    nx::scoped_lock<nx::mutex> held(g.lock);
    if (!g.initialized) return kNotInitialized;
    g.initialized = false;
    g.primitives.clear();
    return 0;
}

const Function kFunctions[] = {
    HLE_FUNCTION(0x789D867C, sceSystemGestureGetTouchEvents),
    HLE_FUNCTION(0x5570B83E, sceSystemGestureGetTouchEventByEventID),
    HLE_FUNCTION(0x0D941B90, sceSystemGestureResetTouchRecognizer),
    HLE_FUNCTION(0xA9DB29F6, sceSystemGestureUpdateTouchRecognizerRectangle),
    HLE_FUNCTION(0xB3875104, sceSystemGestureFinalizePrimitiveTouchRecognizer),
    HLE_FUNCTION(0x6078A08B, sceSystemGestureInitializePrimitiveTouchRecognizer),
    HLE_FUNCTION(0xDF4C665A, sceSystemGestureUpdatePrimitiveTouchRecognizer),
    HLE_FUNCTION(0xC3367370, sceSystemGestureCreateTouchRecognizer),
    HLE_FUNCTION(0x851FB144, sceSystemGestureUpdateTouchRecognizer),
    HLE_FUNCTION(0x13AD2218, sceSystemGestureGetTouchEventsCount),
    HLE_FUNCTION(0x74724147, sceSystemGestureGetTouchEventByIndex),
};

}  // namespace

void register_systemgesture() {
    {
        Gestures& g = gestures();
        nx::scoped_lock<nx::mutex> held(g.lock);
        g.initialized = false;
        g.primitives.clear();
        g.recognizers.clear();
    }
    register_library({"SceSystemGesture", kSceSystemGesture, kFunctions});
}

}  // namespace psvrecore::runtime::hle
