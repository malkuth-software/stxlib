// control_protocol.h: the supervisor's control channel, its fixed message
// format (C2; design dev/notes/planning/stages/C2-web.md, section 4).
//
// A service whose definition says "control = read" or "control = operate"
// (config/boot_config.h) is started with one end of an AF_UNIX
// SOCK_SEQPACKET socket pair as its descriptor 3; the supervisor keeps the
// other end (control_channels.h). The channel is the authority: no path in
// the file system, nothing another process can connect to. The service
// sends one request at a time and waits for its response; each request and
// each response is exactly one packet.
//
// Pure code: no system call, no operating-system header, no allocation. Both
// sides (the supervisor and the web service, runtime/web/) use it, and its
// host tests (test/test_control_protocol.cpp) check it exhaustively within
// small bounds.
//
// Every integer is little-endian, written and read byte by byte, so the
// layout is ours and not a C++ structure's.
//
// Request, exactly request_frame_bytes (48):
//
//   offset  size  field
//    0      4     magic, frame_magic ("STXC")
//    4      2     version, protocol_version
//    6      2     command (Command)
//    8      4     request_id, chosen by the service, echoed in the response
//   12      1     name_length, 0..service_name_bytes_max
//   13      3     zero
//   16     32     name: a service name, [a-z][a-z0-9-]*, then zero bytes
//
// A command that names a service (command_takes_name) has name_length 1 or
// more; any other has 0. Every byte the layout does not use is zero. A
// frame that breaks any rule is malformed: decoding is strict, so each
// request has exactly one encoding.
//
// Response, response_header_bytes (16) plus at most response_payload_max:
//
//    0      4     magic
//    4      2     version
//    6      2     command, echoed
//    8      4     request_id, echoed
//   12      2     status (Status)
//   14      2     payload_length; the packet is exactly 16 + payload_length
//   16      ...   payload, by command (the records below)
//
// Payloads:
//   supervisor_state             SupervisorStateRecord (16 bytes), status ok
//   list_services                ServiceList: count, then count records
//   service_state, start, stop,  ServiceRecord (56 bytes) when the service
//   restart                      exists (status ok, already_running,
//                                not_running, spawn_failed, refused_self);
//                                empty otherwise
//   slot_status                  SlotRecord (8 bytes)
//   boot_log_tail                BootLogTail: flags, then text
// Any status but ok or the ones listed carries an empty payload.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_PROTOCOL_H
#define STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxcontrol {

// ---------------------------------------------------------------------------
// Limits and constants.

// The bytes 'S' 'T' 'X' 'C' in order, read as a little-endian integer.
constexpr std::uint32_t frame_magic = 0x43585453u;
constexpr std::uint16_t protocol_version = 1;
constexpr std::size_t request_frame_bytes = 48;
constexpr std::size_t response_header_bytes = 16;
// Small enough that one response always fits a fresh socket buffer: the
// AF_UNIX SOCK_SEQPACKET send space defaults to 8192 bytes (unconfirmed at
// the pin: sys/kern/uipc_usrreq.c, unpsp_sendspace; to cite).
constexpr std::size_t response_payload_max = 4000;
constexpr std::size_t response_frame_bytes_max = response_header_bytes + response_payload_max;
constexpr std::size_t service_name_bytes_max = 32;
// The configuration holds at most 16 services (config/boot_config.h,
// services_max; checked by a static assertion in control_server.cpp).
constexpr std::size_t services_listed_max = 16;
constexpr std::size_t supervisor_state_record_bytes = 16;
constexpr std::size_t service_record_bytes = 56;
constexpr std::size_t service_list_header_bytes = 4;
constexpr std::size_t slot_record_bytes = 8;
constexpr std::size_t boot_log_header_bytes = 4;
// The boot log text a response carries at most.
constexpr std::size_t boot_log_text_max = response_payload_max - boot_log_header_bytes;

static_assert(service_list_header_bytes + services_listed_max * service_record_bytes <= response_payload_max,
              "a full service list fits one response");
static_assert(response_payload_max <= 0xffffu, "payload_length is 16 bits");

// ---------------------------------------------------------------------------
// Commands and statuses.

enum class Command : std::uint16_t {
    supervisor_state = 1,
    list_services = 2,
    service_state = 3,
    start = 4,
    stop = 5,
    restart = 6,
    slot_status = 7,
    boot_log_tail = 8,
};
constexpr std::uint16_t command_first = 1;
constexpr std::uint16_t command_last = 8;

// The command of that wire code, or nothing.
[[nodiscard]] std::optional<Command> command_from_code(std::uint16_t code) noexcept;
[[nodiscard]] const char* command_name(Command command) noexcept;
// Whether the request names a service.
[[nodiscard]] bool command_takes_name(Command command) noexcept;
// Whether the command changes a service (needs "control = operate").
[[nodiscard]] bool command_operates(Command command) noexcept;

enum class Status : std::uint16_t {
    ok = 0,
    malformed = 1,
    not_permitted = 2,
    unknown_service = 3,
    already_running = 4,
    not_running = 5,
    spawn_failed = 6,
    refused_self = 7,
    unavailable = 8,
};
constexpr std::uint16_t status_last = 8;

[[nodiscard]] std::optional<Status> status_from_code(std::uint16_t code) noexcept;
[[nodiscard]] const char* status_name(Status status) noexcept;

// ---------------------------------------------------------------------------
// Service names on the wire.

struct ServiceName {
    std::uint8_t length;
    char bytes[service_name_bytes_max];
};

// A name as the configuration allows it: [a-z][a-z0-9-]*, 1..32 bytes.
[[nodiscard]] bool service_name_valid(std::span<const char> name) noexcept;
// The name of those bytes, or nothing when they are not a valid name.
[[nodiscard]] std::optional<ServiceName> service_name_of(std::span<const char> name) noexcept;
// The NUL-terminated copy of a name; destination holds 33 bytes.
void service_name_c_string(const ServiceName& name, char (&destination)[service_name_bytes_max + 1]) noexcept;
[[nodiscard]] bool service_names_equal(const ServiceName& left, const ServiceName& right) noexcept;

// ---------------------------------------------------------------------------
// Requests.

struct Request {
    Command command;
    std::uint32_t request_id;
    // length 0 for commands that name no service.
    ServiceName name;
};

// A request for a command without a name, or with one (asserted to match
// command_takes_name).
[[nodiscard]] Request request_without_name(Command command, std::uint32_t request_id) noexcept;
[[nodiscard]] Request request_for_service(Command command, std::uint32_t request_id, const ServiceName& name) noexcept;

// Writes the request's 48 bytes. Requires a well-formed request (asserted):
// what the factories above build.
void encode_request(const Request& request, std::span<std::uint8_t, request_frame_bytes> frame) noexcept;

// Why a request frame was refused.
enum class RequestError : std::uint8_t {
    wrong_size,
    wrong_magic,
    wrong_version,
    unknown_command,
    name_too_long,
    name_invalid,
    name_missing,
    name_unexpected,
    reserved_not_zero,
};
[[nodiscard]] const char* request_error_name(RequestError error) noexcept;

struct RequestRefused {
    RequestError error;
    // Echoed in the malformed response: the frame's request_id and command
    // fields when the frame is long enough to hold them, else 0.
    std::uint32_t request_id;
    std::uint16_t command_code;
};

class RequestOutcome {
public:
    [[nodiscard]] static RequestOutcome decoded(const Request& request) noexcept;
    [[nodiscard]] static RequestOutcome refused(const RequestRefused& refusal) noexcept;
    [[nodiscard]] const Request* as_request() const noexcept;
    [[nodiscard]] const RequestRefused* as_refused() const noexcept;

private:
    using Alternatives = std::variant<Request, RequestRefused>;
    explicit RequestOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

[[nodiscard]] RequestOutcome decode_request(std::span<const std::uint8_t> frame) noexcept;

// ---------------------------------------------------------------------------
// Responses.

struct ResponseHeader {
    std::uint16_t command_code;
    std::uint32_t request_id;
    Status status;
};

// Writes header and payload into frame. Returns the frame's size, or
// nothing when the payload is longer than response_payload_max or the
// frame is too small; frame is then unspecified. payload and frame must
// not overlap.
[[nodiscard]] std::optional<std::size_t> encode_response(const ResponseHeader& header,
                                                         std::span<const std::uint8_t> payload,
                                                         std::span<std::uint8_t> frame) noexcept;

// A decoded response: its header and its payload, borrowed from the frame.
struct ResponseView {
    Command command;
    std::uint32_t request_id;
    Status status;
    std::span<const std::uint8_t> payload;
};

enum class ResponseError : std::uint8_t {
    too_short,
    wrong_magic,
    wrong_version,
    unknown_command,
    unknown_status,
    wrong_length,
};
[[nodiscard]] const char* response_error_name(ResponseError error) noexcept;

class ResponseOutcome {
public:
    [[nodiscard]] static ResponseOutcome decoded(const ResponseView& view) noexcept;
    [[nodiscard]] static ResponseOutcome refused(ResponseError error) noexcept;
    [[nodiscard]] const ResponseView* as_view() const noexcept;
    [[nodiscard]] const ResponseError* as_refused() const noexcept;

private:
    using Alternatives = std::variant<ResponseView, ResponseError>;
    explicit ResponseOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// The view borrows frame, which must outlive it. A malformed response
// (from the client's side, a malformed command echo counts too) is refused.
[[nodiscard]] ResponseOutcome decode_response(std::span<const std::uint8_t> frame) noexcept;

// ---------------------------------------------------------------------------
// Payload records. Each encode writes exactly the record's size into the
// front of out (asserted large enough) and returns that size; each decode
// takes exactly the record's bytes and is strict (reserved bytes zero, codes
// in range), returning nothing otherwise.

// SupervisorStateRecord, 16 bytes:
//    0  8  uptime_ms
//    8  4  restart_count; 0xffffffff when unknown (saturates below it)
//   12  1  flags: bit 0 shim registered, 1 init done, 2 failsafe taken,
//              3 boot marked good; other bits zero
//   13  1  service_count, 0..16
//   14  2  zero
constexpr std::uint32_t restart_count_unknown = 0xffffffffu;
struct SupervisorStateRecord {
    std::uint64_t uptime_ms;
    std::optional<std::uint32_t> restart_count;
    bool shim_registered;
    bool init_done;
    bool failsafe_taken;
    bool boot_marked_good;
    std::uint8_t service_count;
};
[[nodiscard]] std::size_t encode_supervisor_state(const SupervisorStateRecord& record,
                                                  std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<SupervisorStateRecord> decode_supervisor_state(std::span<const std::uint8_t> bytes) noexcept;

// ServiceRecord, 56 bytes:
//    0  1  name_length, 1..32
//    1 32  name, then zero bytes
//   33  1  state: 0 stopped, 1 running, 2 waiting (to restart), 3 stopping
//   34  1  restart policy: 0 never, 1 on-failure, 2 always
//   35  1  control access: 0 none, 1 read, 2 operate
//   36  1  flags: bit 0 a restart by request is pending; other bits zero
//   37  3  zero
//   40  4  pid (two's complement), 0 when stopped or waiting
//   44  4  restart_count, saturating at 0xffffffff
//   48  8  state_ms: running or stopping, ms since the process started;
//              waiting, ms until the restart; stopped, 0
enum class ServiceStateCode : std::uint8_t { stopped = 0, running = 1, waiting = 2, stopping = 3 };
enum class RestartCode : std::uint8_t { never = 0, on_failure = 1, always = 2 };
enum class AccessCode : std::uint8_t { none = 0, read = 1, operate = 2 };
[[nodiscard]] const char* service_state_code_name(ServiceStateCode code) noexcept;
[[nodiscard]] const char* restart_code_name(RestartCode code) noexcept;
[[nodiscard]] const char* access_code_name(AccessCode code) noexcept;

struct ServiceRecord {
    ServiceName name;
    ServiceStateCode state;
    RestartCode restart;
    AccessCode control;
    bool restart_pending;
    std::int32_t pid;
    std::uint32_t restart_count;
    std::uint64_t state_ms;
};
[[nodiscard]] std::size_t encode_service_record(const ServiceRecord& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ServiceRecord> decode_service_record(std::span<const std::uint8_t> bytes) noexcept;

// ServiceList: 1 byte count (0..16), 3 zero bytes, then count records.
// encode_service_list writes the header for count records; the caller
// writes each record after it. decode_service_list_count checks the
// header against the payload's size and returns the count.
[[nodiscard]] std::size_t encode_service_list_header(std::uint8_t count, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<std::uint8_t> decode_service_list_count(std::span<const std::uint8_t> payload) noexcept;
// The bytes of record index in a list payload already checked by
// decode_service_list_count (index below the count, asserted).
[[nodiscard]] std::span<const std::uint8_t> service_list_record(std::span<const std::uint8_t> payload,
                                                                std::size_t index) noexcept;

// SlotRecord, 8 bytes:
//    0  1  booted slot: 0 none (no slot loader), 1 A, 2 B
//    1  1  tries, saturating at 255
//    2  1  flags: bit 0 fallback, bit 1 this boot marked good; others zero
//    3  1  last good slot: 0 unknown, 1 A, 2 B
//    4  4  zero
enum class SlotCode : std::uint8_t { none = 0, a = 1, b = 2 };
[[nodiscard]] const char* slot_code_name(SlotCode code) noexcept;
struct SlotRecord {
    SlotCode booted;
    std::uint8_t tries;
    bool fallback;
    bool marked_good;
    SlotCode last_good;
};
[[nodiscard]] std::size_t encode_slot_record(const SlotRecord& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<SlotRecord> decode_slot_record(std::span<const std::uint8_t> bytes) noexcept;

// BootLogTail: 1 byte flags (bit 0 the log exists, bit 1 its front was cut
// to fit; others zero), 3 zero bytes, then at most boot_log_text_max bytes
// of the log's end, whole lines. A log that does not exist has no text.
struct BootLogTailView {
    bool present;
    bool front_cut;
    std::span<const std::uint8_t> text;
};
[[nodiscard]] std::size_t encode_boot_log_header(bool present, bool front_cut, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<BootLogTailView> decode_boot_log_tail(std::span<const std::uint8_t> payload) noexcept;

}  // namespace stxcontrol

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_PROTOCOL_H
