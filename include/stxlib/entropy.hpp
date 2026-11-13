#pragma once

// Host entropy (Capability::entropy): bytes from the host's cryptographic
// generator. Ported from stx_auxlib's random::fill_entropy
// (src/hosted/entropy_<host>.cpp at b6e330a). Fails closed: there is no
// fallback source.
//
// Hosts: macOS (and the BSDs) arc4random_buf, which cannot fail; glibc and
// musl getentropy, at most 256 bytes per call; Windows BCryptGenRandom with
// the system-preferred generator.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// Fills `output`. Failure: foreign_failure with the host's code; `output`
// is then zeroed.
[[nodiscard]] Status fill_entropy(MutableView<Byte> output) noexcept;

} // namespace stxlib
