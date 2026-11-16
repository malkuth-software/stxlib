# stxlib::text bidi: design, differences from SheenBidi, GUI.Forms swap

`include/stxlib/bidi.hpp` implements UAX #9 at Unicode 18.0.0 from the
specification. SheenBidi 2.9.0 (stx_text's vendored copy, commit
`829d99918adf05761e9f35611dc13e76e6810f80`) served only as the oracle.

## Design

- **First pass.** One scan per paragraph classifies each code point. It
  keeps three things in locals: a running OR of the classes present, the
  first strong direction, and the paragraph end (CR LF together). No
  counter in memory.
- **Choosing a form.** The form is settled once, from what the scan found:
  - **uniform**: base level 0 with no R, AL or AN, or base level 1 with no
    L, EN or AN, and no explicit formatting characters. Every level is the
    base level, so only the levels are written.
  - **implicit**: no explicit formatting characters. The whole paragraph is
    one isolating run sequence.
  - **explicit**: X1 to X10 per code point, then each isolating run
    sequence.
- **Rules over runs.** W1 to I2 run over a list of runs: one class within
  one level run, with X9's removed characters absorbed, and each bracket
  candidate a run of its own. They do not run over code points. Each rule
  is a fixed pass that keeps its state in locals.
- **Lines.** Rule L1 is skipped for uniform paragraphs. Rule L2 reorders
  runs, not characters.
- **Measured distribution.** `tools/bidi_case_histogram.py` was run over
  GUI.Forms' sources, tests, benchmarks and documents (2026-11-16):
  - uniform: 38,560,189 code points;
  - implicit: 909 code points;
  - explicit: none.
- **Reference.** `tests/reference/bidi_reference.hpp` is a per-code-point
  reading of the specification. It is compared with the library exactly.

## Choices the specification leaves open

- **P1.** CR LF is one paragraph separator, as in SheenBidi.
- **X9.** A removed character takes the level of the nearest preceding
  character that is not removed. If there is none, it takes the level of
  the nearest following one. If there is none either, it takes the
  paragraph level. L1 treats removed characters as white space.
- **BD16.** Pairing stops at the 64th opener, and the pairs already found
  stand.
- **Levels.** Levels are given per code unit, as in SheenBidi.

## Differences from SheenBidi

For every one of BidiCharacterTest.txt's 91,707 cases, `tests/bidi_bench.cpp`
compared the two on:
- the paragraph level;
- the level of every character that X9 does not remove.

The comparison found no difference. Where they can still disagree:
- **Levels of removed characters.** UAX #9 leaves these unassigned
  (BidiTest's "x"). The two were not compared there.
- **Code points whose Bidi_Class changed between SheenBidi's Unicode version
  and 18.0.0.** These follow 18.0.0 here.
- **Invalid UTF-8.** SheenBidi decodes it with replacement. STXlib takes
  only a validated `TextView`, so GUI.Forms validates first.

## GUI.Forms swap note

GUI.Forms' calls are in:
- `render/text/harfbuzz/harfbuzz_font_engine.cpp`: `visual_direction_runs`
  and the shaping loop;
- `render/text/text_mask/text_mask_shape.cpp`;
- `render/text/text_mask/text_mask_native.hpp`.

Each SheenBidi call maps to STXlib as follows.

| SheenBidi | STXlib |
|---|---|
| `SBCodepointSequence{UTF8, data, size}` + `SBAlgorithmCreate` | `stxlib::validate_utf8(data, size)` once, giving a `TextView` |
| `SBAlgorithmCreateParagraph(algorithm, offset, size - offset, SBLevelDefaultLTR)` | `analyse_paragraph(text, offset, BaseDirection::automatic_left_to_right, paragraph)` |
| `SBParagraphGetLength` | `paragraph.length()` |
| `SBParagraphCreateLine(paragraph, offset, length)` | `analyse_line(paragraph, offset, length, line)` |
| `SBLineGetRunsPtr` / `SBLineGetRunCount`, `SBRun{offset, length, level}` | `line.runs()`, `BidiRun{offset, length, level}`; offsets are bytes of the whole text, as in SheenBidi |
| `SBParagraphGetLevelsPtr` | `paragraph.levels()` |
| `SBLineRelease`, `SBParagraphRelease`, `SBAlgorithmRelease` | none: keep one `BidiParagraph` and one `BidiLine` and reuse them; they allocate only when they grow |
| a null result (`std::bad_alloc` in GUI.Forms) | a failed `Status` (`out_of_memory`, `out_of_range`, `invalid_argument`) |
| `SBMirrorLocator` (not used by GUI.Forms) | `bidi_mirroring_glyph`, `bidi_mirrored`, applied to the code points at odd levels |

The paragraph loop in `harfbuzz_font_engine.cpp` keeps its shape:
1. Advance `offset` by `paragraph.length()`.
2. `length == 0` cannot occur: a paragraph holds at least one code point.

**Not covered: script itemisation.** `text_mask_shape.cpp` uses SheenBidi's
`SBScriptLocator*` and `SBScriptGetUnicodeTag`, which run UAX #24 over
Scripts.txt. That is outside bidi. Until STXlib has a script itemiser,
those calls keep GUI.Forms on SheenBidi.
