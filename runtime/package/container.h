// container.h: a program package file's layout (C2, appliance/C2_DESIGN.md,
// "Packages").
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_container.cpp).
//
//   STXPKG 1\n
//   manifest <length>\n         1..manifest_size_max, decimal, no leading 0
//   <length bytes: the manifest (manifest.h)>
//   signature 64\n
//   <64 bytes: Ed25519 over signed_message_prefix followed by the manifest>
//   <every file's bytes, in the manifest's order, nothing between>
//
// and nothing after the last file: the package's size is exactly its head
// plus the manifest's total size. The signature's message is the prefix
// then the manifest, so a signature made for anything else (another format,
// another purpose of the same key) never verifies as a package's.

#ifndef STXLIB_RUNTIME_PACKAGE_CONTAINER_H
#define STXLIB_RUNTIME_PACKAGE_CONTAINER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "manifest.h"

namespace stxpackage {

constexpr std::size_t signature_size = 64;
constexpr std::size_t ed25519_public_key_size = 32;
// The domain separation before the manifest in the signed message.
constexpr const char signed_message_prefix[] = "stxpkg signature 1\n";
constexpr std::size_t signed_message_prefix_length = sizeof(signed_message_prefix) - 1u;
// The longest head: "STXPKG 1\n", "manifest " and 5 digits and "\n", the
// manifest, "signature 64\n", the signature.
constexpr std::size_t container_head_max = 9u + 15u + manifest_size_max + 13u + signature_size;

struct ContainerHead {
    std::size_t manifest_offset;
    std::size_t manifest_length;
    std::size_t signature_offset;
    // Where the first file's bytes begin: the head's length.
    std::size_t data_offset;
};

enum class ContainerRefusal : std::uint8_t {
    truncated,
    magic_malformed,
    manifest_line_malformed,
    signature_line_malformed,
};

class ContainerOutcome {
public:
    [[nodiscard]] static ContainerOutcome head(const ContainerHead& value) noexcept;
    [[nodiscard]] static ContainerOutcome refused(ContainerRefusal reason) noexcept;

    [[nodiscard]] const ContainerHead* as_head() const noexcept;
    [[nodiscard]] const ContainerRefusal* as_refused() const noexcept;

private:
    using Alternatives = std::variant<ContainerRefusal, ContainerHead>;
    explicit ContainerOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] const char* container_refusal_name(ContainerRefusal reason) noexcept;

// bytes: the package's first min(size, container_head_max) bytes.
// truncated when they end before the head does.
[[nodiscard]] ContainerOutcome parse_container_head(std::span<const char> bytes) noexcept;

// Writes the head for a manifest of manifest_length bytes into out (at
// least 24 bytes), up to and without the manifest: "STXPKG 1\nmanifest
// <length>\n". Returns the bytes written, or 0 when manifest_length is 0
// or above manifest_size_max. (For the package builder and the tests.)
[[nodiscard]] std::size_t write_container_prefix(std::size_t manifest_length, std::span<char> out) noexcept;

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_CONTAINER_H
