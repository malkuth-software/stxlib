#pragma once

// Gates: the only places STXlib-based code catches exceptions (A1 design,
// notes planning/stages/A1-shadowing.md).
//
// call_gate(operation, target, arguments...)
//   For a boundary that recovers (a C ABI slot, a trampoline entry, an
//   interface module's call into the host's C++ library). Calls
//   target(arguments...), which returns Status or Result<Value>, and
//   returns its result. An exception that leaves the target is caught here,
//   turned into an error record for `operation` and returned as the same
//   type's failure. Unwinding therefore runs only inside the caller's side,
//   between the throw and this gate, and never past it.
//
// call_gate_classified(classifier, operation, target, arguments...)
//   As call_gate, with the consumer's policy for its own exception types
//   (GUI.Forms' WrongThreadError and DisposedError) tried first.
//
// invoke_callback(operation, target, arguments...)
// invoke_callback_target(operation, callback, arguments...)
//   For a call into client code that must not throw (survey S2, owner
//   2026-11-13: record, fail fast). An exception that leaves the client's
//   code is recorded and the process fails fast (fail_fast.hpp); nothing
//   unwinds into the caller.
//
// Targets are named functors called with named arguments, passed by
// reference (house style: no lambdas). Built without exceptions
// (__cpp_exceptions undefined), every gate is a direct call that returns
// the target's result: nothing changes shape when GUI.Forms' release builds
// drop exceptions after its 1.0.
//
// Each gate opens a shadow frame for its operation around the target and
// marks it closed when the target returns (shadow.hpp), so an exception
// that leaves the target is seen by the frames it unwinds, whichever module
// threw it, and the innermost captures the stack at the throw (survey S1 =
// 1). A caught exception is recorded by one internal function
// (src/core/exception_shadow.cpp), which attaches that capture.

#include "stxlib/callback.hpp"
#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/shadow.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// What a consumer's classifier decides for an exception it recognises.
struct ExceptionClass final {
    ErrorKind kind{ErrorKind::internal_failure};
    u32 code{0};
};

// A consumer's exception policy. `classify` runs inside the gate's handler,
// may rethrow the current exception inside its own try block to test its
// type, and returns true with `decided` filled when it recognises it; false
// leaves the standard classification. It must not let an exception escape.
struct ExceptionClassifier final {
    bool (*classify)(void* context, ExceptionClass& decided) noexcept{nullptr};
    void* context{nullptr};
};

namespace detail {

// Never defined: names a value of Type in unevaluated operands only.
template <typename Type>
Type&& declare() noexcept;

// The type target(arguments...) returns.
template <typename Target, typename... Arguments>
struct TargetReturn final {
    using Type = decltype(declare<Target&>()(declare<Arguments&>()...));
};

template <typename Type>
struct IsGateResult final {
    static constexpr bool value = false;
};
template <>
struct IsGateResult<Status> final {
    static constexpr bool value = true;
};
template <typename Value>
struct IsGateResult<Result<Value>> final {
    static constexpr bool value = true;
};

template <typename Left, typename Right>
struct SameType final {
    static constexpr bool value = false;
};
template <typename Type>
struct SameType<Type, Type> final {
    static constexpr bool value = true;
};

// Called only inside a catch handler. Classifies the current exception
// (standard types; then foreign_exception), copies its what() text when it
// is a standard exception, adds its type in a development build, keeps the
// record in the thread's recent failures, and returns it. Defined in
// src/core/exception_shadow.cpp, which is compiled with exceptions in every
// build. Allocates nothing.
[[nodiscard]] ErrorRecord record_current_exception(Operation operation) noexcept;
// As above, asking `classifier` first.
[[nodiscard]] ErrorRecord record_current_exception_classified(Operation operation,
                                                              const ExceptionClassifier& classifier) noexcept;

// Calls a CallbackTarget's function with its context.
template <typename Return, typename... Arguments>
struct CallbackCall final {
    const CallbackTarget<Return(Arguments...)>& callback;

    Return operator()(Arguments&... arguments) const {
        typename CallbackTarget<Return(Arguments...)>::Function function = callback.function();
        if constexpr (SameType<Return, void>::value) {
            function(callback.context(), arguments...);
        } else {
            Return result = function(callback.context(), arguments...);
            return result;
        }
    }
};

} // namespace detail

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate(const Operation operation,
                                                                                  Target& target,
                                                                                  Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
    static_assert(detail::IsGateResult<Return>::value, "a gated target returns Status or Result<Value>");
#if defined(__cpp_exceptions)
    try {
        detail::ShadowFrameScope frame(operation, 0, 0);
        Return result = target(arguments...);
        frame.close();
        return result;
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception(operation);
        return Return::failure(record);
    }
#else
    detail::ShadowFrameScope frame(operation, 0, 0);
    Return result = target(arguments...);
    frame.close();
    return result;
#endif
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate_classified(
    const ExceptionClassifier& classifier, const Operation operation, Target& target, Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
    static_assert(detail::IsGateResult<Return>::value, "a gated target returns Status or Result<Value>");
#if defined(__cpp_exceptions)
    try {
        detail::ShadowFrameScope frame(operation, 0, 0);
        Return result = target(arguments...);
        frame.close();
        return result;
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception_classified(operation, classifier);
        return Return::failure(record);
    }
#else
    static_cast<void>(classifier);
    detail::ShadowFrameScope frame(operation, 0, 0);
    Return result = target(arguments...);
    frame.close();
    return result;
#endif
}

template <typename Target, typename... Arguments>
typename detail::TargetReturn<Target, Arguments...>::Type invoke_callback(const Operation operation, Target& target,
                                                                          Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
#if defined(__cpp_exceptions)
    try {
        if constexpr (detail::SameType<Return, void>::value) {
            in_shadow_frame(operation, 0, 0, target, arguments...);
            return;
        } else {
            Return result = in_shadow_frame(operation, 0, 0, target, arguments...);
            return result;
        }
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception(operation);
        fail_fast(record);
    }
#else
    if constexpr (detail::SameType<Return, void>::value) {
        in_shadow_frame(operation, 0, 0, target, arguments...);
    } else {
        Return result = in_shadow_frame(operation, 0, 0, target, arguments...);
        return result;
    }
#endif
}

// Requirement: callback.bound(). The arguments are lvalues of the
// callback's parameter types.
template <typename Return, typename... Arguments>
Return invoke_callback_target(const Operation operation, const CallbackTarget<Return(Arguments...)>& callback,
                              Arguments&... arguments) noexcept {
    STXLIB_REQUIRE(callback.bound(), "invoke_callback_target: unbound callback");
    detail::CallbackCall<Return, Arguments...> call{callback};
    if constexpr (detail::SameType<Return, void>::value) {
        invoke_callback(operation, call, arguments...);
    } else {
        Return result = invoke_callback(operation, call, arguments...);
        return result;
    }
}

} // namespace stxlib
