// The terminal on POSIX hosts (include/stxlib/terminal.hpp), over termios
// and TIOCGWINSZ, shared by macOS, glibc and musl. Ported from
// stx_auxlib's src/hosted/terminal_posix.cpp at b6e330a.
//
// Changes from stx_auxlib: the saved termios is kept inside the
// RawTerminal (no heap block), and the one-at-a-time flag lives in
// stxlib_state.
//
// Emscripten (owner decision 2026-11-15): a page has no terminal, though
// Emscripten's file system gives the standard streams a tty device, so
// isatty alone would not say so. Both calls ask Capability::raw_terminal
// first, false there (core/capabilities.cpp), and fail with unsupported
// before any host call; on the other POSIX families it is true and the
// check changes nothing.

#include "stxlib/capabilities.hpp"
#include "stxlib/terminal.hpp"

#include "core/host_failure.hpp"
#include "state/state.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace stxlib {

static_assert(sizeof(termios) <= raw_terminal_state_bytes, "RawTerminal's storage is too small for termios");

struct RawTerminal::HostAccess final {
    static void save(RawTerminal& terminal, const termios& original) noexcept {
        std::memcpy(terminal.saved_, &original, sizeof original);
        terminal.active_ = true;
    }
    static termios saved(const RawTerminal& terminal) noexcept {
        termios result{};
        std::memcpy(&result, terminal.saved_, sizeof result);
        return result;
    }
};

namespace {

void restore(const termios& original) noexcept {
    while (::tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0 && errno == EINTR) {
    }
}

ForeignCode errno_code() noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(errno));
    return result;
}

} // namespace

Result<TerminalSize> terminal_size() noexcept {
    if (!capability_supported(Capability::raw_terminal)) {
        const ErrorRecord none = host_failure::record(ErrorKind::unsupported, "stxlib.terminal.size", HostCode::none);
        return Result<TerminalSize>::failure(none);
    }
    if (::isatty(STDOUT_FILENO) != 1) {
        const ErrorRecord none =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.size", HostCode::not_a_terminal);
        return Result<TerminalSize>::failure(none);
    }
    winsize dimensions{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions) != 0) {
        const ErrorRecord failed =
            host_failure::record(ErrorKind::io_failure, "stxlib.terminal.size", HostCode::none, errno_code());
        return Result<TerminalSize>::failure(failed);
    }
    const TerminalSize size{static_cast<u32>(dimensions.ws_col), static_cast<u32>(dimensions.ws_row)};
    return Result<TerminalSize>::success(size);
}

RawTerminal::~RawTerminal() { release(); }

RawTerminal::RawTerminal(RawTerminal&& other) noexcept : active_(other.active_) {
    std::memcpy(saved_, other.saved_, raw_terminal_state_bytes);
    std::memset(other.saved_, 0, raw_terminal_state_bytes);
    other.active_ = false;
}

RawTerminal& RawTerminal::operator=(RawTerminal&& other) noexcept {
    if (this == &other) return *this;
    release();
    std::memcpy(saved_, other.saved_, raw_terminal_state_bytes);
    active_ = other.active_;
    std::memset(other.saved_, 0, raw_terminal_state_bytes);
    other.active_ = false;
    return *this;
}

void RawTerminal::release() noexcept {
    if (!active_) return;
    const termios original = HostAccess::saved(*this);
    restore(original);
    std::memset(saved_, 0, raw_terminal_state_bytes);
    active_ = false;
    state::ProcessState& process = state::current_process();
    process.raw_terminal_active.store(false, std::memory_order_release);
}

Result<RawTerminal> RawTerminal::enter() noexcept {
    if (!capability_supported(Capability::raw_terminal)) {
        const ErrorRecord none =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.enter_raw", HostCode::none);
        return Result<RawTerminal>::failure(none);
    }
    if (::isatty(STDIN_FILENO) != 1 || ::isatty(STDOUT_FILENO) != 1) {
        const ErrorRecord none =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.enter_raw", HostCode::not_a_terminal);
        return Result<RawTerminal>::failure(none);
    }
    state::ProcessState& process = state::current_process();
    if (process.raw_terminal_active.exchange(true, std::memory_order_acq_rel)) {
        const ErrorRecord busy =
            host_failure::record(ErrorKind::limit_exceeded, "stxlib.terminal.enter_raw", HostCode::busy);
        return Result<RawTerminal>::failure(busy);
    }
    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) {
        const ForeignCode foreign = errno_code();
        process.raw_terminal_active.store(false, std::memory_order_release);
        const ErrorRecord failed =
            host_failure::record(ErrorKind::io_failure, "stxlib.terminal.enter_raw", HostCode::none, foreign);
        return Result<RawTerminal>::failure(failed);
    }
    termios raw = original;
    ::cfmakeraw(&raw);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        const ForeignCode foreign = errno_code();
        process.raw_terminal_active.store(false, std::memory_order_release);
        const ErrorRecord failed =
            host_failure::record(ErrorKind::io_failure, "stxlib.terminal.enter_raw", HostCode::none, foreign);
        return Result<RawTerminal>::failure(failed);
    }
    RawTerminal result;
    HostAccess::save(result, original);
    return Result<RawTerminal>::success(static_cast<RawTerminal&&>(result));
}

} // namespace stxlib
