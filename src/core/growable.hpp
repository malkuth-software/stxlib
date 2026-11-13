#pragma once

// A growable array of trivially copyable elements for STXlib's own
// implementation code (the platform layer's line, name and UTF-16
// buffers), over memory::allocate. Growth reports failure instead of
// aborting (survey D8's recommendation a, provisional), which a std::string
// in a build without exceptions cannot. Private to one call or one object:
// its block is released in the module that allocated it.

#include "stxlib/memory.hpp"
#include "stxlib/types.hpp"

#include <cstring>

namespace stxlib {

template <typename Element>
class Growable final {
    static_assert(__is_trivially_copyable(Element), "Growable holds trivially copyable elements");

public:
    Growable() noexcept = default;
    ~Growable() { memory::release(elements_); }
    Growable(const Growable&) = delete;
    Growable& operator=(const Growable&) = delete;
    Growable(Growable&&) = delete;
    Growable& operator=(Growable&&) = delete;

    [[nodiscard]] Element* data() noexcept { return elements_; }
    [[nodiscard]] const Element* data() const noexcept { return elements_; }
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] Element& operator[](const usize index) noexcept { return elements_[index]; }
    [[nodiscard]] const Element& operator[](const usize index) const noexcept { return elements_[index]; }

    // Room for at least `wanted` elements; false when it cannot be had.
    [[nodiscard]] bool reserve(const usize wanted) noexcept {
        if (wanted <= capacity_) return true;
        if (wanted > usize_max / sizeof(Element)) return false;
        usize next = capacity_ < 16 ? 16 : capacity_;
        while (next < wanted) {
            if (next > usize_max / 2 / sizeof(Element)) {
                next = wanted;
                break;
            }
            next = next * 2;
        }
        void* const block = memory::allocate(next * sizeof(Element), alignof(Element));
        if (block == nullptr) return false;
        if (count_ != 0) std::memcpy(block, elements_, count_ * sizeof(Element));
        memory::release(elements_);
        elements_ = static_cast<Element*>(block);
        capacity_ = next;
        return true;
    }

    [[nodiscard]] bool append(const Element* const source, const usize source_count) noexcept {
        if (source_count == 0) return true;
        if (source_count > usize_max - count_) return false;
        if (!reserve(count_ + source_count)) return false;
        std::memcpy(elements_ + count_, source, source_count * sizeof(Element));
        count_ = count_ + source_count;
        return true;
    }

    [[nodiscard]] bool push(const Element element) noexcept {
        const bool result = append(&element, 1);
        return result;
    }

    // Requirement: smaller <= count().
    void shrink(const usize smaller) noexcept { count_ = smaller; }
    void clear() noexcept { count_ = 0; }

private:
    Element* elements_{nullptr};
    usize count_{0};
    usize capacity_{0};
};

} // namespace stxlib
