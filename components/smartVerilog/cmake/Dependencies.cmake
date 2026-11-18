# External dependencies are installed under .deps/ and otherTools/ by
# install.sh. No third-party source is committed to this repository.
#
# hw-cbmc: the Verilog frontend we link against and the EBMC binary we invoke.
#
set(SMART_DEPS_ROOT "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
    "Locally installed SMART dependencies")
set(HWCBMC_DIR "${SMART_DEPS_ROOT}/hw-cbmc")
set(HWCBMC_CBMC_DIR ${HWCBMC_DIR}/lib/cbmc)

if(NOT EXISTS ${HWCBMC_CBMC_DIR}/src)
    message(FATAL_ERROR
        "hw-cbmc is not installed under ${HWCBMC_DIR}. Run ./install.sh, "
        "or pass -DSMART_DEPS_ROOT=<dependency-prefix>.")
endif()

find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp
    HINTS ${SMART_DEPS_ROOT}/include)
if(NOT NLOHMANN_INCLUDE_DIR)
    message(FATAL_ERROR
        "nlohmann/json.hpp not found. Run ./install.sh, or place it under "
        "${SMART_DEPS_ROOT}/include.")
endif()

# Link order matters and is resolved with --start-group below, but list the
# frontend first so the intent stays readable.
set(HWCBMC_LIBS
    ${HWCBMC_DIR}/src/verilog/verilog.a
    ${HWCBMC_DIR}/src/temporal-logic/temporal-logic.a
    ${HWCBMC_DIR}/src/trans-word-level/trans-word-level.a
    ${HWCBMC_CBMC_DIR}/src/langapi/langapi.a
    ${HWCBMC_CBMC_DIR}/src/solvers/solvers.a
    ${HWCBMC_CBMC_DIR}/src/util/util.a
    ${HWCBMC_CBMC_DIR}/src/big-int/big-int.a
    ${HWCBMC_CBMC_DIR}/src/json/json.a)

set(HWCBMC_EBMC ${HWCBMC_DIR}/src/ebmc/ebmc)

# install.sh builds a local copy. An existing installation may be shared
# read-only by configuring SMART_DEPS_ROOT.
foreach(artifact IN LISTS HWCBMC_LIBS HWCBMC_EBMC)
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR "Missing ${artifact}. Build the pinned hw-cbmc with ./install.sh or point SMART_DEPS_ROOT at a complete installation.")
    endif()
endforeach()

# What consumers link against.
add_library(hwcbmc_frontend INTERFACE)
# These are external headers from pinned hw-cbmc/CBMC. Treat them as system
# includes so project warning policies do not fail on upstream diagnostics.
target_include_directories(hwcbmc_frontend SYSTEM INTERFACE
    ${HWCBMC_DIR}/src
    ${HWCBMC_CBMC_DIR}/src)
# Mandatory: without this our irep ids disagree with the libraries'.
target_compile_definitions(hwcbmc_frontend INTERFACE
    "LOCAL_IREP_IDS=<hw_cbmc_irep_ids.h>")
target_link_libraries(hwcbmc_frontend INTERFACE
    -Wl,--start-group ${HWCBMC_LIBS} -Wl,--end-group)
