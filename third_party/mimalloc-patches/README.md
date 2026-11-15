# STXlib's patches to mimalloc

STXlib keeps its own patched copy of mimalloc (the owner, 2026-11-15; notes
`decisions/2026-11-15-mimalloc-audit-answers.md`). `third_party/mimalloc/`
is the pinned upstream release named in `PROVENANCE.md` with the patches of
this directory applied, in the order of `series`. This applies to mimalloc
only. Every other vendored tree is still never edited in place (AGENTS.md).

## A patch

- One file, `NNNN-short-name.patch`. `NNNN` is its place in the series.
- A unified diff with paths relative to `third_party/mimalloc/`
  (`--- a/src/heap/page-map.c`, `+++ b/src/heap/page-map.c`). It changes
  existing files; it never adds, removes or renames one.
- A header before the first `--- ` line, in plain text:
  - `Subject:` one line;
  - `Fixes:` the audit ID or IDs (`smi/dev/mimalloc-audit.md` and later
    audits) and the defect in one sentence;
  - `Upstream:` one of `to report`, `reported <URL>`, `fixed upstream in
    <commit>, not yet released`, or `not for upstream: <reason>`;
  - a paragraph on what the patch changes and why it is safe.
- Hashed in `PROVENANCE.md`: its SHA-256 in the patch table, and the
  SHA-256 of every file it changes, as patched, in the patched-file table.

## The check

`tools/check_mimalloc_patches.py` (ctest `stxlib_mimalloc_patches`) checks:
- that the series, the files here and PROVENANCE agree;
- that every hash matches;
- that reverse-applying the series exactly (no offset, no fuzz) gives back
  the pinned release, file for file, by Git blob SHA-1 and SHA-256.

`--print-tables` prints the rows to paste into PROVENANCE after a change.

## Changing the series

- **A new patch:** write the change in `third_party/mimalloc/`, save the
  diff here under the next number, add it to `series`, update PROVENANCE
  with `--print-tables`, and run the check. Each patch is audited, and its
  `Upstream:` line says whether it is to be reported. Sending a report is
  the owner's approval first.
- **A port forward:** replace the tree with the new pinned release, then
  re-apply each patch. Drop a patch that upstream has taken, and rebase one
  that no longer applies. Re-hash everything and re-audit each patch against
  the new code.
