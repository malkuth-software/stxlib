// console_echo.h: the start of each target run's output, echoed to the
// console (sb2; appliance/C2_DESIGN.md, "The supervisor interface", item
// 9; coordinator's call, 2026-11-15).
//
// A target's output goes into its ring (target_log.h). The first
// console_echo_bytes bytes after each start are also given to the console,
// one console line per output line, as "STXTARGET <name> out <text>", so a
// boot can be read on the serial line without the web page. The bound is
// per start: every byte counts, the newline included, and what comes
// after the budget goes to the ring only. Starts are bounded by the
// registry's restart back-off, so a target that writes without end, or
// restarts without end, cannot hold the console.
//
// An output line longer than the line width is given in pieces of at most
// that width (each its own console line, so a console line stays within
// line_capacity_bytes, common/console_line.h). A byte outside printable
// ASCII (tab kept) is given as '?', so a target cannot move the cursor or
// start a console line of its own. Text still pending when the budget runs
// out, or when the output ends, is given as it is.
//
// Pure code: no system call, no allocation. Tested on the host
// (test/test_console_echo.cpp), exhaustively over the stated domain.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONSOLE_ECHO_H
#define STXLIB_RUNTIME_SUPERVISOR_CONSOLE_ECHO_H

#include <cstddef>
#include <span>

namespace stxsupervisor {

// Bytes echoed per start.
constexpr std::size_t console_echo_bytes = 512;
// Text per console line: "STXTARGET " (10) + a name (at most 32) + " out "
// (5) + 100 + the newline = 148, within line_capacity_bytes (160).
constexpr std::size_t console_echo_line_bytes = 100;

// Receives each piece of text to echo (no newline; the caller frames it).
class ConsoleEchoSink {
public:
    virtual void line(std::span<const char> text) noexcept = 0;

protected:
    ConsoleEchoSink() noexcept = default;
    ConsoleEchoSink(const ConsoleEchoSink&) noexcept = default;
    ConsoleEchoSink& operator=(const ConsoleEchoSink&) noexcept = default;
    ~ConsoleEchoSink() = default;
};

class ConsoleEcho final {
public:
    // console_echo_bytes per start, console_echo_line_bytes per line.
    ConsoleEcho() noexcept;
    // Other bounds (the host test's); width is at most
    // console_echo_line_bytes, and a larger one is taken as that.
    ConsoleEcho(std::size_t budget, std::size_t width) noexcept;

    // A new start: the whole budget, nothing pending.
    void start() noexcept;
    // Output bytes as they arrive. Until start() is called, nothing.
    void feed(std::span<const char> bytes, ConsoleEchoSink& sink) noexcept;
    // The output ended: pending text is given; nothing more until start().
    void finish(ConsoleEchoSink& sink) noexcept;

private:
    void give(ConsoleEchoSink& sink) noexcept;

    std::size_t budget_;
    std::size_t width_;
    std::size_t left_;
    std::size_t pending_length_;
    char pending_[console_echo_line_bytes];
};

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONSOLE_ECHO_H
