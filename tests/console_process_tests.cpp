// Scenarios for the console (console.hpp) and process control
// (process.hpp, require.hpp), ported from stx_auxlib's
// tests/console_process_tests.cpp at b6e330a. Each mode does one thing
// whose effect is visible only from outside the process (its output, its
// input, its exit status); console_process_check.cmake runs the modes and
// checks those effects.
//
//   write             writes to both streams, interleaved with printf
//   prepare <path>    writes the input file that `read` is run with
//   read              reads standard input line by line (limit 16)
//   exit              registers exit tasks and exits with status 7
//   fail              fails a requirement (stx_auxlib's ensure)
//
// Changes from stx_auxlib: `prepare` writes with the C library (the files
// module is batch 2b); the invalid-text refusal on output is gone (a
// TextView is UTF-8 by construction); `fail` uses STXLIB_REQUIRE.

#include "stxlib/capabilities.hpp"
#include "stxlib/console.hpp"
#include "stxlib/process.hpp"
#include "stxlib/require.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void say(const stxlib::TextView text) {
    static_cast<void>(stxlib::write_console_line(stxlib::ConsoleStream::output, text));
}

int write_mode() {
    std::printf("from printf\n");
    say("after printf");
    say("h\xc3\xa9llo w\xc3\xb6rld \xf0\x9f\x98\x80");
    say(stxlib::console_is_terminal(stxlib::ConsoleStream::output) ? stxlib::TextView("terminal: yes")
                                                                   : stxlib::TextView("terminal: no"));
    static_cast<void>(stxlib::write_console(stxlib::ConsoleStream::error, "to stderr\n"));
    return 0;
}

int prepare_mode(const char* const path) {
    static const char content[] = "first\r\nsecond\n0123456789abcdefXYZ\n\xff\xfe\nexactly-sixteen!\nlast";
    std::FILE* const file = std::fopen(path, "wb");
    if (file == nullptr) return 3;
    const std::size_t count = sizeof(content) - 1;
    const bool written = std::fwrite(content, 1, count, file) == count;
    const bool closed = std::fclose(file) == 0;
    return written && closed ? 0 : 3;
}

void say_line(const stxlib::TextView prefix, const stxlib::TextView text) {
    std::string joined(prefix.data(), prefix.count());
    joined.append(text.data(), text.count());
    stxlib::OperationResult<stxlib::TextView> valid = stxlib::validate_utf8(joined.data(), joined.size());
    if (valid.succeeded()) say(*valid.value());
}

int read_mode() {
    for (;;) {
        const stxlib::OperationResult<stxlib::ConsoleLine> line = stxlib::read_console_line(16);
        if (line.succeeded()) {
            const stxlib::ConsoleLine& got = *line.value();
            if (got.ended()) {
                say("end");
                return 0;
            }
            say_line("ok ", got.text());
            continue;
        }
        const stxlib::ErrorRecord& failure = *line.error();
        const stxlib::HostCode code = static_cast<stxlib::HostCode>(failure.code());
        if (code == stxlib::HostCode::too_long) {
            say("too_long");
        } else if (code == stxlib::HostCode::invalid_text) {
            say("invalid_text");
        } else {
            say("error");
            return 1;
        }
    }
}

void first_task(void*) { say("task one"); }
void third_task(void*) { say("task three, registered by task two"); }
void second_task(void*) {
    say("task two");
    static_cast<void>(stxlib::register_exit_task(stxlib::CallbackTarget<void()>::bind(third_task, nullptr)));
}

int exit_mode() {
    if (stxlib::register_exit_task(stxlib::CallbackTarget<void()>()).failed()) say("unbound task refused");
    static_cast<void>(stxlib::register_exit_task(stxlib::CallbackTarget<void()>::bind(first_task, nullptr)));
    static_cast<void>(stxlib::register_exit_task(stxlib::CallbackTarget<void()>::bind(second_task, nullptr)));
    return 7; // a return from main: the exit tasks run from atexit
}

int fail_mode() {
    STXLIB_REQUIRE(true, "this must not fire");
    say("before the fault");
    const int two = 1 + 1;
    STXLIB_REQUIRE(two == 3, "broken invariant");
    say("after the fault");
    return 0;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count < 2) return 2;
    const char* const mode = arguments[1];
    if (std::strcmp(mode, "write") == 0) return write_mode();
    if (std::strcmp(mode, "prepare") == 0 && argument_count == 3) return prepare_mode(arguments[2]);
    if (std::strcmp(mode, "read") == 0) return read_mode();
    if (std::strcmp(mode, "exit") == 0) return exit_mode();
    if (std::strcmp(mode, "fail") == 0) return fail_mode();
    return 2;
}
