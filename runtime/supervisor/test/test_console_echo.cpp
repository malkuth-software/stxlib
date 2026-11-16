// test_console_echo.cpp: host tests of the console echo of target output
// (console_echo.h).
//
// Exhaustive over the stated domain: every output of 0 to 6 bytes over
// {'a', '\n', 0x01} (a printable byte, the line end, a byte shown as '?'),
// fed in every split into consecutive pieces (2^(n-1) splits of n bytes),
// for every budget 0 to 7 and every width 1 to 3, each with and without
// finish() at the end: 27,994 fed sequences x 8 budgets x 3 widths x 2 =
// 1,343,712 runs. Each run's pieces are compared, in order, with a
// reference that applies the rule to the whole output at once: the first
// min(budget, n) bytes; a newline gives the pending text (possibly empty);
// a byte arriving with width bytes pending gives those first; pending text
// is given when the budget is spent or at finish. The split cannot change
// the result. After each run, more output without start() gives nothing,
// and after start() the budget is whole again. Then the default bounds
// (512 bytes, 100 per line): stated cases at both sides of each.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../supervisor/config/test/test_support.h"
#include "../console_echo.h"

namespace {

using stxsupervisor::console_echo_bytes;
using stxsupervisor::console_echo_line_bytes;
using stxsupervisor::ConsoleEcho;
using stxsupervisor::ConsoleEchoSink;
using stxtest::check;

constexpr std::size_t lines_max = 16;
constexpr std::size_t length_max = 6;
constexpr char alphabet[3] = {'a', '\n', '\x01'};

struct Lines {
    char text[lines_max][console_echo_line_bytes];
    std::size_t length[lines_max];
    std::size_t count;
    bool overflowed;
};

void clear(Lines& lines) noexcept {
    lines.count = 0;
    lines.overflowed = false;
}

void add(Lines& lines, const char* text, std::size_t length) noexcept {
    if (lines.count >= lines_max || length > console_echo_line_bytes) {
        lines.overflowed = true;
        return;
    }
    std::memcpy(lines.text[lines.count], text, length);
    lines.length[lines.count] = length;
    lines.count += 1;
}

[[nodiscard]] bool same(const Lines& left, const Lines& right) noexcept {
    if (left.overflowed || right.overflowed || left.count != right.count) {
        return false;
    }
    for (std::size_t index = 0; index < left.count; index += 1) {
        if (left.length[index] != right.length[index] ||
            std::memcmp(left.text[index], right.text[index], left.length[index]) != 0) {
            return false;
        }
    }
    return true;
}

class Recorder final : public ConsoleEchoSink {
public:
    explicit Recorder(Lines& lines) noexcept : lines_(lines) {}
    void line(std::span<const char> text) noexcept override { add(lines_, text.data(), text.size()); }

private:
    Lines& lines_;
};

// The rule over the whole output at once.
void reference(const char* output, std::size_t length, std::size_t budget, std::size_t width, bool finished,
               Lines& lines) noexcept {
    clear(lines);
    char pending[console_echo_line_bytes] = {};
    std::size_t pending_length = 0;
    const std::size_t taken = length < budget ? length : budget;
    for (std::size_t index = 0; index < taken; index += 1) {
        const char byte = output[index];
        if (byte == '\n') {
            add(lines, pending, pending_length);
            pending_length = 0;
            continue;
        }
        if (pending_length == width) {
            add(lines, pending, pending_length);
            pending_length = 0;
        }
        pending[pending_length] = byte == 'a' ? 'a' : '?';
        pending_length += 1;
    }
    const bool spent = length >= budget;
    if ((spent || finished) && pending_length > 0u) {
        add(lines, pending, pending_length);
    }
}

// Static: large.
Lines got;
Lines wanted;
Lines after;

void test_exhaustive() {
    std::size_t runs = 0;
    std::size_t failures = 0;
    char output[length_max] = {};
    for (std::size_t length = 0; length <= length_max; length += 1) {
        std::size_t outputs = 1;
        for (std::size_t index = 0; index < length; index += 1) {
            outputs *= 3u;
        }
        const std::size_t splits = length == 0u ? 1u : (std::size_t{1} << (length - 1u));
        for (std::size_t number = 0; number < outputs; number += 1) {
            std::size_t rest = number;
            for (std::size_t index = 0; index < length; index += 1) {
                output[index] = alphabet[rest % 3u];
                rest /= 3u;
            }
            for (std::size_t split = 0; split < splits; split += 1) {
                for (std::size_t budget = 0; budget <= 7u; budget += 1) {
                    for (std::size_t width = 1; width <= 3u; width += 1) {
                        for (int finished = 0; finished < 2; finished += 1) {
                            ConsoleEcho echo(budget, width);
                            clear(got);
                            Recorder recorder(got);
                            echo.start();
                            // Bit i of split cuts after byte i + 1.
                            std::size_t begin = 0;
                            for (std::size_t index = 0; index < length; index += 1) {
                                const bool cut = index + 1u == length || ((split >> index) & 1u) != 0u;
                                if (cut) {
                                    echo.feed(std::span<const char>(output + begin, index + 1u - begin), recorder);
                                    begin = index + 1u;
                                }
                            }
                            if (finished != 0) {
                                echo.finish(recorder);
                            }
                            reference(output, length, budget, width, finished != 0, wanted);
                            bool good = same(got, wanted);
                            // Without a start: nothing more once spent or finished.
                            const bool closed = finished != 0 || length >= budget;
                            clear(after);
                            Recorder later(after);
                            echo.feed(std::span<const char>("a\n", 2), later);
                            good = good && (!closed || after.count == 0u);
                            // A start makes the budget whole again.
                            clear(after);
                            echo.start();
                            echo.feed(std::span<const char>("a\n", 2), later);
                            good = good && (budget < 2u || (after.count == 1u && after.length[0] == 1u &&
                                                            after.text[0][0] == 'a'));
                            failures += good ? 0u : 1u;
                            runs += 1;
                        }
                    }
                }
            }
        }
    }
    check(runs == 1343712u, "console echo", "1343712 runs");
    check(failures == 0u, "console echo", "every run matches the reference");
    std::printf("console echo: %zu runs, %zu failing\n", runs, failures);
}

// Static: large.
char big[2048];

void test_default_bounds() {
    // One line of exactly 100 bytes: one piece; 101: 100 and 1.
    std::memset(big, 'x', sizeof(big));
    big[100] = '\n';
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.start();
        echo.feed(std::span<const char>(big, 101), recorder);
        check(got.count == 1u && got.length[0] == 100u, "console echo", "a 100-byte line is one piece");
    }
    big[100] = 'x';
    big[101] = '\n';
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.start();
        echo.feed(std::span<const char>(big, 102), recorder);
        check(got.count == 2u && got.length[0] == 100u && got.length[1] == 1u, "console echo",
              "a 101-byte line is 100 and 1");
    }
    // 2048 bytes without a newline: 512 echoed, as 5 x 100 + 12, in one
    // feed or byte by byte.
    std::memset(big, 'y', sizeof(big));
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.start();
        echo.feed(std::span<const char>(big, sizeof(big)), recorder);
        std::size_t total = 0;
        for (std::size_t index = 0; index < got.count; index += 1) {
            total += got.length[index];
        }
        check(got.count == 6u && total == console_echo_bytes && got.length[5] == 12u, "console echo",
              "512 bytes per start, then nothing");
    }
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.start();
        for (std::size_t index = 0; index < sizeof(big); index += 1) {
            echo.feed(std::span<const char>(big + index, 1), recorder);
        }
        std::size_t total = 0;
        for (std::size_t index = 0; index < got.count; index += 1) {
            total += got.length[index];
        }
        check(got.count == 6u && total == console_echo_bytes, "console echo", "byte by byte: the same");
    }
    // A fresh echo, never started, gives nothing.
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.feed(std::span<const char>("hello\n", 6), recorder);
        echo.finish(recorder);
        check(got.count == 0u, "console echo", "nothing before the first start");
    }
    // Control bytes, carriage return and escape, are shown as '?'.
    {
        ConsoleEcho echo;
        clear(got);
        Recorder recorder(got);
        echo.start();
        echo.feed(std::span<const char>("a\rb\x1b[2J\tc\n", 10), recorder);
        check(got.count == 1u && got.length[0] == 9u && std::memcmp(got.text[0], "a?b?[2J\tc", 9) == 0,
              "console echo", "control bytes shown as '?', tab kept");
    }
}

}  // namespace

int main() {
    test_exhaustive();
    test_default_bounds();
    return stxtest::finish("test_console_echo");
}
