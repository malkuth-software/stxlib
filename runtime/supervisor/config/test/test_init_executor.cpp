// test_init_executor.cpp: host tests of the init section executor, with a
// recorder in place of the system: it logs every call in order and answers
// from a script (which paths are present, what each action returns).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../boot_config.h"
#include "../init_executor.h"
#include "test_support.h"

namespace {

using stxconfig::BootConfig;
using stxconfig::InitSummary;
using stxconfig::InitWorkspace;
using stxconfig::StepOutcome;
using stxtest::check;
using stxtest::same_text;
using stxtest::text_of;

// A log of calls, one text line per call, appended in order.
constexpr std::size_t log_bytes = 8192;

class Recorder final : public stxconfig::InitActions {
public:
    Recorder() noexcept : log_{}, log_length_(0), mount_outcome_(StepOutcome::done()),
                          run_outcome_(StepOutcome::exited(0)), present_path_(nullptr), reports_(0) {}

    // Paths equal to present_path are present; all others absent.
    void set_present_path(const char* path) noexcept {
        present_path_ = path;
    }
    void set_mount_outcome(const StepOutcome& outcome) noexcept {
        mount_outcome_ = outcome;
    }
    void set_run_outcome(const StepOutcome& outcome) noexcept {
        run_outcome_ = outcome;
    }
    [[nodiscard]] const char* log() const noexcept {
        return log_;
    }
    [[nodiscard]] std::size_t reports() const noexcept {
        return reports_;
    }

    [[nodiscard]] bool is_present(stxconfig::PresenceKind kind, const char* path) noexcept override {
        append(kind == stxconfig::PresenceKind::device ? "present? device " : "present? file ");
        append(path);
        append("\n");
        return present_path_ != nullptr && same_text(path, present_path_);
    }
    [[nodiscard]] StepOutcome mount(stxconfig::FileSystemKind file_system, const char* device, const char* directory,
                                    stxconfig::MountAccess access) noexcept override {
        append(file_system == stxconfig::FileSystemKind::ufs ? "mount ufs " : "mount msdosfs ");
        append(device);
        append(" ");
        append(directory);
        append(access == stxconfig::MountAccess::read_only ? " ro\n" : " rw\n");
        return mount_outcome_;
    }
    [[nodiscard]] StepOutcome wait_for(stxconfig::PresenceKind kind, const char* path,
                                       std::uint32_t timeout_ms) noexcept override {
        append(kind == stxconfig::PresenceKind::device ? "wait device " : "wait file ");
        append(path);
        append(timeout_ms == 2000u ? " 2000\n" : " other\n");
        return StepOutcome::timed_out();
    }
    [[nodiscard]] StepOutcome start_service(const char* service, const char* const* environment) noexcept override {
        append("start ");
        append(service);
        append(" env");
        for (std::size_t index = 0; environment[index] != nullptr; index += 1) {
            append(" ");
            append(environment[index]);
        }
        append("\n");
        return StepOutcome::unavailable();
    }
    [[nodiscard]] StepOutcome run_program(const char* const* arguments,
                                          const char* const* environment) noexcept override {
        append("run");
        for (std::size_t index = 0; arguments[index] != nullptr; index += 1) {
            append(" [");
            append(arguments[index]);
            append("]");
        }
        append(" env");
        for (std::size_t index = 0; environment[index] != nullptr; index += 1) {
            append(" ");
            append(environment[index]);
        }
        append("\n");
        return run_outcome_;
    }
    void report(const stxconfig::StepReport& step) noexcept override {
        reports_ += 1;
        char number[16];
        std::snprintf(number, sizeof(number), "%u", step.line);
        append("report ");
        append(number);
        append(" ");
        append(step.keyword);
        append(" ");
        append(step.outcome.name());
        append("\n");
    }

private:
    void append(const char* text) noexcept {
        const std::size_t length = stxtest::c_length(text);
        if (log_length_ + length + 1u > log_bytes) {
            return;
        }
        std::memcpy(&log_[log_length_], text, length);
        log_length_ += length;
        log_[log_length_] = '\0';
    }

    char log_[log_bytes];
    std::size_t log_length_;
    StepOutcome mount_outcome_;
    StepOutcome run_outcome_;
    const char* present_path_;
    std::size_t reports_;
};

BootConfig config;
InitWorkspace workspace;

[[nodiscard]] bool parse(const char* text) {
    return stxconfig::parse_boot_config(text_of(text), config).is_success();
}

void test_order_and_outcomes() {
    const char* const test = "order and outcomes";
    const bool parsed = parse(
        "[init]\n"
        "mount ufs /dev/gpt/user /user rw\n"
        "wait-for device /dev/x 2000\n"
        "if-present file /absent run /never\n"
        "if-present device /dev/here start web\n"
        "set LANG C\n"
        "run /bin/a \"b c\"\n"
        "set LANG POSIX\n"
        "run /bin/d\n"
        "[service-web]\n"
        "command = /bin/web\n");
    check(parsed, test, "parses");
    if (!parsed) {
        return;
    }
    Recorder recorder;
    recorder.set_present_path("/dev/here");
    recorder.set_mount_outcome(StepOutcome::failed(16));
    const InitSummary summary = stxconfig::run_init_section(config, recorder, workspace);
    const char* const expected =
        "mount ufs /dev/gpt/user /user rw\n"
        "report 2 mount failed\n"
        "wait device /dev/x 2000\n"
        "report 3 wait-for timed-out\n"
        "present? file /absent\n"
        "report 4 run skipped\n"
        "present? device /dev/here\n"
        "start web env PATH=/sbin:/bin:/usr/sbin:/usr/bin HOME=/\n"
        "report 5 start unavailable\n"
        "report 6 set ok\n"
        "run [/bin/a] [b c] env PATH=/sbin:/bin:/usr/sbin:/usr/bin HOME=/ LANG=C\n"
        "report 7 run exited\n"
        "report 8 set ok\n"
        "run [/bin/d] env PATH=/sbin:/bin:/usr/sbin:/usr/bin HOME=/ LANG=POSIX\n"
        "report 9 run exited\n";
    check(same_text(recorder.log(), expected), test, "calls in order, environment replaced in place");
    if (!same_text(recorder.log(), expected)) {
        std::printf("--- log\n%s---\n", recorder.log());
    }
    check(summary.commands == 8 && summary.skipped == 1 && summary.failed == 3, test,
          "summary: mount failed, wait timed out, start unavailable");
    check(recorder.reports() == 8, test, "one report per command");
}

void test_exit_status_counting() {
    const char* const test = "exit status";
    check(parse("[init]\nrun /bin/x\n"), test, "parses");
    Recorder recorder;
    // A normal exit with status 1 (waitpid encoding: status << 8).
    recorder.set_run_outcome(StepOutcome::exited(1 << 8));
    InitSummary summary = stxconfig::run_init_section(config, recorder, workspace);
    check(summary.failed == 1, test, "exit 1 counts as a failure");
    Recorder signalled;
    // Killed by signal 9: low bits 9.
    signalled.set_run_outcome(StepOutcome::exited(9));
    summary = stxconfig::run_init_section(config, signalled, workspace);
    check(summary.failed == 1, test, "death by signal counts as a failure");
    Recorder clean;
    summary = stxconfig::run_init_section(config, clean, workspace);
    check(summary.failed == 0 && summary.skipped == 0 && summary.commands == 1, test, "exit 0 does not");
}

void test_environment_limits() {
    const char* const test = "environment";
    InitWorkspace local;
    check(local.entry_count() == 2, test, "base environment of two");
    // 30 new names fill the table (32 with PATH and HOME); the 31st fails.
    char name[8];
    for (std::size_t index = 0; index < stxconfig::environment_entries_max - 2u; index += 1) {
        std::snprintf(name, sizeof(name), "V%zu", index);
        check(local.set(name, "x"), test, "set within the table");
    }
    check(!local.set("ONE_TOO_MANY", "x"), test, "a new name past the table is refused");
    check(local.set("V3", "replaced"), test, "an existing name is still replaced when full");
    check(local.entry_count() == stxconfig::environment_entries_max, test, "table full");
    const char* const* vector = local.environment();
    check(same_text(vector[0], "PATH=/sbin:/bin:/usr/sbin:/usr/bin") && same_text(vector[5], "V3=replaced") &&
              vector[stxconfig::environment_entries_max] == nullptr,
          test, "vector contents and terminator");
    local.reset();
    check(local.entry_count() == 2, test, "reset returns to the base");

    // Through the executor: environment-full is reported and counted.
    char text[4096];
    std::size_t length = 0;
    length += static_cast<std::size_t>(std::snprintf(&text[length], sizeof(text) - length, "[init]\n"));
    for (std::size_t index = 0; index < stxconfig::environment_entries_max - 1u; index += 1) {
        length += static_cast<std::size_t>(std::snprintf(&text[length], sizeof(text) - length, "set W%zu y\n", index));
    }
    const bool parsed = stxconfig::parse_boot_config(std::span<const char>(text, length), config).is_success();
    check(parsed, test, "31 sets parse");
    Recorder recorder;
    const InitSummary summary = stxconfig::run_init_section(config, recorder, workspace);
    check(summary.failed == 1 && summary.commands == 31, test, "the 31st new name fails as environment-full");
    check(std::strstr(recorder.log(), "report 32 set environment-full") != nullptr, test, "reported on its line");
}

void test_empty_and_absent_init() {
    const char* const test = "empty init";
    check(parse("[system]\nhostname = x\n"), test, "parses");
    Recorder recorder;
    const InitSummary summary = stxconfig::run_init_section(config, recorder, workspace);
    check(summary.commands == 0 && recorder.reports() == 0 && same_text(recorder.log(), ""), test,
          "no init section: no calls");
}

void test_outcome_names() {
    const char* const test = "outcome names";
    check(same_text(StepOutcome::done().name(), "ok") && same_text(StepOutcome::skipped().name(), "skipped") &&
              same_text(StepOutcome::exited(0).name(), "exited") &&
              same_text(StepOutcome::failed(2).name(), "failed") &&
              same_text(StepOutcome::timed_out().name(), "timed-out") &&
              same_text(StepOutcome::unavailable().name(), "unavailable") &&
              same_text(StepOutcome::environment_full().name(), "environment-full"),
          test, "names");
    check(!StepOutcome::done().counts_as_failure() && !StepOutcome::skipped().counts_as_failure() &&
              !StepOutcome::exited(0).counts_as_failure() && StepOutcome::exited(256).counts_as_failure() &&
              StepOutcome::failed(2).counts_as_failure() && StepOutcome::timed_out().counts_as_failure() &&
              StepOutcome::unavailable().counts_as_failure() && StepOutcome::environment_full().counts_as_failure(),
          test, "failure counting");
}

}  // namespace

int main() {
    test_order_and_outcomes();
    test_exit_status_counting();
    test_environment_limits();
    test_empty_and_absent_init();
    test_outcome_names();
    return stxtest::finish("test_init_executor");
}
