if("$ENV{TMPDIR}" STREQUAL "" OR NOT IS_DIRECTORY "$ENV{TMPDIR}")
    message(FATAL_ERROR "Set TMPDIR to the project experiment directory")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(work "$ENV{TMPDIR}/veval-aggregate-port-${suffix}")
set(design "${FIXTURES}/aggregate_port_types.sv")
execute_process(COMMAND "${VEVAL}" transport "${design}" --top aggregate_boundary
    -I "${FIXTURES}" --out "${work}/products" --tier2-notes
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_QUIET)
set(product "${work}/products/aggregate_dut_spec.sv")
if(NOT result EQUAL 0 OR NOT EXISTS "${product}")
    message(FATAL_ERROR "Expected an aggregate port product in ${work}\n${error}")
endif()
file(READ "${product}" source)
foreach(text IN ITEMS "words[4:2]" "logic [0:0] flags[0:1]" "logic [1:0] [3:0]" "aggregate_result_t result"
                      "aggregate_pkg::state_t state" "aggregate_pkg::enable_t enable" "words[3][1'b1]" "result.data"
                      "reads_elements_and_fields: assert property" "reads_enum: assert property")
    string(FIND "${source}" "${text}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Missing ${text} in ${product}")
    endif()
endforeach()
execute_process(COMMAND "${EBMC}" --systemverilog --show-properties
    --top aggregate_dut_spec -I "${FIXTURES}" "${design}" "${product}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE properties)
if(NOT result EQUAL 0 OR NOT properties MATCHES "reads_elements_and_fields"
                      OR NOT properties MATCHES "reads_enum")
    message(FATAL_ERROR "EBMC failed to read the generated product in ${work}\n${properties}\n${error}")
endif()
file(REMOVE_RECURSE "${work}")
