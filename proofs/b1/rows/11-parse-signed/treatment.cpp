// Row 11: stx_auxlib::text::parse_signed with integer_prefix, plus_length,
// whole and failure_from (B1.md section 3.1). std::from_chars is the
// tool's model of the standard library, an assumption every run records.
// P0: no undefined behaviour.
// P1: the outcomes value, empty, invalid, out of range and trailing text
//     are exclusive: a value and a failure are never both present.
// P2: the outcome equals the reference below, written from text.hpp's and
//     text.cpp's statements (one leading '+' not followed by a sign; '-';
//     digits of the base; the whole text): empty text is empty; no digit
//     after the optional sign is invalid; a value outside int64 is out of
//     range; bytes after the digits are trailing text; otherwise the value.
// Bound: length at most N (N in {4, 8, 12}) over digits, '+', '-', 'a'-'f'
//        and space; base 10 or 16. Out of range is unreachable in this bound
//        (12 digits fit int64 in both bases) and is reported so.
#include "symbolic_bytes.hpp"
#include "verify.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include "stx_auxlib/text.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/text.hpp sha256=5356cee096afb9642d733039274c328b0baac0da13af89a77836f66a84801bc8
//@ lift source=stx_auxlib path=src/text.cpp lines=174-204 namespace=stx_auxlib::text anonymous sha256=4ced79454a248d954826fc06f8be1947c30008af609a3a630895000b3a3f1168
//@ lift source=stx_auxlib path=src/text.cpp lines=221-226 namespace=stx_auxlib::text anonymous sha256=16b0ecda76cf6679997f982419618ba70704c30016e18f4990830441ec71be79
//@ lift source=stx_auxlib path=src/text.cpp lines=271-274 namespace=stx_auxlib::text sha256=a6c8d12a2c1625aa9870e9ff3bbadad472b945b8b5c6dd10f0c076ba8f3c331b
//@ mutant id=m1 source=stx_auxlib path=src/text.cpp line=183 from="text[1] != '-' && " to=""

namespace text = stx_auxlib::text;

namespace {

constexpr unsigned char alphabet[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '-',
                                      'a', 'b', 'c', 'd', 'e', 'f', ' '};

int spec_digit(const unsigned char byte, const int base) {
    int digit = 99;
    if (byte >= '0' && byte <= '9') digit = byte - '0';
    if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
    if (digit >= base) return -1;
    return digit;
}

struct SpecOutcome final {
    bool has_value{false};
    std::int64_t value{0};
    text::ParseFailure failure{text::ParseFailure::empty};
};

SpecOutcome spec_parse(const unsigned char* bytes, const std::size_t length, const int base) {
    SpecOutcome outcome{};
    if (length == 0) return outcome;
    std::size_t index = 0;
    bool negative = false;
    if (length >= 2 && bytes[0] == '+' && bytes[1] != '+' && bytes[1] != '-') {
        index = 1;
    } else if (bytes[0] == '-') {
        negative = true;
        index = 1;
    }
    const std::size_t digits_start = index;
    __int128 magnitude = 0;
    bool overflow = false;
    while (index < length && spec_digit(bytes[index], base) >= 0) {
        magnitude = magnitude * base + spec_digit(bytes[index], base);
        if (magnitude > (static_cast<__int128>(1) << 63)) overflow = true;
        ++index;
    }
    if (index == digits_start) {
        outcome.failure = text::ParseFailure::invalid;
        return outcome;
    }
    const __int128 signed_value = negative ? -magnitude : magnitude;
    if (overflow || signed_value > INT64_MAX || signed_value < INT64_MIN) {
        outcome.failure = text::ParseFailure::out_of_range;
        return outcome;
    }
    if (index != length) {
        outcome.failure = text::ParseFailure::trailing_text;
        return outcome;
    }
    outcome.has_value = true;
    outcome.value = static_cast<std::int64_t>(signed_value);
    return outcome;
}

} // namespace

int main() {
    SymbolicBytes input(STX_N);
    input.restrict_to(std::span<const unsigned char>(alphabet, sizeof(alphabet)));
    const bool hexadecimal = nondet_bool();
    const text::Base base = hexadecimal ? text::Base::hexadecimal : text::Base::decimal;
    const text::Parsed<std::int64_t> parsed = text::parse_signed(input.view(), base);
    STX_ASSERT((parsed.value() == nullptr) != (parsed.failure() == nullptr), "P1: a value or a failure, never both");
    STX_ASSERT(parsed.succeeded() == (parsed.value() != nullptr), "P1: succeeded exactly with a value");
    const SpecOutcome expected = spec_parse(input.bytes(), input.size(), hexadecimal ? 16 : 10);
    if (expected.has_value) {
        STX_ASSERT(parsed.value() != nullptr && *parsed.value() == expected.value, "P2: the value is the reference's");
    } else {
        STX_ASSERT(parsed.failure() != nullptr && *parsed.failure() == expected.failure, "P2: the failure is the reference's");
    }
    STX_VACUITY_PROBE();
    return 0;
}
