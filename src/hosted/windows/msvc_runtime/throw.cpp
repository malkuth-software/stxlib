// Throwing: _CxxThrowException, `throw;`, and std::uncaught_exceptions'
// count.
//
// clang's `throw expr` (MicrosoftCXXABI.cpp, emitThrow) builds the object
// in the throwing frame and calls _CxxThrowException(object, ThrowInfo);
// `throw;` passes two nulls. The object therefore lives in a frame below
// every handler and is destroyed when the catch that ends its life
// finishes (frame_handler.cpp).
//
// The raise: RaiseException (Microsoft Learn, "RaiseException function")
// with the C++ code, EXCEPTION_NONCONTINUABLE and the four parameters of
// msvc_abi.hpp. The image base is found with RtlPcToFileHeader (winnt.h;
// Microsoft Learn, "RtlPcToFileHeader function"), which names the image
// holding an address.
//
// The count: an exception is uncaught from its throw until its handler is
// active ([except.uncaught]); the throw adds one here and the catch removes
// it once its parameter is initialised (frame_handler.cpp). `throw;` makes
// the handled exception uncaught again.

#include "runtime_state.hpp"

#include "stxlib/detail/early_exit.hpp"

namespace stxlib::msvc_runtime {

void raise_handled(const HandledException& exception) noexcept {
    // RaiseException copies the parameters into the record it builds.
    ULONG_PTR parameters[EXCEPTION_MAXIMUM_PARAMETERS];
    for (u32 index = 0; index < EXCEPTION_MAXIMUM_PARAMETERS; ++index) {
        parameters[index] = exception.parameters[index];
    }
    const DWORD flags = is_cxx_exception(exception) ? EXCEPTION_NONCONTINUABLE : exception.flags;
    const early_exit::Whole raise_marker{"raising is this routine's purpose: no lock is held and the uncaught "
                                         "count was already raised by the caller"};
    RaiseException(exception.code, flags, exception.parameter_count, parameters);
    // A continuable structured exception rethrown under -EHa and resumed by
    // a filter returns here; there is no frame to resume.
    terminate_now();
}

} // namespace stxlib::msvc_runtime

extern "C" {

[[noreturn]] void _CxxThrowException(void* const object, const void* const throw_info) {
    using namespace stxlib::msvc_runtime;
    if (throw_info == nullptr) {
        // `throw;`: the innermost active catch's exception, again.
        CatchRecord* const record = innermost_catch();
        if (record == nullptr) terminate_now();
        (*record).rethrown = true;
        add_uncaught(1);
        raise_handled((*record).exception);
    }
    void* image_base = nullptr;
    RtlPcToFileHeader(const_cast<void*>(throw_info), &image_base);
    if (image_base == nullptr) fail_fast();
    HandledException exception{};
    exception.code = cxx_exception_code;
    exception.flags = EXCEPTION_NONCONTINUABLE;
    exception.parameter_count = cxx_exception_parameters;
    exception.parameters[0] = static_cast<ULONG_PTR>(cxx_exception_magic);
    exception.parameters[1] = reinterpret_cast<ULONG_PTR>(object);
    exception.parameters[2] = reinterpret_cast<ULONG_PTR>(throw_info);
    exception.parameters[3] = reinterpret_cast<ULONG_PTR>(image_base);
    add_uncaught(1);
    raise_handled(exception);
}

int __uncaught_exceptions() {
    const int result = stxlib::msvc_runtime::uncaught_count();
    return result;
}

} // extern "C"
