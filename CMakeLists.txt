cmake_minimum_required(VERSION 3.20)
project(segfault26_ocldbg VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Threading is required across all modules
find_package(Threads REQUIRED)

# LLVM configuration (for DWARFSourceModel and object file inspection)
find_package(LLVM CONFIG QUIET)
if (LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
    include_directories(${LLVM_INCLUDE_DIRS})
    separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
    add_definitions(${LLVM_DEFINITIONS_LIST})
    if (TARGET LLVM)
        set(LLVM_LIBS LLVM)
    else()
        llvm_map_components_to_libnames(LLVM_LIBS core debuginfodwarf object support)
    endif()
else()
    message(WARNING "LLVM development package not found via CMake CONFIG; building with stub DWARF layer.")
    set(LLVM_LIBS "")
endif()

# OpenCL configuration (for test kernels and validation runner)
find_package(OpenCL QUIET)
if (OpenCL_FOUND)
    message(STATUS "Found OpenCL headers and ICD: ${OpenCL_LIBRARIES}")
else()
    message(WARNING "OpenCL headers/ICD not found via CMake; install ocl-icd and opencl-headers.")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ocldbg
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_inject
)

# -----------------------------------------------------------------------------
# Runtime injection library (LD_PRELOAD shim for pocl work-group tracking)
# Owner: Person C
# -----------------------------------------------------------------------------
add_library(ocldbg_rt SHARED
    runtime_inject/wg_dispatch_hook.cpp
    runtime_inject/wi_state_export.cpp
)
target_include_directories(ocldbg_rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_inject
)
target_link_libraries(ocldbg_rt PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
    rt
)
set_target_properties(ocldbg_rt PROPERTIES
    OUTPUT_NAME "ocldbg_rt"
    PREFIX "lib"
)

# -----------------------------------------------------------------------------
# Core debugger library
# -----------------------------------------------------------------------------
add_library(ocldbg_core STATIC
    ocldbg/dwarf/DWARFSourceModel.cpp
    ocldbg/dwarf/LocationExprEval.cpp
    ocldbg/ocl_debug_model/OCLVariableResolver.cpp
    ocldbg/backends/cpu/PoclCPUBackend.cpp
    ocldbg/backends/cpu/WIContextExtractor.cpp
    ocldbg/backends/cpu/WorkGroupTracker.cpp
    ocldbg/backends/oclgrind/OclgrindBackend.cpp
    ocldbg/dap/DAPServer.cpp
)
target_include_directories(ocldbg_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ocldbg
)
target_link_libraries(ocldbg_core PUBLIC
    Threads::Threads
    ${LLVM_LIBS}
    ${CMAKE_DL_LIBS}
)

# -----------------------------------------------------------------------------
# ocldbg CLI / DAP Executable
# Owner: Person E
# -----------------------------------------------------------------------------
add_executable(ocldbg
    ocldbg/main.cpp
)
target_link_libraries(ocldbg PRIVATE
    ocldbg_core
    Threads::Threads
    ${LLVM_LIBS}
)

# -----------------------------------------------------------------------------
# Optional host runner test binary if OpenCL is present
# Owner: Person F
# -----------------------------------------------------------------------------
if (OpenCL_FOUND)
    add_executable(test_host_runner
        tests/host_runner.c
    )
    target_link_libraries(test_host_runner PRIVATE
        OpenCL::OpenCL
    )
endif()

# -----------------------------------------------------------------------------
# LLVM LIT Test Suite
# -----------------------------------------------------------------------------
find_program(LLVM_LIT_BIN NAMES lit llvm-lit)
find_program(FILECHECK_BIN NAMES FileCheck)

if (LLVM_LIT_BIN AND FILECHECK_BIN AND OpenCL_FOUND)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg.py
        @ONLY
    )
    add_custom_target(runtests
        COMMAND ${LLVM_LIT_BIN} -v ${CMAKE_CURRENT_BINARY_DIR}/tests
        DEPENDS test_host_runner
        COMMENT "Running LIT test suite for segfault26"
    )
    add_custom_target(check-lit DEPENDS runtests)
endif()

# -----------------------------------------------------------------------------
# Formatting and Clang-Tidy targets
# -----------------------------------------------------------------------------
find_program(CLANG_FORMAT_BIN NAMES clang-format)
if (CLANG_FORMAT_BIN)
    file(GLOB_RECURSE CXX_FORMAT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ocldbg/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ocldbg/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime_inject/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime_inject/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_BIN} -i ${CXX_FORMAT_SOURCES}
        COMMENT "Formatting all C/C++ source files with clang-format"
    )
    add_custom_target(check-format
        COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${CXX_FORMAT_SOURCES}
        COMMENT "Verifying code formatting with clang-format"
    )
endif()

find_program(RUN_CLANG_TIDY_BIN NAMES run-clang-tidy run-clang-tidy.py)
if (RUN_CLANG_TIDY_BIN)
    add_custom_target(tidy
        COMMAND ${RUN_CLANG_TIDY_BIN} -quiet -p ${CMAKE_BINARY_DIR}
        COMMENT "Running clang-tidy across all translation units"
    )
endif()
