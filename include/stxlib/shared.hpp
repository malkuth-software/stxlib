#pragma once

// Shared<Value>, Weak<Value> and SharedFromThis<Value>: shared ownership of
// one object on STXlib's allocator, the owned vocabulary's replacement for
// std::shared_ptr, std::weak_ptr and std::enable_shared_from_this at a
// public surface (owner, D15 before 1.0). House style, section 1: shared
// ownership for genuinely shared lifetimes; observational links (Weak) do
// not extend the lifetime; prevent cycles.
//
// One block per object, made by Shared::make (D8 = a: failure is
// out_of_memory, nothing constructed). The block starts with its header,
// then (aligned for the object) the object:
//
//   SharedHeader { u64 owners; u64 observers; destroy function }   24 bytes
//   padding to the object's alignment
//   the object
//
// owners counts the Shared values. observers counts the Weak values, plus
// one held jointly by all owners while any exists. When owners reaches zero
// the object is destroyed (as the type make() built, through the header's
// destroy function, so a base needs no virtual destructor); when observers
// reaches zero the block is released. Both counts are atomic (the GCC and
// Clang __atomic builtins, the compilers types.hpp admits): an increment is
// relaxed, a decrement acquire-release, so the thread that destroys sees
// every other owner's writes; Weak::lock increments owners only from a
// non-zero value (a compare-exchange loop), so it never revives a destroyed
// object. Neither count can overflow: each owner or observer is a distinct
// object of at least 16 bytes, so 2^64 of them cannot exist in a 64-bit
// address space.
//
// Thread safety: as std::shared_ptr. Distinct Shared or Weak values that
// share one block may be copied, moved and destroyed on any threads at
// once; one Shared or Weak value is not itself synchronised.
//
// Layout of Shared<Value> and Weak<Value> (defined here, the same on every
// host): the object pointer (as Value*), then the header pointer. Empty:
// both null. Access is through operator* ((*shared).member, house style,
// section 11) and get(); there is no operator->.

// Early exits (owner, 2026-11-15, decisions/2026-11-15-early-exit-
// integrity.md): the object's constructor and destructor are the
// consumer's code. Form 1 at every call: make constructs into a block no
// owner holds yet; reset and both assignments repoint this owner (or
// observer) before dropping the previous count, and the last owner's
// count is already zero when the destructor runs, so Weak::lock refuses
// the object during and after it. An early exit from the destructor leaves
// the owners' joint observer count held: the block leaks, every count
// stays truthful.

#include "stxlib/detail/construct.hpp"
#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/error.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

template <typename Value>
class Shared;
template <typename Value>
class Weak;
template <typename Value>
class SharedFromThis;

namespace detail {

struct SharedHeader final {
    u64 owners;
    u64 observers;
    void (*destroy_object)(SharedHeader* header) noexcept;
};

static_assert(sizeof(SharedHeader) == 24, "the shared block header is 24 bytes on 64-bit targets");

// The object's offset from the block's start: the header, rounded up to the
// object's alignment (a power of two).
template <typename Built>
inline constexpr usize shared_object_offset =
    (sizeof(SharedHeader) + alignof(Built) - 1) / alignof(Built) * alignof(Built);

template <typename Built>
[[nodiscard]] Built* shared_object_of(SharedHeader* const header) noexcept {
    Byte* const bytes = static_cast<Byte*>(static_cast<void*>(header));
    Built* const object = static_cast<Built*>(static_cast<void*>(bytes + shared_object_offset<Built>));
    Built* const result = __builtin_launder(object);
    return result;
}

template <typename Built>
void destroy_shared_object(SharedHeader* const header) noexcept {
    Built* const object = shared_object_of<Built>(header);
    destroy(object);
}

inline void add_owner(SharedHeader* const header) noexcept {
    __atomic_fetch_add(&(*header).owners, static_cast<u64>(1), __ATOMIC_RELAXED);
}

inline void add_observer(SharedHeader* const header) noexcept {
    __atomic_fetch_add(&(*header).observers, static_cast<u64>(1), __ATOMIC_RELAXED);
}

// Drops one observer; the last one releases the block.
inline void drop_observer(SharedHeader* const header) noexcept {
    const u64 before = __atomic_fetch_sub(&(*header).observers, static_cast<u64>(1), __ATOMIC_ACQ_REL);
    if (before == 1) vocabulary_release(header);
}

// Drops one owner; the last one destroys the object, then drops the owners'
// joint observer.
inline void drop_owner(SharedHeader* const header) noexcept {
    const u64 before = __atomic_fetch_sub(&(*header).owners, static_cast<u64>(1), __ATOMIC_ACQ_REL);
    if (before != 1) return;
    (*header).destroy_object(header);
    drop_observer(header);
}

// Adds an owner only while at least one exists (Weak::lock). False when the
// object is already destroyed or being destroyed.
inline bool add_owner_if_alive(SharedHeader* const header) noexcept {
    u64 expected = __atomic_load_n(&(*header).owners, __ATOMIC_RELAXED);
    while (expected != 0) {
        const bool swapped = __atomic_compare_exchange_n(&(*header).owners, &expected, expected + 1, false,
                                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        if (swapped) return true;
    }
    return false;
}

// The non-template base of SharedFromThis: a non-owning link from an object
// to its own block, set by Shared::make. The block outlives the object, so
// the link is valid for the object's whole life. A copy of the object is a
// different object with no block of its own yet, so copying never copies
// the link.
class SharedSelf {
protected:
    SharedSelf() noexcept = default;
    SharedSelf(const SharedSelf&) noexcept : header_(nullptr) {}
    SharedSelf& operator=(const SharedSelf&) noexcept { return *this; }
    ~SharedSelf() = default;

    SharedHeader* header_{nullptr};

private:
    template <typename Value>
    friend class stxlib::Shared;
};

} // namespace detail

template <typename Value>
class Shared final {
    // Value may be incomplete where Shared<Value> is named (an object that
    // holds a Shared or Weak of its own type), so the requirement on its
    // destructor is checked where an object is made (make), not here.

public:
    // Owns nothing.
    constexpr Shared() noexcept = default;

    // A new Value constructed from `arguments` (forwarded) in a block with
    // its header; the result is its first owner. Fails with out_of_memory,
    // constructing nothing, when the allocator refuses. A Value derived from
    // SharedFromThis gets its link to the block here.
    template <typename... Arguments>
    [[nodiscard]] static OperationResult<Shared> make(Arguments&&... arguments) noexcept {
        static_assert(__is_nothrow_constructible(Value, Arguments&&...),
                      "Shared::make needs a nothrow constructor for these arguments");
        static_assert(detail::nothrow_destructible<Value>, "Shared's Value must be nothrow destructible");
        const usize offset = detail::shared_object_offset<Value>;
        const usize alignment = detail::larger(alignof(detail::SharedHeader), alignof(Value));
        void* const block = detail::vocabulary_allocate(offset + sizeof(Value), alignment);
        if (block == nullptr) {
            const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.shared.make");
            return OperationResult<Shared>::failure(exhausted);
        }
        detail::SharedHeader* const header = static_cast<detail::SharedHeader*>(block);
        (*header).owners = 1;
        (*header).observers = 1;
        (*header).destroy_object = &detail::destroy_shared_object<Value>;
        Byte* const bytes = static_cast<Byte*>(block);
        Value* const object = detail::construct<Value>(bytes + offset, static_cast<Arguments&&>(arguments)...);
        if constexpr (__is_base_of(detail::SharedSelf, Value)) {
            detail::SharedSelf& self = static_cast<detail::SharedSelf&>(*object);
            self.header_ = header;
        }
        Shared result;
        result.object_ = object;
        result.header_ = header;
        return OperationResult<Shared>::success(static_cast<Shared&&>(result));
    }

    // Another owner of the same object.
    Shared(const Shared& other) noexcept : object_(other.object_), header_(other.header_) {
        if (header_ != nullptr) detail::add_owner(header_);
    }
    template <typename Other>
        requires(detail::PointerConverts<Other, Value>::value)
    Shared(const Shared<Other>& other) noexcept : object_(other.object_), header_(other.header_) {
        if (header_ != nullptr) detail::add_owner(header_);
    }

    // Takes other's ownership; other is empty.
    Shared(Shared&& other) noexcept : object_(other.object_), header_(other.header_) {
        other.object_ = nullptr;
        other.header_ = nullptr;
    }
    template <typename Other>
        requires(detail::PointerConverts<Other, Value>::value)
    Shared(Shared<Other>&& other) noexcept : object_(other.object_), header_(other.header_) {
        other.object_ = nullptr;
        other.header_ = nullptr;
    }

    // Becomes another owner of other's object (the new owner is added before
    // the old ownership is dropped, so assigning an owner of the same object
    // never destroys it).
    Shared& operator=(const Shared& other) noexcept {
        if (other.header_ != nullptr) detail::add_owner(other.header_);
        detail::SharedHeader* const previous = header_;
        object_ = other.object_;
        header_ = other.header_;
        if (previous != nullptr) detail::drop_owner(previous);
        return *this;
    }

    // Drops this ownership, then takes other's; other is empty.
    Shared& operator=(Shared&& other) noexcept {
        if (this == &other) return *this;
        detail::SharedHeader* const previous = header_;
        object_ = other.object_;
        header_ = other.header_;
        other.object_ = nullptr;
        other.header_ = nullptr;
        if (previous != nullptr) detail::drop_owner(previous);
        return *this;
    }

    ~Shared() { reset(); }

    [[nodiscard]] bool present() const noexcept { return object_ != nullptr; }
    [[nodiscard]] bool empty() const noexcept { return object_ == nullptr; }
    explicit operator bool() const noexcept { return object_ != nullptr; }

    // The object, owned by this and its fellow owners. Null when empty.
    [[nodiscard]] Value* get() const noexcept { return object_; }

    // Requirement: present().
    [[nodiscard]] Value& operator*() const noexcept {
        STXLIB_REQUIRE(object_ != nullptr, "Shared: dereference of an empty owner");
        return *object_;
    }

    // The number of owners at the moment of the call (0 when empty). Another
    // thread may change it at once; for tests and diagnostics only.
    [[nodiscard]] u64 owner_count() const noexcept {
        if (header_ == nullptr) return 0;
        const u64 result = __atomic_load_n(&(*header_).owners, __ATOMIC_RELAXED);
        return result;
    }

    // Drops this ownership (the last owner destroys the object); empty after.
    void reset() noexcept {
        detail::SharedHeader* const previous = header_;
        object_ = nullptr;
        header_ = nullptr;
        if (previous != nullptr) detail::drop_owner(previous);
    }

    // The same object (by address), as std::shared_ptr compares.
    [[nodiscard]] bool operator==(const Shared& other) const noexcept { return object_ == other.object_; }
    [[nodiscard]] bool operator!=(const Shared& other) const noexcept { return object_ != other.object_; }

private:
    template <typename Other>
    friend class Shared;
    template <typename Other>
    friend class Weak;
    template <typename Other>
    friend class SharedFromThis;
    template <typename To, typename From>
    friend Shared<To> static_shared_cast(const Shared<From>& from) noexcept;
    template <typename To, typename From>
    friend Shared<To> dynamic_shared_cast(const Shared<From>& from) noexcept;

    // Adopts one owner count already added for `header`.
    Shared(Value* const object, detail::SharedHeader* const header) noexcept : object_(object), header_(header) {}

    Value* object_{nullptr};
    detail::SharedHeader* header_{nullptr};
};

// Another owner of from's object, as a To (static_cast of the pointer).
// Requirement, as for static_cast: the object is a To.
template <typename To, typename From>
[[nodiscard]] Shared<To> static_shared_cast(const Shared<From>& from) noexcept {
    if (from.header_ == nullptr) return Shared<To>();
    To* const object = static_cast<To*>(from.object_);
    detail::add_owner(from.header_);
    Shared<To> result(object, from.header_);
    return result;
}

// Another owner of from's object as a To when dynamic_cast says it is one;
// empty otherwise (RTTI, which the constrained mission keeps).
template <typename To, typename From>
[[nodiscard]] Shared<To> dynamic_shared_cast(const Shared<From>& from) noexcept {
    if (from.header_ == nullptr) return Shared<To>();
    To* const object = dynamic_cast<To*>(from.object_);
    if (object == nullptr) return Shared<To>();
    detail::add_owner(from.header_);
    Shared<To> result(object, from.header_);
    return result;
}

template <typename Value>
class Weak final {
public:
    // Observes nothing.
    constexpr Weak() noexcept = default;

    // Observes `owner`'s object without owning it.
    template <typename Other>
        requires(detail::PointerConverts<Other, Value>::value)
    Weak(const Shared<Other>& owner) noexcept : object_(owner.object_), header_(owner.header_) {
        if (header_ != nullptr) detail::add_observer(header_);
    }

    Weak(const Weak& other) noexcept : object_(other.object_), header_(other.header_) {
        if (header_ != nullptr) detail::add_observer(header_);
    }
    Weak(Weak&& other) noexcept : object_(other.object_), header_(other.header_) {
        other.object_ = nullptr;
        other.header_ = nullptr;
    }
    Weak& operator=(const Weak& other) noexcept {
        if (other.header_ != nullptr) detail::add_observer(other.header_);
        detail::SharedHeader* const previous = header_;
        object_ = other.object_;
        header_ = other.header_;
        if (previous != nullptr) detail::drop_observer(previous);
        return *this;
    }
    Weak& operator=(Weak&& other) noexcept {
        if (this == &other) return *this;
        detail::SharedHeader* const previous = header_;
        object_ = other.object_;
        header_ = other.header_;
        other.object_ = nullptr;
        other.header_ = nullptr;
        if (previous != nullptr) detail::drop_observer(previous);
        return *this;
    }
    ~Weak() { reset(); }

    // An owner of the object while it is alive; empty once it is destroyed
    // (or being destroyed), or when this observes nothing. Never revives.
    [[nodiscard]] Shared<Value> lock() const noexcept {
        if (header_ == nullptr) return Shared<Value>();
        const bool alive = detail::add_owner_if_alive(header_);
        if (!alive) return Shared<Value>();
        Shared<Value> result(object_, header_);
        return result;
    }

    // True when the object is destroyed or this observes nothing. A false
    // answer can be stale at once on another thread; lock() decides.
    [[nodiscard]] bool expired() const noexcept {
        if (header_ == nullptr) return true;
        const u64 owners = __atomic_load_n(&(*header_).owners, __ATOMIC_ACQUIRE);
        const bool result = owners == 0;
        return result;
    }

    // Stops observing.
    void reset() noexcept {
        detail::SharedHeader* const previous = header_;
        object_ = nullptr;
        header_ = nullptr;
        if (previous != nullptr) detail::drop_observer(previous);
    }

private:
    template <typename Other>
    friend class Weak;
    template <typename Other>
    friend class SharedFromThis;

    // Adopts one observer count already added for `header`.
    Weak(Value* const object, detail::SharedHeader* const header) noexcept : object_(object), header_(header) {}

    Value* object_{nullptr};
    detail::SharedHeader* header_{nullptr};
};

// The explicit opt-in (house style, section 7) for an object that needs an
// owner of itself: `class Control : public SharedFromThis<Control>`.
// shared_from_this() is empty when the object was not made by Shared::make
// or its last owner is already destroying it.
template <typename Value>
class SharedFromThis : public detail::SharedSelf {
public:
    [[nodiscard]] Shared<Value> shared_from_this() noexcept {
        if (header_ == nullptr) return Shared<Value>();
        const bool alive = detail::add_owner_if_alive(header_);
        if (!alive) return Shared<Value>();
        Value* const object = static_cast<Value*>(this);
        Shared<Value> result(object, header_);
        return result;
    }
    [[nodiscard]] Shared<const Value> shared_from_this() const noexcept {
        if (header_ == nullptr) return Shared<const Value>();
        const bool alive = detail::add_owner_if_alive(header_);
        if (!alive) return Shared<const Value>();
        const Value* const object = static_cast<const Value*>(this);
        Shared<const Value> result(object, header_);
        return result;
    }

    // An observer of this object; empty when it was not made by Shared::make.
    [[nodiscard]] Weak<Value> weak_from_this() noexcept {
        if (header_ == nullptr) return Weak<Value>();
        detail::add_observer(header_);
        Value* const object = static_cast<Value*>(this);
        Weak<Value> result(object, header_);
        return result;
    }

protected:
    SharedFromThis() noexcept = default;
    SharedFromThis(const SharedFromThis&) noexcept = default;
    SharedFromThis& operator=(const SharedFromThis&) noexcept = default;
    ~SharedFromThis() = default;
};

} // namespace stxlib
