# stxlib::textlayout: design, differences from the libraries it replaces, GUI.Forms swap

`include/stxlib/textlayout.hpp` analyses a paragraph of text once for the
three things a layout needs before shaping: bidirectional levels (UAX #9),
line-break opportunities (UAX #14) and script runs (UAX #24), all at
Unicode 18.0.0 and all written from the specifications. It replaces three
things GUI.Forms calls today:

| Replaced | By |
|---|---|
| SheenBidi 2.9.0, the bidirectional algorithm | `analyse_paragraph`, `analyse_line` |
| SheenBidi's `SBScriptLocator` | `Paragraph::script_runs()` |
| libunibreak 8.0, `set_linebreaks_utf8` | `Paragraph::breaks()`, or `find_line_breaks` for a text without bidi |

The two libraries served only as oracles (stx_text's vendored copies;
SheenBidi at commit `829d99918adf05761e9f35611dc13e76e6810f80`). Where an
oracle and the specification differ, the specification wins, and the
difference is listed below.

## Design

### One table, one walk

- **One record per code point.** A two-stage table gives 32 bits for any
  code point: Bidi_Class, the paired-bracket and mirrored flags, the
  line-break symbol, the Script and the Script_Extensions set. One lookup
  serves the three analyses. ASCII reads the first block directly.
  `tools/gen_textlayout_tables.py` writes the table from the nine pinned
  Unicode files and checks it; the test reads the same files with parsers
  of its own and compares every code point.
- **One walk per paragraph.** `analyse_paragraph` reads the text once, from
  the offset to the paragraph separator. For each code point it:
  1. takes the record;
  2. steps the line-break automaton and stores the decision for the
     boundary before the code point;
  3. adds the bidi class to the set of classes present and notes the first
     strong direction;
  4. calls the script itemiser only when the code point's script differs
     from the run's or the code point is a bracket.
  All of its state is in locals. The break decisions also mark where code
  points start, so no separate index of code units is built for uniform
  paragraphs.
- **One entry per line.** `analyse_line` applies rules L1 and L2 to a range
  of the paragraph and gives the runs in visual order.

### The forms, each with its own loop

The method is that of the performance focus study: measure which forms the
input takes, give each form a list or a loop of its own with a fixed body,
settle the choice of form once per run and not per item, and keep no
counter in memory that the data indexes. The measurements came first.

- **Line breaking** (`tools/gen_textlayout_tables.py --stats` over the
  CommonMark specification and this repository's documents, 224,591 code
  points): AL 78.8%, SP 10.7%, LF 4.5%, IS 2.0% of the code points; the
  pairs AL AL 65.0%, SP AL 8.9%, AL SP 8.0%; "no break" 84.7% of the
  decisions, "allowed" 10.8%, "mandatory" 4.5%, decided by look-ahead
  0.011%. An ASCII letter is met in the automaton's letter state 43.6% of
  the time. Two forms follow:
  - **letters**: a run of ASCII letters after its first. Every boundary
    inside is "no break" and the automaton stays in one state (the
    generator proves both for every state), so the run is one search for
    its end and one fill.
  - **general**: one code point: the record, one table entry holding the
    decision and the next state, one store.
- **Bidi** (`tools/bidi_case_histogram.py` over GUI.Forms' sources, tests,
  benchmarks and documents, 2026-11-16): uniform 38,560,189 code points,
  implicit 909, explicit none. The form is settled once per paragraph from
  what the walk found:
  - **uniform**: base level 0 with no R, AL or AN, or base level 1 with no
    L, EN or AN, and no explicit formatting characters. Every level is the
    base level, so the levels are one fill.
  - **implicit**: no explicit formatting characters. The whole paragraph is
    one isolating run sequence.
  - **explicit**: X1 to X10 per code point, then each isolating run
    sequence.
  Rules W1 to I2 run over a list of runs (one class within one level run,
  X9's removed characters absorbed, each bracket candidate a run of its
  own), not over code points; each rule is a fixed pass with its state in
  locals. L1 is skipped for uniform paragraphs. L2 reorders runs.
- **Scripts.** Most code points continue the run (same script, or Common or
  Inherited and no bracket): one comparison in the walk. The others are an
  event handled out of line.

### The line-break rules

- **The rule chain.** Rules LB2 to LB31 are stated once as a function from
  a small state and the next symbol to a decision
  (`src/algorithms/textlayout/line_break_chain.hpp`; the same statement in
  Python in the generator). LB1 is resolved in the table: AI, SG and XX
  are AL, CJ is NS, SA is CM for General_Category Mn and Mc and AL
  otherwise. LB9 and LB10 are part of the state: a combining mark or ZWJ
  takes the class of its base, or is AL without one.
- **Symbols.** The rules need more than the class of a code point: whether
  a quotation mark is initial or final punctuation (LB15a, LB15b, LB19a),
  whether it is East Asian wide (LB19a, LB30), U+25CC (LB28a), and an
  unassigned Extended_Pictographic code point (LB30b). Class and these
  together give 62 symbols.
- **The automaton.** The generator explores every state the chain can
  reach from the start of text (6,749), merges the states no continuation
  can tell apart, and writes the result: 103 states, one 16-bit entry per
  state and symbol holding the decision and the next state. The test
  proves the automaton equal to the chain over every reachable state
  (proofs/REPORT.md).
- **Look-ahead.** Five rules depend on what follows: LB15b and LB19a (the
  class after a quotation mark), LB15c and LB25 (a number after a
  separator or an opening bracket), LB28a (a virama after an aksara). The
  automaton marks those entries; the walk then reads forward to the next
  one or two bases, passing over combining marks. Each code point is read
  at most three times in all. Nothing already written is revisited.
- **Paragraphs.** In `analyse_paragraph` the unit is the paragraph of
  UAX #9: the start of the paragraph is the start of text for the rules,
  its end is mandatory (LB3), and the look-ahead does not read past it.
  `find_line_breaks` treats its whole text as one unit. The two agree on
  every paragraph taken alone (tested).

### The script rules

A script run is a longest stretch of text that one script can shape.
- Common and Inherited code points continue the current run.
- The first code point of a real script settles a run that had none yet,
  and the brackets opened in it.
- A code point of another script continues the run when the run's script
  is in its Script_Extensions; otherwise it starts a new run.
- A closing bracket (Bidi_Paired_Bracket, Common or Inherited) takes the
  script that was current at its opening bracket, as UAX #24 section 5.2
  recommends. At most 32 brackets are open at once; one more forgets the
  earlier ones.
- Each paragraph starts afresh.
`script_tag` gives the ISO 15924 code of a script as a four-byte tag, which
is the form HarfBuzz's `hb_script_from_iso15924_tag` takes.

## Choices the specifications leave open

- **P1 (UAX #9).** CR LF is one paragraph separator, as in SheenBidi.
- **X9.** A removed character takes the level of the nearest preceding
  character that is not removed. If there is none, it takes the level of
  the nearest following one. If there is none either, it takes the
  paragraph level. L1 treats removed characters as white space.
- **BD16.** Pairing stops at the 64th opener, and the pairs already found
  stand.
- **Levels and breaks** are given per code unit of the input. A decision
  stands at the last code unit of its code point; the code units before it
  hold `inside_character`.
- **LB3.** The end of the text, and of each paragraph, is `mandatory`.
- **Tailoring (UAX #14).** None: the default algorithm. CB is treated as
  the default rules treat it (LB20). SA text gets no dictionary: it is AL
  or CM by LB1.
- **Input that is not a scalar value** (the code-point entries only) is
  analysed as U+FFFD.

## Conformance and speed (M4, macOS arm64, Clang, 2026-11-16)

| Suite | Result |
|---|---|
| BidiTest.txt | 770,241 of 770,241 cases |
| BidiCharacterTest.txt | 91,707 of 91,707 cases |
| LineBreakTest.txt | 19,346 of 19,346 cases (the rule chain, `find_line_breaks` over code points and over UTF-8) |
| the automaton against the rule chain | 6,749 chain states, 729,781 decisions, 0 differ (exhaustive) |

Speed, Release, best of 15 over 4 MiB of each text, nanoseconds per byte.
"One pass" is `analyse_paragraph` and `analyse_line` per paragraph (levels,
runs, line breaks, script runs), with `validate_utf8` of the text included.
"Three libraries" is SheenBidi per paragraph with one line each, then
`set_linebreaks_utf8`, then `SBScriptLocator`, as GUI.Forms calls them.

| Text | One pass | Three libraries (bidi + breaks + scripts) | Faster by | Against SheenBidi alone |
|---|---|---|---|---|
| ASCII | 3.06 | 31.41 (17.32 + 10.43 + 3.65) | 10.3x | 5.7x |
| Latin and CJK | 3.30 | 28.76 (17.77 + 7.54 + 3.45) | 8.7x | 5.4x |
| Hebrew | 8.89 | 28.90 (18.56 + 7.75 + 2.59) | 3.3x | 2.1x |
| mixed | 9.26 | 30.30 (18.17 + 8.62 + 3.52) | 3.3x | 2.0x |
| explicit | 10.09 | 28.62 (18.15 + 7.21 + 3.26) | 2.8x | 1.8x |

`find_line_breaks` alone against `set_linebreaks_utf8` alone: 1.76 against
10.43 (5.9x) on ASCII, 1.84 against 7.54 (4.1x) on Latin and CJK, 2.50
against 7.75 (3.1x) on Hebrew, 2.02 against 8.62 (4.3x) on mixed, 1.83
against 7.21 (3.9x) on explicit. Validation is 0.75 to 1.31 of each
stxlib figure; a caller that already holds a `TextView` does not pay it
again.

Before the three analyses shared one walk, the bidi analysis alone was
7.9x SheenBidi on ASCII and 2.1x to 2.4x on the right-to-left texts. The
last column is lower because the same pass now also finds the line breaks
and the script runs. Paragraphs that are not uniform still read their
records a second time for the bidi rules; removing that second read is
the known next step for the right-to-left texts.

## Differences from SheenBidi (bidi)

For every one of BidiCharacterTest.txt's 91,707 cases,
`tests/textlayout_bench.cpp` compared the paragraph level and the level of
every character that X9 does not remove: no difference. Where the two can
still disagree:
- **Levels of removed characters.** UAX #9 leaves these unassigned
  (BidiTest's "x"). The two were not compared there.
- **Code points whose Bidi_Class changed between SheenBidi's Unicode version
  and 18.0.0.** These follow 18.0.0 here.
- **Invalid UTF-8.** SheenBidi decodes it with replacement. STXlib takes
  only a validated `TextView`.

## Differences from libunibreak 8.0 (line breaking)

libunibreak 8.0 carries Unicode 17.0.0 data and UAX #14 revision 55.
`tests/textlayout_bench.cpp` compared `find_line_breaks` with
`set_linebreaks_utf8(..., "-strict", ...)` on LineBreakTest.txt (18.0.0)
and on 2,000,000 random texts of 1 to 24 code points drawn from every
class (25,012,172 boundaries). Each text that differs is counted once,
under its first difference.

| Difference | LineBreakTest | Random texts | Which is right |
|---|---|---|---|
| The end of the text: libunibreak gives "indeterminate" unless the text ends in a hard break; STXlib gives `mandatory` | 18,242 | 1,957,528 | LB3. GUI.Forms already forces "must break" there |
| A code point of the text has another Line_Break class in 17.0.0 than in 18.0.0 | 22 | 5,145 | the data: 18.0.0 |
| `BA × GL`: no break in 18.0.0 (LB12a no longer excepts BA) | 8 | 7,967 | LineBreakTest 18.0.0, rule 12.1 |
| libunibreak's own tailoring: no break inside "++" (PR before "+") or between "-" and "`" | 0 | 16,447 | not in UAX #14; STXlib allows the break the default rules give |
| Quotation marks (LB15a, LB15b, LB19, LB19a) | 0 | 11,896 | see below |
| `SP ÷ IS` only before a number (LB15c) | 0 | 1,034 | see below |
| `(PR | PO) × OP NU` (LB25) | 0 | 246 | see below |
| Other | 0 | 5,264 | not examined one by one (below) |

The quotation, LB15c and LB25 rows are the rules whose answer depends on
what follows the boundary. Examples, with the reading taken here:
- `4E00 2018 201D`: an ideograph, an opening quotation mark, a closing
  one. LB19a keeps an opening quotation mark with what precedes it when
  what follows it is not East Asian. U+201D is not, so there is no break
  before U+2018; libunibreak breaks.
- `0020 201D 094D 002C`: LB15b keeps a closing quotation mark with a
  space before it when a separator follows. A combining mark stands
  between; by LB9 it belongs to the quotation mark, so the rule applies;
  libunibreak looks at the mark itself and breaks. The 5,864 differences
  counted with combining marks near are mostly of this kind.
- `0020 002E 201D`: LB15c allows a break between a space and a decimal
  mark only before a digit. No digit follows, so LB13 keeps them together;
  libunibreak breaks.
- `005C 0028 0031`: a prefix, an opening bracket, a digit: LB25 keeps
  them together; libunibreak breaks in some of these contexts.

In all four the rule chain is the text of the rule, the automaton is
proved equal to the chain, and the chain passes every case of
LineBreakTest 18.0.0; libunibreak passes its own 17.0.0 file, so these
sequences are ones neither file holds. They were decided here by reading
the rules, not by a third implementation: that is the open point of this
list. The 5,264 "other" texts were counted but their kinds were not
printed by the run recorded here (the program now prints the first of
each cause); many follow an earlier look-ahead difference in the same
text.

GUI.Forms passes "-strict", which in libunibreak only changes CJ (to NS,
as LB1 says). That is what STXlib does.

## Differences from SheenBidi's script locator

SheenBidi 2.9.0 carries older Script data and no Script_Extensions.
- **Script property**: 17,810 code points have a script here and Unknown
  in SheenBidi (new characters and scripts of later Unicode versions:
  11,328 Seal, 4,322 Han, 965 Jurchen, and others); no code point has
  two different known scripts in the two.
- **Runs**: of 2,000,000 random texts of 1 to 16 code points from every
  script, 1,584,546 give identical runs. 415,011 differ and hold a code
  point whose Script differs between the data sets; 443 differ and hold a
  code point with Script_Extensions (STXlib keeps such a character in the
  run when the run's script is in its set; SheenBidi starts a new run or
  not by the Script alone). None differs for another reason, brackets
  included.
- **Paragraphs**: SheenBidi's locator runs over the whole text; script
  runs here restart at each paragraph.
- The ISO 15924 codes are written in the generator. The comparison
  confirms every code SheenBidi knows; those of the scripts it does not
  know (among them Jurc and Seal) rest on ScriptExtensions.txt where it
  names them, and otherwise on the author's reading of ISO 15924.

## Memory

A `Paragraph` keeps its storage grown to the largest paragraph it has held
(nothing is allocated per character). What one paragraph makes it hold
(the security audit of 2026-11-16, A-stxlib-textlayout-01-F3):

| Storage | Per | Bytes |
|---|---|---|
| levels | code unit | 1 |
| breaks (doubling) | code unit | 1 to 2 |
| classes | code point, not one level | 1 |
| code unit offsets | code point, not one level | 4 |
| working bytes | code point | 3 implicit, 6 explicit |
| working words | code point | 20 implicit, 40 explicit |
| bracket keys | bracket | 8 |
| script runs (doubling) | script run | 12 to 24 |

So about 6 bytes per byte of text when the paragraph is one level
throughout, 31 to 39 with a right-to-left character, and up to about 62
with an explicit formatting character. The input's only bound is 2^32 - 2
bytes: a caller that analyses text from outside bounds a paragraph's
length itself, and `Paragraph::release()` (and `Line::release()`) gives the
storage back. A moved-from paragraph or line is empty.

## GUI.Forms swap note

GUI.Forms' calls are in:
- `render/text/harfbuzz/harfbuzz_font_engine.cpp`: `visual_direction_runs`,
  the shaping loop, and `set_linebreaks_utf8`;
- `render/text/text_mask/text_mask_layout.cpp`: `set_linebreaks_utf8`;
- `render/text/text_mask/text_mask_shape.cpp`: SheenBidi and
  `SBScriptLocator`;
- `render/text/text_mask/text_mask_native.hpp`.

The swap is one analysis per paragraph where there are three today:
validate once, then loop `analyse_paragraph` from offset 0, advancing by
`paragraph.length()`, and read levels, breaks and script runs from the one
`Paragraph`. Keep one `Paragraph` and one `Line` and reuse them; they
allocate only when they grow.

| Today | STXlib |
|---|---|
| `SBCodepointSequence{UTF8, data, size}` + `SBAlgorithmCreate` | `stxlib::validate_utf8(data, size)` once, giving a `TextView` |
| `SBAlgorithmCreateParagraph(algorithm, offset, size - offset, SBLevelDefaultLTR)` | `analyse_paragraph(text, offset, BaseDirection::automatic_left_to_right, paragraph)` |
| `SBParagraphGetLength` | `paragraph.length()` |
| `SBParagraphCreateLine(paragraph, offset, length)` | `analyse_line(paragraph, offset, length, line)` |
| `SBLineGetRunsPtr` / `SBLineGetRunCount`, `SBRun{offset, length, level}` | `line.runs()`, `BidiRun{offset, length, level}`; offsets are bytes of the whole text, as in SheenBidi |
| `SBParagraphGetLevelsPtr` | `paragraph.levels()` |
| `SBLineRelease`, `SBParagraphRelease`, `SBAlgorithmRelease` | none |
| a null result (`std::bad_alloc` in GUI.Forms) | a failed `Status` (`out_of_memory`, `out_of_range`, `invalid_argument`, `limit_exceeded`) |
| `SBMirrorLocator` (not used by GUI.Forms) | `bidi_mirroring_glyph`, `bidi_mirrored`, applied to the code points at odd levels |
| `SBScriptLocatorCreate`, `LoadCodepoints`, `MoveNext`, `GetAgent` → `{offset, length, script}` | `paragraph.script_runs()`: `ScriptRun{offset, length, script}`, offsets in bytes of the whole text |
| `SBScriptGetUnicodeTag(script)` → `hb_script_from_iso15924_tag` | `script_tag(run.script)` → `hb_script_from_iso15924_tag` |
| `set_linebreaks_utf8(source, size, "-strict", out)` | `paragraph.breaks()` per paragraph; or `find_line_breaks(out, text)` over the whole text where no bidi is wanted |
| `LINEBREAK_MUSTBREAK` / `ALLOWBREAK` / `NOBREAK` / `INSIDEACHAR` | `LineBreak::mandatory` / `allowed` / `prohibited` / `inside_character` (the numeric values differ: compare by name) |
| the last decision forced to "must break" by GUI.Forms | already `mandatory` (LB3); the forcing can go |

Points to check in GUI.Forms when swapping:
1. `paragraph.breaks()` is indexed from the paragraph's first code unit,
   not from the start of the text: the decision after byte `i` of the text
   is `paragraph.breaks()[i - paragraph.offset()]`.
2. The script locator ran over the whole text; script runs now restart at
   each paragraph. A run that used to span a paragraph separator becomes
   two.
3. The differences listed above change some break opportunities and some
   script runs. Each is the specification's answer; none needs handling.
4. `length == 0` cannot occur: a paragraph holds at least one code point.
5. Invalid UTF-8: the two libraries decode it with replacement. STXlib
   takes only a validated `TextView`, so GUI.Forms validates first and
   decides there what invalid text becomes.

After the swap GUI.Forms links neither SheenBidi nor libunibreak.
