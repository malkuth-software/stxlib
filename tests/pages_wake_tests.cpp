// Tests of sealed pages (pages.hpp) and the wake channel (wake.hpp), A1
// platform-rest. Tested, not proved: both are thin host wrappers. What a
// write to a sealed page does is a must-end case
// (tests/platform_rest_death_tests.cpp).

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/pages.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/wake.hpp"

#include "test_support.hpp"

namespace {

using stxlib::Byte;
using stxlib::Duration;
using stxlib::ErrorKind;
using stxlib::HostCode;
using stxlib::i64;
using stxlib::IntervalTime;
using stxlib::PageBlock;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::usize;
using stxlib::WakeChannel;
using stxlib::WakeWait;

template <typename T>
ErrorKind kind_of(const OperationResult<T>& result) {
    const ErrorKind kind = result.failed() ? (*result.error()).kind() : ErrorKind::internal_failure;
    return kind;
}

template <typename T>
HostCode code_of(const OperationResult<T>& result) {
    const HostCode code = result.failed() ? static_cast<HostCode>((*result.error()).code()) : HostCode::none;
    return code;
}

HostCode status_code(const OperationStatus& status) {
    const HostCode code = status.failed() ? static_cast<HostCode>((*status.error()).code()) : HostCode::none;
    return code;
}

// ---- pages -------------------------------------------------------------------

void page_size_is_a_power_of_two() {
    const usize size = stxlib::page_size();
    CHECK(size >= 4096);
    CHECK((size & (size - 1)) == 0);
}

void empty_block() {
    OperationResult<PageBlock> mapped = PageBlock::map(0);
    CHECK(mapped.succeeded());
    if (mapped.failed()) return;
    PageBlock block = static_cast<OperationResult<PageBlock>&&>(mapped).take_value();
    CHECK(block.count() == 0);
    CHECK(block.mapped_bytes() == 0);
    CHECK(block.bytes().empty());
    CHECK(!block.sealed());
    CHECK(block.seal().succeeded());
    CHECK(block.sealed());

    PageBlock never_mapped;
    CHECK(never_mapped.count() == 0);
    CHECK(never_mapped.bytes().empty());
}

void map_write_seal() {
    CHECK(stxlib::capability_supported(stxlib::Capability::page_protection));
    const usize page = stxlib::page_size();
    const usize asked = page + 100;
    OperationResult<PageBlock> mapped = PageBlock::map(asked);
    CHECK(mapped.succeeded());
    if (mapped.failed()) return;
    PageBlock block = static_cast<OperationResult<PageBlock>&&>(mapped).take_value();
    CHECK(block.count() == asked);
    CHECK(block.mapped_bytes() == 2 * page);
    CHECK(!block.sealed());

    // Zeroed by the host.
    bool zero = true;
    for (usize index = 0; index < block.count(); ++index) zero = zero && block.bytes()[index] == Byte{0};
    CHECK(zero);

    const stxlib::MutableView<Byte> writable = block.writable();
    CHECK(writable.count() == asked);
    for (usize index = 0; index < writable.count(); ++index) writable[index] = static_cast<Byte>(index & 0xff);

    CHECK(block.seal().succeeded());
    CHECK(block.sealed());
    bool kept = true;
    for (usize index = 0; index < block.count(); ++index) {
        kept = kept && block.bytes()[index] == static_cast<Byte>(index & 0xff);
    }
    CHECK(kept);

    // Sealing twice is refused; the block stays sealed.
    const OperationStatus again = block.seal();
    CHECK(again.failed());
    CHECK((*again.error()).kind() == ErrorKind::invalid_argument);
    CHECK(status_code(again) == HostCode::sealed);
    CHECK(block.sealed());

    // Moves hand the pages on.
    const Byte* const first = block.bytes().data();
    PageBlock moved = static_cast<PageBlock&&>(block);
    CHECK(moved.bytes().data() == first);
    CHECK(moved.sealed());
    CHECK(block.count() == 0 && block.mapped_bytes() == 0 && !block.sealed());
    PageBlock assigned;
    assigned = static_cast<PageBlock&&>(moved);
    CHECK(assigned.bytes().data() == first);
    CHECK(moved.bytes().empty());
}

void map_refuses_overflow() {
    const usize largest = ~static_cast<usize>(0);
    OperationResult<PageBlock> overflow = PageBlock::map(largest);
    CHECK(overflow.failed());
    CHECK(kind_of(overflow) == ErrorKind::limit_exceeded);

    // A size the host cannot give (a quarter of the address space).
    OperationResult<PageBlock> refused = PageBlock::map(largest / 4);
    CHECK(refused.failed());
    CHECK(kind_of(refused) == ErrorKind::out_of_memory || kind_of(refused) == ErrorKind::limit_exceeded);
}

// ---- wake channel ---------------------------------------------------------------

OperationResult<WakeChannel> created() {
    OperationResult<WakeChannel> channel = WakeChannel::create();
    CHECK(channel.succeeded());
    return channel;
}

void wake_and_drain() {
    CHECK(stxlib::capability_supported(stxlib::Capability::wake_channel));
    OperationResult<WakeChannel> made = created();
    if (made.failed()) return;
    WakeChannel channel = static_cast<OperationResult<WakeChannel>&&>(made).take_value();
    CHECK(channel.is_open());
    CHECK(channel.native_handle() != -1);
    CHECK(!channel.drain());

    channel.wake();
    CHECK(channel.drain());
    CHECK(!channel.drain());

    // Many wakes collapse into one raised flag (a full pipe is a wake).
    for (int index = 0; index < 100000; ++index) channel.wake();
    CHECK(channel.drain());
    CHECK(!channel.drain());
}

void wait_states() {
    OperationResult<WakeChannel> made = created();
    if (made.failed()) return;
    WakeChannel channel = static_cast<OperationResult<WakeChannel>&&>(made).take_value();

    // Nothing raised: a poll times out at once, a short wait after its time.
    OperationResult<WakeWait> polled = channel.wait(stxlib::View<i64>(), Duration::nanoseconds(0));
    CHECK(polled.succeeded() && (*polled.value()).is_timed_out());
    OperationResult<WakeWait> negative = channel.wait(stxlib::View<i64>(), Duration::milliseconds(-5));
    CHECK(negative.succeeded() && (*negative.value()).is_timed_out());

    const IntervalTime before = IntervalTime::now();
    OperationResult<WakeWait> waited = channel.wait(stxlib::View<i64>(), Duration::milliseconds(30));
    const Duration taken = IntervalTime::now().since(before);
    CHECK(waited.succeeded() && (*waited.value()).is_timed_out());
    CHECK(taken >= Duration::milliseconds(25));
    CHECK((*waited.value()).ready_handle() == nullptr);

    // Raised: the wait reports it and leaves it raised.
    channel.wake();
    OperationResult<WakeWait> woken = channel.wait(stxlib::View<i64>(), Duration::seconds(5));
    CHECK(woken.succeeded() && (*woken.value()).is_woken());
    OperationResult<WakeWait> still = channel.wait(stxlib::View<i64>(), Duration::nanoseconds(0));
    CHECK(still.succeeded() && (*still.value()).is_woken());
    CHECK(channel.drain());
}

// The other thread's entry: raises the channel after 20 ms.
void wake_later(const stxlib::StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    const WakeChannel& channel = *static_cast<const WakeChannel*>(context);
    stxlib::sleep_for(Duration::milliseconds(20));
    channel.wake();
}

void wake_from_another_thread() {
    OperationResult<WakeChannel> made = created();
    if (made.failed()) return;
    WakeChannel channel = static_cast<OperationResult<WakeChannel>&&>(made).take_value();
    OperationResult<stxlib::Thread> started = stxlib::Thread::start(wake_later, &channel);
    CHECK(started.succeeded());
    if (started.failed()) return;
    stxlib::Thread waker = static_cast<OperationResult<stxlib::Thread>&&>(started).take_value();
    const IntervalTime before = IntervalTime::now();
    OperationResult<WakeWait> woken = channel.wait(stxlib::View<i64>(), Duration::seconds(30));
    const Duration taken = IntervalTime::now().since(before);
    const stxlib::JoinResult joined = waker.join();
    static_cast<void>(joined);
    CHECK(woken.succeeded() && (*woken.value()).is_woken());
    CHECK(taken < Duration::seconds(20));
    CHECK(channel.drain());
}

void handle_ready() {
    OperationResult<WakeChannel> made_waiter = created();
    OperationResult<WakeChannel> made_other = created();
    if (made_waiter.failed() || made_other.failed()) return;
    WakeChannel waiter = static_cast<OperationResult<WakeChannel>&&>(made_waiter).take_value();
    WakeChannel other = static_cast<OperationResult<WakeChannel>&&>(made_other).take_value();

    // Another channel's handle, watched by this channel's wait.
    const i64 handles[2] = {other.native_handle(), other.native_handle()};
    OperationResult<WakeWait> quiet = waiter.wait(stxlib::View<i64>(handles), Duration::nanoseconds(0));
    CHECK(quiet.succeeded() && (*quiet.value()).is_timed_out());

    other.wake();
    OperationResult<WakeWait> ready = waiter.wait(stxlib::View<i64>(handles), Duration::seconds(5));
    CHECK(ready.succeeded());
    if (ready.succeeded()) {
        const usize* const index = (*ready.value()).ready_handle();
        CHECK(index != nullptr);
        if (index != nullptr) CHECK(*index == 0);
    }

    // The channel itself is reported before the handles.
    waiter.wake();
    OperationResult<WakeWait> both = waiter.wait(stxlib::View<i64>(handles), Duration::seconds(5));
    CHECK(both.succeeded() && (*both.value()).is_woken());
    CHECK(waiter.drain());
    CHECK(other.drain());
}

void too_many_handles() {
    OperationResult<WakeChannel> made = created();
    if (made.failed()) return;
    WakeChannel channel = static_cast<OperationResult<WakeChannel>&&>(made).take_value();
    i64 handles[stxlib::maximum_wake_wait_handles + 1];
    for (usize index = 0; index < stxlib::maximum_wake_wait_handles + 1; ++index) {
        handles[index] = channel.native_handle();
    }
    OperationResult<WakeWait> refused = channel.wait(stxlib::View<i64>(handles), Duration::nanoseconds(0));
    CHECK(refused.failed());
    CHECK(kind_of(refused) == ErrorKind::limit_exceeded);
    CHECK(code_of(refused) == HostCode::too_many_handles);

    // The most handles allowed is accepted.
    OperationResult<WakeWait> most =
        channel.wait(stxlib::View<i64>(handles, stxlib::maximum_wake_wait_handles), Duration::nanoseconds(0));
    CHECK(most.succeeded() && (*most.value()).is_timed_out());
}

void closed_channel() {
    WakeChannel closed;
    CHECK(!closed.is_open());
    CHECK(closed.native_handle() == -1);
    closed.wake();
    CHECK(!closed.drain());
    OperationResult<WakeWait> refused = closed.wait(stxlib::View<i64>(), Duration::nanoseconds(0));
    CHECK(kind_of(refused) == ErrorKind::disposed);
    CHECK(code_of(refused) == HostCode::closed_handle);

    OperationResult<WakeChannel> made = created();
    if (made.failed()) return;
    WakeChannel channel = static_cast<OperationResult<WakeChannel>&&>(made).take_value();
    const i64 handle = channel.native_handle();
    WakeChannel moved = static_cast<WakeChannel&&>(channel);
    CHECK(!channel.is_open());
    CHECK(moved.native_handle() == handle);
    channel.wake();
    moved.wake();
    CHECK(moved.drain());
    WakeChannel assigned;
    assigned = static_cast<WakeChannel&&>(moved);
    CHECK(assigned.is_open() && !moved.is_open());
}

} // namespace

int main() {
    page_size_is_a_power_of_two();
    empty_block();
    map_write_seal();
    map_refuses_overflow();
    wake_and_drain();
    wait_states();
    wake_from_another_thread();
    handle_ready();
    too_many_handles();
    closed_channel();
    const int status = stxlib_tests::finish("stxlib_pages_wake_tests");
    return status;
}
