# Use the Verilog frontend and CBMC IR from Crisp's pinned submodule.
get_filename_component(hwcbmc_root "${CMAKE_CURRENT_SOURCE_DIR}/../hw-cbmc" ABSOLUTE)
set(hwcbmc_src "${hwcbmc_root}/src")
set(cbmc_src "${hwcbmc_root}/lib/cbmc/src")
find_package(BISON 3.0 REQUIRED)
find_package(FLEX REQUIRED)

set(frontend_sources
    ebmc/ebmc_language.cpp
    ebmc/ebmc_properties.cpp
    ebmc/instrument_past.cpp
    ebmc/show_modules.cpp
    ebmc/transition_system.cpp
    temporal-logic/hoa.cpp
    temporal-logic/ltl_sva_to_string.cpp
    temporal-logic/ltl_to_buechi.cpp
    temporal-logic/normalize_property.cpp
    temporal-logic/rewrite_sva_sequence.cpp
    temporal-logic/temporal_logic.cpp
    temporal-logic/trivial_sva.cpp
    trans-word-level/show_module_hierarchy.cpp
    verilog/aval_bval_encoding.cpp
    verilog/convert_literals.cpp
    verilog/expr2verilog.cpp
    verilog/flex_interface.cpp
    verilog/sva_expr.cpp
    verilog/top_level_modules.cpp
    verilog/typename.cpp
    verilog/verilog_bind.cpp
    verilog/verilog_bits.cpp
    verilog/verilog_constant_folding.cpp
    verilog/verilog_ebmc_language.cpp
    verilog/verilog_elaborate.cpp
    verilog/verilog_elaborate_compilation_unit.cpp
    verilog/verilog_elaborate_module_instances.cpp
    verilog/verilog_elaborate_type.cpp
    verilog/verilog_expr.cpp
    verilog/verilog_generate.cpp
    verilog/verilog_initializer.cpp
    verilog/verilog_instantiate_module.cpp
    verilog/verilog_interfaces.cpp
    verilog/verilog_interpreter.cpp
    verilog/verilog_language.cpp
    verilog/verilog_lowering.cpp
    verilog/verilog_parse_tree.cpp
    verilog/verilog_parser.cpp
    verilog/verilog_preprocessor.cpp
    verilog/verilog_preprocessor_tokenizer.cpp
    verilog/verilog_rtl.cpp
    verilog/verilog_simplifier.cpp
    verilog/verilog_standard.cpp
    verilog/verilog_symbol_table.cpp
    verilog/verilog_transition_relation.cpp
    verilog/verilog_tristate.cpp
    verilog/verilog_typecheck.cpp
    verilog/verilog_typecheck_base.cpp
    verilog/verilog_typecheck_expr.cpp
    verilog/verilog_typecheck_sva.cpp
    verilog/verilog_types.cpp
    verilog/vtype.cpp)
list(TRANSFORM frontend_sources PREPEND "${hwcbmc_src}/")
set(cbmc_sources
    big-int/bigint-func.cc
    big-int/bigint.cc
    langapi/language.cpp
    langapi/language_util.cpp
    langapi/mode.cpp
    util/arith_tools.cpp
    util/bitvector_expr.cpp
    util/bitvector_types.cpp
    util/bv_arithmetic.cpp
    util/byte_operators.cpp
    util/c_types.cpp
    util/cmdline.cpp
    util/config.cpp
    util/cout_message.cpp
    util/dstring.cpp
    util/edit_distance.cpp
    util/endianness_map.cpp
    util/exception_utils.cpp
    util/expr.cpp
    util/expr_util.cpp
    util/fixedbv.cpp
    util/floatbv_expr.cpp
    util/format_expr.cpp
    util/format_type.cpp
    util/get_module.cpp
    util/ieee_float.cpp
    util/invariant.cpp
    util/irep.cpp
    util/irep_ids.cpp
    util/json.cpp
    util/json_irep.cpp
    util/json_stream.cpp
    util/lower_byte_operators.cpp
    util/mathematical_types.cpp
    util/memory_units.cpp
    util/message.cpp
    util/mp_arith.cpp
    util/namespace.cpp
    util/options.cpp
    util/output_file.cpp
    util/parser.cpp
    util/pointer_expr.cpp
    util/pointer_offset_size.cpp
    util/pointer_offset_sum.cpp
    util/pointer_predicates.cpp
    util/rational.cpp
    util/rational_tools.cpp
    util/replace_expr.cpp
    util/replace_symbol.cpp
    util/signal_catcher.cpp
    util/simplify_expr.cpp
    util/simplify_expr_array.cpp
    util/simplify_expr_boolean.cpp
    util/simplify_expr_floatbv.cpp
    util/simplify_expr_if.cpp
    util/simplify_expr_int.cpp
    util/simplify_expr_pointer.cpp
    util/simplify_expr_struct.cpp
    util/simplify_utils.cpp
    util/source_location.cpp
    util/std_expr.cpp
    util/std_types.cpp
    util/string2int.cpp
    util/string_constant.cpp
    util/string_container.cpp
    util/string_hash.cpp
    util/string_utils.cpp
    util/structured_data.cpp
    util/substitute_symbols.cpp
    util/symbol.cpp
    util/symbol_table.cpp
    util/symbol_table_base.cpp
    util/threeval.cpp
    util/timestamper.cpp
    util/typecheck.cpp
    util/ui_message.cpp
    util/union_find.cpp
    util/xml.cpp
    util/xml_irep.cpp)
list(TRANSFORM cbmc_sources PREPEND "${cbmc_src}/")

BISON_TARGET(VerilogParser "${hwcbmc_src}/verilog/parser.y"
    "${CMAKE_CURRENT_BINARY_DIR}/verilog_y.tab.cpp"
    DEFINES_FILE "${CMAKE_CURRENT_BINARY_DIR}/verilog_y.tab.h"
    COMPILE_FLAGS "-pyyverilog -Wno-error=other")
FLEX_TARGET(VerilogScanner "${hwcbmc_src}/verilog/scanner.l"
    "${CMAKE_CURRENT_BINARY_DIR}/verilog_lex.yy.cpp" COMPILE_FLAGS "-Pyyverilog")
FLEX_TARGET(VerilogPreprocessor "${hwcbmc_src}/verilog/verilog_preprocessor_tokenizer.l"
    "${CMAKE_CURRENT_BINARY_DIR}/verilog_preprocessor_lex.yy.cpp"
    COMPILE_FLAGS "-Pyyverilog_preprocessor")
ADD_FLEX_BISON_DEPENDENCY(VerilogScanner VerilogParser)

# verilog_scope.cpp includes the token header with quotes. Compile it beside
# Bison's output so a header left by the submodule's Make build cannot win.
configure_file("${hwcbmc_src}/verilog/verilog_scope.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/verilog_scope.cpp" COPYONLY)
add_library(hw_cbmc_frontend STATIC ${frontend_sources} ${cbmc_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/verilog_scope.cpp"
    "${VEVAL_SRC}/frontend/rtl/HwCbmcVerilogSynthesis.cpp"
    "${VEVAL_SRC}/util/process/RunFrontendCommand.cpp"
    "${VEVAL_SRC}/util/verilog/ProductExpression.cpp"
    ${BISON_VerilogParser_OUTPUTS} ${FLEX_VerilogScanner_OUTPUTS}
    ${FLEX_VerilogPreprocessor_OUTPUTS})
target_include_directories(hw_cbmc_frontend PUBLIC
    "${hwcbmc_src}" "${cbmc_src}" "${VEVAL_SRC}"
    PRIVATE "${hwcbmc_src}/verilog" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(hw_cbmc_frontend PUBLIC "LOCAL_IREP_IDS=<hw_cbmc_irep_ids.h>")
