// restart_argument.h: the one argument stxinit gives the supervisor,
// "--restart-count=<decimal>" (runtime/init/stxinit.cpp), parsed.
//
// The argument is input: it is checked for its prefix, for 1 to 20 digits and
// nothing else, and for overflow, and anything else is "no count".

#ifndef STXLIB_RUNTIME_SUPERVISOR_RESTART_ARGUMENT_H
#define STXLIB_RUNTIME_SUPERVISOR_RESTART_ARGUMENT_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace stxsupervisor {

using RestartCount = std::optional<std::uint64_t>;

[[nodiscard]] inline RestartCount parse_restart_count(const char* argument) noexcept {
    constexpr char prefix[] = "--restart-count=";
    constexpr std::size_t prefix_length = sizeof(prefix) - 1;
    constexpr std::size_t digits_max = 20;
    if (argument == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < prefix_length; index += 1) {
        if (argument[index] != prefix[index]) {
            return std::nullopt;
        }
    }
    std::uint64_t value = 0;
    std::size_t digits = 0;
    for (const char* cursor = argument + prefix_length; *cursor != '\0'; cursor += 1) {
        if (*cursor < '0' || *cursor > '9' || digits == digits_max) {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return std::nullopt;
        }
        value = value * 10u + digit;
        digits += 1;
    }
    if (digits == 0) {
        return std::nullopt;
    }
    const RestartCount count = value;
    return count;
}

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_RESTART_ARGUMENT_H
