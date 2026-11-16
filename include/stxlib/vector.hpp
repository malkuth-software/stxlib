#pragma once

// Vector<Element>: an owned, growable, contiguous array on STXlib's
// allocator, the owned vocabulary's replacement for std::vector at a public
// surface (owner, D15 before 1.0, notes decisions/2026-11-15-roadmap-choices.md).
//
// Semantics (D8 = a, as answered): every operation that allocates returns a
// Status or a Result, and a refused allocation is the named failure
// out_of_memory (or limit_exceeded when the size is not representable) with
// the vector unchanged: same elements, same order, same capacity, and an
// argument passed by rvalue reference not moved from. Nothing throws. The
// one exception to reporting is the ordinary copy (the copy constructor and
// copy assignment, owner 2026-11-16, notes decisions/2026-11-16-
// vocabulary-copy.md: "copying is simply creating a new string and setting
// its contents to the content of the old string", "that's an allocation
// problem, not a copy problem"): a copy has no result to carry a failure,
// so an allocation it cannot have goes to the allocator's policy, which
// fails fast (take_or_fail_fast, the out_of_memory record of copy_of).
// copy() and copy_of() report exhaustion for callers that handle it.
//
// Elements: any type whose move constructor and destructor cannot throw
// (checked below). Elements are relocated by move construction followed by
// destruction. Insert and erase shift by move assignment when Element has a
// nothrow one, and otherwise by construction and destruction, so an element
// type needs no assignment operator. Copying operations (the copy
// constructor and copy assignment, copy, copy_of, push_copy, append_copies)
// additionally need a non-throwing copy constructor. The copy constructor
// and copy assignment are declared for every Element (as std::vector's
// are, so a type that holds a Vector of itself stays declarable) and check
// that requirement where they are instantiated.
//
// Layout (defined here, the same on every host): a pointer to the first
// element, the count of live elements, then the capacity, in elements. The
// empty vector holds a null pointer and allocates nothing.
//
// Borrows: view(), mutable_view(), operator[], begin() and end() borrow from
// the vector. They stay valid until an operation that may reallocate
// (reserve, push, push_copy, emplace, insert, resize, append_copies), one
// that ends the element's lifetime (erase, remove_last, truncate, clear,
// release), a move from the vector, or its destruction. Indexing never
// grows the vector.
//
// Comparison: operator== compares counts, then elements with Element's own
// operator== in order. For floating-point elements that is IEEE equality:
// a NaN element makes two vectors unequal, and +0 equals -0.
//
// Early exits (owner, 2026-11-15, notes decisions/2026-11-15-early-exit-
// integrity.md): Element's constructors, destructor, assignment and
// operator== are the consumer's code, so each call to them is a possible
// early exit. The form used at each call:
// - growth (reserve, push, push_copy, emplace, insert, append_copies,
//   resize past the capacity): form 1. New elements and copies are built in
//   the new block, and the old elements moved into it, while the vector
//   still owns its old block whole; the vector adopts the new block only
//   when it is complete, and only then destroys the moved-from old
//   elements, which are no longer its own. An early exit leaks the
//   unfinished block, never corrupts the vector;
// - appending within the capacity (push, push_copy, emplace,
//   append_copies, resize): form 1. The count rises after each
//   construction, so a slot is counted only once it is live;
// - removal (truncate, clear, release, remove_last, take_last): form 1.
//   The count falls before each destructor call, so a slot is uncounted
//   before its element ends;
// - insert before the end and erase, when Element is nothrow move
//   assignable: form 1. Elements shift by move assignment, so every counted
//   slot stays a live element at every call; in insert the moved-out
//   argument held in a local is all an early exit can lose (a leak);
// - insert before the end and erase, when Element has no nothrow move
//   assignment (none, or one that can throw): the shift by construction
//   and destruction leaves one counted slot dead between two calls, which
//   is not safe to observe. These sites rely on the noexcept boundary:
//   Element's move constructor and destructor are required noexcept, so an
//   exception or forced unwind leaving one ends the process
//   (std::terminate). The owner accepted this on 2026-11-15 (notes
//   decisions/2026-11-15-thread-local-exit-safety.md, "Vector's in-place
//   moves"). It fits rule 1 as reworded, "termination frees nothing
//   twice": termination releases none of our state, the operating system
//   reclaims it, so the dead slot is never destroyed or freed. A longjmp
//   across these frames is undefined behaviour in C++ and is not defended;
// - operator== and the queries: no change of state, form 1;
// - the allocation path (detail::vocabulary_allocate and _release, then
//   memory::allocate) is STXlib's and never unwinds; exhaustion is the
//   out_of_memory result.

#include "stxlib/detail/construct.hpp"
#include "stxlib/detail/growth.hpp"
#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/error.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

template <typename Element>
class Vector final {
    // Element may be incomplete where Vector<Element> is named (a tree node
    // holding its children: struct Node { Vector<Node> children; }), so
    // nothing at class scope asks about Element: the requirements on it are
    // checked in the destructor, which every vector object reaches, and the
    // assignability question is a function, evaluated where it is used.
    static constexpr void check_element() noexcept {
        static_assert(__is_nothrow_constructible(Element, Element&&),
                      "Vector's Element must be nothrow move constructible (relocation has no failure path)");
        static_assert(detail::nothrow_destructible<Element>, "Vector's Element must be nothrow destructible");
    }

    static constexpr void check_copyable() noexcept {
        static_assert(__is_nothrow_constructible(Element, const Element&),
                      "copying a Vector needs Element's nothrow copy constructor");
    }

    // Whether insert and erase can shift by move assignment (form 1, above).
    [[nodiscard]] static constexpr bool assignable() noexcept {
        const bool result = __is_nothrow_assignable(Element&, Element&&);
        return result;
    }

public:
    // The empty vector: no storage.
    constexpr Vector() noexcept = default;

    // An empty vector with room for `room` elements. Fails with
    // limit_exceeded when the byte size is not representable, out_of_memory
    // when the allocator refuses. Zero yields the empty vector.
    [[nodiscard]] static Result<Vector> with_capacity(const usize room) noexcept {
        Vector result;
        const Status reserved = result.reserve(room);
        if (reserved.failed()) return Result<Vector>::failure(*reserved.error());
        return Result<Vector>::success(static_cast<Vector&&>(result));
    }

    // A vector holding copies of `elements`, with capacity exactly their
    // count (an exact allocation, not the growth policy; security audit
    // 2026-11-15, A-stxlib-include-01-F4). `elements` may borrow from any
    // storage, another vector included. Fails with limit_exceeded when the
    // byte size is not representable, out_of_memory when the allocator
    // refuses.
    [[nodiscard]] static Result<Vector> copy_of(const View<Element> elements) noexcept {
        const usize count = elements.count();
        Vector result;
        if (count == 0) return Result<Vector>::success(static_cast<Vector&&>(result));
        if (count > usize_max / sizeof(Element)) {
            return Result<Vector>::failure(not_representable("stxlib.vector.copy_of"));
        }
        Element* const block = allocate_block(count);
        if (block == nullptr) return Result<Vector>::failure(exhausted("stxlib.vector.copy_of"));
        result.elements_ = block;
        result.capacity_ = count;
        result.construct_copies(result.elements_, elements);
        result.count_ = count;
        return Result<Vector>::success(static_cast<Vector&&>(result));
    }

    // A named copy of this vector (capacity exactly its count).
    [[nodiscard]] Result<Vector> copy() const noexcept {
        Result<Vector> result = copy_of(view());
        return result;
    }

    // The ordinary copy: copies of other's elements, capacity exactly their
    // count. An allocation the copy cannot have ends the process through
    // fail_fast with copy_of's out_of_memory or limit_exceeded record (see
    // "Semantics"); copy() reports it instead.
    Vector(const Vector& other) noexcept : Vector(take_or_fail_fast(other.copy())) { check_copyable(); }

    // Copy assignment: the copy is built whole first (it may fail fast,
    // leaving this vector as it was), then taken by the move assignment,
    // which releases the old elements only after this vector holds the new
    // ones (form 1). Safe when other is this vector, or lives inside one of
    // its elements: other is read completely before anything is released.
    Vector& operator=(const Vector& other) noexcept {
        check_copyable();
        if (this == &other) return *this;
        Vector made = take_or_fail_fast(other.copy());
        *this = static_cast<Vector&&>(made);
        return *this;
    }

    // Takes other's storage; other is the empty vector.
    Vector(Vector&& other) noexcept : elements_(other.elements_), count_(other.count_), capacity_(other.capacity_) {
        other.elements_ = nullptr;
        other.count_ = 0;
        other.capacity_ = 0;
    }

    // Takes other's storage, then destroys this vector's old elements and
    // frees its old storage; other is the empty vector. Self-assignment
    // changes nothing. The source is taken first because it may live inside
    // an element being destroyed (`node.children =
    // move(node.children[0].children)`; security audit 2026-11-15,
    // A-stxlib-include-01-F2). Both vectors hold their new storage before
    // any old element's destructor runs (form 1).
    Vector& operator=(Vector&& other) noexcept {
        if (this == &other) return *this;
        Element* const elements = other.elements_;
        const usize count = other.count_;
        const usize capacity = other.capacity_;
        other.elements_ = nullptr;
        other.count_ = 0;
        other.capacity_ = 0;
        Vector previous;
        previous.elements_ = elements_;
        previous.count_ = count_;
        previous.capacity_ = capacity_;
        elements_ = elements;
        count_ = count;
        capacity_ = capacity;
        previous.release();
        return *this;
    }

    ~Vector() {
        check_element();
        release();
    }

    // ---- queries ----------------------------------------------------------

    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }

    // The live elements, borrowed (see "Borrows" above).
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
    [[nodiscard]] Element& operator[](const usize index) & noexcept {
        STXLIB_REQUIRE(index < count_, "Vector: index out of range");
        return elements_[index];
    }
    [[nodiscard]] const Element& operator[](const usize index) const& noexcept {
        STXLIB_REQUIRE(index < count_, "Vector: index out of range");
        return elements_[index];
    }
    Element& operator[](usize index) && = delete;

    // Requirement: not empty().
    [[nodiscard]] Element& first_element() & noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::first_element: the vector is empty");
        return elements_[0];
    }
    [[nodiscard]] const Element& first_element() const& noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::first_element: the vector is empty");
        return elements_[0];
    }
    [[nodiscard]] Element& last_element() & noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::last_element: the vector is empty");
        return elements_[count_ - 1];
    }
    [[nodiscard]] const Element& last_element() const& noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::last_element: the vector is empty");
        return elements_[count_ - 1];
    }

    // For a range-based for over the live elements; borrowed like view().
    [[nodiscard]] Element* begin() & noexcept { return elements_; }
    [[nodiscard]] Element* end() & noexcept { return elements_ + count_; }
    [[nodiscard]] const Element* begin() const& noexcept { return elements_; }
    [[nodiscard]] const Element* end() const& noexcept { return elements_ + count_; }

    // ---- growth -----------------------------------------------------------

    // Room for at least `wanted` elements; the elements keep their values and
    // order. Failure (limit_exceeded, out_of_memory) changes nothing.
    [[nodiscard]] Status reserve(const usize wanted) noexcept {
        if (wanted <= capacity_) return Status::success();
        const usize next = detail::grown_capacity<usize>(capacity_, wanted, sizeof(Element));
        if (next == 0) return Status::failure(not_representable("stxlib.vector.reserve"));
        Element* const block = allocate_block(next);
        if (block == nullptr) return Status::failure(exhausted("stxlib.vector.reserve"));
        move_into(block, elements_, count_);
        adopt_block(block, next, count_);
        return Status::success();
    }

    // Appends `element`, moving from it. Failure changes nothing and leaves
    // `element` not moved from. `element` may be an element of this vector.
    [[nodiscard]] Status push(Element&& element) noexcept {
        const Status result = insert_moved(count_, element, "stxlib.vector.push");
        return result;
    }

    // Appends a copy of `element`, which may be an element of this vector.
    // Failure changes nothing.
    [[nodiscard]] Status push_copy(const Element& element) noexcept {
        static_assert(__is_nothrow_constructible(Element, const Element&),
                      "Vector::push_copy needs a nothrow copy constructor");
        if (count_ < capacity_) {
            detail::construct<Element>(elements_ + count_, element);
            count_ = count_ + 1;
            return Status::success();
        }
        Element* block = nullptr;
        usize next = 0;
        const Status grown = block_for_one_more("stxlib.vector.push_copy", block, next);
        if (grown.failed()) return grown;
        // The copy is made before the old storage goes, so `element` may
        // live in it.
        detail::construct<Element>(block + count_, element);
        move_into(block, elements_, count_);
        adopt_block(block, next, count_ + 1);
        return Status::success();
    }

    // Appends an element constructed in place from `arguments` (forwarded).
    // Failure changes nothing and constructs nothing. An argument may refer
    // into this vector (`emplace(vector[0])`): when the vector grows, the
    // element is built in the new block while the old storage is still
    // live, as push_copy does (security audit 2026-11-15,
    // A-stxlib-include-01-F3).
    template <typename... Arguments>
    [[nodiscard]] Status emplace(Arguments&&... arguments) noexcept {
        static_assert(__is_nothrow_constructible(Element, Arguments&&...),
                      "Vector::emplace needs a nothrow constructor for these arguments");
        if (count_ < capacity_) {
            detail::construct<Element>(elements_ + count_, static_cast<Arguments&&>(arguments)...);
            count_ = count_ + 1;
            return Status::success();
        }
        Element* block = nullptr;
        usize next = 0;
        const Status grown = block_for_one_more("stxlib.vector.emplace", block, next);
        if (grown.failed()) return grown;
        detail::construct<Element>(block + count_, static_cast<Arguments&&>(arguments)...);
        move_into(block, elements_, count_);
        adopt_block(block, next, count_ + 1);
        return Status::success();
    }

    // Inserts `element` before position `index` (count() appends), moving
    // from it; later elements move up one place. Failure changes nothing
    // and leaves `element` not moved from. `element` may be an element of
    // this vector. Requirement: index <= count().
    [[nodiscard]] Status insert(const usize index, Element&& element) noexcept {
        STXLIB_REQUIRE(index <= count_, "Vector::insert: index out of range");
        const Status result = insert_moved(index, element, "stxlib.vector.insert");
        return result;
    }

    // Appends copies of `elements`, which may borrow from this vector.
    // Failure changes nothing.
    [[nodiscard]] Status append_copies(const View<Element> elements) noexcept {
        static_assert(__is_nothrow_constructible(Element, const Element&),
                      "Vector::append_copies needs a nothrow copy constructor");
        const usize added = elements.count();
        if (added == 0) return Status::success();
        if (added > usize_max - count_) return Status::failure(not_representable("stxlib.vector.append_copies"));
        const usize wanted = count_ + added;
        if (wanted <= capacity_) {
            // The source is live elements or foreign storage, never the
            // spare capacity being written. The count rises per copy.
            const Element* const from = elements.data();
            for (usize index = 0; index < added; ++index) {
                detail::construct<Element>(elements_ + count_, from[index]);
                count_ = count_ + 1;
            }
            return Status::success();
        }
        const usize next = detail::grown_capacity<usize>(capacity_, wanted, sizeof(Element));
        if (next == 0) return Status::failure(not_representable("stxlib.vector.append_copies"));
        Element* const block = allocate_block(next);
        if (block == nullptr) return Status::failure(exhausted("stxlib.vector.append_copies"));
        // Copies first, while a source inside the old storage is still live.
        construct_copies(block + count_, elements);
        move_into(block, elements_, count_);
        adopt_block(block, next, wanted);
        return Status::success();
    }

    // Exactly `wanted` elements: extra ones are destroyed from the end; new
    // ones are value-initialised (Element(), zero for arithmetic types).
    // Failure changes nothing.
    [[nodiscard]] Status resize(const usize wanted) noexcept {
        static_assert(__is_nothrow_constructible(Element), "Vector::resize needs a nothrow default constructor");
        if (wanted <= count_) {
            truncate(wanted);
            return Status::success();
        }
        const Status reserved = reserve(wanted);
        if (reserved.failed()) return reserved;
        while (count_ < wanted) {
            detail::construct<Element>(elements_ + count_);
            count_ = count_ + 1;
        }
        return Status::success();
    }

    // ---- removal (never allocates, never fails) ---------------------------

    // Destroys the element at `index`; later elements move down one place.
    // Requirement: index < count().
    void erase(const usize index) noexcept {
        STXLIB_REQUIRE(index < count_, "Vector::erase: index out of range");
        erase_range(index, 1);
    }

    // Destroys elements [first, first + length); later elements move down.
    // Requirement: first <= count() and length <= count() - first.
    void erase_range(const usize first, const usize length) noexcept {
        STXLIB_REQUIRE(first <= count_ && length <= count_ - first, "Vector::erase_range: range out of range");
        if (length == 0) return;
        if constexpr (assignable()) {
            // Form 1: every counted slot stays live while the tail moves
            // down; then the moved-from tail is truncated.
            for (usize index = first; index + length < count_; ++index) {
                elements_[index] = static_cast<Element&&>(elements_[index + length]);
            }
            truncate(count_ - length);
        } else {
            // The noexcept boundary (header): slots are dead while counted.
            for (usize index = first; index < first + length; ++index) {
                detail::destroy(elements_ + index);
            }
            for (usize index = first + length; index < count_; ++index) {
                detail::construct<Element>(elements_ + index - length, static_cast<Element&&>(elements_[index]));
                detail::destroy(elements_ + index);
            }
            count_ = count_ - length;
        }
    }

    // Moves the last element out and ends its place. Requirement: not empty().
    [[nodiscard]] Element take_last() noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::take_last: the vector is empty");
        Element result(static_cast<Element&&>(elements_[count_ - 1]));
        count_ = count_ - 1;
        detail::destroy(elements_ + count_);
        return result;
    }

    // Destroys the last element. Requirement: not empty().
    void remove_last() noexcept {
        STXLIB_REQUIRE(count_ != 0, "Vector::remove_last: the vector is empty");
        count_ = count_ - 1;
        detail::destroy(elements_ + count_);
    }

    // Destroys the elements from `smaller` on, last first. Requirement:
    // smaller <= count().
    void truncate(const usize smaller) noexcept {
        STXLIB_REQUIRE(smaller <= count_, "Vector::truncate: count out of range");
        while (count_ > smaller) {
            count_ = count_ - 1;
            detail::destroy(elements_ + count_);
        }
    }

    // Destroys every element; the capacity remains.
    void clear() noexcept { truncate(0); }

    // Destroys every element and frees the storage: the empty vector.
    void release() noexcept {
        clear();
        detail::vocabulary_release(elements_);
        elements_ = nullptr;
        capacity_ = 0;
    }

    // ---- comparison -------------------------------------------------------

    [[nodiscard]] bool operator==(const Vector& other) const noexcept {
        if (count_ != other.count_) return false;
        for (usize index = 0; index < count_; ++index) {
            const bool same = elements_[index] == other.elements_[index];
            if (!same) return false;
        }
        return true;
    }
    [[nodiscard]] bool operator!=(const Vector& other) const noexcept {
        const bool equal = *this == other;
        return !equal;
    }

private:
    static ErrorRecord not_representable(const Operation operation) noexcept {
        const ErrorRecord result =
            ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                              ErrorDetail{.message = "the element count times the element size is not representable"});
        return result;
    }
    static ErrorRecord exhausted(const Operation operation) noexcept {
        const ErrorRecord result = ErrorRecord::make(ErrorKind::out_of_memory, operation);
        return result;
    }

    // Storage for `room` elements (its byte size is representable:
    // grown_capacity's result), or null when the allocator refuses.
    static Element* allocate_block(const usize room) noexcept {
        void* const block = detail::vocabulary_allocate(room * sizeof(Element), alignof(Element));
        Element* const result = static_cast<Element*>(block);
        return result;
    }

    // Move-constructs `length` live elements from `source` into
    // uninitialised `target` (a different block). The sources stay live
    // (moved from) until adopt_block ends them.
    static void move_into(Element* const target, Element* const source, const usize length) noexcept {
        if (length == 0) return;
        if constexpr (__is_trivially_copyable(Element)) {
            __builtin_memcpy(static_cast<void*>(target), static_cast<const void*>(source), length * sizeof(Element));
        } else {
            for (usize index = 0; index < length; ++index) {
                detail::construct<Element>(target + index, static_cast<Element&&>(source[index]));
            }
        }
    }

    // Copy-constructs `source` into uninitialised `target`. The two do not
    // overlap: the target is spare capacity or a new block.
    static void construct_copies(Element* const target, const View<Element> source) noexcept {
        static_assert(__is_nothrow_constructible(Element, const Element&), "copying needs a nothrow copy constructor");
        const usize length = source.count();
        const Element* const from = source.data();
        for (usize index = 0; index < length; ++index) {
            detail::construct<Element>(target + index, from[index]);
        }
    }

    // Adopts the completed `block` holding `live` elements, then ends the
    // old elements (moved from by move_into, no longer the vector's) and
    // frees the old storage.
    void adopt_block(Element* const block, const usize room, const usize live) noexcept {
        Element* const previous = elements_;
        const usize previous_count = count_;
        elements_ = block;
        count_ = live;
        capacity_ = room;
        if constexpr (!__is_trivially_copyable(Element)) {
            for (usize index = 0; index < previous_count; ++index) detail::destroy(previous + index);
        }
        detail::vocabulary_release(previous);
    }

    // A new block with room for count() + 1, for a caller that builds into
    // it before relocating. Failure leaves block null.
    Status block_for_one_more(const Operation operation, Element*& block, usize& next) noexcept {
        if (count_ == usize_max) return Status::failure(not_representable(operation));
        next = detail::grown_capacity<usize>(capacity_, count_ + 1, sizeof(Element));
        if (next == 0) return Status::failure(not_representable(operation));
        block = allocate_block(next);
        if (block == nullptr) return Status::failure(exhausted(operation));
        return Status::success();
    }

    // Inserts `element` (moved from only on success) at `index`.
    Status insert_moved(const usize index, Element& element, const Operation operation) noexcept {
        if (count_ == capacity_) {
            Element* block = nullptr;
            usize next = 0;
            const Status grown = block_for_one_more(operation, block, next);
            if (grown.failed()) return grown;
            // The new element is built first, while `element` (possibly in
            // the old storage) is still live; then the others move around it.
            detail::construct<Element>(block + index, static_cast<Element&&>(element));
            move_into(block, elements_, index);
            move_into(block + index + 1, elements_ + index, count_ - index);
            adopt_block(block, next, count_ + 1);
            return Status::success();
        }
        // Room exists, so nothing can fail now. `element` is moved out first,
        // since the shift below may move or overwrite its place.
        Element held(static_cast<Element&&>(element));
        if (index == count_) {
            detail::construct<Element>(elements_ + count_, static_cast<Element&&>(held));
            count_ = count_ + 1;
            return Status::success();
        }
        if constexpr (assignable()) {
            // Form 1: the last element is moved into the spare slot and
            // counted, then the rest shift up by assignment, every counted
            // slot live throughout.
            detail::construct<Element>(elements_ + count_, static_cast<Element&&>(elements_[count_ - 1]));
            count_ = count_ + 1;
            for (usize position = count_ - 2; position > index; --position) {
                elements_[position] = static_cast<Element&&>(elements_[position - 1]);
            }
            elements_[index] = static_cast<Element&&>(held);
        } else {
            // The noexcept boundary (header): a slot is dead while counted.
            for (usize position = count_; position > index; --position) {
                detail::construct<Element>(elements_ + position, static_cast<Element&&>(elements_[position - 1]));
                detail::destroy(elements_ + position - 1);
            }
            detail::construct<Element>(elements_ + index, static_cast<Element&&>(held));
            count_ = count_ + 1;
        }
        return Status::success();
    }

    Element* elements_{nullptr};
    usize count_{0};
    usize capacity_{0};
};

} // namespace stxlib
