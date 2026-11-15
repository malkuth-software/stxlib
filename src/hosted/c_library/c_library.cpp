// The C standard library's part of the interface modules (host.hpp):
// abnormal termination, stream flushing and atexit, the same calls on every
// family. The host allocator that was here (stx_auxlib's host backend)
// moved into stxlib_state with the rest of the allocator (layout 5,
// src/state/allocator.cpp), so every module shares one heap.
//
// Emscripten (owner decision 2026-11-15; early exit,
// decisions/2026-11-15-early-exit-integrity.md). Facts at emscripten
// 6.0.13:
//   - abort (end_process_abnormally, fail-fast's end) prints "Aborted(...)"
//     and, in a build with wasm exceptions (GUI.Forms' -fwasm-exceptions),
//     traps (src/preamble.js:247-290, the trap at :275-287): a trap is not
//     an exception, so no frame's cleanup runs and nothing catches it. The
//     runtime is then aborted; no invariant of STXlib's is observed again.
//   - exit (exit_normally) on the main thread ends the runtime and runs
//     atexit handlers only with EXIT_RUNTIME (src/settings.js; exit_tasks is
//     false without it). Without EXIT_RUNTIME, and on any worker thread, it
//     instead throws to unwind to the JavaScript event loop
//     (src/lib/libcore.js:128-141, handled as control flow at :2084-2095),
//     and the page and its memory live on. That throw crosses STXlib's
//     frames, compiled without exceptions, without running their cleanups:
//     an early exit of the second kind. STXlib calls exit only from
//     exit_process (process.hpp), one call with no STXlib lock, shadow
//     frame or partial structure of its own open around it, so what STXlib
//     leaves is whole; anything of the caller's above it is the caller's to
//     keep whole, as with any call that does not return. The exit tasks
//     run inside exit, from its atexit handler, so only with EXIT_RUNTIME.
//     emscripten_force_exit (emscripten.h:83) behaves the same without
//     EXIT_RUNTIME (libcore.js:2032-2046) and is not used.

#include "hosted/host.hpp"

#include <cstdio>
#include <cstdlib>

namespace stxlib::host {

void end_process_abnormally() noexcept { std::abort(); }

void flush_c_stream(const bool error) noexcept { static_cast<void>(std::fflush(error ? stderr : stdout)); }

bool register_at_exit(void (*const handler)()) noexcept {
    const bool result = std::atexit(handler) == 0;
    return result;
}

} // namespace stxlib::host
