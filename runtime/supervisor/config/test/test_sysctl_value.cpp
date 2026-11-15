// test_sysctl_value.cpp: host tests of the sysctl command's value reading
// (sysctl_value.h).
//
// 1. Syntax, exhaustive: every text of 0 to 4 bytes over an alphabet of 14
//    ("0", "1", "7", "8", "9", "a", "f", "F", "x", "X", "-", "+", "g" and a
//    blank): 41,371 texts, for each of the 12 integer kinds (496,452
//    cases), against a reference built on the host C library's strtoll and
//    strtoull with base 0 (what sysctl(8) calls) plus the stated
//    differences: no leading blank, no minus for an unsigned kind, the
//    whole text, and the kind's range. Checked: accepted exactly when the
//    reference accepts, the same bits when accepted, and out_of_range
//    exactly when the reference read the whole text but the value does not
//    fit (malformed or empty otherwise).
// 2. Ranges, exhaustive for the narrow kinds: every integer from -70,000
//    to 70,000 written in decimal, in hexadecimal ("0x" and the magnitude)
//    and in octal ("0" and the magnitude), each with its sign, for the
//    8- and 16-bit kinds: 1,680,012 cases against the kind's range.
// 3. Ranges at the edges for the 32- and 64-bit kinds: each bound and the
//    values one past it, in decimal and hexadecimal, and 2^64, 2^64 - 1
//    and -2^63 - 1.
// 4. The other kinds and flags: strings, a read-only variable, a node, an
//    opaque variable, kind 0, an "IK" format, and an empty number.

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../sysctl_value.h"
#include "test_support.h"

namespace {

using stxconfig::parse_sysctl_value;
using stxconfig::SysctlNumber;
using stxconfig::SysctlRefusal;
using stxconfig::SysctlValue;
using stxtest::check;

constexpr std::uint32_t writable = stxconfig::sysctl_flag_writable;

struct Kind {
    std::uint32_t type;
    unsigned width;
    bool is_signed;
};

constexpr Kind integer_kinds[12] = {
    {stxconfig::sysctl_type_int, 4, true},   {stxconfig::sysctl_type_uint, 4, false},
    {stxconfig::sysctl_type_long, 8, true},  {stxconfig::sysctl_type_ulong, 8, false},
    {stxconfig::sysctl_type_s8, 1, true},    {stxconfig::sysctl_type_s16, 2, true},
    {stxconfig::sysctl_type_s32, 4, true},   {stxconfig::sysctl_type_s64, 8, true},
    {stxconfig::sysctl_type_u8, 1, false},   {stxconfig::sysctl_type_u16, 2, false},
    {stxconfig::sysctl_type_u32, 4, false},  {stxconfig::sysctl_type_u64, 8, false},
};

enum class Expected : std::uint8_t {
    accepted,
    out_of_range,
    malformed,
};

struct Reference {
    Expected outcome;
    std::uint64_t bits;
};

[[nodiscard]] std::uint64_t mask_of(unsigned width) noexcept {
    return width == 8u ? UINT64_MAX : (std::uint64_t{1} << (width * 8u)) - 1u;
}

// The reference: the C library's reading with base 0, then the stated rules.
[[nodiscard]] Reference reference(const Kind& kind, const char* text) noexcept {
    Reference result{Expected::malformed, 0};
    if (text[0] == '\0' || text[0] == ' ' || text[0] == '\t') {
        return result;
    }
    if (!kind.is_signed && text[0] == '-') {
        return result;
    }
    char* end = nullptr;
    errno = 0;
    if (kind.is_signed) {
        const long long value = std::strtoll(text, &end, 0);
        if (end == text || *end != '\0') {
            return result;
        }
        if (errno == ERANGE) {
            result.outcome = Expected::out_of_range;
            return result;
        }
        const unsigned bits = kind.width * 8u;
        const long long maximum = bits == 64u ? INT64_MAX : static_cast<long long>((std::uint64_t{1} << (bits - 1u)) - 1u);
        const long long minimum = -maximum - 1;
        if (value > maximum || value < minimum) {
            result.outcome = Expected::out_of_range;
            return result;
        }
        result.outcome = Expected::accepted;
        result.bits = static_cast<std::uint64_t>(value) & mask_of(kind.width);
        return result;
    }
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return result;
    }
    if (errno == ERANGE) {
        result.outcome = Expected::out_of_range;
        return result;
    }
    if (value > mask_of(kind.width)) {
        result.outcome = Expected::out_of_range;
        return result;
    }
    result.outcome = Expected::accepted;
    result.bits = value;
    return result;
}

void compare(const Kind& kind, const char* text, const char* test) noexcept {
    const Reference expected = reference(kind, text);
    const SysctlValue got = parse_sysctl_value(kind.type | writable, "", text);
    const SysctlNumber* const number = got.as_number();
    const SysctlRefusal* const refusal = got.as_refused();
    if (expected.outcome == Expected::accepted) {
        const bool same = number != nullptr && (*number).bits == expected.bits &&
                          (*number).width == kind.width && (*number).is_signed == kind.is_signed;
        check(same, test, "accepted with the reference's bits");
        if (!same) {
            std::printf("  type=%u text=\"%s\"\n", kind.type, text);
        }
        return;
    }
    const bool refused = refusal != nullptr;
    check(refused, test, "refused when the reference refuses");
    if (!refused) {
        std::printf("  type=%u text=\"%s\"\n", kind.type, text);
        return;
    }
    if (expected.outcome == Expected::out_of_range) {
        check(*refusal == SysctlRefusal::out_of_range, test, "out_of_range where the reference read a number");
    } else {
        check(*refusal == SysctlRefusal::malformed_number || *refusal == SysctlRefusal::empty_number, test,
              "malformed or empty otherwise");
    }
}

std::size_t test_syntax() {
    constexpr char alphabet[] = {'0', '1', '7', '8', '9', 'a', 'f', 'F', 'x', 'X', '-', '+', 'g', ' '};
    constexpr std::size_t symbols = sizeof(alphabet);
    std::size_t cases = 0;
    char text[5];
    for (std::size_t length = 0; length <= 4u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                text[position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            text[length] = '\0';
            for (const Kind& kind : integer_kinds) {
                compare(kind, text, "syntax");
                cases += 1;
            }
        }
    }
    return cases;
}

std::size_t test_narrow_ranges() {
    std::size_t cases = 0;
    char text[32];
    for (long long value = -70000; value <= 70000; value += 1) {
        const unsigned long long magnitude =
            value < 0 ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
        const char* const sign = value < 0 ? "-" : "";
        for (int form = 0; form < 3; form += 1) {
            if (form == 0) {
                (void)std::snprintf(text, sizeof(text), "%s%llu", sign, magnitude);
            } else if (form == 1) {
                (void)std::snprintf(text, sizeof(text), "%s0x%llx", sign, magnitude);
            } else {
                (void)std::snprintf(text, sizeof(text), "%s0%llo", sign, magnitude);
            }
            for (const Kind& kind : integer_kinds) {
                if (kind.width > 2u) {
                    continue;
                }
                compare(kind, text, "narrow ranges");
                cases += 1;
            }
        }
    }
    return cases;
}

void test_wide_edges() {
    const char* const texts[] = {
        "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296",
        "0x7fffffff", "0x80000000", "-0x80000000", "-0x80000001", "0xffffffff", "0x100000000",
        "9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809",
        "18446744073709551615", "18446744073709551616", "0xffffffffffffffff", "0x10000000000000000",
        "-18446744073709551615", "0777777777777777777777", "01777777777777777777777", "02000000000000000000000",
    };
    for (const char* const text : texts) {
        for (const Kind& kind : integer_kinds) {
            if (kind.width < 4u) {
                continue;
            }
            compare(kind, text, "wide edges");
        }
    }
}

void test_other_kinds() {
    const SysctlValue text_value = parse_sysctl_value(stxconfig::sysctl_type_string | writable, "A", "a b=c");
    const stxconfig::SysctlString* const string = text_value.as_string();
    check(string != nullptr && (*string).length == 5u && std::strcmp((*string).bytes, "a b=c") == 0, "kinds",
          "a string is written as given, without its NUL");
    const SysctlValue empty_string = parse_sysctl_value(stxconfig::sysctl_type_string | writable, "A", "");
    check(empty_string.as_string() != nullptr && (*empty_string.as_string()).length == 0u, "kinds",
          "an empty string is a string");
    const SysctlValue read_only = parse_sysctl_value(stxconfig::sysctl_type_int, "I", "1");
    check(read_only.as_refused() != nullptr && *read_only.as_refused() == SysctlRefusal::read_only, "kinds",
          "a read-only variable is refused");
    const std::uint32_t not_settable[] = {stxconfig::sysctl_type_node, stxconfig::sysctl_type_opaque, 0u};
    for (const std::uint32_t type : not_settable) {
        const SysctlValue value = parse_sysctl_value(type | writable, "", "1");
        check(value.as_refused() != nullptr && *value.as_refused() == SysctlRefusal::not_settable, "kinds",
              "nodes, opaque variables and kind 0 are refused");
    }
    const SysctlValue kelvin = parse_sysctl_value(stxconfig::sysctl_type_int | writable, "IK", "2931");
    check(kelvin.as_refused() != nullptr && *kelvin.as_refused() == SysctlRefusal::format_unsupported, "kinds",
          "an IK format is refused");
    const SysctlValue empty = parse_sysctl_value(stxconfig::sysctl_type_int | writable, "I", "");
    check(empty.as_refused() != nullptr && *empty.as_refused() == SysctlRefusal::empty_number, "kinds",
          "an empty number is refused");
    const SysctlValue zero = parse_sysctl_value(stxconfig::sysctl_type_int | writable, "I", "0");
    check(zero.as_number() != nullptr && (*zero.as_number()).bits == 0u && (*zero.as_number()).width == 4u, "kinds",
          "poweroff_delay's 0 is an int 0");
}

}  // namespace

int main() {
    const std::size_t syntax = test_syntax();
    const std::size_t narrow = test_narrow_ranges();
    test_wide_edges();
    test_other_kinds();
    std::printf("exhaustive sysctl syntax: %zu cases; narrow ranges: %zu cases\n", syntax, narrow);
    check(syntax == 496452u && narrow == 1680012u, "counts", "the stated domains were covered");
    return stxtest::finish("test_sysctl_value");
}
