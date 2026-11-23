// Tests of StopSource, StopToken and StopCallback (include/stxlib/
// stop_token.hpp; the shield's item 3, 2026-11-22). The null-function misuse
// is a must-end case in death_tests.cpp. Tested, not proved
// (proofs/REPORT.md): the stress cases are evidence of likely defects only;
// the sanitizer builds read them for races and leaks of the shared state.

#include "stxlib/stop_token.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

namespace {

using stxlib::Duration;
using stxlib::JoinResult;
using stxlib::OperationResult;
using stxlib::StopCallback;
using stxlib::StopSignal;
using stxlib::StopSource;
using stxlib::StopToken;
using stxlib::Thread;
using stxlib::ThreadIdentity;

Thread start_or_fail(const Thread::Entry entry, void* const context) {
    OperationResult<Thread> started = Thread::start(entry, context);
    CHECK(started.succeeded());
    if (started.failed()) return Thread();
    Thread result = std::move(started).take_value();
    return result;
}

StopSource create_or_fail() {
    OperationResult<StopSource> created = StopSource::create();
    CHECK(created.succeeded());
    if (created.failed()) return StopSource();
    StopSource result = std::move(created).take_value();
    return result;
}

// ---- Sources and tokens ------------------------------------------------------------

void empty_states() {
    const StopToken token{};
    CHECK(!token.stop_requested());
    CHECK(!token.stop_possible());
    StopSource source{};
    CHECK(!source.stop_possible());
    CHECK(!source.stop_requested());
    CHECK(!source.request_stop());
    const StopToken from_empty = source.token();
    CHECK(!from_empty.stop_possible());
    CHECK(from_empty.same_state(token));
}

void requests() {
    StopSource source = create_or_fail();
    CHECK(source.stop_possible());
    CHECK(!source.stop_requested());
    const StopToken token = source.token();
    CHECK(token.stop_possible());
    CHECK(!token.stop_requested());

    // Copies share the state.
    StopToken copied(token);
    StopToken assigned{};
    assigned = token;
    CHECK(copied.same_state(token) && assigned.same_state(token));
    const StopSource source_copy(source);
    CHECK(source_copy.token().same_state(token));

    // The first request is the one; later ones answer false.
    CHECK(source_copy.stop_possible());
    CHECK(source.request_stop());
    CHECK(!source.request_stop());
    StopSource second_copy = source_copy;
    CHECK(!second_copy.request_stop());
    CHECK(token.stop_requested() && copied.stop_requested() && assigned.stop_requested());
    CHECK(source_copy.stop_requested());

    // Moving leaves the source of the move empty.
    StopToken moved(std::move(copied));
    CHECK(!copied.stop_possible()); // NOLINT(bugprone-use-after-move): the moved-from state is specified
    CHECK(moved.stop_requested());
    StopSource moved_source(std::move(second_copy));
    CHECK(!second_copy.stop_possible()); // NOLINT(bugprone-use-after-move): specified
    CHECK(moved_source.stop_requested());

    // A distinct state is not shared.
    const StopSource other = create_or_fail();
    CHECK(!other.token().same_state(token));
    CHECK(!other.stop_requested());
}

void possible_after_sources() {
    StopToken token{};
    {
        const StopSource source = create_or_fail();
        token = source.token();
        CHECK(token.stop_possible());
    }
    // No source left and no request: a stop can no longer happen.
    CHECK(!token.stop_possible());
    CHECK(!token.stop_requested());

    StopToken requested{};
    {
        StopSource source = create_or_fail();
        requested = source.token();
        CHECK(source.request_stop());
    }
    CHECK(requested.stop_possible());
    CHECK(requested.stop_requested());
}

// ---- Callbacks ------------------------------------------------------------------------

constexpr int callback_count = 5;

struct Record final {
    int order[callback_count]{};
    int entries{0};
    ThreadIdentity thread[callback_count]{};
};

struct Entry final {
    Record* record{nullptr};
    int number{0};
};

void note_entry(void* const address) noexcept {
    Entry& entry = *static_cast<Entry*>(address);
    Record& record = *entry.record;
    if (record.entries < callback_count) {
        record.order[record.entries] = entry.number;
        record.thread[record.entries] = stxlib::current_thread();
    }
    record.entries = record.entries + 1;
}

struct Requester final {
    StopSource* source{nullptr};
    ThreadIdentity identity{};
    bool made{false};
};

void request_from_thread(const StopSignal&, void* const address) noexcept {
    Requester& requester = *static_cast<Requester*>(address);
    requester.identity = stxlib::current_thread();
    requester.made = (*requester.source).request_stop();
}

void callbacks_in_order() {
    StopSource source = create_or_fail();
    const StopToken token = source.token();
    Record record{};
    Entry entries[callback_count]{};
    for (int index = 0; index < callback_count; ++index) {
        entries[index].record = &record;
        entries[index].number = index;
    }
    const StopCallback first(token, note_entry, &entries[0]);
    const StopCallback second(token, note_entry, &entries[1]);
    std::optional<StopCallback> withdrawn{};
    withdrawn.emplace(token, note_entry, &entries[4]);
    const StopCallback third(token, note_entry, &entries[2]);
    withdrawn.reset(); // unregistered before the request: never runs
    CHECK(record.entries == 0);

    // The requesting thread runs them, in registration order, before its
    // request_stop returns.
    Requester requester{};
    requester.source = &source;
    Thread thread = start_or_fail(request_from_thread, &requester);
    CHECK(thread.join() == JoinResult::joined);
    CHECK(requester.made);
    CHECK(record.entries == 3);
    CHECK(record.order[0] == 0 && record.order[1] == 1 && record.order[2] == 2);
    for (int index = 0; index < 3; ++index) CHECK(record.thread[index] == requester.identity);

    // Registered after the request: runs at once, here.
    const StopCallback late(token, note_entry, &entries[3]);
    CHECK(record.entries == 4);
    CHECK(record.order[3] == 3);
    CHECK(record.thread[3] == stxlib::current_thread());
}

void callbacks_never_run() {
    Record record{};
    Entry entry{};
    entry.record = &record;
    // No state.
    {
        const StopToken token{};
        const StopCallback callback(token, note_entry, &entry);
    }
    // No source left: a stop can no longer be requested.
    {
        StopToken token{};
        {
            const StopSource source = create_or_fail();
            token = source.token();
        }
        const StopCallback callback(token, note_entry, &entry);
    }
    // Destroyed before the request.
    StopSource source = create_or_fail();
    {
        const StopCallback callback(source.token(), note_entry, &entry);
    }
    CHECK(source.request_stop());
    CHECK(record.entries == 0);
}

// A callback that destroys itself from its own function.
struct SelfDestroying final {
    std::optional<StopCallback> callback{};
    int runs{0};
};

void destroy_own_callback(void* const address) noexcept {
    SelfDestroying& self = *static_cast<SelfDestroying*>(address);
    self.runs = self.runs + 1;
    self.callback.reset();
}

void callback_destroys_itself() {
    StopSource source = create_or_fail();
    SelfDestroying self{};
    self.callback.emplace(source.token(), destroy_own_callback, &self);
    Record record{};
    Entry after{};
    after.record = &record;
    const StopCallback next(source.token(), note_entry, &after);
    CHECK(source.request_stop());
    CHECK(self.runs == 1);
    CHECK(!self.callback.has_value());
    // The request went on to the next callback.
    CHECK(record.entries == 1);
}

// A callback that destroys the source whose request runs it.
struct SourceHolder final {
    std::optional<StopSource> source{};
    int runs{0};
};

void destroy_the_source(void* const address) noexcept {
    SourceHolder& holder = *static_cast<SourceHolder*>(address);
    holder.runs = holder.runs + 1;
    holder.source.reset();
}

void callback_destroys_source() {
    SourceHolder holder{};
    holder.source.emplace(create_or_fail());
    const StopToken token = (*holder.source).token();
    const StopCallback callback(token, destroy_the_source, &holder);
    CHECK((*holder.source).request_stop());
    CHECK(holder.runs == 1);
    CHECK(!holder.source.has_value());
    CHECK(token.stop_requested());
}

// A callback destroyed on one thread while it runs on another: the
// destructor waits for it to return.
struct SlowCallback final {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
};

void run_slowly(void* const address) noexcept {
    SlowCallback& slow = *static_cast<SlowCallback*>(address);
    slow.entered.store(true, std::memory_order_release);
    stxlib::sleep_for(Duration::milliseconds(30));
    slow.finished.store(true, std::memory_order_release);
}

void destructor_waits_for_running() {
    StopSource source = create_or_fail();
    SlowCallback slow{};
    Requester requester{};
    requester.source = &source;
    Thread thread{};
    {
        const StopCallback callback(source.token(), run_slowly, &slow);
        thread = start_or_fail(request_from_thread, &requester);
        while (!slow.entered.load(std::memory_order_acquire)) stxlib::yield_thread();
    }
    CHECK(slow.finished.load(std::memory_order_acquire));
    CHECK(thread.join() == JoinResult::joined);
    CHECK(requester.made);
}

// Re-entry: a callback that requests the stop again, and one that
// registers another callback, from inside the request.
struct Reentry final {
    StopSource* source{nullptr};
    bool second_request{true};
    Record* record{nullptr};
    Entry* entry{nullptr};
};

void request_again(void* const address) noexcept {
    Reentry& reentry = *static_cast<Reentry*>(address);
    reentry.second_request = (*reentry.source).request_stop();
    const StopCallback inner((*reentry.source).token(), note_entry, reentry.entry);
}

void callback_reenters() {
    StopSource source = create_or_fail();
    Record record{};
    Entry entry{};
    entry.record = &record;
    Reentry reentry{};
    reentry.source = &source;
    reentry.record = &record;
    reentry.entry = &entry;
    const StopCallback callback(source.token(), request_again, &reentry);
    CHECK(source.request_stop());
    CHECK(!reentry.second_request);
    CHECK(record.entries == 1); // the inner callback ran at its construction
}

// ---- Stress --------------------------------------------------------------------------

constexpr int racer_count = 8;

// Eight threads race to request one stop: exactly one makes it, and each
// callback registered before runs exactly once.
struct Race final {
    StopSource source{};
    std::atomic<int> ready{0};
    std::atomic<int> made{0};
    std::atomic<int> runs{0};
};

void count_run(void* const address) noexcept {
    std::atomic<int>& runs = *static_cast<std::atomic<int>*>(address);
    runs.fetch_add(1, std::memory_order_relaxed);
}

void race_to_request(const StopSignal&, void* const address) noexcept {
    Race& race = *static_cast<Race*>(address);
    StopSource mine(race.source); // each racer requests through its own copy
    race.ready.fetch_add(1, std::memory_order_acq_rel);
    while (race.ready.load(std::memory_order_acquire) < racer_count) stxlib::yield_thread();
    if (mine.request_stop()) race.made.fetch_add(1, std::memory_order_relaxed);
}

void racing_requests() {
    for (int round = 0; round < 200; ++round) {
        Race race{};
        race.source = create_or_fail();
        std::optional<StopCallback> callbacks[4]{};
        for (std::optional<StopCallback>& slot : callbacks) slot.emplace(race.source.token(), count_run, &race.runs);
        {
            std::vector<Thread> threads{};
            for (int index = 0; index < racer_count; ++index) threads.push_back(start_or_fail(race_to_request, &race));
        }
        CHECK(race.made.load() == 1);
        CHECK(race.runs.load() == 4);
    }
}

// Workers copy tokens and sources and register and drop callbacks in a
// loop while the main thread requests the stop; after it, a new callback
// runs at once on every worker.
constexpr int worker_count = 8;

struct Churn final {
    StopSource source{};
    std::atomic<int> started{0};
    std::atomic<int> after_request_runs{0};
    std::atomic<int> total_runs{0};
};

struct WorkerTally final {
    Churn* churn{nullptr};
    int runs{0};
};

void tally_run(void* const address) noexcept {
    WorkerTally& tally = *static_cast<WorkerTally*>(address);
    tally.runs = tally.runs + 1;
    (*tally.churn).total_runs.fetch_add(1, std::memory_order_relaxed);
}

void churn_tokens(const StopSignal&, void* const address) noexcept {
    Churn& churn = *static_cast<Churn*>(address);
    const StopToken token = churn.source.token();
    churn.started.fetch_add(1, std::memory_order_acq_rel);
    WorkerTally tally{};
    tally.churn = &churn;
    while (!token.stop_requested()) {
        const StopToken copy(token);
        const StopSource source_copy(churn.source);
        StopToken assigned{};
        assigned = source_copy.token();
        const StopCallback callback(assigned, tally_run, &tally);
    }
    const int before = tally.runs;
    {
        const StopCallback late(token, tally_run, &tally);
    }
    if (tally.runs == before + 1) churn.after_request_runs.fetch_add(1, std::memory_order_relaxed);
}

void churning_callbacks() {
    Churn churn{};
    churn.source = create_or_fail();
    std::vector<Thread> threads{};
    for (int index = 0; index < worker_count; ++index) threads.push_back(start_or_fail(churn_tokens, &churn));
    while (churn.started.load(std::memory_order_acquire) < worker_count) stxlib::yield_thread();
    stxlib::sleep_for(Duration::milliseconds(20));
    CHECK(churn.source.request_stop());
    for (Thread& thread : threads) CHECK(thread.join() == JoinResult::joined);
    CHECK(churn.after_request_runs.load() == worker_count);
    // Each worker's late callback ran, and some in-loop callbacks may have
    // been registered when the request came.
    CHECK(churn.total_runs.load() >= worker_count);
}

} // namespace

int main() {
    empty_states();
    requests();
    possible_after_sources();
    callbacks_in_order();
    callbacks_never_run();
    callback_destroys_itself();
    callback_destroys_source();
    destructor_waits_for_running();
    callback_reenters();
    racing_requests();
    churning_callbacks();
    return stxlib_tests::finish("stop_token");
}
