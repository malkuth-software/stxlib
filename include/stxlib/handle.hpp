#pragma once

// UniqueHandle<Traits>: the unique owner of a foreign resource identified by
// a value (a descriptor, a Windows HANDLE, a library object pointer), which
// is released through the call its foreign contract names (house style,
// section 2: thin wrappers restore native resource conventions).
//
// Traits opt in explicitly (house style, section 7: no member detection
// choosing architecture) and supply:
//
//   static constexpr bool stxlib_handle_traits = true;
//   using Value = <the handle's value type>;
//   static constexpr Value empty_value() noexcept;      // owns nothing
//   static constexpr bool is_empty(Value value) noexcept;
//   static void release(Value value) noexcept;          // the foreign release
//
// Interface modules define the traits; their headers are private, so no
// platform type reaches a public header.

#include "stxlib/types.hpp"

namespace stxlib {

template <typename Traits>
class UniqueHandle final {
    static_assert(Traits::stxlib_handle_traits, "UniqueHandle needs traits that opt in");

public:
    using Value = typename Traits::Value;

    // Owns nothing.
    constexpr UniqueHandle() noexcept : value_(Traits::empty_value()) {}

    // Takes ownership of `value`: this handle releases it.
    [[nodiscard]] static UniqueHandle adopt(const Value value) noexcept {
        UniqueHandle result;
        result.value_ = value;
        return result;
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_) { other.value_ = Traits::empty_value(); }

    // Releases what this handle owns, then takes other's; other owns nothing.
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        value_ = other.value_;
        other.value_ = Traits::empty_value();
        return *this;
    }

    ~UniqueHandle() { reset(); }

    [[nodiscard]] bool owns() const noexcept {
        const bool empty = Traits::is_empty(value_);
        return !empty;
    }

    // The value, still owned by this handle (a borrow for a foreign call).
    [[nodiscard]] Value get() const noexcept { return value_; }

    // Gives up ownership without releasing: the caller now owns the value
    // and this handle owns nothing.
    [[nodiscard]] Value relinquish() noexcept {
        const Value result = value_;
        value_ = Traits::empty_value();
        return result;
    }

    // Releases what this handle owns; it owns nothing afterwards.
    void reset() noexcept {
        if (Traits::is_empty(value_)) return;
        const Value owned = value_;
        value_ = Traits::empty_value();
        Traits::release(owned);
    }

private:
    Value value_;
};

} // namespace stxlib
