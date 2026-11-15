// stxlib::markdown, phase B: the block records written to the tape, in
// order (internal.hpp). After MD4C 0.6.0's md_process_all_blocks,
// md_process_leaf_block and md_process_code_block_contents.
//
// Events are written through reserve_events and put_event: room is made
// once per record or per line, never per byte.

#include "algorithms/markdown/internal.hpp"

namespace stxlib::markdown::detail {

namespace {

[[nodiscard]] Event block_event(const EventKind kind, const BlockType type) noexcept {
    const Event event{.kind = kind, .type = static_cast<u8>(type), .flags = 0, .data = 0, .begin = 0, .end = 0, .detail = 0};
    return event;
}

[[nodiscard]] Event text_event(const TextType type, const u32 begin, const u32 end) noexcept {
    const Event event{.kind = EventKind::text,
                      .type = static_cast<u8>(type),
                      .flags = 0,
                      .data = 0,
                      .begin = begin,
                      .end = end,
                      .detail = 0};
    return event;
}

// The opening or closing of a quote, a list or a list item.
[[nodiscard]] bool write_container(Context& ctx, const BlockRecord& record) noexcept {
    if (!reserve_events(ctx, 1)) return false;
    const bool is_list = record.type == BlockType::bullet_list || record.type == BlockType::ordered_list;
    if (record.kind == RecordKind::close) {
        Event event = block_event(EventKind::leave_block, record.type);
        event.data = record.data;
        put_event(ctx, event);
        if (is_list || record.type == BlockType::quote) ctx.loose_stack.remove_last();
        return true;
    }
    Event event = block_event(EventKind::enter_block, record.type);
    event.data = record.data;
    event.detail = record.number;
    const bool loose = (record.flags & record_flags::loose_list) != 0;
    if (is_list && !loose) event.flags = event_flags::tight_list;
    put_event(ctx, event);
    if (is_list || record.type == BlockType::quote) {
        // Inside a quote a paragraph is always a paragraph, whatever list
        // the quote is in.
        const u8 entry = (record.type == BlockType::quote || loose) ? 1 : 0;
        if (ctx.loose_stack.push_copy(entry).failed()) return fail(ctx, ErrorKind::out_of_memory);
    }
    return true;
}

// The code of one line, [begin, end): pieces of code around each NUL.
[[nodiscard]] bool write_code_text(Context& ctx, const u32 begin, const u32 end) noexcept {
    u32 piece = begin;
    for (u32 offset = begin; offset < end; ++offset) {
        if (ctx.text[offset] != '\0') continue;
        if (!reserve_events(ctx, 2)) return false;
        if (offset > piece) put_event(ctx, text_event(TextType::code, piece, offset));
        put_event(ctx, text_event(TextType::null_character, offset, offset + 1));
        piece = offset + 1;
    }
    if (end > piece) {
        if (!reserve_events(ctx, 1)) return false;
        put_event(ctx, text_event(TextType::code, piece, end));
    }
    return true;
}

[[nodiscard]] bool write_code_lines(Context& ctx, const BlockRecord& record) noexcept {
    u32 first = record.first_line;
    u32 count = record.line_count;
    if (record.data != 0) {
        // The first line of a fenced block is its fence.
        first = first + 1;
        count = count - 1;
    } else {
        // An indented block does not begin or end with blank lines.
        while (count > 0 && ctx.lines[first].begin == ctx.lines[first].end) {
            first = first + 1;
            count = count - 1;
        }
        while (count > 0 && ctx.lines[first + count - 1].begin == ctx.lines[first + count - 1].end) count = count - 1;
    }
    for (u32 index = first; index < first + count; ++index) {
        const Line line = ctx.lines[index];
        if (!reserve_events(ctx, 2)) return false;
        if (line.indent > 0) {
            Event indent = text_event(TextType::code_indent, line.begin, line.begin);
            indent.detail = line.indent;
            put_event(ctx, indent);
        }
        if (!write_code_text(ctx, line.begin, line.end)) return false;
        // The line ending, as the source has it.
        u32 ending = line.end;
        if (ending < ctx.size && ctx.text[ending] == '\r') ending = ending + 1;
        if (ending < ctx.size && ctx.text[ending] == '\n') ending = ending + 1;
        if (!reserve_events(ctx, 1)) return false;
        put_event(ctx, text_event(TextType::line_end, line.end, ending));
    }
    return true;
}

// A fenced block's info string and its first word, as two attributes.
[[nodiscard]] bool fence_attributes(Context& ctx, const BlockRecord& record, u32& attribute) noexcept {
    const Line fence = ctx.lines[record.first_line];
    const char mark = static_cast<char>(record.data);
    u32 begin = fence.begin;
    u32 end = fence.end;
    while (begin < end && ctx.text[begin] == mark) begin = begin + 1;
    while (begin < end && ctx.text[begin] == ' ') begin = begin + 1;
    while (end > begin && ctx.text[end - 1] == ' ') end = end - 1;
    if (!build_attribute(ctx, begin, end, true, attribute)) return false;
    u32 word_end = begin;
    while (word_end < end && !is_whitespace(ctx.text[word_end])) word_end = word_end + 1;
    u32 language = 0;
    const bool built = build_attribute(ctx, begin, word_end, true, language);
    return built;
}

[[nodiscard]] bool write_leaf(Context& ctx, const BlockRecord& record) noexcept {
    const bool in_tight_list = !ctx.loose_stack.empty() && ctx.loose_stack.last_element() == 0;
    // A paragraph in a tight list stands directly in its item.
    const bool wrapped = !(in_tight_list && record.type == BlockType::paragraph);
    Event enter = block_event(EventKind::enter_block, record.type);
    if (record.line_count != 0) {
        enter.begin = ctx.lines[record.first_line].begin;
        enter.end = ctx.lines[record.first_line + record.line_count - 1].end;
    } else {
        // A rule made of a line the definitions above it left behind.
        enter.begin = record.first_line < ctx.lines.count() ? ctx.lines[record.first_line].begin : ctx.size;
        enter.end = enter.begin;
    }
    Event leave = block_event(EventKind::leave_block, record.type);
    leave.begin = enter.end;
    leave.end = enter.end;
    if (record.type == BlockType::heading) {
        enter.data = record.data;
        leave.data = record.data;
        if ((record.flags & record_flags::setext) != 0) enter.flags = event_flags::setext;
    } else if (record.type == BlockType::code_block) {
        enter.data = record.data;
        leave.data = record.data;
        enter.detail = none;
        if (record.data != 0) {
            u32 attribute = 0;
            if (!fence_attributes(ctx, record, attribute)) return false;
            enter.detail = attribute;
        }
    }
    if (!reserve_events(ctx, 2)) return false;
    if (wrapped) put_event(ctx, enter);
    if (record.type == BlockType::code_block) {
        if (!write_code_lines(ctx, record)) return false;
    } else if (record.type == BlockType::paragraph || record.type == BlockType::heading) {
        if (!write_inlines(ctx, record.first_line, record.line_count)) return false;
    }
    if (!reserve_events(ctx, 1)) return false;
    if (wrapped) put_event(ctx, leave);
    return true;
}

} // namespace

bool write_blocks(Context& ctx) noexcept {
    if (!reserve_events(ctx, 1)) return false;
    Event document = block_event(EventKind::enter_block, BlockType::document);
    document.end = ctx.size;
    put_event(ctx, document);
    const usize count = ctx.blocks.count();
    for (usize index = 0; index < count; ++index) {
        if (ctx.stop != nullptr && (*ctx.stop).load(std::memory_order_relaxed)) return fail(ctx, ErrorKind::interrupted);
        const BlockRecord record = ctx.blocks[index];
        if (record.kind == RecordKind::leaf) {
            if (!write_leaf(ctx, record)) return false;
        } else {
            if (!write_container(ctx, record)) return false;
        }
    }
    if (!reserve_events(ctx, 1)) return false;
    Event end = block_event(EventKind::leave_block, BlockType::document);
    end.begin = ctx.size;
    end.end = ctx.size;
    put_event(ctx, end);
    return true;
}

} // namespace stxlib::markdown::detail
