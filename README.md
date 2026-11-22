# STXlib

The Standard Trustworthy eXtended library: the interfacing library that
stands between GUI.Forms and the outside world (host C and C++ libraries,
operating systems, kernels, and the client programs GUI.Forms serves), and
the replacement for stx_auxlib. Not a standard library; a boundary.

STXlib sideloads alongside the host C library (glibc, musl, FreeBSD's
libc, the UCRT); it does not run on it. Whatever the host does, STXlib's
behaviour inside is the same: it owns what it can, and calls the host C
library only for what it does not provide itself, while other code in the
process may use that library too (notes
`decisions/2026-11-15-boundary-sideloading.md`).

stxlib::boundary's first batch is written (vocabulary, error record, gates,
trampolines, ownership tags, the first interface services); the hosted
modules and stxlib::algorithms follow. The programme, the owner's decisions, the
verification programme and the trust chain are on the `stxlib` branch of
`malkuth-software/notes` (`planning/`, `decisions/`).

Planned layout:

| Path | What |
|---|---|
| `include/stxlib/` | owned vocabulary types, error records, capability declarations; no `std::` or platform type |
| `src/core/` | the foreign-interface discipline: exception shadowing, call gates, callback trampolines, ownership tags, validation |
| `src/state/` | `stxlib_state`, the one module that owns STXlib's per-thread and per-process state; every module's copy of STXlib reaches it through exported accessors |
| `src/hosted/<family>/` | interface modules, one per host family STXlib sideloads alongside: POSIX (macOS, Linux 4.11 or later with glibc or musl, FreeBSD), Windows (UCRT) and Emscripten (browsers and Node, over the POSIX modules that hold there); `c_library/` and `cxx_runtime/` hold the calls every family shares |
| `runtime/` | the own runtime over FreeBSD's system calls, for the appliance supervisor |
| `appliance/` | the FreeBSD appliance: loader changes, kernel configuration, the kernel shim, the supervisor, image tooling |
| `proofs/` | proof harnesses and the proof report |
| `trust/` | the trust-chain checker |
| `tests/` | conformance, equivalence and death tests |
| `tools/` | repository checks (`check_public_headers.py`; `check_std_rules.py`, the governance check over STXlib's sources, which consumers can run over theirs: `python3 -I tools/check_std_rules.py --list-rules`) |

## Release signing specification

The STX Release Seal v1 specification defines a CMS and X.509 file format, circular Ed25519 and ratcheted LMS signing, and protected-provider requirements. The fixtures cover both suites. [Whitepaper](docs/release-seal/stx-release-seal-v1.pdf) · [TeX source](docs/release-seal/stx-release-seal-v1.tex) · [Interoperability vectors](docs/release-seal/stx-release-seal-v1-vectors.json)

## Platform layer

stx_auxlib's hosted services, ported module by module from stx_auxlib
`b6e330a` (PROVENANCE.md). Each is a `Capability` (`capabilities.hpp`);
`capability_supported` answers per build and host, and a capability whose
module has not landed yet is reported unsupported.

| Module | Header | State |
|---|---|---|
| console | `console.hpp` | batch 2a |
| environment | `environment.hpp` | batch 2a |
| clocks | `clocks.hpp` | batch 2a; batch 3b (D12 a: IntervalTime and ElapsedTime, the interval and elapsed clocks) |
| entropy | `entropy.hpp` | batch 2a |
| exit tasks | `process.hpp` | batch 2a |
| raw terminal | `terminal.hpp` | batch 2a |
| process events | `events.hpp` | batch 2a |
| paths, files | `path.hpp`, `files.hpp` | batch 2b; batch 2b2 (D13 a: lossless names, durability levels, atomic replace) |
| desktop, child processes | `desktop.hpp`, `children.hpp` | batch 2b |
| threads, thread pool | `threads.hpp`, `thread_pool.hpp` | batch 2c; batch 3a (D11 b: 4 MiB stacks, guard pages, lock checks, predicate waits); the reduction rule (F5, fixed chunks) |
| time zones, local time zone | `local_time.hpp` | batch 2c; batch 3b (D12 a: IANA tzdata 2026f compiled in, the host names the zone; `tools/tzcompile.py`, `tools/TZDATA_POLICY.md`) |

Process-wide state of these modules (locks, the console's read-ahead, the
exit-task table, the event watch, the raw-terminal flag) lives in
`stxlib_state`, so a program built from several modules shares it.

### Emscripten

The Emscripten family (owner decision 2026-11-15, in 1.0 after M3; for
GUI.Forms' web host) builds with emsdk 6.0.13 through
`cmake/emscripten.cmake`, for wasm32 (owner decision 2026-11-15; no
`-m64`, as GUI.Forms' cp34 sets none). wasm32 is STXlib's one 32-bit
target; the pointer-width audit is in `proofs/REPORT.md`. Configure, build
and test (Node runs each test; the toolchain's platform file names it as
the emulator):

```sh
cmake -S . -B build-web -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
      -DSTXLIB_EMSDK=/path/to/emsdk
cmake --build build-web
ctest --test-dir build-web --output-on-failure
```

Options: `STXLIB_EMSCRIPTEN_FILESYSTEM` (`js`, MEMFS and IDBFS, the
default; or `wasmfs`, memory and OPFS), `STXLIB_EMSCRIPTEN_POOL_SIZE` (the
pthread pool, 8), `STXLIB_EMSCRIPTEN_EXIT_RUNTIME` (on in test builds, off
for pages). `stxlib_state` is static there; the allocator is stx_minmallocpp, as
elsewhere (`STXLIB_ALLOCATOR=host` is admitted, with the ownership tags on).
CI: a web job is added after the first green web run on the M4; it will
run the three commands above after installing emsdk 6.0.13 (`emsdk install
6.0.13 && emsdk activate 6.0.13`), once per file system. What a page provides is in
`capabilities.hpp`; web storage and its durability per volume in `web.hpp`.

## Algorithms

stxlib::algorithms (`stxlib::algorithms` in CMake) holds the host-free
algorithms, ported from stx_auxlib `b6e330a`: they use the boundary's
vocabulary and services and make no host call of their own.

| Module | Header | State |
|---|---|---|
| BLAKE2s-256 | `hash.hpp` | batch 3c |
| random (ChaCha12, xoshiro256++, distributions, shuffle) | `random.hpp` | batch 3c |
| SIMD level (detection, one selection per process) | `simd.hpp` | batch 3c |
| numeric text (locale-free formatting into caller storage, parsing, scanning) | `numeric_text.hpp` | batch 3c2 |
| calendar (civil arithmetic, weekdays, ISO 8601 text) | `calendar.hpp` | batch 3c2 |
| simple Unicode case folding (Unicode 18.0.0, C and S mappings; fold, measure, compare) | `case_folding.hpp` | M2 C7 |
| precedence chain (hash-only proof of recording order) | `precedence.hpp` | batch 3c3 |
| Preference Ranked Voting | `voting.hpp` | batch 3c4 |
| math (sincos, tan, inverse trigonometric, exp, logarithms, pow, hypot, cbrt, hyperbolic, half-turn sine and cosine, sinc, cosine losses; the same bits on every target; no subnormals in or out) | `math.hpp` | batch 3c5; subnormal rule batch 3c8; half-turn functions and the reciprocal-grid pow 2026-11-22 |
| row height index (stx_btree's sequence tree behind a compiled facade: offsets, rows at offsets, cursors) | `row_height_index.hpp` | batch 3c6 |
| ordered map (stx_btree's unsync map and pooled pages; private, no interface yet) | none (`src/algorithms/stx_btree/unsync_map.hpp`) | batch 3c7 |

Licence: MIT (see `LICENSE`). STXlib's own allocator, stx_minmallocpp, is a conversion of mimalloc's design and carries mimalloc's MIT notice (`src/state/stx_minmallocpp/NOTICE`). stxlib::markdown is a conversion of MD4C's design and carries MD4C's MIT notice (`src/algorithms/markdown/NOTICE`); its table of named character references comes from the HTML Standard (`third_party/whatwg-entities/LICENSE`). The CommonMark specification's text under `third_party/commonmark-0.31.2` is test data, CC-BY-SA 4.0, by John MacFarlane (`LICENSE` beside it). Third-party code keeps its own licence: stx_btree, a C++ form of bbtree, is MIT with its own notice (`src/algorithms/stx_btree/LICENSE`, `NOTICE`); FreeBSD components keep their BSD licences.
