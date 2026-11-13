#pragma once

// BLAKE2s-256 (RFC 7693): the library's cryptographic hash, used by the
// precedence chain (precedence.hpp). BLAKE2s is fast in portable scalar
// code, which keeps it within reach of hardware-accelerated SHA-256 on
// every host. Output is identical on every host. Ported from stx_auxlib's
// include/stx_auxlib/hash.hpp at b6e330a; the interface is restated in
// STXlib's vocabulary (fixed arrays, View and MutableView of Byte).

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::hash {

inline constexpr usize digest_bytes = 32;
// A longer key is cut to this many bytes.
inline constexpr usize maximum_key_bytes = 32;

// A 32-byte digest.
struct Digest final {
    Byte bytes[digest_bytes]{};

    [[nodiscard]] bool operator==(const Digest& other) const noexcept;
    [[nodiscard]] bool operator!=(const Digest& other) const noexcept;
};

// Incremental BLAKE2s with a 32-byte output and an optional key of up to
// 32 bytes (keyed BLAKE2s is a MAC). Feed with update, then finish once;
// finish leaves the object unusable until reset.
class Blake2s final {
public:
    Blake2s() noexcept;
    // `key` longer than 32 bytes is cut to 32.
    explicit Blake2s(View<Byte> key) noexcept;
    // Erases the key, the state and the buffered input.
    ~Blake2s();
    Blake2s(const Blake2s&) = delete;
    Blake2s& operator=(const Blake2s&) = delete;

    void reset() noexcept;
    void update(View<Byte> input) noexcept;
    [[nodiscard]] Digest finish() noexcept;

private:
    void compress(bool last) noexcept;

    u32 state_[8]{};
    Byte buffer_[64]{};
    usize buffered_{0};
    // Bytes compressed so far (RFC 7693's t), as a 64-bit counter.
    u64 counted_{0};
    Byte key_[maximum_key_bytes]{};
    usize key_length_{0};
};

[[nodiscard]] Digest blake2s(View<Byte> input) noexcept;
[[nodiscard]] Digest blake2s_keyed(View<Byte> key, View<Byte> input) noexcept;

// Overwrites `bytes` with zeros in a way the compiler may not remove, for
// secrets that must not outlive their use.
void secure_erase(MutableView<Byte> bytes) noexcept;

} // namespace stxlib::hash
