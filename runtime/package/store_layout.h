// store_layout.h: where the program store keeps things (C2,
// appliance/C2_DESIGN.md, "Programs").
//
// Pure code: no system call, no operating-system header, no allocation.
//
//   /data/programs/<program>/state             program_slots.h; the
//                                              supervisor writes it (by
//                                              state.new and a rename)
//   /data/programs/<program>/<a|b>/manifest    the slot's manifest
//   /data/programs/<program>/<a|b>/signature   its 64-byte signature
//   /data/programs/<program>/<a|b>/files/...   the package's files
//   /data/programs/<program>/incoming/         an install being written;
//                                              renamed to the slot when
//                                              complete
//   /data/programs/<program>/discard/          a replaced slot, removed
//                                              after the rename
//
// A download is never staged: stxpkg verifies the manifest's signature
// from the package's head, then writes each file into incoming/ as it
// arrives, checking its hash, and renames incoming/ only when every file
// and the end of the stream check out.
//
// Everything here is root's; directories 0755, files 0444 or 0555 (as
// the manifest says), state 0644. Targets read their program's files and
// nothing else needs to.

#ifndef STXLIB_RUNTIME_PACKAGE_STORE_LAYOUT_H
#define STXLIB_RUNTIME_PACKAGE_STORE_LAYOUT_H

#include <cstddef>
#include <optional>

namespace stxpackage {

constexpr const char* programs_root = "/data/programs";
constexpr const char* trust_keys_directory = "/etc/stx/keys";
constexpr std::size_t trust_keys_max = 8;
// Programs in the store: at most one per target.
constexpr std::size_t programs_max = 24;
// The file listing the root slots' raw devices for this board, one line
// each: "A <device>\nB <device>\n" (the image build writes it).
constexpr const char* root_devices_path = "/etc/stx/root-devices";
// The running system's version: its decimal and a LF.
constexpr const char* system_version_path = "/etc/stx/version";
// The CA bundle the downloader trusts (built into the root from the base
// system's /usr/share/certs/trusted at image build).
constexpr const char* ca_bundle_path = "/etc/ssl/cert.pem";
// The longest store path: the root, a name, "/a/files/", a package path.
constexpr std::size_t store_path_max = 320;

struct StorePath {
    char text[store_path_max + 1u];
    std::size_t length;
};

// The parts joined with '/' after programs_root (or, when the first part
// is null, the parts alone); each part NUL-terminated. Nothing when the
// result would not fit. The caller has checked the parts (program names
// are configuration names, package paths package_path_valid).
[[nodiscard]] std::optional<StorePath> store_path(const char* const* parts, std::size_t count) noexcept;

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_STORE_LAYOUT_H
