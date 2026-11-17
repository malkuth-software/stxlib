// The shared program start (program_startup.cpp), called by each program
// entry point (entry_*.cpp) before the user's main.

#pragma once

#include "hosted/windows_msvc_startup/startup.hpp"

namespace stxlib::msvc_startup {

// Which argument and environment form the user's entry function takes.
enum class ArgumentForm : int {
    narrow = 0,
    wide = 1,
};

// Initializes the image as a program: cookie, application type, unhandled
// exception filter, arguments and environment, C and C++ initializers and
// the main thread's thread_locals. Ends the process with _exit(255) if the
// UCRT cannot configure the arguments or a C initializer refuses.
void start_program(ArgumentForm form, _crt_app_type application_type) noexcept;

}  // namespace stxlib::msvc_startup
