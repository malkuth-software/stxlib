#pragma once

// stxlib::markdown: what the reader's parts share. markdown.hpp states the
// contract; this is the working state of one parse.
//
// Phase A (lines.cpp) reads the source once and writes `blocks`, `lines`
// and the link reference definitions. Phase B (blocks.cpp, inlines.cpp)
// walks `blocks` in order and writes the tape.
//
// Failure inside the reader is a bool: a function that can fail returns
// false after fail() has recorded why, and its caller returns false at
// once. parse (parse.cpp) turns the record into the Status.

#include "algorithms/markdown/characters.hpp"
#include "stxlib/markdown.hpp"

namespace stxlib::markdown::detail {

inline constexpr u32 none = 0xffffffff;
// The indentation at which a line is indented code.
inline constexpr u32 code_indent = 4;

// ---- phase A's tables ---------------------------------------------------------------

// One line of a leaf block: [begin, end) is its content (for a line of
// text, without the indentation, the container marks and the trailing
// blanks; for a code line, everything after the indentation taken off).
// `indent` is, for a code line, the columns of indentation that remain
// part of the code.
struct Line final {
    u32 begin{0};
    u32 end{0};
    u32 indent{0};
};

enum class RecordKind : u8 {
    leaf = 0,
    open = 1,
    close = 2,
};

namespace record_flags {
inline constexpr u8 loose_list = 0x01;
inline constexpr u8 setext = 0x02;
} // namespace record_flags

// A leaf block with its lines, or the opening or closing of a container.
struct BlockRecord final {
    BlockType type{BlockType::paragraph};
    RecordKind kind{RecordKind::leaf};
    u8 flags{0};
    // heading: the level. code_block: the fence character, 0 if indented.
    // bullet_list: the bullet. ordered_list: the delimiter.
    u8 data{0};
    // leaf: its lines are lines[first_line, first_line + line_count).
    u32 first_line{0};
    u32 line_count{0};
    // ordered_list: the first item's number.
    u32 number{0};
};

// A block quote or a list the current line is inside.
struct Container final {
    // '>' for a quote; the bullet or the delimiter for a list.
    char ch{0};
    u32 start{0};
    u32 mark_indent{0};
    u32 contents_indent{0};
    // The list's opening record, to mark it loose.
    u32 block_index{0};
};

// ---- link reference definitions ------------------------------------------------------

// Where a link's destination and title are in the source, without their
// delimiters and not decoded.
struct LinkTarget final {
    u32 destination_begin{0};
    u32 destination_end{0};
    u32 title_begin{0};
    u32 title_end{0};
    // The line (an index into the context's lines) on which the title
    // begins, when it runs over more than one; none when it does not.
    u32 title_line{none};
};

struct Definition final {
    // The label in its matching form (case folded, whitespace collapsed):
    // a range of the context's label_text.
    u32 label_begin{0};
    u32 label_end{0};
    u32 hash{0};
    LinkTarget target{};
    // The attribute index of the decoded destination (the title is the
    // next), made at the definition's first use and shared by every link
    // that names it; none until then.
    u32 attribute{none};
};

// ---- inline marks --------------------------------------------------------------------

// A place in a block's text that may begin or end a span, or stands for
// something other than itself (inlines.cpp).
struct Mark final {
    u32 begin{0};
    u32 end{0};
    // Resolved as a range: the opener's `next` is its closer and the
    // closer's `previous` its opener. An unresolved opener's `next` is the
    // opener below it on its stack.
    u32 previous{none};
    u32 next{none};
    char ch{0};
    u8 flags{0};
};

// ---- the context ---------------------------------------------------------------------

struct Context final {
    // The source.
    const char* text{nullptr};
    u32 size{0};
    Options options{};
    Limits limits{};
    Document* document{nullptr};
    const StopSignal* stop{nullptr};

    // Why the parse failed; ErrorKind::internal_failure stands for "has not".
    bool failed{false};
    ErrorKind failure{ErrorKind::internal_failure};

    // Phase A.
    Vector<BlockRecord> blocks{};
    Vector<Line> lines{};
    Vector<Container> containers{};
    // The indexes of the open quotes among the containers, rising: a blank
    // line finds the first quote it is not inside by one binary search.
    Vector<u32> quotes{};
    // The leaf block being built: an index into blocks, or none.
    u32 current_block{none};
    // The length of the fence that opened the fenced code block being read.
    u32 fence_length{0};
    bool last_line_loosens{false};
    u32 consecutive_blank_lines{0};

    // Definitions in source order; their labels' matching forms; and the
    // definitions' indices sorted by (hash, label, index), for lookup.
    Vector<Definition> definitions{};
    Vector<char> label_text{};
    Vector<u32> definition_order{};

    // Phase B: the tape. events_ is the document's event list resized to
    // its room; event_count are written.
    Event* events{nullptr};
    usize event_count{0};
    usize event_room{0};
    // Whether each open list is loose (a quote counts as loose).
    Vector<u8> loose_stack{};
    // Inline work (inlines.cpp).
    Vector<Mark> marks{};
    // The openers of the bracket pairs of the block being read, in the
    // order of their closers.
    Vector<u32> bracket_pairs{};
};

// Records the first failure. Always returns false, for `return fail(...)`.
bool fail(Context& ctx, ErrorKind kind) noexcept;

// ---- phase A (lines.cpp) -------------------------------------------------------------

// Reads the whole source into blocks, lines and definitions.
[[nodiscard]] bool read_lines(Context& ctx) noexcept;

// Whether `offset` begins a thematic break. On true `end` is the end of the
// line's content; on false `killer` is how far the scan got.
[[nodiscard]] bool is_rule_line(const Context& ctx, u32 offset, u32& end, u32& killer) noexcept;

// ---- links (links.cpp) ---------------------------------------------------------------

// How many of lines[first_line, first_line + line_count) the link reference
// definition at their start takes: 0 if there is none, or -1 on failure.
// A definition found is recorded.
[[nodiscard]] i32 take_definition(Context& ctx, u32 first_line, u32 line_count) noexcept;

// Sorts the definitions for lookup, once every one is known.
[[nodiscard]] bool index_definitions(Context& ctx) noexcept;

// The line of lines[first_line, first_line + line_count) that holds
// `offset`, or the one after the gap it falls in: an index into lines.
[[nodiscard]] u32 line_at(const Context& ctx, u32 first_line, u32 line_count, u32 offset) noexcept;

// The definition a reference's label [begin, end) names (the label without
// its brackets, which may run over the block's lines), or none.
[[nodiscard]] u32 find_definition(Context& ctx, u32 first_line, u32 line_count, u32 begin, u32 end) noexcept;

// Whether an inline link's "(destination title)" stands at `begin`, a '('.
// On true `end` is just after the ')' and `target` says where its parts are.
[[nodiscard]] bool inline_link_target(const Context& ctx, u32 first_line, u32 line_count, u32 begin, u32& end,
                                      LinkTarget& target) noexcept;

// Decodes a destination and a title into two consecutive attributes;
// `attribute` is the first. For a definition, once: later calls give the
// same attributes.
[[nodiscard]] bool target_attributes(Context& ctx, const LinkTarget& target, u32& attribute) noexcept;
[[nodiscard]] bool definition_attributes(Context& ctx, u32 definition, u32& attribute) noexcept;

// Decodes the source range [begin, end) into one new attribute: character
// references resolved, a NUL as U+FFFD, and, if `escapes`, a backslash
// before ASCII punctuation dropped.
[[nodiscard]] bool build_attribute(Context& ctx, u32 begin, u32 end, bool escapes, u32& attribute) noexcept;

// Whether a character reference's shape stands at text[begin], an '&',
// ending before `limit`. On true `end` is just after its ';'.
[[nodiscard]] bool reference_at(const char* text, u32 begin, u32 limit, u32& end) noexcept;

// The UTF-8 text the reference text[begin, end) stands for, written to
// out[0, 8); returns its length, or 0 for a name that is not a character
// reference (it is then text).
[[nodiscard]] u32 decode_reference(const char* text, u32 begin, u32 end, char* out) noexcept;

// ---- phase B -------------------------------------------------------------------------

// Makes room for `count` more events, or fails with limit_exceeded.
[[nodiscard]] bool reserve_events(Context& ctx, usize count) noexcept;

// Writes one event. Requirement: reserve_events made room for it.
inline void put_event(Context& ctx, const Event& event) noexcept {
    ctx.events[ctx.event_count] = event;
    ctx.event_count = ctx.event_count + 1;
}

// Appends bytes to the document's own text; `range` is where they went.
[[nodiscard]] bool append_text(Context& ctx, const char* bytes, u32 count, TextRange& range) noexcept;

// Writes every block of the document to the tape (blocks.cpp).
[[nodiscard]] bool write_blocks(Context& ctx) noexcept;

// Writes the text and spans of one leaf block of inline content, whose
// lines are lines[first_line, first_line + line_count) (inlines.cpp).
[[nodiscard]] bool write_inlines(Context& ctx, u32 first_line, u32 line_count) noexcept;

} // namespace stxlib::markdown::detail
