// sysctl_value.h: reading a sysctl command's value by the variable's type
// (C2; the init command "sysctl <name>=<value>", boot_config.h).
//
// Pure code: the system side (system_actions.cpp) asks the kernel for the
// variable's kind and format (the OIDFMT query sbin/sysctl makes,
// sbin/sysctl/sysctl.c:980-1010 at fb9033d2876d) and passes them here; this
// decides what to write. Tested on the host (test/test_sysctl_value.cpp)
// exhaustively over bounded domains, against the C library's strtoll and
// strtoull as the reference for the number syntax sysctl(8) accepts.
//
// What is accepted, and where it differs from sysctl(8) (sysctl.c:291-387,
// 520-566):
// - The kinds sysctl(8) sets: the integer kinds and strings. A node, an
//   opaque variable or an unknown kind is refused (as sysctl(8) refuses).
// - A variable that is not writable is refused (sysctl(8) likewise).
// - Strings: the value as written, its length without a NUL (sysctl(8)
//   passes strlen).
// - Numbers: the syntax of strtol with base 0, as sysctl(8): an optional
//   sign, then "0x" or "0X" and hexadecimal digits, or "0" and octal
//   digits, or decimal digits. Stricter than sysctl(8) in four ways, each
//   refusing what it would accept with another meaning:
//   1. A value outside the variable's type is refused; sysctl(8) casts it
//      (an int8_t given 300 gets 44).
//   2. A minus sign is refused for an unsigned type; strtoul negates (an
//      unsigned int given -1 gets 4294967295).
//   3. Leading blanks are refused (strtol skips them; a config word cannot
//      hold them unless quoted, and then they are a mistake).
//   4. One value only: sysctl(8) also takes a list separated by blanks or
//      commas for array variables; a list is refused here.
//   And an empty value is refused for a number, as sysctl(8) does.
// - Integer formats with a unit suffix ("IK": tenths of a kelvin, which
//   sysctl(8) reads as "20.5C" and the like) are refused, not converted.
//
// The integer kinds' widths are FreeBSD arm64's (LP64): int and unsigned
// int 4 bytes, long and unsigned long 8.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSCTL_VALUE_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSCTL_VALUE_H

#include <cstddef>
#include <cstdint>
#include <variant>

namespace stxconfig {

// The kernel's kind word: the type in its low four bits (sys/sys/sysctl.h:
// 63-79), and CTLFLAG_WR (sysctl.h:82). Spelled here so that this pure code
// needs no system header; the system side checks them against the header.
constexpr std::uint32_t sysctl_type_mask = 0xfu;
constexpr std::uint32_t sysctl_flag_writable = 0x40000000u;
constexpr std::uint32_t sysctl_type_node = 1;
constexpr std::uint32_t sysctl_type_int = 2;
constexpr std::uint32_t sysctl_type_string = 3;
constexpr std::uint32_t sysctl_type_s64 = 4;
constexpr std::uint32_t sysctl_type_opaque = 5;
constexpr std::uint32_t sysctl_type_uint = 6;
constexpr std::uint32_t sysctl_type_long = 7;
constexpr std::uint32_t sysctl_type_ulong = 8;
constexpr std::uint32_t sysctl_type_u64 = 9;
constexpr std::uint32_t sysctl_type_u8 = 0xa;
constexpr std::uint32_t sysctl_type_u16 = 0xb;
constexpr std::uint32_t sysctl_type_s8 = 0xc;
constexpr std::uint32_t sysctl_type_s16 = 0xd;
constexpr std::uint32_t sysctl_type_s32 = 0xe;
constexpr std::uint32_t sysctl_type_u32 = 0xf;

// A number to write: width bytes (1, 2, 4 or 8) of the two's-complement
// value in bits, in the host's byte order when the system side stores it.
struct SysctlNumber {
    std::uint64_t bits;
    std::uint8_t width;
    bool is_signed;
};

// A string to write: the value's bytes, borrowed from the caller's text.
struct SysctlString {
    const char* bytes;
    std::size_t length;
};

enum class SysctlRefusal : std::uint8_t {
    not_settable,    // a node, an opaque variable or an unknown kind
    read_only,       // the variable is not writable
    format_unsupported,  // an integer with a unit suffix format ("IK...")
    empty_number,    // an empty value for a number
    malformed_number,  // not a number in the accepted syntax
    out_of_range,    // a number outside the variable's type
};

class SysctlValue {
public:
    [[nodiscard]] static SysctlValue number(const SysctlNumber& value) noexcept;
    [[nodiscard]] static SysctlValue string(const SysctlString& value) noexcept;
    [[nodiscard]] static SysctlValue refused(SysctlRefusal reason) noexcept;

    [[nodiscard]] const SysctlNumber* as_number() const noexcept;
    [[nodiscard]] const SysctlString* as_string() const noexcept;
    [[nodiscard]] const SysctlRefusal* as_refused() const noexcept;

private:
    using Alternatives = std::variant<SysctlRefusal, SysctlNumber, SysctlString>;
    explicit SysctlValue(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// The refusal's name as printed ("out_of_range", ...).
[[nodiscard]] const char* sysctl_refusal_name(SysctlRefusal reason) noexcept;

// What to write for text (NUL-terminated) to a variable of kind (the
// kernel's kind word) and format (the kernel's format string, NUL-
// terminated, possibly empty).
[[nodiscard]] SysctlValue parse_sysctl_value(std::uint32_t kind, const char* format, const char* text) noexcept;

// The number syntax alone, for the tests: text read as strtol with base 0
// would read it, with the rules above (no blanks, the whole text, the
// sign only when signed). Fills magnitude and negative and returns true,
// or returns false (malformed, or a magnitude above 2^64 - 1).
[[nodiscard]] bool parse_sysctl_magnitude(const char* text, bool allow_minus, std::uint64_t& magnitude,
                                          bool& negative) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSCTL_VALUE_H
