// control_client.cpp: the web service's control channel (control_client.h).

#include "control_client.h"

#include <cerrno>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace stxweb {

namespace {

constexpr int channel_descriptor = 3;
constexpr int interrupted_retries_max = 16;

[[nodiscard]] std::uint64_t monotonic_ms() noexcept {
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

}  // namespace

ControlClient::ControlClient(int descriptor) noexcept
    : descriptor_(descriptor), closed_(descriptor < 0), next_id_(1), request_{}, response_{} {}

int ControlClient::inherited_descriptor() noexcept {
    struct stat status{};
    if (fstat(channel_descriptor, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        return -1;
    }
    int type = 0;
    socklen_t length = sizeof(type);
    if (getsockopt(channel_descriptor, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_SEQPACKET) {
        return -1;
    }
    const int flags = fcntl(channel_descriptor, F_GETFD);
    if (flags < 0 || fcntl(channel_descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return -1;
    }
    return channel_descriptor;
}

int ControlClient::descriptor() const noexcept {
    return descriptor_;
}

bool ControlClient::closed() const noexcept {
    return closed_;
}

void ControlClient::check_closed() noexcept {
    if (closed_ || descriptor_ < 0) {
        return;
    }
    // A response nobody waits for is read and dropped; end of file closes.
    const ssize_t got = recv(descriptor_, response_, sizeof(response_), MSG_DONTWAIT);
    if (got == 0) {
        closed_ = true;
        return;
    }
    if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        closed_ = true;
    }
}

std::optional<stxcontrol::ResponseView> ControlClient::ask(const stxcontrol::Request& request) noexcept {
    if (closed_ || descriptor_ < 0) {
        return std::nullopt;
    }
    stxcontrol::Request numbered = request;
    numbered.request_id = next_id_;
    // 0 is never used, so a wrapped counter never matches a cleared field.
    next_id_ = next_id_ == 0xffffffffu ? 1u : next_id_ + 1u;
    const std::optional<std::size_t> size = stxcontrol::encode_request(numbered, request_);
    if (!size.has_value()) {
        return std::nullopt;
    }
    int interrupted = 0;
    for (;;) {
        const ssize_t sent = send(descriptor_, request_, *size, MSG_NOSIGNAL);
        if (sent >= 0) {
            if (static_cast<std::size_t>(sent) != *size) {
                return std::nullopt;
            }
            break;
        }
        if (errno == EINTR && interrupted < interrupted_retries_max) {
            interrupted += 1;
            continue;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            closed_ = true;
        }
        return std::nullopt;
    }
    const std::uint64_t deadline_ms = monotonic_ms() + static_cast<std::uint64_t>(control_answer_timeout_ms);
    int stale = 0;
    interrupted = 0;
    // Each pass waits until the deadline at most, and either returns or
    // drops one stale response (bounded) or retries an interruption
    // (bounded).
    while (stale <= control_stale_responses_max && interrupted <= interrupted_retries_max) {
        const std::uint64_t now_ms = monotonic_ms();
        if (now_ms >= deadline_ms) {
            return std::nullopt;
        }
        struct pollfd watch{};
        watch.fd = descriptor_;
        watch.events = POLLIN;
        const int ready = poll(&watch, 1, static_cast<int>(deadline_ms - now_ms));
        if (ready < 0) {
            if (errno == EINTR) {
                interrupted += 1;
                continue;
            }
            return std::nullopt;
        }
        if (ready == 0) {
            return std::nullopt;
        }
        const ssize_t got = recv(descriptor_, response_, sizeof(response_), 0);
        if (got == 0) {
            closed_ = true;
            return std::nullopt;
        }
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                interrupted += 1;
                continue;
            }
            closed_ = true;
            return std::nullopt;
        }
        const std::span<const std::uint8_t> frame(response_, static_cast<std::size_t>(got));
        const stxcontrol::ResponseOutcome outcome = stxcontrol::decode_response(frame);
        const stxcontrol::ResponseView* const view = outcome.as_view();
        if (view == nullptr) {
            return std::nullopt;
        }
        if ((*view).request_id != numbered.request_id) {
            stale += 1;
            continue;
        }
        if ((*view).command != numbered.command) {
            return std::nullopt;
        }
        const std::optional<stxcontrol::ResponseView> answer = *view;
        return answer;
    }
    return std::nullopt;
}

}  // namespace stxweb
