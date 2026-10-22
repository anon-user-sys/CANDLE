cmake_minimum_required(VERSION 3.11)
project(tpch_scan_filter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pxl REQUIRED)
find_package(Threads REQUIRED)

add_executable(main main.cpp)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(main PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(main pxl::pxl Threads::Threads numa)
add_executable(bw bw.cpp)
target_include_directories(bw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bw PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(bw pxl::pxl Threads::Threads)
add_executable(d2h d2h.cpp)
target_include_directories(d2h PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(d2h PRIVATE -O3 -march=native -Wall -Wextra)
target_link_libraries(d2h pxl::pxl Threads::Threads)
install(TARGETS main bw d2h RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

include(ExternalProject)
ExternalProject_Add(build_mu_tpch_scan
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    BUILD_ALWAYS TRUE
    CMAKE_ARGS
        -DMU_LIB_PATH=$ENV{MU_LIB_PATH}
        -DMU_LLVM_PATH=$ENV{MU_LLVM_PATH}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
)
