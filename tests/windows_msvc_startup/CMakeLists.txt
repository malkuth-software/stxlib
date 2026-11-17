# The startup shim's probes (src/hosted/windows_msvc_startup). Each is an
# image linked with the shim in place of Microsoft's startup objects,
# vcruntime for the exception runtime (Stage 1), and the UCRT, hardened as
# GUI.Forms links (stack protector, Control Flow Guard), so that the
# probes exercise the cookie, the load configuration and the guard
# pointers the shim supplies.

set(stx_msvc_startup_probe_compile -fstack-protector-strong "SHELL:-Xclang -cfguard" -Wall -Wextra)
set(stx_msvc_startup_probe_link
    "LINKER:/NODEFAULTLIB:msvcrt.lib" "LINKER:/NODEFAULTLIB:libcmt.lib"
    "LINKER:/NODEFAULTLIB:msvcrtd.lib" "LINKER:/NODEFAULTLIB:libcmtd.lib"
    "LINKER:/guard:cf" "LINKER:/DYNAMICBASE" "LINKER:/HIGHENTROPYVA")

function(stx_msvc_startup_probe target)
    target_compile_options(${target} PRIVATE ${stx_msvc_startup_probe_compile})
    target_link_options(${target} PRIVATE ${stx_msvc_startup_probe_link})
    target_link_libraries(${target} PRIVATE stx_msvc_startup stx_msvc_startup_vcruntime_mode vcruntime.lib ucrt.lib)
endfunction()

# A DLL with static and thread_local state and no DllMain of its own.
add_library(stx_msvc_startup_probe_library SHARED probe_library.cpp)
stx_msvc_startup_probe(stx_msvc_startup_probe_library)

add_executable(stx_msvc_startup_probe startup_probe.cpp)
stx_msvc_startup_probe(stx_msvc_startup_probe)
target_link_libraries(stx_msvc_startup_probe PRIVATE stx_msvc_startup_probe_library)

add_executable(stx_msvc_startup_wide_probe wide_probe.cpp)
stx_msvc_startup_probe(stx_msvc_startup_wide_probe)

add_executable(stx_msvc_startup_window_probe WIN32 window_probe.cpp)
stx_msvc_startup_probe(stx_msvc_startup_window_probe)

add_executable(stx_msvc_startup_overrun_probe overrun_probe.cpp)
stx_msvc_startup_probe(stx_msvc_startup_overrun_probe)

add_executable(stx_msvc_startup_uncaught_probe uncaught_probe.cpp)
stx_msvc_startup_probe(stx_msvc_startup_uncaught_probe)

set(stx_msvc_startup_exit_check "${CMAKE_CURRENT_SOURCE_DIR}/check_exit_code.cmake")

add_test(NAME stx_msvc_startup_probe COMMAND stx_msvc_startup_probe)
set_tests_properties(stx_msvc_startup_probe PROPERTIES
    PASS_REGULAR_EXPRESSION "startup probe: all checks passed"
    FAIL_REGULAR_EXPRESSION "FAILED")
# The exit functions run after main: their order is checked by the last of
# them, which ends the process with 0 only if every earlier one ran in
# order (the regular expression above ignores the exit code).
add_test(NAME stx_msvc_startup_exit_order COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:stx_msvc_startup_probe> -DEXPECTED=0 -P "${stx_msvc_startup_exit_check}")
add_test(NAME stx_msvc_startup_wide COMMAND ${CMAKE_COMMAND}
    "-DPROGRAM=$<TARGET_FILE:stx_msvc_startup_wide_probe>" "-DARGUMENTS=one;two words" -DEXPECTED=3 -P "${stx_msvc_startup_exit_check}")
add_test(NAME stx_msvc_startup_window COMMAND ${CMAKE_COMMAND}
    "-DPROGRAM=$<TARGET_FILE:stx_msvc_startup_window_probe>" "-DARGUMENTS=alpha;beta" -DEXPECTED=11 -P "${stx_msvc_startup_exit_check}")
# A stack overrun ends the process through __fastfail with
# STATUS_STACK_BUFFER_OVERRUN (0xC0000409 = 3221226505).
add_test(NAME stx_msvc_startup_overrun COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:stx_msvc_startup_overrun_probe> -DEXPECTED=3221226505 -P "${stx_msvc_startup_exit_check}")
# An uncaught C++ exception reaches terminate(), whose default handler
# aborts: exit code 3 from the UCRT's abort, or STATUS_STACK_BUFFER_OVERRUN
# when abort reports the fault through __fastfail (its default
# _CALL_REPORTFAULT behaviour, Learn "_set_abort_behavior").
add_test(NAME stx_msvc_startup_uncaught COMMAND ${CMAKE_COMMAND}
    "-DPROGRAM=$<TARGET_FILE:stx_msvc_startup_uncaught_probe>" "-DEXPECTED=3;3221226505" -P "${stx_msvc_startup_exit_check}")
