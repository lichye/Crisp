# Imported Verilog frontend and expression libraries; no embedded solver.
find_package(BISON 3.0 REQUIRED)
find_package(FLEX REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/Sources.cmake)

BISON_TARGET(VerilogParser verilog/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/verilog_y.tab.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/verilog_y.tab.h
    COMPILE_FLAGS "-pyyverilog -Wno-error=other")
FLEX_TARGET(VerilogScanner verilog/scanner.l
    ${CMAKE_CURRENT_BINARY_DIR}/verilog_lex.yy.cpp
    COMPILE_FLAGS "-Pyyverilog")
FLEX_TARGET(VerilogPreprocessor verilog/verilog_preprocessor_tokenizer.l
    ${CMAKE_CURRENT_BINARY_DIR}/verilog_preprocessor_lex.yy.cpp
    COMPILE_FLAGS "-Pyyverilog_preprocessor")
ADD_FLEX_BISON_DEPENDENCY(VerilogScanner VerilogParser)

add_library(hw_cbmc_frontend STATIC ${HW_CBMC_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/frontend/rtl/HwCbmcVerilogSynthesis.cpp
    ${PROJECT_SOURCE_DIR}/src/frontend/rtl/SynthesizeModules.cpp
    ${BISON_VerilogParser_OUTPUTS} ${FLEX_VerilogScanner_OUTPUTS}
    ${FLEX_VerilogPreprocessor_OUTPUTS})
target_include_directories(hw_cbmc_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/util/cbmc
    ${PROJECT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/verilog ${CMAKE_CURRENT_BINARY_DIR})
# All consumers must use the same expression identifier table.
target_compile_definitions(hw_cbmc_frontend PUBLIC
    "LOCAL_IREP_IDS=<hw_cbmc_irep_ids.h>")
