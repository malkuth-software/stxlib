// Proof harness for STXlib's UTF-8 checks (include/stxlib/detail/utf8.hpp):
// exhaustive enumeration of every byte string up to a length against an
// independent reference (FORWARD_PROGRAMME section 10: exhaustive
// enumeration of a whole input domain against a correct reference is a
// proof).
//
// Properties, for every byte string s of length n and every limit L in
// 0 .. n + 1:
//   P1  utf8_well_formed(s) equals the reference's well-formedness;
//   P2  utf8_prefix(s, n, L) equals the reference's longest well-formed
//       prefix that ends at a sequence boundary at or before L.
//
// Scope: lengths 0 to 3 by default (16,843,009 strings; CTest), lengths 0
// to 4 with --scope full (4,311,810,305 strings; run by hand on the M4).
// Strings longer than the scope are not covered by this harness.
//
// The reference decodes by bit patterns and checks the decoded scalar
// (overlong forms by the minimum value of each length, surrogates, the
// U+10FFFF ceiling), the Unicode Standard's section 3.9 definition (D92)
// stated as arithmetic, where the implementation uses Table 3-7's byte
// ranges.

#include "stxlib/detail/utf8.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using stxlib::usize;

// The length of the reference's well-formed sequence at index, or 0.
usize reference_sequence(const unsigned char* const bytes, const usize count, const usize index) {
    const unsigned first = bytes[index];
    usize length = 0;
    std::uint32_t scalar = 0;
    if ((first & 0x80u) == 0x00u) {
        length = 1;
        scalar = first;
    } else if ((first & 0xe0u) == 0xc0u) {
        length = 2;
        scalar = first & 0x1fu;
    } else if ((first & 0xf0u) == 0xe0u) {
        length = 3;
        scalar = first & 0x0fu;
    } else if ((first & 0xf8u) == 0xf0u) {
        length = 4;
        scalar = first & 0x07u;
    } else {
        return 0;
    }
    if (length > count - index) return 0;
    for (usize offset = 1; offset < length; ++offset) {
        const unsigned next = bytes[index + offset];
        if ((next & 0xc0u) != 0x80u) return 0;
        scalar = (scalar << 6) | (next & 0x3fu);
    }
    const std::uint32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (scalar < minimum[length]) return 0;
    if (scalar > 0x10ffffu) return 0;
    if (scalar >= 0xd800u && scalar <= 0xdfffu) return 0;
    return length;
}

usize reference_prefix(const unsigned char* const bytes, const usize count, const usize limit) {
    usize index = 0;
    while (index < count) {
        const usize length = reference_sequence(bytes, count, index);
        if (length == 0) break;
        if (index + length > limit) break;
        index += length;
    }
    return index;
}

struct Totals final {
    std::uint64_t strings{0};
    std::uint64_t comparisons{0};
    std::uint64_t mismatches{0};
};

void check_string(const unsigned char* const bytes, const usize count, Totals& totals) {
    const char* const text = reinterpret_cast<const char*>(bytes);
    totals.strings = totals.strings + 1;
    const bool expected = reference_prefix(bytes, count, count) == count;
    const bool actual = stxlib::detail::utf8_well_formed(text, count);
    totals.comparisons = totals.comparisons + 1;
    if (expected != actual) {
        totals.mismatches = totals.mismatches + 1;
        if (totals.mismatches <= 10) std::fprintf(stderr, "P1 mismatch, length %zu\n", count);
    }
    for (usize limit = 0; limit <= count + 1; ++limit) {
        const usize expected_prefix = reference_prefix(bytes, count, limit);
        const usize actual_prefix = stxlib::detail::utf8_prefix(text, count, limit);
        totals.comparisons = totals.comparisons + 1;
        if (expected_prefix != actual_prefix) {
            totals.mismatches = totals.mismatches + 1;
            if (totals.mismatches <= 10) std::fprintf(stderr, "P2 mismatch, length %zu limit %zu\n", count, limit);
        }
    }
}

// Every string of exactly `count` bytes, as a base-256 counter.
void check_length(const usize count, Totals& totals) {
    unsigned char bytes[4]{};
    if (count == 0) {
        check_string(bytes, 0, totals);
        return;
    }
    for (;;) {
        check_string(bytes, count, totals);
        usize position = 0;
        while (position < count) {
            if (bytes[position] != 0xff) {
                bytes[position] = static_cast<unsigned char>(bytes[position] + 1);
                break;
            }
            bytes[position] = 0;
            position = position + 1;
        }
        if (position == count) return;
    }
}

} // namespace

int main(const int argc, char** const argv) {
    usize longest = 3;
    if (argc == 3 && std::strcmp(argv[1], "--scope") == 0 && std::strcmp(argv[2], "full") == 0) longest = 4;
    Totals totals{};
    for (usize count = 0; count <= longest; ++count) check_length(count, totals);
    std::printf("utf8 exhaustive: lengths 0..%zu, %llu strings, %llu comparisons, %llu mismatches\n", longest,
                static_cast<unsigned long long>(totals.strings), static_cast<unsigned long long>(totals.comparisons),
                static_cast<unsigned long long>(totals.mismatches));
    const int status = totals.mismatches == 0 ? 0 : 1;
    return status;
}
