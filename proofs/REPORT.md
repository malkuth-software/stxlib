# Proof report

One entry per module in proof scope: proved (tool, version, bound,
property), assumed (with the assumption), or not yet proved (AGENTS.md,
"Verification"). "Tested" means a test passes on the inputs it names; it is
evidence, not proof.

## stxlib::boundary, batch 1 (branch `claude/interfaces-a1-boundary`)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `include/stxlib/detail/utf8.hpp` | P1: `utf8_well_formed` agrees with an independent decoder (Unicode 15.1 section 3.9, D92 as arithmetic) on every byte string of length 0 to 4. P2: `utf8_prefix` agrees with the decoder's longest well-formed prefix ending at or before the limit, for every such string and every limit 0 to n + 1 | **not yet run.** Proved for lengths 0 to 3 once `stxlib_utf8_exhaustive` passes in CTest, and for 0 to 4 once `stxlib_utf8_exhaustive --scope full` passes (exhaustive enumeration; tool: the harness itself, compiled by the pinned LLVM). Strings longer than 4 bytes: not proved | `tests/utf8_exhaustive.cpp` |
| `include/stxlib/result.hpp` | a result holds exactly one alternative; queries never read the inactive one | not yet proved; tested | `tests/vocabulary_tests.cpp` |
| `include/stxlib/error.hpp`, `src/core/error.cpp` | text fields hold well-formed UTF-8 cut at a whole sequence; bytes past the count are zero | not yet proved (the cut is `utf8_prefix`, covered above for short strings); tested at the capacity boundary | `tests/vocabulary_tests.cpp` |
| `include/stxlib/gate.hpp`, `src/core/exception_shadow.cpp` | no exception leaves a gate; each standard exception maps to its kind | not yet proved; tested for every row of the classification table | `tests/gate_tests.cpp`, `tests/death_tests.cpp` |
| `src/core/trampoline.cpp` | once revoke returns, no new invocation of the handle starts; a completed revoke returns only after every running invocation has left; a handle is never reissued | not yet proved. The argument (sequential consistency over the enter and revoke pairs) is in the file's header; a model-checking harness (for example CBMC or GenMC at the pinned versions, stage B1's choice) is the next step. Tested on one forced interleaving | `tests/trampoline_tests.cpp` |
| `src/core/memory.cpp` | a release of a block not from allocate in this image, or a second release, ends the process (tag mode) | not yet proved; must-end tests | `tests/death_tests.cpp` |
| `src/core/memory.cpp` with the host allocator | free() or operator delete of a block from allocate ends the process (tag mode; the host allocator's own check, as in stx_auxlib) | not yet proved; must-end tests `foreign_free` and `foreign_delete`. Skipped under ThreadSanitizer, whose free does not check foreign pointers (compiler-rt `lib/tsan/rtl/tsan_mman.cpp:238-245` at `llvmorg-22.1.8`; read, not proved); the property is claimed for the other five modes only | `tests/death_tests.cpp` |
| `include/stxlib/shadow.hpp`, `src/core/shadow_frames.cpp` | the innermost unclosed frame an unwind leaves captures the logical stack at the throw, its own frame included; outer frames of the same unwind add nothing; a capture an ordinary handler consumed is not attached to a later exception; the depth returns to its value before the gate | not yet proved; tested with a planted throw per boundary kind (C ABI slot gate, classified gate, trampoline entry, callback gate through the fail-fast report, parked and rethrown exception, a stale capture, a stack deeper than the capacity); each record printed in full | `tests/shadow_tests.cpp`, `tests/death_tests.cpp` (`callback_frames`) |
| `src/state/state.hpp`, `src/state/state.cpp` | every module's copy of STXlib reaches one thread state and one process state; a module built from another layout fails at its first access | not yet proved; tested with two modules (a shared library and an executable, each with a hidden copy of the boundary): an executable throw through a library frame, a library gate over an executable throw, one depth, one sequence. Not tested: a layout mismatch (needs two builds) | `tests/two_module_tests.cpp` |
| `include/stxlib/identity.hpp` | an issuer never issues 0 or the same identity twice and refuses at 2^64 - 1 | not yet proved; tested at the maximum | `tests/vocabulary_tests.cpp` |

## stxlib::boundary, batch 2a (the platform layer's first modules)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/console.cpp` | lines are split at "\n" and "\r\n", an over-limit line is refused and skipped whole, invalid UTF-8 is refused and consumed, a last line without an ending is returned, then the end | not yet proved; tested on one input file | `tests/console_process_tests.cpp`, `tests/console_process_check.cmake` |
| `src/core/process.cpp` | exit tasks run newest first, a task may register another, the status is kept | not yet proved; tested | the same |
| `src/core/environment.cpp` | concurrent sets and reads are serialized (no torn value) | not yet proved; tested with four threads, 2,000 rounds each (sampled) | `tests/platform_tests.cpp` |
| `include/stxlib/clocks.hpp` | arithmetic saturates at the ends of the 64-bit range | proved at compile time for the cases in `static_assert`s; other values not proved | `tests/platform_tests.cpp` |
| `src/hosted/*/entropy_*.cpp` | fills from the host generator, fails closed | tested: two draws differ (sampled) | `tests/platform_tests.cpp` |
| `src/hosted/*/events_*.cpp`, `terminal_*.cpp` | a watch records raised signals, defers under EventDeferral, restores the earlier handling; one watch and one raw terminal per process, across modules | not yet proved; tested on POSIX (events) and without a terminal; the Windows paths are not run here | `tests/platform_tests.cpp`, `tests/two_module_tests.cpp` |
| `src/core/spin_lock.hpp` | mutual exclusion | not yet proved; an exchange-acquire / store-release lock, the textbook argument; a model-checking harness belongs to stage B1 | none yet |

Nothing in batch 1 has been compiled or run yet: no compiler runs on the
seat's machine; the integrator builds and runs it on the M4.
