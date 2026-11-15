// boot_config.h: the appliance's boot configuration, its text format and its
// parser (C1 step S2; owner decision Q4 = 1: the init script is the config
// format with a small command set).
//
// Pure code: no system call, no operating-system header, no allocation. It
// builds on any C++20 host, so the parser is tested on the host as well as
// run by the supervisor on the appliance.
//
// The format, one text file of at most config_size_max bytes:
//
//   # a comment: a line whose first non-blank character is '#'
//   [system]                      a section header: a name in brackets
//   hostname = stx-vm             a setting: name = value (any section but init)
//
//   [init]                        the init section: one command per line,
//   mount ufs /dev/gpt/user /user rw         run in order, once per boot
//   wait-for device /dev/gpt/user 2000
//   if-present file /user/autorun run /user/autorun
//   set LANG C.UTF-8
//   start web
//   run /sbin/thing "an argument" second
//
// Lines end in LF; one CR before the LF (or at the end of the text) is
// accepted (files edited on other machines' FAT32 user parts). Bytes are printable ASCII, space and tab;
// anything else is refused. Names are [a-z][a-z0-9-]*, at most
// name_length_max characters. A setting's value is the rest of its line
// without the surrounding blanks, possibly empty, taken literally.
//
// An init command is words separated by blanks. A word is bare (printable
// characters but blank and '"') or quoted ("...", where \" and \\ stand for
// " and \; it must be followed by a blank or the line's end). The commands:
//
//   mount <ufs|msdosfs> <device> <directory> <ro|rw>
//   wait-for <device|file> <path> <milliseconds>    at most wait_ms_max
//   set <NAME> <value>          an environment variable for later runs
//   start <service>             a service defined in the file (S3)
//   run <program> [argument...] an absolute path; waits for it to exit
//   sysctl <name>=<value>       sets a kernel variable, as sysctl(8) would
//                               (C2): one word, the name, '=', the value;
//                               the value is read by the variable's type
//                               when the command runs (sysctl_value.h)
//   if-present <device|file> <path> <command>       the command runs only
//                                                   when the path exists
//
// A service is a section named service-<name> (C1 step S3), holding:
//
//   [service-web]
//   command = /usr/local/sbin/webd --port 8443     required; words as in an
//                                                  init command, the program
//                                                  an absolute path
//   directory = /user/services/web                 optional, absolute; "/"
//   restart = on-failure                           optional: never,
//                                                  on-failure (default), always
//   control = operate                              optional: none (default),
//                                                  read, operate: the
//                                                  supervisor's control
//                                                  channel (C2, control/)
//
// A service is a system service: it runs as root, as the configuration's
// author defined it. Only the configuration's author defines one.
//
// A target is a section named target-<name> (C2, appliance/C2_DESIGN.md,
// "Targets"): a registered program the appliance exists to run, from a
// signed package in the program store (/data/programs/<program>/), with
// plain separation and nothing added: its own numeric user and group, its
// folder, and only the listeners and devices it declares.
//
//   [target-radio]
//   role = server                   required: foreground (at most one target:
//                                   it holds the screen) or server
//   id = 7                          required: 1..target_id_max, unique among
//                                   targets; the target runs as uid and gid
//                                   target_identity_base + id. The writer of
//                                   the file issues ids and never reuses one
//   program = radio                 optional: the package's name, a name; the
//                                   target's own name by default
//   command = bin/radiod --port 8000   required: words as in an init command;
//                                   the first is a file of the package, a
//                                   relative path (package_path_valid)
//   folder = /data/targets/radio    required: the target's working and data
//                                   directory, below target_folder_parent;
//                                   the supervisor creates it, owned by the
//                                   target's user, mode 0700. No target's
//                                   folder is another's or inside another's
//   ports = tcp/8000 udp/5353       optional: the listeners it declares,
//                                   1 to ports_max words <tcp|udp>/<1-65535>;
//                                   no two targets declare the same one
//   devices = fb0 ttyv1 "input/*"   optional, foreground only: devfs path
//                                   patterns (devfs.rules(5)) of the devices
//                                   the boot ruleset gives to the target's
//                                   group; 1 to devices_max words of
//                                   [A-Za-z0-9._/*?+-], each shorter than
//                                   devfs's 200-byte pattern, no leading '/'
//                                   or ".." component
//   start = boot                    optional: boot (the default: started
//                                   after the init section, the foreground
//                                   target first) or manual
//   restart = always                optional, as for a service
//   update-url = https://example.com/radio/latest   optional: where the
//                                   update service asks for a newer version
//                                   (https only, at most a line)
//   update-interval = 86400         optional, with update-url: seconds
//                                   between checks, update_interval_min_s to
//                                   update_interval_max_s (default 86400)
//   healthy-after = 30              optional: seconds a newly installed
//                                   version must run before it is kept,
//                                   1 to healthy_after_max_s (default 30)
//
// A [network] section, when present, holds only these settings (stxnet
// applies them; any other is refused):
//
//   mode = dhcp                     dhcp (the default) or static
//   interface = genet0              optional: one interface name, or every
//                                   Ethernet interface when absent
//   address = 192.168.1.20/24       static only, required: IPv4 and prefix
//   gateway = 192.168.1.1           static only, optional: IPv4
//   dns = 192.168.1.1 9.9.9.9       static only, optional: 1 to 3 IPv4
//
// Any other setting in a service or target section is refused, and every
// start in the init section must name a service or target the file defines.
//
// if-present guards one command, which is not itself an if-present: guards
// do not nest. There are no loops, jumps or definitions, so an init section
// of n commands takes exactly n steps, each bounded in time by its own limit
// (init_executor.h). That is the termination property the format exists to
// give.
//
// Every limit below is checked before the parser stores anything that would
// exceed it; a refused file leaves the configuration empty.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_BOOT_CONFIG_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_BOOT_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace stxconfig {

// ---------------------------------------------------------------------------
// Limits. Each is a real bound of the format, not a buffer size chosen to
// be large enough.

constexpr std::size_t config_size_max = 16384;
constexpr std::size_t line_length_max = 255;
constexpr std::size_t name_length_max = 32;
constexpr std::size_t sections_max = 16;
constexpr std::size_t settings_max = 64;
constexpr std::size_t init_commands_max = 64;
// Words of one init command, its keyword and an if-present guard included.
constexpr std::size_t command_words_max = 20;
// A run's argument vector: the program and at most 15 arguments.
constexpr std::size_t run_arguments_max = 16;
constexpr std::uint32_t wait_ms_max = 60000;
// Services and targets together.
constexpr std::size_t services_max = 24;
// The prefixes that make a section a service or a target definition.
constexpr const char* service_section_prefix = "service-";
constexpr const char* target_section_prefix = "target-";
// A foreground target's device patterns.
constexpr std::size_t devices_max = 8;
// devfs keeps a pattern in a 200-byte array with its NUL
// (sys/fs/devfs/devfs.h:80, DEVFS_MAXPTRNLEN, at fb9033d2876d).
constexpr std::size_t device_pattern_length_max = 199;
// The directory a target's folder must be below: on the data partition
// (UFS2, which keeps owners and modes; appliance/disk/LAYOUT.md).
constexpr const char* target_folder_parent = "/data/targets";
// Target ids and the identities they give.
constexpr std::uint32_t target_id_max = 999;
constexpr std::uint32_t target_identity_base = 30000;
// A target's declared listeners.
constexpr std::size_t ports_max = 8;
// A path inside a package (package/manifest.h uses the same rule and bound).
constexpr std::size_t package_path_length_max = 200;
// The update service's interval and a trial's healthy time, in seconds.
constexpr std::uint32_t update_interval_min_s = 300;
constexpr std::uint32_t update_interval_max_s = 2592000;
constexpr std::uint32_t update_interval_default_s = 86400;
constexpr std::uint32_t healthy_after_default_s = 30;
constexpr std::uint32_t healthy_after_max_s = 3600;
// [network]: name servers.
constexpr std::size_t name_servers_max = 3;

// Every stored text (a name, a value, a word) is copied, unescaped and
// NUL-terminated, into the configuration's text store. A stored text is
// never longer than the input bytes it came from, and every stored text but
// the file's last is followed in the input by at least one byte it does not
// store (a blank, '=', ']' or a line end), so one copy of the file needs at
// most config_size_max + 1 bytes. A service's or target's command and a
// target's devices are each stored twice, as the setting's value and as its
// words; no byte of the file belongs to two settings, so the second copies
// together are at most one more copy of the file, and the store holds two
// copies. The parser still checks capacity on every copy.
constexpr std::size_t text_store_bytes = 2u * (config_size_max + 1u);

// ---------------------------------------------------------------------------
// Stored texts.

// A text in the configuration's store: its first byte's offset and its
// length, without the NUL that follows it. Offsets rather than pointers, so
// the configuration stays valid when it is moved.
struct TextRef {
    std::uint32_t offset;
    std::uint32_t length;
};

struct Section {
    TextRef name;
    std::uint32_t line;
};

struct Setting {
    std::uint32_t section_index;
    TextRef name;
    TextRef value;
    std::uint32_t line;
};

// ---------------------------------------------------------------------------
// Init commands.

enum class FileSystemKind : std::uint8_t {
    ufs,
    msdosfs,
};

enum class MountAccess : std::uint8_t {
    read_only,
    read_write,
};

// What a presence test or a wait looks for: a device node or a regular file.
enum class PresenceKind : std::uint8_t {
    device,
    file,
};

struct MountCommand {
    FileSystemKind file_system;
    TextRef device;
    TextRef directory;
    MountAccess access;
};

struct WaitForCommand {
    PresenceKind kind;
    TextRef path;
    std::uint32_t timeout_ms;
};

struct SetCommand {
    TextRef name;
    TextRef value;
};

struct StartCommand {
    TextRef service;
};

struct RunCommand {
    // argument_count is 1..run_arguments_max; arguments[0] is the program.
    std::uint32_t argument_count;
    TextRef arguments[run_arguments_max];
};

// sysctl name=value: the word as stored, and where its '=' is. The name is
// the word's first name_length bytes (not NUL-terminated in the store); the
// value is the rest after the '=', NUL-terminated, possibly empty.
struct SysctlCommand {
    TextRef setting;
    std::uint32_t name_length;
};

// One command's action, without its guard. Built only through the
// factories and read only through the queries below.
class CommandAction {
public:
    [[nodiscard]] static CommandAction mount(const MountCommand& command) noexcept;
    [[nodiscard]] static CommandAction wait_for(const WaitForCommand& command) noexcept;
    [[nodiscard]] static CommandAction set(const SetCommand& command) noexcept;
    [[nodiscard]] static CommandAction start(const StartCommand& command) noexcept;
    [[nodiscard]] static CommandAction run(const RunCommand& command) noexcept;
    [[nodiscard]] static CommandAction sysctl(const SysctlCommand& command) noexcept;

    [[nodiscard]] const MountCommand* as_mount() const noexcept;
    [[nodiscard]] const WaitForCommand* as_wait_for() const noexcept;
    [[nodiscard]] const SetCommand* as_set() const noexcept;
    [[nodiscard]] const StartCommand* as_start() const noexcept;
    [[nodiscard]] const RunCommand* as_run() const noexcept;
    [[nodiscard]] const SysctlCommand* as_sysctl() const noexcept;

    // The command's keyword as written in the file ("mount", "run", ...).
    [[nodiscard]] const char* keyword() const noexcept;

private:
    using Alternatives =
        std::variant<MountCommand, WaitForCommand, SetCommand, StartCommand, RunCommand, SysctlCommand>;
    explicit CommandAction(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

struct PresenceGuard {
    PresenceKind kind;
    TextRef path;
};

struct InitCommand {
    // Absent: the command always runs.
    std::optional<PresenceGuard> guard;
    CommandAction action;
    std::uint32_t line;
    // Column of the command's first word (for refusals found after the
    // line was read: a start naming no service).
    std::uint32_t column;
};

// ---------------------------------------------------------------------------
// Services.

enum class RestartPolicy : std::uint8_t {
    never,
    on_failure,
    always,
};

enum class TargetRole : std::uint8_t {
    foreground,  // holds the screen; at most one
    server,
};

enum class StartPolicy : std::uint8_t {
    boot,    // started after the init section
    manual,  // started only when asked (init start, the web interface)
};

enum class PortProtocol : std::uint8_t {
    tcp,
    udp,
};

struct PortDeclaration {
    PortProtocol protocol;
    std::uint16_t port;  // 1..65535
};

// What makes a definition a target (see the format above).
struct TargetSettings {
    TargetRole role;
    std::uint32_t id;  // 1..target_id_max
    // The package's name: the program setting, or the target's name.
    TextRef program;
    TextRef folder;
    std::uint32_t port_count;  // 0..ports_max
    PortDeclaration ports[ports_max];
    std::uint32_t device_count;  // 0..devices_max; 0 for a server
    TextRef devices[devices_max];
    StartPolicy start;
    std::optional<TextRef> update_url;
    std::uint32_t update_interval_s;
    std::uint32_t healthy_after_s;
};

// The uid and gid a target runs as.
[[nodiscard]] std::uint32_t target_identity(const TargetSettings& target) noexcept;

// What a service may ask of the supervisor through its control channel
// (control/control_protocol.h). none: the service gets no channel. read:
// the supervisor's state, the services, the slots and the boot log.
// operate: read, and start, stop and restart services.
enum class ControlAccess : std::uint8_t {
    none,
    read,
    operate,
};

struct ServiceDefinition {
    // The name after the section prefix ("web" for [service-web], "radio"
    // for [target-radio]); names are unique across both kinds.
    TextRef name;
    // The command's words: argument_count is 1..run_arguments_max. For a
    // service arguments[0] is the program's absolute path; for a target it
    // is a file of its package, relative (package_path_valid).
    std::uint32_t argument_count;
    TextRef arguments[run_arguments_max];
    // Services only. Absent: the service runs in "/".
    std::optional<TextRef> directory;
    // Present for a target, absent for a service.
    std::optional<TargetSettings> target;
    RestartPolicy restart;
    ControlAccess control;
    // The section header's line.
    std::uint32_t line;
};

// A path inside a package: 1..package_path_length_max bytes of
// [A-Za-z0-9._-] and '/', relative, with no empty, "." or ".." component
// and no trailing '/'.
[[nodiscard]] bool package_path_valid(const char* bytes, std::size_t length) noexcept;

// An IPv4 address in dotted-quad form (four decimal numbers 0..255 without
// leading zeros), as a host-order value, or nothing.
[[nodiscard]] std::optional<std::uint32_t> ipv4_of(const char* bytes, std::size_t length) noexcept;

// ---------------------------------------------------------------------------
// The configuration. Large (about 30 KiB) and filled in place by the
// parser: the caller owns one, usually with static storage, and reuses it.
// Not copyable, so it is never duplicated by accident.

class BootConfig {
public:
    BootConfig() noexcept;
    BootConfig(const BootConfig&) = delete;
    BootConfig& operator=(const BootConfig&) = delete;

    // Ends every stored element's lifetime; the configuration is then empty
    // (no section, setting or command), as after construction.
    void clear() noexcept;

    [[nodiscard]] std::size_t section_count() const noexcept;
    [[nodiscard]] std::size_t setting_count() const noexcept;
    [[nodiscard]] std::size_t init_command_count() const noexcept;
    // Whether the file had an [init] section (possibly empty).
    [[nodiscard]] bool has_init_section() const noexcept;

    // Indices must be below the matching count (asserted).
    [[nodiscard]] const Section& section(std::size_t index) const noexcept;
    [[nodiscard]] const Setting& setting(std::size_t index) const noexcept;
    [[nodiscard]] const InitCommand& init_command(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t service_count() const noexcept;
    [[nodiscard]] const ServiceDefinition& service(std::size_t index) const noexcept;
    // The service of that name, or nothing.
    [[nodiscard]] std::optional<std::size_t> find_service(const char* name) const noexcept;

    // The NUL-terminated text a reference names. The pointer is valid
    // while the configuration lives and is not cleared or parsed into again.
    [[nodiscard]] const char* text(const TextRef& reference) const noexcept;

    // A setting's value by section and name, or nothing.
    [[nodiscard]] std::optional<TextRef> find_setting(const char* section_name, const char* setting_name) const noexcept;

    // Bytes of the text store in use (for tests: never above text_store_bytes).
    [[nodiscard]] std::size_t text_store_used() const noexcept;

private:
    friend class ConfigBuilder;

    char text_store_[text_store_bytes];
    std::size_t text_store_used_;
    Section sections_[sections_max];
    std::size_t section_count_;
    Setting settings_[settings_max];
    std::size_t setting_count_;
    // InitCommand has no default constructor (CommandAction has none), so
    // the commands live in raw storage and are constructed as parsed.
    alignas(InitCommand) unsigned char init_command_storage_[init_commands_max * sizeof(InitCommand)];
    std::size_t init_command_count_;
    bool has_init_section_;
    // ServiceDefinition has a std::optional member but a trivial default
    // shape, so a plain array serves.
    ServiceDefinition services_[services_max];
    std::size_t service_count_;
};

// ---------------------------------------------------------------------------
// Parsing.

enum class ParseError : std::uint8_t {
    file_too_large,
    line_too_long,
    byte_not_allowed,
    header_malformed,
    name_malformed,
    section_repeated,
    too_many_sections,
    setting_outside_section,
    setting_malformed,
    setting_repeated,
    too_many_settings,
    quote_unterminated,
    escape_malformed,
    word_after_quote,
    quote_in_word,
    too_many_words,
    command_unknown,
    command_arguments_wrong,
    file_system_unknown,
    access_unknown,
    presence_kind_unknown,
    number_malformed,
    wait_too_long,
    variable_name_malformed,
    program_not_absolute,
    too_many_run_arguments,
    guard_nested,
    too_many_commands,
    text_store_full,
    service_setting_unknown,
    service_command_missing,
    service_restart_unknown,
    directory_not_absolute,
    too_many_services,
    start_unknown_service,
    // C2.
    folder_malformed,
    folder_outside_data,
    device_pattern_malformed,
    too_many_devices,
    sysctl_malformed,
    target_setting_missing,
    target_role_unknown,
    target_id_malformed,
    target_id_repeated,
    second_foreground,
    devices_not_foreground,
    port_malformed,
    too_many_ports,
    port_repeated,
    program_path_malformed,
    start_policy_unknown,
    update_url_malformed,
    update_interval_malformed,
    healthy_after_malformed,
    name_repeated,
    network_setting_unknown,
    network_value_malformed,
    folder_repeated,
    service_control_unknown,
    // A target setting in a [service-<name>] section after a service-only
    // setting (directory, control).
    target_setting_in_service,
};

// The error's name as printed on the console ("line_too_long", ...).
[[nodiscard]] const char* parse_error_name(ParseError error) noexcept;

struct ParseSucceeded {
};

// Where the file was refused: 1-based line and byte column.
struct ParseFailed {
    std::uint32_t line;
    std::uint32_t column;
    ParseError error;
};

class ParseOutcome {
public:
    [[nodiscard]] static ParseOutcome succeeded() noexcept;
    [[nodiscard]] static ParseOutcome failed(std::uint32_t line, std::uint32_t column, ParseError error) noexcept;

    [[nodiscard]] bool is_success() const noexcept;
    [[nodiscard]] const ParseFailed* as_failure() const noexcept;

private:
    using Alternatives = std::variant<ParseSucceeded, ParseFailed>;
    explicit ParseOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Parses text into config. config is cleared first; on success it holds
// the file's sections, settings and init commands; on failure it is empty
// again. text and config must not overlap (the text store is inside
// config). Reads exactly text.size() bytes; text need not be NUL-terminated.
[[nodiscard]] ParseOutcome parse_boot_config(std::span<const char> text, BootConfig& config) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_BOOT_CONFIG_H
