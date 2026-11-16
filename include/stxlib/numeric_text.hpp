#pragma once

// Locale-free formatting and parsing of numbers, replacing snprintf,
// sscanf, std::to_string, std::sto*, atoi/atof, the stream manipulators and
// setlocale. There are no format strings: every format is a typed record,
// so no format can mismatch its arguments or be supplied by input. Output
// is identical on every host and in every locale. Ported from stx_auxlib's
// include/stx_auxlib/text.hpp at b6e330a; the formats, digits, rounding and
// parse rules are unchanged, and the interface is restated in STXlib's
// vocabulary:
// - formatting writes into storage the caller supplies (house style,
//   section 6) instead of appending to a std::string: each format_ call
//   writes the whole item or, when it does not fit, nothing, and reports
//   the bytes it needs; TextWriter composes several items in one buffer;
//   to_text returns a small fixed record (NumberText) for the one-off case;
// - text is TextView (well-formed UTF-8) in and out;
// - Parsed is a tagged union with this header's layout, as OperationResult is.
//
// The numbers are std::to_chars's and std::from_chars's (inside the .cpp),
// whose results the standard fixes exactly: the shortest form of a floating
// value is the shortest text that parses back to the same value (the
// shorter of fixed and scientific forms, fixed on a tie), and fixed and
// scientific forms are correctly rounded. NaN is always spelled "nan" (sign
// and payload are not written) and infinities "inf" and "-inf".
//
// Parsing reads whole texts (or, with TextScanner, consecutive fields) and
// reports failures as values; it never skips whitespace on its own.
//
// Every function here is pure computation: no allocation, no host call, no
// state.

#include "stxlib/require.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::text {

enum class Base : u8 {
    binary = 2,
    octal = 8,
    decimal = 10,
    hexadecimal = 16,
};

enum class Sign : u8 {
    // "-" for negative values only.
    negative_only = 0,
    // "+" or "-" always ("+0" for zero).
    always = 1,
};

enum class Letters : u8 {
    lower = 0,
    upper = 1,
};

enum class Alignment : u8 {
    right = 0,
    left = 1,
};

// Bounds the caller-chosen sizes of a format, so that no format can request
// unbounded output: Field::width and IntegerFormat::minimum_digits above
// maximum_width, and RealFormat::precision above maximum_precision, are
// treated as those maxima.
inline constexpr usize maximum_width = 4096;
// Covers every digit a double's exact decimal expansion can have.
inline constexpr usize maximum_precision = 1100;

// The most bytes one formatted number can take, under the bounds above: a
// sign and maximum_width digits (an integer with minimum_digits at the
// maximum). A real takes at most max(1 + 309 + 1 + maximum_precision,
// maximum_width) = 4096. A buffer of this size never reports too_small for a
// number.
inline constexpr usize maximum_number_bytes = 1 + maximum_width;

// Pads the formatted item to `width` with `fill`, counting Unicode scalar
// values (not bytes, not display columns). An item already at least `width`
// long is not cut. Requirement: `fill` is ASCII (below 0x80), so the output
// stays UTF-8.
struct Field final {
    usize width{0};
    char fill{' '};
    Alignment alignment{Alignment::right};
};

struct IntegerFormat final {
    Base base{Base::decimal};
    // Zero-pads the digits (not the sign) to at least this many: "%04x" is
    // {.base = Base::hexadecimal, .minimum_digits = 4}.
    usize minimum_digits{1};
    Sign sign{Sign::negative_only};
    Letters letters{Letters::lower};
    Field field{};
};

enum class Notation : u8 {
    // The shortest text that parses back to the same value.
    shortest = 0,
    // `precision` digits after the point ("%.2f").
    fixed = 1,
    // One digit, the point, `precision` digits, and a signed exponent of at
    // least two digits ("%.3e").
    scientific = 2,
    // printf's %g in the C locale ("%.17g"; an ostream's default float
    // field after setprecision): `precision` significant digits (0 counts
    // as 1); scientific when the decimal exponent X of the rounded value is
    // below -4 or at least the precision, fixed with precision - 1 - X
    // digits after the point otherwise; trailing zeros, and a point left
    // without digits, removed (std::to_chars with chars_format::general and
    // a precision, which the standard defines by printf's %g). M2 C3.
    general = 3,
};

struct RealFormat final {
    Notation notation{Notation::shortest};
    // Ignored by Notation::shortest.
    usize precision{6};
    Sign sign{Sign::negative_only};
    Field field{};
};

// The outcome of formatting into caller storage: the text written, or the
// bytes the whole text needs when the storage was too small (nothing was
// written then). Built only through the two factories.
class Formatted final {
public:
    // Requirement: `written_text` is the text written.
    [[nodiscard]] static Formatted written(const TextView written_text) noexcept {
        const Formatted result(written_text);
        return result;
    }
    [[nodiscard]] static Formatted too_small(const usize needed_bytes) noexcept {
        const Formatted result(needed_bytes);
        return result;
    }

    [[nodiscard]] bool succeeded() const noexcept { return outcome_ == Outcome::written; }

    // The text, borrowed from the caller's storage (valid while that storage
    // is alive and unchanged); null when the storage was too small.
    [[nodiscard]] const TextView* text() const& noexcept {
        if (outcome_ != Outcome::written) return nullptr;
        return &state_.text;
    }
    const TextView* text() const&& = delete;

    // The bytes the whole text takes: the written count on success, the
    // storage needed otherwise.
    [[nodiscard]] usize required_bytes() const noexcept {
        if (outcome_ == Outcome::written) return state_.text.count();
        return state_.needed;
    }

private:
    enum class Outcome : u8 {
        written = 0,
        too_small = 1,
    };
    union State {
        TextView text;
        usize needed;

        explicit constexpr State(const TextView written_text) noexcept : text(written_text) {}
        explicit constexpr State(const usize needed_bytes) noexcept : needed(needed_bytes) {}
    };

    explicit Formatted(const TextView written_text) noexcept : outcome_(Outcome::written), state_(written_text) {}
    explicit Formatted(const usize needed_bytes) noexcept : outcome_(Outcome::too_small), state_(needed_bytes) {}

    Outcome outcome_;
    State state_;
};

// Formatting into `output`. Each writes the whole item at output[0, n) and
// returns written(that text), or writes nothing and returns too_small(n)
// when n > output.count(). The output is borrowed for the call only.
[[nodiscard]] Formatted format_signed(MutableView<char> output, i64 value, const IntegerFormat& format = {}) noexcept;
[[nodiscard]] Formatted format_unsigned(MutableView<char> output, u64 value, const IntegerFormat& format = {}) noexcept;
[[nodiscard]] Formatted format_real(MutableView<char> output, double value, const RealFormat& format = {}) noexcept;
// Shortest form here is the shortest that round-trips as a float. Both read
// a subnormal value as zero of its sign ("0" or "-0" in shortest form;
// STXlib's subnormal rule, batch 3c9).
[[nodiscard]] Formatted format_real(MutableView<char> output, float value, const RealFormat& format = {}) noexcept;
// Pads `value` to the field. Requirement: `value` does not overlap `output`.
// Needs value.count() plus at most maximum_width bytes.
[[nodiscard]] Formatted format_text(MutableView<char> output, TextView value, const Field& field = {}) noexcept;

namespace detail {

// The integer types the format_integer, write_integer and to_text templates
// accept: the standard integer and character types, bool excluded (as
// std::integral without bool, spelled out because public headers hold no
// std:: name and compiler builtins differ between Clang and GCC).
template <typename Type>
inline constexpr bool integer_value = false;
template <> inline constexpr bool integer_value<char> = true;
template <> inline constexpr bool integer_value<signed char> = true;
template <> inline constexpr bool integer_value<unsigned char> = true;
// std-rules: allow(wide-text) classifies the type for to_text; no wide text is handled
template <> inline constexpr bool integer_value<wchar_t> = true;
template <> inline constexpr bool integer_value<char8_t> = true;
template <> inline constexpr bool integer_value<char16_t> = true;
template <> inline constexpr bool integer_value<char32_t> = true;
template <> inline constexpr bool integer_value<short> = true;
template <> inline constexpr bool integer_value<unsigned short> = true;
template <> inline constexpr bool integer_value<int> = true;
template <> inline constexpr bool integer_value<unsigned int> = true;
template <> inline constexpr bool integer_value<long> = true;
template <> inline constexpr bool integer_value<unsigned long> = true;
template <> inline constexpr bool integer_value<long long> = true;
template <> inline constexpr bool integer_value<unsigned long long> = true;

template <typename Integer>
inline constexpr bool signed_value = static_cast<Integer>(-1) < static_cast<Integer>(0);

template <typename First, typename Second>
inline constexpr bool same_type = false;
template <typename Type>
inline constexpr bool same_type<Type, Type> = true;

} // namespace detail

// Any integer type but bool, as signed or unsigned by its signedness.
template <typename Integer>
concept IntegerValue = detail::integer_value<Integer>;

template <IntegerValue Integer>
[[nodiscard]] Formatted format_integer(const MutableView<char> output, const Integer value,
                                       const IntegerFormat& format = {}) noexcept {
    if constexpr (detail::signed_value<Integer>) {
        const Formatted result = format_signed(output, static_cast<i64>(value), format);
        return result;
    } else {
        const Formatted result = format_unsigned(output, static_cast<u64>(value), format);
        return result;
    }
}

// Several items written one after another into one caller buffer, as
// appending to a string did. Each write either appends its whole item or,
// once an item has not fitted, appends nothing (so the text written is
// always whole items); required_bytes keeps counting, so after a failure
// the caller learns the size the whole text needs and can retry with it.
// Borrows `storage`: declare the writer after the storage it writes.
class TextWriter final {
public:
    explicit TextWriter(const MutableView<char> storage) noexcept : storage_(storage) {}

    void write_signed(i64 value, const IntegerFormat& format = {}) noexcept;
    void write_unsigned(u64 value, const IntegerFormat& format = {}) noexcept;
    void write_real(double value, const RealFormat& format = {}) noexcept;
    void write_real(float value, const RealFormat& format = {}) noexcept;
    // Requirement: `value` does not overlap the storage.
    void write_text(TextView value, const Field& field = {}) noexcept;

    template <IntegerValue Integer>
    void write_integer(const Integer value, const IntegerFormat& format = {}) noexcept {
        if constexpr (detail::signed_value<Integer>) {
            write_signed(static_cast<i64>(value), format);
        } else {
            write_unsigned(static_cast<u64>(value), format);
        }
    }

    // Starts again at the beginning of the storage.
    void clear() noexcept;

    // written(everything) when every item fitted, else too_small(the bytes
    // all the items need, saturating at usize_max). The text is borrowed
    // from the storage.
    [[nodiscard]] Formatted finish() const noexcept;

private:
    void record(const Formatted& piece) noexcept;

    MutableView<char> storage_;
    usize written_{0};
    usize required_{0};
    bool overflowed_{false};
};

// A number's text in a small fixed record: an integer's decimal form, or a
// real's shortest form (at most 24 bytes: "-2.2250738585072014e-308").
class NumberText final {
public:
    static constexpr usize capacity = 32;

    // Borrowed from this record.
    [[nodiscard]] TextView view() const& noexcept {
        const TextView result = stxlib::detail::TextViewAccess::adopt_validated(bytes_, count_);
        return result;
    }
    TextView view() const&& = delete;

private:
    friend NumberText to_text(i64 value) noexcept;
    friend NumberText to_text(u64 value) noexcept;
    friend NumberText to_text(double value) noexcept;
    friend NumberText to_text(float value) noexcept;

    char bytes_[capacity]{};
    usize count_{0};
};

[[nodiscard]] NumberText to_text(i64 value) noexcept;
[[nodiscard]] NumberText to_text(u64 value) noexcept;
[[nodiscard]] NumberText to_text(double value) noexcept;
[[nodiscard]] NumberText to_text(float value) noexcept;

template <IntegerValue Integer>
    requires(!detail::same_type<Integer, i64> && !detail::same_type<Integer, u64>)
[[nodiscard]] NumberText to_text(const Integer value) noexcept {
    if constexpr (detail::signed_value<Integer>) {
        const NumberText result = to_text(static_cast<i64>(value));
        return result;
    } else {
        const NumberText result = to_text(static_cast<u64>(value));
        return result;
    }
}

enum class ParseFailure : u8 {
    empty = 1,
    // A character that cannot start or continue the number.
    invalid = 2,
    // A well-formed number the type cannot hold: integer overflow, or a
    // real whose magnitude rounds to infinity or to zero (a nonzero literal
    // never parses as zero).
    out_of_range = 3,
    // A number followed by more text.
    trailing_text = 4,
};

// The result of a parse: the value, or why it failed. Never both. A
// tagged union laid out here (a one-byte outcome, then the alternative),
// built only through the factories and read only through the queries.
template <typename Value>
class Parsed final {
    static_assert(__is_trivially_copyable(Value), "Parsed holds plain numbers");

public:
    [[nodiscard]] static Parsed success(const Value parsed_value) noexcept {
        const Parsed result(parsed_value);
        return result;
    }
    [[nodiscard]] static Parsed failure(const ParseFailure parse_failure) noexcept {
        const Parsed result(parse_failure);
        return result;
    }
    [[nodiscard]] bool succeeded() const noexcept { return outcome_ == Outcome::success; }
    // The value; null on failure. Borrowed from this record.
    [[nodiscard]] const Value* value() const& noexcept {
        if (outcome_ != Outcome::success) return nullptr;
        return &state_.value;
    }
    const Value* value() const&& = delete;
    // The failure; null on success. Borrowed from this record.
    [[nodiscard]] const ParseFailure* failure() const& noexcept {
        if (outcome_ != Outcome::failure) return nullptr;
        return &state_.failure;
    }
    const ParseFailure* failure() const&& = delete;

private:
    enum class Outcome : u8 {
        success = 0,
        failure = 1,
    };
    union State {
        Value value;
        ParseFailure failure;

        explicit constexpr State(const Value parsed_value) noexcept : value(parsed_value) {}
        explicit constexpr State(const ParseFailure parse_failure) noexcept : failure(parse_failure) {}
    };

    explicit Parsed(const Value parsed_value) noexcept : outcome_(Outcome::success), state_(parsed_value) {}
    explicit Parsed(const ParseFailure parse_failure) noexcept : outcome_(Outcome::failure), state_(parse_failure) {}

    Outcome outcome_;
    State state_;
};

// Whole-text parses. Integers: an optional '+' (or '-' for signed), then
// digits of the base, with no prefix ("0x" is not accepted). Reals: an
// optional sign, decimal digits with an optional point and exponent, or
// "inf", "infinity" or "nan" in any case; hexadecimal floats are not
// accepted. A real whose value is below the type's smallest normal in
// magnitude (after rounding) parses as zero of its sign, on every host;
// out_of_range is for overflow only (STXlib's subnormal rule, batch 3c9;
// stx_auxlib reported underflow below the subnormal range as out_of_range).
[[nodiscard]] Parsed<i64> parse_signed(TextView text, Base base = Base::decimal) noexcept;
[[nodiscard]] Parsed<u64> parse_unsigned(TextView text, Base base = Base::decimal) noexcept;
[[nodiscard]] Parsed<double> parse_double(TextView text) noexcept;
[[nodiscard]] Parsed<float> parse_float(TextView text) noexcept;

// `text` without leading and trailing ASCII whitespace (space, tab, line
// feed, carriage return, vertical tab, form feed). Borrows what `text`
// borrows.
[[nodiscard]] TextView trim(TextView text) noexcept;

// Reads consecutive fields from text it borrows, replacing sscanf. Every
// read either consumes its field and succeeds, or fails and consumes
// nothing. Reads never skip whitespace; call skip_spaces where the format
// allows it. Declare the scanner after the storage its text borrows.
class TextScanner final {
public:
    explicit TextScanner(TextView text) noexcept;

    [[nodiscard]] bool read_signed(i64& value, Base base = Base::decimal) noexcept;
    [[nodiscard]] bool read_unsigned(u64& value, Base base = Base::decimal) noexcept;
    [[nodiscard]] bool read_double(double& value) noexcept;
    // Consumes `literal` exactly when the text continues with it.
    [[nodiscard]] bool expect(TextView literal) noexcept;
    // Reads up to (not including) the first byte of `delimiters`, or to the
    // end; fails on an empty field. Requirement: every delimiter is ASCII,
    // so a field never ends inside a UTF-8 sequence.
    [[nodiscard]] bool read_until(TextView delimiters, TextView& field) noexcept;
    // Skips ASCII whitespace; returns how many bytes it skipped.
    usize skip_spaces() noexcept;

    [[nodiscard]] bool at_end() const noexcept;
    // Bytes consumed so far.
    [[nodiscard]] usize position() const noexcept;
    // The text not yet consumed; borrows what the scanner's text borrows.
    [[nodiscard]] TextView rest() const noexcept;

private:
    TextView text_;
    usize position_{0};
};

} // namespace stxlib::text
