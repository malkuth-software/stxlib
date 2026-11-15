// test_control_protocol.cpp: host tests of the control channel's format
// (control/control_protocol.h).
//
// - every command's request round trip (its name when it takes one, its
//   payload by its rule), and a request that breaks a rule not encoded;
// - every single-byte change of every encoded request header and of a
//   response header, all 256 values at each position: either refused, or
//   decoded into something that encodes back to exactly the changed bytes
//   (the format has one encoding per value: no field is ignored);
// - every truncation of a request refused;
// - the same single-byte property for every record type (supervisor
//   state, service record, slot, config text header, config written,
//   config refusal, install source, program record, operation record);
// - record rules: stopped_by_request only with the stopped state, a
//   program version of visible ASCII up to 31 bytes, an install URL that is
//   https:// and visible ASCII, an operation record that is consistent.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../config/test/test_support.h"
#include "../control_protocol.h"

namespace {

using namespace stxcontrol;
using stxtest::check;
using stxtest::text_of;

std::uint8_t frame[request_frame_bytes_max + 1];
std::uint8_t mutated[request_frame_bytes_max + 1];
std::uint8_t again[request_frame_bytes_max + 1];

[[nodiscard]] std::span<const std::uint8_t> bytes_of(const char* text) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text));
}

[[nodiscard]] ServiceName name_of(const char* text) {
    const std::optional<ServiceName> name = service_name_of(text_of(text));
    return name.has_value() ? *name : ServiceName{};
}

// Every single-byte change of bytes[0, limit): refused, or canonical.
[[nodiscard]] bool request_mutations_canonical(std::size_t size, std::size_t limit) {
    for (std::size_t position = 0; position < limit; position += 1) {
        for (int value = 0; value < 256; value += 1) {
            if (frame[position] == value) {
                continue;
            }
            std::memcpy(mutated, frame, size);
            mutated[position] = static_cast<std::uint8_t>(value);
            const RequestOutcome outcome = decode_request(std::span<const std::uint8_t>(mutated, size));
            const Request* const request = outcome.as_request();
            if (request == nullptr) {
                continue;
            }
            const std::optional<std::size_t> encoded = encode_request(*request, again);
            if (!encoded.has_value() || *encoded != size || std::memcmp(again, mutated, size) != 0) {
                return false;
            }
        }
    }
    return true;
}

void test_requests() {
    std::uint8_t install[install_source_header_bytes + 64];
    const InstallSource staged{InstallKind::staged, std::span<const char>()};
    const std::optional<std::size_t> install_size = encode_install_source(staged, install);
    check(install_size.has_value(), "requests", "staged install source encodes");
    for (std::uint16_t code = command_first; code <= command_last; code += 1) {
        const std::optional<Command> command = command_from_code(code);
        check(command.has_value(), "requests", "every code in range is a command");
        if (!command.has_value()) {
            continue;
        }
        Request request = command_takes_name(*command) ? request_for_service(*command, 7u + code, name_of("web"))
                                                       : request_without_name(*command, 7u + code);
        switch (command_payload(*command)) {
        case PayloadRule::config_text:
            request = request_with_payload(request, bytes_of("[system]\nhostname = a\n"));
            break;
        case PayloadRule::install_source:
            request = request_with_payload(request, std::span<const std::uint8_t>(install, *install_size));
            break;
        case PayloadRule::none:
        default:
            break;
        }
        const std::optional<std::size_t> size = encode_request(request, frame);
        check(size.has_value(), "requests", command_name(*command));
        if (!size.has_value()) {
            continue;
        }
        const RequestOutcome outcome = decode_request(std::span<const std::uint8_t>(frame, *size));
        const Request* const decoded = outcome.as_request();
        check(decoded != nullptr && (*decoded).command == *command && (*decoded).request_id == 7u + code &&
                  service_names_equal((*decoded).name, request.name) &&
                  (*decoded).payload.size() == request.payload.size(),
              "requests", "round trip");
        check(request_mutations_canonical(*size, *size), "requests", "single-byte changes refused or canonical");
        bool truncations_refused = true;
        for (std::size_t length = 0; length < *size; length += 1) {
            const RequestOutcome cut = decode_request(std::span<const std::uint8_t>(frame, length));
            if (cut.as_request() != nullptr && command_payload(*command) == PayloadRule::none) {
                truncations_refused = false;
            }
        }
        check(truncations_refused, "requests", "truncations refused");
        // A payload where the rule admits none, and none where one is due.
        if (command_payload(*command) == PayloadRule::none) {
            const Request extra = request_with_payload(request, bytes_of("x"));
            check(!encode_request(extra, frame).has_value(), "requests", "payload refused where none is due");
        } else {
            const Request missing = request_with_payload(request, std::span<const std::uint8_t>());
            check(!encode_request(missing, frame).has_value(), "requests", "missing payload refused");
        }
    }
    // Names: present exactly when the command takes one.
    const Request unnamed = request_without_name(Command::start, 1);
    check(!encode_request(unnamed, frame).has_value(), "requests", "start without a name refused");
    const Request named = request_for_service(Command::supervisor_state, 1, name_of("web"));
    check(!encode_request(named, frame).has_value(), "requests", "supervisor_state with a name refused");
    // The largest configuration text fits; one byte more does not.
    static char text[config_text_max + 2];
    std::memset(text, 'a', sizeof(text));
    text[0] = '#';
    text[config_text_max - 1] = '\n';
    const Request largest = request_with_payload(request_without_name(Command::config_write, 1),
                                                 std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text),
                                                                               config_text_max));
    check(encode_request(largest, frame).has_value(), "requests", "the largest configuration text fits");
    const Request too_large = request_with_payload(request_without_name(Command::config_write, 1),
                                                   std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text),
                                                                                 config_text_max + 1u));
    check(!encode_request(too_large, frame).has_value(), "requests", "configuration text over the maximum refused");
}

void test_responses() {
    const ResponseHeader header{static_cast<std::uint16_t>(Command::list_services), 99u, Status::ok};
    const std::uint8_t payload[4] = {1, 0, 0, 0};
    const std::optional<std::size_t> size = encode_response(header, payload, frame);
    check(size.has_value() && *size == response_header_bytes + 4u, "responses", "encodes");
    const ResponseOutcome outcome = decode_response(std::span<const std::uint8_t>(frame, *size));
    const ResponseView* const view = outcome.as_view();
    check(view != nullptr && (*view).command == Command::list_services && (*view).request_id == 99u &&
              (*view).status == Status::ok && (*view).payload.size() == 4u,
          "responses", "round trip");
    bool canonical = true;
    for (std::size_t position = 0; position < response_header_bytes; position += 1) {
        for (int value = 0; value < 256; value += 1) {
            if (frame[position] == value) {
                continue;
            }
            std::memcpy(mutated, frame, *size);
            mutated[position] = static_cast<std::uint8_t>(value);
            const ResponseOutcome changed = decode_response(std::span<const std::uint8_t>(mutated, *size));
            const ResponseView* const changed_view = changed.as_view();
            if (changed_view == nullptr) {
                continue;
            }
            const ResponseHeader back{static_cast<std::uint16_t>((*changed_view).command), (*changed_view).request_id,
                                      (*changed_view).status};
            const std::optional<std::size_t> encoded = encode_response(back, (*changed_view).payload, again);
            if (!encoded.has_value() || *encoded != *size || std::memcmp(again, mutated, *size) != 0) {
                canonical = false;
            }
        }
    }
    check(canonical, "responses", "single-byte header changes refused or canonical");
    for (std::uint16_t code = 0; code <= status_last + 1u; code += 1) {
        const std::optional<Status> status = status_from_code(code);
        check(status.has_value() == (code <= status_last), "responses", "status codes in range only");
    }
}

// The single-byte property for one record type.
template <typename Record>
using Encoder = std::size_t (*)(const Record&, std::span<std::uint8_t>);
template <typename Record>
using Decoder = std::optional<Record> (*)(std::span<const std::uint8_t>);

template <typename Record>
[[nodiscard]] bool record_canonical(const Record& record, Encoder<Record> encode, Decoder<Record> decode) {
    std::uint8_t bytes[program_record_bytes + 8];
    std::uint8_t changed[sizeof(bytes)];
    std::uint8_t back[sizeof(bytes)];
    const std::size_t size = encode(record, bytes);
    if (size == 0u || !decode(std::span<const std::uint8_t>(bytes, size)).has_value()) {
        return false;
    }
    for (std::size_t position = 0; position < size; position += 1) {
        for (int value = 0; value < 256; value += 1) {
            if (bytes[position] == value) {
                continue;
            }
            std::memcpy(changed, bytes, size);
            changed[position] = static_cast<std::uint8_t>(value);
            const std::optional<Record> decoded = decode(std::span<const std::uint8_t>(changed, size));
            if (!decoded.has_value()) {
                continue;
            }
            const std::size_t encoded = encode(*decoded, back);
            if (encoded != size || std::memcmp(back, changed, size) != 0) {
                return false;
            }
        }
    }
    return true;
}

void test_records() {
    SupervisorStateRecord supervisor{};
    supervisor.uptime_ms = 123456u;
    supervisor.restart_count = 2u;
    supervisor.shim_registered = true;
    supervisor.init_done = true;
    supervisor.service_count = 3;
    check(record_canonical<SupervisorStateRecord>(supervisor, encode_supervisor_state, decode_supervisor_state),
          "records", "supervisor state");

    const ServiceStateCode states[] = {ServiceStateCode::stopped, ServiceStateCode::running, ServiceStateCode::waiting,
                                       ServiceStateCode::stopping};
    for (const ServiceStateCode state : states) {
        ServiceRecord service{};
        service.name = name_of("sleeper");
        service.state = state;
        service.restart = RestartCode::always;
        service.control = AccessCode::operate;
        service.stopped_by_request = state == ServiceStateCode::stopped;
        service.pid = state == ServiceStateCode::running ? 42 : 0;
        service.restart_count = 1;
        service.state_ms = 900;
        check(record_canonical<ServiceRecord>(service, encode_service_record, decode_service_record), "records",
              service_state_code_name(state));
    }
    // stopped_by_request with another state is not a record the
    // supervisor writes, and is refused.
    ServiceRecord inconsistent{};
    inconsistent.name = name_of("sleeper");
    inconsistent.state = ServiceStateCode::running;
    inconsistent.pid = 42;
    inconsistent.stopped_by_request = true;
    std::uint8_t record_bytes[service_record_bytes];
    const std::size_t record_size = encode_service_record(inconsistent, record_bytes);
    check(record_size == 0u || !decode_service_record(std::span<const std::uint8_t>(record_bytes, record_size)).has_value(),
          "records", "stopped_by_request only when stopped");

    SlotRecord slot{SlotCode::a, 2, false, true, SlotCode::a};
    check(record_canonical<SlotRecord>(slot, encode_slot_record, decode_slot_record), "records", "slot");

    ConfigWritten written{5u, 1u};
    check(record_canonical<ConfigWritten>(written, encode_config_written, decode_config_written), "records",
          "config written");
    ConfigRefusal refusal{3u, 7u, RefusalKind::user_rule, 2u};
    check(record_canonical<ConfigRefusal>(refusal, encode_config_refusal, decode_config_refusal), "records",
          "config refusal");

    ProgramRecord program{};
    program.name = name_of("player");
    program.active = SlotCode::b;
    program.last_good = SlotCode::a;
    program.on_trial = true;
    program.failed_starts = 1;
    const std::optional<ProgramVersion> version_a = program_version_of(text_of("1.0.3"));
    const std::optional<ProgramVersion> version_b = program_version_of(text_of("1.1.0-rc1"));
    check(version_a.has_value() && version_b.has_value(), "records", "versions of visible ASCII");
    if (version_a.has_value() && version_b.has_value()) {
        program.version_a = *version_a;
        program.version_b = *version_b;
    }
    check(record_canonical<ProgramRecord>(program, encode_program_record, decode_program_record), "records",
          "program record");
    check(!program_version_of(text_of("1.0 beta")).has_value(), "records", "a version with a space refused");
    check(!program_version_of(text_of("12345678901234567890123456789012")).has_value(), "records",
          "a version of 32 bytes refused");
    check(program_version_of(text_of("1234567890123456789012345678901")).has_value(), "records",
          "a version of 31 bytes accepted");

    OperationRecord operation = operation_none();
    check(operation_record_valid(operation), "records", "no operation is valid");
    check(record_canonical<OperationRecord>(operation, encode_operation_record, decode_operation_record), "records",
          "operation none");
    operation.kind = OperationKind::program_install;
    operation.state = OperationState::running;
    operation.result = Status::ok;
    operation.elapsed_ms = 1500;
    operation.name = name_of("player");
    check(operation_record_valid(operation), "records", "a running install is valid");
    check(record_canonical<OperationRecord>(operation, encode_operation_record, decode_operation_record), "records",
          "operation running");
}

void test_config_text_and_install() {
    std::uint8_t payload[config_text_header_bytes + 64];
    const std::size_t header = encode_config_text_header(ConfigSource::file_1, true, 9u, payload);
    const char* const text = "[system]\nhostname = a\n";
    std::memcpy(payload + header, text, std::strlen(text));
    const std::size_t size = header + std::strlen(text);
    const std::optional<ConfigTextView> view = decode_config_text(std::span<const std::uint8_t>(payload, size));
    check(view.has_value() && (*view).source == ConfigSource::file_1 && (*view).written_since_boot &&
              (*view).sequence == 9u && (*view).text.size() == std::strlen(text),
          "config_text", "round trip");
    const std::size_t none_header = encode_config_text_header(ConfigSource::none, false, 0u, payload);
    check(decode_config_text(std::span<const std::uint8_t>(payload, none_header)).has_value(), "config_text",
          "no configuration: empty text");
    const std::size_t bad_header = encode_config_text_header(ConfigSource::file_0, false, 0u, payload);
    check(!decode_config_text(std::span<const std::uint8_t>(payload, bad_header)).has_value(), "config_text",
          "a file with sequence 0 refused");

    std::uint8_t install[install_source_header_bytes + install_url_bytes_max + 2];
    const char* const urls_good[] = {"https://example.org/p.pkg", "https://h/x"};
    const char* const urls_bad[] = {"http://example.org/p.pkg", "https://exa mple.org/", "", "https://\x7f"};
    for (const char* const url : urls_good) {
        const std::optional<std::size_t> encoded =
            encode_install_source(InstallSource{InstallKind::url, text_of(url)}, install);
        check(encoded.has_value() &&
                  decode_install_source(std::span<const std::uint8_t>(install, *encoded)).has_value(),
              "install", url);
    }
    for (const char* const url : urls_bad) {
        const std::optional<std::size_t> encoded =
            encode_install_source(InstallSource{InstallKind::url, text_of(url)}, install);
        check(!encoded.has_value(), "install", "bad URL refused");
    }
    static char long_url[install_url_bytes_max + 2];
    std::memset(long_url, 'a', sizeof(long_url));
    std::memcpy(long_url, "https://", 8);
    const std::optional<std::size_t> at_limit = encode_install_source(
        InstallSource{InstallKind::url, std::span<const char>(long_url, install_url_bytes_max)}, install);
    check(at_limit.has_value(), "install", "URL at the limit accepted");
    const std::optional<std::size_t> over_limit = encode_install_source(
        InstallSource{InstallKind::url, std::span<const char>(long_url, install_url_bytes_max + 1u)}, install);
    check(!over_limit.has_value(), "install", "URL over the limit refused");
}

}  // namespace

int main() {
    test_requests();
    test_responses();
    test_records();
    test_config_text_and_install();
    return stxtest::finish("test_control_protocol");
}
