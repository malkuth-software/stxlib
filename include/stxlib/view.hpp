#pragma once

// Borrowed contiguous views: View<Element> reads, MutableView<Element>
// reads and writes. A view owns nothing. It is valid while the storage it
// was made from is alive and not reallocated, replaced or released (house
// style, section 1); every function that takes or returns a view states
// that lifetime.
//
// Layout (defined here, the same on every host): a pointer to the first
// element, then the element count. An empty view may hold a null pointer.
// Indexing checks its bound and fails as a requirement (require.hpp);
// kernels that have established their extent take data() and count()
// once, outside the loop (house style, section 8).

#include "stxlib/require.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

template <typename Element>
class View final {
public:
    constexpr View() noexcept = default;

    // Borrows `element_count` elements starting at `elements`. Requirement:
    // `elements` is non-null unless `element_count` is zero.
    constexpr View(const Element* const elements, const usize element_count) noexcept
        : elements_(elements), count_(element_count) {
        STXLIB_REQUIRE(elements != nullptr || element_count == 0, "View: a null pointer with a non-zero count");
    }

    // Borrows a whole array.
    template <usize Count>
    constexpr explicit View(const Element (&array)[Count]) noexcept : elements_(array), count_(Count) {}

    [[nodiscard]] constexpr usize count() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr const Element* data() const noexcept { return elements_; }

    // Requirement: index < count().
    [[nodiscard]] constexpr const Element& operator[](const usize index) const noexcept {
        STXLIB_REQUIRE(index < count_, "View: index out of range");
        return elements_[index];
    }

    // The first `length` elements. Requirement: length <= count().
    [[nodiscard]] constexpr View first(const usize length) const noexcept {
        STXLIB_REQUIRE(length <= count_, "View::first: length out of range");
        const View result(elements_, length);
        return result;
    }

    // The elements from `offset` to the end. Requirement: offset <= count().
    [[nodiscard]] constexpr View from(const usize offset) const noexcept {
        STXLIB_REQUIRE(offset <= count_, "View::from: offset out of range");
        if (offset == count_) return View();
        const View result(elements_ + offset, count_ - offset);
        return result;
    }

private:
    const Element* elements_{nullptr};
    usize count_{0};
};

template <typename Element>
class MutableView final {
public:
    constexpr MutableView() noexcept = default;

    // Borrows `element_count` elements starting at `elements` for reading and
    // writing. Requirement: `elements` is non-null unless `element_count` is
    // zero.
    constexpr MutableView(Element* const elements, const usize element_count) noexcept
        : elements_(elements), count_(element_count) {
        STXLIB_REQUIRE(elements != nullptr || element_count == 0, "MutableView: a null pointer with a non-zero count");
    }

    template <usize Count>
    constexpr explicit MutableView(Element (&array)[Count]) noexcept : elements_(array), count_(Count) {}

    [[nodiscard]] constexpr usize count() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr Element* data() const noexcept { return elements_; }

    // Requirement: index < count().
    [[nodiscard]] constexpr Element& operator[](const usize index) const noexcept {
        STXLIB_REQUIRE(index < count_, "MutableView: index out of range");
        return elements_[index];
    }

    // The same elements, read-only.
    [[nodiscard]] constexpr View<Element> read_only() const noexcept {
        const View<Element> result(elements_, count_);
        return result;
    }

    // Requirement: length <= count().
    [[nodiscard]] constexpr MutableView first(const usize length) const noexcept {
        STXLIB_REQUIRE(length <= count_, "MutableView::first: length out of range");
        const MutableView result(elements_, length);
        return result;
    }

    // Requirement: offset <= count().
    [[nodiscard]] constexpr MutableView from(const usize offset) const noexcept {
        STXLIB_REQUIRE(offset <= count_, "MutableView::from: offset out of range");
        if (offset == count_) return MutableView();
        const MutableView result(elements_ + offset, count_ - offset);
        return result;
    }

private:
    Element* elements_{nullptr};
    usize count_{0};
};

} // namespace stxlib
