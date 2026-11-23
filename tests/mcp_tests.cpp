// Tests of stx_mcp (include/stxlib/mcp.hpp): a fake MCP client drives a
// real Server over two anonymous pipes, line by line, and checks every
// answer as JSON. Covered: server/discover; tools, resources (and
// templates) and prompts, listed and called; argument validation as a tool
// error; output schema failures; tool errors with codes and hints;
// JSON-RPC errors for unknown tools, unknown resources (modern and legacy
// codes), unsupported and missing protocol versions, missing client
// capabilities, malformed and oversized messages and unknown methods;
// progress (increasing only) and request logs; cancellation (no reply, the
// handler's context ends); subscriptions/listen (acknowledgement, tagged
// notifications, the closing response at shutdown); the legacy era
// (initialize, ping, logging/setLevel, resources/subscribe, untagged
// list_changed); shutdown on end of input and on stop(); bind refusals; and
// the JSON Schema subset itself (src/mcp/schema.hpp).

#include "stxlib/mcp.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"

#include "mcp/schema.hpp"

#include "test_support.hpp"

#include <atomic>
#include <cstring>
#include <string>

namespace {

using stxlib::Byte;
using stxlib::ContextEnd;
using stxlib::Duration;
using stxlib::ErrorKind;
using stxlib::i64;
using stxlib::IntervalTime;
using stxlib::JsonDocument;
using stxlib::JsonKind;
using stxlib::JsonRef;
using stxlib::JsonWriter;
using stxlib::MutableView;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::Optional;
using stxlib::PipePair;
using stxlib::PipeRead;
using stxlib::TextView;
using stxlib::Utf8String;
using stxlib::usize;
using stxlib::View;
using stxlib::mcp::ClientIdentity;
using stxlib::mcp::LogLevel;
using stxlib::mcp::PromptArgument;
using stxlib::mcp::PromptReply;
using stxlib::mcp::PromptRequest;
using stxlib::mcp::PromptSpec;
using stxlib::mcp::ResourceReply;
using stxlib::mcp::ResourceRequest;
using stxlib::mcp::ResourceSpec;
using stxlib::mcp::ResourceTemplateSpec;
using stxlib::mcp::ServeEnd;
using stxlib::mcp::Server;
using stxlib::mcp::ServerEvents;
using stxlib::mcp::ServerIdentity;
using stxlib::mcp::ServerOptions;
using stxlib::mcp::ToolReply;
using stxlib::mcp::ToolRequest;
using stxlib::mcp::ToolSpec;

// ---- text and JSON helpers ----------------------------------------------------------

TextView view_of(const std::string& text) {
    OperationResult<TextView> checked = stxlib::validate_utf8(text.data(), text.size());
    CHECK(checked.succeeded());
    if (checked.failed()) return TextView();
    return *checked.value();
}

std::string text_of(const TextView text) { return std::string(text.data(), text.count()); }

// A value by path: members and array positions separated by '|' (MCP's
// keys hold '/').
Optional<JsonRef> at(const JsonDocument& document, const std::string& path) {
    JsonRef current = document.root();
    usize start = 0;
    while (start <= path.size() && !path.empty()) {
        usize end = path.find('|', start);
        if (end == std::string::npos) end = path.size();
        const std::string step = path.substr(start, end - start);
        Optional<JsonRef> next = Optional<JsonRef>::none();
        if (document.is(current, JsonKind::array)) {
            next = document.element(current, static_cast<usize>(std::stoul(step)));
        } else {
            next = document.member(current, view_of(step));
        }
        if (next.absent()) return next;
        current = *next.value();
        start = end + 1;
        if (end == path.size()) break;
    }
    return Optional<JsonRef>::of(current);
}

bool has(const JsonDocument& document, const std::string& path) { return at(document, path).present(); }

std::string text_at(const JsonDocument& document, const std::string& path) {
    const Optional<JsonRef> found = at(document, path);
    if (found.absent()) return "<absent>";
    return text_of(document.text(*found.value()).value_or(TextView("<not text>")));
}

i64 integer_at(const JsonDocument& document, const std::string& path) {
    const Optional<JsonRef> found = at(document, path);
    if (found.absent()) return -999999;
    return document.integer(*found.value()).value_or(-999998);
}

bool boolean_at(const JsonDocument& document, const std::string& path) {
    const Optional<JsonRef> found = at(document, path);
    if (found.absent()) return false;
    return document.boolean(*found.value()).value_or(false);
}

usize count_at(const JsonDocument& document, const std::string& path) {
    const Optional<JsonRef> found = at(document, path);
    if (found.absent()) return 999;
    return document.child_count(*found.value());
}

bool contains(const std::string& text, const char* const piece) { return text.find(piece) != std::string::npos; }

ErrorKind kind_of(const OperationStatus& status) {
    if (status.succeeded()) return static_cast<ErrorKind>(0);
    return (*status.error()).kind();
}

const char* const modern_meta_core = R"("io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                                     R"("io.modelcontextprotocol/clientCapabilities":{},)"
                                     R"("io.modelcontextprotocol/clientInfo":{"name":"fake","version":"1.0"})";

// A modern request: `params` are the members after _meta (may be empty);
// `meta_extra` are extra _meta members (may be empty).
std::string modern(const std::string& id, const char* const method, const std::string& params,
                   const std::string& meta_extra = "") {
    std::string line = R"({"jsonrpc":"2.0","id":)" + id + R"(,"method":")" + method + R"(","params":{"_meta":{)";
    line += modern_meta_core;
    if (!meta_extra.empty()) line += "," + meta_extra;
    line += "}";
    if (!params.empty()) line += "," + params;
    line += "}}";
    return line;
}

std::string legacy(const std::string& id, const char* const method, const std::string& params) {
    std::string line = R"({"jsonrpc":"2.0","id":)" + id + R"(,"method":")" + method + R"(")";
    if (!params.empty()) line += R"(,"params":{)" + params + "}";
    line += "}";
    return line;
}

// ---- the program under test ----------------------------------------------------------

struct Fixture final {
    std::atomic<int> client_seen{0};
    std::atomic<int> input_ended{0};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<bool> slow_started{false};
    std::atomic<bool> slow_finished{false};
    std::atomic<int> slow_end{-1};
};

void on_client(void* const context, const ClientIdentity& client) noexcept {
    static_cast<void>(client);
    (*static_cast<Fixture*>(context)).client_seen.fetch_add(1);
}

void on_input_ended(void* const context) noexcept { (*static_cast<Fixture*>(context)).input_ended.fetch_add(1); }

void on_started(void* const context, const stxlib::mcp::RequestKind kind, const TextView name) noexcept {
    static_cast<void>(kind);
    static_cast<void>(name);
    (*static_cast<Fixture*>(context)).started.fetch_add(1);
}

void on_finished(void* const context, const stxlib::mcp::RequestKind kind, const TextView name) noexcept {
    static_cast<void>(kind);
    static_cast<void>(name);
    (*static_cast<Fixture*>(context)).finished.fetch_add(1);
}

void echo_tool(void* const binding, const ToolRequest& request, ToolReply& reply) noexcept {
    static_cast<void>(binding);
    const TextView text = request.text("text").value_or(TextView());
    JsonWriter& out = reply.structured();
    out.begin_object();
    out.member_string("echoed", text);
    out.end_object();
}

void broken_tool(void* const binding, const ToolRequest& request, ToolReply& reply) noexcept {
    static_cast<void>(binding);
    static_cast<void>(request);
    JsonWriter& out = reply.structured();
    out.begin_object();
    out.member_string("count", "many"); // the output schema says integer
    out.end_object();
}

void failing_tool(void* const binding, const ToolRequest& request, ToolReply& reply) noexcept {
    static_cast<void>(binding);
    static_cast<void>(request);
    reply.text("kept after the error");
    reply.fail("not_found", "no such window", "call list_windows first");
}

void plain_tool(void* const binding, const ToolRequest& request, ToolReply& reply) noexcept {
    static_cast<void>(binding);
    request.log(LogLevel::info, "note");
    reply.text("hi");
    const Byte png[] = {0x89, 'P', 'N', 'G'};
    reply.image(View<Byte>(png), "image/png");
    reply.resource_link("gui://about", "about", "text/plain");
}

void slow_tool(void* const binding, const ToolRequest& request, ToolReply& reply) noexcept {
    Fixture& fixture = *static_cast<Fixture*>(binding);
    request.progress(1.0, 2.0, "half");
    request.progress(0.5, 2.0, "not above the last: dropped");
    fixture.slow_started.store(true);
    const IntervalTime limit = IntervalTime::now().after(Duration::seconds(10));
    while (!request.context().done() && IntervalTime::now() < limit) stxlib::sleep_for(Duration::milliseconds(2));
    fixture.slow_end.store(static_cast<int>(request.context().end()));
    reply.text("too late");
    fixture.slow_finished.store(true);
}

void about_resource(void* const binding, const ResourceRequest& request, ResourceReply& reply) noexcept {
    static_cast<void>(binding);
    static_cast<void>(request);
    reply.text("", "text/plain", "about this program");
}

void outline_resource(void* const binding, const ResourceRequest& request, ResourceReply& reply) noexcept {
    static_cast<void>(binding);
    const TextView window = request.parameter("window").value_or(TextView());
    if (window == TextView("missing")) {
        reply.not_found();
        return;
    }
    Utf8String text;
    static_cast<void>(text.assign("outline of "));
    static_cast<void>(text.append(window));
    reply.text(request.uri(), "text/plain", text.view());
}

void summarize_prompt(void* const binding, const PromptRequest& request, PromptReply& reply) noexcept {
    static_cast<void>(binding);
    Utf8String text;
    static_cast<void>(text.assign("Summarize "));
    static_cast<void>(text.append(request.argument("topic").value_or(TextView())));
    reply.user_text(text.view());
    reply.assistant_text("Certainly.");
}

constexpr TextView echo_input = R"({"type":"object","properties":{"text":{"type":"string","minLength":1}},)"
                                R"("required":["text"],"additionalProperties":false})";
constexpr TextView echo_output = R"({"type":"object","properties":{"echoed":{"type":"string"}},"required":["echoed"]})";
constexpr TextView count_output = R"({"type":"object","properties":{"count":{"type":"integer"}},"required":["count"]})";

void bind_everything(Server& server, Fixture& fixture) {
    ToolSpec echo;
    echo.name = "echo";
    echo.title = "Echo";
    echo.description = "Returns its text.";
    echo.input_schema = echo_input;
    echo.output_schema = echo_output;
    echo.annotations.read_only = true;
    echo.annotations.idempotent = true;
    CHECK(server.bind_tool(echo, &echo_tool, &fixture).succeeded());
    ToolSpec broken;
    broken.name = "broken";
    broken.output_schema = count_output;
    CHECK(server.bind_tool(broken, &broken_tool, &fixture).succeeded());
    ToolSpec failing;
    failing.name = "fails";
    CHECK(server.bind_tool(failing, &failing_tool, &fixture).succeeded());
    ToolSpec plain;
    plain.name = "plain";
    CHECK(server.bind_tool(plain, &plain_tool, &fixture).succeeded());
    ToolSpec slow;
    slow.name = "slow";
    slow.annotations.destructive = true;
    CHECK(server.bind_tool(slow, &slow_tool, &fixture).succeeded());

    ResourceSpec about;
    about.uri = "gui://about";
    about.name = "about";
    about.mime_type = "text/plain";
    CHECK(server.bind_resource(about, &about_resource, &fixture).succeeded());
    ResourceTemplateSpec outline;
    outline.uri_template = "gui://window/{window}/outline";
    outline.name = "outline";
    CHECK(server.bind_resource_template(outline, &outline_resource, &fixture).succeeded());

    static const PromptArgument arguments[] = {{"topic", "What to summarize", true}};
    PromptSpec summarize;
    summarize.name = "summarize";
    summarize.description = "Summarize a topic";
    summarize.arguments = View<PromptArgument>(arguments);
    CHECK(server.bind_prompt(summarize, &summarize_prompt, &fixture).succeeded());
}

// ---- the fake client ---------------------------------------------------------------------

struct Client final {
    Server server{};
    stxlib::PipeWriter to_server{};
    stxlib::PipeReader from_server{};
    std::string pending{};
    bool ended{false};
};

bool open_client(Client& client, Fixture& fixture, const usize message_limit) {
    ServerIdentity identity;
    identity.name = "app.example.fixture";
    identity.version = "1.2.3";
    identity.title = "Fixture";
    identity.instructions = "Use echo to echo.";
    ServerOptions options;
    options.message_limit = message_limit;
    options.handler_tasks = 3;
    ServerEvents events;
    events.context = &fixture;
    events.client_seen = &on_client;
    events.input_ended = &on_input_ended;
    events.request_started = &on_started;
    events.request_finished = &on_finished;
    OperationResult<Server> created = Server::create(identity, options, events);
    CHECK(created.succeeded());
    if (created.failed()) return false;
    client.server = static_cast<OperationResult<Server>&&>(created).take_value();
    bind_everything(client.server, fixture);
    OperationResult<PipePair> inbound = stxlib::create_pipe();
    OperationResult<PipePair> outbound = stxlib::create_pipe();
    CHECK(inbound.succeeded() && outbound.succeeded());
    if (inbound.failed() || outbound.failed()) return false;
    PipePair in = static_cast<OperationResult<PipePair>&&>(inbound).take_value();
    PipePair out = static_cast<OperationResult<PipePair>&&>(outbound).take_value();
    client.to_server = static_cast<stxlib::PipeWriter&&>(in.writer);
    client.from_server = static_cast<stxlib::PipeReader&&>(out.reader);
    const OperationStatus started = client.server.start(static_cast<stxlib::PipeReader&&>(in.reader),
                                                        static_cast<stxlib::PipeWriter&&>(out.writer));
    CHECK(started.succeeded());
    CHECK(client.server.started());
    return started.succeeded();
}

void send_line(Client& client, const std::string& line) {
    const std::string framed = line + "\n";
    const OperationStatus written =
        client.to_server.write_all(View<Byte>(reinterpret_cast<const Byte*>(framed.data()), framed.size()));
    CHECK(written.succeeded());
}

// The next line from the server, or "" at the end of its output.
std::string read_line(Client& client) {
    for (;;) {
        const usize newline = client.pending.find('\n');
        if (newline != std::string::npos) {
            const std::string line = client.pending.substr(0, newline);
            client.pending.erase(0, newline + 1);
            return line;
        }
        if (client.ended) return "";
        Byte buffer[4096];
        OperationResult<PipeRead> got = client.from_server.read(MutableView<Byte>(buffer));
        if (got.failed() || (*got.value()).is_end()) {
            client.ended = true;
            continue;
        }
        client.pending.append(reinterpret_cast<const char*>(buffer), *(*got.value()).byte_count());
    }
}

JsonDocument read_message(Client& client) {
    const std::string line = read_line(client);
    CHECK(!line.empty());
    CHECK(line.find('\n') == std::string::npos);
    OperationResult<JsonDocument> parsed = JsonDocument::parse(view_of(line));
    CHECK(parsed.succeeded());
    if (parsed.failed()) {
        std::fprintf(stderr, "unreadable line: %s\n", line.c_str());
        return JsonDocument();
    }
    return static_cast<OperationResult<JsonDocument>&&>(parsed).take_value();
}

JsonDocument exchange(Client& client, const std::string& line) {
    send_line(client, line);
    return read_message(client);
}

// ---- the modern era --------------------------------------------------------------------------

void discover_and_list(Client& client) {
    const JsonDocument discovered = exchange(client, modern("1", "server/discover", ""));
    CHECK(text_at(discovered, "jsonrpc") == "2.0");
    CHECK(integer_at(discovered, "id") == 1);
    CHECK(text_at(discovered, "result|supportedVersions|0") == "2026-07-28");
    CHECK(text_at(discovered, "result|resultType") == "complete");
    CHECK(text_at(discovered, "result|_meta|io.modelcontextprotocol/serverInfo|name") == "app.example.fixture");
    CHECK(text_at(discovered, "result|_meta|io.modelcontextprotocol/serverInfo|version") == "1.2.3");
    CHECK(text_at(discovered, "result|instructions") == "Use echo to echo.");
    CHECK(integer_at(discovered, "result|ttlMs") == 0);
    CHECK(text_at(discovered, "result|cacheScope") == "private");
    CHECK(boolean_at(discovered, "result|capabilities|tools|listChanged"));

    const JsonDocument tools = exchange(client, modern("\"two\"", "tools/list", ""));
    CHECK(text_at(tools, "id") == "two");
    CHECK(count_at(tools, "result|tools") == 5);
    // Bindings keep their order until an unbind.
    CHECK(text_at(tools, "result|tools|0|name") == "echo");
    CHECK(text_at(tools, "result|tools|0|title") == "Echo");
    CHECK(text_at(tools, "result|tools|0|inputSchema|type") == "object");
    CHECK(text_at(tools, "result|tools|0|outputSchema|required|0") == "echoed");
    CHECK(boolean_at(tools, "result|tools|0|annotations|readOnlyHint"));
    CHECK(has(tools, "result|tools|0|annotations|destructiveHint"));
    CHECK(!boolean_at(tools, "result|tools|0|annotations|destructiveHint"));
    CHECK(!boolean_at(tools, "result|tools|0|annotations|openWorldHint"));
    // A tool without a declared input schema takes no arguments.
    CHECK(text_at(tools, "result|tools|2|name") == "fails");
    CHECK(has(tools, "result|tools|2|inputSchema|additionalProperties"));
    CHECK(!boolean_at(tools, "result|tools|2|inputSchema|additionalProperties"));
    CHECK(boolean_at(tools, "result|tools|4|annotations|destructiveHint"));
    CHECK(has(tools, "result|ttlMs"));

    const JsonDocument resources = exchange(client, modern("3", "resources/list", ""));
    CHECK(count_at(resources, "result|resources") == 1);
    CHECK(text_at(resources, "result|resources|0|uri") == "gui://about");
    CHECK(text_at(resources, "result|resources|0|mimeType") == "text/plain");
    const JsonDocument templates = exchange(client, modern("4", "resources/templates/list", ""));
    CHECK(count_at(templates, "result|resourceTemplates") == 1);
    CHECK(text_at(templates, "result|resourceTemplates|0|uriTemplate") == "gui://window/{window}/outline");
    const JsonDocument prompts = exchange(client, modern("5", "prompts/list", ""));
    CHECK(text_at(prompts, "result|prompts|0|name") == "summarize");
    CHECK(text_at(prompts, "result|prompts|0|arguments|0|name") == "topic");
    CHECK(boolean_at(prompts, "result|prompts|0|arguments|0|required"));
    const JsonDocument paged = exchange(client, modern("6", "tools/list", R"("cursor":"anything")"));
    CHECK(integer_at(paged, "error|code") == -32602);
}

void tool_calls(Client& client) {
    const JsonDocument echoed = exchange(client, modern("10", "tools/call", R"("name":"echo","arguments":{"text":"hi"})"));
    CHECK(text_at(echoed, "result|structuredContent|echoed") == "hi");
    CHECK(text_at(echoed, "result|content|0|type") == "text");
    CHECK(text_at(echoed, "result|content|0|text") == R"({"echoed":"hi"})");
    CHECK(!has(echoed, "result|isError"));
    CHECK(text_at(echoed, "result|resultType") == "complete");

    const JsonDocument missing = exchange(client, modern("11", "tools/call", R"("name":"echo","arguments":{})"));
    CHECK(boolean_at(missing, "result|isError"));
    const std::string missing_text = text_at(missing, "result|content|0|text");
    CHECK(contains(missing_text, "invalid_arguments"));
    CHECK(contains(missing_text, "text"));
    CHECK(contains(missing_text, "hint"));

    const JsonDocument extra =
        exchange(client, modern("12", "tools/call", R"("name":"echo","arguments":{"text":"hi","more":1})"));
    CHECK(boolean_at(extra, "result|isError"));
    CHECK(contains(text_at(extra, "result|content|0|text"), "/more"));

    const JsonDocument wrong_kind = exchange(client, modern("13", "tools/call", R"("name":"echo","arguments":{"text":5})"));
    CHECK(boolean_at(wrong_kind, "result|isError"));
    CHECK(contains(text_at(wrong_kind, "result|content|0|text"), "expected string"));

    const JsonDocument empty_text = exchange(client, modern("14", "tools/call", R"("name":"echo","arguments":{"text":""})"));
    CHECK(boolean_at(empty_text, "result|isError"));

    const JsonDocument unknown = exchange(client, modern("15", "tools/call", R"("name":"nope")"));
    CHECK(integer_at(unknown, "error|code") == -32602);
    CHECK(integer_at(unknown, "id") == 15);

    const JsonDocument broken = exchange(client, modern("16", "tools/call", R"("name":"broken")"));
    CHECK(boolean_at(broken, "result|isError"));
    CHECK(contains(text_at(broken, "result|content|0|text"), "internal_failure"));
    CHECK(!has(broken, "result|structuredContent"));

    const JsonDocument failed = exchange(client, modern("17", "tools/call", R"("name":"fails")"));
    CHECK(boolean_at(failed, "result|isError"));
    const std::string failure_text = text_at(failed, "result|content|0|text");
    CHECK(contains(failure_text, R"("error":"not_found")"));
    CHECK(contains(failure_text, "no such window"));
    CHECK(contains(failure_text, "call list_windows first"));
    CHECK(text_at(failed, "result|content|1|text") == "kept after the error");

    const JsonDocument plain = exchange(client, modern("18", "tools/call", R"("name":"plain")"));
    CHECK(count_at(plain, "result|content") == 3);
    CHECK(text_at(plain, "result|content|1|type") == "image");
    CHECK(text_at(plain, "result|content|1|data") == "iVBORw==");
    CHECK(text_at(plain, "result|content|2|type") == "resource_link");
    CHECK(!has(plain, "result|structuredContent"));

    // With a log level, the handler's message precedes its response.
    send_line(client, modern("19", "tools/call", R"("name":"plain")", R"("io.modelcontextprotocol/logLevel":"debug")"));
    const JsonDocument note = read_message(client);
    CHECK(text_at(note, "method") == "notifications/message");
    CHECK(text_at(note, "params|level") == "info");
    CHECK(text_at(note, "params|data") == "note");
    const JsonDocument after_note = read_message(client);
    CHECK(integer_at(after_note, "id") == 19);

    const stxlib::mcp::ServerCounters counters = client.server.counters();
    CHECK(counters.argument_failures == 4);
    CHECK(counters.output_schema_failures == 1);
}

void reads_and_prompts(Client& client) {
    const JsonDocument about = exchange(client, modern("20", "resources/read", R"("uri":"gui://about")"));
    CHECK(text_at(about, "result|contents|0|uri") == "gui://about"); // filled in from the request
    CHECK(text_at(about, "result|contents|0|text") == "about this program");
    CHECK(has(about, "result|ttlMs"));
    const JsonDocument outline = exchange(client, modern("21", "resources/read", R"("uri":"gui://window/main/outline")"));
    CHECK(text_at(outline, "result|contents|0|text") == "outline of main");
    const JsonDocument nothing = exchange(client, modern("22", "resources/read", R"("uri":"gui://nothing")"));
    CHECK(integer_at(nothing, "error|code") == -32602);
    CHECK(text_at(nothing, "error|data|uri") == "gui://nothing");
    const JsonDocument gone = exchange(client, modern("23", "resources/read", R"("uri":"gui://window/missing/outline")"));
    CHECK(integer_at(gone, "error|code") == -32602);
    const JsonDocument nested = exchange(client, modern("24", "resources/read", R"("uri":"gui://window/a/b/outline")"));
    CHECK(integer_at(nested, "error|code") == -32602); // a parameter never spans a '/'

    const JsonDocument prompt =
        exchange(client, modern("25", "prompts/get", R"("name":"summarize","arguments":{"topic":"pipes"})"));
    CHECK(text_at(prompt, "result|description") == "Summarize a topic");
    CHECK(text_at(prompt, "result|messages|0|role") == "user");
    CHECK(text_at(prompt, "result|messages|0|content|text") == "Summarize pipes");
    CHECK(text_at(prompt, "result|messages|1|role") == "assistant");
    const JsonDocument no_topic = exchange(client, modern("26", "prompts/get", R"("name":"summarize")"));
    CHECK(integer_at(no_topic, "error|code") == -32602);
    const JsonDocument no_prompt = exchange(client, modern("27", "prompts/get", R"("name":"other")"));
    CHECK(integer_at(no_prompt, "error|code") == -32602);
}

void protocol_errors(Client& client) {
    const std::string wrong_version = R"({"jsonrpc":"2.0","id":30,"method":"tools/list","params":{"_meta":{)"
                                      R"("io.modelcontextprotocol/protocolVersion":"1999-01-01",)"
                                      R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
    const JsonDocument unsupported = exchange(client, wrong_version);
    CHECK(integer_at(unsupported, "error|code") == -32022);
    CHECK(text_at(unsupported, "error|data|requested") == "1999-01-01");
    CHECK(text_at(unsupported, "error|data|supported|0") == "2026-07-28");

    const JsonDocument no_meta = exchange(client, R"({"jsonrpc":"2.0","id":31,"method":"tools/list"})");
    CHECK(integer_at(no_meta, "error|code") == -32602);
    CHECK(has(no_meta, "error|data|supported"));

    const std::string no_capabilities = R"({"jsonrpc":"2.0","id":32,"method":"tools/list","params":{"_meta":{)"
                                        R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})";
    CHECK(integer_at(exchange(client, no_capabilities), "error|code") == -32602);

    const JsonDocument unparsable = exchange(client, "{not json");
    CHECK(integer_at(unparsable, "error|code") == -32700);
    const Optional<JsonRef> null_id = at(unparsable, "id");
    CHECK(null_id.present() && unparsable.is(*null_id.value(), JsonKind::null_value));

    CHECK(integer_at(exchange(client, "[1,2]"), "error|code") == -32600);
    CHECK(integer_at(exchange(client, R"({"jsonrpc":"1.0","id":33,"method":"tools/list"})"), "error|code") == -32600);
    CHECK(integer_at(exchange(client, R"({"jsonrpc":"2.0","id":1.5,"method":"tools/list"})"), "error|code") == -32600);
    CHECK(integer_at(exchange(client, R"({"jsonrpc":"2.0","id":34})"), "error|code") == -32600);
    CHECK(integer_at(exchange(client, modern("35", "nope/nope", "")), "error|code") == -32601);
    CHECK(integer_at(exchange(client, modern("36", "ping", "")), "error|code") == -32601); // removed in 2026-07-28

    // A line past the message limit is refused, and the stream goes on.
    const std::string huge = R"({"jsonrpc":"2.0","id":37,"method":"tools/list","pad":")" + std::string(20000, 'x') + "\"}";
    const JsonDocument oversized = exchange(client, huge);
    CHECK(integer_at(oversized, "error|code") == -32600);
    const JsonDocument after = exchange(client, modern("38", "server/discover", ""));
    CHECK(integer_at(after, "id") == 38);
    CHECK(client.server.counters().oversized_messages == 1);
    CHECK(client.server.counters().parse_errors == 1);

    // Notifications get no answer; a response from the client is ignored.
    send_line(client, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    send_line(client, R"({"jsonrpc":"2.0","id":99,"result":{}})");
    const JsonDocument next = exchange(client, modern("39", "server/discover", ""));
    CHECK(integer_at(next, "id") == 39);
}

void wait_for(const std::atomic<bool>& flag) {
    const IntervalTime limit = IntervalTime::now().after(Duration::seconds(10));
    while (!flag.load() && IntervalTime::now() < limit) stxlib::sleep_for(Duration::milliseconds(2));
}

void cancellation(Client& client, Fixture& fixture) {
    send_line(client, modern("60", "tools/call", R"("name":"slow")", R"("progressToken":"p1")"));
    const JsonDocument progress = read_message(client);
    CHECK(text_at(progress, "method") == "notifications/progress");
    CHECK(text_at(progress, "params|progressToken") == "p1");
    CHECK(integer_at(progress, "params|progress") == 1);
    CHECK(integer_at(progress, "params|total") == 2);
    CHECK(text_at(progress, "params|message") == "half");
    wait_for(fixture.slow_started);
    send_line(client, R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":60,"reason":"enough"}})");
    wait_for(fixture.slow_finished);
    CHECK(fixture.slow_end.load() == static_cast<int>(ContextEnd::cancelled));
    // No reply for 60 (and no second progress): the next line answers 61.
    const JsonDocument next = exchange(client, modern("61", "server/discover", ""));
    CHECK(integer_at(next, "id") == 61);
    const stxlib::mcp::ServerCounters counters = client.server.counters();
    CHECK(counters.cancellations == 1);
    CHECK(counters.replies_discarded >= 1);
}

void subscriptions(Client& client) {
    send_line(client, modern("50", "subscriptions/listen",
                             R"("notifications":{"toolsListChanged":true,"resourceSubscriptions":["gui://about"]})"));
    const JsonDocument acknowledged = read_message(client);
    CHECK(text_at(acknowledged, "method") == "notifications/subscriptions/acknowledged");
    CHECK(integer_at(acknowledged, "params|_meta|io.modelcontextprotocol/subscriptionId") == 50);
    CHECK(boolean_at(acknowledged, "params|notifications|toolsListChanged"));
    CHECK(!boolean_at(acknowledged, "params|notifications|promptsListChanged"));
    CHECK(text_at(acknowledged, "params|notifications|resourceSubscriptions|0") == "gui://about");

    client.server.notify_tools_changed();
    const JsonDocument changed = read_message(client);
    CHECK(text_at(changed, "method") == "notifications/tools/list_changed");
    CHECK(integer_at(changed, "params|_meta|io.modelcontextprotocol/subscriptionId") == 50);

    client.server.notify_resource_updated("gui://about");
    const JsonDocument updated = read_message(client);
    CHECK(text_at(updated, "method") == "notifications/resources/updated");
    CHECK(text_at(updated, "params|uri") == "gui://about");
    CHECK(integer_at(updated, "params|_meta|io.modelcontextprotocol/subscriptionId") == 50);

    // Not subscribed: nothing is sent.
    client.server.notify_prompts_changed();
    client.server.notify_resource_updated("gui://other");
    client.server.log(LogLevel::emergency, "modern clients get logs only through requests");
    const JsonDocument next = exchange(client, modern("51", "server/discover", ""));
    CHECK(integer_at(next, "id") == 51);

    // Binding a tool notifies by itself.
    ToolSpec late;
    late.name = "late";
    Fixture unused;
    CHECK(client.server.bind_tool(late, &echo_tool, &unused).succeeded());
    const JsonDocument bound = read_message(client);
    CHECK(text_at(bound, "method") == "notifications/tools/list_changed");
    CHECK(client.server.unbind_tool("late").succeeded());
    const JsonDocument unbound = read_message(client);
    CHECK(text_at(unbound, "method") == "notifications/tools/list_changed");

    // An id in use by an open subscription is refused.
    const JsonDocument duplicate = exchange(client, modern("50", "tools/call", R"("name":"echo","arguments":{"text":"x"})"));
    CHECK(integer_at(duplicate, "error|code") == -32600);
}

void modern_session() {
    Fixture fixture;
    Client client;
    if (!open_client(client, fixture, 16 * 1024)) return;
    discover_and_list(client);
    tool_calls(client);
    reads_and_prompts(client);
    protocol_errors(client);
    cancellation(client, fixture);
    subscriptions(client);
    CHECK(fixture.client_seen.load() == 1);
    const ClientIdentity seen = client.server.client();
    CHECK(text_of(seen.name()) == "fake");
    CHECK(text_of(seen.version()) == "1.0");
    CHECK(fixture.started.load() > 0);

    // The client closes its output: serving ends, the open subscription
    // gets its closing response, and the output ends.
    client.to_server.close();
    CHECK(client.server.wait() == ServeEnd::input_ended);
    CHECK(client.server.wait() == ServeEnd::input_ended); // a second call returns the same
    CHECK(fixture.input_ended.load() == 1);
    CHECK(fixture.started.load() == fixture.finished.load());
    const JsonDocument closing = read_message(client);
    CHECK(integer_at(closing, "id") == 50);
    CHECK(text_at(closing, "result|resultType") == "complete");
    CHECK(integer_at(closing, "result|_meta|io.modelcontextprotocol/subscriptionId") == 50);
    CHECK(read_line(client).empty());
    CHECK(client.ended);
}

// ---- the legacy era -------------------------------------------------------------------------------

void legacy_session() {
    Fixture fixture;
    Client client;
    if (!open_client(client, fixture, 16 * 1024)) return;
    const JsonDocument early = exchange(client, legacy("1", "tools/list", ""));
    CHECK(integer_at(early, "error|code") == -32602);

    const JsonDocument initialized = exchange(
        client, legacy("2", "initialize",
                       R"("protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"old","version":"2"})"));
    CHECK(text_at(initialized, "result|protocolVersion") == "2025-06-18");
    CHECK(text_at(initialized, "result|serverInfo|name") == "app.example.fixture");
    CHECK(boolean_at(initialized, "result|capabilities|resources|subscribe"));
    CHECK(!has(initialized, "result|resultType"));
    const ClientIdentity seen = client.server.client();
    CHECK(text_of(seen.name()) == "old");
    send_line(client, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    const JsonDocument pong = exchange(client, legacy("3", "ping", ""));
    CHECK(has(pong, "result"));
    CHECK(count_at(pong, "result") == 0);
    const JsonDocument tools = exchange(client, legacy("4", "tools/list", ""));
    CHECK(count_at(tools, "result|tools") == 5);
    CHECK(!has(tools, "result|ttlMs"));
    CHECK(!has(tools, "result|resultType"));

    client.server.notify_tools_changed();
    const JsonDocument changed = read_message(client);
    CHECK(text_at(changed, "method") == "notifications/tools/list_changed");
    CHECK(!has(changed, "params|_meta"));

    CHECK(has(exchange(client, legacy("5", "logging/setLevel", R"("level":"warning")")), "result"));
    CHECK(integer_at(exchange(client, legacy("6", "logging/setLevel", R"("level":"loud")")), "error|code") == -32602);
    client.server.log(LogLevel::info, "below the level");
    client.server.log(LogLevel::error, "bad");
    const JsonDocument logged = read_message(client);
    CHECK(text_at(logged, "method") == "notifications/message");
    CHECK(text_at(logged, "params|level") == "error");
    CHECK(text_at(logged, "params|data") == "bad");

    CHECK(has(exchange(client, legacy("7", "resources/subscribe", R"("uri":"gui://about")")), "result"));
    client.server.notify_resource_updated("gui://about");
    const JsonDocument updated = read_message(client);
    CHECK(text_at(updated, "method") == "notifications/resources/updated");
    CHECK(text_at(updated, "params|uri") == "gui://about");
    CHECK(has(exchange(client, legacy("8", "resources/unsubscribe", R"("uri":"gui://about")")), "result"));
    client.server.notify_resource_updated("gui://about");

    const JsonDocument missing = exchange(client, legacy("9", "resources/read", R"("uri":"gui://nothing")"));
    CHECK(integer_at(missing, "error|code") == -32002); // the legacy code
    const JsonDocument echoed = exchange(client, legacy("10", "tools/call", R"("name":"echo","arguments":{"text":"old"})"));
    CHECK(text_at(echoed, "result|structuredContent|echoed") == "old");
    CHECK(!has(echoed, "result|resultType"));

    const JsonDocument newest =
        exchange(client, legacy("11", "initialize", R"("protocolVersion":"2099-01-01","capabilities":{})"));
    CHECK(text_at(newest, "result|protocolVersion") == text_of(stxlib::mcp::legacy_protocol_version(0)));
    CHECK(integer_at(exchange(client, legacy("12", "subscriptions/listen", "")), "error|code") == -32601);

    // stop(): serving ends with the input still open; the reader is
    // abandoned, the output ends.
    client.server.stop();
    CHECK(client.server.wait() == ServeEnd::stopped);
    CHECK(read_line(client).empty());
    CHECK(fixture.input_ended.load() == 0);
    client.to_server.close();
}

// ---- creation, binding and starting refusals -------------------------------------------------------

void refusals() {
    ServerIdentity nameless;
    CHECK(Server::create(nameless, ServerOptions{}, ServerEvents{}).failed());
    ServerIdentity identity;
    identity.name = "app.example.refusals";
    ServerOptions too_many;
    too_many.handler_tasks = stxlib::task_group_capacity;
    CHECK(Server::create(identity, too_many, ServerEvents{}).failed());
    ServerOptions tiny;
    tiny.message_limit = 10;
    CHECK(Server::create(identity, tiny, ServerEvents{}).failed());

    OperationResult<Server> created = Server::create(identity, ServerOptions{}, ServerEvents{});
    CHECK(created.succeeded());
    if (created.failed()) return;
    Server server = static_cast<OperationResult<Server>&&>(created).take_value();
    CHECK(!server.started());
    CHECK(server.wait() == ServeEnd::stopped); // never started
    Fixture fixture;

    ToolSpec spec;
    spec.name = "has space";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::invalid_argument);
    spec.name = "";
    CHECK(server.bind_tool(spec, &echo_tool, &fixture).failed());
    spec.name = "ok";
    spec.input_schema = R"({"type":"object","properties":{"a":{"$ref":"#/x"}}})";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::unsupported);
    spec.input_schema = R"({"type":"object","anyOf":[]})";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::unsupported);
    spec.input_schema = R"({"type":"string"})";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::invalid_argument);
    spec.input_schema = R"({"type":"object",)";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::invalid_argument);
    spec.input_schema = R"({"$schema":"http://json-schema.org/draft-04/schema#","type":"object"})";
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::unsupported);
    spec.input_schema = TextView();
    spec.output_schema = R"({"type":"array"})";
    CHECK(server.bind_tool(spec, &echo_tool, &fixture).failed());
    spec.output_schema = TextView();
    CHECK(server.bind_tool(spec, &echo_tool, &fixture).succeeded());
    CHECK(kind_of(server.bind_tool(spec, &echo_tool, &fixture)) == ErrorKind::already_exists);
    CHECK(kind_of(server.unbind_tool("absent")) == ErrorKind::not_found);
    CHECK(server.unbind_tool("ok").succeeded());

    ResourceTemplateSpec bad_template;
    bad_template.uri_template = "gui://{a{b}}";
    CHECK(server.bind_resource_template(bad_template, &outline_resource, &fixture).failed());
    bad_template.uri_template = "gui://{}";
    CHECK(server.bind_resource_template(bad_template, &outline_resource, &fixture).failed());
    bad_template.uri_template = "gui://{open";
    CHECK(server.bind_resource_template(bad_template, &outline_resource, &fixture).failed());
    bad_template.uri_template = "gui://{+reserved}";
    CHECK(server.bind_resource_template(bad_template, &outline_resource, &fixture).failed());
    CHECK(server.unbind_resource("gui://nothing").failed());
    CHECK(server.unbind_prompt("nothing").failed());

    // Starting twice is refused; closed ends are refused.
    CHECK(server.start(stxlib::PipeReader(), stxlib::PipeWriter()).failed());
    OperationResult<PipePair> first = stxlib::create_pipe();
    OperationResult<PipePair> second = stxlib::create_pipe();
    CHECK(first.succeeded() && second.succeeded());
    if (first.failed() || second.failed()) return;
    PipePair in = static_cast<OperationResult<PipePair>&&>(first).take_value();
    PipePair out = static_cast<OperationResult<PipePair>&&>(second).take_value();
    CHECK(server.start(static_cast<stxlib::PipeReader&&>(in.reader), static_cast<stxlib::PipeWriter&&>(out.writer))
              .succeeded());
    OperationResult<PipePair> third = stxlib::create_pipe();
    OperationResult<PipePair> fourth = stxlib::create_pipe();
    if (third.succeeded() && fourth.succeeded()) {
        PipePair again_in = static_cast<OperationResult<PipePair>&&>(third).take_value();
        PipePair again_out = static_cast<OperationResult<PipePair>&&>(fourth).take_value();
        const OperationStatus again = server.start(static_cast<stxlib::PipeReader&&>(again_in.reader),
                                                   static_cast<stxlib::PipeWriter&&>(again_out.writer));
        CHECK(again.failed() && (*again.error()).kind() == ErrorKind::invalid_argument);
    }
    in.writer.close();
    CHECK(server.wait() == ServeEnd::input_ended);
}

void client_identity() {
    // 131 bytes; byte 128 is inside the first é, so the cut backs off to 127.
    const std::string long_name = std::string(127, 'n') + "\xC3\xA9\xC3\xA9";
    const ClientIdentity cut = ClientIdentity::of(view_of(long_name), "1", "", "");
    CHECK(cut.name().count() == 127);
    CHECK(text_of(cut.name()) == std::string(127, 'n'));
    const ClientIdentity same = ClientIdentity::of(view_of(long_name), "1", "", "");
    CHECK(cut == same);
    CHECK(cut != ClientIdentity::of("other", "1", "", ""));
    CHECK(ClientIdentity().empty());
    CHECK(stxlib::mcp::legacy_protocol_version_count == 3);
    CHECK(text_of(stxlib::mcp::legacy_protocol_version(0)) == "2025-11-25");
}

// ---- the schema subset ------------------------------------------------------------------------------

std::string validation_problem(const TextView schema_text, const std::string& value) {
    OperationResult<stxlib::mcp::detail::Schema> compiled = stxlib::mcp::detail::Schema::compile(schema_text);
    CHECK(compiled.succeeded());
    if (compiled.failed()) return "<schema refused>";
    const stxlib::mcp::detail::Schema& schema = *compiled.value();
    OperationResult<JsonDocument> parsed = JsonDocument::parse(view_of(value));
    CHECK(parsed.succeeded());
    if (parsed.failed()) return "<value refused>";
    const JsonDocument& document = *parsed.value();
    Utf8String problem;
    const OperationStatus checked = schema.validate(document, document.root(), problem);
    if (checked.succeeded()) return "";
    return text_of(problem.view());
}

void schema_subset() {
    constexpr TextView shape = R"({"type":"object","properties":{)"
                              R"("count":{"type":"integer","minimum":1,"maximum":10},)"
                              R"("ratio":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":1},)"
                              R"("name":{"type":"string","minLength":2,"maxLength":3},)"
                              R"("mode":{"enum":["fast","slow",1]},)"
                              R"("fixed":{"const":true},)"
                              R"("list":{"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":3},)"
                              R"("any":{"type":["string","null"]},)"
                              R"("never":false,)"
                              R"("a/b":{"type":"object","additionalProperties":{"type":"boolean"},"maxProperties":1})"
                              R"(},"required":["count"],"additionalProperties":false})";
    CHECK(validation_problem(shape, R"({"count":5})").empty());
    CHECK(validation_problem(shape, R"({"count":5.0})").empty()); // 5.0 is an integer
    CHECK(contains(validation_problem(shape, R"({})"), "missing the required member count"));
    CHECK(validation_problem(shape, R"({"count":0})") == "/count: below the minimum");
    CHECK(validation_problem(shape, R"({"count":11})") == "/count: above the maximum");
    CHECK(validation_problem(shape, R"({"count":1.5})") == "/count: expected integer, got number");
    CHECK(contains(validation_problem(shape, R"({"count":1,"ratio":1})"), "/ratio"));
    CHECK(validation_problem(shape, R"({"count":1,"ratio":0.5})").empty());
    CHECK(contains(validation_problem(shape, R"({"count":1,"name":"a"})"), "minimum length"));
    CHECK(validation_problem(shape, R"({"count":1,"name":"ééé"})").empty()); // three scalars, six bytes
    CHECK(contains(validation_problem(shape, R"({"count":1,"name":"abcd"})"), "maximum length"));
    CHECK(validation_problem(shape, R"({"count":1,"mode":"slow"})").empty());
    CHECK(validation_problem(shape, R"({"count":1,"mode":1.0})").empty());
    CHECK(contains(validation_problem(shape, R"({"count":1,"mode":"other"})"), "allowed values"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"fixed":false})"), "required value"));
    CHECK(validation_problem(shape, R"({"count":1,"list":[1,"two"]})") == "/list/1: expected integer, got string");
    CHECK(contains(validation_problem(shape, R"({"count":1,"list":[]})"), "fewer items"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"list":[1,2,3,4]})"), "more items"));
    CHECK(validation_problem(shape, R"({"count":1,"any":null})").empty());
    CHECK(contains(validation_problem(shape, R"({"count":1,"any":3})"), "one of the declared types"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"never":1})"), "/never: no value"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"other":1})"), "/other: no value"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"a/b":{"x":1}})"), "/a~1b/x: expected boolean"));
    CHECK(contains(validation_problem(shape, R"({"count":1,"a/b":{"x":true,"y":true}})"), "more members"));
    CHECK(validation_problem(shape, R"([1])") == "(the arguments): expected object, got array");

    // Depth: 32 levels compile, 33 do not.
    std::string deep;
    for (int level = 0; level < 33; ++level) deep += R"({"type":"object","properties":{"x":)";
    deep += "true";
    for (int level = 0; level < 33; ++level) deep += "}}";
    OperationResult<stxlib::mcp::detail::Schema> too_deep = stxlib::mcp::detail::Schema::compile(view_of(deep));
    CHECK(too_deep.failed() && (*too_deep.error()).kind() == ErrorKind::limit_exceeded);

    OperationResult<stxlib::mcp::detail::Schema> draft7 = stxlib::mcp::detail::Schema::compile(
        R"({"$schema":"http://json-schema.org/draft-07/schema#","type":"object","title":"t","x-extension":1})");
    CHECK(draft7.succeeded());
    OperationResult<stxlib::mcp::detail::Schema> tuple =
        stxlib::mcp::detail::Schema::compile(R"({"type":"array","items":[{"type":"string"}]})");
    CHECK(tuple.failed());
    OperationResult<stxlib::mcp::detail::Schema> bad_type = stxlib::mcp::detail::Schema::compile(R"({"type":"text"})");
    CHECK(bad_type.failed());
    OperationResult<stxlib::mcp::detail::Schema> pattern =
        stxlib::mcp::detail::Schema::compile(R"({"type":"string","pattern":"^a"})");
    CHECK(pattern.failed() && (*pattern.error()).kind() == ErrorKind::unsupported);
    CHECK(contains(text_of((*pattern.error()).message()), "pattern"));
}

} // namespace

int main() {
    client_identity();
    schema_subset();
    refusals();
    modern_session();
    legacy_session();
    return stxlib_tests::finish("mcp");
}
