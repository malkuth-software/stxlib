#pragma once

// The interface modules' private surface: the only way code outside
// src/hosted/ reaches the host (AGENTS.md: nothing outside the interface
// modules issues a system call or includes an operating-system header).
//
// One implementation of each function is linked, chosen by the host family
// in CMakeLists.txt:
//   src/hosted/c_library/    the C standard library, the same on every family
//   src/hosted/cxx_runtime/  the C++ runtime's exception interface (Itanium C++ ABI)
//   src/hosted/posix/        POSIX calls shared by macOS, glibc and musl
//   src/hosted/windows/      Win32 and UCRT
// Family directories for macOS, glibc and musl join when a service differs
// between them (the port of stx_auxlib's hosted modules).
//
// These are the first services, the ones the boundary core needs to report
// and fail. Each is used only where a failure is already being reported, so
// none returns a result: a failed diagnostic write is ignored, by design.

#include "stxlib/types.hpp"

namespace stxlib::host {

// Writes bytes[0, count) to standard error, retrying interrupted and
// partial writes; stops silently if the stream is closed or fails.
// Allocates nothing.
void write_standard_error(const char* bytes, usize count) noexcept;

// Ends the process abnormally, so the host keeps a crash report or core
// where it makes one. No exit handler or static destructor runs.
[[noreturn]] void end_process_abnormally() noexcept;

// Offers the processor to another thread (a spin-wait's pause).
void yield_thread() noexcept;

// Suspends the calling thread for about a millisecond (spin_lock.hpp).
void sleep_briefly() noexcept;

// Fills `count` bytes from the host's cryptographic generator; false, with
// the bytes zeroed, when the host cannot (entropy.cpp). Ported from
// stx_auxlib's random::fill_entropy (src/hosted/entropy_<host>.cpp).
[[nodiscard]] bool fill_entropy(Byte* bytes, usize count) noexcept;

// Flushes the C library's stdout (error false) or stderr (error true), so
// console output keeps its order with printf (console.cpp).
void flush_c_stream(bool error) noexcept;

// The C library's exit (normal termination: atexit handlers, static
// destructors, stream flushing) and atexit (process.cpp).
[[noreturn]] void exit_normally(int status) noexcept;
[[nodiscard]] bool register_at_exit(void (*handler)()) noexcept;

// The host allocator behind stxlib::memory (memory.hpp's "Backend: host").
// allocate_block returns `size` bytes aligned to `alignment` (a power of
// two), or null when the size with its bookkeeping is not representable or
// memory is exhausted; release_block takes only its results, or null.
[[nodiscard]] void* allocate_block(usize size, usize alignment) noexcept;
void release_block(void* block) noexcept;

// Writes up to `capacity` return addresses of the calling thread's stack,
// innermost first, starting at the caller of this function's caller or
// deeper; returns how many were written. 0 when the host cannot walk the
// stack. Allocates nothing.
[[nodiscard]] usize capture_return_addresses(uptr* addresses, usize capacity) noexcept;

// The C++ runtime's mangled name of the exception being handled, or null
// when there is none or the runtime cannot say. Called only inside a catch
// handler. The text has static storage (the type's type_info).
[[nodiscard]] const char* current_exception_type_name() noexcept;

// Demangles the NUL-terminated `mangled` into `output` (cut to `capacity`
// bytes, no terminator) and returns the bytes written; 0 when the runtime
// cannot demangle it or memory runs out. Allocates and frees one block.
[[nodiscard]] usize demangle_type_name(const char* mangled, char* output, usize capacity) noexcept;

} // namespace stxlib::host
