#pragma once

// The MSVC C++ runtime's per-thread state, the active-catch record, and the
// entry points the runtime's own files share.
//
// Ownership and lifetime. A thread's state is two values kept in two TLS
// slots this runtime allocates once per runtime instance (TlsAlloc; one
// instance per program in the shared form, one per module in the static
// form): the uncaught-exception count and the innermost active catch. No
// block is allocated, so a thread's exit releases nothing. Each active
// catch is a CatchRecord that lives in the frame of the routine that runs
// the catch (frame_handler.cpp, run_catch), linked from the slot while the
// catch body runs; the record is unlinked before that frame returns, and
// unlinked by the catch guard when an exception leaves the body.
//
// Every function here is compiled without exceptions and never unwinds
// itself; foreign code (copy constructors, destructors, cleanup and catch
// funclets) runs only through the guarded thunks (thunks_x64.S).

#include "msvc_abi.hpp"

#include "stxlib/types.hpp"

#include "system_interim.h"

namespace stxlib::msvc_runtime {

// The exception a catch handles: a copy of its record's fields, enough to
// raise it again (`throw;`, rethrow_exception) and to destroy its object.
struct HandledException final {
    u32 code;                            // cxx_exception_code, or a structured exception's code (-EHa)
    u32 flags;
    u32 parameter_count;
    ULONG_PTR parameters[EXCEPTION_MAXIMUM_PARAMETERS];
};

[[nodiscard]] bool is_cxx_exception(const EXCEPTION_RECORD& record) noexcept;
[[nodiscard]] bool is_cxx_exception(const HandledException& exception) noexcept;
[[nodiscard]] void* exception_object(const HandledException& exception) noexcept;
[[nodiscard]] const ThrowInfo* exception_throw_info(const HandledException& exception) noexcept;
[[nodiscard]] uptr exception_image_base(const HandledException& exception) noexcept;
void copy_handled_exception(const EXCEPTION_RECORD& record, HandledException& out) noexcept;

// Checked in the catch thunk's frame and in each record, so a frame not
// run by this runtime is never read as one.
inline constexpr u64 catch_record_cookie = 0x5354584C43415443ull; // "STXLCATC"
// A record whose catch an exception has left (the catch guard).
inline constexpr u64 ended_catch_cookie = 0x5354584C454E4445ull; // "STXLENDE"

struct CatchRecord final {
    u64 cookie;
    HandledException exception;
    // The state of the catch funclet frame this record's catch runs, while
    // a catch nested in that funclet is active (frame_handler.cpp); else
    // no_state_override.
    i64 callee_state;
    // Set by `throw;` inside this catch: an exception leaving the body may
    // be the same object, which then lives on.
    bool rethrown;
    CatchRecord* outer;                  // the next active catch outward, or null
};

// --- the per-thread state (runtime_state.cpp) ----------------------------
[[nodiscard]] i32 uncaught_count() noexcept;
void add_uncaught(i32 change) noexcept;
[[nodiscard]] CatchRecord* innermost_catch() noexcept;
void set_innermost_catch(CatchRecord* record) noexcept;

// True when `object` is the object of an active catch other than `except`.
[[nodiscard]] bool object_still_handled(const void* object, const CatchRecord* except) noexcept;

// --- termination (terminate.cpp) ------------------------------------------
// std::terminate through the C library's handler (the UCRT's terminate,
// which runs the handler set_terminate installed and then aborts).
[[noreturn]] void terminate_now() noexcept;
// A broken invariant of the tables or of this runtime: no handler runs.
[[noreturn]] void fail_fast() noexcept;

// --- raising (throw.cpp) -----------------------------------------------------
[[noreturn]] void raise_handled(const HandledException& exception) noexcept;

// --- objects (frame_handler.cpp) ----------------------------------------------
// Destroys a C++ exception's object through its ThrowInfo's destructor,
// guarded (a destructor that throws terminates).
void destroy_exception_object(const HandledException& exception) noexcept;
// Copies `source` (the exact type, catchable type entry 0) into `destination`.
void copy_exception_object(const CatchableType& type, uptr image_base, void* destination, void* source) noexcept;
[[nodiscard]] const CatchableType* exact_catchable_type(const ThrowInfo& info, uptr image_base) noexcept;

// --- the C++ library's hooks (cxx_hooks.cpp, compiled with exceptions) -----
[[noreturn]] void raise_bad_cast();
[[noreturn]] void raise_bad_typeid();
// A std::bad_alloc object in static storage and its ThrowInfo, for an
// exception_ptr that could not be allocated.
[[nodiscard]] void* bad_alloc_object(const ThrowInfo** info) noexcept;

} // namespace stxlib::msvc_runtime

// --- the thunks (thunks_x64.S) --------------------------------------------------
extern "C" {
// Calls target(first, second, third) under the terminate guard: an
// exception that would leave `target` terminates.
void stxlib_msvc_call_guarded(const void* target, void* first, void* second, void* third);
// Calls the catch funclet with RDX = the parent's establisher frame,
// keeping `record` at [frame + 32] and the cookie at [frame + 40] under the
// catch guard; returns the continuation address the funclet returns.
void* stxlib_msvc_call_catch(const void* funclet, uptr parent_establisher, stxlib::msvc_runtime::CatchRecord* record);

EXCEPTION_DISPOSITION stxlib_msvc_terminate_guard(EXCEPTION_RECORD* record, void* establisher_frame, CONTEXT* context,
                                                  void* dispatcher_context);
EXCEPTION_DISPOSITION stxlib_msvc_catch_guard(EXCEPTION_RECORD* record, void* establisher_frame, CONTEXT* context,
                                              void* dispatcher_context);
}
