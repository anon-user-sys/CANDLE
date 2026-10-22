cmake_minimum_required(VERSION 3.14)
project(blink_tree_host CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host binaries go to the project root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Default compiler flags (host x86_64)
add_compile_options(-O3 -Wall -Wextra -pthread -march=native)

# Default library paths
link_directories(/usr/local/lib /usr/lib/x86_64-linux-gnu /usr/lib)

# PXL / CXL library paths
if(NOT DEFINED MU_LIB_PATH)
    set(MU_LIB_PATH $ENV{MU_LIB_PATH})
endif()

if(MU_LIB_PATH)
    include_directories(${MU_LIB_PATH}/include)
    link_directories(${MU_LIB_PATH}/lib)
endif()

# Locate the CXL kernel directory
set(MU_KERNEL_DIR "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel/CMakeLists.txt")
    set(MU_KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/CMakeLists.txt")
    set(MU_KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/kernel")
endif()

# Include paths for the host and the CXL kernel headers
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel
)

set(SHARED_LIBS pthread stdc++fs)

# Detect jemalloc
find_library(JEMALLOC_LIB NAMES jemalloc HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib /usr/lib)
if(JEMALLOC_LIB)
    message(STATUS "jemalloc found at: ${JEMALLOC_LIB}")
    list(APPEND SHARED_LIBS ${JEMALLOC_LIB})
endif()

# Find the PXL library package
if(NOT XCENA_SDK_CMAKE_BUILD)
    find_package(pxl REQUIRED)
    list(APPEND SHARED_LIBS pxl::pxl)
else()
    list(APPEND SHARED_LIBS pxl)
endif()

# ------------------------------------------------------------------------------
# Build the CXL RISC-V mu_kernel subproject automatically
# ------------------------------------------------------------------------------
include(ExternalProject)

if(MU_KERNEL_DIR)
    message(STATUS "[Xcena CXL] Found kernel directory at: ${MU_KERNEL_DIR}")
    ExternalProject_Add(build_mu_blink_tree
        SOURCE_DIR ${MU_KERNEL_DIR}
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ""
        CMAKE_ARGS 
            -DMU_LIB_PATH=$ENV{MU_LIB_PATH}
            -DMU_LLVM_PATH=$ENV{MU_LLVM_PATH}
            -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
    )
else()
    message(WARNING "[Xcena CXL] Kernel CMakeLists.txt not found.")
endif()

# Helper that creates a host executable
function(add_blink_binary name_suffix mode_macro extra_flag)
    set(target_name "blink_${name_suffix}")
    add_executable(${target_name} main.cpp)
    
    set(compile_defs ${mode_macro} ${extra_flag})
    
    if(NOT mode_macro STREQUAL "MODE_HOST_DRAM")
        list(APPEND compile_defs USE_CXL)
    endif()
    
        target_compile_definitions(${target_name} PRIVATE ${compile_defs})
    target_link_libraries(${target_name} PRIVATE ${SHARED_LIBS})

    # Build the kernel binary before the host binary
    if(TARGET build_mu_blink_tree)
        add_dependencies(${target_name} build_mu_blink_tree)
    endif()
endfunction()

# ------------------------------------------------------------------------------
# Host benchmark executable targets
# ------------------------------------------------------------------------------
# Mode 1: Host DRAM
add_blink_binary("host_dram_val" "MODE_HOST_DRAM" "ENABLE_VALIDATION")
add_blink_binary("host_dram_tp"  "MODE_HOST_DRAM" "BENCH_THROUGHPUT")
add_blink_binary("host_dram_lat" "MODE_HOST_DRAM" "MEASURE_LATENCY")

# Mode 2: Host CXL
add_blink_binary("host_cxl_val"  "MODE_HOST_CXL"  "ENABLE_VALIDATION")
add_blink_binary("host_cxl_tp"   "MODE_HOST_CXL"  "BENCH_THROUGHPUT")
add_blink_binary("host_cxl_lat"  "MODE_HOST_CXL"  "MEASURE_LATENCY")

# Mode 3: CXL Hybrid
add_blink_binary("cxl_hybrid_val" "MODE_CXL_HYBRID" "ENABLE_VALIDATION")
add_blink_binary("cxl_hybrid_tp"  "MODE_CXL_HYBRID" "BENCH_THROUGHPUT")
add_blink_binary("cxl_hybrid_lat" "MODE_CXL_HYBRID" "MEASURE_LATENCY")

# Mode 4: CXL Full Offload
add_blink_binary("cxl_full_val"  "MODE_CXL_FULL"  "ENABLE_VALIDATION")
add_blink_binary("cxl_full_tp"   "MODE_CXL_FULL"  "BENCH_THROUGHPUT")
add_blink_binary("cxl_full_lat"  "MODE_CXL_FULL"  "MEASURE_LATENCY")