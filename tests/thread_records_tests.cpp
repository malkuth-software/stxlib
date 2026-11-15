// Tests of per-thread records (include/stxlib/thread_records.hpp; notes
// planning/stages/E-thread-records.md, section 6). Tested, not proved:
// every check is one schedule the host chose. The order between a C++
// thread_local destructor and STXlib's thread-exit hook is printed, and
// checked only for its safety property (released once, or refused and never
// created), since it is a host fact the note cites for glibc and asks this
// test to show on macOS. Not covered: the failure reserve after a failed
// first allocation (it needs an allocator that fails on demand); the reserve
// after the hook is covered by the later-key-destructor case.

#include "stxlib/error.hpp"
#include "stxlib/shadow.hpp"
#include "stxlib/thread_records.hpp"

#include "test_support.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace {

using stxlib::ErrorKind;
using stxlib::Result;
using stxlib::ThreadRecordSlot;
using stxlib::ThreadRecordType;

struct Counts final {
    std::atomic<int> initialized{0};
    std::atomic<int> released{0};
    // The order of releases, as type letters.
    char order[8]{};
    std::atomic<int> order_count{0};
    char letter{'?'};
};

Counts first_counts{.letter = 'A'};
Counts second_counts{.letter = 'B'};
ThreadRecordSlot first_slot{};
ThreadRecordSlot second_slot{};

// Shared observation buffer for release order, across both types.
char release_order[16]{};
std::atomic<int> release_order_count{0};
// What each release callback saw of the other slot (1 non-null, 0 null).
std::atomic<int> second_saw_first{-1};
std::atomic<int> first_saw_second{-1};

void count_initialize(void* const record, void* const context) noexcept {
    Counts& counts = *static_cast<Counts*>(context);
    counts.initialized.fetch_add(1);
    *static_cast<int*>(record) = 7;
}

void count_release(void* const record, void* const context) noexcept {
    Counts& counts = *static_cast<Counts*>(context);
    CHECK(*static_cast<int*>(record) == 7);
    counts.released.fetch_add(1);
    const int at = release_order_count.fetch_add(1);
    if (at < 15) release_order[at] = counts.letter;
    if (&counts == &second_counts) {
        second_saw_first.store(stxlib::existing_thread_record(first_slot) != nullptr ? 1 : 0);
    } else {
        first_saw_second.store(stxlib::existing_thread_record(second_slot) != nullptr ? 1 : 0);
    }
}

ThreadRecordType counted_type(Counts& counts) {
    ThreadRecordType type{};
    type.size = sizeof(int);
    type.alignment = alignof(int);
    type.initialize = count_initialize;
    type.release = count_release;
    type.context = &counts;
    return type;
}

void registration_limits() {
    ThreadRecordType empty{};
    empty.size = 0;
    empty.alignment = 8;
    Result<ThreadRecordSlot> refused = stxlib::register_thread_record(empty);
    CHECK(refused.failed() && refused.error()->kind() == ErrorKind::invalid_argument);
    ThreadRecordType odd{};
    odd.size = 8;
    odd.alignment = 3;
    Result<ThreadRecordSlot> odd_refused = stxlib::register_thread_record(odd);
    CHECK(odd_refused.failed() && odd_refused.error()->kind() == ErrorKind::invalid_argument);
    ThreadRecordType wide{};
    wide.size = 8;
    wide.alignment = stxlib::thread_record_alignment_limit * 2;
    Result<ThreadRecordSlot> wide_refused = stxlib::register_thread_record(wide);
    CHECK(wide_refused.failed() && wide_refused.error()->kind() == ErrorKind::invalid_argument);
    ThreadRecordType huge{};
    huge.size = stxlib::thread_record_size_limit + 1;
    huge.alignment = 8;
    Result<ThreadRecordSlot> huge_refused = stxlib::register_thread_record(huge);
    CHECK(huge_refused.failed() && huge_refused.error()->kind() == ErrorKind::invalid_argument);
}

void register_counted_types() {
    Result<ThreadRecordSlot> first = stxlib::register_thread_record(counted_type(first_counts));
    Result<ThreadRecordSlot> second = stxlib::register_thread_record(counted_type(second_counts));
    CHECK(first.succeeded() && second.succeeded());
    if (first.succeeded()) first_slot = *first.value();
    if (second.succeeded()) second_slot = *second.value();
}

// A record is created once per thread and initialized once.
void first_access_creates_once() {
    std::thread worker([] {
        CHECK(stxlib::existing_thread_record(first_slot) == nullptr);
        Result<void*> once = stxlib::thread_record(first_slot);
        Result<void*> twice = stxlib::thread_record(first_slot);
        CHECK(once.succeeded() && twice.succeeded());
        if (once.succeeded() && twice.succeeded()) CHECK(*once.value() == *twice.value());
        CHECK(stxlib::existing_thread_record(first_slot) != nullptr);
    });
    worker.join();
    CHECK(first_counts.initialized.load() == 1);
    CHECK(first_counts.released.load() == 1);
}

// At thread exit (a std::thread: a thread STXlib did not start), the newest
// type is released first, each once; a release callback sees the slots
// still waiting their turn and not those let go.
void release_order_at_exit() {
    first_counts.initialized.store(0);
    first_counts.released.store(0);
    release_order_count.store(0);
    std::thread worker([] {
        CHECK(stxlib::thread_record(first_slot).succeeded());
        CHECK(stxlib::thread_record(second_slot).succeeded());
    });
    worker.join();
    CHECK(first_counts.released.load() == 1);
    CHECK(second_counts.released.load() == 1);
    CHECK(release_order_count.load() == 2);
    CHECK(release_order[0] == 'B' && release_order[1] == 'A');
    CHECK(second_saw_first.load() == 1);
    CHECK(first_saw_second.load() == 0);
}

// Re-entry from inside initialize is refused.
ThreadRecordSlot reentrant_slot{};
std::atomic<int> reentrant_kind{-1};

void reentrant_initialize(void*, void*) noexcept {
    Result<void*> inner = stxlib::thread_record(reentrant_slot);
    reentrant_kind.store(inner.failed() ? static_cast<int>(inner.error()->kind()) : 0);
}

void reentry_from_initialize_refused() {
    ThreadRecordType type{};
    type.size = 16;
    type.alignment = 8;
    type.initialize = reentrant_initialize;
    Result<ThreadRecordSlot> registered = stxlib::register_thread_record(type);
    CHECK(registered.succeeded());
    if (registered.failed()) return;
    reentrant_slot = *registered.value();
    std::thread worker([] { CHECK(stxlib::thread_record(reentrant_slot).succeeded()); });
    worker.join();
    CHECK(reentrant_kind.load() == static_cast<int>(ErrorKind::invalid_argument));
}

// end_thread_records releases now; afterwards the thread gets disposed, and
// STXlib itself stays usable.
void end_thread_records_releases_now() {
    first_counts.released.store(0);
    std::thread worker([] {
        CHECK(stxlib::thread_record(first_slot).succeeded());
        stxlib::end_thread_records();
        CHECK(first_counts.released.load() == 1);
        CHECK(stxlib::existing_thread_record(first_slot) == nullptr);
        Result<void*> after = stxlib::thread_record(first_slot);
        CHECK(after.failed() && after.error()->kind() == ErrorKind::disposed);
        const stxlib::ErrorRecord made = stxlib::ErrorRecord::make(ErrorKind::internal_failure, "stxlib.tests.after_end");
        CHECK(made.sequence() != 0);
        stxlib::end_thread_records();
    });
    worker.join();
    CHECK(first_counts.released.load() == 1);
}

// A C++ thread_local destructor that asks for a record: glibc runs it before
// the key destructors, so the record is created and then released; in the
// other order it is refused and never created. Either way nothing is
// released twice or left armed.
std::atomic<int> late_created{0};
std::atomic<int> late_refused{0};

struct AsksAtExit final {
    ~AsksAtExit() {
        Result<void*> asked = stxlib::thread_record(first_slot);
        if (asked.succeeded()) {
            late_created.fetch_add(1);
        } else if (asked.error()->kind() == ErrorKind::disposed) {
            late_refused.fetch_add(1);
        }
    }
};

thread_local AsksAtExit asks_at_exit;

void thread_local_destructor_order() {
    first_counts.initialized.store(0);
    first_counts.released.store(0);
    std::thread worker([] {
        static_cast<void>(&asks_at_exit);
        CHECK(stxlib::thread_record(second_slot).succeeded());
    });
    worker.join();
    std::printf("thread_records: a thread_local destructor %s (hook after the C++ runtime's thread_local "
                "destructors: %s)\n",
                late_created.load() == 1 ? "got its record" : "was refused",
                late_created.load() == 1 ? "yes" : "no");
    CHECK(late_created.load() + late_refused.load() == 1);
    CHECK(first_counts.released.load() == late_created.load());
}

#if !defined(_WIN32)
// A key destructor that runs after STXlib's hook (its key was created
// later, so it has a higher index): records are refused and STXlib runs on
// the failure reserve.
pthread_key_t later_key{};
std::atomic<int> later_kind{-1};
std::atomic<int> later_depth_inside{-1};
std::atomic<int> later_depth_after{-1};
std::atomic<int> later_sequence_nonzero{-1};

void later_destructor(void*) {
    Result<void*> asked = stxlib::thread_record(first_slot);
    later_kind.store(asked.failed() ? static_cast<int>(asked.error()->kind()) : 0);
    const stxlib::ShadowFrameText text{.kind = 0, .subject = "stxlib.tests", .name = "later"};
    const stxlib::ShadowFrameMark mark = stxlib::enter_shadow_frame(text);
    later_depth_inside.store(static_cast<int>(stxlib::shadow_depth()));
    stxlib::leave_shadow_frame(mark, true);
    later_depth_after.store(static_cast<int>(stxlib::shadow_depth()));
    const stxlib::ErrorRecord made = stxlib::ErrorRecord::make(ErrorKind::internal_failure, "stxlib.tests.later");
    later_sequence_nonzero.store(made.sequence() != 0 ? 1 : 0);
}

void later_key_destructor_sees_reserve() {
    CHECK(stxlib::thread_record(first_slot).succeeded()); // STXlib's key exists before ours
    CHECK(::pthread_key_create(&later_key, later_destructor) == 0);
    std::thread worker([] {
        CHECK(stxlib::thread_record(second_slot).succeeded());
        CHECK(::pthread_setspecific(later_key, &later_key) == 0);
    });
    worker.join();
    CHECK(later_kind.load() == static_cast<int>(ErrorKind::disposed));
    CHECK(later_depth_inside.load() == 1);
    CHECK(later_depth_after.load() == 0);
    CHECK(later_sequence_nonzero.load() == 1);
}
#endif

// The main thread's records are never released by the hook: process exit
// releases nothing. Checked in an atexit handler; a violation ends the
// process with status 3.
void main_thread_kept_at_exit() {
    CHECK(stxlib::thread_record(second_slot).succeeded());
    second_counts.released.store(0);
    std::atexit([] {
        if (stxlib::existing_thread_record(second_slot) == nullptr || second_counts.released.load() != 0) {
            std::fputs("thread_records: the main thread's record was released at exit\n", stderr);
            std::_Exit(3);
        }
    });
}

} // namespace

int main() {
    registration_limits();
    register_counted_types();
    first_access_creates_once();
    release_order_at_exit();
    reentry_from_initialize_refused();
    end_thread_records_releases_now();
    thread_local_destructor_order();
#if !defined(_WIN32)
    later_key_destructor_sees_reserve();
#endif
    main_thread_kept_at_exit();
    return stxlib_tests::finish("thread_records");
}
