#pragma once

// The library half of the two-module test (two_module_tests.cpp): a shared
// library with its own copy of STXlib's code (stxlib::boundary linked
// statically, symbols hidden), as GUI.Forms' modules will have. It shares
// STXlib's state with the executable only through stxlib_state.

#include "stxlib/boundary.hpp"
#include "stxlib/events.hpp"

#if defined(_WIN32)
#if defined(STXLIB_TEST_LIBRARY_BUILDING)
#define STXLIB_TEST_LIBRARY_API __declspec(dllexport)
#else
#define STXLIB_TEST_LIBRARY_API __declspec(dllimport)
#endif
#else
#define STXLIB_TEST_LIBRARY_API __attribute__((visibility("default")))
#endif

namespace two_module {

using Callback = void (*)(void* context);

// Opens the shadow frame "test.library.frame" [5, 6] in the library's copy
// and calls `callback` inside it. An exception from the callback passes
// through unchanged.
STXLIB_TEST_LIBRARY_API void run_in_library_frame(Callback callback, void* context);

// A gate in the library's copy: call_gate("test.library.gate") around
// `callback`.
STXLIB_TEST_LIBRARY_API stxlib::Status run_in_library_gate(Callback callback, void* context);

// The shadow depth as the library's copy reads it.
STXLIB_TEST_LIBRARY_API stxlib::usize library_shadow_depth();

// An error record made by the library's copy (for the shared sequence).
STXLIB_TEST_LIBRARY_API stxlib::ErrorRecord library_record();

// An event watch made by the library's copy (one watch per process, in
// stxlib_state).
STXLIB_TEST_LIBRARY_API stxlib::Result<stxlib::EventWatch> library_watch_interrupts();

} // namespace two_module
