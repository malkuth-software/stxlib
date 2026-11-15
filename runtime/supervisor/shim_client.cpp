// shim_client.cpp: see shim_client.h.

#include "shim_client.h"

#include <cerrno>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../common/console_line.h"

namespace stxsupervisor {

ShimRegistration::ShimRegistration(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

ShimRegistration ShimRegistration::registered(int descriptor) noexcept {
    ShimRegistered state{};
    state.descriptor = descriptor;
    const ShimRegistration outcome(Alternatives(std::in_place_type<ShimRegistered>, state));
    return outcome;
}

ShimRegistration ShimRegistration::absent() noexcept {
    const ShimRegistration outcome(Alternatives(std::in_place_type<ShimAbsent>));
    return outcome;
}

ShimRegistration ShimRegistration::refused(int error_number) noexcept {
    ShimRefused state{};
    state.error_number = error_number;
    const ShimRegistration outcome(Alternatives(std::in_place_type<ShimRefused>, state));
    return outcome;
}

const ShimRegistered* ShimRegistration::as_registered() const noexcept {
    return std::get_if<ShimRegistered>(&alternatives_);
}

bool ShimRegistration::is_absent() const noexcept {
    return std::holds_alternative<ShimAbsent>(alternatives_);
}

const ShimRefused* ShimRegistration::as_refused() const noexcept {
    return std::get_if<ShimRefused>(&alternatives_);
}

TerminateOutcome::TerminateOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

TerminateOutcome TerminateOutcome::done(const stxshim_terminate_result& result) noexcept {
    TerminateDone state{};
    state.result = result;
    const TerminateOutcome outcome(Alternatives(std::in_place_type<TerminateDone>, state));
    return outcome;
}

TerminateOutcome TerminateOutcome::failed(int error_number) noexcept {
    TerminateFailed state{};
    state.error_number = error_number;
    const TerminateOutcome outcome(Alternatives(std::in_place_type<TerminateFailed>, state));
    return outcome;
}

const TerminateDone* TerminateOutcome::as_done() const noexcept {
    return std::get_if<TerminateDone>(&alternatives_);
}

const TerminateFailed* TerminateOutcome::as_failed() const noexcept {
    return std::get_if<TerminateFailed>(&alternatives_);
}

ShimRegistration register_with_shim() noexcept {
    // stxinit hands the file it opened as descriptor
    // STXSHIM_SUPERVISOR_DESCRIPTOR, without close-on-exec so that it
    // survives the supervisor's execve. A closed descriptor means stxinit
    // had no shim to open (a kernel without it, or the open failed): absent.
    const int descriptor = STXSHIM_SUPERVISOR_DESCRIPTOR;
    if (fcntl(descriptor, F_GETFD) < 0) {
        return ShimRegistration::absent();
    }
    // No service or program the supervisor starts may inherit it: the shim
    // accepts one registration per file, and the file is the supervisor's
    // authority.
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
        const int error_number = errno;
        (void)close(descriptor);
        return ShimRegistration::refused(error_number);
    }
    if (ioctl(descriptor, STXSHIM_REGISTER) != 0) {
        const int error_number = errno;
        (void)close(descriptor);
        return ShimRegistration::refused(error_number);
    }
    return ShimRegistration::registered(descriptor);
}

TerminateOutcome terminate_userspace(const ShimRegistered& shim) noexcept {
    stxshim_terminate_result result{};
    // The shim's sequence does not sleep interruptibly, so the request is
    // not ended early by a signal; EINTR is not retried, since the sequence
    // has then not run.
    if (ioctl(shim.descriptor, STXSHIM_TERMINATE, &result) != 0) {
        return TerminateOutcome::failed(errno);
    }
    return TerminateOutcome::done(result);
}

namespace {

void append_error(stxruntime::LineText& line, int error_number) noexcept {
    stxruntime::append_text(line, " errno=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>(error_number)));
}

}  // namespace

void report_registration(int console, const ShimRegistration& registration) noexcept {
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR shim");
    const ShimRefused* const refused = registration.as_refused();
    if (registration.as_registered() != nullptr) {
        stxruntime::append_text(line, " registered");
    } else if (registration.is_absent()) {
        stxruntime::append_text(line, " absent");
    } else if (refused != nullptr) {
        stxruntime::append_text(line, " refused");
        append_error(line, (*refused).error_number);
    } else {
        // Fallback: a registration in no known state.
        stxruntime::append_text(line, " unknown");
    }
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

void report_terminate(int console, const TerminateOutcome& outcome) noexcept {
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR terminate");
    const TerminateDone* const done = outcome.as_done();
    const TerminateFailed* const failed = outcome.as_failed();
    if (done != nullptr) {
        const stxshim_terminate_result& result = (*done).result;
        stxruntime::append_text(line, " done term=");
        stxruntime::append_decimal(line, result.term_signalled);
        stxruntime::append_text(line, " kill=");
        stxruntime::append_decimal(line, result.kill_signalled);
        stxruntime::append_text(line, " rounds=");
        stxruntime::append_decimal(line, result.kill_rounds);
        stxruntime::append_text(line, " remaining=");
        stxruntime::append_decimal(line, result.remaining);
        stxruntime::append_text(line, " ms=");
        stxruntime::append_decimal(line, result.elapsed_ms);
    } else if (failed != nullptr) {
        stxruntime::append_text(line, " failed");
        append_error(line, (*failed).error_number);
    } else {
        // Fallback: an outcome in no known state.
        stxruntime::append_text(line, " unknown");
    }
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

void report_terminate_unavailable(int console) noexcept {
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR terminate unavailable\n");
    stxruntime::write_line(console, line);
}

}  // namespace stxsupervisor
