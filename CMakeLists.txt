cmake_minimum_required(VERSION 3.24)
project(stxlib VERSION 0.1.0 LANGUAGES CXX)

# STXlib: the boundary library between GUI.Forms and everything outside it
# (AGENTS.md). This file builds stxlib::boundary, its interface modules and
# its tests. stxlib::algorithms (stx_auxlib's owned algorithms, copied with
# provenance) joins in a later batch.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(STXLIB_TESTS_DEFAULT ON)
    # Executables and DLLs side by side, so Windows finds stxlib_state and
    # the test library at run time.
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    endif()
else()
    set(STXLIB_TESTS_DEFAULT OFF)
endif()
option(STXLIB_TESTS "Build STXlib's tests and proof harnesses" ${STXLIB_TESTS_DEFAULT})

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "STXlib builds with Clang or GCC (include/stxlib/types.hpp uses their predefined type macros)")
endif()

find_package(Threads REQUIRED)

# ---- host family ------------------------------------------------------------
# One family per build (README: macOS, glibc, musl and Windows in phase I;
# FreeBSD in phase II). Linux is glibc or musl by the C library's own macro.
include(CheckCXXSourceCompiles)
if(WIN32)
    set(STXLIB_HOST_FAMILY windows)
elseif(APPLE)
    set(STXLIB_HOST_FAMILY macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_cxx_source_compiles("
        #include <features.h>
        #if !defined(__GLIBC__)
        #error not glibc
        #endif
        int main() { return 0; }" STXLIB_HOST_IS_GLIBC)
    if(STXLIB_HOST_IS_GLIBC)
        set(STXLIB_HOST_FAMILY glibc)
    else()
        set(STXLIB_HOST_FAMILY musl)
    endif()
else()
    message(FATAL_ERROR "STXlib phase I supports macOS, Linux (glibc, musl) and Windows; ${CMAKE_SYSTEM_NAME} is not one")
endif()
message(STATUS "STXlib host family: ${STXLIB_HOST_FAMILY}")

# ---- verification modes -----------------------------------------------------
# Integer checks: every unsigned wrap, signed overflow, shift out of range and
# value-changing implicit conversion halts the program, except in functions
# marked STXLIB_MODULAR (src/core/checked.hpp). As stx_auxlib's mode.
option(STXLIB_INTEGER_CHECKS "Build with Clang's integer sanitizers, halting on any report" OFF)
if(STXLIB_INTEGER_CHECKS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "STXLIB_INTEGER_CHECKS requires Clang")
    endif()
    set(stxlib_integer_flags
        -fsanitize=signed-integer-overflow,unsigned-integer-overflow,shift,implicit-integer-truncation,implicit-integer-sign-change,float-cast-overflow,pointer-overflow
        -fno-sanitize-recover=all)
    add_compile_options(${stxlib_integer_flags})
    add_link_options(${stxlib_integer_flags})
endif()

# AUTO switches: on in Debug and in builds whose flags enable a sanitizer.
if("${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_INIT}" MATCHES "-fsanitize=" OR STXLIB_INTEGER_CHECKS)
    set(stxlib_sanitized ON)
else()
    set(stxlib_sanitized OFF)
endif()

function(stxlib_switch name description)
    set(${name} AUTO CACHE STRING "${description}: AUTO (Debug and sanitizer builds), ON or OFF")
    set_property(CACHE ${name} PROPERTY STRINGS AUTO ON OFF)
    if(NOT ${name} MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "${name} must be AUTO, ON or OFF")
    endif()
endfunction()

# Exception shadowing in development builds (include/stxlib/shadow.hpp):
# exception types in records and the per-thread recent-failure list.
stxlib_switch(STXLIB_SHADOW "Shadow exceptions at the gates")
# Allocation ownership tags (include/stxlib/memory.hpp): a 16-byte tag below
# every block, checked by release.
stxlib_switch(STXLIB_ALLOCATION_TAGS "Tag and check memory::allocate blocks")
# Lock checks (include/stxlib/threads.hpp, owner decision D11): recursive
# locking and foreign unlocks of a Mutex fail fast.
stxlib_switch(STXLIB_LOCK_CHECKS "Trap recursive locking and foreign unlocks")

# The generator expression that turns a switch on for this configuration.
function(stxlib_switch_condition name out)
    if(${name} STREQUAL "ON")
        set(${out} "1" PARENT_SCOPE)
    elseif(${name} STREQUAL "OFF")
        set(${out} "0" PARENT_SCOPE)
    elseif(stxlib_sanitized)
        set(${out} "1" PARENT_SCOPE)
    else()
        set(${out} "$<CONFIG:Debug>" PARENT_SCOPE)
    endif()
endfunction()
stxlib_switch_condition(STXLIB_SHADOW stxlib_shadow_on)
stxlib_switch_condition(STXLIB_ALLOCATION_TAGS stxlib_tags_on)
stxlib_switch_condition(STXLIB_LOCK_CHECKS stxlib_lock_checks_on)

# STXlib's own code is built without exceptions in every build (AGENTS.md:
# release builds of everything ours use no exceptions; for STXlib from the
# start). The one exception is src/core/exception_shadow.cpp, which records
# exceptions for gates instantiated in consumers built with them.
set(STXLIB_EXCEPTION_RECORDER src/core/exception_shadow.cpp)

set(stxlib_warnings -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# ---- stxlib_state -----------------------------------------------------------
# The one owner of STXlib's per-thread and per-process state
# (src/state/state.hpp): the shadow stack, the pending capture, the shadow
# and recent records, the sequence counters, the open trampoline invocations
# and the fail-fast reporter. Every module that links stxlib::boundary
# statically reaches it through two exported accessors, so a frame opened
# in one shared library and a gate in another work on one stack. Shared by
# default; OFF only for a consumer that is a single module with no shared
# library of its own that links STXlib.
#
# Since layout 5 it also owns the allocator (src/state/allocator.cpp), so a
# block from stxlib::memory in one module may be released in another.
option(STXLIB_STATE_SHARED "Build stxlib_state as a shared library (required when STXlib is in more than one module)" ON)
set(STXLIB_STATE_SOURCES src/state/state.cpp src/state/allocator.cpp)
if(STXLIB_STATE_SHARED)
    add_library(stxlib_state SHARED ${STXLIB_STATE_SOURCES})
    target_compile_definitions(stxlib_state PUBLIC STXLIB_STATE_SHARED)
else()
    add_library(stxlib_state STATIC ${STXLIB_STATE_SOURCES})
endif()
add_library(stxlib::state ALIAS stxlib_state)
target_include_directories(stxlib_state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_state PUBLIC cxx_std_20)
target_compile_definitions(stxlib_state PRIVATE
    STXLIB_STATE_BUILDING
    $<$<BOOL:${stxlib_tags_on}>:STXLIB_ALLOCATION_TAGS>)
target_compile_options(stxlib_state PRIVATE ${stxlib_warnings} -fno-exceptions)
set_target_properties(stxlib_state PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

# The allocator's backend (include/stxlib/memory.hpp). Sanitizers track the
# host allocator only, so a build with sanitizers (or the integer checks,
# which would halt on mimalloc's intended wrap-around) defaults to the host
# backend, and src/state/allocator.cpp refuses mimalloc under address,
# thread or memory sanitizers.
if(stxlib_sanitized)
    set(stxlib_allocator_default host)
else()
    set(stxlib_allocator_default mimalloc)
endif()
set(STXLIB_ALLOCATOR ${stxlib_allocator_default} CACHE STRING "STXlib allocator backend: mimalloc or host")
set_property(CACHE STXLIB_ALLOCATOR PROPERTY STRINGS mimalloc host)
if(NOT STXLIB_ALLOCATOR MATCHES "^(mimalloc|host)$")
    message(FATAL_ERROR "STXLIB_ALLOCATOR must be mimalloc or host")
endif()
message(STATUS "STXlib allocator backend: ${STXLIB_ALLOCATOR}")
if(STXLIB_ALLOCATOR STREQUAL "mimalloc")
    # mimalloc v3.5.3 (third_party/mimalloc, MIT; PROVENANCE.md), compiled as
    # its one translation unit (src/static.c) into stxlib_state, with the
    # definitions stx_auxlib b6e330a used (CMakeLists.txt there). It replaces
    # no C library function (MI_MALLOC_OVERRIDE stays undefined) and its
    # symbols are hidden, so its heap holds STXlib's blocks only. Compiled as
    # C++ (mimalloc's MI_USE_CXX): compiled as C it defines a weak fallback
    # std::get_new_handler that, hidden, would bind in place of the C++
    # library's (stx_auxlib's finding). Vendored code: its warnings are
    # upstream's (-w), and the integer checks do not apply to it.
    set_source_files_properties(third_party/mimalloc/src/static.c PROPERTIES LANGUAGE CXX)
    add_library(stxlib_mimalloc OBJECT third_party/mimalloc/src/static.c)
    set_target_properties(stxlib_mimalloc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(stxlib_mimalloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include)
    target_compile_definitions(stxlib_mimalloc PRIVATE MI_STATIC_LIB MI_BUILD_RELEASE NDEBUG=1)
    target_compile_options(stxlib_mimalloc PRIVATE -w -fvisibility=hidden -ftls-model=initial-exec -fno-sanitize=all)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(stxlib_mimalloc PRIVATE -mno-outline)
    endif()
    target_sources(stxlib_state PRIVATE $<TARGET_OBJECTS:stxlib_mimalloc>)
    target_include_directories(stxlib_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include)
    target_compile_definitions(stxlib_state PRIVATE STXLIB_ALLOCATOR_MIMALLOC)
    if(WIN32)
        target_link_libraries(stxlib_state PRIVATE psapi shell32 user32 advapi32 bcrypt)
    endif()
endif()

# ---- stxlib::boundary -------------------------------------------------------
set(STXLIB_BOUNDARY_SOURCES
    src/core/capabilities.cpp
    src/core/children.cpp
    src/core/console.cpp
    src/core/desktop.cpp
    src/core/entropy.cpp
    src/core/environment.cpp
    src/core/error.cpp
    src/core/exception_shadow.cpp
    src/core/fail_fast.cpp
    src/core/files.cpp
    src/core/local_time.cpp
    src/core/memory.cpp
    src/core/path.cpp
    src/core/process.cpp
    src/core/report.cpp
    src/core/require.cpp
    src/core/shadow_frames.cpp
    src/core/text.cpp
    src/core/thread_pool.cpp
    src/core/threads.cpp
    src/core/trampoline.cpp
    src/hosted/c_library/c_library.cpp
    src/hosted/cxx_runtime/clocks.cpp
    src/hosted/cxx_runtime/current_exception.cpp)

# Interface modules per family. POSIX services shared by macOS, glibc and
# musl live in src/hosted/posix/; a family directory joins when a service
# differs (the stx_auxlib hosted port).
set(STXLIB_HOSTED_POSIX_SOURCES
    src/hosted/posix/children_posix.cpp
    src/hosted/posix/console_posix.cpp
    src/hosted/posix/desktop_posix.cpp
    src/hosted/posix/entropy_posix.cpp
    src/hosted/posix/environment_posix.cpp
    src/hosted/posix/events_posix.cpp
    src/hosted/posix/file_posix.cpp
    src/hosted/posix/local_time_posix.cpp
    src/hosted/posix/terminal_posix.cpp
    src/hosted/posix/threads_posix.cpp
    src/hosted/posix/trace_posix.cpp)
set(STXLIB_HOSTED_WINDOWS_SOURCES
    src/hosted/windows/children_windows.cpp
    src/hosted/windows/console_windows.cpp
    src/hosted/windows/desktop_windows.cpp
    src/hosted/windows/entropy_windows.cpp
    src/hosted/windows/environment_windows.cpp
    src/hosted/windows/events_windows.cpp
    src/hosted/windows/file_windows.cpp
    src/hosted/windows/local_time_windows.cpp
    src/hosted/windows/terminal_windows.cpp
    src/hosted/windows/threads_windows.cpp
    src/hosted/windows/trace_windows.cpp
    src/hosted/windows/utf16.cpp)
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_WINDOWS_SOURCES})
else()
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_POSIX_SOURCES})
endif()

add_library(stxlib_boundary STATIC ${STXLIB_BOUNDARY_SOURCES})
add_library(stxlib::boundary ALIAS stxlib_boundary)
target_include_directories(stxlib_boundary
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_boundary PUBLIC cxx_std_20)
target_compile_definitions(stxlib_boundary PRIVATE
    $<$<BOOL:${stxlib_shadow_on}>:STXLIB_SHADOW>
    $<$<BOOL:${stxlib_tags_on}>:STXLIB_ALLOCATION_TAGS>
    $<$<BOOL:${stxlib_lock_checks_on}>:STXLIB_LOCK_CHECKS>)
target_compile_options(stxlib_boundary PRIVATE ${stxlib_warnings} -fno-exceptions)
# Native traces walk the frame-pointer chain (src/hosted/posix/trace_posix.cpp),
# so every STXlib target keeps frame pointers when shadowing is on.
# Consumers that want full traces through their own frames on Linux build
# their development configurations with the same flag; Apple arm64 keeps
# frame records always.
set(stxlib_frame_pointers $<$<BOOL:${stxlib_shadow_on}>:-fno-omit-frame-pointer>)
target_compile_options(stxlib_boundary PRIVATE ${stxlib_frame_pointers})
set_source_files_properties(${STXLIB_EXCEPTION_RECORDER} PROPERTIES COMPILE_OPTIONS "-fexceptions")
# Each module that links the boundary keeps its copy private (hidden
# symbols) and may be a shared library (position-independent code).
set_target_properties(stxlib_boundary PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(stxlib_boundary PUBLIC stxlib_state Threads::Threads)
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    # BCryptGenRandom (entropy_windows.cpp); ShellExecuteW (desktop_windows.cpp).
    target_link_libraries(stxlib_boundary PUBLIC bcrypt shell32)
endif()
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    # Static libc++ in every module (stx_auxlib docs/ALLOCATOR_OWNERSHIP.md,
    # 2.3): a shared libc++ DLL binds its own operator new and delete.
    set(_stxlib_link_flags
        "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
    if(_stxlib_link_flags MATCHES "-shared-libstdc\\+\\+|-shared-libgcc|libc\\+\\+\\.dll")
        message(FATAL_ERROR "STXlib on Windows requires the static C++ runtime; the flags ask for a shared one")
    endif()
endif()

# ---- tests ------------------------------------------------------------------
if(STXLIB_TESTS)
    enable_testing()
    set(stxlib_test_warnings ${stxlib_warnings} ${stxlib_frame_pointers})

    foreach(test_name IN ITEMS vocabulary gate memory trampoline shadow platform path files children threads thread_pool local_time)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()

    if(STXLIB_ALLOCATOR STREQUAL "mimalloc")
        target_compile_definitions(stxlib_memory_tests PRIVATE STXLIB_EXPECT_MIMALLOC)
    endif()

    # Two modules, each with its own copy of stxlib::boundary, release each
    # other's blocks through the one allocator in stxlib_state.
    add_library(stxlib_two_module_library SHARED tests/two_module_library.cpp)
    target_link_libraries(stxlib_two_module_library PRIVATE stxlib::boundary)
    target_compile_definitions(stxlib_two_module_library PRIVATE STXLIB_TWO_MODULE_BUILDING)
    target_compile_options(stxlib_two_module_library PRIVATE ${stxlib_test_warnings})
    set_target_properties(stxlib_two_module_library PROPERTIES CXX_VISIBILITY_PRESET hidden)
    add_executable(stxlib_two_module_tests tests/two_module_tests.cpp)
    target_link_libraries(stxlib_two_module_tests PRIVATE stxlib::boundary stxlib_two_module_library)
    target_compile_options(stxlib_two_module_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_two_module_tests COMMAND stxlib_two_module_tests)

    # The gates compiled without exceptions: direct calls (A1 design, 6).
    add_executable(stxlib_gate_no_exceptions_tests tests/gate_no_exceptions_tests.cpp)
    target_link_libraries(stxlib_gate_no_exceptions_tests PRIVATE stxlib::boundary)
    target_compile_options(stxlib_gate_no_exceptions_tests PRIVATE ${stxlib_test_warnings} -fno-exceptions)
    add_test(NAME stxlib_gate_no_exceptions_tests COMMAND stxlib_gate_no_exceptions_tests)

    # Must-end cases, one process each.
    add_executable(stxlib_death_tests tests/death_tests.cpp)
    target_link_libraries(stxlib_death_tests PRIVATE stxlib::boundary)
    target_compile_options(stxlib_death_tests PRIVATE ${stxlib_test_warnings})
    set(stxlib_death_cases
        "callback_throw|stxlib: fail fast: stxlib\\.tests\\.callback: internal_failure.*client callback failed"
        "callback_target_throw|stxlib: fail fast: stxlib\\.tests\\.callback: invalid_argument.*client function failed"
        "reporter|consumer reporter: 17"
        "double_release|stxlib::memory::release: the block is not from memory::allocate"
        "foreign_release|stxlib::memory::release: the block is not from memory::allocate"
        "foreign_release_page|stxlib::memory::release: the block is not from memory::allocate"
        "recursive_lock|stxlib: fail fast: stxlib\\.threads\\.lock: internal_failure.*already holds"
        "foreign_unlock|stxlib: fail fast: stxlib\\.threads\\.lock: internal_failure.*does not hold"
        "view_index|stxlib: requirement failed: View: index out of range"
        "take_failed_value|stxlib: requirement failed: Result::take_value on a failure"
        "callback_frames|stxlib: frames \\(throw_site\\): stxlib\\.tests\\.outer\\[7,8\\] > stxlib\\.tests\\.callback\\[0,0\\] > stxlib\\.tests\\.inner\\[9,0\\].stxlib: trace: 0x"
        "foreign_free|ANY"
        "foreign_delete|ANY")
    foreach(death_case IN LISTS stxlib_death_cases)
        string(FIND "${death_case}" "|" separator)
        string(SUBSTRING "${death_case}" 0 ${separator} case_name)
        math(EXPR expect_start "${separator} + 1")
        string(SUBSTRING "${death_case}" ${expect_start} -1 case_expect)
        add_test(NAME stxlib_death_${case_name}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:stxlib_death_tests>
                -DCASE=${case_name}
                "-DEXPECT=${case_expect}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/death_check.cmake)
    endforeach()

    # The console and exit tasks, seen from outside the process.
    add_executable(stxlib_console_process_tests tests/console_process_tests.cpp)
    target_link_libraries(stxlib_console_process_tests PRIVATE stxlib::boundary)
    target_compile_options(stxlib_console_process_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_console_process_tests
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:stxlib_console_process_tests>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/console_process_work
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/console_process_check.cmake)

    # Two modules, one state (tests/two_module_tests.cpp): a shared library
    # with its own hidden copy of the boundary, and an executable with
    # another; an exception thrown in the executable unwinds a frame opened
    # in the library.
    if(STXLIB_STATE_SHARED)
        add_library(stxlib_test_library SHARED tests/two_module_library.cpp)
        target_link_libraries(stxlib_test_library PRIVATE stxlib::boundary)
        target_compile_definitions(stxlib_test_library PRIVATE STXLIB_TEST_LIBRARY_BUILDING)
        target_compile_options(stxlib_test_library PRIVATE ${stxlib_test_warnings})
        set_target_properties(stxlib_test_library PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
        add_executable(stxlib_two_module_tests tests/two_module_tests.cpp)
        target_link_libraries(stxlib_two_module_tests PRIVATE stxlib_test_library stxlib::boundary)
        target_compile_options(stxlib_two_module_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_two_module_tests COMMAND stxlib_two_module_tests)
    endif()

    # Proof harness: UTF-8 checks, exhaustive to length 3 here; length 4 by
    # hand with `stxlib_utf8_exhaustive --scope full` (proofs/REPORT.md).
    add_executable(stxlib_utf8_exhaustive tests/utf8_exhaustive.cpp)
    target_link_libraries(stxlib_utf8_exhaustive PRIVATE stxlib::boundary)
    target_compile_options(stxlib_utf8_exhaustive PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_utf8_exhaustive COMMAND stxlib_utf8_exhaustive)
    set_tests_properties(stxlib_utf8_exhaustive PROPERTIES TIMEOUT 1200)

    # Proof harness: Path's lossless host names (D13 a), exhaustive over the
    # domains tests/path_names_exhaustive.cpp states (proofs/REPORT.md).
    add_executable(stxlib_path_names_exhaustive tests/path_names_exhaustive.cpp)
    target_link_libraries(stxlib_path_names_exhaustive PRIVATE stxlib::boundary)
    target_compile_options(stxlib_path_names_exhaustive PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_path_names_exhaustive COMMAND stxlib_path_names_exhaustive)
    set_tests_properties(stxlib_path_names_exhaustive PROPERTIES TIMEOUT 1200)

    # The public-header boundary (AGENTS.md): no std:: name, no angle include.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME stxlib_public_headers
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_public_headers.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
