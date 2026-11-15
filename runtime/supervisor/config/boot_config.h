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
//
// A service with a folder is confined (C2, appliance/C2_DESIGN.md,
// "Services"): it runs in a jail of its own that sees its folder
// read-write at /srv (its working directory), the system's programs,
// libraries and /etc read-only (without /etc/stx), a /tmp of its own, and
// only the devices it names. Its command's program is a path as the jail
// sees it (/srv/... or a system directory).
//
//   folder = /user/services/web     optional: a directory below /user (the
//                                   user part), absolute, without "."
//                                   or ".." components, empty components or
//                                   a trailing '/', in [A-Za-z0-9._/-]; not
//                                   with directory
//   devices = bpf0 "usb/*"          optional, confined only: devfs path
//                                   patterns (devfs.rules(5)) of the devices
//                                   the jail sees besides null, zero, random,
//                                   urandom, fd and the standard streams; 1 to
//                                   devices_max words of [A-Za-z0-9._/*?+-],
//                                   each shorter than devfs's 200-byte
//                                   pattern, no leading '/' or ".." component
//   network = inherit               optional, confined only: none (the
//                                   default: no network) or inherit (the
//                                   host's addresses)
//
// A service without a folder is a system service: unconfined, as in C1.
// Only the configuration's author can define one; the services console and
// the web interface create confined services only.
//
// Any other setting in a service section is refused, and every start in the
// init section must name a service the file defines.
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
constexpr std::size_t services_max = 16;
// The prefix that makes a section a service definition.
constexpr const char* service_section_prefix = "service-";
// A confined service's device patterns.
constexpr std::size_t devices_max = 8;
// devfs keeps a pattern in a 200-byte array with its NUL
// (sys/fs/devfs/devfs.h:80, DEVFS_MAXPTRNLEN, at fb9033d2876d).
constexpr std::size_t device_pattern_length_max = 199;
// The directory a confined service's folder must be below: the user part.
constexpr const char* service_folder_parent = "/user";

// Every stored text (a name, a value, a word) is copied, unescaped and
// NUL-terminated, into the configuration's text store. A stored text is
// never longer than the input bytes it came from, and every stored text but
// the file's last is followed in the input by at least one byte it does not
// store (a blank, '=', ']' or a line end), so one copy of the file needs at
// most config_size_max + 1 bytes. A service's command and its devices are
// each stored twice, as the setting's value and as its words; no byte of
// the file belongs to two settings, so the second copies together are at
// most one more copy of the file, and the store holds two copies. The
// parser still checks capacity on every copy.
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

// A confined service's network.
enum class NetworkAccess : std::uint8_t {
    none,     // no addresses: the jail cannot use the network
    inherit,  // the host's addresses
};

struct ServiceDefinition {
    // The name after the section prefix ("web" for [service-web]).
    TextRef name;
    // The command's words: argument_count is 1..run_arguments_max and
    // arguments[0] is the program's absolute path.
    std::uint32_t argument_count;
    TextRef arguments[run_arguments_max];
    // Absent: the service runs in "/". Never present with folder.
    std::optional<TextRef> directory;
    // Present: the service is confined (see the format above).
    std::optional<TextRef> folder;
    // Device patterns, confined services only: device_count is
    // 0..devices_max.
    std::uint32_t device_count;
    TextRef devices[devices_max];
    // none unless the file says inherit; meaningful for confined services.
    NetworkAccess network;
    RestartPolicy restart;
    // The section header's line.
    std::uint32_t line;
};

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
    folder_outside_user,
    folder_with_directory,
    confinement_without_folder,
    device_pattern_malformed,
    too_many_devices,
    service_network_unknown,
    sysctl_malformed,
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
