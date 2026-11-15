# Phase D: targeted proofs of STXlib's own modules

Phase D replaces B1's permutation grid with targeted proofs (coordinator's
amendment of 2026-11-13, "stop the permutation grid"): ESBMC for C++
control flow, GenMC for concurrency, exhaustive enumeration wherever the
domain fits; short limits; a second solver only when the first gives no
verdict; the bounded-effect abstraction for solver-hard arithmetic
(`../b1/common/effect.h`). It uses B1's lifter, runner and report format:
every harness lifts the code under test verbatim from STXlib's tree by
`//@ lift` with a SHA-256 per span, so a harness that no longer matches the
code fails to lift (status 3) instead of proving something else.

## Layout

| Path | What |
|---|---|
| `rows/Dnn-name/row.json` | B1's row schema; `tools` names `esbmc`, or `enumeration` for a harness run by `tools/enumerate_d.sh` |
| `rows/Dnn-name/*.cpp` | one harness per property group, each with its mutant |
| `tools/enumerate_d.sh` | lifts, builds (pinned clang) and runs the enumerations, original and mutants |
| `sources.example.json` | the sources file: one key, `stxlib`, the tree under proof (no commit pin: the spans' hashes pin the code) |

## Runs

```
# the targeted runs (120 s, z3 first, bitwuzla only where z3 gave no verdict)
python3 proofs/b1/tools/run_b1.py --sources S.json --out OUT --rows-directory proofs/d/rows --lean
# the enumerations
proofs/d/tools/enumerate_d.sh S.json OUT/enumerate
# the distance of STXlib's own code (every fold), and of the lifted spans
python3 proofs/b1/tools/census.py --sources S.json --out OUT/tree --source stxlib --tree \
    --exclude src/hosted/windows --clang /opt/homebrew/opt/llvm@22/bin/clang++
python3 proofs/b1/tools/census.py --sources S.json --out OUT/rows --source stxlib \
    --rows-directory proofs/d/rows --clang /opt/homebrew/opt/llvm@22/bin/clang++
```

`--lean` (run_b1.py): a 120 s limit unless `--timeout` is given; variants
original, mutant and probe (no unbounded runs unless asked); a tool's
second solver configuration runs only where the first gave no verdict
(timed out, unknown, out of memory, tool error). A row's property counts as
proved only when the original proves, the mutant is refuted and the vacuity
probe is refuted (B1.md section 3.3).

## Rows

| Row | Module | Property | Tool and bound | Abstraction or model | Mutant |
|---|---|---|---|---|---|
| D01 | `src/core/float_exact.hpp` (float) | two_sum exact, error zero or normal; fast_two_sum equals two_sum; split exact with 12-bit halves; Dekker's product exact; fused and Dekker forms agree | ESBMC, every float (pair) in range; no loops | none | one per harness (an operator or operand changed) |
| D02 | `src/core/float_exact.hpp` expansions (double) | P0: reads and writes within the stated counts; P1: the counts | ESBMC, lengths up to 3 | bound effect: every error-free transformation returns any pair (no bound needed) | scratch window shifted by one |
| D03 | `src/core/float_reduce.hpp` | P1: F5's shape for every term value; P2: chunks and the pool's data flow in any task order | enumeration over symbolic terms: n = 0..1100, chunk boundaries to 4 x 4096 + 11 | none (symbolic terms are exact for every value) | the lane tree regrouped |
| D03 | the same | P0: in-bounds reads; P1: signed zero kept | ESBMC, counts up to 40 | chunk_terms scaled to 16 (subst) | one block too many |
| D04 | `src/core/wtf8.hpp` | POSIX names lossless; unescape accepts only canonical text and leaves the output unchanged on failure; UTF-16 bijection | ESBMC, up to 5, 6 and 4 bytes or units | Growable's allocator as the tool's malloc (which may refuse) | continuation mask; missing rollback; pair range |
| D05 | `src/algorithms/row_height_index.cpp` | each edit's failure kind as the header names it | ESBMC, set_heights up to 3 heights | the template as a nondeterministic model of its refusal contract; vocabulary reduced | height check widened |

Not targeted here, with the reason:
- `orient2d`, `incircle`, the compensated sums and the double forms of the
  transforms: their correctness is real-number arithmetic beyond a bounded
  bit-precise run (literature proofs, sampled tests; REPORT batch
  float_exact). Their control flow is straight-line.
- The pool forms' concurrency: the data flow is D03's; that `AtomicPool::run`
  runs each task once and returns after all of them is the thread pool's
  property (a GenMC row of its own, after this batch).

## Distance

The distance of a function is the census (`../b1/tools/census.py`, B1.md
section 5.3, census-3 rules) of its own code. Phase D measures it in two
ways: on the lifted spans of each row (what the proof sees) and, at every
fold, on every file of STXlib's tree (`--tree`), so the record follows the
code rather than the harnesses.

## Re-pinned at the audit A fixes (2026-11-15)

Brought onto `claude/audit-a-fixes` from `claude/verification-b2-and-d` at
`db553d6` (proofs/b1 and proofs/d only; the trust-chain work, B2, stays on
that branch). Two rows were re-pinned to the fixed code, by the lifter's
`fill_hashes` and by hand for the line numbers; every harness lifts
(run_b1.py --dry-run, Neo, 2026-11-15):
- D03: `float_reduce.hpp` 60-223 changed only by `{}` initialisers on the
  two accumulators (audit A, X9); same lines, new hash.
- D04: `growable.hpp` 16-128 (was 16-92): Growable gained `reserve_exact`,
  `take_as_buffer` and `move_to` (audit A, X5). The substitutions follow the
  allocation calls to lines 23, 116 and 119, and a model block declares
  `Buffer` and `detail::BufferAdoption`, which `take_as_buffer` names and
  the harnesses never instantiate.

The census as a CI job: `.github/workflows/ci.yml`, job `census`
(`census.py --tree --fail-on-error`, with `--define
STXLIB_MATH_VARIANT=native` for the math sources).

