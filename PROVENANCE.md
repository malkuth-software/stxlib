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

### mimalloc v3.5.3 (batch 3a)

`third_party/mimalloc/` is mimalloc v3.5.3, <https://github.com/microsoft/mimalloc>,
MIT (`third_party/mimalloc/LICENSE`, Copyright (c) 2018-2025 Microsoft
Corporation, Daan Leijen), copied unmodified from stx_auxlib at `b6e330a`
(`third_party/mimalloc/`, the same 44 files). stx_auxlib's PROVENANCE.md at
`b6e330a` records the upstream source: tag `v3.5.3` (tag object
`deac3dcf5f0614884013d411a5c07e16bec96c9f`) naming commit
`d4881d338125e1cb7c47ba4cfb398d6f7c0c8d45`, released 2026-09-17, cloned from
GitHub on 2026-10-31; kept are `LICENSE`, `include/` and `src/` without the
build, test, binding and override files, and each kept file's Git blob
SHA-1 equals the upstream tree at that commit (stx_auxlib checked it with
`git hash-object` on 2026-10-31). Not downloaded again here: each copied
file's blob SHA-1 was recomputed with `git hash-object` on 2026-11-13 and
equals stx_auxlib's table for all 44 files, and its SHA-256 is recorded
below (decision M2). Compiled into `stxlib_state` as its one translation
unit, `src/static.c`, with stx_auxlib's definitions (CMakeLists.txt), plus
`MI_MINGW_UCRT64` on MinGW, so that mimalloc initialises once there (batch
3b; CMakeLists.txt gives the reason).

| File (under `third_party/mimalloc/`) | Git blob SHA-1 | SHA-256 |
|---|---|---|
| `LICENSE` | `53315ebee557ac22f3396ea1781df0440c26bb15` | `82ade5d7d9b029044b5fbbc326207fc47c2d44ee4dd71e8a559c36d728217de9` |
| `include/mimalloc-profile.h` | `5a1327ff70d725291b837495b92c739427b53972` | `c0aabbf2fbe54ce55e7dee23cb1891c2d65e0e1f5b18d9fe1a4e733c0c91d996` |
| `include/mimalloc-stats.h` | `ab45a0715f70b149b3efa2aabb39e0925bb36cb5` | `57aaf0b0db23521a32b0c6d7e0969e37ce69466fcfb0cb9b2d0114c2b56be182` |
| `include/mimalloc.h` | `2985edd9492a503d2e28005178f9b5023959c2c9` | `8ec1bff9902bab6682e3ecbf7613ccc915e082ca743fdd0c396d3954323fe8bf` |
| `include/mimalloc/atomic.h` | `9c23398024ab74432a58b34bc2be68cc5fe1c9c8` | `7be611200328a99b052eda81abb8ad78b7e03446a0f6d302c1eb31cc9f096398` |
| `include/mimalloc/bits.h` | `5aa0d83755f6c9bfbca00febddf23776f7cf5926` | `1ecfb1bd1a7b6f73c335d21f3411c9ce0de3cbf6710e2d209e6cb6df2baf1ed3` |
| `include/mimalloc/internal.h` | `b79649c72800521a6d4b52ff300de170b4719214` | `26c10f7b990c51629618c74c64e3263a1fe00da787c2e72c56a79f87393f602c` |
| `include/mimalloc/prim-tls.h` | `da78d0193dff9743b2329bfc2b7ece8195f2a044` | `a701ed0c182b4d0039bf683b4ac197011b8b794971a616c76ca64f036bc30d8b` |
| `include/mimalloc/prim.h` | `82b2d9c3d242e6b1dcaaae85a47b22a0c189fa32` | `1987e8e2eedc07bb181bf2a11a27bec80a5309c32cfa66a56900fb4cbb64b172` |
| `include/mimalloc/track.h` | `e6a57c8ec4894667f955cafd10287ff27dc12ba2` | `31d94e981640bc4c340f21887610a9cd7242af4c5adc578f722ad03cb852db2e` |
| `include/mimalloc/types.h` | `530c464a368768866191e59e5dedba80582d802a` | `01abbaf1cc5572675b97f4115f205335e319b93249ee5163fb3bf2ad0fbbceae` |
| `src/alloc-aligned.c` | `51f29732cbd85c67641604620dbfc6dc5501705a` | `8390ff8d0e6c2954db1d95bfc1a4388b302605a1c5102c0495e5163838186e7f` |
| `src/alloc-override.c` | `bcb96b52d7dd675717fab11d800e5a503ce86610` | `6576949f30978f3b7544d9696af12dac9615074a5c2fc05e3681475575bc6ca3` |
| `src/alloc-posix.c` | `a2252494255af0f8883fdcbe3885e664c77b516b` | `a6dcdef4694964c972e6cf37f197f93c95f321566a3b5ccc1761129257255e93` |
| `src/alloc.c` | `a176db77f41a667ed57cfe5155e733d6f887e00f` | `e074a8a622f6317d42eb43e48db8cde3345696c96d6749523ef4fd58aeb55bb7` |
| `src/arena.c` | `1b7c8cb336d0cef7bfe614456983130effe6fe42` | `27fce7003a5108fb9d0c61e92e0cd7870f687f7e3cf12ad19c7fb3954b2a0bc2` |
| `src/bitmap.c` | `238c4d4c4c12a3f3d5918ae992779af3be6a19fa` | `b33f7b220358667fb0eddcd6569ec7b79f7ef61daf08448c8448c8d1a8741b09` |
| `src/bitmap.h` | `79eda77fad6ab85e26eeb5faefd7b029eabee75a` | `f3a9375ec0b56d515bea4b23732bf9a04964efecc516eb8aaac8b58ebf4512d9` |
| `src/free.c` | `722ee7bf6933ae6596c6ee05a92a3e05b320f285` | `56a1c57bd08c49359e19e8cdcd2a11b50c30bb12a53063375374fb3f5c8d6189` |
| `src/heap.c` | `d7f1db13211254dc955b7371b00f76436c226a3b` | `a33d339568e5a7d16d6bae5650a79b1aef649e9b4ad0261be982580c2971eb64` |
| `src/init.c` | `9b0ded5b547d30a35df2f7efc23e70b1f4f8c9ed` | `1452ca684eca6d959eec9eae91750162f7738055f995bd4863f79b87aab88a8c` |
| `src/libc.c` | `8f211230d0cd3a8486d6f71a704ecfb1130b8831` | `b774d5750409006b690ab6262f037d78362ca978729314493636b358249e7fbd` |
| `src/options.c` | `eaa2119def5fc2dd575904d2470e3f6d023788e3` | `5bfe80a0c473371d299840e8ed20831601d4998e4164e2e0bf71303697395824` |
| `src/os.c` | `15111cf94988cfe61ea0428273d96c722a4890a4` | `06b2d6d8a4cf3bfcc1822bb09aeaa4e7b741e7a5241f70b4f693b8038f455b00` |
| `src/page-map.c` | `949e37ac3afabfabb0923717df94a09510f5ac90` | `ff3509ae3d4185e9cb2a95e1f35daba3329e998bac917ac7938f1bf06dba0f78` |
| `src/page-queue.c` | `c8e9b5425ce5adff2998fc63ac828b8189e39bce` | `4128ca8b8d4df1116f73bf9a397903ca9941701c635345b1241295c3a31ff47d` |
| `src/page.c` | `aa669cdd02928218ab0e9863428d7db36b7dd76f` | `ad0df1217e96669ef63539c345ea7dc4d637c3269be2d4de36ac3a5a42fb0c12` |
| `src/prim/emscripten/prim.c` | `992965d56f23bf2bc2fc41aa1daa994335486a76` | `896fc1016a26e10c6cf96259d0524c57e123ac9dcef1ba04f23d0add445286dd` |
| `src/prim/osx/alloc-override-zone.c` | `48f97958218d9e3343249d438da3a4b2c01f0357` | `f1180623115b729fa96e7e36d04cb29f9157dad889f22b46afda540a957e8ac8` |
| `src/prim/osx/prim.c` | `8a2f4e8aa473165a1e61188012a7ca5a8ca1aea5` | `247a9952465eb105be03a9962e922b085ce5d52034775551a61cd8594275be73` |
| `src/prim/prim-tls.c` | `1b79321a05ef00cc23b27033b23de0814143fc1f` | `884443f66808143d645dc6254c38e27f91b540a667ea23fe0ba1ef1caaf2b114` |
| `src/prim/prim.c` | `3d3d0c2e8c881bf27a181366b39526e2ee353411` | `44419a450e742812d8f3c881d957bdd6cf18f40b90dfe1153084d2b64666f12d` |
| `src/prim/unix/prim.c` | `25c2adea039f862665f15541221c58cfe8ae0d57` | `9399a96ceff699f35dad56b9a1785348b1a9743dc369ada488bdca6b6854aee4` |
| `src/prim/wasi/prim.c` | `2d69b7245fb1a5fea8ff7b41ca5b46f5a36efe1a` | `2bc862cddb522314d58b3c6ef041e101c25cf04dfdda35cf78f91dbb6140cf46` |
| `src/prim/windows/etw.h` | `4e0a092a10f4ba96facb51781296dc32c8ff2f84` | `4f3110ef2054c95cb275be96cb279224d3d46a728c5117bd1435425f382de778` |
| `src/prim/windows/prim.c` | `222fa6303e496d70c8c99a271aa20fec7c4a1212` | `41d96c081cbda6bcad0e8270eab7c3d69f94dfe3d3dcd8a9567569c595984cb0` |
| `src/random.c` | `3c012adf45c4abe4221add5e61d0652c7797869c` | `2bd12844f9de0392fdb00ea70f5ad15649ffd8e49f9a4d2388e15d6469a7b703` |
| `src/sample-guarded.c` | `f7c1fe81e99ba7ee610a8e54f2afcea853ae6d93` | `4edc7700c2dd911cd03919d1098e973d887900ab08a5f41fd9648a79517fca57` |
| `src/sample-profile.c` | `f1b5db4913e3e7c09b9b50467e8b64340fba2276` | `3df74fee931244a302293f7d215dc87e3b5c120ab7df207edd92d0ff19eb3729` |
| `src/static.c` | `8a4f8c3d54052876e1c6f04e0c1c2b5b0c62445d` | `fadfb75216098793f9f81e57d6ff3b63914084976526ed7867c9ca212825d784` |
| `src/stats.c` | `c763bfc3c7b5b7c38fd4b8518705cb8778c6c920` | `d78e0467008946b40d5356315a9ba76e04354ead65b1aafabbb7bd091c3826a6` |
| `src/subproc.c` | `f42d4a11c5208b9620e456a412c0cdafa9dec2c9` | `41c147e8eb6e960687b79a201dd4498fb80f58923a0a4d73e04e80ef315b8eb6` |
| `src/theap.c` | `1731830a7aa416b07bc82554779a622ef318830d` | `91271bc76502c743d109f32201c02271f63a9f6ec51ef207f8b533f6de39df27` |
| `src/threadlocal.c` | `6f280d09353c918b28ec414ae5ab10e745797229` | `01dbaacd5ac6685dc59c40d82a3f8932e5290d4d05f9a441424e4ea53e1ba973` |

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

### Unicode 18.0.0 CaseFolding.txt (M2 C7)

`third_party/unicode-18.0.0/CaseFolding.txt` is the Unicode Character
Database's case folding file, version 18.0.0 (its header: "Date:
2026-02-03, 23:09:45 GMT"), from
<https://www.unicode.org/Public/18.0.0/ucd/CaseFolding.txt>. Downloaded by
the coordinator with the owner's approval on 2026-11-15 (notes
decisions/2026-11-15-roadmap-choices.md, choice 9) into the workspace's
read-only `research/unicode-18.0.0/`, and copied here unmodified on
2026-11-15: 88,776 bytes, SHA-256 equal to the download's.

Licence: the Unicode License v3 (SPDX `Unicode-3.0`;
<https://www.unicode.org/license.txt>). It asks that its copyright and
permission notice appear with all copies of the Data Files, so
`third_party/unicode-18.0.0/LICENSE` holds that notice. No separate
download was made for it: it is lines 1 to 40 of the `LICENSE` of the ICU
79.1 package already installed on the workspace's laptop (Homebrew's
`icu4c@79/79.1/LICENSE`, whose first section is the Unicode License v3,
"Copyright © 1991-2026 Unicode, Inc."), the notice alone, without ICU's
third-party sections.

| File (under `third_party/unicode-18.0.0/`) | SHA-256 |
|---|---|
| `CaseFolding.txt` | `a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d` |
| `LICENSE` | `d9a52140f486a859a52aad5a4574d11358028b24e5df81ea3f9a20a6dbcf8583` |

Generated from it (first party): `src/algorithms/case_folding_table.inc` by
`tools/gen_case_folding.py`, which refuses an input with another hash and
keeps the 1,501 C and 32 S mappings (simple case folding; F and T are not
used). The CTest check `stxlib_case_folding_table` regenerates the table
and compares. Taking a later Unicode version: replace the file, change the
pin in the generator (hash and counts), here, and the version constants in
`include/stxlib/case_folding.hpp`, then regenerate.
