execute_process(COMMAND "${VEVAL}" transport "${DESIGN}" --top "${TOP}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_QUIET)
if(NOT result EQUAL 1 OR NOT error MATCHES "block on overlapping elements")
    message(FATAL_ERROR "${TOP}: expected the overlapping-driver diagnostic, got ${result}\n${error}")
endif()
