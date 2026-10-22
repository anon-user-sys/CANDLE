cmake_minimum_required(VERSION 3.11)
project(mu_kernel)

option(ENABLE_CHECKSUM "Enable Checksum for Bit-Exact Validation" ON)

if(ENABLE_CHECKSUM)
    message(STATUS "[mu_kernel] Build Mode: Checksum Enabled (-DENABLE_CHECKSUM)")
else()
    message(STATUS "[mu_kernel] Build Mode: Pure Performance Benchmark (No Checksum)")
endif()

set(CMAKE_C_COMPILER ${MU_LLVM_PATH}/bin/clang)
set(CMAKE_CXX_COMPILER ${MU_LLVM_PATH}/bin/clang)

set(output_path ${CMAKE_CURRENT_SOURCE_DIR})

set(include_directories
    ${MU_LLVM_PATH}/picolibc-rv
    ${MU_LLVM_PATH}/picolibc-rv/include
    ${MU_LLVM_PATH}/libcxx-rv/include/c++/v1
    ${MU_LIB_PATH}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

set(link_directories
    ${MU_LLVM_PATH}/builtins-rv/lib/linux
    ${MU_LLVM_PATH}/libcxx-rv/lib
    ${MU_LLVM_PATH}/libcxxabi-rv/lib
    ${MU_LLVM_PATH}/picolibc-rv/newlib
    ${MU_LIB_PATH}/lib
)

# -mcpu=metisx-d keeps the xmetis extension, which mu_startup.s (csrget) needs. Do not add a
# -march that narrows it.
set(compile_options
    --target=riscv64
    -mcpu=metisx-d
    -ffreestanding
    -nostdlib
    -nodefaultlibs
    -fno-exceptions
    -Werror=return-type
    -Wno-atomic-alignment
    -Qunused-arguments
    -g
    -O3
    -std=c++17
    -fno-rtti
    -ffunction-sections
    -fdata-sections
)

if(ENABLE_CHECKSUM)
    list(APPEND compile_options -DENABLE_CHECKSUM -DENABLE_CHECKSUM_ON_BUILD_FLAG)
endif()

set(link_options
    --target=riscv64
    -mcpu=metisx-d
    -nostdlib
    -nodefaultlibs
    -Qunused-arguments
    -lclang_rt.builtins-riscv64
    -z max-page-size=4
    -lc
    -lc++
    -lc++abi
    -Wl,--gc-sections
    --ld-path=${MU_LLVM_PATH}/bin/ld.lld
)

set(start_up ${MU_LIB_PATH}/script/mu_startup.s)
set(linker ${MU_LIB_PATH}/script/mu_linker.ld)

add_executable(mu_kernel_build.mubin cxl_build_kernel.cpp)
target_include_directories(mu_kernel_build.mubin PRIVATE ${include_directories})
target_link_libraries(mu_kernel_build.mubin PRIVATE mu_std)
target_link_directories(mu_kernel_build.mubin PRIVATE ${link_directories})
target_compile_options(mu_kernel_build.mubin PRIVATE ${compile_options})
target_link_options(mu_kernel_build.mubin PRIVATE ${link_options} ${start_up} ${linker})
set_target_properties(mu_kernel_build.mubin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${output_path})

add_executable(mu_kernel_probe.mubin cxl_probe_kernel.cpp)
target_include_directories(mu_kernel_probe.mubin PRIVATE ${include_directories})
target_link_libraries(mu_kernel_probe.mubin PRIVATE mu_std)
target_link_directories(mu_kernel_probe.mubin PRIVATE ${link_directories})
target_compile_options(mu_kernel_probe.mubin PRIVATE ${compile_options})
target_link_options(mu_kernel_probe.mubin PRIVATE ${link_options} ${start_up} ${linker})
set_target_properties(mu_kernel_probe.mubin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${output_path})