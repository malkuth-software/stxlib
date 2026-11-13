// The library half of the two-module test (two_module_library.hpp).

#include "two_module_library.hpp"

namespace two_module {

namespace {

struct CallsBack final {
    Callback callback{nullptr};
    void* context{nullptr};

    void operator()() const { callback(context); }
};

struct CallsBackForGate final {
    Callback callback{nullptr};
    void* context{nullptr};

    stxlib::Status operator()() const {
        callback(context);
        return stxlib::Status::success();
    }
};

// GUI.Forms' ShadowFrameCall, in this module's source.
struct DispatchFrame final {
    stxlib::ShadowFrameMark mark;
    bool returned{false};

    explicit DispatchFrame(const stxlib::ShadowFrameText& frame) noexcept : mark(stxlib::enter_shadow_frame(frame)) {}
    ~DispatchFrame() { stxlib::leave_shadow_frame(mark, returned); }
    DispatchFrame(const DispatchFrame&) = delete;
    DispatchFrame& operator=(const DispatchFrame&) = delete;
};

} // namespace

void run_in_library_dispatch_frame(const Callback callback, void* const context) {
    const stxlib::ShadowFrameText dispatch{.kind = 2, .subject = "form.main", .name = "key"};
    DispatchFrame frame(dispatch);
    callback(context);
    frame.returned = true;
}

void run_in_library_frame(const Callback callback, void* const context) {
    CallsBack calls{callback, context};
    stxlib::in_shadow_frame("test.library.frame", 5, 6, calls);
}

stxlib::Status run_in_library_gate(const Callback callback, void* const context) {
    CallsBackForGate calls{callback, context};
    const stxlib::Status result = stxlib::call_gate("test.library.gate", calls);
    return result;
}

stxlib::usize library_shadow_depth() {
    const stxlib::usize result = stxlib::shadow_depth();
    return result;
}

stxlib::ErrorRecord library_record() {
    const stxlib::ErrorRecord result = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "test.library.record");
    return result;
}

stxlib::Result<stxlib::EventWatch> library_watch_interrupts() {
    stxlib::Result<stxlib::EventWatch> result =
        stxlib::EventWatch::watch(stxlib::EventSet().with(stxlib::ProcessEvent::interrupt));
    return result;
}

} // namespace two_module
