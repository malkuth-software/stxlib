#pragma once

// stx_mcp: a Model Context Protocol server over anonymous pipes (target
// stxlib::mcp). The design is notes planning/studies/MCP_AUTOMATION_STUDY.md
// (public notes, gui_forms branch), sections 1, 4, 6, 7, 16 (stage X1) and 17;
// rpclib's shape is followed and the code written fresh (17.6).
//
// What it is:
// - JSON-RPC 2.0, one message per line (newline-delimited, no embedded line
//   breaks), over a PipeReader and a PipeWriter (pipes.hpp): the stdio
//   transport. Nothing else: no sockets, no HTTP, no attaching (ruling 1.8).
// - A dispatcher in rpclib's manner: bind_tool(ToolSpec, handler, binding).
//   The spec declares the tool's name, text, input schema, optional output
//   schema and annotations. Arguments are validated against the schema
//   before the handler runs; a structured result is validated against the
//   output schema before it is sent (study 13.2: the mistake in Xcode's
//   bridge). Handlers are named functions with a binding pointer, never
//   lambdas.
// - MCP's tools, resources (and templates), prompts, progress, cancellation
//   and change notifications.
// - Tasks and channels (channels.hpp): a reader task, a pool of handler
//   tasks and a writer task, joined by bounded channels; closing them is how
//   shutdown spreads.
// - Failures as results: a malformed message is a JSON-RPC error and the
//   stream goes on; a tool's failure is a tool result marked isError with a
//   named code and a hint (study 7.5); nothing throws.
//
// Protocol revisions (dual-era, as the specification's versioning page
// allows). The modern revision is 2026-07-28: stateless, with the protocol
// version, client capabilities and client identity in every request's
// `_meta` (io.modelcontextprotocol/protocolVersion, .../clientCapabilities,
// .../clientInfo), `server/discover`, `subscriptions/listen` for change
// notifications, `resultType` in every result, caching hints (ttlMs,
// cacheScope) on discover, lists and reads, and resource-not-found as
// -32602. The legacy revisions (2025-11-25, 2025-06-18, 2025-03-26) open
// with `initialize`; a server that receives one serves that process's
// requests under the negotiated legacy revision (ping, logging/setLevel,
// resources/subscribe and unsubscribe, list_changed sent to the stream).
// A request without the modern `_meta` and without a prior `initialize` is
// refused with -32602, naming the supported versions.
//
// Threads. bind_* and unbind_* may be called at any time from any thread;
// a call that is running keeps the binding it started with (bindings are
// shared records). Handlers run on the handler tasks, several at once
// (ServerOptions::handler_tasks), each call on one task from start to end;
// a handler that needs a GUI thread sends it work and waits on the
// request's context (study 6.2, "one owner per state"). Notifications
// (notify_*) are safe from any thread. ServerEvents callbacks are called on
// the reader or handler tasks with no lock of the server held, and must
// return promptly; they may call notify_*.
//
// Strings in specs are copied at bind: a spec's TextViews need only last
// for the call.
//
// Early exits: handlers and event callbacks are consumer code. The server
// calls them with no lock held and every invariant whole (form 1), and
// each must return: no longjmp out, no thread exit inside.

#include "stxlib/channels.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/json.hpp"
#include "stxlib/optional.hpp"
#include "stxlib/pipes.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::mcp {

// ---- protocol constants ------------------------------------------------------

// The modern revision this server implements.
inline constexpr TextView modern_protocol_version = "2026-07-28";
// The legacy revisions an `initialize` may negotiate, newest first. A client
// asking for another gets the newest.
inline constexpr usize legacy_protocol_version_count = 3;
[[nodiscard]] TextView legacy_protocol_version(usize position) noexcept;

// JSON-RPC and MCP error codes this server sends.
enum class RpcError : i32 {
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602, // also: unknown tool, unknown prompt, resource not found (2026-07-28)
    internal_error = -32603,
    unsupported_protocol_version = -32022,
};

// ---- identities ----------------------------------------------------------------

// The program's MCP identity (study 4.3): `name` is app.<vendor>.<appname>
// by default ("app.malkuthsoft.planpaint"), and is the serverInfo name.
struct ServerIdentity final {
    TextView name{};
    TextView version{};
    // For display ("Plan Paint"); empty: none sent.
    TextView title{};
    // Guidance for the model on using this program; empty: none sent.
    TextView instructions{};
};

// Bytes each part of a ClientIdentity keeps; longer text is cut at a whole
// UTF-8 sequence.
inline constexpr usize client_text_capacity = 128;

// What a client states about itself (not verified: study 4.4): from
// clientInfo in `_meta` (modern) or in `initialize` (legacy), and the model
// it names, when ServerOptions::model_meta_key names a `_meta` key it uses.
// A fixed-size value, copied freely.
class ClientIdentity final {
public:
    ClientIdentity() noexcept = default;

    // Copies each text, cut at client_text_capacity at a whole sequence.
    [[nodiscard]] static ClientIdentity of(TextView name, TextView version, TextView title, TextView model) noexcept;

    [[nodiscard]] TextView name() const& noexcept;
    [[nodiscard]] TextView version() const& noexcept;
    [[nodiscard]] TextView title() const& noexcept;
    [[nodiscard]] TextView model() const& noexcept;
    TextView name() const&& = delete;
    TextView version() const&& = delete;
    TextView title() const&& = delete;
    TextView model() const&& = delete;
    [[nodiscard]] bool empty() const noexcept { return name_count_ == 0; }

    [[nodiscard]] bool operator==(const ClientIdentity& other) const noexcept;
    [[nodiscard]] bool operator!=(const ClientIdentity& other) const noexcept;

private:
    char name_[client_text_capacity]{};
    char version_[client_text_capacity]{};
    char title_[client_text_capacity]{};
    char model_[client_text_capacity]{};
    u8 name_count_{0};
    u8 version_count_{0};
    u8 title_count_{0};
    u8 model_count_{0};
};

static_assert(client_text_capacity <= 255, "ClientIdentity's counts are bytes");

// ---- tools ---------------------------------------------------------------------

// MCP's tool annotations, always sent explicitly (MCP's own defaults for an
// absent destructiveHint and openWorldHint are true; this program's are
// false, rulings 1.16 to 1.18: every act is non-destructive unless marked).
struct ToolAnnotations final {
    bool read_only{false};   // readOnlyHint
    bool destructive{false}; // destructiveHint (meaningful when not read-only)
    bool idempotent{false};  // idempotentHint
    bool open_world{false};  // openWorldHint: reaches beyond this program
};

// A tool's declaration. The schemas are JSON text of the supported subset
// of JSON Schema 2020-12 (below); they are parsed and checked at bind.
struct ToolSpec final {
    // 1 to 128 of A-Z, a-z, 0-9, '_', '-', '.' (the specification's rule,
    // enforced at bind).
    TextView name{};
    TextView title{};
    TextView description{};
    // Empty: {"type":"object","additionalProperties":false} (no arguments).
    TextView input_schema{};
    // Empty: no output schema, and no structured result is checked.
    TextView output_schema{};
    ToolAnnotations annotations{};
};

// The supported JSON Schema subset (validation; annotations such as title,
// description, default, examples, format and $schema are accepted and not
// enforced): type (one name or a list), properties, required,
// additionalProperties (a boolean or a schema), items (one schema), enum,
// const, minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength,
// maxLength (counted in Unicode scalar values), minItems, maxItems,
// minProperties, maxProperties. Any other validation keyword ($ref, anyOf,
// oneOf, allOf, not, if, pattern, patternProperties, prefixItems,
// uniqueItems, dependentRequired, ...) is refused at bind with unsupported,
// naming it, so a schema is never silently treated as permissive (the
// specification's rule for unresolved $ref). A $schema other than 2020-12's
// is refused the same way. Schemas nest at most 32 levels.
inline constexpr u32 schema_depth_limit = 32;

class ToolRequest;
class ToolReply;

// A tool's handler: `binding` is the pointer given at bind; `request` holds
// the validated arguments and the call's context; `reply` collects the
// result. Called on a handler task. A handler that leaves `reply` untouched
// replies with no content and no error.
using ToolHandler = void (*)(void* binding, const ToolRequest& request, ToolReply& reply) noexcept;

// ---- resources -------------------------------------------------------------------

struct ResourceSpec final {
    TextView uri{};
    TextView name{};
    TextView title{};
    TextView description{};
    TextView mime_type{};
};

// A family of resources named by an RFC 6570 level 1 template:
// "gui://window/{window}/outline". Each {name} matches one or more
// characters other than '/', and the matched text is the parameter.
struct ResourceTemplateSpec final {
    TextView uri_template{};
    TextView name{};
    TextView title{};
    TextView description{};
    TextView mime_type{};
};

class ResourceRequest;
class ResourceReply;

using ResourceHandler = void (*)(void* binding, const ResourceRequest& request, ResourceReply& reply) noexcept;

// ---- prompts ---------------------------------------------------------------------

struct PromptArgument final {
    TextView name{};
    TextView description{};
    bool required{false};
};

struct PromptSpec final {
    TextView name{};
    TextView title{};
    TextView description{};
    // Read during bind only.
    View<PromptArgument> arguments{};
};

class PromptRequest;
class PromptReply;

using PromptHandler = void (*)(void* binding, const PromptRequest& request, PromptReply& reply) noexcept;

// ---- requests and replies ----------------------------------------------------------

// MCP's log levels (RFC 5424's), least severe first.
enum class LogLevel : u8 {
    debug = 0,
    info = 1,
    notice = 2,
    warning = 3,
    error = 4,
    critical = 5,
    alert = 6,
    emergency = 7,
};

namespace detail {
struct CallRecord;
struct ReplyRecord;
class ServerAccess;
} // namespace detail

// What every request offers its handler. Borrowed for the handler's call
// only: nothing returned here may be kept after the handler returns.
class RequestView {
public:
    RequestView(const RequestView&) = delete;
    RequestView& operator=(const RequestView&) = delete;

    // The request's context: cancelled when the client sends
    // notifications/cancelled for it, shut_down when the server stops or
    // its input ends. A handler that waits (for a GUI thread, say) waits on
    // it, and stops early when it ends; whatever it replies then is
    // discarded (the specification: no further messages for a cancelled
    // request).
    [[nodiscard]] const Context& context() const noexcept;
    // Who the client says it is, as of this request.
    [[nodiscard]] const ClientIdentity& client() const noexcept;
    // "2026-07-28", or the negotiated legacy revision.
    [[nodiscard]] TextView protocol_version() const noexcept;

    // Reports progress when the client asked for it (a progressToken in the
    // request's `_meta`); otherwise does nothing. `progress` must increase
    // from call to call: a value not above the last one sent is dropped, as
    // is a call after the context ended. `total` <= 0 means unknown.
    void progress(double progress, double total, TextView message) const noexcept;
    // Sends a notifications/message tied to this request when the client
    // asked for messages at `level` or above (modern: the request's
    // io.modelcontextprotocol/logLevel; legacy: logging/setLevel);
    // otherwise does nothing.
    void log(LogLevel level, TextView text) const noexcept;

protected:
    explicit RequestView(const detail::CallRecord& record) noexcept : record_(record) {}
    ~RequestView() = default;

    const detail::CallRecord& record_;
};

class ToolRequest final : public RequestView {
public:
    [[nodiscard]] TextView tool_name() const noexcept;
    // The validated arguments: an object, borrowed for the call.
    [[nodiscard]] const JsonDocument& document() const noexcept;
    [[nodiscard]] JsonRef arguments() const noexcept;
    // Named members of the arguments, or none when absent (the schema has
    // already checked the kinds it declares).
    [[nodiscard]] Optional<TextView> text(TextView name) const noexcept;
    [[nodiscard]] Optional<i64> integer(TextView name) const noexcept;
    [[nodiscard]] Optional<double> number(TextView name) const noexcept;
    [[nodiscard]] Optional<bool> boolean(TextView name) const noexcept;
    [[nodiscard]] Optional<JsonRef> member(TextView name) const noexcept;

private:
    friend class detail::ServerAccess;
    explicit ToolRequest(const detail::CallRecord& record) noexcept : RequestView(record) {}
};

// A tool's result under construction. Content items appear in the order
// added. The first allocation failure is kept: the reply then becomes a
// tool error with code internal_failure.
class ToolReply final {
public:
    ToolReply(const ToolReply&) = delete;
    ToolReply& operator=(const ToolReply&) = delete;

    // A text content item.
    void text(TextView content) noexcept;
    // An image content item: `bytes` (a PNG, say) sent as base64 with its
    // MIME type ("image/png").
    void image(View<Byte> bytes, TextView mime_type) noexcept;
    // A resource_link content item.
    void resource_link(TextView uri, TextView name, TextView mime_type) noexcept;
    // An embedded text resource content item.
    void embedded_text(TextView uri, TextView mime_type, TextView content) noexcept;

    // The structured result (structuredContent): a writer positioned to
    // write exactly one JSON value, which the server checks against the
    // tool's output schema after the handler returns. Calling it again
    // discards what was written and starts over. When no text item was
    // added, the server also sends the structured value as a text item (the
    // specification's backward-compatible form).
    [[nodiscard]] JsonWriter& structured() noexcept;

    // Marks the reply as a tool execution error (isError) with a named code
    // ("not_found", "dialog_open": study 7.5), what happened, and what to do
    // next (empty: none). The error's text item comes first; content added
    // before or after is kept after it; a structured result is dropped (an
    // error is not held to the output schema).
    void fail(TextView code, TextView message, TextView hint) noexcept;

private:
    friend class detail::ServerAccess;
    explicit ToolReply(detail::ReplyRecord& record) noexcept : record_(record) {}

    detail::ReplyRecord& record_;
};

class ResourceRequest final : public RequestView {
public:
    // The URI asked for.
    [[nodiscard]] TextView uri() const noexcept;
    // A template's matched parameter, or none (also for a plain resource).
    [[nodiscard]] Optional<TextView> parameter(TextView name) const noexcept;

private:
    friend class detail::ServerAccess;
    explicit ResourceRequest(const detail::CallRecord& record) noexcept : RequestView(record) {}
};

class ResourceReply final {
public:
    ResourceReply(const ResourceReply&) = delete;
    ResourceReply& operator=(const ResourceReply&) = delete;

    // One contents item of text, for `uri` (empty: the URI asked for).
    void text(TextView uri, TextView mime_type, TextView content) noexcept;
    // One contents item of binary data, sent as base64.
    void blob(TextView uri, TextView mime_type, View<Byte> bytes) noexcept;
    // The resource does not exist: the request answers -32602 with the URI
    // in its data (2026-07-28; legacy revisions get -32002, theirs). Content
    // added is discarded.
    void not_found() noexcept;
    // Reading failed: the request answers -32603 with `message`.
    void fail(TextView message) noexcept;

private:
    friend class detail::ServerAccess;
    explicit ResourceReply(detail::ReplyRecord& record) noexcept : record_(record) {}

    detail::ReplyRecord& record_;
};

class PromptRequest final : public RequestView {
public:
    [[nodiscard]] TextView prompt_name() const noexcept;
    // An argument the client gave, or none. Required arguments are checked
    // before the handler runs.
    [[nodiscard]] Optional<TextView> argument(TextView name) const noexcept;

private:
    friend class detail::ServerAccess;
    explicit PromptRequest(const detail::CallRecord& record) noexcept : RequestView(record) {}
};

class PromptReply final {
public:
    PromptReply(const PromptReply&) = delete;
    PromptReply& operator=(const PromptReply&) = delete;

    // The result's description (empty: the prompt's own).
    void description(TextView text) noexcept;
    // A message of text from the user or the assistant.
    void user_text(TextView text) noexcept;
    void assistant_text(TextView text) noexcept;
    // A user message embedding a text resource.
    void user_resource(TextView uri, TextView mime_type, TextView content) noexcept;
    // The prompt cannot be produced: -32603 with `message`.
    void fail(TextView message) noexcept;

private:
    friend class detail::ServerAccess;
    explicit PromptReply(detail::ReplyRecord& record) noexcept : record_(record) {}

    detail::ReplyRecord& record_;
};

// ---- the server ---------------------------------------------------------------------

struct ServerOptions final {
    // Handler tasks: how many tool calls, reads and prompts run at once.
    // 1 to task_group_capacity - 2 (the reader and writer are tasks too).
    usize handler_tasks{4};
    // Bytes of one message; a longer line is refused (-32600 with null id)
    // and skipped to its end, and the stream goes on. Also the bound on a
    // reply: a reply that would pass it is replaced by an internal error.
    usize message_limit{4 * 1024 * 1024};
    // Requests in flight at once; another is refused with -32603 until one
    // finishes.
    usize request_limit{64};
    // Caching hints sent on discover and the lists (ttlMs), and on reads.
    // 0: immediately stale (the default: a GUI program's state changes).
    u64 list_ttl_milliseconds{0};
    u64 read_ttl_milliseconds{0};
    // A `_meta` key in which a client names its model (study 4.4), read
    // into ClientIdentity::model; empty: none read. The specification
    // defines no such key, so a client and a program agree on one.
    TextView model_meta_key{};
    // The JSON reader's limits for each message (max_bytes is raised to
    // message_limit).
    JsonLimits json_limits{};
};

// Why serving ended.
enum class ServeEnd : u8 {
    input_ended = 0,   // the client closed the input (the specification's shutdown signal)
    stopped = 1,       // stop() was called
    output_failed = 2, // writing to the client failed (it went away)
    input_failed = 3,  // reading the input failed
};

// What a request is, for ServerEvents::request_started: the program's
// visible signs (study 11.1) glint while a model reads or acts.
enum class RequestKind : u8 {
    tool_call = 0,
    resource_read = 1,
    prompt_get = 2,
    listing = 3, // discover, the lists, initialize, ping
};

// Callbacks into the program, all optional (null entries are skipped).
// `context` is passed back to each.
struct ServerEvents final {
    void* context{nullptr};
    // A client named itself or changed its name (initialize, or a request's
    // clientInfo or model that differs from the last one seen).
    void (*client_seen)(void* context, const ClientIdentity& client) noexcept {nullptr};
    // The input ended or failed: the program should shut down as on logout
    // (study 4.1: "the program receives the portable shutdown").
    void (*input_ended)(void* context) noexcept {nullptr};
    // A request began or finished. `name` is the tool's, prompt's or the
    // URI, or the method for a listing; borrowed for the call.
    void (*request_started)(void* context, RequestKind kind, TextView name) noexcept {nullptr};
    void (*request_finished)(void* context, RequestKind kind, TextView name) noexcept {nullptr};
};

// Counters, for tests and diagnostics (a snapshot).
struct ServerCounters final {
    u64 messages_read{0};
    u64 requests{0};
    u64 notifications_received{0};
    u64 parse_errors{0};
    u64 invalid_requests{0};
    u64 oversized_messages{0};
    u64 replies_sent{0};
    u64 replies_discarded{0};      // cancelled requests' replies, not sent
    u64 notifications_sent{0};
    u64 notifications_dropped{0};  // dropped oldest from a full notification channel
    u64 argument_failures{0};      // tool arguments that failed the input schema
    u64 output_schema_failures{0}; // structured results that failed the output schema
    u64 cancellations{0};
};

namespace detail {
struct ServerState;
} // namespace detail

// One MCP server over one pair of pipes. Move-only; destruction stops it
// and waits (stop then wait).
class Server final {
public:
    // A server with no bindings. Failures: invalid_argument for an empty
    // identity name or out-of-range options; out_of_memory.
    [[nodiscard]] static OperationResult<Server> create(const ServerIdentity& identity, const ServerOptions& options,
                                                        const ServerEvents& events) noexcept;

    Server() noexcept = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
    // Stops and waits for this server first.
    Server& operator=(Server&& other) noexcept;

    // ---- bindings (any thread, any time) ----

    // Binds a tool. Failures: invalid_argument for a bad name or a schema
    // that is not JSON or not a schema (the message names the place);
    // unsupported for a schema keyword outside the subset; already_exists
    // for a name bound already; out_of_memory. Clients that opened
    // subscriptions for it (or a legacy client) get list_changed.
    [[nodiscard]] OperationStatus bind_tool(const ToolSpec& spec, ToolHandler handler, void* binding) noexcept;
    // not_found when no such tool is bound.
    [[nodiscard]] OperationStatus unbind_tool(TextView name) noexcept;

    [[nodiscard]] OperationStatus bind_resource(const ResourceSpec& spec, ResourceHandler handler,
                                                void* binding) noexcept;
    [[nodiscard]] OperationStatus bind_resource_template(const ResourceTemplateSpec& spec, ResourceHandler handler,
                                                         void* binding) noexcept;
    // By URI, or by template text.
    [[nodiscard]] OperationStatus unbind_resource(TextView uri_or_template) noexcept;

    [[nodiscard]] OperationStatus bind_prompt(const PromptSpec& spec, PromptHandler handler, void* binding) noexcept;
    [[nodiscard]] OperationStatus unbind_prompt(TextView name) noexcept;

    // ---- serving ----

    // Starts the reader, handler and writer tasks on `input` and `output`
    // (taken), and returns. Failures: invalid_argument with
    // HostCode::invalid_state when started already; a closed end; the
    // tasks' start failures (nothing is left running then).
    [[nodiscard]] OperationStatus start(PipeReader&& input, PipeWriter&& output) noexcept;

    // Blocks until serving ends (the input ends or fails, the output fails,
    // or stop()), then shuts down in order: in-flight requests' contexts end
    // (shut_down), the handler tasks finish their calls and are joined,
    // open subscriptions get their closing response, the writer sends what
    // is queued and closes the output. A reader still blocked on an input
    // that has not ended is abandoned (it owns its own state and ends with
    // the input or the process). Returns why serving ended; a second call
    // returns the same at once. Not started: stopped.
    [[nodiscard]] ServeEnd wait() noexcept;

    // Asks serving to end; wait() does the rest. Any thread; any time.
    void stop() noexcept;

    // ---- notifications (any thread) ----

    // The tool, prompt or resource list changed for reasons of the
    // program's own (a command's enabled state, say); binds and unbinds
    // send these by themselves.
    void notify_tools_changed() noexcept;
    void notify_prompts_changed() noexcept;
    void notify_resources_changed() noexcept;
    // A resource's content changed: sent to subscriptions that list `uri`
    // (modern) and to a legacy client that subscribed to it.
    void notify_resource_updated(TextView uri) noexcept;
    // A legacy-revision log message (logging/setLevel); modern clients get
    // messages only through a request (RequestView::log).
    void log(LogLevel level, TextView text) noexcept;

    // ---- queries ----

    // The last client identity seen (empty before any).
    [[nodiscard]] ClientIdentity client() const noexcept;
    [[nodiscard]] ServerCounters counters() const noexcept;
    [[nodiscard]] bool started() const noexcept;

private:
    explicit Server(detail::ServerState* const state) noexcept : state_(state) {}

    detail::ServerState* state_{nullptr};
};

} // namespace stxlib::mcp
