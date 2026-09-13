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

# LLDB C++ API library
file(GLOB LLVM_LIB_HINTS "/usr/lib/llvm-*/lib")
find_library(LLDB_LIB NAMES lldb liblldb lldb-22 lldb-21 lldb-20 lldb-19 lldb-18
    HINTS ${LLVM_LIBRARY_DIRS} ${LLVM_LIB_HINTS}
)
if (LLDB_LIB)
    message(STATUS "Found LLDB library: ${LLDB_LIB}")
else()
    message(WARNING "LLDB library not found; linking without it.")
    set(LLDB_LIB "")
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
# Git commit hash for version reporting
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE OCLDBG_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if (NOT OCLDBG_GIT_COMMIT)
    set(OCLDBG_GIT_COMMIT "unknown")
endif()

# -----------------------------------------------------------------------------
# Core debugger library
# -----------------------------------------------------------------------------
add_library(ocldbg_core STATIC
    ocldbg/DebuggerContext.cpp
    ocldbg/args.cpp
    ocldbg/cli.cpp
    ocldbg/dwarf/DWARFSourceModel.cpp
    ocldbg/dwarf/LocationExprEval.cpp
    ocldbg/ocl_debug_model/OCLVariableResolver.cpp
    ocldbg/backends/cpu/PoclCPUBackend.cpp
    ocldbg/backends/cpu/WIContextExtractor.cpp
    ocldbg/backends/cpu/WorkGroupTracker.cpp
    ocldbg/backends/cpu/x86_64/X86_64CPUABI.cpp
    ocldbg/backends/oclgrind/OclgrindBackend.cpp
    ocldbg/dap/DAPServer.cpp
)
target_compile_definitions(ocldbg_core PRIVATE
    OCLDBG_GIT_COMMIT="${OCLDBG_GIT_COMMIT}"
    OCLDBG_VERSION="${PROJECT_VERSION}"
)
target_include_directories(ocldbg_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ocldbg
)
target_link_libraries(ocldbg_core PUBLIC
    Threads::Threads
    ${LLVM_LIBS}
    ${LLDB_LIB}
    ${CMAKE_DL_LIBS}
)

add_executable(ocldbg
    ocldbg/main.cpp
)
target_compile_definitions(ocldbg PRIVATE
    OCLDBG_GIT_COMMIT="${OCLDBG_GIT_COMMIT}"
    OCLDBG_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(ocldbg PRIVATE
    ocldbg_core
    Threads::Threads
    ${LLVM_LIBS}
    ${LLDB_LIB}
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
file(GLOB LLVM_PATH_HINTS "/usr/lib/llvm-*/bin")

find_program(LLVM_LIT_BIN NAMES lit lit.py llvm-lit
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_PATH_HINTS}
)
find_program(FILECHECK_BIN NAMES FileCheck FileCheck-20 FileCheck-19 FileCheck-18 FileCheck-17 FileCheck-16 FileCheck-15
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_PATH_HINTS}
)
find_program(LLVM_DIS_BIN NAMES llvm-dis llvm-dis-20 llvm-dis-19 llvm-dis-18 llvm-dis-17 llvm-dis-16 llvm-dis-15
    HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_PATH_HINTS}
)

if (LLVM_LIT_BIN AND FILECHECK_BIN AND OpenCL_FOUND)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/tests/lit.site.cfg.py
        @ONLY
    )
    add_custom_target(runtests
        COMMAND ${LLVM_LIT_BIN} -v ${CMAKE_CURRENT_BINARY_DIR}/tests
        DEPENDS test_host_runner ocldbg
        COMMENT "Running LIT test suite for segfault26"
    )
    add_custom_target(check-lit DEPENDS runtests)
else()
    add_custom_target(runtests
        COMMAND ${CMAKE_COMMAND} -E echo "Error: LIT test prerequisites missing (LIT='${LLVM_LIT_BIN}', FileCheck='${FILECHECK_BIN}', OpenCL='${OpenCL_FOUND}')"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "LIT test suite cannot run because required tools are missing"
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
