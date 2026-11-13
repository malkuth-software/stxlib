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

} // namespace

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

} // namespace two_module
