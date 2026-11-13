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
    # On MinGW, mimalloc's Windows layer (src/prim/windows/prim.c, its default
    # MI_WIN_INIT_USE_CRT_TLS) always registers its loader TLS callbacks, and
    # unless MI_MINGW_UCRT64 is defined src/prim/prim.c also registers a
    # constructor. Both call _mi_auto_process_init, and the second call's
    # mi_add_stderr_output flushed the newline that the first left in
    # mimalloc's delayed-output buffer (options.c, mi_out_buf_flush): one
    # extra 0a at the start of every program's error stream (CI 43441112933).
    # With MI_MINGW_UCRT64 the TLS and C runtime hooks are the only ones.
    # Those hooks need the Universal C Runtime (MSYS2's CLANG64 and UCRT64);
    # older MinGW C runtimes take the host backend.
    if(MINGW)
        check_cxx_source_compiles("#include <stdio.h>
#ifndef _UCRT
#error not the Universal C Runtime
#endif
int main() { return 0; }" stxlib_mingw_ucrt)
        if(NOT stxlib_mingw_ucrt)
            message(FATAL_ERROR "mimalloc on MinGW needs the Universal C Runtime; configure with -DSTXLIB_ALLOCATOR=host")
        endif()
        target_compile_definitions(stxlib_mimalloc PRIVATE MI_MINGW_UCRT64)
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
    src/core/zones.cpp
    src/hosted/c_library/c_library.cpp
    src/hosted/cxx_runtime/clocks.cpp
    src/hosted/cxx_runtime/current_exception.cpp)

# Interface modules per family. POSIX services shared by macOS, glibc and
# musl live in src/hosted/posix/; a family directory joins when a service
# differs (the stx_auxlib hosted port).
set(STXLIB_HOSTED_POSIX_SOURCES
    src/hosted/posix/children_posix.cpp
    src/hosted/posix/clocks_posix.cpp
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
    src/hosted/windows/clocks_windows.cpp
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
    # BCryptGenRandom (entropy_windows.cpp); ShellExecuteW (desktop_windows.cpp);
    # QueryInterruptTimePrecise and QueryUnbiasedInterruptTimePrecise
    # (clocks_windows.cpp), which kernel32's import library does not carry:
    # Microsoft documents them in api-ms-win-core-realtime-l1-1-1.dll
    # (library mincore). mingw-w64 installs that API set only inside
    # libmincore.a (crt Makefile.am: mincore.mri), so mincore is linked, after
    # kernel32 so that kernel32 still provides every function it exports and
    # mincore only these two.
    target_link_libraries(stxlib_boundary PUBLIC bcrypt shell32 kernel32 mincore)
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
    src/algorithms/math/exp.cpp
    src/algorithms/math/hyperbolic.cpp
    src/algorithms/math/hypot.cpp
    src/algorithms/math/log.cpp
    src/algorithms/math/pow.cpp
    src/algorithms/math/sincos.cpp
    src/algorithms/math/tan.cpp)
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
    src/algorithms/hash.cpp
    src/algorithms/numeric_text.cpp
    src/algorithms/precedence.cpp
    src/algorithms/random.cpp
    src/algorithms/row_height_index.cpp
    src/algorithms/simd.cpp
    src/algorithms/voting.cpp)
add_library(stxlib_algorithms STATIC ${STXLIB_ALGORITHMS_SOURCES})
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

    foreach(test_name IN ITEMS vocabulary gate memory trampoline shadow platform path files children threads thread_pool local_time)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::boundary)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()

    if(STXLIB_ALLOCATOR STREQUAL "mimalloc")
        target_compile_definitions(stxlib_memory_tests PRIVATE STXLIB_EXPECT_MIMALLOC)
    endif()

    # stxlib::algorithms.
    foreach(test_name IN ITEMS calendar hash numeric_text precedence random simd)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::algorithms)
        # core/checked.hpp (the modular marker) for the tests' own generators.
        target_include_directories(stxlib_${test_name}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()
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
    # Every variant on this machine: the forced levels run the same frozen
    # hashes through v128 and, where the CPU has it, avx2_fma.
    if(STXLIB_MATH_DISPATCH)
        foreach(level IN ITEMS scalar vector128 avx2_fma)
            add_test(NAME stxlib_math_tests_${level} COMMAND stxlib_math_tests)
            set_tests_properties(stxlib_math_tests_${level} PROPERTIES ENVIRONMENT "STX_SIMD_LEVEL=${level}")
        endforeach()
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
    add_test(NAME stxlib_voting_tests
             COMMAND stxlib_voting_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/prv_cases.txt)

    # The thread pool against the C original it ports (tests/reference/,
    # PROVENANCE.md): one conformance source, written against the
    # reference's C interface, checks STXlib's AtomicPool (through
    # tests/reference/atomic_pool_adapter.cpp) and the reference itself; both
    # must pass. From stx_auxlib b6e330a.
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

    # Two modules, each with its own copy of stxlib::boundary, release each
    # other's blocks through the one allocator in stxlib_state.
    add_library(stxlib_allocator_two_module_library SHARED tests/allocator_two_module_library.cpp)
    target_link_libraries(stxlib_allocator_two_module_library PRIVATE stxlib::boundary)
    target_compile_definitions(stxlib_allocator_two_module_library PRIVATE STXLIB_ALLOCATOR_TWO_MODULE_BUILDING)
    target_compile_options(stxlib_allocator_two_module_library PRIVATE ${stxlib_test_warnings})
    set_target_properties(stxlib_allocator_two_module_library PROPERTIES CXX_VISIBILITY_PRESET hidden)
    add_executable(stxlib_allocator_two_module_tests tests/allocator_two_module_tests.cpp)
    target_link_libraries(stxlib_allocator_two_module_tests PRIVATE stxlib::boundary stxlib_allocator_two_module_library)
    target_compile_options(stxlib_allocator_two_module_tests PRIVATE ${stxlib_test_warnings})
    add_test(NAME stxlib_allocator_two_module_tests COMMAND stxlib_allocator_two_module_tests)

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
        "callback_frames|stxlib: frames \\(throw_site\\): stxlib\\.tests\\.outer\\[7,8\\] > stxlib\\.tests\\.callback\\[0,0\\] > stxlib\\.tests\\.inner\\[9,0\\]"
        "callback_frame_texts|stxlib: fail fast: stxlib\\.tests\\.event: invalid_argument.*stxlib: frames \\(throw_site\\): 3:button\\.ok:click\\[0,0\\]#12 > stxlib\\.tests\\.inner\\[9,0\\]"
        "reporter_contract|reporter: console.*reporter: classified 1, message 300, frames 2, kind 3, type (named|none), console ok"
        "reporter_opens_frame|stxlib: requirement failed: a fail-fast reporter opened a shadow frame"
        "reporter_nested|reporter entered.*stxlib: fail fast: stxlib\\.tests\\.callback: internal_failure"
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
        # The cross-host bit identity: each run must print the frozen lines
        # (unfrozen items are reported, not failed, until the first freeze).
        set(stxlib_bit_identity_compare ${Python3_EXECUTABLE} -I
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_bit_identity.py
            --expected ${CMAKE_CURRENT_SOURCE_DIR}/tests/bit_identity_expected.txt --run)
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
