execute_process(COMMAND "${LAUNCHER}" --help RESULT_VARIABLE result OUTPUT_VARIABLE help)
if(NOT result EQUAL 0 OR NOT help MATCHES "INDEX:BYTES")
  message(FATAL_ERROR "launcher help failed")
endif()
execute_process(COMMAND "${LAUNCHER}" --entry read --input 0:64 --output 00:64
  --threads 4 --mode thread -- "${TARGET}" exit_code RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "overlapping parameter roles must fail before child creation: ${result}")
endif()
execute_process(COMMAND "${LAUNCHER}" --entry read --input 0:64 --output 1:64
  --threads 257 --mode thread -- "${TARGET}" exit_code RESULT_VARIABLE result)
if(NOT result EQUAL 2)
  message(FATAL_ERROR "oversized CTA accepted: ${result}")
endif()
execute_process(COMMAND "${LAUNCHER}" --entry read --input 0:64 --output 1:64
  --threads 4 --mode thread -- "${TARGET}" exit_code RESULT_VARIABLE result)
if(NOT result EQUAL 37)
  message(FATAL_ERROR "child exit code not preserved: ${result}")
endif()
