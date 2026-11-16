// test_control_server.cpp: host tests of the supervisor's side of the
// control channel (control/control_server.h), with fake actions and a fake
// system.
//
// - authority, exhaustively over every command: a process that is no
//   service, and a service with control none, are refused everything; a
//   service with control read gets exactly the commands that do not
//   operate;
// - a service's stop of itself refused (refused_self); a stop of another
//   answered with its record;
// - the user configuration: nothing at first; a text that does not parse
//   refused with its line (kind parse); a text that breaks the user rules
//   refused (kind user_rule, target_name_taken for a factory service);
//   a good text written through the system, then read back with the
//   written-since-start flag; a store failure answered store_failed;
// - reboot asked through the system, and only for operate;
// - the program commands: unsupported passed on; a record about another
//   target, or one that breaks the format, withheld (unavailable); a good
//   record passed on;
// - operation_state: an inconsistent record withheld;
// - whole_lines_of_end over every window of up to 7 bytes of {a, LF}, and
//   last_good_slot over every boot fact combination, against their
//   definitions.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../config/boot_config.h"
#include "../../config/service_registry.h"
#include "../../config/test/test_support.h"
#include "../../config/user_config.h"
#include "../control_protocol.h"
#include "../control_server.h"
#include "control_fakes.h"

namespace {

using namespace stxcontrol;
using stxcontrol::test::FakeActions;
using stxcontrol::test::FakeSystem;
using stxtest::check;
using stxtest::text_of;

const char* const factory_text =
    "[init]\n"
    "start web\n"
    "[service-web]\n"
    "command = /usr/sbin/stxweb\n"
    "control = operate\n"
    "[service-reader]\n"
    "command = /bin/sleep 3600\n"
    "control = read\n"
    "[service-plain]\n"
    "command = /bin/sleep 3600\n"
    "[service-sleeper]\n"
    "command = /bin/sleep 3600\n"
    "restart = always\n";

stxconfig::BootConfig factory;
stxconfig::ServiceRegistry registry;
FakeActions actions;
FakeSystem fake_system;
ControlServer* server_pointer = nullptr;
std::uint8_t request_frame[request_frame_bytes_max];
std::uint8_t response_frame[response_frame_bytes_max];

// pids by start order (web, reader, plain, sleeper).
constexpr int web_pid = 100;
constexpr int reader_pid = 101;
constexpr int plain_pid = 102;

[[nodiscard]] ServiceName name_of(const char* text) {
    const std::optional<ServiceName> name = service_name_of(text_of(text));
    return name.has_value() ? *name : ServiceName{};
}

[[nodiscard]] ControlFacts facts() {
    ControlFacts value{};
    value.uptime_ms = 5000;
    value.restart_count = 0u;
    value.init_done = true;
    value.boot.slot = stxboot::Slot::a;
    return value;
}

// Sends request from pid; the decoded response.
[[nodiscard]] std::optional<ResponseView> ask(const Request& request, int pid) {
    const std::optional<std::size_t> size = encode_request(request, request_frame);
    if (!size.has_value()) {
        return std::nullopt;
    }
    const ControlHandled handled = (*server_pointer)
                                       .handle(std::span<const std::uint8_t>(request_frame, *size), pid, facts(), 6000,
                                               response_frame);
    const ResponseOutcome outcome = decode_response(std::span<const std::uint8_t>(response_frame, handled.response_size));
    const ResponseView* const view = outcome.as_view();
    if (view == nullptr || (*view).status != handled.status) {
        return std::nullopt;
    }
    return *view;
}

[[nodiscard]] Request request_of(Command command, const char* name) {
    return command_takes_name(command) ? request_for_service(command, 1, name_of(name))
                                       : request_without_name(command, 1);
}

[[nodiscard]] Request config_write_of(const char* text) {
    return request_with_payload(request_without_name(Command::config_write, 2),
                                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)));
}

void test_authority() {
    std::uint8_t install[install_source_header_bytes];
    const std::optional<std::size_t> install_size =
        encode_install_source(InstallSource{InstallKind::staged, std::span<const char>()}, install);
    for (std::uint16_t code = command_first; code <= command_last; code += 1) {
        const Command command = *command_from_code(code);
        Request request = request_of(command, "sleeper");
        if (command_payload(command) == PayloadRule::config_text) {
            request = config_write_of("[system]\nhostname = a\n");
        } else if (command_payload(command) == PayloadRule::install_source && install_size.has_value()) {
            request = request_with_payload(request, std::span<const std::uint8_t>(install, *install_size));
        }
        const std::optional<ResponseView> stranger = ask(request, 4242);
        check(stranger.has_value() && (*stranger).status == Status::not_permitted, "authority",
              "a process that is no service is refused");
        const std::optional<ResponseView> plain = ask(request, plain_pid);
        check(plain.has_value() && (*plain).status == Status::not_permitted, "authority",
              "control none is refused");
        const std::optional<ResponseView> reader = ask(request, reader_pid);
        const bool refused = reader.has_value() && (*reader).status == Status::not_permitted;
        check(reader.has_value() && refused == command_operates(command), "authority",
              "control read: exactly the commands that do not operate");
    }
    check(fake_system.reboots() == 0, "authority", "no refused reboot reached the system");
}

void test_stop_self() {
    const std::optional<ResponseView> self = ask(request_of(Command::stop, "web"), web_pid);
    check(self.has_value() && (*self).status == Status::refused_self, "stop", "a service's stop of itself refused");
    const std::optional<ResponseView> other = ask(request_of(Command::stop, "sleeper"), web_pid);
    const std::optional<ServiceRecord> record =
        other.has_value() ? decode_service_record((*other).payload) : std::nullopt;
    check(other.has_value() && (*other).status == Status::ok && record.has_value() &&
              (*record).state == ServiceStateCode::stopping,
          "stop", "another service stopped, answered with its record");
    const std::optional<ResponseView> unknown = ask(request_of(Command::stop, "absent"), web_pid);
    check(unknown.has_value() && (*unknown).status == Status::unknown_service, "stop", "unknown service");
}

void test_user_config() {
    std::optional<ResponseView> answer = ask(request_of(Command::config_read, ""), web_pid);
    std::optional<ConfigTextView> text = answer.has_value() ? decode_config_text((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::ok && text.has_value() &&
              (*text).source == ConfigSource::none && (*text).text.empty(),
          "user_config", "nothing at first");

    answer = ask(config_write_of("[system\n"), web_pid);
    std::optional<ConfigRefusal> refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::parse && (*refusal).line == 1u,
          "user_config", "a text that does not parse refused on its line");

    answer = ask(config_write_of("[system]\nhostname = a\n\n[service-web]\ncommand = /bin/sh\n"), web_pid);
    refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::user_rule &&
              (*refusal).code == static_cast<std::uint8_t>(stxconfig::UserRule::target_name_taken) &&
              (*refusal).line == 4u,
          "user_config", "a factory service's name refused by the user rules");

    answer = ask(config_write_of("[service-player]\ncommand = /bin/sleep 1\ncontrol = operate\n"), web_pid);
    refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::user_rule &&
              (*refusal).code == static_cast<std::uint8_t>(stxconfig::UserRule::target_control_not_allowed),
          "user_config", "a target with a control channel refused");

    answer = ask(config_write_of("[system]\nhostname = a\n[mounts]\nuser = /dev/da0\n"), web_pid);
    refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::user_rule &&
              (*refusal).code == static_cast<std::uint8_t>(stxconfig::UserRule::section_not_allowed) &&
              (*refusal).line == 3u,
          "user_config", "a section outside the user's refused on its header's line");

    answer = ask(config_write_of("[init]\nrun /bin/sh -c true\n"), web_pid);
    refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::user_rule &&
              (*refusal).code == static_cast<std::uint8_t>(stxconfig::UserRule::command_not_allowed),
          "user_config", "an init command other than start refused");

    // A user configuration's services are targets (user_config.h, rule 5).
    // The id is left out: the supervisor issues it (config/target_ids.h).
    const char* const good =
        "[system]\nhostname = stx\n\n[service-player]\ncommand = bin/player\nrole = server\n"
        "folder = /data/targets/player\n\n[init]\nstart player\n";
    const char* const stored =
        "[system]\nhostname = stx\n\n[service-player]\nid = 1\ncommand = bin/player\nrole = server\n"
        "folder = /data/targets/player\n\n[init]\nstart player\n";
    answer = ask(config_write_of(good), web_pid);
    const std::optional<ConfigWritten> written =
        answer.has_value() ? decode_config_written((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::ok && written.has_value() && (*written).sequence == 1u,
          "user_config", "a good text written as turn 1");

    answer = ask(request_of(Command::config_read, ""), reader_pid);
    text = answer.has_value() ? decode_config_text((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::ok && text.has_value() &&
              (*text).source == ConfigSource::file_0 && (*text).sequence == 1u && (*text).written_since_boot &&
              (*text).text.size() == std::strlen(stored) &&
              std::memcmp((*text).text.data(), stored, std::strlen(stored)) == 0,
          "user_config", "read back with the issued id 1, written since start");

    // An id the supervisor did not issue to that target is refused on its line.
    answer = ask(config_write_of("[service-player]\nid = 5\ncommand = bin/player\nrole = server\n"
                                 "folder = /data/targets/player\n"),
                 web_pid);
    refusal = answer.has_value() ? decode_config_refusal((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::config_refused && refusal.has_value() &&
              (*refusal).kind == RefusalKind::user_rule &&
              (*refusal).code == static_cast<std::uint8_t>(stxconfig::UserRule::target_id_not_issued) &&
              (*refusal).line == 2u,
          "user_config", "an id not issued refused");
    // The issued one is taken back as it is.
    answer = ask(config_write_of(stored), web_pid);
    check(answer.has_value() && (*answer).status == Status::ok, "user_config", "the issued id accepted");

    fake_system.set_fail_writes(true);
    answer = ask(config_write_of(good), web_pid);
    check(answer.has_value() && (*answer).status == Status::store_failed, "user_config", "a failed store answered");
    fake_system.set_fail_writes(false);
}

void test_reboot() {
    const std::optional<ResponseView> answer = ask(request_of(Command::reboot, ""), web_pid);
    check(answer.has_value() && (*answer).status == Status::ok && fake_system.reboots() == 1, "reboot",
          "asked through the system");
}

void test_programs() {
    std::optional<ResponseView> answer = ask(request_of(Command::program_state, "player"), web_pid);
    check(answer.has_value() && (*answer).status == Status::unsupported, "programs", "unsupported passed on");

    ProgramRecord record{};
    record.name = name_of("other");
    record.active = SlotCode::a;
    record.last_good = SlotCode::a;
    ProgramAnswer program{Status::ok, record};
    fake_system.set_program(program);
    answer = ask(request_of(Command::program_state, "player"), web_pid);
    check(answer.has_value() && (*answer).status == Status::unavailable, "programs",
          "a record about another target withheld");

    record.name = name_of("player");
    record.version_a.length = 3;
    std::memcpy(record.version_a.bytes, "1 0", 3);
    program.record = record;
    fake_system.set_program(program);
    answer = ask(request_of(Command::program_state, "player"), web_pid);
    check(answer.has_value() && (*answer).status == Status::unavailable, "programs",
          "a record that breaks the format withheld");

    const std::optional<ProgramVersion> version = program_version_of(text_of("1.0"));
    record.version_a = version.has_value() ? *version : ProgramVersion{};
    program.record = record;
    fake_system.set_program(program);
    answer = ask(request_of(Command::program_state, "player"), web_pid);
    const std::optional<ProgramRecord> decoded =
        answer.has_value() ? decode_program_record((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::ok && decoded.has_value() &&
              service_names_equal((*decoded).name, record.name),
          "programs", "a good record passed on");
    fake_system.set_program(program_answer(Status::unsupported));
}

void test_operation() {
    OperationRecord record = operation_none();
    record.state = OperationState::running;
    fake_system.set_operation(record);
    std::optional<ResponseView> answer = ask(request_of(Command::operation_state, ""), web_pid);
    check(answer.has_value() && (*answer).status == Status::unavailable, "operation",
          "an inconsistent record withheld");
    fake_system.set_operation(operation_none());
    answer = ask(request_of(Command::operation_state, ""), reader_pid);
    check(answer.has_value() && (*answer).status == Status::ok &&
              decode_operation_record((*answer).payload).has_value(),
          "operation", "no operation reported");
}

void test_whole_lines() {
    char window[7];
    for (std::size_t length = 0; length <= sizeof(window); length += 1) {
        for (std::uint32_t pattern = 0; pattern < (1u << length); pattern += 1) {
            for (std::size_t index = 0; index < length; index += 1) {
                window[index] = ((pattern >> index) & 1u) != 0u ? '\n' : 'a';
            }
            const std::span<const char> view(window, length);
            const std::span<const char> all = whole_lines_of_end(view, false);
            const std::span<const char> cut = whole_lines_of_end(view, true);
            std::size_t first_feed = length;
            for (std::size_t index = 0; index < length; index += 1) {
                if (window[index] == '\n') {
                    first_feed = index;
                    break;
                }
            }
            const std::size_t expected = first_feed == length ? 0u : length - first_feed - 1u;
            check(all.size() == length && all.data() == window, "whole_lines", "from the start: everything");
            check(cut.size() == expected && (expected == 0u || cut.data() == window + first_feed + 1u), "whole_lines",
                  "from inside: after the first line feed");
        }
    }
}

void test_last_good() {
    const std::optional<stxboot::Slot> slots[] = {std::nullopt, stxboot::Slot::a, stxboot::Slot::b};
    for (const std::optional<stxboot::Slot>& slot : slots) {
        for (std::uint32_t tries = 0; tries <= 2u; tries += 1) {
            for (int fallback = 0; fallback <= 1; fallback += 1) {
                for (int marked = 0; marked <= 1; marked += 1) {
                    stxboot::BootFacts boot{};
                    boot.slot = slot;
                    boot.tries = tries;
                    boot.fallback = fallback != 0;
                    const std::optional<stxboot::Slot> good = last_good_slot(boot, marked != 0);
                    if (!slot.has_value()) {
                        check(!good.has_value(), "last_good", "no booted slot: nothing");
                        continue;
                    }
                    const bool booted_is_good = marked != 0 || fallback != 0 || tries == 0u;
                    const stxboot::Slot other = *slot == stxboot::Slot::a ? stxboot::Slot::b : stxboot::Slot::a;
                    check(good.has_value() && *good == (booted_is_good ? *slot : other), "last_good",
                          "booted when good, else the other");
                }
            }
        }
    }
}

}  // namespace

// The target log tail (K17): whole lines only when the front was cut, the
// flags, unknown names refused, read access enough, and no service record
// pasted over the text.
void test_target_log_tail() {
    std::optional<ResponseView> answer = ask(request_of(Command::target_log_tail, "plain"), reader_pid);
    std::optional<BootLogTailView> tail =
        answer.has_value() ? decode_boot_log_tail((*answer).payload) : std::nullopt;
    const char* const expected = "whole one\n";
    check(answer.has_value() && (*answer).status == Status::ok && tail.has_value() && (*tail).present &&
              (*tail).front_cut && (*tail).text.size() == std::strlen(expected) &&
              std::memcmp((*tail).text.data(), expected, std::strlen(expected)) == 0,
          "target_log_tail", "whole lines after a cut front, read access enough");
    answer = ask(request_of(Command::target_log_tail, "sleeper"), reader_pid);
    tail = answer.has_value() ? decode_boot_log_tail((*answer).payload) : std::nullopt;
    check(answer.has_value() && (*answer).status == Status::ok && tail.has_value() && !(*tail).present &&
              (*tail).text.empty(),
          "target_log_tail", "nothing kept: absent");
    answer = ask(request_of(Command::target_log_tail, "absent"), reader_pid);
    check(answer.has_value() && (*answer).status == Status::unknown_service, "target_log_tail", "unknown name");
    answer = ask(request_of(Command::target_log_tail, "plain"), plain_pid);
    check(answer.has_value() && (*answer).status == Status::not_permitted, "target_log_tail",
          "a service without control refused");
}

int main() {
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(text_of(factory_text), factory);
    check(parsed.as_failure() == nullptr, "setup", "factory configuration parses");
    registry.load(factory);
    const char* const environment[] = {"PATH=/bin", nullptr};
    registry.set_environment(environment);
    const char* const names[] = {"web", "reader", "plain", "sleeper"};
    for (const char* const name : names) {
        check(registry.start(name, 0, actions) == stxconfig::StartResult::started, "setup", name);
    }
    static ControlServer server(registry, actions, fake_system, factory);
    server_pointer = &server;
    test_authority();
    test_stop_self();
    test_user_config();
    test_reboot();
    test_programs();
    test_target_log_tail();
    test_operation();
    test_whole_lines();
    test_last_good();
    return stxtest::finish("test_control_server");
}
