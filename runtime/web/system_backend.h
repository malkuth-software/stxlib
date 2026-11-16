// system_backend.h: the web service's ApiBackend on FreeBSD (C2; design
// dev/notes/planning/stages/C2-web.md, section 4): the control channel,
// the password store, the pages read once from the assets directory, the
// interfaces' addresses and the factory configuration's file.
//
// The pages are read at start into fixed storage (asset_bytes_max each);
// a page that is missing or too large is reported then and answered 404
// (api.cpp) rather than read on each request. No request path ever names a
// file: the server serves the fixed list in api.h (asset_names).
//
// System calls: open, read, close (pages and configuration), getifaddrs,
// freeifaddrs, and those of ControlClient, PasswordStore and RAND_bytes.

#ifndef STXLIB_RUNTIME_WEB_SYSTEM_BACKEND_H
#define STXLIB_RUNTIME_WEB_SYSTEM_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "api.h"
#include "control_client.h"
#include "crypto_system.h"

namespace stxweb {

// The largest page file: index.html and style.css are a few KiB, app.js
// some tens of KiB (appliance/web/static/); 128 KiB leaves room.
constexpr std::size_t asset_bytes_max = 128u * 1024u;

// How one page read at start.
enum class AssetLoad : std::uint8_t {
    loaded,
    missing,
    too_large,
    failed,
};

[[nodiscard]] const char* asset_load_name(AssetLoad load) noexcept;

// Reads the whole file at path into out (at most out.size() bytes; a
// longer file is too_large). size: the bytes read when loaded.
[[nodiscard]] AssetLoad read_whole_file(const char* path, std::span<char> out, std::size_t& size) noexcept;

class SystemBackend final : public ApiBackend {
public:
    // Borrows client and passwords, which outlive the backend.
    SystemBackend(ControlClient& client, PasswordStore& passwords) noexcept;
    SystemBackend(const SystemBackend&) = delete;
    SystemBackend& operator=(const SystemBackend&) = delete;
    ~SystemBackend() = default;

    // Reads every page below directory; each one's result into loads (by
    // asset index). False when directory is too long for a path.
    [[nodiscard]] bool load_assets(const char* directory, std::span<AssetLoad, asset_count> loads) noexcept;
    // The factory configuration's path (default stxconfig::config_path).
    void set_config_path(const char* path) noexcept;

    [[nodiscard]] std::optional<stxcontrol::ResponseView> ask_supervisor(
        const stxcontrol::Request& request) noexcept override;
    [[nodiscard]] bool password_is_set() noexcept override;
    [[nodiscard]] PasswordCheck verify_password(std::span<const char> password) noexcept override;
    [[nodiscard]] bool set_password(std::span<const char> password) noexcept override;
    [[nodiscard]] std::optional<std::span<const char>> asset(std::size_t index) noexcept override;
    [[nodiscard]] std::optional<std::size_t> list_addresses(std::span<NetworkAddress> out) noexcept override;
    [[nodiscard]] ConfigOutcome read_config(std::span<char> out) noexcept override;
    [[nodiscard]] const char* config_path() noexcept override;
    [[nodiscard]] RandomSource& random() noexcept override;

private:
    ControlClient& client_;
    PasswordStore& passwords_;
    OpenSslRandom random_;
    const char* config_path_;
    // Each asset's bytes; an asset whose file another earlier one already
    // read borrows that one's (index.html serves "/" and "/index.html").
    char asset_storage_[asset_count][asset_bytes_max];
    std::optional<std::span<const char>> assets_[asset_count];
};

}  // namespace stxweb

#endif  // STXLIB_RUNTIME_WEB_SYSTEM_BACKEND_H
