// The environment (include/stxlib/environment.hpp): validation and
// serialization over the family's backend (hosted/environment_backend.hpp).
// Ported from stx_auxlib's src/hosted/environment.cpp at b6e330a.
//
// Changes from stx_auxlib: the lock and the change counter live in
// stxlib_state (state.hpp), so every module serializes on one lock; names
// and values are copied into Growable buffers (allocation failure is a
// result, not an abort).

#include "stxlib/environment.hpp"

#include "core/checked.hpp"
#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "hosted/environment_backend.hpp"
#include "state/state.hpp"

namespace stxlib {

namespace {

bool is_letter_or_underscore(const char character) noexcept {
    const bool result =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
    return result;
}

bool is_digit(const char character) noexcept {
    const bool result = character >= '0' && character <= '9';
    return result;
}

// `text` with a terminating NUL.
bool terminated_copy(const TextView text, Growable<char>& output) noexcept {
    const bool result = output.append(text.data(), text.count()) && output.push('\0');
    return result;
}

ErrorRecord record_of(const environment_backend::Report report, const Operation operation) noexcept {
    switch (report.outcome) {
    case environment_backend::Outcome::not_set:
        return host_failure::record(ErrorKind::not_found, operation, HostCode::none);
    case environment_backend::Outcome::too_long:
        return host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_long);
    case environment_backend::Outcome::invalid_text:
        return host_failure::record(ErrorKind::invalid_argument, operation, HostCode::invalid_text);
    case environment_backend::Outcome::out_of_memory:
        return host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none);
    case environment_backend::Outcome::ok:
    case environment_backend::Outcome::host_error:
        break;
    }
    return host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, report.foreign);
}

ErrorRecord invalid_name(const Operation operation) noexcept {
    const ErrorRecord result = host_failure::record(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    return result;
}

ErrorRecord exhausted(const Operation operation) noexcept {
    const ErrorRecord result = host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none);
    return result;
}

// The environment lock was refused at the process's end (spin_lock.hpp).
ErrorRecord ending(const Operation operation) noexcept {
    const ErrorRecord result = host_failure::record(ErrorKind::interrupted, operation, HostCode::none);
    return result;
}

// Records a change for readers that cache what the host derived from the
// environment. Wraps after 2^64 changes, which only risks one missed
// refresh.
STXLIB_MODULAR void record_change(state::ProcessState& process) noexcept {
    process.environment_generation = process.environment_generation + 1;
}

} // namespace

bool is_portable_variable_name(const TextView name) noexcept {
    if (name.empty() || !is_letter_or_underscore(name.data()[0])) return false;
    for (usize index = 0; index < name.count(); ++index) {
        const char character = name.data()[index];
        if (!is_letter_or_underscore(character) && !is_digit(character)) return false;
    }
    return true;
}

OperationResult<Utf8String> read_variable(const TextView name, const usize limit) noexcept {
    if (!is_portable_variable_name(name)) {
        return OperationResult<Utf8String>::failure(invalid_name("stxlib.environment.read"));
    }
    Growable<char> host_name;
    if (!terminated_copy(name, host_name)) return OperationResult<Utf8String>::failure(exhausted("stxlib.environment.read"));
    const usize bounded = limit < maximum_environment_bytes ? limit : maximum_environment_bytes;
    Growable<char> raw;
    environment_backend::Report report{};
    {
        state::ProcessState& process = state::current_process();
        const spin::Guard lock(process.environment_lock);
        if (!lock.held()) return OperationResult<Utf8String>::failure(ending("stxlib.environment.read"));
        report = environment_backend::read(host_name.data(), bounded, raw);
    }
    if (report.outcome != environment_backend::Outcome::ok) {
        return OperationResult<Utf8String>::failure(record_of(report, "stxlib.environment.read"));
    }
    OperationResult<TextView> valid = validate_utf8(raw.data(), raw.count());
    if (valid.failed()) {
        const ErrorRecord invalid =
            host_failure::record(ErrorKind::invalid_argument, "stxlib.environment.read", HostCode::invalid_text);
        return OperationResult<Utf8String>::failure(invalid);
    }
    OperationResult<Utf8String> result = Utf8String::copy_of(*valid.value());
    return result;
}

OperationStatus set_variable(const TextView name, const TextView value) noexcept {
    if (!is_portable_variable_name(name)) return OperationStatus::failure(invalid_name("stxlib.environment.set"));
    if (value.count() > maximum_environment_bytes) {
        return host_failure::failed(ErrorKind::limit_exceeded, "stxlib.environment.set", HostCode::too_long);
    }
    for (usize index = 0; index < value.count(); ++index) {
        if (value.data()[index] == '\0') {
            return host_failure::failed(ErrorKind::invalid_argument, "stxlib.environment.set", HostCode::invalid_value);
        }
    }
    Growable<char> host_name;
    Growable<char> host_value;
    if (!terminated_copy(name, host_name) || !terminated_copy(value, host_value)) {
        return OperationStatus::failure(exhausted("stxlib.environment.set"));
    }
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.environment_lock);
    if (!lock.held()) return OperationStatus::failure(ending("stxlib.environment.set"));
    const environment_backend::Report report = environment_backend::set(host_name.data(), host_value.data(), value.count());
    if (report.outcome != environment_backend::Outcome::ok) {
        return OperationStatus::failure(record_of(report, "stxlib.environment.set"));
    }
    record_change(process);
    return OperationStatus::success();
}

OperationStatus remove_variable(const TextView name) noexcept {
    if (!is_portable_variable_name(name)) return OperationStatus::failure(invalid_name("stxlib.environment.remove"));
    Growable<char> host_name;
    if (!terminated_copy(name, host_name)) return OperationStatus::failure(exhausted("stxlib.environment.remove"));
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.environment_lock);
    if (!lock.held()) return OperationStatus::failure(ending("stxlib.environment.remove"));
    const environment_backend::Report report = environment_backend::remove(host_name.data());
    if (report.outcome != environment_backend::Outcome::ok) {
        return OperationStatus::failure(record_of(report, "stxlib.environment.remove"));
    }
    record_change(process);
    return OperationStatus::success();
}

} // namespace stxlib
