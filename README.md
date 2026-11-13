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
| clocks | `clocks.hpp` | batch 2a (survey D12 open) |
| entropy | `entropy.hpp` | batch 2a |
| exit tasks | `process.hpp` | batch 2a |
| raw terminal | `terminal.hpp` | batch 2a |
| process events | `events.hpp` | batch 2a |
| paths, files | `path.hpp`, `files.hpp` | batch 2b; batch 2b2 (D13 a: lossless names, durability levels, atomic replace) |
| desktop, child processes | `desktop.hpp`, `children.hpp` | batch 2b |
| threads, thread pool | | batch 2c (survey D11 open) |
| local time zone | | batch 2c (survey D12 open) |

Process-wide state of these modules (locks, the console's read-ahead, the
exit-task table, the event watch, the raw-terminal flag) lives in
`stxlib_state`, so a program built from several modules shares it.

Licence: MIT (see `LICENSE`). FreeBSD components keep their BSD licences.
