#pragma once

// The interactive terminal (Capability::raw_terminal): its size, and raw
// mode for full-screen terminal programs. Ported from stx_auxlib's
// include/stx_auxlib/terminal.hpp at b6e330a.
//
// Raw mode means: input arrives byte by byte without echo or line editing,
// and control keys (Ctrl+C, Ctrl+Z) arrive as bytes instead of signals;
// output is not translated ("\n" does not become "\r\n"). On Windows the
// console additionally switches to virtual-terminal sequences in both
// directions, so the same escape sequences work on every host.
//
// Change from stx_auxlib: the saved modes are kept inside the RawTerminal
// object (fixed storage), not in a heap block, so a RawTerminal may be
// moved to and destroyed in another module than the one that entered it.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

struct TerminalSize final {
    u32 columns{0};
    u32 rows{0};
};

// The size of the terminal on standard output. Failures: unsupported with
// HostCode::not_a_terminal when it is redirected; io_failure; unsupported
// with HostCode::none where Capability::raw_terminal is false (Emscripten).
[[nodiscard]] Result<TerminalSize> terminal_size() noexcept;

// Bytes a RawTerminal keeps for the host's saved modes (termios is 60 bytes
// on glibc and musl and 72 on macOS; two DWORDs on Windows).
inline constexpr usize raw_terminal_state_bytes = 128;

// Holds the terminal in raw mode for its lifetime and restores the earlier
// mode when destroyed. Requires standard input and output to be terminals.
// One at a time in the process: entering while another is active fails
// with limit_exceeded, HostCode::busy. Move-only; a moved-from RawTerminal
// restores nothing.
class RawTerminal final {
public:
    // Failures: unsupported with HostCode::not_a_terminal; limit_exceeded
    // with HostCode::busy; io_failure; unsupported (Windows, when the
    // console refuses virtual-terminal mode); unsupported with
    // HostCode::none where Capability::raw_terminal is false (Emscripten).
    [[nodiscard]] static Result<RawTerminal> enter() noexcept;

    RawTerminal() noexcept = default;
    ~RawTerminal();
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
    RawTerminal(RawTerminal&& other) noexcept;
    // Restores this terminal's mode first, as the destructor does.
    RawTerminal& operator=(RawTerminal&& other) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    // For the host module (terminal_<family>.cpp) only.
    struct HostAccess;

private:
    friend struct HostAccess;

    void release() noexcept;

    alignas(8) unsigned char saved_[raw_terminal_state_bytes]{};
    bool active_{false};
};

} // namespace stxlib
