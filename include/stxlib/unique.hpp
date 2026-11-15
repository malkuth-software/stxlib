#pragma once

// Unique<Value>: the unique owner of one object on STXlib's allocator, the
// owned vocabulary's replacement for std::unique_ptr at a public surface
// (owner, D15 before 1.0).
//
// Made only by make() (allocation and construction in one step, failure as
// out_of_memory, D8 = a). It never adopts a pointer from elsewhere: memory we
// allocate is released only by us, and we never release foreign memory
// (house style, section 1; AGENTS.md).
//
// Polymorphism: a Unique<Derived> converts to a Unique<Base> when Derived*
// converts to Base*. The object is still destroyed as the type make()
// built, through a destroy function recorded at make(), so Base needs no
// virtual destructor and a base subobject at a non-zero offset (multiple
// inheritance) is released at its block's true start.
//
// Layout (defined here, the same on every host): the object pointer (as
// Value*), the block's start, then the destroy function. Empty: all null.
// Moving transfers ownership and leaves the source empty; copying is
// deleted. Access is through operator* ((*owner).member, house style,
// section 11) and get(); there is no operator->.

#include "stxlib/detail/construct.hpp"
#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/error.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

template <typename Value>
class Unique;

namespace detail {

// Destroys the object of type Built at `block` (where make() constructed
// it) and releases the block.
template <typename Built>
void destroy_unique_block(void* const block) noexcept {
    Built* const object = static_cast<Built*>(block);
    destroy(object);
    vocabulary_release(block);
}

} // namespace detail

template <typename Value>
class Unique final {
    static_assert(detail::nothrow_destructible<Value>, "Unique's Value must be nothrow destructible");

public:
    using DestroyFunction = void (*)(void* block) noexcept;

    // Owns nothing.
    constexpr Unique() noexcept = default;

    // A new Value constructed from `arguments` (forwarded), in a block of
    // its own. Fails with out_of_memory, constructing nothing, when the
    // allocator refuses.
    template <typename... Arguments>
    [[nodiscard]] static Result<Unique> make(Arguments&&... arguments) noexcept {
        static_assert(__is_nothrow_constructible(Value, Arguments&&...),
                      "Unique::make needs a nothrow constructor for these arguments");
        void* const block = detail::vocabulary_allocate(sizeof(Value), alignof(Value));
        if (block == nullptr) {
            const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.unique.make");
            return Result<Unique>::failure(exhausted);
        }
        Value* const object = detail::construct<Value>(block, static_cast<Arguments&&>(arguments)...);
        Unique result;
        result.object_ = object;
        result.block_ = block;
        result.destroy_ = &detail::destroy_unique_block<Value>;
        return Result<Unique>::success(static_cast<Unique&&>(result));
    }

    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;

    Unique(Unique&& other) noexcept : object_(other.object_), block_(other.block_), destroy_(other.destroy_) {
        other.object_ = nullptr;
        other.block_ = nullptr;
        other.destroy_ = nullptr;
    }

    // From an owner of a derived (or less const-qualified) type: takes its
    // object, which is destroyed as the type it was built as.
    template <typename Other>
        requires(detail::PointerConverts<Other, Value>::value)
    Unique(Unique<Other>&& other) noexcept
        : object_(other.object_), block_(other.block_), destroy_(other.destroy_) {
        other.object_ = nullptr;
        other.block_ = nullptr;
        other.destroy_ = nullptr;
    }

    // Destroys what this owns, then takes other's object; other is empty.
    Unique& operator=(Unique&& other) noexcept {
        if (this == &other) return *this;
        reset();
        object_ = other.object_;
        block_ = other.block_;
        destroy_ = other.destroy_;
        other.object_ = nullptr;
        other.block_ = nullptr;
        other.destroy_ = nullptr;
        return *this;
    }

    ~Unique() { reset(); }

    [[nodiscard]] bool present() const noexcept { return object_ != nullptr; }
    [[nodiscard]] bool empty() const noexcept { return object_ == nullptr; }
    explicit operator bool() const noexcept { return object_ != nullptr; }

    // The object, still owned by this. Null when empty.
    [[nodiscard]] Value* get() const noexcept { return object_; }

    // Requirement: present().
    [[nodiscard]] Value& operator*() const noexcept {
        STXLIB_REQUIRE(object_ != nullptr, "Unique: dereference of an empty owner");
        return *object_;
    }

    // Destroys and releases the object; this owns nothing afterwards.
    void reset() noexcept {
        if (object_ == nullptr) return;
        void* const block = block_;
        const DestroyFunction destroy = destroy_;
        object_ = nullptr;
        block_ = nullptr;
        destroy_ = nullptr;
        destroy(block);
    }

    [[nodiscard]] bool operator==(const Unique& other) const noexcept { return object_ == other.object_; }
    [[nodiscard]] bool operator!=(const Unique& other) const noexcept { return object_ != other.object_; }

private:
    template <typename Other>
    friend class Unique;

    Value* object_{nullptr};
    void* block_{nullptr};
    DestroyFunction destroy_{nullptr};
};

} // namespace stxlib
