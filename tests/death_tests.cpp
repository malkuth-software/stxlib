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
//   view_index             an out-of-range index is a failed requirement
//   take_failed_value      take_value on a failure is a failed requirement
//   callback_frames        the fail-fast report of a callback that throws
//                          inside shadow frames shows the logical stack at
//                          the throw and a trace (survey S1; development
//                          builds, else skipped)
//   foreign_free           free() of a block from memory::allocate ends the
//                          process (the host allocator's own check; ported
//                          from stx_auxlib tests/ownership_death_tests.cpp)
//   foreign_delete         the same with operator delete
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
#include <new>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
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
    const stxlib::Status installed = stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{report_marker, nullptr});
    if (installed.failed()) return 2;
    const stxlib::Status second = stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{report_marker, nullptr});
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

int view_index() {
    const int numbers[3] = {1, 2, 3};
    const stxlib::View<int> view(numbers);
    const int beyond = view[3];
    std::printf("survived %d\n", beyond);
    return 0;
}

int take_failed_value() {
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "stxlib.tests.take");
    stxlib::Result<int> failed = stxlib::Result<int>::failure(record);
    const int value = static_cast<stxlib::Result<int>&&>(failed).take_value();
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
    if (!stxlib::shadowing_enabled()) {
        std::puts("skipped");
        return 0;
    }
    CallsCallback calls;
    stxlib::in_shadow_frame("stxlib.tests.outer", 7, 8, calls);
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
    if (std::strcmp(name, "view_index") == 0) return view_index();
    if (std::strcmp(name, "take_failed_value") == 0) return take_failed_value();
    if (std::strcmp(name, "callback_frames") == 0) return callback_frames();
    if (std::strcmp(name, "foreign_free") == 0) return foreign_free();
    if (std::strcmp(name, "foreign_delete") == 0) return foreign_delete();
    std::fprintf(stderr, "unknown case %s\n", name);
    return 2;
}
