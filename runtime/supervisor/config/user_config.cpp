// user_config.cpp: the user configuration's rules (user_config.h).

#include "user_config.h"


namespace stxconfig {

namespace {

[[nodiscard]] bool c_strings_equal(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

[[nodiscard]] bool starts_with(const char* text, const char* prefix) noexcept {
    std::size_t index = 0;
    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return false;
        }
        index += 1;
    }
    return true;
}

[[nodiscard]] std::size_t c_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

[[nodiscard]] bool is_base64_byte(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '+' || byte == '/';
}

// Whether bytes [begin, end) equal text exactly.
[[nodiscard]] bool span_is(std::span<const char> bytes, std::size_t begin, std::size_t end, const char* text) noexcept {
    const std::size_t length = c_length(text);
    if (end - begin != length) {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        if (bytes[begin + index] != text[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool section_allowed(const char* name) noexcept {
    return c_strings_equal(name, user_section_system) || c_strings_equal(name, user_section_network) ||
           c_strings_equal(name, user_section_access) || c_strings_equal(name, user_section_init) ||
           starts_with(name, service_section_prefix);
}

[[nodiscard]] UserRuleBroken broken(UserRule rule, std::uint32_t line, std::uint32_t column) noexcept {
    UserRuleBroken result{};
    result.rule = rule;
    result.line = line;
    result.column = column;
    return result;
}

// The line of the setting name in the section whose header is on
// section_line; the header's line when there is no such setting.
[[nodiscard]] std::uint32_t setting_line(const BootConfig& config, std::uint32_t section_line,
                                         const char* name) noexcept {
    for (std::size_t section = 0; section < config.section_count(); section += 1) {
        if (config.section(section).line != section_line) {
            continue;
        }
        for (std::size_t index = 0; index < config.setting_count(); index += 1) {
            const Setting& setting = config.setting(index);
            if (setting.section_index == section && c_strings_equal(config.text(setting.name), name)) {
                return setting.line;
            }
        }
    }
    return section_line;
}

}  // namespace

const char* user_rule_name(UserRule rule) noexcept {
    switch (rule) {
    case UserRule::section_not_allowed: return "section_not_allowed";
    case UserRule::command_not_allowed: return "command_not_allowed";
    case UserRule::target_name_taken: return "target_name_taken";
    case UserRule::target_control_not_allowed: return "target_control_not_allowed";
    case UserRule::service_not_target: return "service_not_target";
    case UserRule::target_start_not_allowed: return "target_start_not_allowed";
    case UserRule::access_setting_not_allowed: return "access_setting_not_allowed";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

std::optional<UserRule> user_rule_from_code(std::uint8_t code) noexcept {
    if (code > user_rule_last) {
        return std::nullopt;
    }
    const std::optional<UserRule> rule = static_cast<UserRule>(code);
    return rule;
}

std::optional<std::size_t> access_key_number(const char* name) noexcept {
    const char prefix[] = "ssh-key-";
    std::size_t index = 0;
    while (prefix[index] != '\0') {
        if (name[index] != prefix[index]) {
            return std::nullopt;
        }
        index += 1;
    }
    // One digit 1..8 and nothing after it.
    if (name[index] < '1' || name[index] > '8' || name[index + 1u] != '\0') {
        return std::nullopt;
    }
    const std::optional<std::size_t> number = static_cast<std::size_t>(name[index] - '0');
    return number;
}

bool access_key_value_valid(std::span<const char> value) noexcept {
    // The type: up to the first space.
    std::size_t type_end = 0;
    while (type_end < value.size() && value[type_end] != ' ') {
        type_end += 1;
    }
    bool type_known = false;
    for (const char* const type : ssh_key_types) {
        if (span_is(value, 0, type_end, type)) {
            type_known = true;
        }
    }
    if (!type_known || type_end + 1u >= value.size()) {
        return false;
    }
    // The base64 part: one space after the type, then the alphabet, then at
    // most two '='.
    const std::size_t key_begin = type_end + 1u;
    std::size_t key_end = key_begin;
    while (key_end < value.size() && is_base64_byte(value[key_end])) {
        key_end += 1;
    }
    std::size_t padding = 0;
    while (key_end < value.size() && value[key_end] == '=' && padding < 2u) {
        key_end += 1;
        padding += 1;
    }
    if (key_end - key_begin < access_key_base64_min) {
        return false;
    }
    if (key_end == value.size()) {
        return true;
    }
    // A comment: one space, then printable ASCII, at least one byte.
    if (value[key_end] != ' ' || key_end + 1u == value.size()) {
        return false;
    }
    for (std::size_t index = key_end + 1u; index < value.size(); index += 1) {
        if (value[index] < 0x20 || value[index] > 0x7e) {
            return false;
        }
    }
    return true;
}

std::optional<UserRuleBroken> check_user_config(const BootConfig& user, const BootConfig& factory) noexcept {
    for (std::size_t index = 0; index < user.section_count(); index += 1) {
        const Section& section = user.section(index);
        if (!section_allowed(user.text(section.name))) {
            const std::optional<UserRuleBroken> found = broken(UserRule::section_not_allowed, section.line, 1u);
            return found;
        }
    }
    for (std::size_t index = 0; index < user.init_command_count(); index += 1) {
        const InitCommand& command = user.init_command(index);
        if (command.guard.has_value() || command.action.as_start() == nullptr) {
            const std::optional<UserRuleBroken> found =
                broken(UserRule::command_not_allowed, command.line, command.column);
            return found;
        }
    }
    for (std::size_t index = 0; index < user.service_count(); index += 1) {
        const ServiceDefinition& target = user.service(index);
        if (factory.find_service(user.text(target.name)).has_value()) {
            const std::optional<UserRuleBroken> found = broken(UserRule::target_name_taken, target.line, 1u);
            return found;
        }
        if (target.control != ControlAccess::none) {
            const std::optional<UserRuleBroken> found =
                broken(UserRule::target_control_not_allowed, setting_line(user, target.line, "control"), 1u);
            return found;
        }
        if (!target.target.has_value()) {
            const std::optional<UserRuleBroken> found = broken(UserRule::service_not_target, target.line, 1u);
            return found;
        }
        const std::uint32_t start_line = setting_line(user, target.line, "start");
        if (start_line != target.line) {
            const std::optional<UserRuleBroken> found = broken(UserRule::target_start_not_allowed, start_line, 1u);
            return found;
        }
    }
    for (std::size_t index = 0; index < user.setting_count(); index += 1) {
        const Setting& setting = user.setting(index);
        if (!c_strings_equal(user.text(user.section(setting.section_index).name), user_section_access)) {
            continue;
        }
        const std::span<const char> value(user.text(setting.value), setting.value.length);
        if (!access_key_number(user.text(setting.name)).has_value() || !access_key_value_valid(value)) {
            const std::optional<UserRuleBroken> found = broken(UserRule::access_setting_not_allowed, setting.line, 1u);
            return found;
        }
    }
    return std::nullopt;
}

}  // namespace stxconfig
