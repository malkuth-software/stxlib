// console_echo.cpp: the start of each target run's output, echoed to the
// console (console_echo.h).

#include "console_echo.h"

namespace stxsupervisor {

namespace {

[[nodiscard]] char shown(char byte) noexcept {
    const bool printable = (byte >= ' ' && byte <= '~') || byte == '\t';
    return printable ? byte : '?';
}

}  // namespace

ConsoleEcho::ConsoleEcho() noexcept : ConsoleEcho(console_echo_bytes, console_echo_line_bytes) {}

ConsoleEcho::ConsoleEcho(std::size_t budget, std::size_t width) noexcept
    : budget_(budget),
      width_(width < console_echo_line_bytes ? width : console_echo_line_bytes),
      left_(0),
      pending_length_(0),
      pending_{} {
    // A width of 0 could never hold a byte: one is the least.
    if (width_ == 0u) {
        width_ = 1u;
    }
}

void ConsoleEcho::start() noexcept {
    left_ = budget_;
    pending_length_ = 0;
}

void ConsoleEcho::give(ConsoleEchoSink& sink) noexcept {
    sink.line(std::span<const char>(pending_, pending_length_));
    pending_length_ = 0;
}

void ConsoleEcho::feed(std::span<const char> bytes, ConsoleEchoSink& sink) noexcept {
    // Each round spends one byte of the budget, so at most budget_ rounds
    // run per start.
    for (std::size_t index = 0; index < bytes.size() && left_ > 0u; index += 1) {
        left_ -= 1u;
        const char byte = bytes[index];
        if (byte == '\n') {
            give(sink);
            continue;
        }
        if (pending_length_ == width_) {
            give(sink);
        }
        pending_[pending_length_] = shown(byte);
        pending_length_ += 1u;
    }
    if (left_ == 0u && pending_length_ > 0u) {
        give(sink);
    }
}

void ConsoleEcho::finish(ConsoleEchoSink& sink) noexcept {
    if (pending_length_ > 0u) {
        give(sink);
    }
    left_ = 0;
}

}  // namespace stxsupervisor
