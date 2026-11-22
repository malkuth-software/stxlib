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
| `src/core/trampoline.cpp` | once revoke returns, no new invocation of the handle starts; a completed revoke returns only after every running invocation has left; a handle is never reissued | not yet proved. The argument (sequential consistency over the enter and revoke pairs) is in the file's header; a model-checking harness (for example CBMC or GenMC at the pinned versions, stage B1's choice) is the next step. Tested on one forced interleaving, and on a stale caller racing 4000 self-revocations (A-stxlib-core-01-F2: the last leaver frees the state's own generation) | `tests/trampoline_tests.cpp` |
| `src/core/memory.cpp` | a release of a block not from allocate in this image, or a second release, ends the process (tag mode), reading no byte at or around an address it did not register | not yet proved. Argument by construction: release reads registry storage until the address is found, and an address is registered only between allocate_tagged writing its header inside the backend block and its release (the file's header). Must-end tests `foreign_release`, `double_release` and `foreign_release_page` (an address after an inaccessible page, the CI TSan fault of 2026-11-13); a model-checking harness of the registry belongs to stage B1 | `tests/death_tests.cpp` |
| `src/core/memory.cpp` with the host allocator | free() or operator delete of a block from allocate ends the process (tag mode; the host allocator's own check, as in stx_auxlib) | not yet proved; must-end tests `foreign_free` and `foreign_delete`. Skipped under ThreadSanitizer, whose free does not check foreign pointers (compiler-rt `lib/tsan/rtl/tsan_mman.cpp:238-245` at `llvmorg-22.1.8`; read, not proved); the property is claimed for the other five modes only | `tests/death_tests.cpp` |
| `include/stxlib/shadow.hpp`, `src/core/shadow_frames.cpp` | the innermost unclosed frame an unwind leaves captures the logical stack at the throw, its own frame included, in every build (trace in development builds only); outer frames of the same unwind add nothing; a capture an ordinary handler consumed is not attached to a later exception; the depth returns to its value before the gate; a gate's frame is the whole frame (kind, numbers, sequence, texts) and appears once; texts are cut at the last whole UTF-8 sequence that fits, flagged, the rest zero; frames opened by hand (enter_shadow_frame, leave_shadow_frame) behave as a gate's; read_shadow_stack reports depth, recorded, pushes and dropped | not yet proved; tested with a planted throw per boundary kind (C ABI slot gate with GUI.Forms' kind and the message sink, classified gate, consumer frames with texts, a hand frame, trampoline entry, callback gate through the fail-fast report, classified callback gate with an event frame, parked and rethrown exception, a stale capture, a stack deeper than the capacity), a slot gate in one module around a hand frame in another, and the reporter contract (reads allowed; a frame opened from a reporter fails a requirement in development builds; a nested fail fast gets STXlib's report); each record printed in full | `tests/shadow_tests.cpp`, `tests/two_module_tests.cpp`, `tests/gate_no_exceptions_tests.cpp`, `tests/death_tests.cpp` (`callback_frames`, `callback_frame_texts`, `reporter_contract`, `reporter_opens_frame`, `reporter_nested`) |
| `src/state/state.hpp`, `src/state/state.cpp` | every module's copy of STXlib reaches one thread state and one process state; a module built from another layout fails at its first access | not yet proved; tested with two modules (a shared library and an executable, each with a hidden copy of the boundary): an executable throw through a library frame, a library gate over an executable throw, one depth, one sequence. Not tested: a layout mismatch (needs two builds) | `tests/two_module_tests.cpp` |
| `include/stxlib/thread_records.hpp`, `src/state/state.cpp`, `src/hosted/*/thread_exit_*.cpp` (layout 10) | each thread's block and records are created on first access and released once, at a normal thread exit or end_thread_records, newest type first and STXlib's state last; nothing is created after the hook starts; process exit releases nothing; the only thread_local is trivially destructible (static_assert); a thread with no block records on the anchor's failure reserve | not yet proved; written, not yet built or run (sb3, 2026-11-15). Tests: registration limits, first access, release order and cross-slot visibility in release callbacks, re-entry from initialize, end_thread_records, a C++ thread_local destructor (order printed, safety checked), a later key destructor (disposed and the reserve), the main thread kept at exit. Not tested: the reserve after a failed first allocation | `tests/thread_records_tests.cpp` |
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
| `src/core/path.cpp` | parse normalizes as path.hpp states, idempotently, and refuses the listed text; join and parent bound their result; the longest host names (a POSIX name of 4,096 escaped bytes, a Windows name of 32,767 units) are within maximum_path_bytes | not yet proved; tested on stx_auxlib's table (22 normalizations, 16 refusals), the bound cases, and both longest names (3 x 4,096 and 3 x 32,767 bytes of text) with one unit over the bound refused | `tests/path_tests.cpp` |
| `src/core/files.cpp`, `src/hosted/*/file_*.cpp` | each operation maps host errors as files.hpp's table; write_file leaves the old content or the new, and no temporary file; read_file and list_directory refuse over-limit content | not yet proved; tested on one scratch directory per host. Atomicity of the Windows replace is the host's and is not promised (batch 2b2: Capability::atomic_replace is false there) | `tests/files_tests.cpp` |
| `src/core/files.cpp` (`remove_tree`) | removes the tree without following links and counts what it removed; terminates (each pass removes an entry or fails, absent concurrent writers) | not yet proved; tested on a nested tree and on a link to a file outside it | the same |
| `src/hosted/*/children_*.cpp` | arguments reach the child unchanged (no shell; Windows quoting for CommandLineToArgvW); start reports a missing program; abandoned children are reaped; a PathText argument reaches the child as the host spelling it stands for (POSIX: escapes become their bytes, any other surrogate is refused with invalid_value; Windows: lone surrogates become their units) | not yet proved; tested with seven argument shapes, exit codes, a signal, timeouts, four abandoned children, and the child's hex echo of one escaped POSIX argument (invalid bytes, ':' and '\'), one Windows argument with two lone surrogates, and one refused surrogate | `tests/children_tests.cpp` |
| `src/core/desktop.cpp` | only http, https and mailto links and absolute paths without control characters reach the helper; reveal passes a path that is not valid Unicode as the host spells it | not yet proved; tested on seven refused targets; nothing is opened in tests | the same |

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

## stxlib::boundary, batch 3a (one allocator across modules; owner decision D11 b)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/state/allocator.cpp` | a block from memory::allocate in any of STXlib's modules is released by memory::release in any other | not yet proved; tested with a shared library and a program, each with its own copy of the boundary, exchanging raw blocks (0 B to 3 MB), a Path, a Thread and an AtomicPool, in tag-checking builds where a block the allocator did not recognise would end the process | `tests/allocator_two_module_tests.cpp` |
| `src/state/allocator.cpp` (tags, host backend) | as batch 2a's foreign-release fix: the registry, unchanged, now in stxlib_state | as before (sanitizer builds and Emscripten use this backend; the C mimalloc backend and its page-map argument were removed on 2026-11-15) | the same |
| `src/core/threads.cpp` (D11) | the stated stack size, rounded to the page size, with a guard page; recursive locking and foreign unlocks fail fast in check builds; a predicate wait returns satisfied only when the predicate is true and timed_out only once MonotonicTime reached the deadline with it false | not yet proved; tested: 4 MiB and 8 MiB stacks filled to three quarters, refused sizes, must-end cases `recursive_lock` and `foreign_unlock`, timed waits. The wait rule holds by construction (the predicate is checked under the lock before each host wait and the deadline read from MonotonicTime); not checked by a tool | `tests/threads_tests.cpp`, `tests/death_tests.cpp` |

## stxlib::boundary, batch 3b (owner decision D12 a: zones and clocks)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `tools/tzcompile.py` (the zone compiler) | for every zone and link of tzdata 2026f, the observable local time type (offset, daylight flag, abbreviation) at every whole second of WallTime's range is zic's | not proved: zic is not run here. Compared by `tools/tzcheck.py` with the host's zic-built TZif files (macOS, tzdata 2026e) through Python's zoneinfo, at every transition of either side and one second before it, and at every change of the reference's footer rule found by bisecting a weekly grid, through 2262: 252,620 instants over 598 names; 568 agree; 7 differ only in the daylight flag (Africa/Casablanca, Africa/El_Aaiun, Africa/Windhoek, Eire, Europe/Bratislava, Europe/Dublin, Europe/Prague: negative saves in tzdata's main form, positive in the rearguard form the reference was built from); 23 differ, all in 1965 and 1966 (16 Brazilian zones and their links) or 2026 and 2027 (America/Havana, Cuba), the changes 2026f's NEWS lists against 2026e. The grid could miss a reference change shorter than a week (none exists in tzdata's rules) | `tools/tzcheck.py` |
| `src/zones/zone_data.inc` with the reader's rule (footer) | the tables and the reader's rule give the compiler's full expansion (through 2263, no footer) at every whole second of the range, for every name | checked exhaustively by `tools/tzvectors.py` over a Python transcription of `src/core/zones.cpp`'s reader applied to the generated file as text: at every change point of the expansion or of the reader (its transitions, footer start and every footer event through 2263) and one second before each, for all 598 names; 252,898 instants, no difference. Both readings are constant between consecutive change points, so this covers every second. A mutation (one footer rule's time moved an hour) gave 948 differences. It checks the transcription, not the compiled C++ | `tools/tzvectors.py` |
| `src/core/zones.cpp` (compiled) | the C++ reader agrees with the expansion | tested, not proved: 10,936 vectors (both range ends, first and last transitions, footer-region transitions in 2026, 2100 and 2262, and spaced transitions, each with the second before) for all 598 names, `tests/local_time_tests.cpp`; the tables' internal agreement (indices in range, transitions ascending) is checked at compile time (`tables_agree`) | `tests/local_time_tests.cpp`, `src/core/zones.cpp` |
| `src/core/civil.hpp` (`civil_from_days`, `civil_from_seconds`) | the civil time names the instant, with a valid month and day, for every day of WallTime's range | exhaustive over the days of the range (one instant per day, its time of day varying), through `local_time_in(Zone::utc())`, against the test's own `days_from_civil`; five compile-time checks | `tests/local_time_tests.cpp` |
| `src/core/local_time.cpp`, `src/hosted/*/local_time_*.cpp` | the host's zone is TZ's name or path, else the host's zone name, by local_time.hpp's rules | tested, not proved: TZ forms (plain, ':', zoneinfo paths with posix/ and right/, empty, ':' alone, a link) and refusals (POSIX rule strings, unknown names, a path with no zoneinfo part) on every host; the system zone only as "some zone the database knows". Windows' ICU mapping takes its function type from the SDK's <icu.h> by decltype where the toolchain carries it, and restates it from ICU's ucal.h by hand only where it does not (MSYS2's mingw-w64 headers), the one remaining exception to house style section 2 | `tests/local_time_tests.cpp` |
| `src/hosted/*/clocks_*.cpp` | IntervalTime stops during suspension and ElapsedTime does not; both never decrease | the suspension behaviour is the hosts' documented clock semantics (cited in each file), not tested (a test would have to suspend the machine). Never decreasing: tested over 100,000 reads; spans over a 50 ms sleep agree with std::chrono::steady_clock | `tests/platform_tests.cpp` |

## stxlib::algorithms, batch 3c (first part: hash, random, SIMD level; the pool conformance test)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/hash.cpp` | BLAKE2s-256 as RFC 7693 defines it, keyed and unkeyed; feeding in pieces gives the one-call digest; a key over 32 bytes is cut to 32 | tested, not proved: RFC 7693's "abc", the empty input, 200 bytes, and the BLAKE2 reference's keyed vectors (key 0..31, empty and 0..255 input); each expected digest was recomputed on 2026-11-13 with Python's hashlib.blake2s and agrees. Pieces of 7 bytes against one call | `tests/hash_tests.cpp` |
| `src/algorithms/hash.cpp` (`secure_erase`) | every byte is zero afterwards and the stores are not removed | the zeroing is tested; that the compiler keeps the stores is argued from the implementation (stores through a volatile pointer), not checked by a tool | `tests/hash_tests.cpp` |
| `src/algorithms/random.cpp` | ChaCha12Stream, Xoshiro256PlusPlus (with jump and long jump) and seed_from_number give stx_auxlib's sequences; the ChaCha core is RFC 8439's | tested, not proved: known answers from `tests/reference/random_reference.py` (an independent Python implementation; its output was rerun on 2026-11-13 and every expected value in the test is either one of its outputs or an input), the RFC 8439 section 2.3.2 block, the 128-bit product checked against Python's integers | `tests/random_tests.cpp` |
| `include/stxlib/random.hpp` (distributions, shuffle) | uniform_below is unbiased (Lemire's rejection), uniform_between covers its closed range, the unit draws lie in [0, 1), shuffle is a permutation | tested, not proved: known answers (ten die rolls, one ten-card shuffle) are exact; ranges are sampled (10,000 draws each, including full-range and degenerate bounds); uniformity is sampled (600,000 rolls of a die from a fixed seed, each face within 1% of its share). Lemire's rejection is unbiased by its published argument, not checked by a tool | `tests/random_tests.cpp` |
| `src/algorithms/simd.cpp` | the hardware level never exceeds what CPUID and XCR0 report; one selection per process, fixed at the first selected_level call; a request lowers the level only before it | tested, not proved, on each CI machine's CPU (the test prints the detection); the once-only rule rests on one compare-exchange of `ProcessState::simd_selection` (layout 7), argued, not checked by a tool | `tests/simd_tests.cpp` |
| `src/core/thread_pool.cpp` (against its C original) | every check the C reference passes, STXlib's pool passes: argument and task validation, which thread runs a batch, the caller-path threshold, one-worker order, exactly-once execution, worker-only execution and serialization of concurrent batches | tested, not proved: one conformance source built twice, against `tests/reference/atomic_pool.c` (unchanged, hashed in PROVENANCE.md) and against `tests/reference/atomic_pool_adapter.cpp` over stxlib::AtomicPool; both must pass. Concurrency checks are sampled | `tests/thread_pool_conformance.cpp` |

## stxlib::algorithms, batch 3c2 (numeric text, the calendar)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/numeric_text.cpp` (formatting) | integers, fixed and scientific reals are as the standard's to_chars defines them, padded as the format says; output is whole or nothing, with the size needed; the format bounds cap the size | tested, not proved: stx_auxlib's exact expectations; fixed and scientific output equals the host's snprintf on 50,000 sampled doubles over every exponent (precision 0 to 24); caller-storage cases (too small, empty storage, fields, TextWriter) exact | `tests/numeric_text_tests.cpp` |
| `src/algorithms/numeric_text.cpp` (shortest form, parsing) | the shortest text parses back to the same value; parse failures are named as the header says | tested, not proved: round trip on 200,000 sampled doubles and 100,000 sampled floats (bit patterns from SplitMix64, so every exponent and subnormals); stx_auxlib's parse and scanner cases exact | `tests/numeric_text_tests.cpp` |
| `src/algorithms/calendar.cpp` (civil days, weekdays) | civil_from_wall and weekday_of agree with std::chrono's year_month_day and weekday, and wall_from_civil inverts civil_from_wall | exhaustive over the days of WallTime's range (one instant per day, at noon) against std::chrono; the range ends exact; 100,000 sampled instants round-trip | `tests/calendar_tests.cpp` |
| `src/algorithms/calendar.cpp` (ISO 8601) | format_iso8601 writes the forms the header names; parse_iso8601 accepts exactly those forms and gives the instant | tested, not proved: stx_auxlib's cases plus truncated offsets, the longest text and too-small storage; 10,000 sampled instants written with nine fraction digits parse back exactly; a compiled zone's local time written as text names its instant | `tests/calendar_tests.cpp` |

## stxlib::algorithms, batch 3c3 (the precedence chain)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/precedence.cpp` | the hashes are stx_auxlib's bytes (first key root, first leaf, signature and key path ends) | known answers from `tests/reference/precedence_reference.py`, rerun on 2026-11-13 with the same output; tested, not proved | `tests/precedence_tests.cpp` |
| `src/algorithms/precedence.cpp` | every certificate a chain issues verifies, in order, across block boundaries; consecutive certificates link; a changed info, count, time, epoch, signature, key path or next root, or another secret's certificate, fails; save and restore continue bit for bit | tested on fixed seeds over 522 epochs (two block boundaries), not proved. WOTS+ and Merkle security is the published constructions' argument, not checked here | `tests/precedence_tests.cpp` |

## stxlib::algorithms, batch 3c4 (Preference Ranked Voting)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/voting.cpp` | the outcome (winner or named failure) is the reference prv-core.mjs's for every election | tested, not proved: 420 generated elections (full, partial and messy ballots; mini counts and brackets) against the reference's outcomes, `tests/reference/prv_cases.txt`, regenerated with node on 2026-11-13; weighted copies against single ballots. The port off std::vector is checked only by these tests | `tests/voting_tests.cpp` |

## stxlib::algorithms, batch 3c5 (the owned math kernels)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/math/` (every function) | the same bits as stx_auxlib's kernels on every input the tests reach, under every variant this machine runs (native or v128 and avx2_fma), and under the Dekker exact product | tested, not proved: the frozen output hashes of `tests/math_tests.cpp`, carried unchanged from stx_auxlib, in ctest as `stxlib_math_tests`, `stxlib_math_dekker_tests` and, with dispatch, one run per forced SIMD level | `tests/math_tests.cpp` |
| `src/algorithms/math/` (float functions) | correctly rounded over the ranges `math.hpp` states (from batch 3c8: of the unflushed functions; the public ones are those results flushed, below) | exhaustive in stx_auxlib (`math_tests --exhaustive`, counts as `math.hpp` gives them). Rerun on STXlib's build (macOS arm64, Clang 22.1.8, Release, 2026-11-13, sbi): sincos, exp, log, log2 and log10 correctly rounded at every float (stride 1); pow(float) correctly rounded at 94,553,354 random pairs and hypot(float) at 100,000,000 (sampled, as in stx_auxlib). Inverse trigonometric and the remaining float functions not yet rerun (the full run stopped at pow(double), below). **Superseded for the logs (R3):** those screens accepted a result equal to the C++ library's double rounded to float (R2 note 4.1); the hardened screens (M4, main 45b275b) found log(float) misrounded at five floats (0x1.827a74p-7, 0x1.2f1fd6p+3, 0x1.bacb4ap+25, 0x1.b121a6p+76, 0x1.6351d8p+95), each confirmed against the 400-bit reference (region_exhaustive.py --double-rounding, Neo, every positive normal float): today's cold fallback rounded the double result to float, a second rounding. The same model finds log10 wrong at 0x1.fddcf4p-98 (not yet reached on the M4) and log2 at none. Fixed on `claude/math-r3` (the fallback calls the region kernel); the corrected function's exhaustive run is not yet done | `tests/math_tests.cpp --exhaustive` |
| `src/algorithms/math/sincos_core.hpp` (double sin, cos, tan) | within 0.51 ulp for 2^-27 <= abs(x) <= 2^32 | proved in stx_auxlib by `tools/math/sincos_certificate.py` (interval error analysis over every binade's worst remainders); the certificate reads this tree's constants, and its search tool builds from batch 3c7 (`stxlib_sincos_minimum_remainder`). Rerun on STXlib's tree 2026-11-13 (sbi, macOS arm64, Clang 22.1.8, Release): `--search stxlib_sincos_minimum_remainder --check tests/reference/sincos_worst_cases.hpp` holds, composed bound 0.5100 ulp | `tools/math/sincos_certificate.py` |
| `src/algorithms/math/` (double functions other than sin, cos, tan) | within the ulp bounds `math.hpp` states | sampled in stx_auxlib (sample counts as `math.hpp` gives them); not proved | `tests/math_tests.cpp` |
| `src/algorithms/math/pow.cpp` (pow(double), subnormal results; from batch 3c8 `stxlib::math::unflushed::pow`, whose subnormal results the public pow flushes, row below) | within the stated bound, 0.576 ulp | **known open accuracy defect, inherited from stx_auxlib**: `math_tests --exhaustive` (20,000,000 inputs in the subnormal region) finds 6 results not correctly rounded, worst 1.000000 ulp of the subnormal grid (2^-1074) at x = 0x1.01015c07299dbp+0, y = -0x1.61db87648208dp+17. stx_auxlib b6e330a's own test, built with its own flags (identical: -O3 -ffp-contract=off -fno-fast-math -fno-math-errno), gives the same count, worst case and input, so the port changed nothing. The other pow(double) regions are within the bound (worst 0.508798 ulp near overflow). Open; the owner decides the fix (2026-11-13, sbi) | `tests/math_tests.cpp --exhaustive` |

## stxlib::algorithms, batch 3c8 (no subnormals in the math)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/math/flush.cpp` (every public function, float) | the public result is, bit for bit, flushed(unflushed(flushed(arguments))) and never subnormal | **not yet run.** Once `stxlib_math_tests --exhaustive` passes, this is proved for every float argument of the one-argument functions and of sincos, by exhaustive enumeration of all 2^32 patterns (tool: the test, compiled with STXlib's flags), and for every subnormal float against 28 fixed partners for pow, hypot and atan2. Other pairs: sampled. The ctest run strides (tested, not proved) | `tests/math_tests.cpp` (`flush_rule`) |
| `src/algorithms/math/flush.cpp` (double) | as above | sampled, not proved: 20 million rounds of 51 cases in `--exhaustive` (random bit patterns, random subnormals, exp and pow arguments with subnormal results, atan2's tiny quotients) | `tests/math_tests.cpp` (`flush_rule`) |
| `include/stxlib/math.hpp` (subnormal outcomes) | each outcome the contract lists (sincos (zero, 1), exp 1 and +0, log -infinity for either sign, pow's Annex F zero cases, hypot, atan2, cbrt's zero, the hyperbolic cases) | tested case by case, not proved | `tests/math_tests.cpp` (`public_special_values`) |
| `src/algorithms/math/` (exp, pow and hypot double, subnormal results; log double, subnormal arguments) | the flush is exact: +0 wherever the correctly rounded result is at least one subnormal ulp below 2^-1022 (+0 or 2^-1022 within that ulp); -infinity for subnormal log arguments of either sign | **not yet run;** sampled, not proved, at 20 million inputs per region in `--exhaustive`. This replaces the ulp measurement there, so the 6 of 20 million pow results one ulp off on the subnormal grid (stx_auxlib b6e330a's kernel, found by sbi) are no longer within the contract | `tests/math_tests.cpp` |
| `src/algorithms/math/sincos_core.hpp` (the certificate) | the 0.51 ulp bound carries to the public sincos and tan | unchanged kernel. For 2^-27 <= abs(x) <= 2^32 the argument is normal, and so is every sine and cosine (none is below 2^-1022), so the flush never acts there. The certificate is still to be rerun on STXlib's build (batch 3c5 row) | `tools/math/sincos_certificate.py` |

## stxlib::algorithms, roadmap R2 (trig region kernels, float sincos proof, float pow search)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/math/trig_region.cpp` (sincos, tan double; an experiment outside the library) | within 0.51 ulp for every double 2^-27 <= \|x\| <= 2^32 | proved by `tools/math/trig_region_certificate.py` (Neo, Python 3.15.2 fractions, 2026-11-15): core error 0.001104 ulp (sin, cos), 0.001059 (tan); contract \|delta r\| <= min(2^-59.83 \|r\|, 2^-67.50); the library's reductions within it above 2^6, the region reduction below 2^6 with margin 2^8.4 at each binade's certified minimum (minima from `tests/reference/sincos_worst_cases.hpp`, written by `sincos_certificate.py`'s complete search). Sampled check and bench: not yet run | `tools/math/trig_region_certificate.py`, `tests/math_region_tests.cpp` |
| `src/algorithms/math/sincos.cpp` (float) | correctly rounded for every float 2^-27 <= \|x\| <= 2^32 | proved by `tools/math/sincos_float_screen.py` (Neo, numpy 2.5.4, 2026-11-15): a proved bound of 2^-52.65 on the kernel's double per table index, a replica screen of all 989,855,746 floats, 44 decided exactly from certified sin and cos. Found sin(+-9830.3984375) misrounded by the kernel; fixed by a one-entry exception table, which the tool applies and decides. Rests on the replica performing the C++'s operations (`-ffp-contract=off`); the M4 run of `stxlib_math_tests --exhaustive` checks the compiled kernel against the reference | `tools/math/sincos_float_screen.py`, `tests/math_tests.cpp` (`float_exceptions`) |
| `tests/math_tests.cpp` (float screens) | a screen result is trusted only when the C++ library's double is not within 2^-50 of a float midpoint | fixed: the five screens passed double-rounded results (sincos at 9830.3984375 so); claims resting on them are rechecked by the next `--exhaustive` run, not yet run | `tests/math_tests.cpp` (`screen_holds`) |
| region pow(float) | correctly rounded at every pair whose x^y is not within 2^-94 of a float midpoint; at exact results and midpoints; at every integer y in [1, 40] | **proved, with one class stated open**: round, test, decide (owner decision 2026-11-15). Stage 1 within 2^-69.31 against the test distance 2^-67 (`region_certificate.py pow`); exact cases detected exactly (`pow_float_exact`, argued in region.cpp: y = n/2^k with k <= 3, n <= 15); the accurate stage within 2^-94.10 (`pow_float_accurate_certificate.py`, Python 3.15.2, exact rationals, Neo); integer y decided by `pow_float_midpoints.py integer`. Not proved: pairs within 2^-94 of a midpoint without being one; none known, expected about 2^-9.4 (heuristic density, sampled). No published complete worst-case list for binary32 pow found (R2 note 2.2). Round 2 (M4, d5f0995): the test's accuracy check failed against its own reference, not the stage: `pow_float_accurate_replay.py` (Neo, Python 3.15.2) replays the stage operation for operation on the test's 198,616 pairs against the 400-bit reference, stage worst 2^-98.32, while tests/reference/wide.hpp's pow_reference errs up to 2^-90.53 (its log x is off by about 2^-104 absolute, times \|y\| up to 2^13); the check now applies for \|y\| <= 64 (reference worst 2^-98.09). The 2^-94 proof is unaffected (sampled replay, consistent with it) | `tools/math/pow_float_accurate_certificate.py`, `tools/math/region_certificate.py`, `tools/math/pow_float_midpoints.py`, `tests/math_region_tests.cpp` |
| region hypot(float) | correctly rounded at every pair (ties to even), by construction | argued in region.cpp (exact a^2 + b^2, the root rounded, the rounding decided by the exact sign of a^2 + b^2 - m^2), not machine-checked. Round 2 (M4, d5f0995) found a coding error: float_bits_value masked the pattern with 0x7f7fffff, halving every float with an odd biased exponent (hypot(1, 0) = 1 + 2^-23); fixed on `claude/round2-math`. `tools/math/hypot_float_replay.py` (Neo): 99,991 sampled pairs, 0 misrounded with the fix, 49,715 with the mask (sampled). Not yet rerun on the M4 | `tools/math/hypot_float_replay.py`, `tests/math_region_tests.cpp`, `tests/math_tests.cpp` |
| `src/algorithms/math/paths.hpp` (two kernel paths) | each switched function's variant entry gives today's kernel's bits on today's path and the region kernel's on the region path (subnormal arguments and flushed pow results on today's, so `unflushed::` keeps its contract); the public functions are exactly the chosen kernel's | argued from the code, not machine-checked; tested by `math_region_tests` (region against today's by name) and by each path's frozen values (`math_tests`, `bit_identity`: today's values must not move in an all-OFF build). M4 run on 45b275b: all-OFF matched all 58 frozen items; region values frozen by sbi (f5cbc30), except the pow hash with only one pow switch on (R3: each switch setting has its own value) and math_region_tests' own hash | `tests/math_region_tests.cpp`, `tests/math_tests.cpp`, `tests/bit_identity.cpp`, `tools/compare_bit_identity.py` |

## stxlib::algorithms, roadmap R1 (math completions: exp2, expm1, log1p, sin, cos)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/math/completions_core.hpp` (exp2, expm1, log1p double) | within 0.5000001, 0.5012 and 0.5000001 ulp for normal results (owner decision D10: at most 0.51) | proved by `tools/math/r1_certificate.py` (interval error analysis over exact rationals, Python 3.15.2 fractions, references to 2^-380 from `tools/math/region_reference.py`; Neo, 2026-11-15): relative error before the one rounding 2^-79.92, 2^-62.80 and 2^-75.80. Builds on `region_core.hpp`'s region_exp2 and region_log1p, whose bounds `tools/math/region_certificate.py` proves. Sampled check against the references: not yet run | `tools/math/r1_certificate.py`, `tests/math_completion_tests.cpp` |
| the same, float | correctly rounded for every float | **not yet run.** Proved once `stxlib_math_completion_tests --exhaustive` passes: the float screen checks every float's double-double against the certificate's bound (2^-79.92, 2^-62.80, 2^-75.80), so the exact value rounds to the same float, and decides any unsettled float against the reference | `tests/math_completion_tests.cpp` (`float_screen`) |
| `src/algorithms/math/flush.cpp` (sin, cos) | sincos' s and c, bit for bit | by construction (fronts that call sincos); tested at 100,000 inputs per precision (10 million in `--exhaustive`), not yet run | `tests/math_completion_tests.cpp` (`fronts`) |
| `src/algorithms/math/completions.cpp` (product forms) | the fused and Dekker forms give the same bits | argued: every two_product has operands in float_exact's exact range (x and the 1/ln 2 head with \|x\| in [2^-5, 745.2]; x^2 with \|x\| >= 2^-54; region_exp2's and region_log1p's products as in the region certificate); tested by the Dekker target's identical hash (not yet frozen) and `tests/bit_identity.cpp` (new items, unfrozen) | `tests/math_completion_tests.cpp`, `tests/bit_identity.cpp` |

## stxlib::algorithms, batch 3c9 (no subnormals in text and random)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/numeric_text.cpp` (parsing) | a real below the smallest normal parses as zero of its sign on every host; out_of_range only on overflow | tested, not proved, at named texts in both types: subnormal results, results below the subnormal range, the boundary on either side of the smallest normal, and saturating exponents. Risk: a host that reports out_of_range for a text rounding up to the smallest normal would give zero for it | `tests/numeric_text_tests.cpp` (`parsing`) |
| `src/algorithms/numeric_text.cpp` (formatting) | a subnormal formats as zero of its sign; normal values unchanged (round trip and host agreement over 200,000 doubles and 100,000 floats, subnormal samples taken flushed) | tested, not proved | `tests/numeric_text_tests.cpp` (`reals`, `round_trips_and_host_agreement`) |
| `src/algorithms/random.cpp` (`scale_into`) | never subnormal, within [low, high) of the read bounds, or `low` | tested at named bounds and units, not proved | `tests/random_tests.cpp` |

## Batch float_exact (src/core/float_exact.hpp)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `split` (float) | hi + lo == a exactly; each half at most 12 significant bits; both zero or normal | **not yet run.** Proved by exhaustive enumeration of every float in range (2^32 patterns screened) once `stxlib_float_exact_tests --exhaustive` passes; the ctest run strides. ESBMC harness `harness_split` written, not run | `tests/float_exact_tests.cpp` (`float_split`); `proofs/esbmc/float_exact_float.cpp` |
| `two_product`, `two_sum`, `fast_two_sum` (float) | exact; the error zero or normal; the fused and Dekker forms agree bit for bit | **not yet run.** `--exhaustive` takes every significand of a binade, both signs, against 512 partners: that is a sample of pairs, not a proof. ESBMC harnesses (bit-precise, over all pairs in range) are written but not run; once they pass, this is proved for float, by ESBMC at the pinned version. **Range corrected (claude/float-exact-edges):** the M5 runs (ESBMC and z3, D01 and the ESBMC harness) refuted the earlier range: two_sum at a = 0x767e0000, b = -FLT_MAX (NaN error, finite sum); two_product at a = -0, b = -FLT_MAX (NaN, Dekker's split of b) and at a = 0x5edbbe36, b = 0x60151e43 (infinite error, finite product). A first correction (sum operands at most 2^emax) was refuted in turn by D01's overflow check on the first add: two_sum at a = -0x1.fffffep126, b = -0x1p127 and fast_two_sum at a = 0x1p127, b = 0x1.fffffep126 (the sum is the tie above FLT_MAX). The range now requires sum operands at most 2^(emax - 1) (float 2^126, double 2^1022), a bound on each operand, checkable before the add, that makes the sum at most 2^emax and so finite; and product operands both in split's range (a zero included) with exponent(a) + exponent(b) <= emax - 2. The no-overflow arguments are in the header (argued). M5 batch on the first correction: ESBMC proved split, fast_two_sum and two_sum (64-bit); those sum verdicts were over the refuted 2^127 range with a finite-sum assumption after the add, so the sums need a rerun over the new range, where the harnesses assert the finite sum instead. **Open (timeouts):** two_sum at 32-bit (600 s), ESBMC two_product (600 s), D01 product and product_fused (120 s). What would close them: for two_sum at 32-bit, the 64-bit harness's verdict carries only if the pointer width cannot matter (no usize or uptr in the lifted code; argued, not a run), or one property per run (exactness, zero-or-normal and finiteness separately); for the products, split the domain by exponent pairs (one run per band of exponent(a) + exponent(b), each band a smaller SMT problem), or the bounded-effect abstraction of Dekker's split (prove split once, D01 split.cpp, then prove the product from any hi, lo with hi + lo = a and 12-bit halves), or a longer limit; product_fused also has the tool's fmaf model as a dependency. The raw forms and every caller's bits are unchanged; only the debug checks and the stated range moved | `tests/float_exact_tests.cpp` (`range_edges` for the counterexamples); `proofs/esbmc/float_exact_float.cpp`; `proofs/d/rows/D01-float-exact-transforms` |
| `two_sum`, `fast_two_sum`, `two_product`, `split` (double) | as above, over the same corrected range (sum operands 2^1022; exponent sum <= 1021) | sampled against the exact integer reference (20 million in `--exhaustive`), not proved; the overflow edge is argued with the float one (the same argument in p and emax). Literature proofs: Knuth Theorem B, Dekker 1971, the Flocq formalisation (Boldo and Melquiond 2011; Boldo 2006 for Veltkamp and Dekker) | `tests/float_exact_tests.cpp` |
| expansions | exact sums, scalings and products; nonoverlapping output in increasing magnitude; the sign | sampled against the exact reference, not proved; Shewchuk 1997's theorems (which assume no overflow). Since claude/float-exact-edges the header states the overflow side as a precondition (two-sum operands at most 2^(emax - 1), product steps within split's limit and exponent sum <= emax - 2; sufficient: the components' magnitudes sum to at most 2^(emax - 2)) and debug builds check it at every step | `tests/float_exact_tests.cpp` (`expansions`) |
| `KahanSum`, `NeumaierSum` | within Higham's bounds (section 4.3) | sampled, not proved | `tests/float_exact_tests.cpp` (`compensated`) |
| `orient2d`, `incircle` | the exact sign, in float and double | sampled against the exact reference, with exactly collinear and cocircular inputs (scaled to the range bounds) nudged by one ulp, not proved. The filters are Shewchuk's stage-A bounds, plus an absolute slack for incircle's underflowing products, argued in the header. Their coordinate ranges (2^500, 2^250) meet the expansions' overflow precondition with room (exponent sums at most 1000 and 1010, component sums below 2^1003 and 2^1014; argued in the header), so the M5 edge does not reach them; ranges unchanged | `tests/float_exact_tests.cpp` (`predicates`) |
| `src/algorithms/math/kernel_support.hpp` | the math's bits are unchanged by the move | the frozen hashes of `tests/math_tests.cpp` at every level and in the Dekker build (to run) | `tests/math_tests.cpp` |

## Batch reduction order (src/core/float_reduce.hpp, the bit identity, the flag check)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/float_reduce.hpp` (shape) | ordered_sum and ordered_dot compute F5's shape: 8 lanes, the fixed tree, the tail in index order, no added zero | tested, not proved: hand-worked cases on which sequential order, another grouping, or a tail in lane 0 give different bits; every length 0..300 against a restatement (sampled terms); signed zeros | `tests/float_reduce_tests.cpp` (`shape_by_hand`, `shape_against_reference`, `signed_zeros`) |
| `src/core/float_reduce.hpp` (pool) | the pool forms give the serial chunked bits at every thread count | tested at 1, 2, 3 and 8 threads, for batches on the calling thread and on the workers (601 chunks), not proved. The argument: each chunk's value depends only on its terms, and the combine runs on the caller in chunk order after run returns | `tests/float_reduce_tests.cpp` (`chunks`, `pool`) |
| `tests/bit_identity.cpp` | every host, SIMD level and product form prints the frozen hash per item (math, float_exact, reductions, numeric text, random) | **frozen** (58 items) from the M4's library-build run of `5aaedf7`, before the pow fix; the fixed build must reproduce it (not yet run). A match is evidence over the fixed inputs only (sampled), not a proof of identity on all inputs | `tools/compare_bit_identity.py`, `tests/bit_identity_expected.txt`, `.github/workflows/ci.yml` |
| `src/algorithms/math/pow.cpp` (pow(double), product forms) | the fused and Dekker forms give the same bits for every input | argued, not machine-checked: every exact product's operands lie in the range where both forms are exact (pow.cpp's head: the kernel takes 2^-80 <= \|y\| < 2^64; the other factors are zero or in [2^-600, 2^11]), and every other operation is a correctly rounded IEEE operation with contraction off. Tested: `pow_kernel_y_range()` in both builds, and math.pow.double in the bit identity (not yet run on the fix) | `tests/math_tests.cpp`, `tests/bit_identity.cpp` |
| `CMakeLists.txt` (rule 1) | every target defined there has `-ffp-contract=off`, and no target option or global flag undoes the discipline | checked at configure time over every target (`stxlib_check_float_flags`); not yet configured | `CMakeLists.txt` |

## stxlib::algorithms, batch 3c6 (stx_btree's sequence tree, RowHeightIndex)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/stx_btree/sequence_tree.hpp` | every query, cursor walk and seek equals a flat model, and invariants I1-I6 hold, after every operation sequence | exhaustive within its small scope (depths 2 and 1 from five start states, at the smallest capacities, four configurations; `--scope full` by hand: 3 and 2), as in stx_auxlib; beyond that scope it rests on the study's proof sketches (§8.2), not on a mechanical proof | `tests/stx_btree/sequence_tree_exhaustive_test.cpp`; `src/algorithms/stx_btree/ORDER_STATISTIC_RESEARCH.md` |
| `src/algorithms/stx_btree/sequence_tree.hpp` (failure) | a page the allocator refuses leaves the tree unchanged and the edit then succeeds; a tree without its first page leaks nothing | tested over every allocation of eight edit kinds at ten positions (null pages from a budget allocator), not proved | `tests/stx_btree/sequence_tree_test.cpp` |
| `src/algorithms/stx_btree/sequence_tree.hpp` (Lemma S, coarsen, finger) | splitting a run keeps every row's height; coarsen keeps offsets outside its range bit for bit; seek_offset_from equals seek_offset | exhaustive over the stated small domains (every run of up to 16 rows and 80 units; every range of a 40-row sequence; every row of a deep small tree), as in stx_auxlib | `tests/stx_btree/sequence_tree_test.cpp` |
| `src/algorithms/row_height_index.cpp` (the facade) | the facade answers as the template and the model, and each failure has the kind its cause names | the exhaustive enumeration and the packing cases run through the facade at production capacity ("facade-production"); at that capacity the sequences split no page, so this checks the translation, not the structure | `tests/stx_btree/sequence_tree_exhaustive_test.cpp` |
| `src/algorithms/row_height_index.cpp` (cost) | the facade's cost over the template on row-to-offset, offset-to-row and set_heights | not measured yet: `stxlib_row_height_bench` is for the integrator | `tests/stx_btree/row_height_bench.cpp` |

## stxlib::algorithms, batch 3c7 (stx_btree's unsync ordered map)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/stx_btree/unsync_map.hpp` | the map answers as std::map (insert, try_insert, remove, find, lower_bound, pop, insert_many, iteration), and every structural invariant (depths, fills, heads, order, windows at exact prefixes, padding, hash tags, shared prefix) holds after each phase | tested, not proved: the differential runs carried from stx_auxlib (seven key and value shapes, random and ordered, both page allocators, batches) | `tests/stx_btree/unsync_map_test.cpp`, `tests/stx_btree/tree_invariants.hpp` |
| `src/algorithms/stx_btree/unsync_map.hpp` (failure) | an insertion the allocator refuses a page leaves the map unchanged and reports `out_of_memory()`; it then succeeds | tested over 20000 insertions of small-leaf entries with every page first refused (first root page, leaf splits and splits that climb), not proved | `tests/stx_btree/unsync_map_test.cpp` (`run_refusals`) |
| `src/algorithms/stx_btree/pooled_page_allocator.hpp` | maps sharing a pool get every page back and reuse freed pages; the pool ends with its last sharer | tested, not proved | `tests/stx_btree/unsync_map_test.cpp` (`run_shared_pool`) |
| `src/algorithms/stx_btree/detail/latched_tree.hpp` | see batch 3c10 | adapted and instantiated in batch 3c10 | batch 3c10 |

## stxlib::algorithms, batch 3c10 (stx_btree's shared ordered map)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/stx_btree/bbtree_map.hpp` | single-thread answers equal std::map's, with every structural invariant; threaded writers, readers, counters, scans and batches see consistent entries (bbtree's map tests) | tested, not proved: stx_auxlib's runs, unchanged; the concurrency rests on bbtree's optimistic lock coupling (DESIGN.md), not on a mechanical proof | `tests/stx_btree/bbtree_map_test.cpp`, `tests/stx_btree/stress_test.cpp` |
| `src/algorithms/stx_btree/detail/latched_tree.hpp` (failure) | an insertion the allocator refuses a page leaves the map, key and value unchanged, says refused in its own result, runs no visitor, and then succeeds | tested single-threaded over 20000 insertions with every page first refused (first root page, leaf splits, climbing splits; four write forms), not proved; refusals racing other threads are not tested | `tests/stx_btree/bbtree_map_test.cpp` (`shared_refusals`) |

## stxlib::algorithms, batch 3c11 (stx_btree's optimal binary search trees)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/stx_btree/optimal_tree.hpp` (knuth, knuth_scattered, garsia_wachs) | the returned tree is a search tree, its cost is the reported cost, and that cost is optimal; both Knuth layouts give the same tree | exhaustive for n <= 8 (every tree enumerated, 8 weight families, 6 trials each), and against the cubic recurrence for 9 <= n < 149 (sampled); larger n against Knuth only, as in stx_auxlib | `tests/stx_btree/optimal_tree_test.cpp` |
| `src/algorithms/stx_btree/optimal_tree.hpp` (balanced, bottom_up, weight_balanced) | a valid search tree whose cost is at least the optimum; balanced exact on constant weights | tested on the same instances, not proved | `tests/stx_btree/optimal_tree_test.cpp` |
| `src/algorithms/stx_btree/optimal_tree.hpp` (refusals) | malformed weights, and garsia_wachs on key weights, give invalid_argument | tested | `tests/stx_btree/optimal_tree_test.cpp` (`check_edges`) |
| `src/algorithms/stx_btree/optimal_tree.hpp` (exhaustion) | storage that cannot be had gives out_of_memory with no tree | not tested: no allocation fault injection reaches memory::allocate here | none |

## stxlib::algorithms, batch 3c12 (stx_btree's tree packing and static optimal map)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/stx_btree/detail/tree_packing.hpp` (pack) | every key in exactly one node, nodes ascending and within capacity; the reported cost equals the walked cost; capacity 1 gives the binary tree's cost; the cost is the least over every way of cutting the tree into nodes | exhaustive over edge subsets for 300 random trees of n <= 12 at capacities 1 to 4 (sampled trees, exhaustive cuts); walked cost also on 6 trees of 2000 to 4999 keys | `tests/stx_btree/optimal_map_test.cpp` (`check_packing`) |
| `src/algorithms/stx_btree/detail/tree_packing.hpp` (relayout) | node shape as above; reported cost equals walked cost; with slack 0 never above the packing | tested on the same instances, not proved | `tests/stx_btree/optimal_map_test.cpp` (`check_packing`) |
| `src/algorithms/stx_btree/optimal_map.hpp` | every key found with its value, absent keys not found, find_many equal to find, for u64, i64 and string keys and each construction | tested, not proved: stx_auxlib's instances, unchanged | `tests/stx_btree/optimal_map_test.cpp` |
| `src/algorithms/stx_btree/optimal_map.hpp` (refusals) | unsorted keys, mismatched lengths and malformed weights give invalid_argument, and the caller's keys and values are left unchanged | tested | `tests/stx_btree/optimal_map_test.cpp` (`check_rejections`) |
| `src/algorithms/stx_btree/optimal_map.hpp`, `detail/tree_packing.hpp` (exhaustion) | storage that cannot be had gives out_of_memory with no map and the keys and values unchanged | not tested: no allocation fault injection reaches memory::allocate here; the ordering (every allocation before the first move) is by reading of `lay_out` | none |

## stxlib::boundary, A1 platform-rest (process, libraries, images, stack trace, pages, wake, known directories, FreeBSD)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/command_line.hpp` | W1: the split equals an independent restatement of the documented rules on every line of 0 to 7 bytes over {a, space, tab, `"`, `\`}, meets its count bounds, measures as it writes, and writes nothing outside short rooms. W2: quoting then splitting returns every list of 1 to 3 arguments of 0 to 3 such bytes; a quoting takes at most 2n + 2 bytes. W3: Microsoft Learn's six examples | **not yet run.** Proved on those domains once `stxlib_platform_rest_exhaustive` passes (exhaustive enumeration; the reference is in the harness). ESBMC harnesses `harness_split` and `harness_quote` (lines and arguments up to 8 nondeterministic bytes, any rooms) written, not run. Agreement with the UCRT's own argv: tested by `tests/process_tests.cpp` on the Windows CI host, not proved | `tests/platform_rest_exhaustive.cpp`, `proofs/esbmc/platform_rest.cpp`, `tests/process_tests.cpp` |
| `src/core/stack_line.hpp` | S1: for every capacity 0 to 200 and five frames (control bytes, names to 300 bytes, unknown and empty images, the largest values), the line fits, ends with a newline, has no other control byte, writes nothing past the capacity, and is the reference line cut | **not yet run.** Proved for those frames and capacities once the harness passes; `harness_stack_line` (any frame with names to 8 bytes, capacities to 64) written, not run | the same |
| `src/core/pe_image.hpp` | P1: the synthetic image's headers, sections (exact and grouped) and nearest exports (forwarder skipped). P2: every truncation and every single-byte damage (five values) of its first 0x4C0 bytes fails or answers inside the view, a truncation answering as the whole image | **not yet run.** Exhaustive over that damage domain once the harness passes (an address-sanitizer build makes reads past an exactly sized block visible); `harness_pe_image` (every byte of a 256-byte image nondeterministic) written, not run | the same |
| `src/core/process_info.cpp`, `src/hosted/*/process_*.cpp` | process_arguments equals argv (POSIX) or the UCRT's split (Windows) and is lossless for bytes that are not UTF-8 and lone surrogates; process_id, memory and the main thread as process.hpp states | not yet proved; tested with six argument shapes and a child started with a non-UTF-8 argument (POSIX) or a lone surrogate (Windows) | `tests/process_tests.cpp` |
| `src/core/library.cpp`, `images.cpp`, `src/hosted/*/library_*`, `images_*`, `image_regions_*` | refusals as library.hpp and images.hpp state; a system library's symbol resolves to one address by name, by find_loaded and by absolute path; describe_address names an image and never writes past its rooms; the test's own region is found in its image and among all images | not yet proved; tested per family (musl: refusals only) | `tests/images_tests.cpp` |
| `src/core/stack_trace.cpp`, `trace_*` | at most the maximum, innermost first; writing returns for unknown addresses | not yet proved; tested | `tests/images_tests.cpp` |
| `stack_trace.hpp`, `images.hpp`, `fail_fast.hpp` (the reporter's calls, A2 P0) | capture_stack_trace, write_stack_trace and describe_address allocate nothing and take no STXlib lock, so a fail-fast reporter may call them. capture_stack_trace takes STXlib's own walk (`host::capture_report_addresses`) on every POSIX host: the frame-pointer chain bounded by the stack recorded in the thread's anchor before any failure (layout 12; `ThreadFlags::stack_low`), so it asks the host nothing at failure time; the host's query (`stack_bounds_posix.cpp`), which allocates and locks on glibc and FreeBSD, runs only at stxlib_state's load, at a thread's block and at a Thread's start, never while the thread fails fast or reports. The unwinder is the fallback with no recorded bounds (macOS asks its own lock-free query instead), off the recorded stack, or without frame pointers. RtlCaptureStackBackTrace on Windows. write_stack_trace holds cancellation off for its writes | not proved. Read for the bounds: glibc 2.41 `pthread_getattr_np.c` (the initial thread's rule STXlib applies itself on Linux), musl 1.2.5 `pthread_getattr_np.c` (the initial thread's bound is only the part mapped so far), FreeBSD fb9033d2876d `thr_init.c` (`init_main_thread`). Not read: Linux `acct_stack_growth` and `mmap_base` (what keeps the initial thread's recorded reach unmapped but for the stack). Read: glibc 2.41 `pthread_getattr_np.c` and `dl-addr.c`, FreeBSD fb9033d2876d `thr_attr.c:125-175`, libpthread `pthread.c` and dyld `DyldAPIs.cpp` (main branches, unpinned), libgcc 14.2.0 `unwind-dw2-fde.c` and `unwind-dw2-fde-dip.c` (first lookup in a registered object allocates under object_mutex). Not read: glibc `_dl_find_object`, LLVM libunwind, musl and FreeBSD dladdr locking, Windows (closed). Tested: a reporter with C++ allocation refused everywhere and the C library's refused on glibc | `tests/reporter_stack_tests.cpp` (`reporter_stack_unallocated`) |
| `shadow.hpp` (`park_shadow_into`, `restore_shadow_from`; M1 option 2) | the frames of a parked record survive any number of later captures on the thread, carried by the caller; the room is checked against its stated size before anything is parked; the restored record is pending, so the outer gate attaches it with the throw's frames and point; no layout change | not yet proved; tested in every build with one more intervening capture than a development build keeps | `tests/shadow_tests.cpp` (`a_copied_park_survives_later_captures`, `a_park_into_too_little_room_parks_nothing`) |
| `src/hosted/*/pages_*.cpp` | zeroed whole pages; sealing is final and a later write ends the process; writable() after sealing is a failed requirement | not yet proved; tested, with must-end cases `sealed_writable` and `sealed_store` | `tests/pages_wake_tests.cpp`, `tests/platform_rest_death_tests.cpp` |
| `src/hosted/*/wake_*.cpp` | a wake raised before or during a wait is reported, never lost, with drain; many wakes collapse into one; the channel is reported before handles; a handle's readiness names its index; over 63 handles is refused | not yet proved; tested (the cross-thread wake at 20 ms, timeouts at 30 ms, sampled) | `tests/pages_wake_tests.cpp` |

Nothing in batch 1 has been compiled or run yet: no compiler runs on the


## stxlib::boundary, batch M2b (the owned vocabulary; owner, D15 before 1.0)

Exhaustive means every case of the stated domain is enumerated and checked
by the test program; the property is proved for that domain once the
program passes in CTest, and not beyond it.

| Module | Property | Status | Evidence |
|---|---|---|---|
| `include/stxlib/detail/growth.hpp` | `grown_capacity` returns 0 exactly when the request is unrepresentable, and otherwise a capacity at least the request, at least the current one, equal to it when it suffices, and representable in bytes | **not yet run.** Proved for 8-bit operands once `stxlib_owned_vocabulary_tests` passes (exhaustive over every operand triple meeting the precondition). 64-bit: ESBMC harness written, not run | `tests/owned_vocabulary_tests.cpp` (`growth_policy_exhaustive_8bit`), `proofs/esbmc/vocabulary_growth.cpp` |
| `include/stxlib/vector.hpp` | after every operation the elements equal a std::vector model's; a refused allocation fails with out_of_memory and leaves elements, order, capacity and the moved argument unchanged; every element constructed is destroyed exactly once, and none is used after destruction; an argument that is an element of the vector itself (push, insert, push_copy, append_copies) gives the model's result | **not yet run.** Proved for every sequence of up to 4 operations from the empty vector over the operation set of `vector_candidates` (all indices and lengths the state allows, counts up to 9), each operation also run refused, once the test passes; deeper with `--depth N`. Not covered: element types whose relocation is observable beyond the counts kept | `tests/owned_vocabulary_tests.cpp` (`vector_exhaustive`, `vector_limits_and_moves`) |
| `include/stxlib/optional.hpp` | presence and value equal a std::optional model's through set, reset, copy, move, take and copy-swap; no leak or double destruction; a trivially copyable Value gives a trivially copyable Optional | proved for every sequence of up to 4 operations over two optionals once the test passes; the triviality by `static_assert` at compile time | `tests/owned_vocabulary_tests.cpp` (`optional_exhaustive`) |
| `include/stxlib/unique.hpp` | the object is destroyed as the type make() built and its block released at its start, also through a base at a non-zero offset without a virtual destructor; a refused make constructs nothing | tested | `tests/owned_vocabulary_tests.cpp` (`unique_owners`) |
| `include/stxlib/shared.hpp` (one thread) | owner counts, presence, identity and expiry equal std::shared_ptr's and std::weak_ptr's; every object is destroyed once and every block released once, after its last observer | **not yet run.** Proved for every sequence of up to 4 operations over three owner and two observer slots (make, copy, move, reset, observe, lock, forget) once the test passes | `tests/owned_vocabulary_tests.cpp` (`shared_exhaustive`) |
| `include/stxlib/shared.hpp` (threads) | the counts are race-free; lock never revives a destroyed object; the object is destroyed exactly once | not yet proved. Argument: increments relaxed, decrements acquire-release, lock a compare-exchange from a non-zero value (the file's header), the scheme of libc++'s shared_ptr. Sampled: four threads copying, locking and dropping 20000 times (TSan evidence when run under the TSan build). A model-checking harness (GenMC or CBMC at the pinned versions) is the next step | `tests/owned_vocabulary_tests.cpp` (`shared_threads`) |
| `include/stxlib/shared.hpp` (SharedFromThis, casts) | shared_from_this is an owner of the same block, empty for an object not made by make, for a copy, and during destruction; conversions and casts keep one block | tested | `tests/owned_vocabulary_tests.cpp` (`shared_self_and_casts`) |
| `include/stxlib/text.hpp`, `src/core/text.cpp` (Utf8String) | every replacement (behind assign, append, insert, erase, truncate) gives the std::string model's text when both ends are boundaries, and otherwise invalid_argument or out_of_range with nothing changed; the text stays well-formed and NUL-terminated; a refused allocation changes nothing; a replacement borrowed from the string itself gives the same result | **not yet run.** Proved, once `stxlib_owned_text_tests` passes, for every text of up to three pieces from {a, é, €, 𝄞}, three starting capacities, every offset and length up to count + 1, and every such text and boundary substring of the target as the replacement (inline and block storage, both crossings), each case also refused | `tests/owned_text_tests.cpp` (`replace_exhaustive`) |
| `src/core/text.cpp` (append_code_point) | accepted exactly for the scalar values; the bytes are well-formed and decode back to the code point | **not yet run.** Proved for every u32 from 0 to 0x10FFFF + 16 and the top two values once the test passes; values in between above U+10FFFF follow the same single comparison | `tests/owned_text_tests.cpp` (`append_code_point_exhaustive`) |
| `src/core/text.cpp` (compare_text) | byte order equals code point order | proved for every pair of the texts above once the test passes | `tests/owned_text_tests.cpp` (`compare_exhaustive`) |
| `src/core/vocabulary.cpp` | the refusal switch refuses exactly as armed and never alters a granted block | tested (through every refusal check above) | the two test programs |
| `include/stxlib/ascii.hpp` (R2) | every class and case mapping equals the C library's in the "C" locale on bytes 0x00..0x7F; bytes 0x80..0xFF are in no class and never changed; `ascii_digit_value` is the base-36 digit value or -1 | **not yet run.** Proved for all 256 byte values once `stxlib_ascii_search_tests` passes (the domain is the whole input type). The reference is the host C library, so the evidence is per CI host | `tests/ascii_search_tests.cpp` (`classes_exhaustive`) |
| `include/stxlib/ascii.hpp` (folded comparison) | `compare_ignoring_ascii_case` and `equals_ignoring_ascii_case` equal byte order after C-locale `tolower` | **not yet run.** Proved for every pair of one-byte ASCII texts and every pair of texts of up to 3 bytes over {a, A, z, Z, @, [, `, {} once the test passes | `tests/ascii_search_tests.cpp` (`folded_comparison_exhaustive`) |
| `include/stxlib/search.hpp` (R3) | `find` at every start, `find_last`, `contains`, `starts_with`, `ends_with` and the ASCII case-insensitive and byte forms equal a naive search; matches in UTF-8 text are code point boundaries | **not yet run.** Proved once the test passes for: haystacks up to 10 and needles up to 6 bytes over {a, b}; up to 7 and 4 over {a, b, c}; folded forms up to 7 and 4 over {a, A, b}; byte forms up to 7 and 4 over {0x00, 0x80, 0xFF}; texts up to 4 characters over {a, é, 𝄞} with needles up to 2. Beyond that domain the correctness rests on the published algorithm (Crochemore and Perrin 1991) as transcribed, which is reported, not proved | `tests/ascii_search_tests.cpp` (`search_exhaustive`, `byte_search_exhaustive`, `utf8_search_exhaustive`) |
| `include/stxlib/search.hpp` (linear time) | a search makes at most 2n + 5m byte comparisons | **not yet run.** Checked over the first two domains above (every case) and four periodic shapes on a 20000-byte haystack (sampled). The general bound is the published one (2n - m for the search phase, linear preprocessing), not proved here. The test prints the worst excess observed | `tests/ascii_search_tests.cpp` (`check_comparisons`, `long_cases`) |
| `include/stxlib/search.hpp` (TextPieces) | the pieces equal Python's str.split(separator) model; an empty separator yields the text once | **not yet run.** Proved for every text of up to 7 bytes over {a, ','} with the separators ",", ",,", "" and "a," once the test passes | `tests/ascii_search_tests.cpp` (`split_exhaustive`) |
| `include/stxlib/heap.hpp` (R6) | PriorityQueue and stable_heap_push/pop over caller storage remove values in (priority, insertion) order, so equal priorities leave in insertion order; the heap property holds after every step; a refused push leaves the queue and the value unchanged | **not yet run.** Proved for every sequence of up to 9 operations over {push 0, push 1, push 2, pop} once `stxlib_heap_tests` passes (deeper with `--depth N`, up to 16). Uniqueness of the removal order for any operations follows from the combined order being total (the header's argument). The sequence-exhaustion refusal (2^64 - 1 insertions without emptying) is not exercised. Sampled: move-only values, LargestFirst, a 20000-step pseudo-random run | `tests/heap_tests.cpp` |
| `include/stxlib/transcode.hpp`, `src/core/transcode.cpp` (R15, scalars) | UTF-8 to UTF-16 and UTF-32 and back are exact for every scalar value; `code_point_at`, `code_point_count` and `utf16_count` agree with them | **not yet run.** Proved for every scalar value U+0000..U+10FFFF and every surrogate pair once `stxlib_transcode_tests` passes | `tests/transcode_tests.cpp` (`scalars_exhaustive`, `surrogate_pairs_exhaustive`) |
| `src/core/transcode.cpp` (16-bit units, WTF-8) | conversion to UTF-8 accepts exactly the unit sequences without an unpaired surrogate and reports the first one's index; WTF-8 carries every sequence and gives it back; `is_wtf8` accepts exactly generalized UTF-8 with no high-low surrogate pair; a failure leaves every output unchanged | **not yet run.** Proved once the test passes for: every single unit; every sequence of up to 4 units from 15 edge units; every byte string of up to 2 bytes and every 3-byte string with a lead E0..EF (against an independent decoder). Longer inputs follow from the same per-code-point loop, which is reported, not proved | `tests/transcode_tests.cpp` (`units_exhaustive`, `wtf8_exhaustive`, `failures_unchanged`) |
| `include/stxlib/frozen_map.hpp`, `src/algorithms/frozen_map.cpp` (R5, numbers) | find, contains, lower_bound, key_at and value_at equal a sorted model; uniform and weighted builds agree on every answer | **not yet run.** Proved once `stxlib_frozen_map_tests` passes for: every subset of eleven edge keys, at every key and its neighbours; every count 0..300 at five strides (multi-level trees), at and around every key; 5000 keys (bottom-up construction) against every query 0..15002. The template's own layout proofs are its batch 3c12 rows | `tests/frozen_map_tests.cpp` (`number_subsets_exhaustive`, `number_families`) |
| `src/algorithms/frozen_map.cpp` (R5, text) | the same for text keys in byte order, through the string_view traits generated from the template's heap-string traits | **not yet run.** Proved once the test passes for: every subset of the 13 texts of up to two characters over {a, b, é} against every text of up to three; the 511 texts of up to 8 bytes over {a, b}, bare and behind a 70-byte prefix, against every text of up to 9; the 31 texts over {NUL, a, DEL, é, U+10FFFF}, all and every subset of the first 12 | `tests/frozen_map_tests.cpp` (`text_exhaustive`) |
| `src/algorithms/frozen_map.cpp` (refusals) | unsorted, repeated and mismatched inputs fail with invalid_argument (code: first index out of order); a refused allocation fails with out_of_memory; a moved-from map is empty | tested | `tests/frozen_map_tests.cpp` (`refusals`) |

### Early exits (owner, 2026-11-15: every foreign call is a possible early exit)

The form at each call into code STXlib does not control (form 1: the
invariants are whole before the call; form 2: the partial state is safe to
unwind through). The headers state the same at their call sites.

| Module | Foreign calls | Form |
|---|---|---|
| `vector.hpp` | Element constructors, destructor, move assignment, operator== | form 1 for growth (built in the new block, the old one adopted out only when complete), in-capacity appends (count after each construction), removal (count before each destruction), insert and erase when Element is nothrow move assignable (every counted slot live). **Termination, accepted by the owner (2026-11-15, decisions/2026-11-15-thread-local-exit-safety.md):** insert before the end and erase for an Element without a nothrow move assignment (none, or one that can throw) rely on the noexcept boundary, so an unwind ends the process. That fits rule 1 as reworded ("termination frees nothing twice"): termination releases none of our state, so the dead slot is never destroyed or freed. A longjmp is undefined behaviour and is not defended. The exhaustive vector test now runs both element kinds |
| `optional.hpp` | Value constructors, destructor, operator== | form 1: presence set after construction, cleared before destruction |
| ordinary copies (`Vector`, `Utf8String`, `Buffer`, `Path`, `PriorityQueue`; owner 2026-11-16) | Element copy constructors (inside copy()) | form 1: the copy is built whole in a new object by copy(), then taken by the move assignment, which holds the new storage before releasing the old (rule 1: let go, then release); self-assignment returns at once; a source inside an element is read completely before anything is released. Exhaustion goes to the allocator's policy (take_or_fail_fast: fail fast with copy()'s record) |
| `unique.hpp`, `shared.hpp` | the object's constructor and destructor | form 1: built in an unowned block; fields repointed and counts final before the destructor runs |
| `heap.hpp` | Order::before (required noexcept), Value moves and destructor | form 2: every storage slot is a live entry at every call; only a local entry can be lost |
| `text.hpp` (Utf8String) | host memcpy, memmove | form 1 for growth; form 2 in place (the destructor reads only pointer and capacity) |
| `transcode.hpp`, `frozen_map.hpp`, `search.hpp`, `ascii.hpp` | none from consumers; host memcpy/memcmp, string_view inline members | form 1: check and count first, publish when complete; search and ascii make no foreign call |
| the allocation path (`vocabulary_allocate`, `memory::allocate`) | STXlib's own | never unwinds; exhaustion is out_of_memory (D8 = a) |
| `stack_trace.cpp`, `images.cpp` and their host modules | the host's unwinder, loader (dladdr, GetModuleHandleExW, GetModuleFileNameW), write | form 2: the only state changed is the caller's array, rooms and the stream; write runs with cancellation held off |
| `shadow_frames.cpp` (`park_shadow_into`, `restore_shadow_from`) | none | form 1: room checked before parking; a record is published after it is filled |

These forms are argued from the code, not tested: no test injects an early
exit (longjmp out of consumer code across STXlib frames is undefined in C++).

Nothing in this batch has been compiled or run yet: no compiler runs on the
seat's machine; the integrator builds and runs it on the M4.

## Batch A3 prep (results without exceptions; branch `claude/interfaces-a3-prep`)

| Module | Property | Status | Evidence |
|---|---|---|---|
| every object of `stxlib_state`, `stxlib_boundary`, `stxlib_algorithms` (and `stxlib_math_avx2_fma` where built) | no object names a symbol of the exception runtime (throw, rethrow, catch, personality routines, `_Unwind_Resume`, the MSVC handler names; the list in `tools/check_no_exceptions.py`), except the recorder and the exception-type query while `STXLIB_EXCEPTIONS` is ON | **not yet run.** Proved for a build once `stxlib_no_exception_symbols` passes in it: an exhaustive scan of that build's objects with the toolchain's `nm`, made non-vacuous by the probe object, which must show those symbols. It rests on the symbol list being complete for the Itanium C++ ABI on the toolchains in use (macOS, Linux, LLVM MinGW) | `tools/check_no_exceptions.py`, `tests/exception_probe.cpp` |
| `CMakeLists.txt` | every library target is built with -fno-exceptions; only the recorder adds -fexceptions, and only with `STXLIB_EXCEPTIONS` ON; with it OFF, every target but the probe is built without exceptions | checked at configure time by `stxlib_check_exception_flags` (a configure that passes is the check) | `CMakeLists.txt` |
| `include/stxlib/gate.hpp` (`call_as_result`, `call_as_result_classified`) | a plain target's value comes back as the success; with exceptions, a thrown exception comes back as the failure, classified as the gate's are (the consumer's classifier first) and with the frame's operation and shadow record; without exceptions, a direct call; a const class result is held without the const; reference results and targets that already return results are refused at compile time | not yet proved; tested with and without exceptions | `tests/gate_tests.cpp` (`plain_targets_become_results`), `tests/gate_no_exceptions_tests.cpp` |
| `include/stxlib/fail_fast.hpp` (`take_or_fail_fast`, `check_or_fail_fast`) | a success passes on (its value moved out); a failure ends the process through fail_fast with the record unchanged | not yet proved; tested: must-end tests `take_or_fail_fast` and `check_or_fail_fast`, success paths in both gate tests | `tests/death_tests.cpp`, `tests/gate_no_exceptions_tests.cpp` |
| the `STXLIB_EXCEPTIONS=OFF` build | every library target and every test that does not throw builds and passes with -fno-exceptions in every translation unit, and every public header compiles without exceptions | not yet run; CI step "No exceptions (Release)" on glibc | `.github/workflows/ci.yml`, `tests/gate_no_exceptions_tests.cpp` (includes every public header) |

Written on Neo, not compiled or run (no compiler on the seat's machine).
The checker's parsing was exercised with a stand-in nm script printing
Mach-O-style and import-prefixed names, not with a real toolchain.

## stxlib::boundary and stxlib::algorithms, M2 items (C2, K-1 to K-3, C1, C6, C3, R4, C7)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/files.cpp`, `src/hosted/*/file_*.cpp` (C2) | read_at and write_all_at at an offset, leaving the file position alone; refusals by mode (invalid_state) and above i64_max (too_long); information() of the open file; set_modified exact to 100 ns, before 1970 included; set_size | not yet proved; tested (`positioned_and_metadata`) | `tests/files_tests.cpp` |
| the same, remove_tree (C2) | removes entries relative to held directories, never following a link or reparse point met inside the tree, to a depth of 4,096; a top-level link is removed as a link; an absent tree is success(0) | not yet proved; tested at depth 40 and width 600, with links to an outside directory inside the tree and at the top. Not tested: the swap race itself, the depth limit (4,097 levels exceed PATH_MAX), deleting a read-only file on Windows | `tests/files_tests.cpp` (`deep_and_wide_trees`, `link_checks`) |
| `src/hosted/*/threads_*.cpp` (K-1 to K-3) | processor_count counts every online processor (all groups on Windows) and equals std::thread::hardware_concurrency; a 1 ms sleep waits at least 1 ms and twenty take under 250 ms on Windows' high-resolution timer | not yet proved; tested (timing thresholds sampled) | `tests/threads_tests.cpp` (`short_sleeps`) |
| `include/stxlib/clocks.hpp` pace (C1) | the next tick is the least origin + k * period strictly after now, k >= 1, elapsed_periods = k - 1; now before the origin gives the origin; a period of zero or less gives now; saturation at the range's end | **refuted, then fixed, not yet rerun.** ESBMC's 64-bit run of `harness_pace` found origin = -6052837882811285506, period = 3458764513820475424, now = 2017612641652015039 returning 3170534154043490301 for 4323455658650140766 (the offset (k + 1) * period saturated though origin plus it fits); pace now forms the last tick at or before now exactly and compares before adding the period. Exhaustive over origin and now in [-20, 20] and period in [-3, 7], the counterexample and its neighbours at compile time, and a 128-bit sweep of 12 edge values by 7 periods, once the test passes; `harness_pace` (every i64 triple, 128-bit statement; 64-bit only) to be rerun | `tests/platform_tests.cpp`, `proofs/esbmc/time_for_ui.cpp` |
| `include/stxlib/rounding.hpp` (C6) | each of five rules equals the host's std::round, nearbyint (default mode), trunc, floor or ceil where the host is defined; above, below and NaN states otherwise; subnormals read as zero; -2^63 in range | **not yet run.** Every float with `--exhaustive` (every 4,099th pattern in CTest), double binade boundaries and 2,000,000 sampled doubles; `harness_round_double` and `harness_round_float` (bit-precise with --floatbv, every value and rule) written, not run | `tests/rounding_tests.cpp`, `proofs/esbmc/time_for_ui.cpp` |
| `src/algorithms/numeric_text.cpp` Notation::general (C3) | %.Pg in the C locale: P significant digits, the switch to scientific below 1e-4 and from 10^P, trailing zeros removed; equal to the host's snprintf; %.17g round trips | not yet proved; exact cases, 50,000 sampled doubles (P 0 to 24) and 25,000 floats (P 0 to 11) against snprintf, the round trip over the same doubles. The digits are std::to_chars's, whose result the standard fixes exactly | `tests/numeric_text_tests.cpp` |
| `tools/check_std_rules.py` (R4) | the governance rules over STXlib's sources: no finding under --profile stxlib over src and include; every atomic type lock-free (`src/core/atomic_rules.cpp`, a compile-time proof on each host built) | the check passes on the laptop (Python, no build); its tests (G1 to G7) pass on the laptop. A text check: what it reports is certain, what it passes is not (macros, function pointers) | `tests/check_std_rules_tests.py`, CTest `stxlib_std_rules`, `stxlib_std_rules_tool` |
| CMake STXLIB_RANDOMIZE_UNSTABLE_ORDER (R4) | with libc++, unstable sorts shuffle equal elements, so a test relying on their order fails | evidence only (a random shuffle per run), not a proof; first run on the M4 | every test, through `_LIBCPP_DEBUG_RANDOMIZE_UNSPECIFIED_STABILITY` |
| `src/algorithms/case_folding*.{hpp,cpp}` (C7) | F1: every code point 0 to 0x10FFFF folds to CaseFolding.txt 18.0.0's C or S mapping, or to itself; F2: idempotent; F3: every scalar as text folds to the UTF-8 of its folding within count + count / 2; F5: compare_folded over every pair of the table's sources and targets agrees with the reference order | **not yet run.** F1 to F3 exhaustive (a proof by enumeration once the test passes, against the file read by the test's own parser); the table is the generator's output for the pinned hash (`stxlib_case_folding_table`, passing on the laptop). `harness_fold_scalar` (every u32, against a linear scan), `harness_fold_text` and `harness_compare` (well-formed texts to 8 bytes) written, not run | `tests/case_folding_tests.cpp`, `proofs/esbmc/case_folding.cpp`, `tools/gen_case_folding.py` |

None of these has been compiled or run except the Python checks named:
the integrator builds and runs them on the M4.

## stx_minmallocpp (STXlib's own allocator; the owner, 2026-11-15)

A conversion of mimalloc v3's design to house-style C++
(`src/state/stx_minmallocpp/`), chosen with `STXLIB_ALLOCATOR=stx_minmallocpp`.
Written on Neo. Built and run on the M4 on 2026-11-15 (macOS arm64, Clang;
the seat's logs `stx_minmallocpp/dev/logs/battery-1`, `round-2`, `round-3`),
and on no other host yet. "Enumerated" below means a test that covers its
whole domain, which is a proof once it has run: those ran and passed
(`stxlib_stx_minmallocpp_tests`, 18 checks). The arithmetic is integer
arithmetic on 64-bit values, so the result does not depend on the host.

What ran on the M4, all passing:
- the whole suite with this allocator as `stxlib_state`'s backend, Debug and
  Release (76 of 76 tests each, no compiler warning), and the default
  configuration (76 of 76);
- `stxlib_stx_minmallocpp_direct_tests` (89 checks, entry and exit checks
  on) in Release, under ThreadSanitizer, and under AddressSanitizer with
  UndefinedBehaviorSanitizer: 50 runs each, no failure and no report;
- the same under the integer sanitizers this host's compiler accepts
  (signed and unsigned overflow, shift, implicit truncation, pointer
  overflow), halting on any report: none. `STXLIB_INTEGER_CHECKS` itself
  does not configure on macOS arm64 (`implicit-integer-sign-change` is
  refused), so the repository's own integer build is still to run on Linux;
- `stxlib_stx_minmallocpp_surface`: the objects' leaves are exactly `mmap`,
  `mprotect`, `munmap`, `sched_yield`, `sysconf`, `pthread_key_create`,
  `pthread_setspecific`, `pthread_getspecific`, `abort` and the thread-local
  access routine.
These are evidence over the paths the tests take, not proof.

wasm32 (Emscripten), written on Neo, not yet built or run. The first web
run (emsdk 6.0.13 under Node, 9d1eb59) failed the direct tests at every
allocation: the 1 GiB regions cannot be placed in a 32-bit address space
of at most 4 GiB (2 GiB by default), the address test shifted a 32-bit
address by 48 (undefined), and commits went to `mprotect`, which Emscripten
does not implement. With 32-bit addresses the layout is now 64 MiB regions
(1,024 slices, 32 of metadata), an address limit of 2^31 and mappings of
their own above 16 MiB, every table position derived from the region's
size and the 64-bit layout pinned by `static_assert` (`layout.hpp`,
`page.hpp`, `region.hpp`); the address space comes from
`emscripten_builtin_memalign`, zeroed, with commit a no-op and decommit a
zeroing (`src/hosted/emscripten/minmallocpp_os_emscripten.cpp`). To run:
`stxlib_stx_minmallocpp_tests` (the 32-bit layout's numbers),
`stxlib_stx_minmallocpp_direct_tests` and `stxlib_stx_minmallocpp_surface`
(family `web`) in both web configurations.

| Module | Property | Status | Evidence |
|---|---|---|---|
| `layout.hpp` (`class_of`, `class_bytes`) | every size from 1 to 524,288 is held by its class and by no smaller one; sizes increase in multiples of 16 | the table's invariants are `static_assert`ed for all 56 classes; the rest enumerated, not yet run; ESBMC harness written, not run | `tests/stx_minmallocpp_tests.cpp`, `proofs/esbmc/stx_minmallocpp.cpp` (`harness_class_of`) |
| `layout.hpp` (the alignment rule) | for an alignment from 16 to 65,536, the class of the size rounded up to it has a block size that is a multiple of it, so aligned allocation is ordinary allocation | proved on paper (the header's comment and the seat's DESIGN.md 5.2); enumerated, not yet run; harness written, not run | the same (`harness_alignment_rule`) |
| `page.hpp` (`block_index`) | the multiply and shift equals the division for every class and every offset of its page | bound argued in the header; enumerated, not yet run; harness written, not run | the same (`harness_block_index`) |
| `region.hpp` (`run_list_of`, `run_list_floor`) | the lists partition run lengths 1 to 16,383 in increasing order | enumerated, not yet run; harness written, not run | the same (`harness_run_list_floor`) |
| `region.cpp` (the free-run index) | every free run is maximal, tagged at both ends and in the list of its length; an allocated run's end tags are 0; taking and giving a run are a fixed number of steps | not yet proved; argued in `region.hpp`. No harness yet (a model of the index against a set of intervals is the plan) | `tests/stx_minmallocpp_direct_tests.cpp` exercises it |
| `heap.cpp` (the ownership protocol) | no released block is lost; a page never has two owners; `free` and `remote` never both name a block unless a client released it twice; the count in a page's state is only ever a hint | not proved in general; argued in the seat's DESIGN.md 4.3. Checked by enumeration within a bound: `tests/stx_minmallocpp_interleave.cpp` runs the allocator's own code (each shared-memory step marked, `STX_MINMALLOCPP_STEP`) under every ordering of the steps with up to N forced switches, a fresh process per ordering, for five scenarios of two threads on one page (a release racing an abandon; two releasers finishing a page; a free and re-make under a late release; thread exit racing releases; two keepers claiming one page). A correction: the scenarios first used the largest class, whose one page is a heap's whole allowance, so no release ever claimed a page and the claim was not exercised (the runs of N = 2, 2,825 orderings; N = 3, 31,507 and 60,914; N = 4, 372,231 cover the other paths only). The scenarios now use the 64 KiB class, where a heap claims. On the M4, 2026-11-15, at `245a5ce`'s allocator: N = 3, 56,114 orderings, all keep the properties (no allocator check fails, no entitled release is refused, no deadlock, every page is given back once its blocks are); N = 4 not run on these scenarios. The checker is shown to see: four deliberately broken protocols (no announce; abandon ignoring pending; collect resetting the state after its scan; a claim that does not merge) are each caught as a lost release at N = 1. Limits: two threads, one page's worth of blocks, N forced switches, a sequentially consistent ordering of the steps (one thread runs at a time); nothing is claimed beyond them | ctest `stxlib_stx_minmallocpp_interleave` (N = 2); the direct tests' thread cases; ThreadSanitizer |
| the whole allocator, as a graph | every entry, every exit with what is written before it, every call that leaves the allocator, every cycle with its bound; no write precedes a refusal of an address that is not a block of ours | read off the source, not run (`proofs/stx_minmallocpp_graph.md`). The leaves are checked on the compiled objects by `tools/check_object_surface.py` (component file `tools/object_surface/stx_minmallocpp.toml`) (ctest `stxlib_stx_minmallocpp_surface`; its parsing tried with a stand-in `nm` only) | the same |
| entry and exit conditions | each function's stated conditions hold at run time | checked when `STXLIB_STX_MINMALLOCPP_CHECKS` is defined (always in `stxlib_stx_minmallocpp_direct_tests`): `STX_MINMALLOCPP_CHECK`, `page_whole`; evidence over the tests' paths only | `page.hpp`, `region.cpp`, `heap.cpp` |
| `allocator.cpp` (`release`) | an address that is not the first byte of a live block of this allocator is refused without reading or writing it or anything near it | argued from the code: the directory, `slice_head` and the block index decide before any write; a release that races the freeing of the same page is outside the argument (a client error already) | the direct tests' `refusals` |
| `allocator.cpp`, `heap.cpp` (early exit) | no path throws, calls back, or calls a cancellation point | argued: the host layer's calls are `mmap`, `mprotect`, `munmap`, `sched_yield` (POSIX.1-2024 XSH 2.9.5.2: in neither list), `pthread_key_create` and `pthread_setspecific` (not looked up: unconfirmed), and on Windows `VirtualAlloc`, `VirtualFree`, `SwitchToThread`; on Emscripten `emscripten_builtin_memalign` and `emscripten_builtin_free` (dlmalloc, C without exceptions, a pthread mutex inside, never calling back) in place of the three mapping calls | `src/hosted/posix/minmallocpp_os_posix.cpp`, `src/hosted/windows/minmallocpp_os_windows.cpp`, `src/hosted/emscripten/minmallocpp_os_emscripten.cpp` |
| the region tables | zero pages from the host are read as objects of the table types holding zero | assumed (`page.hpp`): `std::atomic<u32>` and `std::atomic<u64>` are checked to have the integer's size and to be lock free; the standard does not name `mmap` as creating objects | `page.hpp` |
| performance | no slower than mimalloc v3.6.1 on each path; never more than twice the host's allocator | measured on the M4 only, while mimalloc was still in the tree (it was removed on 2026-11-15, so the mimalloc columns cannot be repeated from this tree) (`tests/memory_bench.cpp`, through `stxlib::memory`, best of 7, three runs). Against the host's `malloc`: 0.13 to 0.95 of its time on every workload. Against mimalloc as `main` builds it (no release checking): about 1.3 ns slower per call on blocks up to 1 KiB (4.3 against 3.0 to 3.7) and on the 100,000-block churn (15.0 against 12.5); faster from 8 KiB up, in the 4 KiB batch (5.3 against 17.5), at 1 MiB (16 against 165) and across threads (3.3 against 3.7 to 4.8). Against mimalloc with its own checking on (`MI_SECURE=4`): equal on the smallest pairs (4.3 against 4.1), faster on everything else. Where a small call's time goes (`tests/stx_minmallocpp_floor_bench.cpp`, the allocator linked directly): 3.5 ns per call, against 4.3 through `stxlib::memory` and `stxlib_state`, so about 0.8 ns is the path into the library, which every backend pays; passing the thread's heap in saves nothing measurable on macOS (the thread-local read is not the cost); passing the size class in saves about 0.3 ns at most. A direct path for unaligned requests and a simpler `slice_head` then took the direct call to about 3.15 ns. A second wave made each of allocate and release one common path that keeps nothing across a call (rare cases go to an out-of-line general routine), dropped release's second multiply for a test on the product's low bits (proved by enumeration, `block_starts_at`), and keeps the heap's "mine, nothing pending" state ready: through `stxlib::memory`, 3.7 to 3.8 ns on pairs up to 64 KiB (mimalloc 3.0 to 3.4 at 16 to 256 bytes, 3.8 to 3.9 at 1 KiB), 3.75 on the 256-byte batch (mimalloc 4.15), 4.3 on the 4,096-block churn (mimalloc 5.2 to 5.5); the 100,000-block churn is still behind (16 to 17 against 13.9 to 14.3, on a loaded machine): the system sampler puts over half of it on the release side, where almost every release claims back a page the allocation before abandoned as full. A change that keeps full pages instead passed every check and was slower on that workload at the present allowance (19.5 to 20.5), so it is not in the tree (the seat's DESIGN.md 6.2 and `dev/review-efficiency.md`). Not measured on Linux, FreeBSD or Windows, where the thread-local access differs | the seat's DESIGN.md 6 |

## The allocator binding, layout 11 (branch `claude/mimalloc-removal`)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/memory.cpp` (`module_calls`, F9) | every call in a module uses stxlib_state's one AllocatorCalls object, and a module whose layout differs from stxlib_state's ends the process at its first allocator call | argued from the code, not proved: the memo is null until the first call, which goes through `current_allocator` (the layout check) and stores the address it returns with a compare-exchange from null; the object is constant-initialised and never written (state.hpp), so a relaxed load of the memo yields that object or null; the address stays valid while the module is loaded, since the module depends on stxlib_state. No test makes a layout mismatch | the file's comment |
| `src/core/memory.cpp`, `src/state/allocator.cpp`, `src/state/stx_minmallocpp/allocator.cpp` (F8) | `memory::allocate` returns null for an alignment that is not a power of two | read off the code: memory.cpp no longer tests it; every backend's `allocate` does on every path (`allocate_block` in both tag modes; stx_minmallocpp's `allocate_common` before its common path and `allocate_general` first). Tested by `refusals` (alignments 0, 3 and 24) on whichever backend the build links | `tests/memory_tests.cpp` |
| `src/state/state.hpp` (layout 11) | AllocatorCalls has no `heap_failure`; a module of layout 10 is refused by stxlib_state of layout 11 and the reverse | by construction (`layout_version` raised with the shape change) | `src/state/state.hpp` |

None of these has been compiled or run: the integrator builds them.

## stxlib::glyphs, the allocator's securing approach (the owner, 2026-11-16)

The font reader, the glyph store, the hinter, the rasterizer and the face
(`src/algorithms/glyphs/`) take a font as an application hands it in. The
owner asked that what was done for stx_minmallocpp be done here: the code
traced as a graph, its leaves checked on the compiled objects, its carried
state checked at run time in a checked build. Written on Neo. Built and run
on the M4 on 2026-11-16 (macOS arm64, Clang; the seat's logs
`stx_gtraster/dev/logs/gt-75` to `gt-81`), and on no other host.

What ran on the M4, all passing:
- the whole suite (109 of 109 tests, no compiler warning);
- the glyph tests, with the four programs built with the checks on
  (`stxlib_font_file_checked_tests`, `stxlib_glyph_hinter_checked_tests`,
  `stxlib_glyph_untrusted_checked_tests`,
  `stxlib_glyph_shape_buffer_checked_tests`), under AddressSanitizer with
  UndefinedBehaviorSanitizer: no report;
- the same under the integer sanitizers this host's compiler accepts (signed
  and unsigned overflow, shift, implicit truncation, pointer overflow),
  halting on any report: none. `implicit-integer-sign-change` is refused by
  Apple's compiler on arm64, so that one check is still to run on Linux;
- `stxlib_glyph_hinter_tests`, with the checks off and on, under
  ThreadSanitizer (its case of four threads, each with a face of its own on
  one typeface): no report;
- `stxlib_glyphs_surface`: the objects' leaves are exactly the allocator
  (`vocabulary_allocate`, `vocabulary_release`), `ErrorRecord::make`,
  `Mutex` (made, `lock`, `unlock`, unmade), `requirement_failed`, and the
  compiler's `bzero`, `memcpy`, `memset_pattern16` and stack protection.
These are evidence over the paths the tests take, not proof.

Turned text by a mask's surface (the owner, 2026-11-16) was added after
that run and built and run on the M4 the same day (logs `gt-116` to
`gt-120`): the whole suite, 113 of 113 with no compiler warning; the glyph
tests, now with six programs built with the checks on (the two more are
`stxlib_glyph_shape_checked_tests` and `stxlib_glyph_turn_checked_tests`),
under AddressSanitizer with UndefinedBehaviorSanitizer, under the integer
sanitizers as above, and the hinter tests under ThreadSanitizer: no report.

| Module | Property | Status | Evidence |
|---|---|---|---|
| the glyph code, as a graph | every entry; every call that leaves the glyph code; no function calls itself; every cycle has a stated bound | read off the source, not run (`proofs/glyphs_graph.md`). The exits, with what is written before each, are tabled for the reader's first entries (`open_font`, `glyph_of`, `next_stretch`, `glyph_in_stretch`, `has_feature`, `advance_of`, the top of `load_glyph`), the glyph store and the face; the outline loaders, the hinter and the rasterizer have their cycles and bounds stated and their exits not yet tabled | the same, section 6 for what is missing |
| the leaves | the glyph objects call nothing but the allocator, the error record, the mutex, the requirement handler and the compiler's helpers: no file, stream, environment, clock, host call, C library function, `operator new` or exception runtime | checked on the compiled objects on macOS arm64 (ctest `stxlib_glyphs_surface`); the tool's parsing tried with stand-in listings, 79 checks (ctest `stxlib_glyphs_surface_tool`); its Microsoft spellings written from the mangling rules and not run on that toolchain | `tools/check_glyphs_surface.py`, `tests/check_glyphs_surface_tests.py` |
| `font_file.cpp` (the reader's entries) | every read of the font's bytes is inside them; an entry given any bytes returns, refuses or answers "nothing", and writes only its results | argued from the code (every read behind `has`, whose two comparisons cannot wrap) and tabled exit by exit; not proved. Searched within a bound: `stxlib_glyph_untrusted_tests` (1,202 wrong character maps and their prefixes, 60,600 altered maps, 30,744 altered fonts, 31,080 altered layout tables, from fixed seeds), each answer checked against the promise stated in the header, and `stxlib_font_file_tests` P8 (four whole fonts, TrueType and CFF: every prefix, every byte set to each of five values, 60,000 altered copies each; 256,012 fonts through the reader, 76,012 of them through a `Face` in all three fits), under the sanitizers above | `proofs/glyphs_graph.md` 3; `tests/glyph_untrusted_tests.cpp`, `tests/font_file_tests.cpp` |
| `font_file.cpp` (`next_stretch`) | going through a character map, in order or not, takes no more calls than it has entries, and one | argued (`proofs/glyphs_graph.md` 3.3: the search's answer rises with `from` and cannot stay); the count is checked on every map the search above tries | the same |
| `font_truetype.cpp`, `font_cff.cpp` | a glyph that names itself, or runs without end, is refused within `FontLimits`; an outline given is whole (as many tags as points, contours in order and ending at the last point, within the limits) | the bounds read off the source (a fixed number of turns over a fixed stack; a count of operators); "whole" checked at the exits in a checked build and on the 415,410 outlines the search loads; exits not tabled | `proofs/glyphs_graph.md` 3.6; `glyph_checks.hpp` (`outline_whole`) |
| `glyph_store.cpp` | the index and the three parts agree after every call, whatever was refused or ran out of memory on the way | tabled (`proofs/glyphs_graph.md` 4.1) and checked at every exit of `glyph` and `keep` in a checked build (`GlyphStore::whole`); evidence over the tests' paths only. No test makes the allocator refuse inside the store | `tests/glyph_hinter_tests.cpp` (the store's and the face's cases) |
| `glyph_face.cpp` (`Typeface`) | one lock, taken once and released on every exit; nothing called under it takes another or leaves the glyph code except to the allocator | read off the code (`ScopedLock`; the leaves above). Four threads on one typeface, each with its own face, under ThreadSanitizer: no report, and a sum over what each thread draws equals the single thread's; evidence over that one case's orderings, not a search of orderings | `proofs/glyphs_graph.md` 4.2; `tests/glyph_hinter_tests.cpp` (F8) |
| `glyph_face.cpp` (`Face`) | the fitted outlines lie end to end, each with the points its segments need, each found by its glyph, size and fit; the table is never more than half full, so a search of it ends | tabled (4.3) and checked at the exits of `fitted`, `mask`, `measure_run` and `draw_run` in a checked build (`Face::whole`); evidence over the tests' paths only | the same tests |
| the hinter | the same numbers as FreeType 2.14.2's on every glyph of the 37 bundled faces at seven sizes, light and monochrome; a glyph with more segments than the limit is left unhinted on that axis | measured in the seat against FreeType (412,244 glyph sizes a mode, all equal), not a test of this repository: FreeType is not in the tree. The bounds read off the source; exits not tabled | the seat's notes (`stx_gtraster/dev/SEAT-NOTES.md`); `proofs/glyphs_graph.md` 5 |
| integer arithmetic | no signed or unsigned wrap outside a function marked `STXLIB_MODULAR` (one: the fitted table's hash) | run under the integer sanitizers this host accepts, over the glyph tests: no report. Not proved; `implicit-integer-sign-change` not run | `src/core/checked.hpp`; the seat's `mm_gt16.sh` |
| `turn_lattice.cpp`, `turn_lattice.hpp` (a mask's surface, by which lightly fitted text is turned) | the cell is stx_conv's line cell exactly (whole numbers of 14400ths, every division exact); a lattice's controls at the mask's pixels are the pixels; no turn at a whole pixel gives the mask back, box and bytes; a turn writes every byte of the box it is given and reads nothing outside the lattice; no step leaves 64 bits in the cell or 32 in a pixel's sums | the bounds argued at the head of `turn_lattice.hpp` and tabled (`proofs/glyphs_graph.md` 4.4); not proved. Tested against numbers worked out apart from the code, by `tools/turn_lattice_oracle.py` (stx_conv's formula in exact fractions): 40 cells, a whole lattice, 6 turns byte for byte, 36 more masks and turns as two sums; and searched within a bound: masks of 1 to 256 pixels a side of four kinds at turns and shifts of every size, under the sanitizers above. Measured in the seat against stx_conv's own compiled cell in binary64 (not a test of this repository) on four bundled faces at five sizes: of 4,097,030 controls, 520 differ, each by one 32nd of a level | `tests/glyph_turn_tests.cpp`, `tests/glyph_hinter_tests.cpp` (`face_turned_surface`) |

## stxlib::boundary, the Emscripten family (owner decision 2026-11-15)

Hosted modules only: no pure core was added, so nothing here is in proof
scope; each row is tested under Node (`tests/emscripten_tests.cpp`,
`tests/platform_tests.cpp`, `tests/simd_tests.cpp`), and what Node cannot
show waits for a browser run.

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/core/capabilities.cpp` (Emscripten table, capabilities 30 to 33) | every capability answered; those a page lacks false; shared_memory, cross_origin_isolated and fixed_thread_pool as the runtime reports; native answers for 30 to 33 | not yet proved; tested under Node (no fixed pool there) and natively | `tests/platform_tests.cpp` |
| `src/hosted/emscripten/clocks_emscripten.cpp` | never backwards within a thread; ElapsedTime reads the interval clock; the conversion clamps to [0, i64_max] (NaN to 0) | not yet proved; tested on one thread (100,000 reads), sampled. Order across threads unconfirmed | `tests/emscripten_tests.cpp` (`clocks`) |
| `src/hosted/emscripten/wake_emscripten.cpp` | as the POSIX row above, without host handles (a list is refused with unsupported); the flag released once, after the channel lets go of it | not yet proved; tested (cross-thread wake at 20 ms, timeout at 30 ms, sampled) | `tests/emscripten_tests.cpp` (`wake_channel`) |
| `src/hosted/emscripten/storage_*`, `src/hosted/posix/file_posix.cpp` (volume rules), `src/core/web.cpp` | memory volumes accept every level; IDBFS honours ordered and refuses durable; a durable replace refused before its rename where it cannot be durable; restore and persist refused on the main thread; mounts refused where the browser lacks the volume | not yet proved; under Node: memory volumes, the refusals, and persist from a thread with no IndexedDB volume. Not tested: IDBFS and OPFS themselves (absent in Node), the page-hide hook | `tests/emscripten_tests.cpp` |
| `src/algorithms/simd.cpp` (SIMD128) | vector128 exactly when the module was compiled with SIMD128; the name "wasm-simd-128" | not yet proved; tested | `tests/simd_tests.cpp`, `tests/emscripten_tests.cpp` |
| the math kernels on wasm | the frozen bit-identity hashes (scalar kernels; the dispatch is x86-64 only) | **not yet run**: the first wasm run is cross-architecture evidence for the bit identity | CTest `stxlib_bit_identity`, `stxlib_bit_identity_dekker` |

Nothing in this section has been compiled or run: emsdk is not installed
on the M4 (a download the owner approves first), and no compiler runs on
the seat's machine.

## Pointer width (wasm32, owner decision 2026-11-15)

The Emscripten family builds for wasm32, so usize and uptr are 32 bits
there (clang's WebAssembly target: size_t is unsigned long, 32 bits on
wasm32; __int128 is available and 64-bit atomics are inline,
`clang/lib/Basic/Targets/WebAssembly.h` on llvm main, not read at the
pin). Emscripten's musl keeps time_t and off_t 64-bit on wasm32
(`system/lib/libc/musl/arch/emscripten/bits/alltypes.h` at 6.0.13:
`typedef _Int64 time_t;`, `typedef _Int64 off_t;`), so the static_asserts
on them in `src/hosted/posix/` hold. Every place found that depends on
the width, and how it is classed: **essential** (kept; wasm32 refused at
that point), **width-neutral** (correct at either width as written) or
**fixed** (changed so 32-bit pointers work, with a test). Found by
reading and `git grep`; the first wasm32 build (`-Wconversion` on) and an
integer-checks run under Node would find what reading missed.

| Place | Width dependence | Class | Evidence |
|---|---|---|---|
| `include/stxlib/types.hpp` | `sizeof(void*) == 8` | **fixed**: 4 admitted under `__wasm32__` only; other 32-bit targets stay refused (not audited) | the static_asserts; `tests/emscripten_tests.cpp` (`pointer_width`) |
| `include/stxlib/trampoline.hpp`, `src/core/trampoline.cpp` | a handle packed 16 index bits and 48 generation bits into a context pointer | **fixed**: the generation takes the pointer's remaining bits (`trampoline_generation_limit`: 2^48 - 1, or 65535 on wasm32); a slot retires after it, a generation past it is stale | static_asserts in `trampoline.cpp`; `tests/trampoline_tests.cpp` (`handles_fit_the_pointer_width`: exhausts a slot on wasm32) |
| `src/core/thread_pool.cpp` (create) | `sizeof(Thread) * thread_count` (u32) wraps in 32-bit usize, and the start loop would overrun the short array | **fixed**: refused with out_of_memory before any allocation | `tests/thread_pool_tests.cpp` (`creation`, wasm32 branch) |
| `src/algorithms/stx_btree/optimal_tree.hpp` (Knuth's triangles) | (n + 1)(n + 2) / 2 cells, the row offsets and column starts formed through products that wrap in 32 bits from n = 92681 | **fixed**: `detail::triangle_number` (checked, even factor halved first), offsets built by addition, column starts by `known_triangle_number` | static_asserts in `tests/stx_btree/optimal_tree_test.cpp` (32-bit arithmetic) |
| `src/algorithms/stx_btree/detail/tree_packing.hpp` (count separators) | the child-capacity growth formed an unchecked product; at capacity 12 it wraps in 32 bits for counts above 815730720 and went on from the wrapped value | **fixed**: `detail::child_capacity_for` stops before an unrepresentable product (which is at least the count) | static_asserts in `tests/stx_btree/optimal_map_test.cpp` (32-bit and 64-bit) |
| `src/core/pe_image.hpp` (pure core, proof scope) | a header field plus a displacement in usize wraps on wasm32 (export RVA 0xFFFFFFF0 + 20 = 4), so a damaged image read the wrong bytes in bounds | **fixed**: offsets are u64 and the readers check them against the view | `tests/platform_rest_exhaustive.cpp` (`offsets_near_the_u32_end`); `proofs/esbmc/platform_rest.cpp` (32-bit run, below) |
| `include/stxlib/shadow.hpp` (`ShadowFrameRecord`, 120 bytes) | fixed-width fields only; u64 is 8-aligned on wasm32 as on the 64-bit targets | width-neutral (the size assert holds at both widths) | `static_assert` in `shadow.hpp`; `tests/emscripten_tests.cpp` |
| `src/state/state.hpp` (layout 8) | usize and uptr fields, so the layout differs by width | width-neutral: the layout is per process, and a process has one width; `layout_version` needs no change | reading |
| `src/state/allocator.cpp` | the host backend's slot is `sizeof(void*)`; tags are u64 with the address widened; the registry hashes the widened address | width-neutral; one hardening for sb1 (the hand-in): the stored offset is narrowed to usize before it is checked | reading |
| `src/core/atomic_rules.cpp` | atomics of usize, uptr, u64 | width-neutral: 64-bit atomics are lock-free on wasm32 (the asserts would refuse otherwise) | the static_asserts |
| `src/core/growable.hpp`, `include/stxlib/buffer.hpp`, `stx_btree/detail/array.hpp`, `optimal_map.hpp`, `src/algorithms/voting.cpp` | element count times element size | width-neutral: each product is checked against the usize maximum first | reading; `tests/vocabulary_tests.cpp`, `tests/memory_tests.cpp` |
| `src/core/files.cpp` (read_file) | a u64 file size narrowed to usize | width-neutral: narrowed only after `bytes <= limit` (a usize) | reading |
| `src/algorithms/precedence.cpp`, `src/algorithms/row_height_index.cpp` | u64 block numbers and counts narrowed to usize | width-neutral: a block below `known_blocks()`, counts of objects in memory | reading |
| `src/algorithms/stx_btree/detail/node_layout.hpp` | edges are `uintptr_t`, so internal pages hold more edges on wasm32 | width-neutral: capacities are derived from sizeof; no test states a capacity | reading |
| `src/core/stack_line.hpp` | addresses printed to 16 hex digits | width-neutral: the same text, leading zeros on wasm32 | `tests/platform_rest_exhaustive.cpp` |
| the bit-identity digests (`tests/bit_identity.cpp`) | counts and lengths enter the digest | width-neutral: every value is widened to u64 before it is hashed, and no item digests an address, a sizeof or usize_max; no expected line depends on the width | reading; the first wasm run compares against `tests/bit_identity_expected.txt` |
| the bounded proofs (`proofs/esbmc/`) | proved at 64 bits only | to run twice: as written and with `--32 -D__wasm32__` (each harness's header) | not yet run |

No place was classed essential: nothing in the tree needs 64-bit
pointers once the rows marked fixed are in.

## Security audit 2026-11-15, the include section (branch `claude/audit-include`)

| Finding | Change | Evidence |
|---|---|---|
| A-stxlib-include-01-F1 (Medium) | `Unique` move assignment takes the source before destroying the old object; the destructor requirement moved from the class to `make`, so a node may own its successor | tested: `tests/owned_vocabulary_tests.cpp` (`unique_list_pop`: three pops, live counts and released blocks), not proved |
| F1 pattern elsewhere | the same order in `Vector`, `Optional` (copy and move), `Buffer`, `PriorityQueue`, `precedence::Verifier`, stx_btree's `OwnedEntries` and `UnsyncTree`; `Shared`, `Weak`, `Array` already had it | tested for `Vector` and `Optional` (`vector_from_inside_an_element`, `optional_from_inside_its_value`); the rest by reading |
| F2 | `Vector` and `Optional` assignment, as above | as above |
| F3 | `Vector::emplace` builds the new element in the new block before the old storage goes; an argument may refer into the vector | tested: `vector_emplace_and_exact_copies` |
| F4 | `Vector::copy_of` and `copy` allocate exactly the count | tested: capacities 5, 1 and 0 |
| F5 | `find_ignoring_ascii_case` starts from the next boundary, as `find` does | static_asserts and the UTF-8 exhaustive domain (`tests/ascii_search_tests.cpp`, `utf8_search_exhaustive`: every text of up to 4 pieces of 1, 2 and 4 bytes, every needle of up to 2, every start) |
| F6 | `precedence::Verifier` declares its moves; a moved-from verifier knows the anchor's block only | tested: `tests/precedence_tests.cpp` (`lifecycle`) |
| F7 | the gates are in an inline namespace named for the unit's exception setting, and `Trampolines::invoke` takes the setting as a defaulted template argument, so the two bodies are two entities | by reading (no mixed-setting link is built) |
| F8 | `open_link_with_desktop`: links only; `open_with_desktop` is documented for paths the program made | tested: `tests/children_tests.cpp` (`desktop_refusals`) |
| F9 | `copy_file` carries 0777 by default, 07777 with `CopyPermissions::with_special_bits` | tested on POSIX hosts: `tests/files_tests.cpp` (`copy_permissions`) |
| F10 | a trampoline table packs its slot index in as few bits as its capacity needs; the generation takes the rest, capped at 2^48 - 1 | static_asserts in `trampoline.cpp`; `tests/trampoline_tests.cpp` (`handles_fit_the_pointer_width`: exhausts slot 0 of a 65535-slot table on wasm32) |
| F11 | AUTO tags are on whenever the host backend is chosen; the Emscripten default is stx_minmallocpp (the finding's premise is stale at e549318), docs corrected | by reading CMakeLists.txt |
| D15 follow-up: incomplete element types | `Vector` checks its element in its destructor and asks about assignability through a function; `Shared` checks its value in `make`; `Weak` asked nothing at class scope already. `struct Node { Vector<Node> children; }` and a node holding `Shared` and `Weak` of its own type compile | tested: `vector_of_its_own_node`, `shared_of_its_own_node` |
| needs a check: restored frames' text counts | `file_copy` cuts each restored frame's counts to the arrays and to the well-formed UTF-8 prefix | tested: `tests/shadow_tests.cpp` (`a_park_into_too_little_room_parks_nothing`) |

Nothing on this branch has been compiled or run yet (no compiler on Neo).

## stxlib::textlayout (bidi, line breaking, script runs; branch `claude/stx-textlayout`)

`include/stxlib/textlayout.hpp`, `src/algorithms/textlayout/`. The code is
the model: the statements below are about the call graph of the two
objects (`analysis.cpp`, `line_break.cpp`), its entries and its exits, each
checked on the code or on the compiled objects, and about small finite
domains that a test covers whole. "Enumerated" means such a test: a proof
once it has run. What ran, and where, is at the end.

### The graph

- **No recursion.** The functions of the two files, by what they call:
  - `line_break.cpp`: `find_line_breaks` (two) → `break_utf8` or
    `break_code_points` → `kernel::decode`, `tables::record`,
    `kernel::look_*` → `kernel::peek_*` → `decode`, `record`. The property
    queries call `tables::record` only.
  - `analysis.cpp`: `analyse_paragraph` (two) → `Access::clear`,
    `Access::scan_*` (→ `grow_breaks`, `decode`, `record`, `look_*`,
    `script_event` → `append_script_run`), `Access::analyse` (→ `grow`,
    `prepare`, `append_runs` → `key_at`; `resolve_sequence` →
    `pair_brackets`; `write_levels`; `resolve_explicit` → `first_strong`,
    `append_runs`, `resolve_sequence`, `write_levels`). `analyse_line` →
    `Access::clear`, `Access::line` (→ `starts_code_point`, `point_at`,
    `grow`, `fill`).
  The calls go down by layer (entry, walk or resolver, rule, lookup); no
  function is reachable from itself. Read off the code; the object check
  below bounds what the graph can reach outside the two files.
- **No callbacks.** No function pointer, virtual call, lambda or
  `std::function` in the two files; the reader of the input in the second
  pass (`Utf8Next`, `CodePointNext`) is a template argument, resolved at
  compile time.
- **Foreign calls: a short list of leaves.** The two objects leave
  undefined only symbols of namespace stxlib and the compiler's block
  helpers. Checked on the objects by `tools/check_textlayout_surface.py`
  (CTest `stxlib_textlayout_surface`), which fails on any other symbol: no
  C library function, no `operator new`, no exception runtime, no guard of
  a local static. The leaves it listed on the M4:
  `stxlib::ErrorRecord::make` (two overloads), `stxlib::detail::
  vocabulary_allocate` and `vocabulary_release` (the allocator, through
  `Vector`), and `bzero`, `memcpy`, `memset` and the stack protector's two
  symbols. In the Debug build the same check passed.
  `line_break.cpp` allocates nothing; `analysis.cpp` allocates only
  through `Vector::resize` on the caller's `Paragraph` and `Line`.

### Bounds

Every loop of the two files, with its bound. `n` is the number of code
units of the paragraph (or text), `p ≤ n` its code points, `r ≤ p` the runs
of a sequence.

| Function | Loop | Bound |
|---|---|---|
| `break_utf8`, `break_code_points` | the walk | `index` rises by at least 1 each step: ≤ n steps |
| `break_utf8`, `scan_utf8` | the letters search and fill | each visits the run's code units once; the walk resumes after them, so the walk and its letters loops together visit each code unit once |
| `kernel::peek_*` | forward to the next base | ≤ n − index; over a whole walk each code point is read by the look-aheads of at most the two bases before it (a look-ahead stops at the second base), so ≤ 2p reads in all |
| `scan_*` | blocks, then code points | ≤ ⌈n / 65,536⌉ blocks; `index` rises by at least 1 each inner step |
| `grow_breaks` | none; it at least doubles | ≤ 32 reallocations for any input (n < 2^32) |
| `script_event` | search of the open brackets; settling them | ≤ 32 each (`script_bracket_depth`) |
| `append_script_run` | none; doubles | amortised constant |
| `key_at`, `point_at`, `bidi_mirroring_glyph`, the level-run search in `resolve_explicit` | binary searches | ≤ 32 steps |
| `append_runs` | code points of a level run | each code point of the paragraph is in one level run, so ≤ p over a paragraph |
| `pair_brackets` | runs; the stack search | r; ≤ 63 per closing bracket |
| `resolve_sequence` | W1 to W7, N1, N2, I1, I2: one pass each over the runs; the ET stretch scan advances `stretch_end` monotonically | a fixed number of passes of r |
| `resolve_sequence`, N0 | per pair, the runs between its brackets; then back to the last strong run | at most 63 pairs are open at any run (BD16), so the forward scans sum to ≤ 63 · r; the backward scan is ≤ r per pair, so ≤ r · pairs in the worst case, pairs ≤ r / 2. This is the one bound that is not linear |
| `resolve_explicit` | BD9, X1 to X8, level runs: one pass each of p; the stack unwind at a PDI | ≤ p each; the unwind ≤ 127 (the stack's depth) |
| `resolve_explicit` | sequences: `while (true)` following an initiator to its PDI | `current` strictly increases (the search starts at `current + 1`): ≤ level runs ≤ p; over all sequences each level run is appended once |
| `analyse`, second pass | code points; brackets | p each |
| `Access::line` | copy; L1 backwards; runs; L2 | n; p; n; L2: at most 126 passes (levels) over the line's runs, each run swapped at most once per pass |
| `fill`, `fill_breaks`, `write_levels` | the range | its length; `write_levels` writes each code unit of the sequence once |

So `find_line_breaks` is linear in the text, with at most three reads per
code point; `analyse_paragraph` is linear except rule N0 on adversarial
bracket nesting; `analyse_line` is linear in the line times the number of
levels present.

### Exit tables

Every public entry is `noexcept` and returns by value. "Writes" are the
writes that have happened when the exit is taken.

`find_line_breaks(output, text)` and `(output, code_points)`:

| Condition | Writes before it | Result |
|---|---|---|
| `output.count()` differs from the input's count | none | `invalid_argument` |
| the input is empty | none | success |
| otherwise | every element of `output` (the last one twice: by the walk, then by LB3) | success |

`analyse_paragraph(text, offset, direction, paragraph)` and the code-point form:

| Condition | Writes before it | Result |
|---|---|---|
| the input has more than 2^32 − 2 elements | the paragraph cleared | `limit_exceeded` |
| `offset` ≥ the count | cleared | `out_of_range` |
| (UTF-8) `offset` is inside a code point | cleared | `invalid_argument` |
| storage for the decisions or the script runs cannot grow (in the walk) | cleared; what the walk stored is in the vectors but unreachable (`length() == 0`, `script_runs()` empty) | the allocator's failure |
| storage for levels, classes, units, keys or the workspace cannot grow | cleared, as above | the same |
| otherwise | offset, length, base level, the level of every code unit, decisions, script runs | success |

`analyse_line(paragraph, offset, length, line)`:

| Condition | Writes before it | Result |
|---|---|---|
| `length == 0`, or the range is not inside the paragraph | the line cleared | `out_of_range` |
| either end is inside a code point | cleared | `invalid_argument` |
| storage for the line's levels or runs cannot grow | cleared | the allocator's failure |
| otherwise | the line's levels and runs | success |

The queries (`bidi_class`, `bidi_mirrored`, `bidi_mirroring_glyph`,
`bidi_paired_bracket`, `line_break_class`, `east_asian_wide`, `script_of`,
`script_extends_to`, `script_tag`) have one or two exits each and write
nothing; a value above U+10FFFF takes the stated default without a table
read.

### Facts per node

| Node | Entry condition | Fact | Status |
|---|---|---|---|
| `tables::record` | `code_point ≤ 0x10FFFF` | both table indices are in range | the generator's check on the table it writes; every caller tests the range, or passes a decoded scalar or `record_of_input`'s substitute |
| `kernel::decode` | at the first byte of a code point of well-formed UTF-8 | reads only that code point's bytes; the value is a scalar | `TextView`'s invariant; `analyse_paragraph` checks the byte at the offset itself |
| the automaton step | `state < 103`, `symbol < 62` | the index `(state << 6) | symbol` is inside `lb_dfa`, and the entry's state is again below 103 | enumerated by the generator over the table it writes; T2 visits every reachable entry |
| the look-ahead | the action is ≥ 3 | the first row is below `lb_look_first_count`, the symbol index at most 62 (62 is the end of text), a second row below `lb_look_second_count` | enumerated by the generator over every entry |
| the automaton against the rules | none | from the start of text, for every sequence of symbols of every length, the automaton and its look-ahead give the rule chain's decision at every boundary | enumerated (T2): the pairs (chain state, automaton state) reachable from the start are closed under every symbol, the decisions agree at each for every look-ahead the chain reads, and the pairing is a function |
| the rule chain against UAX #14 | none | the chain gives LineBreakTest.txt's expected result on every case | the conformance suite (T4), all cases; not a proof beyond what the suite exercises |
| the letters form | the byte before was an ASCII letter and has been stepped | the automaton is in the letter state; every further ASCII letter gives "no break" and stays there; ASCII letters are bidi L, Latin, without extensions, not brackets | enumerated (T3) over every state and the 52 letters, and checked by the generator |
| decisions as boundaries | after a successful walk | the decision after a code unit is `inside_character` exactly when the next code unit continues the same code point | by construction of the walk (the only writer of `inside_character`); T6 and the fuzz compare it with the input |
| `prepare` | `points` = code points + 1 | the arrays carved from the two workspaces are disjoint and each holds `points` entries | read off the code: consecutive offsets of `points` in blocks of 3 or 6 (bytes) and 5 or 10 (words) times `points` |
| run lists | `append_runs` over a level run | at most one run per code point, so the arrays of `points` entries hold them | read off the code |
| the X1 to X8 stack | depth 125 | at most 127 entries are used (UAX #9, 3.3.2) | constants in the code; BidiTest's and BidiCharacterTest's deep cases under the sanitizers |
| `script_event` | none | `depth ≤ 32` at every array access | read off the code: reset to 0 before the write when it is 32 |
| u32 arithmetic | the input has at most 2^32 − 2 elements | no offset or count overflows | the entries refuse a longer input; the integer sanitizers halt on any wrap in the tested runs |

### Tests

`tests/textlayout_tests.cpp` (line breaking, scripts, the walk) and
`tests/textlayout_bidi_tests.cpp` (the bidi rules):

| Test | Domain | Kind | Result on the M4 |
|---|---|---|---|
| T1 | every code point: Line_Break after LB1, East Asian width, Script, Script_Extensions against the Unicode files, read by the test's own parsers | enumerated | 1,114,112 code points, 0 mismatches |
| T2 | the automaton against the rule chain | enumerated | 6,749 chain states paired with all 103 automaton states, 729,781 decisions, 0 differ |
| T3 | the letters form's facts | enumerated | 103 states, 52 letters, 0 wrong |
| T4 | LineBreakTest.txt, through the chain and both entries | conformance suite | 19,346 of 19,346 |
| T5 | `find_line_breaks` against the chain on every sequence of symbols to length 4 (one code point per symbol), and random ones | enumerated to length 4; then sampled | 15,018,570 sequences and 20,000 random, 0 differ |
| T6 | `analyse_paragraph`: breaks against `find_line_breaks` of the paragraph alone, script runs against the test's itemiser, UTF-8 against code points, paragraphs longer than two blocks | sampled | 34,198 paragraphs, 0 fail |
| T7 | bounded fuzz: random bytes kept when well formed, random scalars; structural invariants of every entry | sampled | 1,000,000 inputs under the sanitizers (760,777 well formed), 0 fail |
| T8 | arguments and a two-script example | examples | pass |
| B1 | every code point: Bidi_Class, mirroring, brackets against the Unicode files | enumerated | 1,114,112, 0 mismatches |
| B2, B3 | BidiTest.txt, BidiCharacterTest.txt | conformance suites | 770,241 of 770,241; 91,707 of 91,707 |
| B4 | the library against the per-code-point reference: every class sequence to length 5, bracket sequences to length 6, random | enumerated over those domains; then sampled | 55,092,732 comparisons, 0 differ |
| B5 | paragraphs and lines | sampled | 46,148 paragraphs, 0 mismatches |

### What ran

On the M4 (macOS arm64, Apple Clang), 2026-11-16, from the working tree
before its commit (the seat's logs `sc1/dev/logs/tl-quick2-*` and
`tl-full1-*`):
- Release: both test programs pass (42 and 50 checks) with the counts
  above, no compiler warning in the new files; the surface check passes;
  the generator's `--check` passes.
- Debug: CTest `stxlib_textlayout_bidi_tests`, `stxlib_textlayout_tests`,
  `stxlib_textlayout_surface`, `stxlib_textlayout_tables`,
  `stxlib_public_headers`, `stxlib_std_rules`: 6 of 6 pass.
- AddressSanitizer, UndefinedBehaviorSanitizer and Clang's integer group
  (`-fsanitize=address,undefined,integer -fno-sanitize-recover=all`),
  Debug: `stxlib_textlayout_tests` with a fuzz of 1,000,000 inputs passes
  with no report; `stxlib_textlayout_bidi_tests` passes its 50 checks
  with no report (BidiTest 770,241 of 770,241, BidiCharacterTest 91,707 of
  91,707, 33,541,180 comparisons with the reference, 0 differ), and the
  surface check passes on the sanitized objects.
- The oracle comparisons and the timing are in `docs/TEXTLAYOUT.md`.

After the security audit's fixes (A-stxlib-textlayout-01 F1-F4) and the
byte counts, on the M4 at the fold (the integrator's logs `tl-fix1` and
`round14-gate`): the seat's run above again passes on the fixed tree
(`stxlib_textlayout_tests` 53 checks before the byte counts); with the
byte counts, the full CTest suite passes in Debug, Release, Release
without exceptions, ASan with UBSan, and the integer checks, with
`STXLIB_UNICODE_TEST_DATA` set, and in Debug, the integer build and ASan
with UBSan `stxlib_textlayout_tests` passes 64 checks (LineBreakTest
19,346 of 19,346) and `stxlib_textlayout_bidi_tests` 50 (BidiTest 770,241
of 770,241, BidiCharacterTest 91,707 of 91,707).

Open: the 5,264 texts where libunibreak and STXlib differ for an "other"
cause (`docs/TEXTLAYOUT.md`) have not been examined one by one. LB12a at
18.0.0 is inferred from LineBreakTest 18.0.0, not read in UAX #14 18.0.0
(unconfirmed).

Not proved: that the rule chain is UAX #14 beyond its conformance suite;
that the bidi rules are UAX #9 beyond BidiTest, BidiCharacterTest and the
exact comparison with the reference; the script rules against any
conformance suite (UAX #24 has none), only against the test's own
itemiser and the oracle. Run on one host only (macOS arm64, Clang).

## Ordinary copies of the owned vocabulary (branch `claude/vocab-copy`; owner, 2026-11-16)

The owner's direction: copying is creating a new object with the old one's
contents; exhaustion is an allocation problem, not a copy problem (notes
`decisions/2026-11-16-vocabulary-copy.md`). The following now have noexcept
copy constructors and copy assignment:
- `Utf8String`;
- `Vector<T>`, for any T with a nothrow copy constructor (checked where the
  copy is instantiated, so a type holding a Vector of itself stays
  declarable);
- `Buffer<T>`, `Path` and `PriorityQueue<V, O>`. `Buffer` and
  `PriorityQueue` gain a reporting `copy()` to match.

The copy allocates through STXlib's allocator. An allocation it cannot have
ends the process through `take_or_fail_fast`, with the out_of_memory record
of `copy()`. `copy()` and `copy_of()` still return `OperationResult`. Q1 is
unchanged.

`Optional`'s rule does not change: it is copyable whenever its value has a
nothrow copy constructor, so `Optional<Utf8String>` and
`Optional<Vector<T>>` copy now.

Still not copyable:
- `Unique`: one owner by definition;
- `FrozenMap` and `FrozenTextMap`: they own an opaque search structure, and
  a copy would rebuild or clone it, so `build` stays the way to make a
  second map;
- `OperationResult`: a return vehicle;
- the objects that own host resources (File, Thread, Child and the rest).

| Module | Property | Status | Evidence |
|---|---|---|---|
| `vector.hpp` (copies) | the copy constructor and copy assignment (over 0, 1 and 4 old elements, and to itself) give the model's elements, capacity exactly the count for a fresh copy, with no leak, double destruction or use after destruction; `copy()` refused fails with out_of_memory and changes nothing | **not yet run.** Proved for every sequence of up to 4 operations over `vector_candidates`, which now include the four copy steps, for both element forms, once the test passes. The refused branch leaves out the two ordinary copy steps, which fail fast by design | `tests/owned_vocabulary_tests.cpp` (`vector_exhaustive`) |
| `text.hpp` (copies) | a copy equals the original, has its own storage, and is unchanged by changing the original; assignment over every other length gives the source | **not yet run.** Proved for every length from 0 to 40 bytes (inline and allocated) against every old length from 0 to 40, and for self-assignment, once the test passes | `tests/owned_vocabulary_tests.cpp` (`ordinary_copies`) |
| aliasing and the other types | a tree node's children assigned from a grandchild's children (the source dies in the assignment); an element assigned from a sibling; `Optional<Utf8String>`, `Buffer`, `Path` and `PriorityQueue` copies (same contents, same pop order) | tested | `tests/owned_vocabulary_tests.cpp` (`ordinary_copies`) |
| fail fast on exhaustion in a copy | the process ends with the out_of_memory record | not tested: the copy constructor has no refusal hook, and arming the vocabulary refusals would end the test program; argued from `take_or_fail_fast`, which is already tested by the death tests | `include/stxlib/fail_fast.hpp` |

Nothing on this branch has been compiled or run yet (no compiler on Neo).


## stxlib::algorithms, 2026-11-22 (the reciprocal-grid pow, half-turn trigonometry)

| Module | Property | Status | Evidence |
|---|---|---|---|
| `src/algorithms/math/pow.cpp` (pow(double), today's kernel: the reciprocal grid) | within 0.51 ulp for normal results | **budget proved for the model, correspondence argued**: `tools/math/pow_grid_certificate.py` (Python 3, exact rationals, Neo, 2026-11-22) bounds the logarithm per cell (relative 9.7026e-22, worst cell 1) and the exponential (3.5694e-19), composed 0.509424 ulp; the model follows `pow_positive` operation by operation, but that correspondence is read off the code, not machine checked. Sampled by the study before the reimplementation (0.501712 ulp over 2.45 million pairs against MPFR, M4); not yet measured on this code | `tools/math/pow_grid_certificate.py`, `tests/math_tests.cpp` (`pow_ulp_limit` 0.51) |
| `src/algorithms/math/pow_grid_table1024.hpp` | every cell's reduction is exact (0 < M n < 2^63, \|M n - 2^62\| <= 2^53 at both ends of the cell), heads on 2^-41 in [0, 1], tails zero or at least 2^-70, unity cells first and last | proved by compile-time evaluation (`pow_grid_table_valid`, a `static_assert` over all 1024 cells; the residual is linear in M, so the ends bound it) and by the generator's checks (`tools/math/gen_pow_grid_table.py --check`) | `src/algorithms/math/pow.cpp` |
| `src/algorithms/math/pow.cpp` (product forms) | the fused and Dekker forms give the same bits | argued in pow.cpp's head: every exact product's factors lie on grids that keep them zero or within [2^-788, 2^64) with exponent sums above -918 (the grid argument replaces the log2-table kernel's); tested by the bit identity and the Dekker targets once frozen (not yet run) | `tests/bit_identity.cpp`, `tests/math_tests.cpp` |
| `src/algorithms/math/half_turn.cpp` (the exact head) | anchor + slope d1 is exact for every cell and every reduced d | proved: the lattice facts by compile-time evaluation over all 256 cells (`half_turn_cells_valid`), the bit counts by deduction (`tools/math/half_turn_certificate.py`), which also checks the series coefficients are correctly rounded (both ends of the certified pi interval) and bounds the truncations (sin below 2^-75, cos below 2^-83 absolute; sinc below 2^-68 relative) | `tools/math/half_turn_certificate.py` |
| `src/algorithms/math/half_turn.cpp` (accuracy) | sinpi, cospi, sincpi, sinc_radians, cosm1 within 0.5005 ulp, cosm1pi within 0.502 ulp (the tests' limits) | **sampled, not proved**: the rounding of the low parts, the quotients and the squared half-angles has no certificate. The study measured 0.500020, 0.500113, 0.500295, 0.499992, 0.500100 and 0.501493 ulp for a kernel with these operations (172,288 inputs against MPFR at 256 bits); `half_turn_accuracy` samples four regions against wide.hpp's double-double series: at most 0.500102, 0.500063, 0.500129, 0.500001, 0.500154 and 0.500169 ulp (2026-11-22, 4 x 100,000 inputs, after the rotation's second-order terms took the whole cell values; sincpi had measured 0.500748) | `tests/math_completion_tests.cpp` |
| `src/algorithms/math/half_turn.cpp` (exact cases and reductions) | whole and half numbers exact with the stated signs; the reduction modulo 2 from 2^44 gives the bits of the reduced phase; the float functions are the double results rounded once | tested case by case and over 512 phases and 4,096 floats (not yet run) | `tests/math_completion_tests.cpp` (`half_turn_special_values`, `half_turn_reference_values`) |

Nothing on this branch has been compiled or run yet (no compiler on Neo).
