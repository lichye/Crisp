if("$ENV{TMPDIR}" STREQUAL "" OR NOT IS_DIRECTORY "$ENV{TMPDIR}")
    message(FATAL_ERROR "Set TMPDIR to the project experiment directory")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(work "$ENV{TMPDIR}/veval-aggregate-function-${suffix}")
execute_process(COMMAND "${VEVAL}" transport "${DESIGN}" --top aggregate_function_boundary
    --out "${work}/products" --tier2-notes
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_QUIET)
set(product "${work}/products/aggregate_function_dut_spec.sv")
if(NOT result EQUAL 0 OR NOT EXISTS "${product}")
    message(FATAL_ERROR "Expected a function product in ${work}\n${error}")
endif()
file(READ "${product}" source)
foreach(pattern IN ITEMS "typedef logic \\[7:0\\] __cf_function_[^ ]+_t\\[7:4\\]"
                         "function automatic __cf_function_[^ ]+_result_t"
                         "__cf_element_" "reads_array: assert property" "reads_return: assert property")
    if(NOT source MATCHES "${pattern}")
        message(FATAL_ERROR "Missing ${pattern} in ${product}")
    endif()
endforeach()
if(source MATCHES "updates_array: assert property")
    message(FATAL_ERROR "An unsupported aggregate update was published in ${product}")
endif()
file(READ "${work}/products/Tier2/aggregate_function_dut_notes.md" notes)
if(NOT notes MATCHES "pure function contains an aggregate update")
    message(FATAL_ERROR "Missing the aggregate update refusal in ${work}")
endif()
execute_process(COMMAND "${EBMC}" --systemverilog --show-properties
    --top aggregate_function_dut_spec "${DESIGN}" "${product}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE properties)
if(NOT result EQUAL 0 OR NOT properties MATCHES "reads_array" OR NOT properties MATCHES "reads_return")
    message(FATAL_ERROR "EBMC failed to read the function product in ${work}\n${properties}\n${error}")
endif()
file(REMOVE_RECURSE "${work}")
