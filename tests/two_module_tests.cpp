// Two modules, one state (the GUI.Forms coordinator's constraint,
// 2026-11-13; A1 design, section 3.6). The executable and
// stxlib_test_library each link their own copy of stxlib::boundary with
// hidden symbols; both reach STXlib's state through stxlib_state. Shown:
//
//   1. An exception thrown in the executable unwinds a frame opened in the
//      library: the library's frame, not the executable's gate, captures,
//      and the executable's gate attaches that capture (frames [exe gate,
//      library frame], captured at the throw site). This is the case that
//      std::uncaught_exceptions() missed under static runtimes.
//   2. A gate in the library catches an exception thrown in the executable
//      inside an executable frame.
//   3. Both copies see one shadow depth and one record sequence.
//   4. A one-per-process capability (an event watch) taken in the library
//      is busy for the executable, and released by the executable.
//
// Tested, not proved (proofs/REPORT.md).

#include "two_module_library.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <stdexcept>

namespace {

using stxlib::usize;

struct Shown final {
    bool present{false};
    stxlib::ShadowSummary summary{};
    stxlib::ShadowFrameRecord frames[stxlib::shadow_frame_capacity]{};
    stxlib::uptr trace[stxlib::shadow_trace_capacity]{};
};

void print_text(const stxlib::TextView text) {
    std::fwrite(text.data(), 1, text.count(), stdout);
}

Shown show(const char* const title, const stxlib::ErrorRecord& record) {
    std::printf("--- %s\n  operation: ", title);
    print_text(record.operation().name());
    std::printf("\n  kind: ");
    print_text(stxlib::error_kind_name(record.kind()));
    std::printf("\n  message: ");
    print_text(record.message());
    std::printf("\n  type: ");
    char readable[stxlib::exception_type_display_capacity];
    print_text(stxlib::exception_type_for_display(record, stxlib::MutableView<char>(readable)));
    std::printf(" (recorded as ");
    print_text(record.exception_type());
    std::printf(")");
    std::printf("\n  sequence: %llu\n", static_cast<unsigned long long>(record.sequence()));
    Shown shown{};
    stxlib::OperationResult<stxlib::ShadowSummary> found = stxlib::read_shadow(
        record.shadow(), stxlib::MutableView<stxlib::ShadowFrameRecord>(shown.frames),
        stxlib::MutableView<stxlib::uptr>(shown.trace));
    if (found.failed()) {
        std::printf("  shadow: none\n");
        return shown;
    }
    shown.present = true;
    shown.summary = *found.value();
    std::printf("  shadow: captured at the %s\n",
                shown.summary.point == stxlib::CapturePoint::throw_site ? "throw site" : "gate");
    for (usize index = 0; index < shown.summary.frame_count; ++index) {
        std::printf("    frame %zu: ", index);
        std::printf("kind %u subject \"", shown.frames[index].kind);
        print_text(shown.frames[index].subject_text());
        std::printf("\" name \"");
        print_text(shown.frames[index].name_text());
        std::printf("\" [%llu, %llu]\n", static_cast<unsigned long long>(shown.frames[index].first),
                    static_cast<unsigned long long>(shown.frames[index].second));
    }
    std::printf("  trace: %zu return addresses\n", shown.summary.trace_count);
    return shown;
}

bool frame_is(const Shown& shown, const usize index, const stxlib::TextView name, const stxlib::u64 first,
              const stxlib::u64 second) {
    if (index >= shown.summary.frame_count) return false;
    const stxlib::ShadowFrameRecord& frame = shown.frames[index];
    const bool result = frame.kind == stxlib::shadow_frame_kind_gate && frame.name_text() == name &&
                        frame.first == first && frame.second == second;
    return result;
}

// ---- 1. thrown in the executable, through a library frame -------------------

void throws_in_executable(void*) { throw std::invalid_argument("thrown in the executable"); }

struct ThroughLibrary final {
    stxlib::OperationStatus operator()() const {
        two_module::run_in_library_frame(throws_in_executable, nullptr);
        return stxlib::OperationStatus::success();
    }
};

void an_executable_throw_is_captured_by_the_library_frame() {
    ThroughLibrary through;
    const stxlib::OperationStatus status = stxlib::call_gate("test.exe.gate", through);
    CHECK(status.failed());
    const stxlib::ErrorRecord record = *status.error();
    const Shown shown = show("executable throw through a library frame", record);
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(record.message() == stxlib::TextView("thrown in the executable"));
    // Frames in every build (the A2 swap-in); trace and type in a
    // development build only.
    CHECK(shown.present);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.exe.gate", 0, 0));
        CHECK(frame_is(shown, 1, "test.library.frame", 5, 6));
        CHECK((shown.summary.trace_count > 0) == stxlib::shadowing_enabled());
        CHECK(record.exception_type().empty() != stxlib::shadowing_enabled());
    }
    CHECK(stxlib::shadow_depth() == 0);
    CHECK(two_module::library_shadow_depth() == 0);
}

// ---- 2. thrown in the executable, caught by a library gate ------------------

struct ThrowsInFrame final {
    void operator()() const { throw std::invalid_argument("thrown under an executable frame"); }
};

void opens_executable_frame(void*) {
    ThrowsInFrame thrower;
    stxlib::in_shadow_frame("test.exe.frame", 7, 0, thrower);
}

void a_library_gate_catches_an_executable_throw() {
    const stxlib::OperationStatus status = two_module::run_in_library_gate(opens_executable_frame, nullptr);
    CHECK(status.failed());
    const stxlib::ErrorRecord record = *status.error();
    const Shown shown = show("library gate, executable throw", record);
    // The kind depends on the library's copy of the C++ runtime matching
    // the executable's std::invalid_argument: certain with a shared runtime
    // (ELF, Mach-O), unconfirmed with a static runtime per module
    // (Windows). Either answer is printed; the shadow below is what this
    // case proves.
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument ||
          record.kind() == stxlib::ErrorKind::foreign_exception);
    CHECK(record.operation().name() == stxlib::TextView("test.library.gate"));
    CHECK(shown.present);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.library.gate", 0, 0));
        CHECK(frame_is(shown, 1, "test.exe.frame", 7, 0));
    }
    CHECK(stxlib::shadow_depth() == 0);
}

// ---- 2b. GUI.Forms' slot: a slot gate here, a hand frame in the library ----

// GUI.Forms' shape across modules: the C ABI slot's gate (kind
// library_call, GateFrame) in one module, the window's dispatch frame
// opened by hand (ShadowFrameCall: enter_shadow_frame and
// leave_shadow_frame) in the other, and the throw back in the first.
int slot_classifications = 0;

bool classify_counting(void*, stxlib::ExceptionClass&) noexcept {
    slot_classifications = slot_classifications + 1;
    return false;
}

void throws_under_dispatch(void*) { throw std::out_of_range("thrown under the library's dispatch frame"); }

struct SlotThroughLibrary final {
    stxlib::OperationResult<int> operator()() const {
        two_module::run_in_library_dispatch_frame(throws_under_dispatch, nullptr);
        return stxlib::OperationResult<int>::success(0);
    }
};

void a_slot_sees_a_library_hand_frame() {
    slot_classifications = 0;
    const stxlib::ExceptionClassifier classifier{classify_counting, nullptr};
    const stxlib::GateFrame slot{.operation = "test.exe.slot", .first = 24, .second = 3, .kind = 1};
    SlotThroughLibrary body;
    const stxlib::OperationResult<int> result = stxlib::call_gate_classified(classifier, slot, body);
    CHECK(result.failed());
    if (result.succeeded()) return;
    const stxlib::ErrorRecord record = *result.error();
    const Shown shown = show("slot gate, library hand frame", record);
    CHECK(slot_classifications == 1);
    CHECK(record.kind() == stxlib::ErrorKind::out_of_range);
    CHECK(shown.present);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 2);
        CHECK(shown.frames[0].kind == 1 && shown.frames[0].first == 24 && shown.frames[0].second == 3);
        CHECK(shown.frames[1].kind == 2 && shown.frames[1].subject_text() == stxlib::TextView("form.main"));
        CHECK(shown.frames[1].name_text() == stxlib::TextView("key"));
    }
    CHECK(stxlib::shadow_depth() == 0);
    CHECK(two_module::library_shadow_depth() == 0);
}

// ---- 3. one depth, one sequence -------------------------------------------

struct ReadsDepths final {
    usize executable{0};
    usize library{0};

    void operator()() {
        executable = stxlib::shadow_depth();
        library = two_module::library_shadow_depth();
    }
};

void both_copies_share_the_state() {
    ReadsDepths reads;
    stxlib::in_shadow_frame("test.exe.depth", 0, 0, reads);
    CHECK(reads.executable == 1);
    CHECK(reads.library == 1);
    const stxlib::ErrorRecord first = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "test.exe.record");
    const stxlib::ErrorRecord second = two_module::library_record();
    const stxlib::ErrorRecord third = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "test.exe.record");
    std::printf("--- sequences: executable %llu, library %llu, executable %llu\n",
                static_cast<unsigned long long>(first.sequence()), static_cast<unsigned long long>(second.sequence()),
                static_cast<unsigned long long>(third.sequence()));
    CHECK(second.sequence() == first.sequence() + 1);
    CHECK(third.sequence() == second.sequence() + 1);
}

// ---- 4. one event watch per process -----------------------------------------

void one_event_watch_for_both() {
    const stxlib::EventSet interrupts = stxlib::EventSet().with(stxlib::ProcessEvent::interrupt);
    {
        const stxlib::OperationResult<stxlib::EventWatch> held = two_module::library_watch_interrupts();
        CHECK(held.succeeded());
        const stxlib::OperationResult<stxlib::EventWatch> second = stxlib::EventWatch::watch(interrupts);
        CHECK(second.failed() && static_cast<stxlib::HostCode>((*second.error()).code()) == stxlib::HostCode::busy);
    }
    // The library's watch ended in the executable's copy; the flag is free.
    const stxlib::OperationResult<stxlib::EventWatch> again = stxlib::EventWatch::watch(interrupts);
    CHECK(again.succeeded());
}

} // namespace

int main() {
    std::printf("shadowing %s\n", stxlib::shadowing_enabled() ? "on (development build)" : "off (release build)");
    an_executable_throw_is_captured_by_the_library_frame();
    a_library_gate_catches_an_executable_throw();
    a_slot_sees_a_library_hand_frame();
    both_copies_share_the_state();
    one_event_watch_for_both();
    const int status = stxlib_tests::finish("stxlib two modules");
    return status;
}
