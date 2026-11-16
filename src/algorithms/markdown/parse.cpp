// stxlib::markdown: the entry, the tape's storage and the document
// (markdown.hpp states the contract).

#include "algorithms/markdown/internal.hpp"

namespace stxlib::markdown {

namespace detail {

bool fail(Context& ctx, const ErrorKind kind) noexcept {
    if (!ctx.failed) {
        ctx.failed = true;
        ctx.failure = kind;
    }
    return false;
}

bool reserve_events(Context& ctx, const usize count) noexcept {
    if (ctx.event_room - ctx.event_count >= count) return true;
    const usize limit = ctx.limits.events;
    if (ctx.event_count > limit || count > limit - ctx.event_count) return fail(ctx, ErrorKind::limit_exceeded);
    const usize wanted = ctx.event_count + count;
    // Grow by doubling, within the limit, so that the tape is resized a
    // number of times logarithmic in its length.
    usize room = ctx.event_room < 256 ? 256 : ctx.event_room;
    while (room < wanted) room = room > limit / 2 ? limit : room * 2;
    if (room > limit) room = limit;
    Document& document = *ctx.document;
    if (document.events.resize(room).failed()) return fail(ctx, ErrorKind::out_of_memory);
    ctx.events = document.events.mutable_view().data();
    ctx.event_room = room;
    return true;
}

bool append_text(Context& ctx, const char* const bytes, const u32 count, TextRange& range) noexcept {
    Document& document = *ctx.document;
    const usize used = document.text.count();
    if (used > ctx.limits.text_bytes || count > ctx.limits.text_bytes - used) return fail(ctx, ErrorKind::limit_exceeded);
    // The document's text is addressed with 32 bits.
    if (used + count > largest_source_bytes) return fail(ctx, ErrorKind::limit_exceeded);
    if (count != 0) {
        const View<char> piece(bytes, count);
        if (document.text.append_copies(piece).failed()) return fail(ctx, ErrorKind::out_of_memory);
    }
    range = TextRange{.begin = static_cast<u32>(used), .end = static_cast<u32>(used + count)};
    return true;
}

} // namespace detail

View<char> Document::attribute(const u32 index) const& noexcept {
    STXLIB_REQUIRE(index < attributes.count(), "markdown::Document::attribute: no such attribute");
    const TextRange range = attributes[index];
    const View<char> result = text.view().from(range.begin).first(range.end - range.begin);
    return result;
}

View<char> Document::reference_text(const Event& event) const& noexcept {
    STXLIB_REQUIRE(event.kind == EventKind::text && event.type == static_cast<u8>(TextType::reference),
                   "markdown::Document::reference_text: not a reference event");
    const View<char> result = text.view().from(event.detail).first(event.data);
    return result;
}

void Document::clear() noexcept {
    events.clear();
    attributes.clear();
    text.clear();
}

OperationStatus parse(const View<char> source, const Options& options, const Limits& limits, Document& document,
                      const StopSignal* const stop) noexcept {
    const Operation operation("stxlib.markdown.parse");
    document.clear();
    if (source.count() > limits.source_bytes || source.count() > largest_source_bytes) {
        const OperationStatus too_large = OperationStatus::failure(ErrorRecord::make(ErrorKind::limit_exceeded,
                                                                                     operation));
        return too_large;
    }
    detail::Context ctx{};
    ctx.text = source.data();
    ctx.size = static_cast<u32>(source.count());
    ctx.options = options;
    ctx.limits = limits;
    ctx.document = &document;
    ctx.stop = stop;
    const bool read = detail::read_lines(ctx) && detail::write_blocks(ctx);
    if (!read) {
        document.clear();
        const OperationStatus failed = OperationStatus::failure(ErrorRecord::make(ctx.failure, operation));
        return failed;
    }
    document.events.truncate(ctx.event_count);
    const OperationStatus done = OperationStatus::success();
    return done;
}

} // namespace stxlib::markdown
