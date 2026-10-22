cmake_minimum_required(VERSION 3.11)
project(coherence_tax CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pxl REQUIRED)
find_package(Threads REQUIRED)

add_executable(coh main.cpp)
target_include_directories(coh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# AVX2 only: -mavx512f SIGILLs on this host (memory: server_numa_cxl_topology).
target_compile_options(coh PRIVATE -O2 -mavx2 -mclflushopt -Wall -Wextra)
target_link_libraries(coh pxl::pxl Threads::Threads)
install(TARGETS coh RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

include(ExternalProject)
ExternalProject_Add(build_mu_coherence_tax
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    BUILD_ALWAYS TRUE
    CMAKE_ARGS
        -DMU_LIB_PATH=$ENV{MU_LIB_PATH}
        -DMU_LLVM_PATH=$ENV{MU_LLVM_PATH}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
)
