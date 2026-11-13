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
option(STXLIB_STATE_SHARED "Build stxlib_state as a shared library (required when STXlib is in more than one module)" ON)
if(STXLIB_STATE_SHARED)
    add_library(stxlib_state SHARED src/state/state.cpp)
    target_compile_definitions(stxlib_state PUBLIC STXLIB_STATE_SHARED)
else()
    add_library(stxlib_state STATIC src/state/state.cpp)
endif()
add_library(stxlib::state ALIAS stxlib_state)
target_include_directories(stxlib_state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_state PUBLIC cxx_std_20)
target_compile_definitions(stxlib_state PRIVATE STXLIB_STATE_BUILDING)
target_compile_options(stxlib_state PRIVATE ${stxlib_warnings} -fno-exceptions)
set_target_properties(stxlib_state PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

# ---- stxlib::boundary -------------------------------------------------------
set(STXLIB_BOUNDARY_SOURCES
    src/core/error.cpp
    src/core/exception_shadow.cpp
    src/core/fail_fast.cpp
    src/core/memory.cpp
    src/core/report.cpp
    src/core/require.cpp
    src/core/shadow_frames.cpp
    src/core/text.cpp
    src/core/trampoline.cpp
    src/hosted/c_library/c_library.cpp
    src/hosted/cxx_runtime/current_exception.cpp)

# Interface modules per family. POSIX services shared by macOS, glibc and
# musl live in src/hosted/posix/; a family directory joins when a service
# differs (the stx_auxlib hosted port).
set(STXLIB_HOSTED_POSIX_SOURCES
    src/hosted/posix/console_posix.cpp
    src/hosted/posix/trace_posix.cpp)
set(STXLIB_HOSTED_WINDOWS_SOURCES
    src/hosted/windows/console_windows.cpp
    src/hosted/windows/trace_windows.cpp)
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
    $<$<BOOL:${stxlib_tags_on}>:STXLIB_ALLOCATION_TAGS>)
target_compile_options(stxlib_boundary PRIVATE ${stxlib_warnings} -fno-exceptions)
set_source_files_properties(${STXLIB_EXCEPTION_RECORDER} PROPERTIES COMPILE_OPTIONS "-fexceptions")
# Each module that links the boundary keeps its copy private (hidden
# symbols) and may be a shared library (position-independent code).
set_target_properties(stxlib_boundary PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(stxlib_boundary PUBLIC stxlib_state Threads::Threads)
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
    set(stxlib_test_warnings ${stxlib_warnings})

    foreach(test_name IN ITEMS vocabulary gate memory trampoline shadow)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()

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

    # The public-header boundary (AGENTS.md): no std:: name, no angle include.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME stxlib_public_headers
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_public_headers.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
