#pragma once

// Making the platform layer's error records (capabilities.hpp, "Failures").

#include "stxlib/capabilities.hpp"
#include "stxlib/error.hpp"
#include "stxlib/result.hpp"

namespace stxlib::host_failure {

[[nodiscard]] inline ErrorRecord record(const ErrorKind kind, const Operation operation, const HostCode code,
                                        const ForeignCode foreign) noexcept {
    const ErrorRecord result =
        ErrorRecord::make(kind, operation, ErrorDetail{.code = static_cast<u32>(code), .foreign = foreign});
    return result;
}

[[nodiscard]] inline ErrorRecord record(const ErrorKind kind, const Operation operation,
                                        const HostCode code) noexcept {
    const ErrorRecord result = record(kind, operation, code, ForeignCode::none());
    return result;
}

[[nodiscard]] inline Status failed(const ErrorKind kind, const Operation operation, const HostCode code,
                                   const ForeignCode foreign) noexcept {
    const Status result = Status::failure(record(kind, operation, code, foreign));
    return result;
}

[[nodiscard]] inline Status failed(const ErrorKind kind, const Operation operation, const HostCode code) noexcept {
    const Status result = Status::failure(record(kind, operation, code));
    return result;
}

} // namespace stxlib::host_failure
