# Runs PROGRAM with ARGUMENTS (a list) and fails unless its exit code is
# one of EXPECTED (a list, decimal; Windows status codes as unsigned).
execute_process(COMMAND "${PROGRAM}" ${ARGUMENTS} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
# CMake reports a status code beyond int as a negative number or a text.
if(result MATCHES "^-[0-9]+$")
    math(EXPR result "${result} + 4294967296")
elseif(result STREQUAL "Exit code 0xc0000409")
    set(result 3221226505)
endif()
if(NOT result IN_LIST EXPECTED)
    message(FATAL_ERROR "${PROGRAM} exited with ${result}, expected ${EXPECTED}\n${output}")
endif()
