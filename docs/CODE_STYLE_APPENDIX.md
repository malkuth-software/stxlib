# STXlib code style appendix

This appendix adds STXlib's own rules to the house style (`HOUSE_STYLE.md` on
`main` of `malkuth-software/notes`). It binds STXlib only. The house style
applies here in full, and this appendix adds to it without overriding any
of its sections. Where the two seem to disagree, the house style wins, and
the disagreement is raised, not settled locally.

Source: the owner's decision of 2026-11-13. Of four proposed changes to the
house style, only the locals rule (house style §1) went into the house
style. The other three were made "a formal code style appendix" for this
library, the successor to stx_auxlib. The record is the notes branch
`stxlib`, `decisions/2026-11-13-locals-float-mutability.md`, last section.
The floating-point rules restate options D1 and F1-F5 of the same record.
Their reasoning and citations are in `planning/stages/A1-float-discipline.md` on the same branch.

Sections are named after the house-style section they extend. A4 has its
own source, given in the section.

---

## A1. Floating point and subnormals (extends house style §3, "Types and values")

House style §3's "Choose numerical precision once" stands as written. In
STXlib, floating point is IEEE 754 `float` and `double` on the hardware, at
full speed. It is allowed and well bounded. What is forbidden:
- a leak;
- a value outside a function's stated domain;
- a floating-point environment left changed;
- a promised result that differs between targets.

Every result STXlib promises is reproducible bit for bit on every target it
builds, by these rules.

1. **No contraction, no fast-math.**
   - Build with `-ffp-contract=off -fno-fast-math -fno-math-errno`. The
     last flag is needed because `-fno-fast-math` turns math-errno back on
     for GNU and MinGW targets.
   - Never `-ffast-math`, `-Ofast`, `-funsafe-math-optimizations`,
     `-fassociative-math` or `-freciprocal-math`.
   - A fused multiply-add appears only where we write one. It is allowed
     only where it is exact (an error-free transformation) or where the
     result is shown to be the same with and without it.
   - One exception (owner, 2026-11-23): stx_fft's walk
     (`src/algorithms/fft/walk.inc`) has two builds of one source. The
     exact build has no fused multiply-add. The fused build fuses exactly
     the products the source marks, each of the form `a + b*c` or
     `a - b*c`, written as explicit intrinsics (`vfmaq`/`vfmsq` on NEON,
     `_mm_fmadd`/`_mm_fnmadd` on SSE with FMA3), never by contraction:
     `-ffp-contract=off` holds everywhere, this kernel included. Each build
     is bit-identical on every target that runs it, and frozen hashes in
     `tests/fft_walk_tests.cpp` hold both. A plan runs the fused build at
     the upper tier (NEON, or 128-bit SSE with FMA3) and the exact build at
     the lower tier (scalar, or 128-bit vectors without FMA: wasm SIMD128,
     x86-64 without FMA3). `include/stxlib/fft.hpp` states which results
     differ between the two.
   - `CMakeLists.txt` refuses a target without these options, or with a
     flag that undoes them. `src/core/float_discipline.hpp` stops a
     fast-math build at compile time.
2. **Our own math for every promised result.**
   - Transcendental and other library functions come from STXlib's math
     (`include/stxlib/math.hpp`), never the host's libm, wherever a result
     crosses our contract.
   - Host functions whose results IEEE 754 or the C++ standard fixes
     exactly are allowed, and tested: `sqrt`, `fabs`, `copysign`, `ldexp`,
     `ilogb`, comparison and classification, `std::to_chars` and
     `std::from_chars`.
3. **Never change the floating-point environment.**
   - No `fesetround`, no `fesetenv`, no MXCSR or FPCR writes, and no
     flush-to-zero or denormals-are-zero modes.
   - Assume round-to-nearest-even, the default.
4. **No subnormals.**
   - STXlib's numeric code neither accepts nor produces subnormal values. A
     subnormal argument is read as zero of its sign. A result that would be
     subnormal is returned as zero of its sign.
   - The rule is applied in software, on the values, never by the
     environment's modes (rule 3). `src/core/subnormal.hpp` holds the
     helpers.
   - Each function's contract says so. Its tests check the flush exactly
     over the subnormal range instead of measuring error there.
5. **No extended precision.**
   - Every intermediate is evaluated in its own type: `FLT_EVAL_METHOD == 0`,
     no x87, and no `long double` in computation.
   - A target without SSE2-class or NEON scalar floating point is not one
     STXlib builds.
6. **Evaluation as written**, with C++'s left-to-right grouping. A
   reduction does one of two things:
   - it runs in a stated, fixed order that depends neither on the SIMD
     level nor on the thread count (F5: 8 partial accumulators combined by a
     fixed pairwise tree; fixed chunks combined in index order;
     `src/core/float_reduce.hpp`);
   - or it runs through an exact or compensated accumulator whose contract
     permits its order to vary.
7. **At a foreign boundary, nothing more.**
   - The rules bind STXlib's code. A value a foreign caller passes us is
     read by our contract, the subnormal rule included. What foreign code
     does with our values, and the flags it builds with, are its own
     affair.
   - Code inlined from a public header compiles under the consumer's flags.
     A public header therefore holds only floating-point operations that are
     exact in any mode, such as a single exactly representable operation or
     a comparison. Anything else goes out of line.

Rules 1 and 6 bind every result STXlib promises. Code whose output is never
promised, such as benchmarks and test references, may take the freedoms
house style §8 lists. It must state them and hold to the reference's error
bound. Types stay `float` and `double`; there are no STXlib floating-point
types.

## A2. Freedoms in repeated kernels (extends house style §8, "Repeated kernels")

House style §8 says to state the numerical freedoms taken and hold them to
the reference's error bound. Its list: reassociation, partial accumulators,
fused operations, reciprocal substitution, and NaN and signed-zero
behaviour.

In STXlib, results promised bit for bit take none of these freedoms
(A1, rules 1 and 6). A kernel's fixed reduction shape (A1, rule 6) is part
of its contract, not a freedom.

## A3. Intended immutability (extends house style §11, "C++ spelling")

House style §11's "Mark read-only inputs `const`" stands. In STXlib,
immutability is stated where it is intended, through the type. This covers
key inputs and data expected not to change:
- views and borrows are read-only by default. `View<T>` reads, and writing
  needs the explicitly mutable `MutableView<T>`;
- key inputs cross interfaces as read-only types, so the signature carries
  the intent.

Immutability is not mandatory elsewhere. A mutable local or field is
ordinary code.

An advisory check (clang-tidy `misc-const-correctness`, in the repository's
`.clang-tidy`) reports locals and parameters that are never written but not
declared `const`. It informs and never fails a build. It never asks for
mutable code to become immutable. The integrator runs it at each fold and
reports what it finds.

## A4. Result and state shapes (extends house style §2, "Failure and foreign interfaces")

Source: the owner's decision of 2026-11-15, notes
`decisions/2026-11-15-roadmap-choices.md`, choice 10 (audit A, X2).

House style §2 keeps a result's or a state's `std::variant` behind named
factories and named queries. A public header of STXlib names no `std::`
type (AGENTS, "Hard boundaries"), so a public result or state shape is a
tagged union written by hand, under the same discipline:
- one discriminant (an enumeration, or a `bool` for two states) and the
  alternatives' data, in a `union` where they do not coexist;
- constructed only through named factories, one per alternative, with the
  constructors private, so a contradictory value cannot be made;
- read only through named queries, which return null, or are refused by a
  requirement, for an alternative that is not the active one;
- no flag beside a payload that could contradict it (house style §2 and
  §3): the state is the discriminant.

`OperationStatus`, `OperationResult`, `text::Formatted`, `text::Parsed`, `PrvOutcome` and
`ChildEnd` are the examples. Private code (`src/`) may use `std::variant`
behind the same factories and queries.

## Not part of this appendix

- The exact floating-point primitives (TwoSum, TwoProduct, expansions) and
  their range precondition (F3). They are an interface, documented in
  `src/core/float_exact.hpp` (F4: private until a consumer needs them).
- The locals rule. It is house style §1, not STXlib's.
