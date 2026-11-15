// control_protocol.cpp: the control channel's message format
// (control_protocol.h).

#include "control_protocol.h"

#include <cassert>

namespace stxcontrol {

namespace {

// ---------------------------------------------------------------------------
// Little-endian fields, byte by byte.

void put_u16(std::span<std::uint8_t> out, std::size_t at, std::uint16_t value) noexcept {
    out[at] = static_cast<std::uint8_t>(value & 0xffu);
    out[at + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void put_u32(std::span<std::uint8_t> out, std::size_t at, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4u; index += 1) {
        out[at + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xffu);
    }
}

void put_u64(std::span<std::uint8_t> out, std::size_t at, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8u; index += 1) {
        out[at + index] = static_cast<std::uint8_t>((value >> (8u * index)) & 0xffu);
    }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    const std::uint32_t low = bytes[at];
    const std::uint32_t high = bytes[at + 1u];
    return static_cast<std::uint16_t>(low | (high << 8u));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4u; index += 1) {
        value |= static_cast<std::uint32_t>(bytes[at + index]) << (8u * index);
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8u; index += 1) {
        value |= static_cast<std::uint64_t>(bytes[at + index]) << (8u * index);
    }
    return value;
}

void zero_bytes(std::span<std::uint8_t> out, std::size_t at, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; index += 1) {
        out[at + index] = 0u;
    }
}

[[nodiscard]] bool bytes_zero(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; index += 1) {
        if (bytes[at + index] != 0u) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_lower(char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] bool is_name_byte(char byte) noexcept {
    return is_lower(byte) || (byte >= '0' && byte <= '9') || byte == '-';
}

// Name field: length byte at `at`, 32 name bytes at `at + name_offset`.
void put_name(std::span<std::uint8_t> out, std::size_t length_at, std::size_t name_at, const ServiceName& name) noexcept {
    out[length_at] = name.length;
    for (std::size_t index = 0; index < service_name_bytes_max; index += 1) {
        const bool used = index < name.length;
        out[name_at + index] = used ? static_cast<std::uint8_t>(name.bytes[index]) : 0u;
    }
}

enum class NameField : std::uint8_t { valid, empty, too_long, invalid };

// Reads a name field strictly: length within bounds, the used bytes a valid
// name, the rest zero.
[[nodiscard]] NameField get_name(std::span<const std::uint8_t> bytes, std::size_t length_at, std::size_t name_at,
                                 ServiceName& name) noexcept {
    const std::uint8_t length = bytes[length_at];
    name.length = 0;
    for (std::size_t index = 0; index < service_name_bytes_max; index += 1) {
        name.bytes[index] = '\0';
    }
    if (length > service_name_bytes_max) {
        return NameField::too_long;
    }
    if (!bytes_zero(bytes, name_at + length, service_name_bytes_max - length)) {
        return NameField::invalid;
    }
    if (length == 0u) {
        return NameField::empty;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        name.bytes[index] = static_cast<char>(bytes[name_at + index]);
    }
    const std::span<const char> text(name.bytes, length);
    if (!service_name_valid(text)) {
        return NameField::invalid;
    }
    name.length = length;
    return NameField::valid;
}

constexpr std::size_t offset_magic = 0;
constexpr std::size_t offset_version = 4;
constexpr std::size_t offset_command = 6;
constexpr std::size_t offset_request_id = 8;
constexpr std::size_t offset_name_length = 12;
constexpr std::size_t offset_request_reserved = 13;
constexpr std::size_t offset_request_payload_length = 14;
constexpr std::size_t offset_name = 16;
constexpr std::size_t offset_status = 12;
constexpr std::size_t offset_payload_length = 14;

static_assert(offset_name + service_name_bytes_max == request_header_bytes, "request layout");

// A byte the configuration format allows (printable ASCII, space, tab) or
// a line end. The parser refuses a lone CR and everything else in detail;
// the protocol only keeps bytes outside that set off the channel.
[[nodiscard]] bool is_config_byte(std::uint8_t byte) noexcept {
    return (byte >= 0x20u && byte <= 0x7eu) || byte == '\t' || byte == '\n' || byte == '\r';
}

[[nodiscard]] bool is_config_text(std::span<const std::uint8_t> bytes) noexcept {
    for (const std::uint8_t byte : bytes) {
        if (!is_config_byte(byte)) {
            return false;
        }
    }
    return true;
}

// Printable ASCII without space: URLs and versions.
[[nodiscard]] bool is_visible(std::uint8_t byte) noexcept {
    return byte >= 0x21u && byte <= 0x7eu;
}

}  // namespace

// ---------------------------------------------------------------------------
// Commands and statuses.

std::optional<Command> command_from_code(std::uint16_t code) noexcept {
    if (code < command_first || code > command_last) {
        return std::nullopt;
    }
    const std::optional<Command> command = static_cast<Command>(code);
    return command;
}

const char* command_name(Command command) noexcept {
    switch (command) {
    case Command::supervisor_state: return "supervisor-state";
    case Command::list_services: return "list-services";
    case Command::service_state: return "service-state";
    case Command::start: return "start";
    case Command::stop: return "stop";
    case Command::restart: return "restart";
    case Command::slot_status: return "slot-status";
    case Command::boot_log_tail: return "boot-log-tail";
    case Command::config_read: return "config-read";
    case Command::config_write: return "config-write";
    case Command::reboot: return "reboot";
    case Command::program_state: return "program-state";
    case Command::program_install: return "program-install";
    case Command::program_rollback: return "program-rollback";
    case Command::program_check: return "program-check";
    case Command::system_update: return "system-update";
    case Command::operation_state: return "operation-state";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

bool command_takes_name(Command command) noexcept {
    switch (command) {
    case Command::service_state:
    case Command::start:
    case Command::stop:
    case Command::restart:
    case Command::program_state:
    case Command::program_install:
    case Command::program_rollback:
    case Command::program_check:
        return true;
    case Command::supervisor_state:
    case Command::list_services:
    case Command::slot_status:
    case Command::boot_log_tail:
    case Command::config_read:
    case Command::config_write:
    case Command::reboot:
    case Command::system_update:
    case Command::operation_state:
        return false;
    default:
        break;
    }
    // Fallback: an unknown command takes no name (decoding refuses it first).
    return false;
}

bool command_operates(Command command) noexcept {
    switch (command) {
    case Command::start:
    case Command::stop:
    case Command::restart:
    case Command::config_write:
    case Command::reboot:
    case Command::program_install:
    case Command::program_rollback:
    case Command::program_check:
    case Command::system_update:
        return true;
    case Command::supervisor_state:
    case Command::list_services:
    case Command::service_state:
    case Command::slot_status:
    case Command::boot_log_tail:
    case Command::config_read:
    case Command::program_state:
    case Command::operation_state:
        return false;
    default:
        break;
    }
    // Fallback: an unknown command is treated as operating, the stricter
    // authority.
    return true;
}

PayloadRule command_payload(Command command) noexcept {
    switch (command) {
    case Command::config_write:
        return PayloadRule::config_text;
    case Command::program_install:
        return PayloadRule::install_source;
    case Command::supervisor_state:
    case Command::list_services:
    case Command::service_state:
    case Command::start:
    case Command::stop:
    case Command::restart:
    case Command::slot_status:
    case Command::boot_log_tail:
    case Command::config_read:
    case Command::reboot:
    case Command::program_state:
    case Command::program_rollback:
    case Command::program_check:
    case Command::system_update:
    case Command::operation_state:
        return PayloadRule::none;
    default:
        break;
    }
    // Fallback: an unknown command carries nothing.
    return PayloadRule::none;
}

std::optional<Status> status_from_code(std::uint16_t code) noexcept {
    if (code > status_last) {
        return std::nullopt;
    }
    const std::optional<Status> status = static_cast<Status>(code);
    return status;
}

const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::malformed: return "malformed";
    case Status::not_permitted: return "not_permitted";
    case Status::unknown_service: return "unknown_service";
    case Status::already_running: return "already_running";
    case Status::not_running: return "not_running";
    case Status::spawn_failed: return "spawn_failed";
    case Status::refused_self: return "refused_self";
    case Status::unavailable: return "unavailable";
    case Status::config_refused: return "config_refused";
    case Status::unsupported: return "unsupported";
    case Status::store_failed: return "store_failed";
    case Status::busy: return "busy";
    case Status::signature_refused: return "signature_refused";
    case Status::source_unavailable: return "source_unavailable";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

// ---------------------------------------------------------------------------
// Names.

bool service_name_valid(std::span<const char> name) noexcept {
    if (name.empty() || name.size() > service_name_bytes_max || !is_lower(name[0])) {
        return false;
    }
    for (const char byte : name) {
        if (!is_name_byte(byte)) {
            return false;
        }
    }
    return true;
}

std::optional<ServiceName> service_name_of(std::span<const char> name) noexcept {
    if (!service_name_valid(name)) {
        return std::nullopt;
    }
    ServiceName result{};
    result.length = static_cast<std::uint8_t>(name.size());
    for (std::size_t index = 0; index < service_name_bytes_max; index += 1) {
        result.bytes[index] = index < name.size() ? name[index] : '\0';
    }
    const std::optional<ServiceName> found = result;
    return found;
}

void service_name_c_string(const ServiceName& name, char (&destination)[service_name_bytes_max + 1]) noexcept {
    const std::size_t length = name.length <= service_name_bytes_max ? name.length : 0u;
    for (std::size_t index = 0; index < length; index += 1) {
        destination[index] = name.bytes[index];
    }
    destination[length] = '\0';
}

bool service_names_equal(const ServiceName& left, const ServiceName& right) noexcept {
    if (left.length != right.length || left.length > service_name_bytes_max) {
        return false;
    }
    for (std::size_t index = 0; index < left.length; index += 1) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Requests.

Request request_without_name(Command command, std::uint32_t request_id) noexcept {
    assert(!command_takes_name(command));
    Request request{};
    request.command = command;
    request.request_id = request_id;
    request.name.length = 0;
    for (std::size_t index = 0; index < service_name_bytes_max; index += 1) {
        request.name.bytes[index] = '\0';
    }
    request.payload = std::span<const std::uint8_t>();
    return request;
}

Request request_for_service(Command command, std::uint32_t request_id, const ServiceName& name) noexcept {
    assert(command_takes_name(command));
    Request request{};
    request.command = command;
    request.request_id = request_id;
    request.name = name;
    request.payload = std::span<const std::uint8_t>();
    return request;
}

Request request_with_payload(const Request& request, std::span<const std::uint8_t> payload) noexcept {
    Request result = request;
    result.payload = payload;
    return result;
}

bool payload_fits_rule(PayloadRule rule, std::span<const std::uint8_t> payload) noexcept {
    switch (rule) {
    case PayloadRule::none:
        return payload.empty();
    case PayloadRule::config_text:
        return !payload.empty() && payload.size() <= config_text_max && is_config_text(payload);
    case PayloadRule::install_source:
        return decode_install_source(payload).has_value();
    default:
        break;
    }
    // Fallback: an unknown rule admits nothing.
    return false;
}

std::optional<std::size_t> encode_request(const Request& request, std::span<std::uint8_t> frame) noexcept {
    if (command_takes_name(request.command) != (request.name.length > 0u) ||
        request.name.length > service_name_bytes_max ||
        (request.name.length > 0u &&
         !service_name_valid(std::span<const char>(request.name.bytes, request.name.length))) ||
        !payload_fits_rule(command_payload(request.command), request.payload) ||
        frame.size() < request_header_bytes || frame.size() - request_header_bytes < request.payload.size()) {
        return std::nullopt;
    }
    put_u32(frame, offset_magic, frame_magic);
    put_u16(frame, offset_version, protocol_version);
    put_u16(frame, offset_command, static_cast<std::uint16_t>(request.command));
    put_u32(frame, offset_request_id, request.request_id);
    frame[offset_request_reserved] = 0u;
    put_u16(frame, offset_request_payload_length, static_cast<std::uint16_t>(request.payload.size()));
    put_name(frame, offset_name_length, offset_name, request.name);
    for (std::size_t index = 0; index < request.payload.size(); index += 1) {
        frame[request_header_bytes + index] = request.payload[index];
    }
    const std::optional<std::size_t> size = request_header_bytes + request.payload.size();
    return size;
}

const char* request_error_name(RequestError error) noexcept {
    switch (error) {
    case RequestError::wrong_size: return "wrong_size";
    case RequestError::wrong_magic: return "wrong_magic";
    case RequestError::wrong_version: return "wrong_version";
    case RequestError::unknown_command: return "unknown_command";
    case RequestError::name_too_long: return "name_too_long";
    case RequestError::name_invalid: return "name_invalid";
    case RequestError::name_missing: return "name_missing";
    case RequestError::name_unexpected: return "name_unexpected";
    case RequestError::reserved_not_zero: return "reserved_not_zero";
    case RequestError::payload_refused: return "payload_refused";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

RequestOutcome::RequestOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

RequestOutcome RequestOutcome::decoded(const Request& request) noexcept {
    const RequestOutcome outcome(Alternatives(std::in_place_type<Request>, request));
    return outcome;
}

RequestOutcome RequestOutcome::refused(const RequestRefused& refusal) noexcept {
    const RequestOutcome outcome(Alternatives(std::in_place_type<RequestRefused>, refusal));
    return outcome;
}

const Request* RequestOutcome::as_request() const noexcept {
    return std::get_if<Request>(&alternatives_);
}

const RequestRefused* RequestOutcome::as_refused() const noexcept {
    return std::get_if<RequestRefused>(&alternatives_);
}

RequestOutcome decode_request(std::span<const std::uint8_t> frame) noexcept {
    RequestRefused refusal{};
    refusal.request_id = frame.size() >= offset_request_id + 4u ? get_u32(frame, offset_request_id) : 0u;
    refusal.command_code = frame.size() >= offset_command + 2u ? get_u16(frame, offset_command) : 0u;
    if (frame.size() < request_header_bytes || frame.size() > request_frame_bytes_max ||
        frame.size() != request_header_bytes + get_u16(frame, offset_request_payload_length)) {
        refusal.error = RequestError::wrong_size;
        return RequestOutcome::refused(refusal);
    }
    if (get_u32(frame, offset_magic) != frame_magic) {
        refusal.error = RequestError::wrong_magic;
        return RequestOutcome::refused(refusal);
    }
    if (get_u16(frame, offset_version) != protocol_version) {
        refusal.error = RequestError::wrong_version;
        return RequestOutcome::refused(refusal);
    }
    const std::optional<Command> command = command_from_code(refusal.command_code);
    if (!command.has_value()) {
        refusal.error = RequestError::unknown_command;
        return RequestOutcome::refused(refusal);
    }
    if (frame[offset_request_reserved] != 0u) {
        refusal.error = RequestError::reserved_not_zero;
        return RequestOutcome::refused(refusal);
    }
    Request request{};
    request.command = *command;
    request.request_id = refusal.request_id;
    request.payload = frame.subspan(request_header_bytes);
    const NameField name = get_name(frame, offset_name_length, offset_name, request.name);
    const bool takes_name = command_takes_name(*command);
    bool name_fits = false;
    switch (name) {
    case NameField::too_long:
        refusal.error = RequestError::name_too_long;
        return RequestOutcome::refused(refusal);
    case NameField::invalid:
        refusal.error = RequestError::name_invalid;
        return RequestOutcome::refused(refusal);
    case NameField::empty:
        refusal.error = RequestError::name_missing;
        name_fits = !takes_name;
        break;
    case NameField::valid:
        refusal.error = RequestError::name_unexpected;
        name_fits = takes_name;
        break;
    default:
        // Fallback: a name field in no known state is invalid.
        refusal.error = RequestError::name_invalid;
        name_fits = false;
        break;
    }
    if (!name_fits) {
        return RequestOutcome::refused(refusal);
    }
    if (!payload_fits_rule(command_payload(*command), request.payload)) {
        refusal.error = RequestError::payload_refused;
        return RequestOutcome::refused(refusal);
    }
    return RequestOutcome::decoded(request);
}

// ---------------------------------------------------------------------------
// Responses.

std::optional<std::size_t> encode_response(const ResponseHeader& header, std::span<const std::uint8_t> payload,
                                           std::span<std::uint8_t> frame) noexcept {
    if (payload.size() > response_payload_max || frame.size() < response_header_bytes ||
        frame.size() - response_header_bytes < payload.size()) {
        return std::nullopt;
    }
    put_u32(frame, offset_magic, frame_magic);
    put_u16(frame, offset_version, protocol_version);
    put_u16(frame, offset_command, header.command_code);
    put_u32(frame, offset_request_id, header.request_id);
    put_u16(frame, offset_status, static_cast<std::uint16_t>(header.status));
    put_u16(frame, offset_payload_length, static_cast<std::uint16_t>(payload.size()));
    for (std::size_t index = 0; index < payload.size(); index += 1) {
        frame[response_header_bytes + index] = payload[index];
    }
    const std::optional<std::size_t> size = response_header_bytes + payload.size();
    return size;
}

const char* response_error_name(ResponseError error) noexcept {
    switch (error) {
    case ResponseError::too_short: return "too_short";
    case ResponseError::wrong_magic: return "wrong_magic";
    case ResponseError::wrong_version: return "wrong_version";
    case ResponseError::unknown_command: return "unknown_command";
    case ResponseError::unknown_status: return "unknown_status";
    case ResponseError::wrong_length: return "wrong_length";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

ResponseOutcome::ResponseOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ResponseOutcome ResponseOutcome::decoded(const ResponseView& view) noexcept {
    const ResponseOutcome outcome(Alternatives(std::in_place_type<ResponseView>, view));
    return outcome;
}

ResponseOutcome ResponseOutcome::refused(ResponseError error) noexcept {
    const ResponseOutcome outcome(Alternatives(std::in_place_type<ResponseError>, error));
    return outcome;
}

const ResponseView* ResponseOutcome::as_view() const noexcept {
    return std::get_if<ResponseView>(&alternatives_);
}

const ResponseError* ResponseOutcome::as_refused() const noexcept {
    return std::get_if<ResponseError>(&alternatives_);
}

ResponseOutcome decode_response(std::span<const std::uint8_t> frame) noexcept {
    if (frame.size() < response_header_bytes) {
        return ResponseOutcome::refused(ResponseError::too_short);
    }
    if (get_u32(frame, offset_magic) != frame_magic) {
        return ResponseOutcome::refused(ResponseError::wrong_magic);
    }
    if (get_u16(frame, offset_version) != protocol_version) {
        return ResponseOutcome::refused(ResponseError::wrong_version);
    }
    const std::optional<Command> command = command_from_code(get_u16(frame, offset_command));
    if (!command.has_value()) {
        return ResponseOutcome::refused(ResponseError::unknown_command);
    }
    const std::optional<Status> status = status_from_code(get_u16(frame, offset_status));
    if (!status.has_value()) {
        return ResponseOutcome::refused(ResponseError::unknown_status);
    }
    const std::size_t payload_length = get_u16(frame, offset_payload_length);
    if (payload_length > response_payload_max || frame.size() != response_header_bytes + payload_length) {
        return ResponseOutcome::refused(ResponseError::wrong_length);
    }
    ResponseView view{};
    view.command = *command;
    view.request_id = get_u32(frame, offset_request_id);
    view.status = *status;
    view.payload = frame.subspan(response_header_bytes, payload_length);
    return ResponseOutcome::decoded(view);
}

// ---------------------------------------------------------------------------
// SupervisorStateRecord.

namespace {

constexpr std::uint8_t state_flag_registered = 0x01u;
constexpr std::uint8_t state_flag_init_done = 0x02u;
constexpr std::uint8_t state_flag_failsafe = 0x04u;
constexpr std::uint8_t state_flag_marked_good = 0x08u;
constexpr std::uint8_t state_flags_known = 0x0fu;

}  // namespace

std::size_t encode_supervisor_state(const SupervisorStateRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= supervisor_state_record_bytes);
    assert(record.service_count <= services_listed_max);
    put_u64(out, 0, record.uptime_ms);
    std::uint32_t restart_count = restart_count_unknown;
    if (record.restart_count.has_value()) {
        restart_count = *record.restart_count < restart_count_unknown ? *record.restart_count
                                                                      : restart_count_unknown - 1u;
    }
    put_u32(out, 8, restart_count);
    std::uint8_t flags = 0;
    flags = static_cast<std::uint8_t>(flags | (record.shim_registered ? state_flag_registered : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.init_done ? state_flag_init_done : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.failsafe_taken ? state_flag_failsafe : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.boot_marked_good ? state_flag_marked_good : 0u));
    out[12] = flags;
    out[13] = record.service_count;
    zero_bytes(out, 14, 2u);
    return supervisor_state_record_bytes;
}

std::optional<SupervisorStateRecord> decode_supervisor_state(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != supervisor_state_record_bytes) {
        return std::nullopt;
    }
    const std::uint8_t flags = bytes[12];
    if ((flags & static_cast<std::uint8_t>(~state_flags_known)) != 0u || bytes[13] > services_listed_max ||
        !bytes_zero(bytes, 14, 2u)) {
        return std::nullopt;
    }
    SupervisorStateRecord record{};
    record.uptime_ms = get_u64(bytes, 0);
    const std::uint32_t restart_count = get_u32(bytes, 8);
    if (restart_count != restart_count_unknown) {
        record.restart_count = restart_count;
    }
    record.shim_registered = (flags & state_flag_registered) != 0u;
    record.init_done = (flags & state_flag_init_done) != 0u;
    record.failsafe_taken = (flags & state_flag_failsafe) != 0u;
    record.boot_marked_good = (flags & state_flag_marked_good) != 0u;
    record.service_count = bytes[13];
    const std::optional<SupervisorStateRecord> decoded = record;
    return decoded;
}

// ---------------------------------------------------------------------------
// ServiceRecord and ServiceList.

const char* service_state_code_name(ServiceStateCode code) noexcept {
    switch (code) {
    case ServiceStateCode::stopped: return "stopped";
    case ServiceStateCode::running: return "running";
    case ServiceStateCode::waiting: return "waiting";
    case ServiceStateCode::stopping: return "stopping";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

const char* restart_code_name(RestartCode code) noexcept {
    switch (code) {
    case RestartCode::never: return "never";
    case RestartCode::on_failure: return "on-failure";
    case RestartCode::always: return "always";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

const char* access_code_name(AccessCode code) noexcept {
    switch (code) {
    case AccessCode::none: return "none";
    case AccessCode::read: return "read";
    case AccessCode::operate: return "operate";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

constexpr std::size_t record_name_length = 0;
constexpr std::size_t record_name = 1;
constexpr std::size_t record_state = 33;
constexpr std::size_t record_restart = 34;
constexpr std::size_t record_control = 35;
constexpr std::size_t record_flags = 36;
constexpr std::size_t record_reserved = 37;
constexpr std::size_t record_pid = 40;
constexpr std::size_t record_restart_count = 44;
constexpr std::size_t record_state_ms = 48;
constexpr std::uint8_t record_flag_restart_pending = 0x01u;
constexpr std::uint8_t record_flag_stopped_by_request = 0x02u;
constexpr std::uint8_t record_flags_known = 0x03u;

static_assert(record_state_ms + 8u == service_record_bytes, "service record layout");

}  // namespace

std::size_t encode_service_record(const ServiceRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= service_record_bytes);
    assert(record.name.length >= 1u && record.name.length <= service_name_bytes_max);
    put_name(out, record_name_length, record_name, record.name);
    out[record_state] = static_cast<std::uint8_t>(record.state);
    out[record_restart] = static_cast<std::uint8_t>(record.restart);
    out[record_control] = static_cast<std::uint8_t>(record.control);
    std::uint8_t flags = 0;
    flags = static_cast<std::uint8_t>(flags | (record.restart_pending ? record_flag_restart_pending : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.stopped_by_request ? record_flag_stopped_by_request : 0u));
    out[record_flags] = flags;
    zero_bytes(out, record_reserved, 3u);
    put_u32(out, record_pid, static_cast<std::uint32_t>(record.pid));
    put_u32(out, record_restart_count, record.restart_count);
    put_u64(out, record_state_ms, record.state_ms);
    return service_record_bytes;
}

std::optional<ServiceRecord> decode_service_record(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != service_record_bytes) {
        return std::nullopt;
    }
    ServiceRecord record{};
    if (get_name(bytes, record_name_length, record_name, record.name) != NameField::valid) {
        return std::nullopt;
    }
    if (bytes[record_state] > static_cast<std::uint8_t>(ServiceStateCode::stopping) ||
        bytes[record_restart] > static_cast<std::uint8_t>(RestartCode::always) ||
        bytes[record_control] > static_cast<std::uint8_t>(AccessCode::operate) ||
        (bytes[record_flags] & static_cast<std::uint8_t>(~record_flags_known)) != 0u ||
        !bytes_zero(bytes, record_reserved, 3u)) {
        return std::nullopt;
    }
    record.state = static_cast<ServiceStateCode>(bytes[record_state]);
    record.restart = static_cast<RestartCode>(bytes[record_restart]);
    record.control = static_cast<AccessCode>(bytes[record_control]);
    record.restart_pending = (bytes[record_flags] & record_flag_restart_pending) != 0u;
    record.stopped_by_request = (bytes[record_flags] & record_flag_stopped_by_request) != 0u;
    // Each flag belongs to one state: a pending restart to stopping, a stop
    // by request to stopped.
    if ((record.restart_pending && record.state != ServiceStateCode::stopping) ||
        (record.stopped_by_request && record.state != ServiceStateCode::stopped)) {
        return std::nullopt;
    }
    record.pid = static_cast<std::int32_t>(get_u32(bytes, record_pid));
    record.restart_count = get_u32(bytes, record_restart_count);
    record.state_ms = get_u64(bytes, record_state_ms);
    const std::optional<ServiceRecord> decoded = record;
    return decoded;
}

std::size_t encode_service_list_header(std::uint8_t count, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= service_list_header_bytes);
    assert(count <= services_listed_max);
    out[0] = count;
    zero_bytes(out, 1, 3u);
    return service_list_header_bytes;
}

std::optional<std::uint8_t> decode_service_list_count(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < service_list_header_bytes || payload[0] > services_listed_max ||
        !bytes_zero(payload, 1, 3u)) {
        return std::nullopt;
    }
    const std::uint8_t count = payload[0];
    if (payload.size() != service_list_header_bytes + static_cast<std::size_t>(count) * service_record_bytes) {
        return std::nullopt;
    }
    const std::optional<std::uint8_t> found = count;
    return found;
}

std::span<const std::uint8_t> service_list_record(std::span<const std::uint8_t> payload, std::size_t index) noexcept {
    assert(index < services_listed_max);
    assert(service_list_header_bytes + (index + 1u) * service_record_bytes <= payload.size());
    return payload.subspan(service_list_header_bytes + index * service_record_bytes, service_record_bytes);
}

// ---------------------------------------------------------------------------
// SlotRecord.

const char* slot_code_name(SlotCode code) noexcept {
    switch (code) {
    case SlotCode::none: return "none";
    case SlotCode::a: return "A";
    case SlotCode::b: return "B";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

constexpr std::uint8_t slot_flag_fallback = 0x01u;
constexpr std::uint8_t slot_flag_marked_good = 0x02u;
constexpr std::uint8_t slot_flags_known = 0x03u;

}  // namespace

std::size_t encode_slot_record(const SlotRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= slot_record_bytes);
    out[0] = static_cast<std::uint8_t>(record.booted);
    out[1] = record.tries;
    std::uint8_t flags = 0;
    flags = static_cast<std::uint8_t>(flags | (record.fallback ? slot_flag_fallback : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.marked_good ? slot_flag_marked_good : 0u));
    out[2] = flags;
    out[3] = static_cast<std::uint8_t>(record.last_good);
    zero_bytes(out, 4, 4u);
    return slot_record_bytes;
}

std::optional<SlotRecord> decode_slot_record(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != slot_record_bytes) {
        return std::nullopt;
    }
    const std::uint8_t slot_max = static_cast<std::uint8_t>(SlotCode::b);
    if (bytes[0] > slot_max || bytes[3] > slot_max ||
        (bytes[2] & static_cast<std::uint8_t>(~slot_flags_known)) != 0u || !bytes_zero(bytes, 4, 4u)) {
        return std::nullopt;
    }
    SlotRecord record{};
    record.booted = static_cast<SlotCode>(bytes[0]);
    record.tries = bytes[1];
    record.fallback = (bytes[2] & slot_flag_fallback) != 0u;
    record.marked_good = (bytes[2] & slot_flag_marked_good) != 0u;
    record.last_good = static_cast<SlotCode>(bytes[3]);
    const std::optional<SlotRecord> decoded = record;
    return decoded;
}

// ---------------------------------------------------------------------------
// BootLogTail.

namespace {

constexpr std::uint8_t log_flag_present = 0x01u;
constexpr std::uint8_t log_flag_front_cut = 0x02u;
constexpr std::uint8_t log_flags_known = 0x03u;

}  // namespace

std::size_t encode_boot_log_header(bool present, bool front_cut, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= boot_log_header_bytes);
    std::uint8_t flags = 0;
    flags = static_cast<std::uint8_t>(flags | (present ? log_flag_present : 0u));
    flags = static_cast<std::uint8_t>(flags | (front_cut ? log_flag_front_cut : 0u));
    out[0] = flags;
    zero_bytes(out, 1, 3u);
    return boot_log_header_bytes;
}

std::optional<BootLogTailView> decode_boot_log_tail(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < boot_log_header_bytes || payload.size() - boot_log_header_bytes > boot_log_text_max) {
        return std::nullopt;
    }
    const std::uint8_t flags = payload[0];
    if ((flags & static_cast<std::uint8_t>(~log_flags_known)) != 0u || !bytes_zero(payload, 1, 3u)) {
        return std::nullopt;
    }
    BootLogTailView view{};
    view.present = (flags & log_flag_present) != 0u;
    view.front_cut = (flags & log_flag_front_cut) != 0u;
    view.text = payload.subspan(boot_log_header_bytes);
    // A log that does not exist has no text, and its front cannot be cut.
    if (!view.present && (view.front_cut || !view.text.empty())) {
        return std::nullopt;
    }
    const std::optional<BootLogTailView> decoded = view;
    return decoded;
}

// ---------------------------------------------------------------------------
// ConfigText, ConfigWritten, ConfigRefusal.

const char* config_source_name(ConfigSource source) noexcept {
    switch (source) {
    case ConfigSource::none: return "none";
    case ConfigSource::file_0: return "file-0";
    case ConfigSource::file_1: return "file-1";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

namespace {

constexpr std::uint8_t config_flag_written = 0x01u;

}  // namespace

std::size_t encode_config_text_header(ConfigSource source, bool written_since_boot, std::uint32_t sequence,
                                      std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= config_text_header_bytes);
    out[0] = static_cast<std::uint8_t>(source);
    out[1] = written_since_boot ? config_flag_written : 0u;
    zero_bytes(out, 2, 2u);
    put_u32(out, 4, sequence);
    return config_text_header_bytes;
}

std::optional<ConfigTextView> decode_config_text(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < config_text_header_bytes || payload.size() - config_text_header_bytes > config_text_max) {
        return std::nullopt;
    }
    if (payload[0] > static_cast<std::uint8_t>(ConfigSource::file_1) ||
        (payload[1] & static_cast<std::uint8_t>(~config_flag_written)) != 0u || !bytes_zero(payload, 2, 2u)) {
        return std::nullopt;
    }
    ConfigTextView view{};
    view.source = static_cast<ConfigSource>(payload[0]);
    view.written_since_boot = (payload[1] & config_flag_written) != 0u;
    view.sequence = get_u32(payload, 4);
    view.text = payload.subspan(config_text_header_bytes);
    if (!is_config_text(view.text)) {
        return std::nullopt;
    }
    // No configuration: no sequence, no text, and nothing written since
    // boot (a write leaves a file behind).
    if (view.source == ConfigSource::none &&
        (view.sequence != 0u || !view.text.empty() || view.written_since_boot)) {
        return std::nullopt;
    }
    // A turn's sequence starts at 1 (store/turn_file.h).
    if (view.source != ConfigSource::none && view.sequence == 0u) {
        return std::nullopt;
    }
    const std::optional<ConfigTextView> decoded = view;
    return decoded;
}

std::size_t encode_config_written(const ConfigWritten& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= config_written_bytes);
    assert(record.file_index <= 1u);
    put_u32(out, 0, record.sequence);
    out[4] = record.file_index;
    zero_bytes(out, 5, 3u);
    return config_written_bytes;
}

std::optional<ConfigWritten> decode_config_written(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != config_written_bytes || bytes[4] > 1u || !bytes_zero(bytes, 5, 3u) ||
        get_u32(bytes, 0) == 0u) {
        return std::nullopt;
    }
    ConfigWritten record{};
    record.sequence = get_u32(bytes, 0);
    record.file_index = bytes[4];
    const std::optional<ConfigWritten> decoded = record;
    return decoded;
}

std::size_t encode_config_refusal(const ConfigRefusal& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= config_refusal_bytes);
    put_u32(out, 0, record.line);
    put_u32(out, 4, record.column);
    out[8] = static_cast<std::uint8_t>(record.kind);
    out[9] = record.code;
    zero_bytes(out, 10, 2u);
    return config_refusal_bytes;
}

std::optional<ConfigRefusal> decode_config_refusal(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != config_refusal_bytes || bytes[8] > static_cast<std::uint8_t>(RefusalKind::user_rule) ||
        !bytes_zero(bytes, 10, 2u)) {
        return std::nullopt;
    }
    ConfigRefusal record{};
    record.line = get_u32(bytes, 0);
    record.column = get_u32(bytes, 4);
    record.kind = static_cast<RefusalKind>(bytes[8]);
    record.code = bytes[9];
    const std::optional<ConfigRefusal> decoded = record;
    return decoded;
}

// ---------------------------------------------------------------------------
// InstallSource.

namespace {

[[nodiscard]] bool url_acceptable(std::span<const char> url) noexcept {
    const char prefix[] = "https://";
    const std::size_t prefix_length = sizeof(prefix) - 1u;
    if (url.size() <= prefix_length || url.size() > install_url_bytes_max) {
        return false;
    }
    for (std::size_t index = 0; index < prefix_length; index += 1) {
        if (url[index] != prefix[index]) {
            return false;
        }
    }
    for (const char byte : url) {
        if (!is_visible(static_cast<std::uint8_t>(byte))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<std::size_t> encode_install_source(const InstallSource& source, std::span<std::uint8_t> out) noexcept {
    if (out.size() < install_source_header_bytes + source.url.size()) {
        return std::nullopt;
    }
    switch (source.kind) {
    case InstallKind::staged:
        if (!source.url.empty()) {
            return std::nullopt;
        }
        break;
    case InstallKind::url:
        if (!url_acceptable(source.url)) {
            return std::nullopt;
        }
        break;
    default:
        // Fallback: an unknown kind is refused.
        return std::nullopt;
    }
    out[0] = static_cast<std::uint8_t>(source.kind);
    zero_bytes(out, 1, 3u);
    for (std::size_t index = 0; index < source.url.size(); index += 1) {
        out[install_source_header_bytes + index] = static_cast<std::uint8_t>(source.url[index]);
    }
    const std::optional<std::size_t> size = install_source_header_bytes + source.url.size();
    return size;
}

std::optional<InstallSource> decode_install_source(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size() < install_source_header_bytes || !bytes_zero(payload, 1, 3u)) {
        return std::nullopt;
    }
    InstallSource source{};
    const std::span<const std::uint8_t> rest = payload.subspan(install_source_header_bytes);
    // char may alias the bytes.
    source.url = std::span<const char>(reinterpret_cast<const char*>(rest.data()), rest.size());
    if (payload[0] == static_cast<std::uint8_t>(InstallKind::staged)) {
        source.kind = InstallKind::staged;
        if (!rest.empty()) {
            return std::nullopt;
        }
    } else if (payload[0] == static_cast<std::uint8_t>(InstallKind::url)) {
        source.kind = InstallKind::url;
        if (!url_acceptable(source.url)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    const std::optional<InstallSource> decoded = source;
    return decoded;
}

// ---------------------------------------------------------------------------
// ProgramRecord.

namespace {

constexpr std::size_t program_name_length = 0;
constexpr std::size_t program_name = 1;
constexpr std::size_t program_active = 33;
constexpr std::size_t program_last_good = 34;
constexpr std::size_t program_flags = 35;
constexpr std::size_t program_failed_starts = 36;
constexpr std::size_t program_reserved = 37;
constexpr std::size_t program_version_a = 40;
constexpr std::size_t program_version_b = 72;
constexpr std::uint8_t program_flag_on_trial = 0x01u;
constexpr std::uint8_t program_flag_update = 0x02u;
constexpr std::uint8_t program_flags_known = 0x03u;

static_assert(program_version_b + 1u + program_version_bytes_max == program_record_bytes, "program record layout");

void put_version(std::span<std::uint8_t> out, std::size_t at, const ProgramVersion& version) noexcept {
    const std::size_t length = version.length <= program_version_bytes_max ? version.length : 0u;
    out[at] = static_cast<std::uint8_t>(length);
    for (std::size_t index = 0; index < program_version_bytes_max; index += 1) {
        out[at + 1u + index] = index < length ? static_cast<std::uint8_t>(version.bytes[index]) : 0u;
    }
}

[[nodiscard]] bool get_version(std::span<const std::uint8_t> bytes, std::size_t at, ProgramVersion& version) noexcept {
    const std::uint8_t length = bytes[at];
    version.length = 0;
    for (std::size_t index = 0; index < program_version_bytes_max; index += 1) {
        version.bytes[index] = '\0';
    }
    if (length > program_version_bytes_max || !bytes_zero(bytes, at + 1u + length, program_version_bytes_max - length)) {
        return false;
    }
    for (std::size_t index = 0; index < length; index += 1) {
        const std::uint8_t byte = bytes[at + 1u + index];
        if (!is_visible(byte)) {
            return false;
        }
        version.bytes[index] = static_cast<char>(byte);
    }
    version.length = length;
    return true;
}

}  // namespace

std::optional<ProgramVersion> program_version_of(std::span<const char> text) noexcept {
    if (text.size() > program_version_bytes_max) {
        return std::nullopt;
    }
    ProgramVersion version{};
    version.length = static_cast<std::uint8_t>(text.size());
    for (std::size_t index = 0; index < program_version_bytes_max; index += 1) {
        if (index < text.size()) {
            if (!is_visible(static_cast<std::uint8_t>(text[index]))) {
                return std::nullopt;
            }
            version.bytes[index] = text[index];
        } else {
            version.bytes[index] = '\0';
        }
    }
    const std::optional<ProgramVersion> found = version;
    return found;
}

std::size_t encode_program_record(const ProgramRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= program_record_bytes);
    assert(record.name.length >= 1u && record.name.length <= service_name_bytes_max);
    put_name(out, program_name_length, program_name, record.name);
    out[program_active] = static_cast<std::uint8_t>(record.active);
    out[program_last_good] = static_cast<std::uint8_t>(record.last_good);
    std::uint8_t flags = 0;
    flags = static_cast<std::uint8_t>(flags | (record.on_trial ? program_flag_on_trial : 0u));
    flags = static_cast<std::uint8_t>(flags | (record.update_available ? program_flag_update : 0u));
    out[program_flags] = flags;
    out[program_failed_starts] = record.failed_starts;
    zero_bytes(out, program_reserved, 3u);
    put_version(out, program_version_a, record.version_a);
    put_version(out, program_version_b, record.version_b);
    return program_record_bytes;
}

std::optional<ProgramRecord> decode_program_record(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != program_record_bytes) {
        return std::nullopt;
    }
    ProgramRecord record{};
    const std::uint8_t slot_max = static_cast<std::uint8_t>(SlotCode::b);
    if (get_name(bytes, program_name_length, program_name, record.name) != NameField::valid ||
        bytes[program_active] > slot_max || bytes[program_last_good] > slot_max ||
        (bytes[program_flags] & static_cast<std::uint8_t>(~program_flags_known)) != 0u ||
        !bytes_zero(bytes, program_reserved, 3u) || !get_version(bytes, program_version_a, record.version_a) ||
        !get_version(bytes, program_version_b, record.version_b)) {
        return std::nullopt;
    }
    record.active = static_cast<SlotCode>(bytes[program_active]);
    record.last_good = static_cast<SlotCode>(bytes[program_last_good]);
    record.on_trial = (bytes[program_flags] & program_flag_on_trial) != 0u;
    record.update_available = (bytes[program_flags] & program_flag_update) != 0u;
    record.failed_starts = bytes[program_failed_starts];
    // An active or last good slot holds a version; a trial needs an active
    // slot.
    const bool active_empty = (record.active == SlotCode::a && record.version_a.length == 0u) ||
                              (record.active == SlotCode::b && record.version_b.length == 0u);
    const bool good_empty = (record.last_good == SlotCode::a && record.version_a.length == 0u) ||
                            (record.last_good == SlotCode::b && record.version_b.length == 0u);
    if (active_empty || good_empty || (record.on_trial && record.active == SlotCode::none)) {
        return std::nullopt;
    }
    const std::optional<ProgramRecord> decoded = record;
    return decoded;
}

// ---------------------------------------------------------------------------
// OperationRecord.

const char* operation_kind_name(OperationKind kind) noexcept {
    switch (kind) {
    case OperationKind::none: return "none";
    case OperationKind::system_update: return "system-update";
    case OperationKind::program_install: return "program-install";
    case OperationKind::program_check: return "program-check";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

const char* operation_state_name(OperationState state) noexcept {
    switch (state) {
    case OperationState::idle: return "idle";
    case OperationState::running: return "running";
    case OperationState::succeeded: return "succeeded";
    case OperationState::failed: return "failed";
    default: break;
    }
    // Fallback: a value outside the enumeration.
    return "unknown";
}

OperationRecord operation_none() noexcept {
    OperationRecord record{};
    record.kind = OperationKind::none;
    record.state = OperationState::idle;
    record.result = Status::ok;
    record.elapsed_ms = 0;
    record.name.length = 0;
    for (std::size_t index = 0; index < service_name_bytes_max; index += 1) {
        record.name.bytes[index] = '\0';
    }
    return record;
}

namespace {

[[nodiscard]] bool operation_names_target(OperationKind kind) noexcept {
    return kind == OperationKind::program_install || kind == OperationKind::program_check;
}

// The rules a record must keep, shared by encode (asserted) and decode.
[[nodiscard]] bool operation_consistent(const OperationRecord& record) noexcept {
    const bool idle = record.state == OperationState::idle;
    if ((record.kind == OperationKind::none) != idle) {
        return false;
    }
    if (idle && record.elapsed_ms != 0u) {
        return false;
    }
    const bool failed = record.state == OperationState::failed;
    if (failed == (record.result == Status::ok)) {
        return false;
    }
    return operation_names_target(record.kind) == (record.name.length > 0u);
}

}  // namespace

bool operation_record_valid(const OperationRecord& record) noexcept {
    if (record.name.length > service_name_bytes_max) {
        return false;
    }
    if (record.name.length > 0u &&
        !service_name_valid(std::span<const char>(record.name.bytes, record.name.length))) {
        return false;
    }
    if (static_cast<std::uint8_t>(record.kind) > static_cast<std::uint8_t>(OperationKind::program_check) ||
        static_cast<std::uint8_t>(record.state) > static_cast<std::uint8_t>(OperationState::failed) ||
        !status_from_code(static_cast<std::uint16_t>(record.result)).has_value()) {
        return false;
    }
    return operation_consistent(record);
}

std::size_t encode_operation_record(const OperationRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= operation_record_bytes);
    assert(operation_record_valid(record));
    out[0] = static_cast<std::uint8_t>(record.kind);
    out[1] = static_cast<std::uint8_t>(record.state);
    put_u16(out, 2, static_cast<std::uint16_t>(record.result));
    put_u32(out, 4, record.elapsed_ms);
    put_name(out, 8, 9, record.name);
    zero_bytes(out, 41, 7u);
    return operation_record_bytes;
}

std::optional<OperationRecord> decode_operation_record(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() != operation_record_bytes ||
        bytes[0] > static_cast<std::uint8_t>(OperationKind::program_check) ||
        bytes[1] > static_cast<std::uint8_t>(OperationState::failed) || !bytes_zero(bytes, 41, 7u)) {
        return std::nullopt;
    }
    const std::optional<Status> result = status_from_code(get_u16(bytes, 2));
    if (!result.has_value()) {
        return std::nullopt;
    }
    OperationRecord record{};
    record.kind = static_cast<OperationKind>(bytes[0]);
    record.state = static_cast<OperationState>(bytes[1]);
    record.result = *result;
    record.elapsed_ms = get_u32(bytes, 4);
    const NameField name = get_name(bytes, 8, 9, record.name);
    if (name != NameField::valid && name != NameField::empty) {
        return std::nullopt;
    }
    if (!operation_consistent(record)) {
        return std::nullopt;
    }
    const std::optional<OperationRecord> decoded = record;
    return decoded;
}

}  // namespace stxcontrol
