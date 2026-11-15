// target_log.cpp: each target's own output (target_log.h).

#include "target_log.h"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace stxtargets {

TargetLogs::TargetLogs() noexcept : rings_{}, read_ends_{} {
    for (std::size_t index = 0; index < stxconfig::services_max; index += 1) {
        read_ends_[index] = -1;
    }
}

TargetLogs::~TargetLogs() {
    for (std::size_t index = 0; index < stxconfig::services_max; index += 1) {
        if (read_ends_[index] >= 0) {
            (void)close(read_ends_[index]);
        }
    }
}

int TargetLogs::open_output(std::size_t service_index) noexcept {
    if (service_index >= stxconfig::services_max) {
        return -1;
    }
    if (read_ends_[service_index] >= 0) {
        drain_one(service_index);
        abandon(service_index);
    }
    int ends[2] = {-1, -1};
    if (pipe2(ends, O_CLOEXEC) != 0) {
        return -1;
    }
    const int flags = fcntl(ends[0], F_GETFL);
    if (flags < 0 || fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(ends[0]);
        (void)close(ends[1]);
        return -1;
    }
    read_ends_[service_index] = ends[0];
    return ends[1];
}

void TargetLogs::abandon(std::size_t service_index) noexcept {
    if (service_index >= stxconfig::services_max || read_ends_[service_index] < 0) {
        return;
    }
    (void)close(read_ends_[service_index]);
    read_ends_[service_index] = -1;
}

void TargetLogs::note(std::size_t service_index, std::span<const char> line) noexcept {
    if (service_index < stxconfig::services_max) {
        rings_[service_index].append(line);
    }
}

void TargetLogs::drain_one(std::size_t service_index) noexcept {
    char buffer[stxsupervisor::log_ring_bytes];
    int interrupted = 0;
    for (int round = 0; round < target_log_reads_per_round && read_ends_[service_index] >= 0;) {
        const ssize_t got = read(read_ends_[service_index], buffer, sizeof(buffer));
        if (got > 0) {
            rings_[service_index].append(std::span<const char>(buffer, static_cast<std::size_t>(got)));
            round += 1;
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < 16) {
            interrupted += 1;
            continue;
        }
        if (got < 0 && errno == EAGAIN) {
            return;
        }
        // End of file (every writer gone) or a failure: the pipe is done.
        abandon(service_index);
        return;
    }
}

void TargetLogs::drain() noexcept {
    for (std::size_t index = 0; index < stxconfig::services_max; index += 1) {
        if (read_ends_[index] >= 0) {
            drain_one(index);
        }
    }
}

std::optional<TargetLogEnd> TargetLogs::read_end(std::size_t service_index,
                                                 std::span<char> destination) const noexcept {
    if (service_index >= stxconfig::services_max) {
        return std::nullopt;
    }
    const std::optional<TargetLogEnd> end = rings_[service_index].copy_end(destination);
    return end;
}

std::size_t TargetLogs::descriptors(std::span<int> out) const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < stxconfig::services_max && count < out.size(); index += 1) {
        if (read_ends_[index] >= 0) {
            out[count] = read_ends_[index];
            count += 1;
        }
    }
    return count;
}

}  // namespace stxtargets
