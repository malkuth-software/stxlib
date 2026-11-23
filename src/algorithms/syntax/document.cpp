// stxlib::syntax::Document (include/stxlib/syntax.hpp): line starts and entry states under
// edits.
//
// Line starts are prefix sums of line lengths (terminators included), kept in STXlib's
// order-statistic tree through RowHeightIndex: a row's "height" is a line's byte length, so
// the line at an offset and a line's start are each one descent, and an edit changes the
// lengths of the lines it touches and nothing after them (about 4 bytes a line).
//
// Entry states are one LineState a line in a vector; splicing lines moves the states after
// them (8 bytes a line, a memmove). A second tree would remove that; it has not been needed
// at the sizes measured so far, and this note is the record that it is linear.
//
// An edit re-lexes from the line holding it (or the line before, when it begins a line:
// a deletion there can join two lines) until a new line that lies wholly after the edit
// starts where an old line started and is entered in that old line's state. From there on
// the old states are still right.

#include "stxlib/syntax.hpp"

#include "stxlib/memory.hpp"
#include "stxlib/row_height_index.hpp"
#include "stxlib/vector.hpp"

#include <new>

namespace stxlib::syntax {

namespace detail {

struct DocumentState final {
    explicit DocumentState(RowHeightIndex&& index) noexcept : lines(static_cast<RowHeightIndex&&>(index)) {}

    Language language{Language::plain};
    bool ready{false};
    usize text_size{0};
    // Row r's height is line r's byte length, its terminator included.
    RowHeightIndex lines;
    Vector<LineState> states;
    // Reused by apply_edit: the replacement lines' lengths and entry states.
    Vector<i64> new_lengths;
    Vector<LineState> new_states;
    DocumentCounters counters;
};

namespace {

struct LineBreak final {
    usize content_end{0};
    usize next_start{0};
    bool terminated{false};
};

// Where the line starting at `start` ends: at LF, CR or CRLF, or the text's end.
[[nodiscard]] LineBreak find_line_break(const View<char> text, const usize start) noexcept {
    const char* const bytes = text.data();
    const usize count = text.count();
    usize index = start;
    while (index < count && bytes[index] != '\n' && bytes[index] != '\r') ++index;
    LineBreak result;
    result.content_end = index;
    if (index == count) {
        result.next_start = count;
        return result;
    }
    const bool crlf = bytes[index] == '\r' && index + 1 < count && bytes[index + 1] == '\n';
    result.next_start = index + (crlf ? 2 : 1);
    result.terminated = true;
    return result;
}

[[nodiscard]] OperationStatus exhausted(const Operation operation) noexcept {
    return OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
}

constexpr usize batch_lines = 1024;

// Appends `count` rows with the given lengths at the end of the index.
[[nodiscard]] OperationStatus append_rows(RowHeightIndex& lines, const i64* const lengths, const usize count) noexcept {
    if (count == 0) return OperationStatus::success();
    const u64 row = lines.count();
    const OperationStatus inserted = lines.insert(row, count);
    if (inserted.failed()) return inserted;
    return lines.set_heights(row, View<i64>(lengths, count));
}

[[nodiscard]] OperationStatus lex_all(DocumentState& document, const View<char> text) noexcept {
    document.ready = false;
    document.states.clear();
    if (document.lines.count() != 0) {
        const OperationStatus erased = document.lines.erase(0, document.lines.count());
        if (erased.failed()) return exhausted("stxlib.syntax.document.reset");
    }
    i64 lengths[batch_lines];
    usize pending = 0;
    LineState state;
    usize start = 0;
    u64 lexed = 0;
    for (;;) {
        const LineBreak line = find_line_break(text, start);
        if (document.states.push_copy(state).failed()) return exhausted("stxlib.syntax.document.reset");
        state = line_exit_state(document.language, View<char>(text.data() + start, line.content_end - start), state);
        lengths[pending] = static_cast<i64>(line.next_start - start);
        pending = pending + 1;
        lexed = lexed + 1;
        if (pending == batch_lines || !line.terminated) {
            if (append_rows(document.lines, lengths, pending).failed()) return exhausted("stxlib.syntax.document.reset");
            pending = 0;
        }
        if (!line.terminated) break;
        start = line.next_start;
    }
    document.text_size = text.count();
    document.counters.resets = document.counters.resets + 1;
    document.counters.lines_lexed = document.counters.lines_lexed + lexed;
    document.counters.last_update_lines = lexed;
    document.ready = true;
    return OperationStatus::success();
}

} // namespace

} // namespace detail

using detail::DocumentState;

Document::~Document() { release(); }

Document::Document(Document&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) {
        release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void Document::release() noexcept {
    // The owner lets go before it releases.
    DocumentState* const state = state_;
    state_ = nullptr;
    if (state == nullptr) return;
    (*state).~DocumentState();
    memory::release(state);
}

OperationResult<Document> Document::create(const Language language) noexcept {
    const ErrorRecord refused = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.syntax.document.create");
    OperationResult<RowHeightIndex> index = RowHeightIndex::create(64);
    if (index.failed()) return OperationResult<Document>::failure(refused);
    void* const block = memory::allocate(sizeof(DocumentState), alignof(DocumentState));
    if (block == nullptr) return OperationResult<Document>::failure(refused);
    DocumentState* const state = ::new (block) DocumentState(static_cast<OperationResult<RowHeightIndex>&&>(index).take_value());
    Document document(state);
    (*state).language = static_cast<usize>(language) < language_count ? language : Language::plain;
    if (detail::lex_all(*state, View<char>()).failed()) return OperationResult<Document>::failure(refused);
    (*state).counters = DocumentCounters{};
    return OperationResult<Document>::success(static_cast<Document&&>(document));
}

OperationStatus Document::reset(const View<char> text) noexcept {
    if (state_ == nullptr) {
        return OperationStatus::failure(
            ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.syntax.document.reset", ErrorDetail{.message = "empty document"}));
    }
    if (text.count() > text_byte_limit) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.syntax.document.reset",
                                                          ErrorDetail{.message = "the text is past text_byte_limit"}));
    }
    return detail::lex_all(*state_, text);
}

OperationStatus Document::set_language(const Language language, const View<char> text) noexcept {
    if (state_ == nullptr || static_cast<usize>(language) >= language_count) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.syntax.document.set_language",
                                                          ErrorDetail{.message = "empty document or unknown language"}));
    }
    if (text.count() > text_byte_limit) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.syntax.document.set_language",
                                                          ErrorDetail{.message = "the text is past text_byte_limit"}));
    }
    (*state_).language = language;
    return detail::lex_all(*state_, text);
}

OperationResult<Damage> Document::apply_edit(const View<char> text, const TextEdit& edit) noexcept {
    if (state_ == nullptr || !(*state_).ready) {
        return OperationResult<Damage>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.syntax.document.apply_edit",
                                                                  ErrorDetail{.message = "the document has no content; reset it"}));
    }
    DocumentState& document = *state_;
    const usize old_size = document.text_size;
    const bool removal_fits = edit.start <= old_size && edit.removed <= old_size - edit.start;
    const bool sizes_agree = removal_fits && text.count() == old_size - edit.removed + edit.inserted;
    const bool insertion_fits = sizes_agree && edit.inserted <= text.count() - edit.start;
    if (!insertion_fits) {
        return OperationResult<Damage>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.syntax.document.apply_edit",
                                                                  ErrorDetail{.message = "the edit does not describe the change of the text"}));
    }
    if (text.count() > text_byte_limit) {
        return OperationResult<Damage>::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.syntax.document.apply_edit",
                                                                  ErrorDetail{.message = "the text is past text_byte_limit"}));
    }
    const ErrorRecord refused = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.syntax.document.apply_edit");
    const u64 old_count = document.lines.count();
    u64 first = document.lines.row_at(static_cast<i64>(edit.start));
    if (first > 0 && static_cast<usize>(document.lines.offset_of(first)) == edit.start) first = first - 1;
    const usize old_edit_end = edit.start + edit.removed;
    const usize new_edit_end = edit.start + edit.inserted;
    document.new_lengths.clear();
    document.new_states.clear();
    LineState state = document.states[static_cast<usize>(first)];
    usize start = static_cast<usize>(document.lines.offset_of(first));
    u64 converged = old_count;
    u64 lexed = 0;
    for (;;) {
        if (start >= new_edit_end) {
            const usize old_start = start - new_edit_end + old_edit_end;
            const u64 row = document.lines.row_at(static_cast<i64>(old_start));
            const bool matches = row >= first && static_cast<usize>(document.lines.offset_of(row)) == old_start &&
                                 document.states[static_cast<usize>(row)] == state;
            if (matches) {
                converged = row;
                break;
            }
        }
        const detail::LineBreak line = detail::find_line_break(text, start);
        if (document.new_states.push_copy(state).failed()) return OperationResult<Damage>::failure(refused);
        if (document.new_lengths.push_copy(static_cast<i64>(line.next_start - start)).failed()) return OperationResult<Damage>::failure(refused);
        state = line_exit_state(document.language, View<char>(text.data() + start, line.content_end - start), state);
        lexed = lexed + 1;
        if (!line.terminated) break;
        start = line.next_start;
    }
    // Old lines [first, converged) become the new lines. Everything that can be refused
    // without a change is reserved first.
    const usize added = document.new_lengths.count();
    const usize replaced = static_cast<usize>(converged - first);
    const usize old_lines = static_cast<usize>(old_count);
    const usize new_lines = old_lines - replaced + added;
    if (document.states.reserve(new_lines).failed()) return OperationResult<Damage>::failure(refused);
    OperationStatus index = OperationStatus::success();
    if (added > replaced) index = document.lines.insert(first, added - replaced);
    if (added < replaced) index = document.lines.erase(first, replaced - added);
    if (index.succeeded() && added != 0) index = document.lines.set_heights(first, document.new_lengths.view());
    if (index.failed()) {
        document.ready = false;
        return OperationResult<Damage>::failure(refused);
    }
    const usize head = static_cast<usize>(first);
    const usize tail = old_lines - head - replaced;
    if (added > replaced) {
        if (document.states.resize(new_lines).failed()) {
            document.ready = false;
            return OperationResult<Damage>::failure(refused);
        }
        for (usize k = tail; k > 0; --k) document.states[head + added + k - 1] = document.states[head + replaced + k - 1];
    } else if (added < replaced) {
        for (usize k = 0; k < tail; ++k) document.states[head + added + k] = document.states[head + replaced + k];
        document.states.truncate(new_lines);
    }
    for (usize k = 0; k < added; ++k) document.states[head + k] = document.new_states[k];
    document.text_size = text.count();
    document.counters.edits = document.counters.edits + 1;
    document.counters.lines_lexed = document.counters.lines_lexed + lexed;
    document.counters.last_update_lines = lexed;
    Damage damage;
    damage.first_line = first;
    damage.line_count = added;
    return OperationResult<Damage>::success(damage);
}

bool Document::ready() const noexcept { return state_ != nullptr && (*state_).ready; }

Language Document::language() const noexcept { return state_ == nullptr ? Language::plain : (*state_).language; }

usize Document::text_size() const noexcept { return ready() ? (*state_).text_size : 0; }

u64 Document::line_count() const noexcept { return ready() ? (*state_).lines.count() : 0; }

u64 Document::line_at(const usize offset) const noexcept {
    if (!ready()) return 0;
    const usize clamped = offset < (*state_).text_size ? offset : (*state_).text_size;
    return (*state_).lines.row_at(static_cast<i64>(clamped));
}

usize Document::line_start(const u64 line) const noexcept {
    if (!ready()) return 0;
    return static_cast<usize>((*state_).lines.offset_of(line));
}

LineExtent Document::line_extent(const View<char> text, const u64 line) const noexcept {
    LineExtent extent;
    if (!ready() || line >= (*state_).lines.count() || text.count() != (*state_).text_size) return extent;
    const RowExtent row = (*state_).lines.extent_of(line);
    extent.start = static_cast<usize>(row.top);
    extent.length = static_cast<usize>(row.height);
    const char* const bytes = text.data() + extent.start;
    if (extent.length >= 1 && bytes[extent.length - 1] == '\n') {
        extent.terminator = extent.length >= 2 && bytes[extent.length - 2] == '\r' ? 2 : 1;
    } else if (extent.length >= 1 && bytes[extent.length - 1] == '\r') {
        extent.terminator = 1;
    }
    return extent;
}

LineState Document::line_entry_state(const u64 line) const noexcept {
    if (!ready() || line >= (*state_).states.count()) return LineState{};
    return (*state_).states[static_cast<usize>(line)];
}

LineCursor Document::line_cursor(const u64 line) const noexcept { return LineCursor::at_line_start(line_entry_state(line)); }

DocumentCounters Document::counters() const noexcept { return state_ == nullptr ? DocumentCounters{} : (*state_).counters; }

usize Document::storage_bytes() const noexcept {
    if (state_ == nullptr) return 0;
    const DocumentState& document = *state_;
    const usize total = sizeof(DocumentState) + document.lines.storage_bytes() + document.states.capacity() * sizeof(LineState) +
                        document.new_lengths.capacity() * sizeof(i64) + document.new_states.capacity() * sizeof(LineState);
    return total;
}

} // namespace stxlib::syntax
