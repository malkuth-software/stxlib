# Provenance

Code in this repository that came from elsewhere, with its source, commit
and how it was taken. Third-party code is pinned and hashed here and never
edited in place (AGENTS.md). First-party code ported from stx_auxlib is
listed so the port can be compared with its origin.

## Ported from stx_auxlib (first party, MIT)

Copy point (decision M2): malkuth-software/stx_auxlib main at
`b6e330a73771253c87bf7afbe436476dbfc55cd2` (2026-11-13: `3d9bed2` plus the
CMake fix `d93d169`; stx_auxlib is frozen there). Every later batch copies
from this commit. Batch 1 was first read at `08c8132d41a8b210dd8dfda34b13d8da81b09e79`;
`git diff --stat 08c8132 b6e330a` over the five source files below and
`tests/ownership_death_tests.cpp` is empty, so the ports are the same at
`b6e330a`. Read from the STXlib workspace's reference clone. "Ported" means
rewritten to STXlib's vocabulary and layout with the same algorithm; the
differences are stated in each file's header comment.

| STXlib file | stx_auxlib file | Change |
|---|---|---|
| `src/core/memory.cpp` | `src/hosted/memory.cpp` | the tag value is derived from addresses, not host entropy (entropy module not yet ported); host calls through `src/hosted/host.hpp`; the report goes through `src/core/report.cpp`; release consults a per-image registry of live blocks before reading any header (stx_auxlib read the tag below any address, which faults for a foreign block at the start of a mapping) |
| `src/hosted/c_library/c_library.cpp` (`allocate_block`, `release_block`) | `src/hosted/memory_host.cpp` | the alignment is validated here too |
| `src/hosted/posix/console_posix.cpp` (`write_standard_error`) | `src/hosted/console_posix.cpp` (`write`) | standard error only; failures are ignored (used only while reporting a failure) |
| `src/core/checked.hpp` (`STXLIB_MODULAR`) | `src/checked.hpp` (`STX_AUXLIB_MODULAR`) | the marker only |
| `include/stxlib/memory.hpp` | `include/stx_auxlib/memory.hpp` | the contract, restated for STXlib; `in_backend_heap` not carried (no mimalloc backend yet) |
| `tests/death_tests.cpp` (`double_release`, `foreign_release`, `foreign_free`, `foreign_delete`) | `tests/ownership_death_tests.cpp` | one process per case started by CTest instead of a parent that starts children; `foreign_free` and `foreign_delete` skipped under ThreadSanitizer (proofs/REPORT.md) |

### Batch 2a: the platform layer's first modules, from `b6e330a`

Ported files keep the algorithm and the settled platform differences; the
interface is restated in STXlib's vocabulary (no `std::` in headers;
failures as error records; process-wide state in `stxlib_state`). Each
file's header comment names its changes.

| STXlib file | stx_auxlib file (at `b6e330a`) |
|---|---|
| `include/stxlib/capabilities.hpp`, `src/core/capabilities.cpp` | `include/stx_auxlib/capabilities.hpp`; `capability_available` in `src/hosted/desktop_posix.cpp` and `desktop_windows.cpp` |
| `include/stxlib/clocks.hpp`, `src/hosted/cxx_runtime/clocks.cpp` | `include/stx_auxlib/clocks.hpp`, `src/hosted/clocks.cpp` |
| `include/stxlib/console.hpp`, `src/core/console.cpp`, `src/hosted/console_backend.hpp` | `include/stx_auxlib/console.hpp`, `src/hosted/console.cpp`, `src/hosted/console_backend.hpp` |
| `src/hosted/posix/console_posix.cpp`, `src/hosted/windows/console_windows.cpp` | `src/hosted/console_posix.cpp`, `src/hosted/console_windows.cpp` |
| `include/stxlib/environment.hpp`, `src/core/environment.cpp`, `src/hosted/environment_backend.hpp` | `include/stx_auxlib/environment.hpp`, `src/hosted/environment.cpp`, `src/hosted/environment_backend.hpp` |
| `src/hosted/posix/environment_posix.cpp`, `src/hosted/windows/environment_windows.cpp` | `src/hosted/environment_posix.cpp`, `src/hosted/environment_windows.cpp` |
| `src/core/spin_lock.hpp` (the yielding part) | `src/hosted/environment_lock.hpp` |
| `include/stxlib/entropy.hpp`, `src/hosted/posix/entropy_posix.cpp`, `src/hosted/windows/entropy_windows.cpp` | `random::fill_entropy` in `include/stx_auxlib/random.hpp`, `src/hosted/entropy_posix.cpp`, `src/hosted/entropy_windows.cpp` |
| `include/stxlib/process.hpp`, `src/core/process.cpp` | `include/stx_auxlib/process.hpp`, `src/hosted/process.cpp` (exit tasks; `fail` and `ensure` replaced by `STXLIB_REQUIRE`) |
| `include/stxlib/terminal.hpp`, `src/hosted/posix/terminal_posix.cpp`, `src/hosted/windows/terminal_windows.cpp` | `include/stx_auxlib/terminal.hpp`, `src/hosted/terminal_posix.cpp`, `src/hosted/terminal_windows.cpp` |
| `include/stxlib/events.hpp`, `src/hosted/posix/events_posix.cpp`, `src/hosted/windows/events_windows.cpp` | `include/stx_auxlib/events.hpp`, `src/hosted/events_posix.cpp`, `src/hosted/events_windows.cpp` |
| `src/hosted/windows/utf16.hpp`, `utf16.cpp` | `append_utf16` and `append_utf8` in `src/unicode.cpp` |
| `src/core/memory.cpp` (the image value now drawn from entropy) | `src/hosted/memory.cpp` |
| `tests/console_process_tests.cpp`, `tests/console_process_check.cmake` | the same paths |
| `tests/platform_tests.cpp` | `tests/clocks_tests.cpp`, `tests/environment_tests.cpp`, the terminal and events parts of `tests/capabilities_tests.cpp` |

The remaining stx_auxlib content (hosted modules, owned algorithms, and
stx_btree with the sequence tree and RowHeightIndex) is copied from
`b6e330a` in later batches, each file with its source path, commit and
SHA-256 (decision M2, `decisions/2026-11-13-survey-round-4.md`).

## Third party

None yet. mimalloc v3.5.3 arrives with the algorithms batch, with its
PROVENANCE entry and hashes from stx_auxlib unchanged.
