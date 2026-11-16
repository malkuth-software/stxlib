// The terminal on Windows (include/stxlib/terminal.hpp), over the console
// modes. Ported from stx_auxlib's src/hosted/terminal_windows.cpp at
// b6e330a. Raw mode switches both directions to virtual-terminal sequences,
// so full-screen programs use the same escape sequences as on POSIX hosts.
//
// Changes from stx_auxlib: the saved modes are kept inside the RawTerminal
// (no heap block), and the one-at-a-time flag lives in stxlib_state.

#include "stxlib/terminal.hpp"

#include "core/host_failure.hpp"
#include "state/state.hpp"

#include <atomic>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace {

struct SavedModes final {
    DWORD input_mode{0};
    DWORD output_mode{0};
};

static_assert(sizeof(SavedModes) <= raw_terminal_state_bytes, "RawTerminal's storage is too small");

bool console_mode(const DWORD which, DWORD& mode) noexcept {
    const HANDLE handle = GetStdHandle(which);
    const bool result = handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
    return result;
}

void set_mode(const DWORD which, const DWORD mode) noexcept {
    static_cast<void>(SetConsoleMode(GetStdHandle(which), mode));
}

} // namespace

struct RawTerminal::HostAccess final {
    static void save(RawTerminal& terminal, const SavedModes& modes) noexcept {
        std::memcpy(terminal.saved_, &modes, sizeof modes);
        terminal.active_ = true;
    }
    static SavedModes saved(const RawTerminal& terminal) noexcept {
        SavedModes result{};
        std::memcpy(&result, terminal.saved_, sizeof result);
        return result;
    }
};

OperationResult<TerminalSize> terminal_size() noexcept {
    CONSOLE_SCREEN_BUFFER_INFO information{};
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(output, &information)) {
        const ErrorRecord none =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.size", HostCode::not_a_terminal);
        return OperationResult<TerminalSize>::failure(none);
    }
    const TerminalSize size{static_cast<u32>(information.srWindow.Right - information.srWindow.Left + 1),
                            static_cast<u32>(information.srWindow.Bottom - information.srWindow.Top + 1)};
    return OperationResult<TerminalSize>::success(size);
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
    const SavedModes modes = HostAccess::saved(*this);
    set_mode(STD_INPUT_HANDLE, modes.input_mode);
    set_mode(STD_OUTPUT_HANDLE, modes.output_mode);
    std::memset(saved_, 0, raw_terminal_state_bytes);
    active_ = false;
    state::ProcessState& process = state::current_process();
    process.raw_terminal_active.store(false, std::memory_order_release);
}

OperationResult<RawTerminal> RawTerminal::enter() noexcept {
    SavedModes modes{};
    if (!console_mode(STD_INPUT_HANDLE, modes.input_mode) || !console_mode(STD_OUTPUT_HANDLE, modes.output_mode)) {
        const ErrorRecord none =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.enter_raw", HostCode::not_a_terminal);
        return OperationResult<RawTerminal>::failure(none);
    }
    state::ProcessState& process = state::current_process();
    if (process.raw_terminal_active.exchange(true, std::memory_order_acq_rel)) {
        const ErrorRecord busy =
            host_failure::record(ErrorKind::limit_exceeded, "stxlib.terminal.enter_raw", HostCode::busy);
        return OperationResult<RawTerminal>::failure(busy);
    }
    const DWORD raw_input =
        (modes.input_mode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) |
        ENABLE_VIRTUAL_TERMINAL_INPUT;
    const DWORD raw_output = modes.output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), raw_input) ||
        !SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), raw_output)) {
        set_mode(STD_INPUT_HANDLE, modes.input_mode);
        set_mode(STD_OUTPUT_HANDLE, modes.output_mode);
        process.raw_terminal_active.store(false, std::memory_order_release);
        const ErrorRecord refused =
            host_failure::record(ErrorKind::unsupported, "stxlib.terminal.enter_raw", HostCode::none);
        return OperationResult<RawTerminal>::failure(refused);
    }
    RawTerminal result;
    HostAccess::save(result, modes);
    return OperationResult<RawTerminal>::success(static_cast<RawTerminal&&>(result));
}

} // namespace stxlib
