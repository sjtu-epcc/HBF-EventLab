execute_process(COMMAND "${PROGRAM}" "${WORKLOAD}" ${ARGS}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT "${result}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "exit=${result}, expected=${EXPECTED}\n${output}\n${errors}")
endif()
if(NOT "${output}" MATCHES "${EXPECT_TEXT}")
    message(FATAL_ERROR "missing expected report: ${EXPECT_TEXT}\n${output}\n${errors}")
endif()
message(STATUS "${output}")

