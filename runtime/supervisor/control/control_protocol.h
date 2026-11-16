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
// Request, request_header_bytes (48) plus a payload of payload_length bytes:
//
//   offset  size  field
//    0      4     magic, frame_magic ("STXC")
//    4      2     version, protocol_version
//    6      2     command (Command)
//    8      4     request_id, chosen by the service, echoed in the response
//   12      1     name_length, 0..service_name_bytes_max
//   13      1     zero
//   14      2     payload_length; the packet is exactly 48 + payload_length
//   16     32     name: a service name, [a-z][a-z0-9-]*, then zero bytes
//   48     ...    payload, by command (command_payload)
//
// A command that names a service (command_takes_name) has name_length 1 or
// more; any other has 0. A command's payload follows its PayloadRule: none
// (length 0), configuration text (1..config_text_max bytes, each a byte
// the configuration format allows or a line end; the configuration parser
// decides the rest), or an install source (InstallSource below). Every
// byte the layout does not use is zero. A frame that breaks any rule is
// malformed: decoding is strict, so each request has exactly one encoding.
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
//   target_log_tail              BootLogTail's layout for the named
//                                target's output (flag bit 0: output
//                                kept; status unknown_service for a
//                                name that is no service)
//   config_read                  ConfigText: header, then the user
//                                configuration's text
//   config_write                 ConfigWritten (8 bytes) with ok;
//                                ConfigRefusal (12 bytes) with
//                                config_refused
//   program_state, program_install, program_rollback, program_check
//                                ProgramRecord (104 bytes) when the target
//                                has a program (status ok, busy,
//                                signature_refused, source_unavailable);
//                                empty otherwise
//   operation_state              OperationRecord (48 bytes), status ok
//   reboot, system_update        empty
// Any status but ok or the ones listed carries an empty payload.
//
// Sizes: the largest request is 48 + 16384 bytes and the largest response
// 16 + 16392. FreeBSD's AF_UNIX SOCK_SEQPACKET send and receive space is
// 64 KiB at the pin (sys/kern/uipc_usrreq.c, unpsp_sendspace and
// unpsp_recvspace, both 64*1024, freebsd-src fb9033d2876d), so one packet
// of either always fits a fresh socket buffer.
//
// Commands 9 to 17 are the web interface's 1.0 pages (C2-web.md, section
// 4.3). The supervisor answers config_read, config_write and reboot
// itself; the program and system-update commands are carried out by the
// host side's targets, packages and updater (sb2's C2 note, K5 and K11 to
// K14) and answer unsupported until that side implements them.

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
constexpr std::size_t request_header_bytes = 48;
// The configuration format's size limit (config/boot_config.h,
// config_size_max; checked by a static assertion in control_server.cpp).
constexpr std::size_t config_text_max = 16384;
constexpr std::size_t request_payload_max = config_text_max;
constexpr std::size_t request_frame_bytes_max = request_header_bytes + request_payload_max;
constexpr std::size_t response_header_bytes = 16;
constexpr std::size_t config_text_header_bytes = 8;
constexpr std::size_t response_payload_max = config_text_header_bytes + config_text_max;
constexpr std::size_t response_frame_bytes_max = response_header_bytes + response_payload_max;
constexpr std::size_t service_name_bytes_max = 32;
// The configuration holds at most 24 services and targets together
// (config/boot_config.h, services_max; checked by a static assertion in
// control_server.cpp). The supervisor and the web service ship in the same
// image, so the list grew from C2's first 16 with no older peer to serve.
constexpr std::size_t services_listed_max = 24;
constexpr std::size_t supervisor_state_record_bytes = 16;
constexpr std::size_t service_record_bytes = 56;
constexpr std::size_t service_list_header_bytes = 4;
constexpr std::size_t slot_record_bytes = 8;
constexpr std::size_t boot_log_header_bytes = 4;
// The boot log text a response carries at most (kept at C2's first size:
// the page shows the end of the log, not all of it).
constexpr std::size_t boot_log_text_max = 3996;
constexpr std::size_t config_written_bytes = 8;
constexpr std::size_t config_refusal_bytes = 12;
constexpr std::size_t program_record_bytes = 104;
constexpr std::size_t program_version_bytes_max = 31;
constexpr std::size_t operation_record_bytes = 48;
constexpr std::size_t install_source_header_bytes = 4;
constexpr std::size_t install_url_bytes_max = 1024;

// The files a web upload leaves for the supervisor's side to take: fixed
// names, so no request ever carries a path. The web service writes them
// (runtime/web/upload_store.cpp); the host side reads them.
constexpr const char* staging_directory = "/user/stx/staging";
constexpr const char* staged_package_path = "/user/stx/staging/program.pkg";
constexpr const char* staged_system_path = "/user/stx/staging/system.img";

static_assert(service_list_header_bytes + services_listed_max * service_record_bytes <= response_payload_max,
              "a full service list fits one response");
static_assert(boot_log_header_bytes + boot_log_text_max <= response_payload_max, "a log tail fits one response");
static_assert(response_payload_max <= 0xffffu, "payload_length is 16 bits");
static_assert(request_payload_max <= 0xffffu, "payload_length is 16 bits");

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
    // The user configuration (config/user_config.h), read and written by
    // turns on the user partition; it takes effect at the next boot.
    config_read = 9,
    config_write = 10,
    // Stop every service, then reboot.
    reboot = 11,
    // A target's program (signed package, two slots on the user partition).
    program_state = 12,
    program_install = 13,
    program_rollback = 14,
    program_check = 15,
    // Write the staged system image to the inactive slot and set it to try.
    system_update = 16,
    // The long operation running or last finished (install, check, update).
    operation_state = 17,
    // The end of a target's own output (K17, sb2): standard output and
    // error, kept by the supervisor in memory (target_log.h). Read access.
    target_log_tail = 18,
};
constexpr std::uint16_t command_first = 1;
constexpr std::uint16_t command_last = 18;

// What a command's request carries after its header.
enum class PayloadRule : std::uint8_t {
    none,
    config_text,
    install_source,
};

// The command of that wire code, or nothing.
[[nodiscard]] std::optional<Command> command_from_code(std::uint16_t code) noexcept;
[[nodiscard]] const char* command_name(Command command) noexcept;
// Whether the request names a service (or a target, for program commands).
[[nodiscard]] bool command_takes_name(Command command) noexcept;
// Whether the command changes something (needs "control = operate").
[[nodiscard]] bool command_operates(Command command) noexcept;
[[nodiscard]] PayloadRule command_payload(Command command) noexcept;

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
    // config_write: the text does not parse, or breaks a user-configuration
    // rule; the ConfigRefusal says where.
    config_refused = 9,
    // This supervisor does not carry out the command (yet).
    unsupported = 10,
    // Writing to the user partition failed, or the store is full.
    store_failed = 11,
    // Another long operation is running.
    busy = 12,
    // A package or image whose signature or manifest does not check.
    signature_refused = 13,
    // The staged file is absent, or the download failed.
    source_unavailable = 14,
};
constexpr std::uint16_t status_last = 14;

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
    // Borrowed: the encoder's input, or the decoded frame's bytes. Empty
    // for commands whose PayloadRule is none.
    std::span<const std::uint8_t> payload;
};

// A request for a command without a name, or with one (asserted to match
// command_takes_name), with an empty payload.
[[nodiscard]] Request request_without_name(Command command, std::uint32_t request_id) noexcept;
[[nodiscard]] Request request_for_service(Command command, std::uint32_t request_id, const ServiceName& name) noexcept;
// The same request carrying payload (borrowed; it must outlive the request).
[[nodiscard]] Request request_with_payload(const Request& request, std::span<const std::uint8_t> payload) noexcept;

// Whether payload follows the command's rule (what decoding checks).
[[nodiscard]] bool payload_fits_rule(PayloadRule rule, std::span<const std::uint8_t> payload) noexcept;

// Writes the request into frame. Returns the frame's size, or nothing when
// the request breaks a rule (name against command, payload against its
// rule) or frame is too small.
[[nodiscard]] std::optional<std::size_t> encode_request(const Request& request, std::span<std::uint8_t> frame) noexcept;

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
    payload_refused,
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

// The decoded request's payload borrows frame, which must outlive it.
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
//   13  1  service_count, 0..24
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
//   36  1  flags: bit 0 a restart by request is pending; bit 1 stopped by
//              request; other bits zero
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
    // Only with state stopped.
    bool stopped_by_request;
    std::int32_t pid;
    std::uint32_t restart_count;
    std::uint64_t state_ms;
};
[[nodiscard]] std::size_t encode_service_record(const ServiceRecord& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ServiceRecord> decode_service_record(std::span<const std::uint8_t> bytes) noexcept;

// ServiceList: 1 byte count (0..24), 3 zero bytes, then count records.
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

// ConfigText, the config_read payload: an 8-byte header, then the text.
//    0  1  source: 0 none (no user configuration yet), 1 file 0, 2 file 1
//    1  1  flags: bit 0 written since this supervisor started (the
//              running services do not follow it until the next boot);
//              others zero
//    2  2  zero
//    4  4  sequence: the turn's sequence number, 0 with source none
//    8 ... the text, at most config_text_max bytes; none with source none
enum class ConfigSource : std::uint8_t { none = 0, file_0 = 1, file_1 = 2 };
[[nodiscard]] const char* config_source_name(ConfigSource source) noexcept;
struct ConfigTextView {
    ConfigSource source;
    bool written_since_boot;
    std::uint32_t sequence;
    std::span<const std::uint8_t> text;
};
// Writes the header only; the caller places the text after it.
[[nodiscard]] std::size_t encode_config_text_header(ConfigSource source, bool written_since_boot,
                                                    std::uint32_t sequence, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ConfigTextView> decode_config_text(std::span<const std::uint8_t> payload) noexcept;

// ConfigWritten, 8 bytes: 0 sequence (4), 4 file index 0 or 1 (1), 5 zero (3).
struct ConfigWritten {
    std::uint32_t sequence;
    std::uint8_t file_index;
};
[[nodiscard]] std::size_t encode_config_written(const ConfigWritten& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ConfigWritten> decode_config_written(std::span<const std::uint8_t> bytes) noexcept;

// ConfigRefusal, 12 bytes:
//    0  4  line (1-based)
//    4  4  column (1-based)
//    8  1  kind: 0 the parser refused it (code is a stxconfig::ParseError),
//              1 a user-configuration rule (code is a stxconfig::UserRule)
//    9  1  code
//   10  2  zero
// The codes are checked against their enumerations by the reader (the web
// service names them); the protocol carries them as numbers.
enum class RefusalKind : std::uint8_t { parse = 0, user_rule = 1 };
struct ConfigRefusal {
    std::uint32_t line;
    std::uint32_t column;
    RefusalKind kind;
    std::uint8_t code;
};
[[nodiscard]] std::size_t encode_config_refusal(const ConfigRefusal& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ConfigRefusal> decode_config_refusal(std::span<const std::uint8_t> bytes) noexcept;

// InstallSource, the program_install payload:
//    0  1  kind: 1 the staged package (staged_package_path), 2 a URL
//    1  3  zero
//    4 ... the URL, 1..install_url_bytes_max bytes of printable ASCII
//              without space, starting "https://"; nothing for kind 1
enum class InstallKind : std::uint8_t { staged = 1, url = 2 };
struct InstallSource {
    InstallKind kind;
    // Borrowed; empty for staged.
    std::span<const char> url;
};
// Into out (at least install_source_header_bytes + install_url_bytes_max);
// nothing when the source breaks the rules above.
[[nodiscard]] std::optional<std::size_t> encode_install_source(const InstallSource& source,
                                                               std::span<std::uint8_t> out) noexcept;
// Borrows payload.
[[nodiscard]] std::optional<InstallSource> decode_install_source(std::span<const std::uint8_t> payload) noexcept;

// ProgramRecord, 104 bytes:
//    0  1  name_length, 1..32
//    1 32  the target's name, then zero bytes
//   33  1  active slot: 0 none (no program installed), 1 A, 2 B
//   34  1  last good slot: 0 none, 1 A, 2 B
//   35  1  flags: bit 0 the active slot is on trial (not yet kept after
//              its healthy time); bit 1 an update is known to be available;
//              others zero
//   36  1  failed starts of the slot on trial, saturating at 255
//   37  3  zero
//   40  1  version A length, 0..31 (0: slot A empty)
//   41 31  version A, printable ASCII without space, then zero bytes
//   72  1  version B length, 0..31
//   73 31  version B
// The version's form is the package manifest's (sb2's C2 note, K12).
struct ProgramVersion {
    std::uint8_t length;
    char bytes[program_version_bytes_max];
};
struct ProgramRecord {
    ServiceName name;
    SlotCode active;
    SlotCode last_good;
    bool on_trial;
    bool update_available;
    std::uint8_t failed_starts;
    ProgramVersion version_a;
    ProgramVersion version_b;
};
[[nodiscard]] std::optional<ProgramVersion> program_version_of(std::span<const char> text) noexcept;
[[nodiscard]] std::size_t encode_program_record(const ProgramRecord& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<ProgramRecord> decode_program_record(std::span<const std::uint8_t> bytes) noexcept;

// OperationRecord, 48 bytes:
//    0  1  kind: 0 none since boot, 1 system update, 2 program install,
//              3 program check
//    1  1  state: 0 idle (kind none), 1 running, 2 succeeded, 3 failed
//    2  2  result: the Status it finished with (0 while running)
//    4  4  elapsed_ms: running, since it began; finished, how long it took;
//              saturating
//    8  1  name_length: the target's, 1..32, for program kinds; 0 otherwise
//    9 32  name, then zero bytes
//   41  7  zero
enum class OperationKind : std::uint8_t { none = 0, system_update = 1, program_install = 2, program_check = 3 };
enum class OperationState : std::uint8_t { idle = 0, running = 1, succeeded = 2, failed = 3 };
[[nodiscard]] const char* operation_kind_name(OperationKind kind) noexcept;
[[nodiscard]] const char* operation_state_name(OperationState state) noexcept;
struct OperationRecord {
    OperationKind kind;
    OperationState state;
    Status result;
    std::uint32_t elapsed_ms;
    // length 0 unless kind is a program kind.
    ServiceName name;
};
// The record for "nothing since boot".
[[nodiscard]] OperationRecord operation_none() noexcept;
// Whether a record keeps the rules above (kind none exactly when idle; a
// failed operation has a status but ok, any other ok; a name exactly for
// program kinds, and a valid one). encode asserts it; decode checks it.
[[nodiscard]] bool operation_record_valid(const OperationRecord& record) noexcept;
[[nodiscard]] std::size_t encode_operation_record(const OperationRecord& record, std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::optional<OperationRecord> decode_operation_record(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace stxcontrol

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CONTROL_PROTOCOL_H
