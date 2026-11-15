// Row 10: stx_auxlib::text::append_signed with append_magnitude,
// append_padded, padding_for, raise_letters and append_fill (B1.md section
// 3.1). std::to_chars and std::from_chars are the tool's model of the
// standard library, an assumption every run records.
// P0: no undefined behaviour for every int64 value, the INT64_MIN magnitude
//     (line 234) included; fail() is unreachable.
// P1: the output is the prior text followed by max(width, length) bytes,
//     where length is the sign, the zeros and the digits.
// P2: with no field width, parse_signed of the appended text (row 11's
//     code) returns the value.
// Bound: all int64 values; base in {2, 8, 10, 16}; width and minimum digits
//        at most 8; sign, letters, alignment and fill symbolic. Unwind 66
//        (64 binary digits and the 128-byte scratch's use).
#include "verify.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

#include "stx_auxlib/process.hpp"
#include "stx_auxlib/text.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/text.hpp sha256=5356cee096afb9642d733039274c328b0baac0da13af89a77836f66a84801bc8
//@ pin source=stx_auxlib path=include/stx_auxlib/process.hpp sha256=167df96da0b91ae17c5c0f1a04069b4691a1459c753d46c107cb853d634c96d6
//@ lift source=stx_auxlib path=src/checked.hpp lines=27-31 namespace=stx_auxlib::checked sha256=9876341cc7ada133234c8923111088d3c725908dd9053937ea3f0b96aabcc56c
//@ lift source=stx_auxlib path=src/text.cpp lines=36-39 namespace=stx_auxlib::text anonymous sha256=cf905b5788524d866fc736811a08ecf2d98fff7ea03b157e6b987108abb9d4c6
//@ lift source=stx_auxlib path=src/text.cpp lines=70-121 namespace=stx_auxlib::text anonymous sha256=b56a3e313f6a0c00f95a7e44fa5f9145c674e82eab15c408645882bf42430f14
//@ lift source=stx_auxlib path=src/text.cpp lines=174-204 namespace=stx_auxlib::text anonymous sha256=4ced79454a248d954826fc06f8be1947c30008af609a3a630895000b3a3f1168
//@ lift source=stx_auxlib path=src/text.cpp lines=221-226 namespace=stx_auxlib::text anonymous sha256=16b0ecda76cf6679997f982419618ba70704c30016e18f4990830441ec71be79
//@ lift source=stx_auxlib path=src/text.cpp lines=230-237 namespace=stx_auxlib::text sha256=10783f636d0dbf467326b3599cf0b72afdce9d613f213ee2d3f10b170aca248e
//@ lift source=stx_auxlib path=src/text.cpp lines=271-274 namespace=stx_auxlib::text sha256=a6c8d12a2c1625aa9870e9ff3bbadad472b945b8b5c6dd10f0c076ba8f3c331b
//@ mutant id=m1 source=stx_auxlib path=src/text.cpp line=234 from=")) + 1" to="))"

//@ model-begin reason="stx_auxlib::fail (src/hosted/process.cpp) reports and aborts; in this row it must be unreachable"
namespace stx_auxlib {
[[noreturn]] void fail(const std::string_view message, const std::source_location location) noexcept {
    (void)message;
    (void)location;
    STX_ASSERT(false, "P0: fail() is unreachable in append_signed");
    std::abort();
}
} // namespace stx_auxlib
//@ model-end

namespace text = stx_auxlib::text;

namespace {

std::size_t spec_digit_count(std::uint64_t magnitude, const std::uint64_t base) {
    std::size_t count = 1;
    while (magnitude >= base) {
        magnitude /= base;
        ++count;
    }
    return count;
}

} // namespace

int main() {
    const std::int64_t value = static_cast<std::int64_t>(nondet_longlong());
    text::IntegerFormat format{};
    const std::uint8_t base_choice = nondet_uchar();
    STX_ASSUME(base_choice < 4);
    const text::Base bases[4] = {text::Base::binary, text::Base::octal, text::Base::decimal, text::Base::hexadecimal};
    format.base = bases[base_choice];
    format.minimum_digits = nondet_size();
    STX_ASSUME(format.minimum_digits <= 8);
    format.sign = nondet_bool() ? text::Sign::always : text::Sign::negative_only;
    format.letters = nondet_bool() ? text::Letters::upper : text::Letters::lower;
    format.field.width = nondet_size();
    STX_ASSUME(format.field.width <= 8);
    format.field.fill = static_cast<char>(nondet_uchar());
    format.field.alignment = nondet_bool() ? text::Alignment::left : text::Alignment::right;

    std::string output{};
    const bool has_prior = nondet_bool();
    if (has_prior) output.push_back(static_cast<char>(nondet_uchar()));
    const std::size_t prior = output.size();

    text::append_signed(output, value, format);

    const bool negative = value < 0;
    const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
    const std::size_t digits = spec_digit_count(magnitude, static_cast<std::uint64_t>(format.base));
    const std::size_t sign = (negative || format.sign == text::Sign::always) ? 1 : 0;
    const std::size_t numeral = sign + std::max(digits, format.minimum_digits);
    const std::size_t field = std::max(numeral, format.field.width);
    STX_ASSERT(output.size() == prior + field, "P1: the field holds the padded numeral");

    if (format.field.width == 0) {
        const std::string_view appended = std::string_view(output).substr(prior);
        const text::Parsed<std::int64_t> parsed = text::parse_signed(appended, format.base);
        STX_ASSERT(parsed.value() != nullptr, "P2: the numeral parses");
        STX_ASSERT(*parsed.value() == value, "P2: the numeral parses back to the value");
    }
    STX_VACUITY_PROBE();
    return 0;
}
