#pragma once

// The state behind stxlib::mcp::Server (include/stxlib/mcp.hpp), shared by
// server.cpp (lifecycle, tasks, bindings, the request and reply classes)
// and protocol.cpp (messages, methods, results). Private to the
// stxlib::mcp target.
//
// Tasks and channels:
//
//   reader ──inbox──▶ dispatcher ──calls──▶ handlers (N) ──replies──▶ writer
//                         │                                   ▲          ▲
//                         └──────────── replies ──────────────┘          │
//   notify_* (any thread) ─────────────── notices (drop oldest) ─────────┘
//
// - The reader frames lines from the input into the inbox. It owns its own
//   block (ReaderLink, shared) so that a reader still blocked on an input
//   that never ends can be abandoned at shutdown (closing a descriptor
//   another thread is reading is a race; abandoning is not).
// - The dispatcher parses each message, answers the quick methods (discover,
//   lists, initialize, ping, listen) itself, validates tool arguments, and
//   queues tool calls, reads and prompts to the handler tasks.
// - Responses and request-scoped notifications (progress, request logs,
//   subscription acknowledgements) go through `replies`, whose sends wait
//   for room: a request's progress always precedes its response. Change
//   notifications go through `notices`, which drops its oldest record when
//   full (a slow client misses a duplicate list_changed, never a reply).
// - The writer selects over both and writes one line per record.
//
// Locks. `mutex` guards the bindings, the in-flight table, the
// subscriptions, the client identity and the legacy session. It is never
// held while calling a handler or an event callback. The one nesting is
// `mutex`, then a channel's lock (a notice sent while the subscriptions are
// read); no channel lock is held while taking `mutex`.

#include "mcp/schema.hpp"
#include "stxlib/channels.hpp"
#include "stxlib/json.hpp"
#include "stxlib/mcp.hpp"
#include "stxlib/optional.hpp"
#include "stxlib/pipes.hpp"
#include "stxlib/shared.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::mcp::detail {

inline constexpr usize inbox_capacity = 16;
inline constexpr usize call_capacity = 64;
inline constexpr usize reply_capacity = 64;
inline constexpr usize notice_capacity = 64;
inline constexpr usize subscription_capacity = 16;
inline constexpr usize request_limit_ceiling = 256;
inline constexpr usize message_limit_floor = 1024;
inline constexpr usize message_limit_ceiling = 64 * 1024 * 1024;
inline constexpr usize subscribed_uri_limit = 64;

// ---- bindings --------------------------------------------------------------------

struct ToolBinding final {
    Utf8String name{};
    Utf8String title{};
    Utf8String description{};
    Schema input{};
    Schema output{};
    bool has_output{false};
    ToolAnnotations annotations{};
    ToolHandler handler{nullptr};
    void* binding{nullptr};
};

struct ResourceBinding final {
    // The URI, or the template text when is_template.
    Utf8String uri{};
    bool is_template{false};
    Utf8String name{};
    Utf8String title{};
    Utf8String description{};
    Utf8String mime_type{};
    ResourceHandler handler{nullptr};
    void* binding{nullptr};
};

struct PromptArgumentCopy final {
    Utf8String name{};
    Utf8String description{};
    bool required{false};
};

struct PromptBinding final {
    Utf8String name{};
    Utf8String title{};
    Utf8String description{};
    Vector<PromptArgumentCopy> arguments{};
    PromptHandler handler{nullptr};
    void* binding{nullptr};
};

struct TemplateParameter final {
    Utf8String name{};
    Utf8String value{};
};

// ---- the reader's block ------------------------------------------------------------

// One framed line (without its line break), or the mark of a line that
// passed the message limit (its bytes dropped).
struct InboxRecord final {
    Vector<char> bytes{};
    bool oversized{false};
};

enum class InputEnd : u8 {
    open = 0,
    ended = 1,
    failed = 2,
};

struct ReaderLink final {
    Channel<InboxRecord, inbox_capacity> inbox{};
    PipeReader input{};
    usize message_limit{0};
    // InputEnd, written by the reader before it closes the inbox.
    u8 end{0};
    // Set (release) as the reader's last act on the link: a reader that has
    // set it is joined, one that has not is abandoned.
    u8 finished{0};
};

// ---- outgoing ------------------------------------------------------------------------

struct OutRecord final {
    Utf8String line{};
    // A response (counted as a reply), else a notification.
    bool response{false};
};

// ---- requests in flight -----------------------------------------------------------------

struct ServerState;

struct CallRecord final {
    CallRecord(ServerState& owner, const Context& parent) noexcept
        : server(&owner), context(Context::ChildTag{}, parent) {}
    CallRecord(const CallRecord&) = delete;
    CallRecord& operator=(const CallRecord&) = delete;

    ServerState* server;
    // Cancelled by notifications/cancelled; shut_down through its parent,
    // the server's serving context.
    Context context;
    RequestKind kind{RequestKind::listing};
    // The request id as JSON text, written canonically (protocol.cpp).
    Utf8String id_json{};
    // The whole request message.
    JsonDocument document{};
    // tools/call: the arguments, in `document` or (when the request had
    // none) in `empty_arguments`, an empty object.
    JsonDocument empty_arguments{};
    const JsonDocument* arguments_document{nullptr};
    JsonRef arguments{};
    // prompts/get: the arguments object in `document`, when present.
    JsonRef prompt_arguments{};
    bool has_prompt_arguments{false};
    // The tool's or prompt's name, or the URI read.
    Utf8String name{};
    Shared<ToolBinding> tool{};
    Shared<ResourceBinding> resource{};
    Shared<PromptBinding> prompt{};
    Vector<TemplateParameter> parameters{};
    ClientIdentity client{};
    // A static literal: modern_protocol_version or a legacy one.
    TextView protocol_version{};
    bool modern{false};
    bool has_progress{false};
    Utf8String progress_token_json{};
    // The least severe LogLevel the client asked for, or -1 for none.
    i32 log_threshold{-1};
    usize slot{0};
    // The handler's progress so far (RequestView::progress is const: the
    // record is the handler's, one thread at a time).
    mutable double last_progress{0.0};
    mutable bool progress_sent{false};
};

struct InFlight final {
    bool used{false};
    Utf8String id_json{};
    Shared<CallRecord> call{};
};

// A modern subscriptions/listen stream.
struct Subscription final {
    bool used{false};
    Utf8String id_json{};
    bool tools{false};
    bool prompts{false};
    bool resources{false};
    Vector<Utf8String> uris{};
};

// ---- a reply under construction ----------------------------------------------------------

enum class ReplyOutcome : u8 {
    normal = 0,
    not_found = 1, // resources
    failed = 2,    // resources and prompts: -32603
};

struct ReplyRecord final {
    explicit ReplyRecord(const usize byte_limit) noexcept : limit(byte_limit), structured_writer(structured, byte_limit) {}
    ReplyRecord(const ReplyRecord&) = delete;
    ReplyRecord& operator=(const ReplyRecord&) = delete;

    usize limit;
    // Resources: the URI asked for, for contents given an empty URI.
    TextView default_uri{};
    // Content items (tools), contents (resources) or messages (prompts),
    // one JSON value each.
    Vector<Utf8String> items{};
    Utf8String scratch{};
    bool has_text_item{false};
    // ToolReply::fail.
    bool is_error{false};
    Utf8String error_item{};
    // ToolReply::structured.
    Utf8String structured{};
    JsonWriter structured_writer;
    bool structured_started{false};
    // PromptReply::description.
    Utf8String description{};
    bool has_description{false};
    ReplyOutcome outcome{ReplyOutcome::normal};
    Utf8String failure_message{};
    // The first failure while building (out of memory, the limit).
    Optional<ErrorRecord> trouble{};
};

// ---- the server ------------------------------------------------------------------------------

struct ServerState final {
    ServerState() noexcept = default;
    ServerState(const ServerState&) = delete;
    ServerState& operator=(const ServerState&) = delete;

    // Fixed at create.
    Utf8String name{};
    Utf8String version{};
    Utf8String title{};
    Utf8String instructions{};
    Utf8String model_meta_key{};
    ServerOptions options{};
    ServerEvents events{};

    // Guarded by `mutex`.
    Mutex mutex{};
    Vector<Shared<ToolBinding>> tools{};
    Vector<Shared<ResourceBinding>> resources{};
    Vector<Shared<PromptBinding>> prompts{};
    Vector<InFlight> in_flight{};
    usize in_flight_count{0};
    Subscription subscriptions[subscription_capacity]{};
    ClientIdentity client{};
    // The negotiated legacy revision (a static literal), empty before an
    // initialize.
    TextView legacy_version{};
    i32 legacy_log_threshold{-1};
    Vector<Utf8String> legacy_subscriptions{};

    // Serving.
    Context serving{};
    Channel<Shared<CallRecord>, call_capacity> calls{};
    Channel<OutRecord, reply_capacity> replies{};
    Channel<OutRecord, notice_capacity> notices{};
    Shared<ReaderLink> link{};
    PipeWriter output{};
    Thread reader{};
    Thread dispatcher{};
    Thread writer{};
    TaskGroup handlers{};
    // Atomic flags: started once; wait() done; why serving ended (0xFF
    // while it has not; the first reason is kept).
    u8 started{0};
    u8 waited{0};
    u8 end_reason{0xFF};

    // Atomic counters (bump, counters()).
    ServerCounters counters{};
};

// ---- shared helpers ----------------------------------------------------------------------------

inline void bump(u64& counter) noexcept { static_cast<void>(__atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED)); }

// Records why serving ended, if nothing was recorded first.
void note_end(ServerState& state, ServeEnd reason) noexcept;

// Queues a line for the writer: `replies` waits for room (responses and
// request-scoped notifications), `notices` drops its oldest (change
// notifications). A closed channel drops the line.
void send_reply(ServerState& state, Utf8String& line, bool response) noexcept;
void send_notice(ServerState& state, Utf8String& line) noexcept;

// Frees an in-flight slot.
void release_slot(ServerState& state, usize slot) noexcept;

// protocol.cpp: the dispatcher task's entry.
void dispatch_entry(const StopSignal& stop, void* context) noexcept;
// protocol.cpp: runs one queued call and sends its reply (or discards it).
void execute_call(ServerState& state, CallRecord& call) noexcept;
// protocol.cpp: the closing responses of open subscriptions, at shutdown.
void close_subscriptions(ServerState& state) noexcept;

enum class ListKind : u8 {
    tools = 0,
    prompts = 1,
    resources = 2,
};

// protocol.cpp: change notifications to subscriptions and a legacy client.
void broadcast_list_changed(ServerState& state, ListKind kind) noexcept;
void broadcast_resource_updated(ServerState& state, TextView uri) noexcept;
void broadcast_log(ServerState& state, LogLevel level, TextView text) noexcept;

// protocol.cpp: writes a request-scoped notification (progress, log).
void write_progress(const CallRecord& call, double progress, double total, TextView message) noexcept;
void write_request_log(const CallRecord& call, LogLevel level, TextView text) noexcept;

// The friend of the request and reply classes: builds them around records
// and calls the handlers (protocol.cpp).
class ServerAccess final {
public:
    static void call_tool(const ToolBinding& tool, const CallRecord& call, ReplyRecord& reply) noexcept;
    static void call_resource(const ResourceBinding& resource, const CallRecord& call, ReplyRecord& reply) noexcept;
    static void call_prompt(const PromptBinding& prompt, const CallRecord& call, ReplyRecord& reply) noexcept;
};

// Keeps the first failure of a reply under construction.
void note_trouble(ReplyRecord& reply, const OperationStatus& status) noexcept;

} // namespace stxlib::mcp::detail
