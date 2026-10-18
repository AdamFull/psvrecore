// Synchronisation objects: semaphores, event flags, mutexes (and lightweight mutexes), condition variables (and
// lightweight ones), callbacks.

#include <algorithm>

#include "kernel_impl.hpp"

namespace psvrecore::runtime {

using namespace kernel;

namespace {

constexpr u32 kMaxSyncObjects = 2048;

constexpr u32 kEvfOr = 0x1, kEvfClearAll = 0x2, kEvfClearPattern = 0x4;

bool event_satisfied(u32 pattern, u32 bits, u32 mode) {
    return (mode & kEvfOr) ? (pattern & bits) != 0 : (pattern & bits) == bits;
}

u32 event_after(u32 pattern, u32 bits, u32 mode) {
    if (mode & kEvfClearAll) return 0;
    if (mode & kEvfClearPattern) return pattern & ~bits;
    return pattern;
}

i32 unknown_mutex(ObjectClass cls) { return cls == ObjectClass::LwMutex ? err::kUnknownLwMutexId : err::kUnknownMutexId; }
i32 unknown_cond(ObjectClass cls) { return cls == ObjectClass::LwCond ? err::kUnknownLwCondId : err::kUnknownCondId; }

}  // namespace

// ---- semaphores

Uid Kernel::create_semaphore(nx::string_view name, u32 attr, i32 init, i32 max) {
    Lock guard(impl_->lock);
    if (max <= 0 || init < 0 || init > max) return err::kIllegalCount;
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto s = std::make_unique<Semaphore>();
    s->count = s->init = init;
    s->max = max;
    s->waiters.by_priority = attr & kAttrThreadPriority;
    ++impl_->sync_count;
    return impl_->add(std::move(s), name, attr)->uid;
}

i32 Kernel::delete_semaphore(Uid uid) {
    Lock guard(impl_->lock);
    Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
    if (!s) return err::kUnknownSemaphoreId;
    while (!s->waiters.empty()) impl_->end_wait(s->waiters.front(), err::kWaitDelete);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::wait_semaphore(Uid uid, i32 count, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    const auto deadline = impl_->deadline(timeout);
    for (;;) {
        Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
        if (!s) return err::kUnknownSemaphoreId;
        if (count <= 0 || count > s->max) return err::kIllegalCount;
        if (s->count >= count) {
            s->count -= count;
            impl_->write_remaining(timeout, deadline);
            return err::kOk;
        }
        Thread* self = current();
        if (!self) return err::kIllegalContext;
        if (deadline && impl_->now_ns() >= *deadline) {
            impl_->write_remaining(timeout, deadline);
            return err::kWaitTimeout;
        }
        self->wait = Wait{};
        self->wait.type = WaitType::Semaphore;
        self->wait.object = uid;
        self->wait.count = static_cast<u32>(count);
        s->waiters.add(self);
        const i32 r = impl_->block(self, &s->waiters, deadline, callbacks);
        if (r == kRunCallbacks) {
            impl_->run_callbacks(self);
            continue;
        }
        impl_->write_remaining(timeout, deadline);
        return r;  // (kOk: signal_semaphore gave this thread its resources)
    }
}

i32 Kernel::poll_semaphore(Uid uid, i32 count) {
    Lock guard(impl_->lock);
    Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
    if (!s) return err::kUnknownSemaphoreId;
    if (count <= 0) return err::kIllegalCount;
    if (s->count < count) return err::kSemaphoreZero;
    s->count -= count;
    return err::kOk;
}

i32 Kernel::signal_semaphore(Uid uid, i32 count) {
    Lock guard(impl_->lock);
    Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
    if (!s) return err::kUnknownSemaphoreId;
    if (count <= 0) return err::kIllegalCount;
    if (s->count + static_cast<i64>(count) > s->max) return err::kSemaphoreOverflow;
    s->count += count;
    // Waiters take their resources from the head of the queue on, until one cannot be satisfied.
    while (Thread* w = s->waiters.front()) {
        if (static_cast<i32>(w->wait.count) > s->count) break;
        s->count -= static_cast<i32>(w->wait.count);
        impl_->end_wait(w, err::kOk);
    }
    return err::kOk;
}

i32 Kernel::cancel_semaphore(Uid uid, i32 count, u32* waiting) {
    Lock guard(impl_->lock);
    Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
    if (!s) return err::kUnknownSemaphoreId;
    if (count < -1 || count > s->max) return err::kIllegalCount;
    if (waiting) *waiting = static_cast<u32>(s->waiters.threads.size());
    while (!s->waiters.empty()) impl_->end_wait(s->waiters.front(), err::kWaitCancel);
    s->count = count == -1 ? s->init : count;
    return err::kOk;
}

i32 Kernel::semaphore_info(Uid uid, Semaphore* out, u32* waiting) {
    Lock guard(impl_->lock);
    Semaphore* s = impl_->get<Semaphore>(uid, ObjectClass::Semaphore);
    if (!s) return err::kUnknownSemaphoreId;
    out->uid = s->uid;
    out->name = s->name;
    out->attr = s->attr;
    out->count = s->count;
    out->max = s->max;
    out->init = s->init;
    if (waiting) *waiting = static_cast<u32>(s->waiters.threads.size());
    return err::kOk;
}

// ---- event flags

Uid Kernel::create_event_flag(nx::string_view name, u32 attr, u32 pattern) {
    Lock guard(impl_->lock);
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto e = std::make_unique<EventFlag>();
    e->pattern = e->init = pattern;
    e->waiters.by_priority = attr & kAttrThreadPriority;
    ++impl_->sync_count;
    return impl_->add(std::move(e), name, attr)->uid;
}

i32 Kernel::delete_event_flag(Uid uid) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    while (Thread* w = e->waiters.front()) {
        w->wait.pattern = e->pattern;
        impl_->end_wait(w, err::kWaitDelete);
    }
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::wait_event_flag(Uid uid, u32 bits, u32 mode, u32* result, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    const auto deadline = impl_->deadline(timeout);
    if ((mode & ~0x7u) || ((mode & kEvfClearAll) && (mode & kEvfClearPattern))) return err::kIllegalMode;
    if (bits == 0) return err::kEventFlagCond;  // (no condition could ever be met)
    for (;;) {
        EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
        if (!e) return err::kUnknownEventFlagId;
        if (!(e->attr & kAttrMulti) && !e->waiters.empty()) return err::kEventFlagMulti;
        if (event_satisfied(e->pattern, bits, mode)) {
            if (result) *result = e->pattern;
            e->pattern = event_after(e->pattern, bits, mode);
            impl_->write_remaining(timeout, deadline);
            return err::kOk;
        }
        Thread* self = current();
        if (!self) return err::kIllegalContext;
        if (deadline && impl_->now_ns() >= *deadline) {
            if (result) *result = e->pattern;
            impl_->write_remaining(timeout, deadline);
            return err::kWaitTimeout;
        }
        self->wait = Wait{};
        self->wait.type = WaitType::EventFlag;
        self->wait.object = uid;
        self->wait.pattern = bits;
        self->wait.mode = mode;
        e->waiters.add(self);
        const i32 r = impl_->block(self, &e->waiters, deadline, callbacks);
        if (r == kRunCallbacks) {
            impl_->run_callbacks(self);
            continue;
        }
        if (r == err::kWaitTimeout) {
            if (EventFlag* still = impl_->get<EventFlag>(uid, ObjectClass::EventFlag)) self->wait.pattern = still->pattern;
        }
        if (result) *result = self->wait.pattern;  // (set_event_flag/cancel/delete leave the flag's value there)
        impl_->write_remaining(timeout, deadline);
        return r;
    }
}

i32 Kernel::poll_event_flag(Uid uid, u32 bits, u32 mode, u32* result) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    if ((mode & ~0x7u) || ((mode & kEvfClearAll) && (mode & kEvfClearPattern))) return err::kIllegalMode;
    if (bits == 0) return err::kEventFlagCond;
    if (!(e->attr & kAttrMulti) && !e->waiters.empty()) return err::kEventFlagMulti;
    if (result) *result = e->pattern;
    if (!event_satisfied(e->pattern, bits, mode)) return err::kEventFlagCond;
    e->pattern = event_after(e->pattern, bits, mode);
    return err::kOk;
}

i32 Kernel::set_event_flag(Uid uid, u32 bits) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    e->pattern |= bits;
    // In queue order; a waiter that clears bits can leave those behind it waiting.
    for (usize i = 0; i < e->waiters.threads.size();) {
        Thread* w = e->waiters.threads[i];
        if (event_satisfied(e->pattern, w->wait.pattern, w->wait.mode)) {
            const u32 bits_wanted = w->wait.pattern, mode = w->wait.mode;
            w->wait.pattern = e->pattern;
            e->pattern = event_after(e->pattern, bits_wanted, mode);
            impl_->end_wait(w, err::kOk);  // (removes it from the queue)
        } else {
            ++i;
        }
    }
    return err::kOk;
}

i32 Kernel::clear_event_flag(Uid uid, u32 bits) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    e->pattern &= bits;
    return err::kOk;
}

i32 Kernel::cancel_event_flag(Uid uid, u32 pattern, u32* waiting) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    if (waiting) *waiting = static_cast<u32>(e->waiters.threads.size());
    while (Thread* w = e->waiters.front()) {
        w->wait.pattern = pattern;
        impl_->end_wait(w, err::kWaitCancel);
    }
    e->pattern = pattern;
    return err::kOk;
}

i32 Kernel::event_flag_info(Uid uid, EventFlag* out, u32* waiting) {
    Lock guard(impl_->lock);
    EventFlag* e = impl_->get<EventFlag>(uid, ObjectClass::EventFlag);
    if (!e) return err::kUnknownEventFlagId;
    out->uid = e->uid;
    out->name = e->name;
    out->attr = e->attr;
    out->pattern = e->pattern;
    out->init = e->init;
    if (waiting) *waiting = static_cast<u32>(e->waiters.threads.size());
    return err::kOk;
}

// ---- mutexes

void Kernel::Impl::hand_over_mutex(Mutex* m) {
    if (m->owner) {
        auto& owned = m->owner->owned_mutexes;
        owned.erase(std::remove(owned.begin(), owned.end(), m->uid), owned.end());
    }
    m->owner = nullptr;
    m->count = 0;
    if (Thread* w = m->waiters.front()) {
        m->owner = w;
        m->count = static_cast<i32>(w->wait.count);
        if (m->cls == ObjectClass::Mutex) w->owned_mutexes.push_back(m->uid);
        end_wait(w, err::kOk);
    }
}

void Kernel::Impl::unlock_owned_mutexes(Thread* t) {
    // A thread that ends releases the mutexes it owns (not lightweight ones: those stay locked).
    while (!t->owned_mutexes.empty()) {
        Mutex* m = get<Mutex>(t->owned_mutexes.back(), ObjectClass::Mutex);
        if (!m || m->owner != t) {
            t->owned_mutexes.pop_back();
            continue;
        }
        hand_over_mutex(m);
    }
}

Uid Kernel::create_mutex(nx::string_view name, u32 attr, i32 init, ObjectClass cls, u32 work) {
    Lock guard(impl_->lock);
    if (init < 0 || (init > 1 && !(attr & 0x2))) return cls == ObjectClass::LwMutex ? err::kIllegalCount : err::kIllegalCount;
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto m = std::make_unique<Mutex>(cls);
    m->init = init;
    m->waiters.by_priority = attr & kAttrThreadPriority;
    m->work = work;
    Mutex* raw = impl_->add(std::move(m), name, attr);
    if (init > 0) {
        raw->owner = current();
        raw->count = init;
        if (raw->owner && cls == ObjectClass::Mutex) raw->owner->owned_mutexes.push_back(raw->uid);
    }
    if (work) impl_->lw_objects[work] = raw->uid;
    ++impl_->sync_count;
    return raw->uid;
}

i32 Kernel::delete_mutex(Uid uid, ObjectClass cls) {
    Lock guard(impl_->lock);
    Mutex* m = impl_->get<Mutex>(uid, cls);
    if (!m) return unknown_mutex(cls);
    while (!m->waiters.empty()) impl_->end_wait(m->waiters.front(), err::kWaitDelete);
    if (m->owner) {
        auto& owned = m->owner->owned_mutexes;
        owned.erase(std::remove(owned.begin(), owned.end(), uid), owned.end());
    }
    if (m->work) impl_->lw_objects.erase(m->work);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::lock_mutex(Uid uid, i32 count, Timeout timeout, bool callbacks, ObjectClass cls) {
    Lock guard(impl_->lock);
    const bool lw = cls == ObjectClass::LwMutex;
    const auto deadline = impl_->deadline(timeout);
    for (;;) {
        Mutex* m = impl_->get<Mutex>(uid, cls);
        if (!m) return unknown_mutex(cls);
        if (count <= 0 || (count > 1 && !m->recursive())) return err::kIllegalCount;
        Thread* self = current();
        if (!self) return err::kIllegalContext;
        if (!m->owner) {
            m->owner = self;
            m->count = count;
            if (!lw) self->owned_mutexes.push_back(uid);
            impl_->write_remaining(timeout, deadline);
            return err::kOk;
        }
        if (m->owner == self) {
            if (!m->recursive()) return lw ? err::kLwMutexRecursive : err::kMutexRecursive;
            if (static_cast<i64>(m->count) + count > 0x7FFFFFFF) return lw ? err::kLwMutexLockOverflow : err::kMutexLockOverflow;
            m->count += count;
            impl_->write_remaining(timeout, deadline);
            return err::kOk;
        }
        if (deadline && impl_->now_ns() >= *deadline) {
            impl_->write_remaining(timeout, deadline);
            return err::kWaitTimeout;
        }
        self->wait = Wait{};
        self->wait.type = lw ? WaitType::LwMutex : WaitType::Mutex;
        self->wait.object = uid;
        self->wait.count = static_cast<u32>(count);
        m->waiters.add(self);
        const i32 r = impl_->block(self, &m->waiters, deadline, callbacks);
        if (r == kRunCallbacks) {
            impl_->run_callbacks(self);
            continue;
        }
        impl_->write_remaining(timeout, deadline);
        return r;  // (kOk: hand_over_mutex made this thread the owner)
    }
}

i32 Kernel::try_lock_mutex(Uid uid, i32 count, ObjectClass cls) {
    Lock guard(impl_->lock);
    const bool lw = cls == ObjectClass::LwMutex;
    Mutex* m = impl_->get<Mutex>(uid, cls);
    if (!m) return unknown_mutex(cls);
    if (count <= 0 || (count > 1 && !m->recursive())) return err::kIllegalCount;
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    if (!m->owner) {
        m->owner = self;
        m->count = count;
        if (!lw) self->owned_mutexes.push_back(uid);
        return err::kOk;
    }
    if (m->owner == self) {
        if (!m->recursive()) return lw ? err::kLwMutexRecursive : err::kMutexRecursive;
        m->count += count;
        return err::kOk;
    }
    return lw ? err::kLwMutexFailedToOwn : err::kMutexFailedToOwn;
}

i32 Kernel::unlock_mutex(Uid uid, i32 count, ObjectClass cls) {
    Lock guard(impl_->lock);
    const bool lw = cls == ObjectClass::LwMutex;
    Mutex* m = impl_->get<Mutex>(uid, cls);
    if (!m) return unknown_mutex(cls);
    if (count <= 0 || (count > 1 && !m->recursive())) return err::kIllegalCount;
    if (m->owner != current() || !m->owner) return lw ? err::kLwMutexNotOwned : err::kMutexNotOwned;
    if (m->count < count) return lw ? err::kLwMutexUnlockUnderflow : err::kMutexUnlockUnderflow;
    m->count -= count;
    if (m->count == 0) impl_->hand_over_mutex(m);
    return err::kOk;
}

i32 Kernel::cancel_mutex(Uid uid, i32 count, u32* waiting) {
    Lock guard(impl_->lock);
    Mutex* m = impl_->get<Mutex>(uid, ObjectClass::Mutex);
    if (!m) return err::kUnknownMutexId;
    if (count < -1 || (count > 1 && !m->recursive())) return err::kIllegalCount;
    if (waiting) *waiting = static_cast<u32>(m->waiters.threads.size());
    while (!m->waiters.empty()) impl_->end_wait(m->waiters.front(), err::kWaitCancel);
    if (m->owner) {
        auto& owned = m->owner->owned_mutexes;
        owned.erase(std::remove(owned.begin(), owned.end(), uid), owned.end());
    }
    const i32 n = count == -1 ? m->init : count;
    m->owner = n > 0 ? current() : nullptr;
    m->count = n;
    if (m->owner) m->owner->owned_mutexes.push_back(uid);
    return err::kOk;
}

i32 Kernel::mutex_info(Uid uid, ObjectClass cls, Mutex* out, Uid* owner, u32* waiting) {
    Lock guard(impl_->lock);
    Mutex* m = impl_->get<Mutex>(uid, cls);
    if (!m) return unknown_mutex(cls);
    out->uid = m->uid;
    out->name = m->name;
    out->attr = m->attr;
    out->count = m->count;
    out->init = m->init;
    out->work = m->work;
    if (owner) *owner = m->owner ? m->owner->uid : 0;
    if (waiting) *waiting = static_cast<u32>(m->waiters.threads.size());
    return err::kOk;
}

// ---- condition variables

Uid Kernel::create_cond(nx::string_view name, u32 attr, Uid mutex, ObjectClass cls, u32 work) {
    Lock guard(impl_->lock);
    const ObjectClass mutex_cls = cls == ObjectClass::LwCond ? ObjectClass::LwMutex : ObjectClass::Mutex;
    if (!impl_->get<Mutex>(mutex, mutex_cls)) return unknown_mutex(mutex_cls);
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto c = std::make_unique<Cond>(cls);
    c->mutex = mutex;
    c->waiters.by_priority = attr & kAttrThreadPriority;
    c->work = work;
    Cond* raw = impl_->add(std::move(c), name, attr);
    if (work) impl_->lw_objects[work] = raw->uid;
    ++impl_->sync_count;
    return raw->uid;
}

i32 Kernel::delete_cond(Uid uid, ObjectClass cls) {
    Lock guard(impl_->lock);
    Cond* c = impl_->get<Cond>(uid, cls);
    if (!c) return unknown_cond(cls);
    while (!c->waiters.empty()) impl_->end_wait(c->waiters.front(), err::kWaitDelete);
    if (c->work) impl_->lw_objects.erase(c->work);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::wait_cond(Uid uid, Timeout timeout, bool callbacks, ObjectClass cls) {
    Lock guard(impl_->lock);
    const bool lw = cls == ObjectClass::LwCond;
    const ObjectClass mutex_cls = lw ? ObjectClass::LwMutex : ObjectClass::Mutex;
    const auto deadline = impl_->deadline(timeout);
    Cond* c = impl_->get<Cond>(uid, cls);
    if (!c) return unknown_cond(cls);
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    const Uid mutex_uid = c->mutex;
    Mutex* m = impl_->get<Mutex>(mutex_uid, mutex_cls);
    if (!m) return unknown_mutex(mutex_cls);
    if (m->owner != self) return lw ? err::kLwMutexNotOwned : err::kMutexNotOwned;

    // Release the mutex by one lock, wait for a signal, then own it again with one lock.
    m->count -= 1;
    if (m->count == 0) impl_->hand_over_mutex(m);
    self->wait = Wait{};
    self->wait.type = lw ? WaitType::LwCondSignal : WaitType::CondSignal;
    self->wait.object = uid;
    c->waiters.add(self);
    i32 r = impl_->block(self, &c->waiters, deadline, false);
    (void)callbacks;  // (callbacks run once the mutex is held again, below)

    // Take the mutex back, whatever ended the wait (the guest expects to own it on return).
    for (;;) {
        m = impl_->get<Mutex>(mutex_uid, mutex_cls);
        if (!m) {
            r = lw ? err::kUnknownLwMutexId : err::kUnknownMutexId;
            break;
        }
        if (!m->owner) {
            m->owner = self;
            m->count = 1;
            if (!lw) self->owned_mutexes.push_back(mutex_uid);
            break;
        }
        if (m->owner == self) {
            m->count += 1;
            break;
        }
        self->wait = Wait{};
        self->wait.type = lw ? WaitType::LwCondMutex : WaitType::CondMutex;
        self->wait.object = mutex_uid;
        self->wait.count = 1;
        m->waiters.add(self);
        const i32 owned = impl_->block(self, &m->waiters, std::nullopt, false);
        if (owned == err::kOk) break;  // (hand_over_mutex made it the owner)
        r = owned;
        break;
    }
    if (callbacks && impl_->has_callbacks(self)) impl_->run_callbacks(self);
    impl_->write_remaining(timeout, deadline);
    return r;
}

i32 Kernel::signal_cond(Uid uid, bool all, Uid to, ObjectClass cls) {
    Lock guard(impl_->lock);
    Cond* c = impl_->get<Cond>(uid, cls);
    if (!c) return unknown_cond(cls);
    if (to) {
        auto it = std::find_if(c->waiters.threads.begin(), c->waiters.threads.end(), [&](const Thread* t) { return t->uid == to; });
        if (it == c->waiters.threads.end()) return err::kUnknownThreadId;  // (not waiting on this condition)
        impl_->end_wait(*it, err::kOk);
        return err::kOk;
    }
    if (all) {
        while (!c->waiters.empty()) impl_->end_wait(c->waiters.front(), err::kOk);
    } else if (Thread* w = c->waiters.front()) {
        impl_->end_wait(w, err::kOk);
    }
    return err::kOk;
}

// ---- callbacks

Uid Kernel::create_callback(nx::string_view name, u32 attr, u32 function, u32 common) {
    Lock guard(impl_->lock);
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    if (!function) return err::kInvalidArgument;
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto cb = std::make_unique<Callback>();
    cb->thread = self;
    cb->function = function;
    cb->common = common;
    Callback* raw = impl_->add(std::move(cb), name, attr);
    self->callbacks.push_back(raw->uid);
    ++impl_->sync_count;
    return raw->uid;
}

i32 Kernel::delete_callback(Uid uid) {
    Lock guard(impl_->lock);
    Callback* cb = impl_->get<Callback>(uid, ObjectClass::Callback);
    if (!cb) return err::kUnknownCallbackId;
    auto& list = cb->thread->callbacks;
    list.erase(std::remove(list.begin(), list.end(), uid), list.end());
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::notify_callback(Uid uid, i32 arg) {
    Lock guard(impl_->lock);
    Callback* cb = impl_->get<Callback>(uid, ObjectClass::Callback);
    if (!cb) return err::kUnknownCallbackId;
    ++cb->notify_count;
    cb->notify_arg = arg;
    Impl::wake(cb->thread);  // (a thread in a CB wait runs it)
    return err::kOk;
}

i32 Kernel::cancel_callback(Uid uid) {
    Lock guard(impl_->lock);
    Callback* cb = impl_->get<Callback>(uid, ObjectClass::Callback);
    if (!cb) return err::kUnknownCallbackId;
    cb->notify_count = 0;
    cb->notify_arg = 0;
    return err::kOk;
}

i32 Kernel::callback_count(Uid uid) {
    Lock guard(impl_->lock);
    Callback* cb = impl_->get<Callback>(uid, ObjectClass::Callback);
    if (!cb) return err::kUnknownCallbackId;
    return cb->notify_count;
}

i32 Kernel::check_callbacks() {
    Lock guard(impl_->lock);
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    return impl_->run_callbacks(self);
}

}  // namespace psvrecore::runtime
