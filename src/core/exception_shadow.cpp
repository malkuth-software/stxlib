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

#include "stxlib/gate.hpp"
#include "stxlib/shadow.hpp"

#include "state/state.hpp"

#if defined(__cpp_exceptions)
#include "core/error_writer.hpp"
#include "core/shadow_capture.hpp"
#include "hosted/host.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>
#endif

namespace stxlib {

#if defined(__cpp_exceptions)
namespace {

// What the standard classification found.
struct StandardClass final {
    ErrorKind kind{ErrorKind::foreign_exception};
    ForeignCode foreign{};
    const char* what{nullptr};
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
        result.kind = ErrorKind::out_of_memory;
        result.what = failure.what();
    } catch (const std::invalid_argument& failure) {
        result.kind = ErrorKind::invalid_argument;
        result.what = failure.what();
    } catch (const std::out_of_range& failure) {
        result.kind = ErrorKind::out_of_range;
        result.what = failure.what();
    } catch (const std::length_error& failure) {
        result.kind = ErrorKind::limit_exceeded;
        result.what = failure.what();
    } catch (const std::logic_error& failure) {
        result.kind = ErrorKind::internal_failure;
        result.what = failure.what();
    } catch (const std::system_error& failure) {
        result.kind = ErrorKind::foreign_failure;
        result.what = failure.what();
        const std::error_code code = failure.code();
        if (code.category() == std::generic_category()) {
            result.foreign = ForeignCode::of(errno_family(), static_cast<i64>(code.value()));
        }
    } catch (const std::exception& failure) {
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
    detail::ErrorRecordWriter::copy_exception_type(record, host::current_exception_type_name());
    // The recent ring is the block's only: a thread on the failure reserve
    // keeps none (state.hpp, ThreadFlags).
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) keep_recent(*thread, record);
#endif
}

} // namespace

namespace detail {

ErrorRecord record_current_exception(const Operation operation) noexcept {
    const StandardClass standard = classify_standard();
    ErrorRecord record = ErrorRecord::make(standard.kind, operation, ErrorDetail{.foreign = standard.foreign});
    finish_record(record, standard);
    return record;
}

ErrorRecord record_current_exception_classified(const Operation operation,
                                                const ExceptionClassifier& classifier) noexcept {
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
