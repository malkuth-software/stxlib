// Tests of child processes and the desktop module (include/stxlib/
// children.hpp, desktop.hpp), ported from the children and desktop parts of
// stx_auxlib's tests/capabilities_tests.cpp at b6e330a. Tested, not proved
// (proofs/REPORT.md).
//
// The child processes are this program itself, started with a mode:
//   echo <arguments...>   prints each argument on its own line
//   exit <status>         exits with that status
//   sleep                 sleeps ten seconds
//   sigpipe               prints whether SIGPIPE is ignored (POSIX)
// The desktop functions are checked only for what they refuse, so the tests
// never open a browser or a file manager.
//
// On Windows, main's arguments are in the ANSI code page and stdout turns
// "\n" into "\r\n", so echo reads its arguments as UTF-8 from the wide
// command line (the CommandLineToArgvW rules children.hpp promises) and
// writes standard output in binary mode.

#include "stxlib/capabilities.hpp"
#include "stxlib/children.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/desktop.hpp"
#include "stxlib/path.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#endif

namespace {

using stxlib::Buffer;
using stxlib::Byte;
using stxlib::Child;
using stxlib::ChildEnd;
using stxlib::ChildRequest;
using stxlib::ChildStreams;
using stxlib::Duration;
using stxlib::ErrorKind;
using stxlib::HostCode;
using stxlib::Result;
using stxlib::Status;
using stxlib::TextView;
using stxlib::usize;

TextView view_of(const std::string& text) {
    Result<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

template <typename Value>
ErrorKind kind_of(const Result<Value>& result) {
    const ErrorKind kind = result.succeeded() ? static_cast<ErrorKind>(0) : (*result.error()).kind();
    return kind;
}

ErrorKind kind_of(const Status& status) {
    const ErrorKind kind = status.succeeded() ? static_cast<ErrorKind>(0) : (*status.error()).kind();
    return kind;
}

template <typename Value>
HostCode code_of(const Result<Value>& result) {
    const HostCode code = result.succeeded() ? HostCode::none : static_cast<HostCode>((*result.error()).code());
    return code;
}

HostCode code_of(const Status& status) {
    const HostCode code = status.succeeded() ? HostCode::none : static_cast<HostCode>((*status.error()).code());
    return code;
}

std::string self_program() {
    Result<stxlib::Path> self = stxlib::current_executable();
    CHECK(self.succeeded());
    if (self.failed()) return std::string();
    // The test's own build directory has a Unicode name.
    const stxlib::PathText text = (*self.value()).text();
    CHECK(text.is_unicode());
    std::string result(text.data(), text.count());
    return result;
}

// Starts this program with `arguments`; an empty Child when it fails.
Child start_self(const std::vector<std::string>& arguments, const ChildStreams streams, const bool detached = false) {
    const std::string program = self_program();
    std::vector<TextView> views;
    for (const std::string& argument : arguments) views.push_back(view_of(argument));
    const ChildRequest request{.program = view_of(program),
                               .arguments = stxlib::View<TextView>(views.data(), views.size()),
                               .streams = streams,
                               .detached = detached};
    Result<Child> started = Child::start(request);
    CHECK(started.succeeded());
    if (started.failed()) return Child();
    Child result = static_cast<Result<Child>&&>(started).take_value();
    return result;
}

std::string output_text(Child& child, const usize limit) {
    Result<Buffer<Byte>> output = child.read_output(limit);
    CHECK(output.succeeded());
    if (output.failed()) return std::string();
    const stxlib::View<Byte> view = (*output.value()).view();
    std::string result(reinterpret_cast<const char*>(view.data()), view.count());
    return result;
}

bool exited_with(const Result<ChildEnd>& end, const stxlib::u32 status) {
    const bool result = end.succeeded() && (*end.value()).exit_status() != nullptr && *(*end.value()).exit_status() == status;
    return result;
}

void availability() {
    CHECK(stxlib::capability_supported(stxlib::Capability::child_processes));
    CHECK(stxlib::capability_supported(stxlib::Capability::current_executable));
    const std::string self = self_program();
    Result<stxlib::Path> parsed = stxlib::Path::parse(view_of(self));
    CHECK(parsed.succeeded() && (*parsed.value()).is_absolute());
}

void child_requests() {
    const ChildRequest empty{};
    const Result<Child> no_program = Child::start(empty);
    CHECK(kind_of(no_program) == ErrorKind::invalid_argument && code_of(no_program) == HostCode::empty_request);
    const std::string program = self_program();
    const TextView with_nul[] = {TextView("a\0b")};
    const ChildRequest nul_request{.program = view_of(program), .arguments = stxlib::View<TextView>(with_nul)};
    const Result<Child> nul = Child::start(nul_request);
    CHECK(kind_of(nul) == ErrorKind::invalid_argument && code_of(nul) == HostCode::invalid_value);
    const ChildRequest missing{.program = "/nonexistent-stxlib/program"};
    CHECK(kind_of(Child::start(missing)) == ErrorKind::not_found);
    const ChildRequest unlisted{.program = "stxlib-no-such-program"};
    CHECK(kind_of(Child::start(unlisted)) == ErrorKind::not_found);
}

void child_output_and_status() {
    // Arguments reach the child exactly: spaces, quotes, backslashes, an
    // empty argument and UTF-8 are not reinterpreted by any shell.
    const std::vector<std::string> words = {"plain", "two words", "quote\"inside", "back\\slash\\", "",
                                            "caf\xc3\xa9 \xf0\x9f\x98\x80", "$HOME;rm -rf /"};
    std::vector<std::string> arguments = {"echo"};
    arguments.insert(arguments.end(), words.begin(), words.end());
    Child echo = start_self(arguments, ChildStreams::capture);
    std::string expected;
    for (const std::string& word : words) expected += word + "\n";
    const std::string echoed = output_text(echo, 4096);
    CHECK(echoed == expected);
    if (echoed != expected) {
        // The bytes, so an invisible difference shows in a CI log.
        std::fprintf(stderr, "echo output (%lu bytes):", static_cast<unsigned long>(echoed.size()));
        for (const char byte : echoed) std::fprintf(stderr, " %02x", static_cast<unsigned>(static_cast<unsigned char>(byte)));
        std::fprintf(stderr, "\nexpected (%lu bytes)\n", static_cast<unsigned long>(expected.size()));
    }
    const Result<ChildEnd> echo_end = echo.wait(Duration::seconds(30));
    CHECK(exited_with(echo_end, 0));
    const Result<ChildEnd> twice = echo.wait(Duration::seconds(1));
    CHECK(kind_of(twice) == ErrorKind::invalid_argument && code_of(twice) == HostCode::invalid_state);
    CHECK(code_of(echo.read_output(16)) == HostCode::invalid_state);
    Child exiting = start_self({"exit", "3"}, ChildStreams::discard);
    CHECK(exited_with(exiting.wait(Duration::seconds(30)), 3));
    // Over the limit: too_long, after the child's output is drained.
    Child chatty = start_self({"echo", std::string(5000, 'x')}, ChildStreams::capture);
    const Result<Buffer<Byte>> clipped = chatty.read_output(100);
    CHECK(kind_of(clipped) == ErrorKind::limit_exceeded && code_of(clipped) == HostCode::too_long);
    CHECK(chatty.wait(Duration::seconds(30)).succeeded());
    // A detached child (its own session) still starts, exits and is waited for.
    Child detached = start_self({"exit", "5"}, ChildStreams::discard, true);
    CHECK(exited_with(detached.wait(Duration::seconds(30)), 5));
    Child no_capture = start_self({"exit", "0"}, ChildStreams::discard);
    CHECK(code_of(no_capture.read_output(16)) == HostCode::invalid_state);
    CHECK(no_capture.wait(Duration::seconds(30)).succeeded());
    // An empty Child refuses every call.
    Child nothing;
    CHECK(!nothing.started());
    CHECK(code_of(nothing.wait(Duration())) == HostCode::invalid_state);
    CHECK(code_of(nothing.kill()) == HostCode::invalid_state);
}

void timeouts_and_kill() {
    Child sleeper = start_self({"sleep"}, ChildStreams::discard);
    const stxlib::MonotonicTime before = stxlib::MonotonicTime::now();
    const Result<ChildEnd> timed = sleeper.wait(Duration::milliseconds(50));
    CHECK(kind_of(timed) == ErrorKind::limit_exceeded && code_of(timed) == HostCode::timed_out);
    CHECK(stxlib::MonotonicTime::now().since(before) >= Duration::milliseconds(50));
    CHECK(code_of(sleeper.wait(Duration())) == HostCode::timed_out);
    CHECK(sleeper.kill().succeeded());
    const Result<ChildEnd> killed = sleeper.wait(Duration::seconds(30));
    CHECK(killed.succeeded());
#if !defined(_WIN32)
    CHECK(killed.succeeded() && (*killed.value()).signal() != nullptr && *(*killed.value()).signal() == SIGKILL);
#endif
    CHECK(code_of(sleeper.kill()) == HostCode::invalid_state);
    // Abandoned children are released without blocking the owner.
    for (int index = 0; index < 4; ++index) {
        const Child abandoned = start_self({"exit", "0"}, ChildStreams::discard);
    }
    Child later = start_self({"exit", "0"}, ChildStreams::discard);
    CHECK(later.wait(Duration::seconds(30)).succeeded());
    // Moving transfers the child.
    Child first = start_self({"exit", "7"}, ChildStreams::discard);
    Child second(static_cast<Child&&>(first));
    CHECK(!first.started() && second.started());
    CHECK(exited_with(second.wait(Duration::seconds(30)), 7));
}

#if !defined(_WIN32)
// Children start with default signal handling whatever the parent ignores.
void children_reset_signals() {
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction previous {};
    CHECK(::sigaction(SIGPIPE, &ignore, &previous) == 0);
    Child child = start_self({"sigpipe"}, ChildStreams::capture);
    CHECK(output_text(child, 64) == "default\n");
    CHECK(child.wait(Duration::seconds(30)).succeeded());
    (void)::sigaction(SIGPIPE, &previous, nullptr);
}
#endif

void desktop_refusals() {
    const TextView refused[] = {"-x", "--help", "file:///etc/passwd", "javascript:alert(1)",
                                "relative/path", "https://example.com/\nsecond", ""};
    for (const TextView target : refused) {
        const Status status = stxlib::open_with_desktop(target);
        CHECK(kind_of(status) == ErrorKind::invalid_argument && code_of(status) == HostCode::invalid_value);
    }
    Result<stxlib::Path> relative = stxlib::Path::parse("some/file");
    CHECK(relative.succeeded());
    if (relative.succeeded()) {
        const Status status = stxlib::reveal_in_file_manager(*relative.value());
        CHECK(kind_of(status) == ErrorKind::invalid_argument);
    }
#if !defined(_WIN32)
    // A drive path is not absolute on a POSIX host.
    Result<stxlib::Path> drive = stxlib::Path::parse("C:/x");
    CHECK(drive.succeeded() && kind_of(stxlib::reveal_in_file_manager(*drive.value())) == ErrorKind::invalid_argument);
#endif
}

#if defined(_WIN32)
// This process's arguments as UTF-8, parsed from the wide command line.
std::vector<std::string> utf8_arguments() {
    std::vector<std::string> result;
    int count = 0;
    wchar_t** const wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) return result;
    for (int index = 0; index < count; ++index) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) continue;
        std::string argument(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, argument.data(), bytes, nullptr, nullptr);
        argument.resize(static_cast<std::size_t>(bytes - 1)); // drops the converted terminator
        result.push_back(argument);
    }
    LocalFree(wide);
    return result;
}
#endif

int echo_arguments(const int argument_count, char** const arguments) {
#if defined(_WIN32)
    (void)argument_count;
    (void)arguments;
    _setmode(_fileno(stdout), _O_BINARY);
    const std::vector<std::string> words = utf8_arguments();
    for (usize index = 2; index < words.size(); ++index) std::printf("%s\n", words[index].c_str());
#else
    for (int index = 2; index < argument_count; ++index) std::printf("%s\n", arguments[index]);
#endif
    return 0;
}

int child_mode(const int argument_count, char** const arguments) {
    const std::string mode = arguments[1];
    if (mode == "echo") return echo_arguments(argument_count, arguments);
    if (mode == "exit" && argument_count == 3) return std::atoi(arguments[2]);
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
#if !defined(_WIN32)
    if (mode == "sigpipe") {
        struct sigaction current {};
        (void)::sigaction(SIGPIPE, nullptr, &current);
        std::printf("%s\n", current.sa_handler == SIG_IGN ? "ignored" : "default");
        return 0;
    }
#endif
    return 2;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count >= 2) return child_mode(argument_count, arguments);
    availability();
    child_requests();
    child_output_and_status();
    timeouts_and_kill();
#if !defined(_WIN32)
    children_reset_signals();
#endif
    desktop_refusals();
    return stxlib_tests::finish("stxlib children tests");
}
