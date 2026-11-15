#pragma once

// Constructing and destroying objects in storage STXlib owns, for the owned
// vocabulary (vector.hpp, optional.hpp, unique.hpp, shared.hpp), without a
// standard header (AGENTS.md: no angle includes in a public header).
//
// The standard placement form `operator new(std::size_t, void*)` lives in
// <new>. STXlib declares its own placement allocation function instead,
// told apart by a parameter of STXlib's own tag type. [new.delete.placement]
// reserves only the library's own placement forms; a form with an extra
// parameter of a program-defined type is an ordinary placement allocation
// function ([expr.new], [basic.stc.dynamic.allocation]), the same mechanism
// as `new (std::nothrow)`. It is non-throwing and returns the storage it is
// given, so a new-expression through it allocates nothing.

#include "stxlib/types.hpp"

namespace stxlib::detail {

struct ConstructTag final {};

} // namespace stxlib::detail

[[nodiscard]] inline void* operator new(const stxlib::usize, const stxlib::detail::ConstructTag,
                                        void* const storage) noexcept {
    return storage;
}

// The matching deallocation function, which a new-expression calls only if
// the constructor throws. Every constructor the vocabulary calls through
// construct() is required to be non-throwing (each type's static_assert), so
// this is never reached; it releases nothing, since the storage is the
// caller's.
inline void operator delete(void* const, const stxlib::detail::ConstructTag, void* const) noexcept {}

namespace stxlib::detail {

// Constructs a Value in `storage` from `arguments`, forwarded as given.
// Requirement: storage is non-null, aligned for Value, at least sizeof(Value)
// bytes, and holds no live object.
template <typename Value, typename... Arguments>
Value* construct(void* const storage, Arguments&&... arguments) noexcept {
    static_assert(__is_nothrow_constructible(Value, Arguments&&...),
                  "the owned vocabulary constructs only through non-throwing constructors");
    Value* const result = ::new (ConstructTag{}, storage) Value(static_cast<Arguments&&>(arguments)...);
    return result;
}

// Ends the lifetime of *object. Its storage is not released.
template <typename Value>
void destroy(Value* const object) noexcept {
    (*object).~Value();
}

// True when destroying a Value cannot throw. A destructor is noexcept unless
// it says otherwise, so this fails only for a type that opts out.
template <typename Value>
inline constexpr bool nothrow_destructible = noexcept((*static_cast<Value*>(nullptr)).~Value());

// True when a From* converts implicitly to a To* (a derived class to an
// unambiguous public base, or to a more const-qualified pointer). Used to
// constrain the owning pointers' converting constructors.
template <typename From, typename To>
struct PointerConverts final {
private:
    // Declared only: named in an unevaluated sizeof, never called.
    struct Converts final {
        char byte;
    };
    struct DoesNotConvert final {
        char bytes[2];
    };
    static Converts test(To* pointer) noexcept;
    static DoesNotConvert test(...) noexcept;

public:
    static constexpr bool value = sizeof(test(static_cast<From*>(nullptr))) == sizeof(Converts);
};

// The larger of two sizes, for alignments and block layouts.
[[nodiscard]] constexpr usize larger(const usize left, const usize right) noexcept {
    const usize result = left < right ? right : left;
    return result;
}

} // namespace stxlib::detail
