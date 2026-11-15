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
    }
    return std::nullopt;
}

}  // namespace stxconfig
