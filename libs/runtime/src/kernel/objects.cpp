// Reader-writer locks, message pipes and simple events, and opening objects by name. They wait as the other objects
// do (Impl::block on the object's queue; woken by end_wait), so the scheduling model is the same.
//
// A reader-writer lock gives read locks while no thread writes and no writer waits ahead (so writers are not starved),
// a write lock when nobody holds it; with the recursive attribute a holder may lock again, as reader or writer. An
// unlock hands the lock to the queue's head: a writer, or every reader up to the next writer.
//
// A message pipe moves bytes from senders to receivers through its buffer (a receiver waiting is given bytes straight
// from a sender when the buffer is empty, so a pipe without a buffer works too), in queue order. A FULL send or
// receive moves all of its bytes or waits; an ASAP one is done once it has moved any. DONT_WAIT fails at once
// (MSG_PIPE_FULL, MSG_PIPE_EMPTY) where it would wait; DONT_REMOVE receives without taking from the buffer.
//
// A simple event releases a waiter when any of the bits it waits for are set, with the pattern's bits it asked for and
// the user data set with them; one made AUTO_RESET clears those bits as it releases a waiter. Clearing keeps the bits
// given (as an event flag's clear does).

#include <algorithm>
#include <cstring>
#include <optional>

#include "kernel_impl.hpp"

namespace psvrecore::runtime {

using namespace kernel;

namespace {

constexpr u32 kMaxSyncObjects = 2048;
constexpr u32 kModeFull = 0x1, kModeDontWait = 0x10, kModeDontRemove = 0x100;

// A thread's read lock count on a lock.
i32* read_count(RWLock* l, Thread* t) {
    for (auto& [thread, count] : l->readers)
        if (thread == t) return &count;
    return nullptr;
}
bool writer_waiting(const RWLock* l) {
    return std::any_of(l->waiters.threads.begin(), l->waiters.threads.end(), [](Thread* t) { return t->wait.mode == 1; });
}

}  // namespace

// ---- reader-writer locks

Uid Kernel::create_rwlock(nx::string_view name, u32 attr) {
    Lock guard(impl_->lock);
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto l = nx::make_unique<RWLock>();
    l->waiters.by_priority = attr & kAttrThreadPriority;
    ++impl_->sync_count;
    return impl_->add(std::move(l), name, attr)->uid;
}

i32 Kernel::delete_rwlock(Uid uid) {
    Lock guard(impl_->lock);
    RWLock* l = impl_->get<RWLock>(uid, ObjectClass::RWLock);
    if (!l) return err::kUnknownRWLockId;
    while (!l->waiters.empty()) impl_->end_wait(l->waiters.front(), err::kWaitDelete);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

namespace {

// Takes the lock for `t` if it can have it now: 0, or 1 when it must wait, or an error.
i32 take_rwlock(RWLock* l, Thread* t, bool write, bool queued_ahead) {
    if (write) {
        if (l->writer == t) {
            if (!l->recursive()) return err::kRWLockRecursive;
            ++l->write_count;
            return 0;
        }
        if (l->writer || !l->readers.empty() || queued_ahead) return 1;
        l->writer = t;
        l->write_count = 1;
        return 0;
    }
    if (i32* held = read_count(l, t)) {
        if (!l->recursive()) return err::kRWLockRecursive;
        ++*held;
        return 0;
    }
    if (l->writer == t) {
        if (!l->recursive()) return err::kRWLockRecursive;
        l->readers.push_back({t, 1});
        return 0;
    }
    if (l->writer || writer_waiting(l) || queued_ahead) return 1;
    l->readers.push_back({t, 1});
    return 0;
}

}  // namespace

i32 Kernel::lock_rwlock(Uid uid, bool write, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    const auto deadline = impl_->deadline(timeout);
    for (;;) {
        RWLock* l = impl_->get<RWLock>(uid, ObjectClass::RWLock);
        if (!l) return err::kUnknownRWLockId;
        Thread* self = current();
        if (!self) return err::kIllegalContext;
        const i32 r = take_rwlock(l, self, write, false);
        if (r <= 0) {
            impl_->write_remaining(timeout, deadline);
            return r;
        }
        if (deadline && impl_->now_ns() >= *deadline) {
            impl_->write_remaining(timeout, deadline);
            return err::kWaitTimeout;
        }
        self->wait = Wait{};
        self->wait.type = WaitType::RWLock;
        self->wait.object = uid;
        self->wait.mode = write ? 1 : 0;
        l->waiters.add(self);
        const i32 w = impl_->block(self, &l->waiters, deadline, callbacks);
        if (w == kRunCallbacks) {
            impl_->run_callbacks(self);
            continue;
        }
        impl_->write_remaining(timeout, deadline);
        return w;  // (kOk: an unlock handed it the lock)
    }
}

i32 Kernel::try_lock_rwlock(Uid uid, bool write) {
    Lock guard(impl_->lock);
    RWLock* l = impl_->get<RWLock>(uid, ObjectClass::RWLock);
    if (!l) return err::kUnknownRWLockId;
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    const i32 r = take_rwlock(l, self, write, false);
    return r > 0 ? err::kRWLockFailedToLock : r;
}

// The lock to the queue's head: a writer, or the readers up to the next writer.
void Kernel::Impl::hand_over_rwlock(RWLock* l) {
    while (Thread* w = l->waiters.front()) {
        if (w->wait.mode == 1) {
            if (l->writer || !l->readers.empty()) return;
            l->writer = w;
            l->write_count = 1;
            end_wait(w, err::kOk);
            return;
        }
        if (l->writer) return;
        l->readers.push_back({w, 1});
        end_wait(w, err::kOk);
    }
}

i32 Kernel::unlock_rwlock(Uid uid, bool write) {
    Lock guard(impl_->lock);
    RWLock* l = impl_->get<RWLock>(uid, ObjectClass::RWLock);
    if (!l) return err::kUnknownRWLockId;
    Thread* self = current();
    if (!self) return err::kIllegalContext;
    if (write) {
        if (l->writer != self) return err::kRWLockNotOwned;
        if (--l->write_count == 0) l->writer = nullptr;
    } else {
        i32* held = read_count(l, self);
        if (!held) return err::kRWLockNotOwned;
        if (--*held == 0)
            l->readers.erase(std::find_if(l->readers.begin(), l->readers.end(), [&](auto& r) { return r.first == self; }));
    }
    impl_->hand_over_rwlock(l);
    return err::kOk;
}

i32 Kernel::cancel_rwlock(Uid uid, u32* waiting_readers, u32* waiting_writers) {
    Lock guard(impl_->lock);
    RWLock* l = impl_->get<RWLock>(uid, ObjectClass::RWLock);
    if (!l) return err::kUnknownRWLockId;
    u32 readers = 0, writers = 0;
    for (Thread* t : l->waiters.threads) (t->wait.mode == 1 ? writers : readers)++;
    if (waiting_readers) *waiting_readers = readers;
    if (waiting_writers) *waiting_writers = writers;
    while (!l->waiters.empty()) impl_->end_wait(l->waiters.front(), err::kWaitCancel);
    return err::kOk;
}

// ---- message pipes

Uid Kernel::create_msg_pipe(nx::string_view name, u32 attr, u32 size) {
    Lock guard(impl_->lock);
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto p = nx::make_unique<MsgPipe>();
    p->size = size;
    p->senders.by_priority = attr & 0x1;    // (TH_PRIO_S)
    p->receivers.by_priority = attr & 0x2;  // (TH_PRIO_R)
    ++impl_->sync_count;
    return impl_->add(std::move(p), name, attr)->uid;
}

i32 Kernel::delete_msg_pipe(Uid uid) {
    Lock guard(impl_->lock);
    MsgPipe* p = impl_->get<MsgPipe>(uid, ObjectClass::MsgPipe);
    if (!p) return err::kUnknownMsgPipeId;
    while (!p->senders.empty()) impl_->end_wait(p->senders.front(), err::kWaitDelete);
    while (!p->receivers.empty()) impl_->end_wait(p->receivers.front(), err::kWaitDelete);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

namespace {

// A side's wait: the guest buffer (pattern), its size (count), the mode, the bytes moved so far (restore_count).
bool satisfied(const Wait& w) {
    return (w.mode & kModeFull) ? w.restore_count == w.count : w.restore_count > 0 || w.count == 0;
}

}  // namespace

// Moves what can move now: the buffer to the first receiver, a sender's bytes straight to it (the buffer empty), the
// first sender's into the buffer; a side's wait ends once it is satisfied (the calling thread's: taken out of its
// queue and marked done, not woken).
void Kernel::Impl::pump_msg_pipe(MsgPipe* p, Thread* self) {
    const auto finish = [&](WaitQueue& q, Thread* t) {
        if (t == self) {
            q.remove(t);
            t->wait.done = true;
        } else {
            end_wait(t, err::kOk);
        }
    };
    for (bool moved = true; moved;) {
        moved = false;
        if (Thread* r = p->receivers.front()) {
            Wait& w = r->wait;
            const u32 want = w.count - w.restore_count;
            if (want && !p->data.empty()) {
                const u32 n = nx::min<u32>(want, static_cast<u32>(p->data.size()));
                std::memcpy(memory.host(w.pattern + w.restore_count), p->data.data(), n);
                p->data.erase(p->data.begin(), p->data.begin() + n);
                w.restore_count += n;
                moved = true;
            } else if (want) {
                if (Thread* s = p->senders.front()) {
                    Wait& v = s->wait;
                    if (const u32 n = nx::min<u32>(want, v.count - v.restore_count)) {
                        std::memcpy(memory.host(w.pattern + w.restore_count), memory.host(v.pattern + v.restore_count), n);
                        w.restore_count += n;
                        v.restore_count += n;
                        moved = true;
                        if (satisfied(v)) finish(p->senders, s);
                    }
                }
            }
            if (satisfied(w)) {
                finish(p->receivers, r);
                moved = true;
            }
        }
        if (Thread* s = p->senders.front()) {
            Wait& v = s->wait;
            const u32 room = p->size - static_cast<u32>(p->data.size());
            if (const u32 n = nx::min<u32>(room, v.count - v.restore_count)) {
                const u8* from = memory.host(v.pattern + v.restore_count);
                p->data.insert(p->data.end(), from, from + n);
                v.restore_count += n;
                moved = true;
            }
            if (satisfied(v)) {
                finish(p->senders, s);
                moved = true;
            }
        }
    }
}

i32 Kernel::Impl::transfer_msg_pipe(Uid uid, bool send, u32 buffer, u32 size, u32 mode, u32* moved,
                                    const Timeout& timeout, bool callbacks) {
    const auto deadline_ns = deadline(timeout);
    MsgPipe* p = get<MsgPipe>(uid, ObjectClass::MsgPipe);
    if (!p) return err::kUnknownMsgPipeId;
    if (mode & ~(kModeFull | kModeDontWait | (send ? 0u : kModeDontRemove))) return err::kIllegalMode;
    if (size && !memory.valid(buffer, size, !send)) return err::kIllegalAddress;
    if (!send && (mode & kModeDontRemove)) {
        // (a look at the buffer: its bytes copied, left there)
        const u32 n = nx::min<u32>(size, static_cast<u32>(p->data.size()));
        if (!n || ((mode & kModeFull) && n < size)) return err::kMsgPipeEmpty;
        std::memcpy(memory.host(buffer), p->data.data(), n);
        if (moved) *moved = n;
        return err::kOk;
    }
    Thread* self = Kernel::current();
    if (!self) return err::kIllegalContext;
    self->wait = Wait{};
    self->wait.type = WaitType::MsgPipe;
    self->wait.object = uid;
    self->wait.pattern = buffer;
    self->wait.count = size;
    self->wait.mode = mode;
    for (;;) {
        WaitQueue& queue = send ? p->senders : p->receivers;
        self->wait.done = false;
        queue.add(self);
        pump_msg_pipe(p, self);
        i32 r = err::kOk;
        if (!self->wait.done) {
            if (mode & kModeDontWait) {
                queue.remove(self);
                r = send ? err::kMsgPipeFull : err::kMsgPipeEmpty;
            } else if (deadline_ns && now_ns() >= *deadline_ns) {
                queue.remove(self);
                r = err::kWaitTimeout;
            } else {
                r = block(self, &queue, deadline_ns, callbacks);
                if (r == kRunCallbacks) {
                    run_callbacks(self);
                    p = get<MsgPipe>(uid, ObjectClass::MsgPipe);
                    if (!p) return err::kMsgPipeDeleted;
                    continue;
                }
            }
        }
        if (moved) *moved = self->wait.restore_count;
        write_remaining(timeout, deadline_ns);
        return r;
    }
}

i32 Kernel::send_msg_pipe(Uid uid, u32 buffer, u32 size, u32 mode, u32* moved, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    return impl_->transfer_msg_pipe(uid, true, buffer, size, mode, moved, timeout, callbacks);
}

i32 Kernel::receive_msg_pipe(Uid uid, u32 buffer, u32 size, u32 mode, u32* moved, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    return impl_->transfer_msg_pipe(uid, false, buffer, size, mode, moved, timeout, callbacks);
}

i32 Kernel::cancel_msg_pipe(Uid uid, u32* waiting_senders, u32* waiting_receivers) {
    Lock guard(impl_->lock);
    MsgPipe* p = impl_->get<MsgPipe>(uid, ObjectClass::MsgPipe);
    if (!p) return err::kUnknownMsgPipeId;
    if (waiting_senders) *waiting_senders = static_cast<u32>(p->senders.threads.size());
    if (waiting_receivers) *waiting_receivers = static_cast<u32>(p->receivers.threads.size());
    while (!p->senders.empty()) impl_->end_wait(p->senders.front(), err::kWaitCancel);
    while (!p->receivers.empty()) impl_->end_wait(p->receivers.front(), err::kWaitCancel);
    p->data.clear();
    return err::kOk;
}

// ---- simple events

Uid Kernel::create_simple_event(nx::string_view name, u32 attr, u32 pattern) {
    Lock guard(impl_->lock);
    if (impl_->sync_count >= kMaxSyncObjects) return err::kUidOverflow;
    auto e = nx::make_unique<SimpleEvent>();
    e->pattern = pattern;
    e->waiters.by_priority = attr & kAttrThreadPriority;
    ++impl_->sync_count;
    return impl_->add(std::move(e), name, attr)->uid;
}

i32 Kernel::delete_simple_event(Uid uid) {
    Lock guard(impl_->lock);
    SimpleEvent* e = impl_->get<SimpleEvent>(uid, ObjectClass::SimpleEvent);
    if (!e) return err::kUnknownEventId;
    while (!e->waiters.empty()) impl_->end_wait(e->waiters.front(), err::kWaitDelete);
    impl_->objects.erase(uid);
    --impl_->sync_count;
    return err::kOk;
}

i32 Kernel::set_event(Uid uid, u32 bits, u64 user_data) {
    Lock guard(impl_->lock);
    SimpleEvent* e = impl_->get<SimpleEvent>(uid, ObjectClass::SimpleEvent);
    if (!e) return err::kUnknownEventId;
    e->pattern |= bits;
    e->user_data = user_data;
    // (waiters released in queue order; an auto-reset event clears the bits each one takes)
    for (usize i = 0; i < e->waiters.threads.size();) {
        Thread* w = e->waiters.threads[i];
        const u32 got = e->pattern & w->wait.count;
        if (!got) {
            ++i;
            continue;
        }
        w->wait.pattern = got;
        if (e->auto_reset()) e->pattern &= ~got;
        impl_->end_wait(w, err::kOk);
    }
    return err::kOk;
}

i32 Kernel::clear_event(Uid uid, u32 bits) {
    Lock guard(impl_->lock);
    SimpleEvent* e = impl_->get<SimpleEvent>(uid, ObjectClass::SimpleEvent);
    if (!e) return err::kUnknownEventId;
    e->pattern &= bits;
    return err::kOk;
}

i32 Kernel::wait_event(Uid uid, u32 bits, u32* result, u64* user_data, Timeout timeout, bool callbacks) {
    Lock guard(impl_->lock);
    const auto deadline = impl_->deadline(timeout);
    if (!bits) return err::kEventCond;
    for (;;) {
        SimpleEvent* e = impl_->get<SimpleEvent>(uid, ObjectClass::SimpleEvent);
        if (!e) return err::kUnknownEventId;
        if (const u32 got = e->pattern & bits) {
            if (e->auto_reset()) e->pattern &= ~got;
            if (result) *result = got;
            if (user_data) *user_data = e->user_data;
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
        self->wait.type = WaitType::Event;
        self->wait.object = uid;
        self->wait.count = bits;
        e->waiters.add(self);
        const i32 w = impl_->block(self, &e->waiters, deadline, callbacks);
        if (w == kRunCallbacks) {
            impl_->run_callbacks(self);
            continue;
        }
        impl_->write_remaining(timeout, deadline);
        if (w == err::kOk) {
            if (result) *result = self->wait.pattern;
            if (user_data) {
                SimpleEvent* again = impl_->get<SimpleEvent>(uid, ObjectClass::SimpleEvent);
                *user_data = again ? again->user_data : 0;
            }
        }
        return w;
    }
}

// ---- opening by name

Uid Kernel::open_object(nx::string_view name, ObjectClass cls, i32 unknown) {
    Lock guard(impl_->lock);
    for (const auto& [uid, object] : impl_->objects)
        if (object->cls == cls && (object->attr & 0x80) && object->name == name) return uid;
    return unknown;
}

i32 Kernel::close_object(Uid uid, ObjectClass cls, i32 unknown) {
    Lock guard(impl_->lock);
    auto it = impl_->objects.find(uid);
    return it != impl_->objects.end() && it->second->cls == cls ? err::kOk : unknown;
}

}  // namespace psvrecore::runtime
