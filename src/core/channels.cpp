// Contexts, channel cores, select and task groups (include/stxlib/channels.hpp).
//
// Waits. A channel wait sleeps on its condition variable with a deadline:
// the context's deadline when nothing can cancel it sooner, otherwise at
// most context_poll_interval ahead, so a cancellation (a flag) is noticed
// within one slice (the header's "Waiting for a context"). Every wait
// re-checks its condition under the lock before sleeping (threads.hpp's
// predicate rule), so a send or close between the check and the sleep is
// never missed.

#include "stxlib/channels.hpp"

namespace stxlib {

namespace {

// The earliest of a context's deadline and the next poll slice: the moment a
// wait wakes to look at the context again. With no context, never.
IntervalTime next_look(const Context* const context) noexcept {
    if (context == nullptr) return IntervalTime::from_nanoseconds(i64_max);
    const IntervalTime slice = IntervalTime::now().after(context_poll_interval);
    const IntervalTime deadline = (*context).deadline();
    const IntervalTime result = deadline < slice ? deadline : slice;
    return result;
}

bool context_ended(const Context* const context) noexcept {
    if (context == nullptr) return false;
    const bool result = (*context).done();
    return result;
}

} // namespace

// ---- Context -------------------------------------------------------------------

Context::Context(const IntervalTime deadline) noexcept : deadline_(deadline) {}

Context::Context(const Context& parent, const IntervalTime deadline) noexcept
    : parent_(&parent), deadline_(deadline), depth_(parent.depth_ + 1) {
    STXLIB_REQUIRE(parent.depth_ + 1 < context_depth_limit, "Context: the chain of parents is too deep");
}

Context::Context(const ChildTag, const Context& parent) noexcept
    : parent_(&parent), deadline_(IntervalTime::from_nanoseconds(i64_max)), depth_(parent.depth_ + 1) {
    STXLIB_REQUIRE(parent.depth_ + 1 < context_depth_limit, "Context: the chain of parents is too deep");
}

void Context::cancel(const ContextEnd reason) noexcept {
    STXLIB_REQUIRE(reason != ContextEnd::live, "Context::cancel: a context cannot be cancelled into live");
    u8 expected = static_cast<u8>(ContextEnd::live);
    // The first end is kept: the exchange succeeds only from live.
    static_cast<void>(__atomic_compare_exchange_n(&reason_, &expected, static_cast<u8>(reason), false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

ContextEnd Context::end() const noexcept {
    // Own and ancestors' explicit ends first, nearest first; then the
    // deadlines. The chain is at most context_depth_limit long (checked at
    // construction), so the walk is bounded.
    const Context* link = this;
    for (u32 step = 0; step < context_depth_limit && link != nullptr; ++step) {
        const u8 reason = __atomic_load_n(&(*link).reason_, __ATOMIC_ACQUIRE);
        if (reason != static_cast<u8>(ContextEnd::live)) return static_cast<ContextEnd>(reason);
        link = (*link).parent_;
    }
    const IntervalTime earliest = deadline();
    if (earliest.count_nanoseconds() != i64_max && !(IntervalTime::now() < earliest)) {
        return ContextEnd::deadline_passed;
    }
    return ContextEnd::live;
}

IntervalTime Context::deadline() const noexcept {
    IntervalTime earliest = deadline_;
    const Context* link = parent_;
    for (u32 step = 0; step < context_depth_limit && link != nullptr; ++step) {
        if ((*link).deadline_ < earliest) earliest = (*link).deadline_;
        link = (*link).parent_;
    }
    return earliest;
}

// ---- ChannelNotifier -------------------------------------------------------------

namespace {

struct NotifierWait final {
    const u64* signals;
    u64 seen;
};

bool notifier_signalled(const void* const context) noexcept {
    const NotifierWait& wait = *static_cast<const NotifierWait*>(context);
    const bool result = *wait.signals != wait.seen;
    return result;
}

} // namespace

void ChannelNotifier::signal() noexcept {
    {
        ScopedLock lock(mutex_);
        signals_ = signals_ + 1; // wraps after 2^64 signals: only inequality is read
    }
    signalled_.notify_all();
}

u64 ChannelNotifier::signals() noexcept {
    ScopedLock lock(mutex_);
    const u64 result = signals_;
    return result;
}

void ChannelNotifier::wait(const u64 seen, const IntervalTime until) noexcept {
    ScopedLock lock(mutex_);
    const NotifierWait wait_state{&signals_, seen};
    static_cast<void>(signalled_.wait_until(lock, &notifier_signalled, &wait_state, until));
}

// ---- ChannelCore -------------------------------------------------------------------

namespace {

struct RoomWait final {
    const usize* count;
    usize capacity;
    const bool* closed;
};

bool room_or_closed(const void* const context) noexcept {
    const RoomWait& wait = *static_cast<const RoomWait*>(context);
    const bool result = *wait.closed || *wait.count < wait.capacity;
    return result;
}

struct RecordWait final {
    const usize* count;
    const bool* closed;
};

bool record_or_closed(const void* const context) noexcept {
    const RecordWait& wait = *static_cast<const RecordWait*>(context);
    const bool result = *wait.closed || *wait.count != 0;
    return result;
}

} // namespace

usize ChannelCore::write_slot() const noexcept {
    // head_ < capacity_ and count_ < capacity_ (a send waits for room), so
    // the sum is below twice the capacity and the subtraction wraps once.
    const usize position = head_ + count_;
    const usize result = position < capacity_ ? position : position - capacity_;
    return result;
}

void ChannelCore::note_sent() noexcept { count_ = count_ + 1; }

void ChannelCore::note_received() noexcept {
    head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
    count_ = count_ - 1;
}

ChannelStatus ChannelCore::wait_for_room(ScopedLock& lock, const Context* const context) noexcept {
    const RoomWait wait{&count_, capacity_, &closed_};
    for (;;) {
        if (closed_) return ChannelStatus::closed;
        if (count_ < capacity_) return ChannelStatus::delivered;
        if (context_ended(context)) return ChannelStatus::ended;
        if (context == nullptr) {
            not_full_.wait(lock, &room_or_closed, &wait);
        } else {
            static_cast<void>(not_full_.wait_until(lock, &room_or_closed, &wait, next_look(context)));
        }
    }
}

ChannelStatus ChannelCore::wait_for_record(ScopedLock& lock, const Context* const context) noexcept {
    const RecordWait wait{&count_, &closed_};
    for (;;) {
        if (count_ != 0) return ChannelStatus::delivered;
        if (closed_) return ChannelStatus::closed;
        if (context_ended(context)) return ChannelStatus::ended;
        if (context == nullptr) {
            not_empty_.wait(lock, &record_or_closed, &wait);
        } else {
            static_cast<void>(not_empty_.wait_until(lock, &record_or_closed, &wait, next_look(context)));
        }
    }
}

void ChannelCore::wake_receivers() noexcept {
    not_empty_.notify_one();
    // The notifiers are signalled under this channel's lock: a select
    // detaches its notifier (taking this lock) before the notifier ends, so
    // a notifier read here is alive until the lock is released.
    // ChannelNotifier::signal takes only the notifier's own lock, and no
    // code takes a channel's lock under a notifier's, so the one nesting is
    // channel, then notifier. (The header's "never" refers to channel
    // locks: no channel lock is taken under another's.)
    ScopedLock lock(mutex_);
    for (usize slot = 0; slot < notifier_capacity; ++slot) {
        if (notifiers_[slot] != nullptr) (*notifiers_[slot]).signal();
    }
}

void ChannelCore::wake_senders() noexcept { not_full_.notify_one(); }

void ChannelCore::close() noexcept {
    {
        ScopedLock lock(mutex_);
        if (closed_) return;
        closed_ = true;
        for (usize slot = 0; slot < notifier_capacity; ++slot) {
            if (notifiers_[slot] != nullptr) (*notifiers_[slot]).signal();
        }
    }
    not_empty_.notify_all();
    not_full_.notify_all();
}

bool ChannelCore::is_closed() noexcept {
    ScopedLock lock(mutex_);
    const bool result = closed_;
    return result;
}

usize ChannelCore::count() noexcept {
    ScopedLock lock(mutex_);
    const usize result = count_;
    return result;
}

bool ChannelCore::receivable() noexcept {
    ScopedLock lock(mutex_);
    const bool result = count_ != 0 || closed_;
    return result;
}

bool ChannelCore::attach(ChannelNotifier& notifier) noexcept {
    ScopedLock lock(mutex_);
    for (usize slot = 0; slot < notifier_capacity; ++slot) {
        if (notifiers_[slot] == nullptr) {
            notifiers_[slot] = &notifier;
            return true;
        }
    }
    return false;
}

void ChannelCore::detach(ChannelNotifier& notifier) noexcept {
    ScopedLock lock(mutex_);
    for (usize slot = 0; slot < notifier_capacity; ++slot) {
        if (notifiers_[slot] == &notifier) notifiers_[slot] = nullptr;
    }
}

// ---- select --------------------------------------------------------------------

namespace {

// Detaches a select's notifier from the channels it was attached to, on
// every exit (house style, section 2: state set for an operation belongs to
// a scope object).
class SelectAttachment final {
public:
    SelectAttachment(const View<ChannelCore*> channels, ChannelNotifier& notifier) noexcept
        : channels_(channels), notifier_(notifier) {}
    ~SelectAttachment() {
        for (usize index = 0; index < attached_; ++index) (*channels_[index]).detach(notifier_);
    }
    SelectAttachment(const SelectAttachment&) = delete;
    SelectAttachment& operator=(const SelectAttachment&) = delete;

    // Attaches to every channel, in order; false when one refuses (the
    // ones attached so far are detached by the destructor).
    [[nodiscard]] bool attach_all() noexcept {
        for (usize index = 0; index < channels_.count(); ++index) {
            if (!(*channels_[index]).attach(notifier_)) return false;
            attached_ = index + 1;
        }
        return true;
    }

private:
    View<ChannelCore*> channels_;
    ChannelNotifier& notifier_;
    usize attached_{0};
};

} // namespace

SelectResult select_receivable(const View<ChannelCore*> channels, const Context& context) noexcept {
    STXLIB_REQUIRE(!channels.empty(), "select_receivable: no channels");
    for (usize index = 0; index < channels.count(); ++index) {
        STXLIB_REQUIRE(channels[index] != nullptr, "select_receivable: a null channel");
    }
    ChannelNotifier notifier;
    SelectAttachment attachment(channels, notifier);
    if (!attachment.attach_all()) return SelectResult::too_many_selects();
    for (;;) {
        // Read the signal count before looking, so a send after the look
        // is a signal the wait sees.
        const u64 seen = notifier.signals();
        for (usize index = 0; index < channels.count(); ++index) {
            if ((*channels[index]).receivable()) return SelectResult::ready(index);
        }
        if (context.done()) return SelectResult::ended();
        notifier.wait(seen, next_look(&context));
    }
}

// ---- TaskGroup --------------------------------------------------------------------

TaskGroup::~TaskGroup() {
    request_stop();
    join();
}

OperationStatus TaskGroup::start(const Thread::Entry entry, void* const context, const usize stack_bytes) noexcept {
    if (count_ == task_group_capacity) {
        const ErrorRecord full = ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.tasks.start");
        return OperationStatus::failure(full);
    }
    OperationResult<Thread> started = Thread::start(entry, context, stack_bytes);
    if (started.failed()) return started.status();
    threads_[count_] = static_cast<OperationResult<Thread>&&>(started).take_value();
    count_ = count_ + 1;
    return OperationStatus::success();
}

void TaskGroup::request_stop() noexcept {
    for (usize index = 0; index < count_; ++index) threads_[index].request_stop();
}

void TaskGroup::join() noexcept {
    for (usize index = 0; index < count_; ++index) static_cast<void>(threads_[index].join());
    for (usize index = 0; index < count_; ++index) threads_[index] = Thread();
    count_ = 0;
}

} // namespace stxlib
