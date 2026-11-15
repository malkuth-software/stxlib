// device_rules.cpp: see device_rules.h.

#include "device_rules.h"

namespace stxdevices {

namespace {

void copy_pattern(char (&destination)[pattern_size], const char* source) noexcept {
    std::size_t index = 0;
    while (source[index] != '\0' && index + 1u < pattern_size) {
        destination[index] = source[index];
        index += 1;
    }
    destination[index] = '\0';
}

}  // namespace

DevicePlan plan_device_rules(const stxconfig::BootConfig& config) noexcept {
    DevicePlan plan{};
    plan.count = 0;
    DeviceRule hide{};
    hide.action = RuleAction::hide_all;
    plan.rules[plan.count] = hide;
    plan.count += 1;
    for (std::size_t index = 0; index < base_pattern_count; index += 1) {
        DeviceRule show{};
        show.action = RuleAction::show;
        copy_pattern(show.pattern, base_patterns[index]);
        plan.rules[plan.count] = show;
        plan.count += 1;
    }
    for (std::size_t index = 0; index < config.service_count(); index += 1) {
        const stxconfig::ServiceDefinition& definition = config.service(index);
        if (!definition.target.has_value() || (*definition.target).role != stxconfig::TargetRole::foreground) {
            continue;
        }
        const stxconfig::TargetSettings& target = *definition.target;
        for (std::uint32_t device = 0; device < target.device_count && plan.count < rules_max; device += 1) {
            DeviceRule rule{};
            rule.action = RuleAction::show_to_group;
            copy_pattern(rule.pattern, config.text(target.devices[device]));
            rule.group = stxconfig::target_identity(target);
            rule.mode = 0660;
            plan.rules[plan.count] = rule;
            plan.count += 1;
        }
    }
    return plan;
}

}  // namespace stxdevices
