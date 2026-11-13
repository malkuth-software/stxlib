// Exception shadowing (survey S1 = 1): a planted throw per boundary kind,
// with the record each produces printed in full and checked. Boundary kinds
// covered here, in one module: a C ABI slot's gate (call_gate), a gate with
// a consumer classifier, a trampoline entry, a parked and rethrown
// exception (GUI.Forms' class tables, A1 design 3.3), a stale capture after
// an ordinary handler, and a stack deeper than the capacity. The callback
// gate's record is shown by the must-end case callback_frames
// (death_tests.cpp), and the two-module cases by two_module_tests.cpp.
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>

namespace {

using stxlib::usize;

// ---- showing a record -------------------------------------------------------

struct ShownShadow final {
    bool present{false};
    stxlib::ShadowSummary summary{};
    stxlib::ShadowFrameRecord frames[stxlib::shadow_frame_capacity]{};
    stxlib::uptr trace[stxlib::shadow_trace_capacity]{};
};

void print_text(const stxlib::TextView text) {
    std::fwrite(text.data(), 1, text.count(), stdout);
}

// Prints every field of the record and its shadow record, and returns the
// shadow record for checking.
ShownShadow show(const char* const title, const stxlib::ErrorRecord& record) {
    std::printf("--- %s\n  operation: ", title);
    print_text(record.operation().name());
    std::printf("\n  kind: ");
    print_text(stxlib::error_kind_name(record.kind()));
    std::printf("\n  code: %u\n  message: ", record.code());
    print_text(record.message());
    std::printf("%s\n  type: ", record.message_truncated() ? " (truncated)" : "");
    print_text(record.exception_type());
    std::printf("\n  sequence: %llu\n", static_cast<unsigned long long>(record.sequence()));
    ShownShadow shown{};
    stxlib::Result<stxlib::ShadowSummary> found = stxlib::read_shadow(
        record.shadow(), stxlib::MutableView<stxlib::ShadowFrameRecord>(shown.frames),
        stxlib::MutableView<stxlib::uptr>(shown.trace));
    if (found.failed()) {
        std::printf("  shadow: none\n");
        return shown;
    }
    shown.present = true;
    shown.summary = *found.value();
    std::printf("  shadow: %llu, captured at the %s\n", static_cast<unsigned long long>(record.shadow().sequence),
                shown.summary.point == stxlib::CapturePoint::throw_site ? "throw site" : "gate");
    for (usize index = 0; index < shown.summary.frame_count; ++index) {
        std::printf("    frame %zu: ", index);
        print_text(shown.frames[index].operation);
        std::printf(" [%llu, %llu]\n", static_cast<unsigned long long>(shown.frames[index].first),
                    static_cast<unsigned long long>(shown.frames[index].second));
    }
    if (shown.summary.frames_lost != 0) std::printf("    (%zu deeper frames not recorded)\n", shown.summary.frames_lost);
    std::printf("  trace: %zu return addresses:", shown.summary.trace_count);
    for (usize index = 0; index < shown.summary.trace_count; ++index) {
        std::printf(" %#llx", static_cast<unsigned long long>(shown.trace[index]));
    }
    std::printf("\n");
    return shown;
}

bool frame_is(const ShownShadow& shown, const usize index, const stxlib::TextView name, const stxlib::u64 first,
              const stxlib::u64 second) {
    if (index >= shown.summary.frame_count) return false;
    const stxlib::ShadowFrameRecord& frame = shown.frames[index];
    const bool result = frame.operation == name && frame.first == first && frame.second == second;
    return result;
}

// Checks what every development-build gate record carries.
void check_development_record(const ShownShadow& shown, const stxlib::ErrorRecord& record) {
    if (!stxlib::shadowing_enabled()) {
        CHECK(!shown.present);
        CHECK(record.exception_type().empty());
        return;
    }
    CHECK(shown.present);
    CHECK(!record.exception_type().empty());
    CHECK(shown.summary.trace_count > 0);
}

// ---- targets ----------------------------------------------------------------

struct ThrowsInvalid final {
    void operator()() const { throw std::invalid_argument("width must be positive"); }
};

struct Measure final {
    void operator()() const {
        ThrowsInvalid thrower;
        stxlib::in_shadow_frame("test.measure", 3, 4, thrower);
    }
};

struct Layout final {
    stxlib::Status operator()() const {
        Measure measure;
        stxlib::in_shadow_frame("test.layout", 1, 2, measure);
        return stxlib::Status::success();
    }
};

void a_slot_gate_records_the_stack_at_the_throw() {
    Layout layout;
    const stxlib::Status status = stxlib::call_gate("test.slot", layout);
    CHECK(status.failed());
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("C ABI slot gate", record);
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(record.message() == stxlib::TextView("width must be positive"));
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 3);
        CHECK(frame_is(shown, 0, "test.slot", 0, 0));
        CHECK(frame_is(shown, 1, "test.layout", 1, 2));
        CHECK(frame_is(shown, 2, "test.measure", 3, 4));
    }
    CHECK(stxlib::shadow_depth() == 0);
}

struct DisposedError final {};

bool classify_disposed(void*, stxlib::ExceptionClass& decided) noexcept {
    try {
        throw;
    } catch (const DisposedError&) {
        decided.kind = stxlib::ErrorKind::disposed;
        decided.code = 4;
        return true;
    } catch (...) {
        return false;
    }
}

struct ThrowsDisposed final {
    void operator()() const { throw DisposedError(); }
};

struct UsesDisposed final {
    stxlib::Status operator()() const {
        ThrowsDisposed thrower;
        stxlib::in_shadow_frame("test.handler", 11, 0, thrower);
        return stxlib::Status::success();
    }
};

void a_classified_gate_records_the_consumer_kind() {
    const stxlib::ExceptionClassifier classifier{classify_disposed, nullptr};
    UsesDisposed uses;
    const stxlib::Status status = stxlib::call_gate_classified(classifier, "test.classified", uses);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("classified gate", record);
    CHECK(record.kind() == stxlib::ErrorKind::disposed && record.code() == 4);
    CHECK(record.message().empty());
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.classified", 0, 0));
        CHECK(frame_is(shown, 1, "test.handler", 11, 0));
    }
}

struct TrampolineTarget final {
    stxlib::Status operator()(int& value) {
        ThrowsInvalid thrower;
        stxlib::in_shadow_frame("test.inside_target", static_cast<stxlib::u64>(value), 0, thrower);
        return stxlib::Status::success();
    }
};

void a_trampoline_entry_records_the_target_stack() {
    stxlib::Result<stxlib::Trampolines<TrampolineTarget>> made =
        stxlib::Trampolines<TrampolineTarget>::create("test.trampoline", 2);
    stxlib::Trampolines<TrampolineTarget> table =
        static_cast<stxlib::Result<stxlib::Trampolines<TrampolineTarget>>&&>(made).take_value();
    TrampolineTarget target;
    stxlib::Result<stxlib::TrampolineHandle> attached = table.attach(target);
    int value = 9;
    const stxlib::Status status = table.invoke(*attached.value(), value);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("trampoline entry", record);
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument);
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.trampoline", 0, 0));
        CHECK(frame_is(shown, 1, "test.inside_target", 9, 0));
    }
}

// GUI.Forms' class tables (A1 design 3.3): an exception under `base` is
// caught, parked with its shadow, reported as a status, and rethrown after
// the client's entry returns, to reach the outer gate.
struct ThrowsUnderBase final {
    void operator()() const { throw std::out_of_range("row 12"); }
};

struct BaseCall final {
    std::exception_ptr* parked{nullptr};
    stxlib::ShadowReference* parked_shadow{nullptr};

    void operator()() const {
        try {
            ThrowsUnderBase thrower;
            stxlib::in_shadow_frame("test.base", 2, 0, thrower);
        } catch (...) {
            *parked = std::current_exception();
            *parked_shadow = stxlib::park_shadow();
        }
    }
};

struct ClientEntry final {
    BaseCall base;

    void operator()() const { stxlib::in_shadow_frame("test.entry", 1, 0, base); }
};

struct Raise final {
    stxlib::Status operator()() const {
        std::exception_ptr parked{};
        stxlib::ShadowReference parked_shadow{};
        ClientEntry entry{BaseCall{&parked, &parked_shadow}};
        stxlib::in_shadow_frame("test.client_entry_call", 0, 0, entry);
        if (parked) {
            stxlib::restore_shadow(parked_shadow);
            std::rethrow_exception(parked);
        }
        return stxlib::Status::success();
    }
};

void a_parked_exception_keeps_its_throw_site() {
    Raise raise;
    const stxlib::Status status = stxlib::call_gate("test.raise", raise);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("parked and rethrown (class tables)", record);
    CHECK(record.kind() == stxlib::ErrorKind::out_of_range);
    CHECK(record.message() == stxlib::TextView("row 12"));
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 4);
        CHECK(frame_is(shown, 0, "test.raise", 0, 0));
        CHECK(frame_is(shown, 1, "test.client_entry_call", 0, 0));
        CHECK(frame_is(shown, 2, "test.entry", 1, 0));
        CHECK(frame_is(shown, 3, "test.base", 2, 0));
    }
    CHECK(stxlib::shadow_depth() == 0);
}

// An exception an ordinary handler consumed must not lend its capture to
// the next one.
struct ConsumesThenThrows final {
    stxlib::Status operator()() const {
        try {
            Measure measure;
            stxlib::in_shadow_frame("test.consumed", 0, 0, measure);
        } catch (const std::invalid_argument&) {
            // handled here
        }
        Succeeds succeeds;
        stxlib::in_shadow_frame("test.after", 0, 0, succeeds);
        throw std::length_error("second failure");
    }

    struct Succeeds final {
        void operator()() const {}
    };
};

void a_consumed_capture_is_not_reused() {
    ConsumesThenThrows target;
    const stxlib::Status status = stxlib::call_gate("test.fresh", target);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("after an ordinary handler", record);
    CHECK(record.kind() == stxlib::ErrorKind::limit_exceeded);
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.frame_count == 1);
        CHECK(frame_is(shown, 0, "test.fresh", 0, 0));
    }
}

// Nests frames to a fixed depth, then throws: bounded by `remaining`.
struct Deep final {
    usize remaining{0};

    void operator()() const {
        if (remaining == 0) throw std::invalid_argument("deep");
        Deep inner{remaining - 1};
        stxlib::in_shadow_frame("test.deep", static_cast<stxlib::u64>(remaining), 0, inner);
    }
};

struct DeepGate final {
    stxlib::Status operator()() const {
        Deep deep{70};
        deep();
        return stxlib::Status::success();
    }
};

void a_deep_stack_counts_what_it_cannot_keep() {
    DeepGate target;
    const stxlib::Status status = stxlib::call_gate("test.deep_gate", target);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("71 frames", record);
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.frame_count == stxlib::shadow_frame_capacity);
        CHECK(shown.summary.frames_lost == usize{71} - stxlib::shadow_frame_capacity);
        CHECK(frame_is(shown, 0, "test.deep_gate", 0, 0));
        CHECK(frame_is(shown, 1, "test.deep", 70, 0));
    }
    CHECK(stxlib::shadow_depth() == 0);
}

} // namespace

int main() {
    std::printf("shadowing %s\n", stxlib::shadowing_enabled() ? "on (development build)" : "off (release build)");
    a_slot_gate_records_the_stack_at_the_throw();
    a_classified_gate_records_the_consumer_kind();
    a_trampoline_entry_records_the_target_stack();
    a_parked_exception_keeps_its_throw_site();
    a_consumed_capture_is_not_reused();
    a_deep_stack_counts_what_it_cannot_keep();
    const int status = stxlib_tests::finish("stxlib shadowing");
    return status;
}
