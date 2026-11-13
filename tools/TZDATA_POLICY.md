# Taking a new IANA tzdata release

STXlib compiles its time zones from IANA's tzdata (owner decision D12 = a;
`include/stxlib/local_time.hpp`). The release in use is pinned in
`third_party/tzdata/version` and `PROVENANCE.md`. This is the proposed
procedure for replacing it; it is not an owner decision until the owner
adopts it.

## When

IANA publishes several releases a year, often at short notice when a
government changes its rules. A release is taken when it changes a zone's
past or future readings inside WallTime's range (its NEWS says so). A
release that only changes comments or code is recorded as seen and not
taken.

## Steps

1. **Ask.** Downloading is the owner's decision each time (AGENTS.md). The
   request names the files `tzdata<release>.tar.gz` and
   `tzdata<release>.tar.gz.asc`, the source
   (`https://data.iana.org/time-zones/releases/`), and their sizes. Nothing
   is fetched before the answer.
2. **Unpack for reading.** Into `research/tzdata-<release>/` of the
   workspace (`archive/` keeps the tarball and signature; `src/` the
   unpacked files), read-only. Record the archive's SHA-256. Check the
   signature against IANA's published key when gpg is available, or record
   that it was not checked.
3. **Copy.** Replace the twelve files of `third_party/tzdata/` (the ten data
   files, `version`, `LICENSE`) with the release's, unmodified, and check
   each against the member extracted from the archive. Replace the table in
   `PROVENANCE.md` with the new SHA-256s, the archive's hash and the date.
   If `zic`'s default data file list (the Makefile's `TDATA`) changed,
   change `DATA_FILES` in `tools/tzcompile.py` and say so.
4. **Generate.**
   `python3 tools/tzcompile.py third_party/tzdata src/zones/zone_data.inc`.
   The compiler checks every zone's footer against its full expansion and
   refuses to write tables otherwise; a refusal (for example a rule set
   with other than two rules running to "max") is a change to the
   compiler, reviewed on its own.
5. **Check.**
   - `python3 tools/tzvectors.py third_party/tzdata src/zones/zone_data.inc tests/zone_vectors.inc`:
     the reader's rule over the new tables agrees with the expansion at
     every change point of either, for every name (exit status 0), and the
     test vectors are rewritten.
   - `python3 tools/tzcheck.py third_party/tzdata <zoneinfo>` against a
     zic-built reference of the same release where one is at hand (or the
     host's, of an earlier release): every name agrees, differs only in
     the daylight flag (tzdata's main and rearguard forms), or differs in a
     zone the NEWS between the two releases names. Record the counts and
     the differing names in `proofs/REPORT.md`.
   - The integrator builds and runs the suite (`tests/local_time_tests.cpp`
     checks the C++ reader against the vectors) on every host family.
6. **Pin.** One commit with the copied files, the regenerated tables and
   vectors, `PROVENANCE.md` and the report entry, titled with the release.
   `tzdata_version()` then returns the new release's name.

## What a release can break

- A zone removed or renamed: `Zone::named` with the old name fails with
  not_found unless `backward` keeps it as a link (IANA's practice). The
  release's NEWS is read for removals before taking it.
- New FORMAT or rule syntax: the compiler refuses what it does not parse,
  so a new form fails at step 4, not at run time.
- Readings in the past can change (IANA corrects history); programs that
  stored local times rather than instants see different ones.
