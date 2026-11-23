// The cache module on Linux, glibc and musl alike (hosted/cache_backend.hpp).
// Read from sysfs, the same files for both C libraries, so the two report the
// same machine (glibc's sysconf(_SC_LEVEL1_DCACHE_SIZE) has no musl
// counterpart). Host facts (the kernel's
// Documentation/ABI/testing/sysfs-devices-system-cpu as published;
// unconfirmed at a pin): /sys/devices/system/cpu/cpu0/cache/index<i>/ holds
// `level` ("1"), `type` ("Data", "Instruction" or "Unified"), `size`
// ("48K"), `ways_of_associativity` and `coherency_line_size`, one decimal
// value per file. cpu0 stands for every core (a machine with two kinds of
// core reports its first core's caches).

#include "hosted/cache_backend.hpp"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace stxlib::cache_backend {

namespace {

// The most cache descriptions read (index0 .. index7).
constexpr int index_limit = 8;

// The first bytes of a sysfs file, NUL-terminated; false when it cannot be
// read.
[[nodiscard]] bool read_small_file(const char* const name, char (&text)[32]) noexcept {
    text[0] = '\0';
    int fd = ::open(name, O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR) fd = ::open(name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t got = ::read(fd, text, sizeof text - 1);
    while (got < 0 && errno == EINTR) got = ::read(fd, text, sizeof text - 1);
    static_cast<void>(::close(fd));
    if (got <= 0) return false;
    text[static_cast<usize>(got)] = '\0';
    return true;
}

// The leading decimal value of `text`, times 1024 for a K suffix and 1024^2
// for M; 0 when there is none or it is implausibly large.
[[nodiscard]] usize decimal_value(const char* const text) noexcept {
    usize value = 0;
    usize index = 0;
    while (text[index] >= '0' && text[index] <= '9') {
        if (value > (usize{1} << 40)) return 0;
        value = value * 10 + static_cast<usize>(text[index] - '0');
        index = index + 1;
    }
    if (text[index] == 'K') value = value * 1024;
    if (text[index] == 'M') value = value * 1024 * 1024;
    return value;
}

// "/sys/devices/system/cpu/cpu0/cache/index<i>/<leaf>" into `path`.
void cache_path(const int index, const char* const leaf, char (&path)[96]) noexcept {
    const char prefix[] = "/sys/devices/system/cpu/cpu0/cache/index";
    usize at = 0;
    for (usize k = 0; prefix[k] != '\0'; ++k) {
        path[at] = prefix[k];
        at = at + 1;
    }
    path[at] = static_cast<char>('0' + index);
    path[at + 1] = '/';
    at = at + 2;
    for (usize k = 0; leaf[k] != '\0' && at + 1 < sizeof path; ++k) {
        path[at] = leaf[k];
        at = at + 1;
    }
    path[at] = '\0';
}

[[nodiscard]] usize index_value(const int index, const char* const leaf) noexcept {
    char path[96]{};
    cache_path(index, leaf, path);
    char text[32]{};
    if (!read_small_file(path, text)) return 0;
    const usize result = decimal_value(text);
    return result;
}

// Whether index<i> describes a data or unified cache.
[[nodiscard]] bool holds_data(const int index) noexcept {
    char path[96]{};
    cache_path(index, "type", path);
    char text[32]{};
    if (!read_small_file(path, text)) return false;
    const bool result = text[0] == 'D' || text[0] == 'U';
    return result;
}

} // namespace

void read_data_caches(DataCaches& caches) noexcept {
    for (int index = 0; index < index_limit; ++index) {
        if (!holds_data(index)) continue;
        const usize level = index_value(index, "level");
        const usize size = index_value(index, "size");
        if (size == 0) continue;
        if (level == 1) {
            caches.level1 = size;
            const usize ways = index_value(index, "ways_of_associativity");
            if (ways != 0) caches.ways = ways;
            const usize line = index_value(index, "coherency_line_size");
            if (line != 0) caches.line = line;
        }
        if (level == 2) caches.level2 = size;
    }
}

} // namespace stxlib::cache_backend
