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
constexpr std::size_t offset_name = 16;
constexpr std::size_t offset_status = 12;
constexpr std::size_t offset_payload_length = 14;

static_assert(offset_name + service_name_bytes_max == request_frame_bytes, "request layout");

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
        return true;
    case Command::supervisor_state:
    case Command::list_services:
    case Command::slot_status:
    case Command::boot_log_tail:
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
        return true;
    case Command::supervisor_state:
    case Command::list_services:
    case Command::service_state:
    case Command::slot_status:
    case Command::boot_log_tail:
        return false;
    default:
        break;
    }
    // Fallback: an unknown command is treated as operating, the stricter
    // authority.
    return true;
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
    return request;
}

Request request_for_service(Command command, std::uint32_t request_id, const ServiceName& name) noexcept {
    assert(command_takes_name(command));
    Request request{};
    request.command = command;
    request.request_id = request_id;
    request.name = name;
    return request;
}

void encode_request(const Request& request, std::span<std::uint8_t, request_frame_bytes> frame) noexcept {
    assert(command_takes_name(request.command) == (request.name.length > 0u));
    assert(request.name.length <= service_name_bytes_max);
    const std::span<std::uint8_t> out(frame.data(), frame.size());
    put_u32(out, offset_magic, frame_magic);
    put_u16(out, offset_version, protocol_version);
    put_u16(out, offset_command, static_cast<std::uint16_t>(request.command));
    put_u32(out, offset_request_id, request.request_id);
    zero_bytes(out, offset_request_reserved, 3u);
    put_name(out, offset_name_length, offset_name, request.name);
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
    if (frame.size() != request_frame_bytes) {
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
    if (!bytes_zero(frame, offset_request_reserved, 3u)) {
        refusal.error = RequestError::reserved_not_zero;
        return RequestOutcome::refused(refusal);
    }
    Request request{};
    request.command = *command;
    request.request_id = refusal.request_id;
    const NameField name = get_name(frame, offset_name_length, offset_name, request.name);
    const bool takes_name = command_takes_name(*command);
    switch (name) {
    case NameField::too_long:
        refusal.error = RequestError::name_too_long;
        return RequestOutcome::refused(refusal);
    case NameField::invalid:
        refusal.error = RequestError::name_invalid;
        return RequestOutcome::refused(refusal);
    case NameField::empty:
        if (takes_name) {
            refusal.error = RequestError::name_missing;
            return RequestOutcome::refused(refusal);
        }
        return RequestOutcome::decoded(request);
    case NameField::valid:
        if (!takes_name) {
            refusal.error = RequestError::name_unexpected;
            return RequestOutcome::refused(refusal);
        }
        return RequestOutcome::decoded(request);
    default:
        break;
    }
    // Fallback: a name field in no known state is invalid.
    refusal.error = RequestError::name_invalid;
    return RequestOutcome::refused(refusal);
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

static_assert(record_state_ms + 8u == service_record_bytes, "service record layout");

}  // namespace

std::size_t encode_service_record(const ServiceRecord& record, std::span<std::uint8_t> out) noexcept {
    assert(out.size() >= service_record_bytes);
    assert(record.name.length >= 1u && record.name.length <= service_name_bytes_max);
    put_name(out, record_name_length, record_name, record.name);
    out[record_state] = static_cast<std::uint8_t>(record.state);
    out[record_restart] = static_cast<std::uint8_t>(record.restart);
    out[record_control] = static_cast<std::uint8_t>(record.control);
    out[record_flags] = record.restart_pending ? record_flag_restart_pending : 0u;
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
        (bytes[record_flags] & static_cast<std::uint8_t>(~record_flag_restart_pending)) != 0u ||
        !bytes_zero(bytes, record_reserved, 3u)) {
        return std::nullopt;
    }
    record.state = static_cast<ServiceStateCode>(bytes[record_state]);
    record.restart = static_cast<RestartCode>(bytes[record_restart]);
    record.control = static_cast<AccessCode>(bytes[record_control]);
    record.restart_pending = (bytes[record_flags] & record_flag_restart_pending) != 0u;
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

}  // namespace stxcontrol
