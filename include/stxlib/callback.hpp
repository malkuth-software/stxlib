#pragma once

// Named callback targets with context (house style, section 5: a
// non-owning delegate when a function and a context pointer suffice;
// binding need not allocate).
//
// CallbackTarget<Return(Arguments...)> holds a function pointer and the
// context it is called with. It owns nothing: whoever binds it keeps the
// context alive until the target can no longer be invoked, and states how
// (a subscription's revocation, a trampoline's revocation, the end of an
// operation).
//
// A CallbackTarget is invoked only through invoke_callback (gate.hpp), the
// callback gate: it has no call operator, so no call can bypass the gate.

#include "stxlib/types.hpp"

namespace stxlib {

template <typename Signature>
class CallbackTarget;

template <typename Return, typename... Arguments>
class CallbackTarget<Return(Arguments...)> final {
public:
    using Function = Return (*)(void* context, Arguments... arguments);

    // Unbound.
    constexpr CallbackTarget() noexcept = default;

    // Binds `bound_function` with `bound_context`, which is borrowed (see
    // above).
    [[nodiscard]] static constexpr CallbackTarget bind(const Function bound_function, void* const bound_context) noexcept {
        CallbackTarget result;
        result.function_ = bound_function;
        result.context_ = bound_context;
        return result;
    }

    [[nodiscard]] constexpr bool bound() const noexcept { return function_ != nullptr; }
    [[nodiscard]] constexpr Function function() const noexcept { return function_; }
    [[nodiscard]] constexpr void* context() const noexcept { return context_; }

private:
    Function function_{nullptr};
    void* context_{nullptr};
};

} // namespace stxlib
