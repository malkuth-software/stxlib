// shim_client.h: the supervisor's side of the STXlib kernel shim (C1 step
// S5; the interface is appliance/shim/stxshim.h).
//
// The supervisor opens /dev/stxshim once (close-on-exec, so that no service
// inherits it) and registers; a kernel without the shim has no such device,
// and the supervisor then runs as before (the "absent" outcome). The
// descriptor stays open for the supervisor's life; the registration ends
// when the supervisor exits, by any cause, through the shim's death watch.
//
// System calls: open(2), ioctl(2). Hosted on FreeBSD's libc for C1.

#ifndef STXLIB_RUNTIME_SUPERVISOR_SHIM_CLIENT_H
#define STXLIB_RUNTIME_SUPERVISOR_SHIM_CLIENT_H

#include <variant>

#include "../../appliance/shim/stxshim.h"

namespace stxsupervisor {

struct ShimRegistered {
    int descriptor;
};
struct ShimAbsent {
};
struct ShimRefused {
    int error_number;
};

class ShimRegistration {
public:
    [[nodiscard]] static ShimRegistration registered(int descriptor) noexcept;
    [[nodiscard]] static ShimRegistration absent() noexcept;
    [[nodiscard]] static ShimRegistration refused(int error_number) noexcept;

    [[nodiscard]] const ShimRegistered* as_registered() const noexcept;
    [[nodiscard]] bool is_absent() const noexcept;
    [[nodiscard]] const ShimRefused* as_refused() const noexcept;

private:
    using Alternatives = std::variant<ShimAbsent, ShimRegistered, ShimRefused>;
    explicit ShimRegistration(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Opens the shim and registers the calling process as the supervisor.
[[nodiscard]] ShimRegistration register_with_shim() noexcept;

struct TerminateDone {
    stxshim_terminate_result result;
};
struct TerminateFailed {
    int error_number;
};

class TerminateOutcome {
public:
    [[nodiscard]] static TerminateOutcome done(const stxshim_terminate_result& result) noexcept;
    [[nodiscard]] static TerminateOutcome failed(int error_number) noexcept;

    [[nodiscard]] const TerminateDone* as_done() const noexcept;
    [[nodiscard]] const TerminateFailed* as_failed() const noexcept;

private:
    using Alternatives = std::variant<TerminateFailed, TerminateDone>;
    explicit TerminateOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Asks the shim to terminate userspace (stxshim.h, STXSHIM_TERMINATE) and
// waits for it: at most about 2 s plus the sync. The caller's children
// exit meanwhile; it reaps them afterwards.
[[nodiscard]] TerminateOutcome terminate_userspace(const ShimRegistered& shim) noexcept;

// Console lines:
//   STXSUPERVISOR shim registered | absent | refused errno=<n>
//   STXSUPERVISOR terminate done term=<n> kill=<n> rounds=<n> remaining=<n> ms=<n>
//   STXSUPERVISOR terminate failed errno=<n> | unavailable
void report_registration(int console, const ShimRegistration& registration) noexcept;
void report_terminate(int console, const TerminateOutcome& outcome) noexcept;
void report_terminate_unavailable(int console) noexcept;

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_SHIM_CLIENT_H
