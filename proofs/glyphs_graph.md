# stxlib::glyphs as a graph: entries, exits, leaves and cycles

The owner's frame (2026-11-15): the code is the model. As
`stx_minmallocpp_graph.md` does for the allocator, this file traces the glyph
code (`src/algorithms/glyphs/`, headers `include/stxlib/font_file.hpp`,
`glyph_store.hpp`, `glyph_hinter.hpp`, `glyph_mask.hpp`, `glyph_face.hpp`) as a
graph: every way in, every way out, what is written before each way out,
every call that leaves the glyph code, and every cycle with its bound. The
owner asked on 2026-11-16 that the allocator's approach be applied here, because
a font is whatever an application hands in.

It is read off the source at the commit that carries it and must be kept in
step with it. What is tabled here and what is not is said in section 6: the
tables below cover the part that meets a font's bytes first (the reader's
entries in `font_file.cpp`), the glyph store and the face. The outline
loaders, the hinter and the rasterizer are entered below with their cycles
and bounds, and their exits are not yet tabled.

Three kinds of fact follow from the graph, each with its own check:

| Kind | Example | Check |
|---|---|---|
| Shape | which exits exist; what leaves the glyph code | this file, by reading; the leaves by `tools/check_glyphs_surface.py` on the compiled objects (ctest `stxlib_glyphs_surface`) |
| Local | an offset is inside its table; a sum cannot wrap | every read is behind `has` (`font_reader.hpp`); the AddressSanitizer, UBSan and integer-check builds on `stxlib_glyph_untrusted_tests` and the other glyph tests |
| Carried state | the store's index agrees with its parts; a face's table finds every outline it holds | assumed at each entry and checked at each exit when `STXLIB_GLYPHS_CHECKS` is defined (`glyph_checks.hpp`: `GlyphStore::whole`, `Face::whole`, `ShapeBuffer::whole`, `outline_whole`), which the three `_checked_tests` programs are built with |

The call graph has no cycle: no function of the glyph code calls itself,
directly or through another; there is no virtual call. The only calls
through a pointer are the rasterizer's plain flattener (a sink chosen from
two functions of `mask.cpp`) and the shaper buffer's three predicates
(`SameGroup`, `SortsAfter`, `Unwanted`), which its callers name. What other
code does by calling itself is done with an array of fixed depth: a TrueType
glyph's parts (`font_truetype.cpp`: 16 levels, of which
`FontLimits::component_depth`, 8 unless a caller says otherwise, are used), a
CFF glyph's subroutines (`font_cff.cpp`: 10 calls deep), a cubic curve's
halving (`flatten.hpp`: 16 deep).

## 1. Entries

| Header | Entries |
|---|---|
| `font_file.hpp` | `open_font`, `glyph_of`, `next_stretch`, `glyph_in_stretch`, `has_feature`, `advance_of`, `load_glyph`, `load_scaled_glyph`, `scale_of`, `scale_outline`, `to_segments`, `glyph_checks_enabled` |
| `glyph_store.hpp` | `GlyphStore::open`, `keep`, `glyph`, `glyphs_read`, `bytes_held`, `whole`, `release` |
| `glyph_hinter.hpp` | `Hinter::prepare`, `hint_glyph`, `hint_stored`, `bytes_held`, `release` |
| `glyph_mask.hpp` | `measure`, `Rasterizer::render`, `render_monochrome`, `bytes_held`, `release` |
| `glyph_face.hpp` | `open_typeface`; `Typeface::open`, `font`, `keep`, `fit`, `ink_box`, `glyphs_read`, `whole`, `bytes_held`, `bytes_peak`, `reset_peak`; `Face::open` (two), `font`, `typeface`, `warm`, `fitted`, `mask`, `measure_run`, `draw_run`, `glyphs_fitted`, `whole`, `bytes_held`, `bytes_peak`, `reset_peak`, `release` |

The layout tables (`layout_tables.hpp`) and the shaper's buffer
(`shape_buffer.hpp`) are internal: nothing outside the glyph code and its
tests calls them yet.

Nothing runs at load, at process exit, or in a static constructor: every
object of static storage is a `constexpr` table.

## 2. Leaves: calls that leave the glyph code

| Leaf | From | Why |
|---|---|---|
| `stxlib::detail::vocabulary_allocate`, `vocabulary_release` | `Vector`, `Unique`, `Shared` | the arrays, a hinter's and a face's working memory, a typeface |
| `stxlib::ErrorRecord::make` | `refuse_font` (`font_file.cpp`), `Status`/`Result` | a refusal's record |
| `stxlib::Mutex` made, `lock`, `unlock`, unmade | `glyph_face.cpp` (`TypefaceState`, `ScopedLock`) | a typeface is the program's |
| `stxlib::detail::requirement_failed` | `STXLIB_REQUIRE` in the vocabulary types; `STXLIB_GLYPHS_CHECK` in a checked build | a broken requirement stops the program |

The compiler may add block set and copy and stack protection; the symbol
check lists them. Nothing else: no file, stream, environment, clock, host
call, C library function, `operator new` or exception runtime. On the M4 at
this commit the objects' leaves are exactly these nine functions of STXlib's
and `bzero`, `memcpy`, `memset_pattern16`, `__stack_chk_fail`,
`__stack_chk_guard`.

`requirement_failed` does not return. The glyph code itself states no
requirement on its input; the ones reachable are the vocabulary types' own
(an index inside a `Vector`, a `truncate` no larger than the count), and the
indices given to them are checked first by the code that computes them. That
last sentence is argued, not proved: the builds under AddressSanitizer and the
bounded search of altered fonts are the evidence that no altered font reaches
one.

## 3. Exits: the reader's first entries (`font_file.cpp`)

"Writes" means writes through the entry's reference parameters. None of
these entries allocates. Every read of the font's bytes is behind `has`
(`font_reader.hpp`: the offset is inside the span and the count fits in what
is left; both in `usize`, with no sum that could wrap).

### 3.1 `open_font(bytes, face, font)`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| O1 | more than 2^32 - 1 bytes, or fewer than 12 | `font` emptied | invalid_argument |
| O2 | a collection, and `face` is not one of its faces, or its list of faces or the face's 12-byte header is not inside the bytes | `font` emptied | invalid_argument |
| O3 | not a collection, and `face` is not 0 | `font` emptied | invalid_argument |
| O4 | the face's first four bytes are none of the three kinds read | `font` emptied | invalid_argument |
| O5 | its table records (16 bytes each, as many as it says) are not inside the bytes | `font` emptied | invalid_argument |
| O6 | no `head` of 54 bytes, `maxp` of 6 or `hhea` of 36 inside the bytes | `font` emptied again | invalid_argument |
| O7 | units per em outside 16 to 16384, no glyphs, or a location format other than 0 or 1 | `font` emptied again | invalid_argument |
| O8 | CFF outlines whose header, indexes or dictionaries `open_cff` refuses | `font` emptied again | what `open_cff` gave |
| O9 | otherwise | `font` filled: every table kept is a range inside the bytes | success |

A table whose range is not inside the bytes is as if absent. A font with no
character map this reads, no outlines, no `hmtx`, `OS/2`, `post` or layout
tables still opens (O9): the entries below answer "nothing" for what is not
there.

Cycles: one loop over the table records (at most 65,535, each a fixed
number of reads); `choose_character_map`, one loop over the map's records
(at most 65,535).

### 3.2 `glyph_of(font, character)`

| Exit | Condition | Writes | Result |
|---|---|---|---|
| G1 | the character map is not readable in its format (`map_readable`: its fixed part and as many entries as it says are inside the table) | none | 0 |
| G2 | format 0, 4, 6 or 12, and the map has no glyph for the character, or the glyph's place is outside the table | none | 0 |
| G3 | the map's glyph is not less than `glyph_count` | none | 0 |
| G4 | otherwise | none | the glyph |

Cycles: format 4 and 12, one search by halving over the entries: at most 15
steps (a map has fewer than 32,768 segments) and 29 (fewer than 2^29 groups,
a table being shorter than 4 GiB); formats 0 and 6, none.

### 3.3 `next_stretch(font, from, stretch)` and `glyph_in_stretch(font, stretch, character)`

| Exit | Condition | Writes | Result |
|---|---|---|---|
| S1 | the map is not readable | `stretch` emptied | false |
| S2 | no entry ends at or after `from` (format 0: `from` above 255; format 4: `from` above 65535; format 6: no characters, or `from` past them) | `stretch` emptied | false |
| S3 | the entry found ends before it begins, or before `from` (a map not in order) | `stretch` emptied | false |
| S4 | otherwise | `stretch`: first, last, the entry's number | true, with first <= last and from <= last |

`glyph_in_stretch` gives 0 for a character outside [first, last], for an
entry number the map has not, for a glyph place outside the table and for a
glyph not less than `glyph_count`; else the glyph. It writes nothing.

Cycles: one search by halving. Going through a map (from 0, then from one
past each stretch's end, stopping at a stretch that ends at the last
character) takes at most as many calls as the map has entries, and one. In
a map that is in order or not: the search's answer never goes down as `from`
goes up (two searches go the same way until one entry is below the larger
`from` and not the smaller, and there the larger goes right of it), and it
cannot stay, since the entry found ends at or after `from` (it is the last
place the search stepped left to) and the next `from` is past that end.
`stxlib_glyph_untrusted_tests` checks the count on every map it tries
(entries and two).

### 3.4 `has_feature(font, tag)`

| Exit | Condition | Result |
|---|---|---|
| F1 | no table of substitutions, or its 10-byte header is not inside it, or it has no feature list, or the list's count or its records (6 bytes each) are not inside it | false |
| F2 | no record has the tag | false |
| F3 | one has | true |

Cycle: one loop over the feature records (at most 65,535).

### 3.5 `advance_of(font, glyph)`

| Exit | Condition | Result |
|---|---|---|
| A1 | the font has not the glyph, or no advances, or `hmtx` is shorter than its advances (4 bytes each) | 0 |
| A2 | otherwise | the glyph's advance, or the last one's for a glyph past the last, 0 to 65535 |

No cycle.

### 3.6 `load_glyph`, `load_scaled_glyph`, `to_segments`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| L1 | the font has not the glyph (and for the scaled one, a scale not above 0) | the outline emptied | invalid_argument |
| L2 | TrueType outlines | sub-graph `load_truetype_glyph` | its result |
| L3 | CFF outlines | sub-graph `load_cff_glyph` (then `scale_outline` for the scaled one) | its result |
| L4 | neither | the outline emptied | invalid_argument |

The two sub-graphs' exits are not yet tabled (section 6). Their cycles and
bounds:

- `load_truetype_glyph`: one loop of at most `3 * FontLimits::components + 8`
  turns (each begins a glyph, reads a part or ends a level), over a stack of
  16 levels of which `component_depth` (8 by default) are used; a simple
  glyph's flags and
  coordinates are two passes over at most `FontLimits::points` points. A
  glyph that contains itself runs into the depth or the count of parts and
  is refused (limit_exceeded).
- `load_cff_glyph`: one loop of at most `FontLimits::charstring_steps`
  operators (2^20 by default) over a stack of 10 subroutine calls and a
  fixed stack of numbers; points and contours within `FontLimits`.
- `to_segments`: one pass over the points.

An outline that is given (success) is whole: as many tags as points, the
contours' last points going up and ending at the last point, within the
limits (`outline_whole`; checked at these exits in a checked build, and by
`stxlib_glyph_untrusted_tests` on the 415,410 outlines it loads from 30,744
altered fonts).

## 4. Exits: the glyph store and the face

These allocate, through `Vector` only; "out of memory" is the allocator's
refusal and is an exit of every one of them that grows an array.

### 4.1 `GlyphStore::glyph(font, glyph, stored)` (and `keep`, the same per character)

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| T1 | the store was not opened for a font of this many glyphs, or the font has not the glyph | `stored` emptied | invalid_argument |
| T2 | the glyph is remembered as refused | `stored` emptied | the refusal's kind, as remembered |
| T3 | held in the kept part, or in the newer half | none in the store | success; `stored` views into the store |
| T4 | held in the older half | copied to the working outline; then as T8 | success |
| T5 | held in the older half, and the working outline cannot grow | none in the store | out_of_memory |
| T6 | not held; `load_glyph` refuses with other than out of memory | `glyphs_read` + 1; the index remembers the refusal | the refusal |
| T7 | not held; `load_glyph` is out of memory | `glyphs_read` + 1 | out_of_memory; the glyph stays unread |
| T8 | loaded or copied; fits a half | if the newer half has no room the older is given up (its glyphs' index entries set to nowhere) and becomes the newer; the outline is appended there and the index places it | success; `stored` views into the store |
| T9 | loaded; larger than a half | `glyphs_read` + 1; nothing kept, the index says nowhere | success; `stored` views the working outline, until the next call |
| T10 | the append of T8 runs out of memory | the part appended to is cut back to what it held; a half given up just before stays given up, so a glyph that was in it is nowhere and will be read again | out_of_memory |

`keep` is T1, then for each character with a glyph the same with the kept
part tried first (while it has room); it stops at out of memory and passes
over a glyph's own refusal.

Cycles: copying an outline (its points and contours); emptying a half (its
records). No search: the index is by glyph number.

Carried state (`GlyphStore::whole`): in each part the records' outlines lie
end to end and fill the arrays; the bytes counted are the bytes held and
within the part's limit; every glyph the index places is that record's
glyph. A record whose glyph has since moved is left where it was until its
half is emptied; it is in no index entry.

### 4.2 `Typeface::fit(glyph, size, fit, outline, advance)`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| Y1 | not opened; the font has not the glyph; size outside 1 to 2^24 | `outline` and `advance` emptied | invalid_argument |
| Y2 | unfitted, TrueType | under the lock: `load_scaled_glyph` into `outline` | its result |
| Y3 | otherwise, the store's exit is a refusal (T2, T5 to T7, T10) | under the lock: the store's writes | the store's refusal |
| Y4 | unfitted, CFF | under the lock: the stored outline scaled into `outline` | success, or out_of_memory |
| Y5 | hinted | under the lock: `Hinter::prepare` if not yet (once for the typeface); sub-graph `Hinter::hint_stored` | its result |

The lock is taken once and released on every exit (`ScopedLock`). Nothing
called under it takes another lock or calls out of the glyph code but to the
allocator. `ink_box` and `keep` are the same shape.

### 4.3 `Face::fitted`, `mask`, `measure_run`, `draw_run`

| Exit | Condition | Writes before it | Result |
|---|---|---|---|
| R1 | the face is not opened | the result emptied | invalid_argument |
| R2 | a style that cannot be drawn in (size or thickening out of range; a matrix with a hinted fit) | the result emptied | invalid_argument |
| R3 | `fitted`, `mask`: the glyph cannot be had (the font has not it; `Typeface::fit`, `to_segments` or `measure` refuses) | none kept | the refusal |
| R4 | a run: a glyph cannot be had or measured, other than for memory | `missing_glyphs` + 1; the run goes on | (not an exit) |
| R5 | a run with nothing to show (no glyphs, or all missing, empty or outside the window) | counts only; an empty box, no pixels | success |
| R6 | a run's box is wider or taller than the mask limit, or lies beyond 2^30 pixels from the origin | `oversized` set; an empty box, no pixels | success |
| R7 | out of memory at any growth | fitted outlines already held stay whole; `draw_run`'s `coverage` may hold its box and the glyphs drawn before | out_of_memory |
| R8 | otherwise | fitted outlines newly made are held (two halves, as the store's); for `draw_run`, `coverage` is its box's width times height bytes | success |

In `draw_run`'s second pass a glyph is had again (it may have left the
fitted outlines since the first), and a refusal there is an exit with the
refusal; only memory can refuse what the first pass was given.

Cycles: a run's glyphs, twice (placing, then drawing); for each, a search of
a table by open addressing, which ends at an empty slot because a table is
never more than half full (`Face::whole` checks that it is so); a glyph's
points (the matrix, the thickening: for a point, a walk to the nearest
different point of its contour, at most the contour's length); a mask's
rows and columns.

Carried state (`Face::whole`): in each half the fitted outlines lie end to
end; each has as many points as its segments need; each is found by its
glyph, size and fit at its own place; the table is a power of two, no more
than half full, and holds each record once.

## 5. The hinter and the rasterizer: cycles and bounds

Not tabled exit by exit yet (section 6). Read off the source:

- `Hinter::prepare`: for each of the 433 character ranges of the table of
  scripts, a walk of the character map by stretches, at most the range's
  length in characters; then 935 look-ups for the characters whose accents
  are cared for. `hint_stored`: passes over the glyph's points, its segments
  (at most 1,001 an axis; more and the axis has none, and so no edges) and
  its edges; the pairing of segments and
  the gathering of edges are loops inside loops over those, so at most a
  million steps an axis; a style's measures, taken once, load at most the 51
  characters of a blue string and the standard characters.
- `measure`, `render`, `render_monochrome`: passes over the outline's
  segments, each curve cut into chords by a rule with a fixed depth (a
  quadratic into 2^k chords at equal steps, k from its size and at most
  16, a cubic by halving at most 16 times), and over the mask's cells; a mask larger than
  `Limits::work_cells` is drawn a band of rows at a time, the chords gone
  through once for each band.

## 6. What this file does not yet do

- The exits of `load_truetype_glyph`, `load_cff_glyph`, the hinter's stages
  and the rasterizer are not tabled. For those, the evidence is the tests
  (the bounded search of altered fonts, the comparison with FreeType on every
  bundled glyph), the sanitizer builds and the checks of carried state, not a
  reading written down here.
- "No requirement of a vocabulary type can be broken by a font" (section 2)
  is argued from the reads being checked first; it is not proved.
- The layout tables and the shaper's buffer are not in the entries: they are
  internal until the shaper is.
- The surface check's Microsoft spellings are written from the mangling rules
  and have not been run on that toolchain.
