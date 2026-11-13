// Tests of stxlib::text's numbers (include/stxlib/numeric_text.hpp).
// Ported from stx_auxlib's tests/text_tests.cpp at b6e330a with the same
// expectations, plus the caller-storage rules (whole or nothing, the size
// needed, TextWriter) that STXlib's interface adds.
//
// Exact expectations come from the C++ standard's definitions. A
// differential check compares fixed and scientific output with the host's
// snprintf, which is correctly rounded on the hosts tested here (macOS,
// glibc; the Windows UCRT is too) and runs in the "C" locale because
// nothing here calls setlocale; shortest output is checked by round trip.
// The differential and round-trip checks are sampled (200,000 doubles and
// 100,000 floats over every exponent), not exhaustive.

#include "stxlib/numeric_text.hpp"
#include "stxlib/text.hpp"

#include "core/checked.hpp"
#include "test_support.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace {

namespace text = stxlib::text;
using stxlib::i64;
using stxlib::TextView;
using stxlib::u64;
using stxlib::usize;

std::string string_of(const TextView view) {
    const std::string result(view.data(), view.count());
    return result;
}

// A view of a test string. Requirement: the string is UTF-8 and outlives
// the view.
TextView view_of(const std::string& value) {
    stxlib::Result<TextView> validated = stxlib::validate_utf8(value.data(), value.size());
    CHECK(validated.succeeded());
    if (!validated.succeeded()) return TextView();
    const TextView result = static_cast<stxlib::Result<TextView>&&>(validated).take_value();
    return result;
}

char storage[text::maximum_number_bytes];

std::string written(const text::Formatted& formatted) {
    CHECK(formatted.succeeded());
    if (!formatted.succeeded()) return std::string("<too small>");
    const std::string result = string_of(*formatted.text());
    return result;
}

std::string signed_text(const i64 value, const text::IntegerFormat& format) {
    const std::string result = written(text::format_signed(stxlib::MutableView<char>(storage), value, format));
    return result;
}

std::string unsigned_text(const u64 value, const text::IntegerFormat& format) {
    const std::string result = written(text::format_unsigned(stxlib::MutableView<char>(storage), value, format));
    return result;
}

std::string real_text(const double value, const text::RealFormat& format) {
    const std::string result = written(text::format_real(stxlib::MutableView<char>(storage), value, format));
    return result;
}

std::string number(const text::NumberText& record) {
    const std::string result = string_of(record.view());
    return result;
}

void integers() {
    CHECK(signed_text(0, {}) == "0");
    CHECK(signed_text(-42, {}) == "-42");
    CHECK(signed_text(std::numeric_limits<i64>::min(), {}) == "-9223372036854775808");
    CHECK(unsigned_text(std::numeric_limits<u64>::max(), {}) == "18446744073709551615");
    CHECK(signed_text(7, {.sign = text::Sign::always}) == "+7");
    CHECK(signed_text(0, {.sign = text::Sign::always}) == "+0");
    // %02X, %08x, %04u
    CHECK(unsigned_text(10, {.base = text::Base::hexadecimal, .minimum_digits = 2, .letters = text::Letters::upper}) ==
          "0A");
    CHECK(unsigned_text(0xbeef, {.base = text::Base::hexadecimal, .minimum_digits = 8}) == "0000beef");
    CHECK(unsigned_text(42, {.minimum_digits = 4}) == "0042");
    CHECK(signed_text(-5, {.minimum_digits = 4}) == "-0005");
    CHECK(unsigned_text(5, {.base = text::Base::binary}) == "101");
    CHECK(unsigned_text(8, {.base = text::Base::octal}) == "10");
    // %2zu, %-6d, fill
    CHECK(unsigned_text(3, {.field = {.width = 2}}) == " 3");
    CHECK(signed_text(-12, {.field = {.width = 6, .alignment = text::Alignment::left}}) == "-12   ");
    CHECK(signed_text(12, {.field = {.width = 5, .fill = '*'}}) == "***12");
    CHECK(signed_text(123456, {.field = {.width = 3}}) == "123456");
    // Caps: an enormous width or digit count is bounded.
    CHECK(unsigned_text(1, {.minimum_digits = 1u << 30}).size() == text::maximum_width);
    CHECK(signed_text(-1, {.minimum_digits = 1u << 30}).size() == text::maximum_number_bytes);
    CHECK(signed_text(1, {.field = {.width = usize{1} << 40}}).size() == text::maximum_width);
    // The generic forms pick signedness from the type.
    char both[16]{};
    text::TextWriter writer(stxlib::MutableView<char>{both});
    writer.write_integer(static_cast<unsigned char>(200));
    writer.write_integer(static_cast<short>(-3));
    CHECK(written(writer.finish()) == "200-3");
    CHECK(number(text::to_text(-17)) == "-17");
    CHECK(number(text::to_text(17ull)) == "17");
    CHECK(number(text::to_text(std::numeric_limits<i64>::min())) == "-9223372036854775808");
    CHECK(number(text::to_text(std::numeric_limits<u64>::max())) == "18446744073709551615");
}

void reals() {
    CHECK(number(text::to_text(0.1)) == "0.1");
    CHECK(number(text::to_text(1e21)) == "1e+21");
    CHECK(number(text::to_text(123456.0)) == "123456");
    CHECK(number(text::to_text(-0.0)) == "-0");
    // STXlib batch 3c9: a subnormal formats as zero of its sign; the
    // smallest normal is kept.
    CHECK(number(text::to_text(5e-324)) == "0");
    CHECK(number(text::to_text(-5e-324)) == "-0");
    CHECK(number(text::to_text(-0x1.fffffffffffffp-1023)) == "-0");
    CHECK(number(text::to_text(0x1p-149f)) == "0");
    CHECK(number(text::to_text(-2.2250738585072014e-308)) == "-2.2250738585072014e-308");
    CHECK(number(text::to_text(1.17549435e-38f)) == "1.1754944e-38");
    CHECK(number(text::to_text(0.1f)) == "0.1");
    CHECK(number(text::to_text(1.0f / 3.0f)) == "0.33333334");
    CHECK(number(text::to_text(std::numeric_limits<double>::quiet_NaN())) == "nan");
    CHECK(number(text::to_text(-std::numeric_limits<double>::quiet_NaN())) == "nan");
    CHECK(number(text::to_text(std::numeric_limits<double>::infinity())) == "inf");
    CHECK(number(text::to_text(-std::numeric_limits<double>::infinity())) == "-inf");
    // %.1f, %.3f, %3.0f, %.3e
    CHECK(real_text(2.25, {.notation = text::Notation::fixed, .precision = 1}) == "2.2");
    CHECK(real_text(2.35, {.notation = text::Notation::fixed, .precision = 1}) == "2.4");
    CHECK(real_text(-1.0005, {.notation = text::Notation::fixed, .precision = 3}) == "-1.000");
    CHECK(real_text(7.6, {.notation = text::Notation::fixed, .precision = 0, .field = {.width = 3}}) == "  8");
    CHECK(real_text(12345.678, {.notation = text::Notation::scientific, .precision = 3}) == "1.235e+04");
    CHECK(real_text(1.5, {.sign = text::Sign::always}) == "+1.5");
    CHECK(real_text(std::numeric_limits<double>::infinity(), {.sign = text::Sign::always}) == "+inf");
    const std::string huge = real_text(1e308, {.notation = text::Notation::fixed, .precision = 1u << 20});
    CHECK(huge.size() == 309 + 1 + text::maximum_precision);
    const std::string huge_negative =
        real_text(-1.7976931348623157e308, {.notation = text::Notation::fixed, .precision = 1u << 20});
    CHECK(huge_negative.size() == 1 + 309 + 1 + text::maximum_precision);
}

// The caller-storage rules: whole or nothing, and the size needed.
void caller_storage() {
    char small[4] = {'x', 'x', 'x', 'x'};
    const stxlib::MutableView<char> four(small);
    const text::Formatted too_long = text::format_signed(four, -12345);
    CHECK(!too_long.succeeded() && too_long.text() == nullptr && too_long.required_bytes() == 6);
    CHECK(small[0] == 'x' && small[3] == 'x');
    const text::Formatted fits = text::format_signed(four, -123);
    CHECK(fits.succeeded() && fits.required_bytes() == 4 && string_of(*fits.text()) == "-123");
    const text::Formatted nothing = text::format_unsigned(stxlib::MutableView<char>(), 7);
    CHECK(!nothing.succeeded() && nothing.required_bytes() == 1);
    const text::Formatted wide = text::format_real(four, 1.5, {.field = {.width = 10}});
    CHECK(!wide.succeeded() && wide.required_bytes() == 10);

    // Text padding counts scalar values: "é" is one value in two bytes.
    char padded[16]{};
    const text::Formatted accent =
        text::format_text(stxlib::MutableView<char>(padded), "\xc3\xa9", {.width = 3, .fill = '.'});
    CHECK(written(accent) == "..\xc3\xa9");
    const text::Formatted left =
        text::format_text(stxlib::MutableView<char>(padded), "ab", {.width = 4, .alignment = text::Alignment::left});
    CHECK(written(left) == "ab  ");

    // TextWriter: whole items, then the size the whole text needs.
    char line[12]{};
    text::TextWriter writer(stxlib::MutableView<char>{line});
    writer.write_text("Row ");
    writer.write_unsigned(5);
    writer.write_text(" of ");
    writer.write_unsigned(10);
    CHECK(written(writer.finish()) == "Row 5 of 10");
    writer.write_text(" rows");
    const text::Formatted overflowed = writer.finish();
    CHECK(!overflowed.succeeded() && overflowed.required_bytes() == 16);
    // After an item has not fitted, later items are not written even if
    // they would fit, so the storage holds whole items only.
    writer.write_text("!");
    CHECK(writer.finish().required_bytes() == 17 && line[11] == '\0');
    writer.clear();
    writer.write_real(0.25, {.notation = text::Notation::fixed, .precision = 3});
    CHECK(written(writer.finish()) == "0.250");
}

// STXlib batch 3c9: the subnormal rule, written here by classification
// (independently of src/core/subnormal.hpp).
double flushed(const double value) {
    if (std::fpclassify(value) == FP_SUBNORMAL) return std::copysign(0.0, value);
    return value;
}

float flushed(const float value) {
    if (std::fpclassify(value) == FP_SUBNORMAL) return std::copysign(0.0f, value);
    return value;
}

// Deterministic pseudo-random doubles over every exponent, subnormals
// included (SplitMix64 over raw bit patterns). STXlib: a subnormal sample
// formats and parses as its flushed value, zero of its sign, so the round
// trip and the host's text are taken for that value.
STXLIB_MODULAR u64 next_bits(u64& state) {
    state += 0x9e3779b97f4a7c15ULL;
    u64 mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    const u64 result = mixed ^ (mixed >> 31);
    return result;
}

void round_trips_and_host_agreement() {
    u64 state = 1;
    char host[2048];
    bool round_trips = true;
    bool host_agrees = true;
    for (int sample = 0; sample < 200000; ++sample) {
        const double value = flushed(std::bit_cast<double>(next_bits(state)));
        if (!std::isfinite(value)) continue;
        const text::NumberText shortest = text::to_text(value);
        const text::Parsed<double> back = text::parse_double(shortest.view());
        const bool same = back.succeeded() && std::bit_cast<u64>(*back.value()) == std::bit_cast<u64>(value);
        if (!same) std::fprintf(stderr, "no round trip: %s\n", number(shortest).c_str());
        round_trips = round_trips && same;
        if (sample % 4 != 0) continue;
        const int precision = sample % 25;
        std::snprintf(host, sizeof host, "%.*e", precision, value);
        const std::string scientific =
            real_text(value, {.notation = text::Notation::scientific, .precision = static_cast<usize>(precision)});
        if (scientific != host) std::fprintf(stderr, "scientific %s, host %s\n", scientific.c_str(), host);
        host_agrees = host_agrees && scientific == host;
        if (std::fabs(value) < 1e30) {
            std::snprintf(host, sizeof host, "%.*f", precision, value);
            const std::string fixed =
                real_text(value, {.notation = text::Notation::fixed, .precision = static_cast<usize>(precision)});
            if (fixed != host) std::fprintf(stderr, "fixed %s, host %s\n", fixed.c_str(), host);
            host_agrees = host_agrees && fixed == host;
        }
    }
    for (int sample = 0; sample < 100000; ++sample) {
        const float value = flushed(std::bit_cast<float>(static_cast<std::uint32_t>(next_bits(state))));
        if (!std::isfinite(value)) continue;
        const text::NumberText shortest = text::to_text(value);
        const text::Parsed<float> back = text::parse_float(shortest.view());
        const bool same =
            back.succeeded() && std::bit_cast<std::uint32_t>(*back.value()) == std::bit_cast<std::uint32_t>(value);
        if (!same) std::fprintf(stderr, "no float round trip: %s\n", number(shortest).c_str());
        round_trips = round_trips && same;
    }
    CHECK(round_trips);
    CHECK(host_agrees);
}

// The parsed value; zero after a failed check.
template <typename Value>
Value value_of(const text::Parsed<Value>& parsed) {
    CHECK(parsed.succeeded());
    if (!parsed.succeeded()) return Value{};
    const Value result = *parsed.value();
    return result;
}

template <typename Value>
bool fails_with(const text::Parsed<Value>& parsed, const text::ParseFailure failure) {
    const bool result = !parsed.succeeded() && *parsed.failure() == failure;
    return result;
}

void parsing() {
    CHECK(value_of(text::parse_signed("+5")) == 5);
    CHECK(value_of(text::parse_signed("-5")) == -5);
    CHECK(value_of(text::parse_signed("-9223372036854775808")) == std::numeric_limits<i64>::min());
    CHECK(value_of(text::parse_unsigned("ff", text::Base::hexadecimal)) == 255);
    CHECK(value_of(text::parse_unsigned("FF", text::Base::hexadecimal)) == 255);
    CHECK(fails_with(text::parse_signed(""), text::ParseFailure::empty));
    CHECK(fails_with(text::parse_signed(" 5"), text::ParseFailure::invalid));
    CHECK(fails_with(text::parse_signed("5 "), text::ParseFailure::trailing_text));
    CHECK(fails_with(text::parse_signed("5x"), text::ParseFailure::trailing_text));
    CHECK(fails_with(text::parse_signed("+"), text::ParseFailure::invalid));
    CHECK(fails_with(text::parse_signed("+-5"), text::ParseFailure::invalid));
    CHECK(fails_with(text::parse_signed("9223372036854775808"), text::ParseFailure::out_of_range));
    CHECK(fails_with(text::parse_unsigned("-1"), text::ParseFailure::invalid));
    CHECK(fails_with(text::parse_unsigned("0x10", text::Base::hexadecimal), text::ParseFailure::trailing_text));

    CHECK(value_of(text::parse_double("1.5e+3")) == 1500.0);
    CHECK(value_of(text::parse_double("+.5")) == 0.5);
    // STXlib batch 3c9: a real below the smallest normal parses as zero of
    // its sign, whether the host's from_chars gives the subnormal, zero, or
    // out_of_range (stx_auxlib reported 1e-400 and 2e-324 as out_of_range).
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("1e-310"))) == 0);
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("-1e-310"))) == 0x8000000000000000ULL);
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("1e-400"))) == 0);
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("-2e-324"))) == 0x8000000000000000ULL);
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("0.000000001e-320"))) == 0);
    CHECK(std::bit_cast<u64>(value_of(text::parse_double("2.2250738585072011e-308"))) == 0);
    CHECK(value_of(text::parse_double("2.2250738585072014e-308")) == 0x1p-1022);
    CHECK(value_of(text::parse_double("100000e-312")) == 1e-307);
    CHECK(std::bit_cast<std::uint32_t>(value_of(text::parse_float("1e-40"))) == 0);
    CHECK(std::bit_cast<std::uint32_t>(value_of(text::parse_float("-1e-50"))) == 0x80000000U);
    CHECK(value_of(text::parse_float("1.17549435e-38")) == 0x1p-126f);
    CHECK(fails_with(text::parse_double("1e99999999999999999999"), text::ParseFailure::out_of_range));
    CHECK(fails_with(text::parse_double("0.0001e313"), text::ParseFailure::out_of_range));
    CHECK(std::isinf(value_of(text::parse_double("inf"))));
    CHECK(std::isinf(value_of(text::parse_double("-Infinity"))));
    CHECK(std::isnan(value_of(text::parse_double("NaN"))));
    CHECK(std::signbit(value_of(text::parse_double("-0"))));
    CHECK(fails_with(text::parse_double("1e309"), text::ParseFailure::out_of_range));
    CHECK(fails_with(text::parse_float("3.5e38"), text::ParseFailure::out_of_range));
    CHECK(fails_with(text::parse_double("1e"), text::ParseFailure::trailing_text));
    CHECK(fails_with(text::parse_double("0x1p3"), text::ParseFailure::trailing_text));
    CHECK(fails_with(text::parse_double("."), text::ParseFailure::invalid));
    CHECK(fails_with(text::parse_double("1,5"), text::ParseFailure::trailing_text));

    CHECK(text::trim("  \t a b \r\n") == TextView("a b"));
    CHECK(text::trim(" \n ").empty());
    CHECK(text::trim("").empty());
}

void scanning() {
    const std::string input = "12,-34; name=5.5 tail";
    text::TextScanner scanner(view_of(input));
    i64 first = 0;
    i64 second = 0;
    TextView key{};
    double real = 0.0;
    CHECK(scanner.read_signed(first) && first == 12);
    CHECK(!scanner.read_signed(second) && scanner.position() == 2);
    CHECK(scanner.expect(",") && scanner.read_signed(second) && second == -34);
    CHECK(scanner.expect(";") && scanner.skip_spaces() == 1);
    CHECK(scanner.read_until("=", key) && key == TextView("name"));
    CHECK(!scanner.expect(":") && scanner.expect("="));
    CHECK(scanner.read_double(real) && real == 5.5);
    CHECK(scanner.rest() == TextView(" tail") && !scanner.at_end());
    TextView word{};
    static_cast<void>(scanner.skip_spaces());
    CHECK(scanner.read_until(" ", word) && word == TextView("tail") && scanner.at_end());
    CHECK(!scanner.read_until(" ", word));
    CHECK(!scanner.expect("x"));
}

} // namespace

int main() {
    integers();
    reals();
    caller_storage();
    round_trips_and_host_agreement();
    parsing();
    scanning();
    return stxlib_tests::finish("stxlib numeric text");
}
