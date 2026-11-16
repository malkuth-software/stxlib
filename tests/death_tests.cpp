// Must-end tests: each case runs as its own process (tests/death_check.cmake
// starts this program once per case) and must end abnormally with the
// expected report on standard error. A case that survives prints "survived"
// and returns 0, which the check rejects. Cases that need the
// allocation-tag mode print "skipped" without it.
//
//   callback_throw         a callback that throws through invoke_callback
//                          fails fast with STXlib's report (survey S2)
//   callback_target_throw  the same through a CallbackTarget
//   reporter               an installed reporter is called instead
//   double_release         a second release of one block is refused
//   foreign_release        release of a block not from allocate is refused
//   foreign_release_page   the same for an address at the start of a page
//                          whose preceding page is inaccessible, so a check
//                          that read below the address would fault (the CI
//                          TSan failure, 2026-11-13)
//   recursive_lock         locking a Mutex the thread holds fails fast
//                          (owner decision D11; check builds, else skipped)
//   foreign_unlock         unlocking a Mutex the thread does not hold fails
//                          fast (the same)
//   view_index             an out-of-range index is a failed requirement
//   take_failed_value      take_value on a failure is a failed requirement
//   take_or_fail_fast      take_or_fail_fast on a failure fails fast with
//                          the result's record (A3 prep)
//   check_or_fail_fast     check_or_fail_fast on a failed status fails fast
//                          with its record (A3 prep)
//   vector_index           an out-of-range Vector index is a failed requirement
//   optional_take          take on an absent Optional is a failed requirement
//   shared_dereference     dereferencing an empty Shared is a failed requirement
//   callback_frames        the fail-fast report of a callback that throws
//                          inside shadow frames shows the logical stack at
//                          the throw (every build; the trace line in
//                          development builds; survey S1, A2 swap-in)
//   callback_frame_texts   a classified callback gate with GUI.Forms' event
//                          frame (kind, subject, name, sequence): the frame
//                          appears once, with its texts (S2, A2 swap-in)
//   reporter_contract      a reporter reads the shadow record, the type and
//                          writes to the console (fail_fast.hpp's contract),
//                          and the classifier kept the full message
//   reporter_opens_frame   a reporter that opens a shadow frame fails a
//                          requirement (development builds, else skipped)
//   reporter_nested        a reporter that fails fast gets STXlib's own
//                          report, and is not called again
//   foreign_free           free() of a block from memory::allocate ends the
//                          process (the host allocator's own check; ported
//                          from stx_auxlib tests/ownership_death_tests.cpp)
//   foreign_delete         the same with operator delete
//   gate_cancelled         a callback that calls pthread_exit inside
//                          invoke_callback, on glibc with libstdc++ or
//                          libc++ (libc++abi, recognised by the exception
//                          class): the
//                          gate sees the forced unwind and fails fast with
//                          a record naming cancellation (owner decision
//                          2026-11-15, audit E, 1); elsewhere skipped
//   pool_caller_reentry    a task that run is running on the calling
//                          thread calls run on its own pool: fails fast
//                          (batch_still_open) instead of locking run_lock a
//                          second time (A-stxlib-core-01-F5)
//
// foreign_free and foreign_delete are skipped under ThreadSanitizer: its
// free and operator delete do not check that a pointer is the start of a
// block (compiler-rt lib/tsan/rtl/tsan_mman.cpp, lines 238-245, at
// llvmorg-22.1.8; read, not proved), so the process survives. The property
// stays proved in the other five modes (proofs/REPORT.md).

#include "stxlib/boundary.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_THREAD__)
#define STXLIB_TESTS_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STXLIB_TESTS_UNDER_TSAN 1
#endif
#endif

namespace {

struct ThrowingCallback final {
    void operator()() const { throw std::runtime_error("client callback failed"); }
};

int throwing_function(void*, int&) { throw std::invalid_argument("client function failed"); }

void report_marker(void*, const stxlib::ErrorRecord& record) noexcept {
    std::fprintf(stderr, "consumer reporter: %d\n", static_cast<int>(record.kind()));
}

int callback_throw() {
    ThrowingCallback callback;
    stxlib::invoke_callback("stxlib.tests.callback", callback);
    std::puts("survived");
    return 0;
}

int callback_target_throw() {
    const stxlib::CallbackTarget<int(int&)> target = stxlib::CallbackTarget<int(int&)>::bind(throwing_function, nullptr);
    int value = 1;
    const int result = stxlib::invoke_callback_target("stxlib.tests.callback", target, value);
    std::printf("survived %d\n", result);
    return 0;
}

int reporter() {
    const stxlib::OperationStatus installed = stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{report_marker,
                                                                                 nullptr});
    if (installed.failed()) return 2;
    const stxlib::OperationStatus second = stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{report_marker,
                                                                              nullptr});
    if (second.succeeded()) return 3;
    ThrowingCallback callback;
    stxlib::invoke_callback("stxlib.tests.callback", callback);
    std::puts("survived");
    return 0;
}

int double_release() {
    if (!stxlib::memory::tags_checked()) {
        std::puts("skipped");
        return 0;
    }
    void* const block = stxlib::memory::allocate(64, 16);
    if (block == nullptr) return 2;
    stxlib::memory::release(block);
    stxlib::memory::release(block);
    std::puts("survived");
    return 0;
}

int foreign_release() {
    if (!stxlib::memory::tags_checked()) {
        std::puts("skipped");
        return 0;
    }
    void* const block = std::malloc(64);
    if (block == nullptr) return 2;
    stxlib::memory::release(block);
    std::puts("survived");
    return 0;
}

// Two pages, the first made inaccessible; the second's first byte.
unsigned char* page_after_guard() {
#if defined(_WIN32)
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const std::size_t page = information.dwPageSize;
    void* const pages = VirtualAlloc(nullptr, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (pages == nullptr) return nullptr;
    DWORD previous = 0;
    if (!VirtualProtect(pages, page, PAGE_NOACCESS, &previous)) return nullptr;
#else
    const long reported = sysconf(_SC_PAGESIZE);
    if (reported <= 0) return nullptr;
    const std::size_t page = static_cast<std::size_t>(reported);
    void* const pages = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) return nullptr;
    if (mprotect(pages, page, PROT_NONE) != 0) return nullptr;
#endif
    unsigned char* const result = static_cast<unsigned char*>(pages) + page;
    return result;
}

int foreign_release_page() {
    if (!stxlib::memory::tags_checked()) {
        std::puts("skipped");
        return 0;
    }
    unsigned char* const address = page_after_guard();
    if (address == nullptr) return 2;
    stxlib::memory::release(address);
    std::puts("survived");
    return 0;
}

int recursive_lock() {
    if (!stxlib::lock_checks_enabled()) {
        std::puts("skipped");
        return 0;
    }
    stxlib::Mutex mutex{};
    mutex.lock();
    mutex.lock();
    std::puts("survived");
    return 0;
}

int foreign_unlock() {
    if (!stxlib::lock_checks_enabled()) {
        std::puts("skipped");
        return 0;
    }
    stxlib::Mutex mutex{};
    mutex.unlock();
    std::puts("survived");
    return 0;
}

void reenter_own_pool(void* const address) noexcept {
    stxlib::AtomicPool& pool = *static_cast<stxlib::AtomicPool*>(address);
    static_cast<void>(pool.run(stxlib::View<stxlib::PoolTask>()));
}

int pool_caller_reentry() {
    stxlib::OperationResult<stxlib::AtomicPool> created = stxlib::AtomicPool::create(1);
    if (created.failed()) {
        std::puts("skipped");
        return 0;
    }
    stxlib::AtomicPool pool = static_cast<stxlib::OperationResult<stxlib::AtomicPool>&&>(created).take_value();
    // A one-thread pool runs every batch on the caller.
    const stxlib::PoolTask tasks[] = {stxlib::PoolTask{reenter_own_pool, &pool}};
    static_cast<void>(pool.run(stxlib::View<stxlib::PoolTask>(tasks)));
    std::puts("survived");
    return 0;
}

int view_index() {
    const int numbers[3] = {1, 2, 3};
    const stxlib::View<int> view(numbers);
    const int beyond = view[3];
    std::printf("survived %d\n", beyond);
    return 0;
}

int take_failed_value() {
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "stxlib.tests.take");
    stxlib::OperationResult<int> failed = stxlib::OperationResult<int>::failure(record);
    const int value = static_cast<stxlib::OperationResult<int>&&>(failed).take_value();
    std::printf("survived %d\n", value);
    return 0;
}

int take_or_fail_fast() {
    // A success passes its value on first.
    const int kept = stxlib::take_or_fail_fast(stxlib::OperationResult<int>::success(7));
    std::printf("kept %d\n", kept);
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(
        stxlib::ErrorKind::not_found, "stxlib.tests.take", stxlib::ErrorDetail{.message = "no such row"});
    const int value = stxlib::take_or_fail_fast(stxlib::OperationResult<int>::failure(record));
    std::printf("survived %d\n", value);
    return 0;
}

int check_or_fail_fast() {
    stxlib::check_or_fail_fast(stxlib::OperationStatus::success());
    std::printf("passed a success\n");
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(
        stxlib::ErrorKind::limit_exceeded, "stxlib.tests.check", stxlib::ErrorDetail{.message = "too many rows"});
    stxlib::check_or_fail_fast(stxlib::OperationStatus::failure(record));
    std::printf("survived\n");
    return 0;
}

int vector_index() {
    stxlib::Vector<int> numbers;
    const stxlib::OperationStatus pushed = numbers.push(7);
    std::printf("pushed %d\n", pushed.succeeded() ? 1 : 0);
    const int beyond = numbers[1];
    std::printf("survived %d\n", beyond);
    return 0;
}

int optional_take() {
    stxlib::Optional<int> nothing = stxlib::Optional<int>::none();
    const int value = nothing.take();
    std::printf("survived %d\n", value);
    return 0;
}

int shared_dereference() {
    const stxlib::Shared<int> empty;
    const int value = *empty;
    std::printf("survived %d\n", value);
    return 0;
}

struct ThrowsInside final {
    void operator()() const { throw std::invalid_argument("inner failure"); }
};

struct CallbackWithFrame final {
    void operator()() const {
        ThrowsInside thrower;
        stxlib::in_shadow_frame("stxlib.tests.inner", 9, 0, thrower);
    }
};

struct CallsCallback final {
    void operator()() const {
        CallbackWithFrame callback;
        stxlib::invoke_callback("stxlib.tests.callback", callback);
    }
};

int callback_frames() {
    CallsCallback calls;
    stxlib::in_shadow_frame("stxlib.tests.outer", 7, 8, calls);
    std::puts("survived");
    return 0;
}

// ---- the A2 swap-in: GUI.Forms' event frame, the reporter contract ---------

struct ThrowsLongMessage final {
    void operator()() const { throw std::invalid_argument(std::string(300, 'm')); }
};

struct InnerThrow final {
    void operator()() const {
        ThrowsLongMessage thrower;
        stxlib::in_shadow_frame("stxlib.tests.inner", 9, 0, thrower);
    }
};

// The classifier as GUI.Forms writes it: counts its calls and keeps the
// full what() (the message sink, A2-2 = 1). Never recognises the type.
struct SinkState final {
    int calls{0};
    stxlib::usize message_count{0};
};

SinkState sink_state{};

bool classify_keeping_message(void* const context, stxlib::ExceptionClass&) noexcept {
    SinkState& sink = *static_cast<SinkState*>(context);
    sink.calls = sink.calls + 1;
    try {
        throw;
    } catch (const std::exception& failure) {
        sink.message_count = std::strlen(failure.what());
    } catch (...) {
    }
    return false;
}

void throw_in_event_frame() {
    const stxlib::ExceptionClassifier classifier{classify_keeping_message, &sink_state};
    const stxlib::GateFrame event{.operation = "stxlib.tests.event",
                                  .kind = 3,
                                  .subject = "button.ok",
                                  .name = "click",
                                  .sequence = 12};
    InnerThrow inner;
    stxlib::invoke_callback_classified(classifier, event, inner);
}

int callback_frame_texts() {
    throw_in_event_frame();
    std::puts("survived");
    return 0;
}

void contract_reporter(void*, const stxlib::ErrorRecord& record) noexcept {
    stxlib::ShadowFrameRecord frames[stxlib::shadow_frame_capacity]{};
    stxlib::uptr trace[stxlib::shadow_trace_capacity]{};
    const stxlib::OperationResult<stxlib::ShadowSummary> read = stxlib::read_shadow(
        record.shadow(), stxlib::MutableView<stxlib::ShadowFrameRecord>(frames),
        stxlib::MutableView<stxlib::uptr>(trace));
    char type[stxlib::exception_type_display_capacity];
    const stxlib::TextView shown_type = stxlib::exception_type_for_display(record, stxlib::MutableView<char>(type));
    const stxlib::OperationStatus written = stxlib::write_console_line(stxlib::ConsoleStream::error,
                                                                       "reporter: console");
    const stxlib::usize frame_count = read.succeeded() ? (*read.value()).frame_count : 0;
    std::fprintf(stderr, "reporter: classified %d, message %zu, frames %zu, kind %u, type %s, console %s\n",
                 sink_state.calls, sink_state.message_count, frame_count, frame_count > 0 ? frames[0].kind : 0U,
                 shown_type.empty() ? "none" : "named", written.succeeded() ? "ok" : "failed");
}

int reporter_contract() {
    const stxlib::OperationStatus installed =
        stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{contract_reporter, nullptr});
    if (installed.failed()) return 2;
    throw_in_event_frame();
    std::puts("survived");
    return 0;
}

struct Nothing final {
    void operator()() const {}
};

void frame_opening_reporter(void*, const stxlib::ErrorRecord&) noexcept {
    Nothing nothing;
    stxlib::in_shadow_frame("stxlib.tests.in_reporter", 0, 0, nothing);
    std::fprintf(stderr, "reporter: opened a frame\n");
}

int reporter_opens_frame() {
    if (!stxlib::shadowing_enabled()) {
        std::puts("skipped");
        return 0;
    }
    const stxlib::OperationStatus installed =
        stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{frame_opening_reporter, nullptr});
    if (installed.failed()) return 2;
    ThrowingCallback callback;
    stxlib::invoke_callback("stxlib.tests.callback", callback);
    std::puts("survived");
    return 0;
}

void nesting_reporter(void*, const stxlib::ErrorRecord& record) noexcept {
    std::fprintf(stderr, "reporter entered\n");
    stxlib::fail_fast(record);
}

int reporter_nested() {
    const stxlib::OperationStatus installed =
        stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{nesting_reporter, nullptr});
    if (installed.failed()) return 2;
    ThrowingCallback callback;
    stxlib::invoke_callback("stxlib.tests.callback", callback);
    std::puts("survived");
    return 0;
}

bool skipped_for_foreign_free() {
#if defined(STXLIB_TESTS_UNDER_TSAN)
    std::puts("skipped: ThreadSanitizer does not check foreign pointers in free (tsan_mman.cpp:238-245)");
    return true;
#else
    if (!stxlib::memory::tags_checked()) {
        std::puts("skipped");
        return true;
    }
    return false;
#endif
}

int foreign_free() {
    if (skipped_for_foreign_free()) return 0;
    void* const block = stxlib::memory::allocate(64, 16);
    if (block == nullptr) return 2;
    std::free(block);
    std::puts("survived");
    return 0;
}

int foreign_delete() {
    if (skipped_for_foreign_free()) return 0;
    void* const block = stxlib::memory::allocate(64, 16);
    if (block == nullptr) return 2;
    ::operator delete(block);
    std::puts("survived");
    return 0;
}

#if defined(__GLIBC__) && (defined(__GLIBCXX__) || defined(_LIBCPP_VERSION))
struct ExitingCallback final {
    void operator()() const { ::pthread_exit(nullptr); }
};

void* run_exiting_gate(void*) {
    ExitingCallback callback;
    stxlib::invoke_callback("stxlib.tests.cancel", callback);
    return nullptr;
}
#endif

int gate_cancelled() {
#if defined(__GLIBC__) && (defined(__GLIBCXX__) || defined(_LIBCPP_VERSION))
    pthread_t thread{};
    if (::pthread_create(&thread, nullptr, run_exiting_gate, nullptr) != 0) return 2;
    static_cast<void>(::pthread_join(thread, nullptr));
    std::puts("survived");
    return 0;
#else
    std::puts("skipped: the host's thread exit does not unwind as abi::__forced_unwind");
    return 0;
#endif
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: stxlib_death_tests <case>\n");
        return 2;
    }
    const char* const name = argv[1];
    if (std::strcmp(name, "callback_throw") == 0) return callback_throw();
    if (std::strcmp(name, "callback_target_throw") == 0) return callback_target_throw();
    if (std::strcmp(name, "reporter") == 0) return reporter();
    if (std::strcmp(name, "double_release") == 0) return double_release();
    if (std::strcmp(name, "foreign_release") == 0) return foreign_release();
    if (std::strcmp(name, "foreign_release_page") == 0) return foreign_release_page();
    if (std::strcmp(name, "recursive_lock") == 0) return recursive_lock();
    if (std::strcmp(name, "foreign_unlock") == 0) return foreign_unlock();
    if (std::strcmp(name, "view_index") == 0) return view_index();
    if (std::strcmp(name, "take_failed_value") == 0) return take_failed_value();
    if (std::strcmp(name, "take_or_fail_fast") == 0) return take_or_fail_fast();
    if (std::strcmp(name, "check_or_fail_fast") == 0) return check_or_fail_fast();
    if (std::strcmp(name, "vector_index") == 0) return vector_index();
    if (std::strcmp(name, "optional_take") == 0) return optional_take();
    if (std::strcmp(name, "shared_dereference") == 0) return shared_dereference();
    if (std::strcmp(name, "callback_frames") == 0) return callback_frames();
    if (std::strcmp(name, "callback_frame_texts") == 0) return callback_frame_texts();
    if (std::strcmp(name, "reporter_contract") == 0) return reporter_contract();
    if (std::strcmp(name, "reporter_opens_frame") == 0) return reporter_opens_frame();
    if (std::strcmp(name, "reporter_nested") == 0) return reporter_nested();
    if (std::strcmp(name, "foreign_free") == 0) return foreign_free();
    if (std::strcmp(name, "foreign_delete") == 0) return foreign_delete();
    if (std::strcmp(name, "gate_cancelled") == 0) return gate_cancelled();
    if (std::strcmp(name, "pool_caller_reentry") == 0) return pool_caller_reentry();
    std::fprintf(stderr, "unknown case %s\n", name);
    return 2;
}
