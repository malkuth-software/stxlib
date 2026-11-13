// Tests of the platform layer's batch 2a modules: capabilities, clocks,
// environment, entropy, the terminal without one, and process events
// (POSIX). Ported from stx_auxlib's tests at b6e330a (clocks_tests.cpp,
// environment_tests.cpp, and the terminal and events parts of
// capabilities_tests.cpp); console and exit tasks are in
// console_process_tests.cpp. Tested, not proved (proofs/REPORT.md).

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/entropy.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/events.hpp"
#include "stxlib/terminal.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace {

using stxlib::usize;

stxlib::HostCode code_of(const stxlib::ErrorRecord& record) {
    const stxlib::HostCode result = static_cast<stxlib::HostCode>(record.code());
    return result;
}

stxlib::TextView view_of(const std::string& text) {
    stxlib::Result<stxlib::TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const stxlib::TextView result = valid.succeeded() ? *valid.value() : stxlib::TextView("");
    return result;
}

// ---- capabilities -----------------------------------------------------------

void capabilities_report() {
    for (usize index = 0; index < stxlib::capability_count; ++index) {
        const stxlib::Capability capability = static_cast<stxlib::Capability>(index);
        CHECK(stxlib::capability_name(capability) != stxlib::TextView("unknown"));
    }
    CHECK(stxlib::capability_name(static_cast<stxlib::Capability>(200)) == stxlib::TextView("unknown"));
    CHECK(!stxlib::capability_supported(static_cast<stxlib::Capability>(200)));
    CHECK(stxlib::capability_supported(stxlib::Capability::console));
    CHECK(stxlib::capability_supported(stxlib::Capability::environment));
    CHECK(stxlib::capability_supported(stxlib::Capability::interval_clock));
    CHECK(stxlib::capability_supported(stxlib::Capability::wall_clock));
    CHECK(stxlib::capability_supported(stxlib::Capability::entropy));
    CHECK(stxlib::capability_supported(stxlib::Capability::exit_tasks));
    CHECK(stxlib::capability_supported(stxlib::Capability::raw_terminal));
    CHECK(stxlib::capability_supported(stxlib::Capability::interrupt_events));
    CHECK(stxlib::capability_supported(stxlib::Capability::files));
    CHECK(stxlib::capability_supported(stxlib::Capability::child_processes));
    CHECK(stxlib::capability_supported(stxlib::Capability::desktop_open));
    CHECK(stxlib::capability_supported(stxlib::Capability::file_reveal));
    CHECK(stxlib::capability_supported(stxlib::Capability::current_executable));
    CHECK(stxlib::capability_supported(stxlib::Capability::threads));
    CHECK(stxlib::capability_supported(stxlib::Capability::local_time_zone));
    CHECK(stxlib::capability_name(stxlib::Capability::atomic_replace) == stxlib::TextView("atomic_replace"));
#if defined(_WIN32)
    CHECK(!stxlib::capability_supported(stxlib::Capability::resize_events));
    CHECK(!stxlib::capability_supported(stxlib::Capability::suspend_events));
    CHECK(!stxlib::capability_supported(stxlib::Capability::atomic_replace));
#else
    CHECK(stxlib::capability_supported(stxlib::Capability::atomic_replace));
    CHECK(stxlib::capability_supported(stxlib::Capability::resize_events));
    CHECK(stxlib::capability_supported(stxlib::Capability::suspend_events));
#endif
}

// ---- clocks -----------------------------------------------------------------

void steady_clocks_never_decrease() {
    stxlib::IntervalTime previous_interval = stxlib::IntervalTime::now();
    stxlib::ElapsedTime previous_elapsed = stxlib::ElapsedTime::now();
    bool backwards = false;
    for (int read = 0; read < 100000; ++read) {
        const stxlib::IntervalTime next_interval = stxlib::IntervalTime::now();
        const stxlib::ElapsedTime next_elapsed = stxlib::ElapsedTime::now();
        if (next_interval < previous_interval || next_elapsed < previous_elapsed) backwards = true;
        previous_interval = next_interval;
        previous_elapsed = next_elapsed;
    }
    CHECK(!backwards);
}

// Over a 50 ms sleep, the interval and elapsed clocks and the C++ runtime's
// steady clock advance alike (no suspension is expected during the test;
// the clocks count from different epochs, so spans are compared, not
// readings). The wall clock reads as the runtime's system clock does.
void clocks_agree_with_host() {
    CHECK(stxlib::capability_supported(stxlib::Capability::elapsed_clock));
    const std::chrono::steady_clock::time_point host_start = std::chrono::steady_clock::now();
    const stxlib::IntervalTime interval_start = stxlib::IntervalTime::now();
    const stxlib::ElapsedTime elapsed_start = stxlib::ElapsedTime::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const stxlib::ElapsedTime elapsed_end = stxlib::ElapsedTime::now();
    const stxlib::IntervalTime interval_end = stxlib::IntervalTime::now();
    const std::chrono::steady_clock::time_point host_end = std::chrono::steady_clock::now();
    const long long host_span = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_end - host_start).count());
    const long long interval_span = interval_end.since(interval_start).count_nanoseconds();
    const long long elapsed_span = elapsed_end.since(elapsed_start).count_nanoseconds();
    // Windows' interrupt time counts 100 ns units and the reads are
    // nested, so each span lies within the host's, less a tick of slack.
    CHECK(interval_span >= 49000000LL && interval_span <= host_span + 1000000LL);
    CHECK(elapsed_span >= 49000000LL && elapsed_span <= host_span + 1000000LL);
    const std::chrono::system_clock::time_point host_wall = std::chrono::system_clock::now();
    const stxlib::WallTime wall = stxlib::WallTime::now();
    const long long host_wall_ns = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(host_wall.time_since_epoch()).count());
    const long long wall_gap = wall.unix_nanoseconds() - host_wall_ns;
    CHECK(wall_gap > -1000000000LL && wall_gap < 1000000000LL);
}

// Unix time: after 2026-01-01T00:00:00Z (1767225600 s) and before
// 2100-01-01T00:00:00Z (4102444800 s).
void wall_clock_is_unix_time() {
    const long long seconds = stxlib::WallTime::now().unix_seconds();
    CHECK(seconds > 1767225600LL && seconds < 4102444800LL);
}

void time_arithmetic_saturates() {
    static_assert(stxlib::Duration::seconds(2).count_nanoseconds() == 2000000000LL, "seconds scale");
    static_assert(stxlib::Duration::milliseconds(3).count_milliseconds() == 3, "milliseconds round trip");
    static_assert(stxlib::Duration::seconds(stxlib::i64_max) == stxlib::Duration::longest(), "scale saturates");
    static_assert(stxlib::Duration::seconds(-stxlib::i64_max).count_nanoseconds() == stxlib::i64_min,
                  "negative scale saturates");
    static_assert(stxlib::Duration::longest().plus(stxlib::Duration::nanoseconds(1)) == stxlib::Duration::longest(),
                  "plus saturates");
    static_assert(stxlib::IntervalTime::from_nanoseconds(stxlib::i64_max)
                          .after(stxlib::Duration::seconds(1))
                          .count_nanoseconds() == stxlib::i64_max,
                  "a far deadline stays in the future");
    static_assert(stxlib::ElapsedTime::from_nanoseconds(stxlib::i64_min)
                          .since(stxlib::ElapsedTime::from_nanoseconds(1))
                          .count_nanoseconds() == stxlib::i64_min,
                  "elapsed spans saturate");
    static_assert(stxlib::WallTime::from_unix_nanoseconds(-1).unix_seconds() == -1, "seconds round down");
    static_assert(stxlib::WallTime::from_unix_nanoseconds(1999999999).unix_seconds() == 1, "seconds round down");
    const stxlib::IntervalTime start = stxlib::IntervalTime::now();
    const stxlib::IntervalTime later = start.after(stxlib::Duration::milliseconds(5));
    CHECK(later.since(start) == stxlib::Duration::milliseconds(5));
}

// ---- environment ------------------------------------------------------------

constexpr stxlib::TextView variable = "STXLIB_TEST_VARIABLE";

void variable_names() {
    CHECK(stxlib::is_portable_variable_name("HOME"));
    CHECK(stxlib::is_portable_variable_name("_X1"));
    CHECK(!stxlib::is_portable_variable_name(""));
    CHECK(!stxlib::is_portable_variable_name("1A"));
    CHECK(!stxlib::is_portable_variable_name("A-B"));
    CHECK(!stxlib::is_portable_variable_name("A=B"));
    CHECK(!stxlib::is_portable_variable_name("\xc3\xa9"));
    const stxlib::Result<stxlib::Utf8String> refused = stxlib::read_variable("A=B", 64);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument &&
          code_of(*refused.error()) == stxlib::HostCode::invalid_name);
    CHECK(stxlib::set_variable("A B", "x").failed());
}

void set_read_remove() {
    CHECK(stxlib::remove_variable(variable).succeeded());
    const stxlib::Result<stxlib::Utf8String> unset = stxlib::read_variable(variable, 64);
    CHECK(unset.failed() && (*unset.error()).kind() == stxlib::ErrorKind::not_found);
    constexpr stxlib::TextView text = "caf\xc3\xa9 \xf0\x9f\x98\x80 /usr/local/bin";
    CHECK(stxlib::set_variable(variable, text).succeeded());
    const stxlib::Result<stxlib::Utf8String> got = stxlib::read_variable(variable, 64);
    CHECK(got.succeeded() && (*got.value()).view() == text);
    const stxlib::Result<stxlib::Utf8String> over = stxlib::read_variable(variable, text.count() - 1);
    CHECK(over.failed() && (*over.error()).kind() == stxlib::ErrorKind::limit_exceeded &&
          code_of(*over.error()) == stxlib::HostCode::too_long);
    CHECK(stxlib::set_variable(variable, "").succeeded());
    const stxlib::Result<stxlib::Utf8String> empty = stxlib::read_variable(variable, 64);
    CHECK(empty.succeeded() && (*empty.value()).empty());
    const stxlib::Status nul = stxlib::set_variable(variable, "a\0b");
    CHECK(nul.failed() && code_of(*nul.error()) == stxlib::HostCode::invalid_value);
    CHECK(stxlib::remove_variable(variable).succeeded());
    const stxlib::Result<stxlib::Utf8String> removed = stxlib::read_variable(variable, 64);
    CHECK(removed.failed() && (*removed.error()).kind() == stxlib::ErrorKind::not_found);
    const std::string oversized(stxlib::maximum_environment_bytes + 1, 'x');
    const stxlib::Status too_long = stxlib::set_variable(variable, view_of(oversized));
    CHECK(too_long.failed() && (*too_long.error()).kind() == stxlib::ErrorKind::limit_exceeded);
}

// Concurrent setters and readers: serialized, so no read sees a torn value.
std::atomic<int> torn_reads{0};

void churn() {
    for (int round = 0; round < 2000; ++round) {
        const bool long_value = round % 2 == 0;
        const stxlib::TextView value = long_value ? stxlib::TextView("aaaaaaaaaaaaaaaa") : stxlib::TextView("bb");
        static_cast<void>(stxlib::set_variable(variable, value));
        const stxlib::Result<stxlib::Utf8String> got = stxlib::read_variable(variable, 64);
        if (got.succeeded()) {
            const stxlib::TextView seen = (*got.value()).view();
            if (seen != stxlib::TextView("aaaaaaaaaaaaaaaa") && seen != stxlib::TextView("bb")) {
                torn_reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void concurrent_use() {
    std::vector<std::thread> threads;
    for (int index = 0; index < 4; ++index) threads.emplace_back(churn);
    for (std::thread& thread : threads) thread.join();
    CHECK(torn_reads.load() == 0);
    static_cast<void>(stxlib::remove_variable(variable));
}

// ---- entropy ----------------------------------------------------------------

// Sampled: two 32-byte draws are equal, or all zero, with probability
// 2^-256 for a working generator.
void entropy_fills() {
    stxlib::Byte first[32]{};
    stxlib::Byte second[32]{};
    CHECK(stxlib::fill_entropy(stxlib::MutableView<stxlib::Byte>(first)).succeeded());
    CHECK(stxlib::fill_entropy(stxlib::MutableView<stxlib::Byte>(second)).succeeded());
    bool same = true;
    bool zero = true;
    for (usize index = 0; index < 32; ++index) {
        if (first[index] != second[index]) same = false;
        if (first[index] != 0) zero = false;
    }
    CHECK(!same && !zero);
    CHECK(stxlib::fill_entropy(stxlib::MutableView<stxlib::Byte>()).succeeded());
}

// ---- terminal ---------------------------------------------------------------

void terminal_without_one() {
    // CTest runs with standard output redirected.
    const stxlib::Result<stxlib::TerminalSize> size = stxlib::terminal_size();
    CHECK(size.failed() && (*size.error()).kind() == stxlib::ErrorKind::unsupported &&
          code_of(*size.error()) == stxlib::HostCode::not_a_terminal);
    const stxlib::Result<stxlib::RawTerminal> raw = stxlib::RawTerminal::enter();
    CHECK(raw.failed() && code_of(*raw.error()) == stxlib::HostCode::not_a_terminal);
    const stxlib::RawTerminal none;
    CHECK(!none.active());
}

// ---- events -----------------------------------------------------------------

#if !defined(_WIN32)
void events() {
    const stxlib::Result<stxlib::EventWatch> empty = stxlib::EventWatch::watch(stxlib::EventSet());
    CHECK(empty.failed() && code_of(*empty.error()) == stxlib::HostCode::empty_request);
    const stxlib::EventSet watched =
        stxlib::EventSet().with(stxlib::ProcessEvent::interrupt).with(stxlib::ProcessEvent::terminal_resized);
    {
        stxlib::Result<stxlib::EventWatch> started = stxlib::EventWatch::watch(watched);
        CHECK(started.succeeded());
        if (started.failed()) return;
        stxlib::EventWatch& watch = *started.value();
        const stxlib::Result<stxlib::EventWatch> second = stxlib::EventWatch::watch(watched);
        CHECK(second.failed() && code_of(*second.error()) == stxlib::HostCode::busy);
        CHECK(std::raise(SIGINT) == 0);
        CHECK(watch.take().contains(stxlib::ProcessEvent::interrupt));
        CHECK(watch.take().empty());
        CHECK(std::raise(SIGWINCH) == 0);
        CHECK(watch.take().contains(stxlib::ProcessEvent::terminal_resized));
        // Deferred events wait until the deferral ends.
        {
            const stxlib::EventDeferral deferral(watched);
            CHECK(std::raise(SIGINT) == 0);
            CHECK(watch.take().empty());
        }
        CHECK(watch.take().contains(stxlib::ProcessEvent::interrupt));
    }
    // The watch restored the earlier handling and can be taken again.
    struct sigaction current{};
    CHECK(::sigaction(SIGINT, nullptr, &current) == 0 && current.sa_handler == SIG_DFL);
    const stxlib::Result<stxlib::EventWatch> again = stxlib::EventWatch::watch(watched);
    CHECK(again.succeeded());
}
#endif

} // namespace

int main() {
    capabilities_report();
    steady_clocks_never_decrease();
    clocks_agree_with_host();
    wall_clock_is_unix_time();
    time_arithmetic_saturates();
    variable_names();
    set_read_remove();
    concurrent_use();
    entropy_fills();
    terminal_without_one();
#if !defined(_WIN32)
    events();
#endif
    const int status = stxlib_tests::finish("stxlib platform");
    return status;
}
