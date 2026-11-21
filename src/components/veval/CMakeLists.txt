cmake_minimum_required(VERSION 3.16)
project(veval CXX)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(VEVAL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
include(cmake/HwCbmc.cmake)
target_link_libraries(hw_cbmc_frontend PUBLIC Threads::Threads)

# --- one library per role ---------------------------------------------------
add_library(veval_frontend STATIC
    ${VEVAL_SRC}/frontend/rtl/ElaboratedDesign.cpp
    ${VEVAL_SRC}/util/verilog/ExpressionPrinter.cpp
    ${VEVAL_SRC}/frontend/input/ReplaceDontCareConstants.cpp)
target_link_libraries(veval_frontend PUBLIC hw_cbmc_frontend)
target_include_directories(veval_frontend PUBLIC ${VEVAL_SRC})

include(CTest)
if(BUILD_TESTING)
    add_executable(veval_nested_array_writes_test test/NestedArrayWrites.cpp)
    target_link_libraries(veval_nested_array_writes_test PRIVATE veval_frontend)
    foreach(top IN ITEMS dynamic_word_bit dynamic_row_column fixed_column
                         combinational_bit out_of_bounds_bit)
        add_test(NAME veval.array_write.${top} COMMAND veval_nested_array_writes_test
            "${top}" "${CMAKE_CURRENT_SOURCE_DIR}/test/nested_array_writes.sv")
    endforeach()
    add_executable(veval_mixed_array_drivers_test test/MixedArrayDrivers.cpp)
    target_link_libraries(veval_mixed_array_drivers_test PRIVATE veval_frontend)
    foreach(top IN ITEMS mixed mixed_comb static_outer_prefix)
        add_test(NAME veval.array_drivers.${top} COMMAND veval_mixed_array_drivers_test
            "${top}" "${CMAKE_CURRENT_SOURCE_DIR}/test/mixed_array_drivers.sv")
    endforeach()
    foreach(top IN ITEMS overlap dynamic_overlap)
        add_test(NAME veval.array_drivers.${top}
            COMMAND "${CMAKE_COMMAND}" "-DVEVAL=$<TARGET_FILE:veval>"
                "-DTOP=${top}" "-DDESIGN=${CMAKE_CURRENT_SOURCE_DIR}/test/mixed_array_drivers.sv"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/test/RejectMixedArrayDrivers.cmake")
    endforeach()
    set(veval_test_ebmc "${hwcbmc_src}/ebmc/ebmc")
    if(EXISTS "${veval_test_ebmc}")
        add_test(NAME veval.aggregate_port_product
            COMMAND "${CMAKE_COMMAND}" "-DVEVAL=$<TARGET_FILE:veval>"
                "-DEBMC=${veval_test_ebmc}" "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/test"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/test/AggregatePortProduct.cmake")
    endif()
    if(EXISTS "${veval_test_ebmc}")
        add_test(NAME veval.aggregate_function_product
            COMMAND "${CMAKE_COMMAND}" "-DVEVAL=$<TARGET_FILE:veval>"
                "-DEBMC=${veval_test_ebmc}" "-DDESIGN=${CMAKE_CURRENT_SOURCE_DIR}/test/aggregate_function.sv"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/test/AggregateFunctionProduct.cmake")
    endif()
    add_executable(veval_dual_port_ram_test test/DualPortRam.cpp)
    target_link_libraries(veval_dual_port_ram_test PRIVATE veval_frontend)
    add_test(NAME veval.dual_port_ram COMMAND veval_dual_port_ram_test
        "${CMAKE_CURRENT_SOURCE_DIR}/test/dual_port_ram.sv")
    if(EXISTS "${veval_test_ebmc}")
        add_test(NAME veval.dual_port_ram_ebmc
            COMMAND "${CMAKE_COMMAND}" "-DEBMC=${veval_test_ebmc}"
                "-DDESIGN=${CMAKE_CURRENT_SOURCE_DIR}/test/dual_port_ram.sv"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/test/DualPortRamEbmc.cmake")
    endif()
endif()

add_library(veval_sva STATIC ${VEVAL_SRC}/frontend/sva/PropertyMonitor.cpp
    ${VEVAL_SRC}/frontend/sva/LocalVariableMonitor.cpp
    ${VEVAL_SRC}/frontend/sva/ClockSampling.cpp
    ${VEVAL_SRC}/frontend/sva/SafetyMonitor.cpp)
target_link_libraries(veval_sva PUBLIC veval_frontend)

add_library(veval_lowering STATIC
    ${VEVAL_SRC}/lowering/Symbolic/BitRewriting.cpp
    ${VEVAL_SRC}/lowering/Symbolic/PropertySubstitution.cpp
    ${VEVAL_SRC}/lowering/Symbolic/CombinationalDefinitions.cpp
    ${VEVAL_SRC}/lowering/Symbolic/SignalAliases.cpp
    ${VEVAL_SRC}/lowering/connectivity/ConnectionMap.cpp
    ${VEVAL_SRC}/lowering/environment/AssumptionDependencies.cpp
    ${VEVAL_SRC}/lowering/environment/CopyPortAssumptions.cpp
    ${VEVAL_SRC}/lowering/output/WriteLocalSpecs.cpp
    ${VEVAL_SRC}/lowering/propagation/LandingEligibility.cpp
    ${VEVAL_SRC}/lowering/propagation/PropertyLowering.cpp
    ${VEVAL_SRC}/lowering/connectivity/SignalReachability.cpp
    ${VEVAL_SRC}/lowering/reports/ReachabilityReport.cpp
    ${VEVAL_SRC}/lowering/reports/ExportHierarchyGraph.cpp)
target_link_libraries(veval_lowering PUBLIC veval_frontend veval_sva Threads::Threads)

# Shared linear JSON parser for lowering records and engine receipts.
add_library(veval_json STATIC ${VEVAL_SRC}/util/database/Json.cpp)
target_link_libraries(veval_json PUBLIC hw_cbmc_frontend)
target_include_directories(veval_json PUBLIC ${VEVAL_SRC})
# Environment and annotation support does not run a model checker.
add_library(veval_environment STATIC
    ${VEVAL_SRC}/lowering/environment/LocalEnvironment.cpp
    ${VEVAL_SRC}/lowering/LowerDesign.cpp
    ${VEVAL_SRC}/util/database/SavedVerdicts.cpp
    ${VEVAL_SRC}/lowering/reports/VerdictReport.cpp)
target_link_libraries(veval_environment PUBLIC veval_frontend veval_sva
    veval_lowering veval_json)

# --- executables ------------------------------------------------------------
add_library(veval_check_inputs STATIC
    ${VEVAL_SRC}/util/database/LoweringDatabase.cpp
    ${VEVAL_SRC}/check/prepare/ReadLocalSpec.cpp)
target_link_libraries(veval_check_inputs PUBLIC hw_cbmc_frontend veval_json ${SQLite3_LIBRARIES})
target_include_directories(veval_check_inputs PUBLIC ${SQLite3_INCLUDE_DIRS} ${VEVAL_SRC})
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_executable(veval_extract_subtree ${VEVAL_SRC}/util/verilog/ExtractModuleSubtree.cpp)
target_include_directories(veval_extract_subtree PRIVATE ${VEVAL_SRC})
target_link_libraries(veval_extract_subtree PRIVATE OpenSSL::Crypto)
add_library(veval_verification STATIC ${VEVAL_SRC}/util/task/CommandArgumentsAndFiles.cpp
    ${VEVAL_SRC}/util/process/RunWithResourceLimits.cpp ${VEVAL_SRC}/util/sby/SbyHarness.cpp
    ${VEVAL_SRC}/util/sby/SbyRunner.cpp ${VEVAL_SRC}/check/prepare/JobSchema.cpp)
target_link_libraries(veval_verification PUBLIC veval_check_inputs OpenSSL::Crypto)
add_executable(consflow_jobs ${VEVAL_SRC}/check/prepare/GenerateJobs.cpp
    ${VEVAL_SRC}/check/prepare/AuditJobEnvironment.cpp ${VEVAL_SRC}/check/prepare/ProjectEnvironment.cpp)
target_link_libraries(consflow_jobs PRIVATE veval_verification)
add_executable(consflow_sby ${VEVAL_SRC}/util/sby/SbyCommand.cpp)
target_link_libraries(consflow_sby PRIVATE veval_verification)
add_executable(consflow_engine ${VEVAL_SRC}/check/run/RunJob.cpp)
target_link_libraries(consflow_engine PRIVATE veval_verification)
add_executable(consflow_constants ${VEVAL_SRC}/check/prepare/ResolveEnumConstants.cpp)
target_link_libraries(consflow_constants PRIVATE veval_verification hw_cbmc_frontend)
add_executable(consflow_process ${VEVAL_SRC}/util/cli/ProcessSupervisorCli.cpp)
target_link_libraries(consflow_process PRIVATE veval_verification)
target_compile_definitions(consflow_engine PRIVATE VEVAL_VERIFICATION_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
add_library(veval_cli STATIC
    ${VEVAL_SRC}/util/cli/GenerationConfig.cpp
    ${VEVAL_SRC}/util/database/LoweringDatabaseWriter.cpp
    ${VEVAL_SRC}/util/cli/ParseArguments.cpp
    ${VEVAL_SRC}/util/cli/PrintHelp.cpp
    ${VEVAL_SRC}/util/cli/SpecSelectionAndDiagnostics.cpp
    ${VEVAL_SRC}/util/cli/InspectionCommands.cpp
    ${VEVAL_SRC}/util/task/WriteGenerationMetadata.cpp
    ${VEVAL_SRC}/lowering/RunLowering.cpp
    ${VEVAL_SRC}/lowering/output/ExportLoweringResults.cpp)
target_link_libraries(veval_cli PUBLIC
    veval_lowering veval_sva veval_environment veval_check_inputs veval_verification)
add_executable(veval ${VEVAL_SRC}/main.cpp)
add_dependencies(veval consflow_jobs consflow_engine consflow_sby consflow_constants consflow_process)
target_link_libraries(veval PRIVATE veval_cli)

# The suite builds its driver against the same database and process libraries.
if(CRISP_DRIVER_SOURCE_DIR)
    add_subdirectory("${CRISP_DRIVER_SOURCE_DIR}" crisp)
endif()

# Large elaborated designs allocate many small immutable expression nodes.
# Opt in explicitly; only the veval executable uses this allocator. In
# particular, do not preload it into external solvers or Yosys plugins.
option(VEVAL_USE_JEMALLOC "Link veval with jemalloc for large designs" OFF)
if(VEVAL_USE_JEMALLOC)
    find_library(VEVAL_JEMALLOC_LIBRARY NAMES jemalloc)
    if(NOT VEVAL_JEMALLOC_LIBRARY)
        message(FATAL_ERROR "VEVAL_USE_JEMALLOC requires libjemalloc (development package)")
    endif()
    target_link_libraries(veval PRIVATE ${VEVAL_JEMALLOC_LIBRARY})
endif()
add_library(veval_commands STATIC ${VEVAL_SRC}/util/process/RunExternalCommand.cpp)
target_include_directories(veval_commands PUBLIC ${VEVAL_SRC})
add_library(veval_workflow STATIC
    ${VEVAL_SRC}/lowering/LoweringStore.cpp
    src/util/task/TaskInputAndExecution.cpp src/util/process/SuperviseStage.cpp src/util/cli/ReadSbyTask.cpp
    src/frontend/input/PrepareSpecSources.cpp src/check/CheckSpecCommand.cpp src/check/run/RunPropertyChecks.cpp src/util/task/RunProgressLog.cpp src/check/results/CheckReport.cpp)
target_link_libraries(veval_workflow PUBLIC veval_check_inputs veval_lowering veval_commands veval_verification)
target_compile_definitions(veval_workflow PRIVATE VEVAL_WORKFLOW_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(veval_workflow PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(veval PRIVATE veval_workflow)
add_executable(consflow_check ${VEVAL_SRC}/check/CheckSavedLowering.cpp)
target_link_libraries(consflow_check PRIVATE veval_workflow veval_verification)
# Exact identifier boundary for the legacy SBY orchestration shell.
add_executable(consflow_yosys_identifier ${VEVAL_SRC}/util/yosys/YosysIdentifier.cpp)
add_dependencies(veval consflow_yosys_identifier)
add_executable(consflow_sva_monitor ${VEVAL_SRC}/frontend/sva/CompileSvaMonitor.cpp)
target_link_libraries(consflow_sva_monitor PRIVATE veval_sva)
add_dependencies(veval consflow_sva_monitor)
add_executable(consflow_array_observation ${VEVAL_SRC}/util/sby/ReadArrayTrace.cpp)
target_link_libraries(consflow_array_observation PRIVATE veval_check_inputs)
add_dependencies(veval consflow_array_observation)
add_executable(consflow_live_trace ${VEVAL_SRC}/util/sby/ReadLivenessTrace.cpp)
add_dependencies(veval consflow_live_trace)
set(suite_bin "$ENV{OSS_CAD_BIN}")
set(bundled_suite "${CMAKE_CURRENT_SOURCE_DIR}/../smartVerilog/.deps/oss-cad-suite/bin")
if(NOT suite_bin AND EXISTS "${bundled_suite}/yosys-config")
    set(suite_bin "${bundled_suite}")
endif()
if(suite_bin)
    set(VEVAL_YOSYS_CONFIG "${suite_bin}/yosys-config")
else()
    find_program(VEVAL_YOSYS_CONFIG yosys-config PATHS "$ENV{HOME}/.local/oss-cad-suite/bin")
endif()
if(VEVAL_YOSYS_CONFIG)
    set(VEVAL_BUILD_YOSYS_CONFIG "${VEVAL_YOSYS_CONFIG}" CACHE INTERNAL "Yosys used to build the plugins")
    execute_process(COMMAND "${VEVAL_YOSYS_CONFIG}" --datdir
        OUTPUT_VARIABLE VEVAL_YOSYS_DATDIR OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE VEVAL_YOSYS_CONFIG_STATUS)
    if(NOT VEVAL_YOSYS_CONFIG_STATUS EQUAL 0)
        message(FATAL_ERROR "Cannot query ${VEVAL_YOSYS_CONFIG} --datdir. Use yosys-config from the Yosys installation that will run veval's plugins.")
    endif()
endif()
if(suite_bin)
    set(VEVAL_YOSYS_INCLUDE "${VEVAL_YOSYS_DATDIR}/include")
else()
    find_path(VEVAL_YOSYS_INCLUDE kernel/yosys.h
        HINTS "${VEVAL_YOSYS_DATDIR}/include" "$ENV{HOME}/.local/oss-cad-suite/share/yosys/include")
endif()
if(NOT VEVAL_YOSYS_INCLUDE OR NOT EXISTS "${VEVAL_YOSYS_INCLUDE}/kernel/yosys.h")
    message(FATAL_ERROR
        "Missing Yosys plugin headers under ${VEVAL_YOSYS_INCLUDE}. Run 'make deps' from the Crisp root "
        "or set OSS_CAD_BIN to a complete suite. For a standalone header installation, "
        "configure with -DVEVAL_YOSYS_INCLUDE=/path/to/yosys/include.")
endif()
message(STATUS "Yosys plugin headers: ${VEVAL_YOSYS_INCLUDE}")
if(VEVAL_YOSYS_INCLUDE)
    add_library(consflow_observation MODULE ${VEVAL_SRC}/util/yosys/ExposePropertySignalsPass.cpp)
    target_include_directories(consflow_observation PRIVATE ${VEVAL_YOSYS_INCLUDE})
    target_compile_definitions(consflow_observation PRIVATE _YOSYS_)
    set_target_properties(consflow_observation PROPERTIES PREFIX "")
    add_dependencies(veval consflow_observation)
    add_library(consflow_live2safety MODULE ${VEVAL_SRC}/util/yosys/LivenessToSafetyPass.cpp)
    target_include_directories(consflow_live2safety PRIVATE ${VEVAL_YOSYS_INCLUDE})
    target_compile_definitions(consflow_live2safety PRIVATE _YOSYS_)
    set_target_properties(consflow_live2safety PROPERTIES PREFIX "")
    add_dependencies(veval consflow_live2safety)
    add_library(consflow_environment MODULE ${VEVAL_SRC}/util/yosys/AuditMissingEnvironmentPass.cpp)
    target_include_directories(consflow_environment PRIVATE ${VEVAL_YOSYS_INCLUDE})
    target_compile_definitions(consflow_environment PRIVATE _YOSYS_)
    set_target_properties(consflow_environment PROPERTIES PREFIX "")
    add_dependencies(consflow_jobs consflow_environment)
    add_library(consflow_relations MODULE ${VEVAL_SRC}/util/yosys/EncodeSignalRelationsPass.cpp)
    target_include_directories(consflow_relations PRIVATE ${VEVAL_YOSYS_INCLUDE})
    target_compile_definitions(consflow_relations PRIVATE _YOSYS_)
    set_target_properties(consflow_relations PROPERTIES PREFIX "")
    add_dependencies(veval consflow_relations)
    # The C++20 requirement arrived before the Twine API. Check the selected
    # headers so older Linux Yosys installations can still build with C++17.
    set(VEVAL_YOSYS_REQUIRES_CXX20 "")
    if(EXISTS "${VEVAL_YOSYS_INCLUDE}/kernel/yosys_common.h")
        file(STRINGS "${VEVAL_YOSYS_INCLUDE}/kernel/yosys_common.h" VEVAL_YOSYS_REQUIRES_CXX20
            REGEX "C[+][+]20 or later compatible compiler is required")
    endif()
    get_filename_component(yosys_bin_dir "${VEVAL_YOSYS_CONFIG}" DIRECTORY)
    foreach(plugin consflow_observation consflow_live2safety consflow_environment consflow_relations)
        if(NOT CMAKE_CROSSCOMPILING AND EXISTS "${yosys_bin_dir}/yosys")
            add_custom_command(TARGET ${plugin} POST_BUILD
                COMMAND "${yosys_bin_dir}/yosys" -Q -T -q -m "$<TARGET_FILE:${plugin}>" -p stat
                COMMENT "Loading ${plugin} in the selected Yosys" VERBATIM)
        endif()
        if(VEVAL_YOSYS_REQUIRES_CXX20 OR EXISTS "${VEVAL_YOSYS_INCLUDE}/kernel/twine.h")
            set_target_properties(${plugin} PROPERTIES CXX_STANDARD 20)
        endif()
        if(EXISTS "${VEVAL_YOSYS_INCLUDE}/kernel/twine.h")
            target_compile_definitions(${plugin} PRIVATE VEVAL_YOSYS_TWINE)
        endif()
        if(APPLE)
            set_target_properties(${plugin} PROPERTIES SUFFIX ".so")
            target_link_options(${plugin} PRIVATE -undefined dynamic_lookup)
        endif()
    endforeach()
endif()
