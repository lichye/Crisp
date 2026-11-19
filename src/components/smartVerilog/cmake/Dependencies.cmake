# External dependencies are installed under .deps/ and otherTools/ by
# install.sh. No third-party source is committed to this repository.
#
# hw-cbmc: the Verilog frontend we link against and the EBMC binary we invoke.
#
set(SMART_DEPS_ROOT "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
    "Locally installed SMART dependencies")
set(HWCBMC_DIR "${CMAKE_SOURCE_DIR}/../hw-cbmc" CACHE PATH
    "Crisp's built hw-cbmc checkout")
get_filename_component(HWCBMC_DIR "${HWCBMC_DIR}" ABSOLUTE)
set(HWCBMC_CBMC_DIR ${HWCBMC_DIR}/lib/cbmc)

if(NOT EXISTS ${HWCBMC_CBMC_DIR}/src)
    message(FATAL_ERROR
        "hw-cbmc is missing under ${HWCBMC_DIR}. Run 'make ebmc' from the Crisp repository root.")
endif()

find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp
    HINTS ${SMART_DEPS_ROOT}/include)
if(NOT NLOHMANN_INCLUDE_DIR)
    message(FATAL_ERROR
        "nlohmann/json.hpp not found. Run 'make deps' from the Crisp repository root, or place it under "
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
    ${HWCBMC_CBMC_DIR}/src/json/json.a
    ${HWCBMC_DIR}/src/ebmc/ebmc_language.o
    ${HWCBMC_DIR}/src/ebmc/show_modules.o)

set(HWCBMC_EBMC ${HWCBMC_DIR}/src/ebmc/ebmc)

# The same EBMC build supplies the frontend objects and the solver executable.
foreach(artifact IN LISTS HWCBMC_LIBS HWCBMC_EBMC)
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR "Missing ${artifact}. Run 'make ebmc' from the Crisp repository root.")
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
