// Tests of threads and synchronization (include/stxlib/threads.hpp), ported
// from stx_auxlib's tests/threads_tests.cpp at b6e330a, with owner decision
// D11's stack sizes and predicate waits. The trapped lock defects are
// must-end cases in death_tests.cpp. Tested, not proved (proofs/REPORT.md):
// every check here is one schedule the host chose.

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace {

using stxlib::ConditionVariable;
using stxlib::Duration;
using stxlib::JoinResult;
using stxlib::IntervalTime;
using stxlib::Mutex;
using stxlib::OnceFlag;
using stxlib::Result;
using stxlib::ScopedLock;
using stxlib::StopSignal;
using stxlib::Thread;
using stxlib::ThreadIdentity;
using stxlib::WaitResult;

// Starts a thread; a failed start is a failed check and an empty Thread.
Thread start_or_fail(const Thread::Entry entry, void* const context) {
    Result<Thread> started = Thread::start(entry, context);
    CHECK(started.succeeded());
    if (started.failed()) return Thread();
    Thread result = std::move(started).take_value();
    return result;
}

// ---- Identities ---------------------------------------------------------------

struct IdentityReport final {
    ThreadIdentity seen{};
    ThreadIdentity seen_again{};
};

void report_identity(const StopSignal&, void* const address) noexcept {
    IdentityReport& report = *static_cast<IdentityReport*>(address);
    report.seen = stxlib::current_thread();
    report.seen_again = stxlib::current_thread();
}

void identities() {
    const ThreadIdentity none{};
    CHECK(!none.present() && none.value() == 0);
    const ThreadIdentity main_thread = stxlib::current_thread();
    CHECK(main_thread.present());
    CHECK(main_thread == stxlib::current_thread());

    // Sequential threads: each identity is new, never one an ended thread had.
    std::vector<stxlib::u64> issued{main_thread.value()};
    for (int index = 0; index < 64; ++index) {
        IdentityReport report{};
        Thread thread = start_or_fail(report_identity, &report);
        const ThreadIdentity owner_view = thread.identity();
        CHECK(owner_view.present());
        CHECK(thread.join() == JoinResult::joined);
        CHECK(report.seen == owner_view);
        CHECK(report.seen == report.seen_again);
        bool fresh = true;
        for (const stxlib::u64 earlier : issued) fresh = fresh && earlier != report.seen.value();
        CHECK(fresh);
        issued.push_back(report.seen.value());
        CHECK(!thread.identity().present());
    }
}

// ---- Thread lifetime -----------------------------------------------------------

struct StopReport final {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};
};

void run_until_stopped(const StopSignal& stop, void* const address) noexcept {
    StopReport& report = *static_cast<StopReport*>(address);
    report.entered.store(true, std::memory_order_release);
    while (!stop.requested()) stxlib::yield_thread();
    report.left.store(true, std::memory_order_release);
}

struct SelfJoin final {
    std::atomic<Thread*> thread{nullptr};
    std::atomic<int> result{-1};
};

void join_self(const StopSignal&, void* const address) noexcept {
    SelfJoin& context = *static_cast<SelfJoin*>(address);
    Thread* thread = context.thread.load(std::memory_order_acquire);
    while (thread == nullptr) {
        stxlib::yield_thread();
        thread = context.thread.load(std::memory_order_acquire);
    }
    const JoinResult result = (*thread).join();
    context.result.store(static_cast<int>(result), std::memory_order_release);
}

void thread_lifetime() {
    const Result<Thread> rejected = Thread::start(nullptr, nullptr);
    CHECK(rejected.failed());
    CHECK(rejected.failed() && (*rejected.error()).kind() == stxlib::ErrorKind::invalid_argument &&
          (*rejected.error()).code() == static_cast<stxlib::u32>(stxlib::ThreadFailure::no_entry));
    Thread empty{};
    CHECK(empty.join() == JoinResult::not_joinable);
    CHECK(!empty.identity().present());
    empty.request_stop();

    // Destruction requests a stop and joins.
    StopReport report{};
    {
        const Thread thread = start_or_fail(run_until_stopped, &report);
        while (!report.entered.load(std::memory_order_acquire)) stxlib::yield_thread();
    }
    CHECK(report.left.load(std::memory_order_acquire));

    // Moving transfers the thread; move assignment finishes the target's own.
    StopReport first{};
    StopReport second{};
    Thread a = start_or_fail(run_until_stopped, &first);
    Thread b = start_or_fail(run_until_stopped, &second);
    const ThreadIdentity b_identity = b.identity();
    Thread moved(std::move(a));
    CHECK(!a.identity().present()); // NOLINT(bugprone-use-after-move): the moved-from state is specified
    moved = std::move(b);
    CHECK(first.left.load(std::memory_order_acquire));
    CHECK(moved.identity() == b_identity);
    moved.request_stop();
    CHECK(moved.join() == JoinResult::joined);
    CHECK(moved.join() == JoinResult::not_joinable);
    CHECK(second.left.load(std::memory_order_acquire));

    // A thread cannot join itself.
    SelfJoin self{};
    Thread joiner = start_or_fail(join_self, &self);
    self.thread.store(&joiner, std::memory_order_release);
    while (self.result.load(std::memory_order_acquire) < 0) stxlib::yield_thread();
    CHECK(self.result.load() == static_cast<int>(JoinResult::self_join));
    CHECK(joiner.join() == JoinResult::joined);
}

// ---- Mutual exclusion ----------------------------------------------------------

struct Counter final {
    Mutex mutex{};
    stxlib::u64 value{0};
};

void count_up(const StopSignal&, void* const address) noexcept {
    Counter& counter = *static_cast<Counter*>(address);
    for (int step = 0; step < 20000; ++step) {
        const ScopedLock lock(counter.mutex);
        counter.value = counter.value + 1;
    }
}

struct Holder final {
    Mutex* mutex{nullptr};
    std::atomic<bool> other_acquired{true};
};

void try_held(const StopSignal&, void* const address) noexcept {
    Holder& holder = *static_cast<Holder*>(address);
    const bool acquired = (*holder.mutex).try_lock();
    if (acquired) (*holder.mutex).unlock();
    holder.other_acquired.store(acquired, std::memory_order_release);
}

void mutual_exclusion() {
    Counter counter{};
    {
        std::vector<Thread> threads{};
        for (int index = 0; index < 8; ++index) threads.push_back(start_or_fail(count_up, &counter));
    }
    CHECK(counter.value == 8 * 20000);

    Mutex mutex{};
    Holder holder{};
    holder.mutex = &mutex;
    {
        const ScopedLock lock(mutex);
        Thread thread = start_or_fail(try_held, &holder);
        CHECK(thread.join() == JoinResult::joined);
    }
    CHECK(!holder.other_acquired.load(std::memory_order_acquire));
    CHECK(mutex.try_lock());
    mutex.unlock();
}

// ---- Conditions ----------------------------------------------------------------

struct Handshake final {
    Mutex mutex{};
    ConditionVariable changed{};
    int stage{0}; // guarded by mutex
};

bool at_stage_one(const void* const address) noexcept {
    const bool result = (*static_cast<const Handshake*>(address)).stage == 1;
    return result;
}

bool at_stage_two(const void* const address) noexcept {
    const bool result = (*static_cast<const Handshake*>(address)).stage == 2;
    return result;
}

bool never_true(const void* const address) noexcept {
    static_cast<void>(address);
    return false;
}

bool always_true(const void* const address) noexcept {
    static_cast<void>(address);
    return true;
}

void answer_handshake(const StopSignal&, void* const address) noexcept {
    Handshake& handshake = *static_cast<Handshake*>(address);
    ScopedLock lock(handshake.mutex);
    handshake.changed.wait(lock, at_stage_one, &handshake);
    handshake.stage = 2;
    handshake.changed.notify_all();
}

void conditions() {
    Handshake handshake{};
    Thread thread = start_or_fail(answer_handshake, &handshake);
    {
        ScopedLock lock(handshake.mutex);
        handshake.stage = 1;
        handshake.changed.notify_all();
        // An effectively unbounded wait: the interval saturates the deadline.
        const WaitResult result = handshake.changed.wait_for(lock, at_stage_two, &handshake, Duration::longest());
        CHECK(result == WaitResult::satisfied);
        CHECK(handshake.stage == 2);
    }
    CHECK(thread.join() == JoinResult::joined);

    // Timeouts with nobody notifying: timed_out only once the deadline has
    // passed on IntervalTime.
    Mutex mutex{};
    ConditionVariable never{};
    ScopedLock lock(mutex);
    const IntervalTime before = IntervalTime::now();
    CHECK(never.wait_for(lock, never_true, nullptr, Duration::milliseconds(20)) == WaitResult::timed_out);
    CHECK(IntervalTime::now().since(before) >= Duration::milliseconds(20));
    const IntervalTime past = before.after(Duration::seconds(-1));
    CHECK(never.wait_until(lock, never_true, nullptr, past) == WaitResult::timed_out);
    CHECK(never.wait_for(lock, never_true, nullptr, Duration::seconds(-1)) == WaitResult::timed_out);
    CHECK(never.wait_for(lock, never_true, nullptr, Duration::nanoseconds(stxlib::i64_min)) == WaitResult::timed_out);
    // A true predicate returns at once, even with a past deadline.
    CHECK(never.wait_until(lock, always_true, nullptr, past) == WaitResult::satisfied);
    never.wait(lock, always_true, nullptr);
}

// ---- Stacks (D11) --------------------------------------------------------------

// Touches most of a stack of `bytes` in one frame chain, so a stack smaller
// than asked would overflow into the guard page.
struct StackProbe final {
    stxlib::usize bytes{0};
    std::atomic<bool> finished{false};
};

// Recursion bounded by its byte argument (house style §1; audit A, T1): each
// level consumes at least sizeof frame of `remaining`, and the caller passes
// three quarters of the thread's stack.
[[gnu::noinline]] stxlib::u32 use_stack(const stxlib::usize remaining) noexcept {
    volatile unsigned char frame[16384];
    for (stxlib::usize index = 0; index < sizeof frame; index += 512) frame[index] = static_cast<unsigned char>(index);
    if (remaining <= sizeof frame) return frame[0];
    const stxlib::u32 deeper = use_stack(remaining - sizeof frame);
    return deeper + static_cast<stxlib::u32>(frame[512]);
}

void probe_stack(const StopSignal&, void* const address) noexcept {
    StackProbe& probe = *static_cast<StackProbe*>(address);
    // Three quarters of the stack: room left for the frames' own overhead.
    static_cast<void>(use_stack(probe.bytes / 4 * 3));
    probe.finished.store(true, std::memory_order_release);
}

void stacks() {
    CHECK(stxlib::default_thread_stack_bytes == 4194304);
    // The default stack holds three quarters of 4 MiB of frames on every
    // host (musl's own default is 128 KiB, macOS's 512 KiB).
    StackProbe default_probe{};
    default_probe.bytes = stxlib::default_thread_stack_bytes;
    {
        Thread thread = start_or_fail(probe_stack, &default_probe);
        CHECK(thread.join() == JoinResult::joined);
    }
    CHECK(default_probe.finished.load(std::memory_order_acquire));

    // A stated size, not a multiple of any page size, is rounded up.
    StackProbe stated{};
    stated.bytes = 8 * 1048576 + 1;
    {
        Result<Thread> started = Thread::start(probe_stack, &stated, stated.bytes);
        CHECK(started.succeeded());
        if (started.succeeded()) {
            Thread thread = std::move(started).take_value();
            CHECK(thread.join() == JoinResult::joined);
        }
    }
    CHECK(stated.finished.load(std::memory_order_acquire));

    // Sizes outside D11's bounds are refused before any thread starts.
    const stxlib::usize refused_sizes[] = {0, stxlib::minimum_thread_stack_bytes - 1,
                                           stxlib::maximum_thread_stack_bytes + 1};
    for (const stxlib::usize size : refused_sizes) {
        const Result<Thread> refused = Thread::start(probe_stack, &stated, size);
        CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument &&
              (*refused.error()).code() == static_cast<stxlib::u32>(stxlib::ThreadFailure::stack_size_out_of_range));
    }
}

// ---- Once ----------------------------------------------------------------------

struct OnceState final {
    OnceFlag flag{};
    std::atomic<int> calls{0};
    int value{0}; // written once inside call_once; read after it returns
    std::atomic<int> wrong_reads{0};
};

void initialize_once(void* const address) noexcept {
    OnceState& state = *static_cast<OnceState*>(address);
    state.calls.fetch_add(1, std::memory_order_relaxed);
    stxlib::sleep_for(Duration::milliseconds(5));
    state.value = 42;
}

void call_initializer(const StopSignal&, void* const address) noexcept {
    OnceState& state = *static_cast<OnceState*>(address);
    stxlib::call_once(state.flag, initialize_once, &state);
    if (state.value != 42) state.wrong_reads.fetch_add(1, std::memory_order_relaxed);
}

void once() {
    OnceState state{};
    CHECK(!state.flag.done());
    {
        std::vector<Thread> threads{};
        for (int index = 0; index < 8; ++index) threads.push_back(start_or_fail(call_initializer, &state));
    }
    stxlib::call_once(state.flag, initialize_once, &state);
    CHECK(state.flag.done());
    CHECK(state.calls.load() == 1);
    CHECK(state.wrong_reads.load() == 0);
}

// ---- Sleeping and the host -----------------------------------------------------

void sleeping_and_host() {
    const IntervalTime before = IntervalTime::now();
    stxlib::sleep_for(Duration::milliseconds(15));
    const IntervalTime after = IntervalTime::now();
    CHECK(after.since(before) >= Duration::milliseconds(15));
    stxlib::sleep_until(after.after(Duration::milliseconds(10)));
    CHECK(IntervalTime::now().since(after) >= Duration::milliseconds(10));
    const IntervalTime quick = IntervalTime::now();
    stxlib::sleep_for(Duration::seconds(-5));
    stxlib::sleep_for(Duration::nanoseconds(stxlib::i64_min));
    stxlib::sleep_until(quick.after(Duration::seconds(-5)));
    CHECK(IntervalTime::now().since(quick) < Duration::seconds(1));
    CHECK(stxlib::processor_count() >= 1);
    // The same count as libc++'s hardware_concurrency on every host (audit
    // C, K-1 and K-2: all processor groups on Windows, sysconf on POSIX).
    CHECK(stxlib::processor_count() == std::thread::hardware_concurrency());
    CHECK(stxlib::capability_supported(stxlib::Capability::threads));
}

// Short sleeps are not held to the system tick (audit C, K-3): twenty 1 ms
// sleeps take well under the ~312 ms twenty 15.6 ms ticks would. The bound
// is loose for loaded CI hosts; every sleep is still at least 1 ms.
void short_sleeps() {
    const IntervalTime before = IntervalTime::now();
    for (int index = 0; index < 20; ++index) {
        const IntervalTime start = IntervalTime::now();
        stxlib::sleep_for(Duration::milliseconds(1));
        CHECK(IntervalTime::now().since(start) >= Duration::milliseconds(1));
    }
    const Duration taken = IntervalTime::now().since(before);
    CHECK(taken < Duration::milliseconds(250));
}

} // namespace

int main() {
    identities();
    thread_lifetime();
    mutual_exclusion();
    conditions();
    once();
    stacks();
    sleeping_and_host();
    short_sleeps();
    return stxlib_tests::finish("threads");
}
