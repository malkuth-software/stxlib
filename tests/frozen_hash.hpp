#pragma once

// Freezing mode for the tests' frozen output hashes (security audit
// 2026-11-15, B-stxlib-tests-proofs-01-F1). A frozen hash of 0 means "not
// frozen yet"; such a check used to pass silently, so it could never fail.
// Now it fails, unless the environment variable STXLIB_FREEZE_HASHES is set
// to a non-empty value other than "0": the freezing run (the integrator's,
// on the M4) sets it, prints every hash, and the values are written into
// the constants. Every other run treats an unfrozen value as a failure.

#include <cstdlib>

namespace stxlib_tests {

[[nodiscard]] inline bool freezing_hashes() {
    const char* const value = std::getenv("STXLIB_FREEZE_HASHES");
    const bool result = value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
    return result;
}

} // namespace stxlib_tests
