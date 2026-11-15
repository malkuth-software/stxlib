// test_boot_config.cpp: host tests of the boot configuration parser.
//
// Three kinds of test:
// - examples: a full configuration, read back field by field;
// - refusals: one input per error, with its line and column, and every
//   limit at and one past its bound;
// - bounded exhaustive runs: every input up to a stated length over two
//   alphabets (bytes, and init-section tokens), each checked against the
//   parser's invariants (check_invariants). Within those bounds the
//   invariants hold for every input, not a sample; memory errors are what
//   the sanitizers detect, which is evidence, not proof.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "../boot_config.h"
#include "test_support.h"

namespace {

using stxconfig::BootConfig;
using stxconfig::ParseError;
using stxconfig::ParseFailed;
using stxconfig::ParseOutcome;
using stxconfig::TextRef;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

BootConfig config;

// ---------------------------------------------------------------------------
// Invariants of every parse.

[[nodiscard]] bool text_ref_valid(const BootConfig& parsed, const TextRef& reference) {
    const std::size_t end = static_cast<std::size_t>(reference.offset) + reference.length;
    if (end >= parsed.text_store_used()) {
        return false;
    }
    const char* const text = parsed.text(reference);
    return std::strlen(text) == reference.length;
}

[[nodiscard]] bool command_valid(const BootConfig& parsed, const stxconfig::InitCommand& command) {
    if (command.guard.has_value() && !text_ref_valid(parsed, (*command.guard).path)) {
        return false;
    }
    const stxconfig::CommandAction& action = command.action;
    const stxconfig::MountCommand* const mount = action.as_mount();
    if (mount != nullptr) {
        return text_ref_valid(parsed, (*mount).device) && text_ref_valid(parsed, (*mount).directory);
    }
    const stxconfig::WaitForCommand* const wait_for = action.as_wait_for();
    if (wait_for != nullptr) {
        return (*wait_for).timeout_ms <= stxconfig::wait_ms_max && text_ref_valid(parsed, (*wait_for).path);
    }
    const stxconfig::SetCommand* const set = action.as_set();
    if (set != nullptr) {
        return text_ref_valid(parsed, (*set).name) && (*set).name.length >= 1u && text_ref_valid(parsed, (*set).value);
    }
    const stxconfig::StartCommand* const start = action.as_start();
    if (start != nullptr) {
        return text_ref_valid(parsed, (*start).service) && (*start).service.length >= 1u;
    }
    const stxconfig::RunCommand* const run = action.as_run();
    if (run != nullptr) {
        if ((*run).argument_count < 1u || (*run).argument_count > stxconfig::run_arguments_max) {
            return false;
        }
        for (std::uint32_t index = 0; index < (*run).argument_count; index += 1) {
            if (!text_ref_valid(parsed, (*run).arguments[index])) {
                return false;
            }
        }
        return parsed.text((*run).arguments[0])[0] == '/';
    }
    const stxconfig::SysctlCommand* const sysctl = action.as_sysctl();
    if (sysctl != nullptr) {
        return text_ref_valid(parsed, (*sysctl).setting) && (*sysctl).name_length >= 1u &&
               (*sysctl).name_length < (*sysctl).setting.length &&
               parsed.text((*sysctl).setting)[(*sysctl).name_length] == '=';
    }
    return false;
}

// Success: counts within limits, every stored text inside the used store
// and NUL-terminated at its length, every command well-formed. Failure: the
// configuration is empty and the position lies inside the input.
void check_invariants(const char* test, std::span<const char> input, const ParseOutcome& outcome) {
    const ParseFailed* const failure = outcome.as_failure();
    if (failure != nullptr) {
        std::size_t lines = 1;
        for (std::size_t index = 0; index < input.size(); index += 1) {
            if (input[index] == '\n') {
                lines += 1;
            }
        }
        check(config.section_count() == 0 && config.setting_count() == 0 && config.init_command_count() == 0 &&
                  !config.has_init_section() && config.text_store_used() == 0,
              test, "a refused file leaves the configuration empty");
        check((*failure).line >= 1u && (*failure).line <= lines, test, "refusal line inside the input");
        check((*failure).column >= 1u && (*failure).column <= stxconfig::line_length_max + 1u, test,
              "refusal column inside a line");
        return;
    }
    check(config.text_store_used() <= stxconfig::text_store_bytes, test, "text store within its size");
    check(config.text_store_used() <= 2u * (input.size() + 1u), test, "text store within two copies of the input");
    check(config.service_count() <= stxconfig::services_max, test, "services within limit");
    for (std::size_t index = 0; index < config.service_count(); index += 1) {
        const stxconfig::ServiceDefinition& service = config.service(index);
        bool valid = text_ref_valid(config, service.name) && service.argument_count >= 1u &&
                     service.argument_count <= stxconfig::run_arguments_max;
        for (std::uint32_t argument = 0; valid && argument < service.argument_count; argument += 1) {
            valid = text_ref_valid(config, service.arguments[argument]);
        }
        valid = valid && config.text(service.arguments[0])[0] == '/';
        valid = valid && (!service.directory.has_value() || text_ref_valid(config, *service.directory));
        valid = valid && (!service.folder.has_value() || text_ref_valid(config, *service.folder));
        valid = valid && !(service.folder.has_value() && service.directory.has_value());
        valid = valid && service.device_count <= stxconfig::devices_max;
        valid = valid && (service.folder.has_value() || (service.device_count == 0u &&
                                                         service.network == stxconfig::NetworkAccess::none));
        for (std::uint32_t device = 0; valid && device < service.device_count; device += 1) {
            valid = text_ref_valid(config, service.devices[device]) && service.devices[device].length >= 1u &&
                    service.devices[device].length <= stxconfig::device_pattern_length_max;
        }
        check(valid, test, "service well-formed");
    }
    for (std::size_t index = 0; index < config.init_command_count(); index += 1) {
        const stxconfig::StartCommand* const start = config.init_command(index).action.as_start();
        check(start == nullptr || config.find_service(config.text((*start).service)).has_value(), test,
              "every start names a service");
    }
    check(config.section_count() <= stxconfig::sections_max && config.setting_count() <= stxconfig::settings_max &&
              config.init_command_count() <= stxconfig::init_commands_max,
          test, "counts within limits");
    for (std::size_t index = 0; index < config.section_count(); index += 1) {
        check(text_ref_valid(config, config.section(index).name), test, "section name stored");
    }
    for (std::size_t index = 0; index < config.setting_count(); index += 1) {
        const stxconfig::Setting& setting = config.setting(index);
        check(text_ref_valid(config, setting.name) && text_ref_valid(config, setting.value) &&
                  setting.section_index < config.section_count(),
              test, "setting stored");
    }
    for (std::size_t index = 0; index < config.init_command_count(); index += 1) {
        check(command_valid(config, config.init_command(index)), test, "command well-formed");
    }
}

[[nodiscard]] ParseOutcome parse_checked(const char* test, std::span<const char> input) {
    const ParseOutcome outcome = stxconfig::parse_boot_config(input, config);
    check_invariants(test, input, outcome);
    return outcome;
}

// ---------------------------------------------------------------------------
// Examples.

void test_full_example() {
    const char* const test = "full example";
    const char* const text =
        "# appliance configuration\n"
        "[system]\n"
        "hostname = stx-vm  \n"
        "empty =\n"
        "\n"
        "[init]\n"
        "  mount ufs /dev/gpt/user /user rw\n"
        "wait-for device /dev/gpt/user 2000\n"
        "if-present file /user/autorun run /user/autorun\n"
        "set LANG C.UTF-8\n"
        "start web\n"
        "run /sbin/thing \"an \\\"argument\\\"\" second \"\"\n"
        "mount msdosfs /dev/msdosfs/BOOT /boot/efi ro\n"
        "[service-web]\n"
        "command = /usr/local/sbin/webd --port 8443 \"a b\"\n"
        "directory = /user/web\n"
        "restart = always\n"
        "[service-logger]\n"
        "command = /sbin/logger\n";
    const ParseOutcome outcome = parse_checked(test, text_of(text));
    check(outcome.is_success(), test, "parses");
    if (!outcome.is_success()) {
        return;
    }
    check(config.section_count() == 4 && config.setting_count() == 6 && config.init_command_count() == 7 &&
              config.service_count() == 2,
          test, "counts");
    const std::optional<std::size_t> web_index = config.find_service("web");
    check(web_index.has_value() && *web_index == 0 && !config.find_service("absent").has_value(), test,
          "find_service");
    if (web_index.has_value()) {
        const stxconfig::ServiceDefinition& web = config.service(*web_index);
        check(web.argument_count == 4 && same_text(config.text(web.arguments[0]), "/usr/local/sbin/webd") &&
                  same_text(config.text(web.arguments[2]), "8443") && same_text(config.text(web.arguments[3]), "a b") &&
                  web.directory.has_value() && same_text(config.text(*web.directory), "/user/web") &&
                  web.restart == stxconfig::RestartPolicy::always && web.line == 14,
              test, "service web");
        const std::optional<TextRef> literal = config.find_setting("service-web", "command");
        check(literal.has_value() && same_text(config.text(*literal), "/usr/local/sbin/webd --port 8443 \"a b\""), test,
              "the command also kept as a setting, literally");
    }
    if (config.service_count() == 2) {
        const stxconfig::ServiceDefinition& logger = config.service(1);
        check(same_text(config.text(logger.name), "logger") && logger.argument_count == 1 &&
                  !logger.directory.has_value() && logger.restart == stxconfig::RestartPolicy::on_failure,
              test, "service defaults");
    }
    check(config.has_init_section(), test, "init section present");
    const std::optional<TextRef> hostname = config.find_setting("system", "hostname");
    check(hostname.has_value() && same_text(config.text(*hostname), "stx-vm"), test, "hostname, trailing blanks dropped");
    const std::optional<TextRef> empty = config.find_setting("system", "empty");
    check(empty.has_value() && same_text(config.text(*empty), ""), test, "empty value");
    check(!config.find_setting("system", "missing").has_value(), test, "missing setting");
    check(!config.find_setting("network", "hostname").has_value(), test, "missing section");

    const stxconfig::InitCommand& first = config.init_command(0);
    const stxconfig::MountCommand* const mount = first.action.as_mount();
    check(mount != nullptr && !first.guard.has_value() && first.line == 7, test, "mount, line 7");
    if (mount != nullptr) {
        check((*mount).file_system == stxconfig::FileSystemKind::ufs &&
                  (*mount).access == stxconfig::MountAccess::read_write &&
                  same_text(config.text((*mount).device), "/dev/gpt/user") &&
                  same_text(config.text((*mount).directory), "/user"),
              test, "mount fields");
    }
    const stxconfig::WaitForCommand* const wait_for = config.init_command(1).action.as_wait_for();
    check(wait_for != nullptr && (*wait_for).kind == stxconfig::PresenceKind::device && (*wait_for).timeout_ms == 2000 &&
              same_text(config.text((*wait_for).path), "/dev/gpt/user"),
          test, "wait-for fields");
    const stxconfig::InitCommand& guarded = config.init_command(2);
    const stxconfig::RunCommand* const autorun = guarded.action.as_run();
    check(guarded.guard.has_value() && autorun != nullptr, test, "guarded run");
    if (guarded.guard.has_value() && autorun != nullptr) {
        check((*guarded.guard).kind == stxconfig::PresenceKind::file &&
                  same_text(config.text((*guarded.guard).path), "/user/autorun") && (*autorun).argument_count == 1 &&
                  same_text(config.text((*autorun).arguments[0]), "/user/autorun"),
              test, "guard and run fields");
    }
    const stxconfig::SetCommand* const set = config.init_command(3).action.as_set();
    check(set != nullptr && same_text(config.text((*set).name), "LANG") &&
              same_text(config.text((*set).value), "C.UTF-8"),
          test, "set fields");
    const stxconfig::StartCommand* const start = config.init_command(4).action.as_start();
    check(start != nullptr && same_text(config.text((*start).service), "web"), test, "start fields");
    const stxconfig::RunCommand* const run = config.init_command(5).action.as_run();
    check(run != nullptr && (*run).argument_count == 4, test, "run argument count");
    if (run != nullptr && (*run).argument_count == 4) {
        check(same_text(config.text((*run).arguments[0]), "/sbin/thing") &&
                  same_text(config.text((*run).arguments[1]), "an \"argument\"") &&
                  same_text(config.text((*run).arguments[2]), "second") &&
                  same_text(config.text((*run).arguments[3]), ""),
              test, "run arguments, quoted and escaped");
    }
    const stxconfig::MountCommand* const efi = config.init_command(6).action.as_mount();
    check(efi != nullptr && (*efi).file_system == stxconfig::FileSystemKind::msdosfs &&
              (*efi).access == stxconfig::MountAccess::read_only,
          test, "msdosfs read-only mount");
    check(same_text(config.init_command(0).action.keyword(), "mount") &&
              same_text(config.init_command(1).action.keyword(), "wait-for") &&
              same_text(config.init_command(5).action.keyword(), "run"),
          test, "keywords");
}

void test_line_ends_and_empty() {
    const char* const test = "line ends";
    check(parse_checked(test, text_of("")).is_success() && config.section_count() == 0, test, "empty file");
    check(parse_checked(test, text_of("\n\n   \n\t\n# x\n")).is_success() && config.section_count() == 0, test,
          "blank and comment lines");
    const ParseOutcome crlf = parse_checked(test, text_of("[a]\r\nk = v\r\n[init]\r\nrun /s\r"));
    check(crlf.is_success(), test, "CR LF and a final CR");
    if (crlf.is_success()) {
        const std::optional<TextRef> value = config.find_setting("a", "k");
        check(value.has_value() && same_text(config.text(*value), "v"), test, "CR not part of the value");
    }
    check(parse_checked(test, text_of("[a]\nk=v")).is_success() && config.setting_count() == 1, test,
          "no final line end");
    check(parse_checked(test, text_of("[init]\n")).is_success() && config.has_init_section() &&
              config.init_command_count() == 0,
          test, "empty init section");
}

void test_failure_empties_previous() {
    const char* const test = "failure empties";
    check(parse_checked(test, text_of("[a]\nk=v\n[init]\nrun /s\n")).is_success(), test, "first parse");
    const ParseOutcome second = parse_checked(test, text_of("[a]\nk=v\n[init]\nrun s\n"));
    check(!second.is_success() && config.section_count() == 0 && config.init_command_count() == 0, test,
          "second parse refused and configuration empty");
}

// ---------------------------------------------------------------------------
// Refusals.

struct Refusal {
    const char* text;
    std::uint32_t line;
    std::uint32_t column;
    ParseError error;
};

const Refusal refusals[] = {
    {"x = 1\n", 1, 1, ParseError::setting_outside_section},
    {"[Sys]\n", 1, 2, ParseError::name_malformed},
    {"[]\n", 1, 2, ParseError::name_malformed},
    {"[sys\n", 1, 5, ParseError::header_malformed},
    {"[sys] x\n", 1, 7, ParseError::header_malformed},
    {"[a]\n[a]\n", 2, 2, ParseError::section_repeated},
    {"[a]\nk 1\n", 2, 3, ParseError::setting_malformed},
    {"[a]\nk=1\nk=2\n", 3, 1, ParseError::setting_repeated},
    {"[a]\n9k=1\n", 2, 1, ParseError::name_malformed},
    {"[a]\nk=\x01\n", 2, 3, ParseError::byte_not_allowed},
    {"[a]\nk=v\rx\n", 2, 4, ParseError::byte_not_allowed},
    {"[a]\nk=\xc3\xa9\n", 2, 3, ParseError::byte_not_allowed},
    {"[init]\nrun /x \"ab\n", 2, 8, ParseError::quote_unterminated},
    {"[init]\nrun /x \"a\\nb\"\n", 2, 10, ParseError::escape_malformed},
    {"[init]\nrun /x \"a\\\n", 2, 10, ParseError::escape_malformed},
    {"[init]\nrun /x \"a\"b\n", 2, 11, ParseError::word_after_quote},
    {"[init]\nrun /x a\"b\n", 2, 9, ParseError::quote_in_word},
    {"[init]\nfrobnicate\n", 2, 1, ParseError::command_unknown},
    {"[init]\nhostname = x\n", 2, 1, ParseError::command_unknown},
    {"[init]\nmount ufs /dev/x /y\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nmount ufs /d /m rw extra\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nmount zfs /d /m rw\n", 2, 7, ParseError::file_system_unknown},
    {"[init]\nmount ufs /d /m rx\n", 2, 17, ParseError::access_unknown},
    {"[init]\nwait-for disk /d 10\n", 2, 10, ParseError::presence_kind_unknown},
    {"[init]\nwait-for file /d 1x\n", 2, 18, ParseError::number_malformed},
    {"[init]\nwait-for file /d \"\"\n", 2, 18, ParseError::number_malformed},
    {"[init]\nwait-for file /d 12345678901\n", 2, 18, ParseError::number_malformed},
    {"[init]\nwait-for file /d 60001\n", 2, 18, ParseError::wait_too_long},
    {"[init]\nwait-for file /d 9999999999\n", 2, 18, ParseError::wait_too_long},
    {"[init]\nset 1A x\n", 2, 5, ParseError::variable_name_malformed},
    {"[init]\nset A\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nrun x\n", 2, 5, ParseError::program_not_absolute},
    {"[init]\nrun \"\"\n", 2, 5, ParseError::program_not_absolute},
    {"[init]\nrun\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nstart Web\n", 2, 7, ParseError::name_malformed},
    {"[init]\nstart\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nrun /a\n  start web\n", 3, 3, ParseError::start_unknown_service},
    {"[service-web]\ncommand = /w\n[init]\nstart other\n", 4, 1, ParseError::start_unknown_service},
    {"[service-web]\ndirectory = /w\n", 1, 2, ParseError::service_command_missing},
    {"[service-web]\n[init]\n", 1, 2, ParseError::service_command_missing},
    {"[service-web]\ncommand = /w\nuser = x\n", 3, 1, ParseError::service_setting_unknown},
    {"[service-web]\ncommand = w\n", 2, 11, ParseError::program_not_absolute},
    {"[service-web]\ncommand =\n", 2, 10, ParseError::program_not_absolute},
    {"[service-web]\ncommand = /w \"x\n", 2, 14, ParseError::quote_unterminated},
    {"[service-web]\ncommand = /w\ncommand = /v\n", 3, 1, ParseError::setting_repeated},
    {"[service-web]\ncommand = /w\ndirectory = w\n", 3, 13, ParseError::directory_not_absolute},
    {"[service-web]\ncommand = /w\nrestart = sometimes\n", 3, 11, ParseError::service_restart_unknown},
    {"[service-web]\ncommand = /w\ncontrol = all\n", 3, 11, ParseError::service_control_unknown},
    {"[service-web]\ncommand = /w\ncontrol =\n", 3, 10, ParseError::service_control_unknown},
    {"[service-web]\ncommand = /w\ncontrol = read\ncontrol = read\n", 4, 1, ParseError::setting_repeated},
    {"[service-]\n", 1, 2, ParseError::name_malformed},
    {"[service-9]\n", 1, 2, ParseError::name_malformed},
    {"[init]\nif-present file /a if-present file /b run /c\n", 2, 20, ParseError::guard_nested},
    {"[init]\nif-present file /a\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nif-present dir /a run /b\n", 2, 12, ParseError::presence_kind_unknown},
    // C2: targets. [target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n is a complete server target (5 lines).
    {"[target-a]\ncommand = bin/a\n", 1, 2, ParseError::target_setting_missing},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\n", 1, 2, ParseError::target_setting_missing},
    {"[target-a]\nrole = server\nfolder = /data/targets/a\ncommand = bin/a\n", 1, 2,
     ParseError::target_setting_missing},
    {"[target-a]\nid = 1\nfolder = /data/targets/a\ncommand = bin/a\n", 1, 2, ParseError::target_setting_missing},
    {"[target-a]\nrole = server\nid = 1\nfolder = /data/targets/a\n", 1, 2, ParseError::service_command_missing},
    {"[target-a]\nrole = boss\n", 2, 8, ParseError::target_role_unknown},
    {"[target-a]\nid = 0\n", 2, 6, ParseError::target_id_malformed},
    {"[target-a]\nid = 01\n", 2, 6, ParseError::target_id_malformed},
    {"[target-a]\nid = 1000\n", 2, 6, ParseError::target_id_malformed},
    {"[target-a]\nid = -1\n", 2, 6, ParseError::target_id_malformed},
    {"[target-a]\nprogram = A\n", 2, 11, ParseError::name_malformed},
    {"[target-a]\nprogram = a b\n", 2, 11, ParseError::name_malformed},
    {"[target-a]\ncommand = /bin/a\n", 2, 11, ParseError::program_path_malformed},
    {"[target-a]\ncommand = ../a\n", 2, 11, ParseError::program_path_malformed},
    {"[target-a]\ncommand = bin/\n", 2, 11, ParseError::program_path_malformed},
    {"[target-a]\ncommand =\n", 2, 10, ParseError::program_path_malformed},
    {"[target-a]\ndirectory = /x\n", 2, 1, ParseError::service_setting_unknown},
    {"[target-a]\nnetwork = none\n", 2, 1, ParseError::service_setting_unknown},
    // A target never gets a control channel (C2_DESIGN.md, "The supervisor
    // interface", item 2).
    {"[target-a]\ncontrol = read\n", 2, 1, ParseError::service_setting_unknown},
    {"[service-a]\ncommand = /w\nfolder = /data/targets/a\n", 3, 1, ParseError::service_setting_unknown},
    {"[service-a]\ncommand = /w\nid = 1\n", 3, 1, ParseError::service_setting_unknown},
    {"[target-a]\nfolder = /w\n", 2, 10, ParseError::folder_outside_data},
    {"[target-a]\nfolder = data/targets/x\n", 2, 10, ParseError::folder_malformed},
    {"[target-a]\nfolder = /data/targets\n", 2, 10, ParseError::folder_outside_data},
    {"[target-a]\nfolder = /data/targets/\n", 2, 10, ParseError::folder_malformed},
    {"[target-a]\nfolder = /data/targets/a/../b\n", 2, 10, ParseError::folder_malformed},
    {"[target-a]\nfolder = /data/targets//a\n", 2, 10, ParseError::folder_malformed},
    {"[target-a]\nfolder = /data/targets/a b\n", 2, 10, ParseError::folder_malformed},
    {"[target-a]\nfolder = /data/targetsx/a\n", 2, 10, ParseError::folder_outside_data},
    {"[target-a]\nfolder =\n", 2, 9, ParseError::folder_malformed},
    {"[target-a]\nports = tcp/0\n", 2, 9, ParseError::port_malformed},
    {"[target-a]\nports = tcp/65536\n", 2, 9, ParseError::port_malformed},
    {"[target-a]\nports = tcp/080\n", 2, 9, ParseError::port_malformed},
    {"[target-a]\nports = sctp/1\n", 2, 9, ParseError::port_malformed},
    {"[target-a]\nports = tcp/\n", 2, 9, ParseError::port_malformed},
    {"[target-a]\nports =\n", 2, 8, ParseError::port_malformed},
    {"[target-a]\nports = tcp/80 udp/80 tcp/80\n", 2, 23, ParseError::port_repeated},
    {"[target-a]\nports = a b c d e f g h i\n", 2, 25, ParseError::too_many_ports},
    {"[target-a]\nstart = later\n", 2, 9, ParseError::start_policy_unknown},
    {"[target-a]\nrestart = sometimes\n", 2, 11, ParseError::service_restart_unknown},
    {"[target-a]\nupdate-url = http://x/l\n", 2, 14, ParseError::update_url_malformed},
    {"[target-a]\nupdate-url = https:///l\n", 2, 14, ParseError::update_url_malformed},
    {"[target-a]\nupdate-url = https://\n", 2, 14, ParseError::update_url_malformed},
    {"[target-a]\nupdate-url = https://x/a|b\n", 2, 14, ParseError::update_url_malformed},
    {"[target-a]\nupdate-interval = 299\n", 2, 19, ParseError::update_interval_malformed},
    {"[target-a]\nupdate-interval = 2592001\n", 2, 19, ParseError::update_interval_malformed},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "update-interval = 300\n", 6, 1, ParseError::update_interval_malformed},
    {"[target-a]\nhealthy-after = 0\n", 2, 17, ParseError::healthy_after_malformed},
    {"[target-a]\nhealthy-after = 3601\n", 2, 17, ParseError::healthy_after_malformed},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices = fb0\n", 6, 11, ParseError::devices_not_foreground},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices = /dev/x\n", 6, 11, ParseError::device_pattern_malformed},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices = a/../b\n", 6, 11, ParseError::device_pattern_malformed},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices = ok tty[0]\n", 6, 14, ParseError::device_pattern_malformed},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices =\n", 6, 10, ParseError::device_pattern_malformed},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "devices = a b c d e f g h i\n", 6, 27, ParseError::too_many_devices},
    {"[target-a]\nrole = foreground\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "[target-b]\nrole = foreground\nid = 2\ncommand = bin/b\nfolder = /data/targets/b\n", 6, 1,
     ParseError::second_foreground},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "[target-b]\nrole = server\nid = 1\ncommand = bin/b\nfolder = /data/targets/b\n", 6, 1,
     ParseError::target_id_repeated},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "[target-b]\nrole = server\nid = 2\ncommand = bin/b\nfolder = /data/targets/a\n", 6, 1,
     ParseError::folder_repeated},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "[target-b]\nrole = server\nid = 2\ncommand = bin/b\nfolder = /data/targets/a/b\n", 6, 1,
     ParseError::folder_repeated},
    {"[target-b]\nrole = server\nid = 2\ncommand = bin/b\nfolder = /data/targets/a/b\n"
     "[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n", 6, 1, ParseError::folder_repeated},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "ports = tcp/8000\n"
     "[target-b]\nrole = server\nid = 2\ncommand = bin/b\nfolder = /data/targets/b\nports = udp/8000 tcp/8000\n",
     7, 1, ParseError::port_repeated},
    {"[service-a]\ncommand = /w\n"
     "[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n", 3, 1, ParseError::name_repeated},
    {"[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = /data/targets/a\n"
     "[init]\nstart b\n", 7, 1, ParseError::start_unknown_service},
    // C2: the network section.
    {"[network]\nmode = auto\n", 2, 8, ParseError::network_value_malformed},
    {"[network]\nhost = x\n", 2, 1, ParseError::network_setting_unknown},
    {"[network]\ninterface = 9x\n", 2, 13, ParseError::network_value_malformed},
    {"[network]\ninterface = abcdefghijklmnop\n", 2, 13, ParseError::network_value_malformed},
    {"[network]\naddress = 10.0.0.1\n", 2, 11, ParseError::network_value_malformed},
    {"[network]\naddress = 10.0.0.1/33\n", 2, 11, ParseError::network_value_malformed},
    {"[network]\naddress = 10.0.0/8\n", 2, 11, ParseError::network_value_malformed},
    {"[network]\ngateway = 10.0.0.256\n", 2, 11, ParseError::network_value_malformed},
    {"[network]\ngateway = 10.00.0.1\n", 2, 11, ParseError::network_value_malformed},
    {"[network]\ndns = 1.1.1.1 2.2.2.2 3.3.3.3 4.4.4.4\n", 2, 7, ParseError::network_value_malformed},
    {"[network]\ndns =\n", 2, 6, ParseError::network_value_malformed},
    {"[network]\nmode = dhcp\nmode = static\n", 3, 1, ParseError::setting_repeated},
    // C2: sysctl.
    {"[init]\nsysctl a\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl =1\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl .a=1\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl a.=1\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl a..b=1\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl a$=1\n", 2, 8, ParseError::sysctl_malformed},
    {"[init]\nsysctl a=1 b=2\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nsysctl\n", 2, 1, ParseError::command_arguments_wrong},
};

void test_refusals() {
    for (const Refusal& refusal : refusals) {
        const ParseOutcome outcome = parse_checked(refusal.text, text_of(refusal.text));
        const ParseFailed* const failure = outcome.as_failure();
        check(failure != nullptr, refusal.text, "refused");
        if (failure == nullptr) {
            continue;
        }
        check((*failure).error == refusal.error, refusal.text, stxconfig::parse_error_name(refusal.error));
        check((*failure).line == refusal.line && (*failure).column == refusal.column, refusal.text, "position");
        if ((*failure).error != refusal.error || (*failure).line != refusal.line ||
            (*failure).column != refusal.column) {
            std::printf("  got %s at %u:%u\n", stxconfig::parse_error_name((*failure).error), (*failure).line,
                        (*failure).column);
        }
    }
}

// ---------------------------------------------------------------------------
// Limits, at and one past each bound. Inputs are built in one buffer.

char built[stxconfig::config_size_max + 2];
std::size_t built_length = 0;

void build_reset() {
    built_length = 0;
}

void build_append(const char* text) {
    const std::size_t length = stxtest::c_length(text);
    if (built_length + length > sizeof(built)) {
        std::printf("test buffer too small\n");
        return;
    }
    std::memcpy(&built[built_length], text, length);
    built_length += length;
}

// A two-letter name from a number below 26 * 26.
void build_two_letters(std::size_t number) {
    char name[3];
    name[0] = static_cast<char>('a' + static_cast<int>(number / 26u));
    name[1] = static_cast<char>('a' + static_cast<int>(number % 26u));
    name[2] = '\0';
    build_append(name);
}

[[nodiscard]] std::span<const char> built_text() {
    const std::span<const char> view(built, built_length);
    return view;
}

void expect_refusal(const char* test, std::uint32_t line, std::uint32_t column, ParseError error) {
    const ParseOutcome outcome = parse_checked(test, built_text());
    const ParseFailed* const failure = outcome.as_failure();
    check(failure != nullptr && (*failure).error == error && (*failure).line == line && (*failure).column == column,
          test, stxconfig::parse_error_name(error));
    if (failure != nullptr && ((*failure).error != error || (*failure).line != line || (*failure).column != column)) {
        std::printf("  got %s at %u:%u\n", stxconfig::parse_error_name((*failure).error), (*failure).line,
                    (*failure).column);
    }
}

void test_limits() {
    // File size.
    build_reset();
    for (std::size_t index = 0; index < stxconfig::config_size_max; index += 1) {
        build_append("\n");
    }
    check(parse_checked("size at limit", built_text()).is_success(), "size at limit", "accepted");
    build_append("\n");
    expect_refusal("size past limit", 1, 1, ParseError::file_too_large);

    // Line length.
    build_reset();
    build_append("#");
    for (std::size_t index = 1; index < stxconfig::line_length_max; index += 1) {
        build_append("x");
    }
    build_append("\r\n");
    check(parse_checked("line at limit", built_text()).is_success(), "line at limit", "accepted (CR not counted)");
    build_reset();
    for (std::size_t index = 0; index <= stxconfig::line_length_max; index += 1) {
        build_append("#");
    }
    expect_refusal("line past limit", 1, 256, ParseError::line_too_long);

    // Name length: 32 accepted, 33 refused.
    build_reset();
    build_append("[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]\n");
    check(parse_checked("name at limit", built_text()).is_success(), "name at limit", "accepted");
    build_reset();
    build_append("[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]\n");
    expect_refusal("name past limit", 1, 2, ParseError::name_malformed);

    // Sections.
    build_reset();
    for (std::size_t index = 0; index < stxconfig::sections_max; index += 1) {
        build_append("[");
        build_two_letters(index);
        build_append("]\n");
    }
    check(parse_checked("sections at limit", built_text()).is_success(), "sections at limit", "accepted");
    build_append("[zz]\n");
    expect_refusal("sections past limit", 17, 1, ParseError::too_many_sections);

    // Settings.
    build_reset();
    build_append("[a]\n");
    for (std::size_t index = 0; index < stxconfig::settings_max; index += 1) {
        build_two_letters(index);
        build_append(" = v\n");
    }
    check(parse_checked("settings at limit", built_text()).is_success(), "settings at limit", "accepted");
    build_append("zz = v\n");
    expect_refusal("settings past limit", 66, 1, ParseError::too_many_settings);

    // Commands.
    build_reset();
    build_append("[service-s]\ncommand = /s\n[init]\n");
    for (std::size_t index = 0; index < stxconfig::init_commands_max; index += 1) {
        build_append("start s\n");
    }
    check(parse_checked("commands at limit", built_text()).is_success(), "commands at limit", "accepted");
    build_append("start s\n");
    expect_refusal("commands past limit", 68, 1, ParseError::too_many_commands);

    // Services: 16 accepted (with no other section), 17 refused.
    build_reset();
    for (std::size_t index = 0; index < stxconfig::services_max; index += 1) {
        build_append("[service-");
        build_two_letters(index);
        build_append("]\ncommand = /s\n");
    }
    check(parse_checked("services at limit", built_text()).is_success() && config.service_count() == 16,
          "services at limit", "accepted");
    build_reset();
    for (std::size_t index = 0; index < stxconfig::services_max - 1u; index += 1) {
        build_append("[service-");
        build_two_letters(index);
        build_append("]\ncommand = /s\n");
    }
    build_append("[service-zz]\ncommand = /s\n");
    check(parse_checked("services at limit, last named zz", built_text()).is_success(), "services at limit",
          "zz accepted");

    // A service command of 16 words is accepted; 17 refused at the 17th.
    build_reset();
    build_append("[service-s]\ncommand = /p");
    for (std::size_t index = 1; index < stxconfig::run_arguments_max; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    check(parse_checked("service command at limit", built_text()).is_success(), "service command at limit",
          "accepted");
    build_reset();
    build_append("[service-s]\ncommand = /p");
    for (std::size_t index = 0; index < stxconfig::run_arguments_max; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    expect_refusal("service command past limit", 2, 44, ParseError::too_many_run_arguments);

    // Run arguments: 16 accepted, 17 refused; a guarded run of 16 is 20 words.
    build_reset();
    build_append("[init]\nrun /p");
    for (std::size_t index = 1; index < stxconfig::run_arguments_max; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    const ParseOutcome sixteen = parse_checked("run arguments at limit", built_text());
    check(sixteen.is_success() && config.init_command_count() == 1, "run arguments at limit", "accepted");
    if (sixteen.is_success() && config.init_command_count() == 1) {
        const stxconfig::RunCommand* const run = config.init_command(0).action.as_run();
        check(run != nullptr && (*run).argument_count == stxconfig::run_arguments_max, "run arguments at limit",
              "16 arguments");
    }
    build_reset();
    build_append("[init]\nrun /p");
    for (std::size_t index = 0; index < stxconfig::run_arguments_max; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    expect_refusal("run arguments past limit", 2, 38, ParseError::too_many_run_arguments);
    build_reset();
    build_append("[init]\nif-present file /a run /p");
    for (std::size_t index = 1; index < stxconfig::run_arguments_max; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    check(parse_checked("guarded run at word limit", built_text()).is_success(), "guarded run at word limit",
          "20 words accepted");

    // Words: 21 refused at the 21st.
    build_reset();
    build_append("[init]\nrun /p");
    for (std::size_t index = 0; index < 19u; index += 1) {
        build_append(" a");
    }
    build_append("\n");
    expect_refusal("words past limit", 2, 44, ParseError::too_many_words);

    // Wait: 60000 accepted.
    build_reset();
    build_append("[init]\nwait-for file /f 60000\nwait-for device /d 0\n");
    check(parse_checked("wait at limit", built_text()).is_success(), "wait at limit", "accepted");
}

// ---------------------------------------------------------------------------
// Bounded exhaustive runs. An odometer over alphabet indices enumerates
// every sequence of each length in turn; no recursion.

// Advances digits (each below base) as a base-`base` counter. False when it
// wraps to all zeros (every sequence of this length done).
[[nodiscard]] bool advance(std::size_t* digits, std::size_t length, std::size_t base) {
    for (std::size_t position = 0; position < length; position += 1) {
        digits[position] += 1;
        if (digits[position] < base) {
            return true;
        }
        digits[position] = 0;
    }
    return false;
}

// Every byte string of length 0..6 over 12 bytes chosen to reach every
// branch of line splitting, headers, settings and words: 3,257,437 inputs.
void test_exhaustive_bytes() {
    const char alphabet[] = {'[', ']', 'a', '=', ' ', '"', '\\', '#', '\n', '\r', '/', '\x7f'};
    constexpr std::size_t base = sizeof(alphabet);
    constexpr std::size_t length_max = 6;
    std::size_t inputs = 0;
    std::size_t accepted = 0;
    const std::size_t failures_before = stxtest::tally.failures;
    for (std::size_t length = 0; length <= length_max; length += 1) {
        std::size_t digits[length_max] = {};
        char input[length_max];
        for (;;) {
            for (std::size_t position = 0; position < length; position += 1) {
                input[position] = alphabet[digits[position]];
            }
            const std::span<const char> view(input, length);
            const ParseOutcome outcome = stxconfig::parse_boot_config(view, config);
            check_invariants("exhaustive bytes", view, outcome);
            inputs += 1;
            accepted += outcome.is_success() ? 1u : 0u;
            if (stxtest::tally.failures != failures_before) {
                std::printf("  first failing byte input of length %zu\n", length);
                return;
            }
            if (!advance(digits, length, base)) {
                break;
            }
        }
    }
    std::printf("exhaustive bytes: %zu inputs (lengths 0-%zu over %zu bytes), %zu accepted\n", inputs, length_max,
                base, accepted);
}

// Every sequence of 0..5 tokens over 16 init-section tokens, after
// "[init]\n": 1,118,481 inputs.
void test_exhaustive_tokens() {
    const char* const tokens[] = {
        "run", " ", "/p", "\"q r\"", "\n", "if-present", "file", "device",
        "mount", "ufs", "rw", "wait-for", "60000", "set", "A", "start",
    };
    constexpr std::size_t base = sizeof(tokens) / sizeof(tokens[0]);
    constexpr std::size_t length_max = 5;
    std::size_t inputs = 0;
    std::size_t accepted = 0;
    std::size_t with_commands = 0;
    const std::size_t failures_before = stxtest::tally.failures;
    for (std::size_t length = 0; length <= length_max; length += 1) {
        std::size_t digits[length_max] = {};
        for (;;) {
            build_reset();
            build_append("[init]\n");
            for (std::size_t position = 0; position < length; position += 1) {
                build_append(tokens[digits[position]]);
            }
            const std::span<const char> view = built_text();
            const ParseOutcome outcome = stxconfig::parse_boot_config(view, config);
            check_invariants("exhaustive tokens", view, outcome);
            inputs += 1;
            if (outcome.is_success()) {
                accepted += 1;
                with_commands += config.init_command_count() > 0 ? 1u : 0u;
            }
            if (stxtest::tally.failures != failures_before) {
                std::printf("  first failing token input of length %zu\n", length);
                return;
            }
            if (!advance(digits, length, base)) {
                break;
            }
        }
    }
    std::printf("exhaustive tokens: %zu inputs (0-%zu of %zu tokens), %zu accepted, %zu with commands\n", inputs,
                length_max, base, accepted, with_commands);
}

}  // namespace

// A target read back field by field, defaults included, beside a service;
// and a network section.
void test_target_definition() {
    const char* const test = "target definition";
    const char* const text =
        "[service-web]\n"
        "command = /sbin/webd --port 8443\n"
        "[target-show]\n"
        "role = foreground\n"
        "id = 7\n"
        "program = viewer\n"
        "command = bin/viewer --full \"a b\"\n"
        "folder = /data/targets/show\n"
        "ports = tcp/8000 udp/5353\n"
        "devices = fb0 \"input/*\" ttyv?\n"
        "start = manual\n"
        "restart = always\n"
        "update-url = https://updates.example/show/latest\n"
        "update-interval = 3600\n"
        "healthy-after = 60\n"
        "[target-radio]\n"
        "role = server\n"
        "id = 999\n"
        "command = radiod\n"
        "folder = /data/targets/radio\n"
        "[network]\n"
        "mode = static\n"
        "interface = genet0\n"
        "address = 192.168.1.20/24\n"
        "gateway = 192.168.1.1\n"
        "dns = 192.168.1.1 9.9.9.9\n"
        "[init]\n"
        "start radio\n"
        "start show\n";
    const ParseOutcome outcome = parse_checked(test, text_of(text));
    check(outcome.is_success(), test, "parses");
    if (!outcome.is_success() || config.service_count() != 3u) {
        return;
    }
    check(!config.service(0).target.has_value(), test, "the service is no target");
    const stxconfig::ServiceDefinition& show = config.service(1);
    check(same_text(config.text(show.name), "show") && show.target.has_value(), test, "show is a target");
    if (!show.target.has_value()) {
        return;
    }
    const stxconfig::TargetSettings& settings = *show.target;
    check(settings.role == stxconfig::TargetRole::foreground && settings.id == 7u &&
              stxconfig::target_identity(settings) == 30007u,
          test, "role, id, identity");
    check(same_text(config.text(settings.program), "viewer") &&
              same_text(config.text(settings.folder), "/data/targets/show"),
          test, "program and folder");
    check(show.argument_count == 3u && same_text(config.text(show.arguments[0]), "bin/viewer") &&
              same_text(config.text(show.arguments[2]), "a b"),
          test, "command words, the file relative");
    check(settings.port_count == 2u && settings.ports[0].protocol == stxconfig::PortProtocol::tcp &&
              settings.ports[0].port == 8000u && settings.ports[1].protocol == stxconfig::PortProtocol::udp &&
              settings.ports[1].port == 5353u,
          test, "ports");
    check(settings.device_count == 3u && same_text(config.text(settings.devices[1]), "input/*"), test, "devices");
    check(settings.start == stxconfig::StartPolicy::manual && show.restart == stxconfig::RestartPolicy::always, test,
          "start and restart");
    check(settings.update_url.has_value() &&
              same_text(config.text(*settings.update_url), "https://updates.example/show/latest") &&
              settings.update_interval_s == 3600u && settings.healthy_after_s == 60u,
          test, "update settings");
    check(!show.directory.has_value(), test, "no directory");
    const stxconfig::ServiceDefinition& radio = config.service(2);
    check(radio.target.has_value(), test, "radio is a target");
    if (radio.target.has_value()) {
        const stxconfig::TargetSettings& defaults = *radio.target;
        check(same_text(config.text(defaults.program), "radio"), test, "program defaults to the name");
        check(defaults.port_count == 0u && defaults.device_count == 0u &&
                  defaults.start == stxconfig::StartPolicy::boot && !defaults.update_url.has_value() &&
                  defaults.update_interval_s == stxconfig::update_interval_default_s &&
                  defaults.healthy_after_s == stxconfig::healthy_after_default_s &&
                  radio.restart == stxconfig::RestartPolicy::on_failure,
              test, "defaults");
    }
}

// The folder rule, exhaustive: every value made of a prefix (none,
// "/data/target", "/data/targets", "/data/targets/", "/data/targetsx/") and
// 0 to 5 bytes over "/", ".", "a", " ", "-", "$" (5 x 9,331 = 46,655
// values), against a reference that splits the trimmed value into
// components. A folder is accepted when it has at least three components
// and begins "/data/targets/".
[[nodiscard]] bool reference_folder_valid(const char* value) {
    std::size_t begin = 0;
    std::size_t end = stxtest::c_length(value);
    while (begin < end && value[begin] == ' ') {
        begin += 1;
    }
    while (end > begin && value[end - 1u] == ' ') {
        end -= 1;
    }
    if (end - begin < 15u || std::strncmp(&value[begin], "/data/targets/", 14) != 0) {
        return false;
    }
    std::size_t component_begin = begin + 1u;
    std::size_t components = 0;
    for (std::size_t index = begin + 1u; index <= end; index += 1) {
        if (index == end || value[index] == '/') {
            const std::size_t length = index - component_begin;
            if (length == 0u) {
                return false;
            }
            if (length == 1u && value[component_begin] == '.') {
                return false;
            }
            if (length == 2u && value[component_begin] == '.' && value[component_begin + 1u] == '.') {
                return false;
            }
            components += 1;
            component_begin = index + 1u;
            continue;
        }
        const char byte = value[index];
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        if (!allowed) {
            return false;
        }
    }
    return components >= 3u;
}

void test_exhaustive_folders() {
    const char* const prefixes[] = {"", "/data/target", "/data/targets", "/data/targets/", "/data/targetsx/"};
    const char alphabet[] = {'/', '.', 'a', ' ', '-', '$'};
    constexpr std::size_t symbols = sizeof(alphabet);
    std::size_t values = 0;
    char value[32];
    for (const char* const prefix : prefixes) {
        const std::size_t prefix_length = stxtest::c_length(prefix);
        for (std::size_t length = 0; length <= 5u; length += 1) {
            std::size_t total = 1;
            for (std::size_t position = 0; position < length; position += 1) {
                total *= symbols;
            }
            for (std::size_t index = 0; index < total; index += 1) {
                std::memcpy(value, prefix, prefix_length);
                std::size_t rest = index;
                for (std::size_t position = 0; position < length; position += 1) {
                    value[prefix_length + position] = alphabet[rest % symbols];
                    rest /= symbols;
                }
                value[prefix_length + length] = '\0';
                build_reset();
                build_append("[target-a]\nrole = server\nid = 1\ncommand = bin/a\nfolder = ");
                build_append(value);
                build_append("\n");
                const ParseOutcome outcome = parse_checked("exhaustive folders", built_text());
                const bool expected = reference_folder_valid(value);
                check(outcome.is_success() == expected, "exhaustive folders", "accepted exactly per the reference");
                if (outcome.is_success() != expected) {
                    std::printf("  value \"%s\"\n", value);
                }
                values += 1;
            }
        }
    }
    std::printf("exhaustive folders: %zu values\n", values);
    check(values == 46655u, "exhaustive folders", "the stated domain was covered");
}

// package_path_valid, exhaustive over every string of 0 to 7 bytes over
// "/", ".", "a", "-", " ", "$" (335,923 strings), against a reference
// that splits on '/'.
[[nodiscard]] bool reference_package_path(const char* value, std::size_t length) {
    if (length == 0u) {
        return false;
    }
    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= length; index += 1) {
        if (index == length || value[index] == '/') {
            const std::size_t size = index - component_begin;
            if (size == 0u || (size == 1u && value[component_begin] == '.') ||
                (size == 2u && value[component_begin] == '.' && value[component_begin + 1u] == '.')) {
                return false;
            }
            component_begin = index + 1u;
            continue;
        }
        const char byte = value[index];
        if (!((byte >= 'a' && byte <= 'z') || byte == '.' || byte == '-')) {
            return false;
        }
    }
    return true;
}

void test_exhaustive_package_paths() {
    const char alphabet[] = {'/', '.', 'a', '-', ' ', '$'};
    constexpr std::size_t symbols = sizeof(alphabet);
    std::size_t values = 0;
    char value[8];
    for (std::size_t length = 0; length <= 7u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                value[position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            const bool expected = reference_package_path(value, length);
            const bool got = stxconfig::package_path_valid(value, length);
            check(got == expected, "exhaustive package paths", "accepted exactly per the reference");
            values += 1;
        }
    }
    std::printf("exhaustive package paths: %zu values\n", values);
    check(values == 335923u, "exhaustive package paths", "the stated domain was covered");
}

// ipv4_of, exhaustive over every string of 0 to 10 bytes over "0", "2",
// "5", "6", "." (12,207,031 strings: it holds 255 and 256, 0 and 02, a
// fourth digit and a fifth part), against a reference that splits on '.'.
[[nodiscard]] bool reference_ipv4(const char* value, std::size_t length, std::uint32_t& address) {
    std::uint32_t parts = 0;
    std::uint32_t result = 0;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= length; index += 1) {
        if (index < length && value[index] != '.') {
            continue;
        }
        const std::size_t size = index - begin;
        if (size == 0u || size > 3u || (size > 1u && value[begin] == '0')) {
            return false;
        }
        std::uint32_t number = 0;
        for (std::size_t digit = begin; digit < index; digit += 1) {
            number = number * 10u + static_cast<std::uint32_t>(value[digit] - '0');
        }
        if (number > 255u) {
            return false;
        }
        result = (result << 8u) | number;
        parts += 1;
        begin = index + 1u;
    }
    if (parts != 4u) {
        return false;
    }
    address = result;
    return true;
}

void test_exhaustive_ipv4() {
    const char alphabet[] = {'0', '2', '5', '6', '.'};
    constexpr std::size_t symbols = sizeof(alphabet);
    std::size_t values = 0;
    std::size_t accepted = 0;
    char value[10];
    for (std::size_t length = 0; length <= 10u; length += 1) {
        std::size_t total = 1;
        for (std::size_t position = 0; position < length; position += 1) {
            total *= symbols;
        }
        for (std::size_t index = 0; index < total; index += 1) {
            std::size_t rest = index;
            for (std::size_t position = 0; position < length; position += 1) {
                value[position] = alphabet[rest % symbols];
                rest /= symbols;
            }
            std::uint32_t expected_address = 0;
            const bool expected = reference_ipv4(value, length, expected_address);
            const std::optional<std::uint32_t> got = stxconfig::ipv4_of(value, length);
            const bool same = got.has_value() == expected && (!expected || *got == expected_address);
            if (!same) {
                check(false, "exhaustive ipv4", "accepted and valued exactly per the reference");
            }
            accepted += expected ? 1u : 0u;
            values += 1;
        }
    }
    std::printf("exhaustive ipv4: %zu values, %zu addresses\n", values, accepted);
    check(values == 12207031u, "exhaustive ipv4", "the stated domain was covered");
}

// The control setting (C2): each value, and none when it is absent.
void test_control_access() {
    const char* const test = "control access";
    struct Case {
        const char* text;
        stxconfig::ControlAccess expected;
    };
    const Case cases[] = {
        {"[service-web]\ncommand = /w\n", stxconfig::ControlAccess::none},
        {"[service-web]\ncommand = /w\ncontrol = none\n", stxconfig::ControlAccess::none},
        {"[service-web]\ncommand = /w\ncontrol = read\n", stxconfig::ControlAccess::read},
        {"[service-web]\ncontrol = operate  \ncommand = /w\n", stxconfig::ControlAccess::operate},
    };
    for (const Case& item : cases) {
        const ParseOutcome outcome = parse_checked(test, text_of(item.text));
        check(outcome.is_success() && config.service_count() == 1, test, item.text);
        if (outcome.is_success() && config.service_count() == 1) {
            check(config.service(0).control == item.expected, test, item.text);
        }
    }
}

int main() {
    test_full_example();
    test_target_definition();
    test_exhaustive_folders();
    test_exhaustive_package_paths();
    test_exhaustive_ipv4();
    test_control_access();
    test_line_ends_and_empty();
    test_failure_empties_previous();
    test_refusals();
    test_limits();
    test_exhaustive_bytes();
    test_exhaustive_tokens();
    return stxtest::finish("test_boot_config");
}
