# Runs one must-end case of stxlib_death_tests and checks how it ended:
#   cmake -DPROGRAM=<path> -DCASE=<name> -DEXPECT=<regex> -P death_check.cmake
# Passes when the case printed "skipped", or when it ended abnormally (any
# result other than 0, without printing "survived") and its standard error
# matches EXPECT. EXPECT=ANY accepts any standard error: the case ends in a
# host check whose wording STXlib does not own (foreign_free).
execute_process(
    COMMAND "${PROGRAM}" "${CASE}"
    RESULT_VARIABLE case_result
    OUTPUT_VARIABLE case_output
    ERROR_VARIABLE case_error)
if(case_output MATCHES "skipped")
    message(STATUS "${CASE}: skipped (${case_output})")
    return()
endif()
if("${case_result}" STREQUAL "0" OR case_output MATCHES "survived")
    message(FATAL_ERROR "${CASE} survived:\n${case_output}\n${case_error}")
endif()
if(NOT EXPECT STREQUAL "ANY" AND NOT case_error MATCHES "${EXPECT}")
    message(FATAL_ERROR "${CASE} ended (${case_result}) without the expected report '${EXPECT}':\n${case_error}")
endif()
message(STATUS "${CASE}: ended as expected (${case_result})")
