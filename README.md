# stx_boundary

Working name. The interfacing library that stands between GUI.Forms and the
outside world: host C and C++ libraries, operating systems, kernels, and the
client programs GUI.Forms serves. Not a standard library; a boundary.

Nothing is implemented yet. The programme, the owner's decision survey, the
verification programme and the trust chain are on the `stx_boundary` branch
of `malkuth-software/notes` (`planning/`).

Planned layout:

| Path | What |
|---|---|
| `include/stx_boundary/` | owned vocabulary types, error records, capability declarations; no `std::` or platform type |
| `src/core/` | the foreign-interface discipline: call gates, callback trampolines, ownership tags, validation |
| `src/hosted/<family>/` | interface modules over a host: POSIX (glibc, musl, macOS, FreeBSD) and Windows (UCRT) |
| `src/freestanding/<environment>/` | interface modules with no host C library: the Genode environment first |
| `runtime/` | the freestanding C and C++ support subset, linked only in freestanding profiles |
| `proofs/` | proof harnesses and the proof report |
| `trust/` | the trust-chain checker: routine inventory, call-graph and system-call correspondence |
| `tests/` | conformance, equivalence and death tests |

Licence: MIT (see `LICENSE`), as for the other GUI.Forms helper libraries.
The freestanding Genode environment may carry its own terms; see decision D3.
