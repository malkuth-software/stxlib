#pragma once

// stxlib::markdown: a Markdown reader. A conversion of MD4C 0.6.0's design
// to house-style C++ (the owner, 2026-11-15; MD4C is by Martin Mitas, MIT:
// src/algorithms/markdown/NOTICE), for presentation and editing of rich
// text. It reads; it writes no HTML and opens no link or image.
//
// The dialect: CommonMark 0.31.2 without raw HTML, and with
// ~~strikethrough~~ when Options asks. Block quotes, bullet and ordered
// lists, thematic breaks, ATX and setext headings, indented and fenced code
// blocks, paragraphs, link reference definitions; code spans, emphasis and
// strong emphasis, links and images (inline and by reference), autolinks in
// angle brackets, backslash escapes, character references, hard and soft
// line breaks. HTML syntax, block or inline, is text: there is no option
// that passes it through. Tables, task lists and the other extensions of
// MD4C are not read; their source is text.
//
// The result is a tape: parse writes a Document, a flat list of events in
// source order, and returns. No code of the caller runs during the parse.
// An event enters a block, leaves a block, enters a span, leaves a span, or
// is a piece of text; blocks and spans nest properly, and the first and
// last events enter and leave the document block.
//
// Text and ranges. Every event carries a range of the source, in bytes.
// - A piece of text is a range of the source (literal text, code), or a
//   line ending read as a break, or text the reader supplies: a decoded
//   character reference (in the document's own text, Event::detail and
//   Event::data say where), the replacement for a NUL, or the spaces of a
//   code block's indentation (Event::detail says how many).
// - A block's range runs from the first byte of its first line's content
//   to the end of its last line's content; a container's range is empty.
// - A span's enter event covers its opening delimiter and its leave event
//   the closing one (for a link, everything from the "]").
// - Destinations, titles and a code block's info string are decoded
//   (backslash escapes and character references resolved) into the
//   document's own text; Document::attribute gives each.
// Source bytes no text event covers are markup.
//
// Encoding. The source is bytes. Where it is well-formed UTF-8 every piece
// of text and every attribute is well-formed UTF-8. An ill-formed sequence
// is carried through as it is and never treated as whitespace, punctuation
// or markup.
//
// Bounds. The source is at most Limits::source_bytes; the tape, the
// document's text, the depth of containers and the number of link
// reference definitions are bounded by Limits and checked before each
// grows. The work is linear in the source: every loop is over lines, over
// the bytes of a line, over the marks of one block, or over a table of
// fixed size (the design note, section 5). There is no recursion.
//
// Failure. parse returns limit_exceeded (a Limits bound, or a source too
// large), out_of_memory, or interrupted (the stop flag was set; it is read
// once per block). On every failure the document is empty. Nothing throws.
//
// Lifetime. A Document borrows nothing: its ranges are numbers. They are
// positions in the source the caller parsed, which the caller keeps.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

#include <atomic>

namespace stxlib::markdown {

struct Options final {
    // ~~text~~ and ~text~ are strikethrough spans.
    bool strikethrough{false};
};

struct Limits final {
    // The source, in bytes. At most largest_source_bytes.
    usize source_bytes{static_cast<usize>(16) * 1024 * 1024};
    // Events of the tape.
    usize events{static_cast<usize>(4) * 1024 * 1024};
    // The document's own text: decoded attributes and character references.
    usize text_bytes{static_cast<usize>(32) * 1024 * 1024};
    // Block quotes and list items around a block.
    u32 containers{256};
    // Link reference definitions.
    u32 definitions{65536};
};

// Offsets are 32-bit; the two largest values are kept as "none".
inline constexpr usize largest_source_bytes = static_cast<usize>(0xfffffffd);

enum class EventKind : u8 {
    enter_block = 0,
    leave_block = 1,
    enter_span = 2,
    leave_span = 3,
    text = 4,
};

enum class BlockType : u8 {
    document = 0,
    quote = 1,
    // Event::data: the bullet character. Event::flags: tight_list.
    bullet_list = 2,
    // Event::data: the delimiter, '.' or ')'. Event::detail: the first
    // item's number. Event::flags: tight_list.
    ordered_list = 3,
    list_item = 4,
    // A thematic break. No contents.
    rule = 5,
    // Event::data: the level, 1 to 6.
    heading = 6,
    // Event::data: the fence character, or 0 for an indented block.
    // Event::detail: for a fenced block, the attribute index of the info
    // string; the next attribute is its first word (the language). Its
    // text is TextType::code pieces, code_indent pieces and line_end
    // pieces: each line ends with a line_end.
    code_block = 7,
    // In a tight list a paragraph's text stands directly in its list item:
    // no paragraph events.
    paragraph = 8,
};

enum class SpanType : u8 {
    emphasis = 0,
    strong = 1,
    // Event::detail: the attribute index of the destination; the next
    // attribute is the title. Event::flags: autolink.
    link = 2,
    // As link. The events between enter and leave are the description.
    image = 3,
    code = 4,
    strikethrough = 5,
};

enum class TextType : u8 {
    // The range of the source, as it is.
    literal = 0,
    // In a code span or a code block: the range of the source, as it is.
    code = 1,
    // A character reference. The range is the reference; its decoded text
    // is Event::data bytes of the document's text at Event::detail.
    reference = 2,
    // A NUL byte of the source, to be shown as U+FFFD.
    null_character = 3,
    // A line ending inside a paragraph, read as a space. The range is the
    // line ending.
    soft_break = 4,
    // A hard line break. The range is the line ending with the backslash or
    // the spaces before it.
    hard_break = 5,
    // The end of a code block's line. The range is the line ending (empty
    // at the end of a source that does not end with one).
    line_end = 6,
    // A line ending inside a code span, read as one space. The range is the
    // line ending.
    code_space = 7,
    // Event::detail spaces of a code block's indentation that are not
    // bytes of the source (a tab's remainder, or indentation inside a
    // container). The range is empty, where they stand.
    code_indent = 8,
};

namespace event_flags {
// bullet_list, ordered_list: no item is separated from the next by a blank
// line, so the items' paragraphs have no paragraph events.
inline constexpr u8 tight_list = 0x01;
// link: written <like this>; a mail address without "mailto:" also has
// mail_address.
inline constexpr u8 autolink = 0x02;
inline constexpr u8 mail_address = 0x04;
// heading: written with an underline (setext), not with # (ATX).
inline constexpr u8 setext = 0x08;
} // namespace event_flags

// 16 bytes. `type` is a BlockType, a SpanType or a TextType, as `kind` says.
struct Event final {
    EventKind kind{EventKind::text};
    u8 type{0};
    u8 flags{0};
    u8 data{0};
    // The range of the source: [begin, end).
    u32 begin{0};
    u32 end{0};
    u32 detail{0};
};

static_assert(sizeof(Event) == 16, "stxlib::markdown: an event is 16 bytes");

// A range of the document's own text.
struct TextRange final {
    u32 begin{0};
    u32 end{0};
};

// What parse writes. Read it in order; `events` is the whole result, and
// `text` and `attributes` are what its events point into.
class Document final {
public:
    Document() noexcept = default;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    Vector<Event> events{};
    Vector<TextRange> attributes{};
    Vector<char> text{};

    // The decoded text of attribute `index` (a link's destination or
    // title, a code block's info string or language). Borrowed from the
    // document: valid until it is parsed into again, cleared, moved from or
    // destroyed. Requirement: index < attributes.count().
    [[nodiscard]] View<char> attribute(u32 index) const& noexcept;
    View<char> attribute(u32 index) const&& = delete;

    // The decoded text of a TextType::reference event. Borrowed as above.
    // Requirement: the event is one of this document's reference events.
    [[nodiscard]] View<char> reference_text(const Event& event) const& noexcept;
    View<char> reference_text(const Event& event) const&& = delete;

    // Ends every element's lifetime; capacity remains.
    void clear() noexcept;
};

// Reads `source` into `document`, which is replaced; on failure it is
// empty. `source` is only read and is not retained. `stop`, when not null,
// is read once per block (relaxed); if it is set the parse ends with
// interrupted. Requirement: `document` is not being read by another thread.
[[nodiscard]] Status parse(View<char> source, const Options& options, const Limits& limits, Document& document,
                           const std::atomic<bool>* stop) noexcept;

} // namespace stxlib::markdown
