// Exception shadowing (survey S1 = 1): a planted throw per boundary kind,
// with the record each produces printed in full and checked. Boundary kinds
// covered here, in one module: a C ABI slot's gate (call_gate), a gate with
// a consumer classifier, a slot gate with GUI.Forms' frame kind and the
// message sink, consumer frames with texts (cut at a whole UTF-8
// sequence), a frame opened by hand (GUI.Forms' ShadowFrameCall), the live
// stack read back, a trampoline entry, a parked and rethrown exception
// (GUI.Forms' class tables, A1 design 3.3), the same with the frames
// parked into the carrier and captures in between (M1 option 2), the room
// park_shadow_into refuses, a stale capture after an
// ordinary handler, and a stack deeper than the capacity. The callback
// gate's record is shown by the must-end case callback_frames
// (death_tests.cpp), and the two-module cases by two_module_tests.cpp.
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/boundary.hpp"
#include "stxlib/capabilities.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

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
    char readable[stxlib::exception_type_display_capacity];
    print_text(stxlib::exception_type_for_display(record, stxlib::MutableView<char>(readable)));
    std::printf(" (recorded as ");
    print_text(record.exception_type());
    std::printf(")");
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
        const stxlib::ShadowFrameRecord& frame = shown.frames[index];
        std::printf("    frame %zu: kind %u flags %u subject \"", index, frame.kind, frame.flags);
        print_text(frame.subject_text());
        std::printf("\" name \"");
        print_text(frame.name_text());
        std::printf("\" [%llu, %llu] #%llu\n", static_cast<unsigned long long>(frame.first),
                    static_cast<unsigned long long>(frame.second), static_cast<unsigned long long>(frame.sequence));
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
    const bool result = frame.kind == stxlib::shadow_frame_kind_gate && frame.name_text() == name &&
                        frame.subject_count == 0 && frame.first == first && frame.second == second;
    return result;
}

// Checks what every gate record carries: frames in every build (the A2
// swap-in); the type and the trace in a development build only (S5 = 1).
void check_development_record(const ShownShadow& shown, const stxlib::ErrorRecord& record) {
    CHECK(shown.present);
    if (!stxlib::shadowing_enabled()) {
        CHECK(record.exception_type().empty());
        CHECK(shown.summary.trace_count == 0);
        return;
    }
    CHECK(!record.exception_type().empty());
    // A host that cannot walk its stack (Emscripten) records none.
    CHECK(shown.summary.trace_count > 0 || !stxlib::capability_supported(stxlib::Capability::stack_trace));
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

// A gate opened with a GateFrame: its frame carries the slot's numbers (a C
// ABI slot's table and slot index), for the plain and the classified gate.
void a_gate_frame_carries_its_numbers() {
    Layout layout;
    const stxlib::GateFrame slot{"test.c_abi", 24, 3};
    const stxlib::Status status = stxlib::call_gate(slot, layout);
    CHECK(status.failed());
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("gate frame", record);
    CHECK(record.operation().name() == stxlib::TextView("test.c_abi"));
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument);
    check_development_record(shown, record);
    if (shown.present) {
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 3);
        CHECK(frame_is(shown, 0, "test.c_abi", 24, 3));
        CHECK(frame_is(shown, 1, "test.layout", 1, 2));
        CHECK(frame_is(shown, 2, "test.measure", 3, 4));
    }

    const stxlib::ExceptionClassifier classifier{classify_disposed, nullptr};
    UsesDisposed uses;
    const stxlib::Status classified =
        stxlib::call_gate_classified(classifier, stxlib::GateFrame{"test.c_abi", 24, 5}, uses);
    const stxlib::ErrorRecord classified_record = *classified.error();
    const ShownShadow classified_shown = show("classified gate frame", classified_record);
    CHECK(classified_record.kind() == stxlib::ErrorKind::disposed && classified_record.code() == 4);
    check_development_record(classified_shown, classified_record);
    if (classified_shown.present) {
        CHECK(classified_shown.summary.frame_count == 2);
        CHECK(frame_is(classified_shown, 0, "test.c_abi", 24, 5));
        CHECK(frame_is(classified_shown, 1, "test.handler", 11, 0));
    }

    CHECK(stxlib::shadow_depth() == 0);
}

// ---- the A2 swap-in: GUI.Forms' frames ------------------------------------

// GUI.Forms' frame kinds (gf_shadow_frame_kind), as a consumer's.
constexpr stxlib::u32 library_call_kind = 1;
constexpr stxlib::u32 input_dispatch_kind = 2;
constexpr stxlib::u32 event_callback_kind = 3;

// The message sink (A2-2 = 1): the classifier copies the full what() into
// the consumer's buffer and counts its calls.
struct SlotContext final {
    char message[512]{};
    usize message_count{0};
    int classify_calls{0};
};

bool classify_slot(void* const context, stxlib::ExceptionClass& decided) noexcept {
    SlotContext& slot = *static_cast<SlotContext*>(context);
    slot.classify_calls = slot.classify_calls + 1;
    try {
        throw;
    } catch (const std::invalid_argument& failure) {
        const char* const what = failure.what();
        usize count = 0;
        while (count < sizeof(slot.message) && what[count] != '\0') {
            slot.message[count] = what[count];
            count = count + 1;
        }
        slot.message_count = count;
        decided.kind = stxlib::ErrorKind::invalid_argument;
        decided.code = 2; // GF_STATUS_INVALID_ARGUMENT's place
        return true;
    } catch (...) {
        return false;
    }
}

struct ThrowsLong final {
    void operator()() const { throw std::invalid_argument(std::string(300, 'w')); }
};

struct SlotBody final {
    stxlib::Result<int> operator()() const {
        ThrowsLong thrower;
        const stxlib::ShadowFrameText dispatch{
            .kind = input_dispatch_kind, .subject = "form.main", .name = "pointer", .sequence = 7};
        stxlib::in_shadow_frame(dispatch, thrower);
        return stxlib::Result<int>::success(0);
    }
};

void a_slot_frame_is_the_whole_frame() {
    SlotContext context{};
    const stxlib::ExceptionClassifier classifier{classify_slot, &context};
    const stxlib::GateFrame slot{
        .operation = "test.c_abi", .first = 24, .second = 3, .kind = library_call_kind};
    SlotBody body;
    const stxlib::Result<int> result = stxlib::call_gate_classified(classifier, slot, body);
    CHECK(result.failed());
    if (result.succeeded()) return;
    const stxlib::ErrorRecord record = *result.error();
    const ShownShadow shown = show("slot gate with GUI.Forms' kind", record);
    CHECK(record.operation().name() == stxlib::TextView("test.c_abi"));
    CHECK(record.kind() == stxlib::ErrorKind::invalid_argument && record.code() == 2);
    // Classified once, inside the handler, with the full message kept by
    // the consumer while the record keeps its 200 bytes.
    CHECK(context.classify_calls == 1);
    CHECK(context.message_count == 300);
    CHECK(record.message().count() == stxlib::error_message_capacity && record.message_truncated());
    check_development_record(shown, record);
    CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
    CHECK(shown.summary.frame_count == 2); // the slot's frame once, not a second gate frame
    const stxlib::ShadowFrameRecord& outer = shown.frames[0];
    CHECK(outer.kind == library_call_kind && outer.first == 24 && outer.second == 3);
    CHECK(outer.subject_count == 0 && outer.name_count == 0 && outer.flags == 0);
    const stxlib::ShadowFrameRecord& inner = shown.frames[1];
    CHECK(inner.kind == input_dispatch_kind && inner.sequence == 7);
    CHECK(inner.subject_text() == stxlib::TextView("form.main") && inner.name_text() == stxlib::TextView("pointer"));
    CHECK(stxlib::shadow_depth() == 0);
}

// Texts longer than their fields are cut at the last whole UTF-8 sequence
// and flagged; the bytes after the count are zero.
struct ThrowsRange final {
    void operator()() const { throw std::out_of_range("cut"); }
};

struct LongTexts final {
    stxlib::Status operator()() const {
        // 46 ASCII bytes, then U+00E9 (2 bytes, ending at 48) and U+20AC
        // (3 bytes, crossing 48): the subject keeps 48 bytes.
        const char subject[] = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrst\xc3\xa9\xe2\x82\xac";
        // 31 ASCII bytes, then U+00E9 crossing 32: the name keeps 31.
        const char name[] = "abcdefghijklmnopqrstuvwxyzabcde\xc3\xa9";
        const stxlib::ShadowFrameText frame{
            .kind = event_callback_kind,
            .subject = stxlib::detail::TextViewAccess::adopt_validated(subject, sizeof(subject) - 1),
            .name = stxlib::detail::TextViewAccess::adopt_validated(name, sizeof(name) - 1)};
        ThrowsRange thrower;
        stxlib::in_shadow_frame(frame, thrower);
        return stxlib::Status::success();
    }
};

void long_texts_are_cut_at_a_whole_sequence() {
    LongTexts target;
    const stxlib::Status status = stxlib::call_gate("test.texts", target);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("texts cut to their fields", record);
    CHECK(shown.present && shown.summary.frame_count == 2);
    if (!shown.present || shown.summary.frame_count != 2) return;
    const stxlib::ShadowFrameRecord& frame = shown.frames[1];
    CHECK(frame.subject_count == 48);
    CHECK(static_cast<unsigned char>(frame.subject[46]) == 0xc3 && static_cast<unsigned char>(frame.subject[47]) == 0xa9);
    CHECK(frame.name_count == 31);
    CHECK(frame.name[31] == '\0');
    CHECK(frame.flags == (stxlib::shadow_subject_truncated | stxlib::shadow_name_truncated));
}

// A frame opened by hand, as GUI.Forms' ShadowFrameCall does from its own
// source file: closed only when its body returned.
struct HandFrame final {
    stxlib::ShadowFrameMark mark;
    bool returned{false};

    explicit HandFrame(const stxlib::ShadowFrameText& frame) noexcept : mark(stxlib::enter_shadow_frame(frame)) {}
    ~HandFrame() { stxlib::leave_shadow_frame(mark, returned); }
    HandFrame(const HandFrame&) = delete;
    HandFrame& operator=(const HandFrame&) = delete;
};

struct HandFramed final {
    stxlib::Status operator()() const {
        const stxlib::ShadowFrameText layout{.kind = 4, .subject = "form.main", .sequence = 41};
        HandFrame frame(layout);
        ThrowsInvalid thrower;
        thrower();
        frame.returned = true;
        return stxlib::Status::success();
    }
};

struct ReadsTheStack final {
    stxlib::ShadowStackState* state{nullptr};
    stxlib::ShadowFrameRecord* frames{nullptr};

    void operator()() const {
        *state = stxlib::read_shadow_stack(stxlib::MutableView<stxlib::ShadowFrameRecord>(frames, 4));
    }
};

void a_hand_frame_and_the_live_stack() {
    HandFramed target;
    const stxlib::Status status = stxlib::call_gate("test.hand", target);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("a frame opened by hand", record);
    CHECK(shown.present && shown.summary.frame_count == 2);
    if (shown.present && shown.summary.frame_count == 2) {
        CHECK(frame_is(shown, 0, "test.hand", 0, 0));
        CHECK(shown.frames[1].kind == 4 && shown.frames[1].sequence == 41);
        CHECK(shown.frames[1].subject_text() == stxlib::TextView("form.main"));
    }
    CHECK(stxlib::shadow_depth() == 0);

    // The live stack, from inside two frames.
    stxlib::ShadowStackState state{};
    stxlib::ShadowFrameRecord frames[4]{};
    ReadsTheStack reads{&state, frames};
    const stxlib::ShadowStackState before = stxlib::read_shadow_stack(stxlib::MutableView<stxlib::ShadowFrameRecord>());
    const stxlib::ShadowFrameText outer{.kind = event_callback_kind, .subject = "button.ok", .name = "click"};
    const stxlib::ShadowFrameMark mark = stxlib::enter_shadow_frame(outer);
    stxlib::in_shadow_frame("test.reader", 5, 6, reads);
    stxlib::leave_shadow_frame(mark, true);
    CHECK(state.depth == 2 && state.recorded == 2);
    CHECK(state.pushes == before.pushes + 2 && state.dropped == before.dropped);
    CHECK(frames[0].kind == event_callback_kind && frames[0].name_text() == stxlib::TextView("click"));
    CHECK(frames[1].kind == stxlib::shadow_frame_kind_gate && frames[1].name_text() == stxlib::TextView("test.reader"));
    CHECK(frames[1].first == 5 && frames[1].second == 6);
    CHECK(stxlib::shadow_depth() == 0);
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

// The same carrier with the frames copied out (park_shadow_into, M1
// option 2): later captures on the thread overwrite every kept record
// before the rethrow (one in a release build, shadow_record_capacity in a
// development build), and the outer gate still files the throw's frames
// from the carrier's copy (restore_shadow_from).
struct OtherFailure final {
    stxlib::Status operator()() const { throw std::length_error("filed in between"); }
};

struct CopiedShadow final {
    stxlib::ParkedShadow parked{};
    stxlib::ShadowFrameRecord frames[stxlib::parked_shadow_frame_room]{};
    stxlib::uptr trace[stxlib::parked_shadow_trace_room]{};
    bool park_failed{false};
};

struct CopyingBaseCall final {
    std::exception_ptr* parked{nullptr};
    CopiedShadow* copy{nullptr};

    void operator()() const {
        try {
            ThrowsUnderBase thrower;
            stxlib::in_shadow_frame("test.base", 2, 0, thrower);
        } catch (...) {
            *parked = std::current_exception();
            const stxlib::Result<stxlib::ParkedShadow> kept =
                stxlib::park_shadow_into(stxlib::MutableView<stxlib::ShadowFrameRecord>((*copy).frames),
                                         stxlib::MutableView<stxlib::uptr>((*copy).trace));
            if (kept.succeeded()) {
                (*copy).parked = *kept.value();
            } else {
                (*copy).park_failed = true;
            }
        }
    }
};

struct CopyingClientEntry final {
    CopyingBaseCall base;

    void operator()() const { stxlib::in_shadow_frame("test.entry", 1, 0, base); }
};

struct CopyingRaise final {
    CopiedShadow* copy{nullptr};
    bool* old_reference_lost{nullptr};

    stxlib::Status operator()() const {
        std::exception_ptr parked{};
        CopyingClientEntry entry{CopyingBaseCall{&parked, copy}};
        stxlib::in_shadow_frame("test.client_entry_call", 0, 0, entry);
        if (parked) {
            // Failures filed between the park and the rethrow, one more than
            // a development build keeps.
            for (usize round = 0; round <= stxlib::shadow_record_capacity; ++round) {
                OtherFailure other;
                const stxlib::Status filed = stxlib::call_gate("test.in_between", other);
                static_cast<void>(filed);
            }
            stxlib::ShadowFrameRecord probe[1]{};
            const stxlib::Result<stxlib::ShadowSummary> old =
                stxlib::read_shadow((*copy).parked.reference, stxlib::MutableView<stxlib::ShadowFrameRecord>(probe),
                                    stxlib::MutableView<stxlib::uptr>());
            *old_reference_lost = old.failed() && (*old.error()).kind() == stxlib::ErrorKind::not_found;
            const stxlib::ShadowReference restored = stxlib::restore_shadow_from(
                (*copy).parked, stxlib::View<stxlib::ShadowFrameRecord>((*copy).frames, (*copy).parked.frame_count),
                stxlib::View<stxlib::uptr>((*copy).trace, (*copy).parked.trace_count));
            static_cast<void>(restored);
            std::rethrow_exception(parked);
        }
        return stxlib::Status::success();
    }
};

void a_copied_park_survives_later_captures() {
    CopiedShadow copy{};
    bool old_reference_lost = false;
    CopyingRaise raise{&copy, &old_reference_lost};
    const stxlib::Status status = stxlib::call_gate("test.raise", raise);
    const stxlib::ErrorRecord record = *status.error();
    const ShownShadow shown = show("parked into the carrier, captures in between, rethrown", record);
    CHECK(!copy.park_failed);
    CHECK(copy.parked.reference.present());
    CHECK(copy.parked.point == stxlib::CapturePoint::throw_site);
    CHECK(copy.parked.frame_count == 4); // test.raise > test.client_entry_call > test.entry > test.base
    CHECK(copy.parked.frames_lost == 0);
    CHECK(old_reference_lost); // the plain reference would have named nothing
    CHECK(record.kind() == stxlib::ErrorKind::out_of_range);
    CHECK(record.message() == stxlib::TextView("row 12"));
    CHECK(shown.present);
    if (shown.present) {
        // The frames at the throw, from the carrier's copy.
        CHECK(shown.summary.point == stxlib::CapturePoint::throw_site);
        CHECK(shown.summary.frame_count == 4);
        CHECK(frame_is(shown, 0, "test.raise", 0, 0));
        CHECK(frame_is(shown, 1, "test.client_entry_call", 0, 0));
        CHECK(frame_is(shown, 2, "test.entry", 1, 0));
        CHECK(frame_is(shown, 3, "test.base", 2, 0));
        CHECK(shown.summary.trace_count == copy.parked.trace_count);
        if (!stxlib::shadowing_enabled()) CHECK(shown.summary.trace_count == 0);
    }
    CHECK(stxlib::shadow_depth() == 0);
}

// park_shadow_into checks the room against the stated size before it parks:
// too small a room parks nothing, so the capture is still pending for the
// next park.
struct ParksWithTooLittleRoom final {
    stxlib::ErrorKind* frames_refusal{nullptr};
    stxlib::ErrorKind* trace_refusal{nullptr};
    stxlib::ParkedShadow* parked{nullptr};

    void operator()() const {
        try {
            ThrowsUnderBase thrower;
            stxlib::in_shadow_frame("test.small_room", 5, 0, thrower);
        } catch (...) {
            stxlib::ShadowFrameRecord short_frames[stxlib::parked_shadow_frame_room - 1]{};
            stxlib::ShadowFrameRecord frames[stxlib::parked_shadow_frame_room]{};
            stxlib::uptr short_trace[1]{};
            const stxlib::Result<stxlib::ParkedShadow> too_few_frames = stxlib::park_shadow_into(
                stxlib::MutableView<stxlib::ShadowFrameRecord>(short_frames), stxlib::MutableView<stxlib::uptr>());
            if (too_few_frames.failed()) *frames_refusal = (*too_few_frames.error()).kind();
            const stxlib::Result<stxlib::ParkedShadow> too_short_trace = stxlib::park_shadow_into(
                stxlib::MutableView<stxlib::ShadowFrameRecord>(frames), stxlib::MutableView<stxlib::uptr>(short_trace));
            if (too_short_trace.failed()) *trace_refusal = (*too_short_trace.error()).kind();
            const stxlib::Result<stxlib::ParkedShadow> kept = stxlib::park_shadow_into(
                stxlib::MutableView<stxlib::ShadowFrameRecord>(frames), stxlib::MutableView<stxlib::uptr>());
            if (kept.succeeded()) *parked = *kept.value();
        }
    }
};

void a_park_into_too_little_room_parks_nothing() {
    stxlib::ErrorKind frames_refusal = stxlib::ErrorKind::internal_failure;
    stxlib::ErrorKind trace_refusal = stxlib::ErrorKind::internal_failure;
    stxlib::ParkedShadow parked{};
    ParksWithTooLittleRoom target{&frames_refusal, &trace_refusal, &parked};
    target();
    CHECK(frames_refusal == stxlib::ErrorKind::limit_exceeded);
    CHECK(trace_refusal == stxlib::ErrorKind::limit_exceeded);
    // The third call found the throw's capture still pending.
    CHECK(parked.reference.present());
    CHECK(parked.point == stxlib::CapturePoint::throw_site);
    CHECK(parked.frame_count == 1);
    CHECK(parked.trace_count == 0); // no trace room given
    // A carrier that parked nothing restores nothing.
    const stxlib::ShadowReference none =
        stxlib::restore_shadow_from(stxlib::ParkedShadow{}, stxlib::View<stxlib::ShadowFrameRecord>(),
                                    stxlib::View<stxlib::uptr>());
    CHECK(!none.present());
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
        const Deep deep{70};
        deep();
        return stxlib::Status::success();
    }
};

void a_deep_stack_counts_what_it_cannot_keep() {
    const stxlib::ShadowStackState before = stxlib::read_shadow_stack(stxlib::MutableView<stxlib::ShadowFrameRecord>());
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
    const stxlib::ShadowStackState after = stxlib::read_shadow_stack(stxlib::MutableView<stxlib::ShadowFrameRecord>());
    CHECK(after.dropped == before.dropped + (usize{71} - stxlib::shadow_frame_capacity));
    CHECK(after.pushes == before.pushes + 71);
    CHECK(stxlib::shadow_depth() == 0);
}

} // namespace

int main() {
    std::printf("shadowing %s\n", stxlib::shadowing_enabled() ? "on (development build)" : "off (release build)");
    a_slot_gate_records_the_stack_at_the_throw();
    a_classified_gate_records_the_consumer_kind();
    a_gate_frame_carries_its_numbers();
    a_slot_frame_is_the_whole_frame();
    long_texts_are_cut_at_a_whole_sequence();
    a_hand_frame_and_the_live_stack();
    a_trampoline_entry_records_the_target_stack();
    a_parked_exception_keeps_its_throw_site();
    a_copied_park_survives_later_captures();
    a_park_into_too_little_room_parks_nothing();
    a_consumed_capture_is_not_reused();
    a_deep_stack_counts_what_it_cannot_keep();
    const int status = stxlib_tests::finish("stxlib shadowing");
    return status;
}
