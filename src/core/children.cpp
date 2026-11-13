// The host-independent parts of child processes (include/stxlib/children.hpp):
// request validation, moving a Child, and the captured-output helpers both
// family modules use. Ported from stx_auxlib's src/hosted/children_common.cpp
// at b6e330a.

#include "stxlib/children.hpp"

#include "core/host_failure.hpp"
#include "hosted/children_common.hpp"

#include <cstring>

namespace stxlib {

namespace children_detail {

namespace {

bool holds_nul(const TextView text) noexcept {
    for (usize index = 0; index < text.count(); ++index) {
        if (text.data()[index] == '\0') return true;
    }
    return false;
}

} // namespace

Status check_request(const ChildRequest& request) noexcept {
    const Operation operation = "stxlib.children.start";
    if (request.program.empty()) {
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::empty_request);
    }
    if (holds_nul(request.program)) {
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    for (usize index = 0; index < request.arguments.count(); ++index) {
        if (holds_nul(request.arguments[index])) {
            return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
        }
    }
    return Status::success();
}

ErrorRecord invalid_state(const Operation operation) noexcept {
    const ErrorRecord result = host_failure::record(ErrorKind::invalid_argument, operation, HostCode::invalid_state);
    return result;
}

bool keep_output(const Byte* const bytes, const usize count, const usize limit, Growable<Byte>& output,
                 bool& over_limit) noexcept {
    // output.count() counts only kept bytes, so it never exceeds `limit`.
    if (over_limit || count > limit - output.count()) {
        over_limit = true;
        return true;
    }
    const bool result = output.append(bytes, count);
    return result;
}

Result<Buffer<Byte>> finish_output(const Growable<Byte>& output, const bool over_limit,
                                   const Operation operation) noexcept {
    if (over_limit) {
        return Result<Buffer<Byte>>::failure(
            host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_long));
    }
    Result<Buffer<Byte>> result = Buffer<Byte>::allocate_zeroed(output.count());
    if (result.succeeded() && output.count() != 0) {
        std::memcpy((*result.value()).mutable_view().data(), output.data(), output.count());
    }
    return result;
}

} // namespace children_detail

Child::~Child() { release(); }

Child::Child(Child&& other) noexcept : process_(other.process_), output_(other.output_), waited_(other.waited_) {
    other.process_ = none;
    other.output_ = none;
    other.waited_ = false;
}

Child& Child::operator=(Child&& other) noexcept {
    if (this == &other) return *this;
    release();
    process_ = other.process_;
    output_ = other.output_;
    waited_ = other.waited_;
    other.process_ = none;
    other.output_ = none;
    other.waited_ = false;
    return *this;
}

} // namespace stxlib
