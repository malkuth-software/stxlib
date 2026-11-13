#pragma once

// The primitives each family's process module supplies
// (macos/process_macos.cpp, linux/process_linux.cpp,
// freebsd/process_freebsd.cpp, windows/process_windows.cpp); exactly one is
// linked. core/process_info.cpp builds process_arguments, process_memory,
// process_id and is_main_thread (process.hpp, threads.hpp) on them.

#include "core/growable.hpp"
#include "stxlib/process.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib::process_backend {

// The form the family's raw arguments take.
enum class ArgumentForm : u8 {
    // Each argument's bytes followed by one NUL (POSIX hosts: the argument
    // area as the kernel or the C runtime holds it).
    nul_separated = 0,
    // One command line in WTF-8, to split by the C runtime's rules
    // (Windows; core/command_line.hpp).
    windows_command_line = 1,
};

#if defined(_WIN32)
inline constexpr ArgumentForm argument_form = ArgumentForm::windows_command_line;
#else
inline constexpr ArgumentForm argument_form = ArgumentForm::nul_separated;
#endif

// Replaces `raw` with the arguments in the family's form. Fails with
// limit_exceeded (HostCode::too_long) when they hold more than `limit`
// bytes, found before copying where the host says the size first.
[[nodiscard]] Status arguments(usize limit, Growable<char>& raw) noexcept;

[[nodiscard]] u64 process_id() noexcept;

// Whether the calling thread is the initial thread, or unsupported.
[[nodiscard]] Status is_main_thread(bool& initial) noexcept;

// The resident sizes, before core raises the peak to at least the current
// size.
[[nodiscard]] Status memory(ProcessMemory& report) noexcept;

} // namespace stxlib::process_backend
