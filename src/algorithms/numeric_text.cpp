// stxlib::text's numbers (include/stxlib/numeric_text.hpp), over
// std::to_chars and std::from_chars. Ported from stx_auxlib's src/text.cpp
// at b6e330a: the same digits, rounding, bounds and parse rules. Changes:
// output goes into the caller's storage, whole or not at all, instead of
// appending to a std::string; the long scratch for a long fixed or
// scientific real is on the stack (1,427 bytes) instead of the heap, so
// nothing here allocates; text is TextView.
//
// Where the standard library's own behaviour differs between hosts, this
// file decides instead: from_chars rejects a leading '+' (accepted here
// explicitly), and on out_of_range some libraries write infinity or zero
// into the result while the standard leaves it unchanged (here the result
// is always the out_of_range failure, and the library's value is unused).
//
// STXlib batch 3c9 (owner decision D1, 2026-11-13, notes
// decisions/2026-11-13-locals-float-mutability.md): no subnormals. A real
// that would parse to a subnormal parses as zero of its sign, and so does
// one too small for any value; hosts differ on whether from_chars reports
// those as out_of_range, so this file decides by the text's magnitude, and
// out_of_range remains only for overflow. Formatting reads a subnormal as
// zero of its sign.

#include "stxlib/numeric_text.hpp"

#include "core/subnormal.hpp"

#include <charconv>
#include <cmath>
#include <system_error>

namespace stxlib::text {

namespace {

// Scratch sizes. An integer needs at most 64 binary digits; a shortest real
// at most 24 characters ("-2.2250738585072014e-308"); a fixed or scientific
// real with small precision fits the short buffer, and only a long one
// needs the long buffer: a sign, up to 309 integer digits, the point,
// maximum_precision fraction digits, and an exponent.
constexpr usize short_scratch_bytes = 128;
constexpr usize long_scratch_bytes = 1 + 309 + 1 + maximum_precision + 16;

// The longest text a finite real can produce in a format, for choosing its
// scratch. A fixed form's integer digits are at most floor(e * log10 2) + 2
// for a binary exponent e >= 0 (log10 2 < 0.30103), and one below 1.
usize real_length_bound(const Notation notation, const usize precision, const int binary_exponent) noexcept {
    if (notation == Notation::shortest) return 32;
    if (notation == Notation::scientific) return 1 + 1 + 1 + precision + 6;
    // %g: the scientific form, or a fixed form of at most `precision`
    // significant digits after at most "0.000" (exponents down to -4).
    if (notation == Notation::general) return 1 + 1 + 1 + precision + 6 + 5;
    usize integer_digits = 1;
    if (binary_exponent > 0) integer_digits = static_cast<usize>(binary_exponent) * 30103 / 100000 + 2;
    const usize result = 1 + integer_digits + 1 + precision;
    return result;
}

bool is_space(const char character) noexcept {
    const bool result = character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
                        character == '\v' || character == '\f';
    return result;
}

usize scalar_values(const TextView text) noexcept {
    usize count = 0;
    const char* const bytes = text.data();
    const usize length = text.count();
    for (usize index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(bytes[index]);
        if ((byte & 0xc0U) != 0x80U) ++count;
    }
    return count;
}

usize smaller(const usize left, const usize right) noexcept {
    const usize result = left < right ? left : right;
    return result;
}

// The padding a field adds around an item of `length` scalar values.
usize padding_for(const Field& field, const usize length) noexcept {
    const usize width = smaller(field.width, maximum_width);
    if (length >= width) return 0;
    const usize result = width - length;
    return result;
}

// A run of ASCII bytes: the sign, the digits or the fill.
struct Piece final {
    const char* bytes{nullptr};
    usize count{0};
};

void write_fill(char* const target, const char fill, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) target[index] = fill;
}

void write_piece(char* const target, const Piece piece) noexcept {
    for (usize index = 0; index < piece.count; ++index) target[index] = piece.bytes[index];
}

// Writes `sign`, `zeros` zero digits and `digits` (whose scalar values
// number `digit_values`), padded to the field, when the whole fits.
// Requirement: sign and digits are well-formed UTF-8 and do not overlap
// the output; the fill is ASCII.
Formatted write_padded(const MutableView<char> output, const Piece sign, const usize zeros, const Piece digits,
                       const usize digit_values, const Field& field) noexcept {
    STXLIB_REQUIRE(static_cast<unsigned char>(field.fill) < 0x80U, "text::Field: the fill is not ASCII");
    const usize values = sign.count + zeros + digit_values;
    const usize padding = padding_for(field, values);
    // digits.count may be any size for format_text; the rest is bounded.
    const usize fixed_part = sign.count + zeros + padding;
    if (digits.count > usize_max - fixed_part) return Formatted::too_small(usize_max);
    const usize total = fixed_part + digits.count;
    if (total > output.count()) return Formatted::too_small(total);
    char* cursor = output.data();
    if (field.alignment == Alignment::right) {
        write_fill(cursor, field.fill, padding);
        cursor += padding;
    }
    write_piece(cursor, sign);
    cursor += sign.count;
    write_fill(cursor, '0', zeros);
    cursor += zeros;
    write_piece(cursor, digits);
    cursor += digits.count;
    if (field.alignment == Alignment::left) write_fill(cursor, field.fill, padding);
    const TextView text = stxlib::detail::TextViewAccess::adopt_validated(output.data(), total);
    const Formatted result = Formatted::written(text);
    return result;
}

void raise_letters(char* const first, char* const last) noexcept {
    for (char* character = first; character != last; ++character) {
        if (*character >= 'a' && *character <= 'z') *character = static_cast<char>(*character - 'a' + 'A');
    }
}

Formatted write_magnitude(const MutableView<char> output, const u64 magnitude, const bool negative,
                          const IntegerFormat& format) noexcept {
    char scratch[short_scratch_bytes]{};
    char* const first = scratch;
    const std::to_chars_result converted =
        std::to_chars(first, first + short_scratch_bytes, magnitude, static_cast<int>(format.base));
    // 64 binary digits always fit the 128-byte scratch.
    STXLIB_REQUIRE(converted.ec == std::errc(), "text: an integer did not fit its scratch buffer");
    if (format.letters == Letters::upper) raise_letters(first, converted.ptr);
    const Piece digits{first, static_cast<usize>(converted.ptr - first)};
    Piece sign{};
    if (negative) {
        sign = Piece{"-", 1};
    } else if (format.sign == Sign::always) {
        sign = Piece{"+", 1};
    }
    const usize minimum = smaller(format.minimum_digits, maximum_width);
    const usize zeros = minimum > digits.count ? minimum - digits.count : 0;
    const Formatted result = write_padded(output, sign, zeros, digits, digits.count, format.field);
    return result;
}

template <typename Real>
std::to_chars_result convert_real(char* const first, char* const last, const Real value, const Notation notation,
                                  const int precision) noexcept {
    switch (notation) {
    case Notation::fixed:
        return std::to_chars(first, last, value, std::chars_format::fixed, precision);
    case Notation::scientific:
        return std::to_chars(first, last, value, std::chars_format::scientific, precision);
    case Notation::general:
        return std::to_chars(first, last, value, std::chars_format::general, precision);
    case Notation::shortest:
        return std::to_chars(first, last, value);
    }
    // An enumerator outside the set: the shortest form (the default format).
    return std::to_chars(first, last, value);
}

template <typename Real>
Formatted write_converted(const MutableView<char> output, char* const first, char* const last, const Real value,
                          const Piece sign, const RealFormat& format, const int precision) noexcept {
    const std::to_chars_result converted = convert_real(first, last, value, format.notation, precision);
    // real_length_bound sized the scratch for the result; a failure here is
    // a defect in that bound, never output to emit.
    STXLIB_REQUIRE(converted.ec == std::errc(), "text: a real did not fit its scratch buffer");
    // to_chars writes '-' itself for negative values, -0 included.
    const Piece digits{first, static_cast<usize>(converted.ptr - first)};
    const Formatted result = write_padded(output, sign, 0, digits, digits.count, format.field);
    return result;
}

// The long path, in its own frame so the short path does not pay for
// clearing the long scratch.
template <typename Real>
[[gnu::noinline]] Formatted write_long_real(const MutableView<char> output, const Real value, const Piece sign,
                                            const RealFormat& format, const int precision) noexcept {
    char scratch[long_scratch_bytes]{};
    const Formatted result =
        write_converted(output, scratch, scratch + long_scratch_bytes, value, sign, format, precision);
    return result;
}

template <typename Real>
Formatted write_real_value(const MutableView<char> output, const Real given, const RealFormat& format) noexcept {
    // STXlib: a subnormal is formatted as zero of its sign (batch 3c9).
    const Real value = ::stxlib::detail::flush_subnormal(given);
    const bool negative = std::signbit(value);
    if (std::isnan(value)) {
        const Formatted result = write_padded(output, Piece{}, 0, Piece{"nan", 3}, 3, format.field);
        return result;
    }
    Piece sign{};
    if (!negative && format.sign == Sign::always) sign = Piece{"+", 1};
    if (std::isinf(value)) {
        if (negative) sign = Piece{"-", 1};
        const Formatted result = write_padded(output, sign, 0, Piece{"inf", 3}, 3, format.field);
        return result;
    }
    const usize bounded_precision = smaller(format.precision, maximum_precision);
    const int precision = static_cast<int>(bounded_precision);
    const int binary_exponent = value == 0 ? 0 : std::ilogb(value);
    if (real_length_bound(format.notation, bounded_precision, binary_exponent) > short_scratch_bytes) {
        const Formatted result = write_long_real(output, value, sign, format, precision);
        return result;
    }
    char scratch[short_scratch_bytes]{};
    const Formatted result =
        write_converted(output, scratch, scratch + short_scratch_bytes, value, sign, format, precision);
    return result;
}

// A number at the start of `text`: its value or failure, and the bytes it
// spans. `consumed` is 0 on failure.
template <typename Value>
struct Prefix final {
    Parsed<Value> parsed;
    usize consumed{0};
};

// Accepts one leading '+' (not followed by another sign), which from_chars
// refuses; '-' is left for from_chars.
usize plus_length(const TextView text) noexcept {
    const char* const bytes = text.data();
    if (text.count() >= 2 && bytes[0] == '+' && bytes[1] != '-' && bytes[1] != '+') return 1;
    return 0;
}

ParseFailure failure_from(const std::errc error) noexcept {
    if (error == std::errc::result_out_of_range) return ParseFailure::out_of_range;
    return ParseFailure::invalid;
}

template <typename Integer>
Prefix<Integer> integer_prefix(const TextView text, const Base base) noexcept {
    if (text.empty()) return Prefix<Integer>{Parsed<Integer>::failure(ParseFailure::empty), 0};
    const usize skipped = plus_length(text);
    const char* const first = text.data() + skipped;
    const char* const last = text.data() + text.count();
    Integer value = 0;
    const std::from_chars_result read = std::from_chars(first, last, value, static_cast<int>(base));
    if (read.ec != std::errc()) return Prefix<Integer>{Parsed<Integer>::failure(failure_from(read.ec)), 0};
    const usize consumed = static_cast<usize>(read.ptr - text.data());
    return Prefix<Integer>{Parsed<Integer>::success(value), consumed};
}

// STXlib batch 3c9: whether the decimal real spelled at [first, last) (an
// optional '-', digits with an optional point, an optional exponent; the
// span from_chars matched) has magnitude below 1. True for every real too
// small for its type: the out_of_range from_chars reports for such a text is
// underflow, never overflow. Decided from the digits: the place value of the
// first nonzero digit plus the exponent is negative. A text with no nonzero
// digit is zero, also below 1. Exponents saturate, so no arithmetic wraps.
bool below_one(const char* first, const char* const last) noexcept {
    if (first != last && *first == '-') ++first;
    constexpr i64 saturation = 1000000000;
    i64 integer_digits = 0;      // digits before the point, from the first nonzero one
    i64 leading_fraction_zeros = 0;
    bool nonzero = false;
    bool in_fraction = false;
    while (first != last) {
        const char character = *first;
        if (character == '.') {
            in_fraction = true;
        } else if (character >= '0' && character <= '9') {
            if (!in_fraction) {
                if (nonzero || character != '0') {
                    nonzero = true;
                    if (integer_digits < saturation) integer_digits += 1;
                }
            } else if (!nonzero) {
                if (character == '0') {
                    if (leading_fraction_zeros < saturation) leading_fraction_zeros += 1;
                } else {
                    nonzero = true;
                }
            }
        } else {
            break;
        }
        ++first;
    }
    if (!nonzero) return true;
    i64 exponent = 0;
    if (first != last && (*first == 'e' || *first == 'E')) {
        ++first;
        bool negative_exponent = false;
        if (first != last && (*first == '+' || *first == '-')) {
            negative_exponent = *first == '-';
            ++first;
        }
        while (first != last && *first >= '0' && *first <= '9') {
            if (exponent < saturation) exponent = exponent * 10 + (*first - '0');
            ++first;
        }
        if (negative_exponent) exponent = -exponent;
    }
    // The first nonzero digit's place value is 10^(integer_digits - 1) when
    // it is before the point, else 10^-(leading_fraction_zeros + 1).
    const i64 place = integer_digits > 0 ? integer_digits - 1 : -(leading_fraction_zeros + 1);
    return place + exponent < 0;
}

template <typename Real>
Prefix<Real> real_prefix(const TextView text) noexcept {
    if (text.empty()) return Prefix<Real>{Parsed<Real>::failure(ParseFailure::empty), 0};
    const usize skipped = plus_length(text);
    const char* const first = text.data() + skipped;
    const char* const last = text.data() + text.count();
    Real value = 0;
    const std::from_chars_result read = std::from_chars(first, last, value, std::chars_format::general);
    if (read.ec == std::errc::result_out_of_range && read.ptr != first && below_one(first, read.ptr)) {
        // STXlib: underflow, reported by some hosts for subnormal or
        // vanishing results: zero of the text's sign.
        const Real zero = *first == '-' ? -static_cast<Real>(0) : static_cast<Real>(0);
        const usize consumed = static_cast<usize>(read.ptr - text.data());
        return Prefix<Real>{Parsed<Real>::success(zero), consumed};
    }
    if (read.ec != std::errc()) return Prefix<Real>{Parsed<Real>::failure(failure_from(read.ec)), 0};
    const usize consumed = static_cast<usize>(read.ptr - text.data());
    // STXlib: a subnormal result is zero of its sign.
    return Prefix<Real>{Parsed<Real>::success(::stxlib::detail::flush_subnormal(value)), consumed};
}

template <typename Value>
Parsed<Value> whole(const Prefix<Value>& prefix, const TextView text) noexcept {
    if (!prefix.parsed.succeeded()) return prefix.parsed;
    if (prefix.consumed != text.count()) return Parsed<Value>::failure(ParseFailure::trailing_text);
    return prefix.parsed;
}

// text[offset, offset + length): whole UTF-8 sequences when the cut points
// are at sequence starts (callers cut only at ASCII bytes or the ends).
TextView slice(const TextView text, const usize offset, const usize length) noexcept {
    STXLIB_REQUIRE(offset <= text.count() && length <= text.count() - offset, "text: slice out of range");
    const TextView result = stxlib::detail::TextViewAccess::adopt_validated(text.data() + offset, length);
    return result;
}

u64 magnitude_of(const i64 value) noexcept {
    // |value| = -(value + 1) + 1, so i64_min has a magnitude without any
    // overflow or wrap.
    const u64 result = value < 0 ? static_cast<u64>(-(value + 1)) + 1 : static_cast<u64>(value);
    return result;
}

} // namespace

Formatted format_signed(const MutableView<char> output, const i64 value, const IntegerFormat& format) noexcept {
    const Formatted result = write_magnitude(output, magnitude_of(value), value < 0, format);
    return result;
}

Formatted format_unsigned(const MutableView<char> output, const u64 value, const IntegerFormat& format) noexcept {
    const Formatted result = write_magnitude(output, value, false, format);
    return result;
}

Formatted format_real(const MutableView<char> output, const double value, const RealFormat& format) noexcept {
    const Formatted result = write_real_value(output, value, format);
    return result;
}

Formatted format_real(const MutableView<char> output, const float value, const RealFormat& format) noexcept {
    const Formatted result = write_real_value(output, value, format);
    return result;
}

Formatted format_text(const MutableView<char> output, const TextView value, const Field& field) noexcept {
    const Piece bytes{value.data(), value.count()};
    const Formatted result = write_padded(output, Piece{}, 0, bytes, scalar_values(value), field);
    return result;
}

void TextWriter::record(const Formatted& piece) noexcept {
    const usize size = piece.required_bytes();
    required_ = size > usize_max - required_ ? usize_max : required_ + size;
    if (!piece.succeeded()) {
        overflowed_ = true;
        return;
    }
    written_ += size;
}

void TextWriter::write_signed(const i64 value, const IntegerFormat& format) noexcept {
    const MutableView<char> room = overflowed_ ? MutableView<char>() : storage_.from(written_);
    record(format_signed(room, value, format));
}

void TextWriter::write_unsigned(const u64 value, const IntegerFormat& format) noexcept {
    const MutableView<char> room = overflowed_ ? MutableView<char>() : storage_.from(written_);
    record(format_unsigned(room, value, format));
}

void TextWriter::write_real(const double value, const RealFormat& format) noexcept {
    const MutableView<char> room = overflowed_ ? MutableView<char>() : storage_.from(written_);
    record(format_real(room, value, format));
}

void TextWriter::write_real(const float value, const RealFormat& format) noexcept {
    const MutableView<char> room = overflowed_ ? MutableView<char>() : storage_.from(written_);
    record(format_real(room, value, format));
}

void TextWriter::write_text(const TextView value, const Field& field) noexcept {
    const MutableView<char> room = overflowed_ ? MutableView<char>() : storage_.from(written_);
    record(format_text(room, value, field));
}

void TextWriter::clear() noexcept {
    written_ = 0;
    required_ = 0;
    overflowed_ = false;
}

Formatted TextWriter::finish() const noexcept {
    if (overflowed_) return Formatted::too_small(required_);
    const TextView text = stxlib::detail::TextViewAccess::adopt_validated(storage_.data(), written_);
    const Formatted result = Formatted::written(text);
    return result;
}

namespace {

// The default-format text of a number in `bytes`, whose capacity is
// NumberText::capacity; returns its length. The capacity holds every
// default-format number (a sign and 20 integer digits, 24 bytes for a
// shortest double), so a too_small here is a defect.
usize number_bytes(const Formatted& formatted) noexcept {
    STXLIB_REQUIRE(formatted.succeeded(), "text: a number did not fit NumberText");
    const usize result = formatted.required_bytes();
    return result;
}

} // namespace

NumberText to_text(const i64 value) noexcept {
    NumberText result{};
    result.count_ = number_bytes(format_signed(MutableView<char>(result.bytes_), value));
    return result;
}

NumberText to_text(const u64 value) noexcept {
    NumberText result{};
    result.count_ = number_bytes(format_unsigned(MutableView<char>(result.bytes_), value));
    return result;
}

NumberText to_text(const double value) noexcept {
    NumberText result{};
    result.count_ = number_bytes(format_real(MutableView<char>(result.bytes_), value));
    return result;
}

NumberText to_text(const float value) noexcept {
    NumberText result{};
    result.count_ = number_bytes(format_real(MutableView<char>(result.bytes_), value));
    return result;
}

Parsed<i64> parse_signed(const TextView text, const Base base) noexcept {
    const Parsed<i64> result = whole(integer_prefix<i64>(text, base), text);
    return result;
}

Parsed<u64> parse_unsigned(const TextView text, const Base base) noexcept {
    const Parsed<u64> result = whole(integer_prefix<u64>(text, base), text);
    return result;
}

Parsed<double> parse_double(const TextView text) noexcept {
    const Parsed<double> result = whole(real_prefix<double>(text), text);
    return result;
}

Parsed<float> parse_float(const TextView text) noexcept {
    const Parsed<float> result = whole(real_prefix<float>(text), text);
    return result;
}

TextView trim(const TextView text) noexcept {
    const char* const bytes = text.data();
    usize first = 0;
    usize last = text.count();
    while (first < last && is_space(bytes[first])) ++first;
    while (last > first && is_space(bytes[last - 1])) --last;
    const TextView result = slice(text, first, last - first);
    return result;
}

TextScanner::TextScanner(const TextView text) noexcept : text_(text) {}

bool TextScanner::read_signed(i64& value, const Base base) noexcept {
    const Prefix<i64> prefix = integer_prefix<i64>(rest(), base);
    const i64* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::read_unsigned(u64& value, const Base base) noexcept {
    const Prefix<u64> prefix = integer_prefix<u64>(rest(), base);
    const u64* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::read_double(double& value) noexcept {
    const Prefix<double> prefix = real_prefix<double>(rest());
    const double* const read = prefix.parsed.value();
    if (read == nullptr) return false;
    value = *read;
    position_ += prefix.consumed;
    return true;
}

bool TextScanner::expect(const TextView literal) noexcept {
    const TextView remaining = rest();
    if (literal.count() > remaining.count()) return false;
    if (slice(remaining, 0, literal.count()) != literal) return false;
    position_ += literal.count();
    return true;
}

bool TextScanner::read_until(const TextView delimiters, TextView& field) noexcept {
    const char* const stops = delimiters.data();
    const usize stop_count = delimiters.count();
    for (usize stop = 0; stop < stop_count; ++stop) {
        STXLIB_REQUIRE(static_cast<unsigned char>(stops[stop]) < 0x80U, "TextScanner::read_until: a delimiter is not ASCII");
    }
    const TextView remaining = rest();
    const char* const bytes = remaining.data();
    usize length = 0;
    bool stopped = false;
    while (length < remaining.count() && !stopped) {
        for (usize stop = 0; stop < stop_count; ++stop) stopped = stopped || bytes[length] == stops[stop];
        if (!stopped) ++length;
    }
    if (length == 0) return false;
    field = slice(remaining, 0, length);
    position_ += length;
    return true;
}

usize TextScanner::skip_spaces() noexcept {
    const usize start = position_;
    const char* const bytes = text_.data();
    while (position_ < text_.count() && is_space(bytes[position_])) ++position_;
    const usize result = position_ - start;
    return result;
}

bool TextScanner::at_end() const noexcept {
    const bool result = position_ == text_.count();
    return result;
}

usize TextScanner::position() const noexcept { return position_; }

TextView TextScanner::rest() const noexcept {
    const TextView result = slice(text_, position_, text_.count() - position_);
    return result;
}

} // namespace stxlib::text
