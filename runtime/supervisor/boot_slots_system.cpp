// boot_slots_system.cpp: see boot_slots_system.h.

#include "boot_slots_system.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <fcntl.h>
#include <ifaddrs.h>
#include <kenv.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../common/console_line.h"

namespace stxboot {

namespace {

using stxruntime::LineText;

constexpr int interrupted_retries_max = 16;

[[nodiscard]] std::size_t c_string_bytes(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length + 1;
}

void set_option(struct iovec* pair, const char* name, const char* value) noexcept {
    pair[0].iov_base = const_cast<char*>(name);
    pair[0].iov_len = c_string_bytes(name);
    pair[1].iov_base = const_cast<char*>(value);
    pair[1].iov_len = c_string_bytes(value);
}

// 0 on success, else errno. EBUSY (already mounted there) counts as success
// for the caller that only needs the file system present.
[[nodiscard]] int mount_read_write(const char* file_system, const char* device, const char* directory) noexcept {
    char message[128] = {};
    struct iovec options[8]{};
    set_option(&options[0], "fstype", file_system);
    set_option(&options[2], "fspath", directory);
    set_option(&options[4], "from", device);
    options[6].iov_base = const_cast<char*>("errmsg");
    options[6].iov_len = c_string_bytes("errmsg");
    options[7].iov_base = message;
    options[7].iov_len = sizeof(message);
    if (nmount(options, 8, 0) == 0) {
        return 0;
    }
    return errno;
}

void unmount_quietly(const char* directory) noexcept {
    (void)unmount(directory, 0);
}

// Reads at most buffer.size() bytes from the start of path. nullopt on error.
[[nodiscard]] std::optional<std::size_t> read_start(const char* path, std::span<char> buffer) noexcept {
    const int file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return std::nullopt;
    }
    std::size_t total = 0;
    int interrupted = 0;
    // Each pass reads at least one byte or ends; EINTR retries are bounded.
    while (total < buffer.size()) {
        const ssize_t got = read(file, buffer.data() + total, buffer.size() - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        if (got < 0) {
            (void)close(file);
            return std::nullopt;
        }
        break;
    }
    (void)close(file);
    return total;
}

// Writes all of text at offset 0 of an existing file without truncating
// it (the tries file keeps its fixed size), then fsync. 0 or errno.
[[nodiscard]] int overwrite_start(const char* path, std::span<const char> text) noexcept {
    const int file = open(path, O_WRONLY | O_CLOEXEC);
    if (file < 0) {
        return errno;
    }
    const ssize_t put = pwrite(file, text.data(), text.size(), 0);
    int error_number = 0;
    if (put < 0) {
        error_number = errno;
    } else if (static_cast<std::size_t>(put) != text.size()) {
        error_number = EIO;
    } else if (fsync(file) != 0) {
        error_number = errno;
    }
    (void)close(file);
    return error_number;
}

// Replaces path with text: a new file beside it, fsync, rename. 0 or errno.
[[nodiscard]] int replace_file(const char* path, const char* temporary, std::span<const char> text) noexcept {
    const int file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) {
        return errno;
    }
    const ssize_t put = write(file, text.data(), text.size());
    int error_number = 0;
    if (put < 0) {
        error_number = errno;
    } else if (static_cast<std::size_t>(put) != text.size()) {
        error_number = EIO;
    } else if (fsync(file) != 0) {
        error_number = errno;
    }
    (void)close(file);
    if (error_number == 0 && rename(temporary, path) != 0) {
        error_number = errno;
    }
    return error_number;
}

// The slot file on the ESP (FAT32): rewritten in place when it already has
// the canonical size, so that a power loss mid-write touches one sector of
// file data and no directory entry or FAT chain (msdosfs's rename changes
// both, and a cut between them can leave no slot file at all, which
// local.lua reads as "boot A"). A file of another size (edited by hand) is
// replaced as before. 0 or errno.
[[nodiscard]] int write_slot_file(std::span<const char> text) noexcept {
    const char* const path = "/stx/esp/stx/slots.conf";
    struct stat facts{};
    if (stat(path, &facts) == 0 && S_ISREG(facts.st_mode) && facts.st_size == static_cast<off_t>(text.size())) {
        return overwrite_start(path, text);
    }
    return replace_file(path, "/stx/esp/stx/slots.new", text);
}

void print_failure(int console, const char* prefix, const char* step, int error_number) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, prefix);
    stxruntime::append_text(line, step);
    stxruntime::append_text(line, " errno=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>(error_number)));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

void append_slot(LineText& line, const std::optional<Slot>& slot) noexcept {
    if (slot.has_value()) {
        const char letter[2] = {slot_letter(*slot), '\0'};
        stxruntime::append_text(line, letter);
    } else {
        stxruntime::append_text(line, "none");
    }
}

// kenv value of name into buffer (NUL-terminated); nullptr when absent.
[[nodiscard]] const char* kenv_value(const char* name, std::span<char> buffer) noexcept {
    const int length = kenv(KENV_GET, name, buffer.data(), static_cast<int>(buffer.size()));
    if (length <= 0) {
        return nullptr;
    }
    buffer[buffer.size() - 1] = '\0';
    return buffer.data();
}

}  // namespace

BootFacts read_boot_facts(int console) noexcept {
    std::array<char, 16> slot{};
    std::array<char, 16> tries{};
    std::array<char, 16> fallback{};
    const BootFacts facts = boot_facts(kenv_value("stx.boot.slot", std::span<char>(slot.data(), slot.size())),
                                       kenv_value("stx.boot.tries", std::span<char>(tries.data(), tries.size())),
                                       kenv_value("stx.boot.fallback", std::span<char>(fallback.data(), fallback.size())));
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR boot slot=");
    append_slot(line, facts.slot);
    stxruntime::append_text(line, " tries=");
    stxruntime::append_decimal(line, facts.tries);
    stxruntime::append_text(line, facts.fallback ? " fallback=1\n" : " fallback=0\n");
    stxruntime::write_line(console, line);
    return facts;
}

bool network_up() noexcept {
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    bool found = false;
    // The list is finite (the kernel's interfaces); the walk is bounded too.
    std::size_t visited = 0;
    for (const struct ifaddrs* item = list; item != nullptr && visited < 4096u; item = (*item).ifa_next) {
        visited += 1;
        if ((*item).ifa_addr == nullptr || (*(*item).ifa_addr).sa_family != AF_INET) {
            continue;
        }
        const unsigned int flags = (*item).ifa_flags;
        if ((flags & static_cast<unsigned int>(IFF_UP)) != 0 && (flags & static_cast<unsigned int>(IFF_LOOPBACK)) == 0) {
            found = true;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

void mark_boot_good(int console, const BootFacts& facts) noexcept {
    if (!facts.slot.has_value()) {
        return;
    }
    const char* const prefix = "STXSUPERVISOR boot mark-failed step=";
    int error_number = mount_read_write("ufs", bootctl_device, bootctl_directory);
    if (error_number != 0) {
        print_failure(console, prefix, "mount-bootctl", error_number);
        return;
    }
    error_number = mount_read_write("msdosfs", esp_device, esp_directory);
    if (error_number != 0) {
        unmount_quietly(bootctl_directory);
        print_failure(console, prefix, "mount-esp", error_number);
        return;
    }
    std::array<char, tries_file_size> tries_text{};
    const std::optional<std::size_t> tries_read =
        read_start("/stx/bootctl/stx/tries", std::span<char>(tries_text.data(), tries_text.size()));
    std::optional<std::uint32_t> tries;
    if (tries_read.has_value()) {
        tries = parse_tries(std::span<const char>(tries_text.data(), *tries_read));
    }
    std::array<char, slot_file_size_max> slot_text{};
    const std::optional<std::size_t> slot_read =
        read_start("/stx/esp/stx/slots.conf", std::span<char>(slot_text.data(), slot_text.size()));
    std::optional<SlotFile> slot_file;
    if (slot_read.has_value()) {
        slot_file = parse_slot_file(std::span<const char>(slot_text.data(), *slot_read));
    }
    const MarkGoodPlan plan = mark_good_plan(facts, tries, slot_file);
    const char* failed_step = nullptr;
    if (plan.write_tries_zero) {
        const std::array<char, tries_file_size> zero = format_tries(0);
        // In place when the file exists (the loader's file keeps its size);
        // a missing file is created at its full size.
        error_number = overwrite_start("/stx/bootctl/stx/tries", std::span<const char>(zero.data(), zero.size()));
        if (error_number == ENOENT) {
            error_number = replace_file("/stx/bootctl/stx/tries", "/stx/bootctl/stx/tries.new",
                                        std::span<const char>(zero.data(), zero.size()));
        }
        if (error_number != 0) {
            failed_step = "write-tries";
        }
    }
    if (failed_step == nullptr && plan.new_slot_file.has_value()) {
        const std::array<char, slot_file_text_size> text = format_slot_file(*plan.new_slot_file);
        error_number = write_slot_file(std::span<const char>(text.data(), text.size()));
        if (error_number != 0) {
            failed_step = "write-slot-file";
        }
    }
    unmount_quietly(esp_directory);
    unmount_quietly(bootctl_directory);
    if (failed_step != nullptr) {
        print_failure(console, prefix, failed_step, error_number);
        return;
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR boot good slot=");
    append_slot(line, facts.slot);
    stxruntime::append_text(line, plan.write_tries_zero ? " tries=reset" : " tries=kept");
    stxruntime::append_text(line, plan.new_slot_file.has_value() ? " slot-file=written\n" : " slot-file=kept\n");
    stxruntime::write_line(console, line);
}

bool arm_system_update(int console, Slot running, Slot target) noexcept {
    const char* const prefix = "STXSUPERVISOR system arm-failed step=";
    int error_number = mount_read_write("ufs", bootctl_device, bootctl_directory);
    if (error_number != 0) {
        print_failure(console, prefix, "mount-bootctl", error_number);
        return false;
    }
    const std::array<char, tries_file_size> zero = format_tries(0);
    error_number = overwrite_start("/stx/bootctl/stx/tries", std::span<const char>(zero.data(), zero.size()));
    if (error_number == ENOENT) {
        error_number = replace_file("/stx/bootctl/stx/tries", "/stx/bootctl/stx/tries.new",
                                    std::span<const char>(zero.data(), zero.size()));
    }
    unmount_quietly(bootctl_directory);
    if (error_number != 0) {
        print_failure(console, prefix, "write-tries", error_number);
        return false;
    }
    error_number = mount_read_write("msdosfs", esp_device, esp_directory);
    if (error_number != 0) {
        print_failure(console, prefix, "mount-esp", error_number);
        return false;
    }
    SlotFile file{};
    file.good = running;
    file.try_slot = target;
    const std::array<char, slot_file_text_size> text = format_slot_file(file);
    error_number = write_slot_file(std::span<const char>(text.data(), text.size()));
    unmount_quietly(esp_directory);
    if (error_number != 0) {
        print_failure(console, prefix, "write-slot-file", error_number);
        return false;
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR system armed slot=");
    const char letter[3] = {slot_letter(target), '\n', '\0'};
    stxruntime::append_text(line, letter);
    stxruntime::write_line(console, line);
    return true;
}

void append_boot_log(int console, const BootFacts& facts, const char* event, std::uint64_t uptime_ms) noexcept {
    const char* const prefix = "STXSUPERVISOR boot-log failed step=";
    const int mounted = mount_read_write("msdosfs", user_device, user_directory);
    if (mounted != 0 && mounted != EBUSY) {
        print_failure(console, prefix, "mount-user", mounted);
        return;
    }
    (void)mkdir("/user/stx", 0755);
    LineText entry = stxruntime::empty_line();
    stxruntime::append_text(entry, "uptime_ms=");
    stxruntime::append_decimal(entry, uptime_ms);
    stxruntime::append_text(entry, " slot=");
    append_slot(entry, facts.slot);
    stxruntime::append_text(entry, " tries=");
    stxruntime::append_decimal(entry, facts.tries);
    stxruntime::append_text(entry, facts.fallback ? " fallback=1 event=" : " fallback=0 event=");
    stxruntime::append_text(entry, event);
    stxruntime::append_text(entry, "\n");
    const int file = open(boot_log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (file < 0) {
        print_failure(console, prefix, "open", errno);
        return;
    }
    stxruntime::write_line(file, entry);
    const int synced = fsync(file);
    const int sync_error = synced != 0 ? errno : 0;
    (void)close(file);
    if (sync_error != 0) {
        print_failure(console, prefix, "fsync", sync_error);
        return;
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR boot-log written event=");
    stxruntime::append_text(line, event);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

void append_fallback_log(int console, const BootFacts& facts, std::uint64_t uptime_ms) noexcept {
    std::array<char, 32> reason{};
    std::array<char, 16> tried{};
    const char* const reason_value = kenv_value("stx.boot.reason", std::span<char>(reason.data(), reason.size()));
    const char* const tried_value = kenv_value("stx.boot.tried", std::span<char>(tried.data(), tried.size()));
    std::optional<Slot> tried_slot;
    if (tried_value != nullptr && tried_value[0] != '\0' && tried_value[1] == '\0') {
        tried_slot = slot_from_letter(tried_value[0]);
    }
    // The log's tail, from its first whole line; append_boot_log reports a
    // mount failure itself, and an unreadable log reads as no entry.
    std::array<char, boot_log_tail_bytes> tail{};
    std::span<const char> lines;
    const int mounted = mount_read_write("msdosfs", user_device, user_directory);
    if (tried_slot.has_value() && (mounted == 0 || mounted == EBUSY)) {
        const int file = open(boot_log_path, O_RDONLY | O_CLOEXEC);
        if (file >= 0) {
            const off_t size = lseek(file, 0, SEEK_END);
            if (size > 0) {
                const off_t start = size > static_cast<off_t>(tail.size()) ? size - static_cast<off_t>(tail.size()) : 0;
                const ssize_t got = pread(file, tail.data(), static_cast<std::size_t>(size - start), start);
                if (got > 0) {
                    std::size_t first = 0;
                    if (start > 0) {
                        while (first < static_cast<std::size_t>(got) && tail[first] != '\n') {
                            first += 1;
                        }
                        first += 1;
                    }
                    if (first < static_cast<std::size_t>(got)) {
                        lines = std::span<const char>(tail.data() + first, static_cast<std::size_t>(got) - first);
                    }
                }
            }
            (void)close(file);
        }
    }
    std::optional<std::span<const char>> last_event;
    if (tried_slot.has_value()) {
        last_event = last_event_of_slot(lines, *tried_slot);
    }
    LineText event = stxruntime::empty_line();
    stxruntime::append_text(event, "fallback tried=");
    append_slot(event, tried_slot);
    stxruntime::append_text(event, " loader-reason=");
    stxruntime::append_text(event, reason_value != nullptr ? reason_value : "none");
    stxruntime::append_text(event, " tried-last-event=");
    if (last_event.has_value()) {
        // At most last_event_bytes_max of it, so the log entry fits a line
        // (the longest of ours, failsafe-config-unreadable, has 27).
        constexpr std::size_t last_event_bytes_max = 40;
        for (std::size_t index = 0; index < (*last_event).size() && index < last_event_bytes_max; index += 1) {
            const char letter[2] = {(*last_event)[index], '\0'};
            stxruntime::append_text(event, letter);
        }
    } else {
        stxruntime::append_text(event, "none");
    }
    char event_text[stxruntime::line_capacity_bytes + 1];
    stxruntime::copy_line_to_c_string(event, event_text);
    append_boot_log(console, facts, event_text, uptime_ms);
}

}  // namespace stxboot
