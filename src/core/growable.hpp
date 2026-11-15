#pragma once

// A growable array of trivially copyable elements for STXlib's own
// implementation code (the platform layer's line, name and UTF-16
// buffers), over memory::allocate. Growth reports failure instead of
// aborting (survey D8's recommendation a, provisional), which a std::string
// in a build without exceptions cannot. Private to one call or one object.

#include "stxlib/buffer.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/types.hpp"

#include <cstring>

namespace stxlib {

template <typename Element>
class Growable final {
    static_assert(__is_trivially_copyable(Element), "Growable holds trivially copyable elements");

public:
    Growable() noexcept = default;
    // Lets go before releasing (AGENTS.md, rule 1): an early exit from the
    // release leaves nothing that would release the block again.
    ~Growable() {
        Element* const held = elements_;
        elements_ = nullptr;
        memory::release(held);
    }
    Growable(const Growable&) = delete;
    Growable& operator=(const Growable&) = delete;
    Growable(Growable&&) = delete;
    Growable& operator=(Growable&&) = delete;

    [[nodiscard]] Element* data() noexcept { return elements_; }
    [[nodiscard]] const Element* data() const noexcept { return elements_; }
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Requirement: index < the reserved room. Unchecked, unlike View's and
    // Buffer's (audit A, C5): this is internal storage, and some callers
    // read elements a host call wrote through data() into reserved room
    // beyond count() (console_windows.cpp's read_line).
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
        const bool result = move_to(next);
        return result;
    }

    // Room for exactly `wanted` elements when it must grow, for a count known
    // in advance (read_file's reported size); false when it cannot be had.
    [[nodiscard]] bool reserve_exact(const usize wanted) noexcept {
        if (wanted <= capacity_) return true;
        if (wanted > usize_max / sizeof(Element)) return false;
        const bool result = move_to(wanted);
        return result;
    }

    // Hands the elements to a Buffer without copying them; this Growable is
    // empty afterwards. The buffer's block is this Growable's: at most twice
    // its count after growth by doubling, exactly its count after
    // reserve_exact (audit A, X5).
    [[nodiscard]] Buffer<Element> take_as_buffer() noexcept {
        Element* const elements = elements_;
        const usize count = count_;
        elements_ = nullptr;
        count_ = 0;
        capacity_ = 0;
        Buffer<Element> result = detail::BufferAdoption<Element>::adopt(elements, count);
        return result;
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

    // Exactly `wanted` elements; new ones are zero bytes (house style,
    // section 1: initialise every region that will be read). False, with
    // nothing changed, when the room cannot be had.
    [[nodiscard]] bool resize(const usize wanted) noexcept {
        if (wanted <= count_) {
            count_ = wanted;
            return true;
        }
        if (!reserve(wanted)) return false;
        std::memset(elements_ + count_, 0, (wanted - count_) * sizeof(Element));
        count_ = wanted;
        return true;
    }

    // Requirement: smaller <= count() (unchecked, internal; audit A, C5).
    void shrink(const usize smaller) noexcept { count_ = smaller; }
    void clear() noexcept { count_ = 0; }

private:
    // Moves the elements into a block of `next` elements; false when it
    // cannot be had, with nothing changed. Requirement: count_ <= next, and
    // next * sizeof(Element) representable.
    [[nodiscard]] bool move_to(const usize next) noexcept {
        void* const block = memory::allocate(next * sizeof(Element), alignof(Element));
        if (block == nullptr) return false;
        if (count_ != 0) std::memcpy(block, elements_, count_ * sizeof(Element));
        Element* const previous = elements_;
        elements_ = static_cast<Element*>(block);
        capacity_ = next;
        memory::release(previous);
        return true;
    }

    Element* elements_{nullptr};
    usize count_{0};
    usize capacity_{0};
};

} // namespace stxlib
