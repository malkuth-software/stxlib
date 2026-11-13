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
