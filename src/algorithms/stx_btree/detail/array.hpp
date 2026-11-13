#pragma once

// STXlib storage for stx_btree's optimal trees (search_weights.hpp,
// optimal_tree.hpp, detail/tree_packing.hpp, optimal_map.hpp), in place of
// the std::vector stx_auxlib used (allocator-owns-everything): a growable
// array of trivially copyable elements on memory::allocate, owned by one
// object and movable. Growth reports failure (false, nothing changed)
// where std::vector threw std::bad_alloc; the callers turn that into
// out_of_memory. Survey D8 (allocation failure policy) is open, so nothing
// here fails fast.

#include "stxlib/memory.hpp"

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace stxlib::stx_btree::detail {

template <typename Element>
class Array final {
    static_assert(std::is_trivially_copyable_v<Element>, "Array holds trivially copyable elements");

public:
    Array() noexcept = default;
    ~Array() {
        memory::release(elements);
    }
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;
    Array(Array&& other) noexcept : elements(other.elements), length(other.length), capacity(other.capacity) {
        other.elements = nullptr;
        other.length = 0;
        other.capacity = 0;
    }
    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            memory::release(elements);
            elements = other.elements;
            length = other.length;
            capacity = other.capacity;
            other.elements = nullptr;
            other.length = 0;
            other.capacity = 0;
        }
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return length;
    }
    [[nodiscard]] bool empty() const noexcept {
        return length == 0;
    }
    [[nodiscard]] Element* data() noexcept {
        return elements;
    }
    [[nodiscard]] const Element* data() const noexcept {
        return elements;
    }
    // Requirement: index < size().
    [[nodiscard]] Element& operator[](const std::size_t index) noexcept {
        return elements[index];
    }
    [[nodiscard]] const Element& operator[](const std::size_t index) const noexcept {
        return elements[index];
    }
    // Requirement: !empty().
    [[nodiscard]] Element& back() noexcept {
        return elements[length - 1];
    }
    [[nodiscard]] const Element& back() const noexcept {
        return elements[length - 1];
    }

    // Room for at least `wanted` elements.
    [[nodiscard]] bool reserve(const std::size_t wanted) noexcept {
        if (wanted <= capacity) {
            return true;
        }
        const std::size_t largest = static_cast<std::size_t>(-1) / sizeof(Element);
        if (wanted > largest) {
            return false;
        }
        std::size_t next = capacity < 16 ? 16 : capacity;
        while (next < wanted) {
            if (next > largest / 2) {
                next = wanted;
                break;
            }
            next = next * 2;
        }
        void* const block = memory::allocate(next * sizeof(Element), alignof(Element));
        if (block == nullptr) {
            return false;
        }
        if (length != 0) {
            std::memcpy(block, static_cast<const void*>(elements), length * sizeof(Element));
        }
        memory::release(elements);
        elements = static_cast<Element*>(block);
        capacity = next;
        return true;
    }

    // Exactly `wanted` copies of `value` (std::vector::assign).
    [[nodiscard]] bool assign(const std::size_t wanted, const Element value) noexcept {
        if (!reserve(wanted)) {
            return false;
        }
        std::size_t index = 0;
        for (index = 0; index < wanted; index += 1) {
            elements[index] = value;
        }
        length = wanted;
        return true;
    }

    // Exactly `wanted` elements; new ones are copies of `value`.
    [[nodiscard]] bool resize(const std::size_t wanted, const Element value) noexcept {
        if (!reserve(wanted)) {
            return false;
        }
        std::size_t index = 0;
        for (index = length; index < wanted; index += 1) {
            elements[index] = value;
        }
        length = wanted;
        return true;
    }

    [[nodiscard]] bool push_back(const Element value) noexcept {
        if (length == capacity && !reserve(length + 1)) {
            return false;
        }
        elements[length] = value;
        length += 1;
        return true;
    }

    [[nodiscard]] bool append(const Element* const source, const std::size_t source_count) noexcept {
        if (source_count == 0) {
            return true;
        }
        if (!reserve(length + source_count)) {
            return false;
        }
        std::memcpy(static_cast<void*>(elements + length), static_cast<const void*>(source), source_count * sizeof(Element));
        length += source_count;
        return true;
    }

    // Replaces the contents with a copy of `other`'s.
    [[nodiscard]] bool copy_of(const Array& other) noexcept {
        if (this == &other) {
            return true;
        }
        if (!reserve(other.length)) {
            return false;
        }
        if (other.length != 0) {
            std::memcpy(static_cast<void*>(elements), static_cast<const void*>(other.elements), other.length * sizeof(Element));
        }
        length = other.length;
        return true;
    }

    // `value` before position `at` (at <= size()).
    [[nodiscard]] bool insert(const std::size_t at, const Element value) noexcept {
        if (length == capacity && !reserve(length + 1)) {
            return false;
        }
        std::memmove(static_cast<void*>(elements + at + 1), static_cast<const void*>(elements + at), (length - at) * sizeof(Element));
        elements[at] = value;
        length += 1;
        return true;
    }

    // Removes [first, end) (first <= end <= size()).
    void erase(const std::size_t first, const std::size_t end) noexcept {
        if (end == first) {
            return;
        }
        std::memmove(static_cast<void*>(elements + first), static_cast<const void*>(elements + end), (length - end) * sizeof(Element));
        length -= end - first;
    }

    // Requirement: !empty().
    void pop_back() noexcept {
        length -= 1;
    }
    void clear() noexcept {
        length = 0;
    }

private:
    Element* elements = nullptr;
    std::size_t length = 0;
    std::size_t capacity = 0;
};

} // namespace stxlib::stx_btree::detail
