// Tests of the ASCII classes (ascii.hpp, R2) and text search (search.hpp,
// R3).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - every class and case mapping on all 256 byte values: bytes 0x00..0x7F
//   against the host C library in the "C" locale, bytes 0x80..0xFF against
//   the stated rule (in no class, never changed); ascii_digit_value on all
//   256 against an independent rule;
// - equals_ignoring_ascii_case and compare_ignoring_ascii_case on every pair
//   of one-byte ASCII texts, and every pair of texts of up to three bytes
//   from eight letters around the case boundaries;
// - find (at every start), find_last, contains, starts_with and ends_with
//   against a naive search on: every haystack of up to 10 bytes and needle
//   of up to 6 over {a, b}; every haystack of up to 7 and needle of up to 4
//   over {a, b, c}; the ASCII case-insensitive forms over {a, A, b} (7 and
//   4); the byte forms over {0x00, 0x80, 0xFF} (7 and 4); and every text of
//   up to 4 characters over {a, é, 𝄞} with needles of up to 2, where every
//   match must also be a code point boundary;
// - the comparison count of the two-way search stays within 2n + 5m on
//   every case of the first two domains (the bound Crochemore and Perrin
//   prove is 2n - m for the search phase plus a linear preprocessing);
// - TextPieces against a model of str.split on every text of up to 7 bytes
//   over {a, ','} with four separators.

#include "stxlib/ascii.hpp"
#include "stxlib/search.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <cctype>
#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using stxlib::u8;
using stxlib::usize;

// Compile-time use: the search functions are constexpr on literals.
static_assert(stxlib::find("needle in a haystack", "hay") == 12);
static_assert(stxlib::find_last("abcabc", "bc") == 4);
static_assert(stxlib::find("abc", "") == 0 && stxlib::find_last("abc", "") == 3);
static_assert(stxlib::find("abc", "abcd") == stxlib::not_found);
static_assert(stxlib::starts_with("prefix", "pre") && stxlib::ends_with("suffix", "fix"));
static_assert(stxlib::find_ignoring_ascii_case("Hello World", "WORLD") == 6);
// A `from` inside a code point counts from the next boundary, as find's
// does, also for an empty needle (A-stxlib-include-01-F5): "\xc3\xa9" is
// one code point, so position 1 is inside it.
static_assert(stxlib::find_ignoring_ascii_case("\xc3\xa9x", "", 1) == 2);
static_assert(stxlib::find("\xc3\xa9x", "", 1) == 2);
static_assert(stxlib::find_ignoring_ascii_case("\xc3\xa9X", "x", 1) == 2);
static_assert(stxlib::ascii_lower('Q') == 'q' && stxlib::ascii_upper('q') == 'Q');

stxlib::TextView view_of(const std::string& text) {
    const stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(text.data(), text.size());
    if (validated.failed()) {
        std::fprintf(stderr, "test text is not UTF-8\n");
        return stxlib::TextView();
    }
    const stxlib::TextView result = *validated.value();
    return result;
}

// Every concatenation of up to `longest` pieces.
std::vector<std::string> all_texts(const std::vector<std::string>& pieces, const usize longest) {
    std::vector<std::string> result{std::string()};
    usize start = 0;
    for (usize length = 1; length <= longest; ++length) {
        const usize end = result.size();
        for (usize index = start; index < end; ++index) {
            for (const std::string& piece : pieces) result.push_back(result[index] + piece);
        }
        start = end;
    }
    return result;
}

// ---- ASCII ------------------------------------------------------------------

void classes_exhaustive() {
    std::setlocale(LC_ALL, "C");
    for (int value = 0; value < 256; ++value) {
        const char character = static_cast<char>(value);
        if (value < 0x80) {
            CHECK(stxlib::is_ascii(character));
            CHECK(stxlib::is_ascii_upper(character) == (std::isupper(value) != 0));
            CHECK(stxlib::is_ascii_lower(character) == (std::islower(value) != 0));
            CHECK(stxlib::is_ascii_alpha(character) == (std::isalpha(value) != 0));
            CHECK(stxlib::is_ascii_digit(character) == (std::isdigit(value) != 0));
            CHECK(stxlib::is_ascii_alnum(character) == (std::isalnum(value) != 0));
            CHECK(stxlib::is_ascii_hex_digit(character) == (std::isxdigit(value) != 0));
            CHECK(stxlib::is_ascii_space(character) == (std::isspace(value) != 0));
            CHECK(stxlib::is_ascii_blank(character) == (std::isblank(value) != 0));
            CHECK(stxlib::is_ascii_control(character) == (std::iscntrl(value) != 0));
            CHECK(stxlib::is_ascii_printable(character) == (std::isprint(value) != 0));
            CHECK(stxlib::is_ascii_graphic(character) == (std::isgraph(value) != 0));
            CHECK(stxlib::is_ascii_punctuation(character) == (std::ispunct(value) != 0));
            CHECK(static_cast<u8>(stxlib::ascii_lower(character)) == static_cast<u8>(std::tolower(value)));
            CHECK(static_cast<u8>(stxlib::ascii_upper(character)) == static_cast<u8>(std::toupper(value)));
        } else {
            const bool in_a_class =
                stxlib::is_ascii(character) || stxlib::is_ascii_upper(character) ||
                stxlib::is_ascii_lower(character) || stxlib::is_ascii_alpha(character) ||
                stxlib::is_ascii_digit(character) || stxlib::is_ascii_alnum(character) ||
                stxlib::is_ascii_hex_digit(character) || stxlib::is_ascii_space(character) ||
                stxlib::is_ascii_blank(character) || stxlib::is_ascii_control(character) ||
                stxlib::is_ascii_printable(character) || stxlib::is_ascii_graphic(character) ||
                stxlib::is_ascii_punctuation(character);
            CHECK(!in_a_class);
            CHECK(stxlib::ascii_lower(character) == character);
            CHECK(stxlib::ascii_upper(character) == character);
        }
        int expected_digit = -1;
        if (value >= '0' && value <= '9') expected_digit = value - '0';
        if (value >= 'a' && value <= 'z') expected_digit = value - 'a' + 10;
        if (value >= 'A' && value <= 'Z') expected_digit = value - 'A' + 10;
        CHECK(stxlib::ascii_digit_value(character) == expected_digit);
    }
}

int model_compare_folded(const std::string& left, const std::string& right) {
    const usize shorter = left.size() < right.size() ? left.size() : right.size();
    for (usize index = 0; index < shorter; ++index) {
        const int x = std::tolower(static_cast<u8>(left[index]));
        const int y = std::tolower(static_cast<u8>(right[index]));
        if (x != y) return x < y ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

void check_folded_pair(const std::string& left, const std::string& right) {
    const stxlib::TextView a = view_of(left);
    const stxlib::TextView b = view_of(right);
    const int expected = model_compare_folded(left, right);
    CHECK(stxlib::compare_ignoring_ascii_case(a, b) == expected);
    CHECK(stxlib::equals_ignoring_ascii_case(a, b) == (expected == 0));
}

void folded_comparison_exhaustive() {
    std::setlocale(LC_ALL, "C");
    for (int x = 0; x < 0x80; ++x) {
        for (int y = 0; y < 0x80; ++y) {
            check_folded_pair(std::string(1, static_cast<char>(x)), std::string(1, static_cast<char>(y)));
        }
    }
    const std::vector<std::string> texts = all_texts({"a", "A", "z", "Z", "@", "[", "`", "{"}, 3);
    for (const std::string& left : texts) {
        for (const std::string& right : texts) check_folded_pair(left, right);
    }
}

// ---- search -----------------------------------------------------------------

char fold_byte(const char byte, const bool fold) {
    // The C library's tolower in the "C" locale, for ASCII bytes only: a
    // model independent of ascii.hpp.
    if (!fold || static_cast<u8>(byte) >= 0x80) return byte;
    const char result = static_cast<char>(std::tolower(static_cast<u8>(byte)));
    return result;
}

bool naive_matches_at(const std::string& haystack, const std::string& needle, const usize at, const bool fold) {
    if (at + needle.size() > haystack.size()) return false;
    for (usize index = 0; index < needle.size(); ++index) {
        if (fold_byte(haystack[at + index], fold) != fold_byte(needle[index], fold)) return false;
    }
    return true;
}

usize naive_find(const std::string& haystack, const std::string& needle, const usize from, const bool fold) {
    for (usize at = from; at <= haystack.size(); ++at) {
        if (naive_matches_at(haystack, needle, at, fold)) return at;
    }
    return stxlib::not_found;
}

usize naive_find_last(const std::string& haystack, const std::string& needle, const bool fold) {
    if (needle.size() > haystack.size()) return stxlib::not_found;
    usize at = haystack.size() - needle.size() + 1;
    while (at > 0) {
        at = at - 1;
        if (naive_matches_at(haystack, needle, at, fold)) return at;
    }
    return stxlib::not_found;
}

struct CountingProbe final {
    static usize& counter() {
        static usize value = 0;
        return value;
    }
    static void compared() noexcept { counter() = counter() + 1; }
};

usize worst_excess = 0;

void check_comparisons(const std::string& haystack, const std::string& needle) {
    const usize bound = 2 * haystack.size() + 5 * needle.size();
    CountingProbe::counter() = 0;
    const usize first = stxlib::detail::search_first<false, CountingProbe>(haystack.data(), haystack.size(),
                                                                          needle.data(), needle.size(), 0);
    static_cast<void>(first);
    CHECK(CountingProbe::counter() <= bound);
    if (CountingProbe::counter() > worst_excess + 2 * haystack.size()) {
        worst_excess = CountingProbe::counter() - 2 * haystack.size();
    }
    CountingProbe::counter() = 0;
    const usize last = stxlib::detail::search_last<false, CountingProbe>(haystack.data(), haystack.size(),
                                                                        needle.data(), needle.size());
    static_cast<void>(last);
    CHECK(CountingProbe::counter() <= bound);
}

void check_text_search(const std::string& haystack, const std::string& needle, const bool fold,
                       const bool count_comparisons) {
    const stxlib::TextView h = view_of(haystack);
    const stxlib::TextView n = view_of(needle);
    for (usize from = 0; from <= haystack.size() + 1; ++from) {
        const usize expected = from > haystack.size() ? stxlib::not_found : naive_find(haystack, needle, from, fold);
        const usize found = fold ? stxlib::find_ignoring_ascii_case(h, n, from) : stxlib::find(h, n, from);
        CHECK(found == expected);
    }
    const usize expected_last = naive_find_last(haystack, needle, fold);
    const usize expected_first = naive_find(haystack, needle, 0, fold);
    const bool expected_prefix = naive_matches_at(haystack, needle, 0, fold);
    const bool expected_suffix =
        needle.size() <= haystack.size() && naive_matches_at(haystack, needle, haystack.size() - needle.size(), fold);
    if (fold) {
        CHECK(stxlib::find_last_ignoring_ascii_case(h, n) == expected_last);
        CHECK(stxlib::contains_ignoring_ascii_case(h, n) == (expected_first != stxlib::not_found));
        CHECK(stxlib::starts_with_ignoring_ascii_case(h, n) == expected_prefix);
        CHECK(stxlib::ends_with_ignoring_ascii_case(h, n) == expected_suffix);
    } else {
        CHECK(stxlib::find_last(h, n) == expected_last);
        CHECK(stxlib::contains(h, n) == (expected_first != stxlib::not_found));
        CHECK(stxlib::starts_with(h, n) == expected_prefix);
        CHECK(stxlib::ends_with(h, n) == expected_suffix);
    }
    if (count_comparisons) check_comparisons(haystack, needle);
}

void search_domain(const std::vector<std::string>& pieces, const usize haystack_length, const usize needle_length,
                   const bool fold, const bool count_comparisons) {
    const std::vector<std::string> haystacks = all_texts(pieces, haystack_length);
    const std::vector<std::string> needles = all_texts(pieces, needle_length);
    for (const std::string& haystack : haystacks) {
        for (const std::string& needle : needles) check_text_search(haystack, needle, fold, count_comparisons);
    }
}

void search_exhaustive() {
    search_domain({"a", "b"}, 10, 6, false, true);
    search_domain({"a", "b", "c"}, 7, 4, false, true);
    search_domain({"a", "A", "b"}, 7, 4, true, false);
    std::printf("two-way: at most 2n + %zu comparisons over the counted domains\n", worst_excess);
}

void byte_search_exhaustive() {
    const std::vector<std::string> haystacks = all_texts({std::string(1, '\0'), "\x80", "\xff"}, 7);
    const std::vector<std::string> needles = all_texts({std::string(1, '\0'), "\x80", "\xff"}, 4);
    for (const std::string& haystack : haystacks) {
        const stxlib::View<u8> h(reinterpret_cast<const u8*>(haystack.data()), haystack.size());
        for (const std::string& needle : needles) {
            const stxlib::View<u8> n(reinterpret_cast<const u8*>(needle.data()), needle.size());
            for (usize from = 0; from <= haystack.size() + 1; ++from) {
                const usize expected =
                    from > haystack.size() ? stxlib::not_found : naive_find(haystack, needle, from, false);
                CHECK(stxlib::find_bytes(h, n, from) == expected);
            }
            const usize expected_first = naive_find(haystack, needle, 0, false);
            CHECK(stxlib::find_last_bytes(h, n) == naive_find_last(haystack, needle, false));
            CHECK(stxlib::contains_bytes(h, n) == (expected_first != stxlib::not_found));
        }
    }
}

bool on_boundary(const std::string& text, const usize offset) {
    if (offset >= text.size()) return offset == text.size();
    const bool result = (static_cast<u8>(text[offset]) & 0xc0u) != 0x80u;
    return result;
}

void utf8_search_exhaustive() {
    const std::vector<std::string> haystacks = all_texts({"a", "\xc3\xa9", "\xf0\x9d\x84\x9e"}, 4);
    const std::vector<std::string> needles = all_texts({"a", "\xc3\xa9", "\xf0\x9d\x84\x9e"}, 2);
    for (const std::string& haystack : haystacks) {
        const stxlib::TextView h = view_of(haystack);
        for (const std::string& needle : needles) {
            const stxlib::TextView n = view_of(needle);
            for (usize from = 0; from <= haystack.size(); ++from) {
                const usize found = stxlib::find(h, n, from);
                usize boundary = from;
                while (!on_boundary(haystack, boundary)) boundary = boundary + 1;
                CHECK(found == naive_find(haystack, needle, boundary, false));
                if (found != stxlib::not_found) CHECK(on_boundary(haystack, found) && on_boundary(haystack, found + needle.size()));
                // The case-folding search counts from the same boundary
                // (A-stxlib-include-01-F5; the empty needle included).
                const usize folded = stxlib::find_ignoring_ascii_case(h, n, from);
                CHECK(folded == naive_find(haystack, needle, boundary, true));
                if (folded != stxlib::not_found) CHECK(on_boundary(haystack, folded));
            }
            const usize last = stxlib::find_last(h, n);
            CHECK(last == naive_find_last(haystack, needle, false));
            if (last != stxlib::not_found) CHECK(on_boundary(haystack, last));
        }
    }
}

void long_cases() {
    // Highly periodic text and needles: the quadratic worst case of a naive
    // search, linear here (sampled shapes, checked against std::string).
    const std::string ones(20000, 'a');
    const std::string needles[] = {std::string(50, 'a') + "b", "b" + std::string(50, 'a'), std::string(49, 'a'),
                                   std::string(25, 'a') + "b" + std::string(25, 'a')};
    for (const std::string& needle : needles) {
        CountingProbe::counter() = 0;
        const usize found = stxlib::detail::search_first<false, CountingProbe>(ones.data(), ones.size(), needle.data(),
                                                                              needle.size(), 0);
        const std::string::size_type expected = ones.find(needle);
        CHECK(found == (expected == std::string::npos ? stxlib::not_found : static_cast<usize>(expected)));
        CHECK(CountingProbe::counter() <= 2 * ones.size() + 5 * needle.size());
        const usize last = stxlib::find_last(view_of(ones), view_of(needle));
        const std::string::size_type expected_last = ones.rfind(needle);
        CHECK(last == (expected_last == std::string::npos ? stxlib::not_found : static_cast<usize>(expected_last)));
    }
}

// ---- splitting ----------------------------------------------------------------

std::vector<std::string> model_split(const std::string& text, const std::string& separator) {
    std::vector<std::string> result;
    if (separator.empty()) {
        result.push_back(text);
        return result;
    }
    usize start = 0;
    while (true) {
        const std::string::size_type found = text.find(separator, start);
        if (found == std::string::npos) {
            result.push_back(text.substr(start));
            return result;
        }
        result.push_back(text.substr(start, found - start));
        start = found + separator.size();
    }
}

void split_exhaustive() {
    const std::vector<std::string> texts = all_texts({"a", ","}, 7);
    const std::string separators[] = {",", ",,", "", "a,"};
    for (const std::string& text : texts) {
        for (const std::string& separator : separators) {
            const std::vector<std::string> expected = model_split(text, separator);
            stxlib::TextPieces pieces = stxlib::TextPieces::split(view_of(text), view_of(separator));
            usize index = 0;
            bool same = true;
            while (true) {
                const stxlib::Optional<stxlib::TextView> piece = pieces.next();
                if (piece.absent()) break;
                if (index >= expected.size() || *piece.value() != view_of(expected[index])) same = false;
                index = index + 1;
            }
            CHECK(same && index == expected.size());
            CHECK(pieces.next().absent());
        }
    }
}

} // namespace

int main() {
    classes_exhaustive();
    folded_comparison_exhaustive();
    search_exhaustive();
    byte_search_exhaustive();
    utf8_search_exhaustive();
    long_cases();
    split_exhaustive();
    return stxlib_tests::finish("ascii_search");
}
