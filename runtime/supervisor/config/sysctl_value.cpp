// sysctl_value.cpp: see sysctl_value.h.

#include "sysctl_value.h"

#include <utility>

namespace stxconfig {

SysctlValue::SysctlValue(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

SysctlValue SysctlValue::number(const SysctlNumber& value) noexcept {
    const SysctlValue result(Alternatives(std::in_place_type<SysctlNumber>, value));
    return result;
}

SysctlValue SysctlValue::string(const SysctlString& value) noexcept {
    const SysctlValue result(Alternatives(std::in_place_type<SysctlString>, value));
    return result;
}

SysctlValue SysctlValue::refused(SysctlRefusal reason) noexcept {
    const SysctlValue result(Alternatives(std::in_place_type<SysctlRefusal>, reason));
    return result;
}

const SysctlNumber* SysctlValue::as_number() const noexcept {
    return std::get_if<SysctlNumber>(&alternatives_);
}

const SysctlString* SysctlValue::as_string() const noexcept {
    return std::get_if<SysctlString>(&alternatives_);
}

const SysctlRefusal* SysctlValue::as_refused() const noexcept {
    return std::get_if<SysctlRefusal>(&alternatives_);
}

const char* sysctl_refusal_name(SysctlRefusal reason) noexcept {
    switch (reason) {
    case SysctlRefusal::not_settable: return "not_settable";
    case SysctlRefusal::read_only: return "read_only";
    case SysctlRefusal::format_unsupported: return "format_unsupported";
    case SysctlRefusal::empty_number: return "empty_number";
    case SysctlRefusal::malformed_number: return "malformed_number";
    case SysctlRefusal::out_of_range: return "out_of_range";
    default: break;
    }
    // Fallback: a reason outside the set.
    return "unknown";
}

namespace {

// The digit's value in base, or base itself when it is not a digit of base.
[[nodiscard]] std::uint32_t digit_value(char byte, std::uint32_t base) noexcept {
    std::uint32_t value = base;
    if (byte >= '0' && byte <= '9') {
        value = static_cast<std::uint32_t>(byte - '0');
    } else if (byte >= 'a' && byte <= 'f') {
        value = static_cast<std::uint32_t>(byte - 'a') + 10u;
    } else if (byte >= 'A' && byte <= 'F') {
        value = static_cast<std::uint32_t>(byte - 'A') + 10u;
    }
    return value < base ? value : base;
}

// The value's width and signedness for an integer type, or width 0.
struct IntegerType {
    std::uint8_t width;
    bool is_signed;
};

[[nodiscard]] IntegerType integer_type_of(std::uint32_t type) noexcept {
    IntegerType result{0, false};
    switch (type) {
    case sysctl_type_int: result = IntegerType{4, true}; break;
    case sysctl_type_uint: result = IntegerType{4, false}; break;
    case sysctl_type_long: result = IntegerType{8, true}; break;
    case sysctl_type_ulong: result = IntegerType{8, false}; break;
    case sysctl_type_s8: result = IntegerType{1, true}; break;
    case sysctl_type_s16: result = IntegerType{2, true}; break;
    case sysctl_type_s32: result = IntegerType{4, true}; break;
    case sysctl_type_s64: result = IntegerType{8, true}; break;
    case sysctl_type_u8: result = IntegerType{1, false}; break;
    case sysctl_type_u16: result = IntegerType{2, false}; break;
    case sysctl_type_u32: result = IntegerType{4, false}; break;
    case sysctl_type_u64: result = IntegerType{8, false}; break;
    default: break;
    }
    return result;
}

}  // namespace

bool parse_sysctl_magnitude(const char* text, bool allow_minus, std::uint64_t& magnitude, bool& negative) noexcept {
    std::size_t index = 0;
    negative = false;
    magnitude = 0;
    if (text[index] == '+') {
        index += 1;
    } else if (text[index] == '-') {
        if (!allow_minus) {
            return false;
        }
        negative = true;
        index += 1;
    }
    std::uint32_t base = 10;
    if (text[index] == '0' && (text[index + 1u] == 'x' || text[index + 1u] == 'X')) {
        // strtol takes "0x" as a prefix only when a hexadecimal digit
        // follows; "0x" alone reads as 0 and stops at the x, which sysctl(8)
        // then refuses as trailing text. Refused here the same way.
        if (digit_value(text[index + 2u], 16) >= 16u) {
            return false;
        }
        base = 16;
        index += 2;
    } else if (text[index] == '0') {
        base = 8;
    }
    if (digit_value(text[index], base) >= base) {
        return false;
    }
    // Each step consumes one byte of a NUL-terminated text, so the loop ends
    // at its NUL at the latest.
    while (text[index] != '\0') {
        const std::uint32_t digit = digit_value(text[index], base);
        if (digit >= base) {
            return false;
        }
        const std::uint64_t limit = (UINT64_MAX - digit) / base;
        if (magnitude > limit) {
            return false;
        }
        magnitude = magnitude * base + digit;
        index += 1;
    }
    return true;
}

SysctlValue parse_sysctl_value(std::uint32_t kind, const char* format, const char* text) noexcept {
    const std::uint32_t type = kind & sysctl_type_mask;
    if (type == sysctl_type_node || type == sysctl_type_opaque || type == 0u) {
        return SysctlValue::refused(SysctlRefusal::not_settable);
    }
    if ((kind & sysctl_flag_writable) == 0u) {
        return SysctlValue::refused(SysctlRefusal::read_only);
    }
    if (type == sysctl_type_string) {
        std::size_t length = 0;
        while (text[length] != '\0') {
            length += 1;
        }
        return SysctlValue::string(SysctlString{text, length});
    }
    const IntegerType integer = integer_type_of(type);
    if (integer.width == 0u) {
        return SysctlValue::refused(SysctlRefusal::not_settable);
    }
    if (format[0] == 'I' && format[1] == 'K') {
        return SysctlValue::refused(SysctlRefusal::format_unsupported);
    }
    if (text[0] == '\0') {
        return SysctlValue::refused(SysctlRefusal::empty_number);
    }
    std::uint64_t magnitude = 0;
    bool negative = false;
    if (!parse_sysctl_magnitude(text, integer.is_signed, magnitude, negative)) {
        // A magnitude above 2^64 - 1 is out of every range; the syntax is
        // checked again without that limit to tell the two apart.
        std::size_t index = 0;
        bool syntax = true;
        bool any_digit = false;
        if (text[index] == '+' || (text[index] == '-' && integer.is_signed)) {
            index += 1;
        }
        std::uint32_t base = 10;
        if (text[index] == '0' && (text[index + 1u] == 'x' || text[index + 1u] == 'X')) {
            base = 16;
            index += 2;
        } else if (text[index] == '0') {
            base = 8;
        }
        while (text[index] != '\0') {
            if (digit_value(text[index], base) >= base) {
                syntax = false;
                break;
            }
            any_digit = true;
            index += 1;
        }
        return SysctlValue::refused(syntax && any_digit ? SysctlRefusal::out_of_range
                                                        : SysctlRefusal::malformed_number);
    }
    const unsigned bits = static_cast<unsigned>(integer.width) * 8u;
    SysctlNumber number{};
    number.width = integer.width;
    number.is_signed = integer.is_signed;
    if (integer.is_signed) {
        // Range [-2^(bits-1), 2^(bits-1) - 1].
        const std::uint64_t positive_max = (std::uint64_t{1} << (bits - 1u)) - 1u;
        const std::uint64_t negative_max = positive_max + 1u;
        if ((!negative && magnitude > positive_max) || (negative && magnitude > negative_max)) {
            return SysctlValue::refused(SysctlRefusal::out_of_range);
        }
        number.bits = negative ? (~magnitude + 1u) : magnitude;
    } else {
        const std::uint64_t maximum = bits == 64u ? UINT64_MAX : (std::uint64_t{1} << bits) - 1u;
        if (magnitude > maximum) {
            return SysctlValue::refused(SysctlRefusal::out_of_range);
        }
        number.bits = magnitude;
    }
    if (bits < 64u) {
        number.bits &= (std::uint64_t{1} << bits) - 1u;
    }
    return SysctlValue::number(number);
}

}  // namespace stxconfig
