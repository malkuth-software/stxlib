# STXlib

The Standard Trustworthy eXtended library: the interfacing library that
stands between GUI.Forms and the outside world (host C and C++ libraries,
operating systems, kernels, and the client programs GUI.Forms serves), and
the replacement for stx_auxlib. Not a standard library; a boundary.

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
| `src/hosted/<family>/` | interface modules: POSIX (macOS, glibc, musl, FreeBSD) and Windows (UCRT); `c_library/` and `cxx_runtime/` hold the calls every family shares |
| `runtime/` | the own runtime over FreeBSD's system calls, for the appliance supervisor |
| `appliance/` | the FreeBSD appliance: loader changes, kernel configuration, the kernel shim, the supervisor, image tooling |
| `proofs/` | proof harnesses and the proof report |
| `trust/` | the trust-chain checker |
| `tests/` | conformance, equivalence and death tests |
| `tools/` | repository checks (`check_public_headers.py`) |

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
| threads, thread pool | `threads.hpp`, `thread_pool.hpp` | batch 2c; batch 3a (D11 b: 4 MiB stacks, guard pages, lock checks, predicate waits) |
| time zones, local time zone | `local_time.hpp` | batch 2c; batch 3b (D12 a: IANA tzdata 2026f compiled in, the host names the zone; `tools/tzcompile.py`, `tools/TZDATA_POLICY.md`) |

Process-wide state of these modules (locks, the console's read-ahead, the
exit-task table, the event watch, the raw-terminal flag) lives in
`stxlib_state`, so a program built from several modules shares it.

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
| precedence chain (hash-only proof of recording order) | `precedence.hpp` | batch 3c3 |
| Preference Ranked Voting | `voting.hpp` | batch 3c4 |
| math (sincos, tan, inverse trigonometric, exp, logarithms, pow, hypot, cbrt, hyperbolic; the same bits on every target) | `math.hpp` | batch 3c5 |

Licence: MIT (see `LICENSE`). Third-party code keeps its own licence: mimalloc is MIT (`third_party/mimalloc/LICENSE`); FreeBSD components keep their BSD licences.
