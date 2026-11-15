# stx_minmallocpp as a graph: entries, exits, leaves and cycles

The owner's frame (2026-11-15): the code is the model. This file traces the
allocator's code as a graph: every way in, every way out, what is written
before each way out, every call that leaves the allocator, and every cycle
with its bound. It is read off the source at the commit that carries it and
must be kept in step with it. Written on Neo; nothing here has been run.

Three kinds of fact follow from the graph, each with its own check:

| Kind | Example | Check |
|---|---|---|
| Shape | which exits exist; no write before a refusal; which host calls are reachable | this file, by reading; the leaves by `tools/check_stx_minmallocpp_surface.py` on the compiled objects |
| Local | an index is in range; a shift is below 64; a sum cannot wrap | the entry and exit checks (`STX_MINMALLOCPP_CHECK`, on in the direct tests and a Debug `stxlib_state`); the integer-check build; the exhaustive arithmetic tests |
| Carried state | the bitmaps agree with what is live; the run index is whole | assumed at each entry and checked at each exit (`page_whole`, the run tags); not yet proved for all cases |

The call graph has no cycle (no recursion, no virtual call, no callback).
Paths below are relative to `src/state/stx_minmallocpp/`.

## 1. Entries

| Entry | File | Who calls it |
|---|---|---|
| `allocate(size, alignment)` | `allocator.cpp` | `stxlib_state`'s allocator calls (`state_calls.cpp`) |
| `release(address)` | `allocator.cpp` | the same |
| `trim()` | `allocator.cpp` | nobody yet |
| `thread_ending(context)` | `allocator.cpp` | the host layer's thread-exit hook only |

Nothing runs at load, at process exit, or in a static constructor: every
static is constant-initialised (`constinit`).

## 2. Leaves: calls that leave the allocator

All are in `src/hosted/`. None can unwind.

| Host family | Calls |
|---|---|
| POSIX | `mmap`, `mprotect`, `munmap`, `sched_yield`, `sysconf`, `pthread_key_create`, `pthread_setspecific`, `pthread_getspecific`, `abort` |
| Windows | `VirtualAlloc`, `VirtualFree`, `GetSystemInfo`, `SwitchToThread`, `abort` |

`abort` is reached only from `os::stop` (sections 3.5 and 5). The compiler
may add block set and copy, the thread-local access routine and stack
protection; the symbol check lists them.

## 3. Exits

"Writes" means writes to the allocator's tables. No path reads or writes a
block's memory.

### 3.1 `allocate`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| A1 | alignment not a power of two, or above 1 GiB, or size above half the address space | none | null |
| A2 | rounded size up to 512 KiB, alignment up to 64 KiB, the class's current word non-zero | that word without its lowest bit; the page's `live` + 1 | a block |
| A3 | as A2, current word zero | sub-graph `refill` (3.3) | a block, or null |
| A4 | size and alignment up to 32 MiB, not A2/A3 | sub-graph `allocate_singleton`: a run from the index, one page made | a block, or null |
| A5 | otherwise | sub-graph `allocate_huge`: one record, one mapping, its directory cells | a block, or null |

A null result leaves every table as it was, except that a region, a commit
or a heap slot made on the way stays (it is whole and will be used).

### 3.2 `release`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| R1 | address at or above 2^48 | none | refused |
| R2 | directory cell says not ours | none | refused |
| R3 | cell names a huge mapping | sub-graph `release_huge`: one compare-exchange on the record; on success the cells cleared, the mapping released | released, or refused with no write |
| R4 | `slice_head` says no page | none | refused |
| R5 | not a block's first byte, or past the last block | none | refused |
| R6 | the page is another thread's, or abandoned | sub-graph `release_remote` (3.4) | released, or refused with no write |
| R7 | mine; block already free | none; or, if releases were pending, their merge (and the page freed if that emptied it) | refused |
| R8 | mine; block live; page still has live blocks | the `free` bit, the `summary` bit, `live` - 1 (after a merge if releases were pending) | released |
| R9 | mine; that was the page's last live block | as R8, then sub-graph `release_emptied`: the page kept, or unlinked and freed | released |

**No write lies on any path to R1, R2, R4 or R5**, nor to the refusals
inside R3 and R6. That is "a refusal touches nothing" for every address
that is not a block of ours. R7 can follow a merge, which writes only what
other threads' valid releases asked for.

### 3.3 `refill` (allocate's slow path)

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| F1 | the allocator cannot start (host page size unusable, or no address space for the heap table) | none, or `started` set to refused | null |
| F2 | thread has no heap and gets one | a heap slot and identity; the thread's exit hook armed; then as F4 to F8 | a block, or null |
| F3 | thread has no heap and gets none (ended, table full, hook refused) | the thread's pointer set to the ended sentinel; under the orphan lock, as F4 to F8 on the orphan heap, then its oldest page swept | a block, or null |
| F4 | the current page has a free block in a later word | `summary` bits cleared; the class entry repointed | a block |
| F5 | the current page gets free blocks from a merge | the merge | a block |
| F6 | the current page is full; another owned page is first in the queue | the full page unlinked and abandoned | a block |
| F7 | none owned; an abandoned page is adopted | up to 4 pops of the abandoned list; a claim; a merge; a link | a block |
| F8 | none adopted; a fresh page | a run from the index; one page made; a link | a block |
| F9 | none of the above: the host refused | as F6 and F7 | null |

### 3.4 `release_remote`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| M1 | the block's `free` bit is set | none | refused |
| M2 | the block's `remote` bit was already set | none (the OR changed nothing) | refused |
| M3 | an owner already has releases pending | the `remote` bit | released |
| M4 | the page has an owner | the `remote` bit; one count off `state` | released |
| M5 | abandoned; that was its last unreleased block | as M4; then claim as "freeing", merge, unlist, free the page (or abandon again if a block is still live) | released |
| M6 | abandoned; the caller's heap keeps it | as M4; then claim, merge, unlist, sweep the heap's oldest page, link | released |
| M7 | abandoned; it was full | as M4; then one list insert under the lock | released |
| M8 | abandoned; none of M5 to M7 | as M4 | released |

### 3.5 Stops

`os::stop` (abort) is reached only from `integrity_stop`, for: a block
named by both a `free` and a `remote` bit (released twice); more blocks
merged than were live; a page being made over remote bits left by a
release into the page that was there before; and, in a checking build, a
failed entry or exit check. Each needs a client error (a second release)
or a defect in the allocator. Nothing else ends the process.

### 3.6 `trim` and `thread_ending`

| Entry | Path | Writes | Result |
|---|---|---|---|
| `trim` | thread has a heap | every owned page merged; empty ones freed; idle units decommitted | none |
| `trim` | thread has none | the same for the orphan heap, under its lock | none |
| `thread_ending` | the thread has a heap | pointer set to ended; every owned page abandoned (or freed if empty); the slot returned | none |
| `thread_ending` | the thread has none | pointer set to ended | none |

## 4. Cycles and their bounds

Loops with a constant bound unroll and are not cycles of the graph:

| Loop | Bound |
|---|---|
| words of a page (`collect`, `point_at_free_word`, `page_whole`, `make_page`) | 64 |
| adoption attempts (`adopt`) | 4 |
| aligned reservation attempts (Windows `os::reserve`) | 8 |
| size classes (`trim_heap`, `retire_heap`, outer loop) | 56 |

Loops bounded by the request:

| Loop | Bound |
|---|---|
| slices of a page (`make_page`, `free_page`) | the page's slices: 1, 8, 64, or the request / 64 KiB (at most 512) |
| units of a run (`commit_run`, `give_run`) | slices / 16 + 1 (at most 33) |
| directory cells of a huge mapping | the request / 1 GiB + 1 |

The cycles that remain, each with what ends it:

| Cycle | Where | Ends when | Bound |
|---|---|---|---|
| lock wait | `LockScope` | the holder releases | the holder's bound (the head of `region.hpp` lists what a holder does), not a constant |
| claim retry | `claim` | the compare-exchange succeeds or the page has an owner | one retry per completed step of another thread on that page's `state` |
| abandon retry | `abandon` | the compare-exchange succeeds or the page is freed | one retry per release another thread announced in between |
| refill | `refill_owned` | a block is returned or the host refuses | each turn returns, abandons one full page, or follows another thread's release into the current page |
| key wait | `ensure_exit_key` (POSIX) | the one thread making the key finishes | one `pthread_key_create` |
| pages of a heap | `trim_heap`, `retire_heap` | the queue is walked | the pages the heap owns |
| regions and units | `decommit_idle_units` (trim only) | all walked | regions x 1,006 units |

So: with no other thread working on the same page or holding the lock,
every path is bounded by a constant or by the size of the request; thread
exit and trim are bounded by what the thread owns.

## 5. Atomic steps (the skeleton for interleavings)

Only these touch memory another thread may touch at the same time. Every
interleaving question about the allocator is a question about them.

| Word | Step | Where |
|---|---|---|
| `PageMeta::state` | load | `release` (relaxed), `release_remote`, `claim`, `abandon`, `refill_owned`, `sweep_oldest`, `trim_heap`, `list_abandoned` |
| | store "mine, nothing pending" | `collect` |
| | store first state / "no page" | `make_page`, `free_page` |
| | subtract one count | `release_remote` |
| | compare-exchange mine -> abandoned | `abandon`, `refill_owned` |
| | compare-exchange abandoned -> mine | `claim` |
| remote word | OR one bit | `release_remote` |
| | exchange with 0 | `collect` |
| | load (must be 0) | `make_page` |
| free word | load, store (owner; relaxed) | `take_block`, `release`, `collect`, `point_at_free_word`, `make_page` |
| | load (non-owner; relaxed) | `release_remote` |
| `slice_head` entry | load (acquire) | `release` |
| | store (release) | `make_page`, `free_page` |
| directory cell | load (acquire) | `release` |
| | store (release) | `add_region`, `allocate_huge`, `release_huge` |
| `HugeRecord::block` | store; compare-exchange to 0 | `allocate_huge`; `release_huge` |
| `abandoned_count[class]` | load; add; subtract (relaxed) | `adopt`; `list_abandoned`; `unlist` |
| `structure.started` | load; store | `ensure_started` |
| the two locks | exchange; store | `LockScope` |

Everything else a thread writes is its own heap, a page it owns between a
claim (or making) and the abandon or free that ends that ownership, or a
table under the structure lock.

## 6. What this does not establish

- The compiler: the graph is of the source. The symbol check is the one
  fact taken from the compiled objects.
- The host's behaviour at the leaves.
- That the carried state holds for every interleaving of section 5's
  steps. That is the remaining proof (`proofs/REPORT.md`): a check of every
  interleaving for 2 to 3 threads on one page, taken from this skeleton,
  then an argument for any number.
