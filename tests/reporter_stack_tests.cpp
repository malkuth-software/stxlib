// The stack and address calls inside a fail-fast reporter, with allocation
// refused (fail_fast.hpp lists capture_stack_trace, write_stack_trace and
// describe_address among a reporter's calls; GUI.Forms' fail-fast report
// relies on it, A2 P0). One must-end case, run by tests/death_check.cmake:
//
//   reporter_stack_unallocated  a reporter refuses allocation, then captures
//                               the stack, describes its first address and
//                               writes the trace; any allocation while
//                               refused ends the process with "allocation
//                               refused in the reporter" before the
//                               reporter's success line
//
// What is refused, by build:
// - C++ allocation everywhere (this program replaces the global operator
//   new and delete);
// - the C library's malloc, calloc, realloc and free on glibc as well (this
//   program replaces them, forwarding to glibc's __libc_* entry points, as
//   glibc allows a program to replace its allocator). That is the case the
//   check exists for: the frame walk's bounds query, pthread_getattr_np,
//   reallocs on every call (src/hosted/posix/trace_posix.cpp);
// - nothing under a sanitizer that replaces the allocator itself (address,
//   thread and memory sanitizers): the calls are still made in the reporter.
// macOS, Windows, FreeBSD and musl C library allocation is not intercepted
// here; images.hpp and trace_posix.cpp state what was read for them.
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/boundary.hpp"
#include "stxlib/capabilities.hpp"
#include "stxlib/images.hpp"
#include "stxlib/stack_trace.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STXLIB_TESTS_ALLOCATOR_REPLACED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define STXLIB_TESTS_ALLOCATOR_REPLACED 1
#endif
#endif

#if defined(__GLIBC__) && !defined(STXLIB_TESTS_ALLOCATOR_REPLACED)
#define STXLIB_TESTS_REFUSE_C_ALLOCATION 1
#endif

namespace {

// Set only inside the reporter. Relaxed: one thread sets, reads and clears it.
std::atomic<bool> allocation_refused{false};

// Writes the refusal and ends the process, allocating nothing.
[[noreturn]] void refuse(const char* const call) noexcept {
    static const char prefix[] = "allocation refused in the reporter: ";
#if defined(_WIN32)
    static_cast<void>(_write(2, prefix, sizeof prefix - 1));
    static_cast<void>(_write(2, call, static_cast<unsigned>(std::strlen(call))));
    static_cast<void>(_write(2, "\n", 1));
#else
    static_cast<void>(::write(2, prefix, sizeof prefix - 1));
    static_cast<void>(::write(2, call, std::strlen(call)));
    static_cast<void>(::write(2, "\n", 1));
#endif
    std::abort();
}

void check_allowed(const char* const call) noexcept {
    if (allocation_refused.load(std::memory_order_relaxed)) refuse(call);
}

} // namespace

#if defined(STXLIB_TESTS_REFUSE_C_ALLOCATION)
extern "C" {
void* __libc_malloc(std::size_t size) noexcept;
void* __libc_calloc(std::size_t count, std::size_t size) noexcept;
void* __libc_realloc(void* block, std::size_t size) noexcept;
void __libc_free(void* block) noexcept;

// glibc declares these noexcept in C++ (__THROW), so they are defined so.
void* malloc(std::size_t size) noexcept {
    check_allowed("malloc");
    void* const result = __libc_malloc(size);
    return result;
}

void* calloc(std::size_t count, std::size_t size) noexcept {
    check_allowed("calloc");
    void* const result = __libc_calloc(count, size);
    return result;
}

void* realloc(void* const block, std::size_t size) noexcept {
    check_allowed("realloc");
    void* const result = __libc_realloc(block, size);
    return result;
}

void free(void* const block) noexcept {
    if (block != nullptr) check_allowed("free");
    __libc_free(block);
}
}
#endif

#if !defined(STXLIB_TESTS_ALLOCATOR_REPLACED)
void* operator new(std::size_t size) {
    check_allowed("operator new");
    void* const result = std::malloc(size == 0 ? std::size_t{1} : size);
    if (result == nullptr) std::abort(); // this program never runs out
    return result;
}

void* operator new[](std::size_t size) {
    check_allowed("operator new[]");
    void* const result = std::malloc(size == 0 ? std::size_t{1} : size);
    if (result == nullptr) std::abort(); // this program never runs out
    return result;
}

void operator delete(void* const block) noexcept {
    if (block != nullptr) check_allowed("operator delete");
    std::free(block);
}

void operator delete[](void* const block) noexcept {
    if (block != nullptr) check_allowed("operator delete[]");
    std::free(block);
}

void operator delete(void* const block, std::size_t) noexcept {
    if (block != nullptr) check_allowed("operator delete");
    std::free(block);
}

void operator delete[](void* const block, std::size_t) noexcept {
    if (block != nullptr) check_allowed("operator delete[]");
    std::free(block);
}
#endif

namespace {

using stxlib::usize;
using stxlib::uptr;

// What the reporter saw, kept for the line written after the refusal ends.
struct ReporterFindings final {
    usize captured{0};
    bool described{false};
    bool description_unsupported{false};
};

ReporterFindings findings{};

void stack_reporter(void*, const stxlib::ErrorRecord&) noexcept {
    allocation_refused.store(true, std::memory_order_relaxed);
    uptr addresses[stxlib::maximum_stack_trace_addresses]{};
    const usize captured = stxlib::capture_stack_trace(stxlib::MutableView<uptr>(addresses));
    char image_room[256]{};
    char symbol_room[256]{};
    bool described = false;
    bool unsupported = false;
    if (captured > 0) {
        const stxlib::Result<stxlib::AddressDescription> description =
            stxlib::describe_address(addresses[0] - 1, stxlib::MutableView<char>(image_room),
                                     stxlib::MutableView<char>(symbol_room));
        // The image's name may be empty for the main program (the loader's
        // own record of it); found is what is checked.
        described = description.succeeded();
        unsupported = description.failed() && (*description.error()).kind() == stxlib::ErrorKind::unsupported;
    }
    const usize shown = captured < 8 ? captured : usize{8};
    stxlib::write_stack_trace(stxlib::View<uptr>(addresses, shown));
    allocation_refused.store(false, std::memory_order_relaxed);
    findings.captured = captured;
    findings.described = described;
    findings.description_unsupported = unsupported;

    // A host that can walk its stack must have walked it, and a host that
    // can look addresses up must have found the image of the first one.
    const bool walked = captured > 0 || !stxlib::capability_supported(stxlib::Capability::stack_trace);
    const bool named = captured == 0 || described || unsupported ||
                       !stxlib::capability_supported(stxlib::Capability::address_lookup);
#if defined(STXLIB_TESTS_REFUSE_C_ALLOCATION)
    const char* const refused = "C library and C++";
#elif !defined(STXLIB_TESTS_ALLOCATOR_REPLACED)
    const char* const refused = "C++";
#else
    const char* const refused = "none (sanitizer allocator)";
#endif
    std::fprintf(stderr, "reporter: stack %s, captured %zu, described %s, refused %s\n",
                 walked && named ? "ok" : "missing", findings.captured,
                 findings.described ? "yes" : (findings.description_unsupported ? "unsupported" : "no"), refused);
}

int reporter_stack_unallocated() {
    const stxlib::Status installed =
        stxlib::install_fail_fast_reporter(stxlib::FailFastReporter{stack_reporter, nullptr});
    if (installed.failed()) return 2;
    const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(
        stxlib::ErrorKind::internal_failure, "stxlib.tests.reporter_stack",
        stxlib::ErrorDetail{.message = "the reporter's stack, unallocated"});
    stxlib::fail_fast(record); // [[noreturn]]
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "reporter_stack_unallocated") == 0) return reporter_stack_unallocated();
    std::fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
