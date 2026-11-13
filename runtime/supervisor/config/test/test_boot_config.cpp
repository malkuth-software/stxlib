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
    {"[service-]\n", 1, 2, ParseError::name_malformed},
    {"[service-9]\n", 1, 2, ParseError::name_malformed},
    {"[init]\nif-present file /a if-present file /b run /c\n", 2, 20, ParseError::guard_nested},
    {"[init]\nif-present file /a\n", 2, 1, ParseError::command_arguments_wrong},
    {"[init]\nif-present dir /a run /b\n", 2, 12, ParseError::presence_kind_unknown},
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

int main() {
    test_full_example();
    test_line_ends_and_empty();
    test_failure_empties_previous();
    test_refusals();
    test_limits();
    test_exhaustive_bytes();
    test_exhaustive_tokens();
    return stxtest::finish("test_boot_config");
}
