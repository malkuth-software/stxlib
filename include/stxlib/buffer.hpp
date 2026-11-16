#pragma once

// Buffer<Element>: an owned, fixed-length array on STXlib's allocator.
//
// Semantics (survey D8 option a, used provisionally until the owner answers
// D8): a zero-length buffer is the empty buffer, which allocates nothing
// and whose release does nothing; there is no reallocation; exhaustion is a
// named failure (out_of_memory) returned by the call that allocates.
//
// Elements are trivially copyable, and a new buffer's bytes are all zero
// (house style, section 1: initialise every region that will be read). On
// every phase I target that is each element's zero value for integers,
// floating-point numbers, enumerations and object pointers.
//
// Layout: a pointer to the first element, then the count. A Buffer is a
// owner: copying makes a second block (copy() reports exhaustion, the copy
// constructor and assignment fail fast on it); moving transfers the storage and leaves
// the source empty.

#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

namespace detail {
template <typename Element>
struct BufferAdoption;
} // namespace detail

template <typename Element>
class Buffer final {
    static_assert(__is_trivially_copyable(Element), "Buffer holds trivially copyable elements");

public:
    // The empty buffer.
    constexpr Buffer() noexcept = default;

    // A buffer of `count` zeroed elements. Fails with limit_exceeded when
    // count * sizeof(Element) is not representable, out_of_memory when the
    // allocator refuses. Zero yields the empty buffer.
    [[nodiscard]] static Result<Buffer> allocate_zeroed(const usize count) noexcept {
        if (count == 0) return Result<Buffer>::success(Buffer());
        if (count > usize_max / sizeof(Element)) {
            const ErrorRecord too_large = ErrorRecord::make(
                ErrorKind::limit_exceeded, "stxlib.buffer.allocate",
                ErrorDetail{.message = "the element count times the element size is not representable"});
            return Result<Buffer>::failure(too_large);
        }
        const usize bytes = count * sizeof(Element);
        void* const block = memory::allocate(bytes, alignof(Element));
        if (block == nullptr) {
            const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.buffer.allocate");
            return Result<Buffer>::failure(exhausted);
        }
        __builtin_memset(block, 0, bytes);
        Buffer result;
        result.elements_ = static_cast<Element*>(block);
        result.count_ = count;
        return Result<Buffer>::success(static_cast<Buffer&&>(result));
    }

    // A second buffer with the same elements. Fails with out_of_memory when
    // the allocator refuses (nothing changes). The empty buffer copies to
    // the empty buffer without allocating.
    [[nodiscard]] Result<Buffer> copy() const noexcept {
        if (count_ == 0) return Result<Buffer>::success(Buffer());
        const usize bytes = count_ * sizeof(Element);
        void* const block = memory::allocate(bytes, alignof(Element));
        if (block == nullptr) {
            const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.buffer.copy");
            return Result<Buffer>::failure(exhausted);
        }
        __builtin_memcpy(block, elements_, bytes);
        Buffer result;
        result.elements_ = static_cast<Element*>(block);
        result.count_ = count_;
        return Result<Buffer>::success(static_cast<Buffer&&>(result));
    }

    // The ordinary copy (owner 2026-11-16, notes decisions/2026-11-16-
    // vocabulary-copy.md): an allocation it cannot have ends the process
    // through fail_fast with copy()'s out_of_memory record.
    Buffer(const Buffer& other) noexcept : Buffer(take_or_fail_fast(other.copy())) {}

    // The copy is made whole first, then moved in (the move releases the
    // old storage only after taking the new); self-assignment changes
    // nothing.
    Buffer& operator=(const Buffer& other) noexcept {
        if (this == &other) return *this;
        Buffer made = take_or_fail_fast(other.copy());
        *this = static_cast<Buffer&&>(made);
        return *this;
    }

    Buffer(Buffer&& other) noexcept : elements_(other.elements_), count_(other.count_) {
        other.elements_ = nullptr;
        other.count_ = 0;
    }

    // Takes other's storage, then releases this buffer's old storage; other
    // is empty. The source is taken first because it may live in the
    // storage being released (security audit 2026-11-15, the
    // A-stxlib-include-01-F1 pattern).
    Buffer& operator=(Buffer&& other) noexcept {
        if (this == &other) return *this;
        Element* const elements = other.elements_;
        const usize count = other.count_;
        other.elements_ = nullptr;
        other.count_ = 0;
        Element* const previous = elements_;
        elements_ = elements;
        count_ = count;
        if (previous != nullptr) memory::release(previous);
        return *this;
    }

    ~Buffer() { release(); }

    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    // Borrowed from this buffer: valid until it is released, moved from or
    // destroyed.
    [[nodiscard]] View<Element> view() const& noexcept {
        const View<Element> result(elements_, count_);
        return result;
    }
    View<Element> view() const&& = delete;
    [[nodiscard]] MutableView<Element> mutable_view() & noexcept {
        const MutableView<Element> result(elements_, count_);
        return result;
    }
    MutableView<Element> mutable_view() && = delete;

    // Requirement: index < count().
    [[nodiscard]] Element& operator[](const usize index) noexcept {
        STXLIB_REQUIRE(index < count_, "Buffer: index out of range");
        return elements_[index];
    }
    [[nodiscard]] const Element& operator[](const usize index) const noexcept {
        STXLIB_REQUIRE(index < count_, "Buffer: index out of range");
        return elements_[index];
    }

    // Frees the storage; the buffer is empty afterwards. Borrows of it end.
    void release() noexcept {
        if (elements_ == nullptr) return;
        // Lets go before releasing (AGENTS.md, rule 1).
        Element* const held = elements_;
        elements_ = nullptr;
        count_ = 0;
        memory::release(held);
    }

private:
    template <typename>
    friend struct detail::BufferAdoption;

    Element* elements_{nullptr};
    usize count_{0};
};

namespace detail {

// For STXlib's own implementation (src/core/growable.hpp): a Buffer that
// owns `elements`, a block from memory::allocate whose first `count`
// elements are initialised, without copying them (audit A, X5: a result
// collected in a Growable is handed over, not copied into a second block).
// The block may be larger than `count` elements; the excess is released
// with the buffer. A zero count releases the block and yields the empty
// buffer, which owns nothing.
template <typename Element>
struct BufferAdoption final {
    [[nodiscard]] static Buffer<Element> adopt(Element* const elements, const usize count) noexcept {
        Buffer<Element> result;
        if (count == 0) {
            memory::release(elements);
            return result;
        }
        result.elements_ = elements;
        result.count_ = count;
        return result;
    }
};

} // namespace detail

} // namespace stxlib
