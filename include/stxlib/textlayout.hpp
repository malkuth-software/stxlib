#pragma once

// stx_textlayout: what a paragraph of text needs before it can be laid out
// in lines, from one pass over it. Unicode 18.0.0.
//
// - Direction (UAX #9, the Unicode Bidirectional Algorithm): the paragraph's
//   base level and the embedding level of each code unit (rules P2 to I2,
//   the paired brackets of N0 included); per line, the levels after rule L1
//   and the runs in visual order (L2).
// - Line breaking (UAX #14): for each code unit, whether a line may, must
//   or may not end after it (rules LB2 to LB31, no tailoring: AI, SG and XX
//   are AL, CJ is NS, SA is AL or, for its marks, CM).
// - Scripts (UAX #24): the paragraph's runs of one script, Common and
//   Inherited characters taking the script of their run, a closing bracket
//   the script of its opening bracket, and a character continuing a run
//   whose script is among its Script_Extensions.
//
// Replaces SheenBidi 2.9.0 and libunibreak 8.0 in GUI.Forms; written from
// the specifications, with the two libraries as oracles. Where they differ
// the specification is followed; docs/TEXTLAYOUT.md lists each difference.
//
// One table (tools/gen_textlayout_tables.py, from the files pinned in
// PROVENANCE.md under third_party/unicode-18.0.0/; Unicode License v3)
// gives one packed record per code point: Bidi_Class, paired bracket,
// Bidi_Mirrored, the line-break symbol (class after LB1, the quotation
// mark's kind, East Asian width, unassigned Extended_Pictographic), Script
// and Script_Extensions. analyse_paragraph reads it once per code point;
// the line-break automaton, the classes present, the first strong
// direction and the script runs all advance in that one walk. A paragraph
// that turns out to hold both directions is then resolved by the bidi
// rules over its run list, which reads the records of its code points a
// second time.
//
// Shaping, mirroring of glyphs (L4 states which characters mirror: an odd
// level and Bidi_Mirrored), the combining marks of L3, widths, hyphenation
// and the choice among the allowed breaks are the caller's.
//
// In proof scope (proofs/REPORT.md, section stx_textlayout): BidiTest.txt,
// BidiCharacterTest.txt and LineBreakTest.txt in full; the bidi engine
// against an independent reading of UAX #9 on every sequence of classes up
// to a stated length; the line-break automaton against the rule chain on
// every reachable state.
//
// Choices the specifications leave to the implementation, made here:
// - P1: CR followed by LF ends one paragraph, both kept with it; every
//   other Bidi_Class B ends a paragraph after itself. A paragraph is the
//   unit of all three analyses: the line-break rules see its start as the
//   start of text and its end as the end of text (LB3: a mandatory break).
//   That is what the rules give for the whole text wherever the paragraph
//   ends in a line terminator; after U+001C to U+001E, which end a
//   paragraph without ending a line, it is this library's choice.
// - X9: removed characters (BN and the explicit embeddings, overrides and
//   pops) are given a level all the same, so every code unit has one: the
//   level of the nearest preceding character that is not removed; at the
//   paragraph's start, of the nearest following one; in a paragraph of
//   removed characters only, the paragraph level. Rule L1 then treats them
//   as it treats white space.
// - Levels, breaks and run offsets are per code unit of the input (a byte
//   of UTF-8, one element of a code point array), so a caller that keeps
//   byte offsets needs no conversion.
// - Scripts: brackets are those of Bidi_Paired_Bracket; at most 32 are
//   open at once (a 33rd forgets the ones before it).
//
// Memory: a Paragraph and a Line own their storage, grown to the largest
// paragraph or line they have held and reused; nothing is allocated per
// character. What one paragraph holds is linear in its length but not
// small: about 6 bytes per byte of text when it is one level throughout,
// 31 to 39 with a right-to-left character, and up to about 62 with an
// explicit formatting character (A-stxlib-textlayout-01-F3; the table is in
// docs/TEXTLAYOUT.md). The only bound is that of the input (2^32 - 2
// bytes), so a caller that takes text from outside bounds a paragraph
// itself before analysing it; release() gives the storage back. A failure (out_of_memory when storage cannot grow,
// invalid_argument or out_of_range for a bad offset) leaves the object
// empty. No function here uses a host call, a locale or a hidden state;
// every one is noexcept.

#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib::textlayout {

// The Unicode version of the tables.
inline constexpr u32 unicode_major = 18;
inline constexpr u32 unicode_minor = 0;
inline constexpr u32 unicode_update = 0;

// Bidi_Class (UAX #9 table 4), in the order of the table; the short name
// of each value is in its comment.
enum class BidiClass : u8 {
    left_to_right = 0,               // L
    right_to_left = 1,               // R
    arabic_letter = 2,               // AL
    european_number = 3,             // EN
    european_separator = 4,          // ES
    european_terminator = 5,         // ET
    arabic_number = 6,               // AN
    common_separator = 7,            // CS
    nonspacing_mark = 8,             // NSM
    boundary_neutral = 9,            // BN
    paragraph_separator = 10,        // B
    segment_separator = 11,          // S
    white_space = 12,                // WS
    other_neutral = 13,              // ON
    left_to_right_embedding = 14,    // LRE
    left_to_right_override = 15,     // LRO
    right_to_left_embedding = 16,    // RLE
    right_to_left_override = 17,     // RLO
    pop_directional_format = 18,     // PDF
    left_to_right_isolate = 19,      // LRI
    right_to_left_isolate = 20,      // RLI
    first_strong_isolate = 21,       // FSI
    pop_directional_isolate = 22,    // PDI
};

inline constexpr u32 bidi_class_count = 23;

// The Bidi_Class of one code point; for a value that is not a scalar value
// (a surrogate, or above 0x10FFFF), left_to_right, as the tables give
// unassigned code points outside the right-to-left blocks.
[[nodiscard]] BidiClass bidi_class(u32 code_point) noexcept;

// Whether the code point is Bidi_Mirrored: shown with a mirrored glyph at
// an odd level (rule L4).
[[nodiscard]] bool bidi_mirrored(u32 code_point) noexcept;

// The Bidi_Mirroring_Glyph of the code point (U+0028 for U+0029), or the
// code point itself when it has none: a Bidi_Mirrored character without
// one (U+221B CUBE ROOT) is the font's to mirror.
[[nodiscard]] u32 bidi_mirroring_glyph(u32 code_point) noexcept;

// Bidi_Paired_Bracket_Type and Bidi_Paired_Bracket (BidiBrackets.txt).
enum class BracketType : u8 {
    none = 0,
    open = 1,
    close = 2,
};

struct BidiBracket final {
    // The paired bracket (U+0029 for U+0028); the code point itself when
    // type is none.
    u32 pair{0};
    BracketType type{BracketType::none};
    u8 reserved[3]{0, 0, 0};
};

[[nodiscard]] BidiBracket bidi_paired_bracket(u32 code_point) noexcept;

// The deepest embedding level (BD2): 125.
inline constexpr u8 bidi_max_depth = 125;

// How the paragraph's base level is chosen. The automatic forms apply
// rules P2 and P3 (the first strong character outside isolates decides)
// and use their direction when the paragraph has none.
enum class BaseDirection : u8 {
    automatic_left_to_right = 0,
    automatic_right_to_left = 1,
    left_to_right = 2,
    right_to_left = 3,
};

// One run of a line: code units [offset, offset + length) of the input,
// all at `level`, which is right to left when odd. Offsets count from the
// start of the whole input, not of the paragraph.
struct BidiRun final {
    u32 offset{0};
    u32 length{0};
    u8 level{0};
    u8 reserved[3]{0, 0, 0};
};

static_assert(sizeof(BidiRun) == 12, "stxlib::textlayout: a bidi run is 12 bytes");

// ---------------------------------------------------------------------------
// Line breaking.

// What may happen after one code unit (the boundary between it and the
// next one; after the last, the end of the text).
enum class LineBreak : u8 {
    prohibited = 0,       // no line ends here
    allowed = 1,          // a line may end here
    mandatory = 2,        // a line ends here (after a line terminator, and at the end of the text: LB3)
    inside_character = 3, // the next code unit continues this code point
};

// Line_Break after rule LB1, which leaves 44 of the property's values: AI,
// SG and XX are AL; CJ is NS; SA is CM for General_Category Mn and Mc,
// otherwise AL. The short name of each value is its comment.
enum class LineBreakClass : u8 {
    mandatory_break = 0,          // BK
    carriage_return = 1,          // CR
    line_feed = 2,                // LF
    next_line = 3,                // NL
    space = 4,                    // SP
    zero_width_space = 5,         // ZW
    word_joiner = 6,              // WJ
    glue = 7,                     // GL
    combining_mark = 8,           // CM
    zero_width_joiner = 9,        // ZWJ
    open_punctuation = 10,        // OP
    close_punctuation = 11,       // CL
    close_parenthesis = 12,       // CP
    quotation = 13,               // QU
    nonstarter = 14,              // NS
    exclamation = 15,             // EX
    break_symbols = 16,           // SY
    infix_numeric = 17,           // IS
    prefix_numeric = 18,          // PR
    postfix_numeric = 19,         // PO
    numeric = 20,                 // NU
    alphabetic = 21,              // AL
    hebrew_letter = 22,           // HL
    ideographic = 23,             // ID
    inseparable = 24,             // IN
    hyphen = 25,                  // HY
    unambiguous_hyphen = 26,      // HH
    break_after = 27,             // BA
    break_before = 28,            // BB
    break_both = 29,              // B2
    contingent_break = 30,        // CB
    hangul_lv = 31,               // H2
    hangul_lvt = 32,              // H3
    hangul_l_jamo = 33,           // JL
    hangul_v_jamo = 34,           // JV
    hangul_t_jamo = 35,           // JT
    regional_indicator = 36,      // RI
    emoji_base = 37,              // EB
    emoji_modifier = 38,          // EM
    aksara = 39,                  // AK
    aksara_prebase = 40,          // AP
    aksara_start = 41,            // AS
    virama_final = 42,            // VF
    virama = 43,                  // VI
};

inline constexpr u32 line_break_class_count = 44;

// The class of one code point after LB1; alphabetic for a value above
// 0x10FFFF.
[[nodiscard]] LineBreakClass line_break_class(u32 code_point) noexcept;

// Whether East_Asian_Width is F, W or H (the "East Asian" of rules LB19a
// and LB30).
[[nodiscard]] bool east_asian_wide(u32 code_point) noexcept;

// The break opportunities of a whole text, taken as one piece from the
// start of text to the end of text: output[i] says what may happen after
// byte i. The last entry is mandatory (LB3). invalid_argument, and nothing
// written, unless output.count() == text.count(). Requirement: `output`
// does not overlap `text`. No allocation.
[[nodiscard]] OperationStatus find_line_breaks(MutableView<LineBreak> output, TextView text) noexcept;

// The same over code points, one entry each (inside_character does not
// occur). A value that is not a scalar value is taken as U+FFFD.
[[nodiscard]] OperationStatus find_line_breaks(MutableView<LineBreak> output, View<u32> code_points) noexcept;

// ---------------------------------------------------------------------------
// Scripts.

// The Script property. common, inherited and unknown are named; every
// other value is a script whose ISO 15924 code script_tag gives.
enum class Script : u8 {
    common = 0,    // Zyyy
    inherited = 1, // Zinh
    unknown = 2,   // Zzzz
};

// The Script of one code point; unknown for a value above 0x10FFFF.
[[nodiscard]] Script script_of(u32 code_point) noexcept;

// Whether `script` is the code point's Script or among its
// Script_Extensions.
[[nodiscard]] bool script_extends_to(u32 code_point, Script script) noexcept;

// The script's ISO 15924 code as four letters, the first in the highest
// byte ("Latn" is 0x4C61746E, the form OpenType and HarfBuzz tags take);
// Zzzz for a value that is no script.
[[nodiscard]] u32 script_tag(Script script) noexcept;

// One run of a paragraph in one script: code units [offset, offset +
// length) of the input. common when the paragraph has no character of a
// script.
struct ScriptRun final {
    u32 offset{0};
    u32 length{0};
    Script script{Script::common};
    u8 reserved[3]{0, 0, 0};
};

static_assert(sizeof(ScriptRun) == 12, "stxlib::textlayout: a script run is 12 bytes");

// ---------------------------------------------------------------------------
// Analysis.

class Line;

// One analysed paragraph. Filled by analyse_paragraph; read by
// analyse_line and through its accessors. Borrows nothing from the input.
class Paragraph final {
public:
    Paragraph() noexcept = default;
    // A moved-from paragraph is empty, as after clear(), with no capacity
    // (A-stxlib-textlayout-01-F2).
    Paragraph(Paragraph&& other) noexcept;
    Paragraph& operator=(Paragraph&& other) noexcept;
    Paragraph(const Paragraph&) = delete;
    Paragraph& operator=(const Paragraph&) = delete;

    // The paragraph's code units in the input: [offset, offset + length),
    // its ending paragraph separator (CR LF together) included.
    [[nodiscard]] u32 offset() const noexcept { return offset_; }
    [[nodiscard]] u32 length() const noexcept { return length_; }

    // The paragraph embedding level: 0 or 1.
    [[nodiscard]] u8 base_level() const noexcept { return base_level_; }

    // The embedding level of each code unit of the paragraph (before rule
    // L1, which is a line's): levels()[i] is that of code unit offset() + i.
    // Borrowed: valid until the paragraph is analysed into again, moved
    // from or destroyed.
    [[nodiscard]] View<u8> levels() const& noexcept { return View<u8>(levels_.begin(), length_); }
    View<u8> levels() const&& = delete;

    // What may happen after each code unit of the paragraph (LineBreak
    // above); the last is mandatory. Borrowed as levels().
    [[nodiscard]] View<LineBreak> breaks() const& noexcept {
        // breaks_[0] is the slot before the first decision.
        if (length_ == 0) return View<LineBreak>();
        return View<LineBreak>(breaks_.begin() + 1, length_);
    }
    View<LineBreak> breaks() const&& = delete;

    // The paragraph's runs of one script, in order, covering it. Borrowed
    // as levels().
    [[nodiscard]] View<ScriptRun> script_runs() const& noexcept {
        return View<ScriptRun>(script_runs_.begin(), script_run_count_);
    }
    View<ScriptRun> script_runs() const&& = delete;

    // Ends the analysis; capacity remains.
    void clear() noexcept;

    // Ends the analysis and frees the storage, as a new paragraph.
    void release() noexcept;

    // The bytes the paragraph's storage holds, as allocated now, and the
    // most that has been since it was made, released or reset_peak was
    // last called (as Typeface's and Face's); a move carries both with the
    // storage. The storage grows only within analyse_paragraph, so the peak
    // is taken at its end.
    [[nodiscard]] usize bytes_held() const noexcept;
    [[nodiscard]] usize bytes_peak() const noexcept;
    void reset_peak() noexcept;

private:
    friend struct Access;

    // Each vector keeps the most elements it has held (so a reused
    // paragraph writes, and never zeroes, what it uses); the counts below
    // say how many are this paragraph's.
    Vector<u8> levels_{};     // per code unit
    Vector<LineBreak> breaks_{};        // per code unit
    Vector<ScriptRun> script_runs_{};
    Vector<u8> classes_{};    // per code point: Bidi_Class, BRACKET flag (non-uniform paragraphs only)
    Vector<u32> units_{};     // per code point: its first code unit from offset_, then length_
    Vector<u32> keys_{};      // per code point that is a bracket: its BD16 key, bit 31 when it opens
    Vector<u8> work_{};       // working storage of the analysis
    Vector<u32> work_wide_{}; // working storage of the analysis
    u32 offset_{0};
    u32 length_{0};
    u32 code_points_{0};
    u32 unit_count_{0};      // entries of units_ in use: code_points_ + 1, or 0 when every code unit starts one
    u32 key_count_{0};       // (code point, key) pairs in keys_
    u32 script_run_count_{0};
    u32 classes_present_{0}; // bit c set when a code point of class c occurs
    u8 base_level_{0};
    bool uniform_{true};     // one level throughout; classes_ and units_ not kept
    usize peak_{0};          // bytes_peak
};

// Analyses the paragraph that starts at byte `offset` of `text`: up to
// and including its first paragraph separator, or to the end of the text.
// One walk gives the levels, the break opportunities and the script runs.
// The next paragraph starts at paragraph.offset() + paragraph.length().
// invalid_argument when offset is not at a code point's first byte,
// out_of_range when offset >= text.count(), limit_exceeded when the text
// has more than 2^32 - 2 bytes; out_of_memory when storage cannot grow.
[[nodiscard]] OperationStatus analyse_paragraph(TextView text, usize offset, BaseDirection direction,
                                                Paragraph& paragraph) noexcept;

// The same over code points, one code unit each. A value that is not a
// scalar value is taken as U+FFFD (Bidi_Class ON).
[[nodiscard]] OperationStatus analyse_paragraph(View<u32> code_points, usize offset, BaseDirection direction,
                                                Paragraph& paragraph) noexcept;

// One line of an analysed paragraph: its levels after rule L1 and its runs
// in visual order, left to right (rule L2).
class Line final {
public:
    Line() noexcept = default;
    // A moved-from line is empty, as after clear(), with no capacity.
    Line(Line&& other) noexcept;
    Line& operator=(Line&& other) noexcept;
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;

    // The line's code units in the input: [offset, offset + length).
    [[nodiscard]] u32 offset() const noexcept { return offset_; }
    [[nodiscard]] u32 length() const noexcept { return length_; }

    // The runs in visual order; adjacent runs differ in level. Borrowed:
    // valid until the line is analysed into again, moved from or destroyed.
    [[nodiscard]] View<BidiRun> runs() const& noexcept { return View<BidiRun>(runs_.begin(), run_count_); }
    View<BidiRun> runs() const&& = delete;

    // The level of each code unit of the line after rule L1. Borrowed as
    // runs().
    [[nodiscard]] View<u8> levels() const& noexcept { return View<u8>(levels_.begin(), length_); }
    View<u8> levels() const&& = delete;

    // Ends the analysis; capacity remains.
    void clear() noexcept;

    // Ends the analysis and frees the storage, as a new line.
    void release() noexcept;

    // As Paragraph's, for the line's storage (grown only within
    // analyse_line).
    [[nodiscard]] usize bytes_held() const noexcept;
    [[nodiscard]] usize bytes_peak() const noexcept;
    void reset_peak() noexcept;

private:
    friend struct Access;

    // As Paragraph's: the vectors keep their most elements; the counts
    // say how many are this line's.
    Vector<BidiRun> runs_{};
    Vector<u8> levels_{};
    u32 offset_{0};
    u32 length_{0};
    u32 run_count_{0};
    usize peak_{0}; // bytes_peak
};

// Analyses the line [offset, offset + length) of `paragraph` (offsets in
// the input, as the paragraph's): rule L1 over the line, then its runs in
// visual order. A line that ends where the paragraph ends includes its
// paragraph separator. invalid_argument when either end is not at a code
// point boundary, out_of_range when the line is empty or not within the
// paragraph; out_of_memory when storage cannot grow.
[[nodiscard]] OperationStatus analyse_line(const Paragraph& paragraph, usize offset, usize length, Line& line) noexcept;

} // namespace stxlib::textlayout
