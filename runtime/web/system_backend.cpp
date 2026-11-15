// system_backend.cpp: the web service's ApiBackend on FreeBSD
// (system_backend.h).

#include "system_backend.h"

#include <cerrno>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../supervisor/config/system_actions.h"

namespace stxweb {

namespace {

constexpr int interrupted_retries_max = 16;
constexpr std::size_t asset_path_bytes_max = 320;

[[nodiscard]] bool same_c_text(const char* left, const char* right) noexcept {
    std::size_t index = 0;
    while (left[index] != '\0' && left[index] == right[index]) {
        index += 1;
    }
    return left[index] == right[index];
}

[[nodiscard]] bool join_asset_path(char (&out)[asset_path_bytes_max], const char* directory,
                                   const char* file) noexcept {
    std::size_t size = 0;
    for (std::size_t index = 0; directory[index] != '\0'; index += 1) {
        if (size + 1u >= sizeof(out)) {
            return false;
        }
        out[size] = directory[index];
        size += 1;
    }
    if (size + 1u >= sizeof(out)) {
        return false;
    }
    out[size] = '/';
    size += 1;
    for (std::size_t index = 0; file[index] != '\0'; index += 1) {
        if (size + 1u >= sizeof(out)) {
            return false;
        }
        out[size] = file[index];
        size += 1;
    }
    out[size] = '\0';
    return true;
}

// Leading one bits of a netmask (bytes long); a mask that is not a prefix
// counts its leading run.
[[nodiscard]] std::uint8_t prefix_of(const std::uint8_t* mask, std::size_t bytes) noexcept {
    std::uint8_t length = 0;
    for (std::size_t index = 0; index < bytes; index += 1) {
        for (std::uint32_t bit = 0; bit < 8u; bit += 1) {
            if ((mask[index] & static_cast<std::uint8_t>(0x80u >> bit)) == 0u) {
                return length;
            }
            length = static_cast<std::uint8_t>(length + 1u);
        }
    }
    return length;
}

// Owner for getifaddrs' list.
class InterfaceList {
public:
    InterfaceList() noexcept : list_(nullptr), ok_(getifaddrs(&list_) == 0) {}
    InterfaceList(const InterfaceList&) = delete;
    InterfaceList& operator=(const InterfaceList&) = delete;
    ~InterfaceList() {
        if (list_ != nullptr) {
            freeifaddrs(list_);
        }
    }
    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }
    [[nodiscard]] const struct ifaddrs* first() const noexcept {
        return list_;
    }

private:
    struct ifaddrs* list_;
    bool ok_;
};

}  // namespace

const char* asset_load_name(AssetLoad load) noexcept {
    switch (load) {
    case AssetLoad::loaded:
        return "loaded";
    case AssetLoad::missing:
        return "missing";
    case AssetLoad::too_large:
        return "too_large";
    case AssetLoad::failed:
        return "failed";
    default:
        return "unknown";
    }
}

AssetLoad read_whole_file(const char* path, std::span<char> out, std::size_t& size) noexcept {
    size = 0;
    int descriptor = -1;
    for (int attempt = 0; attempt <= interrupted_retries_max; attempt += 1) {
        descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor >= 0 || errno != EINTR) {
            break;
        }
    }
    if (descriptor < 0) {
        return errno == ENOENT ? AssetLoad::missing : AssetLoad::failed;
    }
    AssetLoad result = AssetLoad::loaded;
    int interrupted = 0;
    // Each pass reads at least one byte, ends, or counts an interruption:
    // at most out.size() + 1 + interrupted_retries_max passes.
    for (;;) {
        char probe = 0;
        char* const destination = size < out.size() ? out.data() + size : &probe;
        const std::size_t room = size < out.size() ? out.size() - size : 1u;
        const ssize_t got = read(descriptor, destination, room);
        if (got < 0) {
            if (errno == EINTR && interrupted < interrupted_retries_max) {
                interrupted += 1;
                continue;
            }
            result = AssetLoad::failed;
            break;
        }
        if (got == 0) {
            break;
        }
        if (size >= out.size()) {
            result = AssetLoad::too_large;
            break;
        }
        size += static_cast<std::size_t>(got);
    }
    (void)close(descriptor);
    if (result != AssetLoad::loaded) {
        size = 0;
    }
    return result;
}

SystemBackend::SystemBackend(ControlClient& client, PasswordStore& passwords) noexcept
    : client_(client),
      passwords_(passwords),
      random_(),
      config_path_(stxconfig::config_path),
      asset_storage_{},
      assets_{} {}

bool SystemBackend::load_assets(const char* directory, std::span<AssetLoad, asset_count> loads) noexcept {
    for (std::size_t index = 0; index < asset_count; index += 1) {
        assets_[index].reset();
        loads[index] = AssetLoad::missing;
    }
    for (std::size_t index = 0; index < asset_count; index += 1) {
        // An earlier asset with the same file: share its bytes.
        bool shared = false;
        for (std::size_t earlier = 0; earlier < index; earlier += 1) {
            if (same_c_text(asset_names[earlier].file, asset_names[index].file)) {
                assets_[index] = assets_[earlier];
                loads[index] = loads[earlier];
                shared = true;
                break;
            }
        }
        if (shared) {
            continue;
        }
        char path[asset_path_bytes_max];
        if (!join_asset_path(path, directory, asset_names[index].file)) {
            return false;
        }
        std::size_t size = 0;
        loads[index] = read_whole_file(path, std::span<char>(asset_storage_[index], asset_bytes_max), size);
        if (loads[index] == AssetLoad::loaded) {
            assets_[index] = std::span<const char>(asset_storage_[index], size);
        }
    }
    return true;
}

void SystemBackend::set_config_path(const char* path) noexcept {
    config_path_ = path;
}

std::optional<stxcontrol::ResponseView> SystemBackend::ask_supervisor(const stxcontrol::Request& request) noexcept {
    return client_.ask(request);
}

bool SystemBackend::password_is_set() noexcept {
    return passwords_.is_set();
}

PasswordCheck SystemBackend::verify_password(std::span<const char> password) noexcept {
    return passwords_.verify(password);
}

bool SystemBackend::set_password(std::span<const char> password) noexcept {
    return passwords_.set(password, random_);
}

std::optional<std::span<const char>> SystemBackend::asset(std::size_t index) noexcept {
    if (index >= asset_count) {
        return std::nullopt;
    }
    return assets_[index];
}

std::optional<std::size_t> SystemBackend::list_addresses(std::span<NetworkAddress> out) noexcept {
    const InterfaceList interfaces;
    if (!interfaces.ok()) {
        return std::nullopt;
    }
    std::size_t count = 0;
    // getifaddrs' list is finite and each pass advances along it; count
    // bounds what is kept.
    for (const struct ifaddrs* entry = interfaces.first(); entry != nullptr && count < out.size();
         entry = (*entry).ifa_next) {
        const struct sockaddr* const address = (*entry).ifa_addr;
        if (address == nullptr || ((*entry).ifa_flags & static_cast<unsigned int>(IFF_LOOPBACK)) != 0u) {
            continue;
        }
        NetworkAddress& slot = out[count];
        slot = NetworkAddress{};
        std::size_t name_size = 0;
        while ((*entry).ifa_name[name_size] != '\0' && name_size < interface_name_bytes_max) {
            slot.interface_name[name_size] = (*entry).ifa_name[name_size];
            name_size += 1;
        }
        slot.interface_name[name_size] = '\0';
        if ((*address).sa_family == AF_INET) {
            const struct sockaddr_in* const inet = reinterpret_cast<const struct sockaddr_in*>(address);
            const std::uint8_t* const bytes = reinterpret_cast<const std::uint8_t*>(&(*inet).sin_addr);
            slot.family = AddressFamily::ipv4;
            for (std::size_t index = 0; index < 4u; index += 1) {
                slot.bytes[index] = bytes[index];
            }
            const struct sockaddr* const mask = (*entry).ifa_netmask;
            slot.prefix_length =
                mask != nullptr && (*mask).sa_family == AF_INET
                    ? prefix_of(reinterpret_cast<const std::uint8_t*>(
                                    &(*reinterpret_cast<const struct sockaddr_in*>(mask)).sin_addr),
                                4u)
                    : static_cast<std::uint8_t>(32u);
            count += 1;
        } else if ((*address).sa_family == AF_INET6) {
            const struct sockaddr_in6* const inet6 = reinterpret_cast<const struct sockaddr_in6*>(address);
            const std::uint8_t* const bytes = reinterpret_cast<const std::uint8_t*>(&(*inet6).sin6_addr);
            slot.family = AddressFamily::ipv6;
            for (std::size_t index = 0; index < 16u; index += 1) {
                slot.bytes[index] = bytes[index];
            }
            // FreeBSD keeps a link-local address's scope in bytes 2-3
            // (KAME); clear it for the text.
            if (slot.bytes[0] == 0xfeu && (slot.bytes[1] & 0xc0u) == 0x80u) {
                slot.bytes[2] = 0;
                slot.bytes[3] = 0;
            }
            const struct sockaddr* const mask = (*entry).ifa_netmask;
            slot.prefix_length =
                mask != nullptr && (*mask).sa_family == AF_INET6
                    ? prefix_of(reinterpret_cast<const std::uint8_t*>(
                                    &(*reinterpret_cast<const struct sockaddr_in6*>(mask)).sin6_addr),
                                16u)
                    : static_cast<std::uint8_t>(128u);
            count += 1;
        }
    }
    const std::optional<std::size_t> listed = count;
    return listed;
}

ConfigOutcome SystemBackend::read_config(std::span<char> out) noexcept {
    std::size_t size = 0;
    const AssetLoad load = read_whole_file(config_path_, out, size);
    switch (load) {
    case AssetLoad::loaded:
        return ConfigOutcome::read(size);
    case AssetLoad::missing:
        return ConfigOutcome::absent();
    case AssetLoad::too_large:
        return ConfigOutcome::failed(EFBIG);
    case AssetLoad::failed:
        return ConfigOutcome::failed(EIO);
    default:
        return ConfigOutcome::failed(EIO);
    }
}

const char* SystemBackend::config_path() noexcept {
    return config_path_;
}

RandomSource& SystemBackend::random() noexcept {
    return random_;
}

}  // namespace stxweb
