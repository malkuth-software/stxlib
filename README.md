# STXlib

The Standard Trustworthy eXtended library: the interfacing library that
stands between GUI.Forms and the outside world (host C and C++ libraries,
operating systems, kernels, and the client programs GUI.Forms serves), and
the replacement for stx_auxlib. Not a standard library; a boundary.

Nothing is implemented yet. The programme, the owner's decisions, the
verification programme and the trust chain are on the `stxlib` branch of
`malkuth-software/notes` (`planning/`, `decisions/`).

Planned layout:

| Path | What |
|---|---|
| `include/stxlib/` | owned vocabulary types, error records, capability declarations; no `std::` or platform type |
| `src/core/` | the foreign-interface discipline: exception shadowing, call gates, callback trampolines, ownership tags, validation |
| `src/hosted/<family>/` | interface modules: POSIX (macOS, glibc, musl, FreeBSD) and Windows (UCRT) |
| `runtime/` | the own runtime over FreeBSD's system calls, for the appliance supervisor |
| `appliance/` | the FreeBSD appliance: loader changes, kernel configuration, the kernel shim, the supervisor, image tooling |
| `proofs/` | proof harnesses and the proof report |
| `trust/` | the trust-chain checker |
| `tests/` | conformance, equivalence and death tests |

Licence: MIT (see `LICENSE`). FreeBSD components keep their BSD licences.
