// stx_mcp's protocol: framing errors, eras, methods and results
// (include/stxlib/mcp.hpp; the specification is MCP 2026-07-28, with the
// legacy revisions' initialize handshake). The tasks and the state are
// server.cpp and server_state.hpp.
//
// Every message gets at most one response, and every failure is a result:
// a malformed message is a JSON-RPC error and the stream goes on; a tool's
// failure (its own, or its arguments failing the input schema) is a tool
// result marked isError; a structured result that fails the output schema
// becomes a tool error named internal_failure and is counted.

#include "mcp/server_state.hpp"

#include "stxlib/text.hpp"

namespace stxlib::mcp::detail {

namespace {

constexpr TextView key_version = "io.modelcontextprotocol/protocolVersion";
constexpr TextView key_capabilities = "io.modelcontextprotocol/clientCapabilities";
constexpr TextView key_client = "io.modelcontextprotocol/clientInfo";
constexpr TextView key_log_level = "io.modelcontextprotocol/logLevel";
constexpr TextView key_server = "io.modelcontextprotocol/serverInfo";
constexpr TextView key_subscription = "io.modelcontextprotocol/subscriptionId";

// Legacy revisions' code for a resource that does not exist.
constexpr i32 legacy_resource_not_found = -32002;

constexpr TextView log_level_names[] = {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};

constexpr TextView empty_object = "{}";

i32 log_level_of(const TextView name) noexcept {
    for (usize index = 0; index < 8; ++index) {
        if (log_level_names[index] == name) return static_cast<i32>(index);
    }
    return -1;
}

TextView slice(const TextView text, const usize first, const usize last) noexcept {
    const TextView result = ::stxlib::detail::TextViewAccess::adopt_validated(text.data() + first, last - first);
    return result;
}

// A request id written canonically, so the same id always has the same
// text: a string through the writer's escaping, an integer as digits.
// Other ids (null, fractions, containers) are refused.
bool canonical_id(const JsonDocument& document, const JsonRef id, Utf8String& out) noexcept {
    out.clear();
    JsonWriter writer(out);
    if (document.is(id, JsonKind::string)) {
        writer.string(document.text(id).value_or(TextView()));
    } else {
        const Optional<i64> integral = document.integer(id);
        if (integral.absent()) return false;
        writer.integer(*integral.value());
    }
    const bool result = writer.status().succeeded();
    return result;
}

// ---- writing --------------------------------------------------------------------

void open_envelope(JsonWriter& writer, const TextView id_json) noexcept {
    writer.begin_object();
    writer.member_string("jsonrpc", "2.0");
    writer.key("id");
    if (id_json.empty()) {
        writer.null_value();
    } else {
        writer.raw(id_json);
    }
}

void open_result(JsonWriter& writer, const TextView id_json) noexcept {
    open_envelope(writer, id_json);
    writer.key("result");
    writer.begin_object();
}

void open_notification(JsonWriter& writer, const TextView method) noexcept {
    writer.begin_object();
    writer.member_string("jsonrpc", "2.0");
    writer.member_string("method", method);
}

void write_server_info(JsonWriter& writer, const ServerState& state) noexcept {
    writer.begin_object();
    writer.member_string("name", state.name.view());
    writer.member_string("version", state.version.view());
    if (!state.title.empty()) writer.member_string("title", state.title.view());
    writer.end_object();
}

// The modern result's closing members (resultType, serverInfo), then the
// result and the envelope.
void close_result(JsonWriter& writer, const ServerState& state, const bool modern) noexcept {
    if (modern) {
        writer.member_string("resultType", "complete");
        writer.key("_meta");
        writer.begin_object();
        writer.key(key_server);
        write_server_info(writer, state);
        writer.end_object();
    }
    writer.end_object();
    writer.end_object();
}

void write_cache_hints(JsonWriter& writer, const bool modern, const u64 ttl_milliseconds) noexcept {
    if (!modern) return;
    writer.key("ttlMs");
    writer.unsigned_integer(ttl_milliseconds);
    writer.member_string("cacheScope", "private");
}

void write_supported_versions(JsonWriter& writer) noexcept {
    writer.begin_array();
    writer.string(modern_protocol_version);
    for (usize position = 0; position < legacy_protocol_version_count; ++position) {
        writer.string(legacy_protocol_version(position));
    }
    writer.end_array();
}

enum class ErrorData : u8 {
    none = 0,
    versions = 1, // {supported, requested?}: data_text is the requested version
    uri = 2,      // {uri}: data_text is the URI
};

void send_error(ServerState& state, const TextView id_json, const i32 code, const TextView message,
                const ErrorData data = ErrorData::none, const TextView data_text = TextView()) noexcept {
    if (code == static_cast<i32>(RpcError::parse_error)) bump(state.counters.parse_errors);
    if (code == static_cast<i32>(RpcError::invalid_request)) bump(state.counters.invalid_requests);
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_envelope(writer, id_json);
    writer.key("error");
    writer.begin_object();
    writer.member_integer("code", code);
    writer.member_string("message", message);
    if (data == ErrorData::versions) {
        writer.key("data");
        writer.begin_object();
        writer.key("supported");
        write_supported_versions(writer);
        if (!data_text.empty()) writer.member_string("requested", data_text);
        writer.end_object();
    } else if (data == ErrorData::uri) {
        writer.key("data");
        writer.begin_object();
        writer.member_string("uri", data_text);
        writer.end_object();
    }
    writer.end_object();
    writer.end_object();
    if (writer.status().failed()) {
        // Only an enormous URI in the data can pass the limit: send the
        // error without its data.
        line.clear();
        JsonWriter plain(line);
        open_envelope(plain, id_json);
        plain.key("error");
        plain.begin_object();
        plain.member_integer("code", code);
        plain.member_string("message", message);
        plain.end_object();
        plain.end_object();
        if (plain.status().failed()) return;
    }
    send_reply(state, line, true);
}

void send_rpc_error(ServerState& state, const TextView id_json, const RpcError code, const TextView message) noexcept {
    send_error(state, id_json, static_cast<i32>(code), message);
}

// Sends a written result, or -32603 when it could not be written whole
// (the message limit, or memory).
void send_written(ServerState& state, Utf8String& line, const JsonWriter& writer, const TextView id_json) noexcept {
    if (writer.status().failed()) {
        send_rpc_error(state, id_json, RpcError::internal_error, "the result does not fit the message limit");
        return;
    }
    send_reply(state, line, true);
}

void write_error_text_item(JsonWriter& writer, const TextView code, const TextView message, const TextView hint,
                           const usize limit, bool& fits) noexcept {
    Utf8String body;
    JsonWriter inner(body, limit);
    inner.begin_object();
    inner.member_string("error", code);
    inner.member_string("message", message);
    if (!hint.empty()) inner.member_string("hint", hint);
    inner.end_object();
    if (inner.status().failed()) fits = false;
    writer.begin_object();
    writer.member_string("type", "text");
    writer.member_string("text", body.view());
    writer.end_object();
}

// A tool result that is only an error (isError), sent by the dispatcher
// (arguments failing the input schema) or for a reply that failed.
void send_tool_error(ServerState& state, const TextView id_json, const bool modern, const TextView code,
                     const TextView message, const TextView hint) noexcept {
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    bool fits = true;
    open_result(writer, id_json);
    writer.key("content");
    writer.begin_array();
    write_error_text_item(writer, code, message, hint, state.options.message_limit, fits);
    writer.end_array();
    writer.member_boolean("isError", true);
    close_result(writer, state, modern);
    send_written(state, line, writer, id_json);
}

void write_capabilities(JsonWriter& writer) noexcept {
    writer.begin_object();
    writer.key("tools");
    writer.begin_object();
    writer.member_boolean("listChanged", true);
    writer.end_object();
    writer.key("resources");
    writer.begin_object();
    writer.member_boolean("subscribe", true);
    writer.member_boolean("listChanged", true);
    writer.end_object();
    writer.key("prompts");
    writer.begin_object();
    writer.member_boolean("listChanged", true);
    writer.end_object();
    writer.key("logging");
    writer.begin_object();
    writer.end_object();
    writer.end_object();
}

// ---- one request ------------------------------------------------------------------

struct Incoming final {
    JsonDocument* document{nullptr};
    JsonRef params{};
    bool has_params{false};
    JsonRef meta{};
    bool has_meta{false};
    TextView id_json{};
    TextView method{};
    bool modern{false};
    TextView version{};
    ClientIdentity client{};
    i32 log_threshold{-1};
};

void call_started(ServerState& state, const RequestKind kind, const TextView name) noexcept {
    if (state.events.request_started != nullptr) state.events.request_started(state.events.context, kind, name);
}

void call_finished(ServerState& state, const RequestKind kind, const TextView name) noexcept {
    if (state.events.request_finished != nullptr) state.events.request_finished(state.events.context, kind, name);
}

Optional<TextView> param_text(const Incoming& incoming, const TextView name) noexcept {
    if (!incoming.has_params) return Optional<TextView>::none();
    const Optional<TextView> result = (*incoming.document).member_text(incoming.params, name);
    return result;
}

// Takes a client identity from clientInfo (and the model key), and tells
// the program when it changed.
void note_client(ServerState& state, const JsonDocument& document, const Optional<JsonRef> info,
                 const Optional<TextView> model) noexcept {
    if (info.absent() && model.absent()) return;
    TextView name;
    TextView version;
    TextView title;
    if (info.present()) {
        const JsonRef object = *info.value();
        name = document.member_text(object, "name").value_or(TextView());
        version = document.member_text(object, "version").value_or(TextView());
        title = document.member_text(object, "title").value_or(TextView());
    }
    ClientIdentity seen;
    bool changed = false;
    {
        ScopedLock lock(state.mutex);
        // Without clientInfo, the model alone updates the last identity.
        const ClientIdentity& last = state.client;
        seen = info.present() ? ClientIdentity::of(name, version, title, model.value_or(TextView()))
                              : ClientIdentity::of(last.name(), last.version(), last.title(), model.value_or(TextView()));
        changed = seen != state.client;
        if (changed) state.client = seen;
    }
    if (changed && state.events.client_seen != nullptr) state.events.client_seen(state.events.context, seen);
}

// Registers a call in the in-flight table; false (with the error sent)
// when the table is full or the id is in use.
bool register_call(ServerState& state, const Shared<CallRecord>& call) noexcept {
    CallRecord& record = *call;
    bool full = false;
    bool duplicate = false;
    {
        ScopedLock lock(state.mutex);
        if (state.in_flight_count >= state.in_flight.count()) {
            full = true;
        } else {
            usize free_slot = state.in_flight.count();
            for (usize slot = 0; slot < state.in_flight.count(); ++slot) {
                const InFlight& entry = state.in_flight[slot];
                if (entry.used && entry.id_json.view() == record.id_json.view()) duplicate = true;
                if (!entry.used && free_slot == state.in_flight.count()) free_slot = slot;
            }
            for (usize index = 0; index < subscription_capacity && !duplicate; ++index) {
                const Subscription& open = state.subscriptions[index];
                if (open.used && open.id_json.view() == record.id_json.view()) duplicate = true;
            }
            if (!duplicate) {
                InFlight& entry = state.in_flight[free_slot];
                if (entry.id_json.assign(record.id_json.view()).failed()) {
                    full = true;
                } else {
                    entry.used = true;
                    entry.call = call;
                    record.slot = free_slot;
                    state.in_flight_count = state.in_flight_count + 1;
                }
            }
        }
    }
    if (duplicate) {
        send_rpc_error(state, record.id_json.view(), RpcError::invalid_request, "the request id is in use");
        return false;
    }
    if (full) {
        send_rpc_error(state, record.id_json.view(), RpcError::internal_error, "too many requests are in flight");
        return false;
    }
    return true;
}

// Queues a registered call to the handler tasks; a call refused because
// serving ended is dropped (no reply: the server is shutting down).
void enqueue(ServerState& state, Shared<CallRecord>& call) noexcept {
    const usize slot = (*call).slot;
    if (state.calls.send(call, state.serving) != ChannelStatus::delivered) release_slot(state, slot);
}

// A new call record carrying what every queued request needs.
bool make_call(ServerState& state, Incoming& incoming, const RequestKind kind, const TextView name,
               Shared<CallRecord>& out) noexcept {
    OperationResult<Shared<CallRecord>> made = Shared<CallRecord>::make(state, state.serving);
    if (made.failed()) {
        send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
        return false;
    }
    out = static_cast<OperationResult<Shared<CallRecord>>&&>(made).take_value();
    CallRecord& call = *out;
    call.kind = kind;
    call.client = incoming.client;
    call.protocol_version = incoming.version;
    call.modern = incoming.modern;
    call.log_threshold = incoming.log_threshold;
    bool copied = call.id_json.assign(incoming.id_json).succeeded() && call.name.assign(name).succeeded();
    if (copied && incoming.has_meta) {
        const Optional<JsonRef> token = (*incoming.document).member(incoming.meta, "progressToken");
        if (token.present()) {
            call.has_progress = canonical_id(*incoming.document, *token.value(), call.progress_token_json);
        }
    }
    if (!copied) {
        send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
        return false;
    }
    return true;
}

// ---- methods ----------------------------------------------------------------------------

void handle_discover(ServerState& state, const Incoming& incoming) noexcept {
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_result(writer, incoming.id_json);
    writer.key("supportedVersions");
    writer.begin_array();
    writer.string(modern_protocol_version);
    writer.end_array();
    writer.key("capabilities");
    write_capabilities(writer);
    if (!state.instructions.empty()) writer.member_string("instructions", state.instructions.view());
    write_cache_hints(writer, true, state.options.list_ttl_milliseconds);
    close_result(writer, state, true);
    send_written(state, line, writer, incoming.id_json);
}

void handle_initialize(ServerState& state, const Incoming& incoming) noexcept {
    const Optional<TextView> asked = param_text(incoming, "protocolVersion");
    if (asked.absent()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "initialize needs a protocolVersion");
        return;
    }
    TextView chosen = legacy_protocol_version(0);
    for (usize position = 0; position < legacy_protocol_version_count; ++position) {
        if (legacy_protocol_version(position) == *asked.value()) chosen = legacy_protocol_version(position);
    }
    {
        ScopedLock lock(state.mutex);
        state.legacy_version = chosen;
    }
    const JsonDocument& document = *incoming.document;
    const Optional<JsonRef> info = document.member(incoming.params, "clientInfo");
    Optional<TextView> model = Optional<TextView>::none();
    if (!state.model_meta_key.empty() && incoming.has_meta) {
        model = document.member_text(incoming.meta, state.model_meta_key.view());
    }
    note_client(state, document, info, model);
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_result(writer, incoming.id_json);
    writer.member_string("protocolVersion", chosen);
    writer.key("capabilities");
    write_capabilities(writer);
    writer.key("serverInfo");
    write_server_info(writer, state);
    if (!state.instructions.empty()) writer.member_string("instructions", state.instructions.view());
    close_result(writer, state, false);
    send_written(state, line, writer, incoming.id_json);
}

void write_tool(JsonWriter& writer, const ToolBinding& tool) noexcept {
    writer.begin_object();
    writer.member_string("name", tool.name.view());
    if (!tool.title.empty()) writer.member_string("title", tool.title.view());
    if (!tool.description.empty()) writer.member_string("description", tool.description.view());
    writer.key("inputSchema");
    writer.copy(tool.input.document(), tool.input.document().root());
    if (tool.has_output) {
        writer.key("outputSchema");
        writer.copy(tool.output.document(), tool.output.document().root());
    }
    writer.key("annotations");
    writer.begin_object();
    if (!tool.title.empty()) writer.member_string("title", tool.title.view());
    writer.member_boolean("readOnlyHint", tool.annotations.read_only);
    writer.member_boolean("destructiveHint", tool.annotations.destructive);
    writer.member_boolean("idempotentHint", tool.annotations.idempotent);
    writer.member_boolean("openWorldHint", tool.annotations.open_world);
    writer.end_object();
    writer.end_object();
}

void write_resource(JsonWriter& writer, const ResourceBinding& resource) noexcept {
    writer.begin_object();
    writer.member_string(resource.is_template ? TextView("uriTemplate") : TextView("uri"), resource.uri.view());
    writer.member_string("name", resource.name.view());
    if (!resource.title.empty()) writer.member_string("title", resource.title.view());
    if (!resource.description.empty()) writer.member_string("description", resource.description.view());
    if (!resource.mime_type.empty()) writer.member_string("mimeType", resource.mime_type.view());
    writer.end_object();
}

void write_prompt(JsonWriter& writer, const PromptBinding& prompt) noexcept {
    writer.begin_object();
    writer.member_string("name", prompt.name.view());
    if (!prompt.title.empty()) writer.member_string("title", prompt.title.view());
    if (!prompt.description.empty()) writer.member_string("description", prompt.description.view());
    writer.key("arguments");
    writer.begin_array();
    for (usize index = 0; index < prompt.arguments.count(); ++index) {
        const PromptArgumentCopy& argument = prompt.arguments[index];
        writer.begin_object();
        writer.member_string("name", argument.name.view());
        if (!argument.description.empty()) writer.member_string("description", argument.description.view());
        writer.member_boolean("required", argument.required);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
}

enum class ListMethod : u8 {
    tools = 0,
    resources = 1,
    templates = 2,
    prompts = 3,
};

void handle_list(ServerState& state, const Incoming& incoming, const ListMethod method) noexcept {
    if (param_text(incoming, "cursor").present()) {
        // This server never pages, so no cursor is one it issued.
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "unknown cursor");
        return;
    }
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_result(writer, incoming.id_json);
    {
        ScopedLock lock(state.mutex);
        if (method == ListMethod::tools) {
            writer.key("tools");
            writer.begin_array();
            for (usize index = 0; index < state.tools.count(); ++index) write_tool(writer, *state.tools[index]);
            writer.end_array();
        } else if (method == ListMethod::prompts) {
            writer.key("prompts");
            writer.begin_array();
            for (usize index = 0; index < state.prompts.count(); ++index) write_prompt(writer, *state.prompts[index]);
            writer.end_array();
        } else {
            const bool templates = method == ListMethod::templates;
            writer.key(templates ? TextView("resourceTemplates") : TextView("resources"));
            writer.begin_array();
            for (usize index = 0; index < state.resources.count(); ++index) {
                const ResourceBinding& resource = *state.resources[index];
                if (resource.is_template == templates) write_resource(writer, resource);
            }
            writer.end_array();
        }
    }
    write_cache_hints(writer, incoming.modern, state.options.list_ttl_milliseconds);
    close_result(writer, state, incoming.modern);
    send_written(state, line, writer, incoming.id_json);
}

void handle_tool_call(ServerState& state, Incoming& incoming) noexcept {
    const Optional<TextView> name = param_text(incoming, "name");
    if (name.absent()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "tools/call needs a name");
        return;
    }
    Shared<ToolBinding> tool;
    {
        ScopedLock lock(state.mutex);
        for (usize index = 0; index < state.tools.count() && tool.empty(); ++index) {
            if ((*state.tools[index]).name.view() == *name.value()) tool = state.tools[index];
        }
    }
    if (tool.empty()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "unknown tool");
        return;
    }
    const Optional<JsonRef> arguments = (*incoming.document).member(incoming.params, "arguments");
    if (arguments.present() && !(*incoming.document).is(*arguments.value(), JsonKind::object)) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "a tool's arguments are an object");
        return;
    }
    Shared<CallRecord> call;
    if (!make_call(state, incoming, RequestKind::tool_call, *name.value(), call)) return;
    CallRecord& record = *call;
    record.document = static_cast<JsonDocument&&>(*incoming.document);
    incoming.document = &record.document;
    if (arguments.present()) {
        record.arguments_document = &record.document;
        record.arguments = *arguments.value();
    } else {
        OperationResult<JsonDocument> empty = JsonDocument::parse(empty_object);
        if (empty.failed()) {
            send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
            return;
        }
        record.empty_arguments = static_cast<OperationResult<JsonDocument>&&>(empty).take_value();
        record.arguments_document = &record.empty_arguments;
        record.arguments = record.empty_arguments.root();
    }
    Utf8String problem;
    const OperationStatus valid = (*tool).input.validate(*record.arguments_document, record.arguments, problem);
    if (valid.failed()) {
        if ((*valid.error()).kind() == ErrorKind::out_of_memory) {
            send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
            return;
        }
        bump(state.counters.argument_failures);
        send_tool_error(state, incoming.id_json, incoming.modern, "invalid_arguments", problem.view(),
                        "call tools/list for the tool's inputSchema and send arguments that match it");
        return;
    }
    record.tool = static_cast<Shared<ToolBinding>&&>(tool);
    if (!register_call(state, call)) return;
    enqueue(state, call);
}

// Matches `uri` against an RFC 6570 level 1 template: each {name} takes one
// or more characters up to the template's next literal character or a '/'.
bool match_template(const TextView pattern, const TextView uri, Vector<TemplateParameter>& parameters) noexcept {
    parameters.clear();
    usize at = 0;
    usize position = 0;
    while (at < pattern.count()) {
        const char byte = pattern.data()[at];
        if (byte != '{') {
            if (position >= uri.count() || uri.data()[position] != byte) return false;
            at = at + 1;
            position = position + 1;
            continue;
        }
        usize close = at + 1;
        while (close < pattern.count() && pattern.data()[close] != '}') close = close + 1;
        if (close >= pattern.count()) return false; // refused at bind; kept safe
        const TextView name = slice(pattern, at + 1, close);
        at = close + 1;
        const bool has_stop = at < pattern.count() && pattern.data()[at] != '{';
        const char stop = has_stop ? pattern.data()[at] : '/';
        const usize start = position;
        while (position < uri.count() && uri.data()[position] != '/' && uri.data()[position] != stop) {
            position = position + 1;
        }
        if (position == start) return false;
        TemplateParameter parameter;
        if (parameter.name.assign(name).failed()) return false;
        if (parameter.value.assign(slice(uri, start, position)).failed()) return false;
        if (parameters.push(static_cast<TemplateParameter&&>(parameter)).failed()) return false;
    }
    const bool result = position == uri.count();
    return result;
}

void send_not_found(ServerState& state, const Incoming& incoming, const TextView uri) noexcept {
    const i32 code = incoming.modern ? static_cast<i32>(RpcError::invalid_params) : legacy_resource_not_found;
    send_error(state, incoming.id_json, code, "resource not found", ErrorData::uri, uri);
}

void handle_read(ServerState& state, Incoming& incoming) noexcept {
    const Optional<TextView> asked = param_text(incoming, "uri");
    if (asked.absent()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "resources/read needs a uri");
        return;
    }
    const TextView uri = *asked.value();
    Shared<ResourceBinding> resource;
    Vector<TemplateParameter> parameters;
    {
        ScopedLock lock(state.mutex);
        for (usize index = 0; index < state.resources.count() && resource.empty(); ++index) {
            const ResourceBinding& entry = *state.resources[index];
            if (!entry.is_template && entry.uri.view() == uri) resource = state.resources[index];
        }
        for (usize index = 0; index < state.resources.count() && resource.empty(); ++index) {
            const ResourceBinding& entry = *state.resources[index];
            if (entry.is_template && match_template(entry.uri.view(), uri, parameters)) resource = state.resources[index];
        }
    }
    if (resource.empty()) {
        send_not_found(state, incoming, uri);
        return;
    }
    Shared<CallRecord> call;
    if (!make_call(state, incoming, RequestKind::resource_read, uri, call)) return;
    CallRecord& record = *call;
    record.resource = static_cast<Shared<ResourceBinding>&&>(resource);
    record.parameters = static_cast<Vector<TemplateParameter>&&>(parameters);
    if (!register_call(state, call)) return;
    enqueue(state, call);
}

void handle_prompt(ServerState& state, Incoming& incoming) noexcept {
    const Optional<TextView> name = param_text(incoming, "name");
    if (name.absent()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "prompts/get needs a name");
        return;
    }
    Shared<PromptBinding> prompt;
    {
        ScopedLock lock(state.mutex);
        for (usize index = 0; index < state.prompts.count() && prompt.empty(); ++index) {
            if ((*state.prompts[index]).name.view() == *name.value()) prompt = state.prompts[index];
        }
    }
    if (prompt.empty()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "unknown prompt");
        return;
    }
    const JsonDocument& document = *incoming.document;
    const Optional<JsonRef> arguments = document.member(incoming.params, "arguments");
    if (arguments.present()) {
        bool strings = document.is(*arguments.value(), JsonKind::object);
        Optional<JsonRef> member = strings ? document.first_child(*arguments.value()) : Optional<JsonRef>::none();
        while (member.present() && strings) {
            strings = document.is(*member.value(), JsonKind::string);
            member = document.next_sibling(*member.value());
        }
        if (!strings) {
            send_rpc_error(state, incoming.id_json, RpcError::invalid_params,
                           "a prompt's arguments are an object of strings");
            return;
        }
    }
    const PromptBinding& entry = *prompt;
    for (usize index = 0; index < entry.arguments.count(); ++index) {
        const PromptArgumentCopy& argument = entry.arguments[index];
        if (!argument.required) continue;
        const bool given = arguments.present() && document.member(*arguments.value(), argument.name.view()).present();
        if (!given) {
            Utf8String message;
            static_cast<void>(message.assign("missing the required argument: "));
            static_cast<void>(message.append(argument.name.view()));
            send_rpc_error(state, incoming.id_json, RpcError::invalid_params, message.view());
            return;
        }
    }
    Shared<CallRecord> call;
    if (!make_call(state, incoming, RequestKind::prompt_get, *name.value(), call)) return;
    CallRecord& record = *call;
    record.document = static_cast<JsonDocument&&>(*incoming.document);
    incoming.document = &record.document;
    if (arguments.present()) {
        record.prompt_arguments = *arguments.value();
        record.has_prompt_arguments = true;
    }
    record.prompt = static_cast<Shared<PromptBinding>&&>(prompt);
    if (!register_call(state, call)) return;
    enqueue(state, call);
}

void write_listen_filter(JsonWriter& writer, const Subscription& subscription) noexcept {
    writer.begin_object();
    writer.member_boolean("toolsListChanged", subscription.tools);
    writer.member_boolean("promptsListChanged", subscription.prompts);
    writer.member_boolean("resourcesListChanged", subscription.resources);
    writer.key("resourceSubscriptions");
    writer.begin_array();
    for (usize index = 0; index < subscription.uris.count(); ++index) writer.string(subscription.uris[index].view());
    writer.end_array();
    writer.end_object();
}

void handle_listen(ServerState& state, const Incoming& incoming) noexcept {
    const JsonDocument& document = *incoming.document;
    Subscription wanted;
    const Optional<JsonRef> filter =
        incoming.has_params ? document.member(incoming.params, "notifications") : Optional<JsonRef>::none();
    if (filter.present()) {
        const JsonRef object = *filter.value();
        if (!document.is(object, JsonKind::object)) {
            send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "notifications must be an object");
            return;
        }
        wanted.tools = document.member_boolean(object, "toolsListChanged").value_or(false);
        wanted.prompts = document.member_boolean(object, "promptsListChanged").value_or(false);
        wanted.resources = document.member_boolean(object, "resourcesListChanged").value_or(false);
        const Optional<JsonRef> uris = document.member(object, "resourceSubscriptions");
        if (uris.present()) {
            if (!document.is(*uris.value(), JsonKind::array) || document.child_count(*uris.value()) > subscribed_uri_limit) {
                send_rpc_error(state, incoming.id_json, RpcError::invalid_params,
                               "resourceSubscriptions must be a list of at most 64 URIs");
                return;
            }
            Optional<JsonRef> entry = document.first_child(*uris.value());
            while (entry.present()) {
                const Optional<TextView> uri = document.text(*entry.value());
                if (uri.absent()) {
                    send_rpc_error(state, incoming.id_json, RpcError::invalid_params,
                                   "resourceSubscriptions must be a list of URIs");
                    return;
                }
                Utf8String copy;
                if (copy.assign(*uri.value()).failed() || wanted.uris.push(static_cast<Utf8String&&>(copy)).failed()) {
                    send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
                    return;
                }
                entry = document.next_sibling(*entry.value());
            }
        }
    }
    if (wanted.id_json.assign(incoming.id_json).failed()) {
        send_rpc_error(state, incoming.id_json, RpcError::internal_error, "out of memory");
        return;
    }
    // The acknowledgement is queued under the lock, so no notification for
    // this subscription (queued under the same lock, on the notices
    // channel, which the writer drains only when replies is empty) can
    // precede it.
    bool duplicate = false;
    bool full = true;
    {
        ScopedLock lock(state.mutex);
        for (usize slot = 0; slot < state.in_flight.count(); ++slot) {
            const InFlight& entry = state.in_flight[slot];
            if (entry.used && entry.id_json.view() == incoming.id_json) duplicate = true;
        }
        for (usize index = 0; index < subscription_capacity; ++index) {
            const Subscription& open = state.subscriptions[index];
            if (open.used && open.id_json.view() == incoming.id_json) duplicate = true;
        }
        for (usize index = 0; index < subscription_capacity && full && !duplicate; ++index) {
            Subscription& open = state.subscriptions[index];
            if (open.used) continue;
            open = static_cast<Subscription&&>(wanted);
            open.used = true;
            full = false;
            Utf8String line;
            JsonWriter writer(line, state.options.message_limit);
            open_notification(writer, "notifications/subscriptions/acknowledged");
            writer.key("params");
            writer.begin_object();
            writer.key("_meta");
            writer.begin_object();
            writer.key(key_subscription);
            writer.raw(open.id_json.view());
            writer.end_object();
            writer.key("notifications");
            write_listen_filter(writer, open);
            writer.end_object();
            writer.end_object();
            if (writer.status().succeeded()) send_reply(state, line, false);
        }
    }
    if (duplicate) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_request, "the request id is in use");
    } else if (full) {
        send_rpc_error(state, incoming.id_json, RpcError::internal_error, "too many subscriptions are open");
    }
}

void send_empty_result(ServerState& state, const Incoming& incoming) noexcept {
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_result(writer, incoming.id_json);
    close_result(writer, state, incoming.modern);
    send_written(state, line, writer, incoming.id_json);
}

void handle_set_level(ServerState& state, const Incoming& incoming) noexcept {
    const i32 level = log_level_of(param_text(incoming, "level").value_or(TextView()));
    if (level < 0) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "an unknown log level");
        return;
    }
    {
        ScopedLock lock(state.mutex);
        state.legacy_log_threshold = level;
    }
    send_empty_result(state, incoming);
}

void handle_subscribe(ServerState& state, const Incoming& incoming, const bool subscribe) noexcept {
    const Optional<TextView> asked = param_text(incoming, "uri");
    if (asked.absent()) {
        send_rpc_error(state, incoming.id_json, RpcError::invalid_params, "the request needs a uri");
        return;
    }
    bool refused = false;
    {
        ScopedLock lock(state.mutex);
        Vector<Utf8String>& list = state.legacy_subscriptions;
        usize found = list.count();
        for (usize index = 0; index < list.count(); ++index) {
            if (list[index].view() == *asked.value()) found = index;
        }
        if (subscribe && found == list.count()) {
            Utf8String copy;
            refused = list.count() >= subscribed_uri_limit || copy.assign(*asked.value()).failed() ||
                      list.push(static_cast<Utf8String&&>(copy)).failed();
        } else if (!subscribe && found != list.count()) {
            const usize last = list.count() - 1;
            if (found != last) list[found] = static_cast<Utf8String&&>(list[last]);
            list.remove_last();
        }
    }
    if (refused) {
        send_rpc_error(state, incoming.id_json, RpcError::internal_error, "too many subscriptions");
        return;
    }
    send_empty_result(state, incoming);
}

void cancel_request(ServerState& state, const Utf8String& id_json) noexcept {
    bool cancelled = false;
    {
        ScopedLock lock(state.mutex);
        for (usize slot = 0; slot < state.in_flight.count(); ++slot) {
            InFlight& entry = state.in_flight[slot];
            if (!entry.used || entry.id_json.view() != id_json.view()) continue;
            (*entry.call).context.cancel(ContextEnd::cancelled);
            cancelled = true;
        }
        // A cancelled subscriptions/listen ends without a response.
        for (usize index = 0; index < subscription_capacity; ++index) {
            Subscription& open = state.subscriptions[index];
            if (!open.used || open.id_json.view() != id_json.view()) continue;
            open = Subscription();
            cancelled = true;
        }
    }
    if (cancelled) bump(state.counters.cancellations);
}

void handle_notification(ServerState& state, const JsonDocument& document, const JsonRef root,
                         const TextView method) noexcept {
    bump(state.counters.notifications_received);
    if (method != TextView("notifications/cancelled")) return; // initialized and others: nothing to do
    const Optional<JsonRef> params = document.member(root, "params");
    if (params.absent()) return;
    const Optional<JsonRef> id = document.member(*params.value(), "requestId");
    if (id.absent()) return;
    Utf8String id_json;
    if (!canonical_id(document, *id.value(), id_json)) return;
    cancel_request(state, id_json);
}

// The era and identity of a request; false (with the error sent) when it
// can be served under neither.
bool settle_era(ServerState& state, Incoming& incoming) noexcept {
    const JsonDocument& document = *incoming.document;
    const Optional<JsonRef> version =
        incoming.has_meta ? document.member(incoming.meta, key_version) : Optional<JsonRef>::none();
    if (version.present()) {
        const Optional<TextView> named = document.text(*version.value());
        if (named.absent()) {
            send_error(state, incoming.id_json, static_cast<i32>(RpcError::invalid_params),
                       "the protocol version must be a string", ErrorData::versions);
            return false;
        }
        if (*named.value() != modern_protocol_version) {
            send_error(state, incoming.id_json, static_cast<i32>(RpcError::unsupported_protocol_version),
                       "unsupported protocol version", ErrorData::versions, *named.value());
            return false;
        }
        const Optional<JsonRef> capabilities = document.member(incoming.meta, key_capabilities);
        if (capabilities.absent() || !document.is(*capabilities.value(), JsonKind::object)) {
            send_rpc_error(state, incoming.id_json, RpcError::invalid_params,
                           "the request's _meta needs io.modelcontextprotocol/clientCapabilities");
            return false;
        }
        incoming.modern = true;
        incoming.version = modern_protocol_version;
        const Optional<TextView> level = document.member_text(incoming.meta, key_log_level);
        incoming.log_threshold = level.present() ? log_level_of(*level.value()) : -1;
        const Optional<JsonRef> info = document.member(incoming.meta, key_client);
        Optional<TextView> model = Optional<TextView>::none();
        if (!state.model_meta_key.empty()) model = document.member_text(incoming.meta, state.model_meta_key.view());
        note_client(state, document, info, model);
        ScopedLock lock(state.mutex);
        incoming.client = state.client;
        return true;
    }
    {
        ScopedLock lock(state.mutex);
        incoming.version = state.legacy_version;
        incoming.log_threshold = state.legacy_log_threshold;
        incoming.client = state.client;
    }
    if (incoming.version.empty()) {
        send_error(state, incoming.id_json, static_cast<i32>(RpcError::invalid_params),
                   "the request names no protocol version (_meta io.modelcontextprotocol/protocolVersion) and no "
                   "initialize came first",
                   ErrorData::versions);
        return false;
    }
    incoming.modern = false;
    return true;
}

void handle_request(ServerState& state, JsonDocument& document, const JsonRef root, const TextView id_json,
                    const TextView method) noexcept {
    bump(state.counters.requests);
    Incoming incoming;
    incoming.document = &document;
    incoming.id_json = id_json;
    incoming.method = method;
    const Optional<JsonRef> params = document.member(root, "params");
    if (params.present()) {
        if (!document.is(*params.value(), JsonKind::object)) {
            send_rpc_error(state, id_json, RpcError::invalid_params, "params must be an object");
            return;
        }
        incoming.params = *params.value();
        incoming.has_params = true;
        const Optional<JsonRef> meta = document.member(incoming.params, "_meta");
        if (meta.present() && document.is(*meta.value(), JsonKind::object)) {
            incoming.meta = *meta.value();
            incoming.has_meta = true;
        }
    }
    if (method == TextView("initialize")) {
        call_started(state, RequestKind::listing, method);
        handle_initialize(state, incoming);
        call_finished(state, RequestKind::listing, method);
        return;
    }
    if (!settle_era(state, incoming)) return;
    // Methods handled here are listings; the rest go to the handler tasks,
    // which report their own start and finish.
    bool listing = true;
    if (method == TextView("server/discover")) {
        call_started(state, RequestKind::listing, method);
        handle_discover(state, incoming);
    } else if (method == TextView("tools/list")) {
        call_started(state, RequestKind::listing, method);
        handle_list(state, incoming, ListMethod::tools);
    } else if (method == TextView("resources/list")) {
        call_started(state, RequestKind::listing, method);
        handle_list(state, incoming, ListMethod::resources);
    } else if (method == TextView("resources/templates/list")) {
        call_started(state, RequestKind::listing, method);
        handle_list(state, incoming, ListMethod::templates);
    } else if (method == TextView("prompts/list")) {
        call_started(state, RequestKind::listing, method);
        handle_list(state, incoming, ListMethod::prompts);
    } else {
        listing = false;
        if (method == TextView("tools/call")) {
            handle_tool_call(state, incoming);
        } else if (method == TextView("resources/read")) {
            handle_read(state, incoming);
        } else if (method == TextView("prompts/get")) {
            handle_prompt(state, incoming);
        } else if (incoming.modern && method == TextView("subscriptions/listen")) {
            handle_listen(state, incoming);
        } else if (!incoming.modern && method == TextView("ping")) {
            send_empty_result(state, incoming);
        } else if (!incoming.modern && method == TextView("logging/setLevel")) {
            handle_set_level(state, incoming);
        } else if (!incoming.modern && method == TextView("resources/subscribe")) {
            handle_subscribe(state, incoming, true);
        } else if (!incoming.modern && method == TextView("resources/unsubscribe")) {
            handle_subscribe(state, incoming, false);
        } else {
            send_rpc_error(state, id_json, RpcError::method_not_found, "method not found");
        }
    }
    if (listing) call_finished(state, RequestKind::listing, method);
}

void handle_message(ServerState& state, InboxRecord& record) noexcept {
    if (record.oversized) {
        bump(state.counters.oversized_messages);
        send_rpc_error(state, TextView(), RpcError::invalid_request, "the message exceeds the message limit");
        return;
    }
    OperationResult<TextView> text = validate_utf8(record.bytes.begin(), record.bytes.count());
    if (text.failed()) {
        send_rpc_error(state, TextView(), RpcError::parse_error, "the message is not UTF-8");
        return;
    }
    OperationResult<JsonDocument> parsed = JsonDocument::parse(*text.value(), state.options.json_limits);
    if (parsed.failed()) {
        Utf8String message;
        static_cast<void>(message.assign("the message is not JSON: "));
        static_cast<void>(message.append((*parsed.error()).message()));
        send_rpc_error(state, TextView(), RpcError::parse_error, message.view());
        return;
    }
    JsonDocument document = static_cast<OperationResult<JsonDocument>&&>(parsed).take_value();
    const JsonRef root = document.root();
    if (!document.is(root, JsonKind::object)) {
        send_rpc_error(state, TextView(), RpcError::invalid_request, "a message is one JSON object (batches are not part of MCP)");
        return;
    }
    const Optional<JsonRef> id = document.member(root, "id");
    Utf8String id_json;
    if (id.present() && !canonical_id(document, *id.value(), id_json)) {
        send_rpc_error(state, TextView(), RpcError::invalid_request, "a request id is a string or an integer");
        return;
    }
    if (document.member_text(root, "jsonrpc").value_or(TextView()) != TextView("2.0")) {
        send_rpc_error(state, id_json.view(), RpcError::invalid_request, "the message is not JSON-RPC 2.0");
        return;
    }
    const Optional<TextView> method = document.member_text(root, "method");
    if (method.absent()) {
        // A response: this server sends no requests, so there is nothing
        // to match it to.
        if (document.member(root, "result").present() || document.member(root, "error").present()) return;
        send_rpc_error(state, id_json.view(), RpcError::invalid_request, "the message has no method");
        return;
    }
    // The method's text lives in the document, which a queued call takes:
    // keep a copy for the events after the move.
    Utf8String method_copy;
    if (method_copy.assign(*method.value()).failed()) {
        if (id.present()) send_rpc_error(state, id_json.view(), RpcError::internal_error, "out of memory");
        return;
    }
    if (id.absent()) {
        handle_notification(state, document, root, method_copy.view());
        return;
    }
    handle_request(state, document, root, id_json.view(), method_copy.view());
}

// ---- results of queued calls --------------------------------------------------------------

void finish_tool(ServerState& state, const CallRecord& call, ReplyRecord& reply, JsonWriter& writer,
                 bool& written) noexcept {
    const ToolBinding& tool = *call.tool;
    Utf8String internal;
    JsonDocument structured;
    bool has_structured = false;
    if (reply.trouble.present()) {
        static_cast<void>(internal.assign("the reply could not be built: "));
        static_cast<void>(internal.append((*reply.trouble.value()).message()));
    } else if (!reply.is_error && reply.structured_started) {
        if (reply.structured_writer.status().failed()) {
            static_cast<void>(internal.assign("the structured result is not one whole JSON value"));
            if (tool.has_output) bump(state.counters.output_schema_failures);
        } else {
            OperationResult<JsonDocument> parsed = JsonDocument::parse(reply.structured.view(), state.options.json_limits);
            if (parsed.failed()) {
                static_cast<void>(internal.assign("the structured result could not be read back"));
            } else {
                structured = static_cast<OperationResult<JsonDocument>&&>(parsed).take_value();
                Utf8String problem;
                if (tool.has_output && tool.output.validate(structured, structured.root(), problem).failed()) {
                    bump(state.counters.output_schema_failures);
                    static_cast<void>(internal.assign("the structured result does not match the tool's output schema: "));
                    static_cast<void>(internal.append(problem.view()));
                } else {
                    has_structured = true;
                }
            }
        }
    } else if (!reply.is_error && tool.has_output) {
        bump(state.counters.output_schema_failures);
        static_cast<void>(internal.assign("the tool declares an output schema and returned no structured result"));
    }
    const bool failed_inside = !internal.empty();
    bool fits = true;
    open_result(writer, call.id_json.view());
    writer.key("content");
    writer.begin_array();
    if (failed_inside) {
        write_error_text_item(writer, "internal_failure", internal.view(), "", state.options.message_limit, fits);
    } else {
        if (reply.is_error) writer.raw(reply.error_item.view());
        for (usize index = 0; index < reply.items.count(); ++index) writer.raw(reply.items[index].view());
        if (has_structured && !reply.has_text_item && !reply.is_error) {
            writer.begin_object();
            writer.member_string("type", "text");
            writer.member_string("text", reply.structured.view());
            writer.end_object();
        }
    }
    writer.end_array();
    if (has_structured) {
        writer.key("structuredContent");
        writer.copy(structured, structured.root());
    }
    if (failed_inside || reply.is_error) writer.member_boolean("isError", true);
    close_result(writer, state, call.modern);
    written = writer.status().succeeded() && fits;
}

void finish_resource(ServerState& state, const CallRecord& call, ReplyRecord& reply, JsonWriter& writer) noexcept {
    open_result(writer, call.id_json.view());
    writer.key("contents");
    writer.begin_array();
    for (usize index = 0; index < reply.items.count(); ++index) writer.raw(reply.items[index].view());
    writer.end_array();
    write_cache_hints(writer, call.modern, state.options.read_ttl_milliseconds);
    close_result(writer, state, call.modern);
}

void finish_prompt(ServerState& state, const CallRecord& call, ReplyRecord& reply, JsonWriter& writer) noexcept {
    const PromptBinding& prompt = *call.prompt;
    open_result(writer, call.id_json.view());
    const TextView description = reply.has_description ? reply.description.view() : prompt.description.view();
    if (!description.empty()) writer.member_string("description", description);
    writer.key("messages");
    writer.begin_array();
    for (usize index = 0; index < reply.items.count(); ++index) writer.raw(reply.items[index].view());
    writer.end_array();
    close_result(writer, state, call.modern);
}

// Sends the response to `call`, or the error its reply asked for.
void send_call_reply(ServerState& state, const CallRecord& call, ReplyRecord& reply) noexcept {
    const TextView id_json = call.id_json.view();
    if (call.kind != RequestKind::tool_call) {
        if (reply.outcome == ReplyOutcome::not_found) {
            const i32 code = call.modern ? static_cast<i32>(RpcError::invalid_params) : legacy_resource_not_found;
            send_error(state, id_json, code, "resource not found", ErrorData::uri, call.name.view());
            return;
        }
        if (reply.outcome == ReplyOutcome::failed) {
            send_rpc_error(state, id_json, RpcError::internal_error,
                           reply.failure_message.empty() ? TextView("the request failed") : reply.failure_message.view());
            return;
        }
        if (reply.trouble.present()) {
            send_rpc_error(state, id_json, RpcError::internal_error, (*reply.trouble.value()).message());
            return;
        }
    }
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    bool written = true;
    if (call.kind == RequestKind::tool_call) {
        finish_tool(state, call, reply, writer, written);
        if (!written) {
            send_tool_error(state, id_json, call.modern, "internal_failure", "the result does not fit the message limit",
                            "ask for less at once");
            return;
        }
    } else if (call.kind == RequestKind::resource_read) {
        finish_resource(state, call, reply, writer);
    } else {
        finish_prompt(state, call, reply, writer);
    }
    send_written(state, line, writer, id_json);
}

// Writes a notification with `_meta` naming a subscription (or none: a
// legacy client) and, for resources/updated, the URI.
void notify_one(ServerState& state, const TextView method, const TextView subscription_id, const TextView uri) noexcept {
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_notification(writer, method);
    writer.key("params");
    writer.begin_object();
    if (!subscription_id.empty()) {
        writer.key("_meta");
        writer.begin_object();
        writer.key(key_subscription);
        writer.raw(subscription_id);
        writer.end_object();
    }
    if (!uri.empty()) writer.member_string("uri", uri);
    writer.end_object();
    writer.end_object();
    if (writer.status().succeeded()) send_notice(state, line);
}

} // namespace

// ---- the dispatcher ------------------------------------------------------------------------

void dispatch_entry(const StopSignal& stop, void* const context) noexcept {
    static_cast<void>(stop);
    ServerState& state = *static_cast<ServerState*>(context);
    ReaderLink& link = *state.link;
    InboxRecord record;
    for (;;) {
        const ChannelStatus status = link.inbox.receive(record, state.serving);
        if (status == ChannelStatus::ended) break;
        if (status == ChannelStatus::closed) {
            const u8 end = __atomic_load_n(&link.end, __ATOMIC_ACQUIRE);
            note_end(state, end == static_cast<u8>(InputEnd::failed) ? ServeEnd::input_failed : ServeEnd::input_ended);
            if (state.events.input_ended != nullptr) state.events.input_ended(state.events.context);
            break;
        }
        bump(state.counters.messages_read);
        handle_message(state, record);
        record.bytes.clear();
        record.oversized = false;
    }
}

// ---- queued calls ------------------------------------------------------------------------------

void ServerAccess::call_tool(const ToolBinding& tool, const CallRecord& call, ReplyRecord& reply) noexcept {
    const ToolRequest request(call);
    ToolReply out(reply);
    tool.handler(tool.binding, request, out);
}

void ServerAccess::call_resource(const ResourceBinding& resource, const CallRecord& call, ReplyRecord& reply) noexcept {
    const ResourceRequest request(call);
    ResourceReply out(reply);
    resource.handler(resource.binding, request, out);
}

void ServerAccess::call_prompt(const PromptBinding& prompt, const CallRecord& call, ReplyRecord& reply) noexcept {
    const PromptRequest request(call);
    PromptReply out(reply);
    prompt.handler(prompt.binding, request, out);
}

void execute_call(ServerState& state, CallRecord& call) noexcept {
    if (call.context.done()) {
        bump(state.counters.replies_discarded);
        return;
    }
    const TextView name = call.name.view();
    call_started(state, call.kind, name);
    ReplyRecord reply(state.options.message_limit);
    reply.default_uri = name;
    if (call.kind == RequestKind::tool_call) {
        ServerAccess::call_tool(*call.tool, call, reply);
    } else if (call.kind == RequestKind::resource_read) {
        ServerAccess::call_resource(*call.resource, call, reply);
    } else {
        ServerAccess::call_prompt(*call.prompt, call, reply);
    }
    if (call.context.done()) {
        bump(state.counters.replies_discarded);
    } else {
        send_call_reply(state, call, reply);
    }
    call_finished(state, call.kind, name);
}

void close_subscriptions(ServerState& state) noexcept {
    ScopedLock lock(state.mutex);
    for (usize index = 0; index < subscription_capacity; ++index) {
        Subscription& open = state.subscriptions[index];
        if (!open.used) continue;
        Utf8String line;
        JsonWriter writer(line, state.options.message_limit);
        open_result(writer, open.id_json.view());
        writer.member_string("resultType", "complete");
        writer.key("_meta");
        writer.begin_object();
        writer.key(key_subscription);
        writer.raw(open.id_json.view());
        writer.key(key_server);
        write_server_info(writer, state);
        writer.end_object();
        writer.end_object();
        writer.end_object();
        if (writer.status().succeeded()) send_reply(state, line, true);
        open = Subscription();
    }
}

void broadcast_list_changed(ServerState& state, const ListKind kind) noexcept {
    const TextView method = kind == ListKind::tools     ? TextView("notifications/tools/list_changed")
                            : kind == ListKind::prompts ? TextView("notifications/prompts/list_changed")
                                                        : TextView("notifications/resources/list_changed");
    ScopedLock lock(state.mutex);
    for (usize index = 0; index < subscription_capacity; ++index) {
        const Subscription& open = state.subscriptions[index];
        if (!open.used) continue;
        const bool wanted = kind == ListKind::tools ? open.tools : kind == ListKind::prompts ? open.prompts : open.resources;
        if (wanted) notify_one(state, method, open.id_json.view(), TextView());
    }
    if (!state.legacy_version.empty()) notify_one(state, method, TextView(), TextView());
}

void broadcast_resource_updated(ServerState& state, const TextView uri) noexcept {
    if (uri.empty()) return;
    ScopedLock lock(state.mutex);
    for (usize index = 0; index < subscription_capacity; ++index) {
        const Subscription& open = state.subscriptions[index];
        if (!open.used) continue;
        for (usize position = 0; position < open.uris.count(); ++position) {
            if (open.uris[position].view() != uri) continue;
            notify_one(state, "notifications/resources/updated", open.id_json.view(), uri);
            break;
        }
    }
    if (state.legacy_version.empty()) return;
    for (usize index = 0; index < state.legacy_subscriptions.count(); ++index) {
        if (state.legacy_subscriptions[index].view() != uri) continue;
        notify_one(state, "notifications/resources/updated", TextView(), uri);
        break;
    }
}

void broadcast_log(ServerState& state, const LogLevel level, const TextView text) noexcept {
    ScopedLock lock(state.mutex);
    if (state.legacy_version.empty() || state.legacy_log_threshold < 0) return;
    if (static_cast<i32>(level) < state.legacy_log_threshold) return;
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_notification(writer, "notifications/message");
    writer.key("params");
    writer.begin_object();
    writer.member_string("level", log_level_names[static_cast<usize>(level)]);
    writer.member_string("logger", state.name.view());
    writer.member_string("data", text);
    writer.end_object();
    writer.end_object();
    if (writer.status().succeeded()) send_notice(state, line);
}

void write_progress(const CallRecord& call, const double progress, const double total, const TextView message) noexcept {
    ServerState& state = *call.server;
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_notification(writer, "notifications/progress");
    writer.key("params");
    writer.begin_object();
    writer.key("progressToken");
    writer.raw(call.progress_token_json.view());
    writer.member_number("progress", progress);
    if (total > 0.0) writer.member_number("total", total);
    if (!message.empty()) writer.member_string("message", message);
    writer.end_object();
    writer.end_object();
    if (writer.status().succeeded()) send_reply(state, line, false);
}

void write_request_log(const CallRecord& call, const LogLevel level, const TextView text) noexcept {
    ServerState& state = *call.server;
    Utf8String line;
    JsonWriter writer(line, state.options.message_limit);
    open_notification(writer, "notifications/message");
    writer.key("params");
    writer.begin_object();
    writer.member_string("level", log_level_names[static_cast<usize>(level)]);
    writer.member_string("logger", state.name.view());
    writer.member_string("data", text);
    writer.end_object();
    writer.end_object();
    if (writer.status().succeeded()) send_reply(state, line, false);
}

} // namespace stxlib::mcp::detail
