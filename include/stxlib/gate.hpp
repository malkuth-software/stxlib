#pragma once

// Gates: the only places STXlib-based code catches exceptions (A1 design,
// notes planning/stages/A1-shadowing.md).
//
// Cancellation (owner decision 2026-11-15, audit E, 1): on glibc with
// libstdc++, a thread cancellation or pthread_exit unwinds as the foreign
// exception abi::__forced_unwind. A gate that catches it fails fast with an
// interrupted record naming cancellation; it is never turned into a result.
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
// call_as_result(operation, target, arguments...)
// call_as_result_classified(classifier, operation, target, arguments...)
//   As the two above, for a target that returns a plain value or nothing:
//   the value comes back as Result<Value>'s success (nothing as Status's),
//   and an exception as the failure. For code that offers a result-returning
//   form of an operation whose implementation still throws (notes
//   planning/stages/A3-no-exceptions.md, stage G4); built without
//   exceptions, it is the target's call wrapped as a success.
//
// Every gate also takes a GateFrame (shadow.hpp) in place of the
// operation: the gate's shadow frame is then the whole frame, with the
// consumer's kind, numbers, sequence and texts (a C ABI slot's table and
// slot index, an event callback's sender and event name; A2 swap-in), so a
// consumer frame around a gated call does not appear twice. The operation
// alone means GateFrame{operation, 0, 0}.
//
// invoke_callback(operation, target, arguments...)
// invoke_callback_classified(classifier, frame, target, arguments...)
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
//
// An STXlib built with STXLIB_EXCEPTIONS=OFF (CMakeLists.txt) has no such
// function: it serves processes with no exceptions anywhere, and this
// header refuses a translation unit that has them.

#include "stxlib/callback.hpp"
#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/shadow.hpp"
#include "stxlib/types.hpp"

#if defined(__cpp_exceptions) && defined(STXLIB_NO_EXCEPTION_RECORDER)
#error "STXlib was built with STXLIB_EXCEPTIONS=OFF and records no exceptions: build this translation unit with -fno-exceptions, or STXlib with STXLIB_EXCEPTIONS=ON"
#endif

namespace stxlib {

// What a consumer's classifier decides for an exception it recognises.
struct ExceptionClass final {
    ErrorKind kind{ErrorKind::internal_failure};
    u32 code{0};
};

// A consumer's exception policy. `classify` runs inside the gate's handler,
// on the catching thread, exactly once per caught exception (whatever it
// returns), before the record is made, with `context` passed through
// unchanged. It may rethrow the current exception inside its own try block
// to test its type, and returns true with `decided` filled when it
// recognises it; false leaves the standard classification. It must not let
// an exception escape or longjmp out (AGENTS.md, early exits).
//
// The message sink (decision A2-2 = 1): ErrorRecord keeps
// error_message_capacity (200) bytes of message. A consumer that keeps more
// (GUI.Forms: 512) copies the full what() itself, from classify, into a
// buffer its context points to; the guarantee above is what makes that
// copy belong to the record the gate then makes.
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

// Names Type in a non-deduced context, so that a parameter pack is deduced
// from one parameter only.
template <typename Type>
struct NonDeduced final {
    using type = Type;
};

template <typename Type>
struct IsReference final {
    static constexpr bool value = false;
};
template <typename Type>
struct IsReference<Type&> final {
    static constexpr bool value = true;
};
template <typename Type>
struct IsReference<Type&&> final {
    static constexpr bool value = true;
};

template <typename Value>
struct WithoutConst final {
    using Type = Value;
};
template <typename Value>
struct WithoutConst<const Value> final {
    using Type = Value;
};

// What call_as_result returns for a target returning Return: Status for
// void, Result<Return> (without const) otherwise.
template <typename Return>
struct AsResult final {
    using Type = Result<typename WithoutConst<Return>::Type>;
};
template <>
struct AsResult<void> final {
    using Type = Status;
};

// A gate target that calls `target` and returns what it returns as a
// success, so that call_gate's handler supplies the failure.
template <typename Target>
struct ReturnsAsResult final {
    Target& target;

    template <typename... Arguments>
    typename AsResult<typename TargetReturn<Target, Arguments...>::Type>::Type operator()(
        Arguments&... arguments) const {
        using Return = typename TargetReturn<Target, Arguments...>::Type;
        if constexpr (SameType<Return, void>::value) {
            target(arguments...);
            return Status::success();
        } else {
            using Value = typename WithoutConst<Return>::Type;
            Value value = target(arguments...);
            return Result<Value>::success(static_cast<Value&&>(value));
        }
    }
};

// The checks every call_as_result form makes on its target's return type.
template <typename Return>
struct AsResultTarget final {
    static_assert(!IsGateResult<typename WithoutConst<Return>::Type>::value,
                  "the target returns Status or Result already: use call_gate");
    static_assert(!IsReference<Return>::value,
                  "call_as_result does not carry a reference: return a value or a pointer");
    using Type = typename AsResult<Return>::Type;
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
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate(const GateFrame frame_at,
                                                                                  Target& target,
                                                                                  Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
    static_assert(detail::IsGateResult<Return>::value, "a gated target returns Status or Result<Value>");
#if defined(__cpp_exceptions)
    try {
        detail::ShadowFrameScope frame(detail::push_of(frame_at));
        Return result = target(arguments...);
        frame.close();
        return result;
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception(frame_at.operation);
        return Return::failure(record);
    }
#else
    detail::ShadowFrameScope frame(detail::push_of(frame_at));
    Return result = target(arguments...);
    frame.close();
    return result;
#endif
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate(const Operation operation,
                                                                                  Target& target,
                                                                                  Arguments&... arguments) noexcept {
    const GateFrame frame_at{operation, 0, 0};
    // Not const: a const local is copied, not moved, on return.
    typename detail::TargetReturn<Target, Arguments...>::Type result = call_gate(frame_at, target, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate_classified(
    const ExceptionClassifier& classifier, const GateFrame frame_at, Target& target, Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
    static_assert(detail::IsGateResult<Return>::value, "a gated target returns Status or Result<Value>");
#if defined(__cpp_exceptions)
    try {
        detail::ShadowFrameScope frame(detail::push_of(frame_at));
        Return result = target(arguments...);
        frame.close();
        return result;
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception_classified(frame_at.operation, classifier);
        return Return::failure(record);
    }
#else
    static_cast<void>(classifier);
    detail::ShadowFrameScope frame(detail::push_of(frame_at));
    Return result = target(arguments...);
    frame.close();
    return result;
#endif
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type call_gate_classified(
    const ExceptionClassifier& classifier, const Operation operation, Target& target, Arguments&... arguments) noexcept {
    const GateFrame frame_at{operation, 0, 0};
    // Not const: a const local is copied, not moved, on return.
    typename detail::TargetReturn<Target, Arguments...>::Type result =
        call_gate_classified(classifier, frame_at, target, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type
call_as_result(const GateFrame frame_at, Target& target, Arguments&... arguments) noexcept {
    using Wrapped = typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type;
    detail::ReturnsAsResult<Target> wrapped{target};
    // Not const: a const local is copied, not moved, on return.
    Wrapped result = call_gate(frame_at, wrapped, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type
call_as_result(const Operation operation, Target& target, Arguments&... arguments) noexcept {
    using Wrapped = typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type;
    const GateFrame frame_at{operation, 0, 0};
    Wrapped result = call_as_result(frame_at, target, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type
call_as_result_classified(const ExceptionClassifier& classifier, const GateFrame frame_at, Target& target,
                          Arguments&... arguments) noexcept {
    using Wrapped = typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type;
    detail::ReturnsAsResult<Target> wrapped{target};
    Wrapped result = call_gate_classified(classifier, frame_at, wrapped, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
[[nodiscard]] typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type
call_as_result_classified(const ExceptionClassifier& classifier, const Operation operation, Target& target,
                          Arguments&... arguments) noexcept {
    using Wrapped = typename detail::AsResultTarget<typename detail::TargetReturn<Target, Arguments...>::Type>::Type;
    const GateFrame frame_at{operation, 0, 0};
    Wrapped result = call_as_result_classified(classifier, frame_at, target, arguments...);
    return result;
}

template <typename Target, typename... Arguments>
typename detail::TargetReturn<Target, Arguments...>::Type invoke_callback(const GateFrame frame_at, Target& target,
                                                                          Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
#if defined(__cpp_exceptions)
    try {
        if constexpr (detail::SameType<Return, void>::value) {
            detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
            return;
        } else {
            // Not const: a const local is copied, not moved, on return.
            Return result = detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
            return result;
        }
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception(frame_at.operation);
        fail_fast(record);
    }
#else
    if constexpr (detail::SameType<Return, void>::value) {
        detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
    } else {
        // Not const: a const local is copied, not moved, on return.
        Return result = detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
        return result;
    }
#endif
}

template <typename Target, typename... Arguments>
typename detail::TargetReturn<Target, Arguments...>::Type invoke_callback(const Operation operation, Target& target,
                                                                          Arguments&... arguments) noexcept {
    const GateFrame frame_at{operation, 0, 0};
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
    if constexpr (detail::SameType<Return, void>::value) {
        invoke_callback(frame_at, target, arguments...);
    } else {
        // Not const: a const local is copied, not moved, on return.
        Return result = invoke_callback(frame_at, target, arguments...);
        return result;
    }
}

// As invoke_callback, with the record made by the consumer's classifier
// (so its message sink keeps the full message for the fail-fast report).
template <typename Target, typename... Arguments>
typename detail::TargetReturn<Target, Arguments...>::Type invoke_callback_classified(
    const ExceptionClassifier& classifier, const GateFrame frame_at, Target& target, Arguments&... arguments) noexcept {
    using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
#if defined(__cpp_exceptions)
    try {
        if constexpr (detail::SameType<Return, void>::value) {
            detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
            return;
        } else {
            // Not const: a const local is copied, not moved, on return.
            Return result = detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
            return result;
        }
    } catch (...) {
        const ErrorRecord record = detail::record_current_exception_classified(frame_at.operation, classifier);
        fail_fast(record);
    }
#else
    static_cast<void>(classifier);
    if constexpr (detail::SameType<Return, void>::value) {
        detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
    } else {
        // Not const: a const local is copied, not moved, on return.
        Return result = detail::call_in_frame(detail::push_of(frame_at), target, arguments...);
        return result;
    }
#endif
}

// Requirement: callback.bound(). The arguments are lvalues of the
// callback's parameter types; Arguments is deduced from the callback alone
// (a reference parameter such as int& would otherwise conflict with the
// int deduced from an lvalue argument).
template <typename Return, typename... Arguments>
Return invoke_callback_target(const GateFrame frame_at, const CallbackTarget<Return(Arguments...)>& callback,
                              typename detail::NonDeduced<Arguments>::type&... arguments) noexcept {
    STXLIB_REQUIRE(callback.bound(), "invoke_callback_target: unbound callback");
    detail::CallbackCall<Return, Arguments...> call{callback};
    if constexpr (detail::SameType<Return, void>::value) {
        invoke_callback(frame_at, call, arguments...);
    } else {
        Return result = invoke_callback(frame_at, call, arguments...);
        return result;
    }
}

template <typename Return, typename... Arguments>
Return invoke_callback_target(const Operation operation, const CallbackTarget<Return(Arguments...)>& callback,
                              typename detail::NonDeduced<Arguments>::type&... arguments) noexcept {
    const GateFrame frame_at{operation, 0, 0};
    if constexpr (detail::SameType<Return, void>::value) {
        invoke_callback_target(frame_at, callback, arguments...);
    } else {
        Return result = invoke_callback_target(frame_at, callback, arguments...);
        return result;
    }
}

} // namespace stxlib
