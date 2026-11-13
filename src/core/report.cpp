#include "core/report.hpp"

#include "hosted/host.hpp"

#include <charconv>

namespace stxlib::report {

void c_text(const char* const text, const usize limit) noexcept {
    if (text == nullptr) return;
    usize length = 0;
    while (length < limit && text[length] != '\0') ++length;
    host::write_standard_error(text, length);
}

void text(const TextView text) noexcept { host::write_standard_error(text.data(), text.count()); }

// 20 digits hold 2^64 - 1; a sign makes 21.
void unsigned_number(const u64 value) noexcept {
    char digits[24]{};
    const std::to_chars_result written = std::to_chars(digits, digits + sizeof(digits), value);
    const usize count = static_cast<usize>(written.ptr - digits);
    host::write_standard_error(digits, count);
}

void signed_number(const i64 value) noexcept {
    char digits[24]{};
    const std::to_chars_result written = std::to_chars(digits, digits + sizeof(digits), value);
    const usize count = static_cast<usize>(written.ptr - digits);
    host::write_standard_error(digits, count);
}

void hexadecimal(const u64 value) noexcept {
    char digits[24]{};
    digits[0] = '0';
    digits[1] = 'x';
    const std::to_chars_result written = std::to_chars(digits + 2, digits + sizeof(digits), value, 16);
    const usize count = static_cast<usize>(written.ptr - digits);
    host::write_standard_error(digits, count);
}

} // namespace stxlib::report
