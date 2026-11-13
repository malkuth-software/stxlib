// The POSIX environment backend (hosted/environment_backend.hpp), over
// getenv, setenv and unsetenv, shared by macOS, glibc and musl.
// core/environment.cpp serializes the calls. Ported from stx_auxlib's
// src/hosted/environment_posix.cpp at b6e330a.

#include "hosted/environment_backend.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace stxlib::environment_backend {

Report read(const char* const name, const usize limit, Growable<char>& value) noexcept {
    const char* const found = std::getenv(name);
    if (found == nullptr) return Report{Outcome::not_set, ForeignCode::none()};
    const usize length = std::strlen(found);
    if (length > limit) return Report{Outcome::too_long, ForeignCode::none()};
    if (!value.append(found, length)) return Report{Outcome::out_of_memory, ForeignCode::none()};
    return Report{};
}

Report set(const char* const name, const char* const value, const usize) noexcept {
    if (::setenv(name, value, 1) != 0) {
        return Report{Outcome::host_error, ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(errno))};
    }
    return Report{};
}

Report remove(const char* const name) noexcept {
    if (::unsetenv(name) != 0) {
        return Report{Outcome::host_error, ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(errno))};
    }
    return Report{};
}

} // namespace stxlib::environment_backend
