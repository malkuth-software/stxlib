# STXlib guardrails

STXlib, the Standard Trustworthy eXtended library (`stxlib`), is the
interfacing library between GUI.Forms and everything outside it, and the
replacement for stx_auxlib. It is not a standard library. Every interface it touches is treated as foreign: the host C
library, the host C++ library, the operating system, the kernel, and the
client program on the other side of GUI.Forms. It protects GUI.Forms from
those interfaces, and protects the client from us.

STXlib sideloads alongside the host C library (glibc, musl, FreeBSD's
libc, the UCRT); write that, never that it "runs on" one. Whatever the host
does, STXlib's behaviour inside is the same: own what can be owned, and call
the host C library only for what STXlib does not provide itself (owner,
2026-11-15; notes `decisions/2026-11-15-boundary-sideloading.md`). A host
difference that leaks inside is a boundary defect.

Plans, decisions, surveys and evidence are not kept here. They live on the
`stxlib` branch of `malkuth-software/notes`, under `planning/`. Read
`planning/FORWARD_PROGRAMME.md` there before starting work, and
`planning/DECISION_SURVEY.md` for what the owner has and has not decided.
Coordination state is in the private projects repository,
`desks/stxlib/` and `tracks/stxlib/`.

## Code style (mandatory)

All first-party C++ follows the house style: `HOUSE_STYLE.md` on `main` of
`malkuth-software/notes`. Read it in full before writing code. Its eleven
sections apply here without exception, tests included. STXlib adds its own
rules in `docs/CODE_STYLE_APPENDIX.md` (floating point and subnormals, the
freedoms of repeated kernels, intended immutability; owner, 2026-11-13):
read it too; it binds STXlib only. Two house-style sections are this
library's whole purpose:

- **Section 1, memory:** every datum has an owner and a lifetime. Memory we
  allocate is released only by us, and we never release foreign memory
  (owner, 2026-11-02, the ownership-boundary model).
- **Section 2, failure and foreign interfaces:** say what a failure leaves
  behind; validate before mutating; explicit results for expected
  alternatives; foreign interfaces state types, counts, ownership and errors,
  and take entry-point types from their declarations.

## Hard boundaries

- C++20. No `std::` type, platform type or kernel type in a public header
  under `include/stxlib/`. Public types have a layout we define.
- Native unwinding never crosses a boundary in either direction. A failure
  crosses as an error record we own (the status-review thread, 2026-11-13).
  Development builds shadow every exception at every ABI boundary so it is
  fully visible; release builds of everything ours use no exceptions; for
  GUI.Forms that comes after its 1.0 (owner, 2026-11-13).
- Interface modules live under `src/hosted/<family>/`. The appliance's own
  runtime lives under `runtime/` and the FreeBSD kernel shim under
  `appliance/shim/`. Nothing outside those directories issues a system call,
  includes an operating-system header or names a kernel object. The trust-chain checker enforces this on the compiled
  output.
- Mutable process-wide and per-thread state lives in `stxlib_state`
  (`src/state/state.hpp`), reached through its accessors, so every module
  that links STXlib statically shares one copy. Two exceptions are recorded
  (decisions 2026-11-15, notes `decisions/2026-11-15-roadmap-choices.md`,
  choices 11 and 12):
  - the host allocator backend (`src/state/allocator.cpp`) calls the host C
    allocator (`malloc` and `free`) from `src/state/`, outside
    `src/hosted/`, because the allocator lives in the state so that a block allocated in one module may be released in
    another. It is the only host interface called from `src/state/`. The thread-exit hook that releases each thread's records (`src/hosted/<family>/thread_exit_<family>.cpp`, layout 10) stays under `src/hosted/` but is compiled into `stxlib_state`, beside the state it releases;
  - a module may keep a documented write-once memo: a value derived from a
    state value that never changes once set, written at most once per
    module with the same result in every module, for example the math
    dispatch table (`src/algorithms/math/dispatch.cpp`, derived from
    `ProcessState::simd_selection`). The memo's comment names the state it
    derives from. Mutable shared data outside the state is never allowed.
- The own runtime (`runtime/`) is linked only into the appliance
  supervisor and its test bed. It never replaces the host's runtime in a hosted process: no
  global `operator new`, `malloc` or symbol interposition is imposed on
  foreign code.
- Third-party code is pinned and hashed in `PROVENANCE.md`, updated only by
  replacing it with another pinned, hashed release, never edited in place.
  Every download is the owner's approval first: file, source and size.
- This repository depends on nothing outside itself except its declared,
  pinned dependencies. It may be built on its own.

## Early exits and termination (hard boundary)

Owner decisions of 2026-11-15, notes `decisions/2026-11-15-early-exit-integrity.md` and `decisions/2026-11-15-thread-local-exit-safety.md`. Audit E (notes `audits/E-early-exit.md`) classifies every site. Its section 1 defines the annotations that the shared check (template_harness) enforces.

**Every call to code we do not control is a possible early exit.**
- **Foreign code** is:
  - host C and C++ libraries, including the C and C++ runtimes;
  - operating-system and kernel interfaces;
  - ICU;
  - vendored code (`third_party/`);
  - every consumer callback, whether it reaches us through a gate, a `CallbackTarget`, a pool task, a thread entry, a predicate, a reporter, an exit task or a thread-record callback.
- **An early exit** is any of:
  - a C++ exception;
  - `longjmp`;
  - a forced unwind, including glibc's `pthread_cancel` at any cancellation point, and `pthread_exit`;
  - a callback that never returns.

  Fail-fast and process exit are not early exits in this sense: they leave no observable state behind except durable disk state, which D13 governs.

**At each foreign call site, exactly one of two forms holds, and the code says which** (audit E, section 1.3: `STXLIB_EARLY_EXIT_WHOLE`, `STXLIB_EARLY_EXIT_GUARDED`):
1. **Whole:** every invariant is whole before the call. No lock is held, no counter or flag is half-updated, and no record is published but unfinished. Anything that could break is finished before the call or begun after it.
2. **Guarded:** the partial state is safe to unwind through. A live guard object restores or completes it, and the state its destructor sees is consistent.
   - Our own code is built with `-fno-exceptions`, so its frames have no landing pads. An unwind passes through them *without running destructors*, so form 2 exists only in frames compiled with exceptions: the gate templates and other code instantiated in a consumer's translation unit.
   - No guard runs for `longjmp` (a longjmp past such a frame is undefined under [csetjmp.syn]), or for a callback that never returns. Against those, only form 1 protects, and every callback contract says "must not longjmp out".
   - So a foreign call is never made while an STXlib lock is held, unless the callee provably cannot exit early.

**Allocators must not unwind.**
- The `stxlib_state` allocation path never throws, never `longjmp`s, and never calls code that can. That covers both backends, `memory::allocate` and the per-thread record allocation.
- Exhaustion is a named `out_of_memory` (D8 = a), or fail-fast where the caller cannot take a result.
- No new-handler is ever called.
- Functions on the allocation path carry `STXLIB_NO_UNWIND` and are `noexcept`.

**Termination frees nothing twice (rule 1, as the owner reworded it).**
- An owner lets go of a resource, by clearing the pointer or marking the slot released, *before* it releases the resource. This includes destructors.
- Process termination (`exit`, a return from `main`, fail-fast) releases none of our state: the operating system reclaims it.
- Only a normal thread exit releases that thread's records, once, through our own hook.

**Every thread_local is trivially destructible** (a pointer, an index or a flag).
- Real per-thread state lives in records that `stxlib_state` owns explicitly (`include/stxlib/thread_records.hpp`; notes `planning/stages/E-thread-records.md`).
- Those records are released by our thread-exit hook, never by the C or C++ runtime.

**Our code never calls `exit` or `quick_exit`.** Fail-fast keeps `abort`. STXlib offers no call that ends the process normally (`exit_process` was removed, owner 2026-11-15); a program returns from main or calls `exit` itself, and the exit tasks run from atexit.

**Cancellation is acted on only outside STXlib's locked sections** (owner 2026-11-15): code that reaches a POSIX cancellation point while an STXlib lock is held keeps cancellation off with `detail::CancellationHold` (`include/stxlib/detail/cancellation.hpp`), declared before the lock. A gate that a forced unwind reaches fails fast.

## Verification

- Correctness is proved, not sampled (owner, 2026-11-02). Random runs,
  fuzzing and sanitizers are evidence of likely defects only, and are
  reported as such.
- Each module in scope carries its proof harnesses and a proof report entry:
  proved, assumed (with the assumption), or not yet proved. STXlib carries
  no "verified" label (decision D16, 2026-11-15): its claim is conformance
  to the house style, and each specific check names its tool, its version,
  the bound and the property.
- Each replaced implementation keeps its reference (glibc, musl, UCRT,
  libc++ or libstdc++ behaviour, cited at a pin) and a conformance or
  equivalence check against it (house style section 8).
- Sanitizer and integer-check builds still run on every change: they are
  cheap and catch what a proof's assumptions missed.
- Nothing may be more than twice as slow as its platform equivalent
  (stx_auxlib's ceiling, owner 2026-10-31) unless the owner accepts the
  ratio.

## Work rules

- Workers commit on local branches and never push. The integrator builds;
  coders do not (projects `briefs/STXLIB_BRIEF.md`).
- No compiler runs on the Mac laptop (Neo). Builds go to the M4 through
  `~/Developer/stxlib_workers/bin/m4sb`.
- Preserve negative results. Report exact commands and environments.
