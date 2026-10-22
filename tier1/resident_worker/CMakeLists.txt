cmake_minimum_required(VERSION 3.11)
project(resident_worker)

find_package(pxl REQUIRED)
find_package(Threads REQUIRED)

add_executable(main_spike main_spike.cpp)
target_include_directories(main_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(main_spike pxl::pxl Threads::Threads)
install(TARGETS main_spike RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

include(ExternalProject)
ExternalProject_Add(build_mu_spike
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mu_kernel
    BUILD_ALWAYS TRUE
    CMAKE_ARGS
        -DMU_LIB_PATH=$ENV{MU_LIB_PATH}
        -DMU_LLVM_PATH=$ENV{MU_LLVM_PATH}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}
)
