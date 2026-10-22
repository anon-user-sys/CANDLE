cmake_minimum_required(VERSION 3.11)
project(mu_kernel)

set(CMAKE_C_COMPILER ${MU_LLVM_PATH}/bin/clang)
set(CMAKE_CXX_COMPILER ${MU_LLVM_PATH}/bin/clang)

# Put the .mubin binary inside the mu_kernel folder
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

# Compile options
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

# Link options
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

# Combined kernel target

set(start_up ${MU_LIB_PATH}/script/mu_startup.s)
set(linker ${MU_LIB_PATH}/script/mu_linker.ld)

# Combined CXL kernel target (mu_blink_tree.mubin)
add_executable(mu_blink_tree.mubin cxl_kernel.cpp)
target_include_directories(mu_blink_tree.mubin PRIVATE ${include_directories})
target_link_libraries(mu_blink_tree.mubin PRIVATE mu_std)
target_link_directories(mu_blink_tree.mubin PRIVATE ${link_directories})
target_compile_options(mu_blink_tree.mubin PRIVATE ${compile_options})
target_link_options(mu_blink_tree.mubin PRIVATE ${link_options} ${start_up} ${linker})
set_target_properties(mu_blink_tree.mubin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${output_path})