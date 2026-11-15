// control_server.cpp: the control channel's server, pure part
// (control_server.h).

#include "control_server.h"

#include <cassert>

#include "../config/user_config.h"

namespace stxcontrol {

static_assert(stxconfig::services_max == services_listed_max, "a list response holds every service");
static_assert(stxconfig::name_length_max == service_name_bytes_max, "wire names hold every configuration name");
static_assert(stxconfig::config_size_max == config_text_max, "a configuration fits one request and one response");

// ---------------------------------------------------------------------------
// BootLogRead.

BootLogRead::BootLogRead(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

BootLogRead BootLogRead::text(std::size_t size, bool began_inside) noexcept {
    BootLogText state{};
    state.size = size;
    state.began_inside = began_inside;
    const BootLogRead read(Alternatives(std::in_place_type<BootLogText>, state));
    return read;
}

BootLogRead BootLogRead::absent() noexcept {
    const BootLogRead read(Alternatives(std::in_place_type<BootLogAbsent>));
    return read;
}

BootLogRead BootLogRead::failed(int error_number) noexcept {
    BootLogFailed state{};
    state.error_number = error_number;
    const BootLogRead read(Alternatives(std::in_place_type<BootLogFailed>, state));
    return read;
}

const BootLogText* BootLogRead::as_text() const noexcept {
    return std::get_if<BootLogText>(&alternatives_);
}

bool BootLogRead::is_absent() const noexcept {
    return std::holds_alternative<BootLogAbsent>(alternatives_);
}

const BootLogFailed* BootLogRead::as_failed() const noexcept {
    return std::get_if<BootLogFailed>(&alternatives_);
}

// ---------------------------------------------------------------------------
// UserConfigRead and ProgramAnswer.

UserConfigRead::UserConfigRead(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

UserConfigRead UserConfigRead::text(ConfigSource source, std::uint32_t sequence, std::size_t size) noexcept {
    UserConfigText state{};
    state.source = source;
    state.sequence = sequence;
    state.size = size;
    const UserConfigRead read(Alternatives(std::in_place_type<UserConfigText>, state));
    return read;
}

UserConfigRead UserConfigRead::absent() noexcept {
    const UserConfigRead read(Alternatives(std::in_place_type<UserConfigAbsent>));
    return read;
}

UserConfigRead UserConfigRead::failed(int error_number) noexcept {
    UserConfigFailed state{};
    state.error_number = error_number;
    const UserConfigRead read(Alternatives(std::in_place_type<UserConfigFailed>, state));
    return read;
}

const UserConfigText* UserConfigRead::as_text() const noexcept {
    return std::get_if<UserConfigText>(&alternatives_);
}

bool UserConfigRead::is_absent() const noexcept {
    return std::holds_alternative<UserConfigAbsent>(alternatives_);
}

const UserConfigFailed* UserConfigRead::as_failed() const noexcept {
    return std::get_if<UserConfigFailed>(&alternatives_);
}

ProgramAnswer program_answer(Status status) noexcept {
    ProgramAnswer answer{};
    answer.status = status;
    answer.record = std::nullopt;
    return answer;
}

// ---------------------------------------------------------------------------
// Pure helpers.

std::span<const char> whole_lines_of_end(std::span<const char> window, bool began_inside) noexcept {
    if (!began_inside) {
        return window;
    }
    for (std::size_t index = 0; index < window.size(); index += 1) {
        if (window[index] == '\n') {
            return window.subspan(index + 1u);
        }
    }
    return window.subspan(window.size());
}

std::optional<stxboot::Slot> last_good_slot(const stxboot::BootFacts& boot, bool marked_good) noexcept {
    if (!boot.slot.has_value()) {
        return std::nullopt;
    }
    const stxboot::Slot booted = *boot.slot;
    if (marked_good || boot.fallback || boot.tries == 0u) {
        const std::optional<stxboot::Slot> good = booted;
        return good;
    }
    const std::optional<stxboot::Slot> other = booted == stxboot::Slot::a ? stxboot::Slot::b : stxboot::Slot::a;
    return other;
}

namespace {

[[nodiscard]] SlotCode slot_code_of(std::optional<stxboot::Slot> slot) noexcept {
    if (!slot.has_value()) {
        return SlotCode::none;
    }
    return *slot == stxboot::Slot::a ? SlotCode::a : SlotCode::b;
}

[[nodiscard]] RestartCode restart_code_of(stxconfig::RestartPolicy policy) noexcept {
    switch (policy) {
    case stxconfig::RestartPolicy::never: return RestartCode::never;
    case stxconfig::RestartPolicy::on_failure: return RestartCode::on_failure;
    case stxconfig::RestartPolicy::always: return RestartCode::always;
    default: break;
    }
    // Fallback: an unknown policy is reported as never (the registry does
    // not restart it either).
    return RestartCode::never;
}

[[nodiscard]] AccessCode access_code_of(stxconfig::ControlAccess access) noexcept {
    switch (access) {
    case stxconfig::ControlAccess::none: return AccessCode::none;
    case stxconfig::ControlAccess::read: return AccessCode::read;
    case stxconfig::ControlAccess::operate: return AccessCode::operate;
    default: break;
    }
    // Fallback: an unknown access grants nothing.
    return AccessCode::none;
}

// Whether access permits the command.
[[nodiscard]] bool access_permits(stxconfig::ControlAccess access, Command command) noexcept {
    switch (access) {
    case stxconfig::ControlAccess::none:
        return false;
    case stxconfig::ControlAccess::read:
        return !command_operates(command);
    case stxconfig::ControlAccess::operate:
        return true;
    default:
        break;
    }
    // Fallback: an unknown access permits nothing.
    return false;
}

[[nodiscard]] std::uint32_t saturate_u32(std::uint64_t value) noexcept {
    return value < 0xffffffffu ? static_cast<std::uint32_t>(value) : 0xffffffffu;
}

[[nodiscard]] std::uint64_t elapsed_since(std::uint64_t now_ms, std::uint64_t then_ms) noexcept {
    return now_ms >= then_ms ? now_ms - then_ms : 0u;
}

[[nodiscard]] Status status_of_start(stxconfig::StartResult result) noexcept {
    switch (result) {
    case stxconfig::StartResult::started: return Status::ok;
    case stxconfig::StartResult::already_running: return Status::already_running;
    case stxconfig::StartResult::unknown_service: return Status::unknown_service;
    case stxconfig::StartResult::spawn_failed: return Status::spawn_failed;
    default: break;
    }
    // Fallback: an unknown result is reported as unavailable.
    return Status::unavailable;
}

[[nodiscard]] Status status_of_stop(stxconfig::StopResult result) noexcept {
    switch (result) {
    case stxconfig::StopResult::stopping: return Status::ok;
    case stxconfig::StopResult::stopped: return Status::ok;
    case stxconfig::StopResult::not_running: return Status::not_running;
    case stxconfig::StopResult::unknown_service: return Status::unknown_service;
    default: break;
    }
    // Fallback: an unknown result is reported as unavailable.
    return Status::unavailable;
}

[[nodiscard]] Status status_of_restart(stxconfig::RestartResult result) noexcept {
    switch (result) {
    case stxconfig::RestartResult::restarting: return Status::ok;
    case stxconfig::RestartResult::started: return Status::ok;
    case stxconfig::RestartResult::spawn_failed: return Status::spawn_failed;
    case stxconfig::RestartResult::unknown_service: return Status::unknown_service;
    default: break;
    }
    // Fallback: an unknown result is reported as unavailable.
    return Status::unavailable;
}

// Whether a program command's status carries the program's record.
[[nodiscard]] bool status_carries_program(Status status) noexcept {
    switch (status) {
    case Status::ok:
    case Status::busy:
    case Status::signature_refused:
    case Status::source_unavailable:
        return true;
    case Status::malformed:
    case Status::not_permitted:
    case Status::unknown_service:
    case Status::already_running:
    case Status::not_running:
    case Status::spawn_failed:
    case Status::refused_self:
    case Status::unavailable:
    case Status::config_refused:
    case Status::unsupported:
    case Status::store_failed:
        return false;
    default:
        break;
    }
    // Fallback: an unknown status carries nothing.
    return false;
}

[[nodiscard]] bool is_program_command(Command command) noexcept {
    return command == Command::program_state || command == Command::program_install ||
           command == Command::program_rollback || command == Command::program_check;
}

// Whether a status about an existing service carries its record.
[[nodiscard]] bool status_carries_record(Status status) noexcept {
    switch (status) {
    case Status::ok:
    case Status::already_running:
    case Status::not_running:
    case Status::spawn_failed:
    case Status::refused_self:
        return true;
    case Status::malformed:
    case Status::not_permitted:
    case Status::unknown_service:
    case Status::unavailable:
    case Status::config_refused:
    case Status::unsupported:
    case Status::store_failed:
    case Status::busy:
    case Status::signature_refused:
    case Status::source_unavailable:
        return false;
    default:
        break;
    }
    // Fallback: an unknown status carries nothing.
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// ControlServer.

ControlServer::ControlServer(stxconfig::ServiceRegistry& registry, stxconfig::ServiceActions& actions,
                             ControlSystem& system, const stxconfig::BootConfig& factory) noexcept
    : registry_(registry), actions_(actions), system_(system), factory_(factory), written_since_start_(false),
      payload_{}, check_() {}

Status ControlServer::read_config(std::size_t& payload_size) noexcept {
    const std::span<std::uint8_t> payload(payload_, sizeof(payload_));
    const std::span<std::uint8_t> text_area = payload.subspan(config_text_header_bytes, config_text_max);
    // char may alias the bytes.
    const std::span<char> destination(reinterpret_cast<char*>(text_area.data()), text_area.size());
    const UserConfigRead read = system_.read_user_config(destination);
    const UserConfigText* const text = read.as_text();
    if (text != nullptr && (*text).size <= destination.size() && (*text).source != ConfigSource::none &&
        (*text).sequence != 0u) {
        payload_size =
            encode_config_text_header((*text).source, written_since_start_, (*text).sequence, payload) + (*text).size;
        return Status::ok;
    }
    if (read.is_absent()) {
        payload_size = encode_config_text_header(ConfigSource::none, false, 0u, payload);
        return Status::ok;
    }
    payload_size = 0;
    return Status::unavailable;
}

Status ControlServer::write_config(std::span<const std::uint8_t> text, std::size_t& payload_size) noexcept {
    const std::span<std::uint8_t> payload(payload_, sizeof(payload_));
    // char may alias the bytes.
    const std::span<const char> characters(reinterpret_cast<const char*>(text.data()), text.size());
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(characters, check_);
    const stxconfig::ParseFailed* const failure = parsed.as_failure();
    if (failure != nullptr) {
        ConfigRefusal refusal{};
        refusal.line = (*failure).line;
        refusal.column = (*failure).column;
        refusal.kind = RefusalKind::parse;
        refusal.code = static_cast<std::uint8_t>((*failure).error);
        payload_size = encode_config_refusal(refusal, payload);
        return Status::config_refused;
    }
    const std::optional<stxconfig::UserRuleBroken> broken = stxconfig::check_user_config(check_, factory_);
    if (broken.has_value()) {
        ConfigRefusal refusal{};
        refusal.line = (*broken).line;
        refusal.column = (*broken).column;
        refusal.kind = RefusalKind::user_rule;
        refusal.code = static_cast<std::uint8_t>((*broken).rule);
        payload_size = encode_config_refusal(refusal, payload);
        return Status::config_refused;
    }
    ConfigWritten written{};
    const Status status = system_.write_user_config(characters, written);
    if (status != Status::ok) {
        payload_size = 0;
        return Status::store_failed;
    }
    written_since_start_ = true;
    payload_size = encode_config_written(written, payload);
    return Status::ok;
}

Status ControlServer::program(const Request& request, std::size_t& payload_size) noexcept {
    const std::span<std::uint8_t> payload(payload_, sizeof(payload_));
    ProgramAnswer answer = program_answer(Status::unsupported);
    switch (request.command) {
    case Command::program_state:
        answer = system_.program_state(request.name);
        break;
    case Command::program_install: {
        const std::optional<InstallSource> source = decode_install_source(request.payload);
        // Decoding the request already checked the payload.
        if (!source.has_value()) {
            payload_size = 0;
            return Status::malformed;
        }
        answer = system_.program_install(request.name, *source);
        break;
    }
    case Command::program_rollback:
        answer = system_.program_rollback(request.name);
        break;
    case Command::program_check:
        answer = system_.program_check(request.name);
        break;
    default:
        // Fallback: only program commands come here.
        payload_size = 0;
        return Status::malformed;
    }
    payload_size = 0;
    if (answer.record.has_value() && status_carries_program(answer.status)) {
        // A record about another target is the host side's fault: the
        // answer is withheld rather than passed on.
        if (!service_names_equal((*answer.record).name, request.name)) {
            return Status::unavailable;
        }
        payload_size = encode_program_record(*answer.record, payload);
        // The same rules decoding keeps; a record that breaks them is
        // withheld.
        if (!decode_program_record(std::span<const std::uint8_t>(payload_, payload_size)).has_value()) {
            payload_size = 0;
            return Status::unavailable;
        }
    }
    return answer.status;
}

std::size_t ControlServer::encode_record_of(std::size_t index, std::uint64_t now_ms,
                                            std::span<std::uint8_t> out) const noexcept {
    const stxconfig::ServiceDefinition& definition = registry_.definition(index);
    const stxconfig::ServiceState& state = registry_.state(index);
    ServiceRecord record{};
    // The registry's names come from a parsed configuration, so they are
    // valid names of at most name_length_max bytes.
    const char* const name = registry_.service_name(index);
    const std::span<const char> name_bytes(name, definition.name.length);
    const std::optional<ServiceName> wire_name = service_name_of(name_bytes);
    assert(wire_name.has_value());
    if (wire_name.has_value()) {
        record.name = *wire_name;
    }
    record.restart = restart_code_of(definition.restart);
    record.control = access_code_of(definition.control);
    record.restart_count = saturate_u32(registry_.account(index).restart_count);
    record.restart_pending = false;
    record.stopped_by_request = state.is_stopped_by_request();
    record.pid = 0;
    record.state_ms = 0;
    record.state = ServiceStateCode::stopped;
    const stxconfig::ServiceRunning* const running = state.as_running();
    const stxconfig::ServiceWaiting* const waiting = state.as_waiting();
    const stxconfig::ServiceStopping* const stopping = state.as_stopping();
    if (running != nullptr) {
        record.state = ServiceStateCode::running;
        record.pid = static_cast<std::int32_t>((*running).pid);
        record.state_ms = elapsed_since(now_ms, (*running).started_ms);
    } else if (waiting != nullptr) {
        record.state = ServiceStateCode::waiting;
        record.state_ms = elapsed_since((*waiting).start_at_ms, now_ms);
    } else if (stopping != nullptr) {
        record.state = ServiceStateCode::stopping;
        record.pid = static_cast<std::int32_t>((*stopping).pid);
        record.state_ms = elapsed_since(now_ms, (*stopping).started_ms);
        record.restart_pending = (*stopping).start_after;
    }
    return encode_service_record(record, out);
}

ControlHandled ControlServer::handle(std::span<const std::uint8_t> frame, int requester_pid,
                                     const ControlFacts& facts, std::uint64_t now_ms,
                                     std::span<std::uint8_t> response) noexcept {
    assert(response.size() >= response_frame_bytes_max);
    ControlHandled handled{};
    handled.response_size = 0;
    handled.command = std::nullopt;
    handled.status = Status::malformed;
    handled.requester = std::nullopt;
    const std::span<std::uint8_t> payload(payload_, sizeof(payload_));
    std::size_t payload_size = 0;
    ResponseHeader header{};

    const RequestOutcome outcome = decode_request(frame);
    const Request* const request = outcome.as_request();
    if (request == nullptr) {
        const RequestRefused* const refused = outcome.as_refused();
        header.command_code = refused != nullptr ? (*refused).command_code : 0u;
        header.request_id = refused != nullptr ? (*refused).request_id : 0u;
        header.status = Status::malformed;
    } else {
        handled.command = (*request).command;
        header.command_code = static_cast<std::uint16_t>((*request).command);
        header.request_id = (*request).request_id;
        header.status = Status::ok;
        handled.requester = registry_.find_running_pid(requester_pid);
        if (!handled.requester.has_value() ||
            !access_permits(registry_.definition(*handled.requester).control, (*request).command)) {
            header.status = Status::not_permitted;
        }
    }

    if (request != nullptr && header.status == Status::ok) {
        char name[service_name_bytes_max + 1];
        service_name_c_string((*request).name, name);
        const std::optional<std::size_t> target = command_takes_name((*request).command) ? registry_.find(name)
                                                                                         : std::nullopt;
        switch ((*request).command) {
        case Command::supervisor_state: {
            SupervisorStateRecord record{};
            record.uptime_ms = facts.uptime_ms;
            if (facts.restart_count.has_value()) {
                record.restart_count = saturate_u32(*facts.restart_count);
            }
            record.shim_registered = facts.shim_registered;
            record.init_done = facts.init_done;
            record.failsafe_taken = facts.failsafe_taken;
            record.boot_marked_good = facts.boot_marked_good;
            record.service_count = static_cast<std::uint8_t>(registry_.service_count());
            payload_size = encode_supervisor_state(record, payload);
            break;
        }
        case Command::list_services: {
            const std::size_t count = registry_.service_count();
            payload_size = encode_service_list_header(static_cast<std::uint8_t>(count), payload);
            for (std::size_t index = 0; index < count; index += 1) {
                payload_size += encode_record_of(index, now_ms, payload.subspan(payload_size));
            }
            break;
        }
        case Command::service_state:
            if (!target.has_value()) {
                header.status = Status::unknown_service;
            }
            break;
        case Command::start:
            header.status = status_of_start(registry_.start(name, now_ms, actions_));
            break;
        case Command::stop:
            if (target.has_value() && handled.requester.has_value() && *target == *handled.requester) {
                header.status = Status::refused_self;
            } else {
                header.status = status_of_stop(registry_.stop(name, now_ms, actions_));
            }
            break;
        case Command::restart:
            header.status = status_of_restart(registry_.restart(name, now_ms, actions_));
            break;
        case Command::slot_status: {
            SlotRecord record{};
            record.booted = slot_code_of(facts.boot.slot);
            record.tries = static_cast<std::uint8_t>(facts.boot.tries < 255u ? facts.boot.tries : 255u);
            record.fallback = facts.boot.fallback;
            record.marked_good = facts.boot_marked_good;
            record.last_good = slot_code_of(last_good_slot(facts.boot, facts.boot_marked_good));
            payload_size = encode_slot_record(record, payload);
            break;
        }
        case Command::boot_log_tail: {
            // Read into the payload behind its header, then keep whole lines.
            const std::span<std::uint8_t> text_area = payload.subspan(boot_log_header_bytes, boot_log_text_max);
            // char may alias any object's bytes, so the bytes can be filled
            // as characters.
            const std::span<char> destination(reinterpret_cast<char*>(text_area.data()), text_area.size());
            const BootLogRead read = system_.read_boot_log_end(destination);
            const BootLogText* const text = read.as_text();
            if (text != nullptr && (*text).size <= destination.size()) {
                const std::span<const char> window(destination.data(), (*text).size);
                const std::span<const char> lines = whole_lines_of_end(window, (*text).began_inside);
                // Move the whole lines to the area's front (overlapping,
                // forwards: each byte is read before it is overwritten).
                const std::size_t skip = window.size() - lines.size();
                for (std::size_t index = 0; index < lines.size(); index += 1) {
                    destination[index] = destination[skip + index];
                }
                payload_size = encode_boot_log_header(true, (*text).began_inside, payload) + lines.size();
            } else if (read.is_absent()) {
                payload_size = encode_boot_log_header(false, false, payload);
            } else {
                header.status = Status::unavailable;
            }
            break;
        }
        case Command::config_read:
            header.status = read_config(payload_size);
            break;
        case Command::config_write:
            header.status = write_config((*request).payload, payload_size);
            break;
        case Command::reboot:
            system_.request_reboot();
            break;
        case Command::program_state:
        case Command::program_install:
        case Command::program_rollback:
        case Command::program_check:
            header.status = program(*request, payload_size);
            break;
        case Command::system_update:
            header.status = system_.system_update();
            break;
        case Command::operation_state: {
            const OperationRecord record = system_.operation_state();
            if (operation_record_valid(record)) {
                payload_size = encode_operation_record(record, payload);
            } else {
                // The host side's record breaks the format: withheld.
                header.status = Status::unavailable;
            }
            break;
        }
        default:
            // Fallback: decoding admits only known commands.
            header.status = Status::malformed;
            break;
        }
        // Commands about one service answer with its record when it exists.
        if (command_takes_name((*request).command) && !is_program_command((*request).command) &&
            status_carries_record(header.status)) {
            const std::optional<std::size_t> after = registry_.find(name);
            if (after.has_value()) {
                payload_size = encode_record_of(*after, now_ms, payload);
            }
        }
    }

    handled.status = header.status;
    const std::span<const std::uint8_t> payload_view(payload_, payload_size);
    const std::optional<std::size_t> size = encode_response(header, payload_view, response);
    // The payload never exceeds response_payload_max and response holds a
    // full frame, so encoding succeeds.
    assert(size.has_value());
    handled.response_size = size.has_value() ? *size : 0u;
    return handled;
}

}  // namespace stxcontrol
