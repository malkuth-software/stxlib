# STXlib guardrails

STXlib, the Standard Trustworthy eXtended library (`stxlib`), is the
interfacing library between GUI.Forms and everything outside it, and the
replacement for stx_auxlib. It is not a standard library. Every interface it touches is treated as foreign: the host C
library, the host C++ library, the operating system, the kernel, and the
client program on the other side of GUI.Forms. It protects GUI.Forms from
those interfaces, and protects the client from us.

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
- The own runtime (`runtime/`) is linked only into the appliance
  supervisor and its test bed. It never replaces the host's runtime in a hosted process: no
  global `operator new`, `malloc` or symbol interposition is imposed on
  foreign code.
- Third-party code is pinned and hashed in `PROVENANCE.md`, updated only by
  replacing it with another pinned, hashed release, never edited in place.
  Every download is the owner's approval first: file, source and size.
- One exception, mimalloc (the owner, 2026-11-15; notes
  `decisions/2026-11-15-mimalloc-audit-answers.md`): STXlib keeps its own
  patched copy. `third_party/mimalloc/` is the pinned release that
  `PROVENANCE.md` names, with STXlib's patch series applied. Each patch is a
  numbered diff in `third_party/mimalloc-patches/`, listed in its `series`.
  Its header states the defect it fixes (audit ID) and whether it is to be
  reported upstream; its hash and the hashes of the files it changes are in
  `PROVENANCE.md`. `tools/check_mimalloc_patches.py` (ctest
  `stxlib_mimalloc_patches`) reverse-applies the series and requires exactly
  the pinned release. A change to mimalloc is a new, audited patch, never an
  unrecorded edit. A port forward rebases the series onto the new release.
  Sending anything upstream is the owner's approval first.
- This repository depends on nothing outside itself except its declared,
  pinned dependencies. It may be built on its own.

## Verification

- Correctness is proved, not sampled (owner, 2026-11-02). Random runs,
  fuzzing and sanitizers are evidence of likely defects only, and are
  reported as such.
- Each module in scope carries its proof harnesses and a proof report entry:
  proved, assumed (with the assumption), or not yet proved. A claim of
  "verified" names the tool, its version, the bound and the property.
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
