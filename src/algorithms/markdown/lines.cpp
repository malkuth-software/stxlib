// stxlib::markdown, phase A: the source read once, line by line, into block
// records and line records (internal.hpp). After MD4C 0.6.0's
// md_analyze_line and md_process_line, with typed tables in place of its
// packed byte buffer and without the constructs this dialect does not read
// (raw HTML, tables, task marks, admonitions, footnotes).
//
// A line is classified against the line before it of the same leaf block
// (the "pivot") and against the open containers: which of them it is still
// inside, whether it continues a list with a new item (a "brother"), and
// which new containers it opens ("children"). The loop that does so turns
// once per container mark on the line, and each turn that goes round again
// has taken a mark, so it ends within the line's length.
//
// Two places differ from MD4C on purpose, each closer to CommonMark:
// - A list item that begins with a blank line ends at a second one. MD4C
//   decides "the item has just begun" by reading the last eight bytes of
//   its buffer as a block header, which is a line record when a leaf block
//   was the last thing written; here the last record is looked at as what
//   it is.
// - A closing code fence is looked for only on a line that is still inside
//   every container of the code block. MD4C tests it first, so a fence
//   line after the block's container has ended is taken for the closing
//   fence (and lost), or scanned past and read as text.

#include "algorithms/markdown/internal.hpp"

namespace stxlib::markdown::detail {

namespace {

enum class LineType : u8 {
    blank = 0,
    rule = 1,
    atx_heading = 2,
    setext_underline = 3,
    indented_code = 4,
    fenced_code = 5,
    text = 6,
};

struct LineAnalysis final {
    LineType type{LineType::blank};
    // atx_heading, setext_underline: the level.
    u32 data{0};
    bool enforce_new_block{false};
    u32 begin{0};
    u32 end{0};
    u32 indent{0};
};

[[nodiscard]] bool at_line_end(const Context& ctx, const u32 offset) noexcept {
    const bool result = offset >= ctx.size || is_newline(ctx.text[offset]);
    return result;
}

// The columns of blank space from `begin`, counted from column
// `total_indent` (a tab goes to the next multiple of four). `end` is the
// first byte that is not blank.
[[nodiscard]] u32 line_indentation(const Context& ctx, const u32 total_indent, const u32 begin, u32& end) noexcept {
    u32 offset = begin;
    u32 indent = total_indent;
    while (offset < ctx.size && is_blank(ctx.text[offset])) {
        if (ctx.text[offset] == '\t') {
            indent = (indent + 4) & ~static_cast<u32>(3);
        } else {
            indent = indent + 1;
        }
        offset = offset + 1;
    }
    end = offset;
    const u32 result = indent - total_indent;
    return result;
}

[[nodiscard]] bool is_setext_underline(const Context& ctx, const u32 begin, u32& end, u32& level) noexcept {
    const char mark = ctx.text[begin];
    u32 offset = begin + 1;
    while (offset < ctx.size && ctx.text[offset] == mark) offset = offset + 1;
    while (offset < ctx.size && is_blank(ctx.text[offset])) offset = offset + 1;
    if (!at_line_end(ctx, offset)) return false;
    level = mark == '=' ? 1 : 2;
    end = offset;
    return true;
}

// `begin` is at a '#'. On true `content` is where the heading's text starts.
[[nodiscard]] bool is_atx_heading(const Context& ctx, const u32 begin, u32& content, u32& level) noexcept {
    u32 offset = begin + 1;
    while (offset < ctx.size && ctx.text[offset] == '#' && offset - begin < 7) offset = offset + 1;
    const u32 marks = offset - begin;
    if (marks > 6) return false;
    if (offset < ctx.size && !is_blank(ctx.text[offset]) && !is_newline(ctx.text[offset])) return false;
    while (offset < ctx.size && is_blank(ctx.text[offset])) offset = offset + 1;
    level = marks;
    content = offset;
    return true;
}

// `begin` is at a backtick or a tilde. On true `length` is the fence's.
[[nodiscard]] bool is_opening_fence(const Context& ctx, const u32 begin, u32& length) noexcept {
    const char mark = ctx.text[begin];
    u32 offset = begin;
    while (offset < ctx.size && ctx.text[offset] == mark) offset = offset + 1;
    const u32 run = offset - begin;
    if (run < 3) return false;
    // A backtick fence's info string holds no backtick.
    if (mark == '`') {
        while (offset < ctx.size && !is_newline(ctx.text[offset])) {
            if (ctx.text[offset] == '`') return false;
            offset = offset + 1;
        }
    }
    length = run;
    return true;
}

[[nodiscard]] bool is_closing_fence(const Context& ctx, const char mark, const u32 begin) noexcept {
    u32 offset = begin;
    while (offset < ctx.size && ctx.text[offset] == mark) offset = offset + 1;
    if (offset - begin < ctx.fence_length) return false;
    while (offset < ctx.size && is_blank(ctx.text[offset])) offset = offset + 1;
    const bool result = at_line_end(ctx, offset);
    return result;
}

// Whether a container mark stands at `begin`, on a line indented `indent`.
// On true `end` is just after the mark and `container` describes it.
[[nodiscard]] bool is_container_mark(const Context& ctx, const u32 indent, const u32 begin, u32& end,
                                     Container& container) noexcept {
    if (begin >= ctx.size || indent >= code_indent) return false;
    const char first = ctx.text[begin];
    if (first == '>') {
        container = Container{.ch = '>', .start = 0, .mark_indent = indent, .contents_indent = indent + 1, .block_index = 0};
        end = begin + 1;
        return true;
    }
    if ((first == '-' || first == '+' || first == '*') &&
        (begin + 1 >= ctx.size || is_blank(ctx.text[begin + 1]) || is_newline(ctx.text[begin + 1]))) {
        container = Container{.ch = first, .start = 0, .mark_indent = indent, .contents_indent = indent + 1, .block_index = 0};
        end = begin + 1;
        return true;
    }
    // An ordered item's number: at most nine digits, so it fits 32 bits.
    u32 limit = ctx.size;
    if (ctx.size - begin > 9) limit = begin + 9;
    u32 offset = begin;
    u32 start = 0;
    while (offset < limit && is_digit(ctx.text[offset])) {
        start = start * 10 + static_cast<u32>(ctx.text[offset] - '0');
        offset = offset + 1;
    }
    if (offset > begin && offset < ctx.size && (ctx.text[offset] == '.' || ctx.text[offset] == ')') &&
        (offset + 1 >= ctx.size || is_blank(ctx.text[offset + 1]) || is_newline(ctx.text[offset + 1]))) {
        container = Container{.ch = ctx.text[offset],
                              .start = start,
                              .mark_indent = indent,
                              .contents_indent = indent + (offset - begin) + 1,
                              .block_index = 0};
        end = offset + 1;
        return true;
    }
    return false;
}

// Whether a new item marked `candidate` continues the list `pivot`.
[[nodiscard]] bool continues_list(const Container& pivot, const Container& candidate) noexcept {
    if (candidate.ch == '>') return false;
    if (candidate.ch != pivot.ch) return false;
    const bool result = candidate.mark_indent <= pivot.contents_indent;
    return result;
}

[[nodiscard]] bool is_ordered(const char ch) noexcept {
    const bool result = ch == '.' || ch == ')';
    return result;
}

// ---- records -------------------------------------------------------------------------

[[nodiscard]] bool push_record(Context& ctx, const BlockRecord& record) noexcept {
    if (ctx.blocks.count() >= ctx.limits.records) return fail(ctx, ErrorKind::limit_exceeded);
    if (ctx.blocks.push_copy(record).failed()) return fail(ctx, ErrorKind::out_of_memory);
    return true;
}

[[nodiscard]] bool end_current_block(Context& ctx) noexcept;

[[nodiscard]] bool push_container_record(Context& ctx, const BlockType type, const RecordKind kind, const char data,
                                         const u32 number) noexcept {
    if (!end_current_block(ctx)) return false;
    const BlockRecord record{
        .type = type, .kind = kind, .flags = 0, .data = static_cast<u8>(data), .first_line = 0, .line_count = 0, .number = number};
    const bool pushed = push_record(ctx, record);
    return pushed;
}

[[nodiscard]] bool push_container(Context& ctx, const Container& container) noexcept {
    if (ctx.containers.count() >= ctx.limits.containers) return fail(ctx, ErrorKind::limit_exceeded);
    if (ctx.containers.push_copy(container).failed()) return fail(ctx, ErrorKind::out_of_memory);
    if (container.ch == '>' && ctx.quotes.push_copy(static_cast<u32>(ctx.containers.count() - 1)).failed()) {
        ctx.containers.remove_last();
        return fail(ctx, ErrorKind::out_of_memory);
    }
    return true;
}

// The first open quote at or after the container `from`, or the number of
// open containers when there is none: one binary search over the quotes.
[[nodiscard]] usize next_quote(const Context& ctx, const usize from) noexcept {
    usize low = 0;
    usize high = ctx.quotes.count();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (ctx.quotes[middle] < from) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const usize found = low < ctx.quotes.count() ? ctx.quotes[low] : ctx.containers.count();
    return found;
}

// Opens the last `children` containers: a list and its first item, or a quote.
[[nodiscard]] bool enter_child_containers(Context& ctx, const u32 children) noexcept {
    const usize count = ctx.containers.count();
    for (usize index = count - children; index < count; ++index) {
        const char ch = ctx.containers[index].ch;
        if (ch == '>') {
            if (!push_container_record(ctx, BlockType::quote, RecordKind::open, 0, 0)) return false;
            continue;
        }
        // The list's record is found again if the list turns out loose.
        if (!end_current_block(ctx)) return false;
        ctx.containers[index].block_index = static_cast<u32>(ctx.blocks.count());
        const BlockType list = is_ordered(ch) ? BlockType::ordered_list : BlockType::bullet_list;
        if (!push_container_record(ctx, list, RecordKind::open, ch, ctx.containers[index].start)) return false;
        if (!push_container_record(ctx, BlockType::list_item, RecordKind::open, 0, 0)) return false;
    }
    return true;
}

// Closes containers until `keep` remain.
[[nodiscard]] bool leave_child_containers(Context& ctx, const usize keep) noexcept {
    while (ctx.containers.count() > keep) {
        const char ch = ctx.containers.last_element().ch;
        if (ch == '>') {
            if (!push_container_record(ctx, BlockType::quote, RecordKind::close, 0, 0)) return false;
        } else {
            const BlockType list = is_ordered(ch) ? BlockType::ordered_list : BlockType::bullet_list;
            if (!push_container_record(ctx, BlockType::list_item, RecordKind::close, 0, 0)) return false;
            if (!push_container_record(ctx, list, RecordKind::close, ch, 0)) return false;
        }
        ctx.containers.remove_last();
        if (ch == '>') ctx.quotes.remove_last();
    }
    return true;
}

[[nodiscard]] bool start_new_block(Context& ctx, const LineAnalysis& line) noexcept {
    STXLIB_REQUIRE(ctx.current_block == none, "markdown: a block is already open");
    BlockRecord record{};
    record.kind = RecordKind::leaf;
    record.first_line = static_cast<u32>(ctx.lines.count());
    if (line.type == LineType::rule) {
        record.type = BlockType::rule;
    } else if (line.type == LineType::atx_heading) {
        record.type = BlockType::heading;
        record.data = static_cast<u8>(line.data);
    } else if (line.type == LineType::fenced_code) {
        record.type = BlockType::code_block;
        record.data = static_cast<u8>(ctx.text[line.begin]);
    } else if (line.type == LineType::indented_code) {
        record.type = BlockType::code_block;
    } else {
        record.type = BlockType::paragraph;
    }
    ctx.current_block = static_cast<u32>(ctx.blocks.count());
    const bool pushed = push_record(ctx, record);
    return pushed;
}

[[nodiscard]] bool add_line(Context& ctx, const LineAnalysis& line) noexcept {
    STXLIB_REQUIRE(ctx.current_block != none, "markdown: no block is open");
    const Line record{.begin = line.begin, .end = line.end, .indent = line.indent};
    if (ctx.lines.count() >= ctx.limits.lines) return fail(ctx, ErrorKind::limit_exceeded);
    if (ctx.lines.push_copy(record).failed()) return fail(ctx, ErrorKind::out_of_memory);
    BlockRecord& block = ctx.blocks[ctx.current_block];
    block.line_count = block.line_count + 1;
    return true;
}

// Takes link reference definitions off the front of the current paragraph
// (they cannot interrupt one, so they are only ever at its start). The
// block's lines are the last lines of the table, so removing some from its
// front is a change of first_line.
[[nodiscard]] bool consume_definitions(Context& ctx) noexcept {
    BlockRecord& block = ctx.blocks[ctx.current_block];
    const u32 line_count = block.line_count;
    u32 taken = 0;
    while (taken < line_count) {
        const i32 consumed = take_definition(ctx, block.first_line + taken, line_count - taken);
        if (consumed < 0) return false;
        if (consumed == 0) break;
        taken = taken + static_cast<u32>(consumed);
    }
    if (taken == 0) return true;
    // What follows the definitions may be a thematic break that was read
    // as a setext underline while there was still a paragraph above it.
    bool inject_rule = false;
    if (taken < line_count) {
        u32 ignored_end = 0;
        u32 ignored_killer = 0;
        if (is_rule_line(ctx, ctx.lines[block.first_line + taken].begin, ignored_end, ignored_killer)) {
            inject_rule = true;
            taken = taken + 1;
        }
    }
    // The rule keeps its own line, as a rule read as one does.
    const Line rule_line = inject_rule ? ctx.lines[block.first_line + taken - 1] : Line{};
    if (taken == line_count) {
        // Nothing is left: undo the block.
        ctx.lines.truncate(block.first_line);
        ctx.blocks.remove_last();
        ctx.current_block = none;
    } else {
        block.first_line = block.first_line + taken;
        block.line_count = line_count - taken;
    }
    if (inject_rule) {
        BlockRecord rule{
            .type = BlockType::rule, .kind = RecordKind::leaf, .flags = 0, .data = 0, .first_line = 0, .line_count = 1, .number = 0};
        if (ctx.current_block == none) {
            // The line was the last of the table and was taken off with the block.
            rule.first_line = static_cast<u32>(ctx.lines.count());
            if (ctx.lines.push_copy(rule_line).failed()) return fail(ctx, ErrorKind::out_of_memory);
            if (!push_record(ctx, rule)) return false;
        } else {
            // The rule goes before what is left of the paragraph; its line
            // is still in the table, just before what is left.
            const BlockRecord rest = ctx.blocks[ctx.current_block];
            rule.first_line = rest.first_line - 1;
            ctx.blocks[ctx.current_block] = rule;
            ctx.current_block = static_cast<u32>(ctx.blocks.count());
            if (!push_record(ctx, rest)) return false;
        }
    }
    return true;
}

[[nodiscard]] bool end_current_block(Context& ctx) noexcept {
    if (ctx.current_block == none) return true;
    const BlockRecord opened = ctx.blocks[ctx.current_block];
    const bool is_setext = opened.type == BlockType::heading && (opened.flags & record_flags::setext) != 0;
    if (opened.type == BlockType::paragraph || is_setext) {
        const u32 first = ctx.lines[opened.first_line].begin;
        if (first < ctx.size && ctx.text[first] == '[') {
            if (!consume_definitions(ctx)) return false;
            if (ctx.current_block == none) return true;
        }
    }
    if (is_setext) {
        BlockRecord& block = ctx.blocks[ctx.current_block];
        if (block.line_count > 1) {
            // The underline is not text.
            block.line_count = block.line_count - 1;
            ctx.lines.remove_last();
        } else {
            // The definitions took everything but the underline, which is
            // then the first line of an ordinary paragraph. It stays open.
            block.type = BlockType::paragraph;
            block.flags = 0;
            block.data = 0;
            return true;
        }
    }
    ctx.current_block = none;
    return true;
}

// ---- one line ------------------------------------------------------------------------

// Classifies the line at `begin` and brings the containers up to date.
// `next` is the start of the line after it.
[[nodiscard]] bool analyze_line(Context& ctx, const u32 begin, u32& next, const LineAnalysis& pivot,
                                LineAnalysis& line) noexcept {
    u32 total_indent = 0;
    usize parents = 0;
    u32 brothers = 0;
    u32 children = 0;
    Container container{};
    const bool previous_line_loosens = ctx.last_line_loosens;
    u32 offset = begin;
    u32 rule_killer = 0;
    // The pivot, which a container mark replaces with a blank line.
    LineType pivot_type = pivot.type;
    u32 pivot_begin = pivot.begin;
    u32 pivot_indent = pivot.indent;

    line = LineAnalysis{};
    line.indent = line_indentation(ctx, total_indent, offset, offset);
    total_indent = total_indent + line.indent;
    line.begin = offset;

    // Which of the open containers the line is inside.
    while (parents < ctx.containers.count()) {
        const Container& open = ctx.containers[parents];
        if (open.ch == '>' && line.indent < code_indent && offset < ctx.size && ctx.text[offset] == '>') {
            offset = offset + 1;
            total_indent = total_indent + 1;
            line.indent = line_indentation(ctx, total_indent, offset, offset);
            total_indent = total_indent + line.indent;
            // The one space after '>' belongs to the mark.
            if (line.indent > 0) line.indent = line.indent - 1;
            line.begin = offset;
        } else if (open.ch != '>' && line.indent >= open.contents_indent) {
            line.indent = line.indent - open.contents_indent;
        } else {
            break;
        }
        parents = parents + 1;
    }
    // A blank line needs no indentation to stay inside a list.
    if (at_line_end(ctx, offset)) {
        parents = next_quote(ctx, parents);
    }

    for (;;) {
        if (pivot_type == LineType::fenced_code && parents == ctx.containers.count()) {
            line.begin = offset;
            if (line.indent < code_indent && is_closing_fence(ctx, ctx.text[pivot_begin], offset)) {
                // The closing fence ends the block as a blank line would.
                line.type = LineType::blank;
                ctx.last_line_loosens = false;
                break;
            }
            line.indent = line.indent > pivot_indent ? line.indent - pivot_indent : 0;
            line.type = LineType::fenced_code;
            break;
        }

        if (at_line_end(ctx, offset)) {
            if (pivot_type == LineType::indented_code && parents == ctx.containers.count()) {
                line.type = LineType::indented_code;
                line.indent = line.indent > code_indent ? line.indent - code_indent : 0;
                ctx.last_line_loosens = false;
            } else {
                line.type = LineType::blank;
                ctx.consecutive_blank_lines = ctx.consecutive_blank_lines + 1;
                ctx.last_line_loosens =
                    parents > 0 && brothers + children == 0 && ctx.containers[parents - 1].ch != '>';
            }
            break;
        }
        // A list item that begins with a blank line ends at a second one.
        if (ctx.consecutive_blank_lines >= 2 && parents > 0 && parents == ctx.containers.count() &&
            ctx.containers[parents - 1].ch != '>' && brothers + children == 0 && ctx.current_block == none &&
            !ctx.blocks.empty()) {
            const BlockRecord& last = ctx.blocks.last_element();
            if (last.type == BlockType::list_item && last.kind == RecordKind::open) {
                parents = parents - 1;
                line.indent = total_indent;
                if (parents > 0) {
                    const u32 inside = ctx.containers[parents - 1].contents_indent;
                    line.indent = line.indent > inside ? line.indent - inside : 0;
                }
            }
        }
        ctx.consecutive_blank_lines = 0;
        ctx.last_line_loosens = false;

        if (line.indent < code_indent && pivot_type == LineType::text && parents == ctx.containers.count() &&
            (ctx.text[offset] == '=' || ctx.text[offset] == '-')) {
            u32 end = 0;
            u32 level = 0;
            if (is_setext_underline(ctx, offset, end, level)) {
                offset = end;
                line.type = LineType::setext_underline;
                line.data = level;
                break;
            }
        }

        if (line.indent < code_indent && offset >= rule_killer) {
            u32 end = 0;
            if (is_rule_line(ctx, offset, end, rule_killer)) {
                offset = end;
                line.type = LineType::rule;
                break;
            }
        }

        // Another item of a list already open.
        if (parents < ctx.containers.count() && brothers + children == 0) {
            u32 after_mark = 0;
            if (is_container_mark(ctx, line.indent, offset, after_mark, container) &&
                continues_list(ctx.containers[parents], container)) {
                pivot_type = LineType::blank;
                pivot_begin = 0;
                pivot_indent = 0;
                offset = after_mark;
                total_indent = total_indent + (container.contents_indent - container.mark_indent);
                line.indent = line_indentation(ctx, total_indent, offset, offset);
                total_indent = total_indent + line.indent;
                line.begin = offset;
                // Some of the space after the mark belongs to the mark.
                if (at_line_end(ctx, offset)) {
                    container.contents_indent = container.contents_indent + 1;
                } else if (line.indent <= code_indent) {
                    container.contents_indent = container.contents_indent + line.indent;
                    line.indent = 0;
                } else {
                    container.contents_indent = container.contents_indent + 1;
                    line.indent = line.indent - 1;
                }
                ctx.containers[parents].mark_indent = container.mark_indent;
                ctx.containers[parents].contents_indent = container.contents_indent;
                brothers = brothers + 1;
                continue;
            }
        }

        // Indented code, which cannot interrupt a paragraph.
        if (line.indent >= code_indent && pivot_type != LineType::text) {
            line.type = LineType::indented_code;
            line.indent = line.indent - code_indent;
            break;
        }

        // A new container.
        if (line.indent < code_indent) {
            u32 after_mark = 0;
            if (is_container_mark(ctx, line.indent, offset, after_mark, container)) {
                const bool in_paragraph = pivot_type == LineType::text && parents == ctx.containers.count();
                // A list item cannot interrupt a paragraph if it is empty,
                // or ordered and not numbered 1.
                const bool empty_item = at_line_end(ctx, after_mark) && container.ch != '>';
                const bool late_number = is_ordered(container.ch) && container.start != 1;
                if (!(in_paragraph && (empty_item || late_number))) {
                    offset = after_mark;
                    total_indent = total_indent + (container.contents_indent - container.mark_indent);
                    line.indent = line_indentation(ctx, total_indent, offset, offset);
                    total_indent = total_indent + line.indent;
                    line.begin = offset;
                    if (at_line_end(ctx, offset)) {
                        container.contents_indent = container.contents_indent + 1;
                    } else if (line.indent <= code_indent) {
                        container.contents_indent = container.contents_indent + line.indent;
                        line.indent = 0;
                    } else {
                        container.contents_indent = container.contents_indent + 1;
                        line.indent = line.indent - 1;
                    }
                    if (brothers + children == 0) {
                        pivot_type = LineType::blank;
                        pivot_begin = 0;
                        pivot_indent = 0;
                    }
                    if (children == 0) {
                        if (!leave_child_containers(ctx, parents + brothers)) return false;
                    }
                    children = children + 1;
                    if (!push_container(ctx, container)) return false;
                    continue;
                }
            }
        }

        if (line.indent < code_indent && ctx.text[offset] == '#') {
            u32 content = 0;
            u32 level = 0;
            if (is_atx_heading(ctx, offset, content, level)) {
                line.begin = content;
                offset = content;
                line.type = LineType::atx_heading;
                line.data = level;
                break;
            }
        }

        if (line.indent < code_indent && (ctx.text[offset] == '`' || ctx.text[offset] == '~')) {
            u32 length = 0;
            if (is_opening_fence(ctx, offset, length)) {
                ctx.fence_length = length;
                line.type = LineType::fenced_code;
                line.enforce_new_block = true;
                break;
            }
        }

        line.type = LineType::text;
        // A paragraph's next line continues it whatever its containers.
        if (pivot_type == LineType::text && brothers + children == 0) parents = ctx.containers.count();
        break;
    }

    offset = scan_to_newline(ctx.text, ctx.size, offset);
    line.end = offset;
    if (line.type == LineType::atx_heading) {
        // The closing run of '#', if it stands apart from the text.
        u32 end = line.end;
        while (end > line.begin && is_blank(ctx.text[end - 1])) end = end - 1;
        while (end > line.begin && ctx.text[end - 1] == '#') end = end - 1;
        if (end == line.begin || is_blank(ctx.text[end - 1])) line.end = end;
    }
    if (line.type != LineType::indented_code && line.type != LineType::fenced_code) {
        while (line.end > line.begin && is_blank(ctx.text[line.end - 1])) line.end = line.end - 1;
    }
    if (offset < ctx.size && ctx.text[offset] == '\r') offset = offset + 1;
    if (offset < ctx.size && ctx.text[offset] == '\n') offset = offset + 1;
    next = offset;

    // A line inside a list after a blank line makes the list loose.
    if (previous_line_loosens && line.type != LineType::blank && parents + brothers > 0) {
        const Container& inner = ctx.containers[parents + brothers - 1];
        if (inner.ch != '>') {
            BlockRecord& list = ctx.blocks[inner.block_index];
            list.flags = list.flags | record_flags::loose_list;
        }
    }

    if (children == 0 && parents + brothers < ctx.containers.count()) {
        if (!leave_child_containers(ctx, parents + brothers)) return false;
    }
    if (brothers > 0) {
        if (!push_container_record(ctx, BlockType::list_item, RecordKind::close, 0, 0)) return false;
        if (!push_container_record(ctx, BlockType::list_item, RecordKind::open, 0, 0)) return false;
    }
    if (children > 0) {
        if (!enter_child_containers(ctx, children)) return false;
    }
    return true;
}

// Adds the line to the blocks. `pivot` is replaced by what the next line is
// to be classified against.
[[nodiscard]] bool process_line(Context& ctx, LineAnalysis& pivot, LineAnalysis& line) noexcept {
    if (line.type == LineType::blank) {
        if (!end_current_block(ctx)) return false;
        pivot = LineAnalysis{};
        return true;
    }
    if (line.enforce_new_block) {
        if (!end_current_block(ctx)) return false;
    }
    if (line.type == LineType::rule || line.type == LineType::atx_heading) {
        if (!end_current_block(ctx)) return false;
        if (!start_new_block(ctx, line)) return false;
        if (!add_line(ctx, line)) return false;
        if (!end_current_block(ctx)) return false;
        pivot = LineAnalysis{};
        return true;
    }
    if (line.type == LineType::setext_underline) {
        STXLIB_REQUIRE(ctx.current_block != none, "markdown: an underline with no paragraph");
        BlockRecord& block = ctx.blocks[ctx.current_block];
        block.type = BlockType::heading;
        block.data = static_cast<u8>(line.data);
        block.flags = block.flags | record_flags::setext;
        if (!add_line(ctx, line)) return false;
        if (!end_current_block(ctx)) return false;
        if (ctx.current_block == none) {
            pivot = LineAnalysis{};
        } else {
            // Definitions took the heading's text: the underline is text.
            line.type = LineType::text;
            pivot = line;
        }
        return true;
    }
    if (line.type != pivot.type) {
        if (!end_current_block(ctx)) return false;
    }
    if (ctx.current_block == none) {
        if (!start_new_block(ctx, line)) return false;
        pivot = line;
    }
    const bool added = add_line(ctx, line);
    return added;
}

} // namespace

bool is_rule_line(const Context& ctx, const u32 offset, u32& end, u32& killer) noexcept {
    if (offset >= ctx.size) return false;
    const char mark = ctx.text[offset];
    if (mark != '-' && mark != '_' && mark != '*') return false;
    u32 cursor = offset + 1;
    u32 marks = 1;
    while (cursor < ctx.size && (ctx.text[cursor] == mark || is_blank(ctx.text[cursor]))) {
        if (ctx.text[cursor] == mark) marks = marks + 1;
        cursor = cursor + 1;
    }
    if (marks < 3 || !at_line_end(ctx, cursor)) {
        killer = cursor;
        return false;
    }
    end = cursor;
    return true;
}

bool read_lines(Context& ctx) noexcept {
    LineAnalysis pivot{};
    LineAnalysis line{};
    u32 offset = 0;
    u32 counted = 0;
    while (offset < ctx.size) {
        // The stop signal is read every 256 lines: a line costs at most a
        // step for each of its bytes and each open container.
        counted = counted + 1;
        if ((counted & 255U) == 0 && ctx.stop != nullptr && (*ctx.stop).requested()) {
            return fail(ctx, ErrorKind::interrupted);
        }
        u32 next = offset;
        if (!analyze_line(ctx, offset, next, pivot, line)) return false;
        if (!process_line(ctx, pivot, line)) return false;
        // Every line takes at least one byte: its ending, or what the
        // scan to it passed.
        STXLIB_REQUIRE(next > offset, "markdown: a line took no bytes");
        offset = next;
    }
    if (!end_current_block(ctx)) return false;
    if (!leave_child_containers(ctx, 0)) return false;
    const bool indexed = index_definitions(ctx);
    return indexed;
}

} // namespace stxlib::markdown::detail
