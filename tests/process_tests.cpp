// Tests of the process module's A1 platform-rest additions (process.hpp:
// process_arguments, process_id, process_memory; threads.hpp:
// is_main_thread; files.hpp: known_directory).
//
// CMakeLists.txt starts this program with the arguments listed in
// expected_arguments; started with any others (by hand), the comparisons
// with that list are skipped and only the agreement with argv is checked.
// With "arguments-hex" first, it is the child of lossless_arguments: it
// prints each of its own arguments after the mode, as process_arguments
// reads them, in hex, with a "u" or "n" for is_unicode.

#include "stxlib/capabilities.hpp"
#include "stxlib/desktop.hpp"
#include "stxlib/children.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/files.hpp"
#include "stxlib/path.hpp"
#include "stxlib/process.hpp"
#include "stxlib/text.hpp"
#include "stxlib/threads.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

using stxlib::ArgumentText;
using stxlib::Child;
using stxlib::ChildRequest;
using stxlib::ChildStreams;
using stxlib::Duration;
using stxlib::ErrorKind;
using stxlib::HostCode;
using stxlib::KnownDirectory;
using stxlib::Path;
using stxlib::PathText;
using stxlib::ProcessArguments;
using stxlib::OperationResult;
using stxlib::TextView;
using stxlib::usize;

// Test texts are valid UTF-8; an invalid one becomes the empty view, so the
// comparison using it fails.
TextView view_of(const char* const text) {
    const stxlib::OperationResult<TextView> valid = stxlib::validate_utf8(text, std::strlen(text));
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

// What CMakeLists.txt passes after the program name.
const char* const expected_arguments[] = {"plain", "two words", "", "quote\"inside", "trailing\\", "caf\xc3\xa9"};
constexpr int expected_count = 6;

bool same_bytes(const ArgumentText argument, const char* const text) {
    const usize length = std::strlen(text);
    if (argument.count() != length) return false;
    const bool result = length == 0 || std::memcmp(argument.bytes().data(), text, length) == 0;
    return result;
}

#if defined(_WIN32)
bool is_ascii(const char* const text) {
    for (const char* byte = text; *byte != 0; ++byte) {
        if (static_cast<unsigned char>(*byte) >= 0x80) return false;
    }
    return true;
}
#endif

template <typename T>
ErrorKind kind_of(const OperationResult<T>& result) {
    const ErrorKind kind = result.failed() ? (*result.error()).kind() : ErrorKind::internal_failure;
    return kind;
}

template <typename T>
HostCode code_of(const OperationResult<T>& result) {
    const HostCode code = result.failed() ? static_cast<HostCode>((*result.error()).code()) : HostCode::none;
    return code;
}

// ---- process_arguments ------------------------------------------------------

void arguments_agree_with_argv(const int argument_count, char** const arguments) {
    CHECK(stxlib::capability_supported(stxlib::Capability::process_arguments));
    OperationResult<ProcessArguments> read = stxlib::process_arguments();
    CHECK(read.succeeded());
    if (read.failed()) return;
    const ProcessArguments& all = *read.value();
    CHECK(all.count() == static_cast<usize>(argument_count));
    if (all.count() != static_cast<usize>(argument_count)) return;
    for (int index = 1; index < argument_count; ++index) {
        const ArgumentText argument = all.argument(static_cast<usize>(index));
#if defined(_WIN32)
        // The C runtime's argv is in the ANSI code page; only ASCII
        // arguments read the same in both.
        if (!is_ascii(arguments[index])) continue;
#endif
        CHECK(same_bytes(argument, arguments[index]));
    }
    // The program's name is present and, from the C runtime, never empty.
    CHECK(all.argument(0).count() > 0);
    if (argument_count != expected_count + 1) {
        std::printf("arguments: not started by CTest; the expected list is skipped\n");
        return;
    }
    for (int index = 0; index < expected_count; ++index) {
        const ArgumentText argument = all.argument(static_cast<usize>(index + 1));
        CHECK(same_bytes(argument, expected_arguments[index]));
        CHECK(argument.is_unicode());
        if (argument.is_unicode()) CHECK(argument.unicode() == view_of(expected_arguments[index]));
    }
}

void argument_limit() {
    // One byte holds no program name, so every process exceeds it.
    const OperationResult<ProcessArguments> small = stxlib::process_arguments(1);
    CHECK(kind_of(small) == ErrorKind::limit_exceeded);
    CHECK(code_of(small) == HostCode::too_long);
    // Two reads agree.
    OperationResult<ProcessArguments> first = stxlib::process_arguments();
    OperationResult<ProcessArguments> second = stxlib::process_arguments();
    CHECK(first.succeeded() && second.succeeded());
    if (first.failed() || second.failed()) return;
    CHECK((*first.value()).count() == (*second.value()).count());
}

std::string hex_of(const ArgumentText argument) {
    std::string text;
    char digits[4]{};
    for (usize index = 0; index < argument.count(); ++index) {
        std::snprintf(digits, sizeof digits, "%02x", static_cast<unsigned>(static_cast<unsigned char>(argument.bytes()[index])));
        text += digits;
    }
    text += argument.is_unicode() ? " u" : " n";
    return text;
}

int print_arguments_hex() {
    OperationResult<ProcessArguments> read = stxlib::process_arguments();
    if (read.failed()) return 3;
    const ProcessArguments& all = *read.value();
    for (usize index = 2; index < all.count(); ++index) {
        const std::string line = hex_of(all.argument(index));
        std::printf("%s\n", line.c_str());
    }
    return 0;
}

std::string captured_output(Child& child) {
    OperationResult<stxlib::Buffer<stxlib::Byte>> output = child.read_output(4096);
    std::string text;
    if (output.failed()) return text;
    const stxlib::Buffer<stxlib::Byte>& bytes = *output.value();
    for (usize index = 0; index < bytes.count(); ++index) {
        if (bytes[index] != static_cast<stxlib::Byte>('\r')) text += static_cast<char>(bytes[index]);
    }
    return text;
}

// An argument that is not Unicode reaches process_arguments losslessly
// (D13): host bytes on POSIX, WTF-8 for an unpaired surrogate on Windows.
void lossless_arguments() {
    OperationResult<Path> self = stxlib::current_executable();
    CHECK(self.succeeded());
    if (self.failed()) return;
#if defined(_WIN32)
    const stxlib::u16 units[] = {stxlib::u16{'a'}, 0xd800, stxlib::u16{'b'}, 0xdc01};
    OperationResult<Path> odd = Path::from_windows_name(units, 4);
    // a, U+D800 as WTF-8 (ed a0 80), b, U+DC01 (ed b0 81).
    const std::string expected = "61eda08062edb081 n\n2061 u\n";
#else
    const char bytes[] = {'a', static_cast<char>(0xff), 'b', static_cast<char>(0xc3)};
    OperationResult<Path> odd = Path::from_posix_name(bytes, sizeof(bytes));
    const std::string expected = "61ff62c3 n\n2061 u\n";
#endif
    CHECK(odd.succeeded());
    if (odd.failed()) return;
    const PathText arguments[] = {"arguments-hex", (*odd.value()).text(), " a"};
    const ChildRequest request{.program = (*self.value()).text(),
                               .arguments = stxlib::View<PathText>(arguments),
                               .streams = ChildStreams::capture};
    OperationResult<Child> started = Child::start(request);
    CHECK(started.succeeded());
    if (started.failed()) return;
    Child child = std::move(started).take_value();
    const std::string printed = captured_output(child);
    CHECK(printed == expected);
    if (printed != expected) std::fprintf(stderr, "arguments-hex child printed: %s", printed.c_str());
    const OperationResult<stxlib::ChildEnd> ended = child.wait(Duration::seconds(30));
    CHECK(ended.succeeded());
    if (ended.succeeded()) {
        const stxlib::u32* const status = (*ended.value()).exit_status();
        CHECK(status != nullptr && *status == 0);
    }
}

// ---- process_id, process_memory ---------------------------------------------

void identity_and_memory() {
    CHECK(stxlib::capability_supported(stxlib::Capability::process_identity));
    const stxlib::u64 id = stxlib::process_id();
    CHECK(id != 0);
    CHECK(stxlib::process_id() == id);
    CHECK(stxlib::capability_supported(stxlib::Capability::memory_report));
    const OperationResult<stxlib::ProcessMemory> memory = stxlib::process_memory();
    CHECK(memory.succeeded());
    if (memory.failed()) return;
    const stxlib::ProcessMemory report = *memory.value();
    // A running test holds at least a page, and far less than a terabyte.
    CHECK(report.resident_bytes >= 4096);
    CHECK(report.resident_bytes < (stxlib::u64{1} << 40));
    CHECK(report.peak_resident_bytes >= report.resident_bytes);
}

// ---- is_main_thread ---------------------------------------------------------

struct MainThreadReport final {
    bool answered{false};
    bool initial{true};
};

void report_main_thread(const stxlib::StopSignal&, void* const context) noexcept {
    MainThreadReport& report = *static_cast<MainThreadReport*>(context);
    const OperationResult<bool> answer = stxlib::is_main_thread();
    report.answered = answer.succeeded();
    report.initial = answer.succeeded() && *answer.value();
}

void main_thread() {
    const OperationResult<bool> here = stxlib::is_main_thread();
    if (!stxlib::capability_supported(stxlib::Capability::main_thread)) {
        CHECK(kind_of(here) == ErrorKind::unsupported);
        return;
    }
    CHECK(here.succeeded() && *here.value());
    MainThreadReport report{};
    OperationResult<stxlib::Thread> started = stxlib::Thread::start(report_main_thread, &report);
    CHECK(started.succeeded());
    if (started.failed()) return;
    stxlib::Thread thread = std::move(started).take_value();
    CHECK(thread.join() == stxlib::JoinResult::joined);
    CHECK(report.answered);
    CHECK(!report.initial);
}

// ---- known_directory --------------------------------------------------------

// A variable's value before the test, restored after it.
struct SavedVariable final {
    TextView name{};
    bool present{false};
    std::string value;
};

SavedVariable save(const TextView name) {
    SavedVariable saved{};
    saved.name = name;
    OperationResult<stxlib::Utf8String> read = stxlib::read_variable(name, 65536);
    if (read.succeeded()) {
        saved.present = true;
        const TextView text = (*read.value()).view();
        saved.value.assign(text.data(), text.count());
    }
    return saved;
}

void restore(const SavedVariable& saved) {
    if (saved.present) {
        const OperationResult<TextView> text = stxlib::validate_utf8(saved.value.data(), saved.value.size());
        if (text.succeeded()) CHECK(stxlib::set_variable(saved.name, *text.value()).succeeded());
    } else {
        static_cast<void>(stxlib::remove_variable(saved.name));
    }
}

bool names(const OperationResult<Path>& found, const char* const text) {
    if (found.failed()) return false;
    const PathText actual = (*found.value()).text();
    const bool result = actual == PathText(view_of(text));
    return result;
}

void known_directories() {
    CHECK(stxlib::capability_supported(stxlib::Capability::known_directories));
    for (int which = 0; which <= 3; ++which) {
        const OperationResult<Path> found = stxlib::known_directory(static_cast<KnownDirectory>(which));
        CHECK(found.succeeded());
        if (found.succeeded()) CHECK((*found.value()).is_absolute());
    }
    const OperationResult<Path> outside = stxlib::known_directory(static_cast<KnownDirectory>(9));
    CHECK(kind_of(outside) == ErrorKind::unsupported);
#if !defined(_WIN32)
    const SavedVariable home = save("HOME");
    const SavedVariable config = save("XDG_CONFIG_HOME");
    const SavedVariable data = save("XDG_DATA_HOME");
    const SavedVariable cache = save("XDG_CACHE_HOME");
    CHECK(stxlib::set_variable("HOME", "/stxlib-test-home").succeeded());
    CHECK(names(stxlib::known_directory(KnownDirectory::home), "/stxlib-test-home"));
#if defined(__APPLE__)
    CHECK(names(stxlib::known_directory(KnownDirectory::user_configuration),
                "/stxlib-test-home/Library/Application Support"));
    CHECK(names(stxlib::known_directory(KnownDirectory::user_data), "/stxlib-test-home/Library/Application Support"));
    CHECK(names(stxlib::known_directory(KnownDirectory::user_cache), "/stxlib-test-home/Library/Caches"));
    // Nothing roams on macOS: the local configuration is the configuration.
    CHECK(names(stxlib::known_directory(KnownDirectory::user_local_configuration),
                "/stxlib-test-home/Library/Application Support"));
#else
    static_cast<void>(stxlib::remove_variable("XDG_CONFIG_HOME"));
    static_cast<void>(stxlib::remove_variable("XDG_DATA_HOME"));
    // A relative value is ignored, as the specification requires.
    CHECK(stxlib::set_variable("XDG_CACHE_HOME", "relative/cache").succeeded());
    CHECK(names(stxlib::known_directory(KnownDirectory::user_configuration), "/stxlib-test-home/.config"));
    CHECK(names(stxlib::known_directory(KnownDirectory::user_data), "/stxlib-test-home/.local/share"));
    CHECK(names(stxlib::known_directory(KnownDirectory::user_cache), "/stxlib-test-home/.cache"));
    CHECK(stxlib::set_variable("XDG_CONFIG_HOME", "/stxlib-test-config").succeeded());
    CHECK(names(stxlib::known_directory(KnownDirectory::user_configuration), "/stxlib-test-config"));
    CHECK(names(stxlib::known_directory(KnownDirectory::user_local_configuration), "/stxlib-test-config"));
#endif
    // An empty HOME falls back to the user database, which names an
    // absolute directory for the account running the tests.
    CHECK(stxlib::set_variable("HOME", "").succeeded());
    const OperationResult<Path> database = stxlib::known_directory(KnownDirectory::home);
    CHECK(database.succeeded() || code_of(database) == HostCode::no_home);
    if (database.succeeded()) CHECK((*database.value()).is_absolute());
    restore(home);
    restore(config);
    restore(data);
    restore(cache);
#endif
#if defined(_WIN32)
    // LocalAppData for both: the local configuration never roams.
    const OperationResult<Path> local = stxlib::known_directory(KnownDirectory::user_local_configuration);
    const OperationResult<Path> cache_folder = stxlib::known_directory(KnownDirectory::user_cache);
    CHECK(local.succeeded() == cache_folder.succeeded());
    if (local.succeeded() && cache_folder.succeeded()) CHECK((*local.value()) == (*cache_folder.value()));
#endif
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count >= 2 && std::strcmp(arguments[1], "arguments-hex") == 0) return print_arguments_hex();
    arguments_agree_with_argv(argument_count, arguments);
    argument_limit();
    lossless_arguments();
    identity_and_memory();
    main_thread();
    known_directories();
    return stxlib_tests::finish("stxlib process tests");
}
