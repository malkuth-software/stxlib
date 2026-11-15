// Recording a caught exception (include/stxlib/gate.hpp, shadow.hpp).
//
// This file is compiled with exceptions (CMakeLists.txt) whenever
// STXLIB_EXCEPTIONS is ON, the default, even though the rest of STXlib is
// not: the gates are templates instantiated in the consumer, and a consumer
// built with exceptions calls in here from its catch handlers. Nothing here
// throws out: each rethrow below is caught by the try block around it.
//
// With STXLIB_EXCEPTIONS OFF it is compiled without exceptions, like the
// rest: the recorder is left out (no gate in such a build has a handler to
// call it, and gate.hpp refuses a consumer built with exceptions), and only
// the recent-failure accessors at the end remain.
//
// Classification of standard exceptions, in the order tested (more derived
// first):
//
//   std::bad_alloc                      out_of_memory
//   std::invalid_argument               invalid_argument
//   std::out_of_range                   out_of_range
//   std::length_error                   limit_exceeded
//   std::logic_error (other)            internal_failure
//   std::system_error                   foreign_failure, with the error code as
//                                       the foreign code when its category is
//                                       the generic one (errno values)
//   std::exception (other)              internal_failure
//   anything else                       foreign_exception
//
// For a standard exception the message is its what() text, cut to the
// record's capacity at a whole UTF-8 sequence and stopped at the first
// ill-formed byte (message_truncated() then says so).
//
// Every gate record gets its shadow record (the frames at the throw, in
// every build since the A2 swap-in), and in a development build also the
// exception's type and a place in the recent ring (survey S1 = 1, S5 = 1;
// shadow.hpp), in finish_record(), the one point every gate passes
// through. The consumer's classifier runs first, exactly once (gate.hpp).

#include "stxlib/fail_fast.hpp"
#include "stxlib/gate.hpp"
#include "stxlib/shadow.hpp"

#include "state/state.hpp"

#if defined(__cpp_exceptions)
#include "core/error_writer.hpp"
#include "core/shadow_capture.hpp"
#include "hosted/host.hpp"

#include <atomic>
#include <cstdlib> // on glibc, <features.h> and so __GLIBC__
#include <cstring>
#include <exception>
#include <new>
#if defined(__GLIBCXX__)
#include <cxxabi.h> // abi::__forced_unwind (libstdc++'s <bits/cxxabi_forced.h>)
#elif defined(_LIBCPP_VERSION) && defined(__GLIBC__)
#define STXLIB_FORCED_UNWIND_BY_CLASS 1
#include <cxxabi.h> // libc++abi: abi::__cxa_current_primary_exception
#include <unwind.h> // _Unwind_Exception
#endif
#include <stdexcept>
#include <system_error>
#include <typeinfo> // typeid of a caught standard exception (finish_record)
#endif

namespace stxlib {

#if defined(__cpp_exceptions)
namespace {

// What the standard classification found. `type` is the caught standard
// exception's dynamic type, from its own RTTI (typeid of the reference the
// handler binds), so it does not depend on the C++ runtime's caught-exception
// stack; null for an exception that is not a standard one.
struct StandardClass final {
    ErrorKind kind{ErrorKind::foreign_exception};
    ForeignCode foreign{};
    const char* what{nullptr};
    const char* type{nullptr};
};

// The family of a generic-category (errno) code on this host.
ForeignFamily errno_family() noexcept {
#if defined(_WIN32)
    return ForeignFamily::crt_errno;
#else
    return ForeignFamily::posix_errno;
#endif
}

// Called only inside a catch handler (the gate's), so `throw;` rethrows the
// exception being handled, and every handler below ends that rethrow. The
// what() pointers stay valid after it: the gate's handler keeps the
// exception object alive until the gate returns.
StandardClass classify_standard() noexcept {
    StandardClass result{};
    try {
        throw;
    } catch (const std::bad_alloc& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::out_of_memory;
        result.what = failure.what();
    } catch (const std::invalid_argument& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::invalid_argument;
        result.what = failure.what();
    } catch (const std::out_of_range& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::out_of_range;
        result.what = failure.what();
    } catch (const std::length_error& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::limit_exceeded;
        result.what = failure.what();
    } catch (const std::logic_error& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::internal_failure;
        result.what = failure.what();
    } catch (const std::system_error& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::foreign_failure;
        result.what = failure.what();
        const std::error_code code = failure.code();
        if (code.category() == std::generic_category()) {
            result.foreign = ForeignCode::of(errno_family(), static_cast<i64>(code.value()));
        }
    } catch (const std::exception& failure) {
        result.type = typeid(failure).name();
        result.kind = ErrorKind::internal_failure;
        result.what = failure.what();
    } catch (...) {
        result.kind = ErrorKind::foreign_exception;
        result.what = nullptr;
    }
    return result;
}

#if defined(STXLIB_SHADOW)
// Keeps a gate record in the thread's recent ring (in stxlib_state).
void keep_recent(state::ThreadState& thread, const ErrorRecord& record) noexcept {
    const usize slot = thread.recent_count == 0 ? 0 : (thread.recent_newest + 1) % recent_failure_capacity;
    thread.recent[slot].record = record;
    thread.recent_newest = slot;
    if (thread.recent_count < recent_failure_capacity) thread.recent_count = thread.recent_count + 1;
}
#endif

// The shared end of both recorders: message, then in a development build
// the type, the shadow record (the scope's capture at the throw, or one
// taken here at the catch) and the recent ring.
void finish_record(ErrorRecord& record, const StandardClass& standard) noexcept {
    detail::ErrorRecordWriter::copy_foreign_message(record, standard.what);
    const ShadowReference shadow = shadow_capture::take_for_current_handler();
    detail::ErrorRecordWriter::set_shadow(record, shadow);
#if defined(STXLIB_SHADOW)
    // A standard exception's type comes from its own RTTI; any other from
    // the runtime's record of the exception being handled
    // (__cxa_current_exception_type, src/hosted/cxx_runtime).
    const char* const type = standard.type != nullptr ? standard.type : host::current_exception_type_name();
    detail::ErrorRecordWriter::copy_exception_type(record, type);
    // The recent ring is the block's only: a thread on the failure reserve
    // keeps none (state.hpp, ThreadFlags).
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) keep_recent(*thread, record);
#endif
}

// glibc unwinds a cancelled thread, and one that calls pthread_exit, with a
// foreign exception that libstdc++ matches as abi::__forced_unwind. A gate's
// catch (...) would catch it, and a handler that ends without rethrowing it
// makes glibc abort with no report ("FATAL: exception not rethrown"). Owner
// decision 2026-11-15 (notes decisions/2026-11-15-audit-e-decisions.md, 1):
// a gate that sees it fails fast with a record naming cancellation. The
// fail-fast call is made inside the handler, so the forced unwind is never
// ended. Runs before anything else in the gate's handler, the consumer's
// classifier included (it could swallow it the same way). libstdc++ names
// the exception's type; libc++abi on glibc is recognised by the exception
// class (below). Hosts whose cancellation does not unwind (macOS, musl,
// Windows) compile it to nothing.
[[noreturn]] void fail_fast_cancelled(const Operation operation) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::interrupted, operation,
                          ErrorDetail{.message = "thread cancellation or pthread_exit unwound into a gate"});
    fail_fast(record);
}

#if defined(STXLIB_FORCED_UNWIND_BY_CLASS)
// libc++abi on glibc (STXlib's and GUI.Forms' toolchain on Linux: libc++ 22,
// audit C, C-1 b). libc++abi has no abi::__forced_unwind type, so the
// forced unwind is recognised by its exception class. The mechanism, read in
// llvm-project libcxxabi at release 22.1.8 (as vendored in emsdk 6.0.13,
// system/lib/libcxxabi; Emscripten's fork, whose README names that base)
// and glibc 2.41 (nptl/unwind.c, read in the bminor/glibc mirror at tag
// glibc-2.41):
// - glibc's __pthread_unwind sets the forced unwind's exception_class to 0
//   ("This is not a catchable exception", unwind.c lines 122-124), its
//   cleanup to unwind_cleanup, which aborts with "FATAL: exception not
//   rethrown" (lines 106-112), and raises it with _Unwind_ForcedUnwind
//   (line 127).
// - libc++abi's personality finds catch (...) during a forced unwind
//   (cxa_personality.cpp lines 807-819). __cxa_begin_catch, for an exception
//   whose class is not CLNGC++ (cxa_exception.cpp lines 105-107), pushes
//   (__cxa_exception*)(unwind_exception + 1) - 1 onto the caught stack
//   (lines 463-475 and 503-509; the helper at lines 61-67), or terminates
//   if that stack is not empty. __cxa_end_catch would delete it
//   (lines 592-601), which is glibc's abort above.
// - __cxa_get_globals (Itanium C++ ABI 2.2.2; exported, cxa_exception.h
//   line 183) returns the per-thread globals, whose first member is that
//   stack's top (cxa_exception.h lines 175-181).
// - __cxa_current_primary_exception returns null for a foreign exception
//   (cxa_exception.cpp lines 728-737) and, for one of libc++abi's own, the
//   thrown object, which is the header's end (lines 50-56).
// So, inside a handler: a null primary exception means the caught
// exception is foreign; the caught stack's top plus the header size is the
// end of its _Unwind_Exception; exception_class 0 is glibc's forced unwind.
// The header size is not taken from a copy of libc++abi's private struct:
// it is measured once from the running library (a thrown int's thrown
// object minus its stack entry).
struct CaughtStack final {
    void* caught_exceptions;
    unsigned int uncaught_exceptions;
};

extern "C" CaughtStack* __cxa_get_globals() noexcept;

constinit std::atomic<usize> measured_header_bytes{0};

// libc++abi's __cxa_exception size, measured; 0 when it cannot be.
usize header_bytes() noexcept {
    const usize known = measured_header_bytes.load(std::memory_order_relaxed);
    if (known != 0) return known;
    usize found = 0;
    try {
        throw 0;
    } catch (const int&) {
        const unsigned char* const entry = static_cast<const unsigned char*>((*__cxa_get_globals()).caught_exceptions);
        void* const thrown = abi::__cxa_current_primary_exception();
        if (thrown != nullptr && entry != nullptr && static_cast<const unsigned char*>(thrown) > entry) {
            found = static_cast<usize>(static_cast<const unsigned char*>(thrown) - entry);
        }
        if (thrown != nullptr) abi::__cxa_decrement_exception_refcount(thrown);
    }
    if (found >= sizeof(_Unwind_Exception)) measured_header_bytes.store(found, std::memory_order_relaxed);
    return found >= sizeof(_Unwind_Exception) ? found : 0;
}

// True when the exception being handled is glibc's forced unwind.
bool handling_forced_unwind() noexcept {
    const usize header = header_bytes(); // before the stack is read: it pushes and pops its own
    if (header == 0) return false;
    void* const primary = abi::__cxa_current_primary_exception();
    if (primary != nullptr) {
        abi::__cxa_decrement_exception_refcount(primary);
        return false; // one of libc++abi's own
    }
    const unsigned char* const entry = static_cast<const unsigned char*>((*__cxa_get_globals()).caught_exceptions);
    if (entry == nullptr) return false;
    const unsigned char* const unwind_end = entry + header;
    u64 exception_class = 1;
    std::memcpy(&exception_class, unwind_end - sizeof(_Unwind_Exception), sizeof(exception_class));
    const bool result = exception_class == 0;
    return result;
}
#endif

void fail_fast_if_cancelled(const Operation operation) noexcept {
#if defined(__GLIBCXX__)
    try {
        throw;
    } catch (abi::__forced_unwind&) {
        fail_fast_cancelled(operation);
    } catch (...) {
        // Any other exception: the gate's handler goes on with it.
    }
#elif defined(STXLIB_FORCED_UNWIND_BY_CLASS)
    if (handling_forced_unwind()) fail_fast_cancelled(operation);
#else
    static_cast<void>(operation);
#endif
}

} // namespace

namespace detail {

ErrorRecord record_current_exception(const Operation operation) noexcept {
    fail_fast_if_cancelled(operation);
    const StandardClass standard = classify_standard();
    ErrorRecord record = ErrorRecord::make(standard.kind, operation, ErrorDetail{.foreign = standard.foreign});
    finish_record(record, standard);
    return record;
}

ErrorRecord record_current_exception_classified(const Operation operation,
                                                const ExceptionClassifier& classifier) noexcept {
    fail_fast_if_cancelled(operation);
    ExceptionClass decided{};
    bool recognised = false;
    if (classifier.classify != nullptr) recognised = classifier.classify(classifier.context, decided);
    const StandardClass standard = classify_standard();
    ErrorKind kind = standard.kind;
    u32 code = 0;
    if (recognised) {
        kind = decided.kind;
        code = decided.code;
    }
    ErrorRecord record =
        ErrorRecord::make(kind, operation, ErrorDetail{.code = code, .foreign = standard.foreign});
    finish_record(record, standard);
    return record;
}

} // namespace detail
#endif

usize recent_failure_count() noexcept {
    const state::ThreadState* const thread = state::current_thread_state();
    if (thread == nullptr) return 0; // the failure reserve keeps no recent ring
    const usize result = (*thread).recent_count;
    return result;
}

Result<ErrorRecord> recent_failure(const usize age) noexcept {
    const state::ThreadState* const kept_state = state::current_thread_state();
    if (kept_state == nullptr || age >= (*kept_state).recent_count) {
        const ErrorRecord missing = ErrorRecord::make(
            ErrorKind::not_found, "stxlib.shadow.recent_failure",
            ErrorDetail{.message = "the thread keeps fewer recent failures than that"});
        return Result<ErrorRecord>::failure(missing);
    }
    const state::ThreadState& thread = *kept_state;
    const usize slot = (thread.recent_newest + recent_failure_capacity - age) % recent_failure_capacity;
    const ErrorRecord kept = thread.recent[slot].record;
    return Result<ErrorRecord>::success(kept);
}

void clear_recent_failures() noexcept {
    state::ThreadState* const thread = state::current_thread_state();
    if (thread == nullptr) return;
    (*thread).recent_count = 0;
    (*thread).recent_newest = 0;
}

} // namespace stxlib
