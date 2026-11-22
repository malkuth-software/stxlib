// stx_mcp's server: lifecycle, tasks, bindings and the request and reply
// classes (include/stxlib/mcp.hpp). The protocol itself is protocol.cpp;
// the shared state and the task diagram are server_state.hpp.

#include "mcp/server_state.hpp"

#include "core/host_failure.hpp"
#include "stxlib/detail/construct.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"

namespace stxlib::mcp {

namespace {

constexpr Operation create_operation = "stxlib.mcp.server.create";
constexpr Operation bind_operation = "stxlib.mcp.server.bind";
constexpr Operation unbind_operation = "stxlib.mcp.server.unbind";
constexpr Operation start_operation = "stxlib.mcp.server.start";

constexpr TextView legacy_versions[legacy_protocol_version_count] = {"2025-11-25", "2025-06-18", "2025-03-26"};

constexpr TextView default_input_schema = R"({"type":"object","additionalProperties":false})";

constexpr usize reader_chunk_bytes = 8 * 1024;

// The longest prefix of `text` of at most `capacity` bytes that ends at a
// whole UTF-8 sequence.
usize cut_at_sequence(const TextView text, const usize capacity) noexcept {
    if (text.count() <= capacity) return text.count();
    usize cut = capacity;
    while (cut > 0 && (static_cast<unsigned char>(text.data()[cut]) & 0xC0) == 0x80) cut = cut - 1;
    return cut;
}

u8 copy_cut(char* const target, const TextView text) noexcept {
    const usize count = cut_at_sequence(text, client_text_capacity);
    for (usize index = 0; index < count; ++index) target[index] = text.data()[index];
    return static_cast<u8>(count);
}

TextView stored_text(const char* const bytes, const u8 count) noexcept {
    const TextView result = ::stxlib::detail::TextViewAccess::adopt_validated(bytes, count);
    return result;
}

bool same_text(const char* const left, const u8 left_count, const char* const right, const u8 right_count) noexcept {
    if (left_count != right_count) return false;
    for (u8 index = 0; index < left_count; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

ErrorRecord invalid(const Operation operation, const TextView message) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::invalid_argument, operation, ErrorDetail{.message = message});
    return result;
}

// Copies a spec's text into a binding's string; the first failure is kept.
void copy_into(Utf8String& target, const TextView text, OperationStatus& status) noexcept {
    if (status.failed()) return;
    status = target.assign(text);
}

// MCP's rule for tool names: 1 to 128 of A-Z, a-z, 0-9, '_', '-', '.'.
bool valid_tool_name(const TextView name) noexcept {
    if (name.empty() || name.count() > 128) return false;
    for (usize index = 0; index < name.count(); ++index) {
        const char byte = name.data()[index];
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '_' && byte != '-' && byte != '.') return false;
    }
    return true;
}

// ---- the reader task ---------------------------------------------------------------

// The reader's start block: hands the thread its own reference to the link.
struct ReaderStart final {
    Shared<detail::ReaderLink> link{};
};

void finish_reader(detail::ReaderLink& link, const detail::InputEnd end) noexcept {
    __atomic_store_n(&link.end, static_cast<u8>(end), __ATOMIC_RELEASE);
    link.inbox.close();
}

// Sends a framed line; false when the inbox is closed (serving ended).
bool deliver(detail::ReaderLink& link, detail::InboxRecord& record) noexcept {
    const ChannelStatus status = link.inbox.send(record);
    record.bytes.clear();
    record.oversized = false;
    const bool result = status == ChannelStatus::delivered;
    return result;
}

// Ends the current line: drops a trailing carriage return, skips an empty
// line, delivers the rest.
bool end_line(detail::ReaderLink& link, detail::InboxRecord& record) noexcept {
    if (!record.oversized && record.bytes.count() > 0 && record.bytes[record.bytes.count() - 1] == '\r') {
        record.bytes.remove_last();
    }
    if (!record.oversized && record.bytes.empty()) return true;
    const bool result = deliver(link, record);
    return result;
}

void reader_entry(const StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    ReaderStart* const start = static_cast<ReaderStart*>(context);
    Shared<detail::ReaderLink> held = static_cast<Shared<detail::ReaderLink>&&>((*start).link);
    ::stxlib::detail::destroy(start);
    memory::release(start);
    detail::ReaderLink& link = *held;
    Byte chunk[reader_chunk_bytes];
    detail::InboxRecord record;
    detail::InputEnd end = detail::InputEnd::ended;
    bool open = true;
    while (open) {
        OperationResult<PipeRead> got = link.input.read(MutableView<Byte>(chunk));
        if (got.failed()) {
            end = detail::InputEnd::failed;
            break;
        }
        const PipeRead& read = *got.value();
        if (read.is_end()) {
            // A last line without its line break is still a message.
            if (record.oversized || !record.bytes.empty()) static_cast<void>(end_line(link, record));
            break;
        }
        const usize count = *read.byte_count();
        usize run = 0;
        for (usize index = 0; index <= count && open; ++index) {
            const bool at_break = index < count && chunk[index] == '\n';
            if (index < count && !at_break) continue;
            // The bytes from run to index belong to the current line.
            const usize length = index - run;
            if (!record.oversized && length > 0) {
                if (record.bytes.count() + length > link.message_limit) {
                    record.bytes.clear();
                    record.oversized = true;
                } else {
                    const View<char> piece(reinterpret_cast<const char*>(chunk + run), length);
                    const OperationStatus appended = record.bytes.append_copies(piece);
                    if (appended.failed()) {
                        record.bytes.clear();
                        record.oversized = true;
                    }
                }
            }
            if (at_break) open = end_line(link, record);
            run = index + 1;
        }
    }
    if (open) finish_reader(link, end);
    __atomic_store_n(&link.finished, 1, __ATOMIC_RELEASE);
}

// ---- the writer task -----------------------------------------------------------------

void write_record(detail::ServerState& state, detail::OutRecord& record, bool& healthy) noexcept {
    if (!healthy) {
        if (record.response) bump(state.counters.replies_discarded);
        return;
    }
    if (record.line.empty() || record.line.view().data()[record.line.count() - 1] != '\n') {
        if (record.line.append("\n").failed()) return;
    }
    const TextView text = record.line.view();
    const View<Byte> bytes(reinterpret_cast<const Byte*>(text.data()), text.count());
    const OperationStatus written = state.output.write_all(bytes);
    if (written.failed()) {
        healthy = false;
        detail::note_end(state, ServeEnd::output_failed);
        state.serving.cancel(ContextEnd::shut_down);
        return;
    }
    if (record.response) {
        bump(state.counters.replies_sent);
    } else {
        bump(state.counters.notifications_sent);
    }
}

void writer_entry(const StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    detail::ServerState& state = *static_cast<detail::ServerState*>(context);
    // The writer outlives serving's end (it sends the closing responses),
    // so it waits on a context of its own that never ends; the channels'
    // closing is what ends it.
    const Context forever;
    bool healthy = true;
    bool replies_open = true;
    bool notices_open = true;
    detail::OutRecord record;
    while (replies_open || notices_open) {
        ChannelCore* active[2]{};
        usize active_count = 0;
        if (replies_open) {
            active[active_count] = &state.replies;
            active_count = active_count + 1;
        }
        if (notices_open) {
            active[active_count] = &state.notices;
            active_count = active_count + 1;
        }
        const SelectResult selected = select_receivable(View<ChannelCore*>(active, active_count), forever);
        const usize* const ready = selected.ready_index();
        if (ready == nullptr) break; // never: the context does not end, and two attachments fit
        const bool from_replies = active[*ready] == &state.replies;
        const ChannelStatus status = from_replies ? state.replies.try_receive(record) : state.notices.try_receive(record);
        if (status == ChannelStatus::delivered) {
            write_record(state, record, healthy);
        } else if (status == ChannelStatus::closed) {
            if (from_replies) {
                replies_open = false;
            } else {
                notices_open = false;
            }
        }
    }
    state.output.close();
}

// ---- the handler tasks ----------------------------------------------------------------

void handler_entry(const StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    detail::ServerState& state = *static_cast<detail::ServerState*>(context);
    Shared<detail::CallRecord> call;
    while (state.calls.receive(call) == ChannelStatus::delivered) {
        detail::execute_call(state, *call);
        detail::release_slot(state, (*call).slot);
        call.reset();
    }
}

// The shutdown sequence of wait() (also start's rollback): every step
// tolerates tasks that never started.
void shut_down(detail::ServerState& state) noexcept {
    static_cast<void>(state.dispatcher.join());
    detail::note_end(state, ServeEnd::stopped);
    state.serving.cancel(ContextEnd::shut_down);
    state.calls.close();
    state.handlers.join();
    detail::close_subscriptions(state);
    state.replies.close();
    state.notices.close();
    static_cast<void>(state.writer.join());
    if (state.link.present()) {
        detail::ReaderLink& link = *state.link;
        link.inbox.close();
        if (__atomic_load_n(&link.finished, __ATOMIC_ACQUIRE) != 0) {
            static_cast<void>(state.reader.join());
        } else {
            static_cast<void>(state.reader.abandon());
        }
    }
    state.output.close();
}

void release_state(detail::ServerState* const state) noexcept {
    ::stxlib::detail::destroy(state);
    memory::release(state);
}

} // namespace

// ---- protocol constants and identities ----------------------------------------------------

TextView legacy_protocol_version(const usize position) noexcept {
    STXLIB_REQUIRE(position < legacy_protocol_version_count, "legacy_protocol_version: the position is out of range");
    return legacy_versions[position];
}

ClientIdentity ClientIdentity::of(const TextView name, const TextView version, const TextView title,
                                  const TextView model) noexcept {
    ClientIdentity result;
    result.name_count_ = copy_cut(result.name_, name);
    result.version_count_ = copy_cut(result.version_, version);
    result.title_count_ = copy_cut(result.title_, title);
    result.model_count_ = copy_cut(result.model_, model);
    return result;
}

TextView ClientIdentity::name() const& noexcept { return stored_text(name_, name_count_); }
TextView ClientIdentity::version() const& noexcept { return stored_text(version_, version_count_); }
TextView ClientIdentity::title() const& noexcept { return stored_text(title_, title_count_); }
TextView ClientIdentity::model() const& noexcept { return stored_text(model_, model_count_); }

bool ClientIdentity::operator==(const ClientIdentity& other) const noexcept {
    const bool result = same_text(name_, name_count_, other.name_, other.name_count_) &&
                        same_text(version_, version_count_, other.version_, other.version_count_) &&
                        same_text(title_, title_count_, other.title_, other.title_count_) &&
                        same_text(model_, model_count_, other.model_, other.model_count_);
    return result;
}

bool ClientIdentity::operator!=(const ClientIdentity& other) const noexcept { return !(*this == other); }

// ---- requests ---------------------------------------------------------------------------------

const Context& RequestView::context() const noexcept { return record_.context; }
const ClientIdentity& RequestView::client() const noexcept { return record_.client; }
TextView RequestView::protocol_version() const noexcept { return record_.protocol_version; }

void RequestView::progress(const double progress, const double total, const TextView message) const noexcept {
    if (!record_.has_progress || record_.context.done()) return;
    if (record_.progress_sent && !(progress > record_.last_progress)) return;
    record_.last_progress = progress;
    record_.progress_sent = true;
    detail::write_progress(record_, progress, total, message);
}

void RequestView::log(const LogLevel level, const TextView text) const noexcept {
    if (record_.log_threshold < 0 || static_cast<i32>(level) < record_.log_threshold) return;
    if (record_.context.done()) return;
    detail::write_request_log(record_, level, text);
}

TextView ToolRequest::tool_name() const noexcept { return record_.name.view(); }
const JsonDocument& ToolRequest::document() const noexcept { return *record_.arguments_document; }
JsonRef ToolRequest::arguments() const noexcept { return record_.arguments; }

Optional<TextView> ToolRequest::text(const TextView name) const noexcept {
    const Optional<TextView> result = (*record_.arguments_document).member_text(record_.arguments, name);
    return result;
}

Optional<i64> ToolRequest::integer(const TextView name) const noexcept {
    const Optional<i64> result = (*record_.arguments_document).member_integer(record_.arguments, name);
    return result;
}

Optional<double> ToolRequest::number(const TextView name) const noexcept {
    const JsonDocument& document = *record_.arguments_document;
    const Optional<JsonRef> found = document.member(record_.arguments, name);
    if (found.absent()) return Optional<double>::none();
    const Optional<double> result = document.number(*found.value());
    return result;
}

Optional<bool> ToolRequest::boolean(const TextView name) const noexcept {
    const Optional<bool> result = (*record_.arguments_document).member_boolean(record_.arguments, name);
    return result;
}

Optional<JsonRef> ToolRequest::member(const TextView name) const noexcept {
    const Optional<JsonRef> result = (*record_.arguments_document).member(record_.arguments, name);
    return result;
}

TextView ResourceRequest::uri() const noexcept { return record_.name.view(); }

Optional<TextView> ResourceRequest::parameter(const TextView name) const noexcept {
    for (usize index = 0; index < record_.parameters.count(); ++index) {
        const detail::TemplateParameter& entry = record_.parameters[index];
        if (entry.name.view() == name) return Optional<TextView>::of(entry.value.view());
    }
    return Optional<TextView>::none();
}

TextView PromptRequest::prompt_name() const noexcept { return record_.name.view(); }

Optional<TextView> PromptRequest::argument(const TextView name) const noexcept {
    if (!record_.has_prompt_arguments) return Optional<TextView>::none();
    const Optional<TextView> result = record_.document.member_text(record_.prompt_arguments, name);
    return result;
}

// ---- replies -------------------------------------------------------------------------------------

namespace {

// The string the next item of a reply's list is written into (a scratch
// string when the list cannot grow: the failure is kept, the reply becomes
// an internal failure).
Utf8String& begin_item(detail::ReplyRecord& reply) noexcept {
    const OperationStatus pushed = reply.items.push(Utf8String());
    if (pushed.failed()) {
        detail::note_trouble(reply, pushed);
        reply.scratch.clear();
        return reply.scratch;
    }
    return reply.items[reply.items.count() - 1];
}

void end_item(detail::ReplyRecord& reply, const JsonWriter& writer) noexcept {
    detail::note_trouble(reply, writer.status());
}

} // namespace

void ToolReply::text(const TextView content) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("type", "text");
    writer.member_string("text", content);
    writer.end_object();
    end_item(record_, writer);
    record_.has_text_item = true;
}

void ToolReply::image(const View<Byte> bytes, const TextView mime_type) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("type", "image");
    writer.key("data");
    writer.base64(bytes);
    writer.member_string("mimeType", mime_type);
    writer.end_object();
    end_item(record_, writer);
}

void ToolReply::resource_link(const TextView uri, const TextView name, const TextView mime_type) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("type", "resource_link");
    writer.member_string("uri", uri);
    writer.member_string("name", name.empty() ? uri : name);
    if (!mime_type.empty()) writer.member_string("mimeType", mime_type);
    writer.end_object();
    end_item(record_, writer);
}

void ToolReply::embedded_text(const TextView uri, const TextView mime_type, const TextView content) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("type", "resource");
    writer.key("resource");
    writer.begin_object();
    writer.member_string("uri", uri);
    if (!mime_type.empty()) writer.member_string("mimeType", mime_type);
    writer.member_string("text", content);
    writer.end_object();
    writer.end_object();
    end_item(record_, writer);
}

JsonWriter& ToolReply::structured() noexcept {
    record_.structured.clear();
    record_.structured_writer.reset();
    record_.structured_started = true;
    return record_.structured_writer;
}

void ToolReply::fail(const TextView code, const TextView message, const TextView hint) noexcept {
    // The error's text is a small JSON object, readable by a model and by a
    // program: {"error": code, "message": ..., "hint": ...}.
    Utf8String body;
    {
        JsonWriter inner(body, record_.limit);
        inner.begin_object();
        inner.member_string("error", code);
        inner.member_string("message", message);
        if (!hint.empty()) inner.member_string("hint", hint);
        inner.end_object();
        detail::note_trouble(record_, inner.status());
    }
    record_.error_item.clear();
    JsonWriter writer(record_.error_item, record_.limit);
    writer.begin_object();
    writer.member_string("type", "text");
    writer.member_string("text", body.view());
    writer.end_object();
    detail::note_trouble(record_, writer.status());
    record_.is_error = true;
}

void ResourceReply::text(const TextView uri, const TextView mime_type, const TextView content) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("uri", uri.empty() ? record_.default_uri : uri);
    if (!mime_type.empty()) writer.member_string("mimeType", mime_type);
    writer.member_string("text", content);
    writer.end_object();
    end_item(record_, writer);
}

void ResourceReply::blob(const TextView uri, const TextView mime_type, const View<Byte> bytes) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("uri", uri.empty() ? record_.default_uri : uri);
    if (!mime_type.empty()) writer.member_string("mimeType", mime_type);
    writer.key("blob");
    writer.base64(bytes);
    writer.end_object();
    end_item(record_, writer);
}

void ResourceReply::not_found() noexcept { record_.outcome = detail::ReplyOutcome::not_found; }

void ResourceReply::fail(const TextView message) noexcept {
    record_.outcome = detail::ReplyOutcome::failed;
    detail::note_trouble(record_, record_.failure_message.assign(message));
}

void PromptReply::description(const TextView text) noexcept {
    detail::note_trouble(record_, record_.description.assign(text));
    record_.has_description = true;
}

namespace {

void prompt_text(detail::ReplyRecord& reply, const TextView role, const TextView text) noexcept {
    JsonWriter writer(begin_item(reply), reply.limit);
    writer.begin_object();
    writer.member_string("role", role);
    writer.key("content");
    writer.begin_object();
    writer.member_string("type", "text");
    writer.member_string("text", text);
    writer.end_object();
    writer.end_object();
    end_item(reply, writer);
}

} // namespace

void PromptReply::user_text(const TextView text) noexcept { prompt_text(record_, "user", text); }
void PromptReply::assistant_text(const TextView text) noexcept { prompt_text(record_, "assistant", text); }

void PromptReply::user_resource(const TextView uri, const TextView mime_type, const TextView content) noexcept {
    JsonWriter writer(begin_item(record_), record_.limit);
    writer.begin_object();
    writer.member_string("role", "user");
    writer.key("content");
    writer.begin_object();
    writer.member_string("type", "resource");
    writer.key("resource");
    writer.begin_object();
    writer.member_string("uri", uri);
    if (!mime_type.empty()) writer.member_string("mimeType", mime_type);
    writer.member_string("text", content);
    writer.end_object();
    writer.end_object();
    writer.end_object();
    end_item(record_, writer);
}

void PromptReply::fail(const TextView message) noexcept {
    record_.outcome = detail::ReplyOutcome::failed;
    detail::note_trouble(record_, record_.failure_message.assign(message));
}

// ---- shared helpers ----------------------------------------------------------------------------------

namespace detail {

void note_end(ServerState& state, const ServeEnd reason) noexcept {
    u8 expected = 0xFF;
    static_cast<void>(__atomic_compare_exchange_n(&state.end_reason, &expected, static_cast<u8>(reason), false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

void send_reply(ServerState& state, Utf8String& line, const bool response) noexcept {
    OutRecord record;
    record.line = static_cast<Utf8String&&>(line);
    record.response = response;
    if (state.replies.send(record) != ChannelStatus::delivered && response) bump(state.counters.replies_discarded);
}

void send_notice(ServerState& state, Utf8String& line) noexcept {
    OutRecord record;
    record.line = static_cast<Utf8String&&>(line);
    bool dropped = false;
    const ChannelStatus status = state.notices.send_dropping_oldest(record, dropped);
    if (status == ChannelStatus::delivered && dropped) bump(state.counters.notifications_dropped);
}

void release_slot(ServerState& state, const usize slot) noexcept {
    Shared<CallRecord> last;
    {
        ScopedLock lock(state.mutex);
        InFlight& entry = state.in_flight[slot];
        if (!entry.used) return;
        entry.used = false;
        entry.id_json.clear();
        last = static_cast<Shared<CallRecord>&&>(entry.call);
        state.in_flight_count = state.in_flight_count - 1;
    }
    // `last` (perhaps the record's last owner) ends outside the lock.
}

void note_trouble(ReplyRecord& reply, const OperationStatus& status) noexcept {
    if (status.succeeded() || reply.trouble.present()) return;
    reply.trouble = Optional<ErrorRecord>::of(*status.error());
}

} // namespace detail

// ---- the server ------------------------------------------------------------------------------------------

OperationResult<Server> Server::create(const ServerIdentity& identity, const ServerOptions& options,
                                       const ServerEvents& events) noexcept {
    if (identity.name.empty()) return OperationResult<Server>::failure(invalid(create_operation, "the server needs a name"));
    if (options.handler_tasks == 0 || options.handler_tasks > task_group_capacity - 2) {
        return OperationResult<Server>::failure(
            invalid(create_operation, "handler_tasks must be 1 to task_group_capacity - 2"));
    }
    if (options.message_limit < detail::message_limit_floor || options.message_limit > detail::message_limit_ceiling) {
        return OperationResult<Server>::failure(invalid(create_operation, "message_limit must be 1 KiB to 64 MiB"));
    }
    if (options.request_limit == 0 || options.request_limit > detail::request_limit_ceiling) {
        return OperationResult<Server>::failure(invalid(create_operation, "request_limit must be 1 to 256"));
    }
    void* const block = memory::allocate(sizeof(detail::ServerState), alignof(detail::ServerState));
    if (block == nullptr) {
        return OperationResult<Server>::failure(ErrorRecord::make(ErrorKind::out_of_memory, create_operation));
    }
    detail::ServerState* const state = ::stxlib::detail::construct<detail::ServerState>(block);
    Server server(state);
    OperationStatus copied = OperationStatus::success();
    copy_into((*state).name, identity.name, copied);
    copy_into((*state).version, identity.version, copied);
    copy_into((*state).title, identity.title, copied);
    copy_into((*state).instructions, identity.instructions, copied);
    copy_into((*state).model_meta_key, options.model_meta_key, copied);
    if (copied.failed()) return OperationResult<Server>::failure(*copied.error());
    (*state).options = options;
    (*state).options.model_meta_key = TextView();
    (*state).options.json_limits.max_bytes = options.message_limit;
    (*state).events = events;
    for (usize slot = 0; slot < options.request_limit; ++slot) {
        const OperationStatus pushed = (*state).in_flight.push(detail::InFlight{});
        if (pushed.failed()) return OperationResult<Server>::failure(*pushed.error());
    }
    return OperationResult<Server>::success(static_cast<Server&&>(server));
}

Server::~Server() {
    if (state_ == nullptr) return;
    stop();
    static_cast<void>(wait());
    release_state(state_);
    state_ = nullptr;
}

Server& Server::operator=(Server&& other) noexcept {
    if (this == &other) return *this;
    if (state_ != nullptr) {
        stop();
        static_cast<void>(wait());
        release_state(state_);
    }
    state_ = other.state_;
    other.state_ = nullptr;
    return *this;
}

OperationStatus Server::bind_tool(const ToolSpec& spec, const ToolHandler handler, void* const binding) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::bind_tool: an empty server");
    STXLIB_REQUIRE(handler != nullptr, "Server::bind_tool: a null handler");
    if (!valid_tool_name(spec.name)) {
        return OperationStatus::failure(
            invalid(bind_operation, "a tool name is 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'"));
    }
    OperationResult<Shared<detail::ToolBinding>> made = Shared<detail::ToolBinding>::make();
    if (made.failed()) return made.status();
    Shared<detail::ToolBinding> tool = static_cast<OperationResult<Shared<detail::ToolBinding>>&&>(made).take_value();
    detail::ToolBinding& entry = *tool;
    OperationStatus copied = OperationStatus::success();
    copy_into(entry.name, spec.name, copied);
    copy_into(entry.title, spec.title, copied);
    copy_into(entry.description, spec.description, copied);
    if (copied.failed()) return copied;
    OperationResult<detail::Schema> input =
        detail::Schema::compile(spec.input_schema.empty() ? default_input_schema : spec.input_schema);
    if (input.failed()) return input.status();
    entry.input = static_cast<OperationResult<detail::Schema>&&>(input).take_value();
    if (!entry.input.describes_object()) {
        return OperationStatus::failure(invalid(bind_operation, "a tool's input schema must say \"type\": \"object\""));
    }
    if (!spec.output_schema.empty()) {
        OperationResult<detail::Schema> output = detail::Schema::compile(spec.output_schema);
        if (output.failed()) return output.status();
        entry.output = static_cast<OperationResult<detail::Schema>&&>(output).take_value();
        if (!entry.output.describes_object()) {
            return OperationStatus::failure(
                invalid(bind_operation, "a tool's output schema must say \"type\": \"object\""));
        }
        entry.has_output = true;
    }
    entry.annotations = spec.annotations;
    entry.handler = handler;
    entry.binding = binding;
    {
        ScopedLock lock((*state_).mutex);
        for (usize index = 0; index < (*state_).tools.count(); ++index) {
            if ((*(*state_).tools[index]).name.view() == spec.name) {
                return OperationStatus::failure(ErrorRecord::make(ErrorKind::already_exists, bind_operation,
                                                                  ErrorDetail{.message = "the tool is bound already"}));
            }
        }
        const OperationStatus pushed = (*state_).tools.push(static_cast<Shared<detail::ToolBinding>&&>(tool));
        if (pushed.failed()) return pushed;
    }
    detail::broadcast_list_changed(*state_, detail::ListKind::tools);
    return OperationStatus::success();
}

namespace {

// Removes the entry named `name` from one of the binding lists (its last
// reference may be a running call's: the record ends with the call).
template <typename Binding>
bool remove_named(Vector<Shared<Binding>>& list, const TextView name, Shared<Binding>& removed) noexcept {
    for (usize index = 0; index < list.count(); ++index) {
        if ((*list[index]).name.view() != name) continue;
        removed = static_cast<Shared<Binding>&&>(list[index]);
        const usize last = list.count() - 1;
        if (index != last) list[index] = static_cast<Shared<Binding>&&>(list[last]);
        list.remove_last();
        return true;
    }
    return false;
}

} // namespace

OperationStatus Server::unbind_tool(const TextView name) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::unbind_tool: an empty server");
    Shared<detail::ToolBinding> removed;
    bool found = false;
    {
        ScopedLock lock((*state_).mutex);
        found = remove_named((*state_).tools, name, removed);
    }
    if (!found) {
        return OperationStatus::failure(
            ErrorRecord::make(ErrorKind::not_found, unbind_operation, ErrorDetail{.message = "no such tool is bound"}));
    }
    detail::broadcast_list_changed(*state_, detail::ListKind::tools);
    return OperationStatus::success();
}

namespace {

OperationStatus bind_resource_entry(detail::ServerState& state, const TextView uri, const bool is_template,
                                    const TextView name, const TextView title, const TextView description,
                                    const TextView mime_type, const ResourceHandler handler,
                                    void* const binding) noexcept {
    if (uri.empty()) return OperationStatus::failure(invalid(bind_operation, "a resource needs a URI"));
    if (is_template) {
        // Level 1: {name} expressions, closed, non-empty, not nested.
        bool open = false;
        usize expression = 0;
        for (usize index = 0; index < uri.count(); ++index) {
            const char byte = uri.data()[index];
            if (byte == '{') {
                if (open) return OperationStatus::failure(invalid(bind_operation, "a nested '{' in a URI template"));
                open = true;
                expression = 0;
            } else if (byte == '}') {
                if (!open || expression == 0) {
                    return OperationStatus::failure(invalid(bind_operation, "an empty or unopened '}' in a URI template"));
                }
                open = false;
            } else if (open) {
                const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                   (byte >= '0' && byte <= '9') || byte == '_';
                if (!plain) {
                    return OperationStatus::failure(
                        invalid(bind_operation, "URI template expressions are level 1 names (A-Z, a-z, 0-9, '_')"));
                }
                expression = expression + 1;
            }
        }
        if (open) return OperationStatus::failure(invalid(bind_operation, "an unclosed '{' in a URI template"));
    }
    OperationResult<Shared<detail::ResourceBinding>> made = Shared<detail::ResourceBinding>::make();
    if (made.failed()) return made.status();
    Shared<detail::ResourceBinding> resource =
        static_cast<OperationResult<Shared<detail::ResourceBinding>>&&>(made).take_value();
    detail::ResourceBinding& entry = *resource;
    OperationStatus copied = OperationStatus::success();
    copy_into(entry.uri, uri, copied);
    copy_into(entry.name, name.empty() ? uri : name, copied);
    copy_into(entry.title, title, copied);
    copy_into(entry.description, description, copied);
    copy_into(entry.mime_type, mime_type, copied);
    if (copied.failed()) return copied;
    entry.is_template = is_template;
    entry.handler = handler;
    entry.binding = binding;
    {
        ScopedLock lock(state.mutex);
        for (usize index = 0; index < state.resources.count(); ++index) {
            if ((*state.resources[index]).uri.view() == uri) {
                return OperationStatus::failure(ErrorRecord::make(
                    ErrorKind::already_exists, bind_operation, ErrorDetail{.message = "the resource is bound already"}));
            }
        }
        const OperationStatus pushed = state.resources.push(static_cast<Shared<detail::ResourceBinding>&&>(resource));
        if (pushed.failed()) return pushed;
    }
    detail::broadcast_list_changed(state, detail::ListKind::resources);
    return OperationStatus::success();
}

} // namespace

OperationStatus Server::bind_resource(const ResourceSpec& spec, const ResourceHandler handler,
                                      void* const binding) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::bind_resource: an empty server");
    STXLIB_REQUIRE(handler != nullptr, "Server::bind_resource: a null handler");
    const OperationStatus result = bind_resource_entry(*state_, spec.uri, false, spec.name, spec.title,
                                                       spec.description, spec.mime_type, handler, binding);
    return result;
}

OperationStatus Server::bind_resource_template(const ResourceTemplateSpec& spec, const ResourceHandler handler,
                                               void* const binding) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::bind_resource_template: an empty server");
    STXLIB_REQUIRE(handler != nullptr, "Server::bind_resource_template: a null handler");
    const OperationStatus result = bind_resource_entry(*state_, spec.uri_template, true, spec.name, spec.title,
                                                       spec.description, spec.mime_type, handler, binding);
    return result;
}

OperationStatus Server::unbind_resource(const TextView uri_or_template) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::unbind_resource: an empty server");
    Shared<detail::ResourceBinding> removed;
    bool found = false;
    {
        ScopedLock lock((*state_).mutex);
        Vector<Shared<detail::ResourceBinding>>& list = (*state_).resources;
        for (usize index = 0; index < list.count() && !found; ++index) {
            if ((*list[index]).uri.view() != uri_or_template) continue;
            removed = static_cast<Shared<detail::ResourceBinding>&&>(list[index]);
            const usize last = list.count() - 1;
            if (index != last) list[index] = static_cast<Shared<detail::ResourceBinding>&&>(list[last]);
            list.remove_last();
            found = true;
        }
    }
    if (!found) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::not_found, unbind_operation,
                                                          ErrorDetail{.message = "no such resource is bound"}));
    }
    detail::broadcast_list_changed(*state_, detail::ListKind::resources);
    return OperationStatus::success();
}

OperationStatus Server::bind_prompt(const PromptSpec& spec, const PromptHandler handler, void* const binding) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::bind_prompt: an empty server");
    STXLIB_REQUIRE(handler != nullptr, "Server::bind_prompt: a null handler");
    if (spec.name.empty()) return OperationStatus::failure(invalid(bind_operation, "a prompt needs a name"));
    OperationResult<Shared<detail::PromptBinding>> made = Shared<detail::PromptBinding>::make();
    if (made.failed()) return made.status();
    Shared<detail::PromptBinding> prompt = static_cast<OperationResult<Shared<detail::PromptBinding>>&&>(made).take_value();
    detail::PromptBinding& entry = *prompt;
    OperationStatus copied = OperationStatus::success();
    copy_into(entry.name, spec.name, copied);
    copy_into(entry.title, spec.title, copied);
    copy_into(entry.description, spec.description, copied);
    for (usize index = 0; index < spec.arguments.count() && copied.succeeded(); ++index) {
        const PromptArgument& argument = spec.arguments[index];
        if (argument.name.empty()) return OperationStatus::failure(invalid(bind_operation, "a prompt argument needs a name"));
        detail::PromptArgumentCopy copy;
        copy_into(copy.name, argument.name, copied);
        copy_into(copy.description, argument.description, copied);
        copy.required = argument.required;
        if (copied.succeeded()) copied = entry.arguments.push(static_cast<detail::PromptArgumentCopy&&>(copy));
    }
    if (copied.failed()) return copied;
    entry.handler = handler;
    entry.binding = binding;
    {
        ScopedLock lock((*state_).mutex);
        for (usize index = 0; index < (*state_).prompts.count(); ++index) {
            if ((*(*state_).prompts[index]).name.view() == spec.name) {
                return OperationStatus::failure(ErrorRecord::make(
                    ErrorKind::already_exists, bind_operation, ErrorDetail{.message = "the prompt is bound already"}));
            }
        }
        const OperationStatus pushed = (*state_).prompts.push(static_cast<Shared<detail::PromptBinding>&&>(prompt));
        if (pushed.failed()) return pushed;
    }
    detail::broadcast_list_changed(*state_, detail::ListKind::prompts);
    return OperationStatus::success();
}

OperationStatus Server::unbind_prompt(const TextView name) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::unbind_prompt: an empty server");
    Shared<detail::PromptBinding> removed;
    bool found = false;
    {
        ScopedLock lock((*state_).mutex);
        found = remove_named((*state_).prompts, name, removed);
    }
    if (!found) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::not_found, unbind_operation,
                                                          ErrorDetail{.message = "no such prompt is bound"}));
    }
    detail::broadcast_list_changed(*state_, detail::ListKind::prompts);
    return OperationStatus::success();
}

OperationStatus Server::start(PipeReader&& input, PipeWriter&& output) noexcept {
    STXLIB_REQUIRE(state_ != nullptr, "Server::start: an empty server");
    detail::ServerState& state = *state_;
    PipeReader taken_input = static_cast<PipeReader&&>(input);
    PipeWriter taken_output = static_cast<PipeWriter&&>(output);
    if (!taken_input.is_open() || !taken_output.is_open()) {
        return OperationStatus::failure(invalid(start_operation, "the input or the output is closed"));
    }
    u8 expected = 0;
    if (!__atomic_compare_exchange_n(&state.started, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return host_failure::failed(ErrorKind::invalid_argument, start_operation, HostCode::invalid_state);
    }
    OperationResult<Shared<detail::ReaderLink>> made = Shared<detail::ReaderLink>::make();
    if (made.failed()) {
        __atomic_store_n(&state.waited, 1, __ATOMIC_RELEASE);
        detail::note_end(state, ServeEnd::stopped);
        return made.status();
    }
    state.link = static_cast<OperationResult<Shared<detail::ReaderLink>>&&>(made).take_value();
    (*state.link).input = static_cast<PipeReader&&>(taken_input);
    (*state.link).message_limit = state.options.message_limit;
    state.output = static_cast<PipeWriter&&>(taken_output);

    OperationStatus status = OperationStatus::success();
    {
        OperationResult<Thread> writer = Thread::start(&writer_entry, &state);
        if (writer.failed()) {
            status = writer.status();
        } else {
            state.writer = static_cast<OperationResult<Thread>&&>(writer).take_value();
        }
    }
    for (usize index = 0; index < state.options.handler_tasks && status.succeeded(); ++index) {
        status = state.handlers.start(&handler_entry, &state);
    }
    if (status.succeeded()) {
        OperationResult<Thread> dispatcher = Thread::start(&detail::dispatch_entry, &state);
        if (dispatcher.failed()) {
            status = dispatcher.status();
        } else {
            state.dispatcher = static_cast<OperationResult<Thread>&&>(dispatcher).take_value();
        }
    }
    if (status.succeeded()) {
        void* const block = memory::allocate(sizeof(ReaderStart), alignof(ReaderStart));
        if (block == nullptr) {
            status = OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, start_operation));
        } else {
            ReaderStart* const reader_start = ::stxlib::detail::construct<ReaderStart>(block);
            (*reader_start).link = state.link;
            OperationResult<Thread> reader = Thread::start(&reader_entry, reader_start);
            if (reader.failed()) {
                ::stxlib::detail::destroy(reader_start);
                memory::release(reader_start);
                status = reader.status();
            } else {
                state.reader = static_cast<OperationResult<Thread>&&>(reader).take_value();
            }
        }
    }
    if (status.failed()) {
        // Nothing is left running: the shutdown sequence joins what started.
        detail::note_end(state, ServeEnd::stopped);
        state.serving.cancel(ContextEnd::shut_down);
        shut_down(state);
        __atomic_store_n(&state.waited, 1, __ATOMIC_RELEASE);
    }
    return status;
}

ServeEnd Server::wait() noexcept {
    if (state_ == nullptr) return ServeEnd::stopped;
    detail::ServerState& state = *state_;
    if (__atomic_load_n(&state.started, __ATOMIC_ACQUIRE) == 0) return ServeEnd::stopped;
    if (__atomic_load_n(&state.waited, __ATOMIC_ACQUIRE) == 0) {
        shut_down(state);
        __atomic_store_n(&state.waited, 1, __ATOMIC_RELEASE);
    }
    const u8 reason = __atomic_load_n(&state.end_reason, __ATOMIC_ACQUIRE);
    const ServeEnd result = reason == 0xFF ? ServeEnd::stopped : static_cast<ServeEnd>(reason);
    return result;
}

void Server::stop() noexcept {
    if (state_ == nullptr) return;
    detail::note_end(*state_, ServeEnd::stopped);
    (*state_).serving.cancel(ContextEnd::shut_down);
}

void Server::notify_tools_changed() noexcept {
    if (state_ != nullptr) detail::broadcast_list_changed(*state_, detail::ListKind::tools);
}

void Server::notify_prompts_changed() noexcept {
    if (state_ != nullptr) detail::broadcast_list_changed(*state_, detail::ListKind::prompts);
}

void Server::notify_resources_changed() noexcept {
    if (state_ != nullptr) detail::broadcast_list_changed(*state_, detail::ListKind::resources);
}

void Server::notify_resource_updated(const TextView uri) noexcept {
    if (state_ != nullptr) detail::broadcast_resource_updated(*state_, uri);
}

void Server::log(const LogLevel level, const TextView text) noexcept {
    if (state_ != nullptr) detail::broadcast_log(*state_, level, text);
}

ClientIdentity Server::client() const noexcept {
    if (state_ == nullptr) return ClientIdentity();
    ScopedLock lock((*state_).mutex);
    const ClientIdentity result = (*state_).client;
    return result;
}

ServerCounters Server::counters() const noexcept {
    ServerCounters result;
    if (state_ == nullptr) return result;
    const ServerCounters& live = (*state_).counters;
    result.messages_read = __atomic_load_n(&live.messages_read, __ATOMIC_RELAXED);
    result.requests = __atomic_load_n(&live.requests, __ATOMIC_RELAXED);
    result.notifications_received = __atomic_load_n(&live.notifications_received, __ATOMIC_RELAXED);
    result.parse_errors = __atomic_load_n(&live.parse_errors, __ATOMIC_RELAXED);
    result.invalid_requests = __atomic_load_n(&live.invalid_requests, __ATOMIC_RELAXED);
    result.oversized_messages = __atomic_load_n(&live.oversized_messages, __ATOMIC_RELAXED);
    result.replies_sent = __atomic_load_n(&live.replies_sent, __ATOMIC_RELAXED);
    result.replies_discarded = __atomic_load_n(&live.replies_discarded, __ATOMIC_RELAXED);
    result.notifications_sent = __atomic_load_n(&live.notifications_sent, __ATOMIC_RELAXED);
    result.notifications_dropped = __atomic_load_n(&live.notifications_dropped, __ATOMIC_RELAXED);
    result.argument_failures = __atomic_load_n(&live.argument_failures, __ATOMIC_RELAXED);
    result.output_schema_failures = __atomic_load_n(&live.output_schema_failures, __ATOMIC_RELAXED);
    result.cancellations = __atomic_load_n(&live.cancellations, __ATOMIC_RELAXED);
    return result;
}

bool Server::started() const noexcept {
    if (state_ == nullptr) return false;
    const bool result = __atomic_load_n(&(*state_).started, __ATOMIC_ACQUIRE) != 0;
    return result;
}

} // namespace stxlib::mcp
