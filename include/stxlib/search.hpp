#pragma once

// Text and byte search without allocation (roadmap M2, R3; audit B: the
// standard searchers build their tables on the global heap and throw
// bad_alloc, and consumers search with string_view::find).
//
// The algorithm is Crochemore and Perrin's two-way string matching
// ("Two-way string-matching", Journal of the ACM 38(3), 1991), in the form
// Charras and Lecroq give it ("Handbook of Exact String Matching
// Algorithms", 2004, section "Two Way algorithm"; written from memory of
// that text, not re-read this session). It needs constant extra space and
// makes at most 2n - m comparisons of the haystack (n bytes) against the
// needle (m bytes) after a preprocessing of the needle that is linear in m,
// so a search is linear in the worst case, whatever the needle.
//
// UTF-8 is self-synchronising (Unicode 16.0, section 2.5): a well-formed
// needle begins with a lead byte and ends with a complete sequence, so a
// byte match inside well-formed text begins and ends on code point
// boundaries. The text forms therefore return boundaries, and the pieces of
// a split are well-formed text.
//
// The ASCII case-insensitive forms compare the two texts after mapping
// 'A'..'Z' to 'a'..'z' (ascii.hpp); every other byte is compared exactly,
// so a byte at or above 0x80 never matches across case and the boundary
// argument above still holds. Unicode case folding is C7, not this.
//
// What the tests establish (tests/ascii_search_tests.cpp, proofs/REPORT.md):
// every function here equals a naive search over every haystack and needle
// of bounded length on small alphabets, and the comparison count stays
// within 2n + 5m over that domain.

#include "stxlib/optional.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// The position returned when nothing matches.
inline constexpr usize not_found = usize_max;

namespace detail {

// A probe the tests count comparisons with; the library's own searches use
// the empty one.
struct NoSearchProbe final {
    static constexpr void compared() noexcept {}
};

// Reads the byte at a logical index: from the front, or from the back for
// the last-match searches (the two-way algorithm on the reversed strings),
// optionally with ASCII letters lowered.
template <typename Element, bool Fold, bool Reverse>
struct SearchReader final {
    const Element* base{nullptr};
    i64 count{0};

    [[nodiscard]] constexpr u8 at(const i64 index) const noexcept {
        i64 position = index;
        if constexpr (Reverse) position = count - 1 - index;
        u8 byte = static_cast<u8>(base[position]);
        if constexpr (Fold) {
            if (byte >= 'A' && byte <= 'Z') byte = static_cast<u8>(byte + 0x20);
        }
        return byte;
    }
};

// The start of the maximal suffix of x[0, m) under the byte order, or under
// the reversed order when `reversed_order`, and its period. The start is -1
// when the whole string is that suffix.
template <typename Probe, typename Reader>
[[nodiscard]] constexpr i64 maximal_suffix(const Reader& x, const i64 m, const bool reversed_order,
                                           i64& period) noexcept {
    i64 start = -1;
    i64 j = 0;
    i64 k = 1;
    i64 p = 1;
    while (j + k < m) {
        const u8 a = x.at(j + k);
        const u8 b = x.at(start + k);
        Probe::compared();
        const bool before = reversed_order ? a > b : a < b;
        if (before) {
            j = j + k;
            k = 1;
            p = j - start;
        } else if (a == b) {
            if (k != p) {
                k = k + 1;
            } else {
                j = j + p;
                k = 1;
            }
        } else {
            start = j;
            j = start + 1;
            k = 1;
            p = 1;
        }
    }
    period = p;
    return start;
}

// The first position of x[0, m) in y[0, n), or -1.
template <typename Probe, typename Reader>
[[nodiscard]] constexpr i64 two_way(const Reader& x, const i64 m, const Reader& y, const i64 n) noexcept {
    if (m == 0) return 0;
    if (n < m) return -1;
    i64 p = 1;
    i64 q = 1;
    const i64 forward = maximal_suffix<Probe>(x, m, false, p);
    const i64 backward = maximal_suffix<Probe>(x, m, true, q);
    // The critical factorisation x = x[0, ell] x[ell + 1, m).
    i64 ell = backward;
    i64 period = q;
    if (forward > backward) {
        ell = forward;
        period = p;
    }
    // period is the period of x[ell + 1, m), so period + ell < m.
    bool periodic = true;
    for (i64 index = 0; index <= ell; ++index) {
        Probe::compared();
        if (x.at(index) != x.at(period + index)) {
            periodic = false;
            break;
        }
    }
    if (periodic) {
        // x has period `period`: after a match, the prefix x[0, m - period)
        // is known to match at the next candidate (`memory`).
        i64 j = 0;
        i64 memory = -1;
        while (j <= n - m) {
            i64 i = (ell > memory ? ell : memory) + 1;
            while (i < m) {
                Probe::compared();
                if (x.at(i) != y.at(i + j)) break;
                i = i + 1;
            }
            if (i >= m) {
                i = ell;
                while (i > memory) {
                    Probe::compared();
                    if (x.at(i) != y.at(i + j)) break;
                    i = i - 1;
                }
                if (i <= memory) return j;
                j = j + period;
                memory = m - period - 1;
            } else {
                j = j + (i - ell);
                memory = -1;
            }
        }
        return -1;
    }
    // Not periodic: a shift past the longer side of the factorisation.
    const i64 left = ell + 1;
    const i64 right = m - ell - 1;
    const i64 shift = (left > right ? left : right) + 1;
    i64 j = 0;
    while (j <= n - m) {
        i64 i = ell + 1;
        while (i < m) {
            Probe::compared();
            if (x.at(i) != y.at(i + j)) break;
            i = i + 1;
        }
        if (i >= m) {
            i = ell;
            while (i >= 0) {
                Probe::compared();
                if (x.at(i) != y.at(i + j)) break;
                i = i - 1;
            }
            if (i < 0) return j;
            j = j + shift;
        } else {
            j = j + (i - ell);
        }
    }
    return -1;
}

// Counts are object sizes, so they fit in i64 (an object is at most
// PTRDIFF_MAX bytes).
[[nodiscard]] constexpr i64 search_count(const usize count) noexcept { return static_cast<i64>(count); }

// The first match at or after `from`, or not_found.
template <bool Fold, typename Probe = NoSearchProbe, typename Element>
[[nodiscard]] constexpr usize search_first(const Element* const haystack, const usize haystack_count,
                                           const Element* const needle, const usize needle_count,
                                           const usize from) noexcept {
    if (from > haystack_count) return not_found;
    const SearchReader<Element, Fold, false> x{needle, search_count(needle_count)};
    const SearchReader<Element, Fold, false> y{haystack + from, search_count(haystack_count - from)};
    const i64 found = two_way<Probe>(x, x.count, y, y.count);
    if (found < 0) return not_found;
    const usize result = from + static_cast<usize>(found);
    return result;
}

// The last match, or not_found: the first match of the reversed needle in
// the reversed haystack.
template <bool Fold, typename Probe = NoSearchProbe, typename Element>
[[nodiscard]] constexpr usize search_last(const Element* const haystack, const usize haystack_count,
                                          const Element* const needle, const usize needle_count) noexcept {
    const SearchReader<Element, Fold, true> x{needle, search_count(needle_count)};
    const SearchReader<Element, Fold, true> y{haystack, search_count(haystack_count)};
    const i64 found = two_way<Probe>(x, x.count, y, y.count);
    if (found < 0) return not_found;
    const usize result = haystack_count - needle_count - static_cast<usize>(found);
    return result;
}

// Whether a[0, count) equals b[0, count), with or without folding.
template <bool Fold, typename Element>
[[nodiscard]] constexpr bool same_bytes(const Element* const a, const Element* const b, const usize count) noexcept {
    const SearchReader<Element, Fold, false> left{a, search_count(count)};
    const SearchReader<Element, Fold, false> right{b, search_count(count)};
    for (i64 index = 0; index < left.count; ++index) {
        if (left.at(index) != right.at(index)) return false;
    }
    return true;
}

} // namespace detail

// ---- text ---------------------------------------------------------------

namespace detail {

// `from`, moved forward past continuation bytes to the next code point
// boundary (or the end).
[[nodiscard]] constexpr usize next_boundary(const TextView text, const usize from) noexcept {
    usize start = from;
    while (start < text.count() && (static_cast<unsigned char>(text.data()[start]) & 0xc0u) == 0x80u) {
        start = start + 1;
    }
    return start;
}

} // namespace detail

// The first position of `needle` in `haystack` at or after `from`, or
// not_found. A `from` inside a code point counts from the next boundary.
// An empty needle matches at that boundary. A `from` beyond the text finds
// nothing. The result is a code point boundary.
[[nodiscard]] constexpr usize find(const TextView haystack, const TextView needle, const usize from = 0) noexcept {
    const usize start = detail::next_boundary(haystack, from);
    const usize result =
        detail::search_first<false>(haystack.data(), haystack.count(), needle.data(), needle.count(), start);
    return result;
}

// The last position of `needle` in `haystack`, or not_found. An empty
// needle matches at the end.
[[nodiscard]] constexpr usize find_last(const TextView haystack, const TextView needle) noexcept {
    const usize result = detail::search_last<false>(haystack.data(), haystack.count(), needle.data(), needle.count());
    return result;
}

[[nodiscard]] constexpr bool contains(const TextView haystack, const TextView needle) noexcept {
    const bool result = find(haystack, needle) != not_found;
    return result;
}

[[nodiscard]] constexpr bool starts_with(const TextView text, const TextView prefix) noexcept {
    if (prefix.count() > text.count()) return false;
    const bool result = detail::same_bytes<false>(text.data(), prefix.data(), prefix.count());
    return result;
}

[[nodiscard]] constexpr bool ends_with(const TextView text, const TextView suffix) noexcept {
    if (suffix.count() > text.count()) return false;
    const bool result =
        detail::same_bytes<false>(text.data() + (text.count() - suffix.count()), suffix.data(), suffix.count());
    return result;
}

// The same, with ASCII letters compared without case: a `from` inside a
// code point counts from the next boundary here too, so the result is
// always a boundary (security audit 2026-11-15, A-stxlib-include-01-F5).
[[nodiscard]] constexpr usize find_ignoring_ascii_case(const TextView haystack, const TextView needle,
                                                       const usize from = 0) noexcept {
    const usize start = detail::next_boundary(haystack, from);
    const usize result =
        detail::search_first<true>(haystack.data(), haystack.count(), needle.data(), needle.count(), start);
    return result;
}

[[nodiscard]] constexpr usize find_last_ignoring_ascii_case(const TextView haystack, const TextView needle) noexcept {
    const usize result = detail::search_last<true>(haystack.data(), haystack.count(), needle.data(), needle.count());
    return result;
}

[[nodiscard]] constexpr bool contains_ignoring_ascii_case(const TextView haystack, const TextView needle) noexcept {
    const bool result = find_ignoring_ascii_case(haystack, needle) != not_found;
    return result;
}

[[nodiscard]] constexpr bool starts_with_ignoring_ascii_case(const TextView text, const TextView prefix) noexcept {
    if (prefix.count() > text.count()) return false;
    const bool result = detail::same_bytes<true>(text.data(), prefix.data(), prefix.count());
    return result;
}

[[nodiscard]] constexpr bool ends_with_ignoring_ascii_case(const TextView text, const TextView suffix) noexcept {
    if (suffix.count() > text.count()) return false;
    const bool result =
        detail::same_bytes<true>(text.data() + (text.count() - suffix.count()), suffix.data(), suffix.count());
    return result;
}

// ---- bytes --------------------------------------------------------------

[[nodiscard]] constexpr usize find_bytes(const View<u8> haystack, const View<u8> needle, const usize from = 0) noexcept {
    const usize result =
        detail::search_first<false>(haystack.data(), haystack.count(), needle.data(), needle.count(), from);
    return result;
}

[[nodiscard]] constexpr usize find_last_bytes(const View<u8> haystack, const View<u8> needle) noexcept {
    const usize result = detail::search_last<false>(haystack.data(), haystack.count(), needle.data(), needle.count());
    return result;
}

[[nodiscard]] constexpr bool contains_bytes(const View<u8> haystack, const View<u8> needle) noexcept {
    const bool result = find_bytes(haystack, needle) != not_found;
    return result;
}

// ---- splitting ----------------------------------------------------------

// The pieces of a text between occurrences of a separator, front to back,
// as Python's str.split(separator) gives them: "a,,b" split at "," is "a",
// "", "b"; "" is one empty piece; "," is two. An empty separator yields the
// whole text as one piece. Borrows from the text and the separator, which
// must outlive it; each piece is well-formed text (see the head comment).
class TextPieces final {
public:
    [[nodiscard]] static TextPieces split(const TextView text, const TextView separator) noexcept {
        const TextPieces result(text, separator);
        return result;
    }

    // The next piece, or none after the last.
    [[nodiscard]] Optional<TextView> next() noexcept {
        if (finished_) return Optional<TextView>::none();
        const usize found = separator_.empty() ? not_found : find(rest_, separator_);
        if (found == not_found) {
            finished_ = true;
            return Optional<TextView>::of(rest_);
        }
        const TextView piece = detail::TextViewAccess::adopt_validated(rest_.data(), found);
        const usize after = found + separator_.count();
        rest_ = detail::TextViewAccess::adopt_validated(rest_.data() + after, rest_.count() - after);
        return Optional<TextView>::of(piece);
    }

private:
    TextPieces(const TextView text, const TextView separator) noexcept : rest_(text), separator_(separator) {}

    TextView rest_;
    TextView separator_;
    bool finished_{false};
};

} // namespace stxlib
