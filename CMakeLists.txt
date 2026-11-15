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
#   (src/state/stx_minmallocpp), a conversion of mimalloc's design to
#   house-style C++ (the owner, 2026-11-15; the default by the owner's
#   decision of the same day, notes decisions/2026-11-15-stx-minmallocpp.md).
#   proofs/REPORT.md says what of it is proved, tested and measured.
# - mimalloc: the vendored release (third_party/mimalloc), kept as an
#   option.
# - host: the host's C library. Sanitizers track the host allocator only,
#   so a build with sanitizers or the integer checks defaults to it (their
#   reports then name STXlib's own blocks), and src/state/allocator.cpp
#   refuses mimalloc under address, thread or memory sanitizers.
# Emscripten: stx_minmallocpp is the default there too, with 64 MiB regions
# for 32-bit addresses (src/state/stx_minmallocpp/layout.hpp) and its
# address space taken from Emscripten's dlmalloc
# (src/hosted/emscripten/minmallocpp_os_emscripten.cpp); host (Emscripten's
# malloc, dlmalloc, src/settings.js MALLOC at emscripten 6.0.13) is
# admitted. mimalloc is not: its Emscripten layer takes its memory from
# emmalloc_memalign (third_party/mimalloc/src/prim/emscripten/prim.c), a
# second allocator beside dlmalloc.
if(stxlib_sanitized)
    set(stxlib_allocator_default host)
else()
    set(stxlib_allocator_default stx_minmallocpp)
endif()
set(STXLIB_ALLOCATOR ${stxlib_allocator_default} CACHE STRING "STXlib allocator backend: mimalloc, host or stx_minmallocpp")
set_property(CACHE STXLIB_ALLOCATOR PROPERTY STRINGS mimalloc host stx_minmallocpp)
if(NOT STXLIB_ALLOCATOR MATCHES "^(mimalloc|host|stx_minmallocpp)$")
    message(FATAL_ERROR "STXLIB_ALLOCATOR must be mimalloc, host or stx_minmallocpp")
endif()
if(STXLIB_HOST_FAMILY STREQUAL "emscripten" AND STXLIB_ALLOCATOR STREQUAL "mimalloc")
    message(FATAL_ERROR "STXLIB_ALLOCATOR=mimalloc is not admitted for Emscripten; use stx_minmallocpp or host")
endif()
message(STATUS "STXlib allocator backend: ${STXLIB_ALLOCATOR}")
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
if(WIN32)
    list(APPEND STXLIB_STATE_SOURCES src/hosted/windows/thread_exit_windows.cpp)
else()
    list(APPEND STXLIB_STATE_SOURCES src/hosted/posix/thread_exit_posix.cpp)
endif()
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
# STXLIB_SHADOW as the boundary has it: the thread state's layout depends on
# the build type (state.hpp, layout 9), and the layout key refuses a mix.
target_compile_definitions(stxlib_state PRIVATE
    STXLIB_STATE_BUILDING
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

if(STXLIB_ALLOCATOR STREQUAL "mimalloc")
    # mimalloc v3.6.1 with STXlib's patch series (third_party/mimalloc, MIT;
    # third_party/mimalloc-patches; PROVENANCE.md), compiled as its one
    # translation unit (src/static.c) into stxlib_state, with the
    # definitions stx_auxlib b6e330a used (CMakeLists.txt there). It replaces
    # no C library function (MI_MALLOC_OVERRIDE stays undefined) and its
    # symbols are hidden, so its heap holds STXlib's blocks only. Compiled as
    # C++ (mimalloc's MI_USE_CXX): compiled as C it defines a weak fallback
    # std::get_new_handler that, hidden, would bind in place of the C++
    # library's (stx_auxlib's finding). Since v3.6 its C++ new-handler path
    # has no try/catch and its one throw is guarded by __cpp_exceptions
    # (src/alloc/alloc.c, mi_try_new_handler), so it also compiles under
    # -fno-exceptions; STXlib never calls that path (mi_new and kin).
    # MI_PROFILE=0: v3.6 builds its pprof profiler by default (MI_PROFILE=1,
    # include/mimalloc/types.h), which starts at process init when the
    # MIMALLOC_PROFILE environment variable names a file and then writes
    # profiles there (src/profile/pprof.c, _mi_pprof_profiler_init). Its
    # setuid check needs MI_HAS_UNISTDH, which only upstream's CMake defines,
    # so here it would pass in a setuid process; on FreeBSD it calls
    # kinfo_getvmmap, which needs -lutil. STXlib uses no profiler, so the
    # whole of it is left out. Vendored code: its warnings are upstream's
    # (-w), and the integer checks do not apply to it.
    set_source_files_properties(third_party/mimalloc/src/static.c PROPERTIES LANGUAGE CXX)
    add_library(stxlib_mimalloc OBJECT third_party/mimalloc/src/static.c)
    set_target_properties(stxlib_mimalloc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(stxlib_mimalloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc/include)
    target_compile_definitions(stxlib_mimalloc PRIVATE MI_STATIC_LIB MI_BUILD_RELEASE NDEBUG=1 MI_PROFILE=0)
    # The owner's choices on the mimalloc audit (2026-11-15; notes
    # decisions/2026-11-15-mimalloc-audit-answers.md; the audit's IDs):
    # - MI_NO_GETENV (Q1): mimalloc reads no environment. Unset, it reads up
    #   to 62 MIMALLOC_* variables at load, setuid processes included, before
    #   STXlib runs; they can make every allocation fail, write to stderr,
    #   commit memory up front or crash at exit (D1, D3, D5, C3, C4, E3, B9).
    #   src/util/libc.c, _mi_getenv, then returns "not set" for every name.
    # - The memory settings (Q4), each avoiding a v3.6.1 defect that upstream
    #   fixed after the release: MI_DEFAULT_PAGEMAP_COMMIT=1 commits the page
    #   map's top level up front, so concurrent commits cannot lower its
    #   committed count and make mi_is_in_heap_region miss a live block (A3,
    #   B5; upstream 3e4c338a6). MI_DEFAULT_ARENA_MAX_OBJECT_SIZE=32768 (KiB)
    #   sends blocks over 32 MiB straight to the OS, so a block of about
    #   224-255 MiB no longer reserves an arena it cannot use and no arena
    #   keeps an unusable multi-chunk tail (B2, B6; dfb9e5060, 202e2785c).
    #   MI_NO_ALIGNED_HINT leaves arena placement to the kernel, so a first
    #   allocation on a worker thread cannot leave later arenas at fixed low
    #   addresses (B3; 24a5896da). On FreeBSD, MI_DEFAULT_ARENA_EAGER_COMMIT=0,
    #   below: v3.6.1 reads vm.overcommit inverted (B4, unfixed upstream;
    #   patch 0001 corrects the reading) and would commit whole arenas up
    #   front under strict accounting. The
    #   purge delay of 0 (B1) is an option set at start-up, not a definition.
    target_compile_definitions(stxlib_mimalloc PRIVATE
        MI_NO_GETENV
        MI_DEFAULT_PAGEMAP_COMMIT=1
        MI_DEFAULT_ARENA_MAX_OBJECT_SIZE=32768
        MI_NO_ALIGNED_HINT)
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_compile_definitions(stxlib_mimalloc PRIVATE MI_DEFAULT_ARENA_EAGER_COMMIT=0)
    endif()
    # -fno-exceptions: mimalloc throws only from its operator new path
    # (mi_new and its kind, src/alloc/alloc.c, mi_try_new_handler), which STXlib
    # never calls (src/state/allocator.cpp calls mi_malloc_aligned, mi_free
    # and mi_is_in_heap_region); built without exceptions that path aborts
    # instead, and the object refers to no exception runtime
    # (stxlib_no_exception_symbols).
    # -ftls-model=local-dynamic (the owner, Q2): with initial-exec the loader
    # must place this module's whole TLS block in static TLS, so a dlopen of
    # stxlib_state fails on musl always and on glibc and FreeBSD once their
    # small reserve is used (audit C1), and sb3's thread-records design loads
    # stxlib_state through dlopen too. Where the final link is an executable
    # the linker relaxes the accesses to local-exec again.
    target_compile_options(stxlib_mimalloc PRIVATE
        -w -fvisibility=hidden -ftls-model=local-dynamic -fno-sanitize=all -fno-exceptions)
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
    # On musl, MI_LIBC_MUSL=1 as upstream's CMake defines it, so mimalloc's
    # thread id takes the path upstream tests on musl instead of
    # __builtin_thread_pointer (include/mimalloc/prim-tls.h, the one use of
    # the macro; smi's mimalloc audit, C11). Kept in its own block, apart from
    # the definitions above, so it merges beside smi's settings
    # (claude/mimalloc-settings).
    if(STXLIB_HOST_FAMILY STREQUAL "musl")
        target_compile_definitions(stxlib_mimalloc PRIVATE MI_LIBC_MUSL=1)
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
    src/core/atomic_rules.cpp
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
    src/core/images.cpp
    src/core/known_directories.cpp
    src/core/library.cpp
    src/core/local_time.cpp
    src/core/memory.cpp
    src/core/path.cpp
    src/core/process.cpp
    src/core/process_info.cpp
    src/core/report.cpp
    src/core/require.cpp
    src/core/shadow_frames.cpp
    src/core/stack_trace.cpp
    src/core/text.cpp
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
    src/hosted/posix/terminal_posix.cpp
    src/hosted/posix/threads_posix.cpp
    src/hosted/posix/trace_posix.cpp
    src/hosted/posix/wake_posix.cpp)
set(STXLIB_HOSTED_MACOS_SOURCES
    src/hosted/macos/image_regions_macos.cpp
    src/hosted/macos/process_macos.cpp)
set(STXLIB_HOSTED_LINUX_SOURCES
    src/hosted/elf/image_regions_elf.cpp
    src/hosted/linux/process_linux.cpp)
set(STXLIB_HOSTED_FREEBSD_SOURCES
    src/hosted/elf/image_regions_elf.cpp
    src/hosted/freebsd/process_freebsd.cpp)
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
    src/hosted/emscripten/clocks_emscripten.cpp
    src/hosted/emscripten/images_emscripten.cpp
    src/hosted/emscripten/local_time_emscripten.cpp
    src/hosted/emscripten/process_emscripten.cpp
    src/hosted/emscripten/runtime_emscripten.cpp
    src/hosted/emscripten/trace_emscripten.cpp
    src/hosted/emscripten/wake_emscripten.cpp)
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
    src/hosted/windows/process_windows.cpp
    src/hosted/windows/terminal_windows.cpp
    src/hosted/windows/threads_windows.cpp
    src/hosted/windows/trace_windows.cpp
    src/hosted/windows/utf16.cpp
    src/hosted/windows/wake_windows.cpp)
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
    # process_windows.cpp; PSAPI_VERSION 2) are kernel32's.
    target_link_libraries(stxlib_boundary PUBLIC bcrypt shell32 ole32 kernel32 mincore)
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
# region path where it is proved and not slower by design (paths.hpp says
# why for each); keep them equal to paths.hpp's.
option(STXLIB_MATH_REGION_POW_DOUBLE "pow(double) takes the region kernel" OFF)
option(STXLIB_MATH_REGION_POW_FLOAT "pow(float) takes the region round, test, decide path" OFF)
option(STXLIB_MATH_REGION_LOG_FLOAT "log, log2, log10 (float) take the region kernels" OFF)
option(STXLIB_MATH_REGION_HYPOT_FLOAT "hypot(float) takes the region kernel" ON)
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
    src/algorithms/case_folding.cpp
    src/algorithms/hash.cpp
    src/algorithms/numeric_text.cpp
    src/algorithms/precedence.cpp
    src/algorithms/frozen_map.cpp
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
                               owned_vocabulary owned_text ascii_search heap transcode thread_records)
    if(STXLIB_EXCEPTIONS)
        list(APPEND stxlib_boundary_tests gate trampoline shadow)
    endif()
    if(stxlib_web_tests)
        list(APPEND stxlib_boundary_tests emscripten)
    else()
        list(APPEND stxlib_boundary_tests children images pages_wake)
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

    if(STXLIB_ALLOCATOR STREQUAL "mimalloc")
        target_compile_definitions(stxlib_memory_tests PRIVATE STXLIB_EXPECT_MIMALLOC)
    elseif(STXLIB_ALLOCATOR STREQUAL "stx_minmallocpp")
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
    target_link_libraries(stxlib_stx_minmallocpp_floor_bench PRIVATE Threads::Threads)

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
        target_link_libraries(stxlib_stx_minmallocpp_interleave PRIVATE Threads::Threads)
        add_test(NAME stxlib_stx_minmallocpp_interleave COMMAND stxlib_stx_minmallocpp_interleave 2)
    endif()

    add_executable(stxlib_stx_minmallocpp_direct_tests tests/stx_minmallocpp_direct_tests.cpp
        $<TARGET_OBJECTS:stxlib_stx_minmallocpp_checked>)
    target_include_directories(stxlib_stx_minmallocpp_direct_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(stxlib_stx_minmallocpp_direct_tests PRIVATE cxx_std_20)
    target_compile_options(stxlib_stx_minmallocpp_direct_tests PRIVATE ${stxlib_test_warnings})
    target_link_libraries(stxlib_stx_minmallocpp_direct_tests PRIVATE Threads::Threads)
    add_test(NAME stxlib_stx_minmallocpp_direct_tests COMMAND stxlib_stx_minmallocpp_direct_tests)
    # These programs link no STXlib target, so on Emscripten they take the
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
    # listed host call or a toolchain helper (tools/
    # check_stx_minmallocpp_surface.py). The objects are the checked
    # library's, laid out as the Ninja and Makefile generators do.
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
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_stx_minmallocpp_surface.py
                    --nm ${CMAKE_NM} --family ${stxlib_stx_minmallocpp_family}
                    --objects ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/stxlib_stx_minmallocpp_checked.dir
                    ${stxlib_stx_minmallocpp_sanitized})
    endif()

    # stxlib::algorithms.
    foreach(test_name IN ITEMS calendar frozen_map hash numeric_text precedence random simd)
        add_executable(stxlib_${test_name}_tests tests/${test_name}_tests.cpp)
        target_link_libraries(stxlib_${test_name}_tests PRIVATE stxlib::algorithms)
        # core/checked.hpp (the modular marker) for the tests' own generators.
        target_include_directories(stxlib_${test_name}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(stxlib_${test_name}_tests PRIVATE ${stxlib_test_warnings})
        add_test(NAME stxlib_${test_name}_tests COMMAND stxlib_${test_name}_tests)
    endforeach()
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
            "view_index|stxlib: requirement failed: View: index out of range"
            "take_failed_value|stxlib: requirement failed: Result::take_value on a failure"
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
            "gate_cancelled|stxlib: fail fast: stxlib\\.tests\\.cancel: interrupted.*thread cancellation or pthread_exit")
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
                    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
        add_test(NAME stxlib_case_folding_table
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_case_folding.py --check
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unicode-18.0.0/CaseFolding.txt
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms/case_folding_table.inc)
        add_test(NAME stxlib_std_rules_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_std_rules_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # The allocator surface check's own tests (every family, no nm).
        add_test(NAME stxlib_stx_minmallocpp_surface_tool
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_stx_minmallocpp_surface_tests.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        # STXlib's patched mimalloc (AGENTS.md): the tree is the pinned
        # release with the series of third_party/mimalloc-patches applied,
        # every hash as PROVENANCE.md records it.
        add_test(NAME stxlib_mimalloc_patches
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mimalloc_patches.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
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
            foreach(library IN ITEMS stxlib_state stxlib_mimalloc stxlib_boundary stxlib_algorithms stxlib_math_avx2_fma)
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
    foreach(optional IN ITEMS stxlib_mimalloc stxlib_math_avx2_fma)
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
