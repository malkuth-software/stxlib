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
    stxlib::Result<stxlib::ShadowSummary> found = stxlib::read_shadow(
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
        print_text(shown.frames[index].operation);
        std::printf(" [%llu, %llu]\n", static_cast<unsigned long long>(shown.frames[index].first),
                    static_cast<unsigned long long>(shown.frames[index].second));
    }
    std::printf("  trace: %zu return addresses\n", shown.summary.trace_count);
    return shown;
}

bool frame_is(const Shown& shown, const usize index, const stxlib::TextView name, const stxlib::u64 first,
              const stxlib::u64 second) {
    if (index >= shown.summary.frame_count) return false;
    const stxlib::ShadowFrameRecord& frame = shown.frames[index];
    const bool result = frame.operation == name && frame.first == first && frame.second == second;
    return result;
}

// ---- 1. thrown in the executable, through a library frame -------------------

void throws_in_executable(void*) { throw std::invalid_argument("thrown in the executable"); }

struct ThroughLibrary final {
    stxlib::Status operator()() const {
        two_module::run_in_library_frame(throws_in_executable, nullptr);
        return stxlib::Status::success();
    }
};

void an_executable_throw_is_captured_by_the_library_frame() {
    ThroughLibrary through;
    const stxlib::Status status = stxlib::call_gate("test.exe.gate", through);
    CHECK(status.failed());
    const stxlib::ErrorRecord record = *status.error();
    const Shown shown = show("executable throw through a library frame", record);
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(record.message() == stxlib::TextView("thrown in the executable"));
    CHECK(shown.present == stxlib::shadowing_enabled());
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.exe.gate", 0, 0));
        CHECK(frame_is(shown, 1, "test.library.frame", 5, 6));
        CHECK(shown.summary.trace_count > 0);
        CHECK(!record.exception_type().empty());
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
    const stxlib::Status status = two_module::run_in_library_gate(opens_executable_frame, nullptr);
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
    CHECK(shown.present == stxlib::shadowing_enabled());
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 2);
        CHECK(frame_is(shown, 0, "test.library.gate", 0, 0));
        CHECK(frame_is(shown, 1, "test.exe.frame", 7, 0));
    }
    CHECK(stxlib::shadow_depth() == 0);
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
        const stxlib::Result<stxlib::EventWatch> held = two_module::library_watch_interrupts();
        CHECK(held.succeeded());
        const stxlib::Result<stxlib::EventWatch> second = stxlib::EventWatch::watch(interrupts);
        CHECK(second.failed() && static_cast<stxlib::HostCode>((*second.error()).code()) == stxlib::HostCode::busy);
    }
    // The library's watch ended in the executable's copy; the flag is free.
    const stxlib::Result<stxlib::EventWatch> again = stxlib::EventWatch::watch(interrupts);
    CHECK(again.succeeded());
}

} // namespace

int main() {
    std::printf("shadowing %s\n", stxlib::shadowing_enabled() ? "on (development build)" : "off (release build)");
    an_executable_throw_is_captured_by_the_library_frame();
    a_library_gate_catches_an_executable_throw();
    both_copies_share_the_state();
    one_event_watch_for_both();
    const int status = stxlib_tests::finish("stxlib two modules");
    return status;
}
