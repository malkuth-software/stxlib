# B1 harnesses

The benchmark of stage B1 (notes `planning/stages/B1.md`): 24 functions of
stx_auxlib at `08c8132`, each with its properties, bound, one planted mutant
and one vacuity probe, run under ESBMC, CBMC, cpp-verify and GenMC at equal
bounds. Nothing here has been compiled or run yet; the first runs are the
smoke phase on the M4.

## Layout

| Path | What |
|---|---|
| `rows/NN-name/row.json` | the row: title, harnesses, tools, sizes, unwind, configurations, mutants, unbounded attempts |
| `rows/NN-name/treatment.cpp` | the harness for ESBMC and CBMC (rows 22 and 23: GenMC) |
| `rows/NN-name/cppverify.cpp` | rows 01, 02, 12, 13, 15 and 16: the same code with the row's property as cpp-verify contracts |
| `rows/23-atomic-pool/genmc_std_model.hpp` | pthread models of the host mutex, condition variable and thread for GenMC |
| `common/verify.h` | tool-neutral vocabulary: `nondet_*`, `STX_ASSUME`, `STX_ASSERT`, `STX_VACUITY_PROBE` |
| `common/symbolic_bytes.hpp` | an input buffer of symbolic length and contents, allocated at exactly its length |
| `common/utf8_spec.h`, `common/calendar_spec.h` | reference functions written from RFC 3629, RFC 2781 and the Gregorian rule, independent of the code under test |
| `tools/lift.py` | builds a translation unit from a harness and the pinned sources |
| `tools/run_b1.py` | runs every row, tool, configuration and size; writes `runs.jsonl` |
| `tools/census.py` | the construct census C1-C19 of B1.md section 5.3, per lifted function, from the pinned clang's JSON AST |
| `tools/tools.json` | each tool's command templates, solver or memory-model configurations and verdict strings |
| `sources.example.json` | where the pinned sources are; copy it and set `root` |

## Lifting

No third-party or treatment code is committed here. A harness names the
code under test with `//@ lift` directives (line ranges of a pinned file,
each checked by SHA-256), and `lift.py` copies those lines verbatim into
the translation unit at run time, with `#line` directives so every
diagnostic names the original file and line. The other directives:

- `//@ pin`: a header the harness includes as is, checked by hash.
- `//@ subst`: an environment model edit on a lifted line (row 23's pthread
  models, row 24's foreign lock), applied in every variant and recorded.
- `//@ contract`: the row's property attached as a cpp-verify contract.
- `//@ annotate`, `//@ annotation-begin`/`-end`: what a tool needs beyond
  the row's property and precondition, counted (B1.md section 5).
- `//@ model-begin`/`-end`: an environment model written in the harness
  (an allocator backend, a host mutex, `fail()`), counted and listed.
- `//@ mutant`: the planted mutant, applied only to the mutant run.

A hash mismatch stops the run (exit 3): the pinned source is not the one
the harness was written for.

## Running

```
cp sources.example.json SOURCES.json   # outside the repository; root = a git checkout at 08c8132
tools/run_b1.py --sources SOURCES.json --out OUT --smoke      # phase A: one run per row and tool
tools/run_b1.py --sources SOURCES.json --out OUT              # phase B: every run
tools/run_b1.py --sources SOURCES.json --out OUT --dry-run    # commands only, no tool
```

`SOURCES.json` and `OUT` live outside the repository: the M4 copy is
synced with `rsync --delete`, so anything written inside it is lost at
the next sync, and the runner and the census refuse an `--out` inside
the repository. The stx_auxlib root must be a git checkout (the lifter
reads its commit with `git rev-parse HEAD` and stops on a mismatch).

Per row and harness: the original at every size of its sweep; the mutant
and the vacuity probe at the headline size; for the tools listed under
`unbounded`, the original once more with no length bound (k-induction for
ESBMC, the solver back ends for cpp-verify). Each tool configuration
(ESBMC with Z3 and Bitwuzla, CBMC with its SAT back end and cvc5,
cpp-verify with Z3 and cvc5, GenMC under RC11 and IMM) is a separate run.
The dry run at the head of this branch plans 536 runs.

A row counts as proved only when the original proves, the mutant is
refuted and the probe is reached; the analysis decides that from
`runs.jsonl`, not the runner. Results are classified as proved, refuted,
bound-too-small, timed-out, out-of-memory, unsupported, unknown,
tool-error or lift-error, with each run's full output in `OUT/logs/`.

## Bound-effect configuration

Owner direction of 2026-11-13 (notes `decisions/2026-11-13-root-failsafe-
bound-effect.md`): arithmetic is an effect with stated bounds, not control
flow. Rows 02, 12, 13 and 21 have a second harness, `effect.cpp`, run under
the config `bound-effect`. Its `//@ abstract` directives replace each
arithmetic result on a lifted line by any value within its stated bound
(`common/effect.h`), asserting where needed the operand bounds the effect is
stated for. The run then verifies control flow and the definedness of what
stays exact, and solver-hard arithmetic cannot make it time out; a time-out
there is a control-flow result. The lifter lists every abstraction with its
bound in the run's report (`abstractions`), and each bound is a numeric
obligation established outside the run: exhaustively where the domain
allows, by the D-stage tools otherwise. `properties` in `row.json` names
what the configuration checks; a row with no control-flow property beyond
P0 has no mutant there.

Row 21's bounds are established by `tools/enumerate.sh SOURCES OUT`: it
lifts `rows/21-sincos-float/enumerate.cpp` (sincos with each abstracted
value recorded, nothing else changed), compiles it natively with the pinned
clang and the library's own floating-point flags, runs every one of the
2^32 floats and exits 0 only when every bound holds.

Row 21's runs before kernel_constants was lifted (`constants.cpp`, added
2026-11-13) saw arbitrary constants; `b1_records.py` marks them void
(`_voids` in `b1_closures.json`), not as findings.

Not applied: row 15 (straight-line modular arithmetic, kept as a solver
measure: no control flow to verify), row 19 (its reachable lifted code has
no arithmetic effect: tags, alignment and pointer offsets are control and
memory), row 20 (comparisons and counts over symbolic sorted slots, which
are its control flow).

## Census

```
tools/census.py --sources SOURCES.json --out OUT --clang /path/to/clang++-22.1.8
```

Lifts each row without edits (the census describes the source, not the
environment models), blanks the `#line` directives so clang's locations
are the unit's own lines, dumps the AST as JSON and counts C1-C19 on the
lifted lines only, per function, with the constructs outside Clight's
discipline, StrictC's further exclusions, house style and cpp-verify's
subset (decision B1-5 = 1). Each entry has a status under the owner's
direction of 2026-11-13: "distance" (an escaping local address, a change
of the floating-point environment, a write to an input that crosses as a
read-only type), "permitted by house style" (floating point, locals and
static locals, taking a local's address, writes through explicitly
mutable parameter types), "advisory" (never written but not const, as
misc-const-correctness reports it), "recorded (not decidable from the
AST)" (a local's address passed to a call) or "tool gap" (cpp-verify's
subset). The summary counts distance only. Output: `OUT/census/<row>.json` and
`summary.txt`.
The rules are mechanical and stated in the script; the syntactic ones (a
loop's bound class, a fall-through) are marked so in its header.

## Host environment (macOS arm64)

Recorded per run under `environment` in `runs.jsonl`: GenMC gets
`-isysroot` with `xcrun --show-sdk-path` (it calls clang without the SDK);
ESBMC gets `--idirafter` with the builtin-header directory of the LLVM it
links (`tools.json` `resource_clang`). Every run has a 16 GiB memory
budget, enforced by the runner's watchdog on the process group's RSS
(sampled every 0.25 s; `memory_watch` in the record), and the time limit
of `--timeout`. CBMC's C++ parser cannot read the SDK's libc++ headers;
such runs are recorded as unsupported.

## What the smoke phase must confirm

`tools.json` lists, per tool, the flags and verdict strings read from the
tools' documentation but not yet seen working (`to_confirm`). The smoke
phase checks each before a measured run; a change to a flag is a commit
here, never an edit on the M4.
