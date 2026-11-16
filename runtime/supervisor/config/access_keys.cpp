// access_keys.cpp: the [access] section's SSH keys (access_keys.h).

#include "access_keys.h"

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

}  // namespace

std::optional<std::size_t> access_keys_text(const BootConfig& config, std::span<char> out) noexcept {
    std::size_t used = 0;
    for (std::size_t index = 0; index < config.setting_count(); index += 1) {
        const Setting& setting = config.setting(index);
        const char* const section = config.text(config.section(setting.section_index).name);
        if (!c_strings_equal(section, user_section_access) || !access_key_number(config.text(setting.name)).has_value()) {
            continue;
        }
        const std::span<const char> value(config.text(setting.value), setting.value.length);
        if (!access_key_value_valid(value)) {
            continue;
        }
        if (used + value.size() + 1u > out.size()) {
            return std::nullopt;
        }
        for (std::size_t byte = 0; byte < value.size(); byte += 1) {
            out[used + byte] = value[byte];
        }
        used += value.size();
        out[used] = '\n';
        used += 1;
    }
    const std::optional<std::size_t> written = used;
    return written;
}

}  // namespace stxconfig
