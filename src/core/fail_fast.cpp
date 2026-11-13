// Failing fast with a record (include/stxlib/fail_fast.hpp).
//
// The reporter and each thread's entry count live in stxlib_state
// (state.hpp), so a reporter installed by one module is the one every
// module's fail_fast calls.
//
// STXlib's own report, when no reporter is installed: one line for the
// record, then, when the record has a shadow record (development builds),
// one line of logical frames, outermost first, and one line of return
// addresses, innermost first, for symbolising after the fact:
//
//   stxlib: fail fast: <operation>: <kind> [code <n>] [foreign <family> <value>] [type <mangled>]: <message>
//   stxlib: frames (<throw_site|gate>): <operation>[<first>,<second>] > ... [+<n> deeper]
//   stxlib: trace: 0x<address> 0x<address> ...

#include "stxlib/fail_fast.hpp"

#include "stxlib/shadow.hpp"

#include "core/report.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>

namespace stxlib {

namespace {

void write_shadow(const ShadowReference reference) noexcept {
    ShadowFrameRecord frames[shadow_frame_capacity]{};
    uptr trace[shadow_trace_capacity]{};
    const Result<ShadowSummary> read =
        read_shadow(reference, MutableView<ShadowFrameRecord>(frames), MutableView<uptr>(trace));
    if (read.failed()) return;
    const ShadowSummary summary = *read.value();
    report::text("stxlib: frames (");
    report::text(summary.point == CapturePoint::throw_site ? TextView("throw_site") : TextView("gate"));
    report::text("):");
    for (usize index = 0; index < summary.frame_count; ++index) {
        report::text(index == 0 ? TextView(" ") : TextView(" > "));
        report::text(frames[index].operation);
        report::text("[");
        report::unsigned_number(frames[index].first);
        report::text(",");
        report::unsigned_number(frames[index].second);
        report::text("]");
    }
    if (summary.frames_lost != 0) {
        report::text(" [+");
        report::unsigned_number(static_cast<u64>(summary.frames_lost));
        report::text(" deeper]");
    }
    report::text("\nstxlib: trace:");
    for (usize index = 0; index < summary.trace_count; ++index) {
        report::text(" ");
        report::hexadecimal(static_cast<u64>(trace[index]));
    }
    report::text("\n");
}

void write_own_report(const ErrorRecord& record) noexcept {
    report::text("stxlib: fail fast: ");
    report::text(record.operation().name());
    report::text(": ");
    report::text(error_kind_name(record.kind()));
    if (record.code() != 0) {
        report::text(" code ");
        report::unsigned_number(static_cast<u64>(record.code()));
    }
    const ForeignCode foreign = record.foreign();
    if (foreign.present()) {
        report::text(" foreign ");
        report::unsigned_number(static_cast<u64>(foreign.family()));
        report::text(" ");
        report::signed_number(foreign.value());
    }
    char readable[exception_type_display_capacity];
    const TextView type = exception_type_for_display(record, MutableView<char>(readable));
    if (!type.empty()) {
        report::text(" type ");
        report::text(type);
    }
    report::text(": ");
    report::text(record.message());
    if (record.message_truncated()) report::text("...");
    report::text("\n");
    if (record.shadow().present()) write_shadow(record.shadow());
}

} // namespace

Status install_fail_fast_reporter(const FailFastReporter reporter) noexcept {
    if (reporter.report == nullptr) {
        const ErrorRecord unbound = ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fail_fast.install",
                                                      ErrorDetail{.message = "the reporter has no function"});
        return Status::failure(unbound);
    }
    state::ProcessState& process = state::current_process();
    int expected = static_cast<int>(state::ReporterPhase::none);
    const bool claimed = process.reporter_phase.compare_exchange_strong(
        expected, static_cast<int>(state::ReporterPhase::installing), std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (!claimed) {
        const ErrorRecord taken = ErrorRecord::make(ErrorKind::already_exists, "stxlib.fail_fast.install",
                                                    ErrorDetail{.message = "a reporter is already installed"});
        return Status::failure(taken);
    }
    process.reporter = reporter;
    process.reporter_phase.store(static_cast<int>(state::ReporterPhase::installed), std::memory_order_release);
    return Status::success();
}

void fail_fast(const ErrorRecord& record) noexcept {
    state::ThreadState& thread = state::current_thread();
    thread.fail_fast_depth = thread.fail_fast_depth + 1;
    state::ProcessState& process = state::current_process();
    const int phase = process.reporter_phase.load(std::memory_order_acquire);
    const bool use_reporter =
        thread.fail_fast_depth == 1 && phase == static_cast<int>(state::ReporterPhase::installed);
    if (use_reporter) {
        const FailFastReporter reporter = process.reporter;
        reporter.report(reporter.context, record);
    } else {
        write_own_report(record);
    }
    host::end_process_abnormally();
}

} // namespace stxlib
