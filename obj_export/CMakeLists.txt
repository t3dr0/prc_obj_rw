cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_C_STANDARD 99)

add_executable(nano_prc_obj_export
               src/obj_export.c)

target_link_libraries(nano_prc_obj_export PRIVATE nano_prc)
target_include_directories(nano_prc_obj_export PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Only link the math library if we aren't targeting Windows
if(NOT WIN32)
    target_link_libraries(nano_prc_obj_export PRIVATE m)
endif()
