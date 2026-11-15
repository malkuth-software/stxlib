// stxlib::markdown: link labels, destinations and titles; link reference
// definitions and their lookup; character references; and the decoding of
// an attribute (internal.hpp). After MD4C 0.6.0's md_is_link_label,
// md_is_link_destination, md_is_link_title, md_is_link_reference_definition,
// md_is_inline_link_spec and md_build_attribute.
//
// Reformulated:
// - A label is matched in one fixed form, made once: case folded (full
//   folding), whitespace runs as one space, no leading or trailing space.
//   A definition's form is kept; a reference's is made in a buffer on the
//   stack, which a label's 999-character limit bounds. Matching is then a
//   comparison of bytes. (MD4C folds both labels again at every
//   comparison.)
// - Lookup is a binary search of the definitions sorted by (hash, form,
//   order of definition), built by a heap sort: O(log n) whatever the
//   hashes are, and the first definition of a label is the one found.
//   (MD4C: a hash table whose crowded buckets are sorted.)
// - A definition's destination and title are decoded at its first use and
//   every link that names it shares that text, so many references to one
//   large definition cost nothing more. (MD4C copies per link and caps the
//   total with a budget.)

#include "algorithms/markdown/internal.hpp"

#include "algorithms/markdown/entities.hpp"

namespace stxlib::markdown::detail {

namespace {

// A label is at most this many characters (CommonMark).
inline constexpr u32 label_limit = 999;
// One character folds to at most three code points of at most four bytes.
inline constexpr u32 label_form_bytes = label_limit * 12;

inline constexpr u32 fnv_basis = 2166136261U;
inline constexpr u32 fnv_prime = 16777619U;

[[nodiscard]] u32 hash_bytes(const char* const bytes, const u32 count) noexcept {
    u32 hash = fnv_basis;
    for (u32 index = 0; index < count; ++index) {
        hash = hash ^ static_cast<u8>(bytes[index]);
        // The multiplication is modulo 2^32 by definition (FNV-1a).
        hash = static_cast<u32>(static_cast<u64>(hash) * fnv_prime);
    }
    return hash;
}

// Whether the label at lines' text[begin], a '[', ends within the block's
// lines. On true `end` is just after its ']', `end_line` the line that is
// on, and [contents_begin, contents_end) its contents without the
// whitespace around them, starting on `begin_line`.
struct LabelPlace final {
    u32 end{0};
    u32 begin_line{0};
    u32 end_line{0};
    u32 contents_begin{0};
    u32 contents_end{0};
};

[[nodiscard]] bool link_label(const Context& ctx, const u32 first_line, const u32 line_count, const u32 begin,
                              LabelPlace& place) noexcept {
    if (ctx.text[begin] != '[') return false;
    u32 offset = begin + 1;
    u32 contents_begin = 0;
    u32 contents_end = 0;
    u32 begin_line = first_line;
    u32 length = 0;
    for (u32 line = first_line; line < first_line + line_count; ++line) {
        if (line != first_line) offset = ctx.lines[line].begin;
        const u32 line_end = ctx.lines[line].end;
        while (offset < line_end) {
            const char ch = ctx.text[offset];
            if (ch == '\\' && offset + 1 < ctx.size &&
                (is_ascii_punctuation(ctx.text[offset + 1]) || is_newline(ctx.text[offset + 1]))) {
                if (contents_end == 0) {
                    contents_begin = offset;
                    begin_line = line;
                }
                contents_end = offset + 2;
                offset = offset + 2;
            } else if (ch == '[') {
                return false;
            } else if (ch == ']') {
                // A label has something in it that is not whitespace.
                if (contents_begin >= contents_end) return false;
                place = LabelPlace{.end = offset + 1,
                                   .begin_line = begin_line,
                                   .end_line = line,
                                   .contents_begin = contents_begin,
                                   .contents_end = contents_end};
                return true;
            } else {
                const Decoded decoded = decode_at(ctx.text, ctx.size, offset);
                if (!is_unicode_whitespace(decoded.code_point)) {
                    if (contents_end == 0) {
                        contents_begin = offset;
                        begin_line = line;
                    }
                    contents_end = offset + decoded.length;
                }
                offset = offset + decoded.length;
            }
            length = length + 1;
            if (length > label_limit) return false;
        }
        // The line ending counts as a character.
        length = length + 1;
    }
    return false;
}

// Writes the matching form of the label [begin, end), which starts on
// `line` and may run over the lines after it (up to `line_limit`), to
// out[0, label_form_bytes). Returns its length; 0 for a label of
// whitespace only, or one longer than a label may be.
[[nodiscard]] u32 label_form(const Context& ctx, const u32 line, const u32 line_limit, const u32 begin, const u32 end,
                             char* const out) noexcept {
    u32 written = 0;
    u32 characters = 0;
    bool space_pending = false;
    u32 offset = begin;
    for (u32 index = line; index < line_limit; ++index) {
        if (index != line) {
            // A line ending is whitespace, and one character of the limit.
            offset = ctx.lines[index].begin;
            space_pending = true;
            characters = characters + 1;
        }
        const u32 line_end = ctx.lines[index].end < end ? ctx.lines[index].end : end;
        while (offset < line_end) {
            const Decoded decoded = decode_at(ctx.text, line_end, offset);
            characters = characters + 1;
            if (characters > label_limit) return 0;
            if (is_unicode_whitespace(decoded.code_point)) {
                space_pending = true;
            } else {
                if (space_pending && written != 0) {
                    out[written] = ' ';
                    written = written + 1;
                }
                space_pending = false;
                if (decoded.code_point == not_a_character) {
                    out[written] = ctx.text[offset];
                    written = written + 1;
                } else {
                    u32 folded[3] = {0, 0, 0};
                    const u32 count = fold_case_full(decoded.code_point, folded);
                    for (u32 piece = 0; piece < count; ++piece) {
                        written = written + encode_utf8(folded[piece], out + written);
                    }
                }
            }
            offset = offset + decoded.length;
        }
        if (ctx.lines[index].end >= end) break;
    }
    return written;
}

// A destination in angle brackets, at text[begin] == '<'.
[[nodiscard]] bool pointy_destination(const Context& ctx, const u32 begin, const u32 limit, u32& end, u32& contents_begin,
                                      u32& contents_end) noexcept {
    u32 offset = begin + 1;
    while (offset < limit) {
        const char ch = ctx.text[offset];
        if (ch == '\\' && offset + 1 < limit && is_ascii_punctuation(ctx.text[offset + 1])) {
            offset = offset + 2;
            continue;
        }
        if (is_newline(ch) || ch == '<') return false;
        if (ch == '>') {
            contents_begin = begin + 1;
            contents_end = offset;
            end = offset + 1;
            return true;
        }
        offset = offset + 1;
    }
    return false;
}

// A bare destination: no whitespace or control, parentheses balanced, at
// most 32 deep.
[[nodiscard]] bool bare_destination(const Context& ctx, const u32 begin, const u32 limit, u32& end, u32& contents_begin,
                                    u32& contents_end) noexcept {
    u32 offset = begin;
    u32 depth = 0;
    while (offset < limit) {
        const char ch = ctx.text[offset];
        if (ch == '\\' && offset + 1 < limit && is_ascii_punctuation(ctx.text[offset + 1])) {
            offset = offset + 2;
            continue;
        }
        if (is_whitespace(ch) || is_control(ch)) break;
        if (ch == '(') {
            depth = depth + 1;
            if (depth > 32) return false;
        } else if (ch == ')') {
            if (depth == 0) break;
            depth = depth - 1;
        }
        offset = offset + 1;
    }
    if (depth != 0 || offset == begin) return false;
    contents_begin = begin;
    contents_end = offset;
    end = offset;
    return true;
}

[[nodiscard]] bool link_destination(const Context& ctx, const u32 begin, const u32 limit, u32& end, u32& contents_begin,
                                    u32& contents_end) noexcept {
    if (begin < limit && ctx.text[begin] == '<') {
        const bool pointy = pointy_destination(ctx, begin, limit, end, contents_begin, contents_end);
        return pointy;
    }
    const bool bare = bare_destination(ctx, begin, limit, end, contents_begin, contents_end);
    return bare;
}

struct TitlePlace final {
    u32 end{0};
    u32 begin_line{0};
    u32 end_line{0};
    u32 contents_begin{0};
    u32 contents_end{0};
};

// A title after whitespace with at most one line ending, from text[begin]
// on `line`, within lines up to `line_limit`.
[[nodiscard]] bool link_title(const Context& ctx, const u32 line, const u32 line_limit, const u32 begin,
                              TitlePlace& place) noexcept {
    u32 offset = begin;
    u32 index = line;
    while (offset < ctx.lines[index].end && is_whitespace(ctx.text[offset])) offset = offset + 1;
    if (offset >= ctx.lines[index].end) {
        index = index + 1;
        if (index >= line_limit) return false;
        offset = ctx.lines[index].begin;
    }
    // A title is separated from what is before it.
    if (offset == begin) return false;
    if (offset >= ctx.lines[index].end) return false;
    const char opener = ctx.text[offset];
    char closer = 0;
    if (opener == '"' || opener == '\'') {
        closer = opener;
    } else if (opener == '(') {
        closer = ')';
    } else {
        return false;
    }
    const u32 begin_line = index;
    offset = offset + 1;
    const u32 contents_begin = offset;
    while (index < line_limit) {
        const u32 line_end = ctx.lines[index].end;
        while (offset < line_end) {
            const char ch = ctx.text[offset];
            if (ch == '\\' && offset + 1 < ctx.size &&
                (is_ascii_punctuation(ctx.text[offset + 1]) || is_newline(ctx.text[offset + 1]))) {
                offset = offset + 1;
            } else if (ch == closer) {
                place = TitlePlace{.end = offset + 1,
                                   .begin_line = begin_line,
                                   .end_line = index,
                                   .contents_begin = contents_begin,
                                   .contents_end = offset};
                return true;
            } else if (closer == ')' && ch == '(') {
                return false;
            }
            offset = offset + 1;
        }
        index = index + 1;
        if (index < line_limit) offset = ctx.lines[index].begin;
    }
    return false;
}

// The order of definitions `left` and `right` for lookup.
[[nodiscard]] bool definition_before(const Context& ctx, const u32 left, const u32 right) noexcept {
    const Definition& a = ctx.definitions[left];
    const Definition& b = ctx.definitions[right];
    if (a.hash != b.hash) return a.hash < b.hash;
    const u32 a_length = a.label_end - a.label_begin;
    const u32 b_length = b.label_end - b.label_begin;
    if (a_length != b_length) return a_length < b_length;
    const char* const text = ctx.label_text.view().data();
    const i32 order = __builtin_memcmp(text + a.label_begin, text + b.label_begin, a_length);
    if (order != 0) return order < 0;
    return left < right;
}

// Restores the heap property below `root` in order[0, count).
void sift_down(const Context& ctx, u32* const order, const usize root, const usize count) noexcept {
    usize parent = root;
    for (;;) {
        const usize left = parent * 2 + 1;
        if (left >= count) return;
        usize largest = parent;
        if (definition_before(ctx, order[largest], order[left])) largest = left;
        const usize right = left + 1;
        if (right < count && definition_before(ctx, order[largest], order[right])) largest = right;
        if (largest == parent) return;
        const u32 moved = order[parent];
        order[parent] = order[largest];
        order[largest] = moved;
        parent = largest;
    }
}

// Appends decoded text of the source range [begin, end) to the document's
// text (see build_attribute).
[[nodiscard]] bool append_decoded(Context& ctx, const u32 begin, const u32 end, const bool escapes) noexcept {
    u32 offset = begin;
    u32 plain = begin;
    TextRange ignored{};
    while (offset < end) {
        const char ch = ctx.text[offset];
        if (ch != '\\' && ch != '&' && ch != '\0') {
            offset = offset + 1;
            continue;
        }
        char piece[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        u32 piece_length = 0;
        u32 after = offset;
        if (ch == '\0') {
            piece_length = encode_utf8(0xfffd, piece);
            after = offset + 1;
        } else if (ch == '&') {
            u32 reference_end = 0;
            if (reference_at(ctx.text, offset, end, reference_end)) {
                piece_length = decode_reference(ctx.text, offset, reference_end, piece);
                if (piece_length != 0) after = reference_end;
            }
        } else if (escapes && offset + 1 < end &&
                   (is_ascii_punctuation(ctx.text[offset + 1]) || is_newline(ctx.text[offset + 1]))) {
            // The backslash goes; what it escapes is copied with what follows.
            if (!append_text(ctx, ctx.text + plain, offset - plain, ignored)) return false;
            plain = offset + 1;
            offset = offset + 2;
            continue;
        }
        if (after == offset) {
            // Not markup after all: an ordinary byte.
            offset = offset + 1;
            continue;
        }
        if (!append_text(ctx, ctx.text + plain, offset - plain, ignored)) return false;
        if (!append_text(ctx, piece, piece_length, ignored)) return false;
        offset = after;
        plain = after;
    }
    const bool appended = append_text(ctx, ctx.text + plain, end - plain, ignored);
    return appended;
}

[[nodiscard]] bool push_attribute(Context& ctx, const u32 begin, u32& attribute) noexcept {
    Document& document = *ctx.document;
    const usize count = document.attributes.count();
    if (count >= largest_source_bytes) return fail(ctx, ErrorKind::limit_exceeded);
    const TextRange range{.begin = begin, .end = static_cast<u32>(document.text.count())};
    if (document.attributes.push_copy(range).failed()) return fail(ctx, ErrorKind::out_of_memory);
    attribute = static_cast<u32>(count);
    return true;
}

} // namespace

u32 line_at(const Context& ctx, const u32 first_line, const u32 line_count, const u32 offset) noexcept {
    // The first line whose end is at or after the offset.
    u32 low = first_line;
    u32 high = first_line + line_count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        if (ctx.lines[middle].end < offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const u32 last = first_line + line_count - 1;
    const u32 result = low > last ? last : low;
    return result;
}

bool reference_at(const char* const text, const u32 begin, const u32 limit, u32& end) noexcept {
    u32 offset = begin + 1;
    if (offset + 1 < limit && text[offset] == '#' && (text[offset + 1] == 'x' || text[offset + 1] == 'X')) {
        // One to six hexadecimal digits.
        const u32 digits = offset + 2;
        offset = digits;
        while (offset < limit && is_hex_digit(text[offset]) && offset - digits < 7) offset = offset + 1;
        if (offset == digits || offset - digits > 6) return false;
    } else if (offset < limit && text[offset] == '#') {
        // One to seven decimal digits.
        const u32 digits = offset + 1;
        offset = digits;
        while (offset < limit && is_digit(text[offset]) && offset - digits < 8) offset = offset + 1;
        if (offset == digits || offset - digits > 7) return false;
    } else {
        // A name: a letter, then letters and digits.
        const u32 name = offset;
        if (offset >= limit || !is_alpha(text[offset])) return false;
        while (offset < limit && is_alphanumeric(text[offset]) && offset - name <= longest_entity_name) {
            offset = offset + 1;
        }
        if (offset - name > longest_entity_name) return false;
    }
    if (offset >= limit || text[offset] != ';') return false;
    end = offset + 1;
    return true;
}

u32 decode_reference(const char* const text, const u32 begin, const u32 end, char* const out) noexcept {
    // text[begin] is '&' and text[end - 1] is ';' (reference_at).
    if (text[begin + 1] != '#') {
        u32 first = 0;
        u32 second = 0;
        if (!find_named_reference(text + begin + 1, end - begin - 2, first, second)) return 0;
        u32 length = encode_utf8(first, out);
        if (second != 0) length = length + encode_utf8(second, out + length);
        return length;
    }
    const bool hexadecimal = text[begin + 2] == 'x' || text[begin + 2] == 'X';
    u32 value = 0;
    for (u32 offset = begin + (hexadecimal ? 3 : 2); offset + 1 < end; ++offset) {
        const char ch = text[offset];
        u32 digit = 0;
        if (is_digit(ch)) {
            digit = static_cast<u32>(ch - '0');
        } else {
            digit = static_cast<u32>(ascii_lower(ch) - 'a') + 10;
        }
        // Six hexadecimal or seven decimal digits stay below 2^32.
        value = value * (hexadecimal ? 16 : 10) + digit;
    }
    // Not a scalar value, or NUL: the replacement character.
    if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
    const u32 length = encode_utf8(value, out);
    return length;
}

bool build_attribute(Context& ctx, const u32 begin, const u32 end, const bool escapes, u32& attribute) noexcept {
    const u32 text_begin = static_cast<u32>((*ctx.document).text.count());
    if (!append_decoded(ctx, begin, end, escapes)) return false;
    const bool pushed = push_attribute(ctx, text_begin, attribute);
    return pushed;
}

bool target_attributes(Context& ctx, const LinkTarget& target, u32& attribute) noexcept {
    if (!build_attribute(ctx, target.destination_begin, target.destination_end, true, attribute)) return false;
    const u32 text_begin = static_cast<u32>((*ctx.document).text.count());
    if (target.title_line == none) {
        if (!append_decoded(ctx, target.title_begin, target.title_end, true)) return false;
    } else {
        // A title over several lines: each line's part, joined by line feeds.
        u32 line = target.title_line;
        u32 offset = target.title_begin;
        for (;;) {
            const u32 line_end = ctx.lines[line].end < target.title_end ? ctx.lines[line].end : target.title_end;
            if (!append_decoded(ctx, offset, line_end, true)) return false;
            if (ctx.lines[line].end >= target.title_end) break;
            TextRange ignored{};
            if (!append_text(ctx, "\n", 1, ignored)) return false;
            line = line + 1;
            offset = ctx.lines[line].begin;
        }
    }
    u32 title = 0;
    const bool pushed = push_attribute(ctx, text_begin, title);
    return pushed;
}

bool definition_attributes(Context& ctx, const u32 definition, u32& attribute) noexcept {
    if (ctx.definitions[definition].attribute == none) {
        u32 made = 0;
        if (!target_attributes(ctx, ctx.definitions[definition].target, made)) return false;
        ctx.definitions[definition].attribute = made;
    }
    attribute = ctx.definitions[definition].attribute;
    return true;
}

i32 take_definition(Context& ctx, const u32 first_line, const u32 line_count) noexcept {
    const u32 line_limit = first_line + line_count;
    LabelPlace label{};
    if (!link_label(ctx, first_line, line_count, ctx.lines[first_line].begin, label)) return 0;
    u32 line = label.end_line;
    u32 offset = label.end;
    if (offset >= ctx.lines[line].end || ctx.text[offset] != ':') return 0;
    offset = offset + 1;
    // Whitespace with at most one line ending.
    while (offset < ctx.lines[line].end && is_whitespace(ctx.text[offset])) offset = offset + 1;
    if (offset >= ctx.lines[line].end) {
        line = line + 1;
        if (line >= line_limit) return 0;
        offset = ctx.lines[line].begin;
    }
    LinkTarget target{};
    if (!link_destination(ctx, offset, ctx.lines[line].end, offset, target.destination_begin, target.destination_end)) {
        return 0;
    }
    // A title counts only if nothing follows it on its last line.
    TitlePlace title{};
    if (link_title(ctx, line, line_limit, offset, title) && title.end >= ctx.lines[title.end_line].end) {
        target.title_begin = title.contents_begin;
        target.title_end = title.contents_end;
        if (title.end_line != title.begin_line) target.title_line = title.begin_line;
        line = title.end_line;
        offset = title.end;
    } else {
        target.title_begin = offset;
        target.title_end = offset;
    }
    if (offset < ctx.lines[line].end) return 0;

    char form[label_form_bytes];
    const u32 form_length = label_form(ctx, label.begin_line, line_limit, label.contents_begin, label.contents_end, form);
    if (form_length == 0) return 0;
    if (ctx.definitions.count() >= ctx.limits.definitions) {
        static_cast<void>(fail(ctx, ErrorKind::limit_exceeded));
        return -1;
    }
    const usize label_begin = ctx.label_text.count();
    if (label_begin + form_length > largest_source_bytes) {
        static_cast<void>(fail(ctx, ErrorKind::limit_exceeded));
        return -1;
    }
    // A title that runs over lines is decoded now: the paragraph's lines
    // are given up once its definitions are taken, so the line the title
    // begins on cannot be named later. (One on a single line is a range of
    // the source and is decoded at the definition's first use.)
    u32 attribute = none;
    if (target.title_line != none) {
        if (!target_attributes(ctx, target, attribute)) return -1;
    }
    const View<char> form_view(form, form_length);
    const Definition definition{.label_begin = static_cast<u32>(label_begin),
                                .label_end = static_cast<u32>(label_begin + form_length),
                                .hash = hash_bytes(form, form_length),
                                .target = target,
                                .attribute = attribute};
    if (ctx.label_text.append_copies(form_view).failed() || ctx.definitions.push_copy(definition).failed()) {
        static_cast<void>(fail(ctx, ErrorKind::out_of_memory));
        return -1;
    }
    const i32 taken = static_cast<i32>(line - first_line) + 1;
    return taken;
}

bool index_definitions(Context& ctx) noexcept {
    const usize count = ctx.definitions.count();
    if (count == 0) return true;
    if (ctx.definition_order.resize(count).failed()) return fail(ctx, ErrorKind::out_of_memory);
    u32* const order = ctx.definition_order.mutable_view().data();
    for (usize index = 0; index < count; ++index) order[index] = static_cast<u32>(index);
    // Heap sort: in place, no recursion, n log n whatever the input.
    for (usize root = count / 2; root > 0; --root) sift_down(ctx, order, root - 1, count);
    for (usize last = count - 1; last > 0; --last) {
        const u32 moved = order[0];
        order[0] = order[last];
        order[last] = moved;
        sift_down(ctx, order, 0, last);
    }
    return true;
}

u32 find_definition(Context& ctx, const u32 first_line, const u32 line_count, const u32 begin, const u32 end) noexcept {
    const usize count = ctx.definitions.count();
    if (count == 0 || begin >= end) return none;
    char form[label_form_bytes];
    const u32 line = line_at(ctx, first_line, line_count, begin);
    const u32 length = label_form(ctx, line, first_line + line_count, begin, end, form);
    if (length == 0) return none;
    const u32 hash = hash_bytes(form, length);
    const char* const text = ctx.label_text.view().data();
    const u32* const order = ctx.definition_order.view().data();
    // The first definition, in the sorted order, that is not before the key.
    usize low = 0;
    usize high = count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const Definition& candidate = ctx.definitions[order[middle]];
        const u32 candidate_length = candidate.label_end - candidate.label_begin;
        bool candidate_first = false;
        if (candidate.hash != hash) {
            candidate_first = candidate.hash < hash;
        } else if (candidate_length != length) {
            candidate_first = candidate_length < length;
        } else {
            candidate_first = __builtin_memcmp(text + candidate.label_begin, form, length) < 0;
        }
        if (candidate_first) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= count) return none;
    const u32 found = order[low];
    const Definition& candidate = ctx.definitions[found];
    if (candidate.hash != hash || candidate.label_end - candidate.label_begin != length) return none;
    if (__builtin_memcmp(text + candidate.label_begin, form, length) != 0) return none;
    return found;
}

bool inline_link_target(const Context& ctx, const u32 first_line, const u32 line_count, const u32 begin, u32& end,
                        LinkTarget& target) noexcept {
    const u32 line_limit = first_line + line_count;
    u32 line = line_at(ctx, first_line, line_count, begin);
    u32 offset = begin + 1;
    while (offset < ctx.lines[line].end && is_whitespace(ctx.text[offset])) offset = offset + 1;
    if (offset >= ctx.lines[line].end) {
        line = line + 1;
        if (line >= line_limit) return false;
        offset = ctx.lines[line].begin;
    }
    // "()" : no destination and no title.
    if (offset < ctx.lines[line].end && ctx.text[offset] == ')') {
        target = LinkTarget{.destination_begin = offset,
                            .destination_end = offset,
                            .title_begin = offset,
                            .title_end = offset,
                            .title_line = none};
        end = offset + 1;
        return true;
    }
    LinkTarget found{};
    if (!link_destination(ctx, offset, ctx.lines[line].end, offset, found.destination_begin, found.destination_end)) {
        return false;
    }
    TitlePlace title{};
    if (link_title(ctx, line, line_limit, offset, title)) {
        found.title_begin = title.contents_begin;
        found.title_end = title.contents_end;
        if (title.end_line != title.begin_line) found.title_line = title.begin_line;
        line = title.end_line;
        offset = title.end;
    } else {
        found.title_begin = offset;
        found.title_end = offset;
    }
    while (offset < ctx.lines[line].end && is_whitespace(ctx.text[offset])) offset = offset + 1;
    if (offset >= ctx.lines[line].end) {
        line = line + 1;
        if (line >= line_limit) return false;
        offset = ctx.lines[line].begin;
    }
    if (offset >= ctx.lines[line].end || ctx.text[offset] != ')') return false;
    target = found;
    end = offset + 1;
    return true;
}

} // namespace stxlib::markdown::detail
