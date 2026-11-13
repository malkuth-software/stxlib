// Tests of trampolines with revocation (include/stxlib/trampoline.hpp).
// The concurrent case checks the ordering property revoke promises (it does
// not return before a running invocation has left) on one interleaving the
// test forces; it is a test, not a proof (proofs/REPORT.md).

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>

namespace {

struct Counter final {
    int total{0};

    stxlib::Status operator()(int& amount) {
        total = total + amount;
        return stxlib::Status::success();
    }
};

using CounterTrampolines = stxlib::Trampolines<Counter>;

CounterTrampolines make_counters(const stxlib::usize capacity) {
    stxlib::Result<CounterTrampolines> made = CounterTrampolines::create("stxlib.tests.trampoline", capacity);
    STXLIB_REQUIRE(made.succeeded(), "creating the test table");
    CounterTrampolines result = static_cast<stxlib::Result<CounterTrampolines>&&>(made).take_value();
    return result;
}

stxlib::TrampolineHandle attach_or_fail(CounterTrampolines& table, Counter& counter) {
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(counter);
    STXLIB_REQUIRE(attached.succeeded(), "attaching a test target");
    const stxlib::TrampolineHandle handle = *attached.value();
    return handle;
}

void capacities_are_checked() {
    const stxlib::Result<stxlib::TrampolineTable> empty = stxlib::TrampolineTable::create(0);
    CHECK(empty.failed() && (*empty.error()).kind() == stxlib::ErrorKind::invalid_argument);
    const stxlib::Result<stxlib::TrampolineTable> too_big =
        stxlib::TrampolineTable::create(stxlib::trampoline_capacity_limit + 1);
    CHECK(too_big.failed());
    stxlib::Result<stxlib::TrampolineTable> largest = stxlib::TrampolineTable::create(stxlib::trampoline_capacity_limit);
    CHECK(largest.succeeded() && (*largest.value()).capacity() == stxlib::trampoline_capacity_limit);
}

void invoke_revoke_and_reissue() {
    CounterTrampolines table = make_counters(1);
    Counter first;
    const stxlib::TrampolineHandle handle = attach_or_fail(table, first);
    CHECK(handle.present());
    const stxlib::TrampolineHandle round_trip = stxlib::TrampolineHandle::from_context(handle.as_context());
    CHECK(round_trip == handle);

    int amount = 5;
    CHECK(table.invoke(round_trip, amount).succeeded());
    CHECK(first.total == 5);

    Counter second;
    const stxlib::Result<stxlib::TrampolineHandle> full = table.attach(second);
    CHECK(full.failed() && (*full.error()).kind() == stxlib::ErrorKind::limit_exceeded);

    stxlib::Result<stxlib::Revocation> revoked = table.revoke(handle);
    CHECK(revoked.succeeded() && *revoked.value() == stxlib::Revocation::completed);
    const stxlib::Status after = table.invoke(handle, amount);
    CHECK(after.failed());
    CHECK(first.total == 5);
    const stxlib::Result<stxlib::Revocation> again = table.revoke(handle);
    CHECK(again.failed() && (*again.error()).kind() == stxlib::ErrorKind::stale_identity);

    const stxlib::TrampolineHandle reissued = attach_or_fail(table, second);
    CHECK(reissued != handle);
    const stxlib::Status stale = table.invoke(handle, amount);
    CHECK(stale.failed() && (*stale.error()).kind() == stxlib::ErrorKind::stale_identity);
    CHECK(table.invoke(reissued, amount).succeeded());
    CHECK(second.total == 5 && first.total == 5);

    const stxlib::Status null_context = table.invoke(stxlib::TrampolineHandle::from_context(nullptr), amount);
    CHECK(null_context.failed() && (*null_context.error()).kind() == stxlib::ErrorKind::stale_identity);
    const stxlib::Status out_of_table = table.invoke(stxlib::TrampolineHandle::from_value((1ULL << 16) | 9), amount);
    CHECK(out_of_table.failed());
}

// A target that revokes its own handle while it runs.
struct SelfRevoker final {
    stxlib::Trampolines<SelfRevoker>* table{nullptr};
    stxlib::TrampolineHandle handle{};
    bool deferred{false};

    stxlib::Status operator()() {
        stxlib::Result<stxlib::Revocation> revoked = (*table).revoke(handle);
        deferred = revoked.succeeded() && *revoked.value() == stxlib::Revocation::deferred;
        return stxlib::Status::success();
    }
};

void self_revocation_is_deferred() {
    stxlib::Result<stxlib::Trampolines<SelfRevoker>> made =
        stxlib::Trampolines<SelfRevoker>::create("stxlib.tests.trampoline", 1);
    CHECK(made.succeeded());
    stxlib::Trampolines<SelfRevoker> table = static_cast<stxlib::Result<stxlib::Trampolines<SelfRevoker>>&&>(made).take_value();
    SelfRevoker revoker;
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(revoker);
    CHECK(attached.succeeded());
    revoker.table = &table;
    revoker.handle = *attached.value();
    CHECK(table.invoke(revoker.handle).succeeded());
    CHECK(revoker.deferred);
    CHECK(table.invoke(revoker.handle).failed());
    // The one slot was released when the invocation left, so it can be
    // attached again (capacity 1).
    SelfRevoker next;
    CHECK(table.attach(next).succeeded());
}

// A target that re-enters its own trampoline, to the nesting limit.
struct Nester final {
    stxlib::Trampolines<Nester>* table{nullptr};
    stxlib::TrampolineHandle handle{};
    stxlib::usize depth{0};
    stxlib::usize deepest{0};
    bool refused_at_limit{false};

    stxlib::Status operator()() {
        depth = depth + 1;
        if (depth > deepest) deepest = depth;
        const stxlib::Status inner = (*table).invoke(handle);
        if (inner.failed() && (*inner.error()).kind() == stxlib::ErrorKind::limit_exceeded) refused_at_limit = true;
        depth = depth - 1;
        return stxlib::Status::success();
    }
};

void nesting_is_bounded() {
    stxlib::Result<stxlib::Trampolines<Nester>> made = stxlib::Trampolines<Nester>::create("stxlib.tests.trampoline", 1);
    stxlib::Trampolines<Nester> table = static_cast<stxlib::Result<stxlib::Trampolines<Nester>>&&>(made).take_value();
    Nester nester;
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(nester);
    nester.table = &table;
    nester.handle = *attached.value();
    CHECK(table.invoke(nester.handle).succeeded());
    CHECK(nester.deepest == stxlib::trampoline_nesting_limit);
    CHECK(nester.refused_at_limit);
    stxlib::Result<stxlib::Revocation> revoked = table.revoke(nester.handle);
    CHECK(revoked.succeeded() && *revoked.value() == stxlib::Revocation::completed);
}

struct Thrower final {
    stxlib::Status operator()() { throw std::runtime_error("target failed"); }
};

void a_throwing_target_leaves_its_slot() {
    stxlib::Result<stxlib::Trampolines<Thrower>> made = stxlib::Trampolines<Thrower>::create("stxlib.tests.trampoline", 1);
    stxlib::Trampolines<Thrower> table = static_cast<stxlib::Result<stxlib::Trampolines<Thrower>>&&>(made).take_value();
    Thrower thrower;
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(thrower);
    const stxlib::TrampolineHandle handle = *attached.value();
    const stxlib::Status failed = table.invoke(handle);
    CHECK(failed.failed() && (*failed.error()).kind() == stxlib::ErrorKind::internal_failure);
    CHECK((*failed.error()).operation().name() == stxlib::TextView("stxlib.tests.trampoline"));
    // If the slot had not been left, this would wait forever.
    stxlib::Result<stxlib::Revocation> revoked = table.revoke(handle);
    CHECK(revoked.succeeded() && *revoked.value() == stxlib::Revocation::completed);
}

// A target that announces it is running and waits for permission to leave.
struct Blocker final {
    std::atomic<bool> entered{false};
    std::atomic<bool> may_leave{false};
    std::atomic<bool> left{false};

    stxlib::Status operator()() {
        entered.store(true);
        while (!may_leave.load()) std::this_thread::yield();
        left.store(true);
        return stxlib::Status::success();
    }
};

struct BlockerRun final {
    stxlib::Trampolines<Blocker>* table{nullptr};
    stxlib::TrampolineHandle handle{};
    bool succeeded{false};

    void operator()() {
        const stxlib::Status status = (*table).invoke(handle);
        succeeded = status.succeeded();
    }
};

struct RevokeRun final {
    stxlib::Trampolines<Blocker>* table{nullptr};
    stxlib::TrampolineHandle handle{};
    Blocker* blocker{nullptr};
    std::atomic<bool> done{false};
    bool left_before_return{false};
    bool completed{false};

    void operator()() {
        stxlib::Result<stxlib::Revocation> revoked = (*table).revoke(handle);
        left_before_return = (*blocker).left.load();
        completed = revoked.succeeded() && *revoked.value() == stxlib::Revocation::completed;
        done.store(true);
    }
};

void revoke_waits_for_a_running_invocation() {
    stxlib::Result<stxlib::Trampolines<Blocker>> made = stxlib::Trampolines<Blocker>::create("stxlib.tests.trampoline", 1);
    stxlib::Trampolines<Blocker> table = static_cast<stxlib::Result<stxlib::Trampolines<Blocker>>&&>(made).take_value();
    Blocker blocker;
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(blocker);
    const stxlib::TrampolineHandle handle = *attached.value();

    BlockerRun run{&table, handle, false};
    std::thread invoking(std::ref(run));
    while (!blocker.entered.load()) std::this_thread::yield();

    RevokeRun revoke{&table, handle, &blocker};
    std::thread revoking(std::ref(revoke));
    // The invocation is still inside the target, so revoke must not have
    // returned; then let the target leave.
    const bool returned_early = revoke.done.load();
    blocker.may_leave.store(true);
    invoking.join();
    revoking.join();

    CHECK(!returned_early);
    CHECK(run.succeeded);
    CHECK(revoke.completed);
    CHECK(revoke.left_before_return);
    CHECK(table.invoke(handle).failed());
}

} // namespace

int main() {
    capacities_are_checked();
    invoke_revoke_and_reissue();
    self_revocation_is_deferred();
    nesting_is_bounded();
    a_throwing_target_leaves_its_slot();
    revoke_waits_for_a_running_invocation();
    const int status = stxlib_tests::finish("stxlib trampolines");
    return status;
}
