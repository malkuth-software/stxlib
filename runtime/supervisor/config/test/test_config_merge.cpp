// test_config_merge.cpp: the boot's configuration choice and merge
// (config_merge.h), with the user-configuration rules 5 to 7
// (user_config.h) and the [access] keys (access_keys.h).
//
// What is exhaustive here, and over what:
// - merge_config_text and choose_boot_config over every combination of
//   4 factory texts (with or without [system], with or without [network])
//   and user texts made of: [system] absent, overriding hostname, or adding
//   a setting (3); [network] absent or static (2); [access] absent or one
//   key (2); targets: none, a, a+b, each enabled or not (7): 336 pairs.
//   For each: the merged text parses; the factory's services and init
//   section are kept whole; each setting comes from the side the merge
//   rule says; each user target is a target whose start policy is boot
//   exactly when the user's [init] starts it.
// - access_key_number over "ssh-key-" followed by every string of 0 to 2
//   bytes over "0189a-" (43 names), against a reference.
// - access_key_value_valid over every type in a list of 9 (the 6 known
//   and 3 near misses) followed by " ", 15 'A's and every string of 0 to 7
//   bytes over "A= !\x7f" (9 x 97,656 = 878,904 values), against a
//   reference written from access_keys.h's description.
// Examples (sampled): the fallback order (newest refused, older taken;
// both refused, the factory alone; a clash only the merged text shows),
// rules 5 to 7, and access_keys_text.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "../access_keys.h"
#include "../boot_config.h"
#include "../config_merge.h"
#include "../user_config.h"
#include "test_support.h"

namespace {

using stxconfig::BootConfig;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

BootConfig factory;
BootConfig user;
BootConfig effective;
BootConfig empty_factory;
char merged[stxconfig::config_size_max];

// A text assembled from parts into a fixed buffer.
struct Text {
    char bytes[4096];
    std::size_t length;
};

void append(Text& text, const char* part) {
    const std::size_t length = stxtest::c_length(part);
    std::memcpy(&text.bytes[text.length], part, length);
    text.length += length;
}

[[nodiscard]] std::span<const char> span_of(const Text& text) {
    return std::span<const char>(text.bytes, text.length);
}

[[nodiscard]] const char* setting_or_empty(const BootConfig& config, const char* section, const char* name) {
    const std::optional<stxconfig::TextRef> value = config.find_setting(section, name);
    return value.has_value() ? config.text(*value) : "";
}

const char* const key_text = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIexample owner@laptop";

void test_merge_combinations() {
    const char* const test = "merge combinations";
    std::size_t cases = 0;
    for (std::size_t factory_case = 0; factory_case < 4u; factory_case += 1) {
        const bool factory_system = (factory_case & 1u) != 0u;
        const bool factory_network = (factory_case & 2u) != 0u;
        Text factory_text{};
        append(factory_text, "# factory\n");
        if (factory_system) {
            append(factory_text, "[system]\nhostname = stx\nupdate-url = https://u.example/latest\n");
        }
        if (factory_network) {
            append(factory_text, "[network]\nmode = dhcp\n\n");
        }
        append(factory_text, "[init]\nstart sshd\n[service-sshd]\ncommand = /usr/sbin/sshd -D\nrestart = always\n");
        if (stxconfig::parse_boot_config(span_of(factory_text), factory).as_failure() != nullptr) {
            check(false, test, "factory parses");
            return;
        }
        for (std::size_t system_case = 0; system_case < 3u; system_case += 1) {
            for (std::size_t network_case = 0; network_case < 2u; network_case += 1) {
                for (std::size_t access_case = 0; access_case < 2u; access_case += 1) {
                    for (std::size_t target_case = 0; target_case < 7u; target_case += 1) {
                        // target_case: 0 none; 1-2 a (disabled, enabled);
                        // 3-6 a and b, enabled bits.
                        const bool has_a = target_case >= 1u;
                        const bool has_b = target_case >= 3u;
                        const bool a_enabled = target_case == 2u || target_case == 4u || target_case == 6u;
                        const bool b_enabled = target_case == 5u || target_case == 6u;
                        Text user_text{};
                        append(user_text, "# user\n");
                        if (system_case == 1u) {
                            append(user_text, "[system]\nhostname = box\n");
                        } else if (system_case == 2u) {
                            append(user_text, "[system]\nlocation = shed\n");
                        }
                        if (network_case == 1u) {
                            append(user_text,
                                   "[network]\nmode = static\naddress = 192.168.1.20/24\ngateway = 192.168.1.1\n");
                        }
                        if (access_case == 1u) {
                            append(user_text, "[access]\nssh-key-1 = ");
                            append(user_text, key_text);
                            append(user_text, "\n");
                        }
                        if (has_a) {
                            append(user_text,
                                   "[service-a]\ncommand = bin/a\nrole = server\nid = 1\nfolder = /data/targets/a\n");
                        }
                        if (has_b) {
                            append(user_text,
                                   "[service-b]\nrole = foreground\nid = 2\ncommand = bin/b\nfolder = /data/targets/b\n");
                        }
                        if (a_enabled || b_enabled) {
                            append(user_text, "[init]\n");
                            append(user_text, a_enabled ? "start a\n" : "");
                            append(user_text, b_enabled ? "start b\n" : "");
                        }
                        cases += 1;
                        const std::optional<stxstore::TurnView> turns[2] = {
                            stxstore::TurnView{7u, span_of(user_text)}, std::nullopt};
                        const stxstore::TurnChoice choice{0u, std::nullopt, 1u, 8u};
                        const std::optional<stxconfig::BootChoice> chosen = stxconfig::choose_boot_config(
                            span_of(factory_text), factory, turns, choice, user, effective,
                            std::span<char>(merged, sizeof(merged)));
                        const bool taken = chosen.has_value() && (*chosen).file_index.has_value() &&
                                           *(*chosen).file_index == 0u && (*chosen).sequence == 7u;
                        check(taken, test, "the turn is taken");
                        if (!taken) {
                            std::printf("  case factory=%zu system=%zu network=%zu access=%zu targets=%zu\n",
                                        factory_case, system_case, network_case, access_case, target_case);
                            continue;
                        }
                        // The factory's service and init section, whole.
                        check(effective.find_service("sshd").has_value() && effective.init_command_count() == 1u,
                              test, "factory service and init kept");
                        // [system].
                        const char* const hostname = setting_or_empty(effective, "system", "hostname");
                        const char* const expected_hostname =
                            system_case == 1u ? "box" : (factory_system ? "stx" : "");
                        check(same_text(hostname, expected_hostname), test, "hostname from the right side");
                        check(same_text(setting_or_empty(effective, "system", "update-url"),
                                        factory_system ? "https://u.example/latest" : ""),
                              test, "a factory setting the user does not set is kept");
                        check(same_text(setting_or_empty(effective, "system", "location"),
                                        system_case == 2u ? "shed" : ""),
                              test, "a user setting is added");
                        // [network]: the user's replaces the factory's.
                        const char* const mode = setting_or_empty(effective, "network", "mode");
                        const char* const expected_mode =
                            network_case == 1u ? "static" : (factory_network ? "dhcp" : "");
                        check(same_text(mode, expected_mode), test, "network from the right side");
                        // [access].
                        check(same_text(setting_or_empty(effective, "access", "ssh-key-1"),
                                        access_case == 1u ? key_text : ""),
                              test, "access keys from the user");
                        // Targets and their start policies.
                        const std::size_t expected_services = 1u + (has_a ? 1u : 0u) + (has_b ? 1u : 0u);
                        check(effective.service_count() == expected_services, test, "service count");
                        const char* const names[2] = {"a", "b"};
                        const bool present[2] = {has_a, has_b};
                        const bool enabled[2] = {a_enabled, b_enabled};
                        for (std::size_t index = 0; index < 2u; index += 1) {
                            const std::optional<std::size_t> found = effective.find_service(names[index]);
                            check(found.has_value() == present[index], test, "target present as written");
                            if (!found.has_value()) {
                                continue;
                            }
                            const stxconfig::ServiceDefinition& target = effective.service(*found);
                            check(target.target.has_value() && target.control == stxconfig::ControlAccess::none,
                                  test, "a target without control");
                            if (target.target.has_value()) {
                                const stxconfig::StartPolicy policy = (*target.target).start;
                                check(policy == (enabled[index] ? stxconfig::StartPolicy::boot
                                                                : stxconfig::StartPolicy::manual),
                                      test, "start policy from the user's init");
                            }
                        }
                    }
                }
            }
        }
    }
    check(cases == 336u, test, "336 pairs");
}

const char* const factory_with_target =
    "[init]\nstart sshd\n[service-sshd]\ncommand = /usr/sbin/sshd -D\n"
    "[target-clock]\nrole = server\nid = 5\ncommand = bin/clock\nfolder = /data/targets/clock\n";

void test_fallbacks() {
    const char* const test = "fallbacks";
    if (stxconfig::parse_boot_config(text_of(factory_with_target), factory).as_failure() != nullptr) {
        check(false, test, "factory parses");
        return;
    }
    const char* const bad_parse = "[system\n";
    const char* const clash =
        "[service-a]\ncommand = bin/a\nrole = server\nid = 5\nfolder = /data/targets/a\n";
    const char* const good =
        "[service-a]\ncommand = bin/a\nrole = server\nid = 6\nfolder = /data/targets/a\n[init]\nstart a\n";
    const std::span<char> out(merged, sizeof(merged));
    // Newest refused (does not parse), older taken.
    {
        const std::optional<stxstore::TurnView> turns[2] = {stxstore::TurnView{3u, text_of(good)},
                                                            stxstore::TurnView{4u, text_of(bad_parse)}};
        const stxstore::TurnChoice choice{1u, 0u, 0u, 5u};
        const std::optional<stxconfig::BootChoice> chosen = stxconfig::choose_boot_config(
            text_of(factory_with_target), factory, turns, choice, user, effective, out);
        check(chosen.has_value() && (*chosen).file_index.has_value() && *(*chosen).file_index == 0u &&
                  (*chosen).passed_over_count == 1u &&
                  (*chosen).passed_over[0].refusal == stxconfig::TurnRefusal::parse &&
                  (*chosen).passed_over[0].sequence == 4u && effective.find_service("a").has_value(),
              test, "newest does not parse: older taken");
    }
    // A clash only the merged text shows (target id 5 twice), then nothing
    // older: the factory alone.
    {
        const std::optional<stxstore::TurnView> turns[2] = {stxstore::TurnView{9u, text_of(clash)}, std::nullopt};
        const stxstore::TurnChoice choice{0u, std::nullopt, 1u, 10u};
        const std::optional<stxconfig::BootChoice> chosen = stxconfig::choose_boot_config(
            text_of(factory_with_target), factory, turns, choice, user, effective, out);
        check(chosen.has_value() && !(*chosen).file_index.has_value() && (*chosen).passed_over_count == 1u &&
                  (*chosen).passed_over[0].refusal == stxconfig::TurnRefusal::merged_parse &&
                  (*chosen).passed_over[0].code ==
                      static_cast<std::uint8_t>(stxconfig::ParseError::target_id_repeated) &&
                  !effective.find_service("a").has_value() && effective.find_service("clock").has_value(),
              test, "a merged clash: the factory alone");
    }
    // A rule broken (a root service in the user configuration), and no
    // turns at all.
    {
        const std::optional<stxstore::TurnView> turns[2] = {
            stxstore::TurnView{2u, text_of("[service-x]\ncommand = /bin/sh\n")}, std::nullopt};
        const stxstore::TurnChoice choice{0u, std::nullopt, 1u, 3u};
        const std::optional<stxconfig::BootChoice> chosen = stxconfig::choose_boot_config(
            text_of(factory_with_target), factory, turns, choice, user, effective, out);
        check(chosen.has_value() && !(*chosen).file_index.has_value() &&
                  (*chosen).passed_over[0].refusal == stxconfig::TurnRefusal::rule &&
                  (*chosen).passed_over[0].code == static_cast<std::uint8_t>(stxconfig::UserRule::service_not_target),
              test, "a user service refused by rule 5");
        const std::optional<stxstore::TurnView> none[2] = {std::nullopt, std::nullopt};
        const stxstore::TurnChoice empty{std::nullopt, std::nullopt, 0u, 1u};
        const std::optional<stxconfig::BootChoice> alone = stxconfig::choose_boot_config(
            text_of(factory_with_target), factory, none, empty, user, effective, out);
        check(alone.has_value() && !(*alone).file_index.has_value() && (*alone).passed_over_count == 0u &&
                  (*alone).text_length == stxtest::c_length(factory_with_target),
              test, "no turns: the factory alone");
    }
}

[[nodiscard]] std::optional<stxconfig::UserRule> rule_of(const char* text) {
    if (stxconfig::parse_boot_config(text_of(text), user).as_failure() != nullptr) {
        return std::nullopt;
    }
    const std::optional<stxconfig::UserRuleBroken> broken = stxconfig::check_user_config(user, empty_factory);
    if (!broken.has_value()) {
        return std::nullopt;
    }
    const std::optional<stxconfig::UserRule> rule = (*broken).rule;
    return rule;
}

void test_rules() {
    const char* const test = "user rules 5 to 7";
    check(rule_of("[service-x]\ncommand = /bin/sh\n") == stxconfig::UserRule::service_not_target, test,
          "a service is refused");
    check(rule_of("[service-x]\ncommand = bin/x\nrole = server\nid = 1\nfolder = /data/targets/x\nstart = boot\n") ==
              stxconfig::UserRule::target_start_not_allowed,
          test, "a start setting is refused");
    check(!rule_of("[service-x]\ncommand = bin/x\nrole = server\nid = 1\nfolder = /data/targets/x\n").has_value(),
          test, "a target passes");
    check(rule_of("[access]\nssh-key-9 = ssh-ed25519 AAAAAAAAAAAAAAAAAAAA\n") ==
              stxconfig::UserRule::access_setting_not_allowed,
          test, "ssh-key-9 is refused");
    check(rule_of("[access]\nssh-key-1 = ssh-rsa AAAAAAAAAAAAAAAAAAAA\n") ==
              stxconfig::UserRule::access_setting_not_allowed,
          test, "an unknown key type is refused");
    check(rule_of("[access]\nssh-key-1 = command=\"/bin/sh\" ssh-ed25519 AAAAAAAAAAAAAAAAAAAA\n") ==
              stxconfig::UserRule::access_setting_not_allowed,
          test, "options are refused");
    check(!rule_of("[access]\nssh-key-8 = ssh-ed25519 AAAAAAAAAAAAAAAAAAAA a b\n").has_value(), test,
          "a key with a comment passes");
    check(same_text(stxconfig::user_rule_name(stxconfig::UserRule::access_setting_not_allowed),
                    "access_setting_not_allowed") &&
              stxconfig::user_rule_from_code(6u) == stxconfig::UserRule::access_setting_not_allowed &&
              !stxconfig::user_rule_from_code(7u).has_value(),
          test, "names and codes");
}

void test_access_keys_text() {
    const char* const test = "access keys text";
    const char* const text =
        "[access]\nssh-key-2 = ssh-ed25519 AAAAAAAAAAAAAAAAAAAA second\nssh-key-1 = ecdsa-sha2-nistp256 "
        "BBBBBBBBBBBBBBBB==\n[system]\nssh-key-3 = ssh-ed25519 CCCCCCCCCCCCCCCCCCCC\n";
    if (stxconfig::parse_boot_config(text_of(text), user).as_failure() != nullptr) {
        check(false, test, "parses");
        return;
    }
    char out[stxconfig::access_keys_text_max];
    const std::optional<std::size_t> length = stxconfig::access_keys_text(user, std::span<char>(out, sizeof(out)));
    const char* const expected =
        "ssh-ed25519 AAAAAAAAAAAAAAAAAAAA second\necdsa-sha2-nistp256 BBBBBBBBBBBBBBBB==\n";
    check(length.has_value() && *length == stxtest::c_length(expected) && std::memcmp(out, expected, *length) == 0,
          test, "the [access] keys in setting order, nothing from other sections");
    const std::optional<std::size_t> small = stxconfig::access_keys_text(user, std::span<char>(out, 10));
    check(!small.has_value(), test, "too small an output refused");
}

// References written from the descriptions, independent of the code.
[[nodiscard]] bool reference_number_valid(const char* suffix) {
    return std::strlen(suffix) == 1u && suffix[0] >= '1' && suffix[0] <= '8';
}

void test_key_numbers() {
    const char* const test = "access key numbers";
    const char alphabet[] = {'0', '1', '8', '9', 'a', '-'};
    std::size_t names = 0;
    for (std::size_t length = 0; length <= 2u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= sizeof(alphabet);
        }
        for (std::size_t index = 0; index < total; index += 1) {
            char name[16] = "ssh-key-";
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                name[8u + position] = alphabet[rest % sizeof(alphabet)];
                rest /= sizeof(alphabet);
            }
            name[8u + length] = '\0';
            const std::optional<std::size_t> number = stxconfig::access_key_number(name);
            const bool expected = reference_number_valid(&name[8]);
            check(number.has_value() == expected, test, name);
            if (number.has_value()) {
                check(*number == static_cast<std::size_t>(name[8] - '0'), test, "the number");
            }
            names += 1;
        }
    }
    check(!stxconfig::access_key_number("ssh-key").has_value() && !stxconfig::access_key_number("ssh-kex-1").has_value(),
          test, "prefix required");
    check(names == 43u, test, "43 names");
}

[[nodiscard]] bool reference_value_valid(const char* value, std::size_t length) {
    const char* const types[] = {"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
                                 "sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com"};
    // Split at the first and second space.
    const char* const first_space = static_cast<const char*>(std::memchr(value, ' ', length));
    if (first_space == nullptr) {
        return false;
    }
    const std::size_t type_length = static_cast<std::size_t>(first_space - value);
    bool known = false;
    for (const char* const type : types) {
        known = known || (std::strlen(type) == type_length && std::memcmp(type, value, type_length) == 0);
    }
    if (!known) {
        return false;
    }
    const char* const key = first_space + 1;
    const std::size_t rest_length = length - type_length - 1u;
    const char* const second_space = static_cast<const char*>(std::memchr(key, ' ', rest_length));
    const std::size_t key_length = second_space == nullptr ? rest_length : static_cast<std::size_t>(second_space - key);
    // The key: base64 bytes, then '=' only, at most two, 16 bytes at least.
    std::size_t padding = 0;
    for (std::size_t index = 0; index < key_length; index += 1) {
        const char byte = key[index];
        const bool base64 = std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '+' || byte == '/';
        if (byte == '=') {
            padding += 1;
        } else if (!base64 || padding != 0u) {
            return false;
        }
    }
    if (padding > 2u || key_length < 16u) {
        return false;
    }
    if (second_space == nullptr) {
        return true;
    }
    const std::size_t comment_length = rest_length - key_length - 1u;
    if (comment_length == 0u) {
        return false;
    }
    for (std::size_t index = 0; index < comment_length; index += 1) {
        const unsigned char byte = static_cast<unsigned char>(second_space[1 + index]);
        if (byte < 0x20u || byte > 0x7eu) {
            return false;
        }
    }
    return true;
}

void test_key_values() {
    const char* const test = "access key values";
    const char* const types[] = {"ssh-ed25519",
                                 "ecdsa-sha2-nistp256",
                                 "ecdsa-sha2-nistp384",
                                 "ecdsa-sha2-nistp521",
                                 "sk-ssh-ed25519@openssh.com",
                                 "sk-ecdsa-sha2-nistp256@openssh.com",
                                 "ssh-rsa",
                                 "ssh-ed2551",
                                 ""};
    const char alphabet[] = {'A', '=', ' ', '!', '\x7f'};
    std::size_t values = 0;
    std::size_t disagreements = 0;
    for (const char* const type : types) {
        for (std::size_t length = 0; length <= 7u; length += 1) {
            std::size_t total = 1;
            for (std::size_t position = 0; position < length; position += 1) {
                total *= sizeof(alphabet);
            }
            for (std::size_t index = 0; index < total; index += 1) {
                char value[96];
                std::size_t used = 0;
                const std::size_t type_length = std::strlen(type);
                std::memcpy(value, type, type_length);
                used += type_length;
                value[used] = ' ';
                used += 1;
                std::memset(&value[used], 'A', 15u);
                used += 15u;
                std::size_t rest = index;
                for (std::size_t position = 0; position < length; position += 1) {
                    value[used] = alphabet[rest % sizeof(alphabet)];
                    used += 1;
                    rest /= sizeof(alphabet);
                }
                const bool got = stxconfig::access_key_value_valid(std::span<const char>(value, used));
                if (got != reference_value_valid(value, used)) {
                    disagreements += 1;
                }
                values += 1;
            }
        }
    }
    check(disagreements == 0u, test, "the code agrees with the reference on every value");
    check(values == 9u * 97656u, test, "878,904 values");
}

}  // namespace

int main() {
    test_merge_combinations();
    test_fallbacks();
    test_rules();
    test_access_keys_text();
    test_key_numbers();
    test_key_values();
    return stxtest::finish("test_config_merge");
}
