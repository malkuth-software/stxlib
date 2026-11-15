// latest.h: the document an update URL serves (C2, appliance/C2_DESIGN.md,
// "Updates"): which version is the newest and where its package is.
//
// Pure code: no system call, no operating-system header, no allocation.
// Tested on the host (test/test_latest.cpp).
//
//   stx-latest 1
//   version <version>        1..4294967295, decimal, no leading zero
//   url <https URL>          url.h
//
// Exactly these three lines, each ending in LF, at most latest_size_max
// bytes. The document is not signed: it only points. The package it points
// to is verified (signature, name, version) before anything is installed,
// so a forged document can at worst offer a package that is refused, or
// withhold an update.

#ifndef STXLIB_RUNTIME_PACKAGE_LATEST_H
#define STXLIB_RUNTIME_PACKAGE_LATEST_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "../net/url.h"

namespace stxpackage {

constexpr std::size_t latest_size_max = 1024;

struct Latest {
    std::uint32_t version;
    stxnet::HttpsUrl url;
};

enum class LatestRefusal : std::uint8_t {
    too_large,
    header_malformed,
    version_malformed,
    url_malformed,
    trailing_bytes,
};

class LatestOutcome {
public:
    [[nodiscard]] static LatestOutcome parsed(const Latest& value) noexcept;
    [[nodiscard]] static LatestOutcome refused(LatestRefusal reason) noexcept;

    [[nodiscard]] const Latest* as_parsed() const noexcept;
    [[nodiscard]] const LatestRefusal* as_refused() const noexcept;

private:
    using Alternatives = std::variant<LatestRefusal, Latest>;
    explicit LatestOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] const char* latest_refusal_name(LatestRefusal reason) noexcept;

[[nodiscard]] LatestOutcome parse_latest(std::span<const char> text) noexcept;

}  // namespace stxpackage

#endif  // STXLIB_RUNTIME_PACKAGE_LATEST_H
