// Tests of contexts, channels, select and task groups
// (include/stxlib/channels.hpp): bounds and order, close, the try forms,
// dropping the oldest, waits that end with their context (cancel, deadline,
// a parent's end), select across channels, and task groups that join.

#include "stxlib/channels.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>

namespace {

using stxlib::Channel;
using stxlib::ChannelCore;
using stxlib::ChannelStatus;
using stxlib::Context;
using stxlib::ContextEnd;
using stxlib::Duration;
using stxlib::IntervalTime;
using stxlib::SelectResult;
using stxlib::StopSignal;
using stxlib::TaskGroup;
using stxlib::u32;
using stxlib::usize;
using stxlib::View;

void order_and_bounds() {
    Channel<int, 3> channel;
    int value = 1;
    CHECK(channel.try_send(value) == ChannelStatus::delivered);
    value = 2;
    CHECK(channel.try_send(value) == ChannelStatus::delivered);
    value = 3;
    CHECK(channel.try_send(value) == ChannelStatus::delivered);
    value = 4;
    CHECK(channel.try_send(value) == ChannelStatus::full);
    CHECK(channel.count() == 3);
    int out = 0;
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 1);
    value = 4;
    CHECK(channel.try_send(value) == ChannelStatus::delivered); // wraps the ring
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 2);
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 3);
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 4);
    CHECK(channel.try_receive(out) == ChannelStatus::empty);
}

void close_drains_then_reports() {
    Channel<int, 2> channel;
    int value = 7;
    CHECK(channel.send(value) == ChannelStatus::delivered);
    channel.close();
    CHECK(channel.is_closed());
    value = 8;
    CHECK(channel.send(value) == ChannelStatus::closed);
    CHECK(channel.try_send(value) == ChannelStatus::closed);
    int out = 0;
    CHECK(channel.receive(out) == ChannelStatus::delivered && out == 7);
    CHECK(channel.receive(out) == ChannelStatus::closed);
    CHECK(channel.try_receive(out) == ChannelStatus::closed);
    CHECK(channel.receivable());
}

void dropping_oldest() {
    Channel<int, 2> channel;
    bool dropped = true;
    int value = 1;
    CHECK(channel.send_dropping_oldest(value, dropped) == ChannelStatus::delivered && !dropped);
    value = 2;
    CHECK(channel.send_dropping_oldest(value, dropped) == ChannelStatus::delivered && !dropped);
    value = 3;
    CHECK(channel.send_dropping_oldest(value, dropped) == ChannelStatus::delivered && dropped);
    int out = 0;
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 2);
    CHECK(channel.try_receive(out) == ChannelStatus::delivered && out == 3);
}

void context_ends() {
    Context root;
    CHECK(root.end() == ContextEnd::live);
    Context child(Context::ChildTag{}, root);
    Context grandchild(Context::ChildTag{}, child);
    CHECK(grandchild.depth() == 2);
    root.cancel(ContextEnd::shut_down);
    CHECK(child.end() == ContextEnd::shut_down);
    CHECK(grandchild.done());
    // The first end is kept.
    child.cancel(ContextEnd::cancelled);
    CHECK(child.end() == ContextEnd::cancelled); // its own end is nearer than its parent's
    root.cancel(ContextEnd::cancelled);
    CHECK(root.end() == ContextEnd::shut_down);

    const Context passed(IntervalTime::now());
    CHECK(passed.end() == ContextEnd::deadline_passed);
    const Context later(IntervalTime::now().after(Duration::seconds(60)));
    const Context nested(later, IntervalTime::now().after(Duration::seconds(120)));
    CHECK(nested.deadline() == later.deadline()); // the earlier deadline wins
    CHECK(nested.end() == ContextEnd::live);
}

void receive_ends_at_deadline() {
    Channel<int, 1> channel;
    const IntervalTime began = IntervalTime::now();
    const Context brief(began.after(Duration::milliseconds(30)));
    int out = 0;
    CHECK(channel.receive(out, brief) == ChannelStatus::ended);
    CHECK(!(IntervalTime::now() < began.after(Duration::milliseconds(30))));
    int value = 5;
    CHECK(channel.send(value, brief) == ChannelStatus::delivered); // room: no wait, no end
    CHECK(channel.send(value, brief) == ChannelStatus::ended);     // full, and the context ended
}

struct CancelLater final {
    Context* context;
    std::atomic<bool> fired{false};
};

void cancel_after_a_moment(const StopSignal& stop, void* const address) noexcept {
    static_cast<void>(stop);
    CancelLater& later = *static_cast<CancelLater*>(address);
    stxlib::sleep_for(Duration::milliseconds(20));
    later.fired.store(true);
    (*later.context).cancel();
}

void receive_ends_at_cancel() {
    Channel<int, 1> channel;
    Context context;
    CancelLater later{&context};
    TaskGroup group;
    CHECK(group.start(&cancel_after_a_moment, &later).succeeded());
    int out = 0;
    CHECK(channel.receive(out, context) == ChannelStatus::ended);
    CHECK(later.fired.load());
    CHECK(context.end() == ContextEnd::cancelled);
    group.join();
    CHECK(group.count() == 0);
}

struct Producer final {
    Channel<int, 4>* channel;
    int first;
    int count;
};

void produce(const StopSignal& stop, void* const address) noexcept {
    static_cast<void>(stop);
    Producer& producer = *static_cast<Producer*>(address);
    for (int index = 0; index < producer.count; ++index) {
        int value = producer.first + index;
        static_cast<void>((*producer.channel).send(value));
    }
}

void blocking_handoff() {
    Channel<int, 4> channel;
    Producer producer{&channel, 100, 1000};
    TaskGroup group;
    CHECK(group.start(&produce, &producer).succeeded());
    int expected = 100;
    bool ordered = true;
    for (int index = 0; index < 1000; ++index) {
        int out = 0;
        if (channel.receive(out) != ChannelStatus::delivered || out != expected) ordered = false;
        expected = expected + 1;
    }
    CHECK(ordered);
    group.join();
}

void select_finds_the_ready_one() {
    Channel<int, 2> first;
    Channel<int, 2> second;
    ChannelCore* both[2] = {&first, &second};
    const Context brief(IntervalTime::now().after(Duration::milliseconds(20)));
    const SelectResult nothing = stxlib::select_receivable(View<ChannelCore*>(both), brief);
    CHECK(nothing.is_ended());
    int value = 9;
    CHECK(second.send(value) == ChannelStatus::delivered);
    const Context forever;
    const SelectResult ready = stxlib::select_receivable(View<ChannelCore*>(both), forever);
    CHECK(ready.ready_index() != nullptr && *ready.ready_index() == 1);
    first.close();
    const SelectResult closed = stxlib::select_receivable(View<ChannelCore*>(both), forever);
    CHECK(closed.ready_index() != nullptr && *closed.ready_index() == 0); // closed counts as receivable
}

struct LateSender final {
    Channel<int, 2>* channel;
};

void send_after_a_moment(const StopSignal& stop, void* const address) noexcept {
    static_cast<void>(stop);
    LateSender& sender = *static_cast<LateSender*>(address);
    stxlib::sleep_for(Duration::milliseconds(20));
    int value = 11;
    static_cast<void>((*sender.channel).send(value));
}

void select_wakes_on_send() {
    Channel<int, 2> first;
    Channel<int, 2> second;
    ChannelCore* both[2] = {&first, &second};
    LateSender sender{&first};
    TaskGroup group;
    CHECK(group.start(&send_after_a_moment, &sender).succeeded());
    const Context patient(IntervalTime::now().after(Duration::seconds(10)));
    const SelectResult ready = stxlib::select_receivable(View<ChannelCore*>(both), patient);
    CHECK(ready.ready_index() != nullptr && *ready.ready_index() == 0);
    group.join();
}

void count_up(const StopSignal& stop, void* const address) noexcept {
    std::atomic<u32>& total = *static_cast<std::atomic<u32>*>(address);
    while (!stop.requested()) stxlib::sleep_for(Duration::milliseconds(1));
    total.fetch_add(1);
}

void task_group_stops_and_joins() {
    std::atomic<u32> total{0};
    {
        TaskGroup group;
        for (usize index = 0; index < 4; ++index) CHECK(group.start(&count_up, &total).succeeded());
        CHECK(group.count() == 4);
        group.request_stop();
        group.join();
        CHECK(total.load() == 4);
    }
    {
        TaskGroup group;
        CHECK(group.start(&count_up, &total).succeeded());
        // The destructor stops and joins.
    }
    CHECK(total.load() == 5);
}

void task_group_capacity() {
    std::atomic<u32> total{0};
    TaskGroup group;
    for (usize index = 0; index < stxlib::task_group_capacity; ++index) {
        CHECK(group.start(&count_up, &total).succeeded());
    }
    const stxlib::OperationStatus refused = group.start(&count_up, &total);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    group.request_stop();
    group.join();
    CHECK(total.load() == stxlib::task_group_capacity);
}

} // namespace

int main() {
    order_and_bounds();
    close_drains_then_reports();
    dropping_oldest();
    context_ends();
    receive_ends_at_deadline();
    receive_ends_at_cancel();
    blocking_handoff();
    select_finds_the_ready_one();
    select_wakes_on_send();
    task_group_stops_and_joins();
    task_group_capacity();
    return stxlib_tests::finish("channels");
}
