// manifest.h: a program package's manifest (C2: signed program packages,
// appliance/C2_DESIGN.md, "Packages").
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_manifest.cpp).
//
// A manifest lists every file of a package with its mode, size and SHA-256.
// The package's Ed25519 signature covers the manifest (container.h), so a
// verified manifest vouches for every byte of every file. The format is
// fixed, one canonical spelling per manifest:
//
//   stx-manifest 1
//   name <name>
//   version <version>
//   file <mode> <size> <sha256> <path>
//   ...
//
// - Every line ends in LF, the last one included; no CR, no blank line, no
//   comment; single spaces between fields; bytes printable ASCII.
// - name: [a-z][a-z0-9-]*, 1..name_length_max bytes (as a configuration
//   name). "system" is the system image's package (C2_DESIGN.md, "System
//   updates").
// - version: 1..4294967295 in decimal without a leading zero. A program's
//   versions only move forward.
// - mode: 0444 (data) or 0555 (executable).
// - size: decimal without a leading zero (0 allowed), at most
//   file_size_max; all sizes together at most package_size_max.
// - sha256: 64 lower-case hexadecimal digits.
// - path: a package path (boot_config.h, package_path_valid): relative, no
//   empty, "." or ".." component.
// - 1..manifest_files_max file lines, their paths in strictly ascending
//   byte order (so none repeats), and no path a directory of another ("a"
//   and "a/b" conflict).
// - At most manifest_size_max bytes in all.

#ifndef STXLIB_RUNTIME_PACKAGE_MANIFEST_H
#define STXLIB_RUNTIME_PACKAGE_MANIFEST_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace stxpackage {

constexpr std::size_t manifest_size_max = 65536;
constexpr std::size_t manifest_files_max = 1024;
constexpr std::size_t name_length_max = 32;
constexpr std::uint64_t file_size_max = std::uint64_t{1} << 30;
constexpr std::uint64_t package_size_max = std::uint64_t{1} << 31;
constexpr std::uint16_t mode_data = 0444;
constexpr std::uint16_t mode_executable = 0555;
constexpr std::size_t sha256_size = 32;
constexpr const char* system_package_name = "system";

struct ManifestFile {
    // The path inside the manifest text: offset and length.
    std::uint32_t path_offset;
    std::uint32_t path_length;
    std::uint16_t mode;
    std::uint64_t size;
    std::uint8_t sha256[sha256_size];
};

// A parsed manifest. It refers into the text it was parsed from, which the
// caller keeps unchanged while the manifest is used.
struct Manifest {
    std::uint32_t name_offset;
    std::uint32_t name_length;
    std::uint32_t version;
    std::uint32_t file_count;
    std::uint64_t total_size;
    ManifestFile files[manifest_files_max];
};

enum class ManifestError : std::uint8_t {
    too_large,
    line_malformed,
    header_malformed,
    name_malformed,
    version_malformed,
    file_line_malformed,
    mode_malformed,
    size_malformed,
    file_too_large,
    package_too_large,
    hash_malformed,
    path_malformed,
    not_sorted,
    path_conflict,
    too_many_files,
    no_files,
};

struct ManifestRefused {
    ManifestError error;
    // 1-based; 0 for a refusal of the whole text.
    std::uint32_t line;
};

[[nodiscard]] const char* manifest_error_name(ManifestError error) noexcept;

// Parses text into manifest. On a refusal, manifest's contents are
// unspecified.
[[nodiscard]] std::optional<ManifestRefused> parse_manifest(std::span<const char> text, Manifest& manifest) noexcept;

// Whether the manifest's name is name (NUL-terminated).
[[nodiscard]] bool manifest_name_is(std::span<const char> text, const Manifest& manifest, const char* name) noexcept;

// The index of the file whose path is path (bytes, length), or nothing.
// Binary search: the paths are sorted.
[[nodiscard]] std::optional<std::size_t> find_manifest_file(std::span<const char> text, const Manifest& manifest,
                                                            const char* path, std::size_t length) noexcept;

// 64 lower-case hexadecimal digits into 32 bytes; false on anything else.
[[nodiscard]] bool sha256_of_hex(const char* hex, std::size_t length, std::uint8_t (&digest)[sha256_size]) noexcept;

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_MANIFEST_H
