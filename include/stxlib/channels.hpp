#pragma once

// Tasks, channels and contexts: C++ that behaves somewhat like Go (owner
// ruling 1.6 of notes planning/studies/MCP_AUTOMATION_STUDY.md, section 6.2,
// public notes, gui_forms branch). stx_mcp (mcp.hpp) is built on them; any
// STXlib program may use them. Within the house style: no coroutines, no
// lambdas, no type erasure; named entries and named records.
//
// - Tasks: a few long-lived threads with one job each, like goroutines,
//   started and joined together by a TaskGroup. Each owns its own state;
//   what tasks share is channels.
// - Channels: bounded queues of records between tasks. send, receive,
//   try_send, try_receive, close, and select over several channels with a
//   context. Closing a channel is how shutdown spreads: a receiver drains
//   what was sent before the close, then sees `closed`.
// - Contexts: a deadline, a cancellation any thread may raise, and an
//   optional parent whose end is also this context's end, like Go's
//   context.Context. Waits honour them.
// - Errors as values: every wait answers a ChannelStatus.
//
// Waiting for a context. A cancellation is a flag, not a wake-up: a task
// blocked in a channel wait notices it within context_poll_interval, the
// slice every wait sleeps in while its context can still end. A deadline is
// exact (the wait ends at it), a cancellation is seen within one slice. This
// keeps a context free of waiter lists and of locks, so cancel() is safe
// from any thread at any time, and costs one slice of latency, which no
// caller of stx_mcp can observe beside a pipe's.
//
// Locks: each channel has one Mutex, held only around its own ring; no
// channel calls code it does not own while holding it. One nesting exists:
// a channel signals the notifiers of the selects watching it under its own
// lock (a notifier's lock is taken inside a channel's, never the reverse),
// so a select can detach its notifier safely. No channel's lock is taken
// under another channel's.
//
// Records: a channel moves records in and out (Record's move constructor
// and destructor must not throw). A record is owned by the channel from a
// successful send until the receive that moves it out; records left in a
// channel are destroyed with it.

#include "stxlib/clocks.hpp"
#include "stxlib/detail/construct.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// ---- contexts ---------------------------------------------------------------

// Why a context has ended. `live` while it has not.
enum class ContextEnd : u8 {
    live = 0,
    cancelled = 1,       // cancel() on it or on an ancestor
    deadline_passed = 2, // its deadline or an ancestor's has passed
    shut_down = 3,       // its owner is shutting down (a server's input ended or stop)
};

// The slice a wait sleeps in while its context can still be cancelled.
inline constexpr Duration context_poll_interval = Duration::milliseconds(10);
// How many ancestors a context may have. child_of refuses a deeper chain, so
// end() walks at most this many links.
inline constexpr u32 context_depth_limit = 16;

class Context final {
public:
    // A context with no deadline and no parent. It ends only by cancel().
    Context() noexcept = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    // A context that also ends at `deadline` (IntervalTime).
    explicit Context(IntervalTime deadline) noexcept;

    // A context that ends when `parent` ends, at `deadline` if it has one,
    // or by its own cancel(). Borrows `parent`: declare the child after it,
    // so the child is destroyed first. Requirement: the parent's depth is
    // below context_depth_limit (parent.depth() + 1 < context_depth_limit);
    // a caller that builds chains from input checks depth() first.
    Context(const Context& parent, IntervalTime deadline) noexcept;
    // The same without a deadline of its own.
    struct ChildTag final {};
    Context(ChildTag tag, const Context& parent) noexcept;

    // Ends the context with `reason` (not live). The first end is kept:
    // a later cancel, or a deadline that passes after it, changes nothing.
    // Safe from any thread while the context is alive.
    void cancel(ContextEnd reason = ContextEnd::cancelled) noexcept;

    // live, or why the context ended: its own end, else its ancestors'
    // (nearest first), else deadline_passed once IntervalTime::now() has
    // reached a deadline on the chain.
    [[nodiscard]] ContextEnd end() const noexcept;
    [[nodiscard]] bool done() const noexcept { return end() != ContextEnd::live; }

    // The earliest deadline on the chain, or IntervalTime of i64_max when
    // none.
    [[nodiscard]] IntervalTime deadline() const noexcept;
    // Ancestors above this context: 0 for a root.
    [[nodiscard]] u32 depth() const noexcept { return depth_; }

private:
    const Context* parent_{nullptr};
    IntervalTime deadline_{IntervalTime::from_nanoseconds(i64_max)};
    u32 depth_{0};
    // A ContextEnd, written with a compare-exchange from live (the first
    // end is kept) and read with acquire ordering.
    u8 reason_{0};
};

// ---- channels ---------------------------------------------------------------

// What a channel operation did.
enum class ChannelStatus : u8 {
    delivered = 0, // the record was sent or received
    closed = 1,    // send: the channel is closed, the record was not taken; receive: closed and empty
    full = 2,      // try_send: no room; the record was not taken
    empty = 3,     // try_receive: nothing to receive, not closed
    ended = 4,     // the context ended first; nothing was sent or received
};

class ChannelCore;

// A select's wake-up, registered with the channels it watches.
class ChannelNotifier final {
public:
    ChannelNotifier() noexcept = default;
    ChannelNotifier(const ChannelNotifier&) = delete;
    ChannelNotifier& operator=(const ChannelNotifier&) = delete;

    // Raised by a channel after a send or a close (under the channel's
    // lock; the notifier takes only its own).
    void signal() noexcept;
    // Waits until signalled since `seen` (the value signals() returned
    // before the caller checked the channels), or until `until`.
    void wait(u64 seen, IntervalTime until) noexcept;
    [[nodiscard]] u64 signals() noexcept;

private:
    Mutex mutex_{};
    ConditionVariable signalled_{};
    u64 signals_{0};
};

// The part of a channel that does not depend on its record type: the lock,
// the counts, the waits and the notifiers. Channel<Record, Capacity> keeps
// the ring. Not used alone.
class ChannelCore {
public:
    ChannelCore(const ChannelCore&) = delete;
    ChannelCore& operator=(const ChannelCore&) = delete;
    ChannelCore(ChannelCore&&) = delete;
    ChannelCore& operator=(ChannelCore&&) = delete;

    // The most selects that may watch one channel at once.
    static constexpr usize notifier_capacity = 4;

    // Closes the channel: later sends answer closed; receives drain what is
    // in it, then answer closed. Closing twice changes nothing. Wakes every
    // waiter and notifier.
    void close() noexcept;
    [[nodiscard]] bool is_closed() noexcept;
    // Records now in the channel (a snapshot).
    [[nodiscard]] usize count() noexcept;
    // True when a receive would not wait: a record is in the channel, or it
    // is closed.
    [[nodiscard]] bool receivable() noexcept;

    // For select: registers or removes a notifier. register fails (false)
    // when notifier_capacity selects already watch this channel.
    [[nodiscard]] bool attach(ChannelNotifier& notifier) noexcept;
    void detach(ChannelNotifier& notifier) noexcept;

protected:
    explicit ChannelCore(const usize ring_capacity) noexcept : capacity_(ring_capacity) {}
    ~ChannelCore() = default;

    // Under mutex_. The slot a send writes, or a receive reads.
    [[nodiscard]] usize write_slot() const noexcept;
    [[nodiscard]] usize read_slot() const noexcept { return head_; }

    // Waits under `lock` until a send can take a record (room, or closed),
    // or `context` ends. Returns delivered when there is room, closed, or
    // ended. A null context waits without one.
    [[nodiscard]] ChannelStatus wait_for_room(ScopedLock& lock, const Context* context) noexcept;
    // Waits until a record is present, the channel is closed and empty, or
    // the context ends. delivered means a record is present.
    [[nodiscard]] ChannelStatus wait_for_record(ScopedLock& lock, const Context* context) noexcept;
    // Bookkeeping after a record was constructed at write_slot() / moved out
    // of read_slot(), under the lock.
    void note_sent() noexcept;
    void note_received() noexcept;
    // After the lock is released: wakes the other side and the notifiers.
    void wake_receivers() noexcept;
    void wake_senders() noexcept;
    [[nodiscard]] bool has_room() const noexcept { return count_ < capacity_; }
    [[nodiscard]] bool has_record() const noexcept { return count_ != 0; }
    [[nodiscard]] bool closed_locked() const noexcept { return closed_; }

    Mutex mutex_{};

private:
    ConditionVariable not_empty_{};
    ConditionVariable not_full_{};
    usize capacity_;
    usize head_{0};
    usize count_{0};
    bool closed_{false};
    ChannelNotifier* notifiers_[notifier_capacity]{};
};

// A bounded channel of up to Capacity records, held inline (no allocation;
// a channel's size is Capacity * sizeof(Record) plus its core). Not movable:
// tasks share it by reference, and its owner outlives them.
template <typename Record, usize Capacity>
class Channel final : public ChannelCore {
    static_assert(Capacity > 0, "a channel holds at least one record");
    static_assert(__is_nothrow_constructible(Record, Record&&), "a channel moves records without a failure path");
    static_assert(detail::nothrow_destructible<Record>, "a channel destroys records without a failure path");

public:
    Channel() noexcept : ChannelCore(Capacity) {}
    ~Channel() {
        // Destroys the records still in the ring, oldest first. No task
        // uses the channel any more (its owner outlives them).
        ScopedLock lock(mutex_);
        while (has_record()) {
            Record* const slot = slot_at(read_slot());
            detail::destroy(slot);
            note_received();
        }
    }

    // Sends `record`, waiting while the channel is full. delivered: the
    // channel owns the record (moved from); closed or ended: not taken.
    [[nodiscard]] ChannelStatus send(Record& record) noexcept { return send_with(record, nullptr); }
    [[nodiscard]] ChannelStatus send(Record& record, const Context& context) noexcept {
        return send_with(record, &context);
    }
    // delivered, full or closed; never waits.
    [[nodiscard]] ChannelStatus try_send(Record& record) noexcept {
        ChannelStatus status = ChannelStatus::delivered;
        {
            ScopedLock lock(mutex_);
            if (closed_locked()) {
                status = ChannelStatus::closed;
            } else if (!has_room()) {
                status = ChannelStatus::full;
            } else {
                place(record);
            }
        }
        if (status == ChannelStatus::delivered) wake_receivers();
        return status;
    }
    // Sends without waiting; when full, the oldest record is destroyed to
    // make room (a notification nobody will miss more than the newest).
    // delivered (and `dropped` says whether one was dropped) or closed.
    [[nodiscard]] ChannelStatus send_dropping_oldest(Record& record, bool& dropped) noexcept {
        dropped = false;
        ChannelStatus status = ChannelStatus::delivered;
        {
            ScopedLock lock(mutex_);
            if (closed_locked()) {
                status = ChannelStatus::closed;
            } else {
                if (!has_room()) {
                    Record* const oldest = slot_at(read_slot());
                    detail::destroy(oldest);
                    note_received();
                    dropped = true;
                }
                place(record);
            }
        }
        if (status == ChannelStatus::delivered) wake_receivers();
        return status;
    }

    // Receives the oldest record into `out` (move-assigned), waiting while
    // the channel is empty and open. delivered, closed (closed and empty) or
    // ended; `out` is unchanged unless delivered.
    [[nodiscard]] ChannelStatus receive(Record& out) noexcept { return receive_with(out, nullptr); }
    [[nodiscard]] ChannelStatus receive(Record& out, const Context& context) noexcept {
        return receive_with(out, &context);
    }
    // delivered, empty or closed; never waits.
    [[nodiscard]] ChannelStatus try_receive(Record& out) noexcept {
        ChannelStatus status = ChannelStatus::delivered;
        {
            ScopedLock lock(mutex_);
            if (has_record()) {
                take(out);
            } else if (closed_locked()) {
                status = ChannelStatus::closed;
            } else {
                status = ChannelStatus::empty;
            }
        }
        if (status == ChannelStatus::delivered) wake_senders();
        return status;
    }

private:
    [[nodiscard]] Record* slot_at(const usize index) noexcept {
        Record* const result = reinterpret_cast<Record*>(storage_ + index * sizeof(Record));
        return result;
    }
    // Under the lock, with room.
    void place(Record& record) noexcept {
        detail::construct<Record>(slot_at(write_slot()), static_cast<Record&&>(record));
        note_sent();
    }
    // Under the lock, with a record.
    void take(Record& out) noexcept {
        Record* const slot = slot_at(read_slot());
        out = static_cast<Record&&>(*slot);
        detail::destroy(slot);
        note_received();
    }
    [[nodiscard]] ChannelStatus send_with(Record& record, const Context* const context) noexcept {
        ChannelStatus status = ChannelStatus::delivered;
        {
            ScopedLock lock(mutex_);
            status = wait_for_room(lock, context);
            if (status == ChannelStatus::delivered) place(record);
        }
        if (status == ChannelStatus::delivered) wake_receivers();
        return status;
    }
    [[nodiscard]] ChannelStatus receive_with(Record& out, const Context* const context) noexcept {
        static_assert(__is_nothrow_assignable(Record&, Record&&), "receive move-assigns into the caller's record");
        ChannelStatus status = ChannelStatus::delivered;
        {
            ScopedLock lock(mutex_);
            status = wait_for_record(lock, context);
            if (status == ChannelStatus::delivered) take(out);
        }
        if (status == ChannelStatus::delivered) wake_senders();
        return status;
    }

    // Capacity records' raw storage; slot i is live while it lies in the
    // ring [head, head + count) modulo Capacity.
    alignas(Record) unsigned char storage_[Capacity * sizeof(Record)];
};

// ---- select -----------------------------------------------------------------

// The outcome of a select: which channel is receivable, or why none is.
class SelectResult final {
public:
    [[nodiscard]] static SelectResult ready(const usize channel_index) noexcept {
        const SelectResult result(Kind::ready, channel_index);
        return result;
    }
    [[nodiscard]] static SelectResult ended() noexcept {
        const SelectResult result(Kind::ended, 0);
        return result;
    }
    // More selects watch one of the channels than it admits
    // (ChannelCore::notifier_capacity): a program defect, reported.
    [[nodiscard]] static SelectResult too_many_selects() noexcept {
        const SelectResult result(Kind::too_many_selects, 0);
        return result;
    }

    [[nodiscard]] bool is_ended() const noexcept { return kind_ == Kind::ended; }
    [[nodiscard]] bool is_too_many_selects() const noexcept { return kind_ == Kind::too_many_selects; }
    // The index into the select's channels of the first receivable one, or
    // null in another state. Borrowed from this value.
    [[nodiscard]] const usize* ready_index() const& noexcept {
        if (kind_ != Kind::ready) return nullptr;
        return &index_;
    }
    const usize* ready_index() const&& = delete;

private:
    enum class Kind : u8 {
        ready = 0,
        ended = 1,
        too_many_selects = 2,
    };

    SelectResult(const Kind result_kind, const usize index) noexcept : kind_(result_kind), index_(index) {}

    Kind kind_;
    usize index_;
};

// Waits until one of `channels` is receivable (a record, or closed), in the
// order given: the first receivable one is reported, so earlier channels
// have priority. Ends when `context` ends. Readiness is a snapshot: another
// receiver may take the record first, so the caller try_receives the ready
// channel and selects again when it answers empty. Requirement: channels is
// not empty and holds no null pointer.
[[nodiscard]] SelectResult select_receivable(View<ChannelCore*> channels, const Context& context) noexcept;

// ---- tasks ------------------------------------------------------------------

// The most tasks one group holds.
inline constexpr usize task_group_capacity = 16;

// A group of tasks started together and joined together (Go's goroutines
// with a sync.WaitGroup). Destruction requests every task's stop and joins
// them all. Each task's context is borrowed until it is joined: declare the
// group after what its tasks' contexts point to.
class TaskGroup final {
public:
    TaskGroup() noexcept = default;
    ~TaskGroup();
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    // Starts one task running `entry(stop, context)`. Failures: Thread::start's;
    // limit_exceeded when task_group_capacity tasks are in the group.
    [[nodiscard]] OperationStatus start(Thread::Entry entry, void* context,
                                        usize stack_bytes = default_thread_stack_bytes) noexcept;
    // Asks every task to stop (each sees its StopSignal); waits for none.
    void request_stop() noexcept;
    // Waits for every task's entry to return. The group is then empty.
    void join() noexcept;
    // Tasks started and not yet joined.
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    Thread threads_[task_group_capacity]{};
    usize count_{0};
};

} // namespace stxlib
