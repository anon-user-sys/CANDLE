cmake_minimum_required(VERSION 3.11)
project(data_movement_tax)

set(src main.cpp)
set(include ${CMAKE_CURRENT_SOURCE_DIR})

if (NOT XCENA_SDK_CMAKE_BUILD)
    find_package(pxl REQUIRED)
    message(STATUS "PXL library found at: ${pxl_DIR}")
    set(shared_libs pxl::pxl)
else()
    set(shared_libs pxl)
endif()

find_package(Threads REQUIRED)

foreach(file ${src})
    get_filename_component(target ${file} NAME_WE)
    add_executable(${target} ${file})
    target_include_directories(${target} PRIVATE ${include})
    target_link_libraries(${target} ${shared_libs} Threads::Threads)
    install(TARGETS ${target} RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "  Source: ${src}")
message(STATUS "  Shared Libs: ${shared_libs}")
