cmake_minimum_required(VERSION 3.24)
project(stxlib VERSION 1.0.2 LANGUAGES CXX)

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
# FreeBSD 15.2 joins with A1 platform-rest). Linux is glibc or musl by the C
# library's own macro. FreeBSD is named by CMAKE_SYSTEM_NAME, which CMake
# sets on a FreeBSD host and a cross toolchain file sets for a cross-build
# (the integrator's aarch64-freebsd.cmake). Emscripten (owner decision
# 2026-11-15, in 1.0 after M3) is named by the same variable, which
# Emscripten's own platform file sets (cmake/Modules/Platform/
# Emscripten.cmake:17 at emscripten 6.0.13); cmake/emscripten.cmake is the
# toolchain file that loads it.
include(CheckCXXSourceCompiles)
if(WIN32)
    set(STXLIB_HOST_FAMILY windows)
elseif(APPLE)
    set(STXLIB_HOST_FAMILY macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(STXLIB_HOST_FAMILY emscripten)
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(STXLIB_HOST_FAMILY freebsd)
else()
    message(FATAL_ERROR "STXlib supports macOS, Linux (glibc, musl), FreeBSD, Windows and Emscripten; ${CMAKE_SYSTEM_NAME} is not one")
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
# native traces in shadow records, exception types in records and the
# per-thread recent-failure list. Frames are recorded in every build (the
# A2 swap-in).
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
# exceptions for gates instantiated in consumers built with them, and only
# while STXLIB_EXCEPTIONS is ON (below).
set(STXLIB_EXCEPTION_RECORDER src/core/exception_shadow.cpp)

set(stxlib_warnings -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
# The floating-point discipline (house style "Floating point", rule 1;
# planning/stages/A1-float-discipline.md, F1 = 1, 2026-11-13): no
# contraction and no fast-math in any STXlib target, libraries and tests
# alike. A directory option, so it reaches every target defined here and
# never a consumer's own code: consumers keep fused arithmetic. -fno-fast-math
# turns math-errno back on for GNU and MinGW targets, which leaves a C
# library call (to set errno) behind std::sqrt; nothing of ours reads errno
# after arithmetic, hence -fno-math-errno after it. The check at the end of
# this file refuses a target without these, or any flag that undoes them.
set(stxlib_fp_options -ffp-contract=off -fno-fast-math -fno-math-errno)
add_compile_options(${stxlib_fp_options})
# The exact floating-point primitives' range checks (src/core/float_exact.hpp)
# in Debug builds, for every target alike, so no build mixes checked and
# unchecked copies of those inline functions.
add_compile_definitions($<$<CONFIG:Debug>:STXLIB_FLOAT_CHECKS>)
# One printf for every translation unit on MinGW (audit C, C-9/C10).
# mingw-w64's _mingw.h (v14.0.0, mingw-w64-headers/crt/_mingw.h.in:442-454)
# switches a translation unit to MinGW's own printf family whenever it
# defines _POSIX, _POSIX_SOURCE, _POSIX_C_SOURCE, _ISOC99_SOURCE,
# _XOPEN_SOURCE, _GNU_SOURCE or _SVID_SOURCE (and, for C++ alone, against an
# msvcrt older than the UCRT, :233-234), so the same source could format
# through two implementations in one program. Defining the macro as 0 keeps
# every STXlib unit on the UCRT's printf; STXlib's own text never goes
# through printf (numeric_text), so this governs only test and tool output
# and the C runtime's own uses. Consumers are advised to do the same.
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=0)
endif()
# Unstable order made visible (R4; audit B, 3.16, the sort rule): with
# libc++, _LIBCPP_DEBUG_RANDOMIZE_UNSPECIFIED_STABILITY shuffles ranges
# around std::sort, std::nth_element and std::partial_sort, where the
# standard leaves the order of equivalent elements open
# (libcxx/include/__debug_utils/randomize_range.h; libc++ 15 and later;
# the shuffle is skipped in constant evaluation), so code that relies on the order of equal elements
# fails a test instead of passing on one library and not another. The seed
# is the address of a static (libcxx/include/__algorithm/shuffle.h), so it
# varies per run under address randomisation; STXLIB_UNSTABLE_ORDER_SEED
# fixes it to reproduce a failure. A directory definition, so the library
# and the tests that link it see the same inline algorithms. Test builds
# with libc++ only; libstdc++ has no such switch.
set(stxlib_randomize_default OFF)
if(STXLIB_TESTS)
    check_cxx_source_compiles("
        #include <version>
        #if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 15000
        #error not libc++ 15 or later
        #endif
        int main() { return 0; }" STXLIB_STANDARD_LIBRARY_IS_LIBCXX)
    # Not under STXLIB_INTEGER_CHECKS: libc++'s shuffle steps a linear
    # congruential generator modulo 2^64 (__algorithm/shuffle.h), which the
    # integer sanitizer halts on; the other test builds shuffle.
    if(STXLIB_STANDARD_LIBRARY_IS_LIBCXX AND NOT STXLIB_INTEGER_CHECKS)
        set(stxlib_randomize_default ON)
    endif()
endif()
option(STXLIB_RANDOMIZE_UNSTABLE_ORDER "Shuffle equal elements in unstable sorts (libc++ test builds)"
    ${stxlib_randomize_default})
set(STXLIB_UNSTABLE_ORDER_SEED "" CACHE STRING "A fixed seed for STXLIB_RANDOMIZE_UNSTABLE_ORDER (empty: per run)")
if(STXLIB_RANDOMIZE_UNSTABLE_ORDER)
    add_compile_definitions(_LIBCPP_DEBUG_RANDOMIZE_UNSPECIFIED_STABILITY)
    if(NOT STXLIB_UNSTABLE_ORDER_SEED STREQUAL "")
        add_compile_definitions(_LIBCPP_DEBUG_RANDOMIZE_UNSPECIFIED_STABILITY_SEED=${STXLIB_UNSTABLE_ORDER_SEED})
    endif()
endif()

# ---- exceptions -------------------------------------------------------------
# STXlib's own code never uses exceptions; STXLIB_EXCEPTIONS says whether its
# consumers may. ON (the default) builds the exception recorder with
# exceptions, so a gate instantiated in a consumer built with them records
# what it catches. OFF is for a process with no exceptions anywhere (the
# appliance's supervisor and PID 1, an application built without them;
# shape B of notes planning/stages/A3-no-exceptions.md): every target defined
# here, tests included, is built with -fno-exceptions; the recorder keeps
# only the recent-failure accessors; the C++ runtime's exception-type query
# is left out; and gate.hpp refuses a consumer translation unit that has
# exceptions (STXLIB_NO_EXCEPTION_RECORDER). Tests that throw are not built.
# Checked at configure time (stxlib_check_exception_flags, end of file) and
# on the objects (the ctest stxlib_no_exception_symbols).
option(STXLIB_EXCEPTIONS "Build the exception recorder, for consumers built with exceptions" ON)
if(NOT STXLIB_EXCEPTIONS)
    add_compile_options(-fno-exceptions)
endif()
message(STATUS "STXlib exception recorder: ${STXLIB_EXCEPTIONS}")

# ---- outline-atomics helpers --------------------------------------------------
# Linux and FreeBSD aarch64 only: the compilers turn every atomic
# read-modify-write into a call to an __aarch64_* helper, which STXlib
# defines itself (src/hosted/elf/outline_atomics_aarch64.cpp,
# src/hosted/outline_atomics_backend.hpp; owner, 2026-11-22) instead of
# taking the toolchain runtime's, which asks the C library's getauxval.
# The helpers are hidden, like the toolchain's, so each module needs its own
# copy: each STXlib library carries the helpers' object, which puts one copy in a
# shared stxlib_state and one in every module that links STXlib, ahead of
# the runtime library the compiler driver appends. All helpers are one
# object, so the first reference takes them all. The condition matches the
# source's STXLIB_OUTLINE_ATOMICS.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND STXLIB_HOST_FAMILY MATCHES "^(glibc|musl|freebsd)$")
    set(stxlib_outline_atomics ON)
    # An object library, compiled into each of STXlib's libraries below (and
    # into the tests that name it), so an installed STXlib exports no target
    # of its own for it (stx's stxlibTargets, 2026-11-23: install(EXPORT)
    # refused stxlib_state's link to a target in no export set).
    add_library(stxlib_outline_atomics OBJECT src/hosted/elf/outline_atomics_aarch64.cpp)
    target_include_directories(stxlib_outline_atomics PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_outline_atomics PRIVATE cxx_std_20)
    target_compile_options(stxlib_outline_atomics PRIVATE ${stxlib_warnings} -fno-exceptions)
    set_target_properties(stxlib_outline_atomics PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
    set(stxlib_outline_atomics OFF)
endif()
message(STATUS "STXlib outline-atomics helpers: ${stxlib_outline_atomics}")

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
# Emscripten: a program is one wasm module (STXlib's family asks for no
# dynamic linking; src/hosted/emscripten/images_emscripten.cpp), so the
# state is static there and a shared one is refused.
if(STXLIB_HOST_FAMILY STREQUAL "emscripten")
    set(stxlib_state_shared_default OFF)
else()
    set(stxlib_state_shared_default ON)
endif()
option(STXLIB_STATE_SHARED "Build stxlib_state as a shared library (required when STXlib is in more than one module)"
    ${stxlib_state_shared_default})
if(STXLIB_HOST_FAMILY STREQUAL "emscripten" AND STXLIB_STATE_SHARED)
    message(FATAL_ERROR "STXLIB_STATE_SHARED is not available for Emscripten: the program is one wasm module")
endif()
# The allocator's backend (include/stxlib/memory.hpp).
# - stx_minmallocpp, the default: STXlib's own allocator
#   (src/state/stx_minmallocpp), inspired by and derived from mimalloc v3,
#   with many features of its own (the owner, 2026-11-15; the default by
#   the owner's decision of the same day, notes
#   decisions/2026-11-15-stx-minmallocpp.md).
#   proofs/REPORT.md says what of it is proved, tested and measured.
# - host: the host's C library (src/state/allocator.cpp). Sanitizers track
#   the host allocator only, so a build with sanitizers or the integer
#   checks defaults to it (their reports then name STXlib's own blocks).
# The C mimalloc that was a third backend is gone (the owner, 2026-11-15;
# notes decisions/2026-11-15-gui-forms-priority.md).
# Emscripten: stx_minmallocpp is the default there too, with 64 MiB regions
# for 32-bit addresses (src/state/stx_minmallocpp/layout.hpp) and its
# address space taken from Emscripten's dlmalloc
# (src/hosted/emscripten/minmallocpp_os_emscripten.cpp); host (Emscripten's
# malloc, dlmalloc, src/settings.js MALLOC at emscripten 6.0.13) is
# admitted.
if(stxlib_sanitized)
    set(stxlib_allocator_default host)
else()
    set(stxlib_allocator_default stx_minmallocpp)
endif()
set(STXLIB_ALLOCATOR ${stxlib_allocator_default} CACHE STRING "STXlib allocator backend: stx_minmallocpp or host")
set_property(CACHE STXLIB_ALLOCATOR PROPERTY STRINGS stx_minmallocpp host)
if(NOT STXLIB_ALLOCATOR MATCHES "^(host|stx_minmallocpp)$")
    message(FATAL_ERROR "STXLIB_ALLOCATOR must be stx_minmallocpp or host")
endif()
message(STATUS "STXlib allocator backend: ${STXLIB_ALLOCATOR}")
# With the host backend the ownership check exists only with the tags, so
# AUTO turns them on whenever that backend is chosen, in every
# configuration: a double or foreign release is refused whichever backend
# a build has (stx_minmallocpp refuses one itself in every build; security
# audit 2026-11-15, A-stxlib-include-01-F11; notes
# decisions/2026-11-15-boundary-sideloading.md, consistency within). OFF
# still turns them off.
if(STXLIB_ALLOCATOR STREQUAL "host" AND STXLIB_ALLOCATION_TAGS STREQUAL "AUTO")
    set(stxlib_tags_on "1")
endif()
if(STXLIB_ALLOCATOR STREQUAL "stx_minmallocpp")
    # Its own allocator calls replace src/state/allocator.cpp: it needs no
    # tag header and no registry (src/state/stx_minmallocpp/state_calls.cpp).
    # Its host layer is the one file per family under src/hosted.
    set(STXLIB_STATE_SOURCES
        src/state/state.cpp
        src/state/stx_minmallocpp/state_calls.cpp
        src/state/stx_minmallocpp/allocator.cpp
        src/state/stx_minmallocpp/heap.cpp
        src/state/stx_minmallocpp/region.cpp)
    if(WIN32)
        list(APPEND STXLIB_STATE_SOURCES src/hosted/windows/minmallocpp_os_windows.cpp)
    else()
        list(APPEND STXLIB_STATE_SOURCES src/hosted/posix/minmallocpp_os_posix.cpp)
    endif()
    # Emscripten: the POSIX layer's page size, yield, stop and thread-exit
    # hook, with its address space over linear memory.
    if(STXLIB_HOST_FAMILY STREQUAL "emscripten")
        list(APPEND STXLIB_STATE_SOURCES src/hosted/emscripten/minmallocpp_os_emscripten.cpp)
    endif()
else()
    set(STXLIB_STATE_SOURCES src/state/state.cpp src/state/allocator.cpp)
endif()
# The thread-exit hook that releases each thread's block (layout 10; notes
# planning/stages/E-thread-records.md) lives with the state it releases,
# whatever the backend.
# So does the stack-bounds query that fills each thread's anchor (layout 12;
# hosted/stack_bounds_backend.hpp).
if(WIN32)
    list(APPEND STXLIB_STATE_SOURCES
        src/hosted/windows/thread_exit_windows.cpp
        src/hosted/windows/stack_bounds_windows.cpp)
else()
    list(APPEND STXLIB_STATE_SOURCES
        src/hosted/posix/thread_exit_posix.cpp
        src/hosted/posix/stack_bounds_posix.cpp)
endif()
if(STXLIB_STATE_SHARED)
    add_library(stxlib_state SHARED ${STXLIB_STATE_SOURCES})
    target_compile_definitions(stxlib_state PUBLIC STXLIB_STATE_SHARED)
else()
    add_library(stxlib_state STATIC ${STXLIB_STATE_SOURCES})
endif()
add_library(stxlib::state ALIAS stxlib_state)
if(stxlib_outline_atomics)
    target_sources(stxlib_state PRIVATE $<TARGET_OBJECTS:stxlib_outline_atomics>)
endif()
target_include_directories(stxlib_state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_state PUBLIC cxx_std_20)
# STXLIB_SHADOW as the boundary has it: the thread state's layout depends on
# the build type (state.hpp, layout 9), and the layout key refuses a mix.
# STXLIB_STATE_BUILDING (state.hpp's dllexport on Windows) only for the
# shared library: a static archive is private to the one module that links
# it, and an export directive in its objects would export the accessors
# from that module whatever its .def lists (stxlib_state_static_no_exports
# below checks the objects).
target_compile_definitions(stxlib_state PRIVATE
    $<$<BOOL:${STXLIB_STATE_SHARED}>:STXLIB_STATE_BUILDING>
    $<$<BOOL:${stxlib_shadow_on}>:STXLIB_SHADOW>
    $<$<BOOL:${stxlib_tags_on}>:STXLIB_ALLOCATION_TAGS>)
target_compile_options(stxlib_state PRIVATE ${stxlib_warnings} -fno-exceptions)
target_link_libraries(stxlib_state PRIVATE Threads::Threads)
# Never unloaded under a thread whose block it holds (thread_exit_windows.cpp
# pins the DLL the same way).
if(STXLIB_STATE_SHARED AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(stxlib_state PRIVATE "LINKER:-z,nodelete")
endif()
set_target_properties(stxlib_state PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
if(STXLIB_ALLOCATOR STREQUAL "stx_minmallocpp" AND NOT WIN32)
    # The thread-exit hook is a pthread key (minmallocpp_os_posix.cpp).
    target_link_libraries(stxlib_state PRIVATE Threads::Threads)
endif()

# ---- stxlib::boundary -------------------------------------------------------
set(STXLIB_BOUNDARY_SOURCES
    src/core/atomic_rules.cpp
    src/core/atomic_word.cpp
    src/core/capabilities.cpp
    src/core/channels.cpp
    src/core/children.cpp
    src/core/console.cpp
    src/core/desktop.cpp
    src/core/entropy.cpp
    src/core/environment.cpp
    src/core/error.cpp
    src/core/exception_shadow.cpp
    src/core/fail_fast.cpp
    src/core/files.cpp
    src/core/images.cpp
    src/core/known_directories.cpp
    src/core/library.cpp
    src/core/local_time.cpp
    src/core/memory.cpp
    src/core/path.cpp
    src/core/pipes.cpp
    src/core/process.cpp
    src/core/process_info.cpp
    src/core/report.cpp
    src/core/require.cpp
    src/core/shadow_frames.cpp
    src/core/stack_trace.cpp
    src/core/surface.cpp
    src/core/surface_blit.cpp
    src/core/surface_input.cpp
    src/core/surface_keys.cpp
    src/core/text.cpp
    src/core/stop_token.cpp
    src/core/thread_pool.cpp
    src/core/threads.cpp
    src/core/thread_records.cpp
    src/core/trampoline.cpp
    src/core/vocabulary.cpp
    src/core/transcode.cpp
    src/core/web.cpp
    src/core/zones.cpp
    src/hosted/c_library/c_library.cpp
    src/hosted/cxx_runtime/clocks.cpp
    src/hosted/cxx_runtime/current_exception.cpp)

# Interface modules per family. POSIX services shared by macOS, glibc, musl
# and FreeBSD live in src/hosted/posix/, with small host differences as
# #elif branches; a service that differs whole has a family directory
# (src/hosted/host.hpp): macos/, linux/ (glibc and musl), freebsd/, and
# elf/ (glibc, musl and FreeBSD).
set(STXLIB_HOSTED_POSIX_SOURCES
    src/hosted/posix/children_posix.cpp
    src/hosted/posix/clocks_posix.cpp
    src/hosted/posix/console_posix.cpp
    src/hosted/posix/desktop_posix.cpp
    src/hosted/posix/entropy_posix.cpp
    src/hosted/posix/environment_posix.cpp
    src/hosted/posix/events_posix.cpp
    src/hosted/posix/file_posix.cpp
    src/hosted/posix/home_posix.cpp
    src/hosted/posix/library_posix.cpp
    src/hosted/posix/local_time_posix.cpp
    src/hosted/posix/pages_posix.cpp
    src/hosted/posix/pipes_posix.cpp
    src/hosted/posix/terminal_posix.cpp
    src/hosted/posix/threads_posix.cpp
    src/hosted/posix/trace_posix.cpp
    src/hosted/posix/wake_posix.cpp)
set(STXLIB_HOSTED_MACOS_SOURCES
    src/hosted/macos/atomic_wait_macos.cpp
    src/hosted/macos/image_regions_macos.cpp
    src/hosted/macos/process_macos.cpp
    src/hosted/macos/cache_macos.cpp)
set(STXLIB_HOSTED_LINUX_SOURCES
    src/hosted/elf/image_regions_elf.cpp
    src/hosted/linux/atomic_wait_linux.cpp
    src/hosted/linux/process_linux.cpp
    src/hosted/linux/cache_linux.cpp)
set(STXLIB_HOSTED_FREEBSD_SOURCES
    src/hosted/elf/image_regions_elf.cpp
    src/hosted/freebsd/atomic_wait_freebsd.cpp
    src/hosted/freebsd/process_freebsd.cpp
    src/hosted/cache_default.cpp)
# Emscripten (owner decision 2026-11-15): the POSIX modules that hold on
# Emscripten's musl and file system, and emscripten/ for the services a page
# provides differently (clocks, traces, the wake channel, the zone name, the
# process, images) and its own (the runtime's thread facts, web storage).
# children_posix.cpp and desktop_posix.cpp are kept: they link (posix_spawn
# is Emscripten's stub answering ENOSYS, system/lib/libc/
# emscripten_libc_stubs.c:187-193 at 6.0.13) and fail at run time, behind
# capabilities that are false. One storage module per file system:
# STXLIB_EMSCRIPTEN_FILESYSTEM js (Emscripten's JavaScript file system:
# MEMFS and IDBFS) or wasmfs (WasmFS: memory and OPFS).
set(STXLIB_HOSTED_EMSCRIPTEN_POSIX_SOURCES
    src/hosted/posix/children_posix.cpp
    src/hosted/posix/console_posix.cpp
    src/hosted/posix/desktop_posix.cpp
    src/hosted/posix/entropy_posix.cpp
    src/hosted/posix/environment_posix.cpp
    src/hosted/posix/events_posix.cpp
    src/hosted/posix/file_posix.cpp
    src/hosted/posix/home_posix.cpp
    src/hosted/posix/pages_posix.cpp
    src/hosted/posix/terminal_posix.cpp
    src/hosted/posix/threads_posix.cpp)
set(STXLIB_HOSTED_EMSCRIPTEN_SOURCES
    src/hosted/emscripten/atomic_wait_emscripten.cpp
    src/hosted/emscripten/clocks_emscripten.cpp
    src/hosted/emscripten/images_emscripten.cpp
    src/hosted/emscripten/local_time_emscripten.cpp
    src/hosted/emscripten/pipes_emscripten.cpp
    src/hosted/emscripten/process_emscripten.cpp
    src/hosted/emscripten/runtime_emscripten.cpp
    src/hosted/emscripten/trace_emscripten.cpp
    src/hosted/emscripten/wake_emscripten.cpp
    src/hosted/cache_default.cpp)
set(STXLIB_EMSCRIPTEN_FILESYSTEM js CACHE STRING "Emscripten file system: js (MEMFS and IDBFS) or wasmfs (memory and OPFS)")
set_property(CACHE STXLIB_EMSCRIPTEN_FILESYSTEM PROPERTY STRINGS js wasmfs)
if(NOT STXLIB_EMSCRIPTEN_FILESYSTEM MATCHES "^(js|wasmfs)$")
    message(FATAL_ERROR "STXLIB_EMSCRIPTEN_FILESYSTEM must be js or wasmfs")
endif()
# The pthread pool's size, which STXlib links with and reports
# (threads.hpp, thread_pool_limit); the one count of Web Workers the
# browser makes at start (src/settings.js, PTHREAD_POOL_SIZE).
set(STXLIB_EMSCRIPTEN_POOL_SIZE 8 CACHE STRING "Emscripten pthread pool size (Web Workers made at start)")
if(NOT STXLIB_EMSCRIPTEN_POOL_SIZE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "STXLIB_EMSCRIPTEN_POOL_SIZE must be a decimal count")
endif()
# EXIT_RUNTIME: without it, main's return and exit leave the runtime alive
# and their status unreported (src/settings.js, EXIT_RUNTIME's comment), so
# test builds need it on; a page keeps it off unless it wants exit tasks.
option(STXLIB_EMSCRIPTEN_EXIT_RUNTIME "Emscripten: link with EXIT_RUNTIME, so exit runs atexit handlers (exit_tasks)"
    ${STXLIB_TESTS})
if(STXLIB_HOST_FAMILY STREQUAL "emscripten" AND STXLIB_TESTS AND NOT STXLIB_EMSCRIPTEN_EXIT_RUNTIME)
    message(FATAL_ERROR "STXLIB_TESTS on Emscripten needs STXLIB_EMSCRIPTEN_EXIT_RUNTIME=ON: without it a test's status is lost")
endif()
if(STXLIB_EMSCRIPTEN_FILESYSTEM STREQUAL "wasmfs")
    list(APPEND STXLIB_HOSTED_EMSCRIPTEN_SOURCES src/hosted/emscripten/storage_wasmfs_emscripten.cpp)
else()
    list(APPEND STXLIB_HOSTED_EMSCRIPTEN_SOURCES src/hosted/emscripten/storage_js_emscripten.cpp)
endif()
set(STXLIB_HOSTED_WINDOWS_SOURCES
    src/hosted/windows/atomic_wait_windows.cpp
    src/hosted/windows/children_windows.cpp
    src/hosted/windows/clocks_windows.cpp
    src/hosted/windows/console_windows.cpp
    src/hosted/windows/desktop_windows.cpp
    src/hosted/windows/entropy_windows.cpp
    src/hosted/windows/environment_windows.cpp
    src/hosted/windows/events_windows.cpp
    src/hosted/windows/file_windows.cpp
    src/hosted/windows/images_windows.cpp
    src/hosted/windows/known_folders_windows.cpp
    src/hosted/windows/local_time_windows.cpp
    src/hosted/windows/pages_windows.cpp
    src/hosted/windows/pipes_windows.cpp
    src/hosted/windows/process_windows.cpp
    src/hosted/windows/terminal_windows.cpp
    src/hosted/windows/threads_windows.cpp
    src/hosted/windows/trace_windows.cpp
    src/hosted/windows/utf16.cpp
    src/hosted/windows/wake_windows.cpp
    src/hosted/cache_default.cpp)
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_WINDOWS_SOURCES})
elseif(STXLIB_HOST_FAMILY STREQUAL "macos")
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_POSIX_SOURCES} ${STXLIB_HOSTED_MACOS_SOURCES})
elseif(STXLIB_HOST_FAMILY STREQUAL "freebsd")
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_POSIX_SOURCES} ${STXLIB_HOSTED_FREEBSD_SOURCES})
elseif(STXLIB_HOST_FAMILY STREQUAL "emscripten")
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_EMSCRIPTEN_POSIX_SOURCES} ${STXLIB_HOSTED_EMSCRIPTEN_SOURCES})
else()
    list(APPEND STXLIB_BOUNDARY_SOURCES ${STXLIB_HOSTED_POSIX_SOURCES} ${STXLIB_HOSTED_LINUX_SOURCES})
endif()
# The display surface (surface.hpp): FreeBSD's framebuffer, terminal and
# evdev where the family is FreeBSD's (phase II, the appliance); every
# other family reports it unsupported.
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    list(APPEND STXLIB_BOUNDARY_SOURCES src/hosted/freebsd/surface_freebsd.cpp)
else()
    list(APPEND STXLIB_BOUNDARY_SOURCES src/hosted/surface_unsupported.cpp)
endif()

add_library(stxlib_boundary STATIC ${STXLIB_BOUNDARY_SOURCES})
if(stxlib_outline_atomics)
    target_sources(stxlib_boundary PRIVATE $<TARGET_OBJECTS:stxlib_outline_atomics>)
endif()
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
if(STXLIB_EXCEPTIONS)
    set_source_files_properties(${STXLIB_EXCEPTION_RECORDER} PROPERTIES COMPILE_OPTIONS "-fexceptions")
else()
    target_compile_definitions(stxlib_boundary PUBLIC STXLIB_NO_EXCEPTION_RECORDER)
endif()
# Each module that links the boundary keeps its copy private (hidden
# symbols) and may be a shared library (position-independent code).
set_target_properties(stxlib_boundary PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(stxlib_boundary PUBLIC stxlib_state Threads::Threads)
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    # BCryptGenRandom (entropy_windows.cpp); ShellExecuteW (desktop_windows.cpp);
    # QueryInterruptTimePrecise and QueryUnbiasedInterruptTimePrecise
    # (clocks_windows.cpp), which kernel32's import library does not carry:
    # Microsoft documents them in api-ms-win-core-realtime-l1-1-1.dll
    # (library mincore). mingw-w64 installs that API set only inside
    # libmincore.a (crt Makefile.am: mincore.mri), so mincore is linked, after
    # kernel32 so that kernel32 still provides every function it exports and
    # mincore only these two.
    # SHGetKnownFolderPath (known_folders_windows.cpp) is shell32's;
    # CoTaskMemFree, which frees its result, is ole32's. K32EnumProcessModules,
    # K32GetModuleInformation and K32GetProcessMemoryInfo (images_windows.cpp,
    # process_windows.cpp; PSAPI_VERSION 2) are kernel32's. WaitOnAddress,
    # WakeByAddressSingle and WakeByAddressAll (atomic_wait_windows.cpp) are
    # in the API set api-ms-win-core-synch-l1-2-0, whose import library is
    # synchronization (mingw-w64's libsynchronization.a, the SDK's
    # Synchronization.lib); kernel32's import library does not carry them.
    target_link_libraries(stxlib_boundary PUBLIC bcrypt shell32 ole32 kernel32 mincore synchronization)
elseif(STXLIB_HOST_FAMILY STREQUAL "glibc")
    # dlopen, dlsym, dladdr and dl_iterate_phdr (library_posix.cpp,
    # image_regions_elf.cpp): libc itself from glibc 2.34; CMAKE_DL_LIBS
    # names libdl for older glibc and is empty where none is needed.
    target_link_libraries(stxlib_boundary PUBLIC ${CMAKE_DL_LIBS})
elseif(STXLIB_HOST_FAMILY STREQUAL "emscripten")
    # Settings at emscripten 6.0.13 (src/settings.js, each setting's comment):
    # - -pthread: threads as Web Workers over a SharedArrayBuffer.
    # - PTHREAD_POOL_SIZE and PTHREAD_POOL_SIZE_STRICT=2: a fixed pool made at
    #   start, and pthread_create failing with EAGAIN when it is used up
    #   rather than waiting for a new worker, which a thread that blocks
    #   without returning to the event loop would wait on forever.
    # - ABORTING_MALLOC=0 and ALLOW_MEMORY_GROWTH=1: malloc returns null when
    #   memory cannot grow (growMemory catches the failure,
    #   src/lib/libcore.js:211-235, and emscripten_resize_heap reports it,
    #   :252-262), so STXlib's allocator reports out_of_memory and never
    #   aborts or unwinds (decisions/2026-11-15-early-exit-integrity.md:
    #   "Allocators must not unwind"). ABORTING_MALLOC defaults on
    #   (settings.js:165) and the default memory does not grow (:230).
    # - FORCE_FILESYSTEM and -lidbfs.js (the js file system) or WASMFS.
    # - EXIT_RUNTIME only when STXLIB_EMSCRIPTEN_EXIT_RUNTIME is on.
    # A program linking these settings itself must use the same values
    # (the pool's size is reported from STXLIB_EMSCRIPTEN_POOL_SIZE).
    target_compile_definitions(stxlib_boundary PRIVATE
        STXLIB_EMSCRIPTEN_POOL_SIZE=${STXLIB_EMSCRIPTEN_POOL_SIZE}
        $<$<BOOL:${STXLIB_EMSCRIPTEN_EXIT_RUNTIME}>:STXLIB_EMSCRIPTEN_EXIT_RUNTIME>
        $<$<STREQUAL:${STXLIB_EMSCRIPTEN_FILESYSTEM},wasmfs>:STXLIB_EMSCRIPTEN_WASMFS>)
    target_compile_options(stxlib_boundary PUBLIC -pthread)
    target_link_options(stxlib_boundary INTERFACE
        -pthread
        "SHELL:-s PTHREAD_POOL_SIZE=${STXLIB_EMSCRIPTEN_POOL_SIZE}"
        "SHELL:-s PTHREAD_POOL_SIZE_STRICT=2"
        "SHELL:-s ABORTING_MALLOC=0"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s FORCE_FILESYSTEM=1")
    if(STXLIB_EMSCRIPTEN_FILESYSTEM STREQUAL "wasmfs")
        target_link_options(stxlib_boundary INTERFACE "SHELL:-s WASMFS=1")
    else()
        target_link_options(stxlib_boundary INTERFACE -lidbfs.js)
    endif()
    if(STXLIB_EMSCRIPTEN_EXIT_RUNTIME)
        target_link_options(stxlib_boundary INTERFACE "SHELL:-s EXIT_RUNTIME=1")
    endif()
    target_compile_options(stxlib_state PUBLIC -pthread)
endif()
# musl and FreeBSD carry the dl* functions in libc (FreeBSD: lib/libc/gen/
# dlfcn.c:65-149 at fb9033d2876d, the stubs rtld-elf replaces), and macOS in
# libSystem, so nothing more is linked there.
if(STXLIB_HOST_FAMILY STREQUAL "windows")
    # Static libc++ in every module (stx_auxlib docs/ALLOCATOR_OWNERSHIP.md,
    # 2.3): a shared libc++ DLL binds its own operator new and delete.
    set(_stxlib_link_flags
        "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS}")
    if(_stxlib_link_flags MATCHES "-shared-libstdc\\+\\+|-shared-libgcc|libc\\+\\+\\.dll")
        message(FATAL_ERROR "STXlib on Windows requires the static C++ runtime; the flags ask for a shared one")
    endif()
endif()

# ---- stxlib::algorithms -----------------------------------------------------
# Host-free algorithms (decision M1 = 1; A0 migration note 3.1): they use
# stxlib::boundary's vocabulary and services (the environment, entropy,
# stxlib_state) and make no host call of their own. Ported from stx_auxlib
# b6e330a (PROVENANCE.md, batch 3c and later).
# The owned transcendental kernels (include/stxlib/math.hpp), one source and
# table header per family under src/algorithms/math/, from stx_auxlib
# b6e330a. Each kernel source compiles once per variant
# (src/algorithms/math/variant.hpp); dispatch.cpp selects among them
# (namespace stxlib::math::unflushed), and flush.cpp defines the public
# functions over those with STXlib's subnormal rule (batch 3c8).
set(STXLIB_MATH_KERNEL_SOURCES
    src/algorithms/math/atan.cpp
    src/algorithms/math/cbrt.cpp
    src/algorithms/math/completions.cpp
    src/algorithms/math/exp.cpp
    src/algorithms/math/half_turn.cpp
    src/algorithms/math/hyperbolic.cpp
    src/algorithms/math/hypot.cpp
    src/algorithms/math/log.cpp
    src/algorithms/math/pow.cpp
    src/algorithms/math/region.cpp
    src/algorithms/math/sincos.cpp
    src/algorithms/math/tan.cpp
    src/algorithms/math/trig_region.cpp)
# Two paths for some kernels (src/algorithms/math/paths.hpp): today's and the
# region kernels (region.cpp, trig_region.cpp). Every build compiles both;
# these switches choose the one the public functions take, and every target
# here sees them (the tests pick their frozen hashes by them). Defaults: the
# faster path at today's accuracy, from the R2 bench (region only for sincos,
# sin, cos and tan of a double; paths.hpp says why for each); keep them equal
# to paths.hpp's.
option(STXLIB_MATH_REGION_POW_DOUBLE "pow(double) takes the region kernel" OFF)
option(STXLIB_MATH_REGION_POW_FLOAT "pow(float) takes the region round, test, decide path" OFF)
option(STXLIB_MATH_REGION_LOG_FLOAT "log, log2, log10 (float) take the region kernels" OFF)
option(STXLIB_MATH_REGION_HYPOT_FLOAT "hypot(float) takes the region kernel" OFF)
option(STXLIB_MATH_REGION_TRIG_DOUBLE "sincos, sin, cos and tan (double) take the region kernels" ON)
add_compile_definitions(
    STXLIB_MATH_REGION_POW_DOUBLE=$<BOOL:${STXLIB_MATH_REGION_POW_DOUBLE}>
    STXLIB_MATH_REGION_POW_FLOAT=$<BOOL:${STXLIB_MATH_REGION_POW_FLOAT}>
    STXLIB_MATH_REGION_LOG_FLOAT=$<BOOL:${STXLIB_MATH_REGION_LOG_FLOAT}>
    STXLIB_MATH_REGION_HYPOT_FLOAT=$<BOOL:${STXLIB_MATH_REGION_HYPOT_FLOAT}>
    STXLIB_MATH_REGION_TRIG_DOUBLE=$<BOOL:${STXLIB_MATH_REGION_TRIG_DOUBLE}>)
set(STXLIB_MATH_SOURCES
    ${STXLIB_MATH_KERNEL_SOURCES}
    src/algorithms/math/constants.cpp
    src/algorithms/math/dispatch.cpp
    src/algorithms/math/flush.cpp)
# x86-64 builds carry two kernel variants, v128 (SSE2) and avx2_fma, and
# call the one the process's SIMD level selects (include/stxlib/simd.hpp).
# Every other build has one native variant.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
    set(STXLIB_MATH_DISPATCH_DEFAULT ON)
else()
    set(STXLIB_MATH_DISPATCH_DEFAULT OFF)
endif()
option(STXLIB_MATH_DISPATCH "Build the x86-64 v128 and avx2_fma math variants and select at run time"
    ${STXLIB_MATH_DISPATCH_DEFAULT})
# The kernels' bit-identical results need floating-point contraction off.
# Every target here has it (stxlib_fp_options above); the math targets name
# it again so that a change to the directory option cannot reach them
# unnoticed.
set(stxlib_math_options ${stxlib_fp_options})

set(STXLIB_ALGORITHMS_SOURCES
    ${STXLIB_MATH_SOURCES}
    src/algorithms/calendar.cpp
    src/algorithms/conv/canon.cpp
    src/algorithms/conv/field.cpp
    src/algorithms/conv/kernels.cpp
    src/algorithms/conv/kernels_avx2_fma.cpp
    src/algorithms/conv/kernels_scalar.cpp
    src/algorithms/conv/kernels_v128.cpp
    src/algorithms/conv/line_field.cpp
    src/algorithms/conv/transfer.cpp
    src/algorithms/case_folding.cpp
    src/algorithms/textlayout/analysis.cpp
    src/algorithms/textlayout/line_break.cpp
    src/algorithms/hash.cpp
    src/algorithms/markdown/blocks.cpp
    src/algorithms/markdown/characters.cpp
    src/algorithms/markdown/entities.cpp
    src/algorithms/markdown/inlines.cpp
    src/algorithms/markdown/lines.cpp
    src/algorithms/markdown/links.cpp
    src/algorithms/markdown/parse.cpp
    src/algorithms/numeric_text.cpp
    src/algorithms/precedence.cpp
    src/algorithms/frozen_map.cpp
    # stx_fft (include/stxlib/fft.hpp): the plans, and BFFT's three kernels
    # compiled once per SIMD tier (src/algorithms/fft/variant.inc; the
    # vector128 and avx2_fma files are empty where the tier does not exist,
    # and avx2_fma's code is compiled for that target by an attribute, never
    # by a command-line -mavx2).
    src/algorithms/fft/plan.cpp
    src/algorithms/fft/kernels_scalar.cpp
    src/algorithms/fft/kernels_v128.cpp
    src/algorithms/fft/kernels_avx2_fma.cpp
    # The plain mode's PSFT (src/algorithms/fft/walk.inc) per walk tier: the
    # exact build on plain lanes everywhere, on 128-bit vectors, and the fused
    # build on 128-bit vectors (NEON; SSE with FMA3 through an attribute).
    src/algorithms/fft/walk_plan.cpp
    src/algorithms/fft/walk_portable.cpp
    src/algorithms/fft/walk_exact128.cpp
    src/algorithms/fft/walk_fused128.cpp
    src/algorithms/glyphs/font_cff.cpp
    src/algorithms/glyphs/font_file.cpp
    src/algorithms/glyphs/font_names.cpp
    src/algorithms/glyphs/font_truetype.cpp
    src/algorithms/glyphs/glyph_face.cpp
    src/algorithms/glyphs/glyph_store.cpp
    src/algorithms/glyphs/turn_lattice.cpp
    src/algorithms/glyphs/hinter.cpp
    src/algorithms/glyphs/layout_tables.cpp
    src/algorithms/glyphs/shape_buffer.cpp
    src/algorithms/glyphs/shape_unicode.cpp
    src/algorithms/glyphs/unicode_properties.cpp
    src/algorithms/glyphs/shape_plan.cpp
    src/algorithms/glyphs/shape_apply.cpp
    src/algorithms/glyphs/shape_text.cpp
    src/algorithms/glyphs/glyph_shape.cpp
    src/algorithms/glyphs/hinter_accents.cpp
    src/algorithms/glyphs/hinter_align.cpp
    src/algorithms/glyphs/hinter_edges.cpp
    src/algorithms/glyphs/hinter_fit.cpp
    src/algorithms/glyphs/hinter_metrics.cpp
    src/algorithms/glyphs/hinter_outline.cpp
    src/algorithms/glyphs/hinter_segments.cpp
    src/algorithms/glyphs/mask.cpp
    src/algorithms/random.cpp
    src/algorithms/row_height_index.cpp
    src/algorithms/simd.cpp
    src/algorithms/syntax/css.cpp
    src/algorithms/syntax/document.cpp
    src/algorithms/syntax/engine.cpp
    src/algorithms/syntax/languages.cpp
    src/algorithms/syntax/markup.cpp
    src/algorithms/voting.cpp)
add_library(stxlib_algorithms STATIC ${STXLIB_ALGORITHMS_SOURCES})
if(stxlib_outline_atomics)
    target_sources(stxlib_algorithms PRIVATE $<TARGET_OBJECTS:stxlib_outline_atomics>)
endif()
add_library(stxlib::algorithms ALIAS stxlib_algorithms)
target_include_directories(stxlib_algorithms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_algorithms PUBLIC cxx_std_20)
target_compile_options(stxlib_algorithms PRIVATE ${stxlib_warnings} -fno-exceptions ${stxlib_frame_pointers})
set_target_properties(stxlib_algorithms PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(stxlib_algorithms PUBLIC stxlib_boundary)
# simd.cpp reaches stxlib_state, whose layout key carries the build type
# (state.hpp, layout 9), so this target has STXLIB_SHADOW as the boundary does.
target_compile_definitions(stxlib_algorithms PRIVATE $<$<BOOL:${stxlib_shadow_on}>:STXLIB_SHADOW>)
target_compile_options(stxlib_algorithms PRIVATE ${stxlib_math_options})
if(STXLIB_MATH_DISPATCH)
    target_compile_definitions(stxlib_algorithms PRIVATE STXLIB_MATH_VARIANT=v128 STXLIB_MATH_DISPATCH_X86=1)
    # The avx2_fma variant: the same kernel sources and private options, its
    # own namespace; variant.hpp applies the avx2,fma target to its
    # definitions only, so no command-line -mavx2 reaches shared inline code.
    add_library(stxlib_math_avx2_fma OBJECT ${STXLIB_MATH_KERNEL_SOURCES})
    set_target_properties(stxlib_math_avx2_fma PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(stxlib_math_avx2_fma PRIVATE cxx_std_20)
    target_include_directories(stxlib_math_avx2_fma PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(stxlib_math_avx2_fma PRIVATE STXLIB_MATH_VARIANT=avx2_fma STXLIB_MATH_AVX2_FMA=1)
    target_compile_options(stxlib_math_avx2_fma PRIVATE
        ${stxlib_warnings} -fno-exceptions ${stxlib_frame_pointers} ${stxlib_math_options})
    target_sources(stxlib_algorithms PRIVATE $<TARGET_OBJECTS:stxlib_math_avx2_fma>)
else()
    target_compile_definitions(stxlib_algorithms PRIVATE STXLIB_MATH_VARIANT=native)
endif()

# stx_mcp: a Model Context Protocol server over anonymous pipes
# (include/stxlib/mcp.hpp, json.hpp; channels and pipes are the
# boundary's). Its own target, so a program that serves no model links none
# of it. numeric_text (stxlib_algorithms) reads and writes JSON numbers.
set(STXLIB_MCP_SOURCES
    src/mcp/json.cpp
    src/mcp/protocol.cpp
    src/mcp/schema.cpp
    src/mcp/server.cpp)
add_library(stxlib_mcp STATIC ${STXLIB_MCP_SOURCES})
add_library(stxlib::mcp ALIAS stxlib_mcp)
target_include_directories(stxlib_mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(stxlib_mcp PUBLIC cxx_std_20)
target_compile_options(stxlib_mcp PRIVATE ${stxlib_warnings} -fno-exceptions ${stxlib_frame_pointers})
set_target_properties(stxlib_mcp PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(stxlib_mcp PUBLIC stxlib_algorithms stxlib_boundary)
# Its headers reach thread state through threads.hpp, as the algorithms do.
target_compile_definitions(stxlib_mcp PRIVATE $<$<BOOL:${stxlib_shadow_on}>:STXLIB_SHADOW>)

option(STXLIB_BUILD_TOOLS "Build STXlib's table generators" OFF)
if(STXLIB_BUILD_TOOLS)
    # Each regenerates its family's table header in src/algorithms/math/ byte
    # for byte, at the path given as its one argument (from stx_auxlib
    # b6e330a).
    foreach(table IN ITEMS atan exp log sincos)
        add_executable(stxlib_gen_${table}_table tools/math/gen_${table}_table.cpp)
        target_compile_features(stxlib_gen_${table}_table PRIVATE cxx_std_20)
        target_compile_options(stxlib_gen_${table}_table PRIVATE -ffp-contract=off -fno-fast-math)
    endforeach()
    # The minimum-remainder search of tools/math/sincos_certificate.py (an
    # ordered index from src/algorithms/stx_btree; unsigned __int128, so Clang
    # or GCC only).
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_executable(stxlib_sincos_minimum_remainder tools/math/sincos_minimum_remainder.cpp)
        target_compile_features(stxlib_sincos_minimum_remainder PRIVATE cxx_std_20)
        target_include_directories(stxlib_sincos_minimum_remainder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(stxlib_sincos_minimum_remainder PRIVATE stxlib::algorithms)
    endif()
endif()

# ---- tests ------------------------------------------------------------------
if(STXLIB_TESTS)
    enable_testing()
    set(stxlib_test_warnings ${stxlib_warnings} ${stxlib_frame_pointers})

    # Emscripten: every test runs under Node (CMAKE_CROSSCOMPILING_EMULATOR,
    # cmake/emscripten.cmake) and is linked with EXIT_RUNTIME, so main's
    # status is the process's (the reference conformance test below links
    # no STXlib target, so the option is set for the directory). Tests of
    # services a page lacks (child processes, loaded images, sealed pages,
    # the process's own arguments, a second module) are not built there;
    # tests/emscripten_tests.cpp checks that each of those answers
    # unsupported. A death case runs under the same emulator
    # (tests/death_check.cmake, EMULATOR, '|'-separated).
    set(stxlib_web_tests OFF)
    set(stxlib_emulator_text "")
    if(STXLIB_HOST_FAMILY STREQUAL "emscripten")
        set(stxlib_web_tests ON)
        add_link_options("SHELL:-s EXIT_RUNTIME=1")
        string(JOIN "|" stxlib_emulator_text ${CMAKE_CROSSCOMPILING_EMULATOR})
    endif()

    set(stxlib_boundary_tests vocabulary memory platform path files threads thread_pool local_time rounding
                               owned_vocabulary owned_text ascii_search heap transcode thread_records channels
                               atomic_word stop_token locks)
    if(STXLIB_EXCEPTIONS)
        list(APPEND stxlib_boundary_tests gate trampoline shadow)
    endif()
    if(stxlib_web_tests)
        list(APPEND stxlib_boundary_tests emscripten)
    else()
        list(APPEND stxlib_boundary_tests children images pages_wake debugger_anchor pipes)
    endif()
    foreach(test_name IN LISTS stxlib_boundary_tests)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()
    # rounding_tests' and heap_tests' sample generators are marked
    # STXLIB_MODULAR (core/checked.hpp).
    target_include_directories(stxlib_rounding_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(stxlib_heap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # debugger_anchor_tests reads the registry back from stxlib_state's
    # `.stxshdw` section (include/stxlib/debugger_anchor.hpp); it names the
    # state's module through state.hpp and, on ELF and Mach-O, finds it with
    # dladdr.
    if(NOT stxlib_web_tests)
        target_include_directories(stxlib_debugger_anchor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(stxlib_debugger_anchor_tests PRIVATE ${CMAKE_DL_LIBS})
        # state.hpp's layout key carries the build type (STXLIB_SHADOW), as
        # stxlib_state has it.
        target_compile_definitions(stxlib_debugger_anchor_tests PRIVATE $<$<BOOL:${stxlib_shadow_on}>:STXLIB_SHADOW>)
    endif()
    if(stxlib_web_tests AND STXLIB_EMSCRIPTEN_FILESYSTEM STREQUAL "wasmfs")
        target_compile_definitions(stxlib_emscripten_tests PRIVATE STXLIB_TEST_WASMFS)
        target_compile_definitions(stxlib_files_tests PRIVATE STXLIB_TEST_WASMFS)
    endif()
    # emscripten_tests checks the SIMD level (simd.cpp, in stxlib_algorithms).
    if(stxlib_web_tests)
        target_link_libraries(stxlib_emscripten_tests PRIVATE stxlib::algorithms)
    endif()

    # The process's own arguments, compared with argv: the test is started
    # with spaces, quotes, a trailing backslash, an empty argument and one
    # that is not ASCII; the test starts itself again as a child with an
    # argument that is not UTF-8 (POSIX) or holds a lone surrogate (Windows)
    # (tests/process_tests.cpp).
    if(NOT stxlib_web_tests)
        add_executable(stxlib_process_tests tests/process_tests.cpp)
        target_link_libraries(stxlib_process_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_process_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_process_tests
            COMMAND stxlib_process_tests "plain" "two words" "" "quote\"inside" "trailing\\" "café")
    endif()

    if(STXLIB_ALLOCATOR STREQUAL "stx_minmallocpp")
        target_compile_definitions(stxlib_memory_tests PRIVATE STXLIB_EXPECT_STX_MINMALLOCPP)
    endif()

    # A benchmark of stxlib::memory beside the host's malloc
    # (tests/memory_bench.cpp). Not a test: built, never run by ctest.
    add_executable(stxlib_memory_bench tests/memory_bench.cpp)
    target_link_libraries(stxlib_memory_bench PRIVATE stxlib::boundary Threads::Threads)
    target_compile_options(stxlib_memory_bench PRIVATE ${stxlib_test_warnings})

    # stx_minmallocpp. Its arithmetic (size classes, the alignment rule, the
    # block index, the free-run lists) is in headers and is checked
    # exhaustively in every configuration. Its behaviour is tested with the
    # allocator's sources linked straight into the test, also in every
    # configuration, whatever backend stxlib_state has.
    add_executable(stxlib_stx_minmallocpp_tests tests/stx_minmallocpp_tests.cpp)
    target_include_directories(stxlib_stx_minmallocpp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_tests PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_stx_minmallocpp_tests COMMAND stxlib_stx_minmallocpp_tests)
    # The allocator's sources, compiled as stxlib_state compiles them
    # (warnings, no exceptions, hidden visibility) with its entry and exit
    # checks on (page.hpp, STX_MINMALLOCPP_CHECK).
    set(stxlib_stx_minmallocpp_sources
        src/state/stx_minmallocpp/allocator.cpp
        src/state/stx_minmallocpp/heap.cpp
        src/state/stx_minmallocpp/region.cpp)
    if(WIN32)
        list(APPEND stxlib_stx_minmallocpp_sources src/hosted/windows/minmallocpp_os_windows.cpp)
    else()
        list(APPEND stxlib_stx_minmallocpp_sources src/hosted/posix/minmallocpp_os_posix.cpp)
    endif()
    if(STXLIB_HOST_FAMILY STREQUAL "emscripten")
        list(APPEND stxlib_stx_minmallocpp_sources src/hosted/emscripten/minmallocpp_os_emscripten.cpp)
    endif()
    add_library(stxlib_stx_minmallocpp_checked OBJECT ${stxlib_stx_minmallocpp_sources})
    target_include_directories(stxlib_stx_minmallocpp_checked PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_checked PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_checked PRIVATE ${stxlib_warnings} ${stxlib_frame_pointers} -fno-exceptions)
    target_compile_definitions(stxlib_stx_minmallocpp_checked PRIVATE STXLIB_STX_MINMALLOCPP_CHECKS)
    set_target_properties(stxlib_stx_minmallocpp_checked PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)
    # The same sources without the checks, for the layer-by-layer benchmark
    # (tests/stx_minmallocpp_floor_bench.cpp; built, never run by ctest).
    add_library(stxlib_stx_minmallocpp_plain OBJECT ${stxlib_stx_minmallocpp_sources})
    target_include_directories(stxlib_stx_minmallocpp_plain PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_plain PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_plain PRIVATE ${stxlib_warnings} ${stxlib_frame_pointers} -fno-exceptions)
    set_target_properties(stxlib_stx_minmallocpp_plain PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)
    add_executable(stxlib_stx_minmallocpp_floor_bench tests/stx_minmallocpp_floor_bench.cpp
        $<TARGET_OBJECTS:stxlib_stx_minmallocpp_plain>)
    target_include_directories(stxlib_stx_minmallocpp_floor_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_floor_bench PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_floor_bench PRIVATE ${stxlib_test_warnings})
    target_link_libraries(stxlib_stx_minmallocpp_floor_bench PRIVATE Threads::Threads $<$<BOOL:${stxlib_outline_atomics}>:stxlib_outline_atomics>)

    # The interleaving checker (tests/stx_minmallocpp_interleave.cpp): the
    # same sources with the marks before each shared-memory step turned into
    # hand-overs to the checker, which runs every ordering of the steps with
    # up to two forced switches, each in a child process. POSIX hosts (fork).
    if(NOT WIN32)
        add_library(stxlib_stx_minmallocpp_stepped OBJECT ${stxlib_stx_minmallocpp_sources})
        target_include_directories(stxlib_stx_minmallocpp_stepped PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(stxlib_stx_minmallocpp_stepped PRIVATE cxx_std_20)
        target_compile_options(stxlib_stx_minmallocpp_stepped PRIVATE ${stxlib_warnings} ${stxlib_frame_pointers} -fno-exceptions)
        target_compile_definitions(stxlib_stx_minmallocpp_stepped PRIVATE
            STXLIB_STX_MINMALLOCPP_CHECKS STXLIB_STX_MINMALLOCPP_INTERLEAVE)
        add_executable(stxlib_stx_minmallocpp_interleave tests/stx_minmallocpp_interleave.cpp
            $<TARGET_OBJECTS:stxlib_stx_minmallocpp_stepped>)
        target_include_directories(stxlib_stx_minmallocpp_interleave PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(stxlib_stx_minmallocpp_interleave PRIVATE cxx_std_20)
        target_compile_options(stxlib_stx_minmallocpp_interleave PRIVATE ${stxlib_test_warnings})
        target_compile_definitions(stxlib_stx_minmallocpp_interleave PRIVATE
            STXLIB_STX_MINMALLOCPP_CHECKS STXLIB_STX_MINMALLOCPP_INTERLEAVE)
        target_link_libraries(stxlib_stx_minmallocpp_interleave PRIVATE Threads::Threads $<$<BOOL:${stxlib_outline_atomics}>:stxlib_outline_atomics>)
        add_test(NAME stxlib_stx_minmallocpp_interleave COMMAND stxlib_stx_minmallocpp_interleave 2)
    endif()

    add_executable(stxlib_stx_minmallocpp_direct_tests tests/stx_minmallocpp_direct_tests.cpp
        $<TARGET_OBJECTS:stxlib_stx_minmallocpp_checked>)
    target_include_directories(stxlib_stx_minmallocpp_direct_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_direct_tests PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_direct_tests PRIVATE ${stxlib_test_warnings})
    target_link_libraries(stxlib_stx_minmallocpp_direct_tests PRIVATE Threads::Threads $<$<BOOL:${stxlib_outline_atomics}>:stxlib_outline_atomics>)
    add_test(NAME stxlib_stx_minmallocpp_direct_tests COMMAND stxlib_stx_minmallocpp_direct_tests)
    # On Linux and FreeBSD aarch64 these programs link STXlib's
    # outline-atomics helpers (stxlib_outline_atomics, above), so the
    # allocator under test runs the helpers it runs in stxlib_state, not the
    # toolchain's.
    # These programs link no other STXlib target, so on Emscripten they take the
    # memory and thread settings stxlib_boundary gives every program: memory
    # that grows, malloc (and so the allocator's reservations) null rather
    # than an abort when it cannot, and the fixed pool of workers.
    if(stxlib_web_tests)
        set(stxlib_stx_minmallocpp_web_links
            "SHELL:-s ALLOW_MEMORY_GROWTH=1"
            "SHELL:-s ABORTING_MALLOC=0"
            "SHELL:-s PTHREAD_POOL_SIZE=${STXLIB_EMSCRIPTEN_POOL_SIZE}"
            "SHELL:-s PTHREAD_POOL_SIZE_STRICT=2")
        target_link_options(stxlib_stx_minmallocpp_direct_tests PRIVATE ${stxlib_stx_minmallocpp_web_links})
        target_link_options(stxlib_stx_minmallocpp_floor_bench PRIVATE ${stxlib_stx_minmallocpp_web_links})
        target_link_options(stxlib_stx_minmallocpp_interleave PRIVATE ${stxlib_stx_minmallocpp_web_links})
    endif()
    # The allocator's leaves: every symbol its objects leave undefined is a
    # listed host call or a toolchain helper (tools/check_object_surface.py,
    # template_harness's object surface check, with the component file
    # tools/object_surface/stx_minmallocpp.toml; it needs Python 3.11 for
    # tomllib). The objects are the checked library's, laid out as the Ninja
    # and Makefile generators do.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND CMAKE_NM AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        if(WIN32)
            set(stxlib_stx_minmallocpp_family windows)
        elseif(STXLIB_HOST_FAMILY STREQUAL "emscripten")
            set(stxlib_stx_minmallocpp_family web)
        else()
            set(stxlib_stx_minmallocpp_family posix)
        endif()
        # Sanitizer and integer-check builds allow the instrumentation's
        # runtime entries (the tool's --sanitized); every other build is
        # checked exactly.
        set(stxlib_stx_minmallocpp_sanitized)
        if(stxlib_sanitized)
            set(stxlib_stx_minmallocpp_sanitized --sanitized)
        endif()
        add_test(NAME stxlib_stx_minmallocpp_surface
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_object_surface.py
                    --config ${CMAKE_CURRENT_SOURCE_DIR}/tools/object_surface/stx_minmallocpp.toml
                    --nm ${CMAKE_NM} --family ${stxlib_stx_minmallocpp_family}
                    --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_stx_minmallocpp_checked.dir
                    ${stxlib_stx_minmallocpp_sanitized})
    endif()

    # The display surface's pure parts (core/surface_*.hpp) and its public
    # API where the family has none.
    add_executable(stxlib_surface_tests tests/surface_tests.cpp)
    target_link_libraries(stxlib_surface_tests PRIVATE stxlib::boundary)
    target_include_directories(stxlib_surface_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_surface_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_surface_tests COMMAND stxlib_surface_tests)

    # stxlib::algorithms.
    foreach(test_name IN ITEMS calendar conv frozen_map hash numeric_text precedence random simd syntax)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::algorithms)
        # core/checked.hpp (the modular marker) for the tests' own generators.
        target_include_directories(stxlib_${test_name}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()
    # stxlib::mcp: JSON reading and writing, and the server driven by a fake
    # client over pipes (tests/mcp_tests.cpp). Pipes are not a page's, so
    # neither is built for the web.
    if(NOT stxlib_web_tests)
        foreach(test_name IN ITEMS json mcp)
            add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
            target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::mcp)
            target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
            add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
        endforeach()
        # mcp_tests also checks the schema subset (src/mcp/schema.hpp).
        target_include_directories(stxlib_mcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        set_tests_properties(stxlib_mcp_tests PROPERTIES TIMEOUT 120)
    endif()
    # STXlib's outline-atomics helpers (tests/outline_atomics_tests.cpp):
    # every helper against a plain reference on the loop and, where the CPU
    # has LSE, on the LSE path, the register contract, detection, and a
    # concurrent stress run. Elsewhere it checks Features::lse and passes.
    add_executable(stxlib_outline_atomics_tests tests/outline_atomics_tests.cpp)
    target_link_libraries(stxlib_outline_atomics_tests PRIVATE stxlib::algorithms Threads::Threads)
    target_include_directories(stxlib_outline_atomics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_outline_atomics_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_outline_atomics_tests COMMAND stxlib_outline_atomics_tests)
    # The linked binaries take the helpers from STXlib, not from libgcc or
    # compiler-rt (tools/check_outline_atomics.py): a test program linked
    # through the STXlib targets, one linked from the allocator's objects,
    # a glyph test, and a shared stxlib_state.
    if(stxlib_outline_atomics AND Python3_Interpreter_FOUND AND CMAKE_NM)
        set(stxlib_outline_atomics_binaries
            $<TARGET_FILE:stxlib_outline_atomics_tests>
            $<TARGET_FILE:stxlib_stx_minmallocpp_direct_tests>
            $<TARGET_FILE:stxlib_glyph_shape_tests>)
        if(STXLIB_STATE_SHARED)
            list(APPEND stxlib_outline_atomics_binaries $<TARGET_FILE:stxlib_state>)
        endif()
        add_test(NAME stxlib_outline_atomics_link
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_outline_atomics.py
                    --nm ${CMAKE_NM} ${stxlib_outline_atomics_binaries})
    endif()
    # stxlib::syntax's hand tool: a source file as highlighted HTML, and the
    # engine's speed (tests/syntax_show.cpp). Not a ctest.
    add_executable(stxlib_syntax_show tests/syntax_show.cpp)
    target_link_libraries(stxlib_syntax_show PRIVATE stxlib::algorithms)
    target_compile_options(stxlib_syntax_show PRIVATE ${stxlib_test_warnings})
    # stxlib::conv on every SIMD tier this machine runs: the same frozen digests
    # (tests/conv_tests.cpp) through each forced level.
    foreach(level IN ITEMS scalar vector128 avx2_fma)
        add_test(NAME stxlib_conv_tests_${level} COMMAND stxlib_conv_tests)
        set_tests_properties(stxlib_conv_tests_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
    endforeach()
    # stxlib::glyphs: the coverage mask against exact areas (tests/
    # glyph_mask_tests.cpp).
    add_executable(stxlib_glyph_mask_tests tests/glyph_mask_tests.cpp)
    target_link_libraries(stxlib_glyph_mask_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_mask_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_mask_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_mask_tests COMMAND stxlib_glyph_mask_tests)
    # stxlib::glyphs: the font reader against fonts the test builds in
    # memory (tests/font_file_tests.cpp).
    add_executable(stxlib_font_file_tests tests/font_file_tests.cpp)
    target_link_libraries(stxlib_font_file_tests PRIVATE stxlib::algorithms)
    target_compile_options(stxlib_font_file_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_font_file_tests COMMAND stxlib_font_file_tests)
    # stxlib::glyphs: the hinter against letters the test draws
    # (tests/glyph_hinter_tests.cpp).
    add_executable(stxlib_glyph_hinter_tests tests/glyph_hinter_tests.cpp)
    target_link_libraries(stxlib_glyph_hinter_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_hinter_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_hinter_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_hinter_tests COMMAND stxlib_glyph_hinter_tests)
    # stxlib::glyphs: reading the layout tables (GDEF, GSUB, GPOS), against
    # tables the test writes (tests/glyph_layout_tests.cpp); it reads the
    # internal header.
    add_executable(stxlib_glyph_layout_tests tests/glyph_layout_tests.cpp)
    target_link_libraries(stxlib_glyph_layout_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_layout_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_layout_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_layout_tests COMMAND stxlib_glyph_layout_tests)
    # The glyph rasterizer's speed on a published same-task benchmark
    # (tests/glyph_raster_bench.cpp): one glyph to 8-bit coverage at 10 to
    # 100 px/em. Opt in; built only with the option, never run by ctest. The
    # flags it was built with are printed in its header.
    option(STXLIB_GLYPH_RASTER_BENCH "Build stxlib_glyph_raster_bench (tests/glyph_raster_bench.cpp)" OFF)
    if(STXLIB_GLYPH_RASTER_BENCH)
        add_executable(stxlib_glyph_raster_bench tests/glyph_raster_bench.cpp)
        target_link_libraries(stxlib_glyph_raster_bench PRIVATE stxlib::algorithms)
        target_compile_options(stxlib_glyph_raster_bench PRIVATE ${stxlib_test_warnings})
        string(TOUPPER "${CMAKE_BUILD_TYPE}" stxlib_glyph_bench_config)
        string(JOIN " " stxlib_glyph_bench_options ${stxlib_test_warnings})
        set(stxlib_glyph_bench_flags
            "${CMAKE_BUILD_TYPE}: ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${stxlib_glyph_bench_config}} ${stxlib_glyph_bench_options}")
        string(REPLACE "\"" "'" stxlib_glyph_bench_flags "${stxlib_glyph_bench_flags}")
        string(STRIP "${stxlib_glyph_bench_flags}" stxlib_glyph_bench_flags)
        target_compile_definitions(stxlib_glyph_raster_bench PRIVATE
            "STXLIB_BENCH_FLAGS=\"${stxlib_glyph_bench_flags}\"")
    endif()
    # stxlib::glyphs: a mask's surface and its turning, against numbers
    # worked out by tools/turn_lattice_oracle.py (tests/glyph_turn_tests.cpp);
    # it reads the internal header.
    add_executable(stxlib_glyph_turn_tests tests/glyph_turn_tests.cpp)
    target_link_libraries(stxlib_glyph_turn_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_turn_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_turn_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_turn_tests COMMAND stxlib_glyph_turn_tests)
    # stxlib::glyphs: the shaper's buffer (tests/glyph_shape_buffer_tests.cpp);
    # it reads the internal header.
    add_executable(stxlib_glyph_shape_buffer_tests tests/glyph_shape_buffer_tests.cpp)
    target_link_libraries(stxlib_glyph_shape_buffer_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_shape_buffer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_shape_buffer_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_shape_buffer_tests COMMAND stxlib_glyph_shape_buffer_tests)
    # stxlib::glyphs: what the shaper knows of a character
    # (tests/glyph_shape_unicode_tests.cpp); it reads the internal header.
    add_executable(stxlib_glyph_shape_unicode_tests tests/glyph_shape_unicode_tests.cpp)
    target_link_libraries(stxlib_glyph_shape_unicode_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_shape_unicode_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_shape_unicode_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_shape_unicode_tests COMMAND stxlib_glyph_shape_unicode_tests)
    # stxlib::glyphs: shaping, on a font with layout tables built in the
    # test (tests/glyph_shape_tests.cpp).
    add_executable(stxlib_glyph_shape_tests tests/glyph_shape_tests.cpp)
    target_link_libraries(stxlib_glyph_shape_tests PRIVATE stxlib::algorithms)
    target_compile_options(stxlib_glyph_shape_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_shape_tests COMMAND stxlib_glyph_shape_tests)
    # The block engine (the IC study's phase IC2; tests/ic/engine): the
    # blocks tools/ic_declarations.py generates from the declarations
    # (tests/ic/generated), run by tests/ic/ic_run.cpp in legal and misuse
    # modes against every component with an [engine] table. The ctest runs a
    # bounded run per component; the gate's stated run is
    #   stxlib_ic_run --cases 20000 --gate tests/ic/results/gate_run.json
    # The allocator's component calls stx_minmallocpp directly, which links
    # only where stxlib_state is static. Each minimised failure the runner
    # writes to tests/ic/regressions is a test of its own.
    if(NOT stxlib_web_tests)
        add_library(stxlib_ic_engine STATIC tests/ic/engine/ic_engine.cpp)
        target_include_directories(stxlib_ic_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                   ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(stxlib_ic_engine PUBLIC stxlib::algorithms)
        target_compile_options(stxlib_ic_engine PRIVATE ${stxlib_test_warnings})
        if(NOT STXLIB_STATE_SHARED)
            target_compile_definitions(stxlib_ic_engine PUBLIC STXLIB_IC_ALLOCATOR)
        endif()
        add_executable(stxlib_ic_run tests/ic/ic_run.cpp)
        target_link_libraries(stxlib_ic_run PRIVATE stxlib_ic_engine)
        target_compile_options(stxlib_ic_run PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_ic_engine_run COMMAND stxlib_ic_run --cases 200 --steps 32)
        # The engine locks its Mutex slots in whatever order a case draws, in
        # one thread; ThreadSanitizer's lock-order check reads that as a
        # potential deadlock (STXlib CI, 2026-11-23), so it is off here.
        set_tests_properties(stxlib_ic_engine_run PROPERTIES ENVIRONMENT "TSAN_OPTIONS=detect_deadlocks=0")
        file(GLOB stxlib_ic_regressions CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ic/regressions/*.cpp)
        foreach(regression IN LISTS stxlib_ic_regressions)
            get_filename_component(regression_name ${regression} NAME_WE)
            add_executable(stxlib_ic_regression_${regression_name} ${regression})
            target_link_libraries(stxlib_ic_regression_${regression_name} PRIVATE stxlib_ic_engine)
            target_compile_options(stxlib_ic_regression_${regression_name} PRIVATE ${stxlib_test_warnings})
            add_test(NAME stxlib_ic_regression_${regression_name} COMMAND stxlib_ic_regression_${regression_name})
        endforeach()
        # The engine under libFuzzer (fuzz/ic_fuzz.cpp): Clang only, off by
        # default; libFuzzer drives coverage, the engine checks.
        option(STXLIB_IC_FUZZ "Build the block engine's libFuzzer driver (Clang)" OFF)
        if(STXLIB_IC_FUZZ)
            add_executable(stxlib_ic_fuzz fuzz/ic_fuzz.cpp)
            target_link_libraries(stxlib_ic_fuzz PRIVATE stxlib_ic_engine)
            target_compile_options(stxlib_ic_fuzz PRIVATE ${stxlib_test_warnings} -fsanitize=fuzzer)
            target_link_options(stxlib_ic_fuzz PRIVATE -fsanitize=fuzzer)
        endif()
    endif()
    # stxlib::glyphs with every check on (src/algorithms/glyphs/
    # glyph_checks.hpp): the glyph sources compiled again with
    # STXLIB_GLYPHS_CHECKS, so that each structure carried from call to call
    # is checked whole at the end of every call, and six of the glyph
    # tests run on them. The objects come before the library in the link,
    # so the library's unchecked ones are not taken.
    file(GLOB stxlib_glyph_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/glyphs/*.cpp)
    add_library(stxlib_glyphs_checked OBJECT ${stxlib_glyph_sources})
    target_include_directories(stxlib_glyphs_checked PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_glyphs_checked PRIVATE cxx_std_20)
    target_compile_definitions(stxlib_glyphs_checked PRIVATE STXLIB_GLYPHS_CHECKS)
    target_compile_options(stxlib_glyphs_checked PRIVATE ${stxlib_warnings} -fno-exceptions ${stxlib_frame_pointers})
    foreach(test_name IN ITEMS font_file glyph_hinter glyph_untrusted glyph_shape_buffer glyph_shape glyph_turn)
        add_executable(stxlib_${test_name}_checked_tests tests/${test_name}_tests.cpp $<TARGET_OBJECTS:stxlib_glyphs_checked>)
        target_link_libraries(stxlib_${test_name}_checked_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_${test_name}_checked_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_${test_name}_checked_tests PRIVATE ${stxlib_test_warnings})
        target_compile_definitions(stxlib_${test_name}_checked_tests PRIVATE STXLIB_GLYPHS_CHECKS_EXPECTED)
        add_test(NAME stxlib_${test_name}_checked_tests COMMAND stxlib_${test_name}_checked_tests)
    endforeach()
    # The glyph code's leaves: every symbol its objects leave undefined is
    # STXlib's allocator, lock, error record or requirement failure, or a
    # toolchain helper (tools/check_glyphs_surface.py). The objects are the
    # library's, laid out as the Ninja and Makefile generators do.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND CMAKE_NM AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        set(stxlib_glyphs_surface_options)
        if(stxlib_sanitized)
            list(APPEND stxlib_glyphs_surface_options --sanitized)
        endif()
        if(STXLIB_HOST_FAMILY STREQUAL "emscripten")
            list(APPEND stxlib_glyphs_surface_options --web)
        endif()
        add_test(NAME stxlib_glyphs_surface
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_glyphs_surface.py
                    --nm ${CMAKE_NM} --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_algorithms.dir
                    --under src/algorithms/glyphs/ ${stxlib_glyphs_surface_options})
    endif()
    # stxlib::glyphs against font tables that are wrong on purpose: what each
    # reader promises of untrusted input, and a bounded search of altered
    # tables (tests/glyph_untrusted_tests.cpp); it reads the internal header
    # of the layout tables.
    add_executable(stxlib_glyph_untrusted_tests tests/glyph_untrusted_tests.cpp)
    target_link_libraries(stxlib_glyph_untrusted_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_glyph_untrusted_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_glyph_untrusted_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_glyph_untrusted_tests COMMAND stxlib_glyph_untrusted_tests)
    # General category and canonical composition (unicode_properties.hpp):
    # every code point against UnicodeData.txt, read by the test's own
    # parser. Not on the web (the file is the host's).
    if(NOT stxlib_web_tests)
        add_executable(stxlib_unicode_properties_tests tests/unicode_properties_tests.cpp)
        target_link_libraries(stxlib_unicode_properties_tests PRIVATE stxlib::algorithms)
        target_compile_options(stxlib_unicode_properties_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_unicode_properties_tests
            COMMAND stxlib_unicode_properties_tests ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/UnicodeData.txt)
    endif()
    # Simple case folding (C7): every code point against CaseFolding.txt,
    # read by the test's own parser; and the committed table is the
    # generator's output for the pinned file.
    add_executable(stxlib_case_folding_tests tests/case_folding_tests.cpp)
    target_link_libraries(stxlib_case_folding_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_case_folding_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_case_folding_tests PRIVATE ${stxlib_test_warnings})
    # Under Node with the js file system the test reads the host's file
    # through NODERAWFS (src/settings.js: Node's file system in place of the
    # in-memory one). WasmFS does its file calls in wasm, with a memory
    # backend at the root (system/lib/wasmfs/wasmfs.cpp, initRootDirectory,
    # at emscripten 6.0.13), and the first WasmFS run could not open the
    # host's file with NODERAWFS set; so there the file is embedded at link
    # time (--embed-file, served by libwasmfs.js's FS.createDataFile) and
    # read at /stxlib_test_data/ (stxlib_web_test_data below).
    set(stxlib_web_embeds OFF)
    if(stxlib_web_tests AND STXLIB_EMSCRIPTEN_FILESYSTEM STREQUAL "wasmfs")
        set(stxlib_web_embeds ON)
    endif()
    set(stxlib_case_folding_data ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/CaseFolding.txt)
    set(stxlib_case_folding_argument ${stxlib_case_folding_data})
    if(stxlib_web_embeds)
        set(stxlib_case_folding_argument /stxlib_test_data/CaseFolding.txt)
        target_link_options(stxlib_case_folding_tests PRIVATE
            "SHELL:--embed-file ${stxlib_case_folding_data}@/stxlib_test_data/CaseFolding.txt")
        set_property(TARGET stxlib_case_folding_tests APPEND PROPERTY LINK_DEPENDS ${stxlib_case_folding_data})
    elseif(stxlib_web_tests)
        target_link_options(stxlib_case_folding_tests PRIVATE "SHELL:-s NODERAWFS=1")
    endif()
    add_test(NAME stxlib_case_folding_tests COMMAND stxlib_case_folding_tests ${stxlib_case_folding_argument})
    set_tests_properties(stxlib_case_folding_tests PROPERTIES TIMEOUT 600)
    # Text layout analysis (include/stxlib/textlayout.hpp). Two programs:
    # the bidirectional algorithm (the lookups against the Unicode files,
    # BidiTest.txt and BidiCharacterTest.txt, and the exact comparison with
    # the reference, tests/reference/bidi_reference.hpp), and line breaking,
    # scripts and the one walk (tests/textlayout_tests.cpp: the tables
    # against the Unicode files, the automaton against the rule chain over
    # every reachable state, LineBreakTest.txt, a bounded fuzz). The
    # conformance files are off the repository: STXLIB_UNICODE_TEST_DATA
    # names their directory, and each test says when one is absent. Not for
    # the web targets (the files are read from the host).
    set(STXLIB_UNICODE_TEST_DATA "" CACHE PATH
        "Directory of BidiTest.txt, BidiCharacterTest.txt and LineBreakTest.txt (Unicode 18.0.0)")
    if(NOT stxlib_web_tests)
        add_executable(stxlib_textlayout_bidi_tests tests/textlayout_bidi_tests.cpp)
        target_link_libraries(stxlib_textlayout_bidi_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_textlayout_bidi_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_options(stxlib_textlayout_bidi_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_textlayout_bidi_tests
            COMMAND stxlib_textlayout_bidi_tests ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0 ${STXLIB_UNICODE_TEST_DATA})
        # Slow unoptimized: about 7.5 million bracket sequences to length 6.
        # Windows Debug took 564 s on one CI run and over 1200 s on a runner
        # twice as slow (run 45008874456), Linux Debug 1027 s, so the limit
        # leaves room for a slow runner.
        set_tests_properties(stxlib_textlayout_bidi_tests PROPERTIES TIMEOUT 2700)
        add_executable(stxlib_textlayout_tests tests/textlayout_tests.cpp)
        target_link_libraries(stxlib_textlayout_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_textlayout_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                   ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_textlayout_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_textlayout_tests
            COMMAND stxlib_textlayout_tests ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0 ${STXLIB_UNICODE_TEST_DATA})
        set_tests_properties(stxlib_textlayout_tests PROPERTIES TIMEOUT 1200)
        # What the two objects reach outside themselves
        # (tools/check_textlayout_surface.py; proofs/REPORT.md).
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND AND CMAKE_NM AND NOT MSVC AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
            set(stxlib_textlayout_sanitized)
            if(stxlib_sanitized)
                set(stxlib_textlayout_sanitized --sanitized)
            endif()
            add_test(NAME stxlib_textlayout_surface
                COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_textlayout_surface.py
                        --nm ${CMAKE_NM} --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_algorithms.dir
                        ${stxlib_textlayout_sanitized})
        endif()
        # Speed against SheenBidi and libunibreak and the differences from
        # them: only with both sources named (stx_text's copies); never
        # installed.
        set(STXLIB_SHEENBIDI_DIR "" CACHE PATH "SheenBidi source (Headers/, Source/) for stxlib_textlayout_bench")
        set(STXLIB_LIBUNIBREAK_DIR "" CACHE PATH "libunibreak source (src/) for stxlib_textlayout_bench")
        if(STXLIB_SHEENBIDI_DIR AND STXLIB_LIBUNIBREAK_DIR)
            enable_language(C)
            file(GLOB stxlib_sheenbidi_sources ${STXLIB_SHEENBIDI_DIR}/Source/*.c)
            list(FILTER stxlib_sheenbidi_sources EXCLUDE REGEX "SheenBidi\\.c$")
            add_library(stxlib_sheenbidi_oracle STATIC ${stxlib_sheenbidi_sources})
            target_include_directories(stxlib_sheenbidi_oracle PUBLIC ${STXLIB_SHEENBIDI_DIR}/Headers
                                       PRIVATE ${STXLIB_SHEENBIDI_DIR}/Source)
            add_library(stxlib_libunibreak_oracle STATIC
                ${STXLIB_LIBUNIBREAK_DIR}/src/unibreakbase.c
                ${STXLIB_LIBUNIBREAK_DIR}/src/unibreakdef.c
                ${STXLIB_LIBUNIBREAK_DIR}/src/linebreak.c
                ${STXLIB_LIBUNIBREAK_DIR}/src/linebreakdata.c
                ${STXLIB_LIBUNIBREAK_DIR}/src/linebreakdef.c
                ${STXLIB_LIBUNIBREAK_DIR}/src/eastasianwidthdef.c)
            target_include_directories(stxlib_libunibreak_oracle PUBLIC ${STXLIB_LIBUNIBREAK_DIR}/src)
            add_executable(stxlib_textlayout_bench tests/textlayout_bench.cpp)
            target_link_libraries(stxlib_textlayout_bench PRIVATE stxlib::algorithms stxlib_sheenbidi_oracle
                                  stxlib_libunibreak_oracle)
        endif()
    endif()
    # stxlib::markdown: the CommonMark specification's examples, the tape's
    # form, and the generated tables against the Unicode file, read by the
    # test's own parser (tests/markdown_tests.cpp); and the committed tables
    # are the generator's output for the pinned inputs. Not built for the
    # web tests yet (two data files to embed).
    #
    # The specification's text (CC-BY-SA 4.0, attributed in
    # third_party/commonmark-0.31.2/LICENSE; test data only) is in the tree
    # by the owner's decision of 2026-11-15, so the test runs it everywhere.
    # STXLIB_COMMONMARK_SPEC can name another copy; when no file is there the
    # test runs everything but the specification's examples, and says so.
    set(STXLIB_COMMONMARK_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/commonmark-0.31.2/spec.txt"
        CACHE FILEPATH "The CommonMark 0.31.2 spec.txt for stxlib_markdown_tests")
    set(stxlib_commonmark_argument -)
    if(EXISTS "${STXLIB_COMMONMARK_SPEC}")
        set(stxlib_commonmark_argument "${STXLIB_COMMONMARK_SPEC}")
    else()
        message(STATUS "STXlib: no CommonMark specification at ${STXLIB_COMMONMARK_SPEC}; "
                       "stxlib_markdown_tests runs without its examples")
    endif()
    if(NOT stxlib_web_tests)
        add_executable(stxlib_markdown_tests tests/markdown_tests.cpp)
        target_link_libraries(stxlib_markdown_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_markdown_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_markdown_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_markdown_tests
            COMMAND stxlib_markdown_tests
                    ${stxlib_commonmark_argument}
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/UnicodeData.txt)
        set_tests_properties(stxlib_markdown_tests PROPERTIES TIMEOUT 600)
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME stxlib_markdown_tables
                COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_markdown_tables.py --check)
        endif()
    endif()
    # calendar_tests' reference is std::chrono's year_month_day, whose libc++
    # civil-from-days adds a negative int to an unsigned month by design
    # (__chrono/year_month_day.h:88, LLVM 22.1.8: -9 converted to unsigned,
    # then 10 + 4294967287 wrapping to 1). The sign-change and unsigned-wrap
    # checks are off for the test's translation unit only; calendar.cpp in
    # the library keeps every integer check.
    if(STXLIB_INTEGER_CHECKS)
        target_compile_options(stxlib_calendar_tests PRIVATE
            -fno-sanitize=implicit-integer-sign-change,unsigned-integer-overflow)
    endif()

    # math_tests checks frozen output hashes, so a flag that changes any bit
    # fails it. The Dekker build compiles its own copies of the kernels with
    # the no-FMA exact product forced and must give the same bits: the
    # cross-architecture determinism evidence on an FMA machine. Its kernel
    # objects define the library's math symbols, so the linker never pulls
    # the library's own math objects into it. `--exhaustive` and `--bench`
    # stay out of ctest. From stx_auxlib b6e330a.
    if(STXLIB_EXCEPTIONS)
        add_executable(stxlib_math_tests tests/math_tests.cpp)
        add_executable(stxlib_math_dekker_tests tests/math_tests.cpp ${STXLIB_MATH_SOURCES})
        target_compile_definitions(stxlib_math_dekker_tests PRIVATE STXLIB_MATH_DEKKER STXLIB_MATH_VARIANT=native)
        foreach(math_test IN ITEMS stxlib_math_tests stxlib_math_dekker_tests)
            target_link_libraries(${math_test} PRIVATE stxlib::algorithms)
            target_include_directories(${math_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
            # The test's inlined double-double reference (tests/reference/wide.hpp)
            # needs contraction off too: contracted, its own self-check fails.
            target_compile_options(${math_test} PRIVATE ${stxlib_frame_pointers} ${stxlib_math_options})
            add_test(NAME ${math_test} COMMAND ${math_test})
        endforeach()
        # exp2, expm1, log1p, sin and cos (roadmap R1): accuracy against the
        # references, the float screen (with `--exhaustive`, every float: with
        # tools/math/r1_certificate.py the proof of correct rounding) and the
        # fronts' identity with sincos; the Dekker target as for the math tests.
        add_executable(stxlib_math_completion_tests tests/math_completion_tests.cpp)
        add_executable(stxlib_math_completion_dekker_tests tests/math_completion_tests.cpp ${STXLIB_MATH_SOURCES})
        target_compile_definitions(stxlib_math_completion_dekker_tests PRIVATE STXLIB_MATH_DEKKER STXLIB_MATH_VARIANT=native)
        foreach(math_test IN ITEMS stxlib_math_completion_tests stxlib_math_completion_dekker_tests)
            target_link_libraries(${math_test} PRIVATE stxlib::algorithms)
            target_include_directories(${math_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_options(${math_test} PRIVATE ${stxlib_frame_pointers} ${stxlib_math_options})
            add_test(NAME ${math_test} COMMAND ${math_test})
        endforeach()
        # Both paths of the switched kernels (src/algorithms/math/paths.hpp):
        # region.cpp and trig_region.cpp are library kernel sources; the test
        # calls each path by name in the library's variant (v128 where the
        # build dispatches, native otherwise) and, in the Dekker target, in its
        # own copies with Dekker's product; both must print the same hash.
        # `--exhaustive` (every float through the float logarithms) and
        # `--bench` (each path against the other) stay out of ctest.
        add_executable(stxlib_math_region_tests tests/math_region_tests.cpp)
        target_compile_definitions(stxlib_math_region_tests PRIVATE
            STXLIB_MATH_VARIANT=$<IF:$<BOOL:${STXLIB_MATH_DISPATCH}>,v128,native>)
        add_executable(stxlib_math_region_dekker_tests tests/math_region_tests.cpp ${STXLIB_MATH_SOURCES})
        target_compile_definitions(stxlib_math_region_dekker_tests PRIVATE STXLIB_MATH_DEKKER STXLIB_MATH_VARIANT=native)
        foreach(math_test IN ITEMS stxlib_math_region_tests stxlib_math_region_dekker_tests)
            target_link_libraries(${math_test} PRIVATE stxlib::algorithms)
            target_include_directories(${math_test} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_options(${math_test} PRIVATE ${stxlib_frame_pointers} ${stxlib_math_options})
            add_test(NAME ${math_test} COMMAND ${math_test})
        endforeach()
        # Every variant on this machine: the forced levels run the same frozen
        # hashes through v128 and, where the CPU has it, avx2_fma.
        if(STXLIB_MATH_DISPATCH)
            foreach(level IN ITEMS scalar vector128 avx2_fma)
                add_test(NAME stxlib_math_tests_${level} COMMAND stxlib_math_tests)
                set_tests_properties(stxlib_math_tests_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
            endforeach()
        endif()
    endif()

    # The sequence tree and RowHeightIndex (src/algorithms/stx_btree/
    # ORDER_STATISTIC_RESEARCH.md §8): the proved properties as tests, and
    # the exhaustive small-scope check at its small scope against the
    # private templates and through the facade (`--scope full` by hand).
    # From stx_auxlib b6e330a.
    foreach(sequence_test IN ITEMS sequence_tree sequence_tree_exhaustive)
        add_executable(stxlib_${sequence_test}_tests tests/stx_btree/${sequence_test}_test.cpp)
        target_link_libraries(stxlib_${sequence_test}_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_${sequence_test}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_${sequence_test}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${sequence_test}_tests COMMAND stxlib_${sequence_test}_tests)
        set_tests_properties(stxlib_${sequence_test}_tests PROPERTIES TIMEOUT 1200)
    endforeach()
    # The fixed-order reductions (src/core/float_reduce.hpp, F5 = 1): the
    # shape by hand and against a restatement, and the pool forms against
    # the serial chunked forms at 1, 2, 3 and 8 threads.
    add_executable(stxlib_float_reduce_tests tests/float_reduce_tests.cpp)
    target_link_libraries(stxlib_float_reduce_tests PRIVATE stxlib::boundary)
    target_include_directories(stxlib_float_reduce_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_float_reduce_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_float_reduce_tests COMMAND stxlib_float_reduce_tests)
    set_tests_properties(stxlib_float_reduce_tests PROPERTIES TIMEOUT 600)

    # The cross-host bit identity (tests/bit_identity.cpp): every module with
    # promised floating-point results hashes its outputs over fixed inputs.
    # The Dekker build compiles its own math kernels with the no-FMA product
    # (as stxlib_math_dekker_tests does) and float_exact's Dekker form, and
    # must print the same lines. Run through tools/compare_bit_identity.py
    # against tests/bit_identity_expected.txt below.
    add_executable(stxlib_bit_identity tests/bit_identity.cpp)
    add_executable(stxlib_bit_identity_dekker tests/bit_identity.cpp ${STXLIB_MATH_SOURCES})
    target_compile_definitions(stxlib_bit_identity_dekker PRIVATE
        STXLIB_MATH_DEKKER STXLIB_MATH_VARIANT=native STXLIB_FLOAT_EXACT_DEKKER)
    foreach(identity IN ITEMS stxlib_bit_identity stxlib_bit_identity_dekker)
        target_link_libraries(${identity} PRIVATE stxlib::algorithms)
        target_include_directories(${identity} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(${identity} PRIVATE ${stxlib_test_warnings} ${stxlib_math_options})
    endforeach()

    # The exact floating-point primitives (src/core/float_exact.hpp) against
    # an exact integer reference, with the range checks on; `--exhaustive`
    # by hand (every float for the split, a binade against partners for the
    # float sum and product).
    add_executable(stxlib_float_exact_tests tests/float_exact_tests.cpp)
    target_link_libraries(stxlib_float_exact_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_float_exact_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(stxlib_float_exact_tests PRIVATE STXLIB_FLOAT_CHECKS)
    target_compile_options(stxlib_float_exact_tests PRIVATE ${stxlib_test_warnings} ${stxlib_math_options})
    add_test(NAME stxlib_float_exact_tests COMMAND stxlib_float_exact_tests)
    set_tests_properties(stxlib_float_exact_tests PROPERTIES TIMEOUT 600)

    # The unsync ordered map (src/algorithms/stx_btree/unsync_map.hpp)
    # against std::map with every structural invariant checked after each
    # phase, on both page allocators, plus refused insertions. From
    # stx_auxlib b6e330a.
    add_executable(stxlib_unsync_map_tests tests/stx_btree/unsync_map_test.cpp)
    target_link_libraries(stxlib_unsync_map_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_unsync_map_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_unsync_map_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_unsync_map_tests COMMAND stxlib_unsync_map_tests)
    set_tests_properties(stxlib_unsync_map_tests PROPERTIES TIMEOUT 600)
    # The shared ordered map (src/algorithms/stx_btree/bbtree_map.hpp): the
    # single-thread differential with the structural check, the threaded
    # runs, refused insertions, and the timed stress run (2 seconds a
    # shape, as stx_btree's own build passes it). From stx_auxlib b6e330a,
    # with its ThreadSanitizer handling: no inlining under TSan, so an
    # optimistic reader keeps its frame, and the suppressions name those
    # readers only (tests/stx_btree/tsan_btree_suppressions.txt).
    foreach(shared_test IN ITEMS bbtree_map stress)
        add_executable(stxlib_btree_${shared_test}_tests tests/stx_btree/${shared_test}_test.cpp)
        target_link_libraries(stxlib_btree_${shared_test}_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_btree_${shared_test}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_btree_${shared_test}_tests PRIVATE ${stxlib_test_warnings})
        if("${CMAKE_CXX_FLAGS}" MATCHES "-fsanitize=[^ ]*thread")
            target_compile_options(stxlib_btree_${shared_test}_tests PRIVATE -fno-inline)
        endif()
        set(shared_arguments)
        if(shared_test STREQUAL "stress")
            set(shared_arguments 2)
        endif()
        add_test(NAME stxlib_btree_${shared_test}_tests COMMAND stxlib_btree_${shared_test}_tests ${shared_arguments})
        set_tests_properties(stxlib_btree_${shared_test}_tests PROPERTIES TIMEOUT 600 ENVIRONMENT
            "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/stx_btree/tsan_btree_suppressions.txt:halt_on_error=1")
    endforeach()
    # The optimal binary search trees (src/algorithms/stx_btree/
    # optimal_tree.hpp) against the cubic recurrence and, for n <= 8, every
    # tree enumerated; their maps onto multiway nodes (optimal_map.hpp). From
    # stx_auxlib b6e330a. Exact costs are signed __int128 arithmetic, so
    # GCC or Clang only, as STXlib is built.
    foreach(optimal_test IN ITEMS optimal_tree optimal_map)
        add_executable(stxlib_btree_${optimal_test}_tests tests/stx_btree/${optimal_test}_test.cpp)
        target_link_libraries(stxlib_btree_${optimal_test}_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_btree_${optimal_test}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_btree_${optimal_test}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_btree_${optimal_test}_tests COMMAND stxlib_btree_${optimal_test}_tests)
        set_tests_properties(stxlib_btree_${optimal_test}_tests PROPERTIES TIMEOUT 600)
    endforeach()
    # The facade against the template it wraps (offset to row, row to
    # offset, set_heights), for the integrator to run by hand; not a ctest.
    add_executable(stxlib_row_height_bench tests/stx_btree/row_height_bench.cpp)
    target_link_libraries(stxlib_row_height_bench PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_row_height_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_row_height_bench PRIVATE ${stxlib_test_warnings})

    # Preference Ranked Voting against the reference's own outcomes
    # (tests/reference/prv_cases.txt, made by prv_reference.mjs).
    add_executable(stxlib_voting_tests tests/voting_tests.cpp)
    target_link_libraries(stxlib_voting_tests PRIVATE stxlib::algorithms)
    target_compile_options(stxlib_voting_tests PRIVATE ${stxlib_test_warnings})
    # On the web as the case folding test reads its data: NODERAWFS with the
    # js file system, the file embedded with WasmFS.
    set(stxlib_voting_data ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/prv_cases.txt)
    set(stxlib_voting_argument ${stxlib_voting_data})
    if(stxlib_web_embeds)
        set(stxlib_voting_argument /stxlib_test_data/prv_cases.txt)
        target_link_options(stxlib_voting_tests PRIVATE
            "SHELL:--embed-file ${stxlib_voting_data}@/stxlib_test_data/prv_cases.txt")
        set_property(TARGET stxlib_voting_tests APPEND PROPERTY LINK_DEPENDS ${stxlib_voting_data})
    elseif(stxlib_web_tests)
        target_link_options(stxlib_voting_tests PRIVATE "SHELL:-s NODERAWFS=1")
    endif()
    add_test(NAME stxlib_voting_tests COMMAND stxlib_voting_tests ${stxlib_voting_argument})

    # stx_fft (include/stxlib/fft.hpp): the three kernels against a direct
    # DFT and the direct half-bin sum, round trip, Parseval, every tier
    # against the scalar tier in one process, refusals and the subnormal
    # rule (tests/fft_tests.cpp); once at the machine's level and once per
    # forced level, as the math tests are.
    add_executable(stxlib_fft_tests tests/fft_tests.cpp)
    target_link_libraries(stxlib_fft_tests PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_fft_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_fft_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_fft_tests COMMAND stxlib_fft_tests)
    foreach(level IN ITEMS scalar vector128 avx2_fma)
        add_test(NAME stxlib_fft_tests_${level} COMMAND stxlib_fft_tests)
        set_tests_properties(stxlib_fft_tests_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
    endforeach()
    # The PSFT, the plain mode's kernel from N = 16 (tests/fft_walk_tests.cpp):
    # every size to 2^20 against a long-double DFT, round trip and magnitude,
    # the frozen hashes of its exact and fused builds at every walk tier this
    # machine runs, layout invariance, eight threads on one plan, and the
    # const calls' refusals; once per forced level for make()'s choices.
    add_executable(stxlib_fft_walk_tests tests/fft_walk_tests.cpp)
    target_link_libraries(stxlib_fft_walk_tests PRIVATE stxlib::algorithms Threads::Threads)
    target_include_directories(stxlib_fft_walk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_fft_walk_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_fft_walk_tests COMMAND stxlib_fft_walk_tests)
    foreach(level IN ITEMS scalar vector128)
        add_test(NAME stxlib_fft_walk_tests_${level} COMMAND stxlib_fft_walk_tests)
        set_tests_properties(stxlib_fft_walk_tests_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
    endforeach()
    # The benchmark beside BFFT's (tests/fft_bench.cpp), for the integrator
    # to run by hand; not a ctest.
    # tests/fft_fused_kernels.cpp is the fused measuring variant of the
    # kernels (variant.inc with STXLIB_FFT_FUSED), compiled into the
    # benchmark only, never into the library.
    add_executable(stxlib_fft_bench tests/fft_bench.cpp tests/fft_fused_kernels.cpp)
    target_link_libraries(stxlib_fft_bench PRIVATE stxlib::algorithms)
    target_include_directories(stxlib_fft_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_fft_bench PRIVATE ${stxlib_test_warnings})
    # FFTW 3 (double and float) beside the PSFT in the benchmark's walk table,
    # when a prefix holding include/fftw3.h and lib/libfftw3{,f} is named.
    set(STXLIB_FFT_BENCH_FFTW_DIR "" CACHE PATH "FFTW 3 prefix for stxlib_fft_bench's walk table")
    if(STXLIB_FFT_BENCH_FFTW_DIR)
        target_compile_definitions(stxlib_fft_bench PRIVATE STXLIB_FFT_BENCH_FFTW=1)
        target_include_directories(stxlib_fft_bench SYSTEM PRIVATE ${STXLIB_FFT_BENCH_FFTW_DIR}/include)
        target_link_libraries(stxlib_fft_bench PRIVATE ${STXLIB_FFT_BENCH_FFTW_DIR}/lib/libfftw3.a
                              ${STXLIB_FFT_BENCH_FFTW_DIR}/lib/libfftw3f.a)
    endif()
    # BFFT itself as the oracle (tests/fft_bfft_oracle_tests.cpp) and in the
    # benchmark's table, when a BFFT checkout is named. Its kernel headers
    # are compiled from there unchanged, as system headers.
    set(STXLIB_BFFT_ORACLE_DIR "" CACHE PATH "BFFT checkout (src/detail/) for stxlib_fft_bfft_oracle_tests and stxlib_fft_bench")
    if(STXLIB_BFFT_ORACLE_DIR)
        add_executable(stxlib_fft_bfft_oracle_tests tests/fft_bfft_oracle_tests.cpp)
        target_link_libraries(stxlib_fft_bfft_oracle_tests PRIVATE stxlib::algorithms)
        target_include_directories(stxlib_fft_bfft_oracle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_include_directories(stxlib_fft_bfft_oracle_tests SYSTEM PRIVATE
            ${STXLIB_BFFT_ORACLE_DIR}/src ${STXLIB_BFFT_ORACLE_DIR}/include)
        target_compile_options(stxlib_fft_bfft_oracle_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_fft_bfft_oracle_tests COMMAND stxlib_fft_bfft_oracle_tests)
        target_compile_definitions(stxlib_fft_bench PRIVATE STXLIB_FFT_BENCH_BFFT=1)
        target_include_directories(stxlib_fft_bench SYSTEM PRIVATE
            ${STXLIB_BFFT_ORACLE_DIR}/src ${STXLIB_BFFT_ORACLE_DIR}/include)
    endif()

    # The thread pool against the C original it ports (tests/reference/,
    # PROVENANCE.md): one conformance source, written against the
    # reference's C interface, checks STXlib's AtomicPool (through
    # tests/reference/atomic_pool_adapter.cpp) and the reference itself; both
    # must pass. From stx_auxlib b6e330a.
    if(STXLIB_EXCEPTIONS)
        enable_language(C)
        add_executable(stxlib_thread_pool_conformance
            tests/thread_pool_conformance.cpp tests/reference/atomic_pool_adapter.cpp)
        target_link_libraries(stxlib_thread_pool_conformance PRIVATE stxlib::boundary)
        add_executable(stxlib_thread_pool_reference_conformance
            tests/thread_pool_conformance.cpp tests/reference/atomic_pool.c)
        foreach(conformance IN ITEMS stxlib_thread_pool_conformance stxlib_thread_pool_reference_conformance)
            # core/checked.hpp (the modular marker) and the reference's header.
            target_include_directories(${conformance} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference)
            target_compile_features(${conformance} PRIVATE cxx_std_20)
            # The reference calls pthreads directly; CLANG64's libc++ uses native
            # Windows threads and supplies no implicit pthread dependency.
            if(MINGW)
                target_link_libraries(${conformance} PRIVATE winpthread)
            else()
                target_link_libraries(${conformance} PRIVATE Threads::Threads)
            endif()
            add_test(NAME ${conformance} COMMAND ${conformance})
            # A hang detector: a broken claim or lock deadlocks rather than
            # fails. RUN_SERIAL: bound by scheduler latency beside other tests
            # (stx_auxlib's measurement: 88 ms alone, 15 s contended).
            set_tests_properties(${conformance} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endforeach()
    endif()

    # Two modules, each with its own copy of stxlib::boundary, release each
    # other's blocks through the one allocator in stxlib_state. Not on
    # Emscripten, where a program is one module.
    if(NOT stxlib_web_tests)
        add_library(stxlib_allocator_two_module_library SHARED tests/allocator_two_module_library.cpp)
        target_link_libraries(stxlib_allocator_two_module_library PRIVATE stxlib::boundary)
        target_compile_definitions(stxlib_allocator_two_module_library PRIVATE STXLIB_ALLOCATOR_TWO_MODULE_BUILDING)
        target_compile_options(stxlib_allocator_two_module_library PRIVATE ${stxlib_test_warnings})
        set_target_properties(stxlib_allocator_two_module_library PROPERTIES CXX_VISIBILITY_PRESET hidden)
        add_executable(stxlib_allocator_two_module_tests tests/allocator_two_module_tests.cpp)
        target_link_libraries(stxlib_allocator_two_module_tests PRIVATE stxlib::boundary
                              stxlib_allocator_two_module_library)
        target_compile_options(stxlib_allocator_two_module_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_allocator_two_module_tests COMMAND stxlib_allocator_two_module_tests)

        # A module's unload (tests/allocator_unload_tests.cpp): a library
        # holding its own static stxlib_state is dlopened, allocates, and is
        # dlclosed; under LSan a registry table left behind fails the test.
        # Linux with a static state only (a shared one is linked
        # -z nodelete, so its table stays reachable); CI's linux-glibc job
        # builds it so.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT STXLIB_STATE_SHARED)
            add_library(stxlib_allocator_unload_library MODULE tests/allocator_unload_library.cpp)
            target_link_libraries(stxlib_allocator_unload_library PRIVATE stxlib::boundary)
            target_compile_options(stxlib_allocator_unload_library PRIVATE ${stxlib_test_warnings})
            set_target_properties(stxlib_allocator_unload_library PROPERTIES CXX_VISIBILITY_PRESET hidden)
            add_executable(stxlib_allocator_unload_tests tests/allocator_unload_tests.cpp)
            target_link_libraries(stxlib_allocator_unload_tests PRIVATE ${CMAKE_DL_LIBS})
            target_compile_options(stxlib_allocator_unload_tests PRIVATE ${stxlib_test_warnings})
            add_dependencies(stxlib_allocator_unload_tests stxlib_allocator_unload_library)
            add_test(NAME stxlib_allocator_unload_tests COMMAND stxlib_allocator_unload_tests
                     $<TARGET_FILE:stxlib_allocator_unload_library>)
        endif()

        # The allocator at process end (the logic review's L-05): a library
        # whose threads allocate until the host stops them, and whose
        # detach-time destructor then allocates on every locking path
        # (tests/allocator_exit_tests.cpp). Sampled over 20 child processes;
        # the first hang ends the run, and the timeout catches any other
        # stall, so a hang fails rather than holding up the suite.
        add_library(stxlib_allocator_exit_library SHARED tests/allocator_exit_library.cpp)
        target_link_libraries(stxlib_allocator_exit_library PRIVATE stxlib::boundary Threads::Threads)
        target_compile_definitions(stxlib_allocator_exit_library PRIVATE STXLIB_ALLOCATOR_EXIT_BUILDING)
        target_compile_options(stxlib_allocator_exit_library PRIVATE ${stxlib_test_warnings})
        set_target_properties(stxlib_allocator_exit_library PROPERTIES CXX_VISIBILITY_PRESET hidden)
        add_executable(stxlib_allocator_exit_tests tests/allocator_exit_tests.cpp)
        target_link_libraries(stxlib_allocator_exit_tests PRIVATE stxlib::boundary stxlib_allocator_exit_library)
        target_compile_options(stxlib_allocator_exit_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_allocator_exit_tests COMMAND stxlib_allocator_exit_tests)
        set_tests_properties(stxlib_allocator_exit_tests PROPERTIES TIMEOUT 300)
    endif()

    # The gates compiled without exceptions: direct calls (A1 design, 6).
    add_executable(stxlib_gate_no_exceptions_tests tests/gate_no_exceptions_tests.cpp)
    target_link_libraries(stxlib_gate_no_exceptions_tests PRIVATE stxlib::boundary)
    target_compile_options(stxlib_gate_no_exceptions_tests PRIVATE ${stxlib_test_warnings} -fno-exceptions)
    add_test(NAME stxlib_gate_no_exceptions_tests COMMAND stxlib_gate_no_exceptions_tests)

    # Must-end cases, one process each.
    if(STXLIB_EXCEPTIONS)
        add_executable(stxlib_death_tests tests/death_tests.cpp)
        target_link_libraries(stxlib_death_tests PRIVATE stxlib::boundary Threads::Threads)
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
            "recursive_foreign_unlock|stxlib: fail fast: stxlib\\.threads\\.lock: internal_failure.*RecursiveMutex it does not hold"
            "recursive_destroyed_held|stxlib: fail fast: stxlib\\.threads\\.lock: internal_failure.*RecursiveMutex was destroyed while held"
            "unique_unlock_unheld|stxlib: requirement failed: UniqueLock::unlock while it does not hold its mutex"
            "unique_lock_twice|stxlib: requirement failed: UniqueLock::lock while it holds its mutex"
            "unique_wait_unheld|stxlib: requirement failed: ConditionVariable: a wait needs a UniqueLock that holds its mutex"
            "multi_lock_same_mutex|stxlib: requirement failed: MultiLock: one Mutex named twice"
            "atomic_load_order|stxlib: requirement failed: AtomicWord::load: not a load order"
            "stop_callback_null|stxlib: requirement failed: StopCallback: a callback needs a function"
            "view_index|stxlib: requirement failed: View: index out of range"
            "take_failed_value|stxlib: requirement failed: OperationResult::take_value on a failure"
            "vector_index|stxlib: requirement failed: Vector: index out of range"
            "optional_take|stxlib: requirement failed: Optional::take on an absent value"
            "shared_dereference|stxlib: requirement failed: Shared: dereference of an empty owner"
            "take_or_fail_fast|stxlib: fail fast: stxlib\\.tests\\.take: not_found.*no such row"
            "check_or_fail_fast|stxlib: fail fast: stxlib\\.tests\\.check: limit_exceeded.*too many rows"
            "callback_frames|stxlib: frames \\(throw_site\\): stxlib\\.tests\\.outer\\[7,8\\] > stxlib\\.tests\\.callback\\[0,0\\] > stxlib\\.tests\\.inner\\[9,0\\]"
            "callback_frame_texts|stxlib: fail fast: stxlib\\.tests\\.event: invalid_argument.*stxlib: frames \\(throw_site\\): 3:button\\.ok:click\\[0,0\\]#12 > stxlib\\.tests\\.inner\\[9,0\\]"
            "reporter_contract|reporter: console.*reporter: classified 1, message 300, frames 2, kind 3, type (named|none), console ok"
            "reporter_opens_frame|stxlib: requirement failed: a fail-fast reporter opened a shadow frame"
            "reporter_nested|reporter entered.*stxlib: fail fast: stxlib\\.tests\\.callback: internal_failure"
            "foreign_free|ANY"
            "foreign_delete|ANY"
            "gate_cancelled|stxlib: fail fast: stxlib\\.tests\\.cancel: interrupted.*thread cancellation or pthread_exit"
            "pool_caller_reentry|stxlib: fail fast: stxlib\\.thread_pool\\.run: invalid_argument.*holds the pool")
        # Emscripten's malloc is not known to check a foreign free (dlmalloc,
        # src/settings.js MALLOC; its checks were not read at the pin): the
        # two cases that end in the host's own check are not run there.
        if(stxlib_web_tests)
            list(REMOVE_ITEM stxlib_death_cases "foreign_free|ANY" "foreign_delete|ANY")
        endif()
        foreach(death_case IN LISTS stxlib_death_cases)
            string(FIND "${death_case}" "|" separator)
            string(SUBSTRING "${death_case}" 0 ${separator} case_name)
            math(EXPR expect_start "${separator} + 1")
            string(SUBSTRING "${death_case}" ${expect_start} -1 case_expect)
            add_test(NAME stxlib_death_${case_name}
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:stxlib_death_tests>
                    "-DEMULATOR=${stxlib_emulator_text}"
                    -DCASE=${case_name}
                    "-DEXPECT=${case_expect}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/death_check.cmake)
        endforeach()
    endif()

    # The stack and address calls inside a fail-fast reporter with allocation
    # refused (tests/reporter_stack_tests.cpp; fail_fast.hpp's contract, A2
    # P0). Its own program: it replaces the global operator new and delete,
    # and on glibc malloc, calloc, realloc and free.
    add_executable(stxlib_reporter_stack_tests tests/reporter_stack_tests.cpp)
    target_link_libraries(stxlib_reporter_stack_tests PRIVATE stxlib::boundary)
    target_compile_options(stxlib_reporter_stack_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_death_reporter_stack_unallocated
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:stxlib_reporter_stack_tests>
            "-DEMULATOR=${stxlib_emulator_text}"
            -DCASE=reporter_stack_unallocated
            "-DEXPECT=reporter: stack ok, captured [0-9]+, described (yes|no|unsupported), refused "
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/death_check.cmake)

    # Sealed pages, one process each (tests/platform_rest_death_tests.cpp),
    # and the console and exit tasks seen from outside the process
    # (a child process): neither exists on Emscripten.
    if(NOT stxlib_web_tests)
        add_executable(stxlib_platform_rest_death_tests tests/platform_rest_death_tests.cpp)
        target_link_libraries(stxlib_platform_rest_death_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_platform_rest_death_tests PRIVATE ${stxlib_test_warnings})
        set(stxlib_platform_rest_death_cases
            "sealed_writable|stxlib: requirement failed: PageBlock::writable: the pages are sealed"
            "sealed_store|ANY")
        foreach(death_case IN LISTS stxlib_platform_rest_death_cases)
            string(FIND "${death_case}" "|" separator)
            string(SUBSTRING "${death_case}" 0 ${separator} case_name)
            math(EXPR expect_start "${separator} + 1")
            string(SUBSTRING "${death_case}" ${expect_start} -1 case_expect)
            add_test(NAME stxlib_platform_rest_death_${case_name}
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:stxlib_platform_rest_death_tests>
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
    endif()

    # Two modules, one state (tests/two_module_tests.cpp): a shared library
    # with its own hidden copy of the boundary, and an executable with
    # another; an exception thrown in the executable unwinds a frame opened
    # in the library.
    if(STXLIB_STATE_SHARED AND STXLIB_EXCEPTIONS)
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

    # The positive control of stxlib_no_exception_symbols (below): one
    # object compiled with exceptions on purpose, which must show references
    # to the exception runtime. Linked into nothing.
    add_library(stxlib_exception_probe OBJECT tests/exception_probe.cpp)
    target_compile_options(stxlib_exception_probe PRIVATE -fexceptions)

    # The positive control of stxlib_state_static_no_exports (below): one
    # object with a dllexport on purpose, which must carry an export
    # directive. Windows only; linked into nothing.
    if(WIN32 AND NOT STXLIB_STATE_SHARED)
        add_library(stxlib_export_probe OBJECT tests/export_probe.cpp)
        target_compile_options(stxlib_export_probe PRIVATE ${stxlib_test_warnings})
    endif()

    # Proof harness: the platform layer's pure cores (A1 platform-rest):
    # Windows command-line splitting and quoting, the stack line's bounds and
    # the PE reader on damaged headers, exhaustive over the domains
    # tests/platform_rest_exhaustive.cpp states (proofs/REPORT.md).
    add_executable(stxlib_platform_rest_exhaustive tests/platform_rest_exhaustive.cpp)
    target_link_libraries(stxlib_platform_rest_exhaustive PRIVATE stxlib::boundary)
    target_include_directories(stxlib_platform_rest_exhaustive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(stxlib_platform_rest_exhaustive PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_platform_rest_exhaustive COMMAND stxlib_platform_rest_exhaustive)
    set_tests_properties(stxlib_platform_rest_exhaustive PROPERTIES TIMEOUT 1200)

    # The public-header boundary (AGENTS.md): no std:: name, no angle include.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME stxlib_public_headers
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_public_headers.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The governance check (R4; audit B, section 3.16): the standard
        # library rules over STXlib's own sources, and the check's own tests.
        add_test(NAME stxlib_std_rules
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_std_rules.py
                    --profile stxlib --root ${CMAKE_CURRENT_SOURCE_DIR}
                    # The Windows C library headers declare the functions the
                    # rules forbid using; declaring them is their purpose.
                    --exclude src/hosted/windows/crt/*
                    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
        add_test(NAME stxlib_case_folding_table
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_case_folding.py --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/CaseFolding.txt
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/case_folding_table.inc
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/case_folding_full_table.inc)
        add_test(NAME stxlib_textlayout_tables
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_textlayout_tables.py --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/textlayout/textlayout_tables.inc)
        add_test(NAME stxlib_std_rules_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_std_rules_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # Its whole-text reading against the line-by-line reading it replaced.
        add_test(NAME stxlib_std_rules_reading
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_std_rules_reading_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The glyph surface check's own tests (no nm).
        add_test(NAME stxlib_glyphs_surface_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_glyphs_surface_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The outline-atomics link check's own tests (no nm).
        add_test(NAME stxlib_outline_atomics_link_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_outline_atomics_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The object surface check on the allocator's component file (every
        # family, no nm).
        add_test(NAME stxlib_stx_minmallocpp_surface_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_object_surface_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The behavioural constraint declarations (tools/ic/project.toml;
        # template_harness's tools/ic_declarations.py, kept identical here):
        # every generated output current and every generated check called,
        # then STXlib's own cases (the components IC1 covers, each
        # requirement through its generated check, each message spelled once).
        add_test(NAME stxlib_ic_declarations
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/ic_declarations.py check
                    --config ${CMAKE_CURRENT_SOURCE_DIR}/tools/ic/project.toml --root ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME stxlib_ic_declarations_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ic_declarations_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The deductions (IC4, tools/ic_deduce.py): current against the
        # declarations and the sources they read.
        add_test(NAME stxlib_ic_deductions
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/ic_deduce.py check
                    --config ${CMAKE_CURRENT_SOURCE_DIR}/tools/ic/project.toml --root ${CMAKE_CURRENT_SOURCE_DIR})
        # The cross-host bit identity: each run must print the frozen lines
        # (unfrozen items are reported, not failed, until the first freeze).
        # Under Emscripten the program runs under the emulator, named before it.
        set(stxlib_bit_identity_compare ${Python3_EXECUTABLE} -I
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_bit_identity.py
            --expected ${CMAKE_CURRENT_SOURCE_DIR}/tests/bit_identity_expected.txt --run
            ${CMAKE_CROSSCOMPILING_EMULATOR})
        add_test(NAME stxlib_bit_identity COMMAND ${stxlib_bit_identity_compare} $<TARGET_FILE:stxlib_bit_identity>)
        add_test(NAME stxlib_bit_identity_dekker
            COMMAND ${stxlib_bit_identity_compare} $<TARGET_FILE:stxlib_bit_identity_dekker>)
        if(STXLIB_MATH_DISPATCH)
            foreach(level IN ITEMS scalar vector128 avx2_fma)
                add_test(NAME stxlib_bit_identity_${level}
                    COMMAND ${stxlib_bit_identity_compare} $<TARGET_FILE:stxlib_bit_identity>)
                set_tests_properties(stxlib_bit_identity_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
            endforeach()
        endif()
        # The compiled zones (D12 a): the tables' reading agrees with the
        # tzdata source's full expansion at every change point, for every
        # name (tools/tzvectors.py; proofs/REPORT.md, batch 3b).
        add_test(NAME stxlib_zone_tables
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/tzvectors.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tzdata
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/zones/zone_data.inc)
        # No exception machinery in STXlib's objects (notes planning/stages/
        # A3-no-exceptions.md, section 1.5): every object of the library
        # targets is listed with the toolchain's nm, and a reference to the
        # exception runtime fails the test, except in the recorder and the
        # exception-type query while STXLIB_EXCEPTIONS is ON. The probe must
        # show such references, so the scan cannot pass by not seeing them.
        # The object directories are CMake's own (CMakeFiles/<target>.dir), as
        # the Ninja and Makefile generators lay them out.
        if(CMAKE_NM AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
            set(stxlib_exception_free_objects)
            foreach(library IN ITEMS stxlib_state stxlib_boundary stxlib_algorithms stxlib_math_avx2_fma)
                if(TARGET ${library})
                    list(APPEND stxlib_exception_free_objects
                        --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${library}.dir)
                endif()
            endforeach()
            set(stxlib_exception_allowed)
            if(STXLIB_EXCEPTIONS)
                set(stxlib_exception_allowed --allow exception_shadow.cpp --allow current_exception.cpp)
            endif()
            add_test(NAME stxlib_no_exception_symbols
                COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_exceptions.py
                        --nm ${CMAKE_NM}
                        --probe ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_exception_probe.dir
                        ${stxlib_exception_allowed} ${stxlib_exception_free_objects})
        else()
            message(STATUS "STXlib: stxlib_no_exception_symbols not registered (it needs nm and a Ninja or Makefile generator)")
        endif()
        # The export directive check's own tests (objects built in memory, every host).
        add_test(NAME stxlib_export_directives_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_export_directives_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # A static stxlib_state exports nothing from the module that links it
        # (Windows: no export directive in its objects' .drectve sections).
        # The probe must carry one, so the scan cannot pass by not seeing them.
        if(WIN32 AND NOT STXLIB_STATE_SHARED AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
            add_test(NAME stxlib_state_static_no_exports
                COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_export_directives.py
                        --probe ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_export_probe.dir
                        --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_state.dir)
        endif()
    endif()
endif()

# ---- the floating-point flag check ---------------------------------------------
# Rule 1 of the floating-point discipline, checked at configure time:
# -ffp-contract has no predefined macro, so this is the one guard against
# contraction (core/float_discipline.hpp stops a fast-math build itself).
# Every target defined in this file must carry -ffp-contract=off, and no
# target's options and no global flags may hold a flag that undoes the
# discipline (contraction, reassociation, fast-math, or a flush of
# subnormals, which STXlib does in software).
function(stxlib_check_float_flags)
    set(forbidden -ffast-math -Ofast -funsafe-math-optimizations -fassociative-math -freciprocal-math
        -ffinite-math-only -fno-signed-zeros -ffp-contract=fast -ffp-contract=on
        -ffp-contract=fast-honor-pragmas -ffp-model=fast -ffp-model=aggressive -fdenormal-fp-math
        -fdenormal-fp-math-f32 -mdaz-ftz -fcx-limited-range)
    set(global_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND global_flags " ${CMAKE_C_FLAGS_${config}} ${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    separate_arguments(global_list UNIX_COMMAND "${global_flags}")
    foreach(flag IN LISTS global_list)
        foreach(bad IN LISTS forbidden)
            if(flag STREQUAL bad OR flag MATCHES "^${bad}=")
                message(FATAL_ERROR "STXlib floating-point discipline: the global flags hold ${flag} (rule 1)")
            endif()
        endforeach()
    endforeach()
    get_property(targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    set(checked 0)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
            continue()
        endif()
        get_target_property(options ${target} COMPILE_OPTIONS)
        if(NOT options)
            set(options "")
        endif()
        if(NOT "-ffp-contract=off" IN_LIST options)
            message(FATAL_ERROR "STXlib floating-point discipline: ${target} lacks -ffp-contract=off (rule 1)")
        endif()
        foreach(flag IN LISTS options)
            foreach(bad IN LISTS forbidden)
                if(flag STREQUAL bad OR flag MATCHES "^${bad}=")
                    message(FATAL_ERROR "STXlib floating-point discipline: ${target} has ${flag} (rule 1)")
                endif()
            endforeach()
        endforeach()
        math(EXPR checked "${checked} + 1")
    endforeach()
    message(STATUS "STXlib floating-point flags: ${checked} targets checked")
endfunction()
stxlib_check_float_flags()

# ---- the exception flag check ---------------------------------------------------
# STXlib's own code is built without exceptions (AGENTS.md), checked at
# configure time: every library target must end its options with
# -fno-exceptions (the last of -fexceptions and -fno-exceptions wins), and
# the one source of theirs that may add -fexceptions is the recorder, only
# while STXLIB_EXCEPTIONS is ON. With it OFF, every target defined here must
# be built without exceptions, except the probe that shows the object check
# can see them. stxlib_no_exception_symbols checks the objects themselves.
function(stxlib_check_exception_flags)
    set(libraries stxlib_state stxlib_boundary stxlib_algorithms)
    foreach(optional IN ITEMS stxlib_math_avx2_fma)
        if(TARGET ${optional})
            list(APPEND libraries ${optional})
        endif()
    endforeach()
    get_property(targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    set(checked 0)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
            continue()
        endif()
        set(library FALSE)
        if(target IN_LIST libraries)
            set(library TRUE)
        endif()
        set(required ${library})
        if(NOT STXLIB_EXCEPTIONS AND NOT target STREQUAL "stxlib_exception_probe")
            set(required TRUE)
        endif()
        if(required)
            get_target_property(options ${target} COMPILE_OPTIONS)
            if(NOT options)
                set(options "")
            endif()
            set(last "")
            foreach(flag IN LISTS options)
                if(flag STREQUAL "-fexceptions" OR flag STREQUAL "-fno-exceptions")
                    set(last "${flag}")
                endif()
            endforeach()
            if(NOT last STREQUAL "-fno-exceptions")
                message(FATAL_ERROR "STXlib exceptions: ${target} is not built with -fno-exceptions")
            endif()
            math(EXPR checked "${checked} + 1")
        endif()
        if(library)
            get_target_property(sources ${target} SOURCES)
            foreach(source IN LISTS sources)
                get_source_file_property(source_options "${source}" COMPILE_OPTIONS)
                if(source_options AND "-fexceptions" IN_LIST source_options)
                    if(NOT STXLIB_EXCEPTIONS OR NOT "${source}" STREQUAL "${STXLIB_EXCEPTION_RECORDER}")
                        message(FATAL_ERROR "STXlib exceptions: ${source} in ${target} is built with -fexceptions")
                    endif()
                endif()
            endforeach()
        endif()
    endforeach()
    message(STATUS "STXlib exception flags: ${checked} targets checked")
endfunction()
stxlib_check_exception_flags()

# ---- licences ---------------------------------------------------------------
# The notices of what STXlib compiles into a program, installed at
# share/licenses/stxlib under the names stx's packages already use
# (stx cmake/Stxlib.cmake), so a package that installs STXlib carries them
# and a consumer finds them there (GUI.Forms installs them beside its own:
# its CMakeLists.txt, gui_forms_stx_licenses). stx fetches STXlib with
# FetchContent_MakeAvailable, so these rules run in its install.
# stx_minmallocpp's NOTICE (which carries mimalloc's licence, owner decision
# 2026-11-15) is installed whatever the backend, so a consumer's install
# rule for it holds in every configuration, Emscripten's host backend
# included.
include(GNUInstallDirs)
set(stxlib_licence_directory ${CMAKE_INSTALL_DATADIR}/licenses/stxlib)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE DESTINATION ${stxlib_licence_directory})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/stx_btree/LICENSE
        DESTINATION ${stxlib_licence_directory} RENAME stx_btree-LICENSE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/stx_btree/NOTICE
        DESTINATION ${stxlib_licence_directory} RENAME stx_btree-NOTICE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tzdata/LICENSE
        DESTINATION ${stxlib_licence_directory} RENAME tzdata-LICENSE)
# The Unicode License v3 for the tables generated from CaseFolding.txt and
# UnicodeData.txt (src/algorithms/case_folding_table.inc and
# case_folding_full_table.inc, and src/algorithms/markdown/
# unicode_classes.inc, compiled into stxlib_algorithms in every
# configuration; PROVENANCE.md, M2 C7 and stxlib::markdown).
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/LICENSE
        DESTINATION ${stxlib_licence_directory} RENAME unicode-LICENSE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/state/stx_minmallocpp/NOTICE
        DESTINATION ${stxlib_licence_directory} RENAME stx_minmallocpp-NOTICE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/glyphs/NOTICE
        DESTINATION ${stxlib_licence_directory} RENAME glyphs-NOTICE)
# stxlib::markdown (compiled into stxlib_algorithms in every configuration):
# its NOTICE carries MD4C's licence, and its table of named character
# references is generated from the HTML Standard's list (PROVENANCE.md). Its
# Unicode tables are covered by unicode-LICENSE above. The CommonMark
# specification's text (third_party/commonmark-0.31.2, CC-BY-SA 4.0) is test
# data: compiled into nothing and not installed.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/markdown/NOTICE
        DESTINATION ${stxlib_licence_directory} RENAME markdown-NOTICE)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/whatwg-entities/LICENSE
        DESTINATION ${stxlib_licence_directory} RENAME whatwg-entities-LICENSE)

# ---- the MSVC target's own startup and C++ runtime (opt-in) -------------------
# x86_64-pc-windows-msvc, compiled by the pinned clang: the startup shim
# (src/hosted/windows_msvc_startup, in place of Microsoft's vcstartup
# objects) and the C++ runtime (src/hosted/windows/msvc_runtime, in place of
# vcruntime). Owner, 2026-11-16 (projects desks/coordinator/
# DECISIONS_2026-11-16.md, "Windows surfaces study and the MSVC runtime").
# OFF by default, and refused on every other target, so nothing changes for
# MinGW or any other host until both are proven on Windows: the startup
# probes and the 17-case conformance suite against vcruntime
# (tools/msvc_runtime/run_conformance.py). Each part is still buildable as
# its own project, as its README says.
option(STXLIB_MSVC_RUNTIME "Build STXlib's startup shim and C++ runtime for x86_64-pc-windows-msvc (unproven)" OFF)
if(STXLIB_MSVC_RUNTIME)
    if(NOT WIN32 OR MINGW OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        message(FATAL_ERROR "STXLIB_MSVC_RUNTIME is for x86_64-pc-windows-msvc with clang only")
    endif()
    # The runtime's thunks and __chkstk are assembly (thunks_x64.S,
    # chkstk_x64.S); a language is enabled in the highest directory that
    # uses it, as the conformance suite does in its project().
    set(CMAKE_ASM_COMPILER_TARGET "x86_64-pc-windows-msvc")
    enable_language(ASM)
    # The startup shim's probes follow STXlib's own tests switch.
    set(STXLIB_MSVC_STARTUP_TESTS ${STXLIB_TESTS})
    add_subdirectory(src/hosted/windows_msvc_startup)
    add_subdirectory(src/hosted/windows/msvc_runtime)
    message(STATUS "STXlib MSVC startup shim and C++ runtime: ON (unproven)")
endif()
