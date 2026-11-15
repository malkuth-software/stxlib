// stxnet.cpp: brings Ethernet up in normal running (C2), from the boot
// configuration's [network] section (boot_config.h) and [system] hostname.
// An init command of the factory configuration:
//
//   run /sbin/stxnet /etc/stx/appliance.conf
//
// mode = dhcp (the default, also when the section is absent): for the
// configured interface, or else every Ethernet interface, runs the base
// system's "dhclient -b -l /var/db/dhclient.leases.<if> <if>" (it goes to
// the background at once; /etc/dhclient-enter-hooks keeps resolvconf out
// and dhclient-script writes /etc/resolv.conf, a link to
// /var/run/resolv.conf on the read-only root).
// mode = static: "ifconfig <if> inet <address>/<prefix> up" (interface
// required), "route -n add default <gateway>" when a gateway is given, and
// /var/run/resolv.conf with one "nameserver" line per dns address.
// The hostname, when set, is applied first (sethostname).
//
// Each command it runs has the init executor's time limit around stxnet
// as a whole (run_timeout_ms); dhclient -b returns at once. Prints one line
// per action, "STXNET <action> <detail> <outcome>", and exits 0 when every
// action succeeded, 1 otherwise, 2 when the file cannot be read or parsed.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../supervisor/config/boot_config.h"

extern char** environ;

namespace {

using stxconfig::BootConfig;
using stxconfig::TextRef;

BootConfig config;
char file_text[stxconfig::config_size_max + 1u];

constexpr std::size_t interfaces_max = 8;

// Runs program with arguments (null-terminated) and waits; whether it
// exited 0.
[[nodiscard]] bool run(const char* const* arguments) noexcept {
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        (void)execve(arguments[0], const_cast<char* const*>(arguments), environ);
        _exit(127);
    }
    int status = 0;
    int interrupted = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR || interrupted >= 16) {
            return false;
        }
        interrupted += 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void report(const char* action, const char* detail, bool succeeded) noexcept {
    std::printf("STXNET %s %s %s\n", action, detail, succeeded ? "ok" : "failed");
    (void)std::fflush(stdout);
}

// A setting's value as a NUL-terminated copy, or nothing.
[[nodiscard]] bool setting(const char* section, const char* name, char (&out)[stxconfig::line_length_max + 1u]) noexcept {
    const std::optional<TextRef> value = config.find_setting(section, name);
    if (!value.has_value()) {
        return false;
    }
    std::snprintf(out, sizeof(out), "%s", config.text(*value));
    return true;
}

struct InterfaceList {
    char names[interfaces_max][IFNAMSIZ];
    std::size_t count;
};

// The Ethernet interfaces (AF_LINK, IFT_ETHER), as ifconfig -l ether.
[[nodiscard]] InterfaceList ethernet_interfaces() noexcept {
    InterfaceList list{};
    list.count = 0;
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        return list;
    }
    for (struct ifaddrs* entry = addresses; entry != nullptr && list.count < interfaces_max;
         entry = (*entry).ifa_next) {
        if ((*entry).ifa_addr == nullptr || (*(*entry).ifa_addr).sa_family != AF_LINK) {
            continue;
        }
        const struct sockaddr_dl* const link = reinterpret_cast<const struct sockaddr_dl*>((*entry).ifa_addr);
        if ((*link).sdl_type != IFT_ETHER) {
            continue;
        }
        std::snprintf(list.names[list.count], IFNAMSIZ, "%s", (*entry).ifa_name);
        list.count += 1;
    }
    freeifaddrs(addresses);
    return list;
}

[[nodiscard]] bool start_dhcp(const char* interface) noexcept {
    char leases[64] = {};
    std::snprintf(leases, sizeof(leases), "/var/db/dhclient.leases.%s", interface);
    const char* const arguments[] = {"/sbin/dhclient", "-b", "-l", leases, interface, nullptr};
    return run(arguments);
}

// Writes /var/run/resolv.conf with a nameserver line per word of dns.
[[nodiscard]] bool write_resolver(const char* dns) noexcept {
    char text[256] = {};
    std::size_t used = 0;
    std::size_t index = 0;
    while (dns[index] != '\0') {
        while (dns[index] == ' ' || dns[index] == '\t') {
            index += 1;
        }
        const std::size_t begin = index;
        while (dns[index] != '\0' && dns[index] != ' ' && dns[index] != '\t') {
            index += 1;
        }
        if (index > begin) {
            const int written = std::snprintf(&text[used], sizeof(text) - used, "nameserver %.*s\n",
                                              static_cast<int>(index - begin), &dns[begin]);
            if (written < 0 || static_cast<std::size_t>(written) >= sizeof(text) - used) {
                return false;
            }
            used += static_cast<std::size_t>(written);
        }
    }
    const int file = open("/var/run/resolv.conf.new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) {
        return false;
    }
    const bool written = write(file, text, used) == static_cast<ssize_t>(used) && fsync(file) == 0;
    (void)close(file);
    return written && rename("/var/run/resolv.conf.new", "/var/run/resolv.conf") == 0;
}

}  // namespace

int main(int argument_count, char** arguments) {
    if (argument_count != 2) {
        std::printf("STXNET usage stxnet <config> failed\n");
        return 2;
    }
    const int file = open(arguments[1], O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        report("read", arguments[1], false);
        return 2;
    }
    const ssize_t got = read(file, file_text, sizeof(file_text));
    (void)close(file);
    if (got < 0 || static_cast<std::size_t>(got) > stxconfig::config_size_max ||
        !stxconfig::parse_boot_config(std::span<const char>(file_text, static_cast<std::size_t>(got)), config)
             .is_success()) {
        report("parse", arguments[1], false);
        return 2;
    }
    bool all = true;
    char value[stxconfig::line_length_max + 1u] = {};
    if (setting("system", "hostname", value)) {
        const bool set = sethostname(value, static_cast<int>(std::strlen(value))) == 0;
        report("hostname", value, set);
        all = all && set;
    }
    char mode[stxconfig::line_length_max + 1u] = "dhcp";
    (void)setting("network", "mode", mode);
    char interface[stxconfig::line_length_max + 1u] = {};
    const bool has_interface = setting("network", "interface", interface);
    if (std::strcmp(mode, "static") == 0) {
        char address[stxconfig::line_length_max + 1u] = {};
        if (!has_interface || !setting("network", "address", address)) {
            report("static", "needs interface and address", false);
            return 1;
        }
        const char* const up[] = {"/sbin/ifconfig", interface, "inet", address, "up", nullptr};
        const bool configured = run(up);
        report("address", address, configured);
        all = all && configured;
        char gateway[stxconfig::line_length_max + 1u] = {};
        if (setting("network", "gateway", gateway)) {
            const char* const route[] = {"/sbin/route", "-n", "add", "default", gateway, nullptr};
            const bool routed = run(route);
            report("gateway", gateway, routed);
            all = all && routed;
        }
        char dns[stxconfig::line_length_max + 1u] = {};
        if (setting("network", "dns", dns)) {
            const bool resolver = write_resolver(dns);
            report("dns", dns, resolver);
            all = all && resolver;
        }
        return all ? 0 : 1;
    }
    if (has_interface) {
        const bool started = start_dhcp(interface);
        report("dhcp", interface, started);
        return all && started ? 0 : 1;
    }
    const InterfaceList list = ethernet_interfaces();
    if (list.count == 0u) {
        report("dhcp", "no-ethernet-interface", false);
        return 1;
    }
    for (std::size_t index = 0; index < list.count; index += 1) {
        const bool started = start_dhcp(list.names[index]);
        report("dhcp", list.names[index], started);
        all = all && started;
    }
    return all ? 0 : 1;
}
