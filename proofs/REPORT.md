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
| `src/core/memory.cpp` | a release of a block not from allocate in this image, or a second release, ends the process (tag mode), reading no byte at or around an address it did not register | not yet proved. Argument by construction: release reads registry storage until the address is found, and an address is registered only between allocate_tagged writing its header inside the backend block and its release (the file's header). Must-end tests `foreign_release`, `double_release` and `foreign_release_page` (an address after an inaccessible page, the CI TSan fault of 2026-11-13); a model-checking harness of the registry belongs to stage B1 | `tests/death_tests.cpp` |
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

## stxlib::boundary, batch 2b (paths, files, desktop, child processes)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/path.cpp` | parse normalizes as path.hpp states, idempotently, and refuses the listed text; join and parent bound their result | not yet proved; tested on stx_auxlib's table (22 normalizations, 16 refusals) and the bound cases | `tests/path_tests.cpp` |
| `src/core/files.cpp`, `src/hosted/*/file_*.cpp` | each operation maps host errors as files.hpp's table; write_file leaves the old content or the new, and no temporary file; read_file and list_directory refuse over-limit content | not yet proved; tested on one scratch directory per host. Atomicity of the Windows replace is the host's and is not promised (batch 2b2: Capability::atomic_replace is false there) | `tests/files_tests.cpp` |
| `src/core/files.cpp` (`remove_tree`) | removes the tree without following links and counts what it removed; terminates (each pass removes an entry or fails, absent concurrent writers) | not yet proved; tested on a nested tree and on a link to a file outside it | the same |
| `src/hosted/*/children_*.cpp` | arguments reach the child unchanged (no shell; Windows quoting for CommandLineToArgvW); start reports a missing program; abandoned children are reaped | not yet proved; tested with seven argument shapes, exit codes, a signal, timeouts and four abandoned children | `tests/children_tests.cpp` |
| `src/core/desktop.cpp` | only http, https and mailto links and absolute paths without control characters reach the helper | not yet proved; tested on seven refused targets; nothing is opened in tests | the same |

## stxlib::boundary, batch 2b2 (owner decision D13 a: lossless names, durability levels, atomic replace)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/wtf8.hpp`, `src/core/path.cpp` (host names) | N1: for every POSIX name of 1 to 3 bytes over the bytes 1..255 except '/' (16,451,834 names), `from_posix_name` succeeds with the reference escape as its text and `posix_name` returns the bytes. N2: for every text of one or two tokens from the harness's set T (392 tokens: the ASCII characters but `/`, `:` and `\`, all 256 escapes, other surrogate ends, scalar boundaries; 153,664 pairs), `posix_name` succeeds exactly when the reference says the text is an escape image, and `from_posix_name` returns the text (injectivity on the domain). N3: for every 16-bit unit but 0, `/`, `\`, `:` (65,532) and every pair from W (all 2,048 surrogates and 8 others; 4,227,136 pairs), `from_windows_name` gives the reference WTF-8 and `windows_name` returns the units. N4: `parse_wtf8` of that WTF-8 gives the same Path | **not yet run.** Proved on those domains once `stxlib_path_names_exhaustive` passes (exhaustive enumeration against an independent reference: D92 decoding and the WTF-8 definition, written in the harness; tool: the harness, compiled by the pinned LLVM). Longer names, longer token texts and longer unit sequences: not proved (the conversions are left-to-right with at most one sequence of lookahead; that argument is not itself checked) | `tests/path_names_exhaustive.cpp` |
| `src/core/path.cpp` (`PathText`) | `display` writes well-formed UTF-8 of the same length; `is_unicode` is true exactly when the text holds no surrogate | not yet proved; tested | `tests/path_tests.cpp` |
| `src/core/files.cpp`, `src/hosted/*/file_*.cpp` (D13 a) | odd host names are listed, read, written and removed by Path: POSIX bytes that are not UTF-8 (where the filesystem accepts them; APFS refuses them with EILSEQ, observed on macOS 26), `:` and `\`; Windows lone surrogates (where the volume accepts them); a Path no POSIX name spells is refused with invalid_name; the three durability levels flush as files.hpp states | not yet proved; tested on one scratch directory per host. That a level's flush reaches stable storage is the host's promise (fcntl(2), fsync(2), FlushFileBuffers, MoveFileExW) and is not testable without power-loss equipment | `tests/files_tests.cpp`, `tests/platform_tests.cpp` |

## stxlib::boundary, batch 2c (threads, the thread pool, the local time zone)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/threads.cpp`, `src/hosted/*/threads_*.cpp` | identities are distinct and never reused; destruction and move assignment stop and join; a self-join is refused; Mutex excludes; a timed wait reports timed_out only once MonotonicTime has reached the deadline; call_once runs its entry once and publishes its writes; sleeps never end early | not yet proved; tested (64 sequential threads, 8 threads x 20,000 locked increments, 8 racing call_once callers, 15 ms and 20 ms timings). The timeout rule holds by construction (core decides timed_out by MonotonicTime, not the host); that argument is not checked by a tool | `tests/threads_tests.cpp` |
| `src/core/thread_pool.cpp` | each task of a batch runs exactly once; small batches run on the caller in order, larger ones on the workers only; batches are serialized; refusals run nothing | not yet proved; tested on batch sizes around the caller limit and claim sizes (1 to 100,000 tasks). Serialization is sampled (a 20 ms window). The conformance test against stx_auxlib's C reference arrives in batch 3 | `tests/thread_pool_tests.cpp` |
| `src/core/civil.hpp` | days_from_civil is Hinnant's algorithm | four values checked at compile time (static_assert), against Python's datetime; not proved for other dates | `src/core/civil.hpp` |
| `src/core/local_time.cpp`, `src/hosted/*/local_time_*.cpp` | the reading and its offset name the instant; a TZ set through set_variable is seen by the next call | not yet proved; tested on three POSIX TZ rules (UTC, US Eastern in winter and summer, +05:30) and on Windows' system zone and its 1601 limit | `tests/local_time_tests.cpp` |
| `src/hosted/windows/children_windows.cpp` (wait) | timed_out only once MonotonicTime has reached the deadline (CI run 43432600851 saw WaitForSingleObject end a 50 ms wait early) | not yet proved; the wait now loops on MonotonicTime as the POSIX wait does; tested | `tests/children_tests.cpp` |

Nothing in batch 1 has been compiled or run yet: no compiler runs on the
seat's machine; the integrator builds and runs it on the M4.
