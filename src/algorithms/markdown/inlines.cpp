// stxlib::markdown, phase B: the text and spans of one leaf block
// (internal.hpp, write_inlines). After MD4C 0.6.0's md_collect_marks,
// md_analyze_marks, md_resolve_brackets, md_analyze_emph and
// md_process_inlines, for the marks this dialect has.
//
// Three passes over one block, each linear in it:
// 1. Collect. The block's bytes are read once; every byte that may mean
//    something becomes a mark. Code spans, autolinks, escapes, character
//    references and NULs are decided here, because nothing later can
//    change them.
// 2. Resolve. Brackets first (pairs are listed as their closers are met,
//    then decided inner to outer: a link's text cannot hold a link), then
//    emphasis and strikethrough, inside each link's text and then in the
//    rest. An emphasis closer looks only at the tops of the opener stacks
//    it may match (split by character, by "can also close" and by run
//    length mod 3, the rule of three), so each mark is pushed and popped at
//    most once.
// 3. Emit. The resolved marks in order, with the text between them.
//
// Differences from MD4C, on purpose:
// - A character reference is recognised whole where its '&' stands and
//   only if it names a character; there are no ';' marks.
// - The marks inside a reference link's label ("[text][label]") are
//   disabled when the link is made. MD4C leaves them, so an emphasis mark
//   in the label can close a span opened before the link and its closing
//   event is then never sent.
// - A mail autolink's destination is the address as written, with a flag;
//   nothing is copied to put "mailto:" before it.

#include "algorithms/markdown/internal.hpp"

namespace stxlib::markdown::detail {

namespace {

namespace mark_flags {
inline constexpr u8 potential_opener = 0x01;
inline constexpr u8 potential_closer = 0x02;
inline constexpr u8 opener = 0x04;
inline constexpr u8 closer = 0x08;
inline constexpr u8 resolved = 0x10;
// Emphasis: can both open and close; and the run's length mod 3, plus one.
inline constexpr u8 both = 0x20;
inline constexpr u8 mod3_shift = 6;
inline constexpr u8 mod3_mask = 0xc0;
// '[': the byte before it is '!'; and another '[' follows before its ']'.
inline constexpr u8 can_be_image = 0x20;
inline constexpr u8 has_nested = 0x40;
// '<': a mail address.
inline constexpr u8 mail = 0x20;
} // namespace mark_flags

// A mark that stands for nothing: room kept for splitting a run, or for a
// link's attribute index.
inline constexpr char dummy = 'D';
// The mark after the last, at the end of the source.
inline constexpr char sentinel = 127;

inline constexpr u32 code_span_longest = 32;

// The opener stacks: 12 for emphasis (character, then "can also close",
// then length mod 3), 2 for tildes (length 1 and 2), 1 for brackets.
inline constexpr usize tilde_stack = 12;
inline constexpr usize bracket_stack = 14;
inline constexpr usize stack_count = 15;

struct MarkCharacters final {
    bool is_mark[256]{};
};

[[nodiscard]] constexpr MarkCharacters make_mark_characters(const bool strikethrough) noexcept {
    MarkCharacters table{};
    table.is_mark[static_cast<u8>('\\')] = true;
    table.is_mark[static_cast<u8>('*')] = true;
    table.is_mark[static_cast<u8>('_')] = true;
    table.is_mark[static_cast<u8>('`')] = true;
    table.is_mark[static_cast<u8>('&')] = true;
    table.is_mark[static_cast<u8>('<')] = true;
    table.is_mark[static_cast<u8>('[')] = true;
    table.is_mark[static_cast<u8>('!')] = true;
    table.is_mark[static_cast<u8>(']')] = true;
    table.is_mark[0] = true;
    table.is_mark[static_cast<u8>('~')] = strikethrough;
    return table;
}

inline constexpr MarkCharacters plain_marks = make_mark_characters(false);
inline constexpr MarkCharacters strikethrough_marks = make_mark_characters(true);

// The working state of one block's inline pass.
struct Inline final {
    u32 first_line{0};
    u32 line_limit{0};
    Mark* marks{nullptr};
    u32 mark_count{0};
    u32 tops[stack_count]{};
};

// ---- collecting ----------------------------------------------------------------------

[[nodiscard]] bool add_mark(Context& ctx, const char ch, const u32 begin, const u32 end, const u8 flags) noexcept {
    const Mark mark{.begin = begin, .end = end, .previous = none, .next = none, .ch = ch, .flags = flags};
    if (ctx.marks.count() >= ctx.limits.marks) return fail(ctx, ErrorKind::limit_exceeded);
    if (ctx.marks.push_copy(mark).failed()) return fail(ctx, ErrorKind::out_of_memory);
    return true;
}

// 0 whitespace or the line's edge, 1 punctuation, 2 anything else.
[[nodiscard]] u32 level_before(const Context& ctx, const u32 line_begin, const u32 offset) noexcept {
    if (offset == line_begin) return 0;
    const Decoded decoded = decode_before(ctx.text, line_begin, offset);
    if (is_unicode_whitespace(decoded.code_point)) return 0;
    const u32 result = is_unicode_punctuation(decoded.code_point) ? 1 : 2;
    return result;
}

[[nodiscard]] u32 level_at(const Context& ctx, const u32 line_end, const u32 offset) noexcept {
    if (offset == line_end) return 0;
    const Decoded decoded = decode_at(ctx.text, line_end, offset);
    if (is_unicode_whitespace(decoded.code_point)) return 0;
    const u32 result = is_unicode_punctuation(decoded.code_point) ? 1 : 2;
    return result;
}

struct CodeSpan final {
    u32 opener_end{0};
    u32 closer_begin{0};
    u32 closer_end{0};
    // The line the closer is on.
    u32 closer_line{0};
    // Where the opening run of backticks ends, found or not.
    u32 run_end{0};
};

// Whether the backticks at text[begin], on `line`, open a code span.
// `last_closers[n - 1]` is the last place a run of n backticks was seen
// that closed nothing, and `reached_end` whether a search has already run
// to the block's end: together they keep the searches linear.
[[nodiscard]] bool find_code_span(const Context& ctx, const Inline& state, const u32 line, const u32 begin, CodeSpan& span,
                                  u32* const last_closers, bool& reached_end) noexcept {
    u32 line_end = ctx.lines[line].end;
    u32 opener_end = begin;
    while (opener_end < line_end && ctx.text[opener_end] == '`') opener_end = opener_end + 1;
    span.run_end = opener_end;
    const bool space_after_opener = opener_end < line_end && ctx.text[opener_end] == ' ';
    const bool line_end_after_opener = opener_end == line_end;
    const u32 length = opener_end - begin;
    if (length > code_span_longest) return false;
    const u32 block_end = ctx.lines[state.line_limit - 1].end;
    if (last_closers[length - 1] >= block_end || (reached_end && last_closers[length - 1] < opener_end)) return false;

    u32 closer_begin = opener_end;
    u32 closer_end = opener_end;
    u32 index = line;
    bool only_spaces = true;
    for (;;) {
        while (closer_begin < line_end && ctx.text[closer_begin] != '`') {
            if (ctx.text[closer_begin] != ' ') only_spaces = false;
            closer_begin = closer_begin + 1;
        }
        closer_end = closer_begin;
        while (closer_end < line_end && ctx.text[closer_end] == '`') closer_end = closer_end + 1;
        const u32 run = closer_end - closer_begin;
        if (run == length) break;
        if (run > 0) {
            only_spaces = false;
            // Not this span's closer; perhaps a later one's.
            if (run <= code_span_longest && closer_begin > last_closers[run - 1]) last_closers[run - 1] = closer_begin;
        }
        if (closer_end >= line_end) {
            index = index + 1;
            if (index >= state.line_limit) {
                reached_end = true;
                return false;
            }
            line_end = ctx.lines[index].end;
            closer_begin = ctx.lines[index].begin;
        } else {
            closer_begin = closer_end;
        }
    }
    const bool space_before_closer = closer_begin > ctx.lines[index].begin && ctx.text[closer_begin - 1] == ' ';
    const bool line_end_before_closer = closer_begin == ctx.lines[index].begin;
    // One space (or line ending) inside each end belongs to the marks,
    // unless the span is spaces only.
    if (!only_spaces && (space_after_opener || line_end_after_opener) && (space_before_closer || line_end_before_closer)) {
        if (space_after_opener) {
            opener_end = opener_end + 1;
        } else {
            opener_end = ctx.lines[line + 1].begin;
        }
        if (space_before_closer) {
            closer_begin = closer_begin - 1;
        } else {
            closer_begin = ctx.lines[index - 1].end;
            while (closer_begin < ctx.size && is_blank(ctx.text[closer_begin])) closer_begin = closer_begin + 1;
        }
    }
    span.opener_end = opener_end;
    span.closer_begin = closer_begin;
    span.closer_end = closer_end;
    span.closer_line = index;
    return true;
}

// "<scheme:...>" with a scheme of 2 to 32 characters.
[[nodiscard]] bool is_uri_autolink(const Context& ctx, const u32 begin, const u32 limit, u32& end) noexcept {
    u32 offset = begin + 1;
    if (offset >= limit || !is_alphanumeric(ctx.text[offset])) return false;
    offset = offset + 1;
    for (;;) {
        if (offset >= limit || offset - begin > 32) return false;
        const char ch = ctx.text[offset];
        if (ch == ':' && offset - begin >= 3) break;
        if (!is_alphanumeric(ch) && ch != '+' && ch != '-' && ch != '.') return false;
        offset = offset + 1;
    }
    while (offset < limit && ctx.text[offset] != '>') {
        const char ch = ctx.text[offset];
        if (is_whitespace(ch) || is_control(ch) || ch == '<') return false;
        offset = offset + 1;
    }
    if (offset >= limit) return false;
    end = offset + 1;
    return true;
}

[[nodiscard]] bool is_mail_name_character(const char ch) noexcept {
    if (is_alphanumeric(ch)) return true;
    const bool result = ch == '.' || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
                        ch == '*' || ch == '+' || ch == '/' || ch == '=' || ch == '?' || ch == '^' || ch == '_' ||
                        ch == '`' || ch == '{' || ch == '|' || ch == '}' || ch == '~' || ch == '-';
    return result;
}

// "<name@label.label>", each label 1 to 63 letters, digits and inner hyphens.
[[nodiscard]] bool is_mail_autolink(const Context& ctx, const u32 begin, const u32 limit, u32& end) noexcept {
    u32 offset = begin + 1;
    while (offset < limit && is_mail_name_character(ctx.text[offset])) offset = offset + 1;
    if (offset <= begin + 1) return false;
    if (offset >= limit || ctx.text[offset] != '@') return false;
    offset = offset + 1;
    u32 label = 0;
    while (offset < limit) {
        const char ch = ctx.text[offset];
        if (is_alphanumeric(ch)) {
            label = label + 1;
        } else if (ch == '-' && label > 0) {
            label = label + 1;
        } else if (ch == '.' && label > 0 && ctx.text[offset - 1] != '-') {
            label = 0;
        } else {
            break;
        }
        if (label > 63) return false;
        offset = offset + 1;
    }
    if (label == 0 || offset >= limit || ctx.text[offset] != '>' || ctx.text[offset - 1] == '-') return false;
    end = offset + 1;
    return true;
}

// Reads the block once and adds a mark for every byte that may mean
// something. Ends with the sentinel.
[[nodiscard]] bool collect_marks(Context& ctx, const Inline& state) noexcept {
    const bool* const is_mark = ctx.options.strikethrough ? strikethrough_marks.is_mark : plain_marks.is_mark;
    u32 last_closers[code_span_longest] = {};
    bool reached_end = false;
    const u32 block_end = ctx.lines[state.line_limit - 1].end;
    u32 line = state.first_line;
    // Where to go on from on a line reached inside a code span.
    u32 resume = none;
    while (line < state.line_limit) {
        const u32 line_begin = ctx.lines[line].begin;
        const u32 line_end = ctx.lines[line].end;
        u32 offset = resume != none ? resume : line_begin;
        resume = none;
        bool moved_line = false;
        while (offset < line_end) {
            const char ch = ctx.text[offset];
            if (!is_mark[static_cast<u8>(ch)]) {
                offset = offset + 1;
                continue;
            }
            if (ch == '\\') {
                // An escape; before a line ending, a hard break, which the
                // block's last line cannot end with.
                if (offset + 1 < ctx.size &&
                    (is_ascii_punctuation(ctx.text[offset + 1]) || is_newline(ctx.text[offset + 1]))) {
                    if (!is_newline(ctx.text[offset + 1]) || line + 1 < state.line_limit) {
                        if (!add_mark(ctx, ch, offset, offset + 2, mark_flags::resolved)) return false;
                    }
                    offset = offset + 2;
                    continue;
                }
                offset = offset + 1;
                continue;
            }
            if (ch == '*' || ch == '_') {
                u32 run_end = offset + 1;
                while (run_end < line_end && ctx.text[run_end] == ch) run_end = run_end + 1;
                u32 left = level_before(ctx, line_begin, offset);
                u32 right = level_at(ctx, line_end, run_end);
                // An underscore inside a word means nothing.
                if (ch == '_' && left == 2 && right == 2) {
                    left = 0;
                    right = 0;
                }
                if (left != 0 || right != 0) {
                    u8 flags = 0;
                    if (left > 0 && left >= right) flags = flags | mark_flags::potential_closer;
                    if (right > 0 && right >= left) flags = flags | mark_flags::potential_opener;
                    if (flags == (mark_flags::potential_opener | mark_flags::potential_closer)) {
                        flags = flags | mark_flags::both;
                    }
                    const u32 mod3 = (run_end - offset) % 3;
                    flags = flags | static_cast<u8>((mod3 + 1) << mark_flags::mod3_shift);
                    if (!add_mark(ctx, ch, offset, run_end, flags)) return false;
                    // A run may be split when part of it is matched: room
                    // for each piece.
                    for (u32 extra = offset + 1; extra < run_end; ++extra) {
                        if (!add_mark(ctx, dummy, extra, extra, 0)) return false;
                    }
                }
                offset = run_end;
                continue;
            }
            if (ch == '`') {
                CodeSpan span{};
                if (find_code_span(ctx, state, line, offset, span, last_closers, reached_end)) {
                    const u32 opener_index = static_cast<u32>(ctx.marks.count());
                    if (!add_mark(ctx, '`', offset, span.opener_end, mark_flags::opener | mark_flags::resolved)) {
                        return false;
                    }
                    if (!add_mark(ctx, '`', span.closer_begin, span.closer_end, mark_flags::closer | mark_flags::resolved)) {
                        return false;
                    }
                    ctx.marks[opener_index].next = opener_index + 1;
                    ctx.marks[opener_index + 1].previous = opener_index;
                    offset = span.closer_end;
                    if (span.closer_line != line) {
                        line = span.closer_line;
                        resume = span.closer_end;
                        moved_line = true;
                        break;
                    }
                    continue;
                }
                offset = span.run_end;
                continue;
            }
            if (ch == '&') {
                u32 end = 0;
                char decoded[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                if (reference_at(ctx.text, offset, line_end, end) && decode_reference(ctx.text, offset, end, decoded) != 0) {
                    if (!add_mark(ctx, ch, offset, end, mark_flags::resolved)) return false;
                    offset = end;
                    continue;
                }
                offset = offset + 1;
                continue;
            }
            if (ch == '<') {
                u32 end = 0;
                u8 flags = mark_flags::resolved;
                bool found = is_uri_autolink(ctx, offset, block_end, end);
                if (!found && is_mail_autolink(ctx, offset, block_end, end)) {
                    found = true;
                    flags = flags | mark_flags::mail;
                }
                // An autolink holds no line ending, so it ends on this line.
                if (found && end <= line_end) {
                    const u32 opener_index = static_cast<u32>(ctx.marks.count());
                    if (!add_mark(ctx, '<', offset, offset + 1, static_cast<u8>(flags | mark_flags::opener))) return false;
                    if (!add_mark(ctx, '>', end - 1, end, static_cast<u8>(flags | mark_flags::closer))) return false;
                    ctx.marks[opener_index].next = opener_index + 1;
                    ctx.marks[opener_index + 1].previous = opener_index;
                    offset = end;
                    continue;
                }
                offset = offset + 1;
                continue;
            }
            if (ch == '[' || (ch == '!' && offset + 1 < line_end && ctx.text[offset + 1] == '[')) {
                if (ch == '!') {
                    const u8 flags = mark_flags::potential_opener | mark_flags::can_be_image;
                    if (!add_mark(ctx, '[', offset + 1, offset + 2, flags)) return false;
                    offset = offset + 2;
                } else {
                    if (!add_mark(ctx, '[', offset, offset + 1, mark_flags::potential_opener)) return false;
                    offset = offset + 1;
                }
                // Room for the link's attribute index, if it becomes one.
                if (!add_mark(ctx, dummy, offset, offset, 0)) return false;
                continue;
            }
            if (ch == ']') {
                if (!add_mark(ctx, ch, offset, offset + 1, mark_flags::potential_closer)) return false;
                offset = offset + 1;
                continue;
            }
            if (ch == '~') {
                u32 run_end = offset + 1;
                while (run_end < line_end && ctx.text[run_end] == '~') run_end = run_end + 1;
                if (run_end - offset <= 2) {
                    u8 flags = mark_flags::potential_opener | mark_flags::potential_closer;
                    if (level_before(ctx, line_begin, offset) == 2) {
                        flags = static_cast<u8>(flags & ~mark_flags::potential_opener);
                    }
                    if (level_at(ctx, line_end, run_end) == 2) flags = static_cast<u8>(flags & ~mark_flags::potential_closer);
                    if (flags != 0) {
                        if (!add_mark(ctx, ch, offset, run_end, flags)) return false;
                    }
                }
                offset = run_end;
                continue;
            }
            if (ch == '\0') {
                if (!add_mark(ctx, ch, offset, offset + 1, mark_flags::resolved)) return false;
                offset = offset + 1;
                continue;
            }
            // '!' not followed by '[': nothing.
            offset = offset + 1;
        }
        if (!moved_line) line = line + 1;
    }
    const bool ended = add_mark(ctx, sentinel, ctx.size, ctx.size, mark_flags::resolved);
    return ended;
}

// ---- resolving -----------------------------------------------------------------------

void push_opener(Inline& state, const usize stack, const u32 index) noexcept {
    state.marks[index].next = state.tops[stack];
    state.tops[stack] = index;
}

// Takes every opener at or after `index` off every stack, so that no span
// can cross the one being made.
void pop_openers(Inline& state, const u32 index) noexcept {
    for (usize stack = 0; stack < stack_count; ++stack) {
        while (state.tops[stack] != none && state.tops[stack] >= index) state.tops[stack] = state.marks[state.tops[stack]].next;
    }
}

void resolve_range(Inline& state, const u32 opener, const u32 closer) noexcept {
    state.marks[opener].next = closer;
    state.marks[closer].previous = opener;
    state.marks[opener].flags = state.marks[opener].flags | mark_flags::opener | mark_flags::resolved;
    state.marks[closer].flags = state.marks[closer].flags | mark_flags::closer | mark_flags::resolved;
}

void disable_marks(Inline& state, const u32 first, const u32 limit) noexcept {
    for (u32 index = first; index < limit; ++index) {
        state.marks[index].ch = dummy;
        state.marks[index].flags = 0;
    }
}

void reset_stacks(Inline& state) noexcept {
    for (usize stack = 0; stack < stack_count; ++stack) state.tops[stack] = none;
}

// The stack an emphasis mark of `ch` with `flags` waits on as an opener.
[[nodiscard]] usize emphasis_stack(const char ch, const bool both, const u32 mod3) noexcept {
    const usize result = (ch == '*' ? 0 : 6) + (both ? 3 : 0) + mod3;
    return result;
}

[[nodiscard]] u32 mod3_of(const u8 flags) noexcept {
    const u32 result = static_cast<u32>((flags & mark_flags::mod3_mask) >> mark_flags::mod3_shift) - 1;
    return result;
}

// Splits the last `count` characters of an emphasis run off into the dummy
// mark kept for them, and returns that mark's index.
[[nodiscard]] u32 split_run(Inline& state, const u32 index, const u32 count) noexcept {
    Mark& mark = state.marks[index];
    const u32 piece = index + (mark.end - mark.begin - count);
    state.marks[piece] = mark;
    mark.end = mark.end - count;
    state.marks[piece].begin = mark.end;
    return piece;
}

void analyze_emphasis(Inline& state, const u32 index) noexcept {
    const Mark mark = state.marks[index];
    if ((mark.flags & mark_flags::potential_closer) != 0) {
        const u32 mod3 = mod3_of(mark.flags);
        const bool both = (mark.flags & mark_flags::both) != 0;
        // The rule of three: when either side can both open and close, the
        // two runs' lengths must not add up to a multiple of three unless
        // both are multiples of three.
        usize allowed[6] = {};
        usize allowed_count = 0;
        allowed[allowed_count] = emphasis_stack(mark.ch, true, 0);
        allowed_count = allowed_count + 1;
        if (mod3 != 2) {
            allowed[allowed_count] = emphasis_stack(mark.ch, true, 1);
            allowed_count = allowed_count + 1;
        }
        if (mod3 != 1) {
            allowed[allowed_count] = emphasis_stack(mark.ch, true, 2);
            allowed_count = allowed_count + 1;
        }
        allowed[allowed_count] = emphasis_stack(mark.ch, false, 0);
        allowed_count = allowed_count + 1;
        if (!both || mod3 != 2) {
            allowed[allowed_count] = emphasis_stack(mark.ch, false, 1);
            allowed_count = allowed_count + 1;
        }
        if (!both || mod3 != 1) {
            allowed[allowed_count] = emphasis_stack(mark.ch, false, 2);
            allowed_count = allowed_count + 1;
        }
        // The nearest opener among those stacks' tops.
        u32 opener = none;
        usize opener_stack = 0;
        for (usize choice = 0; choice < allowed_count; ++choice) {
            const u32 top = state.tops[allowed[choice]];
            if (top == none) continue;
            if (opener == none || state.marks[top].end > state.marks[opener].end) {
                opener = top;
                opener_stack = allowed[choice];
            }
        }
        if (opener != none) {
            const u32 opener_size = state.marks[opener].end - state.marks[opener].begin;
            const u32 closer_size = mark.end - mark.begin;
            if (opener_size > closer_size) {
                // The inner part of the opener is matched; the outer part
                // stays on its stack, shorter.
                opener = split_run(state, opener, closer_size);
                push_opener(state, opener_stack, opener);
            } else if (opener_size < closer_size) {
                // The rest of the closer is met again as the next mark.
                static_cast<void>(split_run(state, index, closer_size - opener_size));
            }
            pop_openers(state, opener);
            resolve_range(state, opener, index);
            return;
        }
    }
    if ((mark.flags & mark_flags::potential_opener) != 0) {
        const usize stack = emphasis_stack(mark.ch, (mark.flags & mark_flags::both) != 0, mod3_of(mark.flags));
        push_opener(state, stack, index);
    }
}

// Strikethrough: one or two tildes, closed by a run of the same length.
void analyze_tilde(Inline& state, const u32 index) noexcept {
    const Mark mark = state.marks[index];
    const usize stack = tilde_stack + (mark.end - mark.begin == 1 ? 0 : 1);
    if ((mark.flags & mark_flags::potential_closer) != 0 && state.tops[stack] != none) {
        const u32 opener = state.tops[stack];
        pop_openers(state, opener);
        resolve_range(state, opener, index);
        return;
    }
    if ((mark.flags & mark_flags::potential_opener) != 0) push_opener(state, stack, index);
}

// Pairs each ']' with the nearest unpaired '[' and lists the pair.
[[nodiscard]] bool analyze_bracket(Context& ctx, Inline& state, const u32 index) noexcept {
    Mark& mark = state.marks[index];
    if ((mark.flags & mark_flags::potential_opener) != 0) {
        const u32 outer = state.tops[bracket_stack];
        if (outer != none) state.marks[outer].flags = state.marks[outer].flags | mark_flags::has_nested;
        push_opener(state, bracket_stack, index);
        return true;
    }
    const u32 opener = state.tops[bracket_stack];
    if (opener == none) return true;
    state.tops[bracket_stack] = state.marks[opener].next;
    state.marks[opener].next = index;
    mark.previous = opener;
    if (ctx.bracket_pairs.push_copy(opener).failed()) return fail(ctx, ErrorKind::out_of_memory);
    return true;
}

// One pass over marks [first, limit): brackets when `brackets`, else
// emphasis and strikethrough. Resolved ranges are stepped over whole.
[[nodiscard]] bool analyze_marks(Context& ctx, Inline& state, const u32 first, const u32 limit, const bool brackets) noexcept {
    u32 last_end = ctx.lines[state.first_line].begin;
    u32 index = first;
    while (index < limit) {
        const Mark mark = state.marks[index];
        if ((mark.flags & mark_flags::resolved) != 0) {
            index = (mark.flags & mark_flags::opener) != 0 ? mark.next + 1 : index + 1;
            continue;
        }
        const bool is_bracket = mark.ch == '[' || mark.ch == ']';
        const bool is_emphasis = mark.ch == '*' || mark.ch == '_' || mark.ch == '~';
        // A mark inside what an earlier one grew over is not looked at.
        if ((brackets ? !is_bracket : !is_emphasis) || mark.begin < last_end) {
            index = index + 1;
            continue;
        }
        if (is_bracket) {
            if (!analyze_bracket(ctx, state, index)) return false;
        } else if (mark.ch == '~') {
            analyze_tilde(state, index);
        } else {
            analyze_emphasis(state, index);
        }
        const Mark after = state.marks[index];
        if ((after.flags & mark_flags::resolved) != 0) {
            last_end = (after.flags & mark_flags::opener) != 0 ? state.marks[after.next].end : after.end;
        }
        index = index + 1;
    }
    return true;
}

// Emphasis and strikethrough among marks [first, limit), which no span
// outside them may join.
[[nodiscard]] bool analyze_span_contents(Context& ctx, Inline& state, const u32 first, const u32 limit) noexcept {
    const bool analyzed = analyze_marks(ctx, state, first, limit, false);
    reset_stacks(state);
    return analyzed;
}

// The label of the bracket pair (opener, closer), without the brackets.
[[nodiscard]] u32 pair_definition(Context& ctx, const Inline& state, const Mark& opener, const Mark& closer) noexcept {
    const u32 result = find_definition(ctx, state.first_line, state.line_limit - state.first_line, opener.end, closer.begin);
    return result;
}

// Decides the listed bracket pairs, inner to outer.
[[nodiscard]] bool resolve_brackets(Context& ctx, Inline& state) noexcept {
    const usize pair_count = ctx.bracket_pairs.count();
    u32 last_link_begin = 0;
    u32 last_link_end = 0;
    u32 last_image_begin = 0;
    u32 last_image_end = 0;
    usize pair = 0;
    while (pair < pair_count) {
        const u32 opener_index = ctx.bracket_pairs[pair];
        Mark& opener = state.marks[opener_index];
        pair = pair + 1;
        // Disabled by a link made earlier.
        if (opener.ch == dummy) continue;
        const u32 closer_index = opener.next;
        Mark& closer = state.marks[closer_index];
        const bool is_image = (opener.flags & mark_flags::can_be_image) != 0;
        // The mark begins at the '!' of an image.
        const u32 opener_begin = is_image ? opener.begin - 1 : opener.begin;
        // An outer pair cannot end inside the "(...)" of an inner link or
        // image, and a link cannot hold a link.
        if ((opener_begin < last_link_begin && closer.end < last_link_end) ||
            (opener_begin < last_image_begin && closer.end < last_image_end) ||
            (opener_begin < last_link_end && !is_image)) {
            continue;
        }
        const u32 next_index = pair < pair_count ? ctx.bracket_pairs[pair] : none;
        bool is_link = false;
        u32 attribute = 0;
        if (next_index != none && state.marks[next_index].ch != dummy && state.marks[next_index].begin == closer.end &&
            (state.marks[next_index].flags & mark_flags::can_be_image) == 0) {
            // "[text][label]" or "[text][]".
            const Mark& next_opener = state.marks[next_index];
            const u32 next_closer_index = next_opener.next;
            const Mark& next_closer = state.marks[next_closer_index];
            u32 definition = none;
            if (next_closer.begin > closer.end + 1) {
                if ((next_opener.flags & mark_flags::has_nested) == 0) {
                    definition = pair_definition(ctx, state, next_opener, next_closer);
                }
            } else if ((opener.flags & mark_flags::has_nested) == 0) {
                definition = pair_definition(ctx, state, opener, closer);
            }
            if (definition != none) {
                if (!definition_attributes(ctx, definition, attribute)) return false;
                is_link = true;
                // The second pair is the link's, and nothing inside it is
                // looked at again.
                closer.end = next_closer.end;
                disable_marks(state, closer_index + 1, next_closer_index + 1);
                pair = pair + 1;
            }
        } else {
            if (closer.end < ctx.size && ctx.text[closer.end] == '(') {
                u32 target_end = 0;
                LinkTarget target{};
                if (inline_link_target(ctx, state.first_line, state.line_limit - state.first_line, closer.end, target_end,
                                       target)) {
                    is_link = true;
                    // The ")" must not lie inside something already
                    // decided, such as a code span.
                    u32 following = closer_index + 1;
                    while (following < state.mark_count) {
                        const Mark& inside = state.marks[following];
                        if (inside.begin >= target_end) break;
                        const u8 open = mark_flags::opener | mark_flags::resolved;
                        if ((inside.flags & open) == open) {
                            if (state.marks[inside.next].begin >= target_end) {
                                is_link = false;
                                break;
                            }
                            following = inside.next + 1;
                        } else {
                            following = following + 1;
                        }
                    }
                    if (is_link) {
                        if (!target_attributes(ctx, target, attribute)) return false;
                        closer.end = target_end;
                        disable_marks(state, closer_index + 1, following);
                    }
                }
            }
            if (!is_link && (opener.flags & mark_flags::has_nested) == 0) {
                // "[label]" alone.
                const u32 definition = pair_definition(ctx, state, opener, closer);
                if (definition != none) {
                    if (!definition_attributes(ctx, definition, attribute)) return false;
                    is_link = true;
                }
            }
        }
        if (!is_link) continue;
        opener.flags = opener.flags | mark_flags::opener | mark_flags::resolved;
        closer.flags = closer.flags | mark_flags::closer | mark_flags::resolved;
        // The dummy after the opener keeps the attribute index.
        state.marks[opener_index + 1].begin = attribute;
        if (is_image) {
            opener.begin = opener_begin;
            last_image_begin = opener_begin;
            last_image_end = closer.end;
        } else {
            last_link_begin = opener_begin;
            last_link_end = closer.end;
        }
        if (!analyze_span_contents(ctx, state, opener_index + 2, closer_index)) return false;
    }
    return true;
}

// ---- emitting ------------------------------------------------------------------------

[[nodiscard]] bool emit(Context& ctx, const EventKind kind, const u8 type, const u32 begin, const u32 end) noexcept {
    if (!reserve_events(ctx, 1)) return false;
    const Event event{.kind = kind, .type = type, .flags = 0, .data = 0, .begin = begin, .end = end, .detail = 0};
    put_event(ctx, event);
    return true;
}

[[nodiscard]] bool emit_text(Context& ctx, const TextType type, const u32 begin, const u32 end) noexcept {
    const bool emitted = emit(ctx, EventKind::text, static_cast<u8>(type), begin, end);
    return emitted;
}

[[nodiscard]] bool emit_span(Context& ctx, const bool enter, const SpanType type, const u32 begin, const u32 end) noexcept {
    const EventKind kind = enter ? EventKind::enter_span : EventKind::leave_span;
    const bool emitted = emit(ctx, kind, static_cast<u8>(type), begin, end);
    return emitted;
}

// A link's or an image's enter or leave event.
[[nodiscard]] bool emit_link(Context& ctx, const bool enter, const SpanType type, const u32 begin, const u32 end,
                             const u32 attribute, const u8 flags) noexcept {
    if (!reserve_events(ctx, 1)) return false;
    const Event event{.kind = enter ? EventKind::enter_span : EventKind::leave_span,
                      .type = static_cast<u8>(type),
                      .flags = flags,
                      .data = 0,
                      .begin = begin,
                      .end = end,
                      .detail = attribute};
    put_event(ctx, event);
    return true;
}

// A character reference: its decoded text goes to the document's text.
[[nodiscard]] bool emit_reference(Context& ctx, const u32 begin, const u32 end) noexcept {
    char decoded[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const u32 length = decode_reference(ctx.text, begin, end, decoded);
    TextRange range{};
    if (!append_text(ctx, decoded, length, range)) return false;
    if (!reserve_events(ctx, 1)) return false;
    const Event event{.kind = EventKind::text,
                      .type = static_cast<u8>(TextType::reference),
                      .flags = 0,
                      .data = static_cast<u8>(length),
                      .begin = begin,
                      .end = end,
                      .detail = range.begin};
    put_event(ctx, event);
    return true;
}

// Where the line ending after `offset` (which is at it, or at blanks
// before it) ends.
[[nodiscard]] u32 line_ending_end(const Context& ctx, const u32 offset) noexcept {
    u32 end = offset;
    if (end < ctx.size && ctx.text[end] == '\r') end = end + 1;
    if (end < ctx.size && ctx.text[end] == '\n') end = end + 1;
    return end;
}

[[nodiscard]] bool emit_marks(Context& ctx, Inline& state) noexcept {
    u32 line = state.first_line;
    u32 offset = ctx.lines[line].begin;
    const u32 end = ctx.lines[state.line_limit - 1].end;
    u32 index = 0;
    while ((state.marks[index].flags & mark_flags::resolved) == 0) index = index + 1;
    TextType text_type = TextType::literal;
    // Where the backslash of a hard break stands, when the line has one.
    u32 hard_break_at = none;
    for (;;) {
        const Mark mark = state.marks[index];
        const u32 line_end = ctx.lines[line].end;
        const u32 text_end = line_end < mark.begin ? line_end : mark.begin;
        if (text_end > offset) {
            if (!emit_text(ctx, text_type, offset, text_end)) return false;
            offset = text_end;
        }
        if (offset >= mark.begin) {
            if (mark.ch == sentinel) return true;
            const bool opens = (mark.flags & mark_flags::opener) != 0;
            if (mark.ch == '\\') {
                if (is_newline(ctx.text[mark.begin + 1])) {
                    hard_break_at = mark.begin;
                } else {
                    if (!emit_text(ctx, text_type, mark.begin + 1, mark.begin + 2)) return false;
                }
            } else if (mark.ch == '`') {
                if (!emit_span(ctx, opens, SpanType::code, mark.begin, mark.end)) return false;
                text_type = opens ? TextType::code : TextType::literal;
                // A closer at the start of a line begins at the end of the
                // line before it: the line it ends on is the current one.
                // (MD4C stays on the earlier line and sends that line's
                // ending twice.)
                if (!opens) {
                    while (mark.end > ctx.lines[line].end) line = line + 1;
                }
            } else if (mark.ch == '*' || mark.ch == '_') {
                // A run of n: n / 2 strong spans, and one emphasis if n is
                // odd, which is the outermost.
                u32 at = mark.begin;
                if (opens) {
                    if ((mark.end - at) % 2 != 0) {
                        if (!emit_span(ctx, true, SpanType::emphasis, at, at + 1)) return false;
                        at = at + 1;
                    }
                    while (at + 1 < mark.end) {
                        if (!emit_span(ctx, true, SpanType::strong, at, at + 2)) return false;
                        at = at + 2;
                    }
                } else {
                    while (at + 1 < mark.end) {
                        if (!emit_span(ctx, false, SpanType::strong, at, at + 2)) return false;
                        at = at + 2;
                    }
                    if ((mark.end - at) % 2 != 0) {
                        if (!emit_span(ctx, false, SpanType::emphasis, at, at + 1)) return false;
                    }
                }
            } else if (mark.ch == '~') {
                if (!emit_span(ctx, opens, SpanType::strikethrough, mark.begin, mark.end)) return false;
            } else if (mark.ch == '[' || mark.ch == ']') {
                const u32 opener_index = mark.ch == '[' ? index : mark.previous;
                const Mark& opener = state.marks[opener_index];
                const SpanType type = (opener.flags & mark_flags::can_be_image) != 0 ? SpanType::image : SpanType::link;
                const u32 attribute = state.marks[opener_index + 1].begin;
                if (!emit_link(ctx, mark.ch == '[', type, mark.begin, mark.end, attribute, 0)) return false;
                // What follows the "]" may run over lines.
                if (mark.ch == ']') {
                    while (mark.end > ctx.lines[line].end) line = line + 1;
                }
            } else if (mark.ch == '<' || mark.ch == '>') {
                const u32 opener_index = opens ? index : mark.previous;
                const Mark& opener = state.marks[opener_index];
                u8 flags = event_flags::autolink;
                if ((opener.flags & mark_flags::mail) != 0) flags = flags | event_flags::mail_address;
                u32 attribute = 0;
                if (opens) {
                    // The destination as written (references decoded, no
                    // escapes), and an empty title.
                    const u32 closer_begin = state.marks[opener.next].begin;
                    if (!build_attribute(ctx, opener.end, closer_begin, false, attribute)) return false;
                    u32 title = 0;
                    if (!build_attribute(ctx, closer_begin, closer_begin, false, title)) return false;
                    state.marks[opener_index].previous = attribute;
                } else {
                    attribute = opener.previous;
                }
                if (!emit_link(ctx, opens, SpanType::link, mark.begin, mark.end, attribute, flags)) return false;
            } else if (mark.ch == '&') {
                if (!emit_reference(ctx, mark.begin, mark.end)) return false;
            } else if (mark.ch == '\0') {
                if (!emit_text(ctx, TextType::null_character, mark.begin, mark.end)) return false;
            }
            offset = mark.end;
            index = index + 1;
            while ((state.marks[index].flags & mark_flags::resolved) == 0 || state.marks[index].begin < offset) {
                index = index + 1;
            }
        }
        if (offset >= ctx.lines[line].end) {
            if (offset >= end) return true;
            if (text_type == TextType::code) {
                // Inside a code span the blanks before the line ending are
                // code, and the line ending is one space.
                const u32 blanks = offset;
                while (offset < ctx.size && is_blank(ctx.text[offset])) offset = offset + 1;
                if (offset > blanks) {
                    if (!emit_text(ctx, TextType::code, blanks, offset)) return false;
                }
                if (offset < state.marks[index].begin && offset < ctx.size && is_newline(ctx.text[offset])) {
                    if (!emit_text(ctx, TextType::code_space, offset, line_ending_end(ctx, offset))) return false;
                }
            } else {
                if (hard_break_at != none) {
                    // The backslash and the line ending it stands before.
                    const u32 ending_end = line_ending_end(ctx, hard_break_at + 1);
                    if (!emit_text(ctx, TextType::hard_break, hard_break_at, ending_end)) return false;
                } else {
                    const u32 content_end = ctx.lines[line].end;
                    while (offset < ctx.size && is_blank(ctx.text[offset])) offset = offset + 1;
                    const u32 ending_end = line_ending_end(ctx, offset);
                    const bool two_spaces = offset >= content_end + 2 && ctx.text[offset - 2] == ' ' &&
                                            ctx.text[offset - 1] == ' ' && offset < ctx.size && is_newline(ctx.text[offset]);
                    if (two_spaces) {
                        if (!emit_text(ctx, TextType::hard_break, content_end, ending_end)) return false;
                    } else {
                        if (!emit_text(ctx, TextType::soft_break, offset, ending_end)) return false;
                    }
                }
            }
            line = line + 1;
            if (offset < ctx.lines[line].begin) offset = ctx.lines[line].begin;
            hard_break_at = none;
        }
    }
}

} // namespace

bool write_inlines(Context& ctx, const u32 first_line, const u32 line_count) noexcept {
    if (line_count == 0) return true;
    ctx.marks.clear();
    ctx.bracket_pairs.clear();
    Inline state{};
    state.first_line = first_line;
    state.line_limit = first_line + line_count;
    reset_stacks(state);
    if (!collect_marks(ctx, state)) return false;
    // No mark is added from here on, so the table does not move.
    state.marks = ctx.marks.mutable_view().data();
    state.mark_count = static_cast<u32>(ctx.marks.count());
    if (!analyze_marks(ctx, state, 0, state.mark_count, true)) return false;
    if (!resolve_brackets(ctx, state)) return false;
    reset_stacks(state);
    if (!analyze_span_contents(ctx, state, 0, state.mark_count)) return false;
    const bool emitted = emit_marks(ctx, state);
    return emitted;
}

} // namespace stxlib::markdown::detail
