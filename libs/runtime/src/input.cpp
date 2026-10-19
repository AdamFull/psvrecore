#include "psvrecore/runtime/input.hpp"

namespace psvrecore::runtime {

void Input::prune(u64 now_ns) {
    // (events old enough that no sample will ask about the time before them: folded into the base state)
    const u64 keep_from = now_ns > kKeepNs ? now_ns - kKeepNs : 0;
    usize n = 0;
    while (n < pad_events_.size() && pad_events_[n].at_ns < keep_from) pad_ = pad_events_[n++].state;
    if (n) pad_events_.erase(pad_events_.begin(), pad_events_.begin() + static_cast<isize>(n));
    for (u32 p = 0; p < 2; ++p) {
        usize m = 0;
        while (m < touch_events_[p].size() && touch_events_[p][m].at_ns < keep_from) touch_[p] = touch_events_[p][m++].state;
        if (m) touch_events_[p].erase(touch_events_[p].begin(), touch_events_[p].begin() + static_cast<isize>(m));
    }
}

template <typename T>
void insert_ordered(nx::vector<T>& events, T e) {
    // (events usually arrive in order; a late one goes where its stamp puts it)
    usize at = events.size();
    while (at > 0 && events[at - 1].at_ns > e.at_ns) --at;
    events.insert(events.begin() + static_cast<isize>(at), e);
}

void Input::set_pad(const PadState& state, u64 at_ns) {
    nx::scoped_lock<nx::mutex> held(lock_);
    insert_ordered(pad_events_, Event<PadState>{at_ns, state});
    prune(at_ns);
}

void Input::set_touch(u32 port, const TouchState& state, u64 at_ns) {
    if (port > 1) return;
    nx::scoped_lock<nx::mutex> held(lock_);
    insert_ordered(touch_events_[port], Event<TouchState>{at_ns, state});
    prune(at_ns);
}

PadState Input::pad_sample(u64 previous_ns, u64 t_ns) {
    nx::scoped_lock<nx::mutex> held(lock_);
    PadState s = pad_;
    u32 pressed = 0;
    for (const Event<PadState>& e : pad_events_) {
        if (e.at_ns > t_ns) break;
        s = e.state;
        if (e.at_ns > previous_ns) pressed |= e.state.buttons;
    }
    s.buttons |= pressed;
    return s;
}

TouchState Input::touch_sample(u32 port, u64 t_ns) {
    if (port > 1) return {};
    nx::scoped_lock<nx::mutex> held(lock_);
    TouchState s = touch_[port];
    for (const Event<TouchState>& e : touch_events_[port]) {
        if (e.at_ns > t_ns) break;
        s = e.state;
    }
    return s;
}

}  // namespace psvrecore::runtime
