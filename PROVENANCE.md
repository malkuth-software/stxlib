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

### Batch 2b: paths, files, desktop and child processes, from `b6e330a`

As batch 2a. Two changes run through the batch: std::filesystem is no longer
used (it cannot report exhaustion in a build without exceptions), so the
operations stx_auxlib took from it are written over the host calls; and
owned objects that may cross modules (Path, File, Child, DirectoryListing)
keep their state inline or on STXlib's allocator, with process-wide parts
(write_file's serial, the abandoned children) in `stxlib_state` (layout 3).

| STXlib file | stx_auxlib file (at `b6e330a`) |
|---|---|
| `include/stxlib/path.hpp`, `src/core/path.cpp` | `include/stx_auxlib/paths.hpp`, `src/paths.cpp` (join and parent bound only their result) |
| `include/stxlib/files.hpp`, `src/core/files.cpp`, `src/hosted/file_backend.hpp` | `include/stx_auxlib/files.hpp`, `src/hosted/files.cpp`, `src/hosted/file_backend.hpp` |
| `src/hosted/posix/file_posix.cpp`, `src/hosted/windows/file_windows.cpp` | `src/hosted/file_posix.cpp`, `src/hosted/file_windows.cpp`, and the std::filesystem calls of `files.cpp` restated over host calls |
| `include/stxlib/children.hpp`, `src/core/children.cpp`, `src/hosted/children_common.hpp` | `include/stx_auxlib/children.hpp`, `src/hosted/children_common.cpp`, `children_common.hpp` |
| `src/hosted/posix/children_posix.cpp`, `src/hosted/windows/children_windows.cpp` | `src/hosted/children_posix.cpp`, `src/hosted/children_windows.cpp` |
| `include/stxlib/desktop.hpp`, `src/core/desktop.cpp`, `src/hosted/desktop_backend.hpp` | `include/stx_auxlib/desktop.hpp`; the target checks of `src/hosted/desktop_posix.cpp` and `desktop_windows.cpp` |
| `src/hosted/posix/desktop_posix.cpp`, `src/hosted/windows/desktop_windows.cpp` | `src/hosted/desktop_posix.cpp`, `src/hosted/desktop_windows.cpp` |
| `tests/path_tests.cpp`, `tests/files_tests.cpp` | `tests/paths_tests.cpp`, `tests/files_tests.cpp` |
| `tests/children_tests.cpp` | the children and desktop parts of `tests/capabilities_tests.cpp` |

### Batch 2b2: owner decision D13 a, on batch 2b

Not a port: stx_auxlib refuses host names that are not valid Unicode and
has one durability level (its current behaviour, D13's option b). This
batch replaces both as the owner decided (D13 option a,
`decisions/2026-11-13-d13-files-paths.md`): Path text becomes WTF-8
(`src/core/wtf8.hpp`, new), with POSIX bytes that are not UTF-8, ':' and
'\' carried as U+DC00 + byte (the scheme of Python's PEP 383, extended to
the two characters Path's syntax refuses); `Durability` names three levels;
`Capability::atomic_replace` reports whether the final rename is atomic.
Host facts relied on are cited where used (`files.hpp`, `path.hpp`,
`capabilities.cpp`, the backends).

### Batch 2c: threads, the thread pool and the local time zone, from `b6e330a`

As batch 2a. std::thread, std::mutex and std::condition_variable are no
longer used (in a build without exceptions they cannot report a failed
start), so each family module calls pthreads or Win32 directly; thread
identities, their issuer and the zone's load state live in `stxlib_state`
(layout 4). Survey D11 and D12 are open: the behaviour is stx_auxlib's
current one, flagged in `threads.hpp` and `local_time.hpp`.

| STXlib file | stx_auxlib file (at `b6e330a`) |
|---|---|
| `include/stxlib/threads.hpp`, `src/core/threads.cpp`, `src/hosted/thread_backend.hpp` | `include/stx_auxlib/threads.hpp`, `src/hosted/threads.cpp` |
| `src/hosted/posix/threads_posix.cpp`, `src/hosted/windows/threads_windows.cpp` | the host calls under `src/hosted/threads.cpp`'s std::thread, std::mutex and std::condition_variable, made directly |
| `include/stxlib/thread_pool.hpp`, `src/core/thread_pool.cpp` | `include/stx_auxlib/thread_pool.hpp`, `src/hosted/thread_pool.cpp` (the algorithm of `tests/reference/atomic_pool.c`) |
| `include/stxlib/local_time.hpp`, `src/core/local_time.cpp`, `src/core/civil.hpp`, `src/hosted/local_time_backend.hpp` | `CivilTime`, `LocalTime` and `local_time` of `include/stx_auxlib/calendar.hpp`; the civil conversions of `src/calendar_detail.hpp` (Hinnant's algorithms) |
| `src/hosted/posix/local_time_posix.cpp`, `src/hosted/windows/local_time_windows.cpp` | `src/hosted/calendar_posix.cpp`, `src/hosted/calendar_windows.cpp` |
| `tests/threads_tests.cpp` | `tests/threads_tests.cpp` |
| `tests/thread_pool_tests.cpp` | `tests/thread_pool_tests.cpp`, plus checks of the header's scheduling rules (the conformance test against the C reference comes with batch 3) |
| `tests/local_time_tests.cpp` | `local_time_rules` of `tests/calendar_tests.cpp` |

### Batch 3b: owner decision D12 a, on batch 3a

The local time zone no longer reads the host's zone rules: the two
`local_time_<host>.cpp` modules now find only the host's zone name, and
`local_time` reads the compiled IANA zones (`src/core/zones.cpp`, new);
`CivilTime`, `LocalTime` (now with the daylight flag and abbreviation) and
`local_time` keep stx_auxlib's shape. `src/core/civil.hpp` adds Hinnant's
`civil_from_days` (the same paper as `days_from_civil`). stx_auxlib's
`MonotonicTime` is renamed `IntervalTime` and read from each family's own
clock (`src/hosted/<family>/clocks_<family>.cpp`, new) instead of
std::chrono::steady_clock, beside the new `ElapsedTime`.

The remaining stx_auxlib content (hosted modules, owned algorithms, and
stx_btree with the sequence tree and RowHeightIndex) is copied from
`b6e330a` in later batches, each file with its source path, commit and
SHA-256 (decision M2, `decisions/2026-11-13-survey-round-4.md`).

### Batch 3c: stxlib::algorithms (first part) and the pool conformance test, from `b6e330a`

The first modules of the second target, stxlib::algorithms (decision
M1 = 1; A0 migration note 3.1): host-free code over the boundary's
vocabulary. Each port keeps stx_auxlib's algorithm and outputs and restates
its interface in STXlib's vocabulary (fixed C arrays, View, MutableView,
Result) in place of std::array, std::span, std::byte and std::expected; the
differences are stated in each file's header comment. The SIMD selection
word moves into `stxlib_state` (layout 7), so every module of a program
shares one choice. SHA-256 of each source file at `b6e330a`, read from a
`git archive` of the reference clone:

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `include/stxlib/hash.hpp` | `include/stx_auxlib/hash.hpp` | `1c11c55a13334b0dfd5b97f1ac14301765cad2e84b1d9bf12d3e6b21d8521a33` |
| `src/algorithms/hash.cpp` | `src/hash.cpp` | `129f35d7dd51afa53c656d4bdd13ee686420d081cf22c15fec3e06f8450464b1` |
| `include/stxlib/random.hpp` | `include/stx_auxlib/random.hpp` | `07cfb2c525d851ffb85e7770b31148342dd9ba629c4aaa044e67de3a019450bd` |
| `src/algorithms/random.cpp` | `src/random.cpp` | `dd11242f38fe2893d404b91a3857ac32f8ca33d9bba242d9c07332ad7b1be768` |
| `include/stxlib/simd.hpp` | `include/stx_auxlib/simd.hpp` | `5fddbe7b261342e4ef1c35c1441fdf50bae0407750936649f9ec8c30024169f0` |
| `src/algorithms/simd.cpp` | `src/simd.cpp` | `d735259f8249101aa17d7d9293df82ff7b12c3029844fce3abb3ba36b6f47a22` |
| `tests/hash_tests.cpp` | the BLAKE2s part of `tests/precedence_tests.cpp` | `a2740553d2df26cc414b63ce58e607652e69dee3d7fcefcf041336f8c98316f0` |
| `tests/random_tests.cpp` | `tests/random_tests.cpp` | `d3e7330a794592534db3e266e861216781c8cb972b3bf4b4b1d7228336cc64dd` |
| `tests/simd_tests.cpp` | `tests/simd_tests.cpp` | `a28a7708f918b4eea39ca311cb6305c046a9d93b8290e899846b54cdd6b4286a` |
| `tests/thread_pool_conformance.cpp` | `tests/thread_pool_conformance.cpp` (names, header comment and the modular marker's header only) | `7be497fe9eafe04e4d59785ec5e3143e7d0dfc328b7b8d93af0711fb825ec469` |
| `tests/reference/atomic_pool_adapter.cpp` | `tests/reference/atomic_pool_adapter.cpp` (rewritten for STXlib's Status and noexcept tasks) | `2c3beff9cd576f159763ae5b8d5660bf5addd4377560f5a70a590cfb01423ed3` |

Copied unchanged (the file's SHA-256 here equals the source's):

| File | SHA-256 |
|---|---|
| `tests/reference/random_reference.py` | `78764ba0feea222f7635e4aa72e51085f40bb87930af1da8b582cd30e08623b1` |
| `tests/reference/precedence_reference.py` | `cfb5c9604f61e51409278d0476bb99acae09f20dda32ae9f77c2d312eeb7fb2e` |

### Batch 3c2: numeric text and the calendar, from `b6e330a`

As batch 3c. Numeric text writes into storage the caller supplies (house
style section 6), whole or not at all, with the size needed reported
(`text::Formatted`, `TextWriter`, `NumberText`) in place of appending to a
std::string; the formats, digits and parse rules are unchanged. The
calendar keeps stx_auxlib's arithmetic, validity rules and ISO 8601 forms
over the boundary's CivilTime and `src/core/civil.hpp`; Result replaces
std::optional and WallTime replaces WallClock::time_point.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `include/stxlib/numeric_text.hpp` | `include/stx_auxlib/text.hpp` | `5356cee096afb9642d733039274c328b0baac0da13af89a77836f66a84801bc8` |
| `src/algorithms/numeric_text.cpp` | `src/text.cpp` | `01a107df5a7d2fd9726b041fad32ca2602a79d9e8d7f555e03000dc2090ae271` |
| `tests/numeric_text_tests.cpp` | `tests/text_tests.cpp` (without `--bench`) | `97ca940476cc3e530e832c146e4ce68b54c775887df03f78da9549fea51e1743` |
| `include/stxlib/calendar.hpp` | `include/stx_auxlib/calendar.hpp` (less CivilTime, LocalTime and local_time, which batch 2c moved to `local_time.hpp`) | `0c7d9e3535434b49407456503515390be55e976ecca7f261cafee0a341b13297` |
| `src/algorithms/calendar.cpp` | `src/calendar.cpp`, `src/calendar_detail.hpp` (`wall_from_seconds`) | `23bf4e3bae84f6a5f77a534223a49ec851135bb0276989ce2f64d571a37f0db2`, `f773b3b812414fe59d0d2c1271b7bebb7a63fe009e40b1762308437cb14d3aa0` |
| `tests/calendar_tests.cpp` | `tests/calendar_tests.cpp` (without the POSIX TZ rule checks; one compiled-zone check added) | `ee65283c36da731fadfa597722ffeffd1aed4254032380fe7acc19e59f923f9f` |

### Batch 3c3: the precedence chain, from `b6e330a`

As batch 3c. The construction and its bytes are unchanged; the records hold
fixed arrays with counts, the key trees live in Buffers allocated when the
chain is made, and the calls that allocate or need entropy return Result
(precedence.hpp, "Changes from stx_auxlib").

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `include/stxlib/precedence.hpp` | `include/stx_auxlib/precedence.hpp` | `b2a95b3c556c60997bf713f0016be35bc4834b797c44bb9fb0aaacd7a1cbe6ee` |
| `src/algorithms/precedence.cpp` | `src/precedence.cpp` | `2768e87c48741bc71464b7772dc4cd44bc139834057e332c7e99e7f5bef2a442` |
| `tests/precedence_tests.cpp` | the precedence part of `tests/precedence_tests.cpp` | `a2740553d2df26cc414b63ce58e607652e69dee3d7fcefcf041336f8c98316f0` |

### Batch 3c4: Preference Ranked Voting, from `b6e330a`

As batch 3c. The rule and its order of operations are stx_auxlib's; the
working storage moves off std::vector onto STXlib Buffers in one workspace
allocated at the start of a count (voting.hpp, "Storage"; voting.cpp's
header lists the two restructured computations). Ballots stores its
rankings in Buffers and reports a failed growth.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `include/stxlib/voting.hpp` | `include/stx_auxlib/voting.hpp` | `13f46296dc8aeae92485b0af25aeba16fdc9bd34ad1abbd4e18a0017a1409634` |
| `src/algorithms/voting.cpp` | `src/voting.cpp` | `ee1b79996755bd751c9b3a937ea58866b4c061c968b67a0bd91390e6bec345a2` |
| `tests/voting_tests.cpp` | `tests/voting_tests.cpp` | `03375548a39749ba11fa5d15b943739504a58ed247c8cf78977394e6d818e375` |

Copied unchanged (the file's SHA-256 here equals the source's). `prv-core.mjs`
is the owner's reference count, which stx_auxlib copied unchanged from the
owner's site (paymenottowork, ideas/preference-ranked-voting);
`prv_cases.txt` is the output of `node prv_reference.mjs`, regenerated on
2026-11-13 and byte for byte the same:

| File | SHA-256 |
|---|---|
| `tests/reference/prv-core.mjs` | `d57a163e5aefa721e3b09ff3b861ca69d6d75703ec7891c10f8b20b4e68e3166` |
| `tests/reference/prv_reference.mjs` | `249b29702a0942537a6110856ba212b2159d16c0fc1cec86c405984b74d0c8a9` |
| `tests/reference/prv_cases.txt` | `0be6f2625e0ef2b0c00586df02b072981373f7c9c57699868c597ada6fa115d6` |

### Batch 3c5: the owned math kernels, from `b6e330a`

As batch 3c. Every computation, constant, table entry and stated bound is
stx_auxlib's. Changed: the names (stx_auxlib to stxlib, STX_AUXLIB_ to
STXLIB_), the include paths (src/math/ to src/algorithms/math/, the private
`checked.hpp` to `core/checked.hpp`), the generators' paths (tools/ to
tools/math/, so the "Generated by" line of each table header and of
`sincos_worst_cases.hpp` names its new path, and `sincos_certificate.py`
finds the repository root one level further up), and comments naming
STXlib's own flags. `math.hpp` adds where its measured counts come from.
`tools/sincos_minimum_remainder.cpp` (the certificate's search) needs
stx_btree and arrives with it.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `include/stxlib/math.hpp` | `include/stx_auxlib/math.hpp` | `db302af5f82e83925bd90c8db28e475f0d73a9d53b60b384fa873d0e993bb854` |
| `src/algorithms/math/atan.cpp` | `src/math/atan.cpp` | `7b1204aa6dacef1d2dab661882986b207c358b1ed665b6cfd6a1a7f44bc556ca` |
| `src/algorithms/math/atan_table128.hpp` | `src/math/atan_table128.hpp` | `3457eafe5f78624cf29688655fad598291f88b5cb7c40a708f997cea9c17cd4c` |
| `src/algorithms/math/cbrt.cpp` | `src/math/cbrt.cpp` | `65fd20b69f7256fd31ed34fc0a8dab8f6a6913749bc1f387a587ca7691334d5d` |
| `src/algorithms/math/constants.cpp` | `src/math/constants.cpp` | `d4efbc75fdebadf14484a33bc434641b7b95bf5fcf17947348b9f836293cd038` |
| `src/algorithms/math/constants.hpp` | `src/math/constants.hpp` | `59ae223d1fc514c8caa23be40a2d7d56ebdfe9b85f1306c8f699e45b64d08732` |
| `src/algorithms/math/dispatch.cpp` | `src/math/dispatch.cpp` | `40451d687d2214e659928cd44b195821d65508ab4f97b32050aaedcdfe31b7ce` |
| `src/algorithms/math/exp.cpp` | `src/math/exp.cpp` | `e2fcdbbd1c5d1f0488e764c0efd41bd78db7ff4c966ac1f99e8476ae4dc90c6e` |
| `src/algorithms/math/exp_core.hpp` | `src/math/exp_core.hpp` | `07b752ea807190242453628ec411ade334ce5764e1ad78960d15e98a7e2490fb` |
| `src/algorithms/math/exp_table256.hpp` | `src/math/exp_table256.hpp` | `22be4e6fac4f1a8dab3c2413a6143e85e6930ac8b899491545984b5d4b60b708` |
| `src/algorithms/math/hyperbolic.cpp` | `src/math/hyperbolic.cpp` | `bd7f210e3559787feab4b76e72f684c051c176fdf6023ff31d18e5abda8ea479` |
| `src/algorithms/math/hypot.cpp` | `src/math/hypot.cpp` | `2771ea067f58d6608b9340a16e221705c73da9986d4c78f9c9ce0d98d34cc287` |
| `src/algorithms/math/kernel_support.hpp` | `src/math/kernel_support.hpp` | `76160bd560dbd8b65ad64f9133a9b8526c7ab4e034aab90bdf85eceac25f96ff` |
| `src/algorithms/math/log.cpp` | `src/math/log.cpp` | `df7be49c9caf5350daf80c556681abdf8123ce46c3ec1a842294c7c82f2c0a06` |
| `src/algorithms/math/log_core.hpp` | `src/math/log_core.hpp` | `3b2dd203951f6ff040780f06cff3286ea5be90a16912dace1ad179c2cd05d99f` |
| `src/algorithms/math/log_table128.hpp` | `src/math/log_table128.hpp` | `fa28a21e8b0da3de2f8c5ee47bd4e6ff2e70b22427a3d6a5c0425a013246c3d6` |
| `src/algorithms/math/pow.cpp` | `src/math/pow.cpp` | `1fa17dfb1d7c73032b32764e08606312993a67ab597ce8a416cf87322d061618` |
| `src/algorithms/math/sincos.cpp` | `src/math/sincos.cpp` | `ea2c4abe051a70c2e45651e1030a7e4ec6da12ecf1f83dfe9cbaaa10228720e2` |
| `src/algorithms/math/sincos_core.hpp` | `src/math/sincos_core.hpp` | `bc7aa59bb172309901260fdb44d0d986b13eb2b8403563cde59a7ee43deefca1` |
| `src/algorithms/math/sincos_table256.hpp` | `src/math/sincos_table256.hpp` | `d86c98aed66963989d857943a24d8174720384ebbd6a76c24290d3e666390569` |
| `src/algorithms/math/tan.cpp` | `src/math/tan.cpp` | `00af68ac2d6936e15e8bede70ea1e69396fa5fec34fdb42c3443b636fcb29c8f` |
| `src/algorithms/math/variant.hpp` | `src/math/variant.hpp` | `577fdee72902d2bb4a0ff82a7ae2bab386362b5d7ed81a8473303f18df5e01b3` |
| `src/algorithms/math/variant_functions.hpp` | `src/math/variant_functions.hpp` | `5749c71428fbd13eb8654dc87248fc2f34726aa5adff4111a16b4fb193c805a4` |
| `tests/math_tests.cpp` | `tests/math_tests.cpp` | `ff2ea6518839ee12e48407fbf18c8f393d64b78421a903280e56a60e6ac32a43` |
| `tests/reference/sincos_worst_cases.hpp` | `tests/reference/sincos_worst_cases.hpp` | `8d5b35be286b332092cce0aabae449dbd6209895b3681745324a05debfb57105` |
| `tools/math/gen_atan_table.cpp` | `tools/gen_atan_table.cpp` | `02e9bda67f97bbab506328601e52bb7dfb621abfc706dbee6e23be7de4647153` |
| `tools/math/gen_exp_table.cpp` | `tools/gen_exp_table.cpp` | `d6d71f063d881ab3a26f197e3081696bb4cbc038856d791b31b5930db9aebaa9` |
| `tools/math/gen_log_table.cpp` | `tools/gen_log_table.cpp` | `390a8d5a02f346293c5dd995ecc59711c6669d9010be8f14ddba8cd75394492b` |
| `tools/math/gen_sincos_table.cpp` | `tools/gen_sincos_table.cpp` | `c75d2870cb5696048bc2f34ed0d881bae2ffac24de4c8c0ec0834823db2b4b2c` |
| `tools/math/sincos_certificate.py` | `tools/sincos_certificate.py` | `f23a269a9095c103cc4829b5e6a59c39f047ef8bc9efa482a7ec6e316b742fc7` |

Copied unchanged (the file's SHA-256 here equals the source's):

| File | SHA-256 |
|---|---|
| `tests/reference/wide.hpp` | `23edbf50cf5bc23674aacfc0e82770fd2a7e9b5d229d601254f089be540bae1f` |

### Batch 3c6: stx_btree's sequence tree and RowHeightIndex, from `b6e330a`

stx_btree is the owner's C++ form of bbtree (Xiangpeng Hao, MIT OR
Apache-2.0; stx_btree takes MIT): `src/algorithms/stx_btree/LICENSE` and
`NOTICE` are its own, copied unchanged, and bind these files. Only the
sequence tree and its RowHeightIndex come in this batch, the part GUI.Forms
uses; the ordered maps, the optimal search trees and their tests follow
from batch 3c7.
The templates are private (coordinator's option 1, the compiled facade):
namespace `stxlib::stx_btree`, included only by STXlib's own sources and
tests. Changed at the adaptation points only, each listed in the file's
header: pages from memory::allocate (`MemoryPageAllocator` in place of
`GlobalPageAllocator`); allocation failure refuses the edit (false,
`out_of_memory()`) where stx_auxlib threw std::bad_alloc, so `erase` and
`coarsen` return bool, the constructor reports `constructed()`, and the
reservation arithmetic reports overflow by result; `STXLIB_REQUIRE` where
stx_auxlib called std::abort; `core/checked.hpp`'s modular marker;
`set_heights` also takes plain signed 64-bit units (a template over the
element). The tests change with them (the budget allocator returns null;
the failure check reads the refusal), and the exhaustive check also runs
through the facade (`FacadeTree`, new). `include/stxlib/row_height_index.hpp`,
`src/algorithms/row_height_index.cpp` and `tests/stx_btree/row_height_bench.cpp`
are new, written for STXlib.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/page_allocator.hpp` | `btree/include/stx_btree/page_allocator.hpp` | `7e2dcdb884beb74f7c99a17fe02304f1650b33e0efad333ae9ade1f45f008b70` |
| `src/algorithms/stx_btree/sequence_tree.hpp` | `btree/include/stx_btree/sequence_tree.hpp` | `8db3ce9d2d46c8975d67e8bfff6294ce6114584916ae7148abdf67536efd43e5` |
| `src/algorithms/stx_btree/row_height_index.hpp` | `btree/include/stx_btree/row_height_index.hpp` | `df27bd214a4df1d436c4b9be1ff5635b83b849a2edb5e7415bdd24a124f8cd50` |
| `tests/stx_btree/sequence_tree_test.cpp` | `btree/tests/sequence_tree_test.cpp` | `03cf1f191d7cd9cd107009e4b3b9ad213d7d1f7e4ee4dc627369b732608149cf` |
| `tests/stx_btree/sequence_tree_exhaustive_test.cpp` | `btree/tests/sequence_tree_exhaustive_test.cpp` | `54f7b5350eaced04539b064b7388990db0a11ed3f1d21fa8f0b24295725a3bb0` |
| `tests/stx_btree/btree_test_support.hpp` (TestFailure and SplitMix64 only; SplitMix64 marked modular) | `btree/tests/test_support.hpp` | `f5a8ae4d3704175d7e3b09e4913bc180a22e5da15b351f522e71a0da99ef7bab` |

Copied unchanged (the file's SHA-256 here equals the source's):

| File | stx_auxlib file | SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/LICENSE` | `btree/LICENSE` | `fde6789f4937c8c9741e00f823ef07d199269180950b3db03d2d806909a05a62` |
| `src/algorithms/stx_btree/NOTICE` | `btree/NOTICE` | `5a87d6e9a4a55880424b78c8518e4a45f3d2a171f200697f9f552cd593ec2381` |
| `src/algorithms/stx_btree/ORDER_STATISTIC_RESEARCH.md` (the study: design, invariants, proof sketches, measurements) | `btree/docs/ORDER_STATISTIC_RESEARCH.md` | `0bdc4c40dd0cc9fdb8cbab0eee37e6f06c1c0b95d5c2dd222514da4ebfc82f57` |

### Batch 3c7: stx_btree's unsync ordered map, from `b6e330a`

The first part of the rest of stx_btree (the coordinator's direction on the
owner's M1: STXlib takes all of stx_auxlib's content, private until a
consumer needs an interface). The unsync map comes first so that the
minimum-remainder search of `tools/math/sincos_certificate.py` builds on
STXlib (`stxlib_sincos_minimum_remainder`, with `STXLIB_BUILD_TOOLS=ON`).
Same licence, namespace and privacy as batch 3c6. The node and tree headers
are renamed only (include paths, namespace, `STXLIB_BTREE_` inlining
macros, `MemoryPageAllocator`) but for the four marked, whose changes are in
their headers: a page the allocator refuses makes the insertion refuse with
nothing changed and `out_of_memory()` true, where stx_auxlib threw
std::bad_alloc; the pooled allocator counts its sharers itself and takes
chunks from memory::allocate (no std::shared_ptr, no std::vector). The
latched tree comes because `detail/tree_algorithms.hpp` names its policy,
but carries a static assertion that stops any instantiation until the
shared-map batch adapts its page reservation (it still catches
std::bad_alloc there). `inline_key.hpp` keeps its std::string traits,
header only: STXlib's own sources instantiate none. The test runs
unchanged but for the shared pool, now shared by copying one allocator; it
also checks every insertion against `out_of_memory()` and adds refused
insertions (new). The search tool reports a refused insertion as its own
failure; its search is unchanged.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/detail/inlining.hpp` | `btree/include/stx_btree/detail/inlining.hpp` | `31f757a01c1299d5bd49606118758083e6f6cb4b88a2344b1352715567066494` |
| `src/algorithms/stx_btree/detail/prefetch.hpp` | `btree/include/stx_btree/detail/prefetch.hpp` | `8d1becaeb5cc29a0d2eaf2a499de1298c1bf131c012a16f93d612cfe4a849356` |
| `src/algorithms/stx_btree/detail/vector_count.hpp` | `btree/include/stx_btree/detail/vector_count.hpp` | `9e0fbc0e604492019b0e8d4f9cf10541b9b6ba2f16cca7a70ca1ef6d6ef8c16d` |
| `src/algorithms/stx_btree/detail/node_layout.hpp` | `btree/include/stx_btree/detail/node_layout.hpp` | `7966611592b8278c2f94e3d1d83b273a656c5949e7a1cdd2d127c1c9123cb59e` |
| `src/algorithms/stx_btree/detail/node.hpp` (the empty-leaf fault calls `requirement_failed`) | `btree/include/stx_btree/detail/node.hpp` | `59b9128c7b472c5d823171b689b52e6efef4cb41d52c083c9dbfac81bdc09433` |
| `src/algorithms/stx_btree/detail/node_stack.hpp` | `btree/include/stx_btree/detail/node_stack.hpp` | `ebdb097951d07597d12fbeb0971474f3d80d0113c805740e7763c156cbe69837` |
| `src/algorithms/stx_btree/detail/root_page.hpp` | `btree/include/stx_btree/detail/root_page.hpp` | `3fd3fcc81778acf81e9bc9fff1f2e488a035d1ef72ba0e94f5fd651ad76e6d32` |
| `src/algorithms/stx_btree/detail/tree_algorithms.hpp` | `btree/include/stx_btree/detail/tree_algorithms.hpp` | `9885b89fd8c757e66ba5785a8b0be7e6b50115eac806bb4c6920fee2f836e26a` |
| `src/algorithms/stx_btree/detail/tree_walk.hpp` | `btree/include/stx_btree/detail/tree_walk.hpp` | `a780c970259fb436fb1b5780d19cdd0074adad9a025b070cd015083ba05ccf16` |
| `src/algorithms/stx_btree/detail/latch.hpp` (spins yield through `stxlib::yield_thread`) | `btree/include/stx_btree/detail/latch.hpp` | `da1043072930651c4d28bb8aa9e13163ca3f88c74a558045052f814bd78a3943` |
| `src/algorithms/stx_btree/detail/latched_tree.hpp` (guarded: not instantiable until adapted) | `btree/include/stx_btree/detail/latched_tree.hpp` | `2598aeea11a5a5d6dea4b5ddbbc7000043584b0b42c506e568ccb0963d53e67e` |
| `src/algorithms/stx_btree/detail/batch.hpp` | `btree/include/stx_btree/detail/batch.hpp` | `6008ad6c7006fdb485af4a48fae28bac18ca8274f45e7bfd33de48e197bf3904` |
| `src/algorithms/stx_btree/detail/unsync_tree.hpp` (refuses an insertion it has no page for) | `btree/include/stx_btree/detail/unsync_tree.hpp` | `ab0621b94129a488853106030e4bdb486e388804fcbcb4be6d9441dd29cbcfa1` |
| `src/algorithms/stx_btree/inline_key.hpp` | `btree/include/stx_btree/inline_key.hpp` | `0a9cfca23e44f5e3884584791e215ab14518e0690d9096007e9ac93619d98e0a` |
| `src/algorithms/stx_btree/unsync_map.hpp` (`out_of_memory()`) | `btree/include/stx_btree/unsync_map.hpp` | `96a39f913bcf8c257c95e3d323c9cf2818ebf2c58d5a20462d876015ddd08a54` |
| `src/algorithms/stx_btree/pooled_page_allocator.hpp` (rewritten at the ownership and failure points) | `btree/include/stx_btree/pooled_page_allocator.hpp` | `b7730067f3c95cdde921078261e35151e09817ee7fdcdb095e0dc95408163e3c` |
| `tests/stx_btree/unsync_map_test.cpp` | `btree/tests/unsync_map_test.cpp` | `76a3f55e7a2f091c93455d392622178c3f7257d286cb25b913daca7b5a7f7a9f` |
| `tests/stx_btree/tree_invariants.hpp` | `btree/tests/tree_invariants.hpp` | `43a4940562842dd10ab6f67f6cdcc2a161c0410dffafd89c36134945936e1823` |
| `tests/stx_btree/btree_test_support.hpp` (now the whole file; modular marks on the key makers' mixing) | `btree/tests/test_support.hpp` | `f5a8ae4d3704175d7e3b09e4913bc180a22e5da15b351f522e71a0da99ef7bab` |
| `tools/math/sincos_minimum_remainder.cpp` | `tools/sincos_minimum_remainder.cpp` | `f593c2d7c102c9858f01b7ef70aa27a6ad9ff2c5f2303a527691cbc54b4d27a5` |

Still in stx_auxlib, for the next batch: the synchronized map and the
latched tree's adaptation, the optimal search trees, their weights and
packing, and their tests.

### Batch 3c10: stx_btree's shared ordered map, from `b6e330a`

The second part of the rest of stx_btree (the coordinator's direction on
the owner's M1). Same licence, namespace and privacy as batch 3c6.
`bbtree_map.hpp` is new here, and the latched tree carried in batch 3c7 is
adapted, so its static assertion goes. A page the allocator refuses makes
the insertion refuse with nothing changed (the split's pages are reserved
first, as before; the first root page is refused unless another thread's
root is published meanwhile). stx_auxlib threw std::bad_alloc instead. The
refusal comes back in the call's own result, because threads share the
map and a flag on it would race:
- the insert policies gain `refused()`;
- try_insert, update_or_insert and get_or_insert return `Insertion`
  (inserted, present, refused) where stx_auxlib returned bool or nothing;
- insert and insert_many return `Replacement` (outcome and old value)
  where stx_auxlib returned the optional old value.

The tests run as before, with each write's result checked against
refusal. `shared_refusals` is new: it covers refused first pages and
splits through try_insert, insert, get_or_insert and insert_many. The
ThreadSanitizer suppressions and the no-inlining rule are stx_auxlib's.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/bbtree_map.hpp` (adapted, as its header says) | `btree/include/stx_btree/bbtree_map.hpp` | `0c6185f9bff7bd6bf85ca53e2be5ace7f56b645991e026f934287fd8c2735ff3` |
| `src/algorithms/stx_btree/detail/latched_tree.hpp` (adapted in this batch, as its header says) | `btree/include/stx_btree/detail/latched_tree.hpp` | `2598aeea11a5a5d6dea4b5ddbbc7000043584b0b42c506e568ccb0963d53e67e` |
| `tests/stx_btree/bbtree_map_test.cpp` | `btree/tests/bbtree_map_test.cpp` | `e2be42021152b7fe1a3234a1a380bd0023382b839c4e247f967940c33d463e8b` |
| `tests/stx_btree/stress_test.cpp` | `btree/tests/stress_test.cpp` | `e601bdf4b892f2a918630e52b14958107c778e1f0408fe8e98c0ed85342d1e05` |
| `tests/stx_btree/tsan_btree_suppressions.txt` | `tests/tsan_btree_suppressions.txt` | `d455959551bf51d966d8cb359b1a196db274d767ce52507bcbb7d075c86bf1d8` |

### Batch 3c11: stx_btree's optimal binary search trees, from `b6e330a`

The third part of the rest of stx_btree. The coordinator chose option 1:
a full port onto STXlib storage with Result builders. Same licence,
namespace and privacy as batch 3c6. Changes:
- `std::vector` becomes `detail::Array`, a new movable growable array on
  memory::allocate that reports failure;
- the weights become borrowed Views;
- every construction returns `Result<SearchTree>`: invalid_argument where
  stx_auxlib threw std::invalid_argument, out_of_memory where std::vector
  would have thrown std::bad_alloc. Nothing fails fast (survey D8 is open);
- the shape checks (`is_search_tree`, `cost`) return Results, and
  `from_parents` returns false on exhaustion;
- the two assertions become STXLIB_REQUIRE.

The algorithms, their order of operations and their results are
stx_auxlib's. The test's references and instances are unchanged; it keeps
its weights in std::vector and passes views.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/detail/array.hpp` (new, STXlib's own) | none | none |
| `src/algorithms/stx_btree/search_weights.hpp` (adapted, as its header says) | `btree/include/stx_btree/search_weights.hpp` | `06845d9f9ad2359b3f2ea17aeaaa6d36b33356e0e75a70b700af4122e5625dc4` |
| `src/algorithms/stx_btree/optimal_tree.hpp` (adapted, as its header says) | `btree/include/stx_btree/optimal_tree.hpp` | `fa58184ab4873f6586b50e559f5bdcf949ecee0f7c7800ec5621733ceca8d068` |
| `tests/stx_btree/optimal_tree_test.cpp` | `btree/tests/optimal_tree_test.cpp` | `35c28e3e98a36ac90898244d6c2cd57e1d0a55fbc5b37e227d86eb668038104e` |

### Batch 3c12: stx_btree's tree packing and static optimal map, from `b6e330a`

The fourth part of the rest of stx_btree, on batch 3c11's storage and
Results (coordinator's option 1). Same licence, namespace and privacy as
batch 3c6. Changes:
- `std::vector` becomes `detail::Array` in the packing and the map's nodes;
  the map's entries live in one block from memory::allocate holding
  constructed entries (`detail::OwnedEntries`), so Key and Value must move
  without throwing;
- `TreePacker::pack` and `ColdSubtreeLayout::relayout` return false on
  exhaustion, with the output passed by reference;
- `OptimalMap::build` returns `Result<OptimalMap>`: invalid_argument where
  stx_auxlib threw std::invalid_argument, the tree construction's own
  failure passed on, out_of_memory where std::vector would have thrown
  std::bad_alloc. Nothing fails fast (survey D8 is open);
- build takes the keys and values as caller storage (MutableView) and
  moves them only after every allocation has succeeded, so a refused or
  failed build leaves them unchanged; stx_auxlib took std::vector by value;
- find_many and node_order_entries use View and MutableView in place of
  std::span;
- the assertions become STXLIB_REQUIRE.

The packing, the layout, the node format and the searches are stx_auxlib's.
The test's references and instances are unchanged; each build moves from a
fresh copy of the keys and values, and the rejection check also checks that
a refused build leaves std::string keys untouched. `docs/OPTIMAL_TREES.md`
is not carried.

| STXlib file | stx_auxlib file (at `b6e330a`) | Source SHA-256 |
|---|---|---|
| `src/algorithms/stx_btree/detail/tree_packing.hpp` (adapted, as its header says) | `btree/include/stx_btree/detail/tree_packing.hpp` | `472b4f323235324a99e71bf2510f24153c7ab1684b995d5625e597c25a067582` |
| `src/algorithms/stx_btree/optimal_map.hpp` (adapted, as its header says) | `btree/include/stx_btree/optimal_map.hpp` | `e0d43916c27e4623aa7a1a3ecc2cf15e55913f578e3a602565b1ed4f6a224140` |
| `tests/stx_btree/optimal_map_test.cpp` | `btree/tests/optimal_map_test.cpp` | `1bcc43d7e6a0be86b4cbccf3d814d9511fe6fdd8c7a5d034e967d079e029b033` |

### Batch 3c8: no subnormals in the math (STXlib's own change)

Owner decision 2026-11-13 (notes `decisions/2026-11-13-locals-float-mutability.md`,
"Subnormals"): STXlib's numeric code neither accepts nor produces
subnormal values. Nothing is ported in this batch. The kernels, tables and
constants of batch 3c5 are unchanged; so are the functions of
`dispatch.cpp`, which now sit in namespace `stxlib::math::unflushed`.
`src/algorithms/math/flush.cpp` is new: it defines the public functions of
`math.hpp`, reading every subnormal argument as zero of its sign and
returning a subnormal result as zero of its sign. The flush works on the
bits; the floating-point environment is never set. `math.hpp` states the
rule in its shared contract and lists each function's subnormal outcomes.
In `tests/math_tests.cpp`:
- the checks carried from stx_auxlib call the unflushed functions, so their
  frozen hashes and measurements still describe stx_auxlib's bits;
- in the subnormal regions (exp's, pow's and hypot's subnormal results,
  log's subnormal arguments) they verify the flush instead of measuring ulp;
- `flush_rule()` and `public_special_values()` are new. They check the
  public functions against the unflushed ones bit for bit.

### Batch 3c9: no subnormals in text and random (STXlib's own change)

Owner decision D1 = 1 (2026-11-13, notes
`decisions/2026-11-13-locals-float-mutability.md`). Nothing is ported in
this batch.
- `src/core/subnormal.hpp` (new) holds the flush on the bits.
  `src/algorithms/math/flush.cpp` (batch 3c8) now uses it; its behaviour
  is unchanged.
- In `src/algorithms/numeric_text.cpp`:
  - parsing returns zero of the text's sign for every real below the
    smallest normal;
  - hosts that report underflow as out_of_range are recognised from the
    text's magnitude (`below_one`), so out_of_range now means overflow
    only, whereas stx_auxlib reported underflow below the subnormal range
    as out_of_range;
  - formatting reads a subnormal as zero of its sign.
- In `src/algorithms/random.cpp`, `scale_into` flushes its bounds and its
  result.
- The contracts in `numeric_text.hpp` and `random.hpp` state the rule. The
  tests change with it.

### Batch float_exact: the exact floating-point primitives (STXlib's own)

Owner decisions F3 = 1 and F4 = 1 (2026-11-13). `src/core/float_exact.hpp`
is new and private: two_sum, fast_two_sum, split, two_product (fused and
Dekker forms, the same bits), Shewchuk's expansions, Kahan and Neumaier
sums, and orient2d and incircle in float and double. It is written for
STXlib from the literature the header cites:
- Knuth (two_sum);
- Dekker (fast_two_sum, the split and the product);
- Shewchuk 1997 (the expansion algorithms, and the stage-A error bounds of
  orient2d and incircle);
- Kahan and Neumaier (compensated summation).

No source code is copied. Shewchuk's `predicates.c` was not downloaded,
and his intermediate adaptive stages are not taken.

The double forms of two_sum, fast_two_sum, split and two_product are
stx_auxlib's math kernels' operations, moved from
`src/algorithms/math/kernel_support.hpp`; its forms now forward to the raw
forms, so the math's bits are unchanged. `src/core/float_discipline.hpp`
(the compile-time guards for rules 1 and 5) and
`proofs/esbmc/float_exact_float.cpp` are new.

### Batch reduction order: fixed-order reductions and the bit identity (STXlib's own)

Owner decisions F1 = 1 and F5 = 1 (2026-11-13). New and STXlib's own:
`src/core/float_reduce.hpp` (the F5 shape, the fixed 4096-term chunks, the
pool forms), `tests/float_reduce_tests.cpp`, `tests/bit_identity.cpp`,
`tests/bit_identity_expected.txt` (every item unfrozen) and
`tools/compare_bit_identity.py`. Changed: `include/stxlib/thread_pool.hpp`
(the reduction rule in the contract), `CMakeLists.txt` (the floating-point
options on every target, the configure-time flag check, the new tests),
`.github/workflows/ci.yml` (each job prints its bit-identity lines). No
code is copied from anywhere; no STXlib result changes (the math, text and
random code is untouched; the new options were already on every target
that computes in floating point).

### pow(double): the kernel's y range, and the first freeze (STXlib's own change, 2026-11-13)

The bit identity's first M4 and CI runs (main `5aaedf7`) found
math.pow.double differing between the fused and Dekker product forms.
`src/algorithms/math/pow.cpp` passed every finite y to its kernel, and for
x = 1 with |y| > 2^996 the Dekker split of y overflowed: the Dekker form
returned NaN, the fused one 1. Now the kernel takes only
2^-80 <= |y| < 2^64, where every exact product is in range for both forms
(pow.cpp's head says why); larger |y| is decided as infinite y is, and
smaller |y| gives 1 (NaN for negative x). Results change only in the
Dekker form (and on hosts without FMA), for x = 1 or -1 with
|y| > 2^996, from NaN to 1; the fused form's results are unchanged.
`tests/math_tests.cpp` gains `pow_kernel_y_range()`.
`tests/bit_identity_expected.txt` is frozen from the M4's library-build
output of `5aaedf7` (`integration/dev/logs/bit-m4-plain-1113-130811.log`,
via `compare_bit_identity.py --freeze`), on the expectation that the fix
leaves the fused form's results unchanged. The integrator's freeze from the
fixed build (`bit_identity_freeze.sh`) must reproduce the file exactly.

### Lossless child arguments and the path bound (STXlib's own change, 2026-11-13)

Two follow-ups of D13 a (`decisions/2026-11-13-d13-files-paths.md`: no
real file is unreachable because of its name). `ChildRequest`'s program and
arguments become `PathText` (`include/stxlib/children.hpp`): a Path's text
is passed as is, and `src/hosted/posix/children_posix.cpp` writes each
POSIX escape (U+DC80..U+DCFF, U+DC3A, U+DC5C) as its byte and refuses any
other surrogate (invalid_argument, HostCode::invalid_value);
`src/hosted/windows/children_windows.cpp` writes the text's UTF-16, lone
surrogates included. The POSIX conversion is deliberately not
`wtf8::unescape_posix`: that one's canonical check refuses a raw ':' or
'\', which plain argument text holds. `reveal_in_file_manager`
(`src/core/desktop.cpp`, `src/hosted/*/desktop_*.cpp`,
`include/stxlib/desktop.hpp`) no longer refuses a path that is not valid
Unicode (stx_auxlib had no such paths). `maximum_path_bytes`
(`include/stxlib/path.hpp`) rises from stx_auxlib's 4,096 to 98,304 (3 x
32,768), so that a Windows path of 32,767 units and a fully escaped POSIX
path at PATH_MAX are not too_long. Tests: `tests/children_tests.cpp`
(`lossless_arguments`, the `hex` child mode), `tests/path_tests.cpp`
(`longest_host_names`).

### The A2 swap-in: GUI.Forms' frames, release frames, the reporter contract (STXlib's own change, 2026-11-13)

For GUI.Forms' swap-in onto STXlib's shadowing (notes
`planning/stages/A2-swap-in.md`, which lists the API and GUI.Forms'
contract items it answers). `include/stxlib/shadow.hpp`: frames become
`ShadowFrameRecord` (120 bytes: kind, flags, two u64 numbers, sequence, a
48-byte subject and a 32-byte name, copied at the push and cut at a whole
UTF-8 sequence), pushed from a `GateFrame` (now defined here, with kind,
texts and sequence) or a `ShadowFrameText`; `enter_shadow_frame` and
`leave_shadow_frame` (GUI.Forms' ShadowFrameCall); `read_shadow_stack`
with a dropped counter. Frames, the throw-site capture, the records'
ShadowReference and park_shadow/restore_shadow work in every build; the
native trace, the exception type and the recent ring stay
development-only (S5 = 1; the coordinator accepted GUI.Forms' request,
since release builds of GUI.Forms record frames today).
`include/stxlib/gate.hpp`: every gate takes a GateFrame;
`invoke_callback_classified`; the classifier's once-per-exception
guarantee, which is the message sink (A2-2 = 1). `include/stxlib/fail_fast.hpp`
and `src/core/fail_fast.cpp`: the reporter contract, checked in
development builds; the report writes frames with their kinds and texts,
and leaves out an empty trace. `src/state/state.hpp`: layout 8. Tests in
`tests/shadow_tests.cpp`, `tests/two_module_tests.cpp` (with
`two_module_library.*`), `tests/gate_no_exceptions_tests.cpp`,
`tests/gate_tests.cpp`, `tests/death_tests.cpp` and their CMake cases.

## Third party

### mimalloc v3.6.1 (ported forward from v3.5.3, 2026-11-15)

`third_party/mimalloc/` is mimalloc v3.6.1, <https://github.com/microsoft/mimalloc>,
MIT (`third_party/mimalloc/LICENSE`, Copyright (c) 2018-2025 Microsoft
Corporation, Daan Leijen; the same blob as v3.5.3's). Upstream source: tag
`v3.6.1` (tag object `07d6126055867a3c6c8977eb0066be27c0671054`, tagged
2026-10-30, unsigned) naming commit
`56dc5eb80145f9a3ba7ca23d47c6a248b9d8586c` (unsigned), released 2026-10-30.
The owner approved the download on 2026-11-15 (the mimalloc thread; notes
`decisions/2026-11-15-mimalloc.md`). Downloaded on 2026-11-15 by the
mimalloc seat (`smi`): the release's source archive
`mimalloc-v3.6.1-source.tar.gz`, 1,579,209 bytes, SHA-256
`7321fbe8e3d2bb447cba127603c44450db0d8ef35e8240bb4a56a7455eba0a68`, from
<https://github.com/microsoft/mimalloc/releases/download/v3.6.1/mimalloc-v3.6.1-source.tar.gz>
(kept in the seat's `smi/dev/download/`). All 447 files of the archive were
compared with the Git tree of tag `v3.6.1` as GitHub's API lists it
(`git/trees/v3.6.1?recursive=1`): each file's blob SHA-1, computed with
`git hash-object --no-filters`, equals the tree's, and no file is missing
or extra. Kept, as for v3.5.3: `LICENSE` and the `.c` and `.h` files under
`include/` and `src/`, without the override headers
(`include/mimalloc-new-delete.h`, `include/mimalloc-override.h`), the
`readme.md` files and the ETW manifest and profile; 75 files, copied
unmodified (STXlib's patches are applied on top: "STXlib's patch series"
below). Compiled into `stxlib_state` as its one translation unit,
`src/static.c`, with stx_auxlib's definitions (CMakeLists.txt), plus
`MI_MINGW_UCRT64` on MinGW (batch 3b) and `MI_PROFILE=0` (CMakeLists.txt
gives the reasons).

Why v3.6.1 replaces v3.5.3 (the seat's comparison,
`smi/dev/mimalloc-audit.md`): v3.5.3's C++ new-handler path had an
unguarded `try`/`catch` (`src/alloc.c` 792-797) that does not compile under
`-fno-exceptions`; v3.6.1 has no `try` or `catch` anywhere (upstream commit
`f2207e345`, "revise no_throw to not call a new_handler"), and its one
`throw` keeps the `_CPPUNWIND || __cpp_exceptions` guard
(`src/alloc/alloc.c` 814-816). v3.6.0 and v3.6.1 also bring upstream's
fixes for huge-allocation rollback, a heap-destroy segfault, the heap
visitor, fork and the Windows bcrypt dependency, and always use the
two-level page map.

v3.5.3's provenance, for the record: copied unmodified from stx_auxlib at
`b6e330a`, tag `v3.5.3` (tag object
`deac3dcf5f0614884013d411a5c07e16bec96c9f`) naming commit
`d4881d338125e1cb7c47ba4cfb398d6f7c0c8d45`, 44 files; its table is in this
file's history before this change.

| File (under `third_party/mimalloc/`) | Git blob SHA-1 | SHA-256 |
|---|---|---|
| `LICENSE` | `53315ebee557ac22f3396ea1781df0440c26bb15` | `82ade5d7d9b029044b5fbbc326207fc47c2d44ee4dd71e8a559c36d728217de9` |
| `include/mimalloc-profile.h` | `eea3a946afb984cd42d646b1b7b637205b8f0f1c` | `a41f2cdfc265439ee8024a8c782d0f69a1988944f14cca9f0f5bac997d227dc2` |
| `include/mimalloc-stats.h` | `85198984aed127e388a52e3e6eebf7b8607c639f` | `a00649c2be6a14a2a5890c92af0fac61b457bb4b59f4926c27472d65eb05734d` |
| `include/mimalloc.h` | `fbd1c256adbf8da6aba268b52016ab6b5c8ab54a` | `cb0ea5563aad14b2e367f69bcd50220c08dc193b561763e66eb474bcc7f48c2b` |
| `include/mimalloc/alloc/alloc.h` | `314c5150dc8b25c5e097cc8531ad31ed17518868` | `129002d3195dc10d05136d74fd6823db713c8ee5efa897648d3180627153adec` |
| `include/mimalloc/alloc/free.h` | `2f06ea861ee658c387db69c2f95cce42f2e7afc6` | `01e5f545769f058a4a857bb58ed46ca99205b0c10614f26b5bd2e5a77a7fa8a2` |
| `include/mimalloc/alloc/page.h` | `0c0dde4039b64fc0141467d265796ec5221e4dd4` | `a00e502f7b09c568a44838df982f479f444edca37082e959e1962c74838133e1` |
| `include/mimalloc/arena/arena-alloc.h` | `710f29cc902a11f4264a9bed0064cdd3ca9f08b9` | `bd7f43bb037310142f2a941bf8a5668c50971e69d2ba0e41119786874500dd42` |
| `include/mimalloc/arena/arena-page.h` | `40715566b4653c2fa4c68be0cdfa2d56d773eba3` | `8ef812d720d2dcd4cfd8282340e26de4671270a04066d1ab05675fe480c6b000` |
| `include/mimalloc/arena/arena.h` | `18d486a2eb42d1bf352227b596dd5602d2b0e602` | `0079609653ba8ea5418677374e103862c1d3d06674e8bef53f2d23f02f4666da` |
| `include/mimalloc/atomic.h` | `f3ed906988fa0bb4364903fd72a71f0fc7803d1d` | `0523fa50a0fae17840cb600f3e1159970f6972d3da0dd2f94f06249bd48226a1` |
| `include/mimalloc/bits.h` | `68202ba3a6ea3097c80f1d948d33f0b7ad957afb` | `21ff4c1341cc51eef6ba1df8ac1221fc1a82bbac9274b7ce40602c1e4cfcc526` |
| `include/mimalloc/decl.h` | `35fb70ab1e74b54efdbd43c1c5dbbb858f4398d5` | `bc50fc01eeb8755fd0f461b23cfd6b9020e4ed986060fddbc2cf079a371ee587` |
| `include/mimalloc/heap/heap.h` | `db756b4ec47fa8821c7dd1b9d20562767fef0d94` | `d9b53bbaeae61d1ec79df8d390d05c31925434dfa80d9673819025f7998aeb93` |
| `include/mimalloc/heap/page-map.h` | `d9a10723869beaefeff1cd9a07baac53c4ee2671` | `984b69b504f76a75cc90a49c1482b6ef638793d8f1354fcfa9a7ce701f4e7705` |
| `include/mimalloc/heap/process.h` | `e15bdd00f2c738f9616aa653cd5db74c81ca7a0f` | `52f75f65ec9676394ea764d9fe71379c062d65f242f9b06d0918aa1a18d391a0` |
| `include/mimalloc/heap/subproc.h` | `9f0b5a4a2f639b5a4aa04163419e273b55c724f1` | `fe9fca1bd8cc5019ce6b2eca82564bfb23c955b4b80329c9089641154beca204` |
| `include/mimalloc/heap/theap.h` | `3ba7464db3b71b6c3982082c4bcc4cc395e4cf94` | `6ae793742306dfee5dc9c11710c123ec42d6ade86ab50d2230f82b32e594b4ba` |
| `include/mimalloc/internal.h` | `a2834db40681e68ba7fc383bffbb69ec8a154c72` | `f28e02ff6d670cf0e21682ae15a07bbf7f4aead55ddae11ad8ea48cf16c0debf` |
| `include/mimalloc/prim.h` | `f82c886ccbaea2e8cc2b6ca35078c34ecc10337a` | `14120dfaa214a2128a150470c0198709dd63f95299c9454f5bc5d5c16642231f` |
| `include/mimalloc/prim/os.h` | `f9013b744459cefaa71c765b682c37bb21650460` | `8ede6bd63a210fcd1ce235f07ec71213d9a9e85a224f83be5b7ff7a0cec5b5d8` |
| `include/mimalloc/prim/prim-tls.h` | `4c8dcd6d6f7ab0620437d0cc1a575e2c5ba36f9c` | `82d667c44e968a8e4776ec0a0a7fb34dbd847bda828a140f0ceed4fc3ee12cf5` |
| `include/mimalloc/profile/pprof.h` | `c2c641401e7a10ede8dc0012f50fb1507019a6c7` | `e050f8fe3160713de82ae5072b2de9c42e6ad3a7c775a867dfb71e857ab38697` |
| `include/mimalloc/profile/sample-guarded.h` | `4e8cd06272bfbfdda7b3489188975f768f2e52ee` | `ef60e444b37a5b768a35dd4d727b5465048e8c61c78b82fab8518ac664a0826d` |
| `include/mimalloc/profile/sample-profile.h` | `645089d0ae5434c0c5d0c0a4c3e41ad806079a2d` | `737f1c96b4341e6f3f3b46fd828c916aedc65a8c44f59e02bfce48cb99c1b9b9` |
| `include/mimalloc/qsbr.h` | `fcce0fb21bbd2444627ef1b90fa58db921f53b74` | `1892e3b3c30c674f9034d2ebefeda62a4833af743e96b3f2a9a66103335d69cf` |
| `include/mimalloc/track.h` | `f2a24aacfe617cbc7eb8ecc7a5833a4e16dbb85c` | `ec65a353e9ac472f94c59f8c540dd93cb109f9b6c687019bcd6a4fc6a64716e6` |
| `include/mimalloc/types.h` | `933e580f831e336600a59086c32b7322e83388ac` | `103f4d85fbe11a4f634ad690282f042925b6d239ffc1a01e6c03a93f98cf521f` |
| `include/mimalloc/util/libc.h` | `a062d1100f900a68ede67937998d90ca5a5fcb7e` | `a3babaaa1adaab50c80580cc657acd0159ef9c19f225483b500fbc21f074d8c6` |
| `include/mimalloc/util/options.h` | `99873f1ea17e2772352ea3c354379230acc58b67` | `3855f7769a3f62eafc4a6e49d4afb0916a7f28ab18cd1bd037bb3c1d4cc1517c` |
| `include/mimalloc/util/random.h` | `e1720c129e25f76eca6bc472b175a457b863dc5c` | `8798a2ac34108fd3a6a8f47dd32142edf1612ae43ce5ef36882433f4fbf39325` |
| `include/mimalloc/util/stats.h` | `b46979d9c74453e484112c183785b7391ae4f569` | `fe30e9b5576dd25882f7069715761ab52f2328eb8cda279c062b8443b0e1191a` |
| `include/mimalloc/util/threadlocal.h` | `f0207cdb5e6e5c4282cc20d0858afa871296a362` | `00466baf02fb5c0e3a47a151d0da88e07b3b05cba02f6fa7761ae5b0fb24eb14` |
| `src/alloc/alloc-aligned.c` | `2511a81752e001ae7ea964213dca4a2c6d8cba33` | `a1562cf78fe2c20540dca5cfa30dbcfdedd2b6cdd8212250c6e7b20a367f97b9` |
| `src/alloc/alloc-generic.c` | `230dacf90931ac624948455b28e0edb3e9df30cd` | `d341f4bb96ff29cd184def8857aa9e2871886ed053ade8644a00ec4dfa3c7446` |
| `src/alloc/alloc-override.c` | `a490c031cd08012fcf32594137dd8435333864c4` | `2611871128c92da4bc67249c81e790746342205989a42ae764e8c51e544b810f` |
| `src/alloc/alloc-posix.c` | `0b43df8cb0ce643add87056fdaaaf47bfb083239` | `89d63cda8a7bc9e184f9a7ec0038c26bfa305508b2d000dd641613f663fc0276` |
| `src/alloc/alloc.c` | `1be26dc8666793d974faf7a5927d3337759702cd` | `427f087fc1916ac4480e6d7fed29543190205f3e0e7fb2647329830a67465b6c` |
| `src/alloc/free.c` | `77cfa7e21a7906b3cd6b1c2e620565144a704ed5` | `b12d05bfe71b144c56d3b8e88f094d1a6c87113dc052f4c40919466c4ed3c881` |
| `src/alloc/page-queue.c` | `986a48e7e10af37dfb846844bb8b4a5bcde09d3a` | `0422962929c81072ae7e2c1e10f5d1f23dbbd97958650450c097806f484a07c9` |
| `src/alloc/page-queue.h` | `906c8cd41632e4cdb910c224a778eec60dd92f1b` | `c05ab20a8e7f1148a2fe909da075aaf8897e799f41f08f7bc60557dab0bc3e75` |
| `src/alloc/page.c` | `90809d278255292216b9ac43293fc1d657ef864f` | `ad39fc9ee6626f90aed4623e627a13fb604312c4bf6dfa8369a62083fe2c6764` |
| `src/alloc/page.h` | `ae2af1760b0f06ad6cad08639d6a0620840a9c87` | `001b23551a0fd80b7f60e722a62a30af6db4739f67d64f3a076ea90a602128f0` |
| `src/arena/arena-alloc.c` | `004eedcec03bc9004ed02fa976b9bc02f29133f6` | `6c01cd55604b43efbd42f0be6e73621c0b1b6240ed4cf979cae698d60c71d860` |
| `src/arena/arena-page.c` | `c8d397c254b85f9746962d11d09483352c50b427` | `cac200e735cc7573f1a9cdb9a82ebf6082b4f84b4948945ff59d899187e1121e` |
| `src/arena/arena.c` | `16234037fb5284ab3bcb35b52f50f231d2eeac43` | `b3c87c6152e79441727ef7d896d2239a569d5d7e68b30ee444644035132b15f0` |
| `src/arena/arena.h` | `b9957db244a1f63c1bb2b4a5c824b9b026f06637` | `50fa09ad82969e1300d129e2abe4fffc89a2b592ecddf585cd5afb82a04f0430` |
| `src/arena/bitmap-chunk.c` | `32122ff1ab8719725e2ff4eb77abc5c9db31233f` | `79aeef7bd19217115600a031e0b427d8d510e701168bd880e47b782f97945833` |
| `src/arena/bitmap-chunk.h` | `c5b3c4b206696d85ffe4afb8b015e0a1cf52b47a` | `c6b479b2c2ca1aa668a93f891a96de1c2ec103b8e2195344960fada29a4a7774` |
| `src/arena/bitmap.c` | `800d349b81df82291c1210ea20b2b545f7c2aca8` | `bf1540128d2b2760bd8fbe0e03519860ca6be2e3280e5c71501ab7914e583b43` |
| `src/arena/bitmap.h` | `3fe0ede55a8b7320efb1676525b3ef281a58adc4` | `88e9a16f6b063022c27dd0e2f3962786b8e21cead9be6cf21e0eb6cb8eede68a` |
| `src/heap/heap.c` | `5d1c59bb6a81a5dd1571b9e98fc839d101a9424c` | `2efdbb2752a173fc721937e912c4015d56979531acb7e5cf3dc4b2e7923f9e3c` |
| `src/heap/page-map.c` | `a0692b9d2bce27a75b673a4962ba7044a1e8e9de` | `926de8908d0eaed37aac63a2eb112286f3663983c3ec9013ce743567cb000715` |
| `src/heap/process.c` | `2658ebd89fc5217e72d6622371d446c1fe9565a6` | `a522b4c6d6f20875ca77682c9e56d154d6b6b0a76899e3c918589be3f5ec73ea` |
| `src/heap/subproc.c` | `3b8edb59ea9a896de6fb5ec04b3592e32eb09ad2` | `4d07960070eacc2228bf67ac945ef6183dd2afc63b3bbbbc86fa31fb7ce85b0a` |
| `src/heap/theap.c` | `d52e89438394aa92df2e21228c458d10c5e945ce` | `27125073a8b90f3885f05b96a3d7be831d3230d5e9833282aabb5a5c05e0689d` |
| `src/prim/emscripten/prim.c` | `d3dda4c638f90fa143f6e5b8aa43de33a6706619` | `bf695234747299d53b0682c11bd9671eb9ddbb78c5a9c508909a9e0011bd47bf` |
| `src/prim/os.c` | `fdb3f0732c03b639b722cbb46b50c86c692b7b50` | `e89d94492d09f6abc75ba7a0e86cd2ce9dd32579e8daef69876bc5cb0edd4b55` |
| `src/prim/osx/alloc-override-zone.c` | `6602efa64024ba54db917d062648e7ae5edb95d7` | `7f38bfa132d6d3dddce2a4067a78e2e73d8e943e87279e1dba1e58ce2942013b` |
| `src/prim/osx/prim.c` | `8a2f4e8aa473165a1e61188012a7ca5a8ca1aea5` | `247a9952465eb105be03a9962e922b085ce5d52034775551a61cd8594275be73` |
| `src/prim/prim-tls.c` | `3cb1e17cb875838306e16a5235afb5cb794697e1` | `545e5cc7e44f13172c90f49bbe08991c9c1d551dd09b8985f698ad626b159c0d` |
| `src/prim/prim.c` | `1fc1a3a036923fd4935cfc54e1332af181940ec6` | `02af571ed6f9954e21a3d246b349004a4ee77ac5aa09ef86b9156e435fc49d81` |
| `src/prim/unix/prim.c` | `d55d87fb3853ec560afaeed226997da7be10fbbe` | `e54792f8a7fd6adb9ad461aa38e5f12ffc567e89fcaa7238464715edeffa36cf` |
| `src/prim/wasi/prim.c` | `f46ae2d2bb77627714c999215d1d8b14a400b51b` | `f80e7cf341c7af9f48e0888fc23332080ab705f1d7a0c38ed6c8f7aee2db5395` |
| `src/prim/windows/etw.h` | `4e0a092a10f4ba96facb51781296dc32c8ff2f84` | `4f3110ef2054c95cb275be96cb279224d3d46a728c5117bd1435425f382de778` |
| `src/prim/windows/prim.c` | `2ff305ee6540a13624a4af5c10ffc7b97aaf3a93` | `25418509761abd536d0ae8c6a6b3107274d442a858d64538a54a400564af8eca` |
| `src/profile/pprof.c` | `97f16874e1a6fccd33112de46e3bc087152c2a5c` | `ef2c4a0392e8f916a1221486281ac32097fd751d5ecee017e3368a0ecad920c5` |
| `src/profile/sample-guarded.c` | `e59b54d794bc6d73a2f34e186f463515938c6613` | `72370a9b1d0e60a52af387850e3fef2756841ab5c47dfff80be8d9bd00679c7d` |
| `src/profile/sample-profile.c` | `4291493f4bd0753276911f0b4d9f12a6239c4575` | `77044c5c8d26f93efb793e735689ef23b93cbcb90e13eff13eb8f3a3bf62fce9` |
| `src/static.c` | `5d1bed62bcc714df17b512a5bcd52ff18d32ac48` | `044129b62c76a14255931db2962dd19d37154ac396388ca321b6e38468562074` |
| `src/util/libc.c` | `5242ab18cbda177831026b7b51659826818df972` | `51250c13e69a077ebd1ecadb0ddb5331c3b12aa08ed017afcaeebd82de32aac1` |
| `src/util/options.c` | `2db2487688ca296902885b99b16a490685b21d58` | `ce278a4bdc3da50295615fd4e09969b21f31ac8c9785810f2763a84086b2f0ce` |
| `src/util/random.c` | `9e49a00d9dc7487ae77c815f4e4f1f40b2c1aff6` | `d434f4cb4898c925d888aca6f784c5968efa657906c8b4c652b3288b6cbfb09e` |
| `src/util/stats.c` | `33d9a173b80d2c810681f9756428fb3f51e3950a` | `3ec4fdef7a154c1c0511b2f45a12b100e95807547f7cf3dc2a462187840e1070` |
| `src/util/threadlocal.c` | `208fab4e4db45aaf55fff173da148aebfb7ad297` | `4975ca0f00c185b422eff35af31108f18f02f18d24f723c30ed0993322ddebe4` |

#### STXlib's patch series

STXlib keeps its own patched copy of mimalloc (the owner, 2026-11-15; notes
`decisions/2026-11-15-mimalloc-audit-answers.md`; AGENTS.md). The table
above is the pinned release, v3.6.1, file by file. The tree is that release
with the patches of `third_party/mimalloc-patches/` applied in the order of
its `series` (its `README.md` gives the form of a patch). The patch table
gives each patch's SHA-256, the defect it fixes (audit ID) and its upstream
status. The patched-file table gives the SHA-256, in the tree, of every file
a patch changes; every other file still has the release table's hashes.
`tools/check_mimalloc_patches.py` (ctest `stxlib_mimalloc_patches`) checks
all of this, and that reverse-applying the series gives back the release
exactly.

| Patch (under `third_party/mimalloc-patches/`) | SHA-256 | Fixes | Upstream |
|---|---|---|---|
| `0001-freebsd-overcommit-bits.patch` | `743986078953941e6be00e9c4f20e6fda3caed990ff818bcf185104754cf7d4d` | B4 | to report |
| `0002-unix-yield-no-cancellation-point.patch` | `d9fb7687cd92ff9614842a9ebda06771d20db62440b87997f96b7982dcc053d4` | E1 | to report |
| `0003-page-map-full-span.patch` | `ad516574471c083fc81a9147ac44294f0a608f9c80c5388a6503369214aabdfc` | S1, A2 | to report (clamp); count is 221e58feb, unreleased |
| `0004-no-implicit-reinit-after-thread-done.patch` | `f9c7c50cfcb079a9e9b3b32ee2d4d8f33571fa75c6899b4ea9909d533cc3e55c` | A4, C2 | backport of 66b6fd4c2, unreleased |

| Patched file (under `third_party/mimalloc/`) | SHA-256 |
|---|---|
| `include/mimalloc/heap/process.h` | `05f5135d92f8a7ec9741b3dd968bc61b2c104627e4a64335002f4ecde7783d95` |
| `src/alloc/alloc-generic.c` | `5b2954a8b7cde7e20835d6b941ff37ba5c426eead9a7f061255f6201180533cb` |
| `src/heap/heap.c` | `ac54337200785292547c979ef55a541bb784926a3a33b980c66f7fbaa9d08f76` |
| `src/heap/page-map.c` | `0bc401eda64d17cb94f589da3b95c76edeb5d10296f9d0745f1b95061783b7f8` |
| `src/heap/process.c` | `0b1263336522094c68172bcb12613cacb33a10087c8ea9075478585ad81e23fb` |
| `src/prim/unix/prim.c` | `fefa6af0b9209775dd5de6a39f26f89eafd079f5553f9f11a41ace949b32d33d` |

### threadpool_atomic_fast, the pool's C reference (batch 3c)

`src/core/thread_pool.cpp` ports, through stx_auxlib, the owner's
`threadpool_atomic_fast` (MIT, Joshuah Rainstar;
`tests/reference/LICENSE-threadpool_atomic_fast.txt`). Its C original is
kept unchanged as the conformance test's reference, copied from stx_auxlib
`b6e330a` `tests/reference/`; each SHA-256 below equals both the copied
file's and the one stx_auxlib's PROVENANCE.md records. GUI.Forms took the
files from `falseywinchnet/backend`
(`orchestrator/third_party/threadpool_atomic_fast`, revision
`6eb51f84260984b4b293156ed7fefb057267bce5`) and renamed the symbols to the
`gui_forms_atomic_pool_` prefix; the reference keeps those names.

| File | SHA-256 |
|---|---|
| `tests/reference/atomic_pool.c` | `b1282195e93177305d7cfd7f1bdd14586ee325fe02c186128cc0a5b5def5d91d` |
| `tests/reference/atomic_pool.h` | `0dc5257334c940cf6080e26321cbb995fb562031f2d81aec92ac8b0892190fc4` |
| `tests/reference/LICENSE-threadpool_atomic_fast.txt` | `7ffa68e2ff116c26c79f75985354b28cb59148efacaff60ceaa5da8795333ad1` |

### IANA tzdata 2026f (batch 3b)

`third_party/tzdata/` holds the files of IANA's time zone database release
2026f that STXlib compiles (owner decision D12 = a): the ten data files zic
reads by default (tzdata's Makefile: `africa antarctica asia australasia
europe northamerica southamerica etcetera factory backward`; `backzone` and
the leap-second files are not used), `version` and `LICENSE`. Public
domain (`LICENSE`: "all files in the tz code and data ... are in the
public domain", the BSD exceptions naming code files not copied here).
The archive `tzdata2026f.tar.gz` (483,310 bytes, SHA-256
`01b4a91cec5167ed48c11e8ab78f2048cead78ce94b0cafcad0ae8c5a3044d3d`) and its
signature `tzdata2026f.tar.gz.asc` were downloaded by the coordinator from
IANA with the owner's approval on 2026-11-13, into the workspace's
read-only `research/tzdata-2026f/`; the signature has not been checked (no
gpg on the machine). Copied unmodified on 2026-11-13: each file's SHA-256
below equals that of the same member extracted from the archive
(`tar -xOzf`, compared with `shasum -a 256`).

| File (under `third_party/tzdata/`) | SHA-256 |
|---|---|
| `LICENSE` | `0613408568889f5739e5ae252b722a2659c02002839ad970a63dc5e9174b27cf` |
| `africa` | `f2851d4be4a4925cbdc9d56e10d780bccadb89d6ffb9aed78c3e35f97c200aed` |
| `antarctica` | `e410ad71c9450828c592d21419301d41ac79ce50159fd0ac2d6c5031cb6bdfe6` |
| `asia` | `02b69728085f6b0c999da70de2f1f9a2d8952b533a450d53bb41e03eb7d0e92e` |
| `australasia` | `846ba455578e3e0f9eb850f05be3cc06d02ff51bbe4b726f755be9dc765f16c9` |
| `backward` | `b8296b9ef1ece72d813878132c0f17e2c52b26af6004eeb9c37d049b64cd9401` |
| `etcetera` | `7281f095b42c13c4ae36b8bcba884e81dbb38127221fc1d9805c4dbf852487db` |
| `europe` | `fec11bf6c6bd0619a8777bbdaa2ecd184fde291cb4b97efa87d334c7145ba3c2` |
| `factory` | `ae2ec1d36dabf79a69cb7dd4fb6fd9168d05fc8cfd31aee2dd19e4f18beb9885` |
| `northamerica` | `da7366f6e0862ed15cb787dd056dca3a255df2ec50d19ff0a9f54eeb1562083a` |
| `southamerica` | `df4f2ae71a6980a76038f51bfc4cdaaca9af5ba8799df0b5bc73283db68c8cd6` |
| `version` | `9f9a9e6bfd18cd6a0a4d65764931072577edcbb731981a068ef52bba6615b1de` |

Generated from it (first party, not third-party code):
`src/zones/zone_data.inc` by `tools/tzcompile.py` and
`tests/zone_vectors.inc` by `tools/tzvectors.py`, both written for STXlib
in batch 3b from the tzdata source format as zic(8) documents it; no zic
or tzcode source is copied. Taking a later release: `tools/TZDATA_POLICY.md`.
