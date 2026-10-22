cmake_minimum_required(VERSION 3.11)
project(cxl_hash_join)

set(CMAKE_CXX_STANDARD 17)

# Checksum option (default: ON)
option(ENABLE_CHECKSUM "Enable Checksum for Bit-Exact Validation" ON)

if(ENABLE_CHECKSUM)
    add_definitions(-DENABLE_CHECKSUM -DENABLE_CHECKSUM_ON_BUILD_FLAG)
    message(STATUS "[Host] Build Mode: Checksum Enabled (Validation On)")
else()
    message(STATUS "[Host] Build Mode: Pure Performance Benchmark (Checksum Disabled)")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

set(SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmark.cpp"
)

set(include 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    /usr/local/include
)

include_directories(${include})
link_directories(/usr/local/lib /usr/lib/x86_64-linux-gnu /usr/lib)

if(DEFINED ENV{MU_LIB_PATH})
    include_directories($ENV{MU_LIB_PATH}/include)
    link_directories($ENV{MU_LIB_PATH}/lib)
endif()

set(shared_libs pthread)

find_library(JEMALLOC_LIB NAMES jemalloc HINTS /usr/lib/x86_64-linux-gnu /usr/local/lib /usr/lib)

if(JEMALLOC_LIB)
    message(STATUS "jemalloc found at: ${JEMALLOC_LIB}")
    list(APPEND shared_libs ${JEMALLOC_LIB})
else()
    message(FATAL_ERROR "jemalloc library (libjemalloc.so) not found! Please check container installation.")
endif()

if (NOT XCENA_SDK_CMAKE_BUILD)
    find_package(pxl REQUIRED)
    message(STATUS "PXL library found at: ${pxl_DIR}")
    list(APPEND shared_libs pxl::pxl)
else()
    list(APPEND shared_libs pxl)
endif()

set(HOST_COMPILE_OPTIONS -mclflushopt -march=native)

add_executable(hashjoin_host_dram ${SRC_FILES})
target_compile_definitions(hashjoin_host_dram PRIVATE MODE_HOST_DRAM)
target_compile_options(hashjoin_host_dram PRIVATE ${HOST_COMPILE_OPTIONS})
target_include_directories(hashjoin_host_dram PRIVATE ${include})
target_link_libraries(hashjoin_host_dram PRIVATE ${shared_libs})

add_executable(hashjoin_cxl_expander ${SRC_FILES})
target_compile_definitions(hashjoin_cxl_expander PRIVATE MODE_CXL_EXPANDER USE_CXL)
target_compile_options(hashjoin_cxl_expander PRIVATE ${HOST_COMPILE_OPTIONS})
target_include_directories(hashjoin_cxl_expander PRIVATE ${include})
target_link_libraries(hashjoin_cxl_expander PRIVATE ${shared_libs})

add_executable(hashjoin_cxl_hybrid ${SRC_FILES})
target_compile_definitions(hashjoin_cxl_hybrid PRIVATE MODE_CXL_HYBRID USE_CXL)
target_compile_options(hashjoin_cxl_hybrid PRIVATE ${HOST_COMPILE_OPTIONS})
target_include_directories(hashjoin_cxl_hybrid PRIVATE ${include})
target_link_libraries(hashjoin_cxl_hybrid PRIVATE ${shared_libs})

add_executable(hashjoin_cxl_full_nmc ${SRC_FILES})
target_compile_definitions(hashjoin_cxl_full_nmc PRIVATE MODE_CXL_FULL_NMC USE_CXL)
target_compile_options(hashjoin_cxl_full_nmc PRIVATE ${HOST_COMPILE_OPTIONS})
target_include_directories(hashjoin_cxl_full_nmc PRIVATE ${include})
target_link_libraries(hashjoin_cxl_full_nmc PRIVATE ${shared_libs})

# Mode 5: Host CPU + DRAM/CXL tiered placement (system-ram mode, libnuma)
find_library(NUMA_LIB NAMES numa)
if(NUMA_LIB)
    add_executable(hashjoin_host_tiered ${SRC_FILES})
    target_compile_definitions(hashjoin_host_tiered PRIVATE MODE_HOST_TIERED)
    target_compile_options(hashjoin_host_tiered PRIVATE ${HOST_COMPILE_OPTIONS})
    target_include_directories(hashjoin_host_tiered PRIVATE ${include})
    target_link_libraries(hashjoin_host_tiered PRIVATE ${shared_libs} ${NUMA_LIB})
else()
    message(WARNING "libnuma not found: hashjoin_host_tiered (Mode 5) will not be built")
endif()

include(ExternalProject)
ExternalProject_Add(build_mu_${PROJECT_NAME}
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ""
    CMAKE_ARGS 
        -DMU_LIB_PATH=$ENV{MU_LIB_PATH}
        -DMU_LLVM_PATH=$ENV{MU_LLVM_PATH}
        -DENABLE_CHECKSUM=${ENABLE_CHECKSUM}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
)